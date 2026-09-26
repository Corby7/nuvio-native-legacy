// autosync.c's matcher in isolation, on synthetic films: a reference of line
// starts, and a "subtitle" made from it the way another release's would be —
// shifted, maybe stretched, with lines split, dropped and added, and a few
// frames of jitter.
#include "autosync.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned seed;
static double rnd(void) { seed = seed * 1103515245u + 12345u; return ((seed >> 8) & 0xFFFF) / 65536.0; }

// A two-hour film's dialogue: lines of 1-4 s with gaps of 0.3-8 s.
static int film(double *start, double *end, int max, unsigned s) {
  double t = 30.0;
  int n = 0;
  seed = s;
  while (t < 7000.0 && n < max) {
    double d = 1.0 + rnd() * 3.0;
    start[n] = t; end[n] = t + d; n++;
    t += d + 0.3 + rnd() * 7.7;
  }
  return n;
}

// The other release's subtitle: reference = cand * scale + offset, so each line
// sits at (ref - offset) / scale, give or take 40 ms; 15% of the lines are
// missing and 8% are extra.
static int release(const double *ref, int n, double scale, double offset, double *out) {
  int i, k = 0;
  seed = 99;
  for (i = 0; i < n; i++) {
    if (rnd() < 0.15) continue;
    out[k++] = (ref[i] - offset) / scale + (rnd() - 0.5) * 0.08;
    if (rnd() < 0.08) out[k++] = (ref[i] - offset) / scale + 1.5 + rnd();
  }
  return k;
}

static double ref[4000], refEnd[4000], cand[5000];

static void expect(double scale, double offset) {
  AutoSyncResult r;
  int n = film(ref, refEnd, 4000, 7), k = release(ref, n, scale, offset, cand);
  autosync_match(ref, n, cand, k, &r);
  printf("  scale %.4f offset %+7.3f -> ok %d scale %.4f offset %+7.3f matched %d/%d runner %.2f (%s)\n",
         scale, offset, r.ok, r.scale, r.offset, r.matched, r.lines, r.runnerUp, r.why);
  assert(r.ok && !r.inSync);
  assert(fabs(r.scale - scale) < 1e-9);
  assert(fabs(r.offset - offset) < 0.05);
}

int main(void) {
  AutoSyncResult r;
  int n, k, i;

  // Constant offsets, both ways, small and large.
  expect(1.0, 2.5);
  expect(1.0, -37.3);
  expect(1.0, 0.6);
  expect(1.0, 104.0);
  // Frame-rate stretches: a 25 fps subtitle on a 23.976 fps video, and the reverse.
  expect(25.0 / (24000.0 / 1001.0), 1.0);
  expect((24000.0 / 1001.0) / 25.0, -3.2);

  // Already in sync: matched, and left alone.
  n = film(ref, refEnd, 4000, 7); k = release(ref, n, 1.0, 0.03, cand);
  autosync_match(ref, n, cand, k, &r);
  assert(r.ok && r.inSync);

  // A subtitle for a different film: never applied.
  n = film(ref, refEnd, 4000, 7);
  k = film(cand, refEnd, 4000, 12345);
  autosync_match(ref, n, cand, k, &r);
  printf("  unrelated -> ok %d matched %d/%d runner %.2f (%s)\n", r.ok, r.matched, r.lines, r.runnerUp, r.why);
  assert(!r.ok);

  // Too little to go on.
  autosync_match(ref, n, cand, 10, &r);
  assert(!r.ok);

  // A PGS reference indexes every line twice, shown and cleared. The clears are
  // extra onsets that match nothing; the right offset still wins.
  { static double pgs[8000];
    int m = 0;
    n = film(ref, refEnd, 4000, 7);
    for (i = 0; i < n; i++) { pgs[m++] = ref[i]; pgs[m++] = refEnd[i]; }
    for (i = 1; i < m; i++) { double x = pgs[i]; int j = i; while (j > 0 && pgs[j - 1] > x) { pgs[j] = pgs[j - 1]; j--; } pgs[j] = x; }
    k = release(ref, n, 1.0, -4.4, cand);
    autosync_match(pgs, m, cand, k, &r);
    printf("  pgs reference -> ok %d offset %+.3f matched %d/%d runner %.2f\n", r.ok, r.offset, r.matched, r.lines, r.runnerUp);
    assert(r.ok && fabs(r.offset + 4.4) < 0.05); }

  // Picking the reference: text over PGS, then the subtitle's language, never a
  // forced or sparse track.
  { MkvHead h;
    static MkvCue c[9000];
    double *on = NULL;
    int track = 0, m = 0;
    memset(&h, 0, sizeof h);
    h.nTracks = 5;
    h.tracks[0] = (MkvTrack){ 1, 1, "und", "", "V_MPEGH/ISO/HEVC", 0 };
    h.tracks[1] = (MkvTrack){ 4, 17, "en", "English (SDH)", "S_HDMV/PGS", 0 };
    h.tracks[2] = (MkvTrack){ 5, 17, "en", "Forced", "S_TEXT/UTF8", 0 };
    h.tracks[3] = (MkvTrack){ 6, 17, "ms", "Malay", "S_TEXT/UTF8", 0 };
    h.tracks[4] = (MkvTrack){ 7, 17, "en", "English", "S_TEXT/UTF8", 0 };
    for (i = 0; i < 3000; i++) c[m++] = (MkvCue){ 4, i * 2.0, i * 2.0 + 1 };
    for (i = 0; i < 1000; i++) c[m++] = (MkvCue){ 5, i * 3.0, i * 3.0 + 1 };
    for (i = 0; i < 1500; i++) c[m++] = (MkvCue){ 6, i * 4.0, i * 4.0 + 1 };
    for (i = 0; i < 1400; i++) c[m++] = (MkvCue){ 7, i * 4.1, i * 4.1 + 1 };
    // English subtitle: the English TEXT track, not the busier PGS nor the forced one.
    assert(autosync_reference(&h, c, m, "eng", &on, &track) == 1400 && track == 7); free(on);
    // Malay subtitle: the Malay text track.
    assert(autosync_reference(&h, c, m, "may", &on, &track) > 0); free(on);
    assert(autosync_reference(&h, c, m, "ms", &on, &track) == 1500 && track == 6); free(on);
    // A language no text track has: the busiest text track.
    assert(autosync_reference(&h, c, m, "fr", &on, &track) == 1500 && track == 6); free(on);
    // PGS only: PGS it is.
    h.nTracks = 2;
    assert(autosync_reference(&h, c, m, "en", &on, &track) == 3000 && track == 4); free(on);
    // No subtitle tracks at all.
    h.nTracks = 1;
    assert(autosync_reference(&h, c, m, "en", &on, &track) == 0 && !on); }

  puts("autosync: ok");
  return 0;
}
