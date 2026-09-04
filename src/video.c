#include "video.h"
#include <SDL2/SDL.h>
#include "mark.h"
#include "mkv.h"
#include "js.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <ctype.h>
#include <stdint.h>

// Nomes que o uMS aceita em charColor, e os rotulos que a folha mostra.
//
// FORA do #if do aparelho: a folha de faixas desenha os rotulos tambem no Mac,
// onde o resto do modulo e stub. Deixa-los no lado da TV quebrava a ligacao da
// build de desenvolvimento — que e onde a interface e conferida.
const char *const VIDEO_SUB_COLORS[VIDEO_SUB_NCORES] = {
  "white", "yellow", "green", "blue", "red", "black"
};
const char *const VIDEO_SUB_COLORS_PT[VIDEO_SUB_NCORES] = {
  "White", "Amarelo", "Verde", "Azul", "Vermelho", "Preto"
};

static int extSubtitle(const char *start, const char *end) {
  static const char *const ext[] = { ".srt", ".vtt", ".smi", ".ass", ".ssa", ".sub" };
  size_t i;
  for (i = 0; i < sizeof ext / sizeof *ext; i++) {
    size_t n = strlen(ext[i]);
    const char *p;
    if ((size_t)(end - start) < n) continue;
    p = end - n;
    { size_t k; for (k = 0; k < n; k++)
        if (tolower((unsigned char)p[k]) != ext[i][k]) break;
      if (k == n) return 1; }
  }
  return 0;
}

void video_normalize_url_subtitle(const char *url, char *dst, unsigned size) {
  const char *q;
  size_t before, suffix, fits;
  if (!dst || !size) return;
  dst[0] = 0;
  if (!url) return;
  q = strchr(url, '?');
  if (!q) q = url + strlen(url);
  if (extSubtitle(url, q)) { snprintf(dst, size, "%s", url); return; }
  before = (size_t)(q - url); suffix = strlen(q);
  fits = before + 4 + suffix;
  if (fits + 1 > size) { snprintf(dst, size, "%s", url); return; }
  memcpy(dst, url, before);
  memcpy(dst + before, ".srt", 4);
  memcpy(dst + before + 4, q, suffix + 1);
}

// Declarada aqui porque o loadCompleted a chama muito antes de ela ser
// definida. O clang do Mac aceita a implicita; o gcc do ARM recusa — e o ARM
// que esta certo.
static void applyStyle(void);

// Declarada aqui porque o loadCompleted a chama muito antes de ela ser
// definida. O clang do Mac aceita a implicita e o gcc do ARM recusa — e o ARM
// que esta certo.
static void applyStyle(void);


// Definidos adiante (junto de urlAtual, que e o que o fio consome); declarados
// aqui porque o parse do sourceInfo, bem acima, e quem dispara o fio.
static char  urlCurrent[1024];   // URL da reproducao corrente
// Recuperacao de pipeline destruido: pedida pelo fio de resposta do luna e
// executada no fio principal (video_bombear), porque recarregar de dentro do
// tratador de evento reentra no mesmo caminho que acabou de falhar.
static int    recovering;
static double resumeIn;
// Posicao a aplicar assim que o load terminar. Seek antes do loadCompleted e
// mandado para um pipeline que ainda nao existe e some sem erro.
static double posOnLoad;
// FAIXAS a restaurar depois de uma queda de pipeline. Sem isto o video voltava
// com OUTRO audio — o pipeline novo comeca sempre na faixa 0, e o dono, que
// tinha escolhido a dele, via a escolha ser desfeita sozinha. `-1` = nao ha o
// que restaurar.
static int   audioOnLoad = -1, subOnLoad = -1;
static char  subUrlOnLoad[1024];
// URL da legenda EXTERNA em uso. O legAtual nao a representa: quem escolhe uma
// legenda do OpenSubtitles nao mexe em faixa nenhuma do arquivo, so aponta o
// setSubtitleSource. Sem guardar a URL, a recuperacao trazia de volta a legenda
// embutida de antes, ou nenhuma.
static char  subUrlCurrent[1024];
// Avanco pendente: alvo e quando manda-lo. Ver SEEK_REPOUSO_MS.
static int    pauseRequested;   // 1 enquanto a pausa foi pedida por nos
// Sonda de MKV pedida, esperando o buffer. Ver a nota no sourceInfo.
static int    mkvPending;
// 1 quando a fonte foi anunciada como MP4. Ver video_definir_mp4.
static int    sourceMp4;
static double seekTarget;
static Uint32 seekIn;
// Declarada aqui porque video_bombear a chama antes da definicao. O clang do
// Mac aceita a implicita; o gcc do ARM recusa — e o ARM que esta certo. Terceira
// vez neste arquivo.
static void seekNow(double seconds);
static void *readMkv(void *arg);
static pthread_t threadMkv;
static int       threadMkvAlive;
// Identidade monotonica do pipeline. Callbacks do LS2 podem sobreviver ao
// unload; sem uma geracao, a resposta antiga pode ocupar o estado da proxima
// abertura e fazer o load correto ser ignorado.
static unsigned  session;

#ifdef __APPLE__
// No Mac nao existe barramento nem plano de video. Os cotos deixam o resto do
// app compilar e rodar igual, so sem imagem em movimento.
int  video_start(void) { return 0; }
int  video_play(const char *u) { (void)u; return 0; }
void video_pump(void) {}
void video_stop(void) {}
void video_pause(int p) { (void)p; }
void video_fetch(double s) { (void)s; }
void video_window(int x,int y,int w,int h) { (void)x;(void)y;(void)w;(void)h; }
// Coto que FALTAVA: a funcao existia so no ramo do aparelho, entao o build do
// Mac quebrava no link com "_video_janela_fonte, referenced from
// _aplicarAspecto". E o espelho da armadilha ja conhecida — o Mac nao compila a
// metade do pipeline, e por isso nao valida `video.c`; aqui ele cobra a
// declaracao que a outra metade nao tem. Toda funcao nova de video precisa
// aparecer NOS DOIS ramos.
void video_window_source(int sx,int sy,int sw,int sh,int dx,int dy,int dw,int dh) {
  (void)sx;(void)sy;(void)sw;(void)sh;(void)dx;(void)dy;(void)dw;(void)dh;
}
double video_pos(void) { return 0; }
double video_duration(void) { return 0; }
double video_buffer_end(void) { return 0; }
void video_set_dv(int dv) { (void)dv; }
int  video_playing(void) { return 0; }
int  video_ready(void) { return 0; }
int  video_active(void) { return 0; }
int  video_n_audio(void) { return 0; }
int  video_n_subtitle(void) { return 0; }
const VideoTrack *video_audio(int i) { (void)i; return 0; }
const VideoTrack *video_subtitle(int i) { (void)i; return 0; }
int  video_audio_current(void) { return 0; }
int  video_subtitle_current(void) { return -1; }
void video_choose_audio(int i) { (void)i; }
void video_choose_subtitle(int i) { (void)i; }
void video_subtitle_external(const char *u) { (void)u; }
void video_subtitle_style(const VideoSubtitleStyle *e) { (void)e; }
void video_set_mp4(int m) { (void)m; }
int  video_tem_atmos(void) { return 0; }
int  video_tem_dolby_vision(void) { return 0; }
const char *video_hdr(void) { return "none"; }
int  video_width(void) { return 0; }
int  video_height(void) { return 0; }
void video_shutdown(void) {}
#else
#include <dlfcn.h>

typedef struct LSHandle LSHandle;
typedef struct LSMessage LSMessage;
typedef int (*Filter)(LSHandle *, LSMessage *, void *);

// LSError e struct por valor e nao ha header C no SDK. Um buffer folgado evita
// corromper a pilha quando a lib escreve o erro dentro dele.
static char ERROR[256];

static int         (*lsRegister)(const char *, LSHandle **, void *);
static int         (*lsAttach)(LSHandle *, void *, void *);
static int         (*lsCall)(LSHandle *, const char *, const char *, Filter, void *, unsigned long *, void *);
static const char *(*lsPayload)(LSMessage *);
static void *(*loopNew)(void *, int);
static void  (*loopRun)(void *);
static void  (*loopStop)(void *);

// libAcbAPI e SEMPRE por dlopen. Linkar cria um DT_NEEDED e, se a lib faltar
// (ela sumiu no webOS 5), o processo morre antes do main e antes do log —
// nao sobra nem uma linha para diagnosticar.
static long (*acbCreate)(void);
static int  (*acbStart)(long, int, const char *, void *);
static int  (*acbSink)(long, int);
static int  (*acbMedia)(long, const char *);
static int  (*acbState)(long, int, int, long *);
static int  (*acbWindow)(long, long, long, long, long, int, long *);
// Janela CUSTOMIZADA: recorte de fonte + retangulo de destino. E o caminho com
// permissao. Chamar luna://com.webos.service.tv.display/setCustomDisplayWindow
// direto e RECUSADO pelo hub — "Not permitted to send to
// com.webos.service.tv.display" —, porque o app se registra como
// com.webos.media.client.nuvio e esse papel nao alcanca o servico de display.
// A libAcbAPI alcanca: ela expoe AcbAPI_setCustomDisplayWindow e fala com o
// tv.display por dentro, que e como o proprio navegador da TV faz.
static int  (*acbWindowCustom)(long, long, long, long, long,
                               long, long, long, long, int, long *);
static void (*acbDestroy)(long);
// O navegador da TV chama isto e nos nao chamavamos: sem o connect o plano de
// video existe, decodifica e toca o audio, mas nao e ligado a saida — tela
// preta com som, exatamente o sintoma observado.
static int  (*acbConnect)(long, int, long *);
// Recebe JSON como string (confirmado: a lib chama strlen no argumento antes de
// montar um std::string). Sem esta chamada o servico do ACB nunca repassa nada
// para com.webos.service.tv.display e o plano de video nao liga — o sintoma e
// audio normal com tela preta.
static int  (*acbVideoDate)(long, const char *, long *);
static int  (*acbAudioDate)(long, const char *, long *);

static LSHandle *bus;
static void     *loop;
static pthread_t thread;
static long      acb;
// Retangulo pedido pela UI. Guardado porque o ACB so aceita a janela depois do
// loadCompleted, que chega muito depois de quem pediu.
static int       windowX, windowY, windowW = 1920, windowH = 1080;
// Ultimo par fonte/destino aplicado pelo setDisplayWindow do uMS, para nao
// repetir a mesma chamada a cada quadro. fonX = -1 quer dizer "nada aplicado".
static int       fontX = -1, fontY, fontW, fontH, dstX = -1, dstY, dstW, dstH;
// Caracteristicas do fluxo, tiradas do evento videoInfo da assinatura do uMS.
// O ACB precisa delas para descrever o video ao pipeline de exibicao.
static int       vidW = 1920, vidH = 1080, vidRate = 30;
static long      vidBits;
static char      vidScan[24] = "progressive";
// hdrType real informado pelo uMS para a camada que chegou ao decoder. Isto
// vence o rotulo do addon: um arquivo marcado HDR-DV pode entregar apenas a
// camada HDR10 nesta TV/perfil.
static char      vidHdr[24] = "none";
static long      seiX0, seiX1, seiX2, seiY0, seiY1, seiY2;
static long      seiWhiteX, seiWhiteY, seiMinLuma, seiMaxLuma;
static long      seiMaxCLL, seiMaxFALL;
static int       vuiFirst = 2, vuiTrans = 2, vuiMatrix = 2;
static int       vidAtmos, vidDV;
// Estado do recuo de Dolby Vision (ver o bloco em video_tocar). Declarados
// AQUI e nao junto da funcao porque o parser do videoInfo, bem acima, marca
// viuVideo — e no C a ordem de declaracao manda.
static int       dvInLoad, dvInset, viuVideo;

// Faixas lidas do sourceInfo. Guardadas porque a tela precisa delas a cada
// quadro e reprocessar o JSON no desenho seria desperdicio.
static VideoTrack trackAudio[NV_TRACK_MAX], trackSub[NV_TRACK_MAX];
static int nAudio, nSub, audioCurrent, subCurrent = -1;

// Afirmacao de DV da fonte escolhida. Setada por video_definir_dv ANTES do
// tocar, porque o video_tocar zera vidDV ao comecar uma sessao nova.
static int dvRequest;

// Nome legivel do idioma. So os que aparecem de verdade neste acervo; o resto
// fica com o codigo, que e melhor que "Desconhecido" — o codigo ao menos
// identifica.
static const char *languageReadable(const char *c) {
  // Tabela com ACENTO — e nome de idioma na tela, nao identificador. E com os
  // codigos de tres letras (ISO 639-2) alem dos de duas, porque MKV de release
  // etiqueta quase sempre com os de tres.
  static const struct { const char *cod, *name; } T[] = {
    { "pt", "Portuguese" },  { "pob", "Portuguese (BR)" }, { "por", "Portuguese" },
    { "pt-br", "Portuguese (BR)" }, { "ptb", "Portuguese (BR)" },
    { "en", "English" },     { "eng", "English" },
    { "es", "Spanish" },   { "spa", "Spanish" }, { "esp", "Spanish" },
    { "fr", "French" },    { "fre", "French" },  { "fra", "French" },
    { "de", "German" },     { "ger", "German" },   { "deu", "German" },
    { "it", "Italian" },   { "ita", "Italian" },
    { "ja", "Japanese" },    { "jpn", "Japanese" },
    { "ko", "Korean" },    { "kor", "Korean" },
    { "zh", "Chinese" },     { "chi", "Chinese" },   { "zho", "Chinese" },
    { "ru", "Russian" },      { "rus", "Russian" },
    { "ar", "Arabic" },      { "ara", "Arabic" },
    { "hi", "Hindi" },      { "hin", "Hindi" },
    { "nl", "Dutch" },   { "dut", "Dutch" }, { "nld", "Dutch" },
    { "sv", "Swedish" },      { "swe", "Swedish" },
    { "no", "Norwegian" },  { "nor", "Norwegian" },
    { "da", "Danish" },{ "dan", "Danish" },
    { "fi", "Finnish" },  { "fin", "Finnish" },
    { "pl", "Polish" },    { "pol", "Polish" },
    { "tr", "Turkish" },      { "tur", "Turkish" },
    { "he", "Hebrew" },   { "heb", "Hebrew" },
    { "th", "Thai" },  { "tha", "Thai" },
    { "cs", "Czech" },     { "cze", "Czech" },
    { "el", "Greek" },      { "gre", "Greek" },
    { "hu", "Hungarian" },    { "hun", "Hungarian" },
    { "ro", "Romanian" },     { "rum", "Romanian" },
    { "uk", "Ukrainian" },  { "ukr", "Ukrainian" },
    { "vi", "Vietnamese" }, { "vie", "Vietnamese" },
    { "id", "Indonesian" },  { "ind", "Indonesian" },
  };
  size_t i;
  if (!c || !*c) return "";
  for (i = 0; i < sizeof T / sizeof *T; i++)
    if (!strcasecmp(c, T[i].cod)) return T[i].name;
  // Sem nome na tabela, devolve o CODIGO EM MAIUSCULAS — e o que o app web faz
  // quando nao sabe nomear ("ENG", "POR"). Mostrar o codigo diz alguma coisa;
  // cair em "Legenda 3" nao diz nada.
  { static char cx[16]; size_t k;
    for (k = 0; c[k] && k + 1 < sizeof cx; k++)
      cx[k] = (c[k] >= 'a' && c[k] <= 'z') ? (char)(c[k] - 32) : c[k];
    cx[k] = 0;
    return cx; }
}
static char      media[64];
static double    posSeg, durationSeg;
static int       playing, ready, on;

// PLAYER_TYPE_MSE. O ACB usa isto para saber que a fonte e um pipeline de
// midia e nao um sintonizador.
// Os enums do ACB nao tem header publico e chutar sai caro: com playerType 10 e
// sink 1 o aparelho registrou "playerType":"mse","vsmSinkType":"sub" — ou seja,
// o video foi para o plano SECUNDARIO (PIP) e a tela ficou preta. Os numeros
// ficam ajustaveis por /tmp/nuvio-acb justamente para conferir contra o que o
// ls-monitor mostra que o ACB resolveu, em vez de adivinhar de novo.
static int kindPlayer = 0, kindSink = 0, stLoaded = 1, stPlaying = 2;
// hdrType do setMediaVideoData. O padrao e "none"; para testar Dolby Vision,
// escreva na SEGUNDA linha de /tmp/nuvio-acb: "dolby_vision" ou "hdr10".
// Afirmar DV sem o pipeline pedir e mentira — por isso NAO existe deteccao
// automatica: o sourceInfo do uMS nao distingue HEVC main-10 HDR10 de DV.
static char hdrKind[24] = "none";

static void readSettingsAcb(void) {
  FILE *f = fopen("/tmp/nuvio-acb", "r");
  if (!f) return;
  if (fscanf(f, "%d %d %d %d", &kindPlayer, &kindSink, &stLoaded, &stPlaying) > 0)
    printf("[video] acb settings: player=%d sink=%d loaded=%d playing=%d\n",
           kindPlayer, kindSink, stLoaded, stPlaying);
  { // resto da primeira linha descartado; a SEGUNDA linha, se existir, e o hdrType.
    char line[64];
    if (fgets(line, sizeof line, f) && fgets(line, sizeof line, f)) {
      char t[24] = "";
      if (sscanf(line, "%23s", t) == 1 && t[0]) {
        snprintf(hdrKind, sizeof hdrKind, "%s", t);
        if (strstr(hdrKind, "dolby")) vidDV = 1;
        printf("[video] acb settings: hdrType=%s\n", hdrKind);
      }
    }
  }
  fclose(f);
}

#define NV_ACB_FOREGROUND 1

// Procura a chave e exige que o que vem depois seja NUMERO.
//
// O evento e {"currentTime":{"currentTime":8580,...}}: a primeira ocorrencia da
// chave e o objeto externo, e atof("{...") devolve 0. A barra ficava parada em
// 0:00 com a duracao correta ao lado — o tipo de erro que parece "o player nao
// atualiza" e na verdade e leitura do campo errado.
static double numberOf(const char *p, const char *key) {
  const char *q = p;
  size_t n = strlen(key);
  while ((q = strstr(q, key)) != NULL) {
    const char *v = q + n;
    while (*v == ' ') v++;
    if ((*v >= '0' && *v <= '9') || *v == '-' || *v == '.') return atof(v);
    q += n;
  }
  return -1.0;
}

static pthread_t threadBind;
static volatile int bindAlive = 0;   // existe um bind em andamento?
// Se o load novo termina durante o bind lento da sessao anterior, guarda o
// trabalho. video_bombear inicia o bind assim que o fio anterior liberar.
static volatile int bindPending;

// Latencia do pipeline: pedido de load -> loadCompleted -> primeiro quadro.
// Sao os numeros que dizem se o comeco e o buffer estao saudaveis; sem eles
// "ta lento" e impressao.
static struct timespec t0Request;
static int cronRequested, cronLoad, cronFrame;
static long msSinceRequest(void) {
  struct timespec a;
  clock_gettime(CLOCK_MONOTONIC, &a);
  return (a.tv_sec - t0Request.tv_sec) * 1000L + (a.tv_nsec - t0Request.tv_nsec) / 1000000L;
}
// Ate onde o buffer do pipeline ja cobre (segundos), do evento bufferRange.
static double bufferSeg;

static void wait_(int ms) { struct timespec t; t.tv_sec = ms / 1000;
  t.tv_nsec = (long)(ms % 1000) * 1000000L; nanosleep(&t, NULL); }

// O JSON de video do ACB, montado por partes porque os valores de HDR mudam
// com o que se esta afirmando. VUI segue H.273/HEVC: 9=BT.2020, 16=PQ
// (SMPTE 2084) — e o par que HDR10 e DV pedem; SDR fica em 2 (unspecified),
// que e o que sempre foi mandado e toca.
static void buildVideoDate(char *vd, size_t n, const char *ctx,
                            const char *htipo, int first, int trans, int matrix) {
  const char *scan = strstr(vidScan, "inter") ? "VIDEO_INTERLACED"
                   : "VIDEO_PROGRESSIVE";
  char color[768] = "";
  if (!strcmp(htipo, "HDR10")) {
    snprintf(color, sizeof color,
      "\"mediaSei\":{\"displayPrimariesX0\":%ld,\"displayPrimariesX1\":%ld,"
      "\"displayPrimariesX2\":%ld,\"displayPrimariesY0\":%ld,"
      "\"displayPrimariesY1\":%ld,\"displayPrimariesY2\":%ld,"
      "\"maxContentLightLevel\":%ld,\"maxDisplayMasteringLuminance\":%ld,"
      "\"maxPicAverageLightLevel\":%ld,\"minDisplayMasteringLuminance\":%ld,"
      "\"whitePointX\":%ld,\"whitePointY\":%ld},"
      "\"mediaVui\":{\"colorPrimaries\":%d,\"matrixCoeffs\":%d,"
      "\"transferCharacteristics\":%d,\"videoFullRangeFlag\":false},",
      seiX0, seiX1, seiX2, seiY0, seiY1, seiY2, seiMaxCLL, seiMaxLuma,
      seiMaxFALL, seiMinLuma, seiWhiteX, seiWhiteY,
      first, matrix, trans);
  } else if (!strcmp(htipo, "none")) {
    snprintf(color, sizeof color,
      "\"mediaSei\":{\"displayPrimariesX0\":0,\"displayPrimariesX1\":0,"
      "\"displayPrimariesX2\":0,\"displayPrimariesY0\":0,"
      "\"displayPrimariesY1\":0,\"displayPrimariesY2\":0,"
      "\"maxContentLightLevel\":0,\"maxDisplayMasteringLuminance\":0,"
      "\"maxPicAverageLightLevel\":0,\"minDisplayMasteringLuminance\":0,"
      "\"whitePointX\":0,\"whitePointY\":0},"
      "\"mediaVui\":{\"colorPrimaries\":2,\"matrixCoeffs\":2,"
      "\"transferCharacteristics\":2,\"videoFullRangeFlag\":false},");
  }
  snprintf(vd, n,
    //  - "context" com o mediaId TEM de vir no proprio JSON: o servico do
    //    ACB repassa o payload como veio, nao insere o campo. Sem ele o
    //    tv.display responde ERROR_06 "Invalid argument" com
    //    "context": "" — e o erro nao diz qual argumento e.
    "{\"content\":\"movie\",\"context\":\"%s\",\"video\":{"
    "\"adaptive\":false,\"bitRate\":%ld,"
    "\"data3D\":{\"currentPattern\":\"2d\",\"originalPattern\":\"2d\","
            "\"typeLR\":\"LR\"},"
    "\"frameRate\":%d.0,\"hdrType\":\"%s\","
    "\"height\":%d,\"width\":%d,"
    "%s"
    "\"hfr\":false,"
    "\"path\":\"network\","
    "\"pixelAspectRatio\":{\"height\":1,\"width\":1},"
    "\"rotation\":\"0\",\"scanType\":\"%s\",\"specificRendering\":\"none\""
    "}}", ctx, vidBits, vidRate, htipo, vidH, vidW, color, scan);
}

static void *prenderPlane(void *u) {
  (void)u;
  // O bind e POR SESSAO: cada loadCompleted tem de religar o plano. Foi o bug
  // da "segunda reproducao preta com som" — o ACB continuava apontando para o
  // mediaId da sessao anterior, que o unload matou. A guarda de midia cobre a
  // troca de titulo no MEIO do bind (unload+load em menos de ~1,5s de pausas):
  // continuar descreveria ao tv.display um mediaId que ja morreu.
  char my[64];
  snprintf(my, sizeof my, "%s", media);
  long task = 0;
  acbMedia(acb, my);            wait_(200);
  if (strcmp(media, my)) goto fora;
  acbState(acb, NV_ACB_FOREGROUND, stLoaded, &task); wait_(200);
  if (strcmp(media, my)) goto fora;
  printf("[video] connect=%d\n", acbConnect(acb, kindSink, &task)); wait_(200);
  if (strcmp(media, my)) goto fora;
  {
    // Strings e formato copiados de controles positivos na MESMA TV:
    // Apple TV e Nuvio web usam "DolbyVision" sem SEI/VUI; HDR10 usa o SEI/VUI
    // real do videoInfo. A grafia/capitalizacao e semanticamente relevante.
    // Enquanto isso nao existia, "none" ia sempre: o C9 exibia o video
    // mapeado em SDR e o modo HDR/DV da TV nunca ligava — exatamente o
    // sintoma do teste 4K.
    char htipo[24];
    int first = 2, trans = 2, matrix = 2;
    if (strcmp(hdrKind, "none")) { snprintf(htipo, sizeof htipo, "%s", hdrKind);
                                  first = vuiFirst; trans = vuiTrans; matrix = vuiMatrix; }
    else if (!strcasecmp(vidHdr, "DolbyVision") || vidDV) {
                                  snprintf(htipo, sizeof htipo, "DolbyVision"); }
    else if (!strcasecmp(vidHdr, "HDR10")) {
                                  snprintf(htipo, sizeof htipo, "HDR10");
                                  first = vuiFirst; trans = vuiTrans; matrix = vuiMatrix; }
    else                           snprintf(htipo, sizeof htipo, "none");
    char vd[2048];
    buildVideoDate(vd, sizeof vd, my, htipo, first, trans, matrix);
    { char ad[160];
      snprintf(ad, sizeof ad,
               "{\"context\":\"%s\",\"audio\":{\"immersive\":\"none\"}}", my);
      int rvd = acbVideoDate(acb, vd, &task);
      printf("[video] videoData=%d (hdrType=%s)\n", rvd, htipo);
      if (!strcmp(htipo, "DolbyVision") && !strcasecmp(vidHdr, "HDR10")) {
        // O ACB aceita DolbyVision e a TV acende o badge mesmo quando o
        // demuxer do MKV so entregou a camada HDR10 — nesse caso o plano fica
        // sem imagem. O retorno sincrono nao detecta isso. Depois de negociar
        // DV, voltar para o formato REAL do decoder recupera imagem + HDR10.
        wait_(700);
        buildVideoDate(vd, sizeof vd, my, "HDR10", vuiFirst, vuiTrans, vuiMatrix);
        printf("[video] MKV DV delivered as HDR10; real fallback: %d\n",
               acbVideoDate(acb, vd, &task));
      } else if (!strcmp(htipo, "DolbyVision") && !strcasecmp(vidHdr, "none")) {
        // Profile DV que o demuxer desta TV nao reconheceu nem como camada
        // HDR10. O badge liga, mas nao ha quadro DV; voltar a SDR garante
        // imagem. MP4 reconhecido vem como DolbyVision e nao entra aqui.
        wait_(700);
        buildVideoDate(vd, sizeof vd, my, "none", 2, 2, 2);
        printf("[video] DV not recognised by the decoder; SDR fallback: %d\n",
               acbVideoDate(acb, vd, &task));
      } else if (rvd != 1 && strcmp(htipo, "none")) {
        buildVideoDate(vd, sizeof vd, my, "none", 2, 2, 2);
        printf("[video] videoData refused hdrType=%s, retrying without HDR: %d\n",
               htipo, acbVideoDate(acb, vd, &task));
      }
      printf("[video] audioData=%d\n", acbAudioDate(acb, ad, &task));
      fflush(stdout);
    }
    wait_(300);
    if (strcmp(media, my)) goto fora;
  }
  acbWindow(acb, windowX, windowY, windowW, windowH,
          (windowX == 0 && windowY == 0 && windowW == 1920 && windowH == 1080), &task);
  acbState(acb, NV_ACB_FOREGROUND, stPlaying, &task);
  printf("[video] plane pinned at %d,%d %dx%d\n", windowX, windowY, windowW, windowH);
  fflush(stdout);
fora:
  if (strcmp(media, my)) printf("[video] bind aborted: the media changed midway\n");
  bindAlive = 0;
  return NULL;
}

static int aoEvent(LSHandle *h, LSMessage *m, void *u) {
  const char *p = lsPayload(m);
  unsigned mySession = (unsigned)(uintptr_t)u;
  (void)h;
  if (mySession != session) return 1;
  if (!p) return 1;
  printf("[video] ev %s\n", p); fflush(stdout);
  if (strstr(p, "sourceInfo")) {
    const char *q;
    nAudio = nSub = 0;
    vidAtmos = 0;
    // Percorre audioTrackInfo item a item. O sourceInfo e um objeto so, entao
    // andar pelos "{" depois da chave do vetor e o suficiente aqui.
    q = strstr(p, "\"audioTrackInfo\"");
    if (q) {
      const char *endVet = strchr(q, ']');
      const char *o = strchr(q, '{');
      while (o && nAudio < NV_TRACK_MAX && (!endVet || o < endVet)) {
        const char *fo = strchr(o, '}');
        VideoTrack *f = &trackAudio[nAudio];
        char cod[16] = "", ch[8] = "", imm[16] = "";
        memset(f, 0, sizeof *f);
        f->number = nAudio;
        { const char *l = strstr(o, "\"language\":\"");
          if (l && (!fo || l < fo)) {
            size_t k = 0; l += 12;
            while (*l && *l != '"' && k + 1 < sizeof f->language) f->language[k++] = *l++;
            f->language[k] = 0;
            if (!strcmp(f->language, "(null)")) f->language[0] = 0;
          } }
        { const char *c2 = strstr(o, "\"codec\":\"");
          if (c2 && (!fo || c2 < fo)) {
            size_t k = 0; c2 += 9;
            while (*c2 && *c2 != '"' && k + 1 < sizeof cod) cod[k++] = *c2++;
            cod[k] = 0;
          } }
        { const char *m = strstr(o, "\"immersive\":\"");
          if (m && (!fo || m < fo)) {
            size_t k = 0; m += 13;
            while (*m && *m != '"' && k + 1 < sizeof imm) imm[k++] = *m++;
            imm[k] = 0;
            if (!strcasecmp(imm, "ATMOS")) vidAtmos = 1;
          } }
        { double c3 = numberOf(o, "\"channels\":");
          if (c3 == 6) snprintf(ch, sizeof ch, "5.1");
          else if (c3 == 8) snprintf(ch, sizeof ch, "7.1");
          else if (c3 == 2) snprintf(ch, sizeof ch, "2.0"); }
        snprintf(f->label, sizeof f->label, "%s%s%s%s%s",
                 f->language[0] ? languageReadable(f->language) : "Track",
                 imm[0] ? "  \xc2\xb7  " : (ch[0] ? "  \xc2\xb7  " : ""),
                 imm[0] ? "Atmos" : "",
                 (imm[0] && ch[0]) ? " " : "", ch);
        nAudio++;
        o = fo ? strchr(fo, '{') : NULL;
      }
    }
    // DIAGNOSTICO: despeja o sourceInfo CRU uma vez por titulo. A TV nao
    // devolve idioma de legenda nos arquivos do dono (todas saem como
    // "Legenda N"), e sem ver o JSON de verdade qualquer conserto e chute —
    // pode ser outro nome de campo, pode ser que o pipeline nao etiquete mesmo.
    // Ler com: sshpass ... scp root@TV:/tmp/nuvio-faixas.json .
    { static int evicted;
      if (!evicted) {
        FILE *fd = fopen("/tmp/nuvio-tracks.json", "w");
        if (fd) { fputs(p, fd); fclose(fd); evicted = 1; }
      } }

    q = strstr(p, "\"subtitleTrackInfo\"");
    if (q) {
      const char *endVet = strchr(q, ']');
      const char *o = strchr(q, '{');
      while (o && nSub < NV_TRACK_MAX && (!endVet || o < endVet)) {
        const char *fo = strchr(o, '}');
        VideoTrack *f = &trackSub[nSub];
        memset(f, 0, sizeof *f);
        f->number = (int)numberOf(o, "\"trackNum\":");
        { const char *l = strstr(o, "\"language\":\"");
          if (l && (!fo || l < fo)) {
            size_t k = 0; l += 12;
            while (*l && *l != '"' && k + 1 < sizeof f->language) f->language[k++] = *l++;
            f->language[k] = 0;
            if (!strcmp(f->language, "(null)")) f->language[0] = 0;
          } }
        // Arquivo sem etiqueta de idioma e o caso comum em MKV de release.
        // Numerar e honesto; inventar "Ingles" seria pior.
        if (f->language[0])
          snprintf(f->label, sizeof f->label, "%s", languageReadable(f->language));
        else
          snprintf(f->label, sizeof f->label, "Subtitle %d", f->number + 1);
        nSub++;
        o = fo ? strchr(fo, '{') : NULL;
      }
    }
    printf("[video] tracks: audio=%d subtitle=%d atmos=%d\n", nAudio, nSub, vidAtmos);
    fflush(stdout);

    // O PIPELINE NAO DA IDIOMA DE LEGENDA. Medido nesta TV, num arquivo com 43
    // legendas: o audioTrackInfo vem com "en"/"es"/"fr"/"it" e TODA entrada do
    // subtitleTrackInfo vem com "language":"(null)". Nao ha outro campo ali —
    // a informacao nao sai do pipeline, e a lista virava "Legenda 1..43", que
    // nao ajuda ninguem a escolher.
    //
    // O jeito de saber e ler o proprio arquivo, que e o que o navegador faz de
    // graca no app web. Dispara um fio que baixa os primeiros 2 MB por Range e
    // le o elemento Tracks do Matroska; quando volta, casa por trackNum e
    // reescreve os rotulos. Nao bloqueia a reproducao: se falhar, ou se o
    // arquivo nao for MKV, fica o que ja estava.
    { int missing = 0, i;
      for (i = 0; i < nSub; i++) if (!trackSub[i].language[0]) missing = 1;
      // SO ANOTA. Quem dispara e o video_bombear, quando o buffer estiver
      // saudavel — a sonda concorre com a propria reproducao (mesma conexao,
      // mesmo servidor) e o sourceInfo chega justamente no pior instante, com o
      // pipeline ainda enchendo o buffer. MEDIDO na TV: buffer em falta 1,6 s
      // depois da leitura, caindo a 2,8 s e levando 9 s para se recuperar.
      //
      // O idioma da legenda nao tem pressa: so importa quando o dono abre a
      // folha de faixas.
      // MP4 nunca tem Tracks de Matroska: sondar e trafego garantidamente
      // perdido, e ele sai da MESMA conexao do video.
      if (missing && !sourceMp4) mkvPending = 1;
      else if (missing) mark("mkv: source is MP4, probe skipped"); }
  }

  if (strstr(p, "videoInfo")) {
    viuVideo = 1;   // fecha o prazo do recuo de DV
    double v;
    v = numberOf(p, "\"width\":");      if (v > 0) vidW = (int)v;
    v = numberOf(p, "\"height\":");     if (v > 0) vidH = (int)v;
    v = numberOf(p, "\"frameRate\":");  if (v > 0) vidRate = (int)v;
    v = numberOf(p, "\"bitRate\":");    if (v > 0) vidBits = (long)v;
    { const char *q = strstr(p, "\"scanType\":\"");
      if (q) { const char *f; q += 12; f = strchr(q, '"');
        if (f && f - q < (int)sizeof vidScan) {
          memcpy(vidScan, q, f - q); vidScan[f - q] = 0; } } }
    { const char *q = strstr(p, "\"hdrType\":\"");
      if (q) { const char *f; q += 11; f = strchr(q, '"');
        if (f && f - q < (int)sizeof vidHdr) {
          memcpy(vidHdr, q, f - q); vidHdr[f - q] = 0;
          // Junto com o que a FONTE afirmava. Sozinho, o hdrType nao responde a
          // pergunta que importa em MKV: "pedimos Dolby Vision e a TV entregou
          // Dolby Vision, ou ela rebaixou para HDR10?". Os relatos de fora
          // (Kodi, Plex, UMS) dizem que o webOS aciona DV nativo em MP4 perfis
          // 5 e 8 e cai para HDR10 em Matroska; esta linha e o que permite
          // confirmar ou desmentir isso NESTA TV, com medida em vez de fama.
          printf("[video] pipeline HDR: %s (source claimed DV=%d)\n",
                 vidHdr, dvRequest);
          // Vai tambem para os MARCOS, que sao legiveis no aparelho: o stdout
          // do app lancado pelo applicationManager nao chega a lugar nenhum, e
          // era por isso que esta medida — a unica que responde se a TV honrou
          // ou rebaixou o Dolby Vision — so existia em teoria.
          { char m[64];
            snprintf(m, sizeof m, "pipeline hdr: %s (source DV=%d)",
                     vidHdr, dvRequest);
            mark(m); } } } }
    // O Nuvio web que toca corretamente repassa estes valores sem alterar.
    // Para DolbyVision ele omite os dois blocos; montarVideoData faz o mesmo.
    { double x;
#define READ_SEI(name, dst) do { x = numberOf(p, "\"" name "\":"); if (x >= 0) dst = (long)x; } while (0)
      READ_SEI("displayPrimariesX0", seiX0); READ_SEI("displayPrimariesX1", seiX1);
      READ_SEI("displayPrimariesX2", seiX2); READ_SEI("displayPrimariesY0", seiY0);
      READ_SEI("displayPrimariesY1", seiY1); READ_SEI("displayPrimariesY2", seiY2);
      READ_SEI("whitePointX", seiWhiteX); READ_SEI("whitePointY", seiWhiteY);
      READ_SEI("minDisplayMasteringLuminance", seiMinLuma);
      READ_SEI("maxDisplayMasteringLuminance", seiMaxLuma);
      READ_SEI("maxContentLightLevel", seiMaxCLL);
      READ_SEI("maxPicAverageLightLevel", seiMaxFALL);
#undef READ_SEI
      x = numberOf(p, "\"colorPrimaries\":"); if (x >= 0) vuiFirst = (int)x;
      x = numberOf(p, "\"transferCharacteristics\":"); if (x >= 0) vuiTrans = (int)x;
      x = numberOf(p, "\"matrixCoeffs\":"); if (x >= 0) vuiMatrix = (int)x;
    }
  }
  if (strstr(p, "loadCompleted")) {
    mark("video loadCompleted");
    // ORDEM: faixas primeiro, posicao depois. Trocar de faixa reinicia o
    // decode no pipeline; fazer isso DEPOIS do seek jogaria a posicao fora.
    if (audioOnLoad >= 0) {
      int a2 = audioOnLoad; audioOnLoad = -1;
      if (a2 > 0) video_choose_audio(a2);
    }
    if (subUrlOnLoad[0]) {
      char u[1024];
      snprintf(u, sizeof u, "%s", subUrlOnLoad);
      subUrlOnLoad[0] = 0; subOnLoad = -1;
      video_subtitle_external(u);
    } else if (subOnLoad >= 0) {
      int l2 = subOnLoad; subOnLoad = -1;
      video_choose_subtitle(l2);
    }
    if (posOnLoad > 1.0) {
      double target = posOnLoad;
      posOnLoad = 0.0;
      video_fetch(target);
      mark("retomado apos queda do pipeline");
    }
    // O pipeline e novo: o estilo da legenda nao sobrevive ao load anterior.
    applyStyle();
    ready = 1;
    if (cronRequested && !cronLoad) {
      cronLoad = 1;
      printf("[video] load->loadCompleted %lums\n", msSinceRequest());
    }
    // O bind do ACB vai para um fio proprio COM PAUSAS entre os passos.
    // Motivo medido: cada chamada do AcbAPI e assincrona (o servico responde
    // pelo barramento) e disparando tudo em sequencia o setMediaVideoData
    // chegava antes do register terminar — o servico respondia
    // "piplineID key Error!!", parava a sequencia e nunca mandava o stopMute.
    // Sem o stopMute o video fica mudo: tela preta com audio normal.
    // E POR SESSAO, nao uma vez por processo: sem isto a segunda reproducao
    // herda um ACB apontando para o mediaId morto da anterior.
    if (acb && media[0]) bindPending = 1;
  }
  if (strstr(p, "bufferRange")) {
    double e = numberOf(p, "\"endTime\":");
    if (e >= 0) bufferSeg = e;
  }
  // PAUSA POR FALTA DE DADOS. O dono relatou "fica pausando" e os marcos nao
  // registravam NADA — porque encher e esvaziar o buffer nao gera evento neste
  // lado, e uma pausa dessas nao passa por `paused` nem por erro. Sem isto a
  // unica coisa que sobra e adivinhar.
  //
  // Carimba quanto do buffer havia no instante: e o numero que separa "a fonte
  // nao entrega" de "o decoder engasgou".
  if (strstr(p, "bufferingStart")) {
    char m[64];
    snprintf(m, sizeof m, "buffering START (buffer %+.1fs ahead)",
             bufferSeg - posSeg);
    mark(m);
  }
  if (strstr(p, "bufferingEnd")) {
    char m[64];
    snprintf(m, sizeof m, "buffering END (buffer %+.1fs ahead)",
             bufferSeg - posSeg);
    mark(m);
  }
  if (strstr(p, "playing")) {
    playing = 1;
    if (acb && media[0]) {
      long task = 0;
      acbWindow(acb, windowX, windowY, windowW, windowH,
                (windowX == 0 && windowY == 0 && windowW == 1920 && windowH == 1080), &task);
      printf("[video] window reapplied with the stream already playing\n"); fflush(stdout);
    }
  }
  if (strstr(p, "paused")) {
    // So carimba quando NAO fomos nos que pausamos: pausa do dono e esperada,
    // pausa vinda do pipeline e o defeito.
    if (playing && !pauseRequested) mark("paused BY THE PIPELINE");
    playing = 0;
  }
  if (strstr(p, "endOfStream")) { playing = 0; mark("endOfStream"); }

  // ERRO DO PIPELINE. Nao havia tratamento nenhum: quando o uMS recusava um
  // seek ou perdia a fonte, o app simplesmente parava e ninguem sabia por que —
  // "eu passei e ele nao continuou mais" e exatamente o formato desse silencio.
  // Nao ha o que consertar sem saber a causa, e a causa vem no proprio evento.
  // ERRO DE VERDADE, e nao "errorCode: 0".
  //
  // A primeira versao carimbava tudo que tivesse `errorCode`, e o uMS manda
  // esse campo em resposta NORMAL — os marcos encheram de
  // `pipeline erro: errorText":"No Error"`, que e ruido escondendo o sinal.
  if (strstr(p, "errorText") && !strstr(p, "\"No Error\"")) {
    const char *q = strstr(p, "errorText");
    char m[96];
    snprintf(m, sizeof m, "pipeline error: %.60s", q);
    { char *n2; for (n2 = m; *n2; n2++) if (*n2 == '\n' || *n2 == '\r') *n2 = ' '; }
    mark(m);
    // PIPELINE DESTRUIDO. Medido duas vezes na TV do dono: ~71 s depois de um
    // avanco, o uMS responde "com.webos.pipeline.<id> is not running" e o video
    // simplesmente para — o app nao fazia NADA, e era isso que ele descrevia
    // como "passei e nao continuou mais".
    //
    // Recarrega a mesma fonte e volta para onde estava. Nao e conserto da
    // CAUSA (o pipeline morre por algo entre o seek e a fonte do debrid, que
    // este lado nao enxerga), e sim de nao deixar o dono na tela parada.
    if (strstr(p, "is not running") && urlCurrent[0] && !recovering) {
      recovering = 1;
      resumeIn = posSeg;
      // GUARDA AS ESCOLHAS. A posicao sozinha nao basta: o pipeline novo nasce
      // com a faixa 0 e a legenda desligada.
      audioOnLoad = audioCurrent;
      subOnLoad   = subCurrent;
      snprintf(subUrlOnLoad, sizeof subUrlOnLoad, "%s", subUrlCurrent);
      mark("pipeline morreu: recarregando");
    }
  }
  { double v = numberOf(p, "\"currentTime\":");
    if (v >= 0) {
      posSeg = v / 1000.0;
      // Primeiro quadro com avanco: o numero de inicio de verdade.
      if (cronRequested && !cronFrame && posSeg > 0.0) {
        cronFrame = 1;
        printf("[video] load->first frame %lums\n", msSinceRequest());
        fflush(stdout);
      }
    } }
  { double v = numberOf(p, "\"duration\":");
    if (v >= 0) durationSeg = v / 1000.0; }
  return 1;
}

static int soLog(LSHandle *h, LSMessage *m, void *u) {
  (void)h; (void)u;
  printf("[video] %s\n", lsPayload(m)); fflush(stdout);
  return 1;
}

static void call(const char *method, const char *load, Filter cb) {
  char uri[128]; unsigned long token = 0;
  snprintf(uri, sizeof uri, "luna://com.webos.media/%s", method);
  if (!lsCall(bus, uri, load, cb, NULL, &token, ERROR))
    printf("[video] %s failed\n", method);
}

// Variante para callbacks que precisam saber a qual sessao pertencem. O
// contexto e um inteiro convertido em ponteiro; nao ha alocacao para vazar nem
// memoria cujo tempo de vida possa acabar antes da resposta assincrona.
static void callCtx(const char *method, const char *load, Filter cb,
                      void *ctx) {
  char uri[128]; unsigned long token = 0;
  snprintf(uri, sizeof uri, "luna://com.webos.media/%s", method);
  if (!lsCall(bus, uri, load, cb, ctx, &token, ERROR))
    printf("[video] %s failed\n", method);
}

// Chamada a OUTRO servico. O recorte de fonte NAO mora no com.webos.media: ele
// respondeu `Unknown method "setDisplayWindow" for category "/"`, e a lista do
// `ls-monitor -i com.webos.media` confirma que nao existe ali. Quem tem os
// metodos de janela e o com.webos.service.tv.display:
//
//   "setDisplayWindow":       {"provides":["tv.management","private","tv.settings","all","public"]}
//   "setCustomDisplayWindow": idem
//
// setCustomDisplayWindow e o que aceita a fonte junto do destino, que e o
// recorte de que o zoom precisa.
static void callIn(const char *service, const char *method,
                     const char *load, Filter cb) {
  char uri[160]; unsigned long token = 0;
  snprintf(uri, sizeof uri, "luna://%s/%s", service, method);
  if (!lsCall(bus, uri, load, cb, NULL, &token, ERROR))
    printf("[video] %s/%s failed\n", service, method);
}

static int aoLoad(LSHandle *h, LSMessage *m, void *u) {
  const char *p = lsPayload(m), *q;
  char b[256];
  unsigned mySession = (unsigned)(uintptr_t)u;
  (void)h;
  printf("[video] load: %s\n", p ? p : "(null)"); fflush(stdout);
  if (mySession != session) {
    printf("[video] stale load ignored (session %u, current %u)\n",
           mySession, session);
    // Um load cancelado ainda pode criar um pipeline no uMS. Liberar esse
    // recurso evita deixar o decoder ocupado quando o usuario reabre o filme.
    char old[96] = "";
    if (p) js_text(p, NULL, "mediaId", old, sizeof old);
    if (old[0] && strcmp(old, media)) {
      snprintf(b, sizeof b, "{\"mediaId\":\"%s\"}", old);
      call("unload", b, soLog);
    }
    return 1;
  }
  if (!p || media[0]) return 1;
  q = strstr(p, "\"mediaId\":\"");
  if (!q) return 1;
  q += 11;
  { const char *f = strchr(q, '"');
    if (!f || f - q >= (int)sizeof media) return 1;
    memcpy(media, q, f - q); media[f - q] = 0; }

  snprintf(b, sizeof b, "{\"connectionId\":\"%s\"}", media);
  call("notifyForeground", b, soLog);
  snprintf(b, sizeof b, "{\"mediaId\":\"%s\"}", media);
  callCtx("subscribe", b, aoEvent, (void *)(uintptr_t)mySession);
  snprintf(b, sizeof b, "{\"mediaId\":\"%s\",\"type\":\"video\",\"index\":0}", media);
  call("selectTrack", b, soLog);
  snprintf(b, sizeof b, "{\"mediaId\":\"%s\"}", media);
  call("play", b, soLog);
  return 1;
}

static void *runLoop(void *u) { (void)u; loopRun(loop); return NULL; }

// O ACB EXIGE um callback de verdade. Passar NULL nao e ignorado: no primeiro
// evento ele salta para o endereco 0 e o app morre com SIGSEGV em pc=0x0, longe
// do ponto onde o NULL foi escrito.
static void acbNotified(long h, long task, long event,
                         long stApp, long stPlays, int response) {
  (void)h; (void)task;
  printf("[video] acb event=%ld app=%ld play=%ld resp=%d\n",
         event, stApp, stPlays, response);
  fflush(stdout);
}

#define SIM(h, v, n) do { \
    *(void **)(&v) = dlsym(h, n); \
    if (!v) { printf("[video] missing %s\n", n); return 0; } \
  } while (0)

int video_start(void) {
  void *L, *G, *A;
  if (on) return 1;
  L = dlopen("libluna-service2.so.3", RTLD_NOW);
  if (!L) L = dlopen("libluna-service2.so", RTLD_NOW);
  G = dlopen("libglib-2.0.so.0", RTLD_NOW);
  A = dlopen("libAcbAPI.so.1", RTLD_NOW);
  if (!L || !G || !A) { printf("[video] libs: %s\n", dlerror()); return 0; }

  SIM(L, lsRegister, "LSRegister");
  SIM(L, lsAttach,   "LSGmainAttach");
  SIM(L, lsCall,     "LSCall");
  SIM(L, lsPayload,  "LSMessageGetPayload");
  SIM(G, loopNew,   "g_main_loop_new");
  SIM(G, loopRun,  "g_main_loop_run");
  SIM(G, loopStop,  "g_main_loop_quit");
  SIM(A, acbCreate,    "AcbAPI_create");
  SIM(A, acbStart,  "AcbAPI_initialize");
  SIM(A, acbSink,     "AcbAPI_setSinkType");
  SIM(A, acbMedia,    "AcbAPI_setMediaId");
  SIM(A, acbState,   "AcbAPI_setState");
  SIM(A, acbWindow,   "AcbAPI_setDisplayWindow");
  // NAO usa SIM: se a lib desta TV nao tiver o simbolo, o app segue sem zoom
  // em vez de nao iniciar. O recorte e util, mas nao vale o app inteiro.
  *(void **)(&acbWindowCustom) = dlsym(A, "AcbAPI_setCustomDisplayWindow");
  if (!acbWindowCustom) printf("[video] no AcbAPI_setCustomDisplayWindow; zoom is unavailable\n");
  SIM(A, acbDestroy, "AcbAPI_destroy");
  SIM(A, acbConnect, "AcbAPI_connectDass");
  SIM(A, acbVideoDate, "AcbAPI_setMediaVideoData");
  SIM(A, acbAudioDate, "AcbAPI_setMediaAudioData");

  // O nome PRECISA casar com o padrao do papel LS2 do app
  // (allowedNames: "com.webos.media.client.*"). Qualquer outro nome e recusado
  // pelo hub e nada depois disso acontece.
  if (!lsRegister("com.webos.media.client.nuvio", &bus, ERROR)) {
    printf("[video] LSRegister recusado\n"); return 0;
  }
  loop = loopNew(NULL, 0);
  if (!lsAttach(bus, loop, ERROR)) { printf("[video] attach failed\n"); return 0; }
  // Laco proprio: o LS2 exige um GMainLoop girando, e girar isso no laco de
  // desenho custaria quadros. As respostas chegam neste fio e so mexem em
  // variaveis simples, lidas pelo desenho sem trava.
  pthread_create(&thread, NULL, runLoop, NULL);

  readSettingsAcb();
  acb = acbCreate();
    acbStart(acb, kindPlayer, "space.nuvio.native.legacy", (void *)acbNotified);
  acbSink(acb, kindSink);
  on = 1;
  printf("[video] ready (acb=%ld)\n", acb); fflush(stdout);
  return 1;
}

// --- recuo automatico do Dolby Vision ---------------------------------------
//
// MEDIDO na OLED65C9, dois arquivos DV em MKV:
//   arquivo A: sem DolbyHdrInfo toca em HDR10; COM o bloco engata Dolby Vision.
//   arquivo B: sem o bloco toca normal (HDR10, com imagem); COM o bloco fica
//              SO O AUDIO, e o pipeline nunca reporta videoInfo.
// Testado 8/"single" e 7/"dual" no arquivo B: os dois quebram igual.
//
// Como nao demuxamos, nao ha como saber de antemao em qual dos dois casos a
// fonte cai — declarar as cegas ganha DV num arquivo e perde a IMAGEM no outro,
// que e troca ruim. Entao a declaracao vira uma APOSTA COM PRAZO: se o pipeline
// nao reportar videoInfo em NV_DV_PRAZO_MS, recarrega a mesma URL sem o bloco.
// O custo e alguns segundos no arquivo que nao aceita; o ganho e nunca ficar
// sem imagem por causa de uma afirmacao nossa.
#define NV_DV_PRAZO_MS 7000


// --- idioma das legendas lido do proprio arquivo -----------------------------
// Ver a nota no ponto de disparo, logo abaixo do parse do sourceInfo.
static void *readMkv(void *arg) {
  MkvTrack fx[MKV_MAX_TRACKS];
  char url[1024];
  int n, i, j, matched = 0;
  (void)arg;

  snprintf(url, sizeof url, "%s", urlCurrent);

  n = mkv_tracks(url, fx, MKV_MAX_TRACKS);
  if (n < 1) {
    // Sem isto o unico sinal era uma linha de stdout, que na TV nao chega a
    // lugar nenhum — e a lista ficava em "Legenda 1, Legenda 2" sem ninguem
    // saber se o arquivo nao e MKV, se o Range falhou ou se o cabecalho passa
    // dos 2 MB que baixamos.
    mark("mkv: no track read (not an MKV, or Range failed)");
    threadMkvAlive = 0; return NULL;
  }

  // SEM MUTEX, e de proposito: este arquivo nao tem um. faixaLeg ja e escrito
  // pelo fio de resposta do luna e lido pelo desenho sem trava nenhuma, e
  // introduzir uma trava so aqui daria falsa seguranca — protegeria a escrita
  // e nao a leitura. O dano possivel e um rotulo lido pela metade em UM quadro;
  // por isso cada campo e preenchido de uma vez, com um snprintf so, e o
  // rotulo (que e o que aparece) e escrito por ULTIMO, depois do idioma.
  // O trackNum do sourceInfo da LG e o TrackNumber do Matroska: casar por ele,
  // e nao por ordem. As duas listas nao vem na mesma ordem (o sourceInfo desta
  // TV comecou em 42, 40, 41, 32...), e casar por posicao trocaria os idiomas
  // de lugar — pior que nao ter idioma nenhum.
  for (i = 0; i < nSub; i++) {
    if (trackSub[i].language[0]) continue;
    for (j = 0; j < n; j++) {
      if (fx[j].number != trackSub[i].number) continue;
      if (fx[j].language[0] && strcmp(fx[j].language, "und")) {
        snprintf(trackSub[i].language, sizeof trackSub[i].language, "%s", fx[j].language);
        matched++;
      }
      // O NOME da faixa ("Forced", "SDH", "Full") e o que separa duas legendas
      // do MESMO idioma. Sem ele o dono ve "Portugues" tres vezes e escolhe no
      // escuro — e essa e justamente a lista que ele reclamou.
      if (fx[j].name[0])
        snprintf(trackSub[i].label, sizeof trackSub[i].label, "%s%s%s",
                 trackSub[i].language[0] ? languageReadable(trackSub[i].language) : "",
                 trackSub[i].language[0] ? "  \xc2\xb7  " : "", fx[j].name);
      else if (trackSub[i].language[0])
        snprintf(trackSub[i].label, sizeof trackSub[i].label, "%s",
                 languageReadable(trackSub[i].language));
      break;
    }
  }
  { char m[64];
    snprintf(m, sizeof m, "mkv: %d tracks read, %d subtitles with a language", n, matched);
    mark(m); }
  printf("[mkv] %d subtitles gained a language\n", matched);
  fflush(stdout);
  threadMkvAlive = 0;
  return NULL;
}

static long  msOfLoad = 0;

static long nowMs(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int playInternal(const char *url, int comDV);

int video_play(const char *url) {
  dvInset = 0;
  snprintf(urlCurrent, sizeof urlCurrent, "%s", url ? url : "");
  return playInternal(url, 1);
}

// Chamado uma vez por quadro. So existe para o prazo acima: sem ele o recuo
// dependeria de o usuario perceber que nao ha imagem e sair da tela.
void video_pump(void) {
  // O ACB demora cerca de 1,5 s para ligar uma sessao. Se o usuario sair e
  // reabrir nesse intervalo, o loadCompleted novo encontra bindVivo=1. Antes
  // ele simplesmente desistia para sempre; agora o pedido fica pendente.
  if (bindPending && !bindAlive && acb && media[0]) {
    bindPending = 0;
    bindAlive = 1;
    if (pthread_create(&threadBind, NULL, prenderPlane, NULL) == 0)
      pthread_detach(threadBind);
    else {
      bindAlive = 0;
      bindPending = 1;
    }
  }
  // SONDA DE MKV so com folga de buffer. 20 s a frente e o sinal de que a
  // fonte esta entregando mais rapido do que o decoder consome, e portanto de
  // que ha banda sobrando para os 320 KB do cabecalho.
  if (mkvPending && !threadMkvAlive && urlCurrent[0] && bufferSeg - posSeg >= 20.0) {
    mkvPending = 0;
    threadMkvAlive = 1;
    if (pthread_create(&threadMkv, NULL, readMkv, NULL) != 0) threadMkvAlive = 0;
    else pthread_detach(threadMkv);
  }
  // Avanco pendente que ja repousou.
  if (seekIn && SDL_GetTicks() >= seekIn) {
    Uint32 q = seekIn; seekIn = 0; (void)q;
    seekNow(seekTarget);
  }
  // RECUPERACAO DO PIPELINE, no fio principal. Ver a nota em `recuperando`.
  if (recovering) {
    double target = resumeIn;
    recovering = 0;
    mark("reloading the source");
    if (playInternal(urlCurrent, 1) && target > 1.0) {
      // O seek so vale depois do load; guardar o alvo e deixar o
      // loadCompleted aplica-lo evita mandar posicao para um pipeline que
      // ainda nao existe.
      posOnLoad = target;
    }
  }
  // O recuo por prazo foi REMOVIDO por nao funcionar: o gatilho era "o pipeline
  // nao reportou videoInfo", e o uMS reporta videoInfo, sourceInfo e
  // loadCompleted normalmente mesmo nos arquivos que ficam sem imagem. Medido:
  // videoInfo 3840x1606 hdrType=DolbyVision e loadCompleted em 3212ms, tela
  // preta com audio correndo. Nao ha no uMS sinal de QUADRO EXIBIDO — o
  // currentTime avanca puxado pelo audio.
  //
  // A funcao fica porque a batida por quadro e util assim que existir um sinal
  // melhor (contador de quadros, ou o proprio perfil lido do MKV).
  (void)dvInLoad; (void)dvInset; (void)viuVideo;
  (void)msOfLoad; (void)urlCurrent; (void)nowMs; (void)playInternal;
}

static int playInternal(const char *url, int comDV) {
  char load[2048];
  unsigned mySession;
  if (!on && !video_start()) return 0;
  video_stop();
  mySession = ++session;
  viuVideo = 0;
  // O retangulo aplicado e da SESSAO: sem zerar, uma sessao nova que calcule o
  // mesmo rect cairia no "ja e esse" e nunca chegaria a mandar nada ao plano.
  // (semUms NAO zera: se esta TV nao entende o recorte de fonte, nao passa a
  // entender no titulo seguinte, e insistir so arrisca a imagem de novo.)
  fontX = -1; dstX = dstY = dstW = dstH = -1;
  posSeg = durationSeg = bufferSeg = 0; playing = ready = 0; media[0] = 0;
  nAudio = nSub = 0; audioCurrent = 0; subCurrent = -1; vidAtmos = 0;
  subUrlCurrent[0] = 0; mkvPending = 0;
  snprintf(vidHdr, sizeof vidHdr, "none");
  seiX0 = seiX1 = seiX2 = seiY0 = seiY1 = seiY2 = 0;
  seiWhiteX = seiWhiteY = seiMinLuma = seiMaxLuma = seiMaxCLL = seiMaxFALL = 0;
  vuiFirst = vuiTrans = vuiMatrix = 2;
  // A afirmacao de DV da fonte escolhida sobrevive ao reset: e ela que o bind
  // descreve ao tv.display. Sem ela, toda sessao nasceria "none".
  vidDV = dvRequest;
  cronRequested = 1; cronLoad = 0; cronFrame = 0;
  clock_gettime(CLOCK_MONOTONIC, &t0Request);
  // windowId "window_id_dummy" NAO e enfeite: com string vazia o load responde
  // returnValue:true, aloca mediaId e nunca busca o arquivo. Falha muda.
  // DolbyHdrInfo: e assim que o Kodi anuncia Dolby Vision a este mesmo pipeline
  // (xbmc/cores/VideoPlayer/MediaPipelineWebOS.cpp):
  //   contents["DolbyHdrInfo"]["encryptionType"] = "clear"
  //   contents["DolbyHdrInfo"]["profileId"]      = dovi.dv_profile
  //   contents["DolbyHdrInfo"]["trackType"]      = el_present_flag ? "dual" : "single"
  //
  // DIFERENCA QUE PODE INVALIDAR TUDO ISTO, e por isso e um EXPERIMENTO: o Kodi
  // demuxa com ffmpeg e ENTREGA BUFFERS por option.externalStreamingInfo, onde
  // esse bloco vive. Nos passamos uma URI e a TV faz HTTP, demux e decode.
  // Declarar o bloco no modo URI pode ser ignorado em silencio — e a unica forma
  // de saber e medir o hdrType que volta.
  //
  // profileId 8 / "single" e o que o Kodi declara DEPOIS de converter o perfil 7,
  // nao o que o arquivo tem. Como nao demuxamos, nao sabemos o perfil real; por
  // isso os valores sao ajustaveis por variavel de ambiente para poder testar
  // 7/"dual" contra 8/"single" no mesmo arquivo sem recompilar.
  char dolby[192] = "";
  dvInLoad = 0;
  if (dvRequest && comDV) {
    // Os valores tambem saem de /tmp/nuvio-dv.conf ("<perfil> <trilha>", ex:
    // "7 dual"), porque o app e lancado pelo SAM e nao da para passar variavel
    // de ambiente por ali. Sem o arquivo, valem o ambiente e depois o padrao.
    static char pFile[16], tFile[16];
    const char *profile = getenv("NUVIO_DV_PROFILE");
    const char *track = getenv("NUVIO_DV_TRACK");
    { FILE *f = fopen("/tmp/nuvio-dv.conf", "r");
      if (f) {
        pFile[0] = tFile[0] = 0;
        if (fscanf(f, "%15s %15s", pFile, tFile) >= 1) {
          if (pFile[0]) profile = pFile;
          if (tFile[0]) track = tFile;
        }
        fclose(f);
      } }
    // DESLIGADO POR PADRAO, e a razao esta medida:
    //
    //   arquivo A: sem o bloco toca em HDR10; COM o bloco engata Dolby Vision.
    //   varios outros: sem o bloco tocam normal; COM o bloco ficam SEM IMAGEM
    //                  (um chegou a mostrar o primeiro quadro e congelar, com o
    //                  audio correndo).
    //
    // Ganhar DV num arquivo e perder a imagem em varios e troca ruim. E nao ha
    // como escolher sozinho: tentei um prazo que recarregaria sem o bloco caso
    // o pipeline nao reportasse video, e ele NAO SERVE — o uMS reporta videoInfo
    // (3840x1606, hdrType DolbyVision) e loadCompleted normalmente mesmo quando
    // nenhum quadro chega ao plano. "Reportou" nao e "exibiu", e nao existe no
    // uMS um sinal de quadro avancando: currentTime anda com o audio.
    //
    // Entao vira OPT-IN, para experimentar arquivo a arquivo:
    //   echo "8 single" > /tmp/nuvio-dv.conf   (ou "7 dual")
    //   rm /tmp/nuvio-dv.conf                  volta ao seguro
    // O caminho definitivo e saber o perfil real do arquivo antes de afirmar
    // qualquer coisa: ler o cabecalho do MKV por HTTP Range e achar o
    // BlockAdditionMapping com dvcC/dvvC, que e onde o Matroska guarda isso.
    if (!profile || !*profile || !strcmp(profile, "off")) {
      /* sem declaracao: comportamento conhecido e seguro */
    } else {
      snprintf(dolby, sizeof dolby,
               "\"externalStreamingInfo\":{\"contents\":{\"DolbyHdrInfo\":{"
               "\"encryptionType\":\"clear\",\"profileId\":%s,\"trackType\":\"%s\"}}},",
               (profile && *profile) ? profile : "8",
               (track && *track) ? track : "single");
      printf("[video] DolbyHdrInfo declarado: %s\n", dolby); fflush(stdout);
      dvInLoad = 1;
    }
  }
  snprintf(load, sizeof load,
      "{\"payload\":{\"option\":{\"useSeekableRanges\":true,"
      "\"appId\":\"space.nuvio.native.legacy\","
      "%s"
      "\"bufferControl\":{\"userBufferCtrl\":false},"
      "\"windowId\":\"window_id_dummy\"}},"
      "\"uri\":\"%s\",\"type\":\"media\"}", dolby, url);
  printf("[video] URL: %s\n", url); fflush(stdout);
  msOfLoad = nowMs();
  callCtx("load", load, aoLoad, (void *)(uintptr_t)mySession);
  return 1;
}

void video_stop(void) {
  char b[128];
  // Invalida tambem a sessao que ainda esta esperando o retorno de load. Esse
  // era o caso abrir -> sair -> abrir que travava: nao havia mediaId para
  // descarregar, mas o callback antigo continuava vivo e contaminava o novo.
  session++;
  bindPending = 0;
  recovering = 0; resumeIn = posOnLoad = 0.0;
  audioOnLoad = subOnLoad = -1;
  subUrlOnLoad[0] = 0;
  pauseRequested = 0; seekIn = 0; mkvPending = 0;
  if (on && media[0]) {
    snprintf(b, sizeof b, "{\"mediaId\":\"%s\"}", media);
    call("unload", b, soLog);
  }
  media[0] = 0; playing = ready = 0;
}

void video_pause(int paused) {
  char b[128];
  if (!on || !media[0]) return;
  snprintf(b, sizeof b, "{\"mediaId\":\"%s\"}", media);
  call(paused ? "pause" : "play", b, soLog);
  playing = !paused;
  pauseRequested = paused;
}

// AVANCO COM REPOUSO.
//
// Medido na TV: segurar a seta produzia QUATRO seeks em 0,8 s (16 s, 26 s, 36 s,
// 46 s) — quatro pedidos de faixa seguidos a mesma fonte, e ~71 s depois o
// pipeline morria. Nao esta provado que um causa o outro, mas mandar quatro
// posicoes quando o dono quis UMA e desperdicio de qualquer forma: as tres
// primeiras sao descartadas assim que a quarta chega.
//
// A posicao MOSTRADA muda na hora (senao a barra nao responde ao toque); o que
// espera o repouso e o comando ao pipeline.
#define SEEK_IDLE_MS 350

void video_fetch(double seconds) {
  if (!on || !media[0]) return;
  if (seconds < 0) seconds = 0;
  posSeg = seconds;
  seekTarget = seconds;
  seekIn = SDL_GetTicks() + SEEK_IDLE_MS;
}

// Manda de fato. Chamado pelo video_bombear quando o repouso vence.
static void seekNow(double seconds) {
  char b[192];
  if (!on || !media[0]) return;
  snprintf(b, sizeof b, "{\"mediaId\":\"%s\",\"position\":%d}",
           media, (int)(seconds * 1000.0));
  call("seek", b, soLog);
  { char m[48]; snprintf(m, sizeof m, "seek to %ds", (int)seconds); mark(m); }
}

// O retangulo do plano de hardware. E por AQUI que os modos de zoom acontecem:
// o video nao e um elemento com `transform: scale()` como no app web — e um
// plano atras da superficie GL, e ampliar significa mandar um retangulo MAIOR
// que a tela, com x/y negativos, e deixar o excedente sair pela borda. E o
// mesmo resultado do transform do web: a barra preta embutida no quadro sai da
// area visivel em vez de ser (impossivelmente) recortada por object-fit.
//
// O retangulo NAO pode passar da tela. MEDIDO: mandar ao ACB um retangulo com
// origem negativa ou maior que o painel (que era como eu tentava ampliar) NAO
// recorta nada — o plano simplesmente APAGA, e a tela fica preta em todo modo
// com escala, com imagem so no ORIGINAL, o unico onde a escala e 1. O
// acbJanela recebe UM retangulo, o de DESTINO, e nao existe recorte de fonte
// ali; um plano de hardware nao descarta o excedente como o compositor do
// navegador faz com transform: scale(). Quem amplia e o video_janela_fonte
// abaixo, recortando a FONTE.
void video_window(int x, int y, int w, int h) {
  long task = 0;
  int full = (x == 0 && y == 0 && w == 1920 && h == 1080);
  if (w < 1 || h < 1) return;
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > 1920) w = 1920 - x;
  if (y + h > 1080) h = 1080 - y;
  if (w < 1 || h < 1) return;
  if (x == windowX && y == windowY && w == windowW && h == windowH) return;  // sem repetir o mesmo rect a cada quadro
  windowX = x; windowY = y; windowW = w; windowH = h;
  if (!on || !acb || !media[0]) return;   // sem midia presa, aplicar seria no vazio
  printf("[video] window %d,%d %dx%d full=%d\n", x, y, w, h, full);
  fflush(stdout);
  acbWindow(acb, x, y, w, h, full, &task);
}

// A resposta do uMS ao setDisplayWindow, LOGADA — e AGIDA.
//
// A assinatura source/destination ainda nao foi confirmada nesta TV (o
// ls-monitor ficou para depois, a TV estava ocupada). Se ela estiver errada, o
// pedido e recusado e o plano fica com o retangulo de antes — ou seja, o
// sintoma seria de novo "tela preta no zoom", que e exatamente o erro que esta
// rodada corrigiu. Entao a recusa nao pode passar calada: ao primeiro
// returnValue:false o modulo DESISTE do caminho do uMS e volta ao acbJanela em
// tela cheia. Perde-se o zoom, que e um recurso; nao se perde a imagem, que e o
// filme. Errar para o lado de continuar mostrando video e a unica escolha
// defensavel enquanto isto nao foi medido.
static int semUms;   // 1 depois que o uMS recusou o setDisplayWindow

static int aoWindow(LSHandle *h, LSMessage *m, void *u) {
  const char *p = lsPayload(m);
  (void)h; (void)u;
  printf("[video] setDisplayWindow -> %s\n", p ? p : "(null)");
  fflush(stdout);
  if (p && strstr(p, "\"returnValue\":false")) {
    long task = 0;
    semUms = 1;
    printf("[video] uMS refused the source crop; falling back to full screen via ACB\n");
    fflush(stdout);
    windowX = windowY = 0; windowW = 1920; windowH = 1080;
    if (acb) acbWindow(acb, 0, 0, 1920, 1080, 1, &task);
  }
  return 1;
}

// ZOOM DE VERDADE: recorta a FONTE e mantem o destino dentro da tela.
//
// O uMS aceita os dois retangulos na mesma chamada — `source` em coordenadas do
// QUADRO DECODIFICADO e `destination` em coordenadas de tela. Ampliar entao nao
// e inflar o destino (que apaga o plano), e sim pedir um pedaco MENOR da fonte
// para o mesmo destino: e assim que a barra preta embutida no quadro sai da
// area visivel. E a mesma imagem que o web produz com transform: scale(), so
// que calculada do lado certo do escalonador.
//
// Mantem o acbJanela para o caso de tela cheia sem recorte, que ja funcionava.
void video_window_source(int sx, int sy, int sw, int sh,
                        int dx, int dy, int dw, int dh) {
  char b[420];
  int full;
  if (sw < 2 || sh < 2 || dw < 1 || dh < 1) return;
  // O uMS ja recusou uma vez nesta sessao: nao insistir. Cada tentativa nova
  // seria outra chance de deixar o plano num estado sem imagem.
  if (semUms) { video_window(dx, dy, dw, dh); return; }
  // Destino preso a tela: o mesmo limite que vale para o acbJanela.
  if (dx < 0) { dw += dx; dx = 0; }
  if (dy < 0) { dh += dy; dy = 0; }
  if (dx + dw > 1920) dw = 1920 - dx;
  if (dy + dh > 1080) dh = 1080 - dy;
  if (dw < 1 || dh < 1) return;
  full = (dx == 0 && dy == 0 && dw == 1920 && dh == 1080);
  if (sx == fontX && sy == fontY && sw == fontW && sh == fontH &&
      dx == dstX && dy == dstY && dw == dstW && dh == dstH) return;
  fontX = sx; fontY = sy; fontW = sw; fontH = sh;
  dstX = dx; dstY = dy; dstW = dw; dstH = dh;
  windowX = dx; windowY = dy; windowW = dw; windowH = dh;   // o reaplicar do bind usa estes
  if (!on || !media[0]) return;
  // Formato do com.webos.service.tv.display: `sourceInput` e o recorte no
  // quadro decodificado, `displayOutput` o retangulo na tela, `sink` MAIN
  // porque o video vai para o plano principal (o secundario e o PIP).
  snprintf(b, sizeof b,
           "{\"sink\":\"MAIN\",\"fullScreen\":%s,"
           "\"sourceInput\":{\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d},"
           "\"displayOutput\":{\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d}}",
           full ? "true" : "false", sx, sy, sw, sh, dx, dy, dw, dh);
  printf("[video] source %d,%d %dx%d -> destination %d,%d %dx%d\n",
         sx, sy, sw, sh, dx, dy, dw, dh);
  fflush(stdout);
  // O caminho e o ACB, nao o luna direto: o hub recusa o app no tv.display.
  if (acbWindowCustom && acb) {
    long task = 0;
    int r = acbWindowCustom(acb, sx, sy, sw, sh, dx, dy, dw, dh, full, &task);
    printf("[video] acb custom window -> %d\n", r); fflush(stdout);
    if (r) return;
    printf("[video] acb refused the crop; falling back to full screen\n"); fflush(stdout);
  }
  semUms = 1;
  video_window(dx, dy, dw, dh);
  (void)b; (void)aoWindow;
}

double video_pos(void)      { return posSeg; }
double video_duration(void)  { return durationSeg; }
double video_buffer_end(void) { return bufferSeg; }
int    video_playing(void)  { return playing; }
int    video_ready(void)   { return ready; }
// Ha midia carregada. O furo na superficie usa ISTO e nao o loadCompleted:
// abrir o buraco cedo nao custa nada (atras dele so existe o plano de video) e
// esperar o evento deixaria a tela desenhada por cima do video se o evento
// mudar de nome ou nao vier.
int    video_active(void)    { return media[0] != 0; }

int  video_n_audio(void)   { return nAudio; }
int  video_n_subtitle(void) { return nSub; }
const VideoTrack *video_audio(int i)   { return (i >= 0 && i < nAudio) ? &trackAudio[i] : NULL; }
const VideoTrack *video_subtitle(int i) { return (i >= 0 && i < nSub) ? &trackSub[i] : NULL; }
int  video_audio_current(void)   { return audioCurrent; }
int  video_subtitle_current(void) { return subCurrent; }
int  video_tem_atmos(void)        { return vidAtmos; }
// O SELO agora sai do PIPELINE, nao da afirmacao da fonte.
//
// `vidDV` e o que o addon AFIRMOU sobre a URL, e continua sendo o que o bind
// descreve ao tv.display (montarVideoData le vidDV, nao esta funcao) — la a
// afirmacao e a unica informacao disponivel antes de haver imagem, e sem ela
// nao ha como pedir Dolby Vision. Mas para o SELO ela e a fonte errada: esta
// MEDIDO nesta TV que um MKV anunciado como DV volta com hdrType "HDR10" no
// videoInfo. Ligar o selo na afirmacao fazia a tela anunciar Dolby Vision em
// cima de um fluxo HDR10 — e selo que mente e pior que selo ausente, porque e
// nele que o dono confia para saber se pegou a versao boa.
//
// Antes do videoInfo chegar, vidHdr e "none" e a resposta e 0: nenhum selo por
// alguns segundos e honesto; um selo que aparece e depois se desmente, nao.
int  video_tem_dolby_vision(void) {
  return !strcasecmp(vidHdr, "DolbyVision") || !strcasecmp(vidHdr, "dolby_vision");
}
// hdrType cru do pipeline, para a tela poder dizer "HDR10" quando for HDR10 em
// vez de calar. "none" quando o fluxo e SDR ou ainda nao se sabe.
const char *video_hdr(void)       { return vidHdr; }
int  video_width(void)          { return vidW; }
int  video_height(void)           { return vidH; }

void video_set_dv(int dv) { dvRequest = dv ? 1 : 0; }

void video_choose_audio(int i) {
  char b[192];
  const VideoTrack *f = video_audio(i);
  if (!on || !media[0] || !f) return;
  snprintf(b, sizeof b, "{\"mediaId\":\"%s\",\"type\":\"audio\",\"index\":%d}",
           media, f->number);
  call("selectTrack", b, soLog);
  audioCurrent = i;
}

void video_choose_subtitle(int i) {
  char b[192];
  if (!on || !media[0]) return;
  if (i < 0) {
    snprintf(b, sizeof b, "{\"mediaId\":\"%s\",\"enable\":false}", media);
    call("setSubtitleEnable", b, soLog);
    subCurrent = -1;
    return;
  }
  { const VideoTrack *f = video_subtitle(i);
    if (!f) return;
    snprintf(b, sizeof b, "{\"mediaId\":\"%s\",\"enable\":true}", media);
    call("setSubtitleEnable", b, soLog);
    snprintf(b, sizeof b, "{\"mediaId\":\"%s\",\"type\":\"text\",\"index\":%d}",
             media, f->number);
    call("selectTrack", b, soLog);
    subCurrent = i;
    subUrlCurrent[0] = 0;   // voltou para uma faixa do arquivo
    applyStyle(); }
}

// O estilo escolhido, guardado porque o PIPELINE NASCE A CADA LOAD e nao herda
// nada do video anterior. Reaplicado em loadCompleted e sempre que a legenda e
// (re)selecionada.
static VideoSubtitleStyle style = { 120, 0, 0, 3, 1, 0, 0, 0 };
static int temStyle;

static void applyStyle(void) {
  char b[256];
  if (!on || !media[0] || !temStyle) return;
  /* Embutida ainda pertence ao uMS: reduz o percentual aos cinco degraus. */
  { int p=style.size, t=p<=70?0:p<=100?1:p<=130?2:p<=165?3:4;
    snprintf(b, sizeof b, "{\"mediaId\":\"%s\",\"fontSize\":%d}", media, t);
    call("setSubtitleFontSize", b, soLog); }
  { int c = style.color;
    if (c < 0 || c >= VIDEO_SUB_NCORES) c = 0;
    snprintf(b, sizeof b, "{\"mediaId\":\"%s\",\"charColor\":\"%s\"}",
             media, VIDEO_SUB_COLORS[c]);
    call("setSubtitleCharacterColor", b, soLog); }
  // Opacidade DA LETRA, separada da do fundo. O handler existe no firmware da
  // C9 (`setSubtitleCharacterOpacity`) e recebe 0..255. Tres niveis evitam uma
  // folha interminavel no controle remoto e mantem o texto legivel sobre video.
  { int op = style.opacity == 3 ? 64 : style.opacity == 2 ? 128
           : style.opacity == 1 ? 191 : 255;
    snprintf(b, sizeof b, "{\"mediaId\":\"%s\",\"charOpacity\":%d}", media, op);
    call("setSubtitleCharacterOpacity", b, soLog); }
  // O fundo e a dupla cor+opacidade: sem declarar a cor, mudar so a opacidade
  // nao tem o que revelar.
  { int f = style.background; if (f < 0) f = 0; if (f > 4) f = 4;
    int op = f == 4 ? 255 : f * 64;
    snprintf(b, sizeof b, "{\"mediaId\":\"%s\",\"bgColor\":\"black\"}", media);
    call("setSubtitleBackgroundColor", b, soLog);
    snprintf(b, sizeof b, "{\"mediaId\":\"%s\",\"bgOpacity\":%d}", media, op);
    call("setSubtitleBackgroundOpacity", b, soLog); }
  // A folha oferece 0..7; o uMS quer -3..4.
  { int p = style.position; if (p < 0) p = 0; if (p > 7) p = 7;
    snprintf(b, sizeof b, "{\"mediaId\":\"%s\",\"position\":%d}", media, p - 3);
    call("setSubtitlePosition", b, soLog); }
  // VERIFICADO NA TELA: "uniform" desenha contorno em volta das letras.
  // "none" e o sem-borda. O retorno do uMS nao serve de prova aqui — ele
  // respondeu returnValue:true ate para valores inventados.
  { const char *ed = style.border == 2 ? "dropShadow"
                   : (style.border == 1 ? "uniform" : "none");
    snprintf(b, sizeof b, "{\"mediaId\":\"%s\",\"charEdgeType\":\"%s\"}",
             media, ed);
    call("setSubtitleCharacterEdge", b, soLog); }
  if (style.delayMs) {
    snprintf(b, sizeof b, "{\"mediaId\":\"%s\",\"sync\":%d}", media, style.delayMs);
    call("setSubtitleSync", b, soLog);
  }
}

void video_subtitle_style(const VideoSubtitleStyle *e) {
  if (!e) return;
  style = *e;
  temStyle = 1;
  applyStyle();
}

void video_set_mp4(int ehMp4) { sourceMp4 = ehMp4; }

void video_subtitle_external(const char *url) {
  char b[1400], recognizable[1024];
  if (!on || !media[0] || !url || !*url) return;
  // O uMS baixa e sincroniza sozinho — o app so aponta. E o que permite usar
  // legenda do OpenSubtitles em arquivo que nao traz nenhuma embutida. Nesta
  // LG, URI /file/123 produziu errorCode 210 "Unknown Subtitle"; o MESMO
  // arquivo servido como /file/123.srt e reconhecido pelo formato.
  video_normalize_url_subtitle(url, recognizable, sizeof recognizable);
  snprintf(b, sizeof b,
           "{\"mediaId\":\"%s\",\"uri\":\"%s\",\"preferredEncodings\":[\"UTF-8\"]}",
           media, recognizable);
  call("setSubtitleSource", b, soLog);
  snprintf(subUrlCurrent, sizeof subUrlCurrent, "%s", recognizable);
  snprintf(b, sizeof b, "{\"mediaId\":\"%s\",\"enable\":true}", media);
  call("setSubtitleEnable", b, soLog);
  applyStyle();
  printf("[video] external subtitle: %.80s\n", recognizable);
  fflush(stdout);
}

void video_shutdown(void) {
  if (!on) return;
  video_stop();
  if (acb) { acbDestroy(acb); acb = 0; }
  if (loop) loopStop(loop);
  on = 0;
}
#endif
