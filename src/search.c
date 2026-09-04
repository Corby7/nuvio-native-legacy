// Busca, alinhada com a tela do app web (MEDIDA rodando, perfil do dono).
//
// ------------------------------------------------------------------------
// O QUE MUDOU, E POR QUE
//
// O port tinha um teclado em GRADE a esquerda e uma GRADE de posteres 4x a
// direita. Medida a tela do web, nenhuma das duas coisas confere:
//
//   1. O web nao tem grade de resultados. Tem FILEIRAS horizontais, uma por
//      catalogo de addon, com o nome do catalogo em 48/600 e a origem
//      ("from Xperience") em 20/400 logo abaixo. Card de 248 de largura, poster
//      248x372, nome 28/500 e ano 20/400 embaixo; passo 280 entre cards e 562.4
//      entre fileiras.
//   2. O web nao tem teclado nenhum: tem um <input> largo no topo, e quem
//      levanta o teclado e o SISTEMA da TV.
//
// A (1) foi portada inteira. A (2) NAO da para portar: este app e SDL puro e
// nao existe IME para chamar — sem teclado na tela nao ha como digitar, e uma
// busca em que nao se digita nao e uma busca. O teclado ficou, agora ABAIXO do
// cabecalho e a esquerda, ocupando a faixa onde o web desenha o estado vazio; as
// fileiras de resultado correm a direita dele. E a unica divergencia deliberada
// desta tela, e esta anotada aqui para nao ser confundida com descuido.
//
// DECISAO DE PROJETO — o teclado e em GRADE, nao a linha unica do tvOS.
// A faixa horizontal do tvOS e bonita e cabe em pouca altura, mas custa caro no
// D-pad: sao 38 teclas em UMA dimensao, entao a distancia media entre duas
// letras e ~13 toques e o pior caso passa de 37. A grade 6x7 poe a mesma tecla a
// no maximo 5+6 toques e ~5 em media.
#include "search.h"
#include "gfx.h"
#include "text.h"
#include "tex_cache.h"
#include "focus.h"
#include "anim.h"
#include "layout.h"
#include "settings.h"
#include "catalog.h"
#include "discover.h"
#include <string.h>
#include <stdio.h>

// --- Cabecalho: geometria MEDIDA no web -------------------------------------
// .search-header y=22 h=110, padding lateral 104.
//   .search-discover-btn 110x110 em (104,22)   bg #222, borda 1px #333, raio 22
//   .search-voice-btn    110x110 em (262,22)   -> passo 158 (gap 48)
//   .search-input-field  1396x110 em (420,22)  bg #222, raio 22, 34/500,
//                        padding lateral 32, placeholder "Buscar filmes e séries"
//
// Voz e Descobrir nao aparecem como botoes: nao ha captura de audio ou acao
// de descoberta nesta tela nativa. O campo ocupa toda a largura disponivel.
#define SEARCH_HEAD_Y     NV_SEARCH_HEAD_Y
#define SEARCH_HEAD_H     NV_SEARCH_HEAD_H
#define SEARCH_DIR       (NV_TELA_W - NV_CONTENT_DFLT)   // 1816

// --- Teclado (divergencia deliberada; ver o topo) ----------------------------
#define SEARCH_KEY_W     74.0f
#define SEARCH_KEY_GAP   12.0f
#define SEARCH_KB_COLS      6
#define SEARCH_KB_ROWS  7            // 6 fileiras de A-Z/0-9 + 1 de espaco/apagar
#define SEARCH_KB_STEP   (SEARCH_KEY_W + SEARCH_KEY_GAP)
#define SEARCH_KB_W       (SEARCH_KB_COLS * SEARCH_KEY_W + (SEARCH_KB_COLS - 1) * SEARCH_KEY_GAP)
#define SEARCH_KB_Y       NV_SEARCH_EMPTY_Y   // 148: a faixa do estado vazio do web
// Crescimento da tecla em foco. Menor que o do poster de proposito: a tecla e
// pequena e vizinha imediata das outras, e com 14% ela invade o gap de 12px.
#define SEARCH_KEY_SCALE 0.10f
#define SEARCH_MAX_QUERY 48

// --- Fileiras de resultado (geometria do web) --------------------------------
#define SEARCH_RES_X       (SEARCH_KB_X + SEARCH_KB_W + 64.0f)
#define SEARCH_RES_Y       NV_SEARCH_EMPTY_Y
#define SEARCH_RES_AREA_H  (NV_TELA_H - NV_MARGIN_Y - SEARCH_RES_Y)
#define SEARCH_MAX_ROWS FOCUS_MAX_ROWS
#define SEARCH_MAX_POR_FILTER  12

#define SEARCH_KB_X        NV_CONTENT_DFLT

// --- Estado ------------------------------------------------------------------
static Focus  focusKb;
static Focus  focusRes;
static int   panel = 0;            // 0 = teclado, 1 = resultados
static char  query[SEARCH_MAX_QUERY];
static int   nQuery = 0;
static char queryFiltered[SEARCH_MAX_QUERY];
// Resultados agrupados por CATALOGO, como no web: uma fileira por catalogo que
// teve pelo menos um titulo casando. Guardamos indices do catalogo global.
static struct {
  const char *title;      // nome do catalogo ("Top 100 Today - Filme")
  const char *origin;      // "from <addon>"; vazio quando nao se sabe
  int items[SEARCH_MAX_POR_FILTER];
  int n;
} filter[SEARCH_MAX_ROWS];
static int nFilter = 0;
static int sair = 0;
static int request = -1;             // indice de catalogo escolhido, -1 = nenhum
static float animKey[SEARCH_KB_ROWS][SEARCH_KB_COLS];
static float animRes[SEARCH_MAX_ROWS][SEARCH_MAX_POR_FILTER];
static float scrollY = 0.0f, scrollTarget = 0.0f;
static float scrollX[SEARCH_MAX_ROWS];
static HomeItem itemFocus;
static int   temItemFocus = 0;

static const int KB_COLUMNS[SEARCH_KB_ROWS] = { 6, 6, 6, 6, 6, 6, 3 };
// Minusculas como no aparelho: o campo mostra o que foi digitado, e uma consulta
// em caixa alta le como grito. A comparacao ignora caixa de qualquer forma.
static const char *KEYS =
  "abcdefghijklmnopqrstuvwxyz0123456789";   // 36 = 6 fileiras x 6 colunas

// --- Normalizacao ------------------------------------------------------------
// Dobra uma letra latina acentuada (segundo byte de uma sequencia UTF-8 iniciada
// por 0xC3) na letra ASCII correspondente. Sem isto, buscar "fundacao" nao acha
// "Fundação" — o caso de uso mais obvio da tela, ja que ninguem digita cedilha
// num teclado de D-pad.
static char dobraLatin(unsigned char second) {
  unsigned cp = (unsigned)second + 0x40u;
  if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) cp += 0x20;
  if (cp >= 0xE0 && cp <= 0xE6) return 'a';
  if (cp == 0xE7)               return 'c';
  if (cp >= 0xE8 && cp <= 0xEB) return 'e';
  if (cp >= 0xEC && cp <= 0xEF) return 'i';
  if (cp == 0xF0)               return 'd';
  if (cp == 0xF1)               return 'n';
  if ((cp >= 0xF2 && cp <= 0xF6) || cp == 0xF8) return 'o';
  if (cp >= 0xF9 && cp <= 0xFC) return 'u';
  if (cp == 0xFD || cp == 0xFF) return 'y';
  return ' ';
}

static void normalize(const char *s, char *destination, size_t size) {
  size_t k = 0;
  const unsigned char *p = (const unsigned char *)s;
  while (*p && k + 1 < size) {
    unsigned char c = *p++;
    char output;
    if (c < 0x80) {
      output = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c;
    } else if (c == 0xC3 && *p) {
      output = dobraLatin(*p++);
    } else {
      while ((*p & 0xC0) == 0x80) p++;
      output = ' ';
    }
    destination[k++] = output;
  }
  destination[k] = 0;
}

// --- Filtro ------------------------------------------------------------------
// Uma fileira por CATALOGO, exatamente como o web monta `.search-results-row`.
// Antes isto era uma lista plana do acervo inteiro, o que perdia a informacao de
// ONDE cada resultado foi achado — e e essa informacao que o subtitulo "from
// <addon>" mostra.
static void refilter(void) {
  char target[SEARCH_MAX_QUERY * 2];
  int previous = -1, sameQuery = !strcmp(queryFiltered, query);
  if (sameQuery && panel == 1 && focusRes.row < nFilter &&
      focusRes.column < filter[focusRes.row].n)
    previous = filter[focusRes.row].items[focusRes.column];
  snprintf(queryFiltered, sizeof queryFiltered, "%s", query);
  normalize(query, target, sizeof target);
  nFilter = 0;
  // Menos de 2 caracteres = estado vazio, como o web ("Digite ao menos 2
  // caracteres"). Buscar com uma letra devolve o acervo inteiro e nao ajuda.
  if ((int)strlen(target) < 2) { panel = 0; return; }

  // BUSCA NA REDE. A tela so filtrava o que ja estava em memoria — as ~12
  // primeiras linhas de cada catalogo da home — entao qualquer titulo fora
  // disso simplesmente nao existia para a busca. Dispara e volta na hora; o
  // resultado aparece sozinho quando chegar, porque refiltrar roda a cada
  // tecla e desc_busca_n so responde para o termo corrente.
  disc_fetch(target);

  // UMA FILEIRA POR CATALOGO CONSULTADO, com a origem embaixo — igual ao web,
  // que monta uma `.search-results-row` por catalogo em vez de uma lista unica.
  //
  // Antes so o Cinemeta era consultado e tudo caia numa fileira "Resultados da
  // busca". Com dez alvos numa lista so o dono nao tinha como saber de onde
  // veio nada, e os resultados do addon lento pareciam nunca chegar (chegavam;
  // ficavam no fim de uma fileira de 12 que ja estava cheia de Cinemeta).
  //
  // Os itens entram no catalogo global via cat_acrescentar_lote porque a tela
  // abre titulo por INDICE de catalogo — um resultado que vivesse so aqui nao
  // seria abrivel.
  { int targetIdx, nTargets = disc_search_n_targets();
    for (targetIdx = 0; targetIdx < nTargets && nFilter < SEARCH_MAX_ROWS; targetIdx++) {
      int nRem = disc_search_target_n(targetIdx, target), i;
      // DOIS PASSOS, e a separacao e o conserto: primeiro junta os que ainda
      // NAO estao no catalogo, depois acrescenta TODOS numa troca de bloco so.
      //
      // Antes era cat_acrescentar por resultado, e cada chamada copia o
      // catalogo inteiro: com 300 titulos, ~2,3 MB por copia, ate 40 vezes, no
      // fio de DESENHO, a cada tecla digitada. A busca engasgava por isso.
      CatItem new[SEARCH_MAX_POR_FILTER];
      int idxNew[SEARCH_MAX_POR_FILTER];
      int found = 0, nNew = 0;
      int posNew[SEARCH_MAX_POR_FILTER];   // onde cada novo entra em fil[].itens
      if (nRem <= 0) continue;
      for (i = 0; i < nRem && found < SEARCH_MAX_POR_FILTER; i++) {
        CatItem it;
        int idx;
        if (!disc_search_target_item(targetIdx, i, &it)) continue;
        // Ja esta no catalogo? Reaproveita o indice em vez de duplicar o card.
        idx = it.imdb[0] ? cat_index_por_imdb(it.imdb) : -1;
        if (idx >= 0) {
          filter[nFilter].items[found++] = idx;
        } else if (nNew < SEARCH_MAX_POR_FILTER) {
          new[nNew] = it;
          posNew[nNew] = found++;   // reserva o lugar; o indice vem depois
          nNew++;
        }
      }
      if (nNew > 0) {
        int entered = cat_append_lote(new, nNew, idxNew);
        for (i = 0; i < nNew; i++)
          filter[nFilter].items[posNew[i]] = (i < entered) ? idxNew[i] : -1;
        // O que nao coube (catalogo no teto) vira -1 e e COMPACTADO para fora.
        // So diminuir a contagem deixaria buracos no MEIO da fileira, e o card
        // do buraco apontaria para o item errado — pior que faltar um card.
        if (entered < nNew) {
          int r = 0, w = 0;
          for (r = 0; r < found; r++)
            if (filter[nFilter].items[r] >= 0) filter[nFilter].items[w++] = filter[nFilter].items[r];
          found = w;
        }
      }
      if (found > 0) {
        filter[nFilter].title = disc_search_target_title(targetIdx);
        filter[nFilter].origin = disc_search_target_addon(targetIdx);
        filter[nFilter].n = found;
        nFilter++;
      }
    } }

  int nCat = cat_n_rows();
  for (int r = 0; r < nCat && nFilter < SEARCH_MAX_ROWS; r++) {
    const CatRow *cf = cat_row(r);
    if (!cf) break;
    int found = 0;
    for (int i = 0; i < cf->n && found < SEARCH_MAX_POR_FILTER; i++) {
      const CatItem *ci = cat_item(cf->start + i);
      if (!ci) continue;
      char title[320];
      normalize(ci->title, title, sizeof title);
      if (strstr(title, target)) filter[nFilter].items[found++] = cf->start + i;
    }
    if (!found) continue;
    filter[nFilter].title = cf->title;
    // `catalogAddonNameEnabled` decide a linha "de <addon>" sob o titulo da
    // fileira. O dado NAO existe deste lado: `CatFileira` guarda chave, titulo,
    // tipo e a janela no vetor — o nome do addon fica em addons.c e a fileira
    // nao o carrega. Ate descoberta.c passar esse campo adiante, a linha nao e
    // desenhada; escrever o TIPO ("movie") no lugar seria pior que a ausencia,
    // porque leria como se fosse a origem.
    filter[nFilter].origin = NULL;
    filter[nFilter].n = found;
    nFilter++;
  }

  int cols[SEARCH_MAX_ROWS];
  for (int i = 0; i < nFilter; i++) cols[i] = filter[i].n;
  focus_start(&focusRes, nFilter > 0 ? nFilter : 1, nFilter > 0 ? cols : (int[]){ 1 });
  if (previous >= 0) {
    int found = 0;
    for (int r = 0; r < nFilter && !found; r++)
      for (int c = 0; c < filter[r].n; c++)
        if (filter[r].items[c] == previous) {
          focusRes.row = r; focusRes.column = c;
          focusRes.columnRemembered[r] = c;
          found = 1; break;
        }
  }
  if (nFilter == 0) panel = 0;
  memset(animRes, 0, sizeof animRes);
  if (!sameQuery) {
    memset(scrollX, 0, sizeof scrollX);
    scrollY = scrollTarget = 0.0f;
  }
}

// --- Teclas ------------------------------------------------------------------
static void applyKey(void) {
  if (focusKb.row < SEARCH_KB_ROWS - 1) {
    int k = focusKb.row * SEARCH_KB_COLS + focusKb.column;
    if (nQuery + 1 < SEARCH_MAX_QUERY) query[nQuery++] = KEYS[k];
  } else if (focusKb.column == 0) {
    // espaco no comeco nao entra: nao muda o filtro e so acumula lixo no campo
    if (nQuery > 0 && nQuery + 1 < SEARCH_MAX_QUERY) query[nQuery++] = ' ';
  } else if (focusKb.column == 1) {
    if (nQuery > 0) nQuery--;
  } else {
    nQuery = 0;
  }
  query[nQuery] = 0;
  refilter();
}

static GfxRect rectKey(int row, int column) {
  GfxRect r;
  r.y = SEARCH_KB_Y + row * (SEARCH_KEY_W + SEARCH_KEY_GAP);
  r.h = SEARCH_KEY_W;
  if (row < SEARCH_KB_ROWS - 1) {
    r.x = SEARCH_KB_X + column * SEARCH_KB_STEP;
    r.w = SEARCH_KEY_W;
  } else {
    r.w = (SEARCH_KB_W - 2 * SEARCH_KEY_GAP) / 3;
    r.x = SEARCH_KB_X + column * (r.w + SEARCH_KEY_GAP);
  }
  return r;
}

// --- Ciclo de vida -----------------------------------------------------------
int search_start(void) {
  focus_start(&focusKb, SEARCH_KB_ROWS, KB_COLUMNS);
  panel = 0; sair = 0; request = -1;
  nQuery = 0; query[0] = 0;
  queryFiltered[0] = 0;
  scrollY = scrollTarget = 0.0f;
  temItemFocus = 0;
  memset(animKey, 0, sizeof animKey);
  memset(animRes, 0, sizeof animRes);
  memset(scrollX, 0, sizeof scrollX);
  refilter();
  return 1;
}

void search_shutdown(void) { temItemFocus = 0; }
int  search_wants_exit(void) { return sair; }

int search_requested_open(int *indexCatalog) {
  if (request < 0) return 0;
  if (indexCatalog) *indexCatalog = request;
  request = -1;
  return 1;
}

int search_item_focused(HomeItem *out) {
  if (!temItemFocus || !out) return 0;
  *out = itemFocus;
  return 1;
}

void search_event(const SDL_Event *e) {
  if (e->type == SDL_QUIT) { sair = 1; return; }
  if (e->type != SDL_KEYDOWN) return;
  SDL_Keycode k = e->key.keysym.sym;

  if (k == SDLK_BACKSPACE && panel == 0) {
    if (nQuery > 0) { query[--nQuery] = 0; refilter(); }
    return;
  }
  if (k == SDLK_AC_BACK || k == SDLK_ESCAPE || k == SDLK_BACKSPACE) {
    // Nos resultados, o Back volta ao teclado: e o movimento inverso do que
    // levou ate la. So do teclado ele fecha a tela.
    if (panel == 1) panel = 0; else sair = 1;
    return;
  }

  if (panel == 0) {
    if (!(e->key.keysym.mod & (KMOD_CTRL | KMOD_ALT | KMOD_GUI)) &&
        ((k >= SDLK_a && k <= SDLK_z) || (k >= SDLK_0 && k <= SDLK_9) || k == SDLK_SPACE)) {
      if (nQuery + 1 < SEARCH_MAX_QUERY && (k != SDLK_SPACE || nQuery)) {
        query[nQuery++] = (char)k; query[nQuery] = 0; refilter();
      }
      return;
    }
    if (k == SDLK_TAB && nFilter > 0) { panel = 1; return; }
    switch (k) {
      case SDLK_LEFT:  focus_mover(&focusKb, -1, 0); break;
      case SDLK_RIGHT:
        // Passar da ULTIMA coluna do teclado entra nos resultados. E a unica
        // ponte entre os dois paineis, e por isso ela nao pode falhar em
        // silencio: sem resultado nenhum, o foco fica onde esta.
        if (focusKb.column >= KB_COLUMNS[focusKb.row] - 1) {
          if (nFilter > 0) panel = 1;
        } else focus_mover(&focusKb, 1, 0);
        break;
      case SDLK_UP:     focus_mover(&focusKb, 0, -1); break;
      case SDLK_DOWN:   focus_mover(&focusKb, 0,  1); break;
      case SDLK_RETURN: case SDLK_KP_ENTER: applyKey(); break;
      default: break;
    }
    return;
  }

  switch (k) {
    case SDLK_TAB: panel = 0; break;
    case SDLK_LEFT:
      // Voltar da primeira coluna dos resultados devolve o foco ao teclado.
      if (focusRes.column == 0) panel = 0;
      else focus_mover(&focusRes, -1, 0);
      break;
    case SDLK_RIGHT:
      // `fastHorizontalNavigationEnabled`: o web pula de 3 em 3 dentro da
      // fileira quando a preferencia esta ligada. Numa fileira de 12 cards, 4
      // toques em vez de 12 para chegar ao fim.
      focus_mover(&focusRes, 1, 0);
      if (settings_navigation_horizontal_fast()) {
        focus_mover(&focusRes, 1, 0);
        focus_mover(&focusRes, 1, 0);
      }
      break;
    case SDLK_UP:   focus_mover(&focusRes, 0, -1); break;
    case SDLK_DOWN: focus_mover(&focusRes, 0,  1); break;
    case SDLK_RETURN: case SDLK_KP_ENTER:
      if (focusRes.row < nFilter && focusRes.column < filter[focusRes.row].n)
        request = filter[focusRes.row].items[focusRes.column];
      break;
    default: break;
  }
}

void search_update(float dt, Uint32 now) {
  (void)now;
  // O RESULTADO DA REDE CHEGA DEPOIS DA TECLA. refiltrar() so roda quando o
  // dono digita, entao sem isto a resposta do Cinemeta chegava, ficava guardada
  // e NUNCA aparecia — a tela seguia mostrando o filtro local do momento em que
  // a ultima letra foi apertada. Aqui a contagem do termo corrente e vigiada
  // por quadro, e uma mudanca remonta a lista uma vez so.
  { char target[SEARCH_MAX_QUERY * 2];
    static int lastRemote = -1;
    normalize(query, target, sizeof target);
    if ((int)strlen(target) >= 2) {
      int n = disc_search_n(target);
      if (n != lastRemote) { lastRemote = n; refilter(); }
    } else {
      lastRemote = -1;
    } }
  for (int f = 0; f < SEARCH_KB_ROWS; f++)
    for (int c = 0; c < KB_COLUMNS[f]; c++) {
      float target = (panel == 0 && focus_index(&focusKb, f, c)) ? 1.0f : 0.0f;
      animKey[f][c] = anim_mola(animKey[f][c], target, dt,
                                  target > animKey[f][c] ? NV_MOLA_FOCUS : NV_MOLA_DESFOCO);
    }
  for (int r = 0; r < SEARCH_MAX_ROWS; r++)
    for (int c = 0; c < SEARCH_MAX_POR_FILTER; c++) {
      float target = (panel == 1 && focus_index(&focusRes, r, c)) ? 1.0f : 0.0f;
      animRes[r][c] = anim_mola(animRes[r][c], target, dt,
                                target > animRes[r][c] ? NV_MOLA_FOCUS : NV_MOLA_DESFOCO);
    }

  // Rola so o necessario para a fileira em foco caber inteira na area util —
  // rolagem proporcional ao indice esconderia a primeira fileira antes de o
  // usuario ter chegado nela.
  if (panel == 1 && nFilter > 0) {
    float top = focusRes.row * NV_SEARCH_ROW_STEP;
    float base = top + NV_SEARCH_ROW_RAIL + NV_SEARCH_POSTER_H + 70.0f;
    if (top - scrollTarget < 0.0f)             scrollTarget = top;
    if (base - scrollTarget > SEARCH_RES_AREA_H)    scrollTarget = base - SEARCH_RES_AREA_H;

    // Rolagem horizontal da fileira em foco, mesma regra da home.
    int r = focusRes.row;
    float util = SEARCH_DIR - SEARCH_RES_X;
    float esq = focusRes.column * NV_SEARCH_CARD_STEP;
    float dir = esq + NV_SEARCH_CARD_W;
    float targetX = scrollX[r];
    if (dir - targetX > util) targetX = dir - util;
    if (esq - targetX < 0.0f) targetX = esq;
    if (targetX < 0.0f) targetX = 0.0f;
    scrollX[r] = anim_mola(scrollX[r], targetX, dt, NV_MOLA_SCROLL);
  } else {
    scrollTarget = 0.0f;
  }
  if (scrollTarget < 0.0f) scrollTarget = 0.0f;
  scrollY = anim_mola(scrollY, scrollTarget, dt, NV_MOLA_SCROLL);
}

// --- Desenho -----------------------------------------------------------------
// Campo de consulta: nenhum botao decorativo que nao possa receber foco.
static void drawHeader(Uint32 now) {
  float x = NV_CONTENT_DFLT;
  float radius = NV_SEARCH_RADIUS / (NV_SEARCH_HEAD_H * 0.5f) * 0.5f;  // 22 sobre 110

  GfxRect field = { x, SEARCH_HEAD_Y, SEARCH_DIR - x, NV_SEARCH_HEAD_H };
  gfx_color(field, radius, 0.133f, 0.133f, 0.133f, 1.0f);
  // Contorno do campo — ANEL, nao retangulo cheio.
  //
  // Aqui havia um gfx_cor sobre `campo + 2px`, e gfx_cor PREENCHE: o branco a
  // 22% lavava o campo inteiro. A conta bate com o que se media na tela:
  // 0,133 x 0,78 + 0,961 x 0,22 = 0,315, ou seja #505050 no lugar do #222222
  // que a linha de cima acabou de pintar. O campo lia como controle
  // DESABILITADO, e o texto de exemplo quase sumia dentro dele.
  //
  // GFX_ANEL desenha so o contorno (o miolo fica intacto), que era a intencao
  // escrita no comentario antigo.
  { GfxRect halo = { field.x - 2.0f, field.y - 2.0f,
                     field.w + 4.0f, field.h + 4.0f };
    gfx_rect(halo, 0, GFX_RING, 0, 2.0f / halo.h, 0, radius,
             0.961f, 0.961f, 0.961f, panel == 0 ? 0.55f : 0.16f); }

  float tx = field.x + NV_SEARCH_FIELD_PADX;
  if (nQuery) {
    TxtLine l = txt_line_trim(TXT_HEADLINE, query, 245, 246, 250, 255,
                                field.w - 2 * NV_SEARCH_FIELD_PADX - 12);
    txt_draw(l, tx, field.y + (field.h - l.h) * 0.5f);
    tx += l.w + 6.0f;
  } else {
    // Mesmo texto do placeholder do web.
    TxtLine l = txt_line(TXT_HEADLINE, "Search films and series", 255, 255, 255, 255);
    txt_draw_alpha(l, tx, field.y + (field.h - l.h) * 0.5f, 0.40f);
  }
  // O cursor piscando e o unico sinal de que o campo esta ativo.
  if (panel == 0 && (now / 500) % 2 == 0) {
    GfxRect cur = { tx, field.y + 24.0f, 3.0f, field.h - 48.0f };
    gfx_color(cur, 0.0f, 1.0f, 1.0f, 1.0f, 0.85f);
  }
}

static void drawKeyboard(void) {
  char label[8];
  for (int f = 0; f < SEARCH_KB_ROWS; f++) {
    for (int c = 0; c < KB_COLUMNS[f]; c++) {
      float k = animKey[f][c];
      GfxRect base = rectKey(f, c);
      float esc = 1.0f + SEARCH_KEY_SCALE * k;
      GfxRect t = { base.x - base.w * (esc - 1.0f) * 0.5f,
                    base.y - base.h * (esc - 1.0f) * 0.5f,
                    base.w * esc, base.h * esc };
      // A tecla focada INVERTE (fundo claro, glifo escuro) em vez de so acender:
      // a distancia de sofa, a inversao e o unico contraste que se enxerga de
      // relance numa grade de 38 alvos iguais.
      gfx_color(t, NV_RADIUS_CARD, 1.0f, 1.0f, 1.0f, anim_blend(0.09f, 1.0f, k));
      const char *s;
      if (f < SEARCH_KB_ROWS - 1) {
        label[0] = KEYS[f * SEARCH_KB_COLS + c]; label[1] = 0;
        s = label;
      } else s = (c == 0) ? "espa\xc3\xa7o" : (c == 1 ? "delete" : "clear");
      int tom = (int)anim_blend(236.0f, 26.0f, k);
      TxtStyle st = (f < SEARCH_KB_ROWS - 1) ? TXT_TITLE3 : TXT_HEADLINE;
      TxtLine l = txt_line(st, s, tom, tom, tom, 255);
      txt_draw(l, t.x + (t.w - l.w) * 0.5f, t.y + (t.h - l.h) * 0.5f);
    }
  }
  float y = SEARCH_KB_Y + SEARCH_KB_ROWS * SEARCH_KB_STEP + 24;
  TxtLine hint = txt_line_trim(TXT_CAPTION2,
      nFilter ? "Right: results   •   Back: menu" : "OK: type   •   Back: menu",
      179, 183, 190, 255, SEARCH_KB_W);
  txt_draw(hint, SEARCH_KB_X, y);
}

// Estado vazio do web: titulo 56/600 e apoio 24/400 rgb(179,179,179). Aqui ele
// fica a DIREITA, no lugar das fileiras, porque a faixa central esta com o
// teclado.
static void drawEmpty(void) {
  const char *t1 = nQuery >= 2 ? "No titles received" : "What are we watching?";
  const char *t2 = nQuery >= 2 ? "Results from your addons appear here."
                             : "Type at least 2 letters of a film or series.";
  TxtLine l1 = txt_line(TXT_TITLE2, t1, 255, 255, 255, 255);
  TxtLine l2 = txt_line(TXT_BODY, t2, 179, 179, 179, 255);
  float cx = SEARCH_RES_X + (SEARCH_DIR - SEARCH_RES_X) * 0.5f;
  float y = SEARCH_RES_Y + 180.0f;
  txt_draw_alpha(l1, cx - l1.w * 0.5f, y, 0.96f);
  txt_draw_alpha(l2, cx - l2.w * 0.5f, y + l1.h + 18.0f, 0.85f);
  if (nQuery >= 2) {
    TxtLine help = txt_line(TXT_CAPTION2,
        "If they do not, check the connection or try another name.", 179, 183, 190, 255);
    txt_draw(help, cx - help.w * 0.5f, y + l1.h + l2.h + 42);
  }
}

static void drawResults(Uint32 now) {
  (void)now;
  temItemFocus = 0;
  if (nFilter == 0) { drawEmpty(); return; }

  gfx_crop(SEARCH_RES_X - 8.0f, SEARCH_RES_Y - 30.0f,
              (SEARCH_DIR - SEARCH_RES_X) + 16.0f, SEARCH_RES_AREA_H + 30.0f);

  for (int r = 0; r < nFilter; r++) {
    float ry = SEARCH_RES_Y + r * NV_SEARCH_ROW_STEP - scrollY;
    if (ry > NV_TELA_H + 100.0f || ry + NV_SEARCH_ROW_STEP < -100.0f) continue;

    // Titulo do catalogo 48/600 e a origem 20/400 logo abaixo (margin-top 4).
    TxtLine tt = txt_line_trim(TXT_TITLE3, filter[r].title, 255, 255, 255, 255,
                                  SEARCH_DIR - SEARCH_RES_X);
    txt_draw(tt, SEARCH_RES_X, ry);
    if (filter[r].origin) {
      char org[96];
      snprintf(org, sizeof org, "de %s", filter[r].origin);
      TxtLine ts = txt_line_trim(TXT_CAPTION2, org, 179, 179, 179, 255,
                                   SEARCH_DIR - SEARCH_RES_X);
      txt_draw_alpha(ts, SEARCH_RES_X, ry + NV_SEARCH_ROW_SUB, 0.95f);
    }

    float cardY = ry + NV_SEARCH_ROW_RAIL;
    // Dois passes: o item em foco tem de ficar POR CIMA dos vizinhos, senao a
    // borda do poster ao lado corta o anel de foco.
    for (int passe = 0; passe < 2; passe++)
      for (int c = 0; c < filter[r].n; c++) {
        float f = animRes[r][c];
        if ((passe == 1) != (f > 0.01f)) continue;
        const CatItem *ci = cat_item(filter[r].items[c]);
        if (!ci) continue;

        float px = SEARCH_RES_X + c * NV_SEARCH_CARD_STEP - scrollX[r];
        if (px > SEARCH_DIR || px + NV_SEARCH_CARD_W < SEARCH_RES_X - NV_SEARCH_CARD_W) continue;
        GfxRect poster = { px, cardY, NV_SEARCH_CARD_W, NV_SEARCH_POSTER_H };
        // O card do web NAO escala no foco: marca por borda de 2px, como a home.
        float radius = NV_SEARCH_RADIUS / NV_SEARCH_CARD_W;
        if (f > 0.01f) {
          GfxRect b = { poster.x - 2.0f, poster.y - 2.0f,
                        poster.w + 4.0f, poster.h + 4.0f };
          gfx_color(b, radius, 0.961f, 0.961f, 0.961f, f);
        }

        const char *art = ci->poster[0] ? ci->poster
                         : (ci->backdrop[0] ? ci->backdrop : NULL);
        GLuint tex = art ? tex_get_width(art, poster.w) : 0;
        if (tex) {
          // Sem o aspecto a arte 2:3 estica; e o poster e justamente onde isso
          // salta aos olhos, porque todos ficam lado a lado.
          gfx_tex_aspect_current = tex_aspect(art);
          gfx_rect(poster, tex, GFX_CARD, f, 0.0f, 0.0f, radius, 0, 0, 0, 1);
          gfx_tex_aspect_current = 0.0f;
        } else {
          // Esqueleto VISIVEL, o mesmo da home: #2C2C2C. Ver a nota la — placeholder
            // do tom do fundo le como card quebrado, nao como carregando.
            gfx_color(poster, radius, NV_COLOR_SKELETON_R, NV_COLOR_SKELETON_G,
                  NV_COLOR_SKELETON_B, 1.0f);
        }

        // Nome 28/500 branco a 8 do poster; ano 20/400 rgb(179) a 4 do nome.
        TxtLine tn = txt_line_trim(TXT_CALLOUT, ci->title, 255, 255, 255, 255,
                                      NV_SEARCH_CARD_W);
        float ny = poster.y + poster.h + NV_SEARCH_NAME_GAP;
        txt_draw_alpha(tn, poster.x, ny, anim_blend(0.82f, 1.0f, f));
        if (ci->meta[0]) {
          TxtLine td = txt_line_trim(TXT_CAPTION2, ci->meta, 179, 179, 179, 255,
                                        NV_SEARCH_CARD_W);
          txt_draw_alpha(td, poster.x, ny + tn.h + NV_SEARCH_DATE_GAP, 0.92f);
        }

        if (panel == 1 && focus_index(&focusRes, r, c)) {
          itemFocus.index_ = filter[r].items[c];
          itemFocus.rect   = poster;
          itemFocus.art   = ci->backdrop[0] ? ci->backdrop : ci->poster;
          itemFocus.title = ci->title;
          itemFocus.genre = ci->genre;
          itemFocus.meta   = ci->meta;
          temItemFocus = 1;
        }
      }
  }
  gfx_sem_crop();
}

void search_draw(Uint32 now) {
  // Fundo #0d0d0d, medido no .search-screen-shell do web — mais escuro que o
  // cinza da home, e o web usa o mesmo tom nas duas.
  GfxRect screen = { 0, 0, NV_TELA_W, NV_TELA_H };
  // A tela ja foi limpa com ESTA MESMA COR por glClearColor/glClear em
  // main.c antes de app_desenhar. Pintar por cima era uma camada de tela
  // cheia jogada fora por quadro — e o custo dominante nesta GPU e fill
  // rate (gfx.c registra que DUAS camadas de tela cheia derrubavam a
  // Mali-G71 para ~40fps). Nao repor sem antes mudar a cor do clear.
  (void)screen;
  drawHeader(now);
  drawKeyboard();
  drawResults(now);
}
