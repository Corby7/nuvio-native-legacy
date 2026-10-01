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
  *band = NV_CW_BAR_H * scale / r.h;
  *fill = resume_fill(ci->imdb[0] ? ci->imdb : ci->title, ci->season, ci->episode,
                      anim_clamp(ci->progress / 100.f, 0, 1));
  min = NV_CW_BAR_MINW * scale / r.w;
  if (*fill < min) *fill = min;
  return 1;
}

void resume_draw(const CatItem *ci, GfxRect r, int baked) {
  if (!ci) return;
  float scale = r.w / NV_HIGHLIGHT_W;
  // The card's OWN radius, not the NV_RADIUS_CARD constant. The corner comes
  // from `posterCardCornerRadiusDp` (12dp x 2 = 24px by default) while the
  // constant is 0.055 of the height — 13px on this card. The scrim and the bar
  // are both cut by this radius, and drawing them with the tighter corner ON TOP
  // of the wider one leaves dark wedges outside the card's outline, right where
  // the focus ring is. Same expression as home.c's radiusOf: the shader's radius
  // is a fraction of the height.
  float radius = r.h > 0.0f ? settings_radius_poster_px() / r.h : NV_RADIUS_CARD;

  // THE SCRIM, and not a flat veil. `.home-continue-media::after` is a seven-stop
  // ramp that is opaque at the base and gone by the top; what stood here was one
  // GFX_VEIL at 0.85 over the WHOLE card, so the artwork was dimmed uniformly and
  // the card read as a grey plate with a picture faintly behind it. See the note
  // on GFX_CW_SCRIM in gfx.h for the stops.
  // Over art drawn as GFX_CW_CARD it is already in the art's colour.
  if (!baked) gfx_rect(r, 0, GFX_CW_SCRIM, 0, 0, 0, radius, 0, 0, 0, 1.0f);

  // THE REMAINING TIME: A SCRIM AND THE TYPE, WITH NOTHING AROUND IT.
  //
  // This corner has now been drawn four ways — the port's flat dark rectangle, the
  // web's `.home-continue-badge` glass pill, a flat pill, and a frosted plate that
  // blurred the artwork behind it — and the owner turned down all four. Reading
  // the notes back, the objection was never the material: it was that ALL FOUR PUT
  // A BOX THERE. A box on artwork reads as a control, something that could be
  // pressed. What is being shown is a label.
  //
  // So the container is gone and the GROUND does its job: the top-right corner of
  // the card is shaded by a dome (GFX_CORNER_SCRIM) and the type floats on it. The
  // card is left with one shape fewer, and the two scrims — this one and the copy's
  // at the base — darken opposite corners and leave the middle of the frame clear.
  //
  // Do not put a plate back here without asking. It has been asked four times.
  //
  // Never invent a premiere status: with no time known, no scrim and no label.
  //
  // "NEXT UP" IS NOT A TIME, and it is the one label here that is not measured.
  // An episode nobody has opened has its whole runtime left, so the arithmetic
  // below would put "22m left" on a card where nothing is left of anything — the
  // number is true and the sentence is a lie. The web says "Next Up" in that
  // state (buildProgressStatus, homeScreen.js) and drops the clock icon with it,
  // which is the same reasoning: it is a heading, not a measurement.
  if (ci->remainingMin > 0 || ci->progress == 0) {
    char badge[48];
    int h = ci->remainingMin / 60, m = ci->remainingMin % 60;
    if (ci->progress == 0) snprintf(badge, sizeof badge, "Next Up");
    else if (h && m) snprintf(badge, sizeof badge, "%dh %dm left", h, m);
    else if (h) snprintf(badge, sizeof badge, "%dh left", h);
    else snprintf(badge, sizeof badge, "%dm left", m);
    float pad = NV_CW_PAD * scale;
    TxtLine l = txt_line_trim(TXT_CW_BADGE, badge, 255, 255, 255, 255,
                              r.w * NV_CW_TIME_W - pad);
    if (l.tex) {
      // The scrim takes the CARD's rect and the CARD's radius, for the reason
      // GFX_CW_BAR does: the dome has to be cut by the same corner the artwork is,
      // or it squares off the card's top-right.
      gfx_rect(r, 0, GFX_CORNER_SCRIM, 0, NV_CW_TIME_W, NV_CW_TIME_H, radius,
               0.0313f, 0.0313f, 0.0392f, NV_CW_TIME_INK);
      // Full white and full strength. On a plate the label could be held back to
      // 92% so it did not out-shout the title; floating on artwork there is nothing
      // to spare — every point of contrast here is doing work.
      txt_draw_alpha(l, r.x + r.w - pad - l.w,
                        r.y + pad - NV_CW_TIME_RISE * scale, 1.0f);
    }
  }

  // THE COPY: kicker, title, subtitle — three jobs, not three grey lines.
  //
  // It is built BOTTOM-UP from `.home-continue-copy`'s baseline, because that is
  // how the web anchors it: the box hangs 22px off the base and a second line of
  // title grows UPWARD into the scrim instead of pushing the subtitle down onto
  // the progress bar.
  float padX  = NV_CW_COPY_X * scale;
  float width = r.w - padX * 2;
  float x     = r.x + padX;
  float base  = r.y + r.h - NV_CW_COPY_BOTTOM * scale;

  int series = !strcmp(ci->kind, "series") && ci->episode > 0;
  const char *sub = series && ci->nameEpisode[0] ? ci->nameEpisode : NULL;

  if (sub) {
    float box = NV_CW_SUB_LH * scale;
    TxtLine l = txt_line_trim(TXT_CWC_SUB, sub, 255, 255, 255, 255, width);
    txt_draw_alpha(l, x, base - box + halfLead(TXT_CWC_SUB, box), NV_CW_DIM);
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
    // The title wraps to NV_CW_TITLE_LINES. txt_block always draws, so the height
    // comes from a measuring pass first — txt_block_dir with a negative x breaks
    // the same lines and returns what they use without putting them on screen. The
    // lines it rasterises are exactly the ones the drawing pass then asks for, so
    // the second call is a cache hit and the double pass costs two lookups.
    float lead = NV_CW_TITLE_LH * scale;
    float used = txt_block_dir(TXT_CWC_TITLE, ci->title, 255, 255, 255,
                               -1.0f, 0.0f, width, lead, 1.0f, NV_CW_TITLE_LINES);
    if (used < lead) used = lead;
    txt_block(TXT_CWC_TITLE, ci->title, 255, 255, 255,
              x, base - used + halfLead(TXT_CWC_TITLE, lead),
              width, lead, 1.0f, NV_CW_TITLE_LINES);
    base -= used;
  }

  // "S1 E5", the web's `formatEpisodeCode` — a space, not a colon, and the
  // episode alone when the season is not known. Tracked out by 0.14em, which is
  // the whole difference between a label and a third line of text.
  if (series) {
    char code[24];
    if (ci->season > 0) snprintf(code, sizeof code, "S%d E%d", ci->season, ci->episode);
    else                snprintf(code, sizeof code, "E%d", ci->episode);
    float box = NV_CW_KICKER_LH * scale;
    base -= NV_CW_TITLE_GAP * scale;
    txt_tracking(TXT_CWC_KICKER, code, 255, 255, 255,
                 x, base - box + halfLead(TXT_CWC_KICKER, box),
                 NV_CW_DIM, NV_CW_KICKER_TRACK * scale);
  }

  // THE PROGRESS BAR, full-bleed against the base — but ONLY on something that
  // has actually been started.
  //
  // This is a DELIBERATE DIVERGENCE from the web, which renders
  // `.home-continue-progress` unconditionally: the track is a child of the media
  // element, and its span carries `min-width: 12px`, so a next-up episode at 0%
  // still shows an empty rail with a 12px stub lit at the left. The owner's call
  // is that an episode nobody has opened yet must not claim a position — the
  // stub reads as "you are 5% in" on a card where the right answer is "you have
  // not started this".
  //
  // So there is no track-only state: no progress, no bar at all. Do not "restore
  // the web's behaviour" here without asking.
  //
  // AND THE LINE IS 2%, NOT 1. It was `progress > 0`, and the sources round the
  // wrong way for that: trakt.c TRUNCATES /sync/playback's float and discover.c's
  // window opens at 1, so anything from 1.0% up arrived as a started title. A
  // fifty-minute episode is 1% thirty seconds in — a play pressed and thought
  // better of — and because the fill is floored at NV_CW_BAR_MINW it came up as a
  // deliberate 12px stub, which is exactly the "you are a little way in" claim the
  // note above refuses to make for 0%. The detail screen has drawn that line at 2
  // since it was written ("under it the episode counts as not started", detail.c's
  // episode card, which puts a dashed not-started ring there instead), so the two
  // screens now agree about what STARTED means.
  //
  // GFX_CW_BAR takes the CARD's rect so the ends round with the corner; see the
  // note in gfx.h for why a plain rectangle cannot.
  { float band, fill;
    if (!baked && resume_bar(ci, r, &band, &fill))
      gfx_rect(r, 0, GFX_CW_BAR, 0, band, fill, radius, 1, 1, 1, 1.0f); }
}
