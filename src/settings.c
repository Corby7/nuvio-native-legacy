// Ajustes: lista vertical em secoes, rotulo a esquerda e valor a direita.
//
// A regra que organiza a tela inteira: existem TRES naturezas de linha e elas
// TEM que parecer diferentes. Linha de escolha ganha foco e setas ao redor do
// valor; linha NUMERICA acrescenta uma barra de
// preenchimento sob o valor, porque "28%" sem barra nao diz onde fica no
// intervalo; linha so de leitura ganha um realce apagado, sem setas. Com o mesmo
// desenho nas tres, o usuario aperta esquerda e direita em cima da versao do app
// esperando que algo aconteca — foi por isso que a distincao virou requisito.
//
// As chaves e os agrupamentos seguem a tela de Layout do app web
// (js/ui/screens/settings/settingsScreen.js), inclusive os rotulos em portugues
// lidos da tela rodando.
#include "settings.h"
#include "gfx.h"
#include "text.h"
#include "tex_cache.h"
#include "anim.h"
#include "layout.h"
#include "session.h"
#include "sync.h"
#include "profiles.h"
#include "traktauth.h"
#include "simklauth.h"
#include "js.h"
#include <stdio.h>
#include <string.h>

// Versao do app: mesma string do appinfo.json empacotado. Fica aqui porque a
// tela nao tem como ler o manifesto em tempo de execucao no aparelho.
#define SETTING_VERSION       "1.0.1"

#define SETTING_LINE_H       88.0f
#define SETTING_LINE_GAP      8.0f
#define SETTING_SEC_GAP       46.0f    // fim de uma secao ao cabecalho da proxima
#define SETTING_SEC_HEADER     44.0f    // altura reservada ao cabecalho da secao
// Nao e constante: acompanha a rail, como todo o resto do conteudo. Com a
// barra recolhida a lista tambem comeca em 104 — deixar 248 cravado aqui fazia
// a tela de Ajustes ser a unica desalinhada das outras.
#define SETTING_LIST_X      settings_content_x()
#define SETTING_LIST_W     1120.0f
#define SETTING_DFLT           34.0f    // borda da linha ao texto
#define SETTING_TOP        (NV_MARGIN_Y + 118.0f)   // abaixo do titulo da tela
#define SETTING_BASE        (NV_SCREEN_H - NV_MARGIN_Y - 48.0f)
// Raio da linha em fracao do menor lado (o SDF do shader e normalizado):
// 12px sobre 88 de altura.
#define SETTING_RADIUS           0.14f

// Ordem do enum = ordem no arquivo de chaves e nas tabelas. Acrescentar no MEIO
// e seguro: o arquivo e por chave, nao posicional (ver ajustes_dir).
typedef enum {
  // Reproducao
  SETTING_QUALITY, SETTING_DV, SETTING_ATMOS,
  // Layout da Home
  SETTING_LANDSCAPE, SETTING_HERO_FULL,
  // Conteudo da Home
  SETTING_RAIL, SETTING_RAIL_MODERN, SETTING_RAIL_BLUR, SETTING_HERO, SETTING_HERO_CATALOGS,
  SETTING_DISCOVER, SETTING_LABELS, SETTING_NAME_ADDON, SETTING_SUFFIX_KIND,
  SETTING_HIDE_UNRELEASED, SETTING_SCORES_HOME, SETTING_GRADIENT_CLASSIC,
  // Continuar assistindo
  SETTING_CW_ON, SETTING_CW_STYLE, SETTING_CW_THUMB, SETTING_CW_BLUR_NEXT,
  SETTING_CW_FURTHEST, SETTING_CW_NOT_SHOWN, SETTING_CW_ORDER,
  // Pagina de detalhe
  SETTING_DET_BLUR_NOT_WATCHED, SETTING_DET_TRAILER, SETTING_DET_META_EXT, SETTING_DET_DATE_FULL,
  // Foco no poster
  SETTING_EXPAND, SETTING_EXPAND_DELAY, SETTING_NAV_FAST,
  // Profundidade
  SETTING_DEPTH, SETTING_DEPTH_BORDER, SETTING_DEPTH_BRIGHTNESS, SETTING_DEPTH_COVERAGE,
  SETTING_DEPTH_POSTERS, SETTING_DEPTH_CW, SETTING_DEPTH_EPS, SETTING_DEPTH_CAST, SETTING_DEPTH_TRAILERS,
  // Tamanho do item
  SETTING_WIDTH_DP, SETTING_RADIUS_DP,
  // Interface
  SETTING_ANIM,
  // Conta
  SETTING_PROFILE_ACTIVE, SETTING_SYNC, SETTING_TRAKT, SETTING_SIMKL, SETTING_EXIT,
  // Sobre
  SETTING_VERSION_I, SETTING_SPACE,
  SETTING_N
} OptionId;

static const char *V_QUALITY[] = { "Automatic", "4K", "1080p", "720p" };
static const char *V_ON[]      = { "On", "Off" };
static const char *V_ANIM[]      = { "Full", "Reduced" };
// `collapseSidebar`: recolhida = a rail some e o conteudo comeca em 104.
static const char *V_RAIL[]      = { "Collapsed", "Fixed" };
// `continueWatchingCardStyle`, validado em layoutPreferences.js contra
// exatamente estes tres valores.
static const char *V_CW[]        = { "Card", "Wide", "Poster" };
// `continueWatchingSortMode`, normalizado em normalizeContinueWatchingSortMode.
static const char *V_CW_ORDER[]  = { "Default", "Streaming style", "Separate upcoming" };
// `discoverLocation`, validado contra estes tres.
static const char *V_DISCOVER[] = { "Show in Search", "In the sidebar", "Off" };
// `homeImdbRatingsVisibility` — normalizeHomeImdbRatingsVisibility so aceita
// SHOW_ALL e HIDE_ALL.
static const char *V_SCORES[]     = { "Show", "Hide" };

// Natureza da linha.
// OP_ACAO responde ao OK, nao a esquerda/direita. Ela NAO e leitura: uma linha
// que faz alguma coisa tem de ter o mesmo destaque de quem muda valor, senao o
// usuario aperta OK esperando que nada aconteca.
typedef enum { OP_CHOICE, OP_NUMBER, OP_READ, OP_ACTION } OptionKind;

typedef struct {
  const char  *label;
  OptionKind    kind;
  const char **values;   // OP_ESCOLHA
  int          n;         // OP_ESCOLHA: quantos valores
  int          min, max, step;   // OP_NUMERO
  const char  *suffix;            // OP_NUMERO: "%", "s", "dp"
} Option;

#define ESC(rot, vals, count) { rot, OP_CHOICE, vals, count, 0, 0, 0, NULL }
#define NUM(rot, lo, hi, st, suffix) { rot, OP_NUMBER, NULL, 0, lo, hi, st, suffix }
#define READ(rot)            { rot, OP_READ, NULL, 0, 0, 0, 0, NULL }
#define ACTION(rot)           { rot, OP_ACTION,    NULL, 0, 0, 0, 0, NULL }

static const Option OPTIONS[SETTING_N] = {
  ESC("Maximum quality",           V_QUALITY, 4),
  ESC("Dolby Vision",               V_ON, 2),
  ESC("Dolby Atmos",                V_ON, 2),

  ESC("Landscape posters",       V_ON, 2),   // modernLandscapePostersEnabled
  ESC("Full-screen backdrop",        V_ON, 2),   // modernHeroFullScreenBackdropEnabled

  ESC("Sidebar",              V_RAIL, 2),   // collapseSidebar
  ESC("Modern sidebar",      V_ON, 2),   // modernSidebar
  ESC("Modern sidebar blur",  V_ON, 2),   // modernSidebarBlur
  ESC("Show hero",           V_ON, 2),   // heroSectionEnabled
  READ("Hero catalogues"),                   // heroCatalogKeys (contagem)
  ESC("Discover location",         V_DISCOVER, 3), // discoverLocation
  ESC("Poster labels",       V_ON, 2),   // posterLabelsEnabled
  ESC("Addon name in the catalogue",  V_ON, 2),   // catalogAddonNameEnabled
  ESC("Content type",           V_ON, 2),   // catalogTypeSuffixEnabled
  ESC("Hide unreleased",       V_ON, 2),   // hideUnreleasedContent
  ESC("Overall ratings",          V_SCORES, 2),  // homeImdbRatingsVisibility
  ESC("Classic focus gradient", V_ON, 2),   // classicFocusGradientEnabled

  ESC("Show \"Continue watching\"", V_ON, 2), // continueWatchingEnabled
  ESC("\"Continue watching\" style", V_CW, 3), // continueWatchingCardStyle
  ESC("Episode thumbnail",      V_ON, 2),   // useEpisodeThumbnailsInCw
  ESC("Blur next episode",  V_ON, 2),   // blurContinueWatchingNextUp
  ESC("Next from the furthest episode", V_ON, 2),// nextUpFromFurthestEpisode
  ESC("Show unaired episodes", V_ON, 2),// showUnairedNextUp
  ESC("Sort order",                  V_CW_ORDER, 3), // continueWatchingSortMode

  ESC("Blur unwatched",    V_ON, 2),   // blurUnwatchedEpisodes
  ESC("Trailer button",           V_ON, 2),   // detailPageTrailerButtonEnabled
  ESC("Prefer external metadata", V_ON, 2), // preferExternalMetaAddonDetail
  ESC("Full release date", V_ON, 2),  // showFullReleaseDate

  ESC("Expand poster on focus",   V_ON, 2),   // focusedPosterBackdropExpandEnabled
  NUM("Expansion delay",         0, 10, 1, " s"), // ...ExpandDelaySeconds
  ESC("Fast horizontal navigation", V_ON, 2),  // fastHorizontalNavigationEnabled

  ESC("Depth effect",     V_ON, 2),   // cardDepthEnabled
  NUM("Edge brightness",            0, 100, 2, "%"), // cardDepthEdgeStrength
  NUM("Sheen",                      0, 100, 2, "%"), // cardDepthSheenStrength
  NUM("Edge coverage",         0, 100, 2, "%"), // cardDepthEdgeCoverage
  ESC("Depth on posters",  V_ON, 2),
  ESC("Depth on \"Continue\"", V_ON, 2),
  ESC("Depth on episodes", V_ON, 2),
  ESC("Depth on cast",     V_ON, 2),
  ESC("Depth on trailers",  V_ON, 2),

  NUM("Item width",            72, 200, 2, " dp"), // posterCardWidthDp
  NUM("Corner radius",              0, 40, 1, " dp"),   // posterCardCornerRadiusDp

  ESC("Animations",                  V_ANIM, 2),

  READ("Profile"),
  READ("Sync"),
  ACTION("Trakt"),
  ACTION("Simkl"),
  ACTION("Sign out"),
  READ("Version"),
  READ("Memory used by images"),
};

// Nome de cada opcao no arquivo. O formato era POSICIONAL — uma linha por
// opcao, na ordem do enum — e por isso acrescentar uma opcao no meio fazia o
// arquivo de quem ja tinha o app aplicar os valores errados, em silencio. Com
// chave por linha, opcao nova nasce no padrao e as antigas continuam onde
// estavam. Os nomes seguem os do app web onde existe correspondente.
static const char *KEY[] = {
  "quality", "dolbyVision", "dolbyAtmos",
  "modernLandscapePostersEnabled", "modernHeroFullScreenBackdropEnabled",
  "collapseSidebar", "modernSidebar", "modernSidebarBlur",
  "heroSectionEnabled", "-heroCatalogKeys",
  "discoverLocation", "posterLabelsEnabled", "catalogAddonNameEnabled",
  "catalogTypeSuffixEnabled", "hideUnreleasedContent",
  "homeImdbRatingsVisibility", "classicFocusGradientEnabled",
  "continueWatchingEnabled", "continueWatchingCardStyle",
  "useEpisodeThumbnailsInCw", "blurContinueWatchingNextUp",
  "nextUpFromFurthestEpisode", "showUnairedNextUp", "continueWatchingSortMode",
  "blurUnwatchedEpisodes", "detailPageTrailerButtonEnabled",
  "preferExternalMetaAddonDetail", "showFullReleaseDate",
  "focusedPosterBackdropExpandEnabled", "focusedPosterBackdropExpandDelaySeconds",
  "fastHorizontalNavigationEnabled",
  "cardDepthEnabled", "cardDepthEdgeStrength", "cardDepthSheenStrength",
  "cardDepthEdgeCoverage", "cardDepthPostersEnabled",
  "cardDepthContinueWatchingEnabled", "cardDepthEpisodeCardsEnabled",
  "cardDepthCastEnabled", "cardDepthTrailersEnabled",
  "posterCardWidthDp", "posterCardCornerRadiusDp",
  "reducedAnimations",
  // Conta: sao linhas locais, nao vem nem vao para o perfil na nuvem.
  "-profile", "-sync", "-trakt", "-simkl", "-exit",
  "-version", "-space",
};

// O compilador CONFERE que ha uma chave por opcao. Sem isto, acrescentar uma
// opcao no enum e esquecer a chave deixa as ultimas entradas em NULL e
// DESALINHA todas as chaves depois do ponto de insercao — e o defeito nao
// aparece na hora: so quando o ajustes.txt passa a existir, o strcmp(NULL,...)
// derruba o app no arranque seguinte. Foi exatamente o que aconteceu, e o
// unico sintoma na TV foi o app abrir e fechar.
typedef char checked_one_key_per_option[
  (sizeof KEY / sizeof *KEY == SETTING_N) ? 1 : -1];

// Onde cada secao comeca e quantas opcoes ela tem. Secao e um agrupamento
// visual, nao um nivel de navegacao: cima/baixo atravessa os cabecalhos sem
// parar neles, como no aparelho. Os titulos sao os do app web.
static const struct { const char *title; int start, n; } SECTIONS[] = {
  { "Playback",                     SETTING_QUALITY,           3 },
  { "Home layout",                    SETTING_LANDSCAPE,           2 },
  { "Home content",               SETTING_RAIL,               12 },
  { "Continue watching",           SETTING_CW_ON,           7 },
  { "Detail page",             SETTING_DET_BLUR_NOT_WATCHED, 4 },
  { "Poster focus",                 SETTING_EXPAND,            3 },
  { "Depth effect",         SETTING_DEPTH,                9 },
  { "Item size",              SETTING_WIDTH_DP,          2 },
  { "Interface",                      SETTING_ANIM,                  1 },
  { "Account",                          SETTING_PROFILE_ACTIVE,        5 },
  { "About",                          SETTING_VERSION_I,            2 },
};
#define SETTING_N_SECTIONS (int)(sizeof SECTIONS / sizeof *SECTIONS)

// Valor de cada opcao. Para OP_ESCOLHA e o indice; para OP_NUMERO e o proprio
// numero. Os padroes sao os DEFAULTS de layoutPreferences.js, com UMA excecao
// anotada linha a linha: as quatro que o perfil do dono diverge de fabrica
// nascem como ele as deixou, porque e o que ele ve hoje. Todas sao trocaveis
// aqui, que era o ponto.
static int value[SETTING_N] = {
  0, 0, 0,          /* qualidade, DV, Atmos */

  0,                /* posteres deitados: LIGADO (perfil do dono; fabrica: desligado) */
  0,                /* fundo em tela cheia: LIGADO (perfil; fabrica: desligado) */

  0,                /* barra lateral: recolhida (perfil; fabrica: fixa) */
  1,                /* barra lateral moderna: desligada */
  0,                /* desfoque da barra moderna: ligado (perfil) */
  0,                /* mostrar destaque: ligado */
  0,                /* catalogos do destaque: leitura */
  0,                /* local do descobrir: na busca */
  // DESLIGADO por padrao: o cartaz ja traz o titulo impresso na arte, e repetir
  // o nome logo abaixo e a mesma informacao duas vezes ocupando altura de
  // fileira. Continua sendo ajuste — quem quiser o rotulo liga em Ajustes.
  1,                /* rotulos nos posteres: desligado */
  0,                /* nome do addon: ligado */
  0,                /* tipo de conteudo: ligado */
  1,                /* ocultar nao lancados: desligado */
  0,                /* avaliacoes gerais: mostrar (SHOW_ALL) */
  1,                /* gradiente de foco classico: desligado */

  0,                /* continuar assistindo: ligado */
  0,                /* estilo: card */
  0,                /* miniatura do episodio: ligada */
  1,                /* desfocar proximo: desligado */
  0,                /* proximo do episodio mais alto: ligado */
  0,                /* mostrar nao exibidos: ligado */
  0,                /* ordenacao: padrao */

  1,                /* desfocar nao assistidos: desligado */
  0,                /* botao de trailer: ligado */
  0,                /* metadados externos: ligado */
  0,                /* data completa: ligada */

  0,                /* expandir poster ao focar: ligado (DEFAULT do web) */
  3,                /* atraso: 3s */
  1,                /* navegacao horizontal rapida: desligada (fabrica) */

  1,                /* efeito de profundidade: desligado (fabrica) */
  28,               /* brilho da borda */
  10,               /* reflexo */
  0,                /* cobertura da borda */
  0, 0, 0, 0, 0,    /* profundidade em posters, cw, episodios, elenco, trailers */

  126,              /* largura do item, dp (fabrica; o perfil do dono usa 120) */
  12,               /* arredondamento, dp */

  0, 0,             /* idioma, animacoes */
  0, 0,             /* versao, espaco */
};

static int focusOp = 0;
// Uma lista de UMA coluna nao precisa do focus.h: a memoria de coluna que ele
// existe para resolver nao tem o que lembrar aqui, e o indice cru deixa o
// "pula o cabecalho da secao" ser uma soma em vez de um mapa de fileiras.
static float animFocus[SETTING_N];
static float scrollY = 0.0f;
static int sair = 0;

// Quantos catalogos o destaque usa. 0 = todos, que e o que o web escreve como
// "Todos" quando heroCatalogKeys esta vazio — e o caso do perfil do dono.
static int heroCatalogs = 0;

static int on(int op)  { return value[op] == 0; }

int settings_animations_reduced(void) { return value[SETTING_ANIM] == 1; }
int settings_dolby_vision(void)        { return on(SETTING_DV); }
int settings_dolby_atmos(void)         { return on(SETTING_ATMOS); }

// `collapseSidebar: modernSidebar ? false : Boolean(collapseSidebar)` — a barra
// moderna DESLIGA o recolhimento, e nao o contrario. Copiado de
// normalizeLayoutPreferences para nao inventar precedencia.
int settings_rail_modern(void)        { return on(SETTING_RAIL_MODERN); }
int settings_rail_collapsed(void)      { return settings_rail_modern() ? 0 : on(SETTING_RAIL); }
int settings_rail_modern_blur(void)   { return on(SETTING_RAIL_BLUR); }
int settings_hero_on(void)         { return on(SETTING_HERO); }
int settings_hero_full(void)          { return on(SETTING_HERO_FULL); }
int settings_posters_landscape(void)   { return on(SETTING_LANDSCAPE); }
int settings_gradient_focus_classic(void) { return on(SETTING_GRADIENT_CLASSIC); }

int settings_labels_poster(void)      { return on(SETTING_LABELS); }
int settings_name_addon(void)          { return on(SETTING_NAME_ADDON); }
int settings_suffix_kind(void)         { return on(SETTING_SUFFIX_KIND); }
int settings_hide_unreleased(void){ return on(SETTING_HIDE_UNRELEASED); }
int settings_date_full(void)       { return on(SETTING_DET_DATE_FULL); }
int settings_scores_home(void)          { return value[SETTING_SCORES_HOME] == 0; }
int settings_local_discover(void)     { return value[SETTING_DISCOVER]; }
int settings_discover_na_search(void)  { return value[SETTING_DISCOVER] == 0; }

int settings_cw_on(void)           { return on(SETTING_CW_ON); }
int settings_cw_style(void)           { return value[SETTING_CW_STYLE]; }
int settings_cw_thumb_episode(void)   { return on(SETTING_CW_THUMB); }
int settings_cw_blur_next(void) { return on(SETTING_CW_BLUR_NEXT); }
int settings_cw_do_episode_more_alto(void) { return on(SETTING_CW_FURTHEST); }
int settings_cw_show_unaired(void)  { return on(SETTING_CW_NOT_SHOWN); }
int settings_cw_order(void)            { return value[SETTING_CW_ORDER]; }

int settings_blur_unwatched(void) { return on(SETTING_DET_BLUR_NOT_WATCHED); }
int settings_button_trailer(void)       { return on(SETTING_DET_TRAILER); }
int settings_meta_external(void)        { return on(SETTING_DET_META_EXT); }

int   settings_expand_poster(void)   { return on(SETTING_EXPAND); }
float settings_expand_poster_delay(void) { return (float)value[SETTING_EXPAND_DELAY]; }
int   settings_navigation_horizontal_fast(void) { return on(SETTING_NAV_FAST); }

int   settings_depth(void)      { return on(SETTING_DEPTH); }
float settings_depth_border(void)     { return value[SETTING_DEPTH_BORDER] / 100.0f; }
float settings_depth_brightness(void)    { return value[SETTING_DEPTH_BRIGHTNESS] / 100.0f; }
float settings_depth_coverage(void) { return value[SETTING_DEPTH_COVERAGE] / 100.0f; }
int   settings_depth_posters(void)   { return on(SETTING_DEPTH_POSTERS); }
int   settings_depth_cw(void)        { return on(SETTING_DEPTH_CW); }
int   settings_depth_episodes(void) { return on(SETTING_DEPTH_EPS); }
int   settings_depth_cast(void)    { return on(SETTING_DEPTH_CAST); }
int   settings_depth_trailers(void)  { return on(SETTING_DEPTH_TRAILERS); }

int   settings_width_poster_dp(void) { return value[SETTING_WIDTH_DP]; }
int   settings_radius_poster_dp(void)    { return value[SETTING_RADIUS_DP]; }
// dpToPx = 2 em buildModernHomeSizingStyle. 12dp -> 24px, que e o raio medido.
float settings_radius_poster_px(void)    { return (float)value[SETTING_RADIUS_DP] * 2.0f; }

// A regra do web, e nao dois layouts: o conteudo tem sempre 104 de recuo e a
// rail acrescenta os 144 dela quando esta fixa.
float settings_content_x(void) {
  return settings_rail_collapsed() ? NV_CONTENT_DFLT
                                  : NV_LEGACY_RAIL_W + NV_CONTENT_DFLT;
}
const char *settings_quality(void)   { return V_QUALITY[value[SETTING_QUALITY]]; }

// Onde os ajustes ficam. Ate a versao anterior nada era gravado: mexer numa
// opcao valia so enquanto o app estivesse aberto, e voltar depois mostrava tudo
// no padrao — o que faz a tela inteira parecer decorativa.
static char dirSettings[512];


// Valores LITERAIS que o app web grava nas opcoes que nao sao booleanas. A
// ordem casa, uma a uma, com a do vetor de rotulos correspondente — e essa
// correspondencia e o contrato: mexer num vetor sem mexer no outro troca o
// ajuste da pessoa em silencio. Todos conferidos no codigo do app web.
static const char *W_DISCOVER[] = { "in_search", "in_sidebar", "off", NULL };
static const char *W_SCORES[]     = { "SHOW_ALL", "HIDE_ALL", NULL };
static const char *W_CW[]        = { "card", "wide", "poster", NULL };
static const char *W_CW_ORDER[]  = { "default", "streaming_style", "split_upcoming", NULL };

// `heroSectionEnabled` -> `hero_section_enabled`. Uma sequencia de maiusculas
// conta como uma palavra so (`homeImdbRatingsVisibility` ->
// `home_imdb_ratings_visibility`, e nao `home_i_m_d_b_...`).
static void camelToSnake(const char *src, char *dst, size_t size) {
  size_t w = 0;
  int i;
  for (i = 0; src[i] && w + 2 < size; i++) {
    int alto = src[i] >= 'A' && src[i] <= 'Z';
    if (alto && w > 0) {
      int previousBottom = src[i - 1] >= 'a' && src[i - 1] <= 'z';
      int previousDigit = src[i - 1] >= '0' && src[i - 1] <= '9';
      int nextBottom = src[i + 1] >= 'a' && src[i + 1] <= 'z';
      if (previousBottom || previousDigit || nextBottom) dst[w++] = '_';
    }
    dst[w++] = alto ? (char)(src[i] - 'A' + 'a') : src[i];
  }
  dst[w] = 0;
}

static int equalWithoutBox(const char *a, const char *b) {
  for (; *a && *b; a++, b++) {
    char x = (*a >= 'A' && *a <= 'Z') ? (char)(*a - 'A' + 'a') : *a;
    char y = (*b >= 'A' && *b <= 'Z') ? (char)(*b - 'A' + 'a') : *b;
    if (x != y) return 1;
  }
  return *a || *b;   // 0 quando iguais, como strcmp
}

static const char *const *literalsOf(int op) {
  switch (op) {
    case SETTING_DISCOVER:  return W_DISCOVER;
    case SETTING_SCORES_HOME: return W_SCORES;
    case SETTING_CW_STYLE:  return W_CW;
    case SETTING_CW_ORDER:   return W_CW_ORDER;
    default:            return NULL;
  }
}

static int limits(int op, int v) {
  const Option *o = &OPTIONS[op];
  if (o->kind == OP_CHOICE) return (v >= 0 && v < o->n) ? v : value[op];
  if (o->kind == OP_NUMBER)  return v < o->min ? o->min : (v > o->max ? o->max : v);
  return value[op];
}

void settings_dir(const char *dir) {
  FILE *f;
  char path[600], line[96];
  if (!dir || !*dir) return;
  snprintf(dirSettings, sizeof dirSettings, "%s", dir);
  snprintf(path, sizeof path, "%s/settings.txt", dirSettings);
  f = fopen(path, "r");
  if (!f) return;
  while (fgets(line, sizeof line, f)) {
    char key[64]; int v, i;
    if (sscanf(line, "%63s %d", key, &v) != 2) continue;
    for (i = 0; i < SETTING_N; i++) {
      if (!KEY[i] || strcmp(KEY[i], key)) continue;
      if (OPTIONS[i].kind == OP_READ || OPTIONS[i].kind == OP_ACTION) continue;
      // Valor fora da faixa (arquivo de outra versao, ou editado a mao) cai no
      // padrao em vez de indexar fora do vetor.
      value[i] = limits(i, v);
      break;
    }
  }
  fclose(f);
}

static void save(void) {
  char path[600], tmp[600];
  FILE *f;
  int i;
  if (!dirSettings[0]) return;
  snprintf(path, sizeof path, "%s/settings.txt", dirSettings);
  snprintf(tmp, sizeof tmp, "%s/settings.tmp", dirSettings);
  f = fopen(tmp, "w");
  if (!f) return;
  for (i = 0; i < SETTING_N; i++) {
    // "-" marca linha local (versao, espaco, conta): nao tem valor para
    // guardar. Acao tambem nao. E chave ausente NUNCA vai para o arquivo — foi
    // um "(null) 0" gravado assim que derrubou o app na leitura seguinte.
    if (!KEY[i] || KEY[i][0] == '-') continue;
    if (OPTIONS[i].kind == OP_READ || OPTIONS[i].kind == OP_ACTION) continue;
    fprintf(f, "%s %d\n", KEY[i], value[i]);
  }
  fclose(f);
  rename(tmp, path);
}


int settings_apply_blob(const char *json) {
  const char *end;
  int i, changed = 0, recognized = 0;
  if (!json || !*json) return 0;
  end = json + strlen(json);

  for (i = 0; i < SETTING_N; i++) {
    char snake[80], wrapper[400], raw[160];
    int new;
    // Linha de leitura/acao nao tem valor; chave com "-" e marcador local
    // (heroCatalogKeys, versao, espaco) e nao vem do blob.
    if (OPTIONS[i].kind == OP_READ || OPTIONS[i].kind == OP_ACTION) continue;
    if (!KEY[i] || KEY[i][0] == '-') continue;
    // MEDIDO na TV, com uma conta de verdade: o blob NAO e um mapa plano de
    // camelCase. Ele e
    //   {"version":1,"features":{"layout_settings":{
    //      "hero_section_enabled":{"type":"boolean","value":true}, ...}}}
    // — chave em snake_case, aninhada por "feature", e o valor EMBRULHADO num
    // objeto com tipo. Procurando por `heroSectionEnabled` o app achava zero
    // chaves em 12 KB de ajustes e nao aplicava nada, sem erro nenhum.
    //
    // A busca por nome ignora o aninhamento de proposito: js_bruto varre o
    // texto inteiro, e os nomes destas chaves sao unicos no documento.
    camelToSnake(KEY[i], snake, sizeof snake);
    if (!js_raw(json, end, snake, wrapper, sizeof wrapper) &&
        !js_raw(json, end, KEY[i], wrapper, sizeof wrapper)) continue;
    if (wrapper[0] == '{') {
      // Desembrulha {"type":...,"value":X}. O `type` vem antes do `value` no
      // codificador do web, entao a primeira chave "value" e a certa.
      if (!js_raw(wrapper, wrapper + strlen(wrapper), "value",
                    raw, sizeof raw)) continue;
    } else {
      snprintf(raw, sizeof raw, "%s", wrapper);
    }

    if (!strcmp(raw, "true") || !strcmp(raw, "false")) {
      // O primeiro rotulo de V_LIGA e "Ligado" e o de V_RAIL e "Recolhida" —
      // nos dois, o indice 0 e o `true` do web. Coincidencia util, mas
      // coincidencia: se um vetor novo comecar pelo estado desligado, ele
      // precisa de literais proprios em literaisDe().
      new = !strcmp(raw, "true") ? 0 : 1;
    } else if (raw[0] == '"') {
      const char *const *lit = literalsOf(i);
      char text[128];
      size_t n = strlen(raw);
      if (n < 2) continue;
      if (n - 2 >= sizeof text) continue;
      memcpy(text, raw + 1, n - 2);
      text[n - 2] = 0;
      new = -1;
      // Comparacao SEM CAIXA. MEDIDO na TV: o servidor guarda estes enums em
      // MAIUSCULA ("IN_SEARCH", "CARD", "DEFAULT") enquanto o codigo JS do app
      // web os escreve em minuscula. Ler so o codigo do web levava a rejeitar
      // o valor de verdade — e a rejeicao era CORRETA (melhor manter que
      // inventar), mas o efeito era o ajuste nunca chegar.
      if (lit) { int k; for (k = 0; lit[k]; k++) if (!equalWithoutBox(lit[k], text)) { new = k; break; } }
      if (new < 0) {
        // Valor que este app nao conhece (versao nova do web, opcao nova).
        // Manter o que esta e a resposta certa: escolher um padrao aqui
        // inventaria uma preferencia que a pessoa nunca marcou.
        printf("[settings] %s=\"%s\" not recognised; kept\n", KEY[i], text);
        continue;
      }
    } else if ((raw[0] >= '0' && raw[0] <= '9') || raw[0] == '-' || raw[0] == '.') {
      new = (int)(atof(raw) + 0.5);
    } else {
      continue;   // null, objeto, array: nao ha o que aplicar
    }

    recognized++;
    new = limits(i, new);
    if (new != value[i]) { value[i] = new; changed++; }
  }

  if (changed) save();   // o que veio da conta tem de sobreviver ao arranque
  // Registra SEMPRE, inclusive zero. "Nenhuma linha no log" tem duas leituras
  // opostas — o blob nao foi aplicado, ou foi aplicado e ja estava tudo igual —
  // e sem o numero nao da para saber qual. Foi exatamente a duvida que sobrou
  // na primeira verificacao na TV.
  printf("[settings] account blob: %d key(s) recognised, %d changed\n",
         recognized, changed);
  return changed;
}

int settings_start(void) { focusOp = 0; scrollY = 0.0f; sair = 0; return 1; }
void settings_shutdown(void) { }
int settings_wants_exit(void) { return sair; }

// Valor das linhas so de leitura. O espaco em disco NAO e um numero inventado:
// vem do cache de texturas, que e exatamente o que "imagens" consome no
// aparelho — um numero fixo aqui seria mentira e nunca mudaria.
static const char *textRead(int op) {
  static char buf[64];
  if (op == SETTING_VERSION_I) return SETTING_VERSION;
  if (op == SETTING_PROFILE_ACTIVE) {
    static char bufp[80];
    int i;
    for (i = 0; i < profiles_n(); i++)
      if (profiles_item(i)->index_ == profiles_active()) return profiles_item(i)->name;
    // Sem lista de perfis, dizer "Perfil 1" e mais honesto que deixar vazio: e
    // literalmente o que o app esta usando em p_profile_id.
    snprintf(bufp, sizeof bufp, "Profile %d", profiles_active());
    return bufp;
  }
  if (op == SETTING_SYNC) {
    switch (sync_state()) {
      case SYNC_RUNNING: return "sincronizando…";
      case SYNC_FAILED:  return "failed";
      case SYNC_READY:  return sync_summary();
      default:           return session_loggedin() ? "waiting" : "no account";
    }
  }
  if (op == SETTING_TRAKT) {
    switch (traktauth_state()) {
      case TRA_ON:     return "conectado";
      case TRA_REQUESTING:    return "preparando…";
      case TRA_WAITING: return "waiting";
      case TRA_ERROR:       return "failed";
      default:             return "connect";
    }
  }
  if (op == SETTING_SIMKL) {
    switch (simklauth_state()) {
      case SMK_ON:     return "conectado";
      case SMK_REQUESTING:    return "preparando…";
      case SMK_WAITING: return "waiting";
      case SMK_ERROR:       return "failed";
      default:             return "connect";
    }
  }
  if (op == SETTING_EXIT) return "OK";
  if (op == SETTING_HERO_CATALOGS) {
    // "Todos" com a lista vazia e o que o web escreve (common_all), e e o estado
    // do perfil do dono. Um "0" ali leria como "nenhum", o oposto do que e.
    if (heroCatalogs <= 0) return "All";
    snprintf(buf, sizeof buf, "%d", heroCatalogs);
    return buf;
  }
  int items = 0, pending = 0; long bytes = 0;
  tex_stats(&items, &pending, &bytes);
  snprintf(buf, sizeof buf, "%.1f MB across %d images", bytes / 1048576.0, items);
  return buf;
}

// Uma opcao pode ficar INATIVA por causa de outra — o web esconde a linha
// (`model.layout.modernSidebar ? "" : renderToggleRow(...)`), mas esconder num
// D-pad muda a contagem de linhas embaixo do dedo do usuario a cada toque. Aqui
// ela continua no lugar, apagada e sem setas: a dependencia fica visivel em vez
// de a linha sumir.
static int inactive(int op) {
  switch (op) {
    case SETTING_RAIL:         return settings_rail_modern();
    case SETTING_RAIL_BLUR:    return !settings_rail_modern();
    case SETTING_HERO_CATALOGS: return !settings_hero_on();
    case SETTING_CW_STYLE: case SETTING_CW_THUMB: case SETTING_CW_FURTHEST:
    case SETTING_CW_NOT_SHOWN: case SETTING_CW_ORDER:
      return !settings_cw_on();
    case SETTING_CW_BLUR_NEXT: return !settings_cw_on() || !settings_cw_thumb_episode();
    case SETTING_EXPAND_DELAY: return !settings_expand_poster();
    case SETTING_DEPTH_BORDER: case SETTING_DEPTH_BRIGHTNESS: case SETTING_DEPTH_COVERAGE:
    case SETTING_DEPTH_POSTERS: case SETTING_DEPTH_CW: case SETTING_DEPTH_EPS:
    case SETTING_DEPTH_CAST: case SETTING_DEPTH_TRAILERS:
      return !settings_depth();
    default: return 0;
  }
}

// Acao NAO e leitura (tem o destaque de linha ativa), mas tambem NAO e mutavel
// (esquerda/direita nao fazem nada nela). As duas respostas sao diferentes de
// proposito, e e por isso que sao duas funcoes.
static int soRead(int op) { return OPTIONS[op].kind == OP_READ; }
static int mutable(int op)   { return OPTIONS[op].kind != OP_READ &&
                                      OPTIONS[op].kind != OP_ACTION && !inactive(op); }

static int sectionCurrent(void) {
  for (int s = 0; s < SETTING_N_SECTIONS; s++)
    if (focusOp < SECTIONS[s].start + SECTIONS[s].n) return s;
  return SETTING_N_SECTIONS - 1;
}

static const char *helpOption(int op) {
  if (inactive(op)) {
    if (op == SETTING_RAIL) return "Turn off the modern sidebar to choose between collapsed and fixed.";
    if (op == SETTING_RAIL_BLUR) return "Turn on the modern sidebar to use the blur.";
    if (op == SETTING_HERO_CATALOGS) return "Turn on Show hero to display catalogues at the top of Home.";
    if (op >= SETTING_CW_STYLE && op <= SETTING_CW_ORDER)
      return op == SETTING_CW_BLUR_NEXT && settings_cw_on()
        ? "Turn on Episode thumbnail to blur the next episode image."
        : "Turn on Continue watching to adjust the resume cards.";
    if (op == SETTING_EXPAND_DELAY) return "Turn on Expand poster on focus to adjust the delay.";
    return "Turn on Depth effect to customise this detail.";
  }
  switch (op) {
    case SETTING_QUALITY: return "Sets the resolution preference. Availability depends on the addon sources.";
    case SETTING_DV: case SETTING_ATMOS: return "Preference for compatible sources. The available format also depends on the file and the TV.";
    case SETTING_HERO_CATALOGS: return "How many catalogues the hero includes. This row is informational only.";
    case SETTING_CW_FURTHEST: return "Picks the next episode from the furthest one marked as watched.";
    case SETTING_CW_BLUR_NEXT: case SETTING_DET_BLUR_NOT_WATCHED: return "Hides thumbnail detail to avoid spoilers for episodes you have not watched.";
    case SETTING_ANIM: return "Use Reduced for subtler motion when moving through the interface.";
    case SETTING_SPACE: return "Current memory used by the image cache, not space taken on the TV storage.";
    case SETTING_VERSION_I: return "Application version. This information cannot be changed.";
    case SETTING_WIDTH_DP: return "Sets the poster width on rows that use the customisable size.";
    case SETTING_RADIUS_DP: return "Controls how rounded the poster corners are.";
    default: return "Use the left and right arrows to choose. The preference applies as the value changes.";
  }
}

// Deslocamento vertical do topo da lista ate a linha `op`, contando os
// cabecalhos das secoes que vieram antes.
static float yOfOption(int op) {
  float y = 0.0f;
  for (int s = 0; s < SETTING_N_SECTIONS; s++) {
    y += (s ? SETTING_SEC_GAP : 0.0f) + SETTING_SEC_HEADER;
    for (int k = 0; k < SECTIONS[s].n; k++) {
      int o = SECTIONS[s].start + k;
      if (o == op) return y;
      y += SETTING_LINE_H + SETTING_LINE_GAP;
    }
  }
  return y;
}

void settings_event(const SDL_Event *e) {
  if (e->type != SDL_KEYDOWN) return;
  SDL_Keycode k = e->key.keysym.sym;

  // Vinculo em andamento e uma pergunta: enquanto ele esta em pe, nada mais na
  // tela responde ao controle.
  { TraState ta = traktauth_state();
    SmkState sa = simklauth_state();
    int traActive = (ta == TRA_REQUESTING || ta == TRA_WAITING || ta == TRA_ERROR);
    int smkActive = (sa == SMK_REQUESTING || sa == SMK_WAITING || sa == SMK_ERROR);
    if (traActive || smkActive) {
      if (k == SDLK_ESCAPE || k == SDLK_AC_BACK || k == SDLK_BACKSPACE) {
        if (traActive) traktauth_cancel(); else simklauth_cancel();
      } else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
        // OK so refaz o pedido quando deu erro; com o codigo na tela ele nao
        // faz nada de proposito, para nao trocar o codigo que a pessoa acabou
        // de digitar no celular.
        if (traActive && ta == TRA_ERROR) traktauth_begin();
        else if (smkActive && sa == SMK_ERROR) simklauth_begin();
      }
      return;
    } }
  if (k == SDLK_ESCAPE || k == SDLK_AC_BACK || k == SDLK_BACKSPACE ||
      k == SDLK_DELETE) { sair = 1; return; }

  if (k == SDLK_DOWN)      { if (focusOp < SETTING_N - 1) focusOp++; }
  else if (k == SDLK_UP)   { if (focusOp > 0)        focusOp--; }
  else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
    if (OPTIONS[focusOp].kind != OP_ACTION) return;
    if (focusOp == SETTING_TRAKT) { traktauth_begin(); return; }
    if (focusOp == SETTING_SIMKL) { simklauth_begin(); return; }
    if (focusOp == SETTING_EXIT) {
      // Sair apaga a sessao do disco. Sem confirmacao de proposito: o custo de
      // sair sem querer e um login por QR, e uma caixa de confirmacao nesta
      // lista exigiria um modal que a tela nao tem.
      session_exit();
      traktauth_forget();
      simklauth_forget();
      // A sessao sozinha nao basta: addons, Trakt, perfil e progresso ficariam
      // para a proxima pessoa. Ver o cabecalho de sync_esquecer_usuario.
      sync_forget_user();
      sair = 1;   // volta para a home, que cai no login no proximo quadro
    }
  }
  else if (k == SDLK_PAGEUP || k == SDLK_PAGEDOWN) {
    int s = sectionCurrent() + (k == SDLK_PAGEDOWN ? 1 : -1);
    if (s >= 0 && s < SETTING_N_SECTIONS) focusOp = SECTIONS[s].start;
  }
  else if (k == SDLK_LEFT || k == SDLK_RIGHT) {
    // Item so de leitura ou desligado pela dependencia nao muda com nada.
    if (!mutable(focusOp)) return;
    const Option *o = &OPTIONS[focusOp];
    int dir = (k == SDLK_RIGHT) ? 1 : -1;
    if (o->kind == OP_NUMBER) {
      // Numero NAO circula: passar de 100% para 0% com um toque a mais e um
      // salto que ninguem pede, e no controle da TV a seta repete sozinha.
      int v = value[focusOp] + dir * o->step;
      value[focusOp] = limits(focusOp, v);
    } else {
      // Escolha circula: a lista e curta e voltar do fim ao inicio poupa
      // toques no controle. Sem circular, o ultimo valor vira um beco.
      value[focusOp] = (value[focusOp] + (dir > 0 ? 1 : o->n - 1)) % o->n;
    }
    save();   // grava a cada mudanca: nao ha botao de "salvar" nesta tela
  }
}

void settings_update(float dt, Uint32 now) {
  (void)now;
  for (int i = 0; i < SETTING_N; i++) {
    float target = (i == focusOp) ? 1.0f : 0.0f;
    animFocus[i] = settings_animations_reduced() ? target : anim_spring(animFocus[i], target, dt,
                            target > animFocus[i] ? NV_SPRING_FOCUS : NV_SPRING_BLUR);
  }
  // Rola o minimo para a linha focada caber, e leva junto o cabecalho da secao
  // quando a linha e a primeira dela — sem isso, entrar numa secao mostra a
  // opcao sem dizer a que grupo ela pertence.
  float top = yOfOption(focusOp);
  for (int s = 0; s < SETTING_N_SECTIONS; s++)
    if (SECTIONS[s].start == focusOp) { top -= SETTING_SEC_HEADER; break; }
  float base = yOfOption(focusOp) + SETTING_LINE_H;
  float target = scrollY;
  if (base - target > SETTING_BASE - SETTING_TOP) target = base - (SETTING_BASE - SETTING_TOP);
  if (top - target < 0.0f)              target = top;
  if (target < 0.0f) target = 0.0f;
  scrollY = settings_animations_reduced() ? target : anim_spring(scrollY, target, dt, NV_SPRING_SCROLL);
}

// Texto do valor de uma linha. Buffer estatico porque so uma linha e desenhada
// por vez dentro de desenhaLinha.
static const char *textValue(int op) {
  static char buf[48];
  const Option *o = &OPTIONS[op];
  if (o->kind == OP_READ || o->kind == OP_ACTION) return textRead(op);
  if (o->kind == OP_NUMBER) {
    snprintf(buf, sizeof buf, "%d%s", value[op], o->suffix ? o->suffix : "");
    return buf;
  }
  return o->values[value[op]];
}

static void drawLine(int op, float y, float f) {
  if (y + SETTING_LINE_H < SETTING_TOP - 40.0f || y > SETTING_BASE + 40.0f) return;
  // Some antes de cruzar o titulo da tela, como as secoes da pagina de detalhe:
  // texto passando por baixo de texto se le como borrao.
  float a = anim_clamp((y - (SETTING_TOP - 70.0f)) / 60.0f, 0.0f, 1.0f);
  if (a <= 0.005f) return;

  int off = inactive(op);
  int canMudar = mutable(op);
  GfxRect line = { SETTING_LIST_X, y, SETTING_LIST_W, SETTING_LINE_H };
  // Mesmo vocabulário do menu: superfície escura, texto claro e foco explícito.
  gfx_color(line, SETTING_RADIUS, NV_COLOR_FOCUS_R, NV_COLOR_FOCUS_G, NV_COLOR_FOCUS_B,
          (0.34f + 0.66f * f) * a);
  if (op == focusOp)
    gfx_rect(line, 0, GFX_RING, 0, NV_RING_FOCUS / SETTING_LINE_H, 0,
             SETTING_RADIUS, 0.96f, 0.96f, 0.97f, a);

  // Uma linha inativa fica visivelmente mais apagada QUE a de leitura: leitura e
  // informacao, inativa e "isto existe mas depende de outra coisa".
  float aText = a * (off ? 0.65f : 1.0f);
  int cr = canMudar ? 240 : 192;
  TxtLine rot = txt_line_trim(TXT_CALLOUT, OPTIONS[op].label,
                                cr, cr, cr, 255, SETTING_LIST_W - 420.0f);
  txt_draw_alpha(rot, SETTING_LIST_X + SETTING_DFLT,
                     y + (SETTING_LINE_H - rot.h) * 0.5f, aText);

  const char *v = textValue(op);
  int cv = canMudar ? 220 : 176;
  TxtLine val = txt_line_trim(TXT_CALLOUT, v, cv, cv, cv, 255, 310.0f);
  float xDir = SETTING_LIST_X + SETTING_LIST_W - SETTING_DFLT;
  float valueDir = xDir - 36.0f;
  float vy = y + (SETTING_LINE_H - val.h) * 0.5f;

  // Barra de preenchimento da linha numerica. Sem ela, "28%" nao diz nada sobre
  // onde 28 fica no intervalo — e o web mostra um slider justamente por isso.
  if (OPTIONS[op].kind == OP_NUMBER) {
    const Option *o = &OPTIONS[op];
    float t = (o->max > o->min)
            ? (float)(value[op] - o->min) / (float)(o->max - o->min) : 0.0f;
    float bw = 220.0f, bh = 4.0f;
    float bx = valueDir - bw;
    float by = y + SETTING_LINE_H - 17.0f;
    vy -= 8.0f;
    GfxRect rail = { bx, by, bw, bh };
    GfxRect full  = { bx, by, bw * anim_clamp(t, 0.0f, 1.0f), bh };
    gfx_color(rail, 0.5f, 0.94f, 0.94f, 0.96f, 0.22f * aText);
    if (full.w > 0.5f)
      gfx_color(full, 0.5f, 0.94f, 0.94f, 0.96f, 0.92f * aText);
  }

  // As setas so aparecem na linha em foco que MUDA. Elas sao a instrucao: sem
  // elas, nada na tela diz que esquerda/direita e o gesto certo.
  if (canMudar && f > 0.02f) {
    TxtLine dir = txt_line(TXT_CAPTION2, "\xe2\x96\xb6", cv, cv, cv, 255);
    TxtLine left = txt_line(TXT_CAPTION2, "\xe2\x97\x80", cv, cv, cv, 255);
    txt_draw_alpha(dir, xDir - dir.w, y + (SETTING_LINE_H - dir.h) * 0.5f, aText * f);
    txt_draw_alpha(left, valueDir - val.w - 16.0f - left.w,
                       y + (SETTING_LINE_H - left.h) * 0.5f, aText * f);
  }
  txt_draw_alpha(val, valueDir - val.w, vy, aText);
}

// Sobreposicao do vinculo (Trakt ou Simkl). O codigo destes dois e CURTO — 8
// caracteres no Trakt — e o endereco e fixo, entao da para ler da TV e digitar
// no celular. Nao precisa de QR, ao contrario dos 32 digitos hexadecimais do
// login da conta.
static void drawLink(const char *service, const char *code,
                           const char *address, const char *failure, int waiting) {
  GfxRect screen = { 0, 0, NV_SCREEN_W, NV_SCREEN_H };
  GfxRect card = { (NV_SCREEN_W - 1000.0f) * 0.5f, 250.0f, 1000.0f, 560.0f };
  TxtLine l;
  char t[80];
  float y = 300.0f;
  // Veu QUASE opaco mais um cartao solido atras do bloco. Com 0.80 de veu e sem
  // cartao, as linhas de Ajustes atravessavam o texto — "aguardando" caia em
  // cima de "Trakt" e "conectar" em cima de "e informe o codigo". Um codigo que
  // a pessoa precisa transcrever nao pode competir com texto de fundo.
  gfx_color(screen, 0.0f, 0.0f, 0.0f, 0.0f, 0.92f);
  gfx_color(card, 0.045f, NV_COLOR_BACKGROUND_R, NV_COLOR_BACKGROUND_G, NV_COLOR_BACKGROUND_B, 1.0f);

  snprintf(t, sizeof t, "Connect %s", service);
  l = txt_line(TXT_TITLE2, t, 255, 255, 255, 255);
  txt_draw(l, (NV_SCREEN_W - l.w) * 0.5f, y);
  y += 92.0f;

  if (failure && failure[0]) {
    l = txt_line(TXT_HEADLINE, failure, 236, 108, 108, 255);
    txt_draw(l, (NV_SCREEN_W - l.w) * 0.5f, y);
    y += 70.0f;
    l = txt_line(TXT_CAPTION, "OK to try again · Back to close",
                  150, 152, 160, 255);
    txt_draw(l, (NV_SCREEN_W - l.w) * 0.5f, y);
    return;
  }
  if (!code || !code[0]) {
    l = txt_line(TXT_HEADLINE, "Preparing the code…", 210, 212, 220, 255);
    txt_draw(l, (NV_SCREEN_W - l.w) * 0.5f, y);
    return;
  }

  l = txt_line(TXT_BODY, "No celular, abra:", 176, 178, 186, 255);
  txt_draw(l, (NV_SCREEN_W - l.w) * 0.5f, y);
  y += 52.0f;
  l = txt_line(TXT_TITLE3, address && address[0] ? address : "-", 255, 255, 255, 255);
  txt_draw(l, (NV_SCREEN_W - l.w) * 0.5f, y);
  y += 92.0f;
  l = txt_line(TXT_BODY, "and enter the code:", 176, 178, 186, 255);
  txt_draw(l, (NV_SCREEN_W - l.w) * 0.5f, y);
  y += 66.0f;

  // Espacamento entre letras: um codigo curto sem tracking le como palavra, e
  // a pessoa transcreve errado.
  { float width = txt_tracking(TXT_TITLE1, code, 255, 255, 255, -1.0f, 0.0f, 1.0f, 16.0f);
    txt_tracking(TXT_TITLE1, code, 255, 255, 255,
                 (NV_SCREEN_W - width) * 0.5f, y, 1.0f, 16.0f); }
  y += 130.0f;

  if (waiting) {
    l = txt_line(TXT_CAPTION, "Waiting for authorisation…", 150, 152, 160, 255);
    txt_draw(l, (NV_SCREEN_W - l.w) * 0.5f, y);
  }
}

void settings_draw(Uint32 now) {
  (void)now;
  // Fundo opaco proprio: a tela cobre tudo e nao pode depender de quem desenhou
  // antes dela — sem isto a home aparece entre as linhas da lista.
  GfxRect screen = { 0, 0, NV_SCREEN_W, NV_SCREEN_H };
  // A tela ja foi limpa com ESTA MESMA COR por glClearColor/glClear em
  // main.c antes de app_desenhar. Pintar por cima era uma camada de tela
  // cheia jogada fora por quadro — e o custo dominante nesta GPU e fill
  // rate (gfx.c registra que DUAS camadas de tela cheia derrubavam a
  // Mali-G71 para ~40fps). Nao repor sem antes mudar a cor do clear.
  (void)screen;

  TxtLine title = txt_line(TXT_TITLE1, "Settings", 255, 255, 255, 255);
  txt_draw(title, SETTING_LIST_X, NV_MARGIN_Y);

  int sec = sectionCurrent();
  char pos[80];
  snprintf(pos, sizeof pos, "%s  ·  %d of %d", SECTIONS[sec].title,
           focusOp - SECTIONS[sec].start + 1, SECTIONS[sec].n);
  TxtLine context = txt_line(TXT_CAPTION, pos, 178, 180, 186, 255);
  txt_draw(context, SETTING_LIST_X, NV_MARGIN_Y + title.h + 10.0f);

  float hx = SETTING_LIST_X + SETTING_LIST_W + 52.0f;
  float hw = NV_SCREEN_W - NV_MARGIN_X - hx;
  if (hw > 240.0f) {
    TxtLine kind = txt_line(TXT_CAPTION, inactive(focusOp) ? "Option unavailable"
                        : soRead(focusOp) ? "Information" : "Customise", 168, 171, 180, 255);
    txt_draw(kind, hx, SETTING_TOP + SETTING_SEC_HEADER);
    float hy = SETTING_TOP + SETTING_SEC_HEADER + kind.h + 22.0f;
    hy += txt_block(TXT_HEADLINE, OPTIONS[focusOp].label, 237, 238, 242,
                   hx, hy, hw, 40, 1, 3);
    hy += 22.0f;
    hy += txt_block(TXT_CAPTION, helpOption(focusOp), 183, 186, 194,
                   hx, hy, hw, 32, 1, 7);
    hy += 42.0f;
    txt_block(TXT_CAPTION, "↑ ↓  Navigate\n← →  Change value\nBack  Leave settings",
              155, 159, 169, hx, hy, hw, 34, 1, 4);
  }

  gfx_crop(SETTING_LIST_X - NV_RING_FOCUS, SETTING_TOP,
               SETTING_LIST_W + NV_RING_FOCUS * 2, SETTING_BASE - SETTING_TOP);
  float y = SETTING_TOP - scrollY;
  for (int s = 0; s < SETTING_N_SECTIONS; s++) {
    if (s) y += SETTING_SEC_GAP;
    // Cabecalho da secao em corpo pequeno e cinza: ele rotula o grupo, nao
    // compete com os rotulos das opcoes.
    float aC = anim_clamp((y - (SETTING_TOP - 70.0f)) / 60.0f, 0.0f, 1.0f);
    TxtLine ts = txt_line(TXT_CAPTION, SECTIONS[s].title, 150, 152, 160, 255);
    if (aC > 0.005f && y < SETTING_BASE)
      txt_draw_alpha(ts, SETTING_LIST_X + SETTING_DFLT, y + SETTING_SEC_HEADER - ts.h - 10.0f, aC);
    y += SETTING_SEC_HEADER;
    for (int k = 0; k < SECTIONS[s].n; k++) {
      int op = SECTIONS[s].start + k;
      drawLine(op, y, animFocus[op]);
      y += SETTING_LINE_H + SETTING_LINE_GAP;
    }
  }
  gfx_no_crop();

  float total = yOfOption(SETTING_N - 1) + SETTING_LINE_H;
  float window = SETTING_BASE - SETTING_TOP;
  if (total > window) {
    float height = window * window / total;
    float sy = SETTING_TOP + (window - height) * anim_clamp(scrollY / (total - window), 0, 1);
    gfx_color((GfxRect){ SETTING_LIST_X + SETTING_LIST_W + 18, SETTING_TOP, 3, window },
            0.5f, 0.60f, 0.62f, 0.66f, 0.14f);
    gfx_color((GfxRect){ SETTING_LIST_X + SETTING_LIST_W + 18, sy, 3, height },
            0.5f, 0.80f, 0.82f, 0.86f, 0.8f);
  }
  TxtLine footer = txt_line(TXT_CAPTION, "PgUp / PgDn  Change section", 156, 159, 168, 255);
  txt_draw(footer, SETTING_LIST_X, SETTING_BASE + 20);

  // Por cima de tudo: enquanto um vinculo esta em andamento, ele e a pergunta
  // da tela.
  { TraState ta = traktauth_state();
    SmkState sa = simklauth_state();
    if (ta == TRA_REQUESTING || ta == TRA_WAITING || ta == TRA_ERROR)
      drawLink("o Trakt", traktauth_code(), traktauth_url(),
                     traktauth_error(), ta == TRA_WAITING);
    else if (sa == SMK_REQUESTING || sa == SMK_WAITING || sa == SMK_ERROR)
      drawLink("o Simkl", simklauth_code(), simklauth_url(),
                     simklauth_error(), sa == SMK_WAITING); }
}
