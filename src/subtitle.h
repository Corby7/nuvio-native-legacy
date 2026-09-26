#ifndef NV_SUBTITLE_H
#define NV_SUBTITLE_H
#include <stddef.h>

typedef struct {
  double start, end;
  char text[768];
} SubtitleCue;

/* OpenSubtitles is drawn by the UI, above the video plane. */
void subtitle_load(const char *url);
void subtitle_off(void);
int  subtitle_text(double posSeg, int delayMs, char *dst, size_t size);

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

/* Pure parser, also used by the regression test. The caller frees *out. */
int subtitle_parse(const char *body, SubtitleCue **output);

#endif
