#include "spu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void spu_idx(const char *text, SpuIdx *idx) {
  const char *p = text;
  int i;
  idx->w = 720; idx->h = 576;
  for (i = 0; i < 16; i++) idx->palette[i] = (unsigned)(i * 17) * 0x010101u;
  while (p && *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (!strncasecmp(p, "size:", 5)) {
      int w = 0, h = 0;
      if (sscanf(p + 5, " %dx%d", &w, &h) == 2 && w > 0 && h > 0 && w <= 4096 && h <= 4096) {
        idx->w = w; idx->h = h;
      }
    } else if (!strncasecmp(p, "palette:", 8)) {
      const char *q = p + 8;
      for (i = 0; i < 16; i++) {
        char *end;
        unsigned long v;
        while (*q == ' ' || *q == ',') q++;
        v = strtoul(q, &end, 16);
        if (end == q) break;
        idx->palette[i] = (unsigned)(v & 0xFFFFFF);
        q = end;
      }
    }
    p = strchr(p, '\n');
  }
}

static int u16(const unsigned char *p) { return (p[0] << 8) | p[1]; }

// A delay is in units of 1024 ticks of the 90 kHz clock.
#define SPU_TICK (1024.0 / 90000.0)

int spu_parse(const unsigned char *p, int n, SpuFrame *f) {
  int size, at, last = -1, guard = 0, area = 0, fields = 0;
  memset(f, 0, sizeof *f);
  f->off = -1;
  f->color[0] = 0; f->color[1] = 1; f->color[2] = 2; f->color[3] = 3;
  f->alpha[0] = 0; f->alpha[1] = f->alpha[2] = f->alpha[3] = 15;
  if (!p || n < 4) return 0;
  size = u16(p);
  if (size > n || size < 4) size = n;
  at = u16(p + 2);
  // The chain ends at a sequence that points at itself; the guard stops one that
  // loops somewhere else.
  while (at >= 4 && at + 4 <= size && at != last && guard++ < 64) {
    double delay = u16(p + at) * SPU_TICK;
    int next = u16(p + at + 2), c = at + 4, stop = 0;
    last = at;
    while (c < size && !stop) {
      switch (p[c]) {
        case 0x00: f->forced = 1; f->on = delay; c++; break;
        case 0x01: f->on = delay; c++; break;
        case 0x02: f->off = delay; c++; break;
        case 0x03:
          if (c + 3 > size) return 0;
          f->color[3] = p[c + 1] >> 4; f->color[2] = p[c + 1] & 15;
          f->color[1] = p[c + 2] >> 4; f->color[0] = p[c + 2] & 15;
          c += 3; break;
        case 0x04:
          if (c + 3 > size) return 0;
          f->alpha[3] = p[c + 1] >> 4; f->alpha[2] = p[c + 1] & 15;
          f->alpha[1] = p[c + 2] >> 4; f->alpha[0] = p[c + 2] & 15;
          c += 3; break;
        case 0x05: {
          int x1, x2, y1, y2;
          if (c + 7 > size) return 0;
          x1 = (p[c + 1] << 4) | (p[c + 2] >> 4);
          x2 = ((p[c + 2] & 15) << 8) | p[c + 3];
          y1 = (p[c + 4] << 4) | (p[c + 5] >> 4);
          y2 = ((p[c + 5] & 15) << 8) | p[c + 6];
          if (x2 < x1 || y2 < y1) return 0;
          f->x = x1; f->y = y1; f->w = x2 - x1 + 1; f->h = y2 - y1 + 1;
          area = 1; c += 7; break; }
        case 0x06:
          if (c + 5 > size) return 0;
          f->top = u16(p + c + 1); f->bottom = u16(p + c + 3);
          fields = 1; c += 5; break;
        case 0x07:                  // CHG_COLCON: its own length, skipped whole
          if (c + 3 > size) return 0;
          c += 1 + u16(p + c + 1); break;
        case 0xFF: stop = 1; break;
        default: return 0;
      }
    }
    at = next;
  }
  return area && fields && f->top < size && f->bottom < size && f->w > 0 && f->h > 0;
}

// The RLE reads in nibbles. A code is 1 to 4 nibbles long, told by its leading
// zeros; its low 2 bits are the pixel value and the rest the run length, where 0
// means "to the end of the line". Every line starts on a byte boundary.
typedef struct { const unsigned char *p; int n, nib; } Nib;

static int nibble(Nib *r) {
  int b;
  if ((r->nib >> 1) >= r->n) return -1;
  b = r->p[r->nib >> 1];
  b = (r->nib & 1) ? (b & 15) : (b >> 4);
  r->nib++;
  return b;
}

static int decodeLine(Nib *r, unsigned char *row, int w, const unsigned *rgba4) {
  int x = 0;
  while (x < w) {
    static const int more[3] = { 0x4, 0x10, 0x40 };
    int v = nibble(r), k, run, px;
    if (v < 0) return 0;
    for (k = 0; k < 3 && v < more[k]; k++) {
      int m = nibble(r);
      if (m < 0) return 0;
      v = (v << 4) | m;
    }
    run = v >> 2; px = v & 3;
    if (run == 0 || x + run > w) run = w - x;
    { unsigned c = rgba4[px];
      int i;
      for (i = 0; i < run; i++, x++) {
        unsigned char *d = row + x * 4;
        d[0] = (unsigned char)(c >> 24); d[1] = (unsigned char)(c >> 16);
        d[2] = (unsigned char)(c >> 8);  d[3] = (unsigned char)c;
      } }
  }
  if (r->nib & 1) r->nib++;
  return 1;
}

int spu_render(const unsigned char *p, int n, const SpuFrame *f, const SpuIdx *idx,
               unsigned char *rgba) {
  unsigned rgba4[4];
  Nib top, bottom;
  int y, i;
  if (!p || !f || !idx || !rgba || f->w <= 0 || f->h <= 0) return 0;
  for (i = 0; i < 4; i++) {
    unsigned c = idx->palette[f->color[i] & 15];
    rgba4[i] = (c << 8) | (unsigned)(f->alpha[i] * 17);
  }
  top.p = bottom.p = p; top.n = bottom.n = n;
  top.nib = f->top * 2; bottom.nib = f->bottom * 2;
  for (y = 0; y < f->h; y++)
    if (!decodeLine(y & 1 ? &bottom : &top, rgba + (size_t)y * f->w * 4, f->w, rgba4)) {
      // A packet cut short leaves the lines it did not reach transparent
      // rather than failing the whole picture.
      memset(rgba + (size_t)y * f->w * 4, 0, (size_t)(f->h - y) * f->w * 4);
      break;
    }
  return 1;
}
