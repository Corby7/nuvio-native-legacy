#include "resume.h"
#include "layout.h"
#include "text.h"
#include "anim.h"
#include "settings.h"
#include <stdio.h>
#include <string.h>

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

void resume_draw(const CatItem *ci, GfxRect r) {
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
  gfx_rect(r, 0, GFX_CW_SCRIM, 0, 0, 0, radius, 0, 0, 0, 1.0f);

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

  // The title wraps to NV_CW_TITLE_LINES. txt_block always draws, so the height
  // comes from a measuring pass first — txt_block_dir with a negative x breaks
  // the same lines and returns what they use without putting them on screen. The
  // lines it rasterises are exactly the ones the drawing pass then asks for, so
  // the second call is a cache hit and the double pass costs two lookups.
  {
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
  if (ci->progress > NV_CW_BAR_MIN_PCT) {
    float band = NV_CW_BAR_H * scale / r.h;
    float fill = anim_clamp(ci->progress / 100.f, 0, 1);
    float min  = NV_CW_BAR_MINW * scale / r.w;
    if (fill < min) fill = min;
    gfx_rect(r, 0, GFX_CW_BAR, 0, band, fill, radius, 1, 1, 1, 1.0f);
  }
}
