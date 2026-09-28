#include "embsub.h"
#include "subtitle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The frames are put back into a document of their own format and handed to
// subtitle.c's parser, which already knows ASS styles, alignment and \pos —
// one parser for a downloaded file and an embedded track alike.
typedef struct { char *p; size_t n, cap; int broken; } Buf;

static void put(Buf *b, const char *s, size_t n) {
  if (b->broken) return;
  if (b->n + n + 1 > b->cap) {
    size_t cap = (b->n + n + 1) * 2;
    char *g = realloc(b->p, cap);
    if (!g) { b->broken = 1; return; }
    b->p = g; b->cap = cap;
  }
  memcpy(b->p + b->n, s, n); b->n += n; b->p[b->n] = 0;
}
static void putStr(Buf *b, const char *s) { put(b, s, strlen(s)); }

// "H:MM:SS.mmm", which subtitle.c reads to the millisecond.
static void clockOf(double s, char *dst, size_t size) {
  long ms = (long)(s * 1000.0 + 0.5);
  if (ms < 0) ms = 0;
  snprintf(dst, size, "%ld:%02ld:%02ld.%03ld", ms / 3600000, ms / 60000 % 60, ms / 1000 % 60, ms % 1000);
}

// "ReadOrder,Layer,Style,..." back to "Dialogue: Layer,Start,End,Style,...".
static void addAss(Buf *b, const char *frame, double start, double end) {
  const char *layer = strchr(frame, ','), *rest, *stop;
  char a[24], z[24];
  if (!layer) return;
  layer++;
  if (!(rest = strchr(layer, ','))) return;
  clockOf(start, a, sizeof a); clockOf(end, z, sizeof z);
  putStr(b, "Dialogue: ");
  put(b, layer, (size_t)(rest - layer));
  putStr(b, ","); putStr(b, a); putStr(b, ","); putStr(b, z);
  stop = rest + strcspn(rest, "\r\n");
  put(b, rest, (size_t)(stop - rest));
  putStr(b, "\n");
}

static void addSrt(Buf *b, const char *frame, double start, double end, int number) {
  char line[96], a[24], z[24];
  const char *p;
  int blank = 1;
  clockOf(start, a, sizeof a); clockOf(end, z, sizeof z);
  snprintf(line, sizeof line, "%d\n%s --> %s\n", number, a, z);
  putStr(b, line);
  // An empty line would end the cue early, so the frame's own blank lines go.
  for (p = frame; *p; p++) {
    if (*p == '\r') continue;
    if (*p == '\n') { if (blank) continue; blank = 1; }
    else blank = 0;
    put(b, p, 1);
  }
  putStr(b, "\n\n");
}

int embsub_text_cues(const char *codec, const char *header, const char *const *frames,
                     const double *start, const double *end, int n, SubtitleCue **out) {
  Buf b = { NULL, 0, 0, 0 };
  int i, ass = codec && (strstr(codec, "ASS") || strstr(codec, "SSA")), got;
  *out = NULL;
  if (!frames || n <= 0) return 0;
  if (ass) {
    putStr(&b, header && header[0] ? header : "[Script Info]\n");
    // mkvmerge keeps the [Events] heading and its Format line in CodecPrivate;
    // ffmpeg writes them too. A header without them gets the standard one.
    if (!header || !strstr(header, "[Events]"))
      putStr(&b, "\n[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n");
    else putStr(&b, "\n");
  }
  for (i = 0; i < n; i++) {
    if (!frames[i]) continue;
    if (ass) addAss(&b, frames[i], start[i], end[i]);
    else addSrt(&b, frames[i], start[i], end[i], i + 1);
  }
  got = b.p && !b.broken ? subtitle_parse(b.p, out) : 0;
  free(b.p);
  return got;
}
