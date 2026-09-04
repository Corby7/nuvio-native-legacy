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

/* Pure parser, also used by the regression test. The caller frees *out. */
int subtitle_parse(const char *body, SubtitleCue **output);

#endif
