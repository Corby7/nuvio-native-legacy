#include "ctxmenu.h"
#include "catalogo.h"
#include "trakt.h"
#include "extras.h"
#include "gfx.h"
#include "text.h"
#include "tex_cache.h"
#include "layout.h"
#include "anim.h"
#include <stdio.h>
#include <string.h>

// MEDIDO no bundle 1.0.4: o dialogo tem 37,5vw de largura (720 px em 1920) e
// duas linhas de cabecalho — titulo do item e o subtitulo "Opções do título".
#define CTX_W      720.0f
#define CTX_PAD     44.0f
#define CTX_LINHA   86.0f     // altura de cada botao
#define CTX_GAP     12.0f

static int   aberto, idx = -1, foco, pedDetalhes = -1;
static float anim;

// Ate tres: detalhes, biblioteca e — so em filme/serie — assistido.
#define CTX_MAX 3
static struct { const char *rot; int acao; } ops[CTX_MAX];
static int nOps;
enum { OP_DETALHES, OP_LISTA, OP_ASSISTIDO };

static void montar(void) {
  const CatItem *ci = cat_item(idx);
  nOps = 0;
  if (!ci) return;
  ops[nOps].rot = "Ver detalhes";        ops[nOps].acao = OP_DETALHES;  nOps++;
  ops[nOps].rot = ci->naLista ? "Remover da biblioteca"
                              : "Adicionar à biblioteca";
  ops[nOps].acao = OP_LISTA; nOps++;
  // O web so oferece "assistido" em filme e serie — nao em canal nem evento,
  // que sao tipos que os addons do dono tambem declaram.
  if (!strcmp(ci->tipo, "movie") || !strcmp(ci->tipo, "series")) {
    ops[nOps].rot = "Marcar como assistido";
    ops[nOps].acao = OP_ASSISTIDO; nOps++;
  }
}

void ctx_abrir(int indice) {
  if (indice < 0 || !cat_item(indice)) return;
  idx = indice; foco = 0; aberto = 1; pedDetalhes = -1;
  montar();
}

int ctx_aberto(void) { return aberto; }
int ctx_pediu_detalhes(void) { int v = pedDetalhes; pedDetalhes = -1; return v; }

static void aplicar(void) {
  const CatItem *ci = cat_item(idx);
  if (!ci || foco < 0 || foco >= nOps) return;
  switch (ops[foco].acao) {
    case OP_DETALHES: pedDetalhes = idx; break;
    case OP_LISTA:
      // Espelha o estado LOCAL na hora e manda para o Trakt em seguida: sem o
      // reflexo local o rotulo so mudaria quando a rede respondesse, e o dono
      // ficaria sem saber se o toque foi registrado.
      cat_definir_na_lista(idx, !ci->naLista);
      trakt_watchlist(ci->imdb, !ci->naLista);
      break;
    case OP_ASSISTIDO:
      trakt_assistido(ci->imdb, 1);
      break;
  }
  aberto = 0;
}

void ctx_evento(const SDL_Event *e) {
  int k;
  if (!aberto || e->type != SDL_KEYDOWN) return;
  k = e->key.keysym.sym;
  if (k == SDLK_AC_BACK || k == SDLK_ESCAPE || k == SDLK_BACKSPACE ||
      e->key.keysym.scancode == NV_SCANCODE_BACK) { aberto = 0; return; }
  if (k == SDLK_UP)   { if (foco > 0) foco--; return; }
  if (k == SDLK_DOWN) { if (foco + 1 < nOps) foco++; return; }
  if (k == SDLK_RETURN || k == SDLK_KP_ENTER) { aplicar(); return; }
}

void ctx_atualizar(float dt, Uint32 agora) {
  (void)agora;
  anim = anim_mola(anim, aberto ? 1.0f : 0.0f, dt, NV_MOLA_TELA);
}

void ctx_desenhar(Uint32 agora) {
  const CatItem *ci;
  float a = anim, alt, x, y;
  int i;
  (void)agora;
  if (a < 0.01f) return;
  ci = cat_item(idx);
  if (!ci) return;

  { GfxRect tela = { 0, 0, NV_TELA_W, NV_TELA_H };
    gfx_cor(tela, 0.0f, 0, 0, 0, 0.72f * a); }

  alt = CTX_PAD * 2.0f + 96.0f + (float)nOps * (CTX_LINHA + CTX_GAP) - CTX_GAP;
  x = (NV_TELA_W - CTX_W) * 0.5f;
  y = (NV_TELA_H - alt) * 0.5f;
  // Sobe do fundo enquanto aparece, como as outras folhas do app.
  y += (1.0f - a) * 40.0f;

  { GfxRect p = { x, y, CTX_W, alt };
    gfx_cor(p, 0.06f, 0.11f, 0.11f, 0.13f, 0.98f * a); }

  { TxtLinha t = txt_linha_corta(TXT_HEADLINE, ci->titulo, 245, 248, 255, 255,
                                 CTX_W - CTX_PAD * 2.0f);
    txt_desenhar_alpha(t, x + CTX_PAD, y + CTX_PAD, a); }
  { TxtLinha t = txt_linha(TXT_DET_META2, "Opções do título", 150, 154, 163, 255);
    txt_desenhar_alpha(t, x + CTX_PAD, y + CTX_PAD + 44.0f, a * 0.9f); }

  for (i = 0; i < nOps; i++) {
    float by = y + CTX_PAD + 96.0f + (float)i * (CTX_LINHA + CTX_GAP);
    GfxRect r = { x + CTX_PAD, by, CTX_W - CTX_PAD * 2.0f, CTX_LINHA };
    int sel = (i == foco);
    // Mesma linguagem das pilulas: o focado INVERTE (fundo claro, texto
    // escuro), em vez de anel branco sobre preenchimento claro.
    float lum = sel ? 0.961f : 0.176f;
    int cor = sel ? 17 : 240;
    gfx_cor(r, 14.0f / CTX_LINHA, lum, lum, lum, a);
    { TxtLinha t = txt_linha(TXT_PLR_CORPO, ops[i].rot, cor, cor, cor, 255);
      txt_desenhar_alpha(t, r.x + 28.0f, by + (CTX_LINHA - t.h) * 0.5f, a); }
  }
}
