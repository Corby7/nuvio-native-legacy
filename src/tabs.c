#include "tabs.h"
#include "gfx.h"
#include "text.h"
#include "layout.h"

float tab_width(const char *name) {
  return (float)txt_line(TXT_SRC_TAB, name, 255, 255, 255, 255).w + NV_TAB_GAP;
}

float tab_draw(float x, float y, const char *name, int on, int cursor, float a) {
  int c = on ? 245 : 128;
  TxtLine l = txt_line(TXT_SRC_TAB, name, c, c, c, 255);
  float inset = txt_cap_inset(TXT_SRC_TAB), cap = txt_baseline(TXT_SRC_TAB) - inset;
  float ty = y + (NV_TAB_H - NV_TAB_LINE_GAP - NV_TAB_LINE - cap) * 0.5f - inset;
  // No plate or ring for the cursor: the strip is words and a rule and nothing
  // else. Being on the strip shows as the chosen tab turning as you press.
  (void)cursor;
  txt_draw_alpha(l, x, ty, a);
  if (on)
    gfx_color((GfxRect){ x, y + NV_TAB_H - NV_TAB_LINE, (float)l.w, NV_TAB_LINE },
              0.5f, 0.96f, 0.96f, 0.96f, a);
  return (float)l.w + NV_TAB_GAP;
}
