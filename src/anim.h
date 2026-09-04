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
static inline float anim_mola(float current, float target, float dt, float stiffness) {
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
static inline float anim_mola2(float *v, float current, float target, float dt, float w) {
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
static inline float anim_mola2_reduced(float *v, float current, float target,
                                        float dt, float w, int reduced) {
  if (reduced) { *v = 0.0f; return target; }
  return anim_mola2(v, current, target, dt, w);
}

// Symmetric acceleration and deceleration, for animation with its own clock.
static inline float anim_smooth(float t) {
  t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
  return t * t * (3.0f - 2.0f * t);
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
