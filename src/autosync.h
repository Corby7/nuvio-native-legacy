// AUTOSYNC: lines an addon subtitle up with the video on its own.
//
// THE REFERENCE is the file's own embedded subtitles. Every embedded track is
// timed to THIS video, and the Matroska Cues index lists when each of its lines
// starts (mkv.c reads it by Range, a few MB at the end of the file, without
// playing anything). An addon subtitle cut for another release of the same film
// has the same lines at shifted times — a constant offset, and when the release
// ran at another frame rate, a stretch.
//
// THE MATCH is a vote. Every addon line votes for the offset between its start
// and each reference start within two minutes of it; the true offset is where
// the votes pile up, because every line agrees on it while chance spreads the
// rest thin. It is tried at 1x and at the usual frame-rate ratios, and it is
// only applied when it is unambiguous: enough lines land on a reference start,
// and no other offset comes close. A wrong sync is worse than none.
//
// THE RESULT moves the loaded subtitle's own times (subtitle_retime) and does
// NOT touch the delay setting: that one also reaches the pipeline for embedded
// tracks, and an addon subtitle's offset has no business shifting them. The
// manual delay stays a fine-tune on top.
//
// The idea and its shape come from NuvioTV-Reshaped's AutoSync (Android, GPL-3),
// here reduced to its constant-offset and frame-rate core.
#ifndef NV_AUTOSYNC_H
#define NV_AUTOSYNC_H
#include "mkv.h"

typedef struct {
  int    ok;         // 1 when the match is trustworthy
  int    inSync;     // ok, and already within tolerance: nothing to move
  double scale;      // reference = candidate * scale + offset
  double offset;     // seconds
  int    lines;      // candidate lines considered
  int    matched;    // of those, how many land on a reference start
  double runnerUp;   // votes for the best OTHER offset, as a fraction of the winner's
  char   why[64];    // what decided it, for the log
} AutoSyncResult;

// The reference track's line starts, sorted, from the Cues entries `c` of the
// file whose header is `h`. Text tracks beat PGS (a PGS track indexes each line
// twice, shown and cleared), then the addon subtitle's own `language`, then the
// most lines; forced and sparse tracks never qualify. Returns how many starts
// (a fresh *onsets the caller frees) and the track in *track; 0 when no track
// qualifies.
int autosync_reference(const MkvHead *h, const MkvCue *c, int n, const char *language,
                       double **onsets, int *track);

// Matches the candidate's line starts `cand` (any order) against the sorted
// reference `ref`. Pure: also used by the regression test.
void autosync_match(const double *ref, int nRef, const double *cand, int nCand,
                    AutoSyncResult *r);

// A new playback: forget the handled subtitle. The Cues read stays cached by URL.
void autosync_reset(void);
// Once per frame, on the drawing thread. Starts a match when an addon subtitle
// has loaded and the MKV header is in, and applies the result when it arrives.
void autosync_pump(void);
// "Syncing subtitles" while a sync is on its way — the header awaited or the
// match running — and NULL otherwise. Only once a sync is possible at all: an
// MKV with subtitle tracks and an addon subtitle loaded.
const char *autosync_status(void);

#endif
