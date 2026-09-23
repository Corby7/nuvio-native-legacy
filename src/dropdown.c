#include "dropdown.h"
#include "text.h"
#include "anim.h"
#include "layout.h"
#include <stddef.h>

void dd_pill(GfxRect r, const char *label, const char *value,
             float focus, int active, float alpha) {
  float f = focus;
  if (alpha <= 0.004f) return;

  // #222 at rest going to --focus-bg #303030 on focus, with the 1px edge drawn
  // as the gap between two shapes — a 1px stroke on a 100-tall pill cannot be
  // antialiased and breaks up around the cap. The measurement is in search.c.
  { float fill = anim_blend(0.133f, 0.188f, f);
    float edge = 0.2f;
    gfx_color(r, NV_DD_PILL, edge, edge, edge, alpha);
    { GfxRect in = { r.x + 1.0f, r.y + 1.0f, r.w - 2.0f, r.h - 2.0f };
      gfx_color(in, NV_DD_PILL, fill, fill, fill, alpha); } }
  if (f > 0.01f)
    gfx_rect(r, 0, GFX_RING_INSET, 0, NV_DD_RING / r.h, 0,
             NV_DD_PILL, 1.0f, 1.0f, 1.0f, 0.96f * f * alpha);

  { // 20/500 --text-tertiary over 28/500 white, 4 apart, the pair centred in
    // the pill as a block rather than each line on its own. The value is
    // trimmed short of the chevron, not of the pill: a name long enough to
    // reach it would otherwise print straight through the glyph.
    //
    // AN INACTIVE PICKER'S VALUE IS THE LABEL'S GREY, not white. With two
    // pickers side by side and one grid under them, white on both says both are
    // in effect and neither says which list is on screen.
    int ink = active ? 255 : 128;
    float textW = r.w - 2 * NV_DD_PICK_PADX - NV_DD_CHEV - 16.0f;
    TxtLine tl = txt_line(TXT_SRCH_META, label, 128, 128, 128, 255);
    TxtLine tv = txt_line_trim(TXT_CALLOUT, value, ink, ink, ink, 255, textW);
    float block = (float)tl.h + NV_DD_COPY_GAP + (float)tv.h;
    float ty = r.y + (r.h - block) * 0.5f;
    txt_draw_alpha(tl, r.x + NV_DD_PICK_PADX, ty, 0.95f * alpha);
    txt_draw_alpha(tv, r.x + NV_DD_PICK_PADX, ty + (float)tl.h + NV_DD_COPY_GAP,
                   alpha); }

  // THE CHEVRON IS WHAT SAYS "THIS OPENS". Without it the pill reads as a label
  // with a value in it, and nothing on screen suggests OK would do anything.
  // It points DOWN whether the list is open or shut — the web app does not flip
  // it either, and the season picker's note records the same choice.
  { GfxRect ch = { r.x + r.w - NV_DD_PICK_PADX - NV_DD_CHEV,
                   r.y + (r.h - NV_DD_CHEV) * 0.5f, NV_DD_CHEV, NV_DD_CHEV };
    float luma = anim_blend(0.702f, 0.902f, f);
    gfx_icon(ch, "chevron_down", luma, luma, luma, alpha); }
}

// "value · tail": the value in Medium, the dot NV_DD_SEL_DOT each side. Returns
// the width; `draw` 0 only measures.
static float selectCopy(const char *value, const char *tail, float x, float yc,
                        float a, int draw) {
  TxtLine lv = txt_line(TXT_DD_SEL, value, 255, 255, 255, 255);
  float w = (float)lv.w;
  if (draw) txt_draw_alpha(lv, x, yc - (float)lv.h * 0.5f, a);
  if (tail && *tail) {
    TxtLine ld = txt_line(TXT_DETWEB_SEA_EPS, "\xc2\xb7", 179, 179, 179, 255);
    TxtLine lt = txt_line(TXT_DETWEB_SEA_EPS, tail, 179, 179, 179, 255);
    if (draw) {
      txt_draw_alpha(ld, x + w + NV_DD_SEL_DOT, yc - (float)ld.h * 0.5f, a);
      txt_draw_alpha(lt, x + w + NV_DD_SEL_DOT * 2 + (float)ld.w,
                     yc - (float)lt.h * 0.5f, a);
    }
    w += NV_DD_SEL_DOT * 2 + (float)ld.w + (float)lt.w;
  }
  return w;
}

void dd_select(GfxRect r, const char *value, const char *tail, float focus,
               float alpha) {
  float f = focus;
  if (alpha <= 0.004f) return;
  // #222 lifting to rgb(48,48,48); the hair line at rest, which the INSET ring
  // replaces rather than sits on. GFX_RING strokes across the quad's edge and a
  // pill's outline touches it top and bottom — see drawSeason in detail.c.
  { float luma = anim_blend(0.133f, NV_DD_SEL_FOCUS_BG, f);
    gfx_color(r, NV_RADIUS_PILL, luma, luma, luma, alpha); }
  if (f < 0.99f)
    gfx_rect(r, 0, GFX_RING, 0, NV_DD_SEL_BORDER / r.h, 0, NV_RADIUS_PILL,
             1, 1, 1, 0.10f * (1.0f - f) * alpha);
  if (f > 0.01f)
    gfx_rect(r, 0, GFX_RING_INSET, 0, NV_DD_RING / r.h, 0, NV_RADIUS_PILL,
             1, 1, 1, 0.96f * f * alpha);

  selectCopy(value, tail, r.x + NV_DD_SEL_PADX, r.y + r.h * 0.5f, alpha, 1);
  { GfxRect ch = { r.x + r.w - NV_DD_SEL_PADX - NV_DD_CHEV,
                   r.y + (r.h - NV_DD_CHEV) * 0.5f, NV_DD_CHEV, NV_DD_CHEV };
    gfx_icon(ch, "chevron_down", 0.702f, 0.702f, 0.702f, alpha); }
}

float dd_select_width(int n, DdLabel label, void *ctx) {
  float widest = 0.0f;
  int i;
  for (i = 0; i < n; i++) {
    float w = selectCopy(label(ctx, i), NULL, 0, 0, 0, 0);
    if (w > widest) widest = w;
  }
  return NV_DD_SEL_PADX * 2 + widest + NV_DD_SEL_GAP + NV_DD_CHEV;
}

void dd_menu(GfxRect anchor, int n, int focus, DdLabel label, void *ctx,
             float alpha) {
  int vis, first, i;
  if (n <= 0 || !label || alpha <= 0.004f) return;
  vis = n < NV_DD_OPT_VIS ? n : NV_DD_OPT_VIS;

  { float h = NV_DD_MENU_PADY * 2 + vis * NV_DD_OPT_H;
    GfxRect box = { anchor.x, anchor.y + anchor.h + NV_DD_MENU_GAP,
                    anchor.w, h };
    // The radius is 64 CSS px on a box far taller than 128, and gfx normalises
    // the radius to the HEIGHT — so it is 64/h here and NOT the pill constant.
    // At 0.5 a box this tall rounds into a lozenge. Same trap as the season
    // menu's, and recorded there too.
    float radius = 64.0f / box.h;
    // The drop shadow first, then the plate: `0 8px 32px rgba(0,0,0,.6)`.
    // gfx_drop_shadow, NOT GFX_SHADOW — see GFX_DROP in gfx.h for the square
    // corners that one left under the plate.
    gfx_drop_shadow(box, 64.0f, 16.0f, 8.0f, 0.6f * alpha);
    gfx_color(box, radius, 0.133f, 0.133f, 0.133f, alpha);
    gfx_rect(box, 0, GFX_RING, 0, 1.0f / box.h, 0, radius, 1, 1, 1,
             0.08f * alpha);

    // Which six. The focused option is kept in view by scrolling the WINDOW,
    // not by moving the menu.
    first = focus - vis + 1;
    if (first < 0) first = 0;
    if (first > n - vis) first = n - vis;
    if (focus < first) first = focus;

    for (i = 0; i < vis; i++) {
      int c = first + i;
      GfxRect op = { box.x + NV_DD_MENU_PADX,
                     box.y + NV_DD_MENU_PADY + i * NV_DD_OPT_H,
                     box.w - NV_DD_MENU_PADX * 2, NV_DD_OPT_H };
      int on = (c == focus);
      // The focused row inverts to #f5f5f5 with #111 ink; the rest are
      // transparent with white. The CURRENT value gets no mark of its own — the
      // list opened with the focus already on it.
      if (on) gfx_color(op, NV_RADIUS_PILL, 0.961f, 0.961f, 0.961f, alpha);
      { int ink = on ? 17 : 255;
        TxtLine l = txt_line_trim(TXT_DETWEB_OPT, label(ctx, c),
                                  ink, ink, ink, 255,
                                  op.w - NV_DD_OPT_PADX * 2);
        txt_draw_alpha(l, op.x + NV_DD_OPT_PADX,
                       op.y + (op.h - (float)l.h) * 0.5f, alpha); }
    } }
}
