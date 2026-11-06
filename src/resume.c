#include "resume.h"
#include "layout.h"
#include "text.h"
#include "anim.h"
#include "settings.h"
#include "tex_cache.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <SDL2/SDL.h>

// HALF-LEADING: the gap CSS leaves above a line inside its own line box, which is
// what the port has to add once it stacks boxes rather than glyphs. Without it
// every line sits a little high in its box and the three of them drift apart down
// the block.
//
// SDL_ttf renders every string to a surface exactly TTF_FontHeight() tall — the
// glyphs move inside it, the box does not — so ANY string in a style answers the
// question, and a one-character one costs a single cached entry per style rather
// than a texture per title.
//
// A probe that fails to rasterise answers 0 rather than half a line box, which
// would drop the text visibly instead of by the pixel this is worth.
static float halfLead(TxtStyle style, float box) {
  TxtLine probe = txt_line(style, "H", 255, 255, 255, 255);
  if (probe.h <= 0) return 0.0f;
  float half = (box - (float)probe.h) * 0.5f;
  return half > 0.0f ? half : 0.0f;
}

// THE BAR MOVES WHEN THE PROGRESS DOES. A progress figure changes while its bar
// is off screen — the player writes it, or a sync brings it in — and coming back
// to the card the fill was simply longer, with nothing to say that anything had
// happened. Each bar remembers what it last drew and springs to the new figure,
// so the viewer arriving back at the home SEES the twenty minutes they watched.
//
// Keyed by title and episode, in a small table with no dt of its own: the draw
// has none, so each entry keeps the tick it was last drawn at. A bar not drawn for
// a while comes back with its step clamped to one frame, which is the point — it
// starts from where it was and runs, rather than arriving already there. The first
// sighting of a bar is not a change and draws the figure as it is.
#define FILL_SLOTS 64
static struct { unsigned long key; float shown; Uint32 at; } fills[FILL_SLOTS];
static int fillNext;

float resume_fill(const char *id, int season, int episode, float target) {
  unsigned long h = 2166136261u;
  Uint32 now = SDL_GetTicks();
  for (const char *p = id ? id : ""; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
  h = (h ^ (unsigned long)season) * 16777619u;
  h = (h ^ (unsigned long)episode) * 16777619u;
  if (!h) h = 1;
  for (int i = 0; i < FILL_SLOTS; i++) {
    if (fills[i].key != h) continue;
    float dt = (float)(now - fills[i].at) / 1000.0f;
    if (dt > 0.05f) dt = 0.05f;
    fills[i].shown = settings_animations_reduced()
                   ? target : anim_spring(fills[i].shown, target, dt, NV_SPRING_PROGRESS);
    if (fabsf(fills[i].shown - target) < 0.0005f) fills[i].shown = target;
    fills[i].at = now;
    return fills[i].shown;
  }
  fills[fillNext].key = h; fills[fillNext].shown = target; fills[fillNext].at = now;
  fillNext = (fillNext + 1) % FILL_SLOTS;
  return target;
}

int resume_bar(const CatItem *ci, GfxRect r, float *band, float *fill) {
  float scale = r.w / NV_HIGHLIGHT_W, min;
  *band = 0.0f; *fill = 0.0f;
  // See the note on the bar at the end of resume_draw for the 2% line.
  if (!ci || ci->progress <= NV_CW_BAR_MIN_PCT || r.h <= 0.0f || r.w <= 0.0f) return 0;
  *band = NV_CW_FILL_H * scale / r.h;
  *fill = resume_fill(ci->imdb[0] ? ci->imdb : ci->title, ci->season, ci->episode,
                      anim_clamp(ci->progress / 100.f, 0, 1));
  min = NV_CW_BAR_MINW * scale / r.w;
  if (*fill < min) *fill = min;
  return 1;
}

void resume_draw(const CatItem *ci, GfxRect r, int baked, float focus) {
  if (!ci) return;
  float scale = r.w / NV_HIGHLIGHT_W;
  if (focus < 0.0f) focus = 0.0f;
  if (focus > 1.0f) focus = 1.0f;
  // The card's OWN radius, not the NV_RADIUS_CARD constant. The corner comes
  // from `posterCardCornerRadiusDp` (12dp x 2 = 24px by default) while the
  // constant is 0.055 of the height — 13px on this card. The scrim and the bar
  // are both cut by this radius, and drawing them with the tighter corner ON TOP
  // of the wider one leaves dark wedges outside the card's outline, right where
  // the focus ring is. Same expression as home.c's radiusOf: the shader's radius
  // is a fraction of the height.
  float radius = r.h > 0.0f ? settings_radius_poster_px() / r.h : NV_RADIUS_CARD;

  // THE SCRIM, and not a flat veil. See the note on GFX_CW_SCRIM in gfx.h.
  // Over art drawn as GFX_CW_CARD it is already in the art's colour.
  if (!baked) gfx_rect(r, 0, GFX_CW_SCRIM, 0, 0, 0, radius, 0, 0, 0, 1.0f);

  // THE STATUS: "34M LEFT", "1H 12M LEFT" or "UP NEXT", ONLY ON THE FOCUSED
  // CARD, as its own small label above the title.
  //
  // On every card it made a busy row; the owner wanted it on focus only. At the
  // end of the episode line it cut the episode's name short to make room, so it
  // has a line of its own, which never trims anything.
  //
  // The top-right corner has been turned down five times before (see git
  // history): do not put a badge on the art without asking.
  //
  // "UP NEXT" IS NOT A TIME. An episode nobody has opened has its whole runtime
  // left, so "22M LEFT" there is a true number and a false sentence.
  //
  // Never invent a status: with no time known, nothing is drawn.
  char status[24] = "";
  if (ci->progress == 0) snprintf(status, sizeof status, "UP NEXT");
  else if (ci->remainingMin > 0) {
    int h = ci->remainingMin / 60, m = ci->remainingMin % 60;
    if (h && m) snprintf(status, sizeof status, "%dH %dM LEFT", h, m);
    else if (h) snprintf(status, sizeof status, "%dH LEFT", h);
    else snprintf(status, sizeof status, "%dM LEFT", m);
  }

  // THE COPY: the status (focused only), the title, the episode line — built
  // BOTTOM-UP from 22px above the base, as `.home-continue-copy` anchors it, so a
  // second line of title grows UPWARD into the scrim instead of pushing the
  // episode line onto the bar.
  float padX  = NV_CW_COPY_X * scale;
  float width = r.w - padX * 2;
  float x     = r.x + padX;
  float base  = r.y + r.h - NV_CW_COPY_BOTTOM * scale;

  int series = !strcmp(ci->kind, "series") && ci->episode > 0;
  int sub = series;

  // THE EPISODE LINE: "S2 E4  ·  Woe's Hollow" on ONE line — the code in bold
  // white, the episode's name regular and dimmed after it. Picked by the owner
  // over the code as a tracked-out kicker above the title, a single dimmed style
  // for both, and a brighter or smaller line. The code is `formatEpisodeCode`: a
  // space, not a colon, the episode alone when the season is not known. Only the
  // name is ever trimmed; the code always shows whole.
  if (sub) {
    char code[24];
    if (ci->season > 0) snprintf(code, sizeof code, "S%d E%d", ci->season, ci->episode);
    else                snprintf(code, sizeof code, "E%d", ci->episode);
    float box = NV_CW_SUB_LH * scale;
    float y = base - box + halfLead(TXT_CWC_SUB, box);
    TxtLine lc = txt_line(TXT_CWC_CODE, code, 255, 255, 255, 255);
    txt_draw(lc, x, y);
    if (ci->nameEpisode[0]) {
      char rest[160];
      snprintf(rest, sizeof rest, "  \xC2\xB7  %s", ci->nameEpisode);
      float room = width - (float)lc.w;
      if (room > 0.0f) {
        TxtLine ln = txt_line_trim(TXT_CWC_SUB, rest, 255, 255, 255, 255, room);
        txt_draw_alpha(ln, x + (float)lc.w, y, NV_CW_DIM);
      }
    }
    base -= box + NV_CW_SUB_GAP * scale;
  }

  // THE LOGO IN THE TITLE'S PLACE, when Settings asks for it. It is placed by
  // its VISIBLE box (tex_content_box), not the file's: logos arrive with any
  // amount of transparent margin, and placing the file put that margin between
  // the mark and the episode name. The quad is the whole texture, stretched so
  // the visible part lands exactly on the target box — the margin falls outside
  // it and is transparent, so nothing needs cropping.
  //
  // While the logo is still on its way the space stays empty rather than
  // flashing the name first; only a logo the cache has given up on (or none at
  // all) falls back to the name.
  //
  // Asked for at the card's RESTING width, with room for the focus scale: `r.w`
  // grows while the card takes focus, and a width that moves would promote the
  // entry and decode it again mid-animation (see the note on wAsk in home.c).
  int logo = settings_cw_logo() && ci->logo[0] && !tex_failed(ci->logo);
  if (logo) {
    // The subtitle's line box carries half-leading above its glyphs, but the
    // mark's visible edge has none: without this the logo sat on the text.
    if (sub) base -= NV_CW_LOGO_GAP * scale;
    GLuint t = tex_get_width(ci->logo, NV_HIGHLIGHT_W * NV_CW_LOGO_MAXW * 1.1f);
    float ap = t ? tex_aspect(ci->logo) : 0.0f;
    float vb[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    float used = NV_CW_LOGO_WAIT_H * scale;
    if (ap > 0.0f) {
      tex_content_box(ci->logo, vb);
      float bw = vb[2] - vb[0], bh = vb[3] - vb[1];
      float av = ap * bw / bh;                      // the visible mark's aspect
      float hL = sqrtf(NV_CW_LOGO_AREA / av) * scale, wL = hL * av;
      float maxH = NV_CW_LOGO_H * scale, maxW = r.w * NV_CW_LOGO_MAXW;
      if (hL > maxH) { hL = maxH; wL = hL * av; }
      if (wL > maxW) { wL = maxW; hL = wL / av; }
      float qw = wL / bw, qh = hL / bh;
      GfxRect rl = { x - vb[0] * qw, base - hL - vb[1] * qh, qw, qh };
      // GFX_BRAND/GFX_TEXT and not GFX_CARD, for the reason the open card in
      // home.c gives: the card mode crops and throws the alpha away.
      GfxMode m = tex_brand_dark(ci->logo) ? GFX_BRAND : GFX_TEXT;
      gfx_tex_aspect_current = 0.0f;
      gfx_rect(rl, t, m, 0, 0, 0, 0.0f, 1, 1, 1, 1.0f);
      used = hL;
    }
    base -= used;
  } else {
    // The title wraps to NV_CW_TITLE_LINES and the block grows UPWARD into the
    // scrim. txt_block always draws, so the height comes from a measuring pass
    // first (txt_block_dir with a negative x); the drawing pass is a cache hit.
    float lead = NV_CW_TITLE_LH * scale;
    float used = txt_block_dir(TXT_CWC_TITLE, ci->title, 255, 255, 255,
                               -1.0f, 0.0f, width, lead, 1.0f, NV_CW_TITLE_LINES);
    if (used < lead) used = lead;
    txt_block(TXT_CWC_TITLE, ci->title, 255, 255, 255,
              x, base - used + halfLead(TXT_CWC_TITLE, lead),
              width, lead, 1.0f, NV_CW_TITLE_LINES);
    base -= used;
  }

  // THE STATUS LABEL, above the title, on the focused card only: tracked-out
  // capitals at 17/600, dimmed, fading in with the focus. Nothing else moves when
  // it arrives — the block is built bottom-up, so it simply takes the space
  // above the title.
  if (status[0] && focus > 0.01f) {
    float box = NV_CW_KICKER_LH * scale;
    base -= NV_CW_TITLE_GAP * scale;
    txt_tracking(TXT_CWC_KICKER, status, 255, 255, 255,
                 x, base - box + halfLead(TXT_CWC_KICKER, box),
                 NV_CW_DIM * focus, NV_CW_KICKER_TRACK * scale);
  }

  // THE PROGRESS BAR: fill only, along the base — and ONLY on something that has
  // actually been started.
  //
  // A DELIBERATE DIVERGENCE from the web, which renders `.home-continue-progress`
  // unconditionally with a 12px stub at 0%. The owner's call is that an episode
  // nobody has opened must not claim a position. The line is 2%, not 1: trakt.c
  // truncates /sync/playback's float and a play pressed and thought better of
  // arrives as 1%. detail.c's episode card draws the same line at 2.
  //
  // Over art the bar is baked into GFX_CW_CARD. A card with no art falls back to
  // GFX_CW_BAR, which still draws its faint track: that shader is shared with the
  // skip button's countdown.
  { float band, fill;
    if (!baked && resume_bar(ci, r, &band, &fill))
      gfx_rect(r, 0, GFX_CW_BAR, 0, band, fill, radius, 1, 1, 1, 1.0f); }
}
