#include "resume.h"
#include "layout.h"
#include "text.h"
#include "anim.h"
#include "settings.h"
#include <stdio.h>
#include <string.h>

void resume_draw(const CatItem *ci, GfxRect r) {
  if (!ci) return;
  float scale = r.w / NV_HIGHLIGHT_W;
  float pad = NV_CW_PAD * scale, width = r.w - pad * 2;
  // The card's OWN radius, not the NV_RADIUS_CARD constant. The corner comes
  // from `posterCardCornerRadiusDp` (12dp x 2 = 24px by default) while the
  // constant is 0.055 of the height — 13px on this card. The veil is 85% black
  // and it was being drawn with the tighter corner ON TOP of the wider one, so
  // four dark wedges sat outside the card's outline, right where the focus ring
  // is. Same expression as home.c's radiusOf: the shader's radius is a fraction
  // of the height.
  float radius = r.h > 0.0f ? settings_radius_poster_px() / r.h : NV_RADIUS_CARD;
  gfx_rect(r, 0, GFX_VEIL, 0, 0, 0, radius, 0, 0, 0, .85f);

  // A compact rectangle, not a pill. Never invent a premiere status.
  if (ci->remainingMin > 0) {
    char badge[48];
    int h = ci->remainingMin / 60, m = ci->remainingMin % 60;
    if (h && m) snprintf(badge, sizeof badge, "%dh %dm left", h, m);
    else if (h) snprintf(badge, sizeof badge, "%dh left", h);
    else snprintf(badge, sizeof badge, "%dm left", m);
    float px = NV_CW_BADGE_PAD_X * scale, py = NV_CW_BADGE_PAD_Y * scale;
    TxtLine l = txt_line_trim(TXT_CW_BADGE, badge, 242, 243, 247, 255, width - 2*px);
    if (l.tex) {
      GfxRect b = {r.x + r.w - pad - l.w - 2*px, r.y + pad, l.w + 2*px, l.h + 2*py};
      gfx_color(b, NV_CW_BADGE_RADIUS * scale / b.h, .055f, .055f, .065f, .84f);
      txt_draw_alpha(l, b.x + px, b.y + py, 1);
    }
  }

  float base = r.y + r.h - 30*scale;
  int series = !strcmp(ci->kind, "series") && ci->season > 0 && ci->episode > 0;
  if (series && ci->nameEpisode[0]) {
    TxtLine ep = txt_line_trim(TXT_CW_META, ci->nameEpisode, 230, 232, 238, 255, width);
    base -= ep.h;
    txt_draw_alpha(ep, r.x + pad, base, 1);
    base -= 4*scale;
  }
  TxtLine title = txt_line_trim(TXT_CW_TITLE, ci->title, 247, 248, 250, 255, width);
  base -= title.h;
  txt_draw_alpha(title, r.x + pad, base, 1);
  if (series) {
    char te[24]; snprintf(te, sizeof te, "S%d:E%d", ci->season, ci->episode);
    TxtLine ep = txt_line(TXT_CW_META, te, 230, 232, 238, 255);
    txt_draw_alpha(ep, r.x + pad, base - ep.h - 4*scale, 1);
  }

  // A thin line, inset from the frame. The empty part never becomes a grey bar.
  if (ci->progress > 0) {
    float h = NV_CW_BAR_H * scale;
    float filled = width * anim_clamp(ci->progress / 100.f, 0, 1);
    if (filled < h) filled = h;
    GfxRect bar = {r.x + pad, r.y + r.h - NV_CW_BAR_BOTTOM*scale - h, filled, h};
    gfx_color(bar, .5f, .96f, .965f, .98f, .98f);
  }
}
