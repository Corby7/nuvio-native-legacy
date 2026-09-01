#include "faixas.h"
#include "video.h"
#include "addons.h"
#include "gfx.h"
#include "text.h"
#include "anim.h"
#include "layout.h"
#include <stdio.h>
#include <string.h>

#define FX_LARG   1180.0f
#define FX_LINHA    64.0f
#define FX_MAX      10

static int aberta, coluna, foco[2];
static float anim;
// Qual legenda EXTERNA (OpenSubtitles) esta valendo, em indice da lista
// combinada — ou -1 quando a ativa e embutida ou nao ha nenhuma.
//
// Isto vive aqui e nao no video.c porque o pipeline nao devolve essa
// informacao: video_legenda_externa manda o setSubtitleSource com a URL e o
// legAtual do video.c fica intocado, apontando para a legenda EMBUTIDA de
// antes. Sem esta variavel, escolher uma legenda do OpenSubtitles fazia a
// marca de "ativa" ficar em outra linha (ou em "Desativada") e a folha
// reabria com o foco no lugar errado — a legenda certa tocava, so a folha
// mentia sobre qual era.
static int legExterna = -1;

// Chamada quando uma sessao de reproducao nova comeca: a legenda externa e da
// sessao, nao do aparelho. Sem isto o titulo seguinte abriria a folha marcando
// como ativa uma legenda que nao foi escolhida para ele.
void faixas_reiniciar(void) { legExterna = -1; aberta = 0; }

static int nLinhas(int col);

void faixas_abrir(void) {
  int n;
  aberta = 1; coluna = 0; foco[0] = video_audio_atual();
  // A legenda pode estar desligada (-1); a primeira linha da coluna e sempre
  // "Desativada", entao o indice da lista e deslocado em um.
  foco[1] = (legExterna >= 0 ? legExterna : video_legenda_atual()) + 1;
  // Clamp nas duas colunas. A lista de legendas CRESCE durante a sessao (as do
  // OpenSubtitles chegam depois) e a de audio so existe apos o sourceInfo:
  // guardar um indice de antes e reabrir sem conferir poe o foco fora do vetor.
  { int c; for (c = 0; c < 2; c++) {
      n = nLinhas(c);
      if (foco[c] >= n) foco[c] = n > 0 ? n - 1 : 0;
      if (foco[c] < 0)  foco[c] = 0;
    } }
}

int faixas_aberta(void) { return aberta; }

static int nLegendas(void) {
  int n = video_n_legenda() + addons_n_legendas();
  return n > FX_MAX ? FX_MAX : n;
}

static int nLinhas(int col) {
  if (col == 0) { int n = video_n_audio(); return n > FX_MAX ? FX_MAX : n; }
  return nLegendas() + 1;   // +1 pela linha "Desativada"
}

// Rotulo da linha `i` da coluna de legenda. Ate video_n_legenda() sao as
// embutidas; depois vem as do OpenSubtitles.
static const char *rotuloLegenda(int i, const char **marca) {
  int emb = video_n_legenda();
  *marca = NULL;
  if (i < emb) {
    const VideoFaixa *f = video_legenda(i);
    return f ? f->rotulo : "";
  }
  { const Legenda *l = addons_legenda(i - emb);
    if (!l) return "";
    *marca = "OpenSubtitles";
    return l->rotulo; }
}

static void aplicar(void) {
  if (coluna == 0) {
    video_escolher_audio(foco[0]);
  } else {
    int i = foco[1] - 1;
    int emb = video_n_legenda();
    if (i < 0)        { video_escolher_legenda(-1); legExterna = -1; }
    else if (i < emb) { video_escolher_legenda(i);  legExterna = -1; }
    else {
      const Legenda *l = addons_legenda(i - emb);
      // So marca como ativa se houve o que aplicar: sem a URL o uMS nao recebe
      // nada, e a folha diria "ativa" sobre uma legenda que nunca subiu.
      if (l) { video_legenda_externa(l->url); legExterna = i; }
    }
  }
}

void faixas_evento(const SDL_Event *e) {
  SDL_Keycode k;
  if (!aberta || e->type != SDL_KEYDOWN) return;
  k = e->key.keysym.sym;
  if (k == SDLK_AC_BACK || k == SDLK_ESCAPE || k == SDLK_BACKSPACE) { aberta = 0; return; }
  if (k == SDLK_LEFT)  { coluna = 0; return; }
  if (k == SDLK_RIGHT) { coluna = 1; return; }
  if (k == SDLK_UP)    { if (foco[coluna] > 0) foco[coluna]--; return; }
  if (k == SDLK_DOWN)  { if (foco[coluna] < nLinhas(coluna) - 1) foco[coluna]++; return; }
  if (k == SDLK_RETURN || k == SDLK_KP_ENTER) { aplicar(); aberta = 0; return; }
}

void faixas_atualizar(float dt, Uint32 agora) {
  (void)agora;
  anim = anim_mola(anim, aberta ? 1.0f : 0.0f, dt, NV_MOLA_TELA);
}

static void coluna_desenhar(int col, float x, float larg, float y0, float a) {
  int i, n = nLinhas(col);
  TxtLinha t = txt_linha(TXT_HEADLINE, col ? "Legendas" : "Audio",
                         255, 255, 255, 255);
  txt_desenhar_alpha(t, x, y0, a);
  for (i = 0; i < n; i++) {
    float y = y0 + 62.0f + i * FX_LINHA;
    int sel = (col == coluna && i == foco[col]);
    const char *marca = NULL;
    const char *rot;
    if (col == 0) {
      const VideoFaixa *f = video_audio(i);
      rot = f ? f->rotulo : "";
    } else {
      rot = i == 0 ? "Desativada" : rotuloLegenda(i - 1, &marca);
    }
    if (sel) {
      // Raio e FRAÇÃO do menor lado (ver gfx.h), não pixel: 10 aqui degenerava
      // o SDF e a pílula de foco saía invisível — a folha parecia texto solto.
      GfxRect pn = { x - 16, y - 10, larg, FX_LINHA - 8 };
      gfx_cor(pn, 0.30f, 1, 1, 1, 0.16f * a);
    }
    { int cor = sel ? 255 : 214;
      TxtLinha l = txt_linha(TXT_BODY, rot, cor, cor, cor + 6 > 255 ? 255 : cor + 6, 255);
      txt_desenhar_alpha(l, x, y, a * (sel ? 1.0f : 0.86f));
      // Marca de origem a direita: sem ela nao da para saber se a legenda veio
      // do arquivo ou da internet, e as duas falham por motivos diferentes.
      if (marca) {
        TxtLinha m = txt_linha(TXT_MINI, marca, 170, 172, 182, 255);
        txt_desenhar_alpha(m, x + larg - 32 - m.w, y + 6, a * 0.8f);
      } }
    // Marca do que esta ativo agora.
    { int ativo = (col == 0) ? (i == video_audio_atual())
                 : (legExterna >= 0 ? (i - 1 == legExterna)
                                    : (i - 1 == video_legenda_atual()));
      if (ativo) {
        GfxRect pt = { x - 26, y + 12, 8, 8 };
        gfx_cor(pt, 0.5f, 1, 1, 1, 0.9f * a);
      } }
  }
  if (n == 0) {
    TxtLinha l = txt_linha(TXT_CALLOUT, "Nenhuma disponivel", 170, 172, 182, 255);
    txt_desenhar_alpha(l, x, y0 + 62.0f, a * 0.8f);
  }
}

void faixas_desenhar(Uint32 agora) {
  float a = anim;
  float alt, x0, y0;
  GfxRect tela = { 0, 0, NV_TELA_W, NV_TELA_H };
  (void)agora;
  if (a < 0.01f) return;
  gfx_cor(tela, 0.0f, 0, 0, 0, 0.72f * a);
  { int maior = nLinhas(0) > nLinhas(1) ? nLinhas(0) : nLinhas(1);
    alt = 62.0f + maior * FX_LINHA + 80.0f; }
  if (alt > NV_TELA_H - 160.0f) alt = NV_TELA_H - 160.0f;
  x0 = (NV_TELA_W - FX_LARG) * 0.5f;
  y0 = (NV_TELA_H - alt) * 0.5f;
  { // Sobe do fundo enquanto aparece, como as outras folhas do app.
    GfxRect painel = { x0, y0 + (1.0f - a) * 40.0f, FX_LARG, alt };
    gfx_cor(painel, 0.06f, 0.11f, 0.11f, 0.13f, 0.98f * a);
    coluna_desenhar(0, painel.x + 56, FX_LARG * 0.5f - 80, painel.y + 46, a);
    coluna_desenhar(1, painel.x + FX_LARG * 0.5f + 24, FX_LARG * 0.5f - 80,
                    painel.y + 46, a); }
}
