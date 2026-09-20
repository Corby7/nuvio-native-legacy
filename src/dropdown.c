#include "dropdown.h"
#include "text.h"
#include "anim.h"
#include "layout.h"

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
    // The drop shadow first, then the plate. GFX_SHADOW multiplies by uFocus
    // and not by the colour's alpha, so it takes 1.0 there — passed the 0 that
    // every other mode here takes, the blot comes out invisible.
    { GfxRect sh = { box.x, box.y + 8.0f, box.w, box.h };
      gfx_rect(sh, 0, GFX_SHADOW, 1.0f, 0, 0, radius, 0, 0, 0, 0.6f * alpha); }
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
