// Tela de reproducao, no formato do NOSSO APP WEB.
//
// A referencia mudou: esta variante legacy segue o player do app web (o bloco
// #playerUiRoot em css/components.css), nao o do app da Apple TV que o
// prototipo nuvio-native desenha. O que veio de la e a MECANICA — mola de
// foco, auto-esconder, furo do pipeline — porque essa parte nao e questao de
// estilo. O arranjo e as medidas sao do web, anotadas uma a uma abaixo.
//
// Diferencas concretas em relacao ao que estava aqui: os botoes ficam a
// ESQUERDA e nao centralizados; o tempo e UM rotulo "decorrido / total" na
// ponta direita e nao dois com restante negativo; o subtitulo fica ABAIXO do
// titulo; a barra tem 6px e nao 8, sem marcador na cabeca; as tres pilulas
// informativas ("Informacoes", "Em Foco", "Continue Assistindo") sairam, que
// sao mobiliario do app da Apple e nao existem no nosso.
//
// Sao tres comportamentos observados no aparelho, e cada um deles muda o
// desenho inteiro:
//
//   1. Enquanto toca, a tela e SO o quadro. Zero interface. Nenhuma barra
//      residual, nenhum relogio de canto — o que aparece por cima da imagem
//      quando ninguem pediu e ruido.
//   2. Qualquer direcao no D-pad SOBE os controles pela base. Eles nao piscam
//      para dentro: entram com mola, deslizando de baixo, junto com o veu.
//   3. Parado alguns segundos, eles somem sozinhos — mas nao enquanto o video
//      esta pausado. Pausado sem controles o usuario fica olhando um quadro
//      congelado sem saber o que houve.
#include "player.h"
#include "video.h"
#include "tracks.h"
#include "gfx.h"
#include "text.h"
#include "tex_cache.h"
#include "anim.h"
#include "layout.h"
#include "catalog.h"
#include "trakt.h"
#include "sync.h"
#include "parental.h"
#include "episodes.h"
#include "streams.h"
#include "subtitle.h"
#include "intro.h"
#include "home.h"
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>   // strcasecmp, para comparar o hdrType do pipeline
#include <math.h>

// Quanto tempo os controles ficam de pe sem receber tecla. Medido a olho no
// aparelho: perto de 4s. Menos que isso e o usuario perde a barra no meio de
// uma leitura; muito mais e a interface some tarde demais e atrapalha a cena.
#define PLR_HIDES_MS   4000u
// Salto de 10s do avanca/retrocede. E o passo do controle da Apple, e ele so
// vale com os controles em pe: cegamente, seta seria um pulo invisivel.
#define PLR_JUMP_SEG    10.0f
// Duracao de reserva, em segundos, para quando o `meta` do catalogo nao traz
// tempo de filme (as series trazem "3 temporadas", que nao e duracao de nada).
// 1h54 e so um numero plausivel para o layout ter o que mostrar — assim que o
// video real entrar, a duracao vem do decodificador e esta constante morre.
#define PLR_DURATION_DFLT   (114.0f * 60.0f)
// Geometria do bloco de controles, de baixo para cima. Tudo ancorado na BASE
// da tela: e ela que nao se mexe quando o bloco desliza para dentro.
// ---------------------------------------------------------------------------
// MEDIDAS DO PLAYER DO APP WEB
//
// Esta tela nao segue mais o player do app da Apple: segue o nosso app web, que
// e a referencia desta variante legacy. Os valores sao os do CSS resolvidos em
// 1920x1080, que e onde o app roda — no arquivo eles sao min(Xvw, Ypx) e a TV
// cai sempre no teto. A origem de cada um esta anotada para poder conferir.
//
//   #playerUiRoot        --player-controls-x/y      64 / 48
//   .player-control-btn  --player-control-size      96   (gap 4px)
//   .player-progress-track  height 6 -> 10 com foco, radius 3
//   .player-progress-shell  margin-top 12
//   .player-controls-row    margin-top 16
//   .player-controls-gradient-top/bottom   150 / 200
//
// MAS ESSES SAO OS VALORES BASE, E NAO OS DESTA TELA. O bloco `#playerUiRoot`
// (components.css:15251) e o port do player do Android TV e refaz quase todos
// com a conversao x2 que o repositorio usa para o canvas de 1920 ("ATV 6dp ->
// 12px"). O que estava aqui era metade do tamanho certo em quase tudo — a
// barra, o vao dos botoes, o respiro da fileira e os dois degrades. Os que o
// bloco ATV NAO refaz (padding 64/48, margin-top 12 da barra) ficam como estao.
//
//   .player-progress-track  12 -> 20 com foco, radius 6
//   .player-control-buttons gap 8
//   .player-controls-row    margin-top 32
//   .player-control-icon    48
//   gradientes              300 (topo) / 400 (base)
#define PLR_DFLT_X         64.0f
#define PLR_DFLT_Y         48.0f
// Margem lateral do CONTEUDO do rodape (titulo, botoes, relogio). O trilho da
// barra continua em 0..largura; so o conteudo recua, para nao cair na zona que
// a TV corta por overscan. Mesmo valor do gutter da pagina de titulo.
#define PLR_MARGIN        96.0f
#define PLR_BTN_D         76.0f
#define PLR_BTN_GAP        8.0f
// 12px em repouso, 20px com foco — as duas do bloco ATV. A barra PASSOU a receber
// foco (CIMA a partir da fileira de botoes); antes so os botoes recebiam, e por
// isso nao havia como procurar no filme pela barra.
// BARRA MINIMALISTA, DE PONTA A PONTA. Era 12px de altura com 64px de margem
// de cada lado e raio 6 — e o raio era o defeito: nesta API ele e FRACAO do
// menor lado (ver gfx.h), no maximo 0.5, entao 6.0 degenerava o SDF. O efeito
// era o preenchimento inicial virar uma bolha em vez de uma barra crescendo, e
// so "aparecer" depois de muitos minutos de filme, quando ja era largo o
// bastante para a forma se resolver. Foi o que o dono descreveu: "demora muito
// para mostrar ela encher, nao ta bem calibrada".
//
// Agora e um fio reto de canto vivo (raio 0), colado nas bordas da tela. Sem
// raio nao ha SDF para degenerar e o primeiro pixel de progresso ja aparece.
#define PLR_RAIL_H       4.0f
// 20px com foco (`min(1.04vw, 20px)` em .player-progress-shell.focused).
#define PLR_RAIL_H_FOCUS  8.0f
#define PLR_RAIL_R       0.0f   // canto vivo: ver a nota acima
#define PLR_GAP_BAR     12.0f   // meta -> barra
#define PLR_GAP_ROW       32.0f   // barra -> fileira de botoes
#define PLR_GRADIENT_BOTTOM   400.0f
#define PLR_GRADIENT_TOP    300.0f
// #f5f5f5 = --secondary-color, que e o que preenche a barra no web.
#define PLR_FILL_C      (245.0f / 255.0f)

#define PLR_ICON_H       48.0f
// De quanto o bloco desliza para baixo quando escondido. Pequeno de proposito:
// o que faz o movimento ser lido nao e a distancia, e a mola somada ao fade.
#define PLR_SLIDE       46.0f
// Guia parental (.player-parental-*): barra de 6, lista recuada 20, linha de
// 36 com 4 de vao. Nao passam pela conversao x2 do bloco ATV — a regra base
// nao e refeita la.
#define PG_BAR_W         6.0f
// Quanto tempo a guia parental fica na tela, contando do primeiro quadro com
// imagem, e quanto dura o esmaecimento final. Sete segundos e o bastante para
// ler quatro linhas curtas sem virar mobilia — depois disso ela nao volta nesta
// reproducao.
#define PG_SEG_TOTAL       7.0f
#define PG_SEG_OUTPUT       0.8f
#define PG_LIST_PADX     20.0f
#define PG_LINE_H        36.0f
#define PG_LINE_GAP       4.0f
// O veu virou os dois degrades do web (PLR_GRAD_TOPO/BAIXO). Ele existe para o
// texto ler sobre a imagem — sem ele, uma cena clara apaga o nome do titulo.

// Transporte compacto. Os saltos continuam acessiveis pelas setas na barra.
enum { PLR_PLAY, PLR_ASPECT, PLR_CC, PLR_AUDIO,
       PLR_SOURCES, PLR_EPISODES, PLR_NBTNS };

static int   is_open = 0, exiting = 0, requestedExit = 0;
static int   idx = 0;
static int   playing = 1;
// Botao em foco na fileira de transporte. Comeca no PLAY porque e a resposta
// que nove de cada dez aberturas quer: o dedo para no centro e o OK decide.
static int   button = PLR_PLAY;
// A barra de progresso e um alvo de foco, como no web: `.player-progress-shell`
// engorda de 6 para 10px e clareia o trilho quando focada. Fica FORA do enum
// dos botoes porque nao e um botao — o OK nela nao 'aperta' nada, e o
// ESQUERDA/DIREITA muda de significado (procura, em vez de trocar de foco).
static int   barFocus = 0;
static int   visible = 0;          // alvo dos controles (1 = em pe)
static float anim = 0.0f;          // 0..1 seguindo `visivel`, por mola
static float focusB[PLR_NBTNS];     // mola de foco de cada botao
static float entry = 0.0f;       // 0..1 fade de abertura/fechamento da tela
static Uint32 lastInput = 0;
// Instante em que a IMAGEM comecou (nao a abertura da tela: entre uma coisa e
// outra ha a busca de fonte, que pode levar segundos). Zero enquanto nao houve.
// A guia parental se apoia nisto para aparecer UMA vez, no comeco, e sumir.
static Uint32 startImage = 0;
// AS DUAS VARIAVEIS DE MIDIA. Todo o resto do arquivo le so daqui — quando o
// video real entrar, sao elas que passam a ser preenchidas pelo decodificador.
static int   comVideo = 0;
static int   reqTracks = 0;
static int   waitingSource = 0;   // aberto sem URL, esperando o addon responder
static float posSeg = 0.0f;
static float durationSeg = PLR_DURATION_DFLT;

static char lineEp[220];          // "T1, E1 · <sinopse curta>", montada na abertura

static const CatItem *item(void) { return cat_item(idx); }
static int epT, epE, reqSources, errorSource, reqNextT, reqNextE;
static int introIdx=-1, introT=-1, introE=-1;
static int resumeApplied, resumePct;
int player_index(void) { return idx; }
const char *player_line_episode(void) { return lineEp; }
void player_episode_current(int *t, int *e) { *t = epT; *e = epE; }
int player_requested_sources(void) { int p = reqSources; reqSources = 0; return p; }
int player_requested_next(int *t,int *e) {
  if(!reqNextT||!reqNextE)return 0;
  if(t)*t=reqNextT;if(e)*e=reqNextE;reqNextT=reqNextE=0;return 1;
}
const CatEp *player_next_episode(void) {
  const CatEp *best=NULL;
  for(int i=0;i<cat_n_episodes(idx);i++) {
    const CatEp *p=cat_episode(idx,i);if(!p)continue;
    if(p->season<epT||(p->season==epT&&p->episode<=epE))continue;
    if(!best||p->season<best->season||
       (p->season==best->season&&p->episode<best->episode))best=p;
  }
  return best;
}
void player_error_source(void) { waitingSource = 0; errorSource = 1; visible = 1; playing = 0; }
void player_set_episode(int t, int e) {
  const CatItem *c = item();
  epT = t; epE = e; lineEp[0] = 0;
  resumePct = 0;
  if (c && c->progress > 0 && c->progress < 90 &&
      (strcmp(c->kind,"series") || (t==c->season && e==c->episode))) resumePct=c->progress;
  if (!c || strcmp(c->kind, "series")) { epT = epE = 0; intro_off(); return; }
  if (epT < 1) epT = c->season > 0 ? c->season : 1;
  if (epE < 1) epE = c->episode > 0 ? c->episode : 1;
  snprintf(lineEp, sizeof lineEp, "T%dE%d", epT, epE);
  if (epT == c->season && epE == c->episode && c->nameEpisode[0])
    snprintf(lineEp, sizeof lineEp, "T%dE%d · %s", epT, epE, c->nameEpisode);
  for (int i = 0; i < cat_n_episodes(idx); i++) {
    const CatEp *ep = cat_episode(idx, i);
    if (ep && ep->season == epT && ep->episode == epE) {
      snprintf(lineEp, sizeof lineEp, "T%dE%d · %s", epT, epE, ep->name);
      break;
    }
  }
  if(idx!=introIdx||epT!=introT||epE!=introE){
    introIdx=idx;introT=epT;introE=epE;intro_request(c->imdb,epT,epE);
  }
}

// --- duracao a partir do texto livre do catalogo -----------------------------
// O campo `meta` e prosa, nao dado: "2023 · 3 h 28 min" num filme e
// "2022 · 3 temporadas" numa serie. Em vez de um parser posicional (que quebra
// no primeiro titulo com formato diferente), procuro apenas os dois pares
// numero+unidade em qualquer lugar da string. Nao achando NENHUM dos dois,
// devolvo 0 e quem chama cai no padrao — que e o caso correto para series.
static float durationOfMeta(const char *meta) {
  if (!meta) return 0.0f;
  float h = 0.0f, m = 0.0f;
  int found = 0;
  for (const char *p = meta; *p; p++) {
    if (*p < '0' || *p > '9') continue;
    float v = 0.0f;
    while (*p >= '0' && *p <= '9') { v = v * 10.0f + (*p - '0'); p++; }
    while (*p == ' ') p++;
    // "min" tem que ser testado ANTES de "m": senao todo "min" vira minuto por
    // acidente do prefixo — o que ate daria certo aqui, mas escondia o bug do
    // dia em que aparecer uma unidade nova comecando com m.
    if (!strncmp(p, "min", 3))    { m = v; found = 1; p += 2; }
    else if (*p == 'h')           { h = v; found = 1; }
    if (!*p) break;
  }
  return found ? (h * 3600.0f + m * 60.0f) : 0.0f;
}

// Corta a sinopse na primeira frase, sem passar de `maxBytes`. O corte respeita
// UTF-8: os titulos do catalogo sao em portugues e cortar no meio de um "ç" ou
// "ã" produz um retangulo vazio na fonte, nao um acento faltando.
static void fraseFirst(char *dst, size_t n, const char *src, size_t maxBytes) {
  if (!src || !*src) { dst[0] = 0; return; }
  if (maxBytes > n - 4) maxBytes = n - 4;
  size_t i = 0, cut = 0;
  for (; src[i] && i < maxBytes; i++)
    if (src[i] == '.') { cut = i; break; }
  if (!cut) {
    cut = i;
    // volta ate o inicio de um caractere (bytes de continuacao sao 10xxxxxx)
    while (cut > 0 && ((unsigned char)src[cut] & 0xC0) == 0x80) cut--;
    while (cut > 0 && src[cut - 1] == ' ') cut--;
  }
  memcpy(dst, src, cut);
  dst[cut] = 0;
  if (src[i] && src[i] != '.') strncat(dst, "\xe2\x80\xa6", n - strlen(dst) - 1);
}

// --- MODOS DE PROPORCAO ------------------------------------------------------
// A porta do web para o nativo. No web o modo mexe em duas coisas do elemento
// <video>: o `object-fit` e um `transform: scale()`. Aqui nao ha elemento — ha
// um plano de hardware posicionado por video_janela() — entao os dois viram UMA
// coisa so: o retangulo do plano.
//
// A traducao e literal e nesta ordem, igual ao resolveAspectRender do web:
//   1. o retangulo que o object-fit do modo produziria (contain/cover/fill);
//   2. multiplicado pela escala do modo (resolveAspectScale), em torno do
//      CENTRO da tela — que e o `transform-origin: center center` de la.
// O retangulo aqui e VIRTUAL: ele pode sair da tela, e sair da tela e o que
// significa "recortar". Mas ele NAO e o que se manda ao plano — ver
// aplicarAspecto, que o converte em fonte + destino.
//
// ERRO MEDIDO, e vale ficar escrito porque a leitura do web induz a ele: eu
// mandava este retangulo direto ao ACB, com x/y negativos e tamanho maior que a
// tela. O ACB aceitou as quatro chamadas sem reclamar e o log ficou bonito —
//   [video] janela -144,-81  2208x1242 cheia=0   <- Zoom leve   (1.15)
//   [video] janela -326,-184 2573x1447 cheia=0   <- Zoom cinema (1.34)
//   [video] janela -528,-297 2976x1674 cheia=0   <- Zoom ultra  (1.55)
// — batendo ate o pixel com o resolveAspectRender do web. E a TELA FICOU PRETA
// em todos os tres. Aceitar a chamada nao e exibir: um plano de hardware nao
// descarta o excedente como o compositor do navegador faz com transform:
// scale(), entao retangulo fora do painel nao vira recorte, vira retangulo
// invalido e o plano apaga. So o ORIGINAL mostrava imagem, por ser o unico com
// escala 1. A licao: `resolveAspectScale` era justamente a parte do web que NAO
// se traduz, porque a metade que fazia o recorte no web nem esta no arquivo.
//
// NAO da para conferir isto por captura de tela: durante a reproducao o
// /tmp/nuvio-shot.bmp sai PRETO onde esta o video, porque o plano fica atras da
// superficie GL e o glReadPixels nao o enxerga. E foi essa cegueira que deixou
// o erro passar — o log dizia sucesso, a captura era preta de qualquer jeito, e
// so quem olhou a TV viu. Conferir zoom exige olhar o aparelho.
static int    aspect = PLR_ASPECT_ORIGINAL;
static Uint32 toastAte = 0;      // ate quando o aviso de modo fica de pe
static char   dirPrefs[512];

// Rotulos em portugues. Os do web sao "Fit (Original)", "Crop", "Stretch",
// "Slight/Cinema/Ultra Zoom", "Fit Height", "Fit Width" — o resto do app fala
// portugues, entao traduzir aqui e o que mantem a tela coerente.
static const char *ASPECT_LABEL[PLR_ASPECT_N] = {
  "Original", "Crop", "Stretch", "Light zoom",
  "Cinema zoom", "Ultra zoom", "Fit height", "Fit width"
};

const char *player_aspect_label(int mode) {
  if (mode < 0 || mode >= PLR_ASPECT_N) mode = PLR_ASPECT_ORIGINAL;
  return ASPECT_LABEL[mode];
}
int player_aspect(void) { return aspect; }

// Onde o modo escolhido fica gravado. Mesmo diretorio que o main.c passa para o
// resto do app (SDL_GetBasePath()+"art", com /tmp/art de reserva). O web guarda
// isso em DeviceLocalPlayerPreferences, por aparelho: escolher "Zoom cinema" e
// reencontrar "Original" no filme seguinte transformaria o modo em brinquedo.
static const char *prefsFile(void) {
  static char path[600];
  if (!dirPrefs[0]) {
    char *base = SDL_GetBasePath();
    if (base) { snprintf(dirPrefs, sizeof dirPrefs, "%sart", base); SDL_free(base); }
    else      snprintf(dirPrefs, sizeof dirPrefs, "/tmp/art");
  }
  snprintf(path, sizeof path, "%s/player.txt", dirPrefs);
  return path;
}

// ESTILO DA LEGENDA: preferencia DO APARELHO, como o aspecto — nao vai em
// ajustes.txt, que espelha as chaves de layout do app web. Padrao: tamanho 2
// (o do aparelho), branco, sem fundo, posicao central, contorno.
static VideoSubtitleStyle subStyle = { 120, 0, 0, 3, 1, 0, 0, TXT_FAMILY_INTER };

// Keys written by 1.0.1 and earlier. Reading them keeps the device's aspect
// and subtitle style across the rename instead of silently resetting to the
// defaults; the file is rewritten with the new names on the next change.
static const char *canonicalKey(const char *k) {
  static const struct { const char *old, *new; } T[] = {
    { "aspect",       "aspect"         }, { "leg_tamanho", "sub_size"     },
    { "leg_cor",       "sub_color"      }, { "leg_fundo",   "sub_background" },
    { "leg_pos",       "sub_position"   }, { "leg_borda",   "sub_border"   },
    { "leg_atraso",    "sub_delay"      }, { "leg_opacidade","sub_opacity" },
    { "leg_familia",   "sub_family"     },
  };
  size_t i;
  for (i = 0; i < sizeof T / sizeof *T; i++)
    if (!strcmp(k, T[i].old)) return T[i].new;
  return k;
}

static void prefsRead(void) {
  FILE *f = fopen(prefsFile(), "r");
  char raw[64]; const char *key; int v;
  if (!f) return;
  while (fscanf(f, "%63s %d", raw, &v) == 2) {
    key = canonicalKey(raw);
    // Valor de outra versao (ou arquivo editado a mao) cai no padrao em vez de
    // indexar fora do vetor de rotulos.
    if (!strcmp(key, "aspect") && v >= 0 && v < PLR_ASPECT_N) aspect = v;
    else if (!strcmp(key, "sub_size")) {
      /* Migra o arquivo antigo 0..4 sem perder a preferencia do aparelho. */
      static const int old[5]={60,80,120,160,200};
      if(v>=0&&v<=4)subStyle.size=old[v];
      else if(v>=50&&v<=200)subStyle.size=(v/10)*10;
    }
    else if (!strcmp(key, "sub_color")     && v >= 0 && v < VIDEO_SUB_NCOLORS) subStyle.color = v;
    else if (!strcmp(key, "sub_background")   && v >= 0 && v <= 4)  subStyle.background = v;
    else if (!strcmp(key, "sub_position")     && v >= 0 && v <= 7)  subStyle.position = v;
    else if (!strcmp(key, "sub_border")   && v >= 0 && v <= 2)  subStyle.border = v;
    else if (!strcmp(key, "sub_delay")  && v > -10000 && v < 10000) subStyle.delayMs = v;
    else if (!strcmp(key, "sub_opacity") && v >= 0 && v <= 3) subStyle.opacity = v;
    else if (!strcmp(key, "sub_family") && v >= 0 && v < TXT_FAMILY_N) subStyle.family = v;
  }
  fclose(f);
}

static void prefsWrite(void) {
  FILE *f = fopen(prefsFile(), "w");
  if (!f) return;
  fprintf(f, "aspect %d\n", aspect);
  fprintf(f, "sub_size %d\n", subStyle.size);
  fprintf(f, "sub_color %d\n",     subStyle.color);
  fprintf(f, "sub_background %d\n",   subStyle.background);
  fprintf(f, "sub_position %d\n",     subStyle.position);
  fprintf(f, "sub_border %d\n",   subStyle.border);
  fprintf(f, "sub_delay %d\n",  subStyle.delayMs);
  fprintf(f, "sub_opacity %d\n", subStyle.opacity);
  fprintf(f, "sub_family %d\n", subStyle.family);
  fclose(f);
}

// Lidos pela folha de faixas, que e quem desenha os controles.
VideoSubtitleStyle *player_sub_style(void) { return &subStyle; }
void player_sub_style_changed(void) {
  video_subtitle_style(&subStyle);
  prefsWrite();
}

// Proporcao do QUADRO decodificado. Sem videoInfo ainda, 16:9 — que e a
// proporcao de quase todo arquivo entregue, e a suposicao que faz "Original"
// abrir em tela cheia em vez de piscar uma faixa errada por um segundo.
static float aspectFrame(void) {
  int w = video_width(), h = video_height();
  if (w > 0 && h > 0) return (float)w / (float)h;
  return NV_SCREEN_W / NV_SCREEN_H;
}

typedef struct { float x, y, w, h; } PlrRect;

static PlrRect aspectRect(int mode) {
  const float screen = NV_SCREEN_W / NV_SCREEN_H;
  float q = aspectFrame();
  float bw, bh, sx = 1.0f, sy = 1.0f;
  PlrRect r;
  if (q <= 0.0f) q = screen;

  // 1) o object-fit do modo. Os tres casos sao os do ASPECT_MODE_DEFINITIONS.
  switch (mode) {
    case PLR_ASPECT_STRETCH:                       // fill
      bw = NV_SCREEN_W; bh = NV_SCREEN_H;
      break;
    case PLR_ASPECT_CROP:                          // cover
    case PLR_ASPECT_ZOOM_LIGHT:
    case PLR_ASPECT_ZOOM_CINEMA:
    case PLR_ASPECT_FIT_HEIGHT:
      if (q > screen) { bh = NV_SCREEN_H; bw = bh * q; }
      else          { bw = NV_SCREEN_W; bh = bw / q; }
      break;
    default:                                    // contain
      if (q > screen) { bw = NV_SCREEN_W; bh = bw / q; }
      else          { bh = NV_SCREEN_H; bw = bh * q; }
      break;
  }

  // 2) a escala do modo, copiada linha a linha do resolveAspectScale.
  switch (mode) {
    case PLR_ASPECT_CROP:        sx = sy = (q > screen) ? q / screen : screen / q; break;
    case PLR_ASPECT_STRETCH:     if (q > screen) sy = q / screen; else sx = screen / q; break;
    case PLR_ASPECT_ZOOM_LIGHT:   sx = sy = PLR_ZOOM_LIGHT;   break;
    case PLR_ASPECT_ZOOM_CINEMA: sx = sy = PLR_ZOOM_CINEMA; break;
    case PLR_ASPECT_ZOOM_ULTRA:  sx = sy = PLR_ZOOM_ULTRA;  break;
    case PLR_ASPECT_FIT_HEIGHT:  if (q > screen) sx = sy = q / screen; break;
    case PLR_ASPECT_FIT_WIDTH: if (q < screen) sx = sy = screen / q; break;
    default: break;   // ORIGINAL: contain e nada mais
  }

  r.w = bw * sx;
  r.h = bh * sy;
  r.x = (NV_SCREEN_W - r.w) * 0.5f;
  r.y = (NV_SCREEN_H - r.h) * 0.5f;
  return r;
}

// O retangulo VISIVEL do modo: o retangulo virtual cortado pela tela. E ele que
// o furo do GL segue e que vira o destino do plano.
static PlrRect aspectVisible(int mode) {
  PlrRect r = aspectRect(mode), d;
  d.x = r.x < 0.0f ? 0.0f : r.x;
  d.y = r.y < 0.0f ? 0.0f : r.y;
  d.w = (r.x + r.w > NV_SCREEN_W ? NV_SCREEN_W : r.x + r.w) - d.x;
  d.h = (r.y + r.h > NV_SCREEN_H ? NV_SCREEN_H : r.y + r.h) - d.y;
  if (d.w < 0.0f) d.w = 0.0f;
  if (d.h < 0.0f) d.h = 0.0f;
  return d;
}

// Manda o modo ao plano de hardware. Chamado na abertura, na troca de modo e
// quando o videoInfo chega — antes dele a proporcao do quadro e chute, e o modo
// calculado com o chute estaria errado justamente nos filmes widescreen, que
// sao o motivo de tudo isto existir.
//
// AQUI ESTAVA O ERRO que deixava a tela preta em todo modo com zoom. Eu mandava
// o retangulo VIRTUAL direto ao plano — com x/y negativos e tamanho maior que a
// tela — na suposicao de que o excedente sairia pela borda, como sai no web. No
// web quem descarta o excedente e o compositor do navegador; um plano de
// hardware nao tem esse passo, e retangulo fora do painel nao e recorte, e
// retangulo invalido: o plano apaga. So o ORIGINAL sobrevivia, por ser o unico
// com escala 1.
//
// A conta certa e a INVERSA: o destino nunca sai da tela, e o zoom vira um
// pedaco MENOR da FONTE. O retangulo virtual continua sendo o mesmo do web —
// ele so deixa de ser o que se manda e passa a ser o que se USA PARA CALCULAR
// que fatia do quadro cai dentro da tela.
static void applyAspect(void) {
  PlrRect r, d;
  float qw, qh;
  int sx, sy, sw, sh;
  if (!comVideo) return;

  r = aspectRect(aspect);
  d = aspectVisible(aspect);
  if (d.w < 1.0f || d.h < 1.0f || r.w < 1.0f || r.h < 1.0f) return;

  qw = (float)video_width();
  qh = (float)video_height();
  // Sem as dimensoes do quadro nao da para falar em coordenadas de fonte. Cai
  // no caminho antigo, que serve ao caso sem recorte — e o unico em que ele
  // funciona. Assim que o videoInfo chegar, aplicarAspecto roda de novo.
  if (qw < 2.0f || qh < 2.0f) {
    video_window((int)(d.x + 0.5f), (int)(d.y + 0.5f),
                 (int)(d.w + 0.5f), (int)(d.h + 0.5f));
    return;
  }

  // Que fatia do quadro cai dentro do destino: o quadro inteiro mapeia no
  // retangulo virtual `r`, entao a fatia e a regra de tres de `d` dentro de `r`.
  sx = (int)((d.x - r.x) / r.w * qw + 0.5f);
  sy = (int)((d.y - r.y) / r.h * qh + 0.5f);
  sw = (int)(d.w / r.w * qw + 0.5f);
  sh = (int)(d.h / r.h * qh + 0.5f);
  // Par: o escalonador trabalha em 4:2:0 e origem ou tamanho impar em croma da
  // meio pixel de deslocamento de cor na borda do recorte.
  sx &= ~1; sy &= ~1; sw &= ~1; sh &= ~1;
  if (sx < 0) sx = 0;
  if (sy < 0) sy = 0;
  if (sx + sw > (int)qw) sw = (int)qw - sx;
  if (sy + sh > (int)qh) sh = (int)qh - sy;

  video_window_source(sx, sy, sw, sh,
                     (int)(d.x + 0.5f), (int)(d.y + 0.5f),
                     (int)(d.w + 0.5f), (int)(d.h + 0.5f));
}

void player_aspect_set(int mode) {
  if (mode < 0 || mode >= PLR_ASPECT_N) mode = PLR_ASPECT_ORIGINAL;
  aspect = mode;
  prefsWrite();
  applyAspect();
}

void player_aspect_cycle(void) {
  player_aspect_set((aspect + 1) % PLR_ASPECT_N);
  toastAte = SDL_GetTicks() + PLR_TOAST_MS;
}

void player_open(int indexCatalog, const char *url) {
  int n = cat_n(); if (n < 1) n = 1;
  idx = ((indexCatalog % n) + n) % n;
  is_open = 1; exiting = 0; requestedExit = 0; barFocus = 0;
  // Guia parental do titulo: pedido AQUI e nao no desenho, para que a resposta
  // ja tenha chegado quando os controles aparecerem pela primeira vez.
  { const CatItem *ci = cat_item(idx);
    if (ci && ci->imdb[0]) parental_request(ci->imdb); }
  playing = 1; visible = 1; anim = 0.0f; entry = 0.0f;
  reqSources = errorSource = reqTracks = reqNextT = reqNextE = 0; startImage = 0;
  resumeApplied=0;
  button = PLR_PLAY;
  memset(focusB, 0, sizeof focusB);
  posSeg = 0.0f;
  lastInput = SDL_GetTicks();
  waitingSource = (url == NULL);
  // Legenda externa e da sessao que acabou, nao desta.
  tracks_reset();
  // O modo de proporcao e do APARELHO, nao da sessao: reler aqui e o que faz
  // "Zoom cinema" continuar valendo no filme seguinte, como no web.
  prefsRead();
  toastAte = 0;
  comVideo = (url && *url && video_play(url));
  applyAspect();

  const CatItem *c = item();
  float d = c ? durationOfMeta(c->meta) : 0.0f;
  durationSeg = d > 1.0f ? d : PLR_DURATION_DFLT;

  // Identidade do episodio e independente do foco no painel de navegacao.
  player_set_episode(c ? c->season : 0, c ? c->episode : 0);
  if (url && *url && !comVideo) player_error_source();
}

int player_is_open(void)    { return is_open; }
int player_wants_exit(void) { return requestedExit; }
// So depois do loadCompleted. Antes disso o pipeline ainda nao pos nada no
// plano de hardware, e furar a superficie cedo trocava a arte por um retangulo
// PRETO enquanto o fluxo abria — que era o "clica em reproduzir e fica preto".
void player_set_source(const char *url) {
  if (!is_open || !url || !*url) return;
  waitingSource = 0;
  errorSource = 0;
  comVideo = video_play(url);
  if (!comVideo) player_error_source();
  applyAspect();
}

// Consome o pedido de abrir a folha de faixas: quem le, zera.
int  player_requested_tracks(void) { int v = reqTracks; reqTracks = 0; return v; }

int  player_com_video(void) { return comVideo && video_ready(); }

// Esta abrindo o fluxo: ha video pedido, mas ainda nao ha imagem.
int  player_loading(void) { return waitingSource || (comVideo && !video_ready()); }
int  player_controls_visible(void) { return visible; }

void player_shutdown(void) {
  // Salvar ANTES de parar: video_parar descarrega o pipeline e a posicao some
  // junto. Titulo quase no fim conta como visto por inteiro — voltar a um card
  // marcando "2 min restantes" que na verdade acabou e pior que arredondar.
  if (comVideo && video_ready() && durationSeg > 1.0f) {
    float pos = posSeg >= durationSeg - 60.0f ? durationSeg : posSeg;
    const CatItem *ci = cat_item(idx);
    home_registrar_return(idx, pos, durationSeg);
    cat_save_progress_ep(idx, pos, durationSeg,epT,epE);
    // E tambem para o Trakt, que e de onde o "continue assistindo" vem: gravar
    // so aqui deixaria este app discordando dos outros aparelhos do dono.
    if (ci && ci->imdb[0]) {
      char id[64];
      if (epT > 0 && epE > 0) snprintf(id, sizeof id, "%.*s:%d:%d", (int)strcspn(ci->imdb,":"),ci->imdb, epT, epE);
      else snprintf(id, sizeof id, "%s", ci->imdb);
      trakt_mark(id, pos, durationSeg);
      // E para a CONTA. Trakt e conta sao dois destinos diferentes: nem todo
      // usuario liga o Trakt, e o progresso do app oficial vem da conta.
      sync_dirty_progress();
    }
  }
  if (comVideo) video_stop();
  comVideo = 0; waitingSource = 0; is_open = 0; exiting = 0; requestedExit = 0;
  startImage = 0;
  episodes_close();
  intro_off(); introIdx=introT=introE=-1;
  subtitle_off();
}

static int offerNext(void) {
  const CatEp *p=player_next_episode();double end;int kind;
  if(!p||durationSeg<=1)return 0;
  if(intro_active(posSeg,&end,&kind)&&kind==INTRO_CREDITS)return 1;
  return durationSeg-posSeg<=120.0f;
}

// Toda tecla acorda os controles, inclusive a que ja executou alguma acao: no
// aparelho nao existe comando que aconteca com a barra escondida sem trazer a
// barra junto — o usuario precisa ver o efeito do que apertou.
static void wake(void) { visible = 1; lastInput = SDL_GetTicks(); }

static void togglePlaying(void) {
  playing = !playing;
  if (comVideo) video_pause(!playing);
}

// Salto de 10s com limite. So vale com os controles em pe: cegamente, seta
// seria um pulo invisivel — com os botoes, quem aperta esta olhando para um
// botao que diz «10 / 10».
static void jump(int dir) {
  posSeg += dir * PLR_JUMP_SEG;
  posSeg = anim_clamp(posSeg, 0.0f, durationSeg);
  if (comVideo) video_fetch(posSeg);
}

void player_event(const SDL_Event *e) {
  if (!is_open || exiting || e->type != SDL_KEYDOWN) return;
  SDL_Keycode k = e->key.keysym.sym;

  if (k == SDLK_ESCAPE || k == SDLK_AC_BACK || k == SDLK_BACKSPACE ||
      k == SDLK_DELETE) {
    exiting = 1; requestedExit = 1;
    return;
  }

  // CONTROLES ESCONDIDOS: qualquer direcao so acorda a interface. O OK direto
  // pausa/retoma sem navegar nada — e o gesto do aparelho: um toque no centro
  // e o video obedece, sem passos no meio.
  // A TECLA DE PROPORCAO vale sempre, com controles em pe ou escondidos. No web
  // o modo so se troca por um botao dentro de "More Actions" — dois passos com
  // um cursor que aqui nao existe. Numa TV o gesto tem que ser um toque, e o
  // aviso que sobe na troca ja diz em que modo se entrou, entao a tecla nem
  // precisa da interface aberta. O 0 e a tecla livre no controle da LG.
  if (k == SDLK_0 || k == SDLK_KP_0) { player_aspect_cycle(); return; }

  if (!visible) {
    if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) {
      if(offerNext()) { const CatEp*p=player_next_episode();reqNextT=p->season;reqNextE=p->episode;return; }
      { double end;int kind;if(intro_active(posSeg,&end,&kind)&&kind!=INTRO_CREDITS){
          posSeg=(float)end+.25f;if(comVideo)video_fetch(posSeg);return; } }
      togglePlaying(); wake(); return;
    }
    if (k == SDLK_UP || k == SDLK_DOWN || k == SDLK_LEFT || k == SDLK_RIGHT)
      wake();
    return;
  }

  // CONTROLES EM PE: o foco anda pelos botoes e o OK aperta o botao em foco.
  if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) {
    // Na barra o OK pausa/retoma: e o que sobra de util, ja que a barra nao
    // tem acao propria no web.
    if (barFocus) { togglePlaying(); wake(); return; }
    switch (button) {
      case PLR_PLAY:    togglePlaying(); break;
      case PLR_ASPECT: player_aspect_cycle(); break;
      // CC e AUDIO abrem a MESMA folha, mas em colunas diferentes: apertar
      // "legendas" e cair no audio fazia os dois botoes parecerem um so.
      case PLR_CC:      reqTracks = 2;     break;   // 2 = coluna da legenda
      case PLR_SOURCES:  reqSources = 1; break;
      case PLR_EPISODES: if (epT > 0) episodes_open(idx, epT, epE); break;
      default:          reqTracks = 1;     break;   // 1 = coluna do audio
    }
    wake();
    return;
  }
  // CIMA sobe para a BARRA, que no web e um alvo de foco de verdade
  // (`.player-progress-shell.focused` engorda o trilho de 6 para 10px). Sem
  // isso nao havia como adiantar o filme pela barra — so os saltos de 10s dos
  // botoes, que e o defeito que o dono relatou.
  //
  // A folha de faixas NAO se perde: ela continua no CIMA, um nivel acima. Da
  // fileira de botoes o primeiro CIMA pega a barra e o segundo abre a folha.
  // Trocar o gesto por outro (um botao a mais, um menu) seria pior: no aparelho
  // "pra cima revela legendas e audio" e o que a mao ja sabe.
  if (k == SDLK_UP) {
    // Pelo gesto de CIMA a folha abre no AUDIO, que e a coluna que a mao
    // procura mais.
    if (!barFocus) barFocus = 1; else reqTracks = 1;
    wake();
    return;
  }
  if (barFocus) {
    // Na barra, ESQUERDA e DIREITA procuram no filme em vez de trocar de botao.
    if (k == SDLK_LEFT)       jump(-1);
    else if (k == SDLK_RIGHT) jump(1);
    else if (k == SDLK_DOWN)  barFocus = 0;
    wake();
    return;
  }
  if (k == SDLK_DOWN) {
    // BAIXO a partir da fileira significa "tirar os controles da frente".
    // Nao chama acordar(): isso recolocaria a barra no mesmo evento e faria o
    // comando parecer quebrado. O proximo toque direcional a revela de novo.
    barFocus = 0;
    visible = 0;
    lastInput = SDL_GetTicks();
    return;
  }
  // Sem rotacao nas pontas: a fileira e curta e cabe inteira no olhar; dar a
  // volta no fim le como erro, nao como atalho.
  if (k == SDLK_LEFT  && button > 0)          button--;
  else if (k == SDLK_RIGHT && button < PLR_NBTNS - (epT > 0 ? 1 : 2)) button++;
  wake();
}

void player_update(float dt, Uint32 now) {
  if (!is_open) return;

  entry = anim_spring(entry, exiting ? 0.0f : 1.0f, dt, NV_SPRING_SCREEN);
  // Marca o primeiro quadro COM IMAGEM. E daqui que a guia parental conta o
  // tempo dela — contar da abertura da tela faria a guia gastar o prazo
  // enquanto o app ainda procurava fonte, e ela sumiria antes de o filme
  // aparecer.
  if (!startImage && comVideo && video_ready()) { startImage = now; wake(); }
  if (exiting && entry < 0.02f) { is_open = 0; exiting = 0; entry = 0.0f; return; }

  // Havendo pipeline, posicao e duracao vem DELE; o dt so serve para as
  // animacoes. O relogio somado continua existindo para quando nao ha video
  // (no Mac, ou se a fonte falhar): sem ele a barra ficaria parada em zero e a
  // tela mentiria dizendo que nada acontece.
  // O retangulo depende da proporcao do QUADRO, e ela so existe quando o
  // videoInfo chega — segundos depois da abertura. Sem esta releitura o modo
  // ficaria calculado com o chute de 16:9 para sempre, e num arquivo 3840x1606
  // (que e o caso real medido nesta TV) o "Original" cortaria a imagem.
  if (comVideo) {
    static int lastWidth, lastHeight;
    int lw = video_width(), lh = video_height();
    if (lw != lastWidth || lh != lastHeight) { lastWidth = lw; lastHeight = lh; applyAspect(); }
  }

  if (comVideo && video_active()) {
    double d = video_duration();
    posSeg = (float)video_pos();
    if (d > 1.0) durationSeg = (float)d;
    if (!resumeApplied && video_ready() && d>1.0) {
      resumeApplied=1;
      if(resumePct>0) video_fetch(d*resumePct/100.0);
    }
    playing = video_playing();
  } else if (playing && !waitingSource && !errorSource) {
    posSeg += dt;
    if (posSeg >= durationSeg) { posSeg = durationSeg; playing = 0; }
  }

  // Pausado, os controles ficam. Sumir com eles deixaria o usuario diante de um
  // quadro parado sem nenhuma pista de que foi ele quem pausou.
  if (visible && playing && !player_loading() && !episodes_is_open() &&
      !stream_sheet_is_open() && !tracks_is_open() && now - lastInput > PLR_HIDES_MS) visible = 0;
  if (epT > 0 && !strstr(lineEp, " · ")) player_set_episode(epT, epE);

  anim = anim_spring(anim, visible ? 1.0f : 0.0f, dt,
                   visible ? NV_SPRING_FOCUS : NV_SPRING_BLUR);
  for (int i = 0; i < PLR_NBTNS; i++) {
    float target = (visible && button == i) ? 1.0f : 0.0f;
    focusB[i] = anim_spring(focusB[i], target, dt,
                         target > focusB[i] ? NV_SPRING_FOCUS : NV_SPRING_BLUR);
  }
}

// hh:mm:ss so quando passa de uma hora — "0:03:12" num episodio curto le como
// erro de formatacao, nao como tempo.
static void fmtTime(char *b, size_t n, float seg, int negative) {
  if (seg < 0.0f) seg = 0.0f;
  int t = (int)(seg + 0.5f);
  int h = t / 3600, m = (t / 60) % 60, s = t % 60;
  const char *sinal = negative ? "-" : "";
  if (h > 0) snprintf(b, n, "%s%d:%02d:%02d", sinal, h, m, s);
  else       snprintf(b, n, "%s%d:%02d", sinal, m, s);
}

// --- icones -----------------------------------------------------------------
// ARQUIVOS DE VERDADE, de art/icones (os .svg do app web rasterizados a 128px).
// Antes cada glifo era montado com as primitivas — o play de um triangulo, a
// pausa de dois retangulos, a legenda de barras, o aspecto de quatro linhas de
// 3px — e cada um era uma aproximacao do original.
//
// A cor continua vindo daqui: gfx_icone desenha com GFX_MARCA, que tira a forma
// do ALPHA do arquivo, entao o mesmo PNG serve escuro sobre o circulo branco do
// foco e claro sobre o circulo translucido.
//
static void iconFile(float cx, float cy, float a, float luma,
                         const char *name, float size) {
  GfxRect r = { cx - size * 0.5f, cy - size * 0.5f, size, size };
  gfx_icon(r, name, luma, luma, luma, a * 0.94f);
}

static void iconPlayPause(float cx, float cy, float a, int pause, float luma) {
  iconFile(cx, cy, a, luma, pause ? "pause" : "play", PLR_ICON_H * 1.15f);
}

static void iconSubtitles(float cx, float cy, float a, float luma) {
  iconFile(cx, cy, a, luma, "subtitles", PLR_ICON_H * 1.15f);
}

static void iconAudio(float cx, float cy, float a, float luma) {
  iconFile(cx, cy, a, luma, "audio", PLR_ICON_H * 1.15f);
}

static void iconAspect(float cx, float cy, float a, float luma) {
  iconFile(cx, cy, a, luma, "aspect", PLR_ICON_H * 1.15f);
}

// Um botao circular do transporte: translucido quando solto, branco quando em
// foco, e o glifo sempre com o contraste certo contra o fundo dele.
static void buttonCircle(float cx, float cy, float f, float a, int sel) {
  float d = PLR_BTN_D * (1.0f + 0.09f * f);
  GfxRect r = { cx - d * 0.5f, cy - d * 0.5f, d, d };
  if (sel) gfx_color(r, 0.5f, 0.97f, 0.97f, 0.98f, 0.96f * a);
  else     gfx_color(r, 0.5f, 0.05f, 0.05f, 0.06f, 0.42f * a);
}

static void colorSubtitle(int i,int *r,int *g,int *b){
  static const unsigned char c[VIDEO_SUB_NCOLORS][3]={
    {255,255,255},{255,222,48},{64,224,112},{78,156,255},{255,80,80},{18,18,18}};
  if(i<0||i>=VIDEO_SUB_NCOLORS)i=0;*r=c[i][0];*g=c[i][1];*b=c[i][2];
}

/* O uMS da C9 limita fonte e escala. OpenSubtitles passa por este overlay
 * SDL/GLES, exatamente como o overlay HTML do app web. */
static void drawSubtitleExternal(void){
  char text[768],*line,*salva;TxtLine color[4],border[4];int n=0,r,g,b;
  if(!subtitle_text(posSeg,subStyle.delayMs,text,sizeof text))return;
  int pct=subStyle.size;if(pct<50)pct=50;if(pct>200)pct=200;pct=(pct/10)*10;
  TxtStyle st=(TxtStyle)(TXT_SUB_50+(pct-50)/10);colorSubtitle(subStyle.color,&r,&g,&b);
  float alpha=(subStyle.opacity==3?.25f:subStyle.opacity==2?.5f:subStyle.opacity==1?.75f:1.f)*entry;
  line=strtok_r(text,"\n",&salva);
  while(line&&n<4){
    TxtFamily fam=(TxtFamily)subStyle.family;
    color[n]=txt_line_trim_family(st,line,r,g,b,255,1660,fam);
    border[n]=subStyle.border?txt_line_trim_family(st,line,0,0,0,255,1660,fam):(TxtLine){0};
    n++;line=strtok_r(NULL,"\n",&salva);
  }
  if(!n)return;
  float total=0;for(int i=0;i<n;i++)total+=color[i].h+(i?5:0);
  float base=visible?760.f:1000.f;
  if(offerNext())base=690.f;
  base-=(subStyle.position-3)*48.f;
  float y=base-total;
  for(int i=0;i<n;i++){
    TxtLine l=color[i];float x=(NV_SCREEN_W-l.w)*.5f;
    if(subStyle.background){float fa=subStyle.background*.16f*alpha;gfx_color((GfxRect){x-18,y-6,l.w+36,l.h+12},.16f,0,0,0,fa);}
    if(border[i].tex){float d=subStyle.border==2?4.f:2.f;
      txt_draw_alpha(border[i],x+d,y+d,.82f*alpha);
      if(subStyle.border==1){txt_draw_alpha(border[i],x-d,y,.82f*alpha);txt_draw_alpha(border[i],x,y-d,.82f*alpha);}
    }
    txt_draw_alpha(l,x,y,alpha);y+=l.h+5;
  }
}

static void drawActionsEpisode(void){
  const CatEp *next=player_next_episode();double end;int kind=0;
  int chunk=intro_active(posSeg,&end,&kind);
  if(offerNext()&&next){
    GfxRect p={420,720,1080,194};gfx_color(p,.10f,.045f,.045f,.05f,.94f*entry);
    gfx_rect(p,0,GFX_RING,0,.008f,0,.10f,1,1,1,.20f*entry);
    const char *art=next->thumb[0]?next->thumb:(item()&&item()->backdrop[0]?item()->backdrop:NULL);
    if(art){GLuint tx=tex_get_width(art,288);if(tx){gfx_tex_aspect_current=tex_aspect(art);gfx_rect((GfxRect){450,738,288,158},tx,GFX_CARD,0,0,0,.08f,1,1,1,entry);gfx_tex_aspect_current=0;}}
    TxtLine l=txt_line(TXT_PLR_BODY,"Next episode",205,207,213,255);txt_draw_alpha(l,782,752,entry);
    char name[220];snprintf(name,sizeof name,"T%dE%d · %s",next->season,next->episode,next->name);
    TxtLine t=txt_line_trim(TXT_PLR_TITLE,name,250,250,252,255,430);txt_draw_alpha(t,782,794,entry);
    GfxRect bot={1240,775,220,76};gfx_color(bot,.5f,.08f,.08f,.09f,.96f*entry);gfx_rect(bot,0,GFX_RING,0,.018f,0,.5f,1,1,1,.35f*entry);
    gfx_icon((GfxRect){1264,793,40,40},"play",1,1,1,entry);TxtLine rt=txt_line(TXT_BODY,"Play",246,246,248,255);txt_draw_alpha(rt,1310,797,entry);
  } else if(chunk&&kind!=INTRO_CREDITS){
    const char *rot=kind==INTRO_SUMMARY?"Skip recap":"Skip intro";
    TxtLine t=txt_line(TXT_BODY,rot,250,250,252,255);float w=t.w+116;
    GfxRect p={64,730,w,88};gfx_color(p,.5f,.075f,.075f,.085f,.94f*entry);
    gfx_icon((GfxRect){88,752,44,44},"forward",1,1,1,entry);txt_draw_alpha(t,148,752,entry);
  }
}

void player_draw(Uint32 now) {
  (void)now;
  if (!is_open) return;
  const CatItem *c = item();

  // --- o quadro de video ---
  // Com pipeline nao ha o que desenhar: o video esta num plano de hardware ATRAS
  // desta superficie, e o que se faz aqui e abrir o buraco por onde ele aparece.
  // O furo tem de sair DAQUI e nao no fim do quadro: feito por ultimo ele
  // apagaria os proprios controles. Tudo o que vem depois (veu, barra, textos)
  // desenha por cima do buraco e continua visivel, porque o alpha do blend e
  // somado — um veu a 60% sobre o furo devolve 0.6 de opacidade, que e
  // exatamente o escurecimento que se quer sobre o video.
  //
  // Sem pipeline, o lugar do quadro fica com a arte-chave parada. GFX_CARD com
  // raio 0 e o quad de tela inteira: o recorte (cover) do shader e o que impede
  // a arte 16:9 de esticar quando a tela nao for exatamente 16:9.
  GfxRect screen = { 0, 0, NV_SCREEN_W, NV_SCREEN_H };
  if (player_com_video()) {
    // O furo acompanha o MESMO retangulo que foi ao plano de hardware, cortado
    // na tela. Furar sempre a tela inteira, como antes, deixava faixa preta nos
    // modos que nao ocupam tudo ("Original" num 2.39:1 entregue como 2.39:1):
    // o furo mostrava o nada atras do plano em vez de mostrar o plano.
    PlrRect r = aspectVisible(aspect);
    GfxRect hole;
    hole.x = r.x; hole.y = r.y; hole.w = r.w; hole.h = r.h;
    // Fora do furo fica PRETO, e nao a arte-chave: e o que a TV mostra ao lado
    // do plano de video, e pintar outra coisa ali criaria uma borda que nao
    // existe no aparelho.
    if (hole.w < NV_SCREEN_W - 0.5f || hole.h < NV_SCREEN_H - 0.5f)
      gfx_color(screen, 0.0f, 0, 0, 0, 1.0f);
    if (hole.w > 0.0f && hole.h > 0.0f) gfx_hole(hole);
  } else {
    const char *art = (c && c->backdrop[0]) ? c->backdrop : NULL;
    GLuint tex = art ? tex_get_hero(art) : 0;   // ocupa a tela inteira
    if (tex) {
      gfx_tex_aspect_current = tex_aspect(art);
      gfx_rect(screen, tex, GFX_CARD, 0, 0, 0, 0.0f, 0, 0, 0, entry);
      gfx_tex_aspect_current = 0.0f;
    } else {
      gfx_color(screen, 0.0f, 0.04f, 0.04f, 0.05f, entry);
    }
  }

  // Indicador de abertura: pontos pulsando no centro, sobre a arte escurecida.
  // Um giro exigiria rotacao no shader; tres pontos em contrafase dizem a mesma
  // coisa com o que ja existe, e leem bem de longe.
  if (player_loading()) {
    GfxRect dark = { 0, 0, NV_SCREEN_W, NV_SCREEN_H };
    int k;
    gfx_color(dark, 0.0f, 0, 0, 0, 0.55f * entry);
    GLuint logo = c && c->logo[0] ? tex_get_width(c->logo, 520) : 0;
    if (logo) {
      float ar = tex_aspect(c->logo), w = 520, h = ar > 0 ? w / ar : 120;
      if (h > 160) { h = 160; w = h * ar; }
      gfx_rect((GfxRect){(NV_SCREEN_W-w)*.5f,NV_SCREEN_H*.5f-h-60,w,h},logo,
               tex_brand_dark(c->logo)?GFX_BRAND:GFX_TEXT,0,0,0,0,.95f,.95f,.97f,entry);
    } else {
      TxtLine t = txt_line_trim(TXT_PLR_TITLE,c?c->title:"Playing",240,241,244,255,680);
      txt_draw_alpha(t,(NV_SCREEN_W-t.w)*.5f,NV_SCREEN_H*.5f-150,entry);
    }
    // Anel com cauda luminosa, animado sem novas texturas por quadro.
    for (k = 0; k < 12; k++) {
      float ang = k * 6.2831853f / 12.0f + now * .006f;
      float br = .18f + .82f * k / 11.0f;
      GfxRect pt = {NV_SCREEN_W*.5f + cosf(ang)*24 - 4,
                    NV_SCREEN_H*.5f + sinf(ang)*24 - 4,8,8};
      gfx_color(pt,.5f,.95f,.95f,.97f,br*entry);
    }
    { TxtLine lc = txt_line(TXT_CALLOUT, "Opening source", 236, 237, 242, 255);
      txt_draw_alpha(lc, NV_SCREEN_W * 0.5f - lc.w * 0.5f,
                         NV_SCREEN_H * 0.5f + 50, 0.85f * entry); }
    if (lineEp[0]) {
      TxtLine le = txt_line_trim(TXT_PG_END,lineEp,196,198,204,255,680);
      txt_draw_alpha(le,(NV_SCREEN_W-le.w)*.5f,NV_SCREEN_H*.5f+94,entry);
    }
  }
  if (errorSource) {
    gfx_color(screen,0,.02f,.02f,.025f,.65f);
    TxtLine er=txt_line(TXT_CALLOUT,"Could not open the source",240,241,243,255);
    txt_draw_alpha(er,(NV_SCREEN_W-er.w)*.5f,400,entry);
    TxtLine aj=txt_line(TXT_PG_END,"Open Sources to choose another option or reload.",192,194,200,255);
    txt_draw_alpha(aj,(NV_SCREEN_W-aj.w)*.5f,448,entry);
  }

  // --- aviso de troca de modo de proporcao ---------------------------------
  // O #playerAspectToast do web, com as medidas do bloco de TV do CSS:
  //   top min(8.33vw,160px)=160  altura min(6.67vw,128px)=128
  //   padding lateral min(3.33vw,64px)=64  fonte min(2.92vw,56px)=56
  //   fundo rgba(9,13,20,0.88), borda rgba(255,255,255,0.18), raio 999 (pilula)
  // Ele e desenhado ANTES do corte por `a`: a tecla de proporcao funciona com
  // os controles escondidos, e um aviso que so aparecesse com a barra em pe
  // deixaria a troca sem nenhuma confirmacao no caso mais comum.
  if (toastAte > now) {
    // Some com fade nos ultimos 200ms, que e a `transition: opacity 200ms` do
    // bloco de TV. Aparecer e sumir de estalo le como falha de desenho.
    float remains = (float)(toastAte - now);
    float at = (remains < 200.0f ? remains / 200.0f : 1.0f) * entry;
    TxtLine l = txt_line(TXT_PLR_TITLE, player_aspect_label(aspect),
                           243, 248, 255, 242);
    float pw = (float)l.w + 128.0f, ph = 128.0f;
    GfxRect pil = { (NV_SCREEN_W - pw) * 0.5f, 160.0f, pw, ph };
    // Raio e FRACAO do menor lado (ver gfx.h): 0.5 e a pilula completa.
    gfx_color(pil, 0.5f, 9.0f / 255.0f, 13.0f / 255.0f, 20.0f / 255.0f, 0.88f * at);
    txt_draw_alpha(l, pil.x + (pw - l.w) * 0.5f,
                       pil.y + (ph - (float)l.h) * 0.5f, at);
  }

  /* Permanecem quando os controles somem: sao conteudo, nao chrome do player. */
  drawSubtitleExternal();
  drawActionsEpisode();

  float a = anim * entry;
  if (a <= 0.005f) return;   // tocando limpo: nada por cima da imagem

  // Dois degrades, como no web: .player-controls-gradient-top (150px, 0.7 -> 0)
  // e .player-controls-gradient-bottom (200px, 0 -> 0.8). O de baixo sustenta o
  // titulo e a barra; o de cima existe porque os selos e a classificacao ficam
  // no alto e sem ele sumiriam sobre cena clara. Ambos acompanham a animacao
  // dos controles: fixos, deixariam sombra permanente em toda cena.
  GfxRect veil = { 0, NV_SCREEN_H - PLR_GRADIENT_BOTTOM, NV_SCREEN_W, PLR_GRADIENT_BOTTOM };
  // GFX_VEU_BAIXO e nao GFX_VEU: aquele escurece tambem a ESQUERDA (feito para
  // o hero da home) e deixava o canto superior esquerdo deste retangulo escuro
  // com o direito transparente — a borda entre os dois lia como uma placa.
  gfx_rect(veil, 0, GFX_VEIL_BOTTOM, 0, 0, 0, 0.0f, 0, 0, 0, 0.86f * a);
  { GfxRect top = { 0, 0, NV_SCREEN_W, PLR_GRADIENT_TOP };
    gfx_rect(top, 0, GFX_VEIL_TOP, 0, 0, 0, 0.0f, 0, 0, 0, 0.70f * a); }

  // O bloco inteiro desliza junto: titulo, barra e icones sao UM objeto que
  // sobe. Animar cada linha por conta propria produz um escalonamento que o
  // aparelho nao tem.
  // O deslize acompanha as DUAS coisas: o OSD aparecendo/sumindo (`anim`) e a
  // TELA abrindo (`entrada`). Antes so o primeiro entrava aqui, entao abrir o
  // player era um fade seco — os controles nasciam no lugar final, so que
  // transparentes. Com a abertura tambem deslizando, o bloco entra de baixo e a
  // tela deixa de "piscar" para o estado final.
  //
  // A curva da abertura e uma desaceleracao (1-(1-t)^3) e nao a mola crua: a
  // mola passa do ponto e volta, e num bloco de 200px de altura esse repique le
  // como tremida.
  float eEnt  = 1.0f - (1.0f - entry) * (1.0f - entry) * (1.0f - entry);
  float scrolldown = (1.0f - anim) * PLR_SLIDE
              + (1.0f - eEnt) * PLR_SLIDE * 1.8f;

  // Ancoragem de baixo para cima, na ordem da coluna .player-controls-bottom do
  // web lida ao contrario: a fileira de botoes encosta na margem inferior, a
  // barra fica 16px acima dela e a meta 12px acima da barra. A margem e
  // --player-controls-y (48), nao a margem geral do app.
  float yRowTop = NV_SCREEN_H - PLR_DFLT_Y - PLR_BTN_D + scrolldown;
  float cyButtons = yRowTop + PLR_BTN_D * 0.5f;
  float yBar   = yRowTop - PLR_GAP_ROW - PLR_RAIL_H;

  // --- barra de progresso ---
  // A barra ocupa a largura util inteira, entre as margens do player. Sem
  // marcador na cabeca: o web nao tem um — a barra engorda de 6 para 10px
  // quando recebe foco, e e isso que diz que ela e operavel. Aqui o foco anda
  // so pelos botoes, entao ela fica sempre em 6.
  // DE PONTA A PONTA: encosta nas duas bordas da tela. Com margem ela lia como
  // um componente solto no meio do rodape; encostada, ela e a borda do video.
  float bx = 0.0f, bw = NV_SCREEN_W;
  // MARGEM DE SEGURANCA para o CONTEUDO (titulo, meta, botoes, relogio).
  //
  // O trilho continua de ponta a ponta de proposito — encostado, ele le como a
  // borda do video. O que nao pode encostar e o TEXTO: em x=0 ele cai na zona
  // que a TV corta por overscan, e o dono viu o titulo e o tempo cortados nas
  // duas beiradas. Sao dois papeis diferentes que estavam compartilhando o
  // mesmo x so porque nasceram juntos.
  //
  // 96 e a mesma margem lateral da pagina de titulo (NV_DETP_X, o
  // --tv-safe-gutter-width do web), entao o player deixa de ser o unico lugar
  // do app com uma regra propria de borda. Fica como constante local porque
  // player.c nao inclui detail.h — e nao deve incluir so por um numero.
  float cx = bx + PLR_MARGIN;
  float cw = bw - PLR_MARGIN * 2.0f;
  float frac = durationSeg > 0.0f ? anim_clamp(posSeg / durationSeg, 0.0f, 1.0f) : 0.0f;
  // Com foco o trilho engorda de 6 para 10 e clareia de 0.30 para 0.45, e ele
  // cresce para BAIXO a partir da mesma linha de base — subir moveria tambem a
  // meta e o titulo, que estao ancorados nela.
  // O trilho cresce para BAIXO a partir da mesma linha de base — subir moveria
  // tambem o titulo, que esta ancorado nela.
  float hRail = barFocus ? PLR_RAIL_H_FOCUS : PLR_RAIL_H;
  GfxRect rail = { bx, yBar, bw, hRail };
  GfxRect traveled = { bx, yBar, bw * frac, hRail };
  gfx_color(rail, PLR_RAIL_R, 1, 1, 1, (barFocus ? 0.34f : 0.22f) * a);
  // O buffer do pipeline, entre o andado e o fim: e o que mostra que o video
  // esta a frente do relogio. Sem dado do pipeline o segmento nao existe —
  // inventar "quase todo carregado" seria pior que a barra simples. No web ele
  // e a MESMA cor do preenchimento a 0.35 (.player-progress-buffered).
  { float bufFrac = durationSeg > 0.0f ? anim_clamp(video_buffer_end() / durationSeg, 0.0f, 1.0f) : 0.0f;
    if (bufFrac > frac + 0.004f) {
      GfxRect buf = { bx + bw * frac, yBar, bw * (bufFrac - frac), hRail };
      gfx_color(buf, PLR_RAIL_R, PLR_FILL_C, PLR_FILL_C, PLR_FILL_C, 0.35f * a);
    } }
  // Meio pixel ja conta: com o teste em 1.0 o inicio do filme nao desenhava
  // nada, e a barra parecia so comecar a andar depois de um tempo.
  if (traveled.w > 0.5f)
    gfx_color(traveled, PLR_RAIL_R, PLR_FILL_C, PLR_FILL_C, PLR_FILL_C, a);

  // Filme: somente nome. Serie: nome seguido de T/E e titulo do episodio.
  // O arquivo e o provedor pertencem a folha de fontes, nao ao transporte.
  float yMetaBase = yBar - PLR_GAP_BAR;
  if (lineEp[0]) {
    TxtLine le=txt_line_trim(TXT_PLR_BODY,lineEp,218,220,224,255,cw*.67f);
    yMetaBase-=le.h;
    txt_draw_alpha(le,cx,yMetaBase,a);
    yMetaBase-=6;
  }

  // O NOME DO FILME, EM TEXTO. Aqui o player preferia o LOGO do titulo quando
  // havia um, e caia no texto so na falta dele. Duas coisas davam errado: o
  // logo tem altura e proporcao proprias, entao o bloco pulava de titulo para
  // titulo; e quando o TMDB entregava a variante escura o nome sumia sobre a
  // cena. O dono pediu direto: "o titulo do filme que aparece no player pode
  // deixar escrito como tava antes... so o nome do filme".
  //
  // Texto tambem e o que o resto da tela usa (o relogio, o tempo, os selos),
  // entao o canto passa a ter UMA gramatica so.
  float hTitle, yTitle;
  { const char *name = (c && c->title[0]) ? c->title : "Playing";
    TxtLine lt = txt_line_trim(TXT_PLR_TITLE, name, 255, 255, 255, 255,
                                  cw * 0.62f);
    hTitle = (float)lt.h;
    yTitle = yMetaBase - hTitle;
    txt_draw_alpha(lt, cx, yTitle, a); }

  // --- fileira de BOTOES: o transporte do aparelho --------------------------
  // Sem botoes redundantes de salto. O foco percorre so as acoes visiveis.
  {
    // .player-controls-row e space-between: o grupo de botoes a ESQUERDA, com
    // gap de 4px entre eles, e o rotulo de tempo empurrado para a direita por
    // margin-left:auto. Nao e o transporte centralizado do app da Apple.
    float step = PLR_BTN_D + PLR_BTN_GAP;
    float x0    = cx + PLR_BTN_D * 0.5f;
    float cxs[PLR_NBTNS];
    for (int i=0;i<PLR_NBTNS;i++) cxs[i]=x0+i*step;
    for (int i = 0; i < PLR_NBTNS - (epT > 0 ? 0 : 1); i++) {
      float f = focusB[i];
      int sel = (button == i && !barFocus);
      buttonCircle(cxs[i], cyButtons, f, a, sel);
      float luma = sel ? 0.13f : 0.94f;
      switch (i) {
        case PLR_PLAY:    iconPlayPause(cxs[i], cyButtons, a, playing, luma); break;
        case PLR_CC:      iconSubtitles(cxs[i], cyButtons, a, luma); break;
        case PLR_ASPECT: iconAspect(cxs[i], cyButtons, a, luma); break;
        case PLR_SOURCES: iconFile(cxs[i],cyButtons,a,luma,"sources",44); break;
        case PLR_EPISODES: iconFile(cxs[i],cyButtons,a,luma,"episodes",44); break;
        default:          iconAudio(cxs[i], cyButtons, a, luma); break;
      }
    }
    if (!barFocus) {
      const char *labels[]={"Play / pause","Aspect ratio","Subtitles","Audio","Sources","Episodes"};
      TxtLine label=txt_line(TXT_PG_END,labels[button],210,212,218,255);
      txt_draw_alpha(label,cxs[button]-label.w*.5f,cyButtons+PLR_BTN_D*.5f+10,a);
    }
  }

  // --- rotulo de tempo, na ponta direita da mesma fileira --------------------
  // Um rotulo so, "decorrido / total", como o #playerTimeLabel do web. Aqui
  // eram DOIS — decorrido a esquerda da barra e restante NEGATIVO a direita —
  // que e a convencao do app da Apple, nao a nossa. Centrado na vertical com os
  // circulos porque no web ele e um item de uma flex row com align-items:center.
  {
    char t1[24], t2[24], all[52];
    fmtTime(t1, sizeof t1, posSeg, 0);
    fmtTime(t2, sizeof t2, durationSeg, 0);
    snprintf(all, sizeof all, "%s / %s", t1, t2);
    { TxtLine l = txt_line(TXT_PLR_BODY, all, 255, 255, 255, 230);
      txt_draw_alpha(l, cx + cw - l.w,
                         cyButtons - (float)l.h * 0.5f, a * 0.9f); }
  }

  // Selos de formato no alto a direita. Vem do FLUXO, nao de constante: os
  // dois estavam fixos e anunciavam Dolby Vision em arquivo HDR10 e Atmos em
  // faixa estereo. Selo que mente e pior que selo ausente, porque e nele que o
  // dono confia para saber se pegou a versao boa.
  {
    const char *badges[3];
    int nBadges = 0;
    char res[16] = "";
    if (video_width() >= 3840)      snprintf(res, sizeof res, "4K");
    else if (video_width() >= 1920) snprintf(res, sizeof res, "HD");
    if (res[0]) badges[nBadges++] = res;
    // MEDIDO nesta TV, linha do proprio log durante a reproducao de um MKV que
    // o addon anunciava como Dolby Vision:
    //   [video] HDR do pipeline: HDR10 (fonte afirmava DV=1)
    // Era exatamente esse o caso em que o selo mentia.
    //
    // "Dolby Vision" so quando o PIPELINE devolveu DolbyVision no videoInfo —
    // video_tem_dolby_vision nao le mais a afirmacao do addon. Esta MEDIDO que
    // nesta TV um MKV anunciado como DV volta HDR10; o selo dizia Dolby Vision
    // por cima de um fluxo HDR10, e o dono confia nele justamente para saber se
    // pegou a versao boa. Quando o pipeline diz HDR10, o selo diz HDR10 — calar
    // seria esconder metade da resposta.
    if (video_has_dolby_vision())                  badges[nBadges++] = "Dolby Vision";
    else if (!strcasecmp(video_hdr(), "HDR10"))    badges[nBadges++] = "HDR10";
    if (video_has_atmos())        badges[nBadges++] = "Dolby Atmos";

    // RELOGIO e "Termina as", que sao o que o web poe neste canto
    // (.player-controls-top, playerScreen.js:5846). Os selos de qualidade sao
    // acrescimo do port e passam a ficar ABAIXO deles, nao no lugar.
    //
    //   .player-clock    26/600 branco 96%
    //   .player-ends-at  20/400 branco 78%, logo abaixo
    float yRel = PLR_DFLT_Y + scrolldown;
    {
      time_t nowT = time(NULL);
      struct tm lt;
      char hora[8], end[32];
      localtime_r(&nowT, &lt);
      strftime(hora, sizeof hora, "%H:%M", &lt);
      { double missing = durationSeg - posSeg;
        time_t t2 = nowT + (time_t)(missing > 0.0 ? missing : 0.0);
        struct tm lf; char h2[8];
        localtime_r(&t2, &lf);
        strftime(h2, sizeof h2, "%H:%M", &lf);
        snprintf(end, sizeof end, "Ends at %s", h2); }
      TxtLine lh = txt_line(TXT_PG_CLOCK, hora, 255, 255, 255, 255);
      TxtLine lf = txt_line(TXT_PG_END, end, 255, 255, 255, 255);
      txt_draw_alpha(lh, NV_SCREEN_W - PLR_DFLT_X - lh.w, yRel, a * 0.96f);
      txt_draw_alpha(lf, NV_SCREEN_W - PLR_DFLT_X - lf.w, yRel + lh.h + 2.0f,
                         a * 0.78f);
      yRel += lh.h + 2.0f + lf.h;
    }

    { float sy = yRel + 16.0f;
      int i;
      // ENTRADA ESCALONADA. Estes selos ja apareciam um a um, mas por acidente:
      // o rasterizador de texto faz no maximo TXT_POR_QUADRO linhas por quadro
      // (text.c:40, e ha razao medida para isso), entao o terceiro selo chegava
      // dois quadros depois do primeiro. Lido na TV isso e um defeito — "vai
      // aparecendo e mostrando um por um", nas palavras do dono.
      //
      // A correcao nao e apressar o rasterizador: e ASSUMIR o escalonamento e
      // dar a ele uma curva. Cada selo entra 90 ms depois do anterior, subindo
      // 10px e ganhando opacidade. O que era artefato vira cadencia, e o atraso
      // do raster fica escondido dentro da propria animacao.
      float t0 = (float)(now - lastInput) / 1000.0f;
      for (i = 0; i < nBadges; i++) {
        float ts = anim_clamp((t0 - i * 0.09f) / 0.26f, 0.0f, 1.0f);
        float e  = 1.0f - (1.0f - ts) * (1.0f - ts);   // desaceleracao
        TxtLine l = txt_line(TXT_MINI, badges[i], 236, 237, 242, 255);
        if (e > 0.004f)
          txt_draw_alpha(l, NV_SCREEN_W - PLR_DFLT_X - l.w,
                             sy + (1.0f - e) * 10.0f, a * 0.85f * e);
        sy += l.h + 6.0f;
      } }
  }

  // GUIA PARENTAL, canto superior esquerdo (.player-parental-guide).
  //
  // Aqui havia um selo de classificacao com o GENERO do titulo ao lado, que
  // nao existe no app web — genero nao e advertencia de conteudo, e "Drama"
  // dentro de um selo laranja se le como aviso. O web mostra ate cinco linhas
  // "Categoria · Gravidade" vindas do guia parental do IMDb, com uma barra
  // vertical de 6px na cor de destaque encostada a esquerda.
  //
  //   .player-parental-guide  left 64, top 48
  //   .player-parental-line   6 de largura, raio 3, altura = a da lista
  //   .player-parental-list   padding-left 20, gap 4
  //   .player-parental-item   36 de altura
  //   rotulo 22/600 branco 85% · separador 22/400 branco 40% ·
  //   gravidade 22/400 branco 50%
  //
  // TEMPO PROPRIO, e nao o alpha do OSD. Esta guia e um AVISO DE ABERTURA: diz
  // o que o filme contem antes de a cena comecar a valer. Presa ao OSD ela
  // reaparecia toda vez que o dono mexia no controle, no meio do filme, quando
  // a informacao ja nao serve para nada — "ele deveria so aparecer animado no
  // inicio do filme e depois nao deveria aparecer mais".
  //
  // Conta de inicioImagem (o primeiro quadro com imagem, nao a abertura da
  // tela): entra escalonada linha a linha, fica PG_SEG_VISIVEL e sai. Depois
  // disso nao volta nesta reproducao.
  {
    int np = parental_n();
    float tg = startImage ? (float)(now - startImage) / 1000.0f : -1.0f;
    if (np > 0 && tg >= 0.0f && tg < PG_SEG_TOTAL) {
      float output = anim_clamp((PG_SEG_TOTAL - tg) / PG_SEG_OUTPUT, 0.0f, 1.0f);
      float lin = PG_LINE_H, gap = PG_LINE_GAP;
      float height = np * lin + (np - 1) * gap;
      float y0 = PLR_DFLT_Y;
      // A barra so cresce depois que a primeira linha entrou, senao ela aparece
      // sozinha apontando para o vazio.
      float eB = anim_clamp((tg - 0.10f) / 0.34f, 0.0f, 1.0f);
      eB = 1.0f - (1.0f - eB) * (1.0f - eB);
      { GfxRect bar = { PLR_DFLT_X, y0, PG_BAR_W, height * eB };
        if (eB > 0.01f)
          gfx_color(bar, 0.5f * (PG_BAR_W / (height * eB)),
                  PLR_FILL_C, PLR_FILL_C, PLR_FILL_C, entry * output); }
      float xt = PLR_DFLT_X + PG_BAR_W + PG_LIST_PADX;
      for (int i = 0; i < np; i++) {
        float yl = y0 + i * (lin + gap);
        float ts = anim_clamp((tg - 0.18f - i * 0.10f) / 0.30f, 0.0f, 1.0f);
        float ee = 1.0f - (1.0f - ts) * (1.0f - ts);   // desaceleracao
        float ag = entry * output * ee;
        float dx = (1.0f - ee) * 18.0f;                // entra deslizando da esquerda
        TxtLine lr, ls, lg;
        float cy, x;
        if (ag <= 0.004f) continue;
        lr = txt_line(TXT_PG_LABEL, parental_label(i), 255, 255, 255, 255);
        ls = txt_line(TXT_PG_SEV, "\xc2\xb7", 255, 255, 255, 255);
        lg = txt_line(TXT_PG_SEV, parental_severity(i), 255, 255, 255, 255);
        cy = yl + (lin - lr.h) * 0.5f;
        x  = xt - dx;
        txt_draw_alpha(lr, x, cy, ag * 0.85f);  x += lr.w;
        txt_draw_alpha(ls, x, yl + (lin - ls.h) * 0.5f, ag * 0.40f); x += ls.w;
        txt_draw_alpha(lg, x, yl + (lin - lg.h) * 0.5f, ag * 0.50f);
      }
    }
  }
}
