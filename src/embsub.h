// EMBEDDED SUBTITLES, drawn by the app instead of the pipeline.
//
// WHY THIS EXISTS. The LG pipeline's handling of a Matroska file's subtitles
// fails two ways, both MEASURED on Trigun S1E8 (a DVD rip with a "Signs and
// Songs" VobSub, an English VobSub and an English ASS track):
//   - it lists only the ASS track — the two VobSubs do not exist as far as
//     sourceInfo is concerned, so there was nothing to pick;
//   - it draws that ASS track with lines missing: a fansub script overlaps a
//     sign with the dialogue under it, and what came out was "subtitles only
//     sometimes".
// This module reads the chosen track out of the file itself and draws it: text
// through the same overlay (subtitle.c, player.c) an addon subtitle uses, and
// VobSub as pictures (spu.c).
//
// HOW, WITHOUT A DEMUXER. The Cues index (mkv.c) says, for every subtitle frame,
// when it plays and exactly where it sits in the file. The frames are fetched by
// Range a little ahead of the playback position — a few hundred bytes for a
// line of text, a few KB for a picture — so nothing is downloaded up front and a
// seek simply moves where the fetching looks. A file whose index has no frame
// positions cannot be read this way; embsub_failed then says so, and the caller
// falls back to the pipeline.
#ifndef NV_EMBSUB_H
#define NV_EMBSUB_H
#include "mkv.h"
#include "subtitle.h"

// Whether this module can draw a track of that CodecID: ASS, SSA, SRT
// (S_TEXT/UTF8) and VobSub.
int  embsub_renders(const char *codec);

// Starts drawing Matroska track `track` of `url`, whose header is `h`. Again
// with the same url and track while it runs is a no-op. Opens a fresh embedded
// generation in subtitle.c, so turn any addon subtitle off BEFORE this.
void embsub_start(const char *url, const MkvHead *h, int track);
// Stops, and clears the overlay when it is still this module's.
void embsub_stop(void);
// The Matroska track number being drawn, 0 when none.
int  embsub_track(void);
// 1 once the running track has turned out unreadable (no index, no positions,
// frames not where the index says).
int  embsub_failed(void);

// THE PICTURES on screen at `t` (seconds, the delay already applied), for
// player.c to draw. Call from the drawing thread once per frame even when there
// is nothing to draw: textures are uploaded and released here, the only thread
// with the GL context. x/y/w/h are fractions of the video frame.
typedef struct { unsigned tex; float x, y, w, h; } EmbsubPicture;
int  embsub_pictures(double t, EmbsubPicture *out, int max);

// THE PURE HALF (embsub_text.c), also used by the regression test: text frames
// of an ASS/SSA or SRT track, with their times, as the cues subtitle.c draws.
// An ASS frame is its Dialogue line with the ReadOrder in front and the times
// taken out; `header` is the track's CodecPrivate (the script's [Script Info] and
// styles), so alignment and \pos come through. The caller frees *out.
int  embsub_text_cues(const char *codec, const char *header, const char *const *frames,
                      const double *start, const double *end, int n, SubtitleCue **out);

#endif
