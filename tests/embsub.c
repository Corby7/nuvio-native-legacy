// The app's own reading of an embedded subtitle track, the network and GL left
// out: mkv.c finds each frame from the Cues' positions, embsub_text.c turns the
// frames into cues, spu.c decodes a VobSub picture.
//
// tests/fixtures/ass.mkv is ffmpeg's mux of 16 s of black video, silent audio
// and one ASS track of four lines — the second a \pos sign that overlaps the
// first and third, which is the case the TV's own drawing drops.
#include "mkv.h"
#include "embsub.h"
#include "spu.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Not reached: the pure halves are all this test drives.
char *net_download_chunk(const char *url, int seconds, long long start, long long end, long *size) {
  (void)url; (void)seconds; (void)start; (void)end; (void)size; return NULL;
}
char *net_download_bin(const char *url, int seconds, long *n) {
  (void)url; (void)seconds; (void)n; return NULL;
}

static int near(double a, double b) { return fabs(a - b) < 0.0015; }

static void assFrames(void) {
  FILE *f = fopen("tests/fixtures/ass.mkv", "rb");
  unsigned char *file;
  long size;
  MkvHead h;
  MkvCue *c = NULL;
  const MkvTrack *t = NULL;
  char *frames[8], header[8192];
  double st[8], en[8];
  SubtitleCue *cues = NULL;
  int i, n, nf = 0;
  assert(f);
  fseek(f, 0, SEEK_END); size = ftell(f); fseek(f, 0, SEEK_SET);
  file = malloc((size_t)size);
  assert(fread(file, 1, (size_t)size, f) == (size_t)size);
  fclose(f);

  assert(mkv_head_parse(file, size, &h) == 3);
  assert(h.segmentAt > 0 && h.cuesAt > 0);
  for (i = 0; i < h.nTracks; i++) if (h.tracks[i].kind == 17) t = &h.tracks[i];
  assert(t && !strcmp(t->codec, "S_TEXT/ASS") && t->comp == -1);
  // CodecPrivate is the script header, found by position.
  assert(t->privAt > 0 && t->privSize > 100 && t->privSize < (int)sizeof header);
  memcpy(header, file + t->privAt, (size_t)t->privSize); header[t->privSize] = 0;
  assert(strstr(header, "[Script Info]") && strstr(header, "Style: Sign"));

  // Every subtitle frame is indexed with its Cluster and its place in it.
  n = mkv_cues_parse(file + h.cuesAt, size - h.cuesAt, &h, &c);
  assert(n == 4);
  for (i = 0; i < n; i++) {
    long at = 0, len = 0, need = 0;
    long long dur = -1, off;
    assert(c[i].track == t->number && c[i].cluster >= 0 && c[i].rel >= 0);
    off = h.segmentAt + c[i].cluster + c[i].rel;
    assert(mkv_block_find(file + off, size - off, t->number, &at, &len, &dur, &need) == 1);
    // A buffer cut inside the frame asks for the rest instead of failing.
    { long at2, len2, need2; long long dur2;
      assert(mkv_block_find(file + off, 20, t->number, &at2, &len2, &dur2, &need2) == 2 && need2 > 20); }
    // The wrong track at the same place is not taken for it.
    { long at2, len2, need2; long long dur2;
      assert(mkv_block_find(file + off, size - off, t->number + 1, &at2, &len2, &dur2, &need2) == 0); }
    frames[nf] = malloc((size_t)len + 1);
    memcpy(frames[nf], file + off + at, (size_t)len); frames[nf][len] = 0;
    st[nf] = c[i].start;
    en[nf] = dur > 0 ? c[i].start + dur * (double)h.scale / 1e9 : c[i].end;
    nf++;
  }
  assert(near(st[0], 1.0) && near(en[0], 4.0));
  assert(near(st[1], 2.0) && near(en[1], 6.5));
  assert(near(st[3], 12.25) && near(en[3], 14.0));

  // Back to cues, the sign keeping its point and the style's alignment.
  n = embsub_text_cues(t->codec, header, (const char *const *)frames, st, en, nf, &cues);
  assert(n == 4);
  assert(!strcmp(cues[0].text, "Hello there, Vash.") && near(cues[0].start, 1.0));
  assert(!strcmp(cues[1].text, "WANTED: $$60 BILLION") && cues[1].positioned && cues[1].align == 8);
  assert(fabs(cues[1].x - 0.5f) < 0.001f && fabs(cues[1].y - 40.0f / 480.0f) < 0.001f);
  assert(!strcmp(cues[2].text, "Love and peace!") && !cues[2].positioned);
  free(cues);

  // No header at all still gives the text, with the standard columns.
  n = embsub_text_cues("S_TEXT/ASS", NULL, (const char *const *)frames, st, en, nf, &cues);
  assert(n == 4 && !strcmp(cues[3].text, "Second cluster line"));
  free(cues);

  // SRT frames: a blank line inside one does not split it.
  { const char *srt[2] = { "First line\n\nstill the first", "Second" };
    double s2[2] = { 1.5, 3.0 }, e2[2] = { 2.5, 4.25 };
    n = embsub_text_cues("S_TEXT/UTF8", NULL, srt, s2, e2, 2, &cues);
    assert(n == 2 && !strcmp(cues[0].text, "First line\nstill the first"));
    assert(near(cues[1].start, 3.0) && near(cues[1].end, 4.25));
    free(cues); }

  for (i = 0; i < nf; i++) free(frames[i]);
  free(c); free(file);
}

// A 4x2 VobSub picture built by hand: the top line two pixels of colour 1 and
// two of colour 2, the bottom line colour 3 to the end; on at 0, off after 100
// units of 1024/90000 s.
static void vobsub(void) {
  static const unsigned char pkt[] = {
    0x00, 0x25, 0x00, 0x07,
    0x9A,                          // top field: run 2 of 1, run 2 of 2
    0x00, 0x03,                    // bottom field: to the end of the line, 3
    0x00, 0x00, 0x00, 0x1F,        // at 0; the next sequence is at 31
    0x01,                          // start
    0x03, 0x32, 0x10,              // colours 3 2 | 1 0
    0x04, 0xFF, 0xF0,              // alpha 15 15 | 15 0
    0x05, 0x00, 0xA0, 0x0D, 0x01, 0x40, 0x15,   // x 10..13, y 20..21
    0x06, 0x00, 0x04, 0x00, 0x05,  // fields at 4 and 5
    0xFF,
    0x00, 0x64, 0x00, 0x1F,        // at 100 units, the last sequence
    0x02, 0xFF,                    // stop
  };
  SpuIdx idx;
  SpuFrame f;
  unsigned char rgba[4 * 2 * 4];
  spu_idx("# VobSub index file\nsize: 720x480\npalette: 000000, ff0000, 00ff00, 0000ff, 111111\n", &idx);
  assert(idx.w == 720 && idx.h == 480);
  assert(idx.palette[1] == 0xff0000 && idx.palette[3] == 0x0000ff && idx.palette[4] == 0x111111);
  assert(spu_parse(pkt, (int)sizeof pkt, &f));
  assert(f.x == 10 && f.y == 20 && f.w == 4 && f.h == 2);
  assert(near(f.on, 0.0) && near(f.off, 100 * 1024.0 / 90000.0));
  assert(spu_render(pkt, (int)sizeof pkt, &f, &idx, rgba));
  // Top line: red, red, green, green, opaque.
  assert(rgba[0] == 255 && rgba[1] == 0 && rgba[3] == 255);
  assert(rgba[4 * 2 + 1] == 255 && rgba[4 * 3 + 1] == 255 && rgba[4 * 3 + 0] == 0);
  // Bottom line: blue across.
  assert(rgba[16 + 2] == 255 && rgba[16 + 3] == 255 && rgba[28 + 2] == 255);
  // Malformed: a truncated control chain.
  assert(!spu_parse(pkt, 20, &f));
}

int main(void) {
  assFrames();
  vobsub();
  printf("embsub: ok\n");
  return 0;
}
