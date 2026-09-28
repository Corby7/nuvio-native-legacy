#include "mkv.h"
#include "net.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// How much of the file to download. The Tracks element sits right after the
// SeekHead and the Info, before the first Cluster — in practice within the
// first 200 KB.
//
// 320 KB and not 2 MB. The 2 MB were "generous slack" for a remux with cover
// art embedded before Tracks, and they cost dearly at the one moment this read
// happens: WITH THE VIDEO ALREADY PLAYING, over the same connection and from
// the same server. MEASURED on the owner's TV — the read finished at 45.9 s
// and the buffer ran short 1.6 s later, dropping to 2.8 s and taking 9 s to
// recover. The case the 2 MB covered (cover art before Tracks) is rare; the
// cost was paid on EVERY playback. Losing the language on one such file beats
// stuttering the video on all of them.
#define MKV_CHUNK  (320L * 1024)

// The Cues are read in one go when they fit in the first request, and in two
// when they do not — the size is only known once the element's header is in.
// The ceiling is for the same reason as MKV_CHUNK above: this read also happens
// with the video playing. A film with a handful of subtitle tracks indexes in a
// few hundred KB, but a UHD remux carries forty PGS tracks, each indexed twice
// per line (shown, cleared). 8 MB is still about a second of such a remux's own
// bitrate; past it the file loses the embedded timing rather than stuttering.
// MEASURED on the owner's TV: a 54 GB remux with 43 subtitle tracks failed
// under the first ceiling of 2 MB.
#define MKV_CUES_FIRST (128L * 1024)
#define MKV_CUES_MAX   (8L * 1024 * 1024)

// --- EBML: variable-length integers ------------------------------------------
//
// The first byte says, by the position of the highest 1 bit, how many bytes the
// number occupies. In the ID that bit IS PART of the value (which is why IDs
// are written as 0x1A45DFA3); in the SIZE it is a marker and drops out.
// Swapping the two is the classic first-timer's mistake here, and the symptom
// is the whole tree coming out shifted.
static int widthOf(unsigned char b) {
  int i;
  for (i = 0; i < 8; i++) if (b & (0x80 >> i)) return i + 1;
  return 0;                       // byte 0x00: invalid in EBML
}

// Reads an ID (keeping the marker bit). 0 is the end or invalid data.
static unsigned long readId(const unsigned char *p, long remains, int *used) {
  int w, i;
  unsigned long v;
  if (remains < 1) return 0;
  w = widthOf(p[0]);
  if (w < 1 || w > 4 || remains < w) return 0;
  v = 0;
  for (i = 0; i < w; i++) v = (v << 8) | p[i];
  *used = w;
  return v;
}

// Reads a SIZE (stripping the marker bit). Returns -1 on invalid and -2 on the
// "unknown" size (all data bits 1), which Segment uses in a live-streamed file
// — there the read continues INSIDE the element instead of skipping over it.
// 64-bit: a Segment's size is the size of the film.
static long long readSize(const unsigned char *p, long remains, int *used) {
  int w, i;
  unsigned long long v;
  int allOnes = 1;
  if (remains < 1) return -1;
  w = widthOf(p[0]);
  if (w < 1 || w > 8 || remains < w) return -1;
  v = p[0] & (0xFF >> w);
  if ((unsigned char)(p[0] & (0xFF >> w)) != (unsigned char)(0xFF >> w)) allOnes = 0;
  for (i = 1; i < w; i++) {
    if (p[i] != 0xFF) allOnes = 0;
    v = (v << 8) | p[i];
  }
  *used = w;
  if (allOnes) return -2;
  if (v > 0x7FFFFFFFFFFFFFFFULL) return -1;
  return (long long)v;
}

static unsigned long long readUint(const unsigned char *p, long long n) {
  unsigned long long v = 0;
  long long i;
  if (n < 1 || n > 8) return 0;
  for (i = 0; i < n; i++) v = (v << 8) | p[i];
  return v;
}

static void readText(const unsigned char *p, long long n, char *dst, size_t size) {
  size_t k = (size_t)n;
  if (k > size - 1) k = size - 1;
  memcpy(dst, p, k);
  dst[k] = 0;
  // Matroska pads strings with NUL on the right; cutting here stops the rest of
  // the field turning into rubbish on screen.
  { size_t i; for (i = 0; i < k; i++) if (dst[i] == 0) { dst[i] = 0; break; } }
}

// Steps over one child of a master element spanning [0, n): its id, its
// payload's offset in *at and its size in *size. 0 at the end or on anything
// that does not fit — every walker below stops there rather than guess.
static unsigned long child(const unsigned char *p, long long n, long long o,
                           long long *at, long long *size) {
  int ui = 0, ut = 0;
  unsigned long id = readId(p + o, (long)(n - o), &ui);
  if (!id) return 0;
  *size = readSize(p + o + ui, (long)(n - o - ui), &ut);
  if (*size < 0) return 0;
  *at = o + ui + ut;
  if (*at + *size > n) return 0;
  return id;
}

// --- ids that matter ---------------------------------------------------------
#define ID_EBML        0x1A45DFA3UL
#define ID_SEGMENT     0x18538067UL
#define ID_SEEKHEAD    0x114D9B74UL
#define ID_SEEK        0x4DBBUL
#define ID_SEEKID      0x53ABUL
#define ID_SEEKPOS     0x53ACUL
#define ID_INFO        0x1549A966UL
#define ID_TSSCALE     0x2AD7B1UL
#define ID_CLUSTER     0x1F43B675UL
#define ID_TRACKS      0x1654AE6BUL
#define ID_TRACKENTRY  0xAEUL
#define ID_TRACKNUMBER 0xD7UL
#define ID_TRACKTYPE   0x83UL
#define ID_LANGUAGE    0x22B59CUL     // Language (ISO 639-2), the classic one
#define ID_LANG_BCP47  0x22B59DUL     // LanguageBCP47 ("pt-BR"), newer
#define ID_NAME        0x536EUL
#define ID_CODECID     0x86UL
#define ID_FLAGFORCED  0x55AAUL
#define ID_CUES        0x1C53BB6BUL
#define ID_CUEPOINT    0xBBUL
#define ID_CUETIME     0xB3UL
#define ID_CUETRACKPOS 0xB7UL
#define ID_CUETRACK    0xF7UL
#define ID_CUEDURATION 0xB2UL
#define ID_CUECLUSTER  0xF1UL
#define ID_CUERELPOS   0xF0UL
#define ID_CODECPRIV   0x63A2UL
#define ID_ENCODINGS   0x6D80UL
#define ID_ENCODING    0x6240UL
#define ID_ENCSCOPE    0x5032UL
#define ID_ENCTYPE     0x5033UL
#define ID_COMPRESSION 0x5034UL
#define ID_COMPALGO    0x4254UL
#define ID_COMPSETTING 0x4255UL

// ContentEncodings: only the compression of the first encoding is kept. An
// encrypted track is marked with an algorithm nothing decodes, so it is skipped
// rather than shown as rubbish.
static void readEncodings(const unsigned char *p, long long n, MkvTrack *f) {
  long long o = 0, at, size;
  unsigned long id;
  while (o < n && (id = child(p, n, o, &at, &size))) {
    if (id == ID_ENCODING && f->comp < 0) {
      long long q = 0, eat, esize;
      unsigned long eid;
      int type = 0, scope = 1, algo = -1, nStrip = 0;
      unsigned char strip[16];
      while (q < size && (eid = child(p + at, size, q, &eat, &esize))) {
        const unsigned char *v = p + at + eat;
        if (eid == ID_ENCSCOPE) scope = (int)readUint(v, esize);
        else if (eid == ID_ENCTYPE) type = (int)readUint(v, esize);
        else if (eid == ID_COMPRESSION) {
          long long k = 0, cat, csize;
          unsigned long cid;
          algo = 0;                 // ContentCompAlgo's default is zlib
          while (k < esize && (cid = child(v, esize, k, &cat, &csize))) {
            if (cid == ID_COMPALGO) algo = (int)readUint(v + cat, csize);
            else if (cid == ID_COMPSETTING && csize <= (long long)sizeof strip) {
              memcpy(strip, v + cat, (size_t)csize); nStrip = (int)csize;
            }
            k = cat + csize;
          }
        }
        q = eat + esize;
      }
      f->comp = type == 1 ? 99 : algo;
      f->compScope = scope;
      if (algo == 3) { memcpy(f->strip, strip, (size_t)nStrip); f->nStrip = nStrip; }
    }
    o = at + size;
  }
}

// Reads the TrackEntry elements inside an already-located Tracks, whose payload
// starts at `fileAt` in the file.
static int readTracks(const unsigned char *p, long long n, long long fileAt,
                      MkvTrack *output, int max) {
  long long o = 0, at, size;
  unsigned long id;
  int found = 0;
  while (o < n && found < max && (id = child(p, n, o, &at, &size))) {
    if (id == ID_TRACKENTRY) {
      MkvTrack f;
      long long q = 0, fat, fsize;
      unsigned long fid;
      memset(&f, 0, sizeof f);
      f.privAt = -1; f.comp = -1;
      while (q < size && (fid = child(p + at, size, q, &fat, &fsize))) {
        const unsigned char *v = p + at + fat;
        if (fid == ID_TRACKNUMBER) f.number = (int)readUint(v, fsize);
        else if (fid == ID_TRACKTYPE) f.kind = (int)readUint(v, fsize);
        else if (fid == ID_LANGUAGE || fid == ID_LANG_BCP47) {
          // BCP47 beats ISO 639-2 when both exist: "pt-BR" says more than
          // "por", and it is what the owner wants to see in the list.
          if (fid == ID_LANG_BCP47 || !f.language[0])
            readText(v, fsize, f.language, sizeof f.language);
        }
        else if (fid == ID_NAME)    readText(v, fsize, f.name,  sizeof f.name);
        else if (fid == ID_CODECID) readText(v, fsize, f.codec, sizeof f.codec);
        else if (fid == ID_FLAGFORCED) f.forced = readUint(v, fsize) != 0;
        else if (fid == ID_CODECPRIV && fsize > 0 && fsize < (1 << 20)) {
          f.privAt = fileAt + at + fat; f.privSize = (int)fsize;
        }
        else if (fid == ID_ENCODINGS) readEncodings(v, fsize, &f);
        q = fat + fsize;
      }
      if (f.number > 0) output[found++] = f;
    }
    o = at + size;
  }
  return found;
}

// The SeekHead's entry for the Cues, relative to the Segment's payload. -1 when
// it lists none (the file may carry a second SeekHead, which is not followed).
static long long readSeekHead(const unsigned char *p, long long n) {
  long long o = 0, at, size;
  unsigned long id;
  while (o < n && (id = child(p, n, o, &at, &size))) {
    if (id == ID_SEEK) {
      long long q = 0, fat, fsize, pos = -1;
      unsigned long fid, target = 0;
      while (q < size && (fid = child(p + at, size, q, &fat, &fsize))) {
        if (fid == ID_SEEKID && fsize <= 4) target = (unsigned long)readUint(p + at + fat, fsize);
        else if (fid == ID_SEEKPOS) pos = (long long)readUint(p + at + fat, fsize);
        q = fat + fsize;
      }
      if (target == ID_CUES && pos >= 0) return pos;
    }
    o = at + size;
  }
  return -1;
}

// Walks the tree as far as the first Cluster. Descends into Segment (which is a
// giant container) and SKIPS the rest — without the skip the search would sweep
// byte by byte and match any coincidence inside the video data.
int mkv_head_parse(const unsigned char *p, long n, MkvHead *h) {
  long long o = 0, segment = -1, cuesRel = -1;
  memset(h, 0, sizeof *h);
  h->cuesAt = -1;
  h->segmentAt = -1;
  h->scale = 1000000;             // the Matroska default: milliseconds
  // The EBML signature. Without it this is not Matroska (it could be MP4, or an
  // error HTML the server returned with a 200), and going on would read rubbish.
  if (!p || n < 64 || p[0] != 0x1A || p[1] != 0x45 || p[2] != 0xDF || p[3] != 0xA3) return 0;
  while (o < n) {
    int ui = 0, ut = 0;
    unsigned long id = readId(p + o, n - o, &ui);
    long long size, at;
    if (!id) break;
    size = readSize(p + o + ui, (long)(n - o - ui), &ut);
    if (size == -1) break;
    at = o + ui + ut;
    // Segment: descend into it. Its size is the film's; there is nothing to
    // check it against.
    if (id == ID_SEGMENT) { segment = h->segmentAt = at; o = at; continue; }
    // Media data from here on: the header is over.
    if (id == ID_CLUSTER || size == -2) break;
    if (id == ID_TRACKS) {
      long long avail = n - at;
      // A header larger than the downloaded chunk still yields the tracks that
      // fit, which is what this read did before it learnt about the Cues.
      h->nTracks = readTracks(p + at, size < avail ? size : avail, at, h->tracks, MKV_MAX_TRACKS);
    }
    if (at + size > n) break;        // the element runs past what we downloaded
    if (id == ID_SEEKHEAD && cuesRel < 0) cuesRel = readSeekHead(p + at, size);
    else if (id == ID_INFO) {
      long long q = 0, fat, fsize;
      unsigned long fid;
      while (q < size && (fid = child(p + at, size, q, &fat, &fsize))) {
        if (fid == ID_TSSCALE) {
          unsigned long long v = readUint(p + at + fat, fsize);
          if (v) h->scale = v;
        }
        q = fat + fsize;
      }
    }
    else if (id == ID_CUES && h->cuesAt < 0) h->cuesAt = o;   // indexed up front
    o = at + size;
  }
  if (h->cuesAt < 0 && cuesRel >= 0 && segment >= 0) h->cuesAt = segment + cuesRel;
  return h->nTracks;
}

static int isSubtitle(const MkvHead *h, int number) {
  int i;
  for (i = 0; i < h->nTracks; i++)
    if (h->tracks[i].number == number) return h->tracks[i].kind == 17;
  return 0;
}

int mkv_cues_parse(const unsigned char *p, long n, const MkvHead *h, MkvCue **out) {
  const unsigned char *c;
  long long o = 0, cat, csize, at, size;
  unsigned long id;
  double tick;
  int found = 0, cap = 0;
  MkvCue *list = NULL;
  *out = NULL;
  if (!p || !h || child(p, n, 0, &cat, &csize) != ID_CUES) return -1;
  tick = (double)h->scale / 1e9;
  c = p + cat;
  while (o < csize && (id = child(c, csize, o, &at, &size))) {
    if (id == ID_CUEPOINT) {
      const unsigned char *q = c + at;
      long long k = 0, fat, fsize, ticks = -1;
      unsigned long fid;
      // CueTime first: nothing in the format promises it comes before the
      // positions it dates.
      while (k < size && (fid = child(q, size, k, &fat, &fsize))) {
        if (fid == ID_CUETIME) ticks = (long long)readUint(q + fat, fsize);
        k = fat + fsize;
      }
      k = 0;
      while (ticks >= 0 && k < size && (fid = child(q, size, k, &fat, &fsize))) {
        if (fid == ID_CUETRACKPOS) {
          long long j = 0, gat, gsize, dur = 0, cluster = -1, rel = -1;
          unsigned long gid;
          int track = 0;
          while (j < fsize && (gid = child(q + fat, fsize, j, &gat, &gsize))) {
            if (gid == ID_CUETRACK) track = (int)readUint(q + fat + gat, gsize);
            else if (gid == ID_CUEDURATION) dur = (long long)readUint(q + fat + gat, gsize);
            else if (gid == ID_CUECLUSTER) cluster = (long long)readUint(q + fat + gat, gsize);
            else if (gid == ID_CUERELPOS) rel = (long long)readUint(q + fat + gat, gsize);
            j = gat + gsize;
          }
          if (track > 0 && isSubtitle(h, track)) {
            if (found == cap) {
              MkvCue *grown;
              cap = cap ? cap * 2 : 512;
              grown = realloc(list, (size_t)cap * sizeof *list);
              if (!grown) { free(list); return -1; }
              list = grown;
            }
            list[found].track = track;
            list[found].start = (double)ticks * tick;
            list[found].end = (double)(ticks + dur) * tick;
            list[found].cluster = cluster;
            list[found].rel = rel;
            found++;
          }
        }
        k = fat + fsize;
      }
    }
    o = at + size;
  }
  *out = list;
  return found;
}

// A Block's own header: the track as a vint, a 16-bit relative time, the flags.
// Lacing is refused — no muxer laces subtitles, and a laced frame read as one
// would be shown as garbage.
static int blockHeader(const unsigned char *p, long long n, int track, long long *used) {
  int w;
  unsigned long long t = 0;
  int i;
  if (n < 4) return 0;
  w = widthOf(p[0]);
  if (w < 1 || w > 8 || n < w + 3) return 0;
  t = p[0] & (0xFF >> w);
  for (i = 1; i < w; i++) t = (t << 8) | p[i];
  if ((int)t != track || (p[w + 2] & 0x06)) return 0;
  *used = w + 3;
  return 1;
}

#define ID_BLOCKGROUP  0xA0UL
#define ID_BLOCK       0xA1UL
#define ID_SIMPLEBLOCK 0xA3UL
#define ID_BLOCKDUR    0x9BUL

int mkv_block_find(const unsigned char *p, long n, int track, long *at, long *len,
                   long long *duration, long *need) {
  int s;
  *need = 0; *duration = -1;
  for (s = 0; s <= MKV_BLOCK_SLACK && s < n; s++) {
    int ui = 0, ut = 0;
    unsigned long id = readId(p + s, n - s, &ui);
    long long size, body, used;
    if (id != ID_BLOCKGROUP && id != ID_SIMPLEBLOCK) continue;
    size = readSize(p + s + ui, n - s - ui, &ut);
    if (size < 4 || size > MKV_BLOCK_MAX) continue;
    body = s + ui + ut;
    // Too short to hold the whole element: the track number is still checked on
    // what did arrive, so a stray byte pattern does not cost a second request.
    if (id == ID_SIMPLEBLOCK) {
      if (!blockHeader(p + body, (n - body) < size ? n - body : size, track, &used)) continue;
      if (body + size > n) { *need = (long)(body + size); return 2; }
      *at = (long)(body + used); *len = (long)(size - used);
      return 1;
    }
    { long long o = 0, cat, csize, avail = n - body < size ? n - body : size;
      unsigned long cid;
      long blockAt = -1, blockLen = 0;
      int bad = 0;
      // A BlockGroup's children: the Block, and its duration. Walked on the bytes
      // that arrived; one that runs past them means asking for the rest.
      while (o < avail) {
        int ci = 0, ct = 0;
        cid = readId(p + body + o, (long)(avail - o), &ci);
        if (!cid) { bad = 1; break; }
        csize = readSize(p + body + o + ci, (long)(avail - o - ci), &ct);
        if (csize < 0) { bad = 1; break; }
        cat = o + ci + ct;
        if (cid == ID_BLOCK) {
          if (!blockHeader(p + body + cat, (avail - cat) < csize ? avail - cat : csize, track, &used)) { bad = 1; break; }
          blockAt = (long)(body + cat + used); blockLen = (long)(csize - used);
        } else if (cid == ID_BLOCKDUR && cat + csize <= avail) {
          *duration = (long long)readUint(p + body + cat, csize);
        }
        o = cat + csize;
      }
      if (bad || (blockAt < 0 && body + size <= n)) continue;
      if (body + size > n) { *need = (long)(body + size); return 2; }
      *at = blockAt; *len = blockLen;
      return 1; }
  }
  return 0;
}

int mkv_head(const char *url, MkvHead *h) {
  char *buf;
  long n = 0;
  int found;
  memset(h, 0, sizeof *h);
  h->cuesAt = -1;
  h->segmentAt = -1;
  if (!url || !url[0]) return 0;
  buf = net_download_chunk(url, 20, 0, MKV_CHUNK - 1, &n);
  if (!buf) return 0;
  found = mkv_head_parse((const unsigned char *)buf, n, h);
  free(buf);
  printf("[mkv] %d tracks read from the header (%ld bytes), cues at %lld\n", found, n, h->cuesAt);
  fflush(stdout);
  return found;
}

int mkv_cues(const char *url, const MkvHead *h, MkvCue **out, long *bytes,
             char *why, unsigned whySize) {
  unsigned char *buf;
  long n = 0;
  long long size, total;
  int found;
  char none[8];
  if (!why || !whySize) { why = none; whySize = sizeof none; }
  why[0] = 0;
  *out = NULL;
  if (bytes) *bytes = 0;
  if (!url || !url[0] || !h || h->cuesAt < 0) { snprintf(why, whySize, "no index position"); return -1; }
  buf = (unsigned char *)net_download_chunk(url, 20, h->cuesAt, h->cuesAt + MKV_CUES_FIRST - 1, &n);
  if (!buf) { snprintf(why, whySize, "first request failed"); return -1; }
  if (bytes) *bytes = n;
  // A server that ignores Range sends the file from byte 0, which starts with
  // the EBML signature and not with the Cues: refused here, and the net layer's
  // ceiling has already cut the transfer at MKV_CUES_FIRST.
  { int ui = 0, ut = 0;
    unsigned long id = readId(buf, n, &ui);
    if (id != ID_CUES) {
      snprintf(why, whySize, "not the Cues there (id %lx)", id);
      free(buf); return -1;
    }
    size = readSize(buf + ui, n - ui, &ut);
    if (size < 0) { snprintf(why, whySize, "unreadable index size"); free(buf); return -1; }
    total = ui + ut + size; }
  if (total > MKV_CUES_MAX) {
    snprintf(why, whySize, "index of %lld KB, over the ceiling", total / 1024);
    free(buf); return -1;
  }
  if (total > n) {
    long more = 0;
    char *rest = net_download_chunk(url, 30, h->cuesAt + n, h->cuesAt + total - 1, &more);
    unsigned char *whole;
    if (!rest || more != total - n) {
      snprintf(why, whySize, "second request got %ld of %lld bytes", rest ? more : 0L, total - n);
      free(rest); free(buf); return -1;
    }
    whole = realloc(buf, (size_t)total);
    if (!whole) { snprintf(why, whySize, "out of memory"); free(rest); free(buf); return -1; }
    buf = whole;
    memcpy(buf + n, rest, (size_t)more);
    free(rest);
    n += more;
    if (bytes) *bytes = n;
  }
  found = mkv_cues_parse(buf, n, h, out);
  if (found < 0) snprintf(why, whySize, "index of %lld KB did not parse", total / 1024);
  free(buf);
  return found;
}
