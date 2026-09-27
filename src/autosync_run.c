// AutoSync's driving: when to match, the worker thread, the Cues cache, the
// log, and applying the result. The matcher is autosync.c.
#include "autosync.h"
#include "subtitle.h"
#include "video.h"
#include "player.h"
#include "tracks.h"
#include "data.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

// AUTOSYNC.LOG, in the data folder beside tracks.log and for the same reason:
// the TV's /tmp is private to the app, and this is what answers "why did it not
// sync" — which track was the reference, how many lines matched, what it moved.
static void asLog(const char *text) {
  char path[600];
  struct stat sb;
  time_t now = time(NULL);
  struct tm lt;
  FILE *f;
  if (!data_path(path, sizeof path, "autosync.log")) return;
  if (stat(path, &sb) == 0 && sb.st_size > 128L * 1024L) remove(path);
  if (!(f = fopen(path, "a"))) return;
  localtime_r(&now, &lt);
  { char stamp[32]; strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &lt);
    fprintf(f, "%s | %s\n", stamp, text); }
  fclose(f);
}

typedef struct {
  unsigned gen;          // the subtitle this job is for
  MkvHead head;
  char url[1024];
  char language[8];
  double *starts, *ends; // the subtitle's lines, copied
  int n;
} Job;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int busy;             // a worker is running
static int ready;            // a result waits to be applied
static AutoSyncResult result;
static unsigned resultGen;
static unsigned handled;     // the last subtitle generation a job was started for
static int waiting;          // an addon subtitle is loaded and the header awaited

// The Cues of the last stream read. Only the worker touches it, and only one
// worker runs at a time: switching subtitles on one film costs no second read.
static char cacheUrl[1024];
static MkvCue *cache;
static int cacheN;
static char cacheWhy[80];

// The Cues read and what it found, per subtitle track — the measurement that
// said this was worth building, kept.
static void logCues(const MkvHead *h, const MkvCue *c, int n, long bytes, long ms, const char *why) {
  char line[1500];
  size_t k = 0;
  int i, j;
  k += (size_t)snprintf(line, sizeof line, "cues %s%s: %d subtitle entries, %ld KB at %lld MB, %ld ms",
                        n < 0 ? "failed, " : "read", n < 0 ? why : "", n < 0 ? 0 : n,
                        bytes / 1024, h->cuesAt >> 20, ms);
  for (i = 0; n > 0 && i < h->nTracks && k + 80 < sizeof line; i++) {
    const MkvTrack *t = &h->tracks[i];
    int count = 0;
    double last = 0;
    if (t->kind != 17) continue;
    for (j = 0; j < n; j++)
      if (c[j].track == t->number) { count++; if (c[j].start > last) last = c[j].start; }
    k += (size_t)snprintf(line + k, sizeof line - k, " ; #%d %s %s: %d to %.0f min",
                          t->number, t->language[0] ? t->language : "-",
                          t->codec[0] ? t->codec : "-", count, last / 60.0);
  }
  asLog(line);
}

static long msSince(const struct timespec *t0) {
  struct timespec t1;
  clock_gettime(CLOCK_MONOTONIC, &t1);
  return (long)((t1.tv_sec - t0->tv_sec) * 1000 + (t1.tv_nsec - t0->tv_nsec) / 1000000);
}

static void *work(void *arg) {
  Job *j = arg;
  AutoSyncResult r;
  double *ref = NULL;
  int nRef = 0, track = 0;
  struct timespec t0;
  memset(&r, 0, sizeof r);
  r.scale = 1.0;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  if (strcmp(cacheUrl, j->url)) {
    long bytes = 0;
    free(cache); cache = NULL; cacheN = 0; cacheWhy[0] = 0;
    cacheN = mkv_cues(j->url, &j->head, &cache, &bytes, cacheWhy, sizeof cacheWhy);
    logCues(&j->head, cache, cacheN, bytes, msSince(&t0), cacheWhy);
    snprintf(cacheUrl, sizeof cacheUrl, "%s", j->url);
  }
  if (cacheN <= 0)
    snprintf(r.why, sizeof r.why, "no embedded timing (%s)", cacheN < 0 ? cacheWhy : "no subtitle entries");
  else if (!(nRef = autosync_reference(&j->head, cache, cacheN, j->language, &ref, &track)))
    snprintf(r.why, sizeof r.why, "no embedded track to sync against");
  else
    autosync_match(ref, nRef, j->starts, j->n, &r);
  { char m[300];
    snprintf(m, sizeof m, "subtitle %s, %d lines ; reference track #%d, %d starts ; "
             "scale %.5f offset %+.3f s ; matched %d/%d, runner-up %.0f%% ; %s ; %ld ms",
             j->language[0] ? j->language : "-", j->n, track, nRef, r.scale, r.offset,
             r.matched, r.lines, r.runnerUp * 100.0, r.why, msSince(&t0));
    asLog(m); }
  free(ref);
  pthread_mutex_lock(&lock);
  result = r; resultGen = j->gen; ready = 1; busy = 0;
  pthread_mutex_unlock(&lock);
  free(j->starts); free(j->ends); free(j);
  return NULL;
}

const char *autosync_status(void) {
  int b;
  pthread_mutex_lock(&lock); b = busy; pthread_mutex_unlock(&lock);
  return b || waiting ? "Syncing subtitles" : NULL;
}

void autosync_reset(void) {
  pthread_mutex_lock(&lock);
  handled = 0; ready = 0; waiting = 0;
  pthread_mutex_unlock(&lock);
}

static void apply(const AutoSyncResult *r, unsigned gen) {
  char m[96];
  // A result for a subtitle that is no longer on screen says nothing.
  if (subtitle_ready() != gen) return;
  if (!r->ok) { player_toast("Couldn't sync subtitles", 0); return; }
  if (r->inSync) { player_toast("Subtitles already in sync", 1); return; }
  if (!subtitle_retime(gen, r->scale, r->offset)) return;
  // Said the way the owner sees it: "+1.3 s" is the subtitle now coming LATER.
  if (r->scale != 1.0)
    snprintf(m, sizeof m, "Subtitles synced  \xc2\xb7  %+.1f s, frame rate adjusted", r->offset);
  else
    snprintf(m, sizeof m, "Subtitles synced  \xc2\xb7  %+.1f s", r->offset);
  player_toast(m, 1);
}

void autosync_pump(void) {
  static MkvHead head;
  char url[1024];
  unsigned g;
  Job *j;
  pthread_t thread;
  int isBusy;
  pthread_mutex_lock(&lock);
  if (ready) {
    AutoSyncResult r = result;
    unsigned rg = resultGen;
    ready = 0;
    pthread_mutex_unlock(&lock);
    apply(&r, rg);
    pthread_mutex_lock(&lock);
  }
  isBusy = busy;
  pthread_mutex_unlock(&lock);
  if (isBusy) return;
  g = subtitle_ready();
  waiting = 0;
  if (!g || g == handled) return;
  // Until the header is in, the subtitle plays as it came. video.c's probe waits
  // for spare bandwidth, a minute on its own; asked, it goes after ten seconds.
  if (!video_mkv_head(&head, url, sizeof url)) {
    if (video_mkv_waiting()) { video_mkv_hurry(); waiting = 1; }
    return;
  }
  handled = g;
  if (head.cuesAt < 0) { asLog("no Cues position in the header; nothing to sync against"); return; }
  j = calloc(1, sizeof *j);
  if (!j) return;
  j->gen = g;
  j->head = head;
  snprintf(j->url, sizeof j->url, "%s", url);
  snprintf(j->language, sizeof j->language, "%s", tracks_external_language());
  j->n = subtitle_times(g, &j->starts, &j->ends);
  if (j->n < 1) { free(j); handled = 0; return; }
  pthread_mutex_lock(&lock);
  busy = 1;
  pthread_mutex_unlock(&lock);
  if (pthread_create(&thread, NULL, work, j) != 0) {
    pthread_mutex_lock(&lock); busy = 0; pthread_mutex_unlock(&lock);
    free(j->starts); free(j->ends); free(j);
    return;
  }
  pthread_detach(thread);
}
