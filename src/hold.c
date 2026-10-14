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
  h->spin = hold_brand() ? h->pulse : 0.0f;
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
  h->spin = anim_clamp(h->spin - dt / NV_HOLD_SPIN_S, 0.0f, 1.0f);
  // An UNDER-damped spring: the bounce past 1 on release is the point. Stepped
  // at 240Hz so a slow frame cannot blow it up.
  if (reduced) { h->scale = 1.0f; h->scaleV = 0.0f; }
  else {
    float target = pressing ? 1.0f - (1.0f - NV_HOLD_PRESS_SCALE) * anim_ease_out(t)
                 : h->pulse > NV_HOLD_POP_PULSE ? NV_HOLD_POP_SCALE : 1.0f, left = dt;
    while (left > 0.0f) {
      float s = left > 1.0f / 240.0f ? 1.0f / 240.0f : left;
      float w = pressing ? NV_HOLD_SPRING_W : NV_HOLD_RELEASE_W;
      float z = pressing ? NV_HOLD_SPRING_ZETA : NV_HOLD_RELEASE_ZETA;
      h->scaleV += (w * w * (target - h->scale) - 2.0f * z * w * h->scaleV) * s;
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

static float glowPx(const Hold *h) { return hold_glow_px(h->pulse); }

// It widens as it fades. Under the Nuvio ring it is a violet bloom, wider and
// stronger than the white one: the moment the hold lands needs more than the
// ring to say so, and white at 0.55 was barely there on a light poster.
float hold_glow_px(float pulse) {
  return 10.0f + (hold_brand() ? 48.0f : 28.0f) * (1.0f - pulse);
}
void hold_glow_draw(GfxRect card, float radiusPx, float pulse) {
  if (pulse <= 0.0f) return;
  if (hold_brand())
    gfx_glow(card, radiusPx, hold_glow_px(pulse), 0.72f, 0.60f, 1.0f, 0.8f * pulse * pulse);
  else
    gfx_glow(card, radiusPx, hold_glow_px(pulse), 1.0f, 1.0f, 1.0f, 0.55f * pulse * pulse);
}

void hold_glow(const Hold *h, GfxRect card, float radiusPx) {
  hold_glow_draw(card, radiusPx, h->pulse);
}

void hold_ring(const Hold *h, GfxRect card, float thickness, float radius,
               float r, float g, float b, float f) {
  if (f <= 0.01f) return;
  thickness += NV_HOLD_RING_GROW * hold_grow(h) / card.h;
  // DIMMED BY COLOUR, NOT BY ALPHA. The home's ring sits OUTSIDE the art, over
  // the background, so fading it reads as a grey ring for the sweep to fill.
  // This ring sits ON the poster's edge: faded, the poster showed through it and
  // the sweep ran over a muddy band that looked nothing like the home's. Mixing
  // towards the background colour at full opacity is what the home's faded ring
  // looks like.
  { float k = hold_dim_max() * h->dim;
    gfx_rect(card, 0, GFX_RING_INSET, 0, thickness, 0, radius,
             r + (NV_COLOR_BACKGROUND_R - r) * k, g + (NV_COLOR_BACKGROUND_G - g) * k,
             b + (NV_COLOR_BACKGROUND_B - b) * k, f); }
  hold_sweep_inset(h, card, thickness, radius, f);
}

// Eased both ways, so the lap starts and settles rather than ticking round.
float hold_spin_phase(float spin) { float t = 1.0f - spin; return t * t * (3.0f - 2.0f * t); }
// Whole for most of the lap, fading back to the plain ring over its last 30%.
float hold_spin_alpha(float spin) { return spin >= 0.3f ? 1.0f : spin / 0.3f; }

int hold_brand(void) { return settings_hold_ring_brand(); }
float hold_dim_max(void) { return hold_brand() ? NV_HOLD_DIM : NV_HOLD_DIM_PLAIN; }

float hold_grow(const Hold *h) {
  float s;
  if (!hold_brand()) return 0.0f;
  s = h->spin > 0.0f ? hold_spin_alpha(h->spin) : 0.0f;
  return h->dim > s ? h->dim : s;
}

void hold_sweep_inset(const Hold *h, GfxRect card, float thickness, float radius, float f) {
  if (h->shown > 0.0f)
    gfx_rect(card, 0, GFX_RING_INSET_FILL, hold_brand() ? 1.0f : 0.0f, h->shown, thickness, radius,
             1.0f, 1.0f, 1.0f, f);
  if (h->spin > 0.0f)
    gfx_rect(card, 0, GFX_RING_INSET_FILL, 1.0f + 0.999f * hold_spin_phase(h->spin), 1.0f,
             thickness, radius, 1.0f, 1.0f, 1.0f, f * hold_spin_alpha(h->spin));
}

void hold_track(const Hold *h, GfxRect card, float radiusPx) {
  if (ctx_is_open())
    ctx_track_card(card, radiusPx, h->pulse > 0.0f ? glowPx(h) * h->pulse : 0.0f);
}
