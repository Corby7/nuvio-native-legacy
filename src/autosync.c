// The matcher itself — pure, no network, no state. autosync_run.c drives it.
#include "autosync.h"
#include "lang.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Offsets are searched within two minutes either way, in 100 ms bins. Two
// minutes covers a studio logo, a recap or a cold open that one release has and
// the other does not; past that it is a different cut, and a constant offset
// would not fix it anyway.
#define AS_WINDOW 120.0
#define AS_BIN    0.1
#define AS_NBINS  ((int)(2 * AS_WINDOW / AS_BIN) + 1)

// A candidate line counts as MATCHED when, shifted, it starts within this of a
// reference start. Two translations of one film start their lines on the same
// words, give or take a few frames.
#define AS_HIT    0.25

// What makes a result trustworthy. MIN_LINES keeps a signs-only subtitle from
// deciding anything; MIN_FRACTION is well above chance (a reference line every
// few seconds catches a random start within 0.25 s perhaps one time in ten);
// MAX_RUNNER_UP is how close the second-best offset may come.
#define AS_MIN_LINES    20
#define AS_MIN_FRACTION 0.40
#define AS_MAX_RUNNER   0.60

// Under this the subtitle is left alone: it is in sync already, and moving it by
// a frame or two would only be noise.
#define AS_TOLERANCE 0.15

// The stretches tried besides 1x: a subtitle timed on a 25 fps PAL release, or on
// a 24 fps one, laid over a 23.976 fps video, and the reverse.
static const double SCALES[] = {
  1.0,
  25.0 / (24000.0 / 1001.0), (24000.0 / 1001.0) / 25.0,
  24.0 / (24000.0 / 1001.0), (24000.0 / 1001.0) / 24.0,
  25.0 / 24.0, 24.0 / 25.0,
};
#define AS_NSCALES (int)(sizeof SCALES / sizeof *SCALES)

static int cmpDouble(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : x > y;
}

// The first index whose value is >= v.
static int lowerBound(const double *v, int n, double x) {
  int lo = 0, hi = n;
  while (lo < hi) { int m = (lo + hi) / 2; if (v[m] < x) lo = m + 1; else hi = m; }
  return lo;
}

static int isForcedName(const char *name) {
  const char *p;
  for (p = name; *p; p++)
    if ((p[0] == 'F' || p[0] == 'f') && !strncmp(p + 1, "orced", 5)) return 1;
  return 0;
}

int autosync_reference(const MkvHead *h, const MkvCue *c, int n, const char *language,
                       double **onsets, int *track) {
  int i, j, best = -1, bestRank = -1, bestCount = 0, most = 0, found = 0;
  int wanted = language && language[0] ? lang_of(language) : -1;
  double *v;
  *onsets = NULL;
  if (track) *track = 0;
  if (!h || !c || n < 1) return 0;
  // The busiest track first: a track with far fewer lines than it is a forced or
  // signs-only track, whatever its flags say.
  for (i = 0; i < h->nTracks; i++) {
    int count = 0;
    if (h->tracks[i].kind != 17) continue;
    for (j = 0; j < n; j++) count += c[j].track == h->tracks[i].number;
    if (count > most) most = count;
  }
  for (i = 0; i < h->nTracks; i++) {
    const MkvTrack *t = &h->tracks[i];
    int count = 0, rank;
    if (t->kind != 17 || t->forced || isForcedName(t->name)) continue;
    for (j = 0; j < n; j++) count += c[j].track == t->number;
    if (count < AS_MIN_LINES || count * 4 < most) continue;
    rank = (!strncmp(t->codec, "S_TEXT", 6) ? 2 : 0) +
           (wanted >= 0 && lang_of(t->language) == wanted ? 1 : 0);
    if (rank > bestRank || (rank == bestRank && count > bestCount)) {
      best = i; bestRank = rank; bestCount = count;
    }
  }
  if (best < 0) return 0;
  v = malloc((size_t)bestCount * sizeof *v);
  if (!v) return 0;
  for (j = 0; j < n; j++)
    if (c[j].track == h->tracks[best].number) v[found++] = c[j].start;
  qsort(v, (size_t)found, sizeof *v, cmpDouble);
  // Two starts within 50 ms are one onset (PGS: a line cleared and the next
  // shown on the same frame).
  { int k = 0;
    for (j = 0; j < found; j++) if (!k || v[j] - v[k - 1] > 0.05) v[k++] = v[j];
    found = k; }
  *onsets = v;
  if (track) *track = h->tracks[best].number;
  return found;
}

// One stretch: votes, the winning offset refined, and how many lines it matches.
typedef struct { double offset; int matched; double runnerUp; } Trial;

static Trial trial(const double *ref, int nRef, const double *cand, int nCand,
                   double scale, int *hist) {
  Trial t = { 0.0, 0, 1.0 };
  int i, j, lo = 0, b, bestB = -1, bestV = 0, runner = 0, hits = 0;
  double center, sum = 0.0;
  memset(hist, 0, (size_t)AS_NBINS * sizeof *hist);
  for (i = 0; i < nCand; i++) {
    double x = cand[i] * scale;
    while (lo < nRef && ref[lo] < x - AS_WINDOW) lo++;
    for (j = lo; j < nRef && ref[j] <= x + AS_WINDOW; j++) {
      b = (int)floor((ref[j] - x + AS_WINDOW) / AS_BIN + 0.5);
      if (b >= 0 && b < AS_NBINS) hist[b]++;
    }
  }
  // Three bins at a time: an offset of 1.25 s lands half its votes at 1.2 and
  // half at 1.3, and a single bin would halve its own peak.
  for (b = 1; b < AS_NBINS - 1; b++) {
    int v = hist[b - 1] + hist[b] + hist[b + 1];
    if (v > bestV) { bestV = v; bestB = b; }
  }
  if (bestB < 0) return t;
  // The best offset at least a second away from the winner.
  for (b = 1; b < AS_NBINS - 1; b++) {
    int v = hist[b - 1] + hist[b] + hist[b + 1];
    if (abs(b - bestB) > 10 && v > runner) runner = v;
  }
  t.runnerUp = (double)runner / (double)bestV;
  // The winner to the millisecond: the mean of the votes near it.
  center = bestB * AS_BIN - AS_WINDOW;
  for (i = 0; i < nCand; i++) {
    double x = cand[i] * scale;
    j = lowerBound(ref, nRef, x + center - 0.2);
    for (; j < nRef && ref[j] <= x + center + 0.2; j++) { sum += ref[j] - x; hits++; }
  }
  t.offset = hits ? sum / hits : center;
  for (i = 0; i < nCand; i++) {
    double x = cand[i] * scale + t.offset;
    j = lowerBound(ref, nRef, x - AS_HIT);
    if (j < nRef && ref[j] <= x + AS_HIT) t.matched++;
  }
  return t;
}

void autosync_match(const double *ref, int nRef, const double *cand, int nCand,
                    AutoSyncResult *r) {
  int *hist, i, k = 0, best = 0;
  double *v;
  Trial tr[AS_NSCALES];
  memset(r, 0, sizeof *r);
  r->scale = 1.0;
  r->runnerUp = 1.0;
  if (!ref || !cand || nRef < AS_MIN_LINES || nCand < AS_MIN_LINES) {
    snprintf(r->why, sizeof r->why, "too few lines (%d reference, %d subtitle)", nRef, nCand);
    return;
  }
  v = malloc((size_t)nCand * sizeof *v);
  hist = malloc((size_t)AS_NBINS * sizeof *hist);
  if (!v || !hist) { free(v); free(hist); snprintf(r->why, sizeof r->why, "out of memory"); return; }
  memcpy(v, cand, (size_t)nCand * sizeof *v);
  qsort(v, (size_t)nCand, sizeof *v, cmpDouble);
  for (i = 0; i < nCand; i++) if (!k || v[i] - v[k - 1] > 0.05) v[k++] = v[i];
  r->lines = k;
  for (i = 0; i < AS_NSCALES; i++) tr[i] = trial(ref, nRef, v, k, SCALES[i], hist);
  // A stretch has to EARN its place: 1x keeps the result unless another ratio
  // matches clearly more lines. Near 1x the ratios overlap, and a stretch picked
  // on a tie would drift the subtitle over a two-hour film.
  for (i = 1; i < AS_NSCALES; i++)
    if (tr[i].matched > tr[best].matched && tr[i].matched * 10 > tr[0].matched * 12) best = i;
  free(v); free(hist);
  r->scale = SCALES[best];
  r->offset = tr[best].offset;
  r->matched = tr[best].matched;
  r->runnerUp = tr[best].runnerUp;
  if (r->matched < AS_MIN_LINES || r->matched < AS_MIN_FRACTION * k) {
    snprintf(r->why, sizeof r->why, "only %d of %d lines match", r->matched, k);
    return;
  }
  if (r->runnerUp > AS_MAX_RUNNER) {
    snprintf(r->why, sizeof r->why, "ambiguous, runner-up at %.0f%%", r->runnerUp * 100.0);
    return;
  }
  r->ok = 1;
  r->inSync = best == 0 && fabs(r->offset) < AS_TOLERANCE;
  snprintf(r->why, sizeof r->why, "%s", r->inSync ? "already in sync" : "matched");
}
