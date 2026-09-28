#ifndef NV_SUBTITLE_H
#define NV_SUBTITLE_H
#include <stddef.h>

typedef struct {
  double start, end;
  char text[768];
  // Where an ASS line goes; all zero for SRT/VTT, which means the usual band.
  // `align` is the numpad position (1 bottom-left .. 9 top-right), 0 unknown.
  // With `positioned`, (x, y) is a point as a fraction of the video frame and
  // `align` says which corner or edge of the text sits on it.
  int align, positioned;
  float x, y;
} SubtitleCue;

/* OpenSubtitles is drawn by the UI, above the video plane. `language` is the
   addon's code for it, the hint subcharset.c decodes a non-UTF-8 file by. */
void subtitle_load(const char *url, const char *language);
void subtitle_off(void);
int  subtitle_text(double posSeg, int delayMs, char *dst, size_t size);
/* The cues on screen at posSeg, oldest first and with their layout, a repeated
   one only once. Returns how many went into `out`. */
int  subtitle_shown(double posSeg, int delayMs, SubtitleCue *out, int max);

/* An EMBEDDED track, drawn by the same overlay as an addon's: embsub.c opens a
   generation with begin and feeds it cues as it fetches them from the file.
   add returns how many were new, 0 once `g` is no longer the one on (a later
   load, an off). subtitle_ready answers 0 for it: AutoSync has nothing to do
   with a track timed to the file it came out of. */
unsigned subtitle_embedded_begin(void);
int  subtitle_embedded_add(unsigned g, const SubtitleCue *cues, int n);
/* Clears the overlay only while `g` is still the one on: an addon subtitle
   loaded since is left alone. */
void subtitle_embedded_end(unsigned g);

/* For AutoSync (autosync.c). A subtitle is identified by its GENERATION, which
   changes on every load and every off: a result worked out for one subtitle can
   then never land on the next.
   subtitle_ready: the loaded subtitle's generation, 0 while none is loaded.
   subtitle_times: copies its start and end times (fresh arrays, the caller frees
   both); 0 when `g` is no longer the loaded one.
   subtitle_retime: moves every line to start*scale + offset (seconds), once per
   generation; 0 when `g` is gone or already retimed. */
unsigned subtitle_ready(void);
int  subtitle_times(unsigned g, double **starts, double **ends);
int  subtitle_retime(unsigned g, double scale, double offset);

/* The subtitle format at `url` as the sheet names it — "ASS", "SSA", "SRT" or
   "VTT" — from its extension or from the body of an earlier load. NULL while
   not known; with `fetch`, an unknown one is downloaded in the background and
   answers on a later call. The returned pointer is a shared buffer. */
const char *subtitle_kind(const char *url, int fetch);

/* Pure parser, also used by the regression test. The caller frees *out. */
int subtitle_parse(const char *body, SubtitleCue **output);

#endif
