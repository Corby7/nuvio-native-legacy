// Animacao por mola criticamente amortecida, por propriedade.
//
// Por que mola e nao easing de duracao fixa: no D-pad o alvo muda no meio do
// movimento (usuario segura a tecla), e uma tween com duracao precisa ser
// reiniciada a cada troca — o que produz o "engasgo" tipico. A mola so persegue
// o alvo novo a partir da posicao atual, sem descontinuidade.
#ifndef NV_ANIM_H
#define NV_ANIM_H
#include <math.h>

// Independente de framerate: usa exp(-k*dt), nao um passo fixo por quadro.
static inline float anim_mola(float atual, float alvo, float dt, float rigidez) {
  return atual + (alvo - atual) * (1.0f - expf(-rigidez * dt));
}
static inline float anim_clamp(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}
static inline float anim_mistura(float a, float b, float t) { return a + (b - a) * t; }

#endif
