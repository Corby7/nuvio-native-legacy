#include "hold.h"
#include "layout.h"
#include "anim.h"
#include "settings.h"
#include "ctxmenu.h"

static int isOk(SDL_Keycode k) {
  return k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE;
}

int hold_event(Hold *h, const SDL_Event *e, int armable, int *tap) {
  SDL_Keycode k;
  *tap = 0;
  if (e->type != SDL_KEYDOWN && e->type != SDL_KEYUP) return 0;
  k = e->key.keysym.sym;
  if (e->type == SDL_KEYDOWN) {
    if (isOk(k)) {
      // The remote repeats a held key: only the first press arms.
      if (e->key.repeat) return h->pressing || h->fired;
      if (!armable) return 0;
      h->pressing = 1;
      h->fired = 0;
      h->since = SDL_GetTicks();
      return 1;
    }
    // Anything else while held cancels the gesture, and the key goes on to do
    // what it does. The sweep drains back on its own.
    h->pressing = 0;
    return 0;
  }
  if (!isOk(k)) return 0;
  if (h->fired) { h->fired = 0; return 1; }
  if (!h->pressing) return 0;
  h->pressing = 0;
  *tap = 1;
  return 1;
}

int hold_fired(Hold *h, Uint32 now) {
  if (!h->pressing || now - h->since < NV_HOLD_MS) return 0;
  h->pressing = 0;
  h->fired = 1;
  // Complete: the sweep has closed the ring, so the ring is whole and white
  // again with nothing left to draw over it. What marks the moment is the glow
  // and the card springing back out.
  h->shown = 0.0f;
  h->dim = 0.0f;
  h->pulse = settings_animations_reduced() ? 0.0f : 1.0f;
  return 1;
}

// home.c's update for the same four values, step for step.
void hold_animate(Hold *h, float dt, Uint32 now) {
  int reduced = settings_animations_reduced();
  float t = 0.0f;
  int pressing;
  if (h->scale == 0.0f) h->scale = 1.0f;   // a zeroed Hold
  // NV_HOLD_FEEDBACK_MS is the silence a normal tap fits in: nothing shows
  // until the press is clearly not a tap, then it fills over what is left.
  if (h->pressing) {
    Uint32 held = now - h->since;
    t = held <= NV_HOLD_FEEDBACK_MS ? 0.0f
      : anim_clamp((held - NV_HOLD_FEEDBACK_MS) / (NV_HOLD_MS - NV_HOLD_FEEDBACK_MS),
                   0.0f, 1.0f);
  }
  pressing = h->pressing && t > 0.0f;
  // Slow out of the gate and accelerating to the end (a cubic ease-in).
  if (pressing) h->shown = t * t * t;
  else if (h->shown > 0.0f)
    h->shown = anim_clamp(h->shown - dt / NV_HOLD_DRAIN_S, 0.0f, 1.0f);
  h->dim = reduced ? (pressing || h->shown > 0.0f ? 1.0f : 0.0f)
                   : anim_spring(h->dim, pressing || h->shown > 0.0f ? 1.0f : 0.0f,
                                 dt, NV_HOLD_DIM_K);
  h->pulse = anim_clamp(h->pulse - dt / NV_HOLD_PULSE_S, 0.0f, 1.0f);
  // An UNDER-damped spring: the bounce past 1 on release is the point. Stepped
  // at 240Hz so a slow frame cannot blow it up.
  if (reduced) { h->scale = 1.0f; h->scaleV = 0.0f; }
  else {
    float target = pressing ? NV_HOLD_PRESS_SCALE : 1.0f, left = dt;
    while (left > 0.0f) {
      float s = left > 1.0f / 240.0f ? 1.0f / 240.0f : left;
      float w = NV_HOLD_SPRING_W;
      h->scaleV += (w * w * (target - h->scale) -
                    2.0f * NV_HOLD_SPRING_ZETA * w * h->scaleV) * s;
      h->scale += h->scaleV * s;
      left -= s;
    }
  }
}

GfxRect hold_card(const Hold *h, GfxRect c) {
  float s = h->scale == 0.0f ? 1.0f : h->scale;
  if (s == 1.0f) return c;
  return (GfxRect){ c.x + c.w * (1.0f - s) * 0.5f, c.y + c.h * (1.0f - s) * 0.5f,
                    c.w * s, c.h * s };
}

static float glowPx(const Hold *h) { return 10.0f + 28.0f * (1.0f - h->pulse); }

void hold_glow(const Hold *h, GfxRect card, float radiusPx) {
  // It widens as it fades.
  if (h->pulse > 0.0f)
    gfx_glow(card, radiusPx, glowPx(h), 1.0f, 1.0f, 1.0f, 0.55f * h->pulse * h->pulse);
}

void hold_ring(const Hold *h, GfxRect card, float thickness, float radius,
               float r, float g, float b, float f) {
  if (f <= 0.01f) return;
  // DIMMED BY COLOUR, NOT BY ALPHA. The home's ring sits OUTSIDE the art, over
  // the background, so fading it reads as a grey ring for the sweep to fill.
  // This ring sits ON the poster's edge: faded, the poster showed through it and
  // the sweep ran over a muddy band that looked nothing like the home's. Mixing
  // towards the background colour at full opacity is what the home's faded ring
  // looks like.
  { float k = NV_HOLD_DIM * h->dim;
    gfx_rect(card, 0, GFX_RING_INSET, 0, thickness, 0, radius,
             r + (NV_COLOR_BACKGROUND_R - r) * k, g + (NV_COLOR_BACKGROUND_G - g) * k,
             b + (NV_COLOR_BACKGROUND_B - b) * k, f); }
  if (h->shown > 0.0f)
    gfx_rect(card, 0, GFX_RING_INSET_FILL, 0, h->shown, thickness, radius,
             1.0f, 1.0f, 1.0f, f);
}

void hold_track(const Hold *h, GfxRect card, float radiusPx) {
  if (ctx_is_open())
    ctx_track_card(card, radiusPx, h->pulse > 0.0f ? glowPx(h) * h->pulse : 0.0f);
}
