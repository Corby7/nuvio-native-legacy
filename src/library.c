// Biblioteca, alinhada com a tela do app web (MEDIDA rodando, perfil do dono).
//
// ------------------------------------------------------------------------
// O QUE MUDOU, E POR QUE
//
// O port tinha tres pilulas CENTRALIZADAS ("Minha Lista" / "Comprados" /
// "Gêneros") e uma grade de 6 colunas de 212. Medida a tela do web, a estrutura
// e outra e tem QUATRO faixas, todas alinhadas a esquerda em x=96:
//
//   .library-page-title    "Biblioteca" 56/600, letter-spacing 1, em (96,48)
//   .library-page-source   selo "NUVIO" 28/500 rgb(128,128,128) ls 4, a DIREITA
//   .library-view-mode-row y=136: pilulas 150x56 raio 999, 21/400 — "Salvos" e
//                          "Nuvem"; escolhida bg #303030 borda 2px #fff, as
//                          outras bg #222 borda 2px #333
//   .library-picker-row    y=212: DOIS seletores 840x110 raio 36 — "Tipo" e
//                          "Ordenar" —, cada um com rotulo 19/500 rgb(128) e
//                          valor 30/500 branco embaixo, e uma seta a direita
//   .library-grid          6 colunas de 268 (auto-fill com minimo 252 sobre os
//                          1728 uteis, gutter 24), poster 2:3 = 268x402 raio 24
//                          com borda de 4px POR DENTRO, titulo 32/500 a 16 do
//                          poster; passo de linha 487.8
//
// As duas dimensoes do web ("Salvos/Nuvem" e o filtro de Tipo) substituem as
// tres abas inventadas. "Gêneros" nao existe no web e saiu.
//
// Duas decisoes que vieram de erros ja cometidos em outras telas deste app:
//
//   1. A pilula ESCOLHIDA continua marcada quando o foco desce para a grade. Foi
//      o mesmo problema das abas de temporada do detalhe: sem o estado de
//      escolha separado do foco, o usuario perde de vista onde esta.
//   2. A rolagem move o MINIMO para a linha focada caber. Alinhar a linha focada
//      ao topo empurra o cabecalho para fora da tela na primeira descida.
#include "library.h"
#include "trakt.h"
#include "gfx.h"
#include "text.h"
#include "tex_cache.h"
#include "focus.h"
#include "anim.h"
#include "layout.h"
#include "settings.h"
#include "catalog.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

// Fileiras de foco: 0 = modos (Salvos/Nuvem), 1 = seletores (Tipo/Ordenar),
// 2.. = grade.
#define LIB_FILTER_MODE   0
#define LIB_FILTER_PICK   1
#define LIB_FILTER_GRID  2
#define LIB_MAX_LINES (FOCUS_MAX_ROWS - LIB_FILTER_GRID)
#define LIB_GRID_BASE (NV_SCREEN_H - NV_MARGIN_Y)
// Em quantos px um poster desaparece ao subir por baixo do cabecalho. O recorte
// de tesoura resolveria, mas gfx_recorte assume alvo 1:1 com a tela e o Mac em
// retina entrega o dobro; o esmaecimento nao depende do drawable.
#define LIB_FADE       90.0f

// "Salvos" e a lista do proprio aparelho; "Nuvem" e o que veio do Trakt.
enum { MODE_SAVED, MODE_CLOUD, LIB_N_MODES };
static const char *ROT_MODE[LIB_N_MODES] = { "Saved", "Collection" };

// Seletor "Tipo": os mesmos valores do web.
enum { KIND_ALL, KIND_MOVIE, KIND_SERIES, LIB_N_KINDS };
static const char *ROT_KIND[LIB_N_KINDS] = { "All", "Films", "Series" };
// Seletor "Ordenar".
enum { ORDER_ADDED, ORDER_TITLE, ORDER_YEAR, LIB_N_ORDER };
static const char *ROT_ORDER[LIB_N_ORDER] = { "List order", "Title: A to Z", "Year: newest first" };

static int mode = MODE_SAVED;
static int kind = KIND_ALL;
static int order = ORDER_ADDED;
static int pickSel = 0;          // qual dos dois seletores esta em foco

static int filter[CAT_MAX];      // indices do catalogo visiveis
static int nFilter = 0;
static int totalMode = 0;
static Focus focus;
static float animMode[LIB_N_MODES];
static float animPick[2];
static float animFocus[LIB_MAX_LINES][NV_LIB_COLUMNS];
static float scrollY = 0.0f;
static int sair = 0, request = -1;

// Estado de conta. A lista de verdade e a do Trakt, que marca
// ci->naLista/naColecao NO ITEM — nao ha mais tabela por indice aqui: o
// catalogo e reconstruido da rede e um indice guardado aponta para outro titulo
// na volta seguinte. `comprado` sobrevive porque nao ha fonte para ele ainda.
static char bought[CAT_MAX];

static float heightLine(void) {
  // poster + gap + titulo (32/500, lh 1.18 -> 37.8)
  return NV_LIB_POSTER_H + NV_LIB_TITLE_GAP + 37.8f;
}
static float stepLine(void)  { return NV_LIB_LINE_STEP; }
static float stepColumn(void) { return NV_LIB_CARD_W + NV_LIB_CARD_GAP; }
static int   nLines(void)     { return (nFilter + NV_LIB_COLUMNS - 1) / NV_LIB_COLUMNS; }

static int isSeries(const CatItem *ci) {
  return ci && (!strcmp(ci->kind, "series") || ci->nSeasons > 0
                || ci->season > 0);
}

// Refaz a lista visivel e o mapa de foco. Chamada a cada troca de modo, tipo ou
// ordem porque o numero de colunas da ultima linha muda com o filtro, e um foco
// apontando para uma coluna que nao existe mais desenha um retangulo vazio.
static void rebuild(void) {
  int n = cat_n();
  if (n > CAT_MAX) n = CAT_MAX;
  nFilter = 0;
  totalMode = 0;
  for (int i = 0; i < n; i++) {
    const CatItem *ci = cat_item(i);
    if (!ci) continue;
    // "Salvos" = QUERO VER (watchlist do Trakt). "Nuvem" = TENHO (colecao).
    //
    // Os dois modos mostravam quase a MESMA lista: ambos incluiam ci->naLista,
    // entao trocar de pilula praticamente nao mudava nada e as duas nao tinham
    // razao de existir. A divisao agora e a do proprio Trakt, que separa
    // watchlist (o que se pretende ver) de collection (o que se possui) — sao
    // perguntas diferentes e cada pilula responde uma.
    //
    // E le do ITEM, nao mais do vetor naLista[] indexado por posicao. Aquele
    // vetor era um erro conhecido e documentado: o catalogo e RECONSTRUIDO da
    // rede a cada descoberta, entao a posicao 3 de hoje e outro titulo amanha —
    // a marca "salvo" migrava sozinha para um filme que ninguem salvou. A marca
    // tem de viver no item, e vive (CatItem.naLista / .naColecao, preenchidos
    // com a lista de verdade do Trakt).
    int enters = (mode == MODE_SAVED) ? ci->inList
                                      : (ci->inCollection || bought[i]);
    if (!enters) continue;
    totalMode++;
    if (kind == KIND_MOVIE && isSeries(ci)) continue;
    if (kind == KIND_SERIES && !isSeries(ci)) continue;
    // `hideUnreleasedContent`: sem ano em `meta` o titulo ainda nao estreou do
    // ponto de vista do catalogo, e a preferencia manda escondê-lo.
    if (settings_hide_unreleased() && !ci->meta[0]) continue;
    filter[nFilter++] = i;
  }

  // Ordenacao por insercao — sao poucas dezenas de itens, uma vez por troca.
  if (order != ORDER_ADDED) {
    for (int i = 1; i < nFilter; i++) {
      int v = filter[i], j = i - 1;
      while (j >= 0) {
        const CatItem *a = cat_item(filter[j]), *b = cat_item(v);
        int larger;
        if (order == ORDER_TITLE) larger = a && b && strcmp(a->title, b->title) > 0;
        else /* ORD_ANO, decrescente */
          larger = a && b && strcmp(a->meta, b->meta) < 0;
        if (!larger) break;
        filter[j + 1] = filter[j]; j--;
      }
      filter[j + 1] = v;
    }
  }

  int lines = nLines();
  if (lines > LIB_MAX_LINES) lines = LIB_MAX_LINES;
  int cols[FOCUS_MAX_ROWS];
  cols[LIB_FILTER_MODE] = LIB_N_MODES;
  cols[LIB_FILTER_PICK] = 2;
  for (int r = 0; r < lines; r++) {
    int rest = nFilter - r * NV_LIB_COLUMNS;
    cols[LIB_FILTER_GRID + r] = rest > NV_LIB_COLUMNS ? NV_LIB_COLUMNS : rest;
  }
  focus_start(&focus, LIB_FILTER_GRID + lines, cols);
  scrollY = 0.0f;
  memset(animFocus, 0, sizeof animFocus);
}

static int started;
int library_start(void) {
  // Zerado UMA vez por processo, e nao a cada entrada.
  if (!started) {
    memset(bought, 0, sizeof bought);
    started = 1;
  }
  mode = MODE_SAVED; kind = KIND_ALL; order = ORDER_ADDED;
  pickSel = 0;
  memset(animMode, 0, sizeof animMode);
  memset(animPick, 0, sizeof animPick);
  sair = 0; request = -1;
  rebuild();
  // O foco nasce na barra de modos: quem entra ainda esta escolhendo o recorte.
  focus.row = LIB_FILTER_MODE;
  focus.column = mode;
  return 1;
}

void library_shutdown(void) { }

int library_in_list(int i) {
  // A verdade e a marca DO ITEM, que a descoberta preenche com a watchlist do
  // Trakt. O vetor por indice que respondia aqui apontava para outro titulo
  // assim que o catalogo era reconstruido.
  const CatItem *c = cat_item(i);
  return c ? c->inList : 0;
}
int library_bought(int i) { return (i >= 0 && i < CAT_MAX) ? bought[i] : 0; }
void library_toggle_list(int i) {
  // So remonta a lista. Quem vira a marca e cat_definir_na_lista, no mesmo
  // ponto que fala com o Trakt (app.c) — ter DOIS donos do mesmo estado era o
  // que deixava a biblioteca discordando do botao "+" do detalhe.
  (void)i;
  rebuild();
}

int library_wants_exit(void) { return sair; }

int library_requested_open(int *indexCatalog) {
  if (request < 0) return 0;
  if (indexCatalog) *indexCatalog = request;
  request = -1;
  return 1;
}

void library_event(const SDL_Event *e) {
  if (e->type != SDL_KEYDOWN) return;
  SDL_Keycode k = e->key.keysym.sym;
  if (k == SDLK_ESCAPE || k == SDLK_AC_BACK || k == SDLK_BACKSPACE ||
      k == SDLK_DELETE) { sair = 1; return; }

  // Barra de modos: esquerda/direita TROCA o modo, e trocar refaz o mapa de
  // foco. Por isso o modo muda AQUI e nao por focus_mover — chamar os dois na
  // ordem errada devolvia o foco para a coluna 0 a cada movimento.
  if (focus.row == LIB_FILTER_MODE) {
    if (k == SDLK_RIGHT && mode < LIB_N_MODES - 1) {
      mode++; rebuild(); focus.row = LIB_FILTER_MODE; focus.column = mode; return;
    }
    if (k == SDLK_LEFT && mode > 0) {
      mode--; rebuild(); focus.row = LIB_FILTER_MODE; focus.column = mode; return;
    }
    if (k == SDLK_DOWN || k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) {
      focus.row = LIB_FILTER_PICK; focus.column = pickSel;
    }
    return;
  }

  // Linha de seletores: esquerda/direita anda ENTRE os dois; OK cicla o valor do
  // que esta em foco. O web abre um menu suspenso; num D-pad, ciclar no proprio
  // seletor poupa a viagem de ida e volta ate a lista.
  if (focus.row == LIB_FILTER_PICK) {
    if (k == SDLK_RIGHT && pickSel == 0) { pickSel = 1; focus.column = 1; return; }
    if (k == SDLK_LEFT  && pickSel == 1) { pickSel = 0; focus.column = 0; return; }
    if (k == SDLK_UP)   { focus.row = LIB_FILTER_MODE; focus.column = mode; return; }
    if (k == SDLK_DOWN) { if (nFilter) focus_mover(&focus, 0, 1); return; }
    if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) {
      if (pickSel == 0) kind = (kind + 1) % LIB_N_KINDS;
      else              order = (order + 1) % LIB_N_ORDER;
      rebuild();
      focus.row = LIB_FILTER_PICK; focus.column = pickSel;
    }
    return;
  }

  if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) {
    int i = (focus.row - LIB_FILTER_GRID) * NV_LIB_COLUMNS + focus.column;
    if (i >= 0 && i < nFilter) request = filter[i];
    return;
  }
  if (k == SDLK_RIGHT)     focus_mover(&focus, 1, 0);
  else if (k == SDLK_LEFT) focus_mover(&focus, -1, 0);
  else if (k == SDLK_DOWN) focus_mover(&focus, 0, 1);
  else if (k == SDLK_UP) {
    if (focus.row == LIB_FILTER_GRID) { focus.row = LIB_FILTER_PICK; focus.column = pickSel; }
    else focus_mover(&focus, 0, -1);
  }
}

void library_update(float dt, Uint32 now) {
  (void)now;
  for (int a = 0; a < LIB_N_MODES; a++) {
    float target = (focus.row == LIB_FILTER_MODE && focus.column == a) ? 1.0f : 0.0f;
    animMode[a] = anim_spring(animMode[a], target, dt,
                            target > animMode[a] ? NV_SPRING_FOCUS : NV_SPRING_BLUR);
  }
  for (int p = 0; p < 2; p++) {
    float target = (focus.row == LIB_FILTER_PICK && focus.column == p) ? 1.0f : 0.0f;
    animPick[p] = anim_spring(animPick[p], target, dt,
                            target > animPick[p] ? NV_SPRING_FOCUS : NV_SPRING_BLUR);
  }
  int lines = nLines();
  if (lines > LIB_MAX_LINES) lines = LIB_MAX_LINES;
  for (int r = 0; r < lines; r++)
    for (int c = 0; c < NV_LIB_COLUMNS; c++) {
      float target = focus_index(&focus, LIB_FILTER_GRID + r, c) ? 1.0f : 0.0f;
      animFocus[r][c] = anim_spring(animFocus[r][c], target, dt,
                                 target > animFocus[r][c] ? NV_SPRING_FOCUS : NV_SPRING_BLUR);
    }

  // Rola o MINIMO para a linha focada caber inteira na area util. Com o foco no
  // cabecalho o alvo e 0 — voltar ao topo faz parte de voltar para a barra.
  float target = scrollY;
  if (focus.row >= LIB_FILTER_GRID) {
    float top = NV_LIB_GRID_Y + (focus.row - LIB_FILTER_GRID) * stepLine();
    float base = top + heightLine();
    if (base - target > LIB_GRID_BASE)  target = base - LIB_GRID_BASE;
    if (top - target < NV_LIB_GRID_Y)  target = top - NV_LIB_GRID_Y;
  } else {
    target = 0.0f;
  }
  if (target < 0.0f) target = 0.0f;
  scrollY = anim_spring(scrollY, target, dt, NV_SPRING_SCROLL);
}

// Pilula de modo. Escolhida sem foco fica com fundo #303030 e borda branca; com
// foco clareia e o texto escurece. As duas leituras tem de continuar distintas —
// e o erro que as abas de temporada do detalhe ja cometeram.
static void drawMode(int a, float f) {
  GfxRect r = { NV_LIB_X + a * NV_LIB_MODE_STEP, NV_LIB_MODE_Y,
                NV_LIB_MODE_W, NV_LIB_MODE_H };
  int sel = (a == mode);
  float radius = NV_RADIUS_PILL;
  if (f > 0.02f) {
    GfxRect b = { r.x - 2.0f, r.y - 2.0f, r.w + 4.0f, r.h + 4.0f };
    gfx_color(b, radius, 0.961f, 0.961f, 0.961f, f);
  }
  float luma = sel ? 0.188f : 0.133f;        // #303030 contra #222
  gfx_color(r, radius, luma, luma, luma, 1.0f);
  if (sel && f < 0.98f)
    gfx_rect(r, 0, GFX_RING, 0, 1.0f / r.h, 0, radius,
             0.70f, 0.70f, 0.72f, 1.0f - f);
  int color = 245;
  TxtLine l = txt_line(TXT_CAPTION2, ROT_MODE[a], color, color, color, 255);
  txt_draw_alpha(l, r.x + (r.w - l.w) * 0.5f, r.y + (r.h - l.h) * 0.5f,
                     sel ? 1.0f : 0.82f);
}

// Seletor "Tipo" / "Ordenar": rotulo pequeno em cinza e valor grande em branco,
// com a seta encostada na direita.
static void drawPicker(int p, float f) {
  GfxRect r = { NV_LIB_X + p * NV_LIB_PICK_STEP, NV_LIB_PICK_Y,
                NV_LIB_PICK_W, NV_LIB_PICK_H };
  float radius = NV_LIB_PICK_RADIUS / (NV_LIB_PICK_H * 0.5f) * 0.5f;
  if (f > 0.02f) {
    GfxRect b = { r.x - 2.0f, r.y - 2.0f, r.w + 4.0f, r.h + 4.0f };
    gfx_color(b, radius, 0.961f, 0.961f, 0.961f, f);
  }
  float luma = anim_blend(0.133f, 0.188f, f);   // #222 -> #303030 no foco
  gfx_color(r, radius, luma, luma, luma, 1.0f);

  const char *rot = (p == 0) ? "Type" : "Ordenar";
  const char *val = (p == 0) ? ROT_KIND[kind] : ROT_ORDER[order];
  // 19/500 rgb(128,128,128) em cima, 30/500 branco embaixo com 4 de folga.
  TxtLine tr = txt_line(TXT_MINI, rot, 179, 179, 179, 255);
  TxtLine tv = txt_line(TXT_CALLOUT, val, 255, 255, 255, 255);
  float tx = r.x + NV_LIB_PICK_PADX;
  float ty = r.y + NV_LIB_PICK_PADY;
  txt_draw_alpha(tr, tx, ty, 0.95f);
  txt_draw_alpha(tv, tx, ty + tr.h + 4.0f, 1.0f);

  TxtLine seta = txt_line(TXT_CAPTION2, "OK: alterar", 196, 197, 202, 255);
  txt_draw_alpha(seta, r.x + r.w - NV_LIB_PICK_PADX - seta.w,
                     r.y + (r.h - seta.h) * 0.5f, 0.85f);
}

// Estado vazio: 46/500 branco e 28/400 rgb(179,179,179), centrado na largura
// util. Uma grade em branco parece tela quebrada.
static void drawEmpty(void) {
  const char *l1 = totalMode ? "No titles under this filter"
      : mode == MODE_CLOUD ? "Your collection appears here" : "Your next session starts here";
  const char *l2 = totalMode
      ? "Under Type, choose All. Check the filters in Settings too."
      : mode == MODE_CLOUD
        ? "The films and series in your Trakt collection are gathered in this tab."
        : "Open a film or series and choose Add to list to keep it.";
  TxtLine t1 = txt_line(TXT_TITLE2, l1, 255, 255, 255, 255);
  TxtLine t2 = txt_line(TXT_CALLOUT, l2, 179, 179, 179, 255);
  float cx = NV_LIB_X + NV_LIB_W * 0.5f;
  float y = NV_LIB_EMPTY_Y + 190.0f;
  gfx_icon((GfxRect){cx - 32.0f, y - 100.0f, 64.0f, 64.0f},
             "menu_library", 0.70f, 0.70f, 0.72f, 1.0f);
  txt_draw_alpha(t1, cx - t1.w * 0.5f, y, 0.96f);
  txt_draw_alpha(t2, cx - t2.w * 0.5f, y + t1.h + 18.0f, 0.85f);
  TxtLine hint = txt_line(TXT_CAPTION2,
      "↑ Back to filters   ·   Back: menu", 179, 179, 179, 255);
  txt_draw(hint, cx - hint.w * 0.5f, y + t1.h + t2.h + 58.0f);
}

void library_draw(Uint32 now) {
  (void)now;
  // Fundo opaco proprio: a biblioteca cobre a tela inteira e nao pode contar com
  // quem desenhou antes dela.
  GfxRect screen = { 0, 0, NV_SCREEN_W, NV_SCREEN_H };
  // A tela ja foi limpa com ESTA MESMA COR por glClearColor/glClear em
  // main.c antes de app_desenhar. Pintar por cima era uma camada de tela
  // cheia jogada fora por quadro — e o custo dominante nesta GPU e fill
  // rate (gfx.c registra que DUAS camadas de tela cheia derrubavam a
  // Mali-G71 para ~40fps). Nao repor sem antes mudar a cor do clear.
  (void)screen;

  TxtLine title = txt_line(TXT_TITLE2, "Library", 255, 255, 255, 255);
  txt_draw(title, NV_LIB_X, NV_LIB_Y);
  // Selo de origem, alinhado a direita da area util. Espacado de proposito: no
  // web ele tem letter-spacing 4 e le como etiqueta, nao como palavra.
  //
  // O SELO DIZ A ORIGEM DE VERDADE. Estava cravado em "NUVIO", que e o nome do
  // app e nao a fonte dos dados — a lista vem do TRAKT, e o log confirma
  // ("[trakt] credencial carregada", "[trakt] watchlist: 118"). Selo de origem
  // que nao reflete a origem e da mesma familia da classificacao "14" e do
  // elenco de demonstracao: informacao inventada com cara de dado.
  //
  // Sem credencial do Trakt a biblioteca e local, e o selo diz isso.
  { const char *source = trakt_active() ? "TRAKT" : "LOCAL";
    float wBadge = txt_tracking(TXT_CALLOUT, source, 128, 128, 128,
                               -1.0f, 0.0f, 0.0f, 4.0f);
    txt_tracking(TXT_CALLOUT, source, 128, 128, 128,
                 NV_LIB_DIR - wBadge, NV_LIB_Y + 10.0f, 0.9f, 4.0f); }

  for (int a = 0; a < LIB_N_MODES; a++) drawMode(a, animMode[a]);
  {
    char summary[160];
    snprintf(summary, sizeof summary, "%d %s   ·   %s", nFilter,
             nFilter == 1 ? "title" : "titles",
             mode == MODE_SAVED ? "Your watchlist" : "Your Trakt collection");
    TxtLine info = txt_line(TXT_CAPTION2, summary, 179, 179, 179, 255);
    txt_draw(info, NV_LIB_DIR - info.w,
                 NV_LIB_MODE_Y + (NV_LIB_MODE_H - info.h) * 0.5f);
  }
  for (int p = 0; p < 2; p++)           drawPicker(p, animPick[p]);

  if (nFilter == 0) { drawEmpty(); return; }

  int lines = nLines();
  if (lines > LIB_MAX_LINES) lines = LIB_MAX_LINES;
  float stepC = stepColumn(), stepL = stepLine();

  // Dois passes: o item focado escala 2% e precisa ser desenhado por ULTIMO,
  // senao o vizinho da direita corta a borda dele.
  for (int passe = 0; passe < 2; passe++)
    for (int r = 0; r < lines; r++) {
      float top = NV_LIB_GRID_Y + r * stepL - scrollY;
      if (top > NV_SCREEN_H || top + heightLine() < -80.0f) continue;
      // O que sobe para baixo do cabecalho some antes de cruza-lo: sem o
      // esmaecimento, poster e seletor se leem um sobre o outro.
      float a = anim_clamp((top - (NV_LIB_PICK_Y + NV_LIB_PICK_H * 0.5f)) / LIB_FADE,
                           0.0f, 1.0f);
      if (a <= 0.005f) continue;

      for (int c = 0; c < NV_LIB_COLUMNS; c++) {
        int i = r * NV_LIB_COLUMNS + c;
        if (i >= nFilter) break;
        float f = animFocus[r][c];
        if ((passe == 0) == (f > 0.01f)) continue;

        // `.library-grid-card.focused { transform: scale(1.02) }` com origem no
        // TOPO — e a unica escala de foco que o web tem, e ela e de 2%, nao dos
        // 14% que estavam aqui (numero das tabelas de Top Shelf do tvOS).
        float esc = 1.0f + NV_LIB_FOCUS_SCALE * f;
        float bw = NV_LIB_CARD_W * esc, bh = NV_LIB_POSTER_H * esc;
        float bx = NV_LIB_X + c * stepC - (bw - NV_LIB_CARD_W) * 0.5f;
        GfxRect card = { bx, top, bw, bh };
        float radius = 24.0f / NV_LIB_CARD_W;

        const CatItem *ci = cat_item(filter[i]);
        const char *art = (ci && ci->poster[0]) ? ci->poster : NULL;
        GLuint tex = art ? tex_get(art) : 0;
        if (tex) {
          gfx_tex_aspect_current = tex_aspect(art);
          gfx_rect(card, tex, GFX_CARD, f, 0.0f, 0.0f, radius, 0, 0, 0, a);
          gfx_tex_aspect_current = 0.0f;
        } else {
          // Placeholder na cor dos cards: o poster ainda esta decodificando em
          // outra thread e a grade nao pode piscar buraco.
          // Esqueleto VISIVEL, o mesmo da home: #2C2C2C. Ver a nota la — placeholder
            // do tom do fundo le como card quebrado, nao como carregando.
            gfx_color(card, radius, NV_COLOR_SKELETON_R, NV_COLOR_SKELETON_G,
                  NV_COLOR_SKELETON_B, a);
        }
        // A borda de foco do web e de 4px POR DENTRO do poster (o card ja
        // reserva `border: 4px solid transparent`), e nao um halo por fora —
        // "Android TV uses the inside focus border, not an outer halo", diz o
        // proprio comentario da folha.
        if (f > 0.01f) {
          gfx_rect(card, 0, GFX_RING, 0, NV_LIB_POSTER_BORDER / card.w,
                   0, radius, 0.961f, 0.961f, 0.961f, f * a);
        }

        // Titulo 32/500, uma linha, cortado com reticencias — o web usa
        // white-space:nowrap + text-overflow:ellipsis.
        if (ci) {
          TxtLine tl = txt_line_trim(TXT_CALLOUT, ci->title, 255, 255, 255, 255,
                                        NV_LIB_CARD_W);
          txt_draw_alpha(tl, NV_LIB_X + c * stepC,
                             top + NV_LIB_POSTER_H + NV_LIB_TITLE_GAP, a * 0.98f);
        }
      }
    }
}
