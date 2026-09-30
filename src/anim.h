// Critically damped spring animation, per property.
//
// Why a spring and not fixed-duration easing: on the D-pad the target changes
// mid-movement (the user holds the key down), and a tween with a duration has
// to be restarted on every change — which produces the familiar stutter. The
// spring simply chases the new target from wherever it is, with no
// discontinuity.
#ifndef NV_ANIM_H
#define NV_ANIM_H
#include <math.h>

// Frame-rate independent: it uses exp(-k*dt), not a fixed step per frame.
static inline float anim_spring(float current, float target, float dt, float stiffness) {
  return current + (target - current) * (1.0f - expf(-stiffness * dt));
}
static inline float anim_clamp(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}
static inline float anim_blend(float a, float b, float t) { return a + (b - a) * t; }

// SECOND-ORDER, CRITICALLY DAMPED SPRING (position + velocity).
//
// WHY IT EXISTS, alongside the one above. MEASURED against the reference
// (device video, time-stamped frames, RIGHT key on the "Continue watching"
// row): the focus ring jumps to the new card in ONE frame and it is the ROW
// that slides one card step underneath it. The slide reaches halfway in
// ~180 ms, 83% in ~215 ms, and from there decays like an exponential with
// k ~= 12.5 /s until it settles around 450 ms.
//
// In other words: it starts SLOWLY, accelerates, and ends with an exponential
// tail. The first-order `anim_spring` does the opposite — it leaves at MAXIMUM
// speed and only decelerates. Tuned to match the midpoint (k=4.8) it would
// still be at 89% at 470 ms where the reference is already at 99%; tuned to
// match the tail (k=12.5) it crosses halfway at 55 ms instead of 180. No single
// k works, because the shape is different.
//
// The critically damped one has exactly that shape: p(t) = 1-(1+wt)e^-wt. Zero
// initial velocity (a soft start), an e^-wt tail (the measured k) and ZERO
// overshoot — it does not "go past and come back", which is the flaw a raw
// spring has on a large block. And, like the first-order one, it CHASES the
// target: if the key is held and the target changes mid-flight, there is
// nothing to restart.
//
// `v` is the velocity, kept by the caller alongside the position. `w` is the
// frequency in rad/s and equals the k of the measured tail.
static inline float anim_spring2(float *v, float current, float target, float dt, float w) {
  // A dropped frame (hidden tab, long decode) must not become one giant step.
  // The closed form avoids the overshoot of semi-implicit Euler and stays
  // retargetable when the D-pad changes the target mid-movement.
  if (dt <= 0.0f || w <= 0.0f) return current;
  if (dt > 0.05f) dt = 0.05f;
  if ((target - current) * (*v) < 0.0f) *v = 0.0f;
  float x = current - target;
  float e = expf(-w * dt);
  float c = (*v + w * x) * dt;
  float new = target + (x + c) * e;
  float nv = (*v - w * c) * e;
  if ((target > current && new > target) || (target < current && new < target)) {
    new = target;
    nv = 0.0f;
  }
  *v = nv;
  return new;
}

// Reduced motion is a policy, not a detail of each screen.
static inline float anim_reduced(float current, float target, int reduced) {
  return reduced ? target : current;
}
static inline float anim_spring2_reduced(float *v, float current, float target,
                                        float dt, float w, int reduced) {
  if (reduced) { *v = 0.0f; return target; }
  return anim_spring2(v, current, target, dt, w);
}

// THE WALL: a lean towards a D-pad press that had nowhere to go, and back.
//
// Not a target the list moves to and returns from — that is two movements and a
// visible stop between them. It is a KICK: the offset stays at rest and is given a
// velocity, and the same critically damped spring as anim_spring2 (target 0) takes
// it out and home in one shape, x(t) = v0 t e^-wt. That never crosses zero, peaks
// at t = 1/w, and the peak is v0/(w e) — which is how `kick` below turns the
// wanted lean in pixels into the velocity that produces it.
//
// anim_spring2 itself cannot run this: its guard zeroes a velocity pointing away
// from the target, which is exactly what a kick is.
//
// A held key hits the wall ONCE. `walled` remembers that the last press was
// refused, and a repeat against the same wall is ignored: holding RIGHT down a row
// leans once at the end, not in a pulse for as long as the key is down.
typedef struct { float x, v; int walled; } AnimBump;

static inline void anim_bump_hit(AnimBump *b, float dir, int repeat,
                                 float px, float w, int reduced) {
  if (reduced || (repeat && b->walled)) { b->walled = 1; return; }
  b->walled = 1;
  b->v = dir * px * w * 2.718282f;
}
static inline void anim_bump_clear(AnimBump *b) { b->walled = 0; }
static inline float anim_bump_step(AnimBump *b, float dt, float w) {
  if (b->x == 0.0f && b->v == 0.0f) return 0.0f;
  if (dt <= 0.0f) return b->x;
  if (dt > 0.05f) dt = 0.05f;
  float e = expf(-w * dt);
  float c = (b->v + w * b->x) * dt;
  float x = (b->x + c) * e;
  float v = (b->v - w * c) * e;
  if (fabsf(x) < 0.05f && fabsf(v) < 1.0f) x = v = 0.0f;
  b->x = x; b->v = v;
  return x;
}

// THE SAME WALL ON A GRID, where it is the FOCUSED CARD that leans rather than a
// row: a grid has a wall on every side of the card, and leaning the whole grid
// sideways from the middle of it reads as the page slipping. `n[0]` is across,
// `n[1]` down. `dx`/`dy` is the press; `moved` whether it went anywhere.
static inline void anim_nudge(AnimBump n[2], int dx, int dy, int moved,
                              int repeat, int reduced, float px, float w) {
  if (moved) { anim_bump_clear(&n[0]); anim_bump_clear(&n[1]); return; }
  if (dx) anim_bump_hit(&n[0], (float)dx, repeat, px, w, reduced);
  if (dy) anim_bump_hit(&n[1], (float)dy, repeat, px, w, reduced);
}
static inline void anim_nudge_step(AnimBump n[2], float dt, float w) {
  anim_bump_step(&n[0], dt, w);
  anim_bump_step(&n[1], dt, w);
}

// Symmetric acceleration and deceleration, for animation with its own clock.
static inline float anim_smooth(float t) {
  t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
  return t * t * (3.0f - 2.0f * t);
}

// A STAGGERED ENTRANCE: item `order` of a list that filled `msSince` ago, 0..1.
// Each item starts `stepMs` after the one before and takes `durMs`; past
// `maxOrder` they all start together, so a long grid is not still arriving when
// the viewer begins to move through it.
static inline float anim_stagger(float msSince, int order, float stepMs,
                                 int maxOrder, float durMs) {
  if (order < 0) order = 0;
  if (order > maxOrder) order = maxOrder;
  return anim_smooth((msSince - (float)order * stepMs) / durMs);
}

// THE TOP-EDGE DISSOLVE every scrolling grid in this app uses: 1 at rest, ramping
// to 0 across `band` pixels as `y` climbs towards `edge`.
//
// It is the home's — see the top-edge mask in home_draw. A row that is merely
// clipped is guillotined against an invisible line and reads as a rendering
// fault; a row that has already reached zero by the time it arrives there simply
// stops being there, which is what "the things disappear, they don't rise" means.
//
// PASS THE RESTING y, not the animated one. Where a screen offsets its content
// while opening, reading the ramp through that offset fades the whole grid in
// from the top on every entrance. The home records the same trap.
static inline float anim_edge(float y, float edge, float band) {
  return anim_smooth((y - edge) / band);
}

// Progress 0..1 with ITS OWN CLOCK: advances `dt` seconds towards `target`,
// spending `ms` over the whole journey. Used where the timing has to match a
// measurement (the menu veil), not merely "settle quickly".
static inline float anim_ramp(float p, float target, float dt, float ms) {
  float step = dt * (1000.0f / ms);
  if (target > p) { p += step; if (p > target) p = target; }
  else          { p -= step; if (p < target) p = target; }
  return p;
}

#endif
