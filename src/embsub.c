#include "embsub.h"
#include "spu.h"
#include "subtitle.h"
#include "net.h"
#include "data.h"
#include "video.h"
#include "iptv_parse.h"
#include "gfx.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

// How far ahead of the playback position frames are fetched, and how far back a
// frame can have started and still be on screen. A minute ahead keeps a line of
// text ready well before it is due at one request every few seconds; behind
// covers a long sign that started just before a seek landed.
#define AHEAD_S   60.0
#define BEHIND_S  10.0
// Frames close together in the file share one request. Text lines are hundreds
// of KB of video apart on an SD rip and rarely do; a sign stacked on the dialogue
// usually sits in the same Cluster and does.
#define GROUP_SPAN   (256L * 1024)
#define GROUP_MAX    24
// What is asked for past the last frame's position before its size is known.
#define GUESS_TEXT   (2L * 1024)
#define GUESS_PIC    (24L * 1024)
// Frames that fail in a row, from the start, before the track is given up on:
// the index is pointing somewhere this module does not understand.
#define FIRST_FAILS  8

typedef enum { KIND_NONE, KIND_ASS, KIND_SRT, KIND_VOBSUB } Kind;

static Kind kindOf(const char *codec) {
  if (!codec) return KIND_NONE;
  if (!strcmp(codec, "S_TEXT/ASS") || !strcmp(codec, "S_TEXT/SSA") ||
      !strcmp(codec, "S_ASS") || !strcmp(codec, "S_SSA")) return KIND_ASS;
  if (!strcmp(codec, "S_TEXT/UTF8")) return KIND_SRT;
  if (!strcmp(codec, "S_VOBSUB")) return KIND_VOBSUB;
  return KIND_NONE;
}

int embsub_renders(const char *codec) { return kindOf(codec) != KIND_NONE; }

// --- the log ----------------------------------------------------------------
// subtitle-embedded.log in the data folder: the app's stdout goes to a private
// /tmp on the TV, and "the subtitle did not come on" has to be answerable over
// ssh — no index, no positions, a server refusing the Range.
static void emLog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void emLog(const char *fmt, ...) {
  char path[600], line[700];
  struct stat sb;
  time_t now = time(NULL);
  struct tm lt;
  FILE *f;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  printf("[embsub] %s\n", line);
  fflush(stdout);
  if (!data_path(path, sizeof path, "subtitle-embedded.log")) return;
  if (stat(path, &sb) == 0 && sb.st_size > 64L * 1024L) remove(path);
  if (!(f = fopen(path, "a"))) return;
  localtime_r(&now, &lt);
  { char stamp[32]; strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &lt);
    fprintf(f, "%s | %s\n", stamp, line); }
  fclose(f);
}

// --- the state ----------------------------------------------------------------
//
// `job` numbers every start: a worker whose number is no longer current stops at
// its next step and throws away what it was holding. Everything below is under
// `lock`; GL is only ever touched from embsub_pictures, on the drawing thread.
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned job;
static int running, failed, curTrack;
static unsigned curSubGen;          // the overlay generation this module opened
static char curUrl[1024];

// VobSub pictures: the packet as it came (a few KB) and its texture while on
// screen. Decoding happens just in time, so hundreds of frames never become
// hundreds of full bitmaps.
typedef struct {
  double start, end;
  unsigned char *pkt; int len;
  SpuFrame f;
  unsigned tex;
} Pic;
static Pic *pics;
static int nPics, capPics;
static SpuIdx idx;
static unsigned picsJob;
// Textures of a stopped track, released by the next embsub_pictures.
static unsigned grave[64];
static int nGrave;

typedef struct {
  unsigned job, subGen;
  char url[1024];
  MkvHead head;
  int track;
} Job;

static int stale(unsigned j) {
  int s;
  pthread_mutex_lock(&lock); s = j != job; pthread_mutex_unlock(&lock);
  return s;
}

static void dropPics(void) {
  int i;
  for (i = 0; i < nPics; i++) {
    free(pics[i].pkt);
    if (pics[i].tex && nGrave < (int)(sizeof grave / sizeof *grave)) grave[nGrave++] = pics[i].tex;
  }
  free(pics); pics = NULL; nPics = capPics = 0;
}

// --- decoding a frame -----------------------------------------------------------

// ContentCompression applied to one frame (or to CodecPrivate). A fresh buffer
// in *out; 0 when it cannot be undone.
static int uncompress(const MkvTrack *t, int scopeBit, const unsigned char *in, long n,
                      unsigned char **out, long *outN) {
  if (t->comp < 0 || !(t->compScope & scopeBit)) {
    *out = malloc((size_t)n + 1);
    if (!*out) return 0;
    memcpy(*out, in, (size_t)n); (*out)[n] = 0; *outN = n;
    return 1;
  }
  if (t->comp == 3) {                     // header stripping
    *out = malloc((size_t)(t->nStrip + n) + 1);
    if (!*out) return 0;
    memcpy(*out, t->strip, (size_t)t->nStrip);
    memcpy(*out + t->nStrip, in, (size_t)n);
    *outN = t->nStrip + n; (*out)[*outN] = 0;
    return 1;
  }
  if (t->comp == 0) {                     // zlib
    long m = 0;
    char *z = iptv_gunzip((const char *)in, n, &m);
    if (!z) return 0;
    *out = (unsigned char *)z; *outN = m;
    return 1;
  }
  return 0;                               // bzlib, lzo, encryption
}

// --- the worker -------------------------------------------------------------------

typedef struct { MkvCue c; int done; } Item;

static const MkvTrack *trackOf(const MkvHead *h, int number) {
  int i;
  for (i = 0; i < h->nTracks; i++) if (h->tracks[i].number == number) return &h->tracks[i];
  return NULL;
}

// The file offset a Cue's frame starts at (give or take the Cluster header).
static long long where(const Job *j, const Item *it) {
  return j->head.segmentAt + it->c.cluster + it->c.rel;
}

static void *work(void *arg) {
  Job *j = arg;
  const MkvTrack *t = trackOf(&j->head, j->track);
  Kind kind = t ? kindOf(t->codec) : KIND_NONE;
  char final[2048] = "", *header = NULL, why[96] = "";
  MkvCue *all = NULL;
  Item *items = NULL;
  int nAll = -1, nItems = 0, i, fails = 0, okFrames = 0, requests = 0, netFails = 0;
  long bytes = 0, got = 0;
  long long total = 0;

  if (!t || kind == KIND_NONE) { snprintf(why, sizeof why, "track %d not drawable", j->track); goto fail; }
  if (j->head.segmentAt < 0) { snprintf(why, sizeof why, "no Segment position"); goto fail; }
  // THE INDEX THROUGH THE ORIGINAL LINK, the one the header probe and AutoSync
  // read through. MEASURED on Trigun S1E7: with the address resolved first, this
  // first request failed on every start, fourteen times running, and the track
  // fell back to the pipeline. A couple of retries cover a request that merely
  // collided with the video's own buffering.
  for (i = 0; i < 3 && !stale(j->job); i++) {
    nAll = mkv_cues(j->url, &j->head, &all, &bytes, why, sizeof why);
    if (nAll >= 0) break;
    usleep(1500 * 1000);
  }
  if (stale(j->job)) goto out;
  if (nAll < 0) goto fail;
  // The resolved address saves the redirect on every frame, but only if it
  // answers a Range with the file's own bytes: the Cues' ID where the index is.
  snprintf(final, sizeof final, "%s", j->url);
  { char resolved[2048], host[96];
    if (net_url_final(j->url, 15, resolved, sizeof resolved) && strcmp(resolved, j->url)) {
      long m = 0;
      char *probe = net_download_chunk(resolved, 10, j->head.cuesAt, j->head.cuesAt + 3, &m);
      if (probe && m >= 4 && !memcmp(probe, "\x1C\x53\xBB\x6B", 4))
        snprintf(final, sizeof final, "%s", resolved);
      else
        emLog("track %d: resolved address %s refuses the Range, reading through the original link",
              j->track, net_url_public(resolved, host, sizeof host));
      free(probe);
    } }

  if (t->privSize > 0 && t->privAt >= 0) {
    char *raw = net_download_chunk(final, 20, t->privAt, t->privAt + t->privSize - 1, &got);
    unsigned char *plain = NULL;
    long plainN = 0;
    if (raw && got == t->privSize && uncompress(t, 2, (unsigned char *)raw, got, &plain, &plainN))
      header = (char *)plain;
    else
      emLog("track %d: CodecPrivate unreadable (%ld of %d bytes)", j->track, raw ? got : 0L, t->privSize);
    free(raw);
  }
  if (stale(j->job)) goto out;
  if (kind == KIND_VOBSUB) {
    pthread_mutex_lock(&lock);
    if (j->job == job) { spu_idx(header ? header : "", &idx); picsJob = j->job; }
    pthread_mutex_unlock(&lock);
  }

  for (i = 0; i < nAll; i++) if (all[i].track == j->track) nItems++;
  if (!nItems) { snprintf(why, sizeof why, "track %d not in the index (%d entries)", j->track, nAll); goto fail; }
  items = calloc((size_t)nItems, sizeof *items);
  if (!items) { snprintf(why, sizeof why, "out of memory"); goto fail; }
  nItems = 0;
  for (i = 0; i < nAll; i++)
    if (all[i].track == j->track && all[i].cluster >= 0 && all[i].rel >= 0) items[nItems++].c = all[i];
  if (!nItems) { snprintf(why, sizeof why, "index has no frame positions (CueRelativePosition)"); goto fail; }
  // A frame with no duration anywhere lasts until the next one, five seconds at most.
  for (i = 0; i < nItems; i++)
    if (items[i].c.end <= items[i].c.start) {
      double next = i + 1 < nItems ? items[i + 1].c.start : items[i].c.start + 5.0;
      items[i].c.end = next - items[i].c.start > 5.0 || next <= items[i].c.start ? items[i].c.start + 5.0 : next;
    }
  emLog("track %d %s: %d frames indexed, index %ld KB, header %d bytes",
        j->track, t->codec, nItems, bytes / 1024, t->privSize);

  while (!stale(j->job)) {
    double pos = video_pos();
    int first = -1, last, k;
    long long base, end;
    char *buf;
    long n = 0;
    char *frames[GROUP_MAX];
    double fStart[GROUP_MAX], fEnd[GROUP_MAX];
    int nFrames = 0;
    SubtitleCue *parsed = NULL;

    // The earliest frame still wanted: on screen now or due within AHEAD_S.
    for (i = 0; i < nItems; i++) {
      if (items[i].done || items[i].c.end < pos - BEHIND_S) continue;
      if (items[i].c.start > pos + AHEAD_S) break;
      first = i; break;
    }
    if (first < 0) { usleep(300 * 1000); continue; }
    base = where(j, &items[first]);
    last = first;
    for (k = first + 1; k < nItems && k - first < GROUP_MAX; k++) {
      long long w = where(j, &items[k]);
      if (items[k].done || w < base || w - base > GROUP_SPAN) break;
      last = k;
    }
    end = where(j, &items[last]) + MKV_BLOCK_SLACK + (kind == KIND_VOBSUB ? GUESS_PIC : GUESS_TEXT);
    buf = net_download_chunk(final, 20, base, end - 1, &n);
    requests++;
    if (!buf || n < 16) {
      free(buf);
      // The resolved address may have expired; the original link resolves anew.
      if (++netFails == 3 && strcmp(final, j->url)) {
        emLog("track %d: range requests failing, back to the original link", j->track);
        snprintf(final, sizeof final, "%s", j->url);
      }
      if (netFails >= 8) { snprintf(why, sizeof why, "range requests failing (%d)", netFails); goto fail; }
      usleep(1000 * 1000);
      continue;
    }
    netFails = 0;
    total += n;

    for (k = first; k <= last; k++) {
      long off = (long)(where(j, &items[k]) - base), at = 0, len = 0, need = 0;
      long long dur = -1;
      const unsigned char *p;
      unsigned char *extra = NULL, *plain = NULL;
      long plainN = 0;
      int r;
      items[k].done = 1;
      if (off >= n) { fails++; continue; }
      p = (unsigned char *)buf + off;
      r = mkv_block_find(p, n - off, j->track, &at, &len, &dur, &need);
      if (r == 2 && need <= MKV_BLOCK_MAX) {
        long m = 0;
        extra = (unsigned char *)net_download_chunk(final, 20, base + off, base + off + need - 1, &m);
        requests++;
        if (extra) { total += m; p = extra; r = mkv_block_find(p, m, j->track, &at, &len, &dur, &need); }
      }
      if (r != 1 || !uncompress(t, 1, p + at, len, &plain, &plainN)) {
        free(extra);
        if (++fails >= FIRST_FAILS && !okFrames) {
          snprintf(why, sizeof why, "frames not where the index says (%d misses)", fails);
          for (i = 0; i < nFrames; i++) free(frames[i]);
          free(buf); goto fail;
        }
        continue;
      }
      okFrames++;
      { double start = items[k].c.start, stop = items[k].c.end;
        if (dur > 0) stop = start + (double)dur * (double)j->head.scale / 1e9;
        if (kind != KIND_VOBSUB) {
          fStart[nFrames] = start; fEnd[nFrames] = stop;
          frames[nFrames++] = (char *)plain; plain = NULL;
        } else {
          Pic pic;
          memset(&pic, 0, sizeof pic);
          if (spu_parse(plain, (int)plainN, &pic.f)) {
            pic.start = start + pic.f.on;
            pic.end = pic.f.off > pic.f.on ? start + pic.f.off : stop;
            pic.pkt = plain; pic.len = (int)plainN; plain = NULL;
            pthread_mutex_lock(&lock);
            if (j->job == job && picsJob == j->job) {
              if (nPics == capPics) {
                int cap = capPics ? capPics * 2 : 128;
                Pic *g = realloc(pics, (size_t)cap * sizeof *g);
                if (g) { pics = g; capPics = cap; }
              }
              if (nPics < capPics) { pics[nPics++] = pic; pic.pkt = NULL; }
            }
            pthread_mutex_unlock(&lock);
            free(pic.pkt);
          }
        } }
      free(plain); free(extra);
    }
    free(buf);
    if (nFrames) {
      int nText = embsub_text_cues(t->codec, header, (const char *const *)frames, fStart, fEnd, nFrames, &parsed);
      for (i = 0; i < nFrames; i++) free(frames[i]);
      if (nText > 0 && !subtitle_embedded_add(j->subGen, parsed, nText) && stale(j->job)) {
        free(parsed); break;
      }
      free(parsed);
    }
    if (okFrames == 1 || requests % 50 == 0)
      emLog("track %d: %d frames read in %d requests, %lld KB", j->track, okFrames, requests, total / 1024);
    // Gentle when the next frame is far off: the video is on the same link.
    if (first + 1 < nItems && items[last].c.start > pos + 15.0) usleep(250 * 1000);
  }
  goto out;

fail:
  { char host[96];
    emLog("track %d: gave up — %s (%s)", j->track, why, net_url_public(final[0] ? final : j->url, host, sizeof host)); }
  pthread_mutex_lock(&lock);
  if (j->job == job) failed = 1;
  pthread_mutex_unlock(&lock);
out:
  if (okFrames) emLog("track %d: stopped, %d frames in %d requests, %lld KB", j->track, okFrames, requests, total / 1024);
  free(header); free(all); free(items); free(j);
  return NULL;
}

// --- the API ----------------------------------------------------------------------

void embsub_start(const char *url, const MkvHead *h, int track) {
  Job *j;
  pthread_t th;
  if (!url || !url[0] || !h) return;
  pthread_mutex_lock(&lock);
  if (running && curTrack == track && !strcmp(curUrl, url) && !failed) { pthread_mutex_unlock(&lock); return; }
  job++;
  running = 1; failed = 0; curTrack = track;
  snprintf(curUrl, sizeof curUrl, "%s", url);
  dropPics();
  picsJob = 0;
  pthread_mutex_unlock(&lock);
  j = calloc(1, sizeof *j);
  if (!j) return;
  pthread_mutex_lock(&lock);
  j->job = job;
  pthread_mutex_unlock(&lock);
  j->head = *h;
  j->track = track;
  snprintf(j->url, sizeof j->url, "%s", url);
  { const MkvTrack *t = trackOf(h, track);
    if (!t || kindOf(t->codec) != KIND_VOBSUB) j->subGen = subtitle_embedded_begin();
    pthread_mutex_lock(&lock); curSubGen = j->subGen; pthread_mutex_unlock(&lock);
    emLog("start track %d (%s)", track, t ? t->codec : "?"); }
  if (pthread_create(&th, NULL, work, j) == 0) pthread_detach(th);
  else { free(j); pthread_mutex_lock(&lock); failed = 1; pthread_mutex_unlock(&lock); }
}

void embsub_stop(void) {
  unsigned g;
  pthread_mutex_lock(&lock);
  if (!running) { pthread_mutex_unlock(&lock); return; }
  job++;
  running = 0; failed = 0; curTrack = 0; curUrl[0] = 0;
  g = curSubGen; curSubGen = 0;
  dropPics();
  picsJob = 0;
  pthread_mutex_unlock(&lock);
  if (g) subtitle_embedded_end(g);
}

int embsub_track(void) {
  int t;
  pthread_mutex_lock(&lock); t = running ? curTrack : 0; pthread_mutex_unlock(&lock);
  return t;
}

int embsub_failed(void) {
  int f;
  pthread_mutex_lock(&lock); f = running && failed; pthread_mutex_unlock(&lock);
  return f;
}

// Decoded and uploaded on the drawing thread, only while on screen: one upload a
// frame at most, so a burst of signs cannot stall the picture.
static unsigned upload(Pic *p) {
  unsigned char *rgba;
  GLuint tex = 0;
  if (p->f.w <= 0 || p->f.h <= 0 || p->f.w > 4096 || p->f.h > 4096) return 0;
  rgba = malloc((size_t)p->f.w * p->f.h * 4);
  if (!rgba) return 0;
  if (spu_render(p->pkt, p->len, &p->f, &idx, rgba)) {
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, p->f.w, p->f.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    gfx_tex_forget(0);
  }
  free(rgba);
  return tex;
}

int embsub_pictures(double t, EmbsubPicture *out, int max) {
  int i, k = 0, uploads = 0;
  pthread_mutex_lock(&lock);
  while (nGrave > 0) {
    GLuint g = grave[--nGrave];
    gfx_tex_forget(g);
    glDeleteTextures(1, &g);
  }
  for (i = 0; i < nPics; i++) {
    Pic *p = &pics[i];
    int on = t >= p->start && t < p->end;
    if (!on) {
      if (p->tex) { GLuint g = p->tex; gfx_tex_forget(g); glDeleteTextures(1, &g); p->tex = 0; }
      continue;
    }
    if (!p->tex && uploads++ < 1) p->tex = upload(p);
    if (p->tex && k < max && idx.w > 0 && idx.h > 0) {
      out[k].tex = p->tex;
      out[k].x = (float)p->f.x / idx.w; out[k].y = (float)p->f.y / idx.h;
      out[k].w = (float)p->f.w / idx.w; out[k].h = (float)p->f.h / idx.h;
      k++;
    }
  }
  pthread_mutex_unlock(&lock);
  return k;
}
