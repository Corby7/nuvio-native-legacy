#include "tabs.h"
#include "gfx.h"
#include "text.h"
#include "layout.h"
#include "anim.h"

// The two sizes of the strip. Everything else — the colours, the lit state, the
// rule — is one implementation, so the sheets and the Library cannot drift.
typedef struct { TxtStyle font; float h, gap, line, lineGap; int idle; } TabSize;
static const TabSize SHEET = { TXT_SRC_TAB, NV_TAB_H, NV_TAB_GAP, NV_TAB_LINE,
                               NV_TAB_LINE_GAP, 128 };
static const TabSize PAGE  = { TXT_LIB_TAB, NV_LIB_TAB_H, NV_LIB_TAB_GAP,
                               NV_LIB_TAB_LINE, NV_LIB_TAB_LINE_GAP, 110 };

static float width(const TabSize *z, const char *name) {
  return (float)txt_line(z->font, name, 255, 255, 255, 255).w + z->gap;
}

// THE CURSOR SHOWS AS THE CHOSEN TAB LIGHTING UP. No plate or ring: the strip is
// words and a rule. Lit, the chosen word is white and its rule solid; unlit it
// drops to grey and the rule to a trace — still marking which tab is in force,
// without claiming the focus that is somewhere below. Before this the chosen tab
// looked the same either way, and with the cursor on the strip nothing on screen
// said so.
static float draw(const TabSize *z, float x, float y, const char *name, int on,
                  float lit, float a) {
  int c = on ? (int)anim_blend(150.0f, 255.0f, lit) : z->idle;
  TxtLine l = txt_line(z->font, name, c, c, c, 255);
  float inset = txt_cap_inset(z->font), cap = txt_baseline(z->font) - inset;
  float ty = y + (z->h - z->lineGap - z->line - cap) * 0.5f - inset;
  txt_draw_alpha(l, x, ty, a);
  if (on)
    gfx_color((GfxRect){ x, y + z->h - z->line, (float)l.w, z->line },
              0.5f, 0.96f, 0.96f, 0.96f, anim_blend(0.30f, 1.0f, lit) * a);
  return (float)l.w + z->gap;
}

float tab_width(const char *name) { return width(&SHEET, name); }
float tab_draw(float x, float y, const char *name, int on, float lit, float a) {
  return draw(&SHEET, x, y, name, on, lit, a);
}
float tab_page_width(const char *name) { return width(&PAGE, name); }
float tab_page_draw(float x, float y, const char *name, int on, float lit, float a) {
  return draw(&PAGE, x, y, name, on, lit, a);
}
