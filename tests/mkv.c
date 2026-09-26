// mkv.c in isolation, over tests/fixtures/subtitles.mkv: ffmpeg's mux of a
// black video, silent audio and two SRT tracks (eng, fre) of the same 34 lines,
// the first at 2.000-3.836 s and the last at 168.640-172.128 s.
#include "mkv.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *file;
static long fileSize;
static int ignoreRange, shortReads, requests;

// The network, served from the fixture. `ignoreRange` answers from byte 0 like
// a server that does not do Range (the real layer cuts it at the asked size);
// `shortReads` hands back at most 1000 bytes, which makes mkv_cues ask twice.
char *net_download_chunk(const char *url, int seconds, long long start,
                         long long end, long *size) {
  long long n;
  char *b;
  (void)url; (void)seconds;
  requests++;
  if (ignoreRange) { n = end - start + 1; start = 0; }
  else n = end - start + 1;
  if (start >= fileSize) return NULL;
  if (start + n > fileSize) n = fileSize - start;
  if (shortReads && n > 1000) n = 1000;
  b = malloc((size_t)n);
  memcpy(b, file + start, (size_t)n);
  *size = (long)n;
  return b;
}

static int near(double a, double b) { return fabs(a - b) < 0.0015; }

int main(void) {
  FILE *f = fopen("tests/fixtures/subtitles.mkv", "rb");
  MkvHead h;
  MkvCue *c = NULL;
  long bytes = 0;
  int n, i, eng = 0, fre = 0;
  assert(f);
  fseek(f, 0, SEEK_END); fileSize = ftell(f); fseek(f, 0, SEEK_SET);
  file = malloc((size_t)fileSize);
  assert(fread(file, 1, (size_t)fileSize, f) == (size_t)fileSize);
  fclose(f);

  // The head: four tracks, the Cues found through the SeekHead at the end.
  assert(mkv_head_parse(file, fileSize, &h) == 4);
  assert(h.scale == 1000000);
  assert(h.tracks[2].kind == 17 && !strcmp(h.tracks[2].language, "eng"));
  assert(h.tracks[3].kind == 17 && !strcmp(h.tracks[3].language, "fre"));
  assert(!strcmp(h.tracks[2].codec, "S_TEXT/UTF8"));
  assert(h.cuesAt > fileSize / 2 && h.cuesAt < fileSize);

  // The cues: only the subtitle entries, dated and with their durations.
  n = mkv_cues(NULL, &h, &c, &bytes, NULL, 0);
  assert(n == -1 && !c);                        // no url, no read
  n = mkv_cues("x", &h, &c, &bytes, NULL, 0);
  assert(n == 68);
  for (i = 0; i < n; i++) {
    assert(c[i].track == h.tracks[2].number || c[i].track == h.tracks[3].number);
    if (c[i].track == h.tracks[2].number) eng++; else fre++;
  }
  assert(eng == 34 && fre == 34);
  assert(near(c[0].start, 2.0) && near(c[0].end, 3.836));
  assert(near(c[n - 1].start, 168.640) && near(c[n - 1].end, 172.128));
  for (i = 1; i < n; i++) assert(c[i].start >= c[i - 1].start);
  free(c);

  // The index split over two requests joins up the same.
  shortReads = 1; requests = 0;
  n = mkv_cues("x", &h, &c, &bytes, NULL, 0);
  assert(n == 68 && requests == 2 && near(c[n - 1].end, 172.128));
  free(c); shortReads = 0;

  // A server that ignores Range sends the file's start, which is not the Cues.
  ignoreRange = 1;
  { char why[80];
    assert(mkv_cues("x", &h, &c, &bytes, why, sizeof why) == -1 && !c);
    assert(!strncmp(why, "not the Cues there", 18)); }
  ignoreRange = 0;

  // A head chunk that stops inside Tracks still names the tracks that fit, and
  // still knows where the Cues are: the SeekHead that says so comes first.
  { MkvHead cut;
    long upto = 0, k;
    int seen = 0;
    // The SECOND occurrence of the Tracks id: the first is the SeekHead's
    // pointer to it.
    for (k = 0; k + 4 < fileSize; k++)
      if (file[k] == 0x16 && file[k + 1] == 0x54 && file[k + 2] == 0xAE && file[k + 3] == 0x6B &&
          ++seen == 2) { upto = k; break; }
    assert(upto);
    n = mkv_head_parse(file, upto + 200, &cut);
    assert(n >= 1 && n < 4 && cut.cuesAt == h.cuesAt); }

  // Not Matroska at all.
  { unsigned char junk[200];
    memset(junk, 'x', sizeof junk);
    assert(mkv_head_parse(junk, sizeof junk, &h) == 0 && h.cuesAt == -1); }

  // Garbage where the Cues should be.
  { unsigned char junk[64] = { 0x1C, 0x53, 0xBB, 0x6B, 0x88 };
    assert(mkv_cues_parse(junk, 5, &h, &c) == -1); }

  free(file);
  puts("mkv: ok");
  return 0;
}
