// The scrollbar, segmented and continuous. See scrollbar.h.
#include "scrollbar.h"
#include "gfx.h"
#include "text.h"
#include "anim.h"
#include "settings.h"
#include <stdio.h>

#define SB_W          6.0f
#define SB_LIVE_W     10.0f
#define SB_GAP        8.0f
#define SB_SEG_MIN    24.0f    // below this, the continuous bar
#define SB_THUMB_MIN  56.0f
#define SB_REST       0.30f
#define SB_BEHIND     0.26f
#define SB_AHEAD      0.10f
#define SB_GLOW       18.0f
// The settle, in ms since the last move: the chip slides in over IN, holds until
// HOLD, goes over CHIP_OUT, then the bar fades over BAR_OUT.
#define SB_IN         150.0f
#define SB_HOLD       1200.0f
#define SB_CHIP_OUT   200.0f
#define SB_BAR_OUT    400.0f
#define SB_CHIP_H     40.0f
#define SB_CHIP_PAD   14.0f
#define SB_CHIP_GAP   16.0f    // chip to the bar
#define SB_CHIP_SLIDE 24.0f

// Where a move is in its settle: chipIn/chipA for the chip, live 1 -> 0 for the
// bar's width, light and brightness.
typedef struct { float chipIn, chipA, live; } Settle;

static Settle settle(const ScrollBar *b, Uint32 now) {
  Settle s;
  float t = b->moved ? (float)(Uint32)(now - b->moved) : 1e9f;
  s.chipIn = settings_animations_reduced() ? 1.0f
           : anim_smooth(anim_clamp(t / SB_IN, 0.0f, 1.0f));
  s.chipA  = 1.0f - anim_smooth(anim_clamp((t - SB_HOLD) / SB_CHIP_OUT, 0.0f, 1.0f));
  s.live   = 1.0f - anim_smooth(anim_clamp((t - SB_HOLD - SB_CHIP_OUT) / SB_BAR_OUT,
                                           0.0f, 1.0f));
  return s;
}

// Notes a move. The first frame is not one, so a screen opening does not flash.
static void track(ScrollBar *b, int at, float pos, Uint32 now) {
  if (b->seen && (at != b->at || pos - b->pos > 0.001f || b->pos - pos > 0.001f))
    b->moved = now ? now : 1;
  b->at = at; b->pos = pos; b->seen = 1;
}

// A PILL: fully round ends whatever the rect's shape. gfx_color's radius is a
// fraction of the rect's HEIGHT (the SDF's unit), so 0.5 only makes a pill of
// something wider than tall; on a tall segment it asked for corners bigger than
// the width and folded the shape into a pointed lens.
static void pill(GfxRect r, float a) {
  float shortSide = r.w < r.h ? r.w : r.h;
  if (r.h > 0.0f && a > 0.002f) gfx_color(r, 0.5f * shortSide / r.h, 1.0f, 1.0f, 1.0f, a);
}

// The live piece: widened and lit while it moves.
static void livePiece(float axis, float y, float h, float live, float barA, float alpha) {
  float w = anim_blend(SB_W, SB_LIVE_W, live);
  GfxRect r = { axis - w * 0.5f, y, w, h };
  if (live > 0.01f) gfx_glow(r, w * 0.5f, SB_GLOW, 1.0f, 1.0f, 1.0f, 0.22f * live * alpha);
  pill(r, barA);
}

// The count chip, its right edge at `right` and centred on `cy`.
static void chip(int at, int n, float right, float cy, float a) {
  char num[12], of[16];
  TxtLine tn, to;
  float w, x;
  snprintf(num, sizeof num, "%d", at + 1);
  snprintf(of, sizeof of, " / %d", n);
  tn = txt_line(TXT_PLR_DELTA, num, 255, 255, 255, 255);
  to = txt_line(TXT_PLR_STATL, of, 150, 150, 155, 255);
  w = SB_CHIP_PAD * 2.0f + (float)(tn.w + to.w);
  x = right - w;
  // A hairline edge: the lighter pill 1px out, the dark one over it.
  gfx_color((GfxRect){ x - 1.0f, cy - SB_CHIP_H * 0.5f - 1.0f, w + 2.0f,
                       SB_CHIP_H + 2.0f }, 0.5f, 1.0f, 1.0f, 1.0f, 0.10f * a);
  gfx_color((GfxRect){ x, cy - SB_CHIP_H * 0.5f, w, SB_CHIP_H }, 0.5f,
            0.075f, 0.075f, 0.085f, 0.96f * a);
  txt_draw_alpha(tn, x + SB_CHIP_PAD, cy - (float)tn.h * 0.5f, a);
  // The tail sits on the number's baseline, near enough: bottoms aligned.
  txt_draw_alpha(to, x + SB_CHIP_PAD + (float)tn.w,
                 cy + (float)tn.h * 0.5f - (float)to.h - 1.0f, a);
}

static void chipBeside(Settle s, int at, int n, float axis, float cy, float alpha) {
  if (n > 0 && s.chipA * s.chipIn > 0.004f)
    chip(at, n, axis - SB_LIVE_W * 0.5f - SB_CHIP_GAP + (1.0f - s.chipIn) * SB_CHIP_SLIDE,
         cy, s.chipA * s.chipIn * alpha);
}

void scrollbar_draw(ScrollBar *b, float axis, float top, float bottom,
                    float pos, float size, int at, int n, float alpha, Uint32 now) {
  float len = bottom - top, h, y, barA;
  Settle s;
  pos = anim_clamp(pos, 0.0f, 1.0f);
  track(b, n > 0 ? at : 0, pos, now);
  if (len <= SB_THUMB_MIN || alpha <= 0.004f) return;
  s = settle(b, now);
  barA = anim_blend(SB_REST, 1.0f, s.live) * alpha;
  h = len * anim_clamp(size, 0.0f, 1.0f);
  if (h < SB_THUMB_MIN) h = SB_THUMB_MIN;
  y = top + (len - h) * pos;
  // The track, and over it the part behind the thumb lit to SB_BEHIND. It runs
  // on under the thumb's middle so its round end never shows as a notch.
  pill((GfxRect){ axis - SB_W * 0.5f, top, SB_W, len }, SB_AHEAD * barA);
  pill((GfxRect){ axis - SB_W * 0.5f, top, SB_W, y + h * 0.5f - top },
       (1.0f - (1.0f - SB_BEHIND) / (1.0f - SB_AHEAD)) * barA);
  livePiece(axis, y, h, s.live, barA, alpha);
  chipBeside(s, at, n, axis, y + h * 0.5f, alpha);
}

void scrollbar_rows(ScrollBar *b, float axis, float top, float bottom,
                    int row, int rows, float onScreen, float alpha, Uint32 now) {
  float len = bottom - top, segH, barA;
  Settle s;
  if (rows < 1) return;
  if (row < 0) row = 0;
  if (row > rows - 1) row = rows - 1;
  segH = (len - SB_GAP * (float)(rows - 1)) / (float)rows;
  // TOO MANY ROWS TO SEGMENT: the continuous bar, still snapped to the row.
  if (segH < SB_SEG_MIN) {
    scrollbar_draw(b, axis, top, bottom, rows > 1 ? (float)row / (float)(rows - 1) : 0.0f,
                   onScreen / (float)rows, row, rows, alpha, now);
    return;
  }
  track(b, row, 0.0f, now);
  if (alpha <= 0.004f) return;
  s = settle(b, now);
  barA = anim_blend(SB_REST, 1.0f, s.live) * alpha;
  for (int i = 0; i < rows; i++) {
    float y = top + (float)i * (segH + SB_GAP);
    if (i == row) livePiece(axis, y, segH, s.live, barA, alpha);
    else pill((GfxRect){ axis - SB_W * 0.5f, y, SB_W, segH },
              (i < row ? SB_BEHIND : SB_AHEAD) * barA);
  }
  chipBeside(s, row, rows, axis, top + (float)row * (segH + SB_GAP) + segH * 0.5f, alpha);
}
