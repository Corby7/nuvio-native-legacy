#include "resume.h"
#include "layout.h"
#include "text.h"
#include "anim.h"
#include <stdio.h>
#include <string.h>

void resume_draw(const CatItem *ci, GfxRect r) {
  if (!ci) return;
  float esc = r.w / NV_HIGHLIGHT_W;
  float dflt = NV_CW_DFLT * esc, width = r.w - dflt * 2;
  gfx_rect(r, 0, GFX_VEIL, 0, 0, 0, NV_RADIUS_CARD, 0, 0, 0, .85f);

  // A compact rectangle, not a pill. Never invent a premiere status.
  if (ci->remainingMin > 0) {
    char badge[48];
    int h = ci->remainingMin / 60, m = ci->remainingMin % 60;
    if (h && m) snprintf(badge, sizeof badge, "%dh %dmin Restantes", h, m);
    else if (h) snprintf(badge, sizeof badge, "%dh Restantes", h);
    else snprintf(badge, sizeof badge, "%dmin Restantes", m);
    float px = NV_CW_BADGE_DFLT_X * esc, py = NV_CW_BADGE_DFLT_Y * esc;
    TxtLine l = txt_line_trim(TXT_CW_BADGE, badge, 242, 243, 247, 255, width - 2*px);
    if (l.tex) {
      GfxRect b = {r.x + r.w - dflt - l.w - 2*px, r.y + dflt, l.w + 2*px, l.h + 2*py};
      gfx_color(b, NV_CW_BADGE_RADIUS * esc / b.h, .055f, .055f, .065f, .84f);
      txt_draw_alpha(l, b.x + px, b.y + py, 1);
    }
  }

  float base = r.y + r.h - 30*esc;
  int series = !strcmp(ci->kind, "series") && ci->season > 0 && ci->episode > 0;
  if (series && ci->nameEpisode[0]) {
    TxtLine ep = txt_line_trim(TXT_CW_META, ci->nameEpisode, 230, 232, 238, 255, width);
    base -= ep.h;
    txt_draw_alpha(ep, r.x + dflt, base, 1);
    base -= 4*esc;
  }
  TxtLine title = txt_line_trim(TXT_CW_TITLE, ci->title, 247, 248, 250, 255, width);
  base -= title.h;
  txt_draw_alpha(title, r.x + dflt, base, 1);
  if (series) {
    char te[24]; snprintf(te, sizeof te, "T%d:E%d", ci->season, ci->episode);
    TxtLine ep = txt_line(TXT_CW_META, te, 230, 232, 238, 255);
    txt_draw_alpha(ep, r.x + dflt, base - ep.h - 4*esc, 1);
  }

  // A thin line, inset from the frame. The empty part never becomes a grey bar.
  if (ci->progress > 0) {
    float h = NV_CW_BAR_H * esc;
    float filled = width * anim_clamp(ci->progress / 100.f, 0, 1);
    if (filled < h) filled = h;
    GfxRect bar = {r.x + dflt, r.y + r.h - NV_CW_BAR_BOTTOM*esc - h, filled, h};
    gfx_color(bar, .5f, .96f, .965f, .98f, .98f);
  }
}
