#include "video.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>

#ifdef __APPLE__
// No Mac nao existe barramento nem plano de video. Os cotos deixam o resto do
// app compilar e rodar igual, so sem imagem em movimento.
int  video_iniciar(void) { return 0; }
int  video_tocar(const char *u) { (void)u; return 0; }
void video_bombear(void) {}
void video_parar(void) {}
void video_pausar(int p) { (void)p; }
void video_buscar(double s) { (void)s; }
void video_janela(int x,int y,int w,int h) { (void)x;(void)y;(void)w;(void)h; }
double video_pos(void) { return 0; }
double video_duracao(void) { return 0; }
double video_buffer_fim(void) { return 0; }
void video_definir_dv(int dv) { (void)dv; }
int  video_tocando(void) { return 0; }
int  video_pronto(void) { return 0; }
int  video_ativo(void) { return 0; }
int  video_n_audio(void) { return 0; }
int  video_n_legenda(void) { return 0; }
const VideoFaixa *video_audio(int i) { (void)i; return 0; }
const VideoFaixa *video_legenda(int i) { (void)i; return 0; }
int  video_audio_atual(void) { return 0; }
int  video_legenda_atual(void) { return -1; }
void video_escolher_audio(int i) { (void)i; }
void video_escolher_legenda(int i) { (void)i; }
void video_legenda_externa(const char *u) { (void)u; }
int  video_tem_atmos(void) { return 0; }
int  video_tem_dolby_vision(void) { return 0; }
const char *video_hdr(void) { return "none"; }
int  video_largura(void) { return 0; }
int  video_altura(void) { return 0; }
void video_encerrar(void) {}
#else
#include <dlfcn.h>

typedef struct LSHandle LSHandle;
typedef struct LSMessage LSMessage;
typedef int (*Filtro)(LSHandle *, LSMessage *, void *);

// LSError e struct por valor e nao ha header C no SDK. Um buffer folgado evita
// corromper a pilha quando a lib escreve o erro dentro dele.
static char ERRO[256];

static int         (*lsRegister)(const char *, LSHandle **, void *);
static int         (*lsAttach)(LSHandle *, void *, void *);
static int         (*lsCall)(LSHandle *, const char *, const char *, Filtro, void *, unsigned long *, void *);
static const char *(*lsPayload)(LSMessage *);
static void *(*loopNovo)(void *, int);
static void  (*loopRodar)(void *);
static void  (*loopParar)(void *);

// libAcbAPI e SEMPRE por dlopen. Linkar cria um DT_NEEDED e, se a lib faltar
// (ela sumiu no webOS 5), o processo morre antes do main e antes do log —
// nao sobra nem uma linha para diagnosticar.
static long (*acbCriar)(void);
static int  (*acbIniciar)(long, int, const char *, void *);
static int  (*acbSink)(long, int);
static int  (*acbMidia)(long, const char *);
static int  (*acbEstado)(long, int, int, long *);
static int  (*acbJanela)(long, long, long, long, long, int, long *);
static void (*acbDestruir)(long);
// O navegador da TV chama isto e nos nao chamavamos: sem o connect o plano de
// video existe, decodifica e toca o audio, mas nao e ligado a saida — tela
// preta com som, exatamente o sintoma observado.
static int  (*acbConectar)(long, int, long *);
// Recebe JSON como string (confirmado: a lib chama strlen no argumento antes de
// montar um std::string). Sem esta chamada o servico do ACB nunca repassa nada
// para com.webos.service.tv.display e o plano de video nao liga — o sintoma e
// audio normal com tela preta.
static int  (*acbVideoData)(long, const char *, long *);
static int  (*acbAudioData)(long, const char *, long *);

static LSHandle *bus;
static void     *laco;
static pthread_t fio;
static long      acb;
// Retangulo pedido pela UI. Guardado porque o ACB so aceita a janela depois do
// loadCompleted, que chega muito depois de quem pediu.
static int       janX, janY, janW = 1920, janH = 1080;
// Caracteristicas do fluxo, tiradas do evento videoInfo da assinatura do uMS.
// O ACB precisa delas para descrever o video ao pipeline de exibicao.
static int       vidW = 1920, vidH = 1080, vidTaxa = 30;
static long      vidBits;
static char      vidVarredura[24] = "progressive";
// hdrType real informado pelo uMS para a camada que chegou ao decoder. Isto
// vence o rotulo do addon: um arquivo marcado HDR-DV pode entregar apenas a
// camada HDR10 nesta TV/perfil.
static char      vidHdr[24] = "none";
static long      seiX0, seiX1, seiX2, seiY0, seiY1, seiY2;
static long      seiBrancoX, seiBrancoY, seiMinLum, seiMaxLum;
static long      seiMaxCLL, seiMaxFALL;
static int       vuiPrim = 2, vuiTrans = 2, vuiMatriz = 2;
static int       vidAtmos, vidDV;
// Estado do recuo de Dolby Vision (ver o bloco em video_tocar). Declarados
// AQUI e nao junto da funcao porque o parser do videoInfo, bem acima, marca
// viuVideo — e no C a ordem de declaracao manda.
static int       dvNaCarga, dvRecuado, viuVideo;

// Faixas lidas do sourceInfo. Guardadas porque a tela precisa delas a cada
// quadro e reprocessar o JSON no desenho seria desperdicio.
static VideoFaixa faixaAudio[NV_FAIXA_MAX], faixaLeg[NV_FAIXA_MAX];
static int nAudio, nLeg, audioAtual, legAtual = -1;

// Afirmacao de DV da fonte escolhida. Setada por video_definir_dv ANTES do
// tocar, porque o video_tocar zera vidDV ao comecar uma sessao nova.
static int dvPedido;

// Nome legivel do idioma. So os que aparecem de verdade neste acervo; o resto
// fica com o codigo, que e melhor que "Desconhecido" — o codigo ao menos
// identifica.
static const char *idiomaLegivel(const char *c) {
  static const struct { const char *cod, *nome; } T[] = {
    { "pt", "Portugues" }, { "pob", "Portugues (BR)" }, { "por", "Portugues" },
    { "en", "Ingles" },    { "eng", "Ingles" },
    { "es", "Espanhol" },  { "spa", "Espanhol" },
    { "fr", "Frances" },   { "fre", "Frances" }, { "fra", "Frances" },
    { "de", "Alemao" },    { "ger", "Alemao" },
    { "it", "Italiano" },  { "ita", "Italiano" },
    { "ja", "Japones" },   { "jpn", "Japones" },
  };
  size_t i;
  if (!c || !*c) return "";
  for (i = 0; i < sizeof T / sizeof *T; i++)
    if (!strcasecmp(c, T[i].cod)) return T[i].nome;
  return c;
}
static char      midia[64];
static double    posSeg, durSeg;
static int       tocando, pronto, ligado;

// PLAYER_TYPE_MSE. O ACB usa isto para saber que a fonte e um pipeline de
// midia e nao um sintonizador.
// Os enums do ACB nao tem header publico e chutar sai caro: com playerType 10 e
// sink 1 o aparelho registrou "playerType":"mse","vsmSinkType":"sub" — ou seja,
// o video foi para o plano SECUNDARIO (PIP) e a tela ficou preta. Os numeros
// ficam ajustaveis por /tmp/nuvio-acb justamente para conferir contra o que o
// ls-monitor mostra que o ACB resolveu, em vez de adivinhar de novo.
static int tipoJogador = 0, tipoSink = 0, estCarregado = 1, estTocando = 2;
// hdrType do setMediaVideoData. O padrao e "none"; para testar Dolby Vision,
// escreva na SEGUNDA linha de /tmp/nuvio-acb: "dolby_vision" ou "hdr10".
// Afirmar DV sem o pipeline pedir e mentira — por isso NAO existe deteccao
// automatica: o sourceInfo do uMS nao distingue HEVC main-10 HDR10 de DV.
static char hdrTipo[24] = "none";

static void lerAjustesAcb(void) {
  FILE *f = fopen("/tmp/nuvio-acb", "r");
  if (!f) return;
  if (fscanf(f, "%d %d %d %d", &tipoJogador, &tipoSink, &estCarregado, &estTocando) > 0)
    printf("[video] acb ajustes: jogador=%d sink=%d carregado=%d tocando=%d\n",
           tipoJogador, tipoSink, estCarregado, estTocando);
  { // resto da primeira linha descartado; a SEGUNDA linha, se existir, e o hdrType.
    char linha[64];
    if (fgets(linha, sizeof linha, f) && fgets(linha, sizeof linha, f)) {
      char t[24] = "";
      if (sscanf(linha, "%23s", t) == 1 && t[0]) {
        snprintf(hdrTipo, sizeof hdrTipo, "%s", t);
        if (strstr(hdrTipo, "dolby")) vidDV = 1;
        printf("[video] acb ajustes: hdrType=%s\n", hdrTipo);
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
static double numeroDe(const char *p, const char *chave) {
  const char *q = p;
  size_t n = strlen(chave);
  while ((q = strstr(q, chave)) != NULL) {
    const char *v = q + n;
    while (*v == ' ') v++;
    if ((*v >= '0' && *v <= '9') || *v == '-' || *v == '.') return atof(v);
    q += n;
  }
  return -1.0;
}

static pthread_t fioBind;
static volatile int bindVivo = 0;   // existe um bind em andamento?

// Latencia do pipeline: pedido de load -> loadCompleted -> primeiro quadro.
// Sao os numeros que dizem se o comeco e o buffer estao saudaveis; sem eles
// "ta lento" e impressao.
static struct timespec t0Pedido;
static int cronPediu, cronLoad, cronQuadro;
static long msDesdePedido(void) {
  struct timespec a;
  clock_gettime(CLOCK_MONOTONIC, &a);
  return (a.tv_sec - t0Pedido.tv_sec) * 1000L + (a.tv_nsec - t0Pedido.tv_nsec) / 1000000L;
}
// Ate onde o buffer do pipeline ja cobre (segundos), do evento bufferRange.
static double bufferSeg;

static void esperar(int ms) { struct timespec t; t.tv_sec = ms / 1000;
  t.tv_nsec = (long)(ms % 1000) * 1000000L; nanosleep(&t, NULL); }

// O JSON de video do ACB, montado por partes porque os valores de HDR mudam
// com o que se esta afirmando. VUI segue H.273/HEVC: 9=BT.2020, 16=PQ
// (SMPTE 2084) — e o par que HDR10 e DV pedem; SDR fica em 2 (unspecified),
// que e o que sempre foi mandado e toca.
static void montarVideoData(char *vd, size_t n, const char *ctx,
                            const char *htipo, int prim, int trans, int matriz) {
  const char *varr = strstr(vidVarredura, "inter") ? "VIDEO_INTERLACED"
                   : "VIDEO_PROGRESSIVE";
  char cor[768] = "";
  if (!strcmp(htipo, "HDR10")) {
    snprintf(cor, sizeof cor,
      "\"mediaSei\":{\"displayPrimariesX0\":%ld,\"displayPrimariesX1\":%ld,"
      "\"displayPrimariesX2\":%ld,\"displayPrimariesY0\":%ld,"
      "\"displayPrimariesY1\":%ld,\"displayPrimariesY2\":%ld,"
      "\"maxContentLightLevel\":%ld,\"maxDisplayMasteringLuminance\":%ld,"
      "\"maxPicAverageLightLevel\":%ld,\"minDisplayMasteringLuminance\":%ld,"
      "\"whitePointX\":%ld,\"whitePointY\":%ld},"
      "\"mediaVui\":{\"colorPrimaries\":%d,\"matrixCoeffs\":%d,"
      "\"transferCharacteristics\":%d,\"videoFullRangeFlag\":false},",
      seiX0, seiX1, seiX2, seiY0, seiY1, seiY2, seiMaxCLL, seiMaxLum,
      seiMaxFALL, seiMinLum, seiBrancoX, seiBrancoY,
      prim, matriz, trans);
  } else if (!strcmp(htipo, "none")) {
    snprintf(cor, sizeof cor,
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
    "}}", ctx, vidBits, vidTaxa, htipo, vidH, vidW, cor, varr);
}

static void *prenderPlano(void *u) {
  (void)u;
  // O bind e POR SESSAO: cada loadCompleted tem de religar o plano. Foi o bug
  // da "segunda reproducao preta com som" — o ACB continuava apontando para o
  // mediaId da sessao anterior, que o unload matou. A guarda de midia cobre a
  // troca de titulo no MEIO do bind (unload+load em menos de ~1,5s de pausas):
  // continuar descreveria ao tv.display um mediaId que ja morreu.
  char minha[64];
  snprintf(minha, sizeof minha, "%s", midia);
  long tarefa = 0;
  acbMidia(acb, minha);            esperar(200);
  if (strcmp(midia, minha)) goto fora;
  acbEstado(acb, NV_ACB_FOREGROUND, estCarregado, &tarefa); esperar(200);
  if (strcmp(midia, minha)) goto fora;
  printf("[video] connect=%d\n", acbConectar(acb, tipoSink, &tarefa)); esperar(200);
  if (strcmp(midia, minha)) goto fora;
  {
    // Strings e formato copiados de controles positivos na MESMA TV:
    // Apple TV e Nuvio web usam "DolbyVision" sem SEI/VUI; HDR10 usa o SEI/VUI
    // real do videoInfo. A grafia/capitalizacao e semanticamente relevante.
    // Enquanto isso nao existia, "none" ia sempre: o C9 exibia o video
    // mapeado em SDR e o modo HDR/DV da TV nunca ligava — exatamente o
    // sintoma do teste 4K.
    char htipo[24];
    int prim = 2, trans = 2, matriz = 2;
    if (strcmp(hdrTipo, "none")) { snprintf(htipo, sizeof htipo, "%s", hdrTipo);
                                  prim = vuiPrim; trans = vuiTrans; matriz = vuiMatriz; }
    else if (!strcasecmp(vidHdr, "DolbyVision") || vidDV) {
                                  snprintf(htipo, sizeof htipo, "DolbyVision"); }
    else if (!strcasecmp(vidHdr, "HDR10")) {
                                  snprintf(htipo, sizeof htipo, "HDR10");
                                  prim = vuiPrim; trans = vuiTrans; matriz = vuiMatriz; }
    else                           snprintf(htipo, sizeof htipo, "none");
    char vd[2048];
    montarVideoData(vd, sizeof vd, minha, htipo, prim, trans, matriz);
    { char ad[160];
      snprintf(ad, sizeof ad,
               "{\"context\":\"%s\",\"audio\":{\"immersive\":\"none\"}}", minha);
      int rvd = acbVideoData(acb, vd, &tarefa);
      printf("[video] videoData=%d (hdrType=%s)\n", rvd, htipo);
      if (!strcmp(htipo, "DolbyVision") && !strcasecmp(vidHdr, "HDR10")) {
        // O ACB aceita DolbyVision e a TV acende o badge mesmo quando o
        // demuxer do MKV so entregou a camada HDR10 — nesse caso o plano fica
        // sem imagem. O retorno sincrono nao detecta isso. Depois de negociar
        // DV, voltar para o formato REAL do decoder recupera imagem + HDR10.
        esperar(700);
        montarVideoData(vd, sizeof vd, minha, "HDR10", vuiPrim, vuiTrans, vuiMatriz);
        printf("[video] MKV DV entregue como HDR10; fallback real: %d\n",
               acbVideoData(acb, vd, &tarefa));
      } else if (!strcmp(htipo, "DolbyVision") && !strcasecmp(vidHdr, "none")) {
        // Profile DV que o demuxer desta TV nao reconheceu nem como camada
        // HDR10. O badge liga, mas nao ha quadro DV; voltar a SDR garante
        // imagem. MP4 reconhecido vem como DolbyVision e nao entra aqui.
        esperar(700);
        montarVideoData(vd, sizeof vd, minha, "none", 2, 2, 2);
        printf("[video] DV nao reconhecido pelo decoder; fallback SDR: %d\n",
               acbVideoData(acb, vd, &tarefa));
      } else if (rvd != 1 && strcmp(htipo, "none")) {
        montarVideoData(vd, sizeof vd, minha, "none", 2, 2, 2);
        printf("[video] videoData recusou hdrType=%s, repetindo sem HDR: %d\n",
               htipo, acbVideoData(acb, vd, &tarefa));
      }
      printf("[video] audioData=%d\n", acbAudioData(acb, ad, &tarefa));
      fflush(stdout);
    }
    esperar(300);
    if (strcmp(midia, minha)) goto fora;
  }
  acbJanela(acb, janX, janY, janW, janH,
          (janX == 0 && janY == 0 && janW == 1920 && janH == 1080), &tarefa);
  acbEstado(acb, NV_ACB_FOREGROUND, estTocando, &tarefa);
  printf("[video] plano preso em %d,%d %dx%d\n", janX, janY, janW, janH);
  fflush(stdout);
fora:
  if (strcmp(midia, minha)) printf("[video] bind abortado: a midia trocou no meio\n");
  bindVivo = 0;
  return NULL;
}

static int aoEvento(LSHandle *h, LSMessage *m, void *u) {
  const char *p = lsPayload(m);
  (void)h; (void)u;
  if (!p) return 1;
  printf("[video] ev %s\n", p); fflush(stdout);
  if (strstr(p, "sourceInfo")) {
    const char *q;
    nAudio = nLeg = 0;
    vidAtmos = 0;
    // Percorre audioTrackInfo item a item. O sourceInfo e um objeto so, entao
    // andar pelos "{" depois da chave do vetor e o suficiente aqui.
    q = strstr(p, "\"audioTrackInfo\"");
    if (q) {
      const char *fimVet = strchr(q, ']');
      const char *o = strchr(q, '{');
      while (o && nAudio < NV_FAIXA_MAX && (!fimVet || o < fimVet)) {
        const char *fo = strchr(o, '}');
        VideoFaixa *f = &faixaAudio[nAudio];
        char cod[16] = "", ch[8] = "", imm[16] = "";
        memset(f, 0, sizeof *f);
        f->numero = nAudio;
        { const char *l = strstr(o, "\"language\":\"");
          if (l && (!fo || l < fo)) {
            size_t k = 0; l += 12;
            while (*l && *l != '"' && k + 1 < sizeof f->idioma) f->idioma[k++] = *l++;
            f->idioma[k] = 0;
            if (!strcmp(f->idioma, "(null)")) f->idioma[0] = 0;
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
        { double c3 = numeroDe(o, "\"channels\":");
          if (c3 == 6) snprintf(ch, sizeof ch, "5.1");
          else if (c3 == 8) snprintf(ch, sizeof ch, "7.1");
          else if (c3 == 2) snprintf(ch, sizeof ch, "2.0"); }
        snprintf(f->rotulo, sizeof f->rotulo, "%s%s%s%s%s",
                 f->idioma[0] ? idiomaLegivel(f->idioma) : "Faixa",
                 imm[0] ? "  \xc2\xb7  " : (ch[0] ? "  \xc2\xb7  " : ""),
                 imm[0] ? "Atmos" : "",
                 (imm[0] && ch[0]) ? " " : "", ch);
        nAudio++;
        o = fo ? strchr(fo, '{') : NULL;
      }
    }
    q = strstr(p, "\"subtitleTrackInfo\"");
    if (q) {
      const char *fimVet = strchr(q, ']');
      const char *o = strchr(q, '{');
      while (o && nLeg < NV_FAIXA_MAX && (!fimVet || o < fimVet)) {
        const char *fo = strchr(o, '}');
        VideoFaixa *f = &faixaLeg[nLeg];
        memset(f, 0, sizeof *f);
        f->numero = (int)numeroDe(o, "\"trackNum\":");
        { const char *l = strstr(o, "\"language\":\"");
          if (l && (!fo || l < fo)) {
            size_t k = 0; l += 12;
            while (*l && *l != '"' && k + 1 < sizeof f->idioma) f->idioma[k++] = *l++;
            f->idioma[k] = 0;
            if (!strcmp(f->idioma, "(null)")) f->idioma[0] = 0;
          } }
        // Arquivo sem etiqueta de idioma e o caso comum em MKV de release.
        // Numerar e honesto; inventar "Ingles" seria pior.
        if (f->idioma[0])
          snprintf(f->rotulo, sizeof f->rotulo, "%s", idiomaLegivel(f->idioma));
        else
          snprintf(f->rotulo, sizeof f->rotulo, "Legenda %d", f->numero + 1);
        nLeg++;
        o = fo ? strchr(fo, '{') : NULL;
      }
    }
    printf("[video] faixas: audio=%d legenda=%d atmos=%d\n", nAudio, nLeg, vidAtmos);
    fflush(stdout);
  }

  if (strstr(p, "videoInfo")) {
    viuVideo = 1;   // fecha o prazo do recuo de DV
    double v;
    v = numeroDe(p, "\"width\":");      if (v > 0) vidW = (int)v;
    v = numeroDe(p, "\"height\":");     if (v > 0) vidH = (int)v;
    v = numeroDe(p, "\"frameRate\":");  if (v > 0) vidTaxa = (int)v;
    v = numeroDe(p, "\"bitRate\":");    if (v > 0) vidBits = (long)v;
    { const char *q = strstr(p, "\"scanType\":\"");
      if (q) { const char *f; q += 12; f = strchr(q, '"');
        if (f && f - q < (int)sizeof vidVarredura) {
          memcpy(vidVarredura, q, f - q); vidVarredura[f - q] = 0; } } }
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
          printf("[video] HDR do pipeline: %s (fonte afirmava DV=%d)\n",
                 vidHdr, dvPedido); } } }
    // O Nuvio web que toca corretamente repassa estes valores sem alterar.
    // Para DolbyVision ele omite os dois blocos; montarVideoData faz o mesmo.
    { double x;
#define LER_SEI(nome, dst) do { x = numeroDe(p, "\"" nome "\":"); if (x >= 0) dst = (long)x; } while (0)
      LER_SEI("displayPrimariesX0", seiX0); LER_SEI("displayPrimariesX1", seiX1);
      LER_SEI("displayPrimariesX2", seiX2); LER_SEI("displayPrimariesY0", seiY0);
      LER_SEI("displayPrimariesY1", seiY1); LER_SEI("displayPrimariesY2", seiY2);
      LER_SEI("whitePointX", seiBrancoX); LER_SEI("whitePointY", seiBrancoY);
      LER_SEI("minDisplayMasteringLuminance", seiMinLum);
      LER_SEI("maxDisplayMasteringLuminance", seiMaxLum);
      LER_SEI("maxContentLightLevel", seiMaxCLL);
      LER_SEI("maxPicAverageLightLevel", seiMaxFALL);
#undef LER_SEI
      x = numeroDe(p, "\"colorPrimaries\":"); if (x >= 0) vuiPrim = (int)x;
      x = numeroDe(p, "\"transferCharacteristics\":"); if (x >= 0) vuiTrans = (int)x;
      x = numeroDe(p, "\"matrixCoeffs\":"); if (x >= 0) vuiMatriz = (int)x;
    }
  }
  if (strstr(p, "loadCompleted")) {
    pronto = 1;
    if (cronPediu && !cronLoad) {
      cronLoad = 1;
      printf("[video] load->loadCompleted %lums\n", msDesdePedido());
    }
    // O bind do ACB vai para um fio proprio COM PAUSAS entre os passos.
    // Motivo medido: cada chamada do AcbAPI e assincrona (o servico responde
    // pelo barramento) e disparando tudo em sequencia o setMediaVideoData
    // chegava antes do register terminar — o servico respondia
    // "piplineID key Error!!", parava a sequencia e nunca mandava o stopMute.
    // Sem o stopMute o video fica mudo: tela preta com audio normal.
    // E POR SESSAO, nao uma vez por processo: sem isto a segunda reproducao
    // herda um ACB apontando para o mediaId morto da anterior.
    if (acb && midia[0] && !bindVivo) {
      bindVivo = 1;
      if (pthread_create(&fioBind, NULL, prenderPlano, NULL) == 0) pthread_detach(fioBind);
      else bindVivo = 0;
    }
  }
  if (strstr(p, "bufferRange")) {
    double e = numeroDe(p, "\"endTime\":");
    if (e >= 0) bufferSeg = e;
  }
  if (strstr(p, "playing")) {
    tocando = 1;
    if (acb && midia[0]) {
      long tarefa = 0;
      acbJanela(acb, janX, janY, janW, janH,
                (janX == 0 && janY == 0 && janW == 1920 && janH == 1080), &tarefa);
      printf("[video] janela reaplicada com o fluxo ja tocando\n"); fflush(stdout);
    }
  }
  if (strstr(p, "paused"))   tocando = 0;
  if (strstr(p, "endOfStream")) tocando = 0;
  { double v = numeroDe(p, "\"currentTime\":");
    if (v >= 0) {
      posSeg = v / 1000.0;
      // Primeiro quadro com avanco: o numero de inicio de verdade.
      if (cronPediu && !cronQuadro && posSeg > 0.0) {
        cronQuadro = 1;
        printf("[video] load->primeiro quadro %lums\n", msDesdePedido());
        fflush(stdout);
      }
    } }
  { double v = numeroDe(p, "\"duration\":");
    if (v >= 0) durSeg = v / 1000.0; }
  return 1;
}

static int soLog(LSHandle *h, LSMessage *m, void *u) {
  (void)h; (void)u;
  printf("[video] %s\n", lsPayload(m)); fflush(stdout);
  return 1;
}

static void chamar(const char *metodo, const char *carga, Filtro cb) {
  char uri[128]; unsigned long tok = 0;
  snprintf(uri, sizeof uri, "luna://com.webos.media/%s", metodo);
  if (!lsCall(bus, uri, carga, cb, NULL, &tok, ERRO))
    printf("[video] %s falhou\n", metodo);
}

static int aoCarregar(LSHandle *h, LSMessage *m, void *u) {
  const char *p = lsPayload(m), *q;
  char b[256];
  (void)h; (void)u;
  printf("[video] load: %s\n", p ? p : "(nulo)"); fflush(stdout);
  if (!p || midia[0]) return 1;
  q = strstr(p, "\"mediaId\":\"");
  if (!q) return 1;
  q += 11;
  { const char *f = strchr(q, '"');
    if (!f || f - q >= (int)sizeof midia) return 1;
    memcpy(midia, q, f - q); midia[f - q] = 0; }

  snprintf(b, sizeof b, "{\"connectionId\":\"%s\"}", midia);
  chamar("notifyForeground", b, soLog);
  snprintf(b, sizeof b, "{\"mediaId\":\"%s\"}", midia);
  chamar("subscribe", b, aoEvento);
  snprintf(b, sizeof b, "{\"mediaId\":\"%s\",\"type\":\"video\",\"index\":0}", midia);
  chamar("selectTrack", b, soLog);
  snprintf(b, sizeof b, "{\"mediaId\":\"%s\"}", midia);
  chamar("play", b, soLog);
  return 1;
}

static void *rodarLaco(void *u) { (void)u; loopRodar(laco); return NULL; }

// O ACB EXIGE um callback de verdade. Passar NULL nao e ignorado: no primeiro
// evento ele salta para o endereco 0 e o app morre com SIGSEGV em pc=0x0, longe
// do ponto onde o NULL foi escrito.
static void acbNotificou(long h, long tarefa, long evento,
                         long estApp, long estToca, int resposta) {
  (void)h; (void)tarefa;
  printf("[video] acb evento=%ld app=%ld toca=%ld resp=%d\n",
         evento, estApp, estToca, resposta);
  fflush(stdout);
}

#define SIM(h, v, n) do { \
    *(void **)(&v) = dlsym(h, n); \
    if (!v) { printf("[video] falta %s\n", n); return 0; } \
  } while (0)

int video_iniciar(void) {
  void *L, *G, *A;
  if (ligado) return 1;
  L = dlopen("libluna-service2.so.3", RTLD_NOW);
  if (!L) L = dlopen("libluna-service2.so", RTLD_NOW);
  G = dlopen("libglib-2.0.so.0", RTLD_NOW);
  A = dlopen("libAcbAPI.so.1", RTLD_NOW);
  if (!L || !G || !A) { printf("[video] libs: %s\n", dlerror()); return 0; }

  SIM(L, lsRegister, "LSRegister");
  SIM(L, lsAttach,   "LSGmainAttach");
  SIM(L, lsCall,     "LSCall");
  SIM(L, lsPayload,  "LSMessageGetPayload");
  SIM(G, loopNovo,   "g_main_loop_new");
  SIM(G, loopRodar,  "g_main_loop_run");
  SIM(G, loopParar,  "g_main_loop_quit");
  SIM(A, acbCriar,    "AcbAPI_create");
  SIM(A, acbIniciar,  "AcbAPI_initialize");
  SIM(A, acbSink,     "AcbAPI_setSinkType");
  SIM(A, acbMidia,    "AcbAPI_setMediaId");
  SIM(A, acbEstado,   "AcbAPI_setState");
  SIM(A, acbJanela,   "AcbAPI_setDisplayWindow");
  SIM(A, acbDestruir, "AcbAPI_destroy");
  SIM(A, acbConectar, "AcbAPI_connectDass");
  SIM(A, acbVideoData, "AcbAPI_setMediaVideoData");
  SIM(A, acbAudioData, "AcbAPI_setMediaAudioData");

  // O nome PRECISA casar com o padrao do papel LS2 do app
  // (allowedNames: "com.webos.media.client.*"). Qualquer outro nome e recusado
  // pelo hub e nada depois disso acontece.
  if (!lsRegister("com.webos.media.client.nuvio", &bus, ERRO)) {
    printf("[video] LSRegister recusado\n"); return 0;
  }
  laco = loopNovo(NULL, 0);
  if (!lsAttach(bus, laco, ERRO)) { printf("[video] attach falhou\n"); return 0; }
  // Laco proprio: o LS2 exige um GMainLoop girando, e girar isso no laco de
  // desenho custaria quadros. As respostas chegam neste fio e so mexem em
  // variaveis simples, lidas pelo desenho sem trava.
  pthread_create(&fio, NULL, rodarLaco, NULL);

  lerAjustesAcb();
  acb = acbCriar();
    acbIniciar(acb, tipoJogador, "space.nuvio.native.legacy", (void *)acbNotificou);
  acbSink(acb, tipoSink);
  ligado = 1;
  printf("[video] pronto (acb=%ld)\n", acb); fflush(stdout);
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

static char  urlAtual[1024];
static long  msDoLoad = 0;

static long agoraMs(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int tocarInterno(const char *url, int comDV);

int video_tocar(const char *url) {
  dvRecuado = 0;
  snprintf(urlAtual, sizeof urlAtual, "%s", url ? url : "");
  return tocarInterno(url, 1);
}

// Chamado uma vez por quadro. So existe para o prazo acima: sem ele o recuo
// dependeria de o usuario perceber que nao ha imagem e sair da tela.
void video_bombear(void) {
  // O recuo por prazo foi REMOVIDO por nao funcionar: o gatilho era "o pipeline
  // nao reportou videoInfo", e o uMS reporta videoInfo, sourceInfo e
  // loadCompleted normalmente mesmo nos arquivos que ficam sem imagem. Medido:
  // videoInfo 3840x1606 hdrType=DolbyVision e loadCompleted em 3212ms, tela
  // preta com audio correndo. Nao ha no uMS sinal de QUADRO EXIBIDO — o
  // currentTime avanca puxado pelo audio.
  //
  // A funcao fica porque a batida por quadro e util assim que existir um sinal
  // melhor (contador de quadros, ou o proprio perfil lido do MKV).
  (void)dvNaCarga; (void)dvRecuado; (void)viuVideo;
  (void)msDoLoad; (void)urlAtual; (void)agoraMs; (void)tocarInterno;
}

static int tocarInterno(const char *url, int comDV) {
  char carga[2048];
  if (!ligado && !video_iniciar()) return 0;
  video_parar();
  viuVideo = 0;
  posSeg = durSeg = bufferSeg = 0; tocando = pronto = 0; midia[0] = 0;
  nAudio = nLeg = 0; audioAtual = 0; legAtual = -1; vidAtmos = 0;
  snprintf(vidHdr, sizeof vidHdr, "none");
  seiX0 = seiX1 = seiX2 = seiY0 = seiY1 = seiY2 = 0;
  seiBrancoX = seiBrancoY = seiMinLum = seiMaxLum = seiMaxCLL = seiMaxFALL = 0;
  vuiPrim = vuiTrans = vuiMatriz = 2;
  // A afirmacao de DV da fonte escolhida sobrevive ao reset: e ela que o bind
  // descreve ao tv.display. Sem ela, toda sessao nasceria "none".
  vidDV = dvPedido;
  cronPediu = 1; cronLoad = 0; cronQuadro = 0;
  clock_gettime(CLOCK_MONOTONIC, &t0Pedido);
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
  dvNaCarga = 0;
  if (dvPedido && comDV) {
    // Os valores tambem saem de /tmp/nuvio-dv.conf ("<perfil> <trilha>", ex:
    // "7 dual"), porque o app e lancado pelo SAM e nao da para passar variavel
    // de ambiente por ali. Sem o arquivo, valem o ambiente e depois o padrao.
    static char pArq[16], tArq[16];
    const char *perfil = getenv("NUVIO_DV_PROFILE");
    const char *trilha = getenv("NUVIO_DV_TRACK");
    { FILE *f = fopen("/tmp/nuvio-dv.conf", "r");
      if (f) {
        pArq[0] = tArq[0] = 0;
        if (fscanf(f, "%15s %15s", pArq, tArq) >= 1) {
          if (pArq[0]) perfil = pArq;
          if (tArq[0]) trilha = tArq;
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
    if (!perfil || !*perfil || !strcmp(perfil, "off")) {
      /* sem declaracao: comportamento conhecido e seguro */
    } else {
      snprintf(dolby, sizeof dolby,
               "\"externalStreamingInfo\":{\"contents\":{\"DolbyHdrInfo\":{"
               "\"encryptionType\":\"clear\",\"profileId\":%s,\"trackType\":\"%s\"}}},",
               (perfil && *perfil) ? perfil : "8",
               (trilha && *trilha) ? trilha : "single");
      printf("[video] DolbyHdrInfo declarado: %s\n", dolby); fflush(stdout);
      dvNaCarga = 1;
    }
  }
  snprintf(carga, sizeof carga,
      "{\"payload\":{\"option\":{\"useSeekableRanges\":true,"
      "\"appId\":\"space.nuvio.native.legacy\","
      "%s"
      "\"bufferControl\":{\"userBufferCtrl\":false},"
      "\"windowId\":\"window_id_dummy\"}},"
      "\"uri\":\"%s\",\"type\":\"media\"}", dolby, url);
  printf("[video] URL: %s\n", url); fflush(stdout);
  msDoLoad = agoraMs();
  chamar("load", carga, aoCarregar);
  return 1;
}

void video_parar(void) {
  char b[128];
  if (!ligado || !midia[0]) return;
  snprintf(b, sizeof b, "{\"mediaId\":\"%s\"}", midia);
  chamar("unload", b, soLog);
  midia[0] = 0; tocando = pronto = 0;
}

void video_pausar(int pausado) {
  char b[128];
  if (!ligado || !midia[0]) return;
  snprintf(b, sizeof b, "{\"mediaId\":\"%s\"}", midia);
  chamar(pausado ? "pause" : "play", b, soLog);
  tocando = !pausado;
}

void video_buscar(double segundos) {
  char b[192];
  if (!ligado || !midia[0]) return;
  if (segundos < 0) segundos = 0;
  snprintf(b, sizeof b, "{\"mediaId\":\"%s\",\"position\":%d}",
           midia, (int)(segundos * 1000.0));
  chamar("seek", b, soLog);
  posSeg = segundos;
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
void video_janela(int x, int y, int w, int h) {
  long tarefa = 0;
  int cheia = (x == 0 && y == 0 && w == 1920 && h == 1080);
  if (w < 1 || h < 1) return;
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > 1920) w = 1920 - x;
  if (y + h > 1080) h = 1080 - y;
  if (w < 1 || h < 1) return;
  if (x == janX && y == janY && w == janW && h == janH) return;  // sem repetir o mesmo rect a cada quadro
  janX = x; janY = y; janW = w; janH = h;
  if (!ligado || !acb || !midia[0]) return;   // sem midia presa, aplicar seria no vazio
  printf("[video] janela %d,%d %dx%d cheia=%d\n", x, y, w, h, cheia);
  fflush(stdout);
  acbJanela(acb, x, y, w, h, cheia, &tarefa);
}

// A resposta do uMS ao setDisplayWindow, LOGADA. Sem ler a resposta, um payload
// com a assinatura errada falha calado e o sintoma que sobra e "tela preta" —
// exatamente o que custou a descobrir que o caminho do ACB nao recortava.
static int aoJanela(LSHandle *h, LSMessage *m, void *u) {
  const char *p = lsPayload(m);
  (void)h; (void)u;
  printf("[video] setDisplayWindow -> %s\n", p ? p : "(nulo)");
  fflush(stdout);
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
static int fonX = -1, fonY, fonW, fonH, dstX, dstY, dstW, dstH;

void video_janela_fonte(int sx, int sy, int sw, int sh,
                        int dx, int dy, int dw, int dh) {
  char b[420];
  int cheia;
  if (sw < 2 || sh < 2 || dw < 1 || dh < 1) return;
  // Destino preso a tela: o mesmo limite que vale para o acbJanela.
  if (dx < 0) { dw += dx; dx = 0; }
  if (dy < 0) { dh += dy; dy = 0; }
  if (dx + dw > 1920) dw = 1920 - dx;
  if (dy + dh > 1080) dh = 1080 - dy;
  if (dw < 1 || dh < 1) return;
  cheia = (dx == 0 && dy == 0 && dw == 1920 && dh == 1080);
  if (sx == fonX && sy == fonY && sw == fonW && sh == fonH &&
      dx == dstX && dy == dstY && dw == dstW && dh == dstH) return;
  fonX = sx; fonY = sy; fonW = sw; fonH = sh;
  dstX = dx; dstY = dy; dstW = dw; dstH = dh;
  janX = dx; janY = dy; janW = dw; janH = dh;   // o reaplicar do bind usa estes
  if (!ligado || !midia[0]) return;
  snprintf(b, sizeof b,
           "{\"mediaId\":\"%s\","
           "\"source\":{\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d},"
           "\"destination\":{\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d},"
           "\"isFullScreen\":%s}",
           midia, sx, sy, sw, sh, dx, dy, dw, dh, cheia ? "true" : "false");
  printf("[video] fonte %d,%d %dx%d -> destino %d,%d %dx%d\n",
         sx, sy, sw, sh, dx, dy, dw, dh);
  fflush(stdout);
  chamar("setDisplayWindow", b, aoJanela);
}

double video_pos(void)      { return posSeg; }
double video_duracao(void)  { return durSeg; }
double video_buffer_fim(void) { return bufferSeg; }
int    video_tocando(void)  { return tocando; }
int    video_pronto(void)   { return pronto; }
// Ha midia carregada. O furo na superficie usa ISTO e nao o loadCompleted:
// abrir o buraco cedo nao custa nada (atras dele so existe o plano de video) e
// esperar o evento deixaria a tela desenhada por cima do video se o evento
// mudar de nome ou nao vier.
int    video_ativo(void)    { return midia[0] != 0; }

int  video_n_audio(void)   { return nAudio; }
int  video_n_legenda(void) { return nLeg; }
const VideoFaixa *video_audio(int i)   { return (i >= 0 && i < nAudio) ? &faixaAudio[i] : NULL; }
const VideoFaixa *video_legenda(int i) { return (i >= 0 && i < nLeg) ? &faixaLeg[i] : NULL; }
int  video_audio_atual(void)   { return audioAtual; }
int  video_legenda_atual(void) { return legAtual; }
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
int  video_largura(void)          { return vidW; }
int  video_altura(void)           { return vidH; }

void video_definir_dv(int dv) { dvPedido = dv ? 1 : 0; }

void video_escolher_audio(int i) {
  char b[192];
  const VideoFaixa *f = video_audio(i);
  if (!ligado || !midia[0] || !f) return;
  snprintf(b, sizeof b, "{\"mediaId\":\"%s\",\"type\":\"audio\",\"index\":%d}",
           midia, f->numero);
  chamar("selectTrack", b, soLog);
  audioAtual = i;
}

void video_escolher_legenda(int i) {
  char b[192];
  if (!ligado || !midia[0]) return;
  if (i < 0) {
    snprintf(b, sizeof b, "{\"mediaId\":\"%s\",\"enable\":false}", midia);
    chamar("setSubtitleEnable", b, soLog);
    legAtual = -1;
    return;
  }
  { const VideoFaixa *f = video_legenda(i);
    if (!f) return;
    snprintf(b, sizeof b, "{\"mediaId\":\"%s\",\"enable\":true}", midia);
    chamar("setSubtitleEnable", b, soLog);
    snprintf(b, sizeof b, "{\"mediaId\":\"%s\",\"type\":\"text\",\"index\":%d}",
             midia, f->numero);
    chamar("selectTrack", b, soLog);
    legAtual = i; }
}

void video_legenda_externa(const char *url) {
  char b[1200];
  if (!ligado || !midia[0] || !url || !*url) return;
  // O uMS baixa e sincroniza sozinho — o app so aponta. E o que permite usar
  // legenda do OpenSubtitles em arquivo que nao traz nenhuma embutida.
  snprintf(b, sizeof b, "{\"mediaId\":\"%s\",\"uri\":\"%s\"}", midia, url);
  chamar("setSubtitleSource", b, soLog);
  snprintf(b, sizeof b, "{\"mediaId\":\"%s\",\"enable\":true}", midia);
  chamar("setSubtitleEnable", b, soLog);
  printf("[video] legenda externa: %.80s\n", url);
  fflush(stdout);
}

void video_encerrar(void) {
  if (!ligado) return;
  video_parar();
  if (acb) { acbDestruir(acb); acb = 0; }
  if (laco) loopParar(laco);
  ligado = 0;
}
#endif
