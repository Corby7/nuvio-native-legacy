// The poster grid's column count and the geometry that follows from it. See
// gridsize.h.
#include "gridsize.h"
#include "data.h"
#include "text.h"
#include "anim.h"
#include "layout.h"
#include "tex_cache.h"
#include <stdio.h>
#include <stdlib.h>

#define GRID_FILE "grid-columns.txt"

// 0 until the file has been read; read on first use rather than at startup so
// no screen has to remember to call a start function.
static int cols;

int grid_cols(void) {
  if (!cols) {
    char *s = data_read(GRID_FILE);
    int v = s ? atoi(s) : 0;
    free(s);
    cols = (v >= GRID_COLS_MIN && v <= GRID_COLS_MAX) ? v : NV_DSC_COLUMNS;
  }
  return cols;
}

float grid_card_w(void) {
  int c = grid_cols();
  return (NV_DSC_W - (float)(c - 1) * NV_DSC_CARD_GAP) / (float)c;
}
float grid_card_step(void) { return grid_card_w() + NV_DSC_CARD_GAP; }
float grid_poster_h(void)  { return grid_card_w() * 1.5f; }
float grid_line_step(void) {
  return grid_poster_h() + NV_DSC_TITLE_GAP + NV_DSC_LD_TITLE + NV_DSC_ROW_GAP;
}

void grid_warm(const char *art, float top, float width, float step) {
  if (!art || !art[0]) return;
  // One and a half rows each way decoded: the next row down, and the row just
  // scrolled away above, which UP brings straight back.
  if (top < NV_SCREEN_H + step * 1.5f && top + step > -step * 1.5f)
    tex_get_width(art, width);
  else if (top >= NV_SCREEN_H && top < NV_SCREEN_H + step * 4.0f)
    tex_prefetch(art);
}

void grid_cycle(void) {
  char buf[8];
  cols = grid_cols() + 1;
  if (cols > GRID_COLS_MAX) cols = GRID_COLS_MIN;
  snprintf(buf, sizeof buf, "%d\n", cols);
  if (!data_write(GRID_FILE, buf))
    printf("[grid] could not store %d columns\n", cols);
  printf("[grid] %d per row\n", cols);
}

// The search header's button language: #333 at rest going to a white fill on
// focus, with the ink inverting — so it reads as the same kind of control.
#define GRID_BTN_H     64.0f
#define GRID_BTN_PAD   24.0f
#define GRID_BTN_ICON  32.0f
#define GRID_BTN_GAP   12.0f

GfxRect grid_button_draw(float right, float y, float focus, float alpha) {
  char label[16];
  TxtLine t;
  GfxRect r;
  float ink = anim_blend(1.0f, 0.055f, focus);
  int c = (int)(ink * 255.0f + 0.5f);
  snprintf(label, sizeof label, "%d per row", grid_cols());
  t = txt_line(TXT_SRCH_NAME, label, c, c, c, 255);
  r.w = GRID_BTN_PAD * 2.0f + GRID_BTN_ICON + GRID_BTN_GAP + (float)t.w;
  r.h = GRID_BTN_H;
  r.x = right - r.w;
  r.y = y;
  gfx_color(r, 0.5f, anim_blend(0.2f, 1.0f, focus), anim_blend(0.2f, 1.0f, focus),
            anim_blend(0.2f, 1.0f, focus), alpha);
  gfx_icon((GfxRect){ r.x + GRID_BTN_PAD, r.y + (r.h - GRID_BTN_ICON) * 0.5f,
                      GRID_BTN_ICON, GRID_BTN_ICON },
           "ctx_grid", ink, ink, ink, alpha);
  txt_draw_alpha(t, r.x + GRID_BTN_PAD + GRID_BTN_ICON + GRID_BTN_GAP,
                 r.y + (r.h - (float)t.h) * 0.5f, alpha);
  return r;
}

// Centred in Library's and Discover's 64px right gutter (NV_DSC_X), the only
// room right of those grids; the collection grid's gutter is wider still.
#define GRID_BAR_AXIS (NV_SCREEN_W - NV_DSC_X * 0.5f)

void grid_bar_draw(ScrollBar *b, int row, int rows, float step, float top,
                   float bottom, float alpha, Uint32 now) {
  // ONE SCREENFUL: nothing to report.
  if (rows < 1 || (float)rows * step - (step - grid_poster_h()) <= NV_SCREEN_H - top) {
    b->seen = 0;
    return;
  }
  scrollbar_rows(b, GRID_BAR_AXIS, top, bottom, row, rows,
                 (NV_SCREEN_H - top) / step, alpha, now);
}
