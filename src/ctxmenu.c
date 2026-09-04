#include "ctxmenu.h"
#include "catalog.h"
#include "trakt.h"
#include "extras.h"
#include "gfx.h"
#include "text.h"
#include "tex_cache.h"
#include "layout.h"
#include "anim.h"
#include "settings.h"
#include <stdio.h>
#include <string.h>

// Mantem o header publico de Trakt estavel: estas leituras sao o contrato
// interno entre a modal e as escritas assincronas do proprio port.
extern int trakt_operation_state(int kind);
extern int trakt_watchlist_kind(const char *imdb, const char *kind, int add);
extern int trakt_watched_kind(const char *imdb, const char *kind, int mark);
extern int cat_history_state_item(int index_);
extern void cat_history_set_id(const char *imdb, const char *kind, int watched);

enum { CTX_OP_NONE, CTX_OP_LIST = 1, CTX_OP_HISTORY = 2 };
enum { CTX_PENDING = 1, CTX_CONFIRMED = 2, CTX_FAILURE = 3 };

// MEDIDO no bundle 1.0.4: o dialogo tem 37,5vw de largura (720 px em 1920).
#define CTX_W      720.0f
#define CTX_DFLT     44.0f
#define CTX_LINE   86.0f     // altura de cada botao
#define CTX_GAP     12.0f
#define CTX_HEADER    148.0f     // titulo, estados e rotulo do grupo
#define CTX_STATUS_H 34.0f
#define CTX_FOOTER  70.0f

static int   is_open, idx = -1, focus, reqDetails = -1;
static float anim;
static int   operation, intent, stateOperation;
static int   mirrorApplied;
static char  operationImdb[16];
static volatile int holdActive, holdCancelled, holdReady;
static Uint32 holdSince;

static int keyOk(SDL_Keycode k) {
  return k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE;
}

// A home e quem conhece o item focado, por isso ela continua decidindo qual
// indice entregar a ctx_abrir no KEYUP. Este observador fornece o feedback
// durante a retenção e arma a janela longa; setas/Voltar invalidam o gesto
// antes que a home possa transformá-lo em ação.
static int observeHold(void *u, SDL_Event *e) {
  (void)u;
  if (e->type == SDL_KEYDOWN) {
    SDL_Keycode k = e->key.keysym.sym;
    if (keyOk(k) && !e->key.repeat) {
      holdActive = 1;
      holdCancelled = 0;
      holdReady = 0;
      holdSince = SDL_GetTicks();
    } else if (holdActive &&
               (k == SDLK_UP || k == SDLK_DOWN || k == SDLK_LEFT ||
                k == SDLK_RIGHT || k == SDLK_AC_BACK || k == SDLK_ESCAPE ||
                k == SDLK_BACKSPACE || e->key.keysym.scancode == NV_SCANCODE_BACK)) {
      holdCancelled = 1;
    }
  } else if (e->type == SDL_KEYUP && keyOk(e->key.keysym.sym)) {
    if (holdActive && !holdCancelled && SDL_GetTicks() - holdSince >= NV_HOLD_MS)
      holdReady = 1;
    holdActive = 0;
  }
  return 0;
}

// Ate tres: detalhes, biblioteca e — so em filme/serie — assistido.
#define CTX_MAX 3
static struct { const char *rot; int action; } ops[CTX_MAX];
static int nOps;
static float focusAnim[CTX_MAX];
static int holdObserver;
enum { OP_DETAILS, OP_LIST, OP_WATCHED };

static int indexCurrent(void) {
  int n = cat_n();
  int found;
  if (n < 1 || idx < 0 || idx >= n) return -1;
  if (operationImdb[0]) {
    found = cat_index_por_imdb(operationImdb);
    // A resposta pode chegar depois de a descoberta trocar o bloco. Nunca
    // reutilizar `idx` nesse caso, pois ele pode ser outro titulo.
    return found;
  }
  return idx;
}

static void build(void) {
  int i = indexCurrent();
  const CatItem *ci = i >= 0 ? cat_item(i) : NULL;
  nOps = 0;
  if (!ci) return;
  ops[nOps].rot = "See details";        ops[nOps].action = OP_DETAILS;  nOps++;
  // Sem IMDb nao ha endpoint remoto suportado para esta acao. Nao oferecer
  // um botao que so aparentaria funcionar e inventaria estado local.
  if (ci->imdb[0]) {
    if (stateOperation == CTX_PENDING && operation == CTX_OP_LIST)
      ops[nOps].rot = intent ? "Adding to library..."
                               : "Removing from library...";
    else
      ops[nOps].rot = ci->naList ? "Remove from library"
                                  : "Add to library";
    ops[nOps].action = OP_LIST; nOps++;
  }
  // O web so oferece "assistido" em filme e serie — nao em canal nem evento,
  // que sao tipos que os addons do dono tambem declaram.
  if (ci->imdb[0] && (!strcmp(ci->kind, "movie") || !strcmp(ci->kind, "series"))) {
    if (stateOperation == CTX_PENDING && operation == CTX_OP_HISTORY)
      ops[nOps].rot = intent ? "Marking as watched..."
                               : "Unmarking as watched...";
    else
      ops[nOps].rot = cat_history_state_item(i) == 1
                        ? "Unmark as watched"
                        : "Mark as watched";
    ops[nOps].action = OP_WATCHED; nOps++;
  }
}

void ctx_open(int index_) {
  if (holdCancelled) {
    holdCancelled = 0;
    holdReady = 0;
    return;
  }
  if (index_ < 0 || index_ >= cat_n() || !cat_item(index_)) return;
  // A longa ja consumiu o gesto na home. Limpar a sentinela aqui evita que o
  // KEYUP seguinte seja reaproveitado como uma selecao dentro da modal.
  holdReady = 0;
  idx = index_; focus = 0; is_open = 1; reqDetails = -1;
  operation = CTX_OP_NONE; intent = 0; stateOperation = 0;
  mirrorApplied = 0;
  operationImdb[0] = 0;
  memset(focusAnim, 0, sizeof focusAnim);
  build();
}

int ctx_is_open(void) { return is_open; }
int ctx_requested_details(void) { int v = reqDetails; reqDetails = -1; return v; }

static void apply(void) {
  int current = indexCurrent();
  const CatItem *ci = current >= 0 ? cat_item(current) : NULL;
  int action;
  if (!ci || focus < 0 || focus >= nOps) return;
  action = ops[focus].action;
  if (action != OP_DETAILS && operation != CTX_OP_NONE &&
      stateOperation != CTX_FAILURE) return;
  switch (action) {
    case OP_DETAILS: reqDetails = idx; break;
    case OP_LIST:
      // Captura a intencao ANTES de qualquer escrita. O mesmo valor segue para
      // o POST e so chega ao espelho local depois de uma resposta 2xx.
      intent = !ci->naList;
      snprintf(operationImdb, sizeof operationImdb, "%s", ci->imdb);
      operation = CTX_OP_LIST;
      mirrorApplied = 0;
      stateOperation = CTX_PENDING;
      if (!trakt_watchlist_kind(ci->imdb, ci->kind, intent))
        stateOperation = CTX_FAILURE;
      build();
      break;
    case OP_WATCHED:
      // Progresso e posicao de retomada, nao historico. So um retrato de
      // historico confirmado pode inverter a acao para "desmarcar".
      intent = cat_history_state_item(current) == 1 ? 0 : 1;
      snprintf(operationImdb, sizeof operationImdb, "%s", ci->imdb);
      operation = CTX_OP_HISTORY;
      mirrorApplied = 0;
      stateOperation = CTX_PENDING;
      if (!trakt_watched_kind(ci->imdb, ci->kind, intent))
        stateOperation = CTX_FAILURE;
      build();
      break;
  }
  if (action == OP_DETAILS) is_open = 0;
}

void ctx_event(const SDL_Event *e) {
  int k;
  if (!is_open) return;
  if (e->type != SDL_KEYDOWN) return;
  k = e->key.keysym.sym;
  if (k == SDLK_AC_BACK || k == SDLK_ESCAPE || k == SDLK_BACKSPACE ||
      e->key.keysym.scancode == NV_SCANCODE_BACK) { is_open = 0; return; }
  // Enquanto a requisicao esta no ar, OK nao repete a escrita. O foco continua
  // sendo o do modal e Voltar sempre pode cancelar a espera visual.
  if (operation != CTX_OP_NONE) {
    if (stateOperation == CTX_PENDING) return;
    if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) {
      if (stateOperation == CTX_FAILURE) apply();
      else is_open = 0;
    }
    if (k != SDLK_UP && k != SDLK_DOWN) return;
  }
  if (k == SDLK_UP)   { if (focus > 0) focus--; return; }
  if (k == SDLK_DOWN) { if (focus + 1 < nOps) focus++; return; }
  if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) { apply(); return; }
}

void ctx_update(float dt, Uint32 now) {
  int i;
  int current;
  if (!holdObserver) {
    SDL_AddEventWatch(observeHold, NULL);
    holdObserver = 1;
  }
  if (holdActive && now - holdSince >= NV_HOLD_MS) holdReady = 1;
  if (settings_animations_reduced())
    anim = is_open ? 1.0f : 0.0f;
  else
    anim = anim_mola(anim, is_open ? 1.0f : 0.0f, dt, NV_MOLA_SCREEN);
  for (i = 0; i < CTX_MAX; i++)
    focusAnim[i] = settings_animations_reduced()
      ? (is_open && focus == i ? 1.0f : 0.0f)
      : anim_mola(focusAnim[i], is_open && focus == i ? 1.0f : 0.0f,
                  dt, NV_MOLA_FOCUS);

  current = indexCurrent();
  if (is_open && current < 0) { is_open = 0; return; }

  if (operation != CTX_OP_NONE && stateOperation == CTX_PENDING) {
    int new = trakt_operation_state(operation);
    if (new == CTX_CONFIRMED || new == CTX_FAILURE) {
      stateOperation = new;
      if (!mirrorApplied && current >= 0) {
        const CatItem *ci = cat_item(current);
        if (ci && new == CTX_CONFIRMED) {
          if (operation == CTX_OP_LIST) {
            cat_set_na_list(current, intent);
          } else {
            cat_history_set_id(ci->imdb, ci->kind, intent);
          }
        }
        mirrorApplied = 1;
        build();
      }
    }
  }
}

void ctx_draw(Uint32 now) {
  const CatItem *ci;
  const char *states[2];
  const char *message = NULL;
  float a = anim, height, x, y;
  int i, nStates = 1;
  (void)now;
  if (!is_open && holdActive) {
    float p = (float)(SDL_GetTicks() - holdSince) / (float)NV_HOLD_MS;
    TxtLine t;
    if (p > 1.0f) p = 1.0f;
    t = txt_line(TXT_CAPTION2,
                  p >= 1.0f ? "Release to open options" : "Hold OK for options",
                  220, 224, 232, 255);
    txt_draw_alpha(t, (NV_TELA_W - t.w) * 0.5f, NV_TELA_H - 124.0f, 0.94f);
    gfx_color((GfxRect){ (NV_TELA_W - 420.0f) * 0.5f, NV_TELA_H - 82.0f,
                       420.0f, 8.0f }, 4.0f, 0.18f, 0.2f, 0.23f, 0.96f);
    gfx_color((GfxRect){ (NV_TELA_W - 420.0f) * 0.5f, NV_TELA_H - 82.0f,
                       420.0f * p, 8.0f }, 4.0f, 0.78f, 0.84f, 0.96f, 0.98f);
  }
  if (a < 0.01f) return;
  ci = indexCurrent() >= 0 ? cat_item(indexCurrent()) : NULL;
  if (!ci) return;

  if (stateOperation == CTX_PENDING)
    message = operation == CTX_OP_LIST ? "Updating library..."
                                        : (intent ? "Marking as watched..."
                                                    : "Unmarking as watched...");
  else if (stateOperation == CTX_CONFIRMED)
    message = operation == CTX_OP_LIST ? "Library updated"
                                        : (intent ? "Marked as watched"
                                                    : "Unmarked as watched");
  else if (stateOperation == CTX_FAILURE)
    message = "Could not update. Try again.";

  states[0] = ci->naList ? "In library" : "Not in library";
  if (!strcmp(ci->kind, "movie") || !strcmp(ci->kind, "series")) {
    { int history = cat_history_state_item(indexCurrent());
      states[1] = history == 1 ? "Watched"
                   : history == 0 ? "Not watched"
                   : ci->progress > 0 ? "Progress saved"
                   : "History not checked"; }
    nStates = 2;
  }

  { GfxRect screen = { 0, 0, NV_TELA_W, NV_TELA_H };
    gfx_color(screen, 0.0f, 0, 0, 0, 0.72f * a); }

  height = CTX_DFLT * 2.0f + CTX_HEADER +
        (float)nOps * (CTX_LINE + CTX_GAP) - CTX_GAP + CTX_FOOTER;
  x = (NV_TELA_W - CTX_W) * 0.5f;
  y = (NV_TELA_H - height) * 0.5f;
  // Sobe do fundo enquanto aparece, como as outras folhas do app.
  y += (1.0f - a) * 40.0f;

  { GfxRect p = { x, y, CTX_W, height };
    gfx_color(p, 0.06f, 0.11f, 0.11f, 0.13f, 0.98f * a); }

  { TxtLine t = txt_line(TXT_CAPTION2, "SELECTED TITLE", 174, 178, 188, 255);
    txt_draw_alpha(t, x + CTX_DFLT, y + CTX_DFLT, a * 0.95f); }
  { TxtLine t = txt_line_trim(TXT_HEADLINE, ci->title, 245, 248, 255, 255,
                                 CTX_W - CTX_DFLT * 2.0f);
    txt_draw_alpha(t, x + CTX_DFLT, y + CTX_DFLT + 28.0f, a); }
  { const char *subtitle = message ? message : "Title options";
    TxtLine t = txt_line(TXT_DET_META2, subtitle, 150, 154, 163, 255);
    txt_draw_alpha(t, x + CTX_DFLT, y + CTX_DFLT + 70.0f, a * 0.9f); }

  { float sx = x + CTX_DFLT;
    float sy = y + CTX_DFLT + 104.0f;
    for (i = 0; i < nStates; i++) {
      TxtLine t = txt_line(TXT_CAPTION2, states[i], 215, 218, 225, 255);
      float sw = t.w + 24.0f;
      gfx_color((GfxRect){ sx, sy, sw, CTX_STATUS_H }, 0.5f,
              0.16f, 0.17f, 0.19f, 0.96f * a);
      txt_draw_alpha(t, sx + 12.0f,
                         sy + (CTX_STATUS_H - t.h) * 0.5f, a);
      sx += sw + CTX_GAP;
    } }

  for (i = 0; i < nOps; i++) {
    float by = y + CTX_DFLT + CTX_HEADER + (float)i * (CTX_LINE + CTX_GAP);
    GfxRect r = { x + CTX_DFLT, by, CTX_W - CTX_DFLT * 2.0f, CTX_LINE };
    float f = focusAnim[i];
    // Mesma linguagem das pilulas: o focado INVERTE (fundo claro, texto
    // escuro), em vez de anel branco sobre preenchimento claro.
    float luma = anim_blend(0.176f, 0.961f, f);
    int color = i == focus ? 17 : 240;
    gfx_color(r, 14.0f / CTX_LINE, luma, luma, luma, a);
    { TxtLine t = txt_line(TXT_PLR_BODY, ops[i].rot, color, color, color, 255);
      txt_draw_alpha(t, r.x + 44.0f,
                         by + (CTX_LINE - t.h) * 0.5f, a); }
    if (f > 0.02f) {
      TxtLine seta = txt_line(TXT_CAPTION2, "▸", color, color, color, 255);
      txt_draw_alpha(seta, r.x + 16.0f,
                         by + (CTX_LINE - seta.h) * 0.5f, a * f);
    }
  }

  { const char *footer = stateOperation == CTX_PENDING
                           ? "Back Close   Please wait..."
                           : operation != CTX_OP_NONE
                           ? "↑ ↓ Navigate   OK Close   Back Close"
                           : "↑ ↓ Navigate   OK Select   Back Close";
    TxtLine t = txt_line(TXT_CAPTION2, footer,
                           155, 159, 169, 255);
    txt_draw_alpha(t, x + CTX_DFLT,
                       y + height - CTX_DFLT - t.h, a * 0.86f); }
}
