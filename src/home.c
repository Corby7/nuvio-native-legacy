// Home nativa compatível com a interface moderna do Nuvio 1.0.1 legacy:
// hero no topo, rail fixa à esquerda e fileiras horizontais de posters. A
// infraestrutura nativa cuida de cache assíncrono, foco e transições.
#include "home.h"
#include "resume.h"
#include "seeall.h"
#include "ctxmenu.h"
#include "mark.h"
#include "gfx.h"
#include "text.h"
#include "tex_cache.h"
#include "focus.h"
#include "anim.h"
#include "layout.h"
#include "settings.h"
#include "catalog.h"
#include "collections.h"
#include "discover.h"
#include "badges.h"
#include "extras.h"
#include "director.h"
#include <strings.h>
// Declarado a mao em vez de incluir detail.h: aquele header inclui ESTE (por
// causa do HomeItem), e o ciclo so nao explode por causa das guardas. Uma
// funcao de uma linha nao vale amarrar os dois arquivos.
float detail_progress(void);
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <stdlib.h>
#include <math.h>
#include <ctype.h>

#define MAX_ART   64
// TWICE the catalogue cap (CAT_FILTER_MAX, currently 24), because the home's rows
// are not only catalogues: the owner's collections, the packaged groups and the
// synthetic ones (Continue watching, Among friends) all land here too. This array
// is the headroom, not the limit — what actually trims the list is CAT_FILTER_MAX.
#define MAX_FILTER    48
// 13 e nao 12: sao 12 CARTAZES mais a coluna do card "Ver tudo", que ocupa a
// posicao seguinte a ultima arte. Com 12 aqui, animFoco[r][12] escrevia fora do
// vetor — o card nunca acendia ao receber foco e a memoria do vizinho era
// corrompida em silencio.
#define MAX_CARDS 33

typedef struct {
  char title[96];
  KindRow kind;
  int n;
  // Primeiro item DESTA fileira no catalogo. Antes o desenho fazia `r * 8 + c`,
  // ou seja, cada fileira era uma janela fixa de 8 no vetor plano — o que so
  // funcionava porque as fileiras eram quatro e cravadas. Com as fileiras
  // vindo dos catalogos dos addons cada uma tem tamanho proprio.
  int start;
  // O card "Ver tudo" ocupa a coluna `n` (a seguinte a ultima arte). Guardado
  // por fileira porque so as que vieram de catalogo de addon o tem.
  int seeAll;
  char base[600], catId[96];
  // "movie" | "series" do CATALOGO. O `tipo` acima e a forma do card
  // (retrato/deitado), que e outra coisa — nao da para deduzir um do outro.
  char catKind[8];
  // Chave do catalogo desta fileira. Existe para o foco sobreviver a uma
  // republicacao: a descoberta agora publica a cada fileira que chega da rede,
  // e reencontrar por INDICE nao serve — uma fileira nova pode entrar no meio,
  // porque a ordem sai de art/fileiras.txt.
  char key[192];
  int folders[MAX_CARDS];
  int stackN;
} Row;

static char bd[MAX_ART][512];    int nBd = 0;    // backdrops 16:9
static char pst[MAX_ART][512];   int nPst = 0;   // posters 2:3

// RESERVA, e so isso: e o que a home mostra enquanto a rede nao respondeu, ou
// quando nao respondeu nenhuma. As fileiras de verdade vem de cat_fileira(),
// montadas em descoberta.c a partir dos catalogos que os addons declaram.
static Row rows[MAX_FILTER] = {
  { "Continue watching", ROW_CONTINUE, 8, 0  },
  { "Popular - Film",      ROW_NORMAL,   8, 8  },
  { "Popular - Series",  ROW_NORMAL, 8, 16 },
  { "Trending",         ROW_NORMAL,   8, 24 },
};
static int nRows = 4;
static int resumeIndex = -1;
static char resumeId[64];
static unsigned resumeRev, resumeApplied;
static int requestSocial;
static int requestPersonSocial;
static CatItem personSocial;
int home_requested_person_social(CatItem *output) {
  if (!requestPersonSocial) return 0;
  requestPersonSocial=0; if(output)*output=personSocial; return 1;
}
int home_requested_social(void) { int v=requestSocial;requestSocial=0;return v; }

// Classifica somente os nomes públicos do catálogo. Nunca inspeciona a URL
// (que pode conter tokens) nem inventa premiações ou disponibilidade.
static int containsName(const char *name, const char *term) {
  size_t n = strlen(term);
  for (; name && *name; name++) {
    size_t i;
    for (i = 0; i < n && name[i] &&
         tolower((unsigned char)name[i]) == (unsigned char)term[i]; i++) {}
    if (i == n) return 1;
  }
  return 0;
}
static KindRow profileCatalog(const char *name) {
  static const char *awards[] = {"oscar", "academy", "award", "premia", "cannes", "golden globe"};
  static const char *services[] = {"netflix", "disney", "prime video", "amazon", "apple tv", "hbo", "max -", "paramount", "globoplay", "mubi", "crunchyroll"};
  for (size_t i = 0; i < sizeof awards / sizeof awards[0]; i++)
    if (containsName(name, awards[i])) return ROW_COLLECTION;
  for (size_t i = 0; i < sizeof services / sizeof services[0]; i++)
    if (containsName(name, services[i])) return ROW_SERVICE;
  return ROW_NORMAL;
}
static int editorial(KindRow t) {
  return t == ROW_HIGHLIGHT || t == ROW_COLLECTION || t == ROW_SERVICE
      || t == ROW_SOCIAL;
}


static Focus focus;
static HomeItem itemFocus;      // preenchido durante o desenho, lido pela transicao
static int  hasItemFocus = 0;
static float animFocus[MAX_FILTER][MAX_CARDS];
static float scrollX[MAX_FILTER];
static float scrollY = 0.0f;
// Velocidades das molas de 2a ordem do deslize. Ficam ao lado da posicao
// porque anim_mola2() precisa das duas. Ver anim.h.
static float velX[MAX_FILTER];
static float velY = 0.0f;
static int sair = 0, requestOpen = 0, requestMenu = 0;
static Uint32 okSince = 0;
static int okPressing = 0;
static int okLongFired = 0;
static int okConsumeRelease = 0;
static float okHold = 0.0f;

// --- hero-carrossel ---
static int heroCurrent = 0, heroPrevious = 0;
// Candidato a heroi e desde quando ele e o candidato. Ver NV_HERO_REPOUSO_MS.
static int    heroPending = 0;
static Uint32 heroPendingIn = 0;
// ARTE JA ESCOLHIDA MAS AINDA NAO NO AR.
//
// O repouso de NV_HERO_REPOUSO_MS decide QUANDO trocar; isto decide SE ja da
// para trocar. Antes, no instante do repouso a arte velha comecava a apagar e a
// nova so aparecia quando a textura ficasse pronta — no meio ficava VAZIO, e
// andando depressa pela fileira o dono via o fundo piscar entre uma arte e
// outra. Agora a velha fica NO LUGAR ate a nova estar decodificada; so entao a
// troca comeca. Andar rapido deixa de mexer no fundo.
static int    heroWanted = -1;

// --- EXPANSAO DO CARTAZ FOCADO EM REPOUSO ------------------------------------
//
// `focusedPosterBackdropExpandEnabled` e `...DelaySeconds` ja existiam em
// art/ajustes.txt e em ajustes.c, mas NADA no desenho os lia — o ajuste estava
// na tela de Ajustes sem efeito nenhum. E o comportamento que o dono chama de
// "o card crescer quando ta parado": o foco pousa num cartaz, e depois de
// alguns segundos ele se abre na arte DEITADA, empurrando os vizinhos.
//
// Guardado por fileira/coluna e nao so o alvo, porque mover o foco tem de
// FECHAR o que estava aberto no mesmo quadro em que abre o relogio do novo.
static int    expRow = -1, expColumn = -1;
static Uint32 expSince = 0;
static float  expOpen = 0.0f;   // 0 fechado, 1 aberto

// MEDIDO na TCL (1920x1080, com.nuvio.tv), capturando 0,8 s e 7,8 s apos a
// tecla: o card focado vai de 214x320 para 565x320. A ALTURA NAO MUDA e a
// borda ESQUERDA fica parada em x=102 — ele cresce so para a direita e empurra
// os vizinhos. 565/320 = 1,77, ou seja 16:9 na mesma altura.
#define NV_EXP_ASPECT (16.0f / 9.0f)

// Fileira que pode expandir: so a de cartaz EM PE. A de continuar assistindo e
// a de posteres deitados ja mostram a arte larga — nao ha para o que abrir.
static int canExpand(int r) {
  if (r < 0 || r >= nRows) return 0;
  if (rows[r].kind != ROW_NORMAL) return 0;
  return !settings_posters_landscape();
}
static Uint32 heroSwapIn = 0;
// APAGAR -> VAZIO -> CORTE SECO. Ver a medida em NV_HERO_FADE_MS.
// `heroSai`   alfa da arte que esta SAINDO: 1 no instante da troca, 0 no fim.
// `heroEntra` 0 enquanto a arte nova esta escondida; vira 1 de uma vez, no
//             quadro em que a textura fica pronta E o esvanecimento acabou.
static float heroSai   = 0.0f;
static float heroEnters = 1.0f;

static void loadsDir(const char *dir, char destination[][512], int *n, const char *sub) {
  char path[512];
  if (sub) snprintf(path, sizeof path, "%s/%s", dir, sub);
  else snprintf(path, sizeof path, "%s", dir);
  DIR *d = opendir(path);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d)) && *n < MAX_ART) {
    if (!strstr(e->d_name, ".jpg") && !strstr(e->d_name, ".png")) continue;
    snprintf(destination[*n], 512, "%s/%s", path, e->d_name);
    (*n)++;
  }
  closedir(d);
}

// A forma do card sai de DUAS preferencias, e nao do tipo da fileira:
//
//   `modernLandscapePostersEnabled` troca o poster 2:3 (212x322) pelo card
//   deitado 16:9 (318x182.9). MEDIDO no app web com a preferencia ligada.
//
//   `continueWatchingCardStyle` decide a fileira de "Continuar assistindo":
//   "card" e "largo" desenham deitado, "poster" usa o mesmo 2:3 das outras.
static float widthOf(KindRow t) {
  switch (t) {
    case ROW_CONTINUE: return NV_HIGHLIGHT_W;
    case ROW_HIGHLIGHT: return 568.0f;
    case ROW_COLLECTION: return 480.0f;
    case ROW_SERVICE: return 360.0f;
    case ROW_SOCIAL: return 540.0f;
    case ROW_TOP10: return 212.0f;
    case ROW_RETURN: return 680.0f;
    case ROW_CATALOGS: return 360.0f;
    default:               return settings_posters_landscape() ? NV_CARD_LAND_W
                                                              : NV_CARD_W;
  }
}
// Quantos titulos o hero percorre. Vem do catalogo quando existe.
static int nArchiveHero(void) { int n = cat_n(); if (n) return n; return nBd ? nBd : 1; }

// Arte de um titulo nunca pode ser preenchida por uma posição equivalente de
// outro vetor. O catalogo chega em lotes, e a ordem dos backdrops do pacote não
// tem relação estável com a ordem dos itens da rede. Retornar somente arte que
// pertence ao próprio item deixa o estado sem arte explícito, em vez de trocar
// identidade silenciosamente.
static const char *artOfItem(const CatItem *item, int *isPoster) {
  if (isPoster) *isPoster = 0;
  if (!item) return NULL;
  if (item->backdrop[0]) return item->backdrop;
  if (item->poster[0]) {
    if (isPoster) *isPoster = 1;
    return item->poster;
  }
  return NULL;
}

// Um poster é uma boa reserva editorial, mas não deve ser cover-stretched num
// hero 16:9, pois isso corta justamente o rosto e o título. Ele fica contido no
// lado direito, com a mesma vinheta do hero, e o restante da composição segue
// disponível para a cópia do título.
static int drawArtHero(GfxRect r, GfxMode mode, const CatItem *item,
                           const char *path, float alpha) {
  int isPoster = 0;
  const char *art = item ? artOfItem(item, &isPoster) : path;
  GLuint tex;
  if (!art || !art[0]) return 0;
  tex = tex_get_hero(art);
  if (!tex) return 0;
  gfx_tex_aspect_current = tex_aspect(art);
  if (!isPoster) {
    gfx_rect(r, tex, mode, 0, 0, 0, 0, 0, 0, 0, alpha);
  } else {
    float ap = gfx_tex_aspect_current > 0.05f ? gfx_tex_aspect_current : (2.0f / 3.0f);
    float h = r.h, w = h * ap, limit = r.w * 0.42f;
    if (w > limit) { w = limit; h = w / ap; }
    GfxRect poster = { r.x + r.w - w, r.y + (r.h - h) * 0.5f, w, h };
    gfx_rect(poster, tex, GFX_HERO, 0, 0, 0, 0, 0, 0, 0, alpha);
  }
  gfx_tex_aspect_current = 0.0f;
  return 1;
}

static void drawPlaceholderHero(GfxRect r, const CatItem *item, float alpha) {
  GfxRect block = { r.x + r.w * 0.58f, r.y + 32.0f,
                    r.w * 0.34f, r.h - 64.0f };
  gfx_color(block, 0.035f, 0.075f, 0.082f, 0.098f, alpha * 0.92f);
  { TxtLine t = txt_line(TXT_HERO_META, "Art unavailable",
                            185, 191, 204, 255);
    txt_draw_alpha(t, block.x + 28.0f,
                       block.y + block.h * 0.5f - t.h * 0.5f,
                       alpha); }
  if (item && item->title[0]) {
    TxtLine t = txt_line_trim(TXT_CAPTION, item->title,
                                 211, 216, 226, 255, block.w - 56.0f);
    txt_draw_alpha(t, block.x + 28.0f,
                       block.y + block.h * 0.5f + 20.0f, alpha * 0.76f);
  }
}

static void drawArtMissing(GfxRect r, float radius, const CatItem *item,
                               float alpha) {
  gfx_color(r, radius, NV_COLOR_SKELETON_R, NV_COLOR_SKELETON_G,
          NV_COLOR_SKELETON_B, alpha);
  TxtLine state = txt_line_trim(TXT_CAPTION, "Art unavailable",
                                    184, 188, 198, 255, r.w - 32.0f);
  float center = r.y + r.h * 0.5f;
  txt_draw_alpha(state, r.x + (r.w - state.w) * 0.5f,
                     center - state.h * 0.5f - (item && item->title[0] ? 8.0f : 0.0f),
                     alpha * 0.9f);
  if (item && item->title[0]) {
    TxtLine name = txt_line_trim(TXT_MINI, item->title,
                                    160, 165, 178, 255, r.w - 32.0f);
    txt_draw_alpha(name, r.x + (r.w - name.w) * 0.5f,
                       center + 12.0f, alpha * 0.78f);
  }
}

// Escolha de formato para cards. O helper arteDoItem acima informa se precisou
// usar poster como fallback; aqui a ordem visual do card continua explicita.
static const char *art_by_format(const CatItem *item, int landscape) {
  if (!item) return NULL;
  if (landscape) return item->backdrop[0] ? item->backdrop
                                      : (item->poster[0] ? item->poster : NULL);
  return item->poster[0] ? item->poster
                         : (item->backdrop[0] ? item->backdrop : NULL);
}

// `cat_item()` faz wrap para telas que percorrem listas circulares. A home nao
// pode usar esse contrato para resolver arte: indice stale vira ausencia, nunca
// outro titulo.
static const CatItem *cat_item_exact(int i) {
  int n = cat_n();
  if (i < 0 || i >= n) return NULL;
  return cat_item(i);
}

// A pasta art/ e acervo de reserva apenas no modo sem catalogo. Quando a rede
// publicou itens, nenhum arquivo generico pode ocupar o lugar de outro titulo.
static const char *art_by_identity(int index_, int landscape) {
  const CatItem *item = cat_item_exact(index_);
  const char *art = art_by_format(item, landscape);
  if (art) return art;
  if (cat_n() == 0 && index_ >= 0) {
    if (!landscape && index_ < nPst) return pst[index_];
    if (index_ < nBd) return bd[index_];
  }
  return NULL;
}

static int focus_can_press_longa(void) {
  if (focus.row < 0 || focus.row >= nRows) return 0;
  const Row *s = &rows[focus.row];
  if (s->kind == ROW_CATALOGS || s->kind == ROW_SOCIAL ||
      s->kind == ROW_TOP10) return 0;
  if (s->seeAll && focus.column == s->n) return 0;
  return focus.column >= 0 && focus.column < s->n;
}


static float heightOf(KindRow t) {
  switch (t) {
    case ROW_CONTINUE: return NV_HIGHLIGHT_H;
    case ROW_HIGHLIGHT: return 320.0f;
    case ROW_COLLECTION: return 270.0f;
    case ROW_SERVICE: return 203.0f;
    case ROW_SOCIAL: return 240.0f;
    case ROW_RETURN: return 178.0f;
    case ROW_TOP10: return 320.0f;
    case ROW_CATALOGS: return 203.0f;
    default:               return settings_posters_landscape() ? NV_CARD_LAND_H
                                                              : NV_CARD_H;
  }
}
// Altura TOTAL que a fileira ocupa: a arte mais o bloco de rotulo, quando ele
// existe. Sem somar o rotulo aqui, a fileira seguinte sobe por cima do texto —
// foi o mesmo defeito que o titulo de fileira ja tinha tido sobre os cards.
static int hasLabel(KindRow t) {
  // O rotulo abaixo do poster so existe no poster EM PE. No card deitado o web
  // poe a legenda DENTRO da moldura (.home-poster-landscape-copy) e esconde o
  // bloco de fora (.home-poster-card.is-landscape .home-poster-copy{display:none}).
  return t == ROW_NORMAL && settings_labels_poster()
      && !settings_posters_landscape();
}
static float heightTotalOf(KindRow t) {
  return heightOf(t) + (hasLabel(t) ? NV_POSTER_COPY_H : 0.0f);
}
// O gap do tvOS e fixo em 40px e ja foi dimensionado para caber o crescimento
// do foco: um card de 410 crescendo 9% invade 18px de cada lado. O card
// destaque e maior que qualquer coisa que a Apple usa, entao para ele o gap
// segue proporcional — senao a invasao (31px) come quase todo o respiro.
static float gapOf(KindRow t) {
  (void)t;
  return NV_CARD_GAP;
}
// Passo vertical entre fileiras. `.home-modern-landscape-posters` aperta o
// `--home-row-gap` de 32 para 24 (components.css:6473) — a fileira deitada e
// mais baixa e o respiro do poster em pe sobraria nela.
static float rowGap(void) {
  return settings_posters_landscape() ? NV_ROW_GAP_LAND : NV_ROW_GAP;
}
// Raio do card, em fracao do menor lado (o SDF do shader e normalizado). Este e
// o UNICO numero do card moderno que sai mesmo de `posterCardCornerRadiusDp`:
// 12dp x 2 = 24px, conferido no app rodando. A largura NAO sai de la (ver a
// nota em ajustes.h).
static float radiusOf(float w, float h) {
  float smaller = w < h ? w : h;
  if (smaller <= 0.0f) return NV_RADIUS_CARD;
  return settings_radius_poster_px() / smaller;
}

// --- Profundidade dos cartoes (`cardDepth*`) ---------------------------------
// O web faz isto com dois pseudo-elementos sobre a arte: um brilho na borda de
// CIMA com opacidade `--card-depth-edge` e uma faixa clara e discreta —
// `--card-depth-sheen` — atravessando a parte alta do cartao. `--card-depth-
// coverage` engorda a banda da borda: `12 + round(18 * coverage)` px
// (layoutPreferences.js:181). Sao os mesmos tres numeros da tela de Ajustes.
static void drawDepth(GfxRect card, float radius, int onHere) {
  if (!settings_depth() || !onHere) return;
  float border = settings_depth_border();
  float brightness = settings_depth_brightness();
  float coverage = settings_depth_coverage();
  if (border > 0.001f) {
    float h = 12.0f + 18.0f * coverage;
    GfxRect track = { card.x, card.y, card.w, h };
    // Raio proporcional: a faixa e muito mais baixa que o card, entao repetir a
    // fracao do card arredondaria demais e a borda descolaria do canto.
    gfx_color(track, radius * (card.h / (h > 0.0f ? h : 1.0f)) * 0.5f,
            1.0f, 1.0f, 1.0f, border * 0.55f);
  }
  if (brightness > 0.001f) {
    GfxRect refl = { card.x, card.y + card.h * 0.06f, card.w, card.h * 0.28f };
    gfx_color(refl, radius, 1.0f, 1.0f, 1.0f, brightness * 0.18f);
  }
}
// ZERO. MEDIDO no app web (sessao logada, perfil do dono): o card em foco tem
// `transform: none`, `scale: none` e o mesmo getBoundingClientRect do card ao
// lado — 212x322 nos dois, mesma linha, mesmo topo. A escala de 9% e o
// levantamento de 8px vinham das tabelas de Top Shelf do tvOS, e nao desta
// interface. Eram eles que faziam o card focado subir 22px e encostar no titulo
// da fileira, que fica 15px acima dos cards (titulo 518..549, cards em 564).
//
// O foco no web se marca por um ANEL de 2px `#f5f5f5` desenhado por dentro e
// por fora da arte (box-shadow inset + outset), com o card mantendo a caixa.
static float scaleOf(KindRow t) {
  (void)t;
  return 0.0f;
}
static float stepOf(KindRow t) {
  return widthOf(t) + gapOf(t);
}

int home_start(const char *dirArt) {
  extras_load(dirArt);
  col_load(dirArt);
  badges_load(dirArt);
  cat_load(dirArt);
  // O cache da ULTIMA sessao entra por cima do catalogo do pacote, antes de
  // qualquer rede. Se nao existir (primeira execucao) ou for de outra build,
  // segue-se com o do pacote, como sempre foi.
  if (cat_read_cache(dirArt)) mark("cached catalog on screen");
  loadsDir(dirArt, bd, &nBd, NULL);
  loadsDir(dirArt, pst, &nPst, "poster");
  if (!nBd) { printf("home: no backdrop in %s\n", dirArt); return 0; }
  if (!nPst) { printf("home: no portrait posters, Top 10 will use the backdrop\n"); }

  // No layout moderno legacy o hero é informativo; a navegação começa na
  // primeira fileira de conteúdo (como buildModernNavigationRows()).
  int cols[MAX_FILTER];
  for (int i = 0; i < nRows; i++)
    cols[i] = rows[i].n + (rows[i].seeAll ? 1 : 0);
  focus_start(&focus, nRows, cols);
  heroSwapIn = SDL_GetTicks() + NV_HERO_INTERVAL_MS;
  printf("home: %d backdrops, %d posters, %d rows\n", nBd, nPst, nRows);
  return 1;
}

void home_event(const SDL_Event *e) {
  if (e->type == SDL_QUIT) { sair = 1; return; }

  // SEGURAR O OK ABRE O MENU DO CARTAZ.
  //
  // O tempo so e conhecido quando a tecla SOBE, entao o KEYUP tem de ser visto
  // — e ele era descartado logo abaixo, junto com todo evento que nao fosse
  // KEYDOWN. A tela de titulo ja usa esta mesma medida (NV_HOLD_MS) para
  // separar "Reproduzir" de "escolher fonte".
  { SDL_Keycode kk = e->key.keysym.sym;
    int isOk = (kk == SDLK_RETURN || kk == SDLK_KP_ENTER || kk == SDLK_SPACE);
    if (e->type == SDL_KEYDOWN && isOk) {
      if (!okPressing) {
        okPressing = 1;
        okLongFired = 0;
        okConsumeRelease = 0;
        okSince = SDL_GetTicks();
      }
      return;
    } else if (e->type == SDL_KEYUP && isOk) {
      if (okConsumeRelease) {
        okConsumeRelease = 0;
        okSince = 0;
        okPressing = 0;
        okHold = 0.0f;
        return;
      }
      Uint32 duration = okSince ? SDL_GetTicks() - okSince : 0;
      int onSeeAll = (focus.row >= 0 && focus.row < nRows &&
                       rows[focus.row].seeAll &&
                       focus.column == rows[focus.row].n);
      okSince = 0;
      okPressing = 0;
      okLongFired = 0;
      okHold = 0.0f;
      if (focus.row < 0 || focus.row >= nRows) return;
      if(rows[focus.row].kind==ROW_TOP10 && rows[focus.row].stackN) {
        Row *s=&rows[focus.row];
        s->n=s->stackN<10?s->stackN:10;
        s->stackN=0;s->seeAll=1;
        focus.column=0;focus.columnRemembered[focus.row]=0;
        focus.nColumns[focus.row]=s->n+1;
        return;
      }
      if(rows[focus.row].kind==ROW_SOCIAL && rows[focus.row].start<0) {
        requestSocial=1;return;
      }
      if(rows[focus.row].kind==ROW_SOCIAL) {
        const CatItem *ci=cat_item_exact(rows[focus.row].start+focus.column);
        if(ci){personSocial=*ci;requestPersonSocial=1;}return;
      }
      if (rows[focus.row].kind == ROW_CATALOGS) {
        if (focus.column >= 0 && focus.column < rows[focus.row].n) {
          seeall_collection(col_folder(rows[focus.row].folders[focus.column]));
        }
      } else if (onSeeAll) {
        seeall_open(rows[focus.row].base, rows[focus.row].catKind,
                      rows[focus.row].catId, rows[focus.row].title);
      } else if (duration >= NV_HOLD_MS) {
        ctx_open(rows[focus.row].start + focus.column);
      } else {
        requestOpen = 1;
      }
      return;
    } }

  if (e->type != SDL_KEYDOWN) return;
  SDL_Keycode k = e->key.keysym.sym;
#ifdef __APPLE__
  // Rodando no Mac, o Back na home NAO fecha: fechar a janela no meio de um
  // teste custa recompilar e reabrir. No aparelho ele sai do app, como deve.
  if (k == SDLK_ESCAPE || k == SDLK_AC_BACK || k == SDLK_BACKSPACE) return;
  if (k == SDLK_q) { sair = 1; return; }
#else
  if (k == SDLK_ESCAPE || k == SDLK_AC_BACK) { sair = 1; return; }
#endif
  // O OK NAO AGE MAIS NO KEYDOWN. Abrir o titulo ali tornava o "segurar"
  // impossivel: quando a tecla subia, o detalhe ja estava aberto ha meio
  // segundo. Toda a decisao — abrir, "Ver tudo" ou menu do cartaz — mora no
  // KEYUP acima, que e o unico ponto que conhece a DURACAO.
  if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) return;
  if (k == SDLK_RIGHT) {
    // O `&&` aqui era um curto-circuito com efeito colateral: escrito como
    // `if (fileira == 0 && !focus_mover(...))`, o focus_mover so era chamado
    // NO HERO — em qualquer outra fileira a seta direita nao movia nada. Mover
    // primeiro, decidir depois.
    (void)focus_mover(&focus, 1, 0);
  } else if (k == SDLK_LEFT) {
  // Esquerda na primeira coluna chama o menu lateral, em QUALQUER fileira —
    // inclusive no hero. Antes o hero era excecao e usava a esquerda para
    // voltar um titulo no carrossel: quem chegava ali (voltando de outra tela,
    // por exemplo) nao tinha como abrir o menu sem antes descer. O carrossel
    // continua acessivel pela direita e pela troca automatica.
    if (focus.column == 0) { requestMenu = 1; return; }
    focus_mover(&focus, -1, 0);
  }
  else if (k == SDLK_DOWN)  focus_mover(&focus, 0, 1);
  else if (k == SDLK_UP)    focus_mover(&focus, 0, -1);
}

// Reconstroi a lista a partir do catalogo. Chamada a cada quadro porque a
// descoberta roda noutro fio e pode trocar o catalogo a qualquer momento; sai
// cedo quando nada mudou, entao custa uma comparacao de inteiro.
static int filtersApplied = -1;
// Assinatura das preferencias que MUDAM a lista de fileiras. Sem isto, desligar
// "Continuar assistindo" em Ajustes so valia depois que a rede trocasse o
// catalogo — a comparacao de `nCat` saia cedo e a fileira continuava na tela.
static int prefsApplied = -1;
static int subscriptionPrefs(void) {
  return (settings_cw_on() ? 1 : 0)
       | (settings_cw_style() << 1)
       | (settings_posters_landscape() ? 8 : 0)
       | (settings_labels_poster() ? 16 : 0);
}
// THE KNOWN CURATION. It no longer decides the ORDER — the account's list does —
// and is left with two jobs: supplying the display name and the special kind of
// certain rows, and acting as a fallback order when the account has no order at
// all (a fresh account, or a server without the RPC).
//
// The "@Name" entries are packaged collection GROUPS, matched by title. The
// account's own collections do not come through here: they arrive by id, in the
// position the person gave them.
static const char *const CURATED_ID[]={"continue_watching","social_activity","now_playing_movies","@Streaming",
  "trending_movies","trending_series","@Themes","ai_movies_for_you",
  "ai_series_for_you","snoak_top100_movies","snoak_top100_series",
  "@Awards","@Directors","@Genres"};
static const char *const CURATED_NAME[]={"Continue watching","Among friends","Recent Release","Streaming",
  "Trending Movies","Trending Series","Themes","Picked for You · Movies",
  "Picked for You · Series","Top 100 · Movies","Top 100 · Series",
  "Awards","Directors","Genres"};
#define CURATED_N (sizeof CURATED_ID / sizeof CURATED_ID[0])

// Applies the display name and special kind to a catalogue row, if the curation
// recognises it. It does not touch the position: that was already decided by the
// caller.
static void decorateRow(Row *row) {
  size_t s;
  if(!row)return;
  for(s=0;s<CURATED_N;s++) {
    if(CURATED_ID[s][0]=='@')continue;
    if(strcmp(row->catId,CURATED_ID[s])&&strcmp(row->key,CURATED_ID[s]))continue;
    snprintf(row->title,sizeof row->title,"%s",CURATED_NAME[s]);
    if(s==1)row->kind=ROW_SOCIAL;
    else if(s==2)row->kind=ROW_HIGHLIGHT;
    else if(s==9||s==10)row->kind=ROW_TOP10;
    else if(s!=0)row->kind=ROW_NORMAL;
    return;
  }
}

static void syncRows(void) {
  int nCat = cat_n_rows(), r, destination = 0;
  int sub = subscriptionPrefs();
  static unsigned ultimaRevision;
  unsigned revision = 2166136261u;
  for (r = 0; r < nCat; r++) {
    const CatRow *cf = cat_row(r);
    if (!cf) break;
    for (const unsigned char *s = (const unsigned char *)cf->key; *s; s++)
      revision = (revision ^ *s) * 16777619u;
    for (const unsigned char *s = (const unsigned char *)cf->title; *s; s++)
      revision = (revision ^ *s) * 16777619u;
    revision = (revision ^ (unsigned)cf->start) * 16777619u;
    revision = (revision ^ (unsigned)cf->n) * 16777619u;
  }
  // The COLLECTIONS join the sum. Without this, a collection arriving from the
  // account after the catalogue does not change the revision, the early return
  // just below skips the rebuild, and its row never comes to exist.
  revision = (revision ^ col_revision()) * 16777619u;
  // Guardado ANTES do laco abaixo, que sobrescreve fileiras[]: depois dele nao
  // ha mais como saber em que fileira o foco estava.
  char keyFocus[192];
  int colFocus = focus.column;
  keyFocus[0] = 0;
  if (focus.row >= 0 && focus.row < nRows)
    snprintf(keyFocus, sizeof keyFocus, "%s", rows[focus.row].key);
  // `nCat < 1` alone hid the COLLECTIONS: with no catalogue rows the function
  // returned right here, so a home that had only collections came out empty.
  if ((nCat < 1 && !col_n()) || (nCat == filtersApplied && sub == prefsApplied
      && revision == ultimaRevision && resumeApplied == resumeRev)) return;
  // Guardar o estado por chave: inserir o hub não deve transferir a rolagem
  // horizontal de uma fileira para outra.
  static Row old[MAX_FILTER];
  float oldX[MAX_FILTER];
  int nOld = nRows;
  memcpy(old, rows, sizeof old);
  memcpy(oldX, scrollX, sizeof oldX);
  int hasHighlight = 0;
  for (r = 0; r < nCat && destination < MAX_FILTER - 1; r++) {
    const CatRow *cf = cat_row(r);
    if (!cf) break;
    if (cf->n < 1) continue;
    // `continueWatchingEnabled: false` tira a fileira da home inteira — nao a
    // esvazia, tira. E o que renderModernHomeLayout faz quando
    // computeContinueWatchingRenderState devolve a fileira desligada.
    if (!strcmp(cf->key, "continue_watching") && !settings_cw_on()) continue;
    snprintf(rows[destination].title, sizeof rows[destination].title, "%s", cf->title);
    // "Continuar assistindo" e a unica landscape: e o
    // `continueWatchingCardStyle: "card"` do perfil. Todo o resto e poster 2:3.
    // `continueWatchingCardStyle`: "card" e "largo" desenham landscape, "poster"
    // usa o mesmo 2:3 das outras fileiras. E a preferencia, nao o tipo da
    // fileira, que decide a forma.
    rows[destination].kind = (!strcmp(cf->key, "continue_watching")
                              && settings_cw_style() != 2)
                           ? ROW_CONTINUE : profileCatalog(cf->title);
    if (!strcmp(cf->key, "continue_watching")) {
      if (settings_cw_style() == 2) rows[destination].kind = ROW_NORMAL;
    } else if (!hasHighlight && cf->base[0] && cf->catId[0]) {
      rows[destination].kind = ROW_HIGHLIGHT;
      hasHighlight = 1;
    }
    // MAX_CARDS - 1: a ultima coluna e do card "Ver tudo". Sem reservar, uma
    // fileira cheia empurraria o card para fora do vetor de animacao.
    rows[destination].n   = cf->n > 12 ? 12 : cf->n;
    // UMA COLUNA A MAIS: o card "Ver tudo" no fim. So em fileira que veio de um
    // CATALOGO de addon — "Continuar assistindo" e as listas do Trakt nao tem
    // continuacao para pedir (o base fica vazio nelas).
    rows[destination].seeAll = (cf->base[0] && cf->catId[0]) ? 1 : 0;
    snprintf(rows[destination].base,  sizeof rows[destination].base,  "%s", cf->base);
    snprintf(rows[destination].catId, sizeof rows[destination].catId, "%s", cf->catId);
    snprintf(rows[destination].catKind, sizeof rows[destination].catKind, "%s", cf->kind);
    rows[destination].start = cf->start;
    if(!strcmp(cf->key,"social_activity"))rows[destination].kind=ROW_SOCIAL;
    snprintf(rows[destination].key, sizeof rows[destination].key,
             "%s", cf->key);
    destination++;
  }
  if (col_n()) {
    static Row orig[MAX_FILTER];int total=destination;memcpy(orig,rows,sizeof orig);destination=0;
    // pinToTop: the collections the owner pinned go FIRST and are never cut. It
    // is step 5 of the web's algorithm, described in catalog.h and missing here —
    // and without it a collection fell to the end, behind 16 catalogue rows, i.e.
    // off the screen, which is the same as not existing.
    for(int i=0;i<col_n() && destination<MAX_FILTER;i++) {
      const ColFolder *folder=col_folder(i);
      char key[192];
      int already=0;
      if(!folder||!folder->group[0]||!col_group_pinned(folder->group))continue;
      snprintf(key,sizeof key,"collection_%s",folder->group);
      for(int j=0;j<destination;j++) if(!strcmp(rows[j].key,key)){already=1;break;}
      if(already)continue;
      { Row v={0};
        v.n=col_group(folder->group,v.folders,MAX_CARDS);
        if(!v.n)continue;
        v.kind=ROW_CATALOGS;
        snprintf(v.key,sizeof v.key,"%s",key);
        snprintf(v.title,sizeof v.title,"%s",folder->group);
        rows[destination++]=v; }
    }
    // THE ACCOUNT'S ORDER DECIDES. It is a single list interleaving catalogues
    // (`<addonId>_<type>_<catalogId>`) and collections (`collection_<id>`) — the
    // position the person chose in the web app. Collections could previously only
    // appear at the end, behind every catalogue, because nothing here could read
    // that list.
    //
    // The curation table just below no longer decides the ORDER and only
    // DECORATES: it supplies the name and special kind of the rows it recognises.
    // When there is no account order (a fresh account, or a server without the
    // RPC), this loop places nothing and the table becomes the only criterion
    // again, exactly as before.
    for(int i=0;i<disc_prefs_n() && destination<MAX_FILTER;i++) {
      const char *key=disc_prefs_key(i);
      const char *custom;
      int already=0;
      if(!key[0]||disc_prefs_hidden(key))continue;
      if(!strncmp(key,"collection_",11)) {
        // The account identifies a collection by ID; the row is grouped by TITLE.
        // col_group_by_id is the bridge between the two.
        const char *group=col_group_by_id(key+11);
        Row v={0};
        char rowKey[192];
        if(!group||!group[0]) {
          // The order names a collection that is not loaded: deleted on the web,
          // or still on its way. Saying which one avoids hunting for a fault in
          // the wrong place when an expected row fails to appear.
          static int said;
          if(said<4){printf("[home] order names %s, which matches no collection\n",key);said++;}
          continue;
        }
        snprintf(rowKey,sizeof rowKey,"collection_%s",group);
        for(int j=0;j<destination;j++) if(!strcmp(rows[j].key,rowKey)){already=1;break;}
        if(already)continue;             // already placed by the pinToTop pass
        v.n=col_group(group,v.folders,MAX_CARDS);
        if(!v.n)continue;
        v.kind=ROW_CATALOGS;
        snprintf(v.key,sizeof v.key,"%s",rowKey);
        snprintf(v.title,sizeof v.title,"%s",group);
        custom=disc_prefs_title(key);
        if(custom&&*custom)snprintf(v.title,sizeof v.title,"%s",custom);
        rows[destination++]=v;
        continue;
      }
      for(int j=0;j<destination;j++) if(!strcmp(rows[j].key,key)){already=1;break;}
      if(already)continue;
      for(int k=0;k<total;k++) {
        if(strcmp(orig[k].key,key))continue;
        rows[destination]=orig[k];
        decorateRow(&rows[destination]);
        destination++;
        break;
      }
    }
    const char *const *ids=CURATED_ID, *const *names=CURATED_NAME;
    // The known curation takes priority over whatever the account did NOT order,
    // but it is not a cut-off list. The web keeps new keys at the end and the
    // native home has to do the same: catalogues and groups that were not in this
    // table stay reachable.
    for(size_t s=0;s<CURATED_N && destination<MAX_FILTER;s++) {
      if(ids[s][0]=='@') {
        Row v={0};int already=0;
        v.n=col_group(ids[s]+1,v.folders,MAX_CARDS);
        if(!v.n)continue;
        v.kind=ROW_CATALOGS;
        snprintf(v.key,sizeof v.key,"collection_%s",ids[s]+1);
        // May already have been added by the pinToTop pass above.
        for(int j=0;j<destination;j++) if(!strcmp(rows[j].key,v.key)){already=1;break;}
        if(already)continue;
        snprintf(v.title,sizeof v.title,"%s",names[s]);rows[destination++]=v;
      } else for(int k=0;k<total;k++) {
        int dup=0;
        if(strcmp(orig[k].catId,ids[s])&&strcmp(orig[k].key,ids[s]))continue;
        // May already have been placed by the account's order, just above.
        for(int j=0;j<destination;j++) if(!strcmp(rows[j].key,orig[k].key)){dup=1;break;}
        if(dup)break;
        rows[destination]=orig[k];
        snprintf(rows[destination].title,sizeof rows[destination].title,"%s",names[s]);
        if(s==1)rows[destination].kind=ROW_SOCIAL;
        else if(s==2)rows[destination].kind=ROW_HIGHLIGHT;
        else if(s==9||s==10)rows[destination].kind=ROW_TOP10;
        else if(s!=0)rows[destination].kind=ROW_NORMAL;
        destination++;break;
      }
    }
    // Tudo que nao casou com a curadoria acima permanece na ordem declarada
    // pelo addon/preferencia. A comparacao por chave evita duplicar uma linha
    // especial que ja foi promovida.
    for(int k=0;k<total && destination<MAX_FILTER;k++) {
      int watched=0;
      for(int j=0;j<destination;j++) if(!strcmp(rows[j].key,orig[k].key)){watched=1;break;}
      if(!watched) rows[destination++]=orig[k];
    }
    // Grupos adicionais tambem sao configuracao do usuario. Nao dependem de
    // nomes que conheciamos quando a tabela foi escrita, e cada grupo aparece
    // uma vez com todos os seus folders.
    for(int i=0;i<col_n() && destination<MAX_FILTER;i++) {
      const ColFolder *folder=col_folder(i);
      int groupWatched=0;
      if(!folder||!folder->group[0])continue;
      for(int j=0;j<destination;j++) {
        char key[192];snprintf(key,sizeof key,"collection_%s",folder->group);
        if(!strcmp(rows[j].key,key)){groupWatched=1;break;}
      }
      if(groupWatched)continue;
      { Row v={0};
        v.n=col_group(folder->group,v.folders,MAX_CARDS);
        if(!v.n)continue;
        v.kind=ROW_CATALOGS;
        snprintf(v.key,sizeof v.key,"collection_%s",folder->group);
        snprintf(v.title,sizeof v.title,"%s",folder->group);
        rows[destination++]=v;
      }
    }
  }
  for(int i=0;i<destination;i++) {
    Row *s=&rows[i];s->stackN=0;
    if(s->kind==ROW_TOP10 && s->base[0] && s->catId[0]) {
      s->stackN=s->n;s->n=1;s->seeAll=0;
    }
  }
  int socialExists=0;
  for(int i=0;i<destination;i++)if(rows[i].kind==ROW_SOCIAL)socialExists=1;
  if(!socialExists && destination<MAX_FILTER) {
    int pos=destination>0?1:0;
    memmove(rows+pos+1,rows+pos,(destination-pos)*sizeof *rows);
    Row *s=&rows[pos];memset(s,0,sizeof *s);
    s->kind=ROW_SOCIAL;s->start=-1;s->n=1;
    snprintf(s->title,sizeof s->title,"Among friends");
    snprintf(s->key,sizeof s->key,"social_activity");destination++;
  }
  // Um retorno do player e contexto, nao catalogo: entra acima das fileiras e
  // desaparece quando nao existe sessao incompleta. Nao duplica dados nem faz
  // rede; aponta para o item que o player acabou de atualizar em memoria.
  if (resumeId[0]) resumeIndex = cat_index_by_imdb(resumeId);
  if (resumeIndex >= 0 && destination < MAX_FILTER) {
    memmove(rows + 1, rows, sizeof(Row) * (size_t)destination);
    memset(&rows[0], 0, sizeof rows[0]);
    snprintf(rows[0].title, sizeof rows[0].title, "Resume now");
    snprintf(rows[0].key, sizeof rows[0].key, "last_session");
    rows[0].kind = ROW_RETURN;
    rows[0].start = resumeIndex; rows[0].n = 1;
    destination++;
  }
  nRows = destination;
  // THE HOME'S FINAL ORDER, once per rebuild.
  //
  // The "[disc] row" lines only tell half the story: those are the catalogues, and
  // the interleaving with collections happens here. Without this list there is no
  // way to check the order without someone watching the TV — and the order is
  // precisely what the person configured and expects to recognise.
  //
  // Only when it CHANGES. The catalogue is published row by row, so this function
  // runs ~22 times per launch; printing the whole list each time would drown the
  // log in exactly the section it exists to make readable.
  { static unsigned printed;
    unsigned now = 2166136261u;
    int i;
    for (i = 0; i < nRows; i++)
      for (const unsigned char *s = (const unsigned char *)rows[i].key; *s; s++)
        now = (now ^ *s) * 16777619u;
    if (now != printed) {
      printed = now;
      printf("[home] %d row(s):\n", nRows);
      for (i = 0; i < nRows; i++)
        printf("[home]  %2d %-10s %s\n", i,
               rows[i].kind == ROW_CATALOGS ? "COLLECTION" : "catalogue",
               rows[i].title);
      fflush(stdout);
    } }
  resumeApplied = resumeRev;
  ultimaRevision = revision;
  filtersApplied = nCat;
  prefsApplied = sub;
  memset(animFocus, 0, sizeof animFocus);
  memset(velX, 0, sizeof velX);
  memset(scrollX, 0, sizeof scrollX);
  for (r = 0; r < nRows; r++)
    for (int a = 0; a < nOld; a++)
      if (!strcmp(rows[r].key, old[a].key)) {
        if(rows[r].kind==ROW_TOP10 && old[a].kind==ROW_TOP10 &&
           !old[a].stackN && old[a].seeAll && rows[r].stackN) {
          rows[r].n=rows[r].stackN<10?rows[r].stackN:10;
          rows[r].stackN=0;rows[r].seeAll=1;
        }
        scrollX[r] = oldX[a]; break;
      }
  expRow = expColumn = -1; expOpen = 0.0f;
  if (nRows < 1) return;
  {
    int cols[MAX_FILTER], k;
    // PRESERVAR O FOCO. focus_iniciar faz memset e zera fileira e coluna, e a
    // descoberta agora publica o catalogo A CADA FILEIRA que chega da rede —
    // sao ~16 publicacoes nos primeiros segundos. Com o reset, o foco do dono
    // saltaria para o primeiro card umas dezesseis vezes enquanto ele tenta
    // navegar. Antes isso nao aparecia porque a publicacao era unica.
    //
    // A fileira e reencontrada pela CHAVE do catalogo, nao pelo indice: uma
    // fileira nova pode entrar no meio (a ordem vem de art/fileiras.txt), e o
    // indice antigo passaria a apontar para outra coisa.
    int found = -1;
    for (k = 0; k < nRows; k++)
      cols[k] = rows[k].n + (rows[k].seeAll ? 1 : 0);
    focus_start(&focus, nRows, cols);
    if (keyFocus[0])
      for (k = 0; k < nRows; k++)
        if (!strcmp(rows[k].key, keyFocus)) { found = k; break; }
    if (found >= 0) {
      focus.row = found;
      // A fileira pode ter encolhido entre uma publicacao e outra.
      focus.column = colFocus < focus.nColumns[found] ? colFocus
                  : (focus.nColumns[found] > 0 ? focus.nColumns[found] - 1 : 0);
    }
  }
  printf("[home] %d rows from the catalog\n", nRows);
}

void home_update(float dt, Uint32 now) {
  syncRows();

  const int motionReduced = settings_animations_reduced();
  if (okPressing && focus_can_press_longa())
    okHold = anim_clamp((now - okSince) / NV_HOLD_FEEDBACK_MS, 0.0f, 1.0f);
  else if (!okPressing)
    okHold = 0.0f;
  if (okPressing && okHold >= 1.0f && !okLongFired) {
    okLongFired = 1;
    okConsumeRelease = 1;
    okPressing = 0;
    okSince = 0;
    // O menu contextual continua sendo o dono das acoes e da UI. A home so
    // dispara uma vez no limiar e consome o KEYUP seguinte.
    ctx_open(rows[focus.row].start + focus.column);
  }

  // O catalogo pode encolher entre duas respostas. Normalizar os indices do
  // carrossel evita que um estado stale caia no wrap de cat_item().
  { int total = nArchiveHero();
    if (heroCurrent < 0 || heroCurrent >= total) heroCurrent = 0;
    if (heroPrevious < 0 || heroPrevious >= total) heroPrevious = heroCurrent;
    if (heroPending < 0 || heroPending >= total) heroPending = heroCurrent;
  }

  // O HERO SEGUE O FOCO. Pedido do dono, e e uma DIVERGENCIA DELIBERADA do app
  // web: medi duas vezes com o foco andando de verdade (o card mudou de "54
  // minutos restantes" para "1h 12m restantes") e a arte do
  // `.home-hero-backdrop` continuou a mesma — no web ela nao acompanha a
  // selecao. Fica registrado para ninguem "corrigir" isto de volta achando que
  // e desvio: e melhoria escolhida, nao erro.
  //
  // Enquanto ha foco num card, o carrossel automatico nao roda: duas fontes
  // mexendo na mesma arte dariam trocas em cima da escolha do usuario.
  {
    int target = -1;
    if (focus.row >= 0 && focus.row < nRows) {
      int i = rows[focus.row].start + focus.column;
      if (rows[focus.row].kind == ROW_CATALOGS)
        i = -1;
      else if (focus.column >= rows[focus.row].n) i = -1;
      if (i >= 0 && i < cat_n()) target = i;
    }
    // TROCA SO COM O FOCO EM REPOUSO. `heroPendente` e o candidato; enquanto o
    // dono anda pela fileira ele muda a cada passo e o relogio reinicia, entao
    // nenhuma troca chega a acontecer. Quando o foco para por
    // NV_HERO_REPOUSO_MS, o candidato vira o heroi.
    //
    // Isto nao e so estetica: cada troca pede uma textura de 1920 (~8 MB), e
    // atravessar uma fileira pedia uma dezena delas em dois segundos — o cache
    // estourava e despejava os posteres visiveis. Ver a nota em layout.h.
    if (target >= 0 && target != heroPending) {
      heroPending = target;
      heroPendingIn = now;
      // Voltou para a arte que ja esta no ar: cancela a troca que ainda nao
      // aconteceu, senao ela dispararia depois sem ninguem ter pedido.
      if (heroPending == heroCurrent) heroWanted = -1;
    }
    if (target >= 0 && heroPending != heroCurrent &&
        now - heroPendingIn >= NV_HERO_IDLE_MS) {
      // So ANUNCIA o desejo. Quem efetiva a troca e o desenho, quando a textura
      // da arte nova estiver pronta — ver heroDesejado.
      heroWanted = heroPending;
    } else if (target < 0 && now >= heroSwapIn) {
      // Sem card em foco, agenda o proximo item e deixa o desenho efetivar a
      // troca somente quando a textura ou o placeholder ja estiver pronto.
      int total = nArchiveHero();
      int next = total > 0 ? (heroCurrent + 1) % total : 0;
      heroPending = next;
      heroPendingIn = now - NV_HERO_IDLE_MS;
      heroWanted = next;
      heroSwapIn = now + NV_HERO_INTERVAL_MS;
    }
  }
  // Relogio da expansao. Zera a cada movimento; conta so com o foco parado.
  if (settings_expand_poster()) {
    if (focus.row != expRow || focus.column != expColumn) {
      expRow = focus.row; expColumn = focus.column;
      expSince = now;
      expOpen = 0.0f;            // fecha na hora; abrir e que e gradual
    }
    { float delay = settings_expand_poster_delay();
      int ready = expSince && (now - expSince) >= (Uint32)(delay * 1000.0f);
      // Fileira DEITADA (e a de continuar assistindo) ja mostra a arte larga:
      // nao ha para o que expandir.
      if (ready && canExpand(focus.row))
        expOpen = motionReduced ? 1.0f
                   : anim_spring(expOpen, 1.0f, dt, NV_SPRING_SCREEN); }
  } else {
    expOpen = 0.0f; expRow = expColumn = -1;
  }

  if (heroSai > 0.0f) {
    heroSai -= dt * (1000.0f / NV_HERO_FADE_MS);
    if (heroSai < 0.0f) heroSai = 0.0f;
    heroEnters = motionReduced ? 1.0f : 1.0f - heroSai;
  } else {
    heroEnters = 1.0f;
  }

  // O passeio automatico do foco era so para ver o protótipo se mexendo sem
  // ninguem no controle. Com o app navegavel ele atrapalha: rouba o foco no
  // meio de qualquer teste.

  for (int r = 0; r < nRows; r++) {
    int nAnim = rows[r].n + (rows[r].seeAll ? 1 : 0);
    if (nAnim > MAX_CARDS) nAnim = MAX_CARDS;
    for (int c = 0; c < nAnim; c++) {
      float target = focus_index(&focus, r, c) ? 1.0f : 0.0f;
      animFocus[r][c] = motionReduced
                     ? target
                     : anim_spring(animFocus[r][c], target, dt,
                                 target > animFocus[r][c] ? NV_SPRING_FOCUS : NV_SPRING_BLUR);
    }
    if (r == focus.row) {
      // Roda so o necessario para o item focado caber na area util. Deslocar
      // proporcional a coluna, como estava, jogava o primeiro card para fora da
      // tela assim que o foco ia para o segundo — some conteudo a esquerda sem
      // que o usuario tenha andado ate la.
      float lw = rows[r].stackN ? 680.0f : widthOf(rows[r].kind);
      float step = lw + gapOf(rows[r].kind);
      float left = (float)focus.column * step;
      float dir = left + lw;
      float util = NV_SCREEN_W - settings_content_x() - NV_HOME_SAFE_RIGHT;
      float target = scrollX[r];
      // a folga cobre o crescimento do foco: o card cresce para os dois lados,
      // e sem reservar essa metade ele encosta na borda ao ficar em foco
      float slack = lw * scaleOf(rows[r].kind) * 0.5f;
      if (dir + slack - target > util)  target = dir + slack - util;
      if (left - target < 0.0f)  target = left;
      if (target < 0) target = 0;
      scrollX[r] = anim_spring2_reduced(&velX[r], scrollX[r], target, dt,
                                       NV_SPRING2_SCROLL, motionReduced);
    }
  }

  // A FILEIRA EM FOCO FICA SEMPRE NO MESMO Y, e as de cima SOMEM.
  //
  // Observado nas duas capturas de referencia do dono: com o foco em "Continuar
  // assistindo" esse titulo aparece na mesma altura em que, ao descer uma
  // fileira, aparece "For You - Filme". A fileira anterior nao sobe — ela deixa
  // de ser desenhada. Palavras dele: "quando desce uma linha as coisas somem e
  // nao sobem".
  //
  // O que estava aqui era uma CAMERA: mantinha a fileira em foco dentro de um
  // viewport e rolava o minimo necessario. Isso desliza tudo para cima, e foi o
  // que fez o texto do hero passar por cima do titulo da fileira.
  //
  // O deslocamento e a soma das fileiras ANTES da que tem foco, entao o topo da
  // focada cai exatamente em NV_SHELF_TOP. Continua com mola: o salto seco
  // entre fileiras de alturas diferentes le como corte, nao como navegacao.
  float targetY = 0.0f;
  { int r = focus.row;
    for (int i = 0; i < r && i < nRows; i++)
      targetY += NV_LEGACY_ROW_HEAD_H + heightTotalOf(rows[i].kind) + rowGap();
  }
  scrollY = anim_spring2_reduced(&velY, scrollY, targetY, dt,
                                NV_SPRING2_SCROLL, motionReduced);
}

// ---------- Hero do layout moderno legacy ----------------------------------
//
// A mídia ocupa a direita dos 650px superiores; o texto fica no bloco esquerdo
// e as fileiras rolam em um viewport independente abaixo. O hero não captura
// foco: a navegação espacial começa no primeiro card, como no DOM legacy.
// Rect da ARTE do hero no ultimo quadro. A tela de detalhe le isto para
// comecar o backdrop dela EXATAMENTE onde a arte ja estava, em vez de aparecer
// do nada: o fundo e o mesmo do titulo, entao ele nao deve piscar nem crescer.
static GfxRect heroArtRect = { 0, 0, NV_SCREEN_W, NV_SCREEN_H };
void home_hero_rect(float *x, float *y, float *w, float *h) {
  *x = heroArtRect.x; *y = heroArtRect.y;
  *w = heroArtRect.w; *h = heroArtRect.h;
}

// `saida` = 0..1 de quanto o detalhe ja tomou a tela. So o TEXTO do hero sai
// (desce e apaga); a arte fica parada, porque e a mesma arte que o detalhe vai
// usar. Era isso que faltava para a abertura ler como rearranjo de layout e
// nao como troca de tela.
static void drawHero(Uint32 now, float output) {
  (void)now;
  const int motionReduced = settings_animations_reduced();
  float aArt = 1.0f;
  // MEDIDO no app web: .home-modern-hero-media fica em x=555, y=0, 1421x670.
  // A conta que estava aqui (0.28*W - 56 = 481,6 de largura 1438) vinha de
  // proporcao estimada e punha a arte 73px a esquerda do lugar.
  // Faixa ou tela cheia, conforme `modernHeroFullScreenBackdropEnabled`. Sao os
  // dois estados da MESMA tela, nao dois layouts — e cada um tem a sua rampa de
  // degrade, medida separadamente (ver GFX_HERO e GFX_HERO_CHEIO em gfx.c).
  int full = settings_hero_full();
  // Em tela cheia o bloco sobe 70px (ver layout.h).
  GfxMode modeHero = full ? GFX_HERO_FULL : GFX_HERO;
  GfxRect r = full ? (GfxRect){ 0, 0, NV_SCREEN_W, NV_HERO_FULL_H }
                    : (GfxRect){ NV_HERO_ART_X, 0, NV_HERO_ART_W, NV_HERO_ART_H };

  if(focus.row>=0 && focus.row<nRows && rows[focus.row].kind==ROW_SOCIAL) {
    float x=settings_content_x(),a=1-output;
    gfx_rect((GfxRect){0,0,NV_SCREEN_W,NV_SCREEN_H},0,GFX_SOCIAL,0,0,0,0,1,1,1,1);
    const Row *s=&rows[focus.row];
    const CatItem *p=(s->start>=0&&focus.column<s->n)
                    ?cat_item_exact(s->start+focus.column):NULL;
    if(p) {
      const char *art=art_by_format(p, 1);
      GLuint ta=art?tex_get_hero(art):0;
      // A atividade continua com um ambiente discreto, mas quando o Trakt
      // trouxe arte real ela vira o assunto do hero. A pessoa fica apenas na
      // ficha social, onde o avatar tem contexto e não compete com o titulo.
      if(ta){gfx_tex_aspect_current=tex_aspect(art);
        gfx_rect(r,ta,modeHero,0,0,0,0,0,0,0,aArt);gfx_tex_aspect_current=0;}
      else if (!art) drawArtMissing(r, 0.0f, p, aArt);

      const char *name=p->socialName[0]&&strcmp(p->socialName,"Friend")?p->socialName:NULL;
      char authorship[240];
      if(name&&p->socialAction[0])snprintf(authorship,sizeof authorship,"%s  ·  %s",name,p->socialAction);
      else if(name)snprintf(authorship,sizeof authorship,"%s",name);
      else snprintf(authorship,sizeof authorship,"%s",p->socialAction);
      txt_draw_alpha(txt_line_trim(TXT_HERO_META,authorship,210,210,221,255,680),x,146,a);

      GLuint tl=p->logo[0]?tex_get_width(p->logo,520):0;
      if(tl&&tex_aspect(p->logo)>0){
        float ap=tex_aspect(p->logo),w=520,h=w/ap;
        if(h>104){h=104;w=h*ap;}
        gfx_rect((GfxRect){x,208,w,h},tl,tex_brand_dark(p->logo)?GFX_BRAND:GFX_TEXT,
                 0,0,0,0,1,1,1,a);
      } else if(p->title[0]) {
        txt_draw_alpha(txt_line_trim(TXT_TITLE1,p->title,244,243,247,255,680),x,208,a);
      }
      if(p->directing[0])
        txt_draw_alpha(txt_line_trim(TXT_CALLOUT,p->directing,230,231,238,255,680),x,326,a);
      if(p->meta[0])
        txt_draw_alpha(txt_line_trim(TXT_HERO_META,p->meta,190,194,205,255,680),x,364,a);
      if(p->synopsis[0])
        txt_block(TXT_HERO_SIN,p->synopsis,229,231,237,x,402,700,31,a,2);
    } else {
      const char *brand=extras_path_brand_name("trakt_wordmark");
      GLuint logo=tex_get(brand);
      float brandAspect=logo?tex_aspect(brand):2.66f;
      if(brandAspect<=0)brandAspect=2.66f;
      if(logo)gfx_rect((GfxRect){x,144,44*brandAspect,44},logo,GFX_BRAND,0,0,0,0,.96f,.94f,.95f,a);
      txt_draw_alpha(txt_line(TXT_HERO_META,"YOUR COMMUNITY",210,191,199,255),x+44*brandAspect+24,154,a);
      txt_draw_alpha(txt_line(TXT_TITLE1,"Good stories connect us.",244,243,247,255),x,226,a);
      txt_block(TXT_HERO_SIN,"Discover what your friends are watching.\nA new recommendation can start here.",187,190,202,x,330,740,36,a,2);
    }
    heroArtRect=r;
    return;
  }

  if(focus.row>=0&&focus.row<nRows&&rows[focus.row].kind==ROW_CATALOGS) {
    const ColFolder *folder=col_folder(rows[focus.row].folders[focus.column]);
    if(folder) {
      if(folder->editorial) {
        /* Art is authored for this rectangle, not cropped as a movie backdrop.
           The neutral canvas continues below it; no art behind the shelves. */
        float x=settings_content_x(),a=1-output;
        GLuint art=tex_get_hero(folder->hero);
        GfxRect header={0,0,1920,500};
        if(art)gfx_rect(header,art,GFX_TEXT,0,0,0,0,1,1,1,a);
        heroArtRect=header;
        int director=!strcasecmp(folder->group,"Directors");
        txt_draw_alpha(txt_line(TXT_HERO_META,director?"DIRECTORS":"COLLECTIONS",190,193,200,255),x,122,a);
        txt_block(TXT_TITLE1,folder->title,244,243,247,x,183,860,72,a,2);
        char caption[160];
        snprintf(caption,sizeof caption,"%s  ·  %d %s",director?"Filmography":"A selection of film and series",folder->nSources,folder->nSources==1?"list":"lists");
        txt_draw_alpha(txt_line_trim(TXT_HERO_META,caption,190,193,200,255,860),x,358,a);
        txt_draw_alpha(txt_line(TXT_HERO_META,"OK to explore",224,225,230,255),x,406,a);
        return;
      }
      int isDirector=!strcasecmp(folder->group,"Directors");
      // A colecao ja traz o banner certo: e um fundo neutro, sem lettering,
      // feito para receber o conteudo por cima. O retrato do diretor entra como
      // uma segunda camada dissolvida no lado direito — nunca como um card e
      // nunca como o backdrop de um filme conhecido.
      const char *art=folder->hero[0]?folder->hero:folder->cover;
      GLuint t=0;
      if (isDirector) director_request(folder->title);
      if (!t && art[0]) t=tex_get_hero(art);
      if(t){gfx_tex_aspect_current=tex_aspect(art);gfx_rect(r,t,modeHero,0,0,0,0,0,0,0,aArt);gfx_tex_aspect_current=0;}
      heroArtRect=r;
      float x=settings_content_x(),a=1-output;
      TxtLine group=txt_line(TXT_HERO_META,folder->group,201,206,218,255);
      txt_draw_alpha(group,x,NV_COLLECTION_HERO_GROUP_Y,a);
      if (isDirector) {
        const char *photo=director_photo(folder->title);
        GLuint portrait=photo[0]
          ?tex_get_width(photo,full?1280.0f:1100.0f):0;
        if (portrait) {
          // O shader conserva a proporcao vertical e dissolve as quatro bordas.
          // A largura e intencionalmente generosa para a cabeca ter a mesma
          // presenca visual do exemplo aprovado, sem parecer uma foto espremida.
          GfxRect pr=full ? (GfxRect){840.0f,-20.0f,1080.0f,1120.0f}
                           : (GfxRect){980.0f,-15.0f,940.0f,700.0f};
          gfx_tex_aspect_current=tex_aspect(photo);
          gfx_rect(pr,portrait,GFX_PORTRAIT,0,0,0,0,0,0,0,aArt);
          gfx_tex_aspect_current=0.0f;
        }
        TxtLine name=txt_line_trim(TXT_TITLE1,folder->title,244,243,247,255,780);
        txt_draw_alpha(name,x,NV_COLLECTION_HERO_LOGO_Y,a);
        const char *meta=director_meta(folder->title);
        if (meta[0])
          txt_draw_alpha(txt_line_trim(TXT_HERO_META,meta,201,206,218,255,780),
                             x,NV_COLLECTION_HERO_LOGO_Y+92.0f,a);
        const char *con=director_known(folder->title);
        if (con[0]) {
          char line[300];
          snprintf(line,sizeof line,"Known for  %s",con);
          txt_draw_alpha(txt_line_trim(TXT_HERO_META,line,220,224,233,255,780),
                             x,NV_COLLECTION_HERO_LOGO_Y+136.0f,a);
        }
        char caption[96];
        snprintf(caption, sizeof caption, "%d %s · OK to explore",
                 folder->nSources, folder->nSources == 1 ? "list" : "lists");
        txt_draw_alpha(txt_line_trim(TXT_HERO_SIN, caption,
                                           205, 210, 221, 255, 780),
                           x, NV_COLLECTION_HERO_CAPTION_Y, a);
        return;
      }
      // As logos de colecao sao arte, nao texto rasterizado. O limite de
      // decode fica acima do tamanho desenhado para preservar nitidez quando
      // a proporcao da logo pede a altura maxima.
      GLuint logo=(!isDirector && folder->logo[0])
        ?tex_get_width(folder->logo,NV_COLLECTION_HERO_LOGO_MAX_W+40.0f):0;
      float ap=logo?tex_aspect(folder->logo):0;
      float endTitle=NV_COLLECTION_HERO_LOGO_Y+NV_COLLECTION_HERO_LOGO_MAX_H;
      if(logo&&ap>0){
        float w=NV_COLLECTION_HERO_LOGO_MAX_W,h=w/ap;
        if(h>NV_COLLECTION_HERO_LOGO_MAX_H){h=NV_COLLECTION_HERO_LOGO_MAX_H;w=h*ap;}
        gfx_rect((GfxRect){x,NV_COLLECTION_HERO_LOGO_Y,w,h},logo,
                 tex_brand_dark(folder->logo)?GFX_BRAND:GFX_TEXT,
                 0,0,0,0,.96f,.97f,.98f,a);
        endTitle=NV_COLLECTION_HERO_LOGO_Y+h;
      } else {
        TxtLine name=txt_line_trim(TXT_TITLE1,folder->title,241,243,247,255,700);
        txt_draw_alpha(name,x,NV_COLLECTION_HERO_LOGO_Y,a);
        endTitle=NV_COLLECTION_HERO_LOGO_Y+name.h;
      }
      char caption[96];snprintf(caption,sizeof caption,"%d %s · OK to explore",folder->nSources,folder->nSources==1?"list":"lists");
      float yCap=NV_COLLECTION_HERO_CAPTION_Y;
      if(isDirector) {
        // Ficha do TMDB abaixo do nome: quem e, quando e onde nasceu, tres
        // linhas de bio e os titulos por que e conhecido. Chega em segundo
        // plano; ate chegar a legenda fica onde sempre ficou.
        director_request(folder->title);
        if(director_ready(folder->title)) {
          // O bloco comeca logo abaixo do nome e TERMINA antes do cabecalho da
          // fileira (NV_SHELF_TOP): o numero de linhas da bio e o que cede.
          // Largura 780: fica aquem do cartao da capa (que comeca em 1096).
          float yy=endTitle+30,cap=NV_SHELF_TOP-30,width=780;
          const char *meta=director_meta(folder->title),*bio=director_bio(folder->title),*con=director_known(folder->title);
          float fixed=(meta[0]?38:0)+(con[0]?38:0)+34;   // meta + conhecido + legenda
          int lines=(int)((cap-yy-fixed-12)/31);if(lines>3)lines=3;
          if(meta[0]){txt_draw_alpha(txt_line_trim(TXT_HERO_META,meta,201,206,218,255,width),x,yy,a);yy+=38;}
          if(bio[0]&&lines>0){yy+=txt_block(TXT_HERO_SIN,bio,222,225,232,x,yy,width,31,a,lines)+12;}
          if(con[0]){char l[300];snprintf(l,sizeof l,"Known for  %s",con);
            txt_draw_alpha(txt_line_trim(TXT_HERO_META,l,236,232,244,255,width),x,yy,a);yy+=38;}
          yCap=yy;
        }
      }
      TxtLine sub=txt_line(TXT_HERO_SIN,caption,205,210,221,255);txt_draw_alpha(sub,x,yCap,a);
      return;
    }
  }

  // TROCA SO COM A ARTE NOVA JA DECODIFICADA.
  //
  // O pedido e feito aqui, no desenho, porque e aqui que se sabe qual arquivo a
  // arte e (o caminho sai do catalogo, com a pasta como reserva). Enquanto o
  // cache nao devolve textura, heroAtual nao muda e a tela segue com a arte que
  // ja estava — que e exatamente o que o dono pediu ao andar depressa.
  if (heroWanted >= 0 && heroWanted != heroCurrent) {
    const char *artD = art_by_identity(heroWanted, 1);
    // Ausencia de arte tambem e um estado pronto: o placeholder pertence ao
    // item e pode entrar sem apagar o hero anterior primeiro.
    int artReady = !artD || tex_get_hero(artD);
    if (artReady) {
      heroPrevious = heroCurrent;
      heroCurrent = heroWanted;
      heroWanted = -1;
      heroSai = (motionReduced || !artD) ? 0.0f : 1.0f;
      heroEnters = (motionReduced || !artD) ? 1.0f : 0.0f;
      heroSwapIn = SDL_GetTicks() + NV_HERO_INTERVAL_MS;
    }
  }

  const CatItem *ci = cat_item_exact(heroCurrent);
  const char *artA = art_by_identity(heroCurrent, 1);
  const CatItem *cAnt = cat_item_exact(heroPrevious);
  const char *artB = art_by_identity(heroPrevious, 1);

  // Teto de 1920: o hero ocupa a tela e a 960 saia esticado ao dobro.
  // O ANTERIOR so e pedido ENQUANTO a mistura acontece. Estava sendo pedido em
  // TODO quadro, mesmo com a troca ja terminada, quando ele nao e desenhado: se o
  // cache ja o tinha despejado, o pedido o trazia de volta — uma textura de
  // 1920 (~8 MB) re-decodificada para NAO ser desenhada, empurrando os posteres
  // visiveis para fora do orcamento.
  GLuint tAnt = (heroSai > 0.0f && artB) ? tex_get_hero(artB) : 0;
  // Pedir a nova JA, durante o esvanecimento: e este pedido que enfileira o
  // decode, e e por isso que o vazio dura o tempo do carregamento e nao mais.
  GLuint tCurrent = artA ? tex_get_hero(artA) : 0;
  if (tAnt) {
    // Esvanecimento com aceleracao e desaceleracao: o medido fica ~25% do
    // percurso quase parado no comeco, entao rampa reta le como corte na saida.
    (void)drawArtHero(r, modeHero, cAnt, artB,
                          anim_smooth(heroSai) * aArt);
  } else if (heroSai > 0.0f) {
    drawPlaceholderHero(r, cAnt, anim_smooth(heroSai) * aArt);
  }
  if (tCurrent && heroEnters > 0.0f) {
    (void)drawArtHero(r, modeHero, ci, artA,
                          anim_smooth(heroEnters) * aArt);
  } else if (!tCurrent) {
    drawPlaceholderHero(r, ci,
                           aArt * (heroEnters > 0.0f ? 1.0f : heroEnters));
  }
  gfx_tex_aspect_current = 0.0f;
  heroArtRect = r;

  float aText = 1.0f - output;
  float scrolldownCopy = output * NV_SCREEN_H * 0.06f;
  if (aText <= 0.004f) return;

  // BLOCO DE TEXTO DO HERO — transcrito do CSS do app web, nao deduzido de
  // captura. `.home-modern-hero-copy` e um flex column com justify-content
  // flex-end e gap 16, ancorado numa base fixa; os filhos, na ordem:
  //   .home-hero-brand         caixa do logo, 440x200, arte no topo-esquerda
  //   .home-modern-hero-meta-line   21/500 #b3b3b3, tokens separados por •
  //   .home-modern-hero-secondary   18/600 branco 88%, com selos e o IMDb
  //   .home-hero-description        22/400 branco, largura 560, leading 30
  // Cada bloco vazio some (`.is-empty { display: none }`), e e por isso que a
  // altura do conjunto muda de titulo para titulo — nao por posicao absoluta.
  //
  // O conteudo de cada linha vem de buildModernHeroPresentation
  // (homeScreen.js:2497), que separa o caso "continuar assistindo" do resto.
  int contHero = (ci && ci->progress > 0 && ci->remainingMin > 0);

  // Linha de meta. No web sao tokens juntados por "•"; ci->genero ja chega
  // como "Filme · Terror", que e o par (tipo, primeiro genero) do web.
  char metaLine[288];
  metaLine[0] = 0;
  if (contHero && ci->season > 0) {
    char header[64];
    snprintf(header, sizeof header, "S%d E%d", ci->season, ci->episode);
    snprintf(metaLine, sizeof metaLine, "%s%s%s", header,
             (ci->genre[0] ? "  \xc2\xb7  " : ""), ci->genre);
  } else if (ci && ci->genre[0]) {
    snprintf(metaLine, sizeof metaLine, "%s", ci->genre);
  }
  if (ci && ci->meta[0]) {
    size_t n = strlen(metaLine);
    snprintf(metaLine + n, sizeof metaLine - n, "%s%s",
             n ? "   \xe2\x80\xa2   " : "", ci->meta);
  }

  // Linha secundaria: destaque de progresso, selos e a nota do IMDb. O web so
  // mostra o IMDb aqui quando ja existe destaque ou selo (showImdbSecondary);
  // no outro caso ele vai para o fim da linha de meta.
  char highlight[64];
  highlight[0] = 0;
  if (contHero) snprintf(highlight, sizeof highlight, "%d MINUTES LEFT",
                         ci->remainingMin);
  const char *badge = (ci && ci->age_rating[0] && !contHero) ? ci->age_rating : NULL;
  char score[8];
  score[0] = 0;
  if (ci && ci->score > 0) snprintf(score, sizeof score, "%.1f", ci->score / 10.0f);
  int hasSec = (highlight[0] || badge || score[0]);

  const char *synopsis = (ci && ci->synopsis[0]) ? ci->synopsis : "";

  // --- empilhamento de baixo para cima, como o flex-end do CSS ---
  float base = NV_SHELF_TOP - NV_HERO_COPY_GAP + scrolldownCopy;
  float hSin = synopsis[0] ? txt_block(TXT_HERO_SIN, synopsis, 255, 255, 255, -1, 0,
                                      NV_HERO_SIN_W, NV_LD_HERO_SIN, 0.0f, 3)
                          : 0.0f;
  float ySin  = base - hSin;
  float ySec  = hasSec ? (ySin - (synopsis[0] ? NV_HERO_COPY_LINE : 0.0f)
                          - NV_LD_HERO_SEC) : ySin;
  float yMeta = ySec - ((hasSec || synopsis[0]) ? NV_HERO_COPY_LINE : 0.0f)
                - (metaLine[0] ? NV_LD_HERO_META : 0.0f);
  float logoY = yMeta - NV_HERO_COPY_LINE - NV_LOGO_HERO_H;
  float x = settings_content_x();

  // Logo do titulo, ou o nome em texto quando nao ha logo
  // (.home-hero-title-text, 56/600 no modern — nao os 76 do TXT_TITULO1).
  GLuint tlogo = (ci && ci->logo[0]) ? tex_get(ci->logo) : 0;
  if (tlogo) {
    float ap = tex_aspect(ci->logo);
    if (ap <= 0.0f) ap = 4.0f;
    float hTitle = NV_LOGO_HERO_H, wTitle = hTitle * ap;
    float maxW = full ? NV_LOGO_HERO_FULL_MAX_W : NV_LOGO_HERO_MAX_W;
    if (wTitle > maxW) { wTitle = maxW; hTitle = wTitle / ap; }
    // object-position: left top — a arte encosta no TOPO da caixa.
    GfxRect rl = { x, logoY, wTitle, hTitle };
    gfx_tex_aspect_current = 0.0f;
    // Logo escuro vira branco. Mesma regra da tela de detalhe: o TMDB nao marca
    // claro/escuro, entao a decisao sai da luminancia MEDIDA (tex_luminancia).
    // Logo claro ou colorido passa intacto; -1 (ainda carregando) nao tinge.
    { GfxMode m = tex_brand_dark(ci->logo) ? GFX_BRAND : GFX_TEXT;
      // O LOGO DO TITULO acompanha a ARTE, nao o texto. MEDIDO: 205 ms depois
      // da tecla a arte antiga ainda estava a 85% e o logo JA tinha sumido por
      // inteiro; ele so reaparece no mesmo quadro em que a arte nova entra.
      gfx_rect(rl, tlogo, m, 0, 0, 0, 0.0f, 1, 1, 1, aText * heroEnters); }
  } else {
    // .legacy-webos .home-hero-title-text: 76px (components.css:19164), nao os
    // 56 do tema padrao.
    // Sem titulo NAO se inventa titulo. Aqui havia uma lista de demonstracao
    // ("Ruptura", "Silo", "Shrinking"...) que preenchia o hero com o nome de
    // outra serie quando o item ainda nao tinha nome — indistinguivel de dado
    // real para quem olha a tela. Mesma familia do elenco e da classificacao
    // que ja sairam do detalhe. Sem nome, o hero fica so com a arte, que ja
    // basta, e o texto aparece quando o dado chegar.
    if (ci && ci->title[0]) {
      TxtLine title = txt_line(TXT_TITLE1, ci->title, 255, 255, 255, 255);
      txt_draw_alpha(title, x, logoY + NV_LOGO_HERO_H - (float)title.h,
                         aText);
    }
  }

  if (metaLine[0]) {
    float badgeW=ci?badges_draw(badges_provider(ci->providerName),x,yMeta,150,24,aText):0;
    TxtLine lm = txt_line_trim(TXT_HERO_META, metaLine, 179, 179, 179, 255,
                                  NV_HERO_SIN_W-badgeW);
    // META E SINOPSE TROCAM NA HORA, sem esvanecer com a arte. MEDIDO: no
    // quadro a 205 ms, com a arte antiga ainda a 85%, a linha de meta e a
    // sinopse ja eram as do titulo NOVO, com o texto opaco. Multiplicar por um
    // alfa de troca aqui era invencao nossa — e, com o rasterizador fazendo 2
    // linhas por quadro (text.c:40), esvanecer texto que ainda esta assentando
    // e o pior caso possivel.
    txt_draw_alpha(lm, x+badgeW, yMeta, aText);
  }

  if (hasSec) {
    float cx = x;
    float a = aText;
    if (highlight[0]) {
      // .home-modern-hero-highlight: branco cheio, peso 600, tracking 0.04em.
      cx += txt_tracking(TXT_HERO_SEC, highlight, 255, 255, 255, cx, ySec, a,
                         NV_FT_HERO_SEC * 0.04f);
      cx += 14.0f;
    }
    if (badge) {
      // Metadata, not a focus target: compact neutral surface and soft corners.
      TxtLine lb = txt_line(TXT_CAPTION, badge, 235, 235, 240, 255);
      float bw = lb.w + 22.0f, bh = 32.0f;
      float by = ySec + (NV_LD_HERO_SEC - bh) * 0.5f;
      gfx_color((GfxRect){ cx, by, bw, bh }, 0.22f,
              0.13f, 0.14f, 0.16f, 0.94f * a);
      txt_draw_alpha(lb, cx + 11.0f, by + (bh - lb.h) * 0.5f, a);
      cx += bw + 14.0f;
    }
    if (score[0]) {
      // .home-hero-imdb: o selo amarelo de 40px e a nota logo depois, com 10
      // de respiro. O SVG do IMDb nao esta empacotado aqui; o retangulo
      // amarelo com as letras pretas e a mesma leitura a esta escala.
      TxtLine ls = txt_line(TXT_MINI, "IMDb", 8, 8, 8, 255);
      float sw = 40.0f, sh = ls.h + 6.0f;
      gfx_color((GfxRect){ cx, ySec + (NV_LD_HERO_SEC - sh) * 0.5f, sw, sh },
              0.12f, 0.96f, 0.78f, 0.06f, a);
      txt_draw_alpha(ls, cx + (sw - ls.w) * 0.5f,
                         ySec + (NV_LD_HERO_SEC - sh) * 0.5f + 3.0f, a);
      cx += sw + 10.0f;
      TxtLine ln = txt_line(TXT_HERO_SEC, score, 179, 179, 179, 255);
      txt_draw_alpha(ln, cx, ySec, a);
    }
  }

  if (synopsis[0])
    txt_block(TXT_HERO_SIN, synopsis, 255, 255, 255, x, ySin, NV_HERO_SIN_W,
              NV_LD_HERO_SIN, aText, 3);
}

// Fundo CINZA, e so. Eu tinha posto aqui a arte do titulo em destaque
// desfocada, achando que era isso o "cinza do Apple TV" — mas o efeito era o
// oposto do pedido: a arte do hero subia e saia normalmente, e a copia
// desfocada dela continuava no fundo, dando a impressao de que a imagem nunca
// tinha subido. Fundo neutro nao compete com nada.
static void drawBackground(void) {
  GfxRect screen = { 0, 0, NV_SCREEN_W, NV_SCREEN_H };
  // A tela ja foi limpa com ESTA MESMA COR por glClearColor/glClear em
  // main.c antes de app_desenhar. Pintar por cima era uma camada de tela
  // cheia jogada fora por quadro — e o custo dominante nesta GPU e fill
  // rate (gfx.c registra que DUAS camadas de tela cheia derrubavam a
  // Mali-G71 para ~40fps). Nao repor sem antes mudar a cor do clear.
  (void)screen;
}

static void drawShortcuts(int r, float y) {
  float w = widthOf(ROW_CATALOGS), h = heightOf(ROW_CATALOGS);
  static int last=-1;static Uint32 since;
  for (int c = 0; c < rows[r].n; c++) {
    float x = settings_content_x() + c * stepOf(ROW_CATALOGS) - scrollX[r];
    if (x + w < 0 || x > NV_SCREEN_W) continue;
    float f = animFocus[r][c], radius = radiusOf(w, h);
    GfxRect card = {x, y, w, h};
    if (f > .01f) {
      float smaller = w < h ? w : h;
      gfx_color((GfxRect){x - NV_RING_FOCUS, y - NV_RING_FOCUS,
        w + 2*NV_RING_FOCUS, h + 2*NV_RING_FOCUS},
        (radius * smaller + NV_RING_FOCUS) / (smaller + 2*NV_RING_FOCUS), .96f, .97f, .98f, f);
    }
    gfx_color(card, radius, NV_COLOR_SKELETON_R, NV_COLOR_SKELETON_G, NV_COLOR_SKELETON_B, 1);
    const ColFolder *folder=col_folder(rows[r].folders[c]);if(!folder)continue;
    const char *art = folder->cover;
    GLuint tex = art && art[0] ? tex_get_width(art, w) : 0;
    if(focus.row==r&&focus.column==c&&folder->frames>0 &&
       !settings_animations_reduced()) {
      int id=rows[r].folders[c];Uint32 now=SDL_GetTicks();
      if(last!=id){last=id;since=now;}
      if(now-since>350) {
        char frame[700];int index=(int)((now-since-350)/67)%folder->frames+1;
        snprintf(frame,sizeof frame,"%s/%03d.jpg",folder->frameDir,index);
        GLuint motion=tex_get_width(frame,480);
        if(motion)tex=motion;
        snprintf(frame,sizeof frame,"%s/%03d.jpg",folder->frameDir,index%folder->frames+1);
        tex_get_width(frame,480);
        snprintf(frame,sizeof frame,"%s/%03d.jpg",folder->frameDir,(index+1)%folder->frames+1);
        tex_get_width(frame,480);
      }
    }
    if (tex) {
      gfx_tex_aspect_current = tex_aspect(art);
      gfx_rect(card, tex, GFX_CARD, 0, 0, 0, radius, 0, 0, 0, 1);
      gfx_tex_aspect_current = 0;
    } else if (folder->title[0]) {
      // NO ART YET, so the name stands in for it. The packaged collections' covers
      // are local files and appear on the first frame; the ACCOUNT's are CDN URLs
      // and arrive seconds later. Until now the card was a mute grey rectangle —
      // indistinguishable from a broken one, and saying nothing about what it was.
      //
      // The poster rows already solve this with drawArtMissing; here the name is
      // enough, because an "Art unavailable" caption would be a lie: the art is on
      // its way, not missing.
      TxtLine name = txt_line_trim(TXT_CAPTION, folder->title,
                                   184, 188, 198, 255, card.w - 32.0f);
      txt_draw_alpha(name, card.x + (card.w - name.w) * 0.5f,
                     card.y + card.h * 0.5f - name.h * 0.5f, 0.9f);
    }
    // A propria capa e a identidade do catalogo. O nome/logo vinha sendo
    // desenhado novamente por cima dela e criava exatamente a duplicacao que
    // o usuario apontou em Netflix, Prime Video, Disney+ e nas listas IMDb.
    // Titulo de fileira continua no cabecalho; dentro do card fica somente a
    // arte, sem veu, badge ou logo auxiliar.
  }
}

void home_draw(Uint32 now) {
  drawBackground();
  float pd = detail_progress();
  if (settings_hero_on()) drawHero(now, pd);

  // ABERTURA DO DETALHE: as fileiras DESCEM e apagam; a arte de fundo fica.
  //
  // E o movimento que o dono descreveu — "so os posters descem e mantem o
  // background". O detalhe ja nao voa mais a partir do card: a arte dele entra
  // em tela cheia ganhando opacidade, entao o que o olho segue e a saida das
  // fileiras. Descer 8% da altura da tela e o bastante para ler como saida sem
  // que a ultima fileira suma antes da hora.
  //
  // O `pd` vem da MESMA mola que o detalhe usa para desenhar (detail_progresso),
  // e nao de um relogio proprio: dois relogios descasariam e a home sairia
  // adiantada ou atrasada em relacao a arte que entra.
  float scrolldown = pd * NV_SCREEN_H * 0.08f;
  if (pd >= 0.996f) return;   // detalhe assentado: nada da home aparece

  // VIEWPORT DAS FILEIRAS. `.home-modern-rows-viewport` (components.css:6929) e
  // um bloco absoluto com bottom:0, height 52% e overflow-y:auto — ou seja as
  // fileiras rolam DENTRO dos 52% de baixo e o que sobe alem disso e CLIPADO.
  // O port desenhava as fileiras soltas sobre a tela inteira, e por isso a
  // fileira que saia por cima aparecia atravessada no bloco do hero em vez de
  // sumir. O hero nao rola: so o conteudo dele muda com o foco.
  gfx_crop(0, NV_SHELF_TOP-96, NV_SCREEN_W, NV_SCREEN_H - NV_SHELF_TOP+96);
  float y = NV_SHELF_TOP - scrollY + scrolldown;
  for (int r = 0; r < nRows; r++) {
    KindRow kind = rows[r].kind;
    float fade=anim_clamp((y-(NV_SHELF_TOP-80))/80,0,1);
    gfx_opacity_group=fade*fade*(3-2*fade);
    float lw = rows[r].stackN ? 680.0f : widthOf(kind);
    float lh = heightOf(kind), step = lw + gapOf(kind);
    float artH = lh;
    // `y` é o topo do cabeçalho da fileira; os cards começam depois do título.
    // Separar os dois evita que o título da fileira seguinte seja desenhado
    // sobre a arte da anterior quando a fileira tem cards altos.
    float cardY = y + NV_LEGACY_ROW_HEAD_H;

    int landscape = editorial(kind) || ((kind != ROW_CONTINUE) && settings_posters_landscape());
    int labelFora = hasLabel(kind);
    if (y < NV_SCREEN_H + 200 && y + NV_LEGACY_ROW_HEAD_H + lh > -200) {
      // `catalogTypeSuffixEnabled`. formatCatalogRowTitle (homeUtils.js:62) faz
      // `if (!showTypeSuffix) return base;` — devolve o nome capitalizado e
      // pronto. Aqui o sufixo e tirado no DESENHO e nao na descoberta, senao a
      // preferencia so valeria depois que a rede trouxesse os catalogos de
      // novo — ou seja, so no proximo arranque.
      const char *rotFilter = rows[r].title;
      char withoutSuffix[96];
      if (!settings_suffix_kind() && rotFilter) {
        const char *cut = strstr(rotFilter, " - ");
        const char *last = NULL;
        while (cut) { last = cut; cut = strstr(cut + 3, " - "); }
        if (last && (!strcmp(last + 3, "Film")
                       || !strcmp(last + 3, "Series"))) {
          size_t n = (size_t)(last - rotFilter);
          if (n >= sizeof withoutSuffix) n = sizeof withoutSuffix - 1;
          memcpy(withoutSuffix, rotFilter, n);
          withoutSuffix[n] = 0;
          rotFilter = withoutSuffix;
        }
      }
      TxtLine tl = txt_line_trim(TXT_ROW_TITLE, rotFilter, 245, 246, 249, 255,
                                    NV_SCREEN_W - settings_content_x() - 180);
      txt_draw(tl, settings_content_x(), y);
      if(kind==ROW_SOCIAL) {
        const char *brand=extras_path_brand_name("trakt_wordmark");
        GLuint logo=tex_get(brand);float ap=logo?tex_aspect(brand):2.66f;
        if(ap<=0)ap=2.66f;
        if(logo)gfx_rect((GfxRect){settings_content_x()+tl.w+18,y+(tl.h-30)*.5f,30*ap,30},
                         logo,GFX_BRAND,0,0,0,0,.95f,.93f,.94f,1);
      }
      if(!strncmp(rows[r].catId,"ai_",3)) {
        TxtLine ai=txt_line(TXT_HERO_META,"AI-powered",183,192,219,255);
        txt_draw(ai,settings_content_x()+tl.w+22,y+(tl.h-ai.h)*.5f);
      }
      if (focus.row == r) {
        char pos[32];
        if (focus.column < rows[r].n)
          snprintf(pos, sizeof pos, "%d / %d", focus.column + 1, rows[r].n);
        else snprintf(pos, sizeof pos, "See all");
        TxtLine lp = txt_line(TXT_HERO_META, pos, 186, 191, 202, 255);
        txt_draw(lp, NV_SCREEN_W - NV_HOME_SAFE_RIGHT - lp.w, y + (tl.h - lp.h)*.5f);
      }
      if (kind == ROW_CATALOGS) {
        drawShortcuts(r, cardY);
        y += NV_LEGACY_ROW_HEAD_H + heightTotalOf(kind) + rowGap();
        continue;
      }

      // CARD "VER TUDO" no fim da fileira. Desenhado antes do laco dos cartazes
      // para nao herdar as variaveis dele; ele nao e um titulo e nao usa arte.
      //
      // MEDIDO no web (.home-seeall-card-inner): moldura de 2 px em
      // rgba(255,255,255,0.12) sobre rgba(255,255,255,0.06), seta e rotulo
      // empilhados e centrados. Focado, a moldura acende.
      if (rows[r].seeAll) {
        int c = rows[r].n;
        float f = animFocus[r][c];
        float esc = 1.0f + scaleOf(kind) * f;
        float w = lw * esc, h = artH * esc;
        float cx = settings_content_x() + c * step - scrollX[r] + lw * 0.5f;
        float cy = cardY + artH * 0.5f;
        if (cx > -lw * 1.5f && cx < NV_SCREEN_W + lw) {
          float px = cx - w * 0.5f, py = cy - h * 0.5f;
          float radius = radiusOf(w, h);
          GfxRect r0 = { px, py, w, h };
          float luma = 0.06f + 0.10f * f;
          gfx_color(r0, radius, 1, 1, 1, luma);
          gfx_rect(r0, 0, GFX_RING, 0, 2.0f / h, 0, radius,
                   1, 1, 1, (0.12f + 0.70f * f));
          { TxtLine ls = txt_line(TXT_TITLE2, "\xe2\x86\x92",
                                    236, 237, 242, 255);
            TxtLine lr = txt_line(TXT_ROW_TITLE, "See all",
                                    f > 0.5f ? 255 : 190, f > 0.5f ? 255 : 194,
                                    f > 0.5f ? 255 : 203, 255);
            float block = ls.h + 14.0f + lr.h;
            float by = py + (h - block) * 0.5f;
            txt_draw_alpha(ls, px + (w - ls.w) * 0.5f, by, 0.95f);
            txt_draw_alpha(lr, px + (w - lr.w) * 0.5f,
                               by + ls.h + 14.0f, 1.0f); }
        }
      }

      for (int passe = 1; passe < 2; passe++) {
        for (int c = 0; c < rows[r].n; c++) {
          float f = animFocus[r][c];
          if (passe == 0 && f < 0.01f) continue;
          float esc = 1.0f + scaleOf(kind) * f;
          float w = lw * esc, h = artH * esc;
          // EXPANSAO EM REPOUSO. `abre` so e diferente de zero no card focado
          // desta fileira; os DEPOIS dele sao empurrados pela mesma medida.
          //
          // A altura nao entra na conta: na referencia ela nao muda, e o card
          // cresce so para a direita a partir de uma borda esquerda parada.
          float openAmt = (r == expRow && c == expColumn) ? expOpen : 0.0f;
          float widthIs_open = artH * esc * NV_EXP_ASPECT;
          float pushes = 0.0f;
          if (r == expRow && expOpen > 0.0f && c > expColumn)
            pushes = (artH * NV_EXP_ASPECT - lw) * expOpen;
          if (openAmt > 0.0f) w = lw * esc + (widthIs_open - lw * esc) * openAmt;
          float cx = settings_content_x() + c * step - scrollX[r] + lw * 0.5f
                   + pushes + (w - lw * esc) * 0.5f;
          // Sem levantamento: no web o card focado nao sai do lugar.
          float cy = cardY + artH * 0.5f;
          if (cx < -lw * 1.5f || cx > NV_SCREEN_W + lw) continue;
          float px = cx - w * 0.5f, py = cy - h * 0.5f;

          if (passe == 0) {
            // Sem sombra. Ela existia para separar o card do fundo, mas sobre
            // arte colorida vira um halo escuro em volta do item focado — e o
            // aparelho nao tem isso: la o foco se marca por escala e brilho.
            (void)f;
            continue;
          }

          const int idxCat = rows[r].start + c;
          if(kind==ROW_TOP10 && rows[r].stackN) {
            // Sem placa de fundo: os cartazes empilhados ja formam o card.
            int count=rows[r].stackN<6?rows[r].stackN:6;
            for(int k=0;k<count;k++) {
              const CatItem *it=cat_item_exact(idxCat+k);if(!it)continue;
              GfxRect pr={px+20+k*78,py+18,178,h-72};
              const char *pa=art_by_format(it,0);
              GLuint tx=pa?tex_get_width(pa,178):0;
              if(tx){gfx_tex_aspect_current=tex_aspect(pa);gfx_rect(pr,tx,GFX_CARD,0,0,0,.055f,1,1,1,1);gfx_tex_aspect_current=0;}
              else drawArtMissing(pr,.055f,it,1);
            }
            txt_draw(txt_line(TXT_CAPTION,"TOP 100   ·   Explore the first 10",242,235,248,255),px+24,py+h-42);
            if(focus.row==r)hasItemFocus=0;
            continue;
          }
          if(kind==ROW_SOCIAL && rows[r].start<0) {
            GfxRect b={px,py,w,h};
            gfx_color(b,.055f,.115f,.09f,.15f,1);
            if(f>.01f)gfx_rect(b,0,GFX_RING,0,.008f,0,.055f,.95f,.93f,.99f,f);
            txt_draw(txt_line_trim(TXT_CALLOUT,"Among friends",240,234,248,255,w-48),px+24,py+24);
            txt_draw(txt_line_trim(TXT_CAPTION,"No activity available right now.",195,183,211,255,w-48),px+24,py+91);
            txt_draw(txt_line_trim(TXT_CAPTION,"Follow people on Trakt to discover more.",195,183,211,255,w-48),px+24,py+126);
            txt_draw(txt_line_trim(TXT_CAPTION,"OK · Check connection",240,231,250,255,w-48),px+24,py+h-50);
            if(focus.row==r)hasItemFocus=0;
            continue;
          }
          const CatItem *cItem = cat_item_exact(idxCat);
          if(kind==ROW_SOCIAL && cItem) {
            if(focus.row==r)hasItemFocus=0;
            // A atividade social precisa de contexto, nao de um segundo hero.
            // Sem painel e sem contorno: o palco neutro da home faz o trabalho
            // de fundo. A imagem tem um papel editorial menor, thumbnail da
            // obra, enquanto autoria e acao respiram diretamente na tela.
            const float contentTop=py+24.0f;
            const float contentBase=py+h-24.0f;
            const float artW=134.0f;
            const float artX=px+w-24.0f-artW;
            const char *thumbPath=cItem->poster[0]?cItem->poster:
                                  (cItem->backdrop[0]?cItem->backdrop:NULL);
            if(thumbPath){GLuint thumb=tex_get_width(thumbPath,artW);
              if(thumb){GfxRect tr={artX,contentTop,artW,contentBase-contentTop};
                gfx_tex_aspect_current=tex_aspect(thumbPath);
                gfx_rect(tr,thumb,GFX_CARD,0,0,0,.055f,0,0,0,1);gfx_tex_aspect_current=0;
              }
            }
            // Avatar maior e centralizado na mesma faixa vertical da arte.
            // O eixo comum deixa a composicao com cara de ficha editorial,
            // em vez de avatar solto no topo e thumbnail separado embaixo.
            float d=120.0f, ax=px+24.0f, ay=py+(h-d)*.5f;
            GfxRect avatar={ax,ay,d,d};
            GLuint photo=cItem->socialAvatar[0]?tex_get_width(cItem->socialAvatar,220):0;
            // O foco e um disco atras da imagem, nunca um stroke por cima.
            // Assim as duas circunferencias compartilham o mesmo centro e o
            // aro permanece uniforme inclusive no limite superior da fileira.
            float dflt=5.0f*f;
            if(f>.01f)gfx_rect(avatar,0,GFX_DISK,0,0,0,0,.96f,.96f,.98f,f);
            GfxRect core={ax+dflt,ay+dflt,d-dflt*2,d-dflt*2};
            gfx_rect(core,0,GFX_DISK,0,0,0,0,.15f,.16f,.18f,1);
            if(photo){gfx_tex_aspect_current=tex_aspect(cItem->socialAvatar);
              gfx_rect(core,photo,GFX_AVATAR,0,0,0,0,1,1,1,1);gfx_tex_aspect_current=0;}
            else {char initial[8]="?";const char *name=cItem->socialName[0]?cItem->socialName:cItem->pais;
              if(name[0]){size_t z=1;while(z<4 && (name[z]&0xc0)==0x80)z++;memcpy(initial,name,z);initial[z]=0;}
              TxtLine l=txt_line(TXT_TITLE2,initial,235,236,240,255);txt_draw(l,ax+(d-l.w)*.5f,ay+(d-l.h)*.5f);}
            float tx=ax+d+24.0f,tw=thumbPath?artX-tx-24.0f:w-192.0f;
            TxtLine name=txt_line_trim(TXT_CW_TITLE,cItem->socialName[0]?cItem->socialName:cItem->pais,245,245,247,255,tw);
            txt_draw(name,tx,contentTop);
            TxtLine action=txt_line_trim(TXT_MINI,cItem->socialAction[0]?cItem->socialAction:cItem->providerName,181,185,196,255,tw);
            txt_draw(action,tx,contentTop+38.0f);
            TxtLine title=txt_line_trim(TXT_CW_META,cItem->title,228,231,239,255,tw);
            txt_draw(title,tx,contentTop+92.0f);
            TxtLine ep=txt_line_trim(TXT_MINI,cItem->season?cItem->directing:"Film",181,185,196,255,tw);
            txt_draw(ep,tx,contentTop+130.0f);
            TxtLine source=txt_line_trim(TXT_MINI,cItem->providerName[0]?cItem->providerName:"Trakt",155,161,174,255,tw);
            txt_draw(source,tx,contentBase-14.0f);
            if(f>.1f){TxtLine see=txt_line(TXT_MINI,"See profile",235,237,244,255);txt_draw_alpha(see,tx,contentBase-40.0f,f);}
            continue;
          }
          const char *path = NULL;
          // Card DEITADO pede arte deitada. No web o poster do card landscape sai
          // de `landscapePoster` -> `background` -> `backdrop` -> `poster`
          // (homeScreen.js:3155), nao do poster 2:3 — usar o retrato aqui faria o
          // shader recortar a cabeca de todo mundo para caber em 16:9.
          // Aberto, o card mostra a arte DEITADA: e para isso que ele abre.
          // A troca acontece na metade do caminho, quando a moldura ja tem
          // largura de 16:9 e o retrato comecaria a ser recortado feio.
          path = art_by_identity(idxCat, openAmt > 0.5f ||
                                        kind == ROW_CONTINUE ||
                                        kind == ROW_RETURN || landscape);

          if (focus_index(&focus, r, c)) {
            GfxRect here = { px, py, w, h };
            itemFocus.index_ = idxCat;
            itemFocus.rect   = here;
            itemFocus.art   = path;
            itemFocus.title = cItem ? cItem->title : NULL;
            itemFocus.genre = cItem ? cItem->genre : NULL;
            itemFocus.meta   = cItem ? cItem->meta : NULL;
            hasItemFocus = 1;
          }
          // Pede pela largura REAL do card: e esta fileira que multiplica.
          // Com o teto unico de 640 cada poster custava 2,4 MB e o cache
          // estourava com ~40 texturas, despejando o que ainda estava na tela.
          GLuint t = path ? tex_get_width(path, w) : 0;
          // ANEL DE FOCO: 4 px de #FFFFFF, POR FORA da arte.
          //
          // Era 2 px de #f5f5f5, tirado do `box-shadow` do app WEB. MEDIDO no
          // aparelho de referencia (TCL, mesmo card, mesma fileira): 4 px
          // solidos de #FFFFFF, x 102->105 sem rampa. O nosso media 2 px com
          // antialias (#A1A1A2 -> #C6C6C7 -> #E3E3E4 -> #F3F3F3) e a rampa ja
          // entrava na arte.
          //
          // A 3 m de distancia, 2 px cinza-suave contra 4 px branco solido e a
          // diferenca entre ver onde se esta e procurar o foco na tela. O mesmo
          // valor aparece em card de episodio e botao de detalhe na referencia:
          // e UM numero para o app inteiro (NV_DETW_ANEL ja valia 4 e so era
          // usado no detalhe).
          float radius = radiusOf(w, h);
          if (f > 0.01f) {
            GfxRect border = { px - NV_RING_FOCUS, py - NV_RING_FOCUS,
                              w + NV_RING_FOCUS * 2, h + NV_RING_FOCUS * 2 };
            gfx_color(border, radius, 1.0f, 1.0f, 1.0f, f);
          }
          GfxRect card = { px, py, w, h };
          // CARD SEM ARTE: superficie solida, nao o vazio. Sem isto o card
          // ficava da cor do fundo — MEDIDO: #242429 sobre #252629, diferenca
          // de (1,2,0), contraste 1,0:1. Era literalmente invisivel, e foi a
          // origem da queixa "nao aparecem todos os posteres": eles apareciam,
          // do tom exato do fundo. A referencia desenha #2C2C2C na caixa exata.
          if (t) {
            // SEM PARALAXE OSCILANTE no card focado.
            //
            // Havia aqui um sen/cos do relogio deslocando a arte do card em
            // foco para sempre — um "respirar" estilo tvOS. A referencia NAO
            // tem isso, e a prova e direta: o screenrecord do aparelho so
            // escreve quadro quando algo muda na tela, e depois de a navegacao
            // assentar ele ficou 1,8 s e 2,4 s SEM EMITIR UM UNICO QUADRO, em
            // duas gravacoes diferentes. Tela parada de verdade, nao "quase".
            //
            // Alem de nao existir la, era o pior tipo de animacao para esta
            // GPU: obrigava a redesenhar a fileira inteira em todo quadro para
            // sempre, e o custo dominante aqui e fill rate.
            gfx_tex_aspect_current = tex_aspect(path);
            gfx_rect(card, t, GFX_CARD, f, 0.0f, 0.0f,
                     radius, 0, 0, 0, 1);
            gfx_tex_aspect_current = 0.0f;
          } else {
            // CARD SEM ARTE: superficie SOLIDA e visivel, nao o vazio.
            //
            // Aqui era #242429 (0.14,0.14,0.16) — MEDIDO contra o fundo que
            // havia entao, #252629: diferenca de (1,2,0), contraste 1,0:1. O
            // card existia e era literalmente invisivel, e essa foi a origem da
            // queixa "nao aparecem todos os posteres". Eles apareciam, do tom
            // exato do fundo, e o unico sinal era o anel de foco em volta de um
            // retangulo vazio — que le como QUEBRADO, nao como carregando.
            //
            // A referencia usa #2C2C2C sobre #0D0D0D: luminancia ~22x a do
            // fundo, impossivel nao ver.
            drawArtMissing(card, radius, cItem, 1.0f);
          }
          // SELO DE ASSISTIDO: disco branco com um "v" escuro, no canto
          // superior direito do poster. A referencia o tem e nos nao tinhamos
          // indicador nenhum na home — sem ele nao da para varrer uma fileira e
          // ver o que ja foi visto, que e o principal uso da tela.
          //
          // >= 90% e "visto", nao 100%: quase ninguem assiste os creditos, e o
          // proprio player ja arredonda para o fim quando falta menos de um
          // minuto (player_encerrar). Marcar so em 100% deixaria de fora
          // justamente o que acabou de ser assistido.
          if (cItem && cItem->progress >= 90 && kind != ROW_CONTINUE) {
            float d = w * 0.16f;                 // proporcional ao card
            float mx = px + w - d - 10.0f, my = py + 10.0f;
            GfxRect disk = { mx, my, d, d };
            gfx_color(disk, 0.5f, 1, 1, 1, 0.94f);
            // O "v" desenhado com dois tracos: o glifo da fonte nao serve aqui
            // porque precisaria de uma linha de texto so para isto, e o cache
            // de linhas tem 256 entradas disputadas pelos titulos.
            { float cx = mx + d * 0.5f, cy = my + d * 0.5f;
              float e = d * 0.085f;              // espessura
              GfxRect a1 = { cx - d * 0.20f, cy - e * 0.5f, d * 0.20f, e };
              GfxRect a2 = { cx - d * 0.02f, cy - e * 0.5f, d * 0.34f, e };
              gfx_rect(a1, 0, GFX_COLOR, 0, 0, 0, 0.5f, 0.07f, 0.07f, 0.07f, 0.94f);
              gfx_rect(a2, 0, GFX_COLOR, 0, 0, 0, 0.5f, 0.07f, 0.07f, 0.07f, 0.94f); }
          }

          // `cardDepthEnabled` mais o interruptor por secao: `cardDepthPosters`
          // nas fileiras de catalogo, `cardDepthContinueWatching` na primeira.
          drawDepth(card, radius,
                              kind == ROW_CONTINUE ? settings_depth_cw()
                                                       : settings_depth_posters());

          // --- posterLabelsEnabled ---------------------------------------
          // Card DEITADO: a legenda vai DENTRO da moldura, sobre um degrade que
          // cobre 54% da altura, com 14 de recuo lateral e 12 da base
          // (.home-poster-landscape-copy). Card EM PE: vai ABAIXO do poster, num
          // bloco de 74 de altura com 8 de padding no topo (.home-poster-copy).
          if (kind != ROW_CONTINUE && kind != ROW_RETURN && !editorial(kind) && settings_labels_poster() && cItem) {
            const char *name = cItem->title[0] ? cItem->title : NULL;
            const char *sub  = cItem->genre[0] ? cItem->genre : NULL;
            if (landscape && name) {
              GfxRect veil = { px, py + h * (1.0f - NV_LAND_VEIL), w, h * NV_LAND_VEIL };
              gfx_rect(veil, 0, GFX_VEIL, 0, 0, 0, radius, 0, 0, 0, 0.80f);
              float maxW = w * NV_LAND_COPY_MAXW;
              float bx = px + NV_LAND_COPY_DFLT;
              TxtLine tn = txt_line_trim(TXT_CAPTION, name, 245, 246, 250, 255, maxW);
              if (sub) {
                TxtLine ts = txt_line_trim(TXT_MINI, sub, 200, 202, 210, 255, maxW);
                txt_draw_alpha(ts, bx, py + h - NV_LAND_COPY_BASE - ts.h, 0.85f);
                txt_draw_alpha(tn, bx,
                                   py + h - NV_LAND_COPY_BASE - ts.h - 4.0f - tn.h, 0.98f);
              } else {
                txt_draw_alpha(tn, bx, py + h - NV_LAND_COPY_BASE - tn.h, 0.98f);
              }
            } else if (labelFora && name) {
              float bx = px + NV_POSTER_COPY_PADX;
              float by = py + h + NV_POSTER_COPY_PADT;
              float maxW = w - NV_POSTER_COPY_PADX * 2.0f;
              // 16/500 e 13/400 rgba(255,255,255,.7) — os corpos de
              // .home-poster-title e .home-poster-subtitle.
              TxtLine tn = txt_line_trim(TXT_CAPTION2, name, 245, 246, 250, 255, maxW);
              txt_draw_alpha(tn, bx, by, 0.98f);
              if (sub) {
                TxtLine ts = txt_line_trim(TXT_MINI, sub, 255, 255, 255, 255, maxW);
                txt_draw_alpha(ts, bx, by + tn.h + 2.0f, 0.70f);
              }
            }
          }

          if(kind==ROW_TOP10) {
            char rank[8];snprintf(rank,sizeof rank,"%d",c+1);
            TxtLine number=txt_line(TXT_RANK,rank,240,241,245,255);
            TxtLine ink=txt_line(TXT_RANK,rank,16,17,20,255);
            float nx=px-12,ny=py+h-number.h-8;
            for(int dx=-2;dx<=2;dx+=2)for(int dy=-2;dy<=2;dy+=2)
              txt_draw(number,nx+dx,ny+dy);
            txt_draw(ink,nx,ny);
          }

          if (kind == ROW_CONTINUE)
            resume_draw(cItem, (GfxRect){px, py, w, h});
          if (kind == ROW_RETURN)
            resume_draw(cItem, (GfxRect){px, py, w, h});

          // 4. DESTAQUE: titulo e metadados DENTRO da arte, sobre um veu
          // escuro na base — como o Apple TV faz. O titulo faz o papel do logo
          // embutido na arte-chave, que nos nao temos (o TMDB nem sempre tem
          // logo; quando tiver, entra aqui no lugar do texto).
          // CARD ABERTO: veu na base e o LOGO do titulo, como na TCL.
          //
          // O logo e nao o nome escrito com a fonte da interface: cada producao
          // tem tipografia propria, e escrever "The Pitt" em Inter apaga
          // justamente o que faz o titulo ser reconhecido de longe. Sem logo no
          // catalogo o card fica so com a arte — melhor que um nome generico
          // por cima dela.
          if (openAmt > 0.01f && cItem && cItem->logo[0]) {
            GLuint tl = tex_get(cItem->logo);
            if (tl) {
              float dflt = 34.0f * esc;
              float ap = tex_aspect(cItem->logo);
              float hL, wL, maxW;
              GfxRect veil = { px, py, w, h };
              gfx_rect(veil, 0, GFX_VEIL, 0, 0, 0, NV_RADIUS_CARD, 0, 0, 0, 0.72f * openAmt);
              // SEM CHUTE DE ASPECTO. O fallback de 4.0 que estava aqui
              // desenhava um retangulo mais largo que a imagem, e o modo de
              // cartao RECORTA o que sobra — o "REACHER" saia com as duas
              // pontas cortadas. Sem medida do arquivo, nao desenha.
              if (ap <= 0.0f) { tl = 0; }
              // Largura MANDA, altura sai dela: assim o retangulo tem sempre o
              // aspecto da imagem e o recorte nunca acontece.
              //
              // MEDIDO na TCL no card aberto: logo de 163 px num card de 565
              // (29% da largura) e 66 de altura num card de 320 (21%). O teto de
              // altura existe para logo quadrado nao virar um bloco.
              maxW = w * 0.30f;
              wL = maxW; hL = wL / ap;
              if (hL > h * 0.22f) { hL = h * 0.22f; wL = hL * ap; }
              // GFX_MARCA/GFX_TEXTO, NAO GFX_CARD. O modo de cartao e para
              // ARTE: ele faz cover com 3% de over-scan de proposito (a margem
              // de parallax) e descarta o alfa da textura. Num logo isso corta
              // as duas pontas — o "REACHER" saia como "EACHE" — e ainda pinta
              // de preto onde deveria ser transparente.
              //
              // O par certo ja existia no projeto, na fileira de destaque:
              // tex_marca_escura decide se a forma vem do alfa (logo claro) ou
              // do desenho (logo escuro). Reusado aqui em vez de reinventado.
              if (tl) { GfxRect rl = { px + dflt, py + h - dflt - hL, wL, hL };
                GfxMode m = tex_brand_dark(cItem->logo) ? GFX_BRAND : GFX_TEXT;
                gfx_tex_aspect_current = 0.0f;
                gfx_rect(rl, tl, m, 0, 0, 0, 0.0f, 1, 1, 1, openAmt); }
            }
          }

          if (editorial(kind)) {
            GfxRect veil = { px, py, w, h };
            gfx_rect(veil, 0, GFX_VEIL, 0, 0, 0, radius, 0, 0, 0, 0.88f);


            // Logo do titulo, como no aparelho: cada producao tem tipografia
            // propria, e escrever o nome com a fonte da interface apaga isso.
            const CatItem *ci = cItem;
            GLuint tlogo = (ci && ci->logo[0]) ? tex_get_width(ci->logo, w * .65f) : 0;
            // Sem dado, sem texto — nao a lista de demonstracao que ficava
            // aqui e carimbava nome e genero de outro titulo no card.
            const char *name   = (ci && ci->title[0]) ? ci->title : NULL;
            const char *genre = (ci && ci->genre[0]) ? ci->genre
                                : ci ? (!strcmp(ci->kind, "series") ? "Series" : "Film") : NULL;
            TxtLine tg = genre
                        ? txt_line_trim(TXT_HERO_META, genre, 226, 228, 233, 255, w - 64)
                        : (TxtLine){ 0, 0, 0 };

            float dflt = kind == ROW_HIGHLIGHT ? 28.0f : 22.0f;
            float base = py + h - dflt;
            float yMeta = base - tg.h;
            float hTitle;
            if (tlogo) {
              float ap = tex_aspect(ci->logo);
              if (ap <= 0.0f) ap = 4.0f;
              hTitle = h * .22f;
              float wTitle = hTitle * ap, maxW = w * .65f;
              if (wTitle > maxW) { wTitle = maxW; hTitle = wTitle / ap; }
              GfxRect rl = { px + dflt, yMeta - hTitle - 10.0f, wTitle, hTitle };
              gfx_tex_aspect_current = 0.0f;
              { GfxMode m = tex_brand_dark(ci->logo) ? GFX_BRAND : GFX_TEXT;
              gfx_rect(rl, tlogo, m, 0, 0, 0, 0.0f, 1, 1, 1, 1.0f); }
            } else if (name) {
              TxtLine tn = txt_line_trim(TXT_CW_TITLE, name, 245, 246, 249, 255, w - dflt*2);
              hTitle = (float)tn.h;
              txt_draw(tn, px + dflt, yMeta - hTitle - 10.0f);
            } else {
              hTitle = 0.0f;
            }
            if (genre) txt_draw(tg, px + dflt, yMeta);

            // Selo etario vermelho, a direita da linha de genero. SO COM VALOR:
            // o "16" de reserva que estava aqui carimbava uma faixa etaria em
            // todo card sem classificacao, e o selo vermelho tem cara de aviso
            // oficial — e o mesmo defeito do "14" cravado em descoberta.c, so
            // que na home.
            if (ci && ci->age_rating[0] && tg.w + 100 < w - dflt*2) {
              char cls[8];
              snprintf(cls, sizeof cls, "%s%s", ci->age_rating[0] == 'A' ? "" : "A", ci->age_rating);
              { TxtLine tb = txt_line(TXT_CAPTION, cls, 255, 255, 255, 255);
                float bx = px + dflt + tg.w + (genre ? 14.0f : 0.0f);
                GfxRect badge = { bx, yMeta + 2, tb.w + 16, tg.h - 4 };
                gfx_color(badge, NV_RADIUS_BADGE, 0.78f, 0.14f, 0.14f, 0.95f);
                txt_draw(tb, bx + 8, yMeta + 2); }
            }
          }

          // Feedback progressivo do gesto, sem duplicar o menu contextual. A
          // barra aparece somente enquanto o mesmo item esta sob pressao;
          // atingido o limiar, ctxmenu ja foi aberto e a soltura e consumida.
          if (okPressing && okHold > 0.0f &&
              focus_can_press_longa() && focus_index(&focus, r, c)) {
            float bx = px + NV_HOME_TEXT_GUTTER;
            float bw = w - NV_HOME_TEXT_GUTTER * 2.0f;
            GfxRect rail = { bx, py + h - 12.0f, bw, 4.0f };
            gfx_color(rail, 0.5f, 0.18f, 0.19f, 0.22f, 0.92f);
            gfx_color((GfxRect){ bx, rail.y, bw * okHold, rail.h },
                    0.5f, 0.92f, 0.93f, 0.96f, 1.0f);
            TxtLine hint = txt_line(TXT_MINI,
                                      okHold >= 1.0f ? "Release to open options"
                                                     : "Hold for options",
                                      225, 228, 235, 255);
            txt_draw_alpha(hint, bx, py + h - 38.0f, 0.92f);
          }
        }
      }
    }
    y += NV_LEGACY_ROW_HEAD_H + heightTotalOf(kind) + rowGap();
  }
  gfx_opacity_group=1;
  gfx_no_crop();
}

void home_shutdown(void) {}
void home_registrar_return(int index_, double posSeg, double durationSeg) {
  int new = -1;
  if (index_ >= 0 && durationSeg > 1.0) {
    double p = posSeg / durationSeg;
    if (p >= 0.01 && p < 0.90) new = index_;
  }
  if (new != resumeIndex) { resumeIndex = new; resumeRev++; }
  else if (new >= 0) resumeRev++; // atualiza barra/tempo da mesma sessao
  const CatItem *c = new >= 0 ? cat_item_exact(new) : NULL;
  snprintf(resumeId, sizeof resumeId, "%s", c ? c->imdb : "");
}
int home_wants_exit(void) { return sair; }

int home_item_focused(HomeItem *out) {
  if (!hasItemFocus) return 0;
  *out = itemFocus;
  return 1;
}

int home_n_arts(void) { return nBd; }
// Quando ha catalogo, a arte vem dele (na ordem certa, casada com o titulo);
// sem catalogo, cai na varredura da pasta.
const char *home_backdrop(int i) {
  const CatItem *c = cat_item_exact(i);
  return c && c->backdrop[0] ? c->backdrop : NULL;
}
const char *home_art(int i) { return (nBd && i >= 0 && i < nBd) ? bd[i] : NULL; }

// Consome o pedido de abrir: quem le, zera. Assim o OK vale uma vez so, mesmo
// que o quadro demore.
int home_requested_open(void) { int v = requestOpen; requestOpen = 0; return v; }

// Consome o pedido de abrir o menu lateral: quem le, zera.
int home_requested_menu(void) { int v = requestMenu; requestMenu = 0; return v; }
