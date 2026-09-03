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
#include "faixas.h"
#include "gfx.h"
#include "text.h"
#include "tex_cache.h"
#include "anim.h"
#include "layout.h"
#include "catalogo.h"
#include "trakt.h"
#include "sync.h"
#include "parental.h"
#include "episodios.h"
#include "streams.h"
#include "legenda.h"
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
#define PLR_ESCONDE_MS   4000u
// Salto de 10s do avanca/retrocede. E o passo do controle da Apple, e ele so
// vale com os controles em pe: cegamente, seta seria um pulo invisivel.
#define PLR_SALTO_SEG    10.0f
// Duracao de reserva, em segundos, para quando o `meta` do catalogo nao traz
// tempo de filme (as series trazem "3 temporadas", que nao e duracao de nada).
// 1h54 e so um numero plausivel para o layout ter o que mostrar — assim que o
// video real entrar, a duracao vem do decodificador e esta constante morre.
#define PLR_DUR_PADRAO   (114.0f * 60.0f)
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
#define PLR_PAD_X         64.0f
#define PLR_PAD_Y         48.0f
// Margem lateral do CONTEUDO do rodape (titulo, botoes, relogio). O trilho da
// barra continua em 0..largura; so o conteudo recua, para nao cair na zona que
// a TV corta por overscan. Mesmo valor do gutter da pagina de titulo.
#define PLR_MARGEM        96.0f
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
#define PLR_TRILHO_H       4.0f
// 20px com foco (`min(1.04vw, 20px)` em .player-progress-shell.focused).
#define PLR_TRILHO_H_FOCO  8.0f
#define PLR_TRILHO_R       0.0f   // canto vivo: ver a nota acima
#define PLR_GAP_BARRA     12.0f   // meta -> barra
#define PLR_GAP_ROW       32.0f   // barra -> fileira de botoes
#define PLR_GRAD_BAIXO   400.0f
#define PLR_GRAD_TOPO    300.0f
// #f5f5f5 = --secondary-color, que e o que preenche a barra no web.
#define PLR_FILL_C      (245.0f / 255.0f)

#define PLR_ICONE_H       48.0f
// De quanto o bloco desliza para baixo quando escondido. Pequeno de proposito:
// o que faz o movimento ser lido nao e a distancia, e a mola somada ao fade.
#define PLR_DESLIZE       46.0f
// Guia parental (.player-parental-*): barra de 6, lista recuada 20, linha de
// 36 com 4 de vao. Nao passam pela conversao x2 do bloco ATV — a regra base
// nao e refeita la.
#define PG_BARRA_W         6.0f
// Quanto tempo a guia parental fica na tela, contando do primeiro quadro com
// imagem, e quanto dura o esmaecimento final. Sete segundos e o bastante para
// ler quatro linhas curtas sem virar mobilia — depois disso ela nao volta nesta
// reproducao.
#define PG_SEG_TOTAL       7.0f
#define PG_SEG_SAIDA       0.8f
#define PG_LISTA_PADX     20.0f
#define PG_LINHA_H        36.0f
#define PG_LINHA_GAP       4.0f
// O veu virou os dois degrades do web (PLR_GRAD_TOPO/BAIXO). Ele existe para o
// texto ler sobre a imagem — sem ele, uma cena clara apaga o nome do titulo.

// Transporte compacto. Os saltos continuam acessiveis pelas setas na barra.
enum { PLR_PLAY, PLR_ASPECTO, PLR_CC, PLR_AUDIO,
       PLR_FONTES, PLR_EPISODIOS, PLR_NBTNS };

static int   aberto = 0, saindo = 0, pediuSair = 0;
static int   idx = 0;
static int   tocando = 1;
// Botao em foco na fileira de transporte. Comeca no PLAY porque e a resposta
// que nove de cada dez aberturas quer: o dedo para no centro e o OK decide.
static int   botao = PLR_PLAY;
// A barra de progresso e um alvo de foco, como no web: `.player-progress-shell`
// engorda de 6 para 10px e clareia o trilho quando focada. Fica FORA do enum
// dos botoes porque nao e um botao — o OK nela nao 'aperta' nada, e o
// ESQUERDA/DIREITA muda de significado (procura, em vez de trocar de foco).
static int   barraFoco = 0;
static int   visivel = 0;          // alvo dos controles (1 = em pe)
static float anim = 0.0f;          // 0..1 seguindo `visivel`, por mola
static float focoB[PLR_NBTNS];     // mola de foco de cada botao
static float entrada = 0.0f;       // 0..1 fade de abertura/fechamento da tela
static Uint32 ultimoInput = 0;
// Instante em que a IMAGEM comecou (nao a abertura da tela: entre uma coisa e
// outra ha a busca de fonte, que pode levar segundos). Zero enquanto nao houve.
// A guia parental se apoia nisto para aparecer UMA vez, no comeco, e sumir.
static Uint32 inicioImagem = 0;
// AS DUAS VARIAVEIS DE MIDIA. Todo o resto do arquivo le so daqui — quando o
// video real entrar, sao elas que passam a ser preenchidas pelo decodificador.
static int   comVideo = 0;
static int   pedFaixas = 0;
static int   esperandoFonte = 0;   // aberto sem URL, esperando o addon responder
static float posSeg = 0.0f;
static float duracaoSeg = PLR_DUR_PADRAO;

static char linhaEp[220];          // "T1, E1 · <sinopse curta>", montada na abertura

static const CatItem *item(void) { return cat_item(idx); }
static int epT, epE, pedFontes, erroFonte, pedProxT, pedProxE;
static int introIdx=-1, introT=-1, introE=-1;
static int retomadaAplicada, retomarPct;
int player_indice(void) { return idx; }
const char *player_linha_episodio(void) { return linhaEp; }
void player_episodio_atual(int *t, int *e) { *t = epT; *e = epE; }
int player_pediu_fontes(void) { int p = pedFontes; pedFontes = 0; return p; }
int player_pediu_proximo(int *t,int *e) {
  if(!pedProxT||!pedProxE)return 0;
  if(t)*t=pedProxT;if(e)*e=pedProxE;pedProxT=pedProxE=0;return 1;
}
const CatEp *player_proximo_episodio(void) {
  const CatEp *melhor=NULL;
  for(int i=0;i<cat_n_episodios(idx);i++) {
    const CatEp *p=cat_episodio(idx,i);if(!p)continue;
    if(p->temporada<epT||(p->temporada==epT&&p->episodio<=epE))continue;
    if(!melhor||p->temporada<melhor->temporada||
       (p->temporada==melhor->temporada&&p->episodio<melhor->episodio))melhor=p;
  }
  return melhor;
}
void player_erro_fonte(void) { esperandoFonte = 0; erroFonte = 1; visivel = 1; tocando = 0; }
void player_definir_episodio(int t, int e) {
  const CatItem *c = item();
  epT = t; epE = e; linhaEp[0] = 0;
  retomarPct = 0;
  if (c && c->progresso > 0 && c->progresso < 90 &&
      (strcmp(c->tipo,"series") || (t==c->temporada && e==c->episodio))) retomarPct=c->progresso;
  if (!c || strcmp(c->tipo, "series")) { epT = epE = 0; intro_desligar(); return; }
  if (epT < 1) epT = c->temporada > 0 ? c->temporada : 1;
  if (epE < 1) epE = c->episodio > 0 ? c->episodio : 1;
  snprintf(linhaEp, sizeof linhaEp, "T%dE%d", epT, epE);
  if (epT == c->temporada && epE == c->episodio && c->nomeEpisodio[0])
    snprintf(linhaEp, sizeof linhaEp, "T%dE%d · %s", epT, epE, c->nomeEpisodio);
  for (int i = 0; i < cat_n_episodios(idx); i++) {
    const CatEp *ep = cat_episodio(idx, i);
    if (ep && ep->temporada == epT && ep->episodio == epE) {
      snprintf(linhaEp, sizeof linhaEp, "T%dE%d · %s", epT, epE, ep->nome);
      break;
    }
  }
  if(idx!=introIdx||epT!=introT||epE!=introE){
    introIdx=idx;introT=epT;introE=epE;intro_pedir(c->imdb,epT,epE);
  }
}

// --- duracao a partir do texto livre do catalogo -----------------------------
// O campo `meta` e prosa, nao dado: "2023 · 3 h 28 min" num filme e
// "2022 · 3 temporadas" numa serie. Em vez de um parser posicional (que quebra
// no primeiro titulo com formato diferente), procuro apenas os dois pares
// numero+unidade em qualquer lugar da string. Nao achando NENHUM dos dois,
// devolvo 0 e quem chama cai no padrao — que e o caso correto para series.
static float duracaoDeMeta(const char *meta) {
  if (!meta) return 0.0f;
  float h = 0.0f, m = 0.0f;
  int achou = 0;
  for (const char *p = meta; *p; p++) {
    if (*p < '0' || *p > '9') continue;
    float v = 0.0f;
    while (*p >= '0' && *p <= '9') { v = v * 10.0f + (*p - '0'); p++; }
    while (*p == ' ') p++;
    // "min" tem que ser testado ANTES de "m": senao todo "min" vira minuto por
    // acidente do prefixo — o que ate daria certo aqui, mas escondia o bug do
    // dia em que aparecer uma unidade nova comecando com m.
    if (!strncmp(p, "min", 3))    { m = v; achou = 1; p += 2; }
    else if (*p == 'h')           { h = v; achou = 1; }
    if (!*p) break;
  }
  return achou ? (h * 3600.0f + m * 60.0f) : 0.0f;
}

// Corta a sinopse na primeira frase, sem passar de `maxBytes`. O corte respeita
// UTF-8: os titulos do catalogo sao em portugues e cortar no meio de um "ç" ou
// "ã" produz um retangulo vazio na fonte, nao um acento faltando.
static void frasePrimeira(char *dst, size_t n, const char *src, size_t maxBytes) {
  if (!src || !*src) { dst[0] = 0; return; }
  if (maxBytes > n - 4) maxBytes = n - 4;
  size_t i = 0, corte = 0;
  for (; src[i] && i < maxBytes; i++)
    if (src[i] == '.') { corte = i; break; }
  if (!corte) {
    corte = i;
    // volta ate o inicio de um caractere (bytes de continuacao sao 10xxxxxx)
    while (corte > 0 && ((unsigned char)src[corte] & 0xC0) == 0x80) corte--;
    while (corte > 0 && src[corte - 1] == ' ') corte--;
  }
  memcpy(dst, src, corte);
  dst[corte] = 0;
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
static int    aspecto = PLR_ASP_ORIGINAL;
static Uint32 toastAte = 0;      // ate quando o aviso de modo fica de pe
static char   dirPrefs[512];

// Rotulos em portugues. Os do web sao "Fit (Original)", "Crop", "Stretch",
// "Slight/Cinema/Ultra Zoom", "Fit Height", "Fit Width" — o resto do app fala
// portugues, entao traduzir aqui e o que mantem a tela coerente.
static const char *ASP_ROTULO[PLR_ASP_N] = {
  "Original", "Recortar", "Esticar", "Zoom leve",
  "Zoom cinema", "Zoom ultra", "Ajustar altura", "Ajustar largura"
};

const char *player_aspecto_rotulo(int modo) {
  if (modo < 0 || modo >= PLR_ASP_N) modo = PLR_ASP_ORIGINAL;
  return ASP_ROTULO[modo];
}
int player_aspecto(void) { return aspecto; }

// Onde o modo escolhido fica gravado. Mesmo diretorio que o main.c passa para o
// resto do app (SDL_GetBasePath()+"art", com /tmp/art de reserva). O web guarda
// isso em DeviceLocalPlayerPreferences, por aparelho: escolher "Zoom cinema" e
// reencontrar "Original" no filme seguinte transformaria o modo em brinquedo.
static const char *prefsArquivo(void) {
  static char caminho[600];
  if (!dirPrefs[0]) {
    char *base = SDL_GetBasePath();
    if (base) { snprintf(dirPrefs, sizeof dirPrefs, "%sart", base); SDL_free(base); }
    else      snprintf(dirPrefs, sizeof dirPrefs, "/tmp/art");
  }
  snprintf(caminho, sizeof caminho, "%s/player.txt", dirPrefs);
  return caminho;
}

// ESTILO DA LEGENDA: preferencia DO APARELHO, como o aspecto — nao vai em
// ajustes.txt, que espelha as chaves de layout do app web. Padrao: tamanho 2
// (o do aparelho), branco, sem fundo, posicao central, contorno.
static VideoLegendaEstilo legEstilo = { 120, 0, 0, 3, 1, 0, 0, TXT_FAMILIA_INTER };

static void prefsLer(void) {
  FILE *f = fopen(prefsArquivo(), "r");
  char chave[64]; int v;
  if (!f) return;
  while (fscanf(f, "%63s %d", chave, &v) == 2) {
    // Valor de outra versao (ou arquivo editado a mao) cai no padrao em vez de
    // indexar fora do vetor de rotulos.
    if (!strcmp(chave, "aspecto") && v >= 0 && v < PLR_ASP_N) aspecto = v;
    else if (!strcmp(chave, "leg_tamanho")) {
      /* Migra o arquivo antigo 0..4 sem perder a preferencia do aparelho. */
      static const int antigo[5]={60,80,120,160,200};
      if(v>=0&&v<=4)legEstilo.tamanho=antigo[v];
      else if(v>=50&&v<=200)legEstilo.tamanho=(v/10)*10;
    }
    else if (!strcmp(chave, "leg_cor")     && v >= 0 && v < VIDEO_LEG_NCORES) legEstilo.cor = v;
    else if (!strcmp(chave, "leg_fundo")   && v >= 0 && v <= 4)  legEstilo.fundo = v;
    else if (!strcmp(chave, "leg_pos")     && v >= 0 && v <= 7)  legEstilo.posicao = v;
    else if (!strcmp(chave, "leg_borda")   && v >= 0 && v <= 2)  legEstilo.borda = v;
    else if (!strcmp(chave, "leg_atraso")  && v > -10000 && v < 10000) legEstilo.atrasoMs = v;
    else if (!strcmp(chave, "leg_opacidade") && v >= 0 && v <= 3) legEstilo.opacidade = v;
    else if (!strcmp(chave, "leg_familia") && v >= 0 && v < TXT_FAMILIA_N) legEstilo.familia = v;
  }
  fclose(f);
}

static void prefsGravar(void) {
  FILE *f = fopen(prefsArquivo(), "w");
  if (!f) return;
  fprintf(f, "aspecto %d\n", aspecto);
  fprintf(f, "leg_tamanho %d\n", legEstilo.tamanho);
  fprintf(f, "leg_cor %d\n",     legEstilo.cor);
  fprintf(f, "leg_fundo %d\n",   legEstilo.fundo);
  fprintf(f, "leg_pos %d\n",     legEstilo.posicao);
  fprintf(f, "leg_borda %d\n",   legEstilo.borda);
  fprintf(f, "leg_atraso %d\n",  legEstilo.atrasoMs);
  fprintf(f, "leg_opacidade %d\n", legEstilo.opacidade);
  fprintf(f, "leg_familia %d\n", legEstilo.familia);
  fclose(f);
}

// Lidos pela folha de faixas, que e quem desenha os controles.
VideoLegendaEstilo *player_leg_estilo(void) { return &legEstilo; }
void player_leg_estilo_mudou(void) {
  video_legenda_estilo(&legEstilo);
  prefsGravar();
}

// Proporcao do QUADRO decodificado. Sem videoInfo ainda, 16:9 — que e a
// proporcao de quase todo arquivo entregue, e a suposicao que faz "Original"
// abrir em tela cheia em vez de piscar uma faixa errada por um segundo.
static float aspectoQuadro(void) {
  int w = video_largura(), h = video_altura();
  if (w > 0 && h > 0) return (float)w / (float)h;
  return NV_TELA_W / NV_TELA_H;
}

typedef struct { float x, y, w, h; } PlrRect;

static PlrRect aspectoRect(int modo) {
  const float tela = NV_TELA_W / NV_TELA_H;
  float q = aspectoQuadro();
  float bw, bh, sx = 1.0f, sy = 1.0f;
  PlrRect r;
  if (q <= 0.0f) q = tela;

  // 1) o object-fit do modo. Os tres casos sao os do ASPECT_MODE_DEFINITIONS.
  switch (modo) {
    case PLR_ASP_ESTICAR:                       // fill
      bw = NV_TELA_W; bh = NV_TELA_H;
      break;
    case PLR_ASP_CROP:                          // cover
    case PLR_ASP_ZOOM_LEVE:
    case PLR_ASP_ZOOM_CINEMA:
    case PLR_ASP_FIT_ALTURA:
      if (q > tela) { bh = NV_TELA_H; bw = bh * q; }
      else          { bw = NV_TELA_W; bh = bw / q; }
      break;
    default:                                    // contain
      if (q > tela) { bw = NV_TELA_W; bh = bw / q; }
      else          { bh = NV_TELA_H; bw = bh * q; }
      break;
  }

  // 2) a escala do modo, copiada linha a linha do resolveAspectScale.
  switch (modo) {
    case PLR_ASP_CROP:        sx = sy = (q > tela) ? q / tela : tela / q; break;
    case PLR_ASP_ESTICAR:     if (q > tela) sy = q / tela; else sx = tela / q; break;
    case PLR_ASP_ZOOM_LEVE:   sx = sy = PLR_ZOOM_LEVE;   break;
    case PLR_ASP_ZOOM_CINEMA: sx = sy = PLR_ZOOM_CINEMA; break;
    case PLR_ASP_ZOOM_ULTRA:  sx = sy = PLR_ZOOM_ULTRA;  break;
    case PLR_ASP_FIT_ALTURA:  if (q > tela) sx = sy = q / tela; break;
    case PLR_ASP_FIT_LARGURA: if (q < tela) sx = sy = tela / q; break;
    default: break;   // ORIGINAL: contain e nada mais
  }

  r.w = bw * sx;
  r.h = bh * sy;
  r.x = (NV_TELA_W - r.w) * 0.5f;
  r.y = (NV_TELA_H - r.h) * 0.5f;
  return r;
}

// O retangulo VISIVEL do modo: o retangulo virtual cortado pela tela. E ele que
// o furo do GL segue e que vira o destino do plano.
static PlrRect aspectoVisivel(int modo) {
  PlrRect r = aspectoRect(modo), d;
  d.x = r.x < 0.0f ? 0.0f : r.x;
  d.y = r.y < 0.0f ? 0.0f : r.y;
  d.w = (r.x + r.w > NV_TELA_W ? NV_TELA_W : r.x + r.w) - d.x;
  d.h = (r.y + r.h > NV_TELA_H ? NV_TELA_H : r.y + r.h) - d.y;
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
static void aplicarAspecto(void) {
  PlrRect r, d;
  float qw, qh;
  int sx, sy, sw, sh;
  if (!comVideo) return;

  r = aspectoRect(aspecto);
  d = aspectoVisivel(aspecto);
  if (d.w < 1.0f || d.h < 1.0f || r.w < 1.0f || r.h < 1.0f) return;

  qw = (float)video_largura();
  qh = (float)video_altura();
  // Sem as dimensoes do quadro nao da para falar em coordenadas de fonte. Cai
  // no caminho antigo, que serve ao caso sem recorte — e o unico em que ele
  // funciona. Assim que o videoInfo chegar, aplicarAspecto roda de novo.
  if (qw < 2.0f || qh < 2.0f) {
    video_janela((int)(d.x + 0.5f), (int)(d.y + 0.5f),
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

  video_janela_fonte(sx, sy, sw, sh,
                     (int)(d.x + 0.5f), (int)(d.y + 0.5f),
                     (int)(d.w + 0.5f), (int)(d.h + 0.5f));
}

void player_aspecto_definir(int modo) {
  if (modo < 0 || modo >= PLR_ASP_N) modo = PLR_ASP_ORIGINAL;
  aspecto = modo;
  prefsGravar();
  aplicarAspecto();
}

void player_aspecto_ciclar(void) {
  player_aspecto_definir((aspecto + 1) % PLR_ASP_N);
  toastAte = SDL_GetTicks() + PLR_TOAST_MS;
}

void player_abrir(int indiceCatalogo, const char *url) {
  int n = cat_n(); if (n < 1) n = 1;
  idx = ((indiceCatalogo % n) + n) % n;
  aberto = 1; saindo = 0; pediuSair = 0; barraFoco = 0;
  // Guia parental do titulo: pedido AQUI e nao no desenho, para que a resposta
  // ja tenha chegado quando os controles aparecerem pela primeira vez.
  { const CatItem *ci = cat_item(idx);
    if (ci && ci->imdb[0]) parental_pedir(ci->imdb); }
  tocando = 1; visivel = 1; anim = 0.0f; entrada = 0.0f;
  pedFontes = erroFonte = pedFaixas = pedProxT = pedProxE = 0; inicioImagem = 0;
  retomadaAplicada=0;
  botao = PLR_PLAY;
  memset(focoB, 0, sizeof focoB);
  posSeg = 0.0f;
  ultimoInput = SDL_GetTicks();
  esperandoFonte = (url == NULL);
  // Legenda externa e da sessao que acabou, nao desta.
  faixas_reiniciar();
  // O modo de proporcao e do APARELHO, nao da sessao: reler aqui e o que faz
  // "Zoom cinema" continuar valendo no filme seguinte, como no web.
  prefsLer();
  toastAte = 0;
  comVideo = (url && *url && video_tocar(url));
  aplicarAspecto();

  const CatItem *c = item();
  float d = c ? duracaoDeMeta(c->meta) : 0.0f;
  duracaoSeg = d > 1.0f ? d : PLR_DUR_PADRAO;

  // Identidade do episodio e independente do foco no painel de navegacao.
  player_definir_episodio(c ? c->temporada : 0, c ? c->episodio : 0);
  if (url && *url && !comVideo) player_erro_fonte();
}

int player_aberto(void)    { return aberto; }
int player_quer_sair(void) { return pediuSair; }
// So depois do loadCompleted. Antes disso o pipeline ainda nao pos nada no
// plano de hardware, e furar a superficie cedo trocava a arte por um retangulo
// PRETO enquanto o fluxo abria — que era o "clica em reproduzir e fica preto".
void player_definir_fonte(const char *url) {
  if (!aberto || !url || !*url) return;
  esperandoFonte = 0;
  erroFonte = 0;
  comVideo = video_tocar(url);
  if (!comVideo) player_erro_fonte();
  aplicarAspecto();
}

// Consome o pedido de abrir a folha de faixas: quem le, zera.
int  player_pediu_faixas(void) { int v = pedFaixas; pedFaixas = 0; return v; }

int  player_com_video(void) { return comVideo && video_pronto(); }

// Esta abrindo o fluxo: ha video pedido, mas ainda nao ha imagem.
int  player_carregando(void) { return esperandoFonte || (comVideo && !video_pronto()); }
int  player_controles_visiveis(void) { return visivel; }

void player_encerrar(void) {
  // Salvar ANTES de parar: video_parar descarrega o pipeline e a posicao some
  // junto. Titulo quase no fim conta como visto por inteiro — voltar a um card
  // marcando "2 min restantes" que na verdade acabou e pior que arredondar.
  if (comVideo && video_pronto() && duracaoSeg > 1.0f) {
    float pos = posSeg >= duracaoSeg - 60.0f ? duracaoSeg : posSeg;
    const CatItem *ci = cat_item(idx);
    home_registrar_retorno(idx, pos, duracaoSeg);
    cat_salvar_progresso_ep(idx, pos, duracaoSeg,epT,epE);
    // E tambem para o Trakt, que e de onde o "continue assistindo" vem: gravar
    // so aqui deixaria este app discordando dos outros aparelhos do dono.
    if (ci && ci->imdb[0]) {
      char id[64];
      if (epT > 0 && epE > 0) snprintf(id, sizeof id, "%.*s:%d:%d", (int)strcspn(ci->imdb,":"),ci->imdb, epT, epE);
      else snprintf(id, sizeof id, "%s", ci->imdb);
      trakt_marcar(id, pos, duracaoSeg);
      // E para a CONTA. Trakt e conta sao dois destinos diferentes: nem todo
      // usuario liga o Trakt, e o progresso do app oficial vem da conta.
      sync_sujar_progresso();
    }
  }
  if (comVideo) video_parar();
  comVideo = 0; esperandoFonte = 0; aberto = 0; saindo = 0; pediuSair = 0;
  inicioImagem = 0;
  episodios_fechar();
  intro_desligar(); introIdx=introT=introE=-1;
  legenda_desligar();
}

static int ofertaProximo(void) {
  const CatEp *p=player_proximo_episodio();double fim;int tipo;
  if(!p||duracaoSeg<=1)return 0;
  if(intro_ativo(posSeg,&fim,&tipo)&&tipo==INTRO_CREDITOS)return 1;
  return duracaoSeg-posSeg<=120.0f;
}

// Toda tecla acorda os controles, inclusive a que ja executou alguma acao: no
// aparelho nao existe comando que aconteca com a barra escondida sem trazer a
// barra junto — o usuario precisa ver o efeito do que apertou.
static void acordar(void) { visivel = 1; ultimoInput = SDL_GetTicks(); }

static void alternarTocando(void) {
  tocando = !tocando;
  if (comVideo) video_pausar(!tocando);
}

// Salto de 10s com limite. So vale com os controles em pe: cegamente, seta
// seria um pulo invisivel — com os botoes, quem aperta esta olhando para um
// botao que diz «10 / 10».
static void saltar(int dir) {
  posSeg += dir * PLR_SALTO_SEG;
  posSeg = anim_clamp(posSeg, 0.0f, duracaoSeg);
  if (comVideo) video_buscar(posSeg);
}

void player_evento(const SDL_Event *e) {
  if (!aberto || saindo || e->type != SDL_KEYDOWN) return;
  SDL_Keycode k = e->key.keysym.sym;

  if (k == SDLK_ESCAPE || k == SDLK_AC_BACK || k == SDLK_BACKSPACE ||
      k == SDLK_DELETE) {
    saindo = 1; pediuSair = 1;
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
  if (k == SDLK_0 || k == SDLK_KP_0) { player_aspecto_ciclar(); return; }

  if (!visivel) {
    if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) {
      if(ofertaProximo()) { const CatEp*p=player_proximo_episodio();pedProxT=p->temporada;pedProxE=p->episodio;return; }
      { double fim;int tipo;if(intro_ativo(posSeg,&fim,&tipo)&&tipo!=INTRO_CREDITOS){
          posSeg=(float)fim+.25f;if(comVideo)video_buscar(posSeg);return; } }
      alternarTocando(); acordar(); return;
    }
    if (k == SDLK_UP || k == SDLK_DOWN || k == SDLK_LEFT || k == SDLK_RIGHT)
      acordar();
    return;
  }

  // CONTROLES EM PE: o foco anda pelos botoes e o OK aperta o botao em foco.
  if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) {
    // Na barra o OK pausa/retoma: e o que sobra de util, ja que a barra nao
    // tem acao propria no web.
    if (barraFoco) { alternarTocando(); acordar(); return; }
    switch (botao) {
      case PLR_PLAY:    alternarTocando(); break;
      case PLR_ASPECTO: player_aspecto_ciclar(); break;
      // CC e AUDIO abrem a MESMA folha, mas em colunas diferentes: apertar
      // "legendas" e cair no audio fazia os dois botoes parecerem um so.
      case PLR_CC:      pedFaixas = 2;     break;   // 2 = coluna da legenda
      case PLR_FONTES:  pedFontes = 1; break;
      case PLR_EPISODIOS: if (epT > 0) episodios_abrir(idx, epT, epE); break;
      default:          pedFaixas = 1;     break;   // 1 = coluna do audio
    }
    acordar();
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
    if (!barraFoco) barraFoco = 1; else pedFaixas = 1;
    acordar();
    return;
  }
  if (barraFoco) {
    // Na barra, ESQUERDA e DIREITA procuram no filme em vez de trocar de botao.
    if (k == SDLK_LEFT)       saltar(-1);
    else if (k == SDLK_RIGHT) saltar(1);
    else if (k == SDLK_DOWN)  barraFoco = 0;
    acordar();
    return;
  }
  if (k == SDLK_DOWN) {
    // BAIXO a partir da fileira significa "tirar os controles da frente".
    // Nao chama acordar(): isso recolocaria a barra no mesmo evento e faria o
    // comando parecer quebrado. O proximo toque direcional a revela de novo.
    barraFoco = 0;
    visivel = 0;
    ultimoInput = SDL_GetTicks();
    return;
  }
  // Sem rotacao nas pontas: a fileira e curta e cabe inteira no olhar; dar a
  // volta no fim le como erro, nao como atalho.
  if (k == SDLK_LEFT  && botao > 0)          botao--;
  else if (k == SDLK_RIGHT && botao < PLR_NBTNS - (epT > 0 ? 1 : 2)) botao++;
  acordar();
}

void player_atualizar(float dt, Uint32 agora) {
  if (!aberto) return;

  entrada = anim_mola(entrada, saindo ? 0.0f : 1.0f, dt, NV_MOLA_TELA);
  // Marca o primeiro quadro COM IMAGEM. E daqui que a guia parental conta o
  // tempo dela — contar da abertura da tela faria a guia gastar o prazo
  // enquanto o app ainda procurava fonte, e ela sumiria antes de o filme
  // aparecer.
  if (!inicioImagem && comVideo && video_pronto()) { inicioImagem = agora; acordar(); }
  if (saindo && entrada < 0.02f) { aberto = 0; saindo = 0; entrada = 0.0f; return; }

  // Havendo pipeline, posicao e duracao vem DELE; o dt so serve para as
  // animacoes. O relogio somado continua existindo para quando nao ha video
  // (no Mac, ou se a fonte falhar): sem ele a barra ficaria parada em zero e a
  // tela mentiria dizendo que nada acontece.
  // O retangulo depende da proporcao do QUADRO, e ela so existe quando o
  // videoInfo chega — segundos depois da abertura. Sem esta releitura o modo
  // ficaria calculado com o chute de 16:9 para sempre, e num arquivo 3840x1606
  // (que e o caso real medido nesta TV) o "Original" cortaria a imagem.
  if (comVideo) {
    static int ultLarg, ultAlt;
    int lw = video_largura(), lh = video_altura();
    if (lw != ultLarg || lh != ultAlt) { ultLarg = lw; ultAlt = lh; aplicarAspecto(); }
  }

  if (comVideo && video_ativo()) {
    double d = video_duracao();
    posSeg = (float)video_pos();
    if (d > 1.0) duracaoSeg = (float)d;
    if (!retomadaAplicada && video_pronto() && d>1.0) {
      retomadaAplicada=1;
      if(retomarPct>0) video_buscar(d*retomarPct/100.0);
    }
    tocando = video_tocando();
  } else if (tocando && !esperandoFonte && !erroFonte) {
    posSeg += dt;
    if (posSeg >= duracaoSeg) { posSeg = duracaoSeg; tocando = 0; }
  }

  // Pausado, os controles ficam. Sumir com eles deixaria o usuario diante de um
  // quadro parado sem nenhuma pista de que foi ele quem pausou.
  if (visivel && tocando && !player_carregando() && !episodios_aberto() &&
      !stream_folha_aberta() && !faixas_aberta() && agora - ultimoInput > PLR_ESCONDE_MS) visivel = 0;
  if (epT > 0 && !strstr(linhaEp, " · ")) player_definir_episodio(epT, epE);

  anim = anim_mola(anim, visivel ? 1.0f : 0.0f, dt,
                   visivel ? NV_MOLA_FOCO : NV_MOLA_DESFOCO);
  for (int i = 0; i < PLR_NBTNS; i++) {
    float alvo = (visivel && botao == i) ? 1.0f : 0.0f;
    focoB[i] = anim_mola(focoB[i], alvo, dt,
                         alvo > focoB[i] ? NV_MOLA_FOCO : NV_MOLA_DESFOCO);
  }
}

// hh:mm:ss so quando passa de uma hora — "0:03:12" num episodio curto le como
// erro de formatacao, nao como tempo.
static void fmtTempo(char *b, size_t n, float seg, int negativo) {
  if (seg < 0.0f) seg = 0.0f;
  int t = (int)(seg + 0.5f);
  int h = t / 3600, m = (t / 60) % 60, s = t % 60;
  const char *sinal = negativo ? "-" : "";
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
static void iconeArquivo(float cx, float cy, float a, float lum,
                         const char *nome, float tam) {
  GfxRect r = { cx - tam * 0.5f, cy - tam * 0.5f, tam, tam };
  gfx_icone(r, nome, lum, lum, lum, a * 0.94f);
}

static void iconePlayPause(float cx, float cy, float a, int pausar, float lum) {
  iconeArquivo(cx, cy, a, lum, pausar ? "pause" : "play", PLR_ICONE_H * 1.15f);
}

static void iconeLegendas(float cx, float cy, float a, float lum) {
  iconeArquivo(cx, cy, a, lum, "legenda", PLR_ICONE_H * 1.15f);
}

static void iconeAudio(float cx, float cy, float a, float lum) {
  iconeArquivo(cx, cy, a, lum, "audio", PLR_ICONE_H * 1.15f);
}

static void iconeAspecto(float cx, float cy, float a, float lum) {
  iconeArquivo(cx, cy, a, lum, "aspecto", PLR_ICONE_H * 1.15f);
}

// Um botao circular do transporte: translucido quando solto, branco quando em
// foco, e o glifo sempre com o contraste certo contra o fundo dele.
static void botaoCirculo(float cx, float cy, float f, float a, int sel) {
  float d = PLR_BTN_D * (1.0f + 0.09f * f);
  GfxRect r = { cx - d * 0.5f, cy - d * 0.5f, d, d };
  if (sel) gfx_cor(r, 0.5f, 0.97f, 0.97f, 0.98f, 0.96f * a);
  else     gfx_cor(r, 0.5f, 0.05f, 0.05f, 0.06f, 0.42f * a);
}

static void corLegenda(int i,int *r,int *g,int *b){
  static const unsigned char c[VIDEO_LEG_NCORES][3]={
    {255,255,255},{255,222,48},{64,224,112},{78,156,255},{255,80,80},{18,18,18}};
  if(i<0||i>=VIDEO_LEG_NCORES)i=0;*r=c[i][0];*g=c[i][1];*b=c[i][2];
}

/* O uMS da C9 limita fonte e escala. OpenSubtitles passa por este overlay
 * SDL/GLES, exatamente como o overlay HTML do app web. */
static void desenharLegendaExterna(void){
  char texto[768],*linha,*salva;TxtLinha cor[4],borda[4];int n=0,r,g,b;
  if(!legenda_texto(posSeg,legEstilo.atrasoMs,texto,sizeof texto))return;
  int pct=legEstilo.tamanho;if(pct<50)pct=50;if(pct>200)pct=200;pct=(pct/10)*10;
  TxtEstilo est=(TxtEstilo)(TXT_LEG_50+(pct-50)/10);corLegenda(legEstilo.cor,&r,&g,&b);
  float alpha=(legEstilo.opacidade==3?.25f:legEstilo.opacidade==2?.5f:legEstilo.opacidade==1?.75f:1.f)*entrada;
  linha=strtok_r(texto,"\n",&salva);
  while(linha&&n<4){
    TxtFamilia fam=(TxtFamilia)legEstilo.familia;
    cor[n]=txt_linha_corta_familia(est,linha,r,g,b,255,1660,fam);
    borda[n]=legEstilo.borda?txt_linha_corta_familia(est,linha,0,0,0,255,1660,fam):(TxtLinha){0};
    n++;linha=strtok_r(NULL,"\n",&salva);
  }
  if(!n)return;
  float total=0;for(int i=0;i<n;i++)total+=cor[i].h+(i?5:0);
  float base=visivel?760.f:1000.f;
  if(ofertaProximo())base=690.f;
  base-=(legEstilo.posicao-3)*48.f;
  float y=base-total;
  for(int i=0;i<n;i++){
    TxtLinha l=cor[i];float x=(NV_TELA_W-l.w)*.5f;
    if(legEstilo.fundo){float fa=legEstilo.fundo*.16f*alpha;gfx_cor((GfxRect){x-18,y-6,l.w+36,l.h+12},.16f,0,0,0,fa);}
    if(borda[i].tex){float d=legEstilo.borda==2?4.f:2.f;
      txt_desenhar_alpha(borda[i],x+d,y+d,.82f*alpha);
      if(legEstilo.borda==1){txt_desenhar_alpha(borda[i],x-d,y,.82f*alpha);txt_desenhar_alpha(borda[i],x,y-d,.82f*alpha);}
    }
    txt_desenhar_alpha(l,x,y,alpha);y+=l.h+5;
  }
}

static void desenharAcoesEpisodio(void){
  const CatEp *prox=player_proximo_episodio();double fim;int tipo=0;
  int trecho=intro_ativo(posSeg,&fim,&tipo);
  if(ofertaProximo()&&prox){
    GfxRect p={420,720,1080,194};gfx_cor(p,.10f,.045f,.045f,.05f,.94f*entrada);
    gfx_rect(p,0,GFX_ANEL,0,.008f,0,.10f,1,1,1,.20f*entrada);
    const char *arte=prox->thumb[0]?prox->thumb:(item()&&item()->backdrop[0]?item()->backdrop:NULL);
    if(arte){GLuint tx=tex_obter_larg(arte,288);if(tx){gfx_tex_aspect_atual=tex_aspecto(arte);gfx_rect((GfxRect){450,738,288,158},tx,GFX_CARD,0,0,0,.08f,1,1,1,entrada);gfx_tex_aspect_atual=0;}}
    TxtLinha l=txt_linha(TXT_PLR_CORPO,"Próximo episódio",205,207,213,255);txt_desenhar_alpha(l,782,752,entrada);
    char nome[220];snprintf(nome,sizeof nome,"T%dE%d · %s",prox->temporada,prox->episodio,prox->nome);
    TxtLinha t=txt_linha_corta(TXT_PLR_TITULO,nome,250,250,252,255,430);txt_desenhar_alpha(t,782,794,entrada);
    GfxRect bot={1240,775,220,76};gfx_cor(bot,.5f,.08f,.08f,.09f,.96f*entrada);gfx_rect(bot,0,GFX_ANEL,0,.018f,0,.5f,1,1,1,.35f*entrada);
    gfx_icone((GfxRect){1264,793,40,40},"play",1,1,1,entrada);TxtLinha rt=txt_linha(TXT_BODY,"Reproduzir",246,246,248,255);txt_desenhar_alpha(rt,1310,797,entrada);
  } else if(trecho&&tipo!=INTRO_CREDITOS){
    const char *rot=tipo==INTRO_RESUMO?"Pular resumo":"Pular abertura";
    TxtLinha t=txt_linha(TXT_BODY,rot,250,250,252,255);float w=t.w+116;
    GfxRect p={64,730,w,88};gfx_cor(p,.5f,.075f,.075f,.085f,.94f*entrada);
    gfx_icone((GfxRect){88,752,44,44},"avancar",1,1,1,entrada);txt_desenhar_alpha(t,148,752,entrada);
  }
}

void player_desenhar(Uint32 agora) {
  (void)agora;
  if (!aberto) return;
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
  GfxRect tela = { 0, 0, NV_TELA_W, NV_TELA_H };
  if (player_com_video()) {
    // O furo acompanha o MESMO retangulo que foi ao plano de hardware, cortado
    // na tela. Furar sempre a tela inteira, como antes, deixava faixa preta nos
    // modos que nao ocupam tudo ("Original" num 2.39:1 entregue como 2.39:1):
    // o furo mostrava o nada atras do plano em vez de mostrar o plano.
    PlrRect r = aspectoVisivel(aspecto);
    GfxRect furo;
    furo.x = r.x; furo.y = r.y; furo.w = r.w; furo.h = r.h;
    // Fora do furo fica PRETO, e nao a arte-chave: e o que a TV mostra ao lado
    // do plano de video, e pintar outra coisa ali criaria uma borda que nao
    // existe no aparelho.
    if (furo.w < NV_TELA_W - 0.5f || furo.h < NV_TELA_H - 0.5f)
      gfx_cor(tela, 0.0f, 0, 0, 0, 1.0f);
    if (furo.w > 0.0f && furo.h > 0.0f) gfx_furo(furo);
  } else {
    const char *arte = (c && c->backdrop[0]) ? c->backdrop : NULL;
    GLuint tex = arte ? tex_obter_hero(arte) : 0;   // ocupa a tela inteira
    if (tex) {
      gfx_tex_aspect_atual = tex_aspecto(arte);
      gfx_rect(tela, tex, GFX_CARD, 0, 0, 0, 0.0f, 0, 0, 0, entrada);
      gfx_tex_aspect_atual = 0.0f;
    } else {
      gfx_cor(tela, 0.0f, 0.04f, 0.04f, 0.05f, entrada);
    }
  }

  // Indicador de abertura: pontos pulsando no centro, sobre a arte escurecida.
  // Um giro exigiria rotacao no shader; tres pontos em contrafase dizem a mesma
  // coisa com o que ja existe, e leem bem de longe.
  if (player_carregando()) {
    GfxRect escuro = { 0, 0, NV_TELA_W, NV_TELA_H };
    int k;
    gfx_cor(escuro, 0.0f, 0, 0, 0, 0.55f * entrada);
    GLuint logo = c && c->logo[0] ? tex_obter_larg(c->logo, 520) : 0;
    if (logo) {
      float ar = tex_aspecto(c->logo), w = 520, h = ar > 0 ? w / ar : 120;
      if (h > 160) { h = 160; w = h * ar; }
      gfx_rect((GfxRect){(NV_TELA_W-w)*.5f,NV_TELA_H*.5f-h-60,w,h},logo,
               tex_marca_escura(c->logo)?GFX_MARCA:GFX_TEXTO,0,0,0,0,.95f,.95f,.97f,entrada);
    } else {
      TxtLinha t = txt_linha_corta(TXT_PLR_TITULO,c?c->titulo:"Reproduzindo",240,241,244,255,680);
      txt_desenhar_alpha(t,(NV_TELA_W-t.w)*.5f,NV_TELA_H*.5f-150,entrada);
    }
    // Anel com cauda luminosa, animado sem novas texturas por quadro.
    for (k = 0; k < 12; k++) {
      float ang = k * 6.2831853f / 12.0f + agora * .006f;
      float br = .18f + .82f * k / 11.0f;
      GfxRect pt = {NV_TELA_W*.5f + cosf(ang)*24 - 4,
                    NV_TELA_H*.5f + sinf(ang)*24 - 4,8,8};
      gfx_cor(pt,.5f,.95f,.95f,.97f,br*entrada);
    }
    { TxtLinha lc = txt_linha(TXT_CALLOUT, "Abrindo fonte", 236, 237, 242, 255);
      txt_desenhar_alpha(lc, NV_TELA_W * 0.5f - lc.w * 0.5f,
                         NV_TELA_H * 0.5f + 50, 0.85f * entrada); }
    if (linhaEp[0]) {
      TxtLinha le = txt_linha_corta(TXT_PG_FIM,linhaEp,196,198,204,255,680);
      txt_desenhar_alpha(le,(NV_TELA_W-le.w)*.5f,NV_TELA_H*.5f+94,entrada);
    }
  }
  if (erroFonte) {
    gfx_cor(tela,0,.02f,.02f,.025f,.65f);
    TxtLinha er=txt_linha(TXT_CALLOUT,"Não foi possível abrir a fonte",240,241,243,255);
    txt_desenhar_alpha(er,(NV_TELA_W-er.w)*.5f,400,entrada);
    TxtLinha aj=txt_linha(TXT_PG_FIM,"Abra Fontes para escolher outra opção ou recarregar.",192,194,200,255);
    txt_desenhar_alpha(aj,(NV_TELA_W-aj.w)*.5f,448,entrada);
  }

  // --- aviso de troca de modo de proporcao ---------------------------------
  // O #playerAspectToast do web, com as medidas do bloco de TV do CSS:
  //   top min(8.33vw,160px)=160  altura min(6.67vw,128px)=128
  //   padding lateral min(3.33vw,64px)=64  fonte min(2.92vw,56px)=56
  //   fundo rgba(9,13,20,0.88), borda rgba(255,255,255,0.18), raio 999 (pilula)
  // Ele e desenhado ANTES do corte por `a`: a tecla de proporcao funciona com
  // os controles escondidos, e um aviso que so aparecesse com a barra em pe
  // deixaria a troca sem nenhuma confirmacao no caso mais comum.
  if (toastAte > agora) {
    // Some com fade nos ultimos 200ms, que e a `transition: opacity 200ms` do
    // bloco de TV. Aparecer e sumir de estalo le como falha de desenho.
    float resta = (float)(toastAte - agora);
    float at = (resta < 200.0f ? resta / 200.0f : 1.0f) * entrada;
    TxtLinha l = txt_linha(TXT_PLR_TITULO, player_aspecto_rotulo(aspecto),
                           243, 248, 255, 242);
    float pw = (float)l.w + 128.0f, ph = 128.0f;
    GfxRect pil = { (NV_TELA_W - pw) * 0.5f, 160.0f, pw, ph };
    // Raio e FRACAO do menor lado (ver gfx.h): 0.5 e a pilula completa.
    gfx_cor(pil, 0.5f, 9.0f / 255.0f, 13.0f / 255.0f, 20.0f / 255.0f, 0.88f * at);
    txt_desenhar_alpha(l, pil.x + (pw - l.w) * 0.5f,
                       pil.y + (ph - (float)l.h) * 0.5f, at);
  }

  /* Permanecem quando os controles somem: sao conteudo, nao chrome do player. */
  desenharLegendaExterna();
  desenharAcoesEpisodio();

  float a = anim * entrada;
  if (a <= 0.005f) return;   // tocando limpo: nada por cima da imagem

  // Dois degrades, como no web: .player-controls-gradient-top (150px, 0.7 -> 0)
  // e .player-controls-gradient-bottom (200px, 0 -> 0.8). O de baixo sustenta o
  // titulo e a barra; o de cima existe porque os selos e a classificacao ficam
  // no alto e sem ele sumiriam sobre cena clara. Ambos acompanham a animacao
  // dos controles: fixos, deixariam sombra permanente em toda cena.
  GfxRect veu = { 0, NV_TELA_H - PLR_GRAD_BAIXO, NV_TELA_W, PLR_GRAD_BAIXO };
  // GFX_VEU_BAIXO e nao GFX_VEU: aquele escurece tambem a ESQUERDA (feito para
  // o hero da home) e deixava o canto superior esquerdo deste retangulo escuro
  // com o direito transparente — a borda entre os dois lia como uma placa.
  gfx_rect(veu, 0, GFX_VEU_BAIXO, 0, 0, 0, 0.0f, 0, 0, 0, 0.86f * a);
  { GfxRect topo = { 0, 0, NV_TELA_W, PLR_GRAD_TOPO };
    gfx_rect(topo, 0, GFX_VEU_TOPO, 0, 0, 0, 0.0f, 0, 0, 0, 0.70f * a); }

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
  float eEnt  = 1.0f - (1.0f - entrada) * (1.0f - entrada) * (1.0f - entrada);
  float desce = (1.0f - anim) * PLR_DESLIZE
              + (1.0f - eEnt) * PLR_DESLIZE * 1.8f;

  // Ancoragem de baixo para cima, na ordem da coluna .player-controls-bottom do
  // web lida ao contrario: a fileira de botoes encosta na margem inferior, a
  // barra fica 16px acima dela e a meta 12px acima da barra. A margem e
  // --player-controls-y (48), nao a margem geral do app.
  float yRowTopo = NV_TELA_H - PLR_PAD_Y - PLR_BTN_D + desce;
  float cyBotoes = yRowTopo + PLR_BTN_D * 0.5f;
  float yBarra   = yRowTopo - PLR_GAP_ROW - PLR_TRILHO_H;

  // --- barra de progresso ---
  // A barra ocupa a largura util inteira, entre as margens do player. Sem
  // marcador na cabeca: o web nao tem um — a barra engorda de 6 para 10px
  // quando recebe foco, e e isso que diz que ela e operavel. Aqui o foco anda
  // so pelos botoes, entao ela fica sempre em 6.
  // DE PONTA A PONTA: encosta nas duas bordas da tela. Com margem ela lia como
  // um componente solto no meio do rodape; encostada, ela e a borda do video.
  float bx = 0.0f, bw = NV_TELA_W;
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
  float cx = bx + PLR_MARGEM;
  float cw = bw - PLR_MARGEM * 2.0f;
  float frac = duracaoSeg > 0.0f ? anim_clamp(posSeg / duracaoSeg, 0.0f, 1.0f) : 0.0f;
  // Com foco o trilho engorda de 6 para 10 e clareia de 0.30 para 0.45, e ele
  // cresce para BAIXO a partir da mesma linha de base — subir moveria tambem a
  // meta e o titulo, que estao ancorados nela.
  // O trilho cresce para BAIXO a partir da mesma linha de base — subir moveria
  // tambem o titulo, que esta ancorado nela.
  float hTrilho = barraFoco ? PLR_TRILHO_H_FOCO : PLR_TRILHO_H;
  GfxRect trilho = { bx, yBarra, bw, hTrilho };
  GfxRect andado = { bx, yBarra, bw * frac, hTrilho };
  gfx_cor(trilho, PLR_TRILHO_R, 1, 1, 1, (barraFoco ? 0.34f : 0.22f) * a);
  // O buffer do pipeline, entre o andado e o fim: e o que mostra que o video
  // esta a frente do relogio. Sem dado do pipeline o segmento nao existe —
  // inventar "quase todo carregado" seria pior que a barra simples. No web ele
  // e a MESMA cor do preenchimento a 0.35 (.player-progress-buffered).
  { float bufFrac = duracaoSeg > 0.0f ? anim_clamp(video_buffer_fim() / duracaoSeg, 0.0f, 1.0f) : 0.0f;
    if (bufFrac > frac + 0.004f) {
      GfxRect buf = { bx + bw * frac, yBarra, bw * (bufFrac - frac), hTrilho };
      gfx_cor(buf, PLR_TRILHO_R, PLR_FILL_C, PLR_FILL_C, PLR_FILL_C, 0.35f * a);
    } }
  // Meio pixel ja conta: com o teste em 1.0 o inicio do filme nao desenhava
  // nada, e a barra parecia so comecar a andar depois de um tempo.
  if (andado.w > 0.5f)
    gfx_cor(andado, PLR_TRILHO_R, PLR_FILL_C, PLR_FILL_C, PLR_FILL_C, a);

  // Filme: somente nome. Serie: nome seguido de T/E e titulo do episodio.
  // O arquivo e o provedor pertencem a folha de fontes, nao ao transporte.
  float yMetaBase = yBarra - PLR_GAP_BARRA;
  if (linhaEp[0]) {
    TxtLinha le=txt_linha_corta(TXT_PLR_CORPO,linhaEp,218,220,224,255,cw*.67f);
    yMetaBase-=le.h;
    txt_desenhar_alpha(le,cx,yMetaBase,a);
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
  float hTit, yTit;
  { const char *nome = (c && c->titulo[0]) ? c->titulo : "Reproduzindo";
    TxtLinha lt = txt_linha_corta(TXT_PLR_TITULO, nome, 255, 255, 255, 255,
                                  cw * 0.62f);
    hTit = (float)lt.h;
    yTit = yMetaBase - hTit;
    txt_desenhar_alpha(lt, cx, yTit, a); }

  // --- fileira de BOTOES: o transporte do aparelho --------------------------
  // Sem botoes redundantes de salto. O foco percorre so as acoes visiveis.
  {
    // .player-controls-row e space-between: o grupo de botoes a ESQUERDA, com
    // gap de 4px entre eles, e o rotulo de tempo empurrado para a direita por
    // margin-left:auto. Nao e o transporte centralizado do app da Apple.
    float passo = PLR_BTN_D + PLR_BTN_GAP;
    float x0    = cx + PLR_BTN_D * 0.5f;
    float cxs[PLR_NBTNS];
    for (int i=0;i<PLR_NBTNS;i++) cxs[i]=x0+i*passo;
    for (int i = 0; i < PLR_NBTNS - (epT > 0 ? 0 : 1); i++) {
      float f = focoB[i];
      int sel = (botao == i && !barraFoco);
      botaoCirculo(cxs[i], cyBotoes, f, a, sel);
      float lum = sel ? 0.13f : 0.94f;
      switch (i) {
        case PLR_PLAY:    iconePlayPause(cxs[i], cyBotoes, a, tocando, lum); break;
        case PLR_CC:      iconeLegendas(cxs[i], cyBotoes, a, lum); break;
        case PLR_ASPECTO: iconeAspecto(cxs[i], cyBotoes, a, lum); break;
        case PLR_FONTES: iconeArquivo(cxs[i],cyBotoes,a,lum,"fontes",44); break;
        case PLR_EPISODIOS: iconeArquivo(cxs[i],cyBotoes,a,lum,"episodios",44); break;
        default:          iconeAudio(cxs[i], cyBotoes, a, lum); break;
      }
    }
    if (!barraFoco) {
      const char *rotulos[]={"Reproduzir / pausar","Proporção","Legendas","Áudio","Fontes","Episódios"};
      TxtLinha label=txt_linha(TXT_PG_FIM,rotulos[botao],210,212,218,255);
      txt_desenhar_alpha(label,cxs[botao]-label.w*.5f,cyBotoes+PLR_BTN_D*.5f+10,a);
    }
  }

  // --- rotulo de tempo, na ponta direita da mesma fileira --------------------
  // Um rotulo so, "decorrido / total", como o #playerTimeLabel do web. Aqui
  // eram DOIS — decorrido a esquerda da barra e restante NEGATIVO a direita —
  // que e a convencao do app da Apple, nao a nossa. Centrado na vertical com os
  // circulos porque no web ele e um item de uma flex row com align-items:center.
  {
    char t1[24], t2[24], tudo[52];
    fmtTempo(t1, sizeof t1, posSeg, 0);
    fmtTempo(t2, sizeof t2, duracaoSeg, 0);
    snprintf(tudo, sizeof tudo, "%s / %s", t1, t2);
    { TxtLinha l = txt_linha(TXT_PLR_CORPO, tudo, 255, 255, 255, 230);
      txt_desenhar_alpha(l, cx + cw - l.w,
                         cyBotoes - (float)l.h * 0.5f, a * 0.9f); }
  }

  // Selos de formato no alto a direita. Vem do FLUXO, nao de constante: os
  // dois estavam fixos e anunciavam Dolby Vision em arquivo HDR10 e Atmos em
  // faixa estereo. Selo que mente e pior que selo ausente, porque e nele que o
  // dono confia para saber se pegou a versao boa.
  {
    const char *selos[3];
    int nSelos = 0;
    char res[16] = "";
    if (video_largura() >= 3840)      snprintf(res, sizeof res, "4K");
    else if (video_largura() >= 1920) snprintf(res, sizeof res, "HD");
    if (res[0]) selos[nSelos++] = res;
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
    if (video_tem_dolby_vision())                  selos[nSelos++] = "Dolby Vision";
    else if (!strcasecmp(video_hdr(), "HDR10"))    selos[nSelos++] = "HDR10";
    if (video_tem_atmos())        selos[nSelos++] = "Dolby Atmos";

    // RELOGIO e "Termina as", que sao o que o web poe neste canto
    // (.player-controls-top, playerScreen.js:5846). Os selos de qualidade sao
    // acrescimo do port e passam a ficar ABAIXO deles, nao no lugar.
    //
    //   .player-clock    26/600 branco 96%
    //   .player-ends-at  20/400 branco 78%, logo abaixo
    float yRel = PLR_PAD_Y + desce;
    {
      time_t agoraT = time(NULL);
      struct tm lt;
      char hora[8], fim[32];
      localtime_r(&agoraT, &lt);
      strftime(hora, sizeof hora, "%H:%M", &lt);
      { double falta = duracaoSeg - posSeg;
        time_t t2 = agoraT + (time_t)(falta > 0.0 ? falta : 0.0);
        struct tm lf; char h2[8];
        localtime_r(&t2, &lf);
        strftime(h2, sizeof h2, "%H:%M", &lf);
        snprintf(fim, sizeof fim, "Termina \xc3\xa0" "s %s", h2); }
      TxtLinha lh = txt_linha(TXT_PG_RELOGIO, hora, 255, 255, 255, 255);
      TxtLinha lf = txt_linha(TXT_PG_FIM, fim, 255, 255, 255, 255);
      txt_desenhar_alpha(lh, NV_TELA_W - PLR_PAD_X - lh.w, yRel, a * 0.96f);
      txt_desenhar_alpha(lf, NV_TELA_W - PLR_PAD_X - lf.w, yRel + lh.h + 2.0f,
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
      float t0 = (float)(agora - ultimoInput) / 1000.0f;
      for (i = 0; i < nSelos; i++) {
        float ts = anim_clamp((t0 - i * 0.09f) / 0.26f, 0.0f, 1.0f);
        float e  = 1.0f - (1.0f - ts) * (1.0f - ts);   // desaceleracao
        TxtLinha l = txt_linha(TXT_MINI, selos[i], 236, 237, 242, 255);
        if (e > 0.004f)
          txt_desenhar_alpha(l, NV_TELA_W - PLR_PAD_X - l.w,
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
    float tg = inicioImagem ? (float)(agora - inicioImagem) / 1000.0f : -1.0f;
    if (np > 0 && tg >= 0.0f && tg < PG_SEG_TOTAL) {
      float saida = anim_clamp((PG_SEG_TOTAL - tg) / PG_SEG_SAIDA, 0.0f, 1.0f);
      float lin = PG_LINHA_H, gap = PG_LINHA_GAP;
      float alt = np * lin + (np - 1) * gap;
      float y0 = PLR_PAD_Y;
      // A barra so cresce depois que a primeira linha entrou, senao ela aparece
      // sozinha apontando para o vazio.
      float eB = anim_clamp((tg - 0.10f) / 0.34f, 0.0f, 1.0f);
      eB = 1.0f - (1.0f - eB) * (1.0f - eB);
      { GfxRect barra = { PLR_PAD_X, y0, PG_BARRA_W, alt * eB };
        if (eB > 0.01f)
          gfx_cor(barra, 0.5f * (PG_BARRA_W / (alt * eB)),
                  PLR_FILL_C, PLR_FILL_C, PLR_FILL_C, entrada * saida); }
      float xt = PLR_PAD_X + PG_BARRA_W + PG_LISTA_PADX;
      for (int i = 0; i < np; i++) {
        float yl = y0 + i * (lin + gap);
        float ts = anim_clamp((tg - 0.18f - i * 0.10f) / 0.30f, 0.0f, 1.0f);
        float ee = 1.0f - (1.0f - ts) * (1.0f - ts);   // desaceleracao
        float ag = entrada * saida * ee;
        float dx = (1.0f - ee) * 18.0f;                // entra deslizando da esquerda
        TxtLinha lr, ls, lg;
        float cy, x;
        if (ag <= 0.004f) continue;
        lr = txt_linha(TXT_PG_ROTULO, parental_rotulo(i), 255, 255, 255, 255);
        ls = txt_linha(TXT_PG_GRAV, "\xc2\xb7", 255, 255, 255, 255);
        lg = txt_linha(TXT_PG_GRAV, parental_gravidade(i), 255, 255, 255, 255);
        cy = yl + (lin - lr.h) * 0.5f;
        x  = xt - dx;
        txt_desenhar_alpha(lr, x, cy, ag * 0.85f);  x += lr.w;
        txt_desenhar_alpha(ls, x, yl + (lin - ls.h) * 0.5f, ag * 0.40f); x += ls.w;
        txt_desenhar_alpha(lg, x, yl + (lin - lg.h) * 0.5f, ag * 0.50f);
      }
    }
  }
}
