// Ajustes: lista vertical em secoes, rotulo a esquerda e valor a direita.
//
// A regra que organiza a tela inteira: existem TRES naturezas de linha e elas
// TEM que parecer diferentes. Linha de escolha ganha pilula clara com texto
// escuro e as setas ao redor do valor; linha NUMERICA acrescenta uma barra de
// preenchimento sob o valor, porque "28%" sem barra nao diz onde fica no
// intervalo; linha so de leitura ganha um realce apagado, sem setas. Com o mesmo
// desenho nas tres, o usuario aperta esquerda e direita em cima da versao do app
// esperando que algo aconteca — foi por isso que a distincao virou requisito.
//
// As chaves e os agrupamentos seguem a tela de Layout do app web
// (js/ui/screens/settings/settingsScreen.js), inclusive os rotulos em portugues
// lidos da tela rodando.
#include "ajustes.h"
#include "gfx.h"
#include "text.h"
#include "tex_cache.h"
#include "anim.h"
#include "layout.h"
#include <stdio.h>
#include <string.h>

// Versao do app: mesma string do appinfo.json empacotado. Fica aqui porque a
// tela nao tem como ler o manifesto em tempo de execucao no aparelho.
#define AJ_VERSAO       "1.0.1"

#define AJ_LINHA_H       88.0f
#define AJ_LINHA_GAP      8.0f
#define AJ_SEC_GAP       46.0f    // fim de uma secao ao cabecalho da proxima
#define AJ_SEC_CABEC     44.0f    // altura reservada ao cabecalho da secao
// Nao e constante: acompanha a rail, como todo o resto do conteudo. Com a
// barra recolhida a lista tambem comeca em 104 — deixar 248 cravado aqui fazia
// a tela de Ajustes ser a unica desalinhada das outras.
#define AJ_LISTA_X      ajustes_conteudo_x()
#define AJ_LISTA_W     1120.0f
#define AJ_PAD           34.0f    // borda da linha ao texto
#define AJ_TOPO        (NV_MARGEM_Y + 118.0f)   // abaixo do titulo da tela
#define AJ_BASE        (NV_TELA_H - NV_MARGEM_Y)
// Raio da linha em fracao do menor lado (o SDF do shader e normalizado):
// 12px sobre 88 de altura.
#define AJ_RAIO           0.14f

// Ordem do enum = ordem no arquivo de chaves e nas tabelas. Acrescentar no MEIO
// e seguro: o arquivo e por chave, nao posicional (ver ajustes_dir).
typedef enum {
  // Reproducao
  AJ_QUALIDADE, AJ_DV, AJ_ATMOS,
  // Layout da Home
  AJ_LANDSCAPE, AJ_HERO_CHEIO,
  // Conteudo da Home
  AJ_RAIL, AJ_RAIL_MODERNA, AJ_RAIL_BLUR, AJ_HERO, AJ_HERO_CATALOGOS,
  AJ_DESCOBRIR, AJ_ROTULOS, AJ_NOME_ADDON, AJ_SUFIXO_TIPO,
  AJ_OCULTAR_NLANC, AJ_NOTAS_HOME, AJ_GRAD_CLASSICO,
  // Continuar assistindo
  AJ_CW_LIGADO, AJ_CW_ESTILO, AJ_CW_THUMB, AJ_CW_BLUR_PROX,
  AJ_CW_FURTHEST, AJ_CW_NAO_EXIBIDOS, AJ_CW_ORDEM,
  // Pagina de detalhe
  AJ_DET_BLUR_NAO_VISTOS, AJ_DET_TRAILER, AJ_DET_META_EXT, AJ_DET_DATA_CHEIA,
  // Foco no poster
  AJ_EXPANDIR, AJ_EXPANDIR_ATRASO, AJ_NAV_RAPIDA,
  // Profundidade
  AJ_PROF, AJ_PROF_BORDA, AJ_PROF_BRILHO, AJ_PROF_COBERTURA,
  AJ_PROF_POSTERS, AJ_PROF_CW, AJ_PROF_EPS, AJ_PROF_ELENCO, AJ_PROF_TRAILERS,
  // Tamanho do item
  AJ_LARGURA_DP, AJ_RAIO_DP,
  // Interface
  AJ_IDIOMA, AJ_ANIM,
  // Sobre
  AJ_VERSAO_I, AJ_ESPACO,
  AJ_N
} OpcaoId;

static const char *V_QUALIDADE[] = { "Automática", "4K", "1080p", "720p" };
static const char *V_LIGA[]      = { "Ligado", "Desligado" };
static const char *V_IDIOMA[]    = { "Português", "English" };
static const char *V_ANIM[]      = { "Completas", "Reduzidas" };
// `collapseSidebar`: recolhida = a rail some e o conteudo comeca em 104.
static const char *V_RAIL[]      = { "Recolhida", "Fixa" };
// `continueWatchingCardStyle`, validado em layoutPreferences.js contra
// exatamente estes tres valores.
static const char *V_CW[]        = { "Card", "Largo", "P\xc3\xb4ster" };
// `continueWatchingSortMode`, normalizado em normalizeContinueWatchingSortMode.
static const char *V_CW_ORDEM[]  = { "Padrão", "Estilo streaming", "Separar futuros" };
// `discoverLocation`, validado contra estes tres.
static const char *V_DESCOBRIR[] = { "Mostrar na Busca", "Na barra lateral", "Desligado" };
// `homeImdbRatingsVisibility` — normalizeHomeImdbRatingsVisibility so aceita
// SHOW_ALL e HIDE_ALL.
static const char *V_NOTAS[]     = { "Mostrar", "Ocultar" };

// Natureza da linha.
typedef enum { OP_ESCOLHA, OP_NUMERO, OP_LEITURA } OpcaoTipo;

typedef struct {
  const char  *rotulo;
  OpcaoTipo    tipo;
  const char **valores;   // OP_ESCOLHA
  int          n;         // OP_ESCOLHA: quantos valores
  int          min, max, passo;   // OP_NUMERO
  const char  *sufixo;            // OP_NUMERO: "%", "s", "dp"
} Opcao;

#define ESC(rot, vals, qtd) { rot, OP_ESCOLHA, vals, qtd, 0, 0, 0, NULL }
#define NUM(rot, lo, hi, st, suf) { rot, OP_NUMERO, NULL, 0, lo, hi, st, suf }
#define LER(rot)            { rot, OP_LEITURA, NULL, 0, 0, 0, 0, NULL }

static const Opcao OPCOES[AJ_N] = {
  ESC("Qualidade máxima",           V_QUALIDADE, 4),
  ESC("Dolby Vision",               V_LIGA, 2),
  ESC("Dolby Atmos",                V_LIGA, 2),

  ESC("Pôsteres horizontais",       V_LIGA, 2),   // modernLandscapePostersEnabled
  ESC("Fundo em tela cheia",        V_LIGA, 2),   // modernHeroFullScreenBackdropEnabled

  ESC("Barra lateral",              V_RAIL, 2),   // collapseSidebar
  ESC("Barra lateral moderna",      V_LIGA, 2),   // modernSidebar
  ESC("Desfoque da barra moderna",  V_LIGA, 2),   // modernSidebarBlur
  ESC("Mostrar destaque",           V_LIGA, 2),   // heroSectionEnabled
  LER("Catálogos do destaque"),                   // heroCatalogKeys (contagem)
  ESC("Local do Descobrir",         V_DESCOBRIR, 3), // discoverLocation
  ESC("Rótulos nos pôsteres",       V_LIGA, 2),   // posterLabelsEnabled
  ESC("Nome do addon no catálogo",  V_LIGA, 2),   // catalogAddonNameEnabled
  ESC("Tipo de conteúdo",           V_LIGA, 2),   // catalogTypeSuffixEnabled
  ESC("Ocultar não lançados",       V_LIGA, 2),   // hideUnreleasedContent
  ESC("Avaliações gerais",          V_NOTAS, 2),  // homeImdbRatingsVisibility
  ESC("Gradiente de foco clássico", V_LIGA, 2),   // classicFocusGradientEnabled

  ESC("Mostrar \"Continuar assistindo\"", V_LIGA, 2), // continueWatchingEnabled
  ESC("Estilo do \"Continuar assistindo\"", V_CW, 3), // continueWatchingCardStyle
  ESC("Miniatura do episódio",      V_LIGA, 2),   // useEpisodeThumbnailsInCw
  ESC("Desfocar próximo episódio",  V_LIGA, 2),   // blurContinueWatchingNextUp
  ESC("Próximo do episódio mais alto", V_LIGA, 2),// nextUpFromFurthestEpisode
  ESC("Mostrar episódios não exibidos", V_LIGA, 2),// showUnairedNextUp
  ESC("Ordenação",                  V_CW_ORDEM, 3), // continueWatchingSortMode

  ESC("Desfocar não assistidos",    V_LIGA, 2),   // blurUnwatchedEpisodes
  ESC("Botão de trailer",           V_LIGA, 2),   // detailPageTrailerButtonEnabled
  ESC("Priorizar metadados externos", V_LIGA, 2), // preferExternalMetaAddonDetail
  ESC("Data de lançamento completa", V_LIGA, 2),  // showFullReleaseDate

  ESC("Expandir pôster ao focar",   V_LIGA, 2),   // focusedPosterBackdropExpandEnabled
  NUM("Atraso da expansão",         0, 10, 1, " s"), // ...ExpandDelaySeconds
  ESC("Navegação horizontal rápida", V_LIGA, 2),  // fastHorizontalNavigationEnabled

  ESC("Efeito de profundidade",     V_LIGA, 2),   // cardDepthEnabled
  NUM("Brilho da borda",            0, 100, 2, "%"), // cardDepthEdgeStrength
  NUM("Reflexo",                    0, 100, 2, "%"), // cardDepthSheenStrength
  NUM("Cobertura da borda",         0, 100, 2, "%"), // cardDepthEdgeCoverage
  ESC("Profundidade nos pôsteres",  V_LIGA, 2),
  ESC("Profundidade no \"Continuar\"", V_LIGA, 2),
  ESC("Profundidade nos episódios", V_LIGA, 2),
  ESC("Profundidade no elenco",     V_LIGA, 2),
  ESC("Profundidade nos trailers",  V_LIGA, 2),

  NUM("Largura do item",            72, 200, 2, " dp"), // posterCardWidthDp
  NUM("Arredondamento",             0, 40, 1, " dp"),   // posterCardCornerRadiusDp

  ESC("Idioma",                     V_IDIOMA, 2),
  ESC("Animações",                  V_ANIM, 2),

  LER("Versão"),
  LER("Espaço usado por imagens"),
};

// Nome de cada opcao no arquivo. O formato era POSICIONAL — uma linha por
// opcao, na ordem do enum — e por isso acrescentar uma opcao no meio fazia o
// arquivo de quem ja tinha o app aplicar os valores errados, em silencio. Com
// chave por linha, opcao nova nasce no padrao e as antigas continuam onde
// estavam. Os nomes seguem os do app web onde existe correspondente.
static const char *CHAVE[AJ_N] = {
  "qualidade", "dolbyVision", "dolbyAtmos",
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
  "idioma", "animacoes", "-versao", "-espaco",
};

// Onde cada secao comeca e quantas opcoes ela tem. Secao e um agrupamento
// visual, nao um nivel de navegacao: cima/baixo atravessa os cabecalhos sem
// parar neles, como no aparelho. Os titulos sao os do app web.
static const struct { const char *titulo; int ini, n; } SECOES[] = {
  { "Reprodução",                     AJ_QUALIDADE,           3 },
  { "Layout da Home",                 AJ_LANDSCAPE,           2 },
  { "Conteúdo da Home",               AJ_RAIL,               12 },
  { "Continuar assistindo",           AJ_CW_LIGADO,           7 },
  { "Página de Detalhes",             AJ_DET_BLUR_NAO_VISTOS, 4 },
  { "Foco no Pôster",                 AJ_EXPANDIR,            3 },
  { "Efeito de Profundidade",         AJ_PROF,                9 },
  { "Tamanho dos itens",              AJ_LARGURA_DP,          2 },
  { "Interface",                      AJ_IDIOMA,              2 },
  { "Sobre",                          AJ_VERSAO_I,            2 },
};
#define AJ_N_SECOES (int)(sizeof SECOES / sizeof *SECOES)

// Valor de cada opcao. Para OP_ESCOLHA e o indice; para OP_NUMERO e o proprio
// numero. Os padroes sao os DEFAULTS de layoutPreferences.js, com UMA excecao
// anotada linha a linha: as quatro que o perfil do dono diverge de fabrica
// nascem como ele as deixou, porque e o que ele ve hoje. Todas sao trocaveis
// aqui, que era o ponto.
static int valor[AJ_N] = {
  0, 0, 0,          /* qualidade, DV, Atmos */

  0,                /* posteres deitados: LIGADO (perfil do dono; fabrica: desligado) */
  0,                /* fundo em tela cheia: LIGADO (perfil; fabrica: desligado) */

  0,                /* barra lateral: recolhida (perfil; fabrica: fixa) */
  1,                /* barra lateral moderna: desligada */
  0,                /* desfoque da barra moderna: ligado (perfil) */
  0,                /* mostrar destaque: ligado */
  0,                /* catalogos do destaque: leitura */
  0,                /* local do descobrir: na busca */
  0,                /* rotulos nos posteres: ligado */
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

static int focoOp = 0;
// Uma lista de UMA coluna nao precisa do focus.h: a memoria de coluna que ele
// existe para resolver nao tem o que lembrar aqui, e o indice cru deixa o
// "pula o cabecalho da secao" ser uma soma em vez de um mapa de fileiras.
static float animFoco[AJ_N];
static float scrollY = 0.0f;
static int sair = 0;

// Quantos catalogos o destaque usa. 0 = todos, que e o que o web escreve como
// "Todos" quando heroCatalogKeys esta vazio — e o caso do perfil do dono.
static int heroCatalogos = 0;

static int lig(int op)  { return valor[op] == 0; }

int ajustes_animacoes_reduzidas(void) { return valor[AJ_ANIM] == 1; }
int ajustes_dolby_vision(void)        { return lig(AJ_DV); }
int ajustes_dolby_atmos(void)         { return lig(AJ_ATMOS); }
int ajustes_idioma_ingles(void)       { return valor[AJ_IDIOMA] == 1; }

// `collapseSidebar: modernSidebar ? false : Boolean(collapseSidebar)` — a barra
// moderna DESLIGA o recolhimento, e nao o contrario. Copiado de
// normalizeLayoutPreferences para nao inventar precedencia.
int ajustes_rail_moderna(void)        { return lig(AJ_RAIL_MODERNA); }
int ajustes_rail_recolhida(void)      { return ajustes_rail_moderna() ? 0 : lig(AJ_RAIL); }
int ajustes_rail_moderna_blur(void)   { return lig(AJ_RAIL_BLUR); }
int ajustes_hero_ligado(void)         { return lig(AJ_HERO); }
int ajustes_hero_cheio(void)          { return lig(AJ_HERO_CHEIO); }
int ajustes_posteres_deitados(void)   { return lig(AJ_LANDSCAPE); }
int ajustes_gradiente_foco_classico(void) { return lig(AJ_GRAD_CLASSICO); }

int ajustes_rotulos_poster(void)      { return lig(AJ_ROTULOS); }
int ajustes_nome_addon(void)          { return lig(AJ_NOME_ADDON); }
int ajustes_sufixo_tipo(void)         { return lig(AJ_SUFIXO_TIPO); }
int ajustes_ocultar_nao_lancados(void){ return lig(AJ_OCULTAR_NLANC); }
int ajustes_data_completa(void)       { return lig(AJ_DET_DATA_CHEIA); }
int ajustes_notas_home(void)          { return valor[AJ_NOTAS_HOME] == 0; }
int ajustes_local_descobrir(void)     { return valor[AJ_DESCOBRIR]; }
int ajustes_descobrir_na_busca(void)  { return valor[AJ_DESCOBRIR] == 0; }

int ajustes_cw_ligado(void)           { return lig(AJ_CW_LIGADO); }
int ajustes_cw_estilo(void)           { return valor[AJ_CW_ESTILO]; }
int ajustes_cw_thumb_episodio(void)   { return lig(AJ_CW_THUMB); }
int ajustes_cw_desfocar_proximo(void) { return lig(AJ_CW_BLUR_PROX); }
int ajustes_cw_do_episodio_mais_alto(void) { return lig(AJ_CW_FURTHEST); }
int ajustes_cw_mostrar_nao_exibidos(void)  { return lig(AJ_CW_NAO_EXIBIDOS); }
int ajustes_cw_ordem(void)            { return valor[AJ_CW_ORDEM]; }

int ajustes_desfocar_nao_assistidos(void) { return lig(AJ_DET_BLUR_NAO_VISTOS); }
int ajustes_botao_trailer(void)       { return lig(AJ_DET_TRAILER); }
int ajustes_meta_externo(void)        { return lig(AJ_DET_META_EXT); }

int   ajustes_expandir_poster(void)   { return lig(AJ_EXPANDIR); }
float ajustes_expandir_poster_atraso(void) { return (float)valor[AJ_EXPANDIR_ATRASO]; }
int   ajustes_navegacao_horizontal_rapida(void) { return lig(AJ_NAV_RAPIDA); }

int   ajustes_profundidade(void)      { return lig(AJ_PROF); }
float ajustes_profundidade_borda(void)     { return valor[AJ_PROF_BORDA] / 100.0f; }
float ajustes_profundidade_brilho(void)    { return valor[AJ_PROF_BRILHO] / 100.0f; }
float ajustes_profundidade_cobertura(void) { return valor[AJ_PROF_COBERTURA] / 100.0f; }
int   ajustes_profundidade_posters(void)   { return lig(AJ_PROF_POSTERS); }
int   ajustes_profundidade_cw(void)        { return lig(AJ_PROF_CW); }
int   ajustes_profundidade_episodios(void) { return lig(AJ_PROF_EPS); }
int   ajustes_profundidade_elenco(void)    { return lig(AJ_PROF_ELENCO); }
int   ajustes_profundidade_trailers(void)  { return lig(AJ_PROF_TRAILERS); }

int   ajustes_largura_poster_dp(void) { return valor[AJ_LARGURA_DP]; }
int   ajustes_raio_poster_dp(void)    { return valor[AJ_RAIO_DP]; }
// dpToPx = 2 em buildModernHomeSizingStyle. 12dp -> 24px, que e o raio medido.
float ajustes_raio_poster_px(void)    { return (float)valor[AJ_RAIO_DP] * 2.0f; }

// A regra do web, e nao dois layouts: o conteudo tem sempre 104 de recuo e a
// rail acrescenta os 144 dela quando esta fixa.
float ajustes_conteudo_x(void) {
  return ajustes_rail_recolhida() ? NV_CONTENT_PAD
                                  : NV_LEGACY_RAIL_W + NV_CONTENT_PAD;
}
const char *ajustes_qualidade(void)   { return V_QUALIDADE[valor[AJ_QUALIDADE]]; }

// Onde os ajustes ficam. Ate a versao anterior nada era gravado: mexer numa
// opcao valia so enquanto o app estivesse aberto, e voltar depois mostrava tudo
// no padrao — o que faz a tela inteira parecer decorativa.
static char dirAjustes[512];

static int limita(int op, int v) {
  const Opcao *o = &OPCOES[op];
  if (o->tipo == OP_ESCOLHA) return (v >= 0 && v < o->n) ? v : valor[op];
  if (o->tipo == OP_NUMERO)  return v < o->min ? o->min : (v > o->max ? o->max : v);
  return valor[op];
}

void ajustes_dir(const char *dir) {
  FILE *f;
  char caminho[600], linha[96];
  if (!dir || !*dir) return;
  snprintf(dirAjustes, sizeof dirAjustes, "%s", dir);
  snprintf(caminho, sizeof caminho, "%s/ajustes.txt", dirAjustes);
  f = fopen(caminho, "r");
  if (!f) return;
  while (fgets(linha, sizeof linha, f)) {
    char chave[64]; int v, i;
    if (sscanf(linha, "%63s %d", chave, &v) != 2) continue;
    for (i = 0; i < AJ_N; i++) {
      if (strcmp(CHAVE[i], chave) || OPCOES[i].tipo == OP_LEITURA) continue;
      // Valor fora da faixa (arquivo de outra versao, ou editado a mao) cai no
      // padrao em vez de indexar fora do vetor.
      valor[i] = limita(i, v);
      break;
    }
  }
  fclose(f);
}

static void gravar(void) {
  char caminho[600], tmp[600];
  FILE *f;
  int i;
  if (!dirAjustes[0]) return;
  snprintf(caminho, sizeof caminho, "%s/ajustes.txt", dirAjustes);
  snprintf(tmp, sizeof tmp, "%s/ajustes.tmp", dirAjustes);
  f = fopen(tmp, "w");
  if (!f) return;
  for (i = 0; i < AJ_N; i++)
    if (OPCOES[i].tipo != OP_LEITURA) fprintf(f, "%s %d\n", CHAVE[i], valor[i]);
  fclose(f);
  rename(tmp, caminho);
}

int ajustes_iniciar(void) { focoOp = 0; scrollY = 0.0f; sair = 0; return 1; }
void ajustes_encerrar(void) { }
int ajustes_quer_sair(void) { return sair; }

// Valor das linhas so de leitura. O espaco em disco NAO e um numero inventado:
// vem do cache de texturas, que e exatamente o que "imagens" consome no
// aparelho — um numero fixo aqui seria mentira e nunca mudaria.
static const char *textoLeitura(int op) {
  static char buf[64];
  if (op == AJ_VERSAO_I) return AJ_VERSAO;
  if (op == AJ_HERO_CATALOGOS) {
    // "Todos" com a lista vazia e o que o web escreve (common_all), e e o estado
    // do perfil do dono. Um "0" ali leria como "nenhum", o oposto do que e.
    if (heroCatalogos <= 0) return "Todos";
    snprintf(buf, sizeof buf, "%d", heroCatalogos);
    return buf;
  }
  int itens = 0, pend = 0; long bytes = 0;
  tex_estatisticas(&itens, &pend, &bytes);
  snprintf(buf, sizeof buf, "%.1f MB em %d imagens", bytes / 1048576.0, itens);
  return buf;
}

// Uma opcao pode ficar INATIVA por causa de outra — o web esconde a linha
// (`model.layout.modernSidebar ? "" : renderToggleRow(...)`), mas esconder num
// D-pad muda a contagem de linhas embaixo do dedo do usuario a cada toque. Aqui
// ela continua no lugar, apagada e sem setas: a dependencia fica visivel em vez
// de a linha sumir.
static int inativa(int op) {
  switch (op) {
    case AJ_RAIL:         return ajustes_rail_moderna();
    case AJ_RAIL_BLUR:    return !ajustes_rail_moderna();
    case AJ_HERO_CATALOGOS: return !ajustes_hero_ligado();
    case AJ_CW_ESTILO: case AJ_CW_THUMB: case AJ_CW_FURTHEST:
    case AJ_CW_NAO_EXIBIDOS: case AJ_CW_ORDEM:
      return !ajustes_cw_ligado();
    case AJ_CW_BLUR_PROX: return !ajustes_cw_ligado() || !ajustes_cw_thumb_episodio();
    case AJ_EXPANDIR_ATRASO: return !ajustes_expandir_poster();
    case AJ_PROF_BORDA: case AJ_PROF_BRILHO: case AJ_PROF_COBERTURA:
    case AJ_PROF_POSTERS: case AJ_PROF_CW: case AJ_PROF_EPS:
    case AJ_PROF_ELENCO: case AJ_PROF_TRAILERS:
      return !ajustes_profundidade();
    default: return 0;
  }
}

static int soLeitura(int op) { return OPCOES[op].tipo == OP_LEITURA; }
static int mutavel(int op)   { return !soLeitura(op) && !inativa(op); }

// Deslocamento vertical do topo da lista ate a linha `op`, contando os
// cabecalhos das secoes que vieram antes.
static float yDaOpcao(int op) {
  float y = 0.0f;
  for (int s = 0; s < AJ_N_SECOES; s++) {
    y += (s ? AJ_SEC_GAP : 0.0f) + AJ_SEC_CABEC;
    for (int k = 0; k < SECOES[s].n; k++) {
      int o = SECOES[s].ini + k;
      if (o == op) return y;
      y += AJ_LINHA_H + AJ_LINHA_GAP;
    }
  }
  return y;
}

void ajustes_evento(const SDL_Event *e) {
  if (e->type != SDL_KEYDOWN) return;
  SDL_Keycode k = e->key.keysym.sym;
  if (k == SDLK_ESCAPE || k == SDLK_AC_BACK || k == SDLK_BACKSPACE ||
      k == SDLK_DELETE) { sair = 1; return; }

  if (k == SDLK_DOWN)      { if (focoOp < AJ_N - 1) focoOp++; }
  else if (k == SDLK_UP)   { if (focoOp > 0)        focoOp--; }
  else if (k == SDLK_LEFT || k == SDLK_RIGHT) {
    // Item so de leitura ou desligado pela dependencia nao muda com nada.
    if (!mutavel(focoOp)) return;
    const Opcao *o = &OPCOES[focoOp];
    int dir = (k == SDLK_RIGHT) ? 1 : -1;
    if (o->tipo == OP_NUMERO) {
      // Numero NAO circula: passar de 100% para 0% com um toque a mais e um
      // salto que ninguem pede, e no controle da TV a seta repete sozinha.
      int v = valor[focoOp] + dir * o->passo;
      valor[focoOp] = limita(focoOp, v);
    } else {
      // Escolha circula: a lista e curta e voltar do fim ao inicio poupa
      // toques no controle. Sem circular, o ultimo valor vira um beco.
      valor[focoOp] = (valor[focoOp] + (dir > 0 ? 1 : o->n - 1)) % o->n;
    }
    gravar();   // grava a cada mudanca: nao ha botao de "salvar" nesta tela
  }
}

void ajustes_atualizar(float dt, Uint32 agora) {
  (void)agora;
  for (int i = 0; i < AJ_N; i++) {
    float alvo = (i == focoOp) ? 1.0f : 0.0f;
    animFoco[i] = anim_mola(animFoco[i], alvo, dt,
                            alvo > animFoco[i] ? NV_MOLA_FOCO : NV_MOLA_DESFOCO);
  }
  // Rola o minimo para a linha focada caber, e leva junto o cabecalho da secao
  // quando a linha e a primeira dela — sem isso, entrar numa secao mostra a
  // opcao sem dizer a que grupo ela pertence.
  float topo = yDaOpcao(focoOp);
  for (int s = 0; s < AJ_N_SECOES; s++)
    if (SECOES[s].ini == focoOp) { topo -= AJ_SEC_CABEC; break; }
  float base = yDaOpcao(focoOp) + AJ_LINHA_H;
  float alvo = scrollY;
  if (base - alvo > AJ_BASE - AJ_TOPO) alvo = base - (AJ_BASE - AJ_TOPO);
  if (topo - alvo < 0.0f)              alvo = topo;
  if (alvo < 0.0f) alvo = 0.0f;
  scrollY = anim_mola(scrollY, alvo, dt, NV_MOLA_SCROLL);
}

// Texto do valor de uma linha. Buffer estatico porque so uma linha e desenhada
// por vez dentro de desenhaLinha.
static const char *textoValor(int op) {
  static char buf[48];
  const Opcao *o = &OPCOES[op];
  if (o->tipo == OP_LEITURA) return textoLeitura(op);
  if (o->tipo == OP_NUMERO) {
    snprintf(buf, sizeof buf, "%d%s", valor[op], o->sufixo ? o->sufixo : "");
    return buf;
  }
  return o->valores[valor[op]];
}

static void desenhaLinha(int op, float y, float f) {
  if (y + AJ_LINHA_H < AJ_TOPO - 40.0f || y > AJ_BASE + 40.0f) return;
  // Some antes de cruzar o titulo da tela, como as secoes da pagina de detalhe:
  // texto passando por baixo de texto se le como borrao.
  float a = anim_clamp((y - (AJ_TOPO - 70.0f)) / 60.0f, 0.0f, 1.0f);
  if (a <= 0.005f) return;

  int leitura = soLeitura(op);
  int desligada = inativa(op);
  int podeMudar = mutavel(op);
  GfxRect linha = { AJ_LISTA_X, y, AJ_LISTA_W, AJ_LINHA_H };
  // Aqui esta a diferenca visual entre mudavel e nao mudavel: a pilula clara com
  // texto escuro e a marca de "isto responde ao controle". A linha de leitura e
  // a inativa recebem so um realce apagado e mantem o texto claro — elas mostram
  // que o foco esta ali sem prometer interacao.
  if (!podeMudar) {
    gfx_cor(linha, AJ_RAIO, 1, 1, 1, (0.03f + 0.09f * f) * a);
  } else {
    float lum = 0.62f + 0.34f * f;
    gfx_cor(linha, AJ_RAIO, lum, lum, lum, (0.06f + 0.86f * f) * a);
  }

  // Uma linha inativa fica visivelmente mais apagada QUE a de leitura: leitura e
  // informacao, inativa e "isto existe mas depende de outra coisa".
  float aTexto = a * (desligada ? 0.42f : 1.0f);
  int escuro = (podeMudar && f > 0.55f);
  int cr = escuro ? 24 : (podeMudar ? 240 : 176);
  TxtLinha rot = txt_linha(TXT_BODY, OPCOES[op].rotulo, cr, cr, cr, 255);
  txt_desenhar_alpha(rot, AJ_LISTA_X + AJ_PAD,
                     y + (AJ_LINHA_H - rot.h) * 0.5f, aTexto);

  const char *v = textoValor(op);
  int cv = escuro ? 70 : (podeMudar ? 178 : 132);
  TxtLinha val = txt_linha(TXT_BODY, v, cv, cv, cv, 255);
  float xDir = AJ_LISTA_X + AJ_LISTA_W - AJ_PAD;
  float vy = y + (AJ_LINHA_H - val.h) * 0.5f;

  // Barra de preenchimento da linha numerica. Sem ela, "28%" nao diz nada sobre
  // onde 28 fica no intervalo — e o web mostra um slider justamente por isso.
  if (OPCOES[op].tipo == OP_NUMERO) {
    const Opcao *o = &OPCOES[op];
    float t = (o->max > o->min)
            ? (float)(valor[op] - o->min) / (float)(o->max - o->min) : 0.0f;
    float bw = 220.0f, bh = 6.0f;
    float bx = xDir - val.w - 24.0f - bw;
    float by = y + (AJ_LINHA_H - bh) * 0.5f;
    GfxRect trilho = { bx, by, bw, bh };
    GfxRect cheio  = { bx, by, bw * anim_clamp(t, 0.0f, 1.0f), bh };
    gfx_cor(trilho, 0.5f, escuro ? 0.10f : 1.0f, escuro ? 0.10f : 1.0f,
            escuro ? 0.10f : 1.0f, 0.22f * aTexto);
    if (cheio.w > 0.5f)
      gfx_cor(cheio, 0.5f, escuro ? 0.10f : 1.0f, escuro ? 0.10f : 1.0f,
              escuro ? 0.10f : 1.0f, 0.92f * aTexto);
  }

  // As setas so aparecem na linha em foco que MUDA. Elas sao a instrucao: sem
  // elas, nada na tela diz que esquerda/direita e o gesto certo.
  if (podeMudar && f > 0.02f) {
    TxtLinha dir = txt_linha(TXT_CAPTION2, "\xe2\x96\xb6", cv, cv, cv, 255);
    TxtLinha esq = txt_linha(TXT_CAPTION2, "\xe2\x97\x80", cv, cv, cv, 255);
    txt_desenhar_alpha(dir, xDir - dir.w, y + (AJ_LINHA_H - dir.h) * 0.5f, aTexto * f);
    txt_desenhar_alpha(val, xDir - dir.w - 16.0f - val.w, vy, aTexto);
    txt_desenhar_alpha(esq, xDir - dir.w - 16.0f - val.w - 16.0f - esq.w,
                       y + (AJ_LINHA_H - esq.h) * 0.5f, aTexto * f);
  } else {
    txt_desenhar_alpha(val, xDir - val.w, vy, aTexto);
  }
}

void ajustes_desenhar(Uint32 agora) {
  (void)agora;
  // Fundo opaco proprio: a tela cobre tudo e nao pode depender de quem desenhou
  // antes dela — sem isto a home aparece entre as linhas da lista.
  GfxRect tela = { 0, 0, NV_TELA_W, NV_TELA_H };
  gfx_cor(tela, 0.0f, NV_COR_FUNDO_R, NV_COR_FUNDO_G, NV_COR_FUNDO_B, 1.0f);

  TxtLinha tit = txt_linha(TXT_TITULO1, "Ajustes", 255, 255, 255, 255);
  txt_desenhar(tit, AJ_LISTA_X, NV_MARGEM_Y);

  float y = AJ_TOPO - scrollY;
  for (int s = 0; s < AJ_N_SECOES; s++) {
    if (s) y += AJ_SEC_GAP;
    // Cabecalho da secao em corpo pequeno e cinza: ele rotula o grupo, nao
    // compete com os rotulos das opcoes.
    float aC = anim_clamp((y - (AJ_TOPO - 70.0f)) / 60.0f, 0.0f, 1.0f);
    TxtLinha ts = txt_linha(TXT_CAPTION, SECOES[s].titulo, 150, 152, 160, 255);
    if (aC > 0.005f && y < AJ_BASE)
      txt_desenhar_alpha(ts, AJ_LISTA_X + AJ_PAD, y + AJ_SEC_CABEC - ts.h - 10.0f, aC);
    y += AJ_SEC_CABEC;
    for (int k = 0; k < SECOES[s].n; k++) {
      int op = SECOES[s].ini + k;
      desenhaLinha(op, y, animFoco[op]);
      y += AJ_LINHA_H + AJ_LINHA_GAP;
    }
  }
}
