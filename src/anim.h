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

// MOLA DE SEGUNDA ORDEM, CRITICAMENTE AMORTECIDA (posicao + velocidade).
//
// POR QUE ELA EXISTE, ao lado da de cima. MEDIDO na referencia (video do
// aparelho, quadros com carimbo de tempo, tecla DIREITA na fileira "Continuar
// assistindo"): o anel de foco salta para o card novo em UM quadro e e a
// FILEIRA que desliza um passo de card por baixo dele. O deslize chega a
// metade do caminho em ~180 ms, a 83% em ~215 ms, e daí decai como uma
// exponencial de k ~= 12,5 /s ate assentar por volta de 450 ms.
//
// Ou seja: comeca DEVAGAR, acelera, e termina com cauda exponencial. A
// `anim_mola` de primeira ordem faz o contrario — parte com velocidade MAXIMA
// e so desacelera. Ajustada para acertar o meio (k=4,8) ela ainda estaria em
// 89% aos 470 ms, onde a referencia ja esta em 99%; ajustada para acertar a
// cauda (k=12,5) ela cruza a metade aos 55 ms em vez de 180. Nenhum k unico
// serve, porque a forma e outra.
//
// A criticamente amortecida tem exatamente essa forma: p(t) = 1-(1+wt)e^-wt.
// Velocidade inicial zero (partida macia), cauda e^-wt (o k medido) e ZERO
// overshoot — nao "passa do ponto e volta", que e o defeito que a mola crua
// tem em bloco grande. E, como a de primeira ordem, ela PERSEGUE o alvo: se a
// tecla fica presa e o alvo muda no meio do voo, nao ha o que reiniciar.
//
// `v` e a velocidade, guardada pelo chamador junto da posicao. `w` e a
// frequencia em rad/s e vale o k da cauda medida.
static inline float anim_mola2(float *v, float atual, float alvo, float dt, float w) {
  // Um quadro perdido (aba escondida, decode longo) nao pode virar um passo
  // gigante: com dt grande o Euler semi-implicito estoura.
  if (dt > 0.05f) dt = 0.05f;
  *v += (-2.0f * w * (*v) - w * w * (atual - alvo)) * dt;
  return atual + (*v) * dt;
}

// Aceleracao e desaceleracao simetricas, para animacao com relogio proprio.
static inline float anim_suave(float t) {
  t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
  return t * t * (3.0f - 2.0f * t);
}

// Progresso 0..1 com RELOGIO PROPRIO: anda `dt` segundos em direcao a `alvo`
// gastando `ms` no percurso inteiro. Usada onde o tempo precisa bater com uma
// medida (o veu do menu), e nao apenas "assentar rapido".
static inline float anim_rampa(float p, float alvo, float dt, float ms) {
  float passo = dt * (1000.0f / ms);
  if (alvo > p) { p += passo; if (p > alvo) p = alvo; }
  else          { p -= passo; if (p < alvo) p = alvo; }
  return p;
}

#endif
