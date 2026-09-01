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
#include "gfx.h"
#include "text.h"
#include "tex_cache.h"
#include "anim.h"
#include "layout.h"
#include "catalogo.h"
#include "trakt.h"
#include <stdio.h>
#include <string.h>
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
#define PLR_PAD_X         64.0f
#define PLR_PAD_Y         48.0f
#define PLR_BTN_D         96.0f
#define PLR_BTN_GAP        4.0f
// 6px e a altura de repouso. O web tem 10px para .player-progress-shell.focused,
// que aqui nao existe: o foco anda so pelos cinco botoes, a barra nunca o
// recebe. A constante nao entra porque constante que ninguem usa vira mentira
// na proxima leitura.
#define PLR_TRILHO_H       6.0f
#define PLR_TRILHO_R       3.0f
#define PLR_GAP_BARRA     12.0f   // meta -> barra
#define PLR_GAP_ROW       16.0f   // barra -> fileira de botoes
#define PLR_GRAD_BAIXO   200.0f
#define PLR_GRAD_TOPO    150.0f
// #f5f5f5 = --secondary-color, que e o que preenche a barra no web.
#define PLR_FILL_C      (245.0f / 255.0f)

#define PLR_ICONE_H       46.0f
// De quanto o bloco desliza para baixo quando escondido. Pequeno de proposito:
// o que faz o movimento ser lido nao e a distancia, e a mola somada ao fade.
#define PLR_DESLIZE       46.0f
// O veu virou os dois degrades do web (PLR_GRAD_TOPO/BAIXO). Ele existe para o
// texto ler sobre a imagem — sem ele, uma cena clara apaga o nome do titulo.

// A fileira de BOTOES, na ordem do foco. E o transporte do aparelho: retroceder
// 10s, pausar/retomar, avancar 10s — e no canto direito, onde ficam no app da
// Apple, as duas portas para a folha de audio e legenda.
enum { PLR_VOLTAR, PLR_PLAY, PLR_AVANCAR, PLR_CC, PLR_AUDIO, PLR_NBTNS };

static int   aberto = 0, saindo = 0, pediuSair = 0;
static int   idx = 0;
static int   tocando = 1;
// Botao em foco na fileira de transporte. Comeca no PLAY porque e a resposta
// que nove de cada dez aberturas quer: o dedo para no centro e o OK decide.
static int   botao = PLR_PLAY;
static int   visivel = 0;          // alvo dos controles (1 = em pe)
static float anim = 0.0f;          // 0..1 seguindo `visivel`, por mola
static float focoB[PLR_NBTNS];     // mola de foco de cada botao
static float entrada = 0.0f;       // 0..1 fade de abertura/fechamento da tela
static Uint32 ultimoInput = 0;
// AS DUAS VARIAVEIS DE MIDIA. Todo o resto do arquivo le so daqui — quando o
// video real entrar, sao elas que passam a ser preenchidas pelo decodificador.
static int   comVideo = 0;
static int   pedFaixas = 0;
static int   esperandoFonte = 0;   // aberto sem URL, esperando o addon responder
static float posSeg = 0.0f;
static float duracaoSeg = PLR_DUR_PADRAO;

static char linhaEp[220];          // "T1, E1 · <sinopse curta>", montada na abertura

static const CatItem *item(void) { return cat_item(idx); }

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

void player_abrir(int indiceCatalogo, const char *url) {
  int n = cat_n(); if (n < 1) n = 1;
  idx = ((indiceCatalogo % n) + n) % n;
  aberto = 1; saindo = 0; pediuSair = 0;
  tocando = 1; visivel = 0; anim = 0.0f; entrada = 0.0f;
  botao = PLR_PLAY;
  memset(focoB, 0, sizeof focoB);
  posSeg = 0.0f;
  ultimoInput = SDL_GetTicks();
  esperandoFonte = (url == NULL);
  comVideo = (url && *url && video_tocar(url));
  if (comVideo) video_janela(0, 0, (int)NV_TELA_W, (int)NV_TELA_H);

  const CatItem *c = item();
  float d = c ? duracaoDeMeta(c->meta) : 0.0f;
  duracaoSeg = d > 1.0f ? d : PLR_DUR_PADRAO;

  // A linha de cima e "S1, E3 · <sinopse curta>" na foto do aparelho. O
  // catalogo NAO tem lista de episodios — traz um titulo e uma sinopse — entao
  // o marcador de temporada/episodio fica fixo em T1,E1 e a sinopse e real.
  // Quando existir episodio de verdade, e este snprintf que muda.
  char curta[180];
  frasePrimeira(curta, sizeof curta, c ? c->sinopse : NULL, 120);
  int serie = c && strstr(c->meta, "temporada") != NULL;
  if (serie) snprintf(linhaEp, sizeof linhaEp, "T1, E1  \xc2\xb7  %s", curta);
  else       snprintf(linhaEp, sizeof linhaEp, "%s", curta);
}

int player_aberto(void)    { return aberto; }
int player_quer_sair(void) { return pediuSair; }
// So depois do loadCompleted. Antes disso o pipeline ainda nao pos nada no
// plano de hardware, e furar a superficie cedo trocava a arte por um retangulo
// PRETO enquanto o fluxo abria — que era o "clica em reproduzir e fica preto".
void player_definir_fonte(const char *url) {
  if (!aberto || !url || !*url) return;
  esperandoFonte = 0;
  comVideo = video_tocar(url);
  if (comVideo) video_janela(0, 0, (int)NV_TELA_W, (int)NV_TELA_H);
}

// Consome o pedido de abrir a folha de faixas: quem le, zera.
int  player_pediu_faixas(void) { int v = pedFaixas; pedFaixas = 0; return v; }

int  player_com_video(void) { return comVideo && video_pronto(); }

// Esta abrindo o fluxo: ha video pedido, mas ainda nao ha imagem.
int  player_carregando(void) { return esperandoFonte || (comVideo && !video_pronto()); }

void player_encerrar(void) {
  // Salvar ANTES de parar: video_parar descarrega o pipeline e a posicao some
  // junto. Titulo quase no fim conta como visto por inteiro — voltar a um card
  // marcando "2 min restantes" que na verdade acabou e pior que arredondar.
  if (comVideo && duracaoSeg > 1.0f) {
    float pos = posSeg >= duracaoSeg - 60.0f ? duracaoSeg : posSeg;
    const CatItem *ci = cat_item(idx);
    cat_salvar_progresso(idx, pos, duracaoSeg);
    // E tambem para o Trakt, que e de onde o "continue assistindo" vem: gravar
    // so aqui deixaria este app discordando dos outros aparelhos do dono.
    if (ci && ci->imdb[0]) trakt_marcar(ci->imdb, pos, duracaoSeg);
  }
  if (comVideo) video_parar();
  comVideo = 0; esperandoFonte = 0; aberto = 0; saindo = 0; pediuSair = 0;
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
  if (!visivel) {
    if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) {
      alternarTocando(); acordar(); return;
    }
    if (k == SDLK_UP || k == SDLK_DOWN || k == SDLK_LEFT || k == SDLK_RIGHT)
      acordar();
    return;
  }

  // CONTROLES EM PE: o foco anda pelos botoes e o OK aperta o botao em foco.
  if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) {
    switch (botao) {
      case PLR_PLAY:    alternarTocando(); break;
      case PLR_VOLTAR:  saltar(-1);        break;
      case PLR_AVANCAR: saltar(1);         break;
      default:          pedFaixas = 1;     break;   // CC e audio abrem a folha
    }
    acordar();
    return;
  }
  // CIMA abre a folha de audio e legenda — e o unico caminho sem cursor. No
  // aparelho e assim: pra cima revela legendas e audio, sempre, de qualquer
  // botao em que o foco esteja.
  if (k == SDLK_UP) { pedFaixas = 1; acordar(); return; }
  // Sem rotacao nas pontas: a fileira e curta e cabe inteira no olhar; dar a
  // volta no fim le como erro, nao como atalho.
  if (k == SDLK_LEFT  && botao > 0)          botao--;
  else if (k == SDLK_RIGHT && botao < PLR_NBTNS - 1) botao++;
  acordar();
}

void player_atualizar(float dt, Uint32 agora) {
  if (!aberto) return;

  entrada = anim_mola(entrada, saindo ? 0.0f : 1.0f, dt, NV_MOLA_TELA);
  if (saindo && entrada < 0.02f) { aberto = 0; saindo = 0; entrada = 0.0f; return; }

  // Havendo pipeline, posicao e duracao vem DELE; o dt so serve para as
  // animacoes. O relogio somado continua existindo para quando nao ha video
  // (no Mac, ou se a fonte falhar): sem ele a barra ficaria parada em zero e a
  // tela mentiria dizendo que nada acontece.
  if (comVideo && video_ativo()) {
    double d = video_duracao();
    posSeg = (float)video_pos();
    if (d > 1.0) duracaoSeg = (float)d;
    tocando = video_tocando();
  } else if (tocando) {
    posSeg += dt;
    if (posSeg >= duracaoSeg) { posSeg = duracaoSeg; tocando = 0; }
  }

  // Pausado, os controles ficam. Sumir com eles deixaria o usuario diante de um
  // quadro parado sem nenhuma pista de que foi ele quem pausou.
  if (visivel && tocando && agora - ultimoInput > PLR_ESCONDE_MS) visivel = 0;

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
// Os icones sao desenhados com as primitivas que ja existem, nunca com um modo
// de shader novo. Todos recebem a TINTA (0..1) porque a cor muda com o foco:
// botao em foco e circulo branco com glifo escuro; fora do foco, circulo
// translucido com glifo claro — a mesma gramatica das pílulas do aparelho.

static void iconeSalto(float cx, float cy, float a, int paraFrente, float lum) {
  const char *s = paraFrente ? "10\xc2\xbb" : "\xc2\xab" "10";
  int c = (int)(lum * 255.0f + 0.5f);
  TxtLinha l = txt_linha(TXT_CALLOUT, s, c, c, c, 255);
  txt_desenhar_alpha(l, cx - l.w * 0.5f, cy - l.h * 0.5f, a * 0.94f);
}

static void iconePlayPause(float cx, float cy, float a, int pausar, float lum) {
  int c = (int)(lum * 255.0f + 0.5f);
  float h = PLR_ICONE_H;
  if (pausar) {   // mostrando "pause" quer dizer que esta TOCANDO
    float w = h * 0.30f, g = h * 0.26f;
    GfxRect e1 = { cx - g * 0.5f - w, cy - h * 0.5f, w, h };
    GfxRect e2 = { cx + g * 0.5f,     cy - h * 0.5f, w, h };
    gfx_cor(e1, 0.22f, lum, lum, lum, a * 0.95f);
    gfx_cor(e2, 0.22f, lum, lum, lum, a * 0.95f);
    (void)c;
  } else {
    GfxRect tri = { cx - h * 0.30f, cy - h * 0.5f, h * 0.78f, h };
    gfx_rect(tri, 0, GFX_PLAY, 0, 0, 0, 0.0f, lum, lum, lum, a * 0.95f);
  }
}

// "CC" como glifo dentro do circulo — sem caixa interna, que duplicaria a
// moldura que o proprio circulo do botao ja da.
static void iconeLegendas(float cx, float cy, float a, float lum) {
  int c = (int)(lum * 255.0f + 0.5f);
  TxtLinha l = txt_linha(TXT_CAPTION2, "CC", c, c, c, 255);
  txt_desenhar_alpha(l, cx - l.w * 0.5f, cy - l.h * 0.5f, a * 0.94f);
}

// Equalizador de tres barras para a faixa de audio. Barras de alturas
// diferentes, senao le como "sinal" e nao como som.
static void iconeAudio(float cx, float cy, float a, float lum) {
  const float alt[3] = { 0.50f, 1.00f, 0.68f };
  float w = PLR_ICONE_H * 0.16f, g = PLR_ICONE_H * 0.20f;
  float x = cx - (w * 3 + g * 2) * 0.5f;
  for (int i = 0; i < 3; i++) {
    float h = PLR_ICONE_H * alt[i];
    GfxRect b = { x, cy - h * 0.5f, w, h };
    gfx_cor(b, 0.5f, lum, lum, lum, a * 0.94f);
    x += w + g;
  }
}

// Um botao circular do transporte: translucido quando solto, branco quando em
// foco, e o glifo sempre com o contraste certo contra o fundo dele.
static void botaoCirculo(float cx, float cy, float f, float a, int sel) {
  float d = PLR_BTN_D * (1.0f + 0.09f * f);
  GfxRect r = { cx - d * 0.5f, cy - d * 0.5f, d, d };
  if (sel) gfx_cor(r, 0.5f, 0.97f, 0.97f, 0.98f, 0.96f * a);
  else     gfx_cor(r, 0.5f, 0.05f, 0.05f, 0.06f, 0.42f * a);
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
    gfx_furo(tela);
  } else {
    const char *arte = (c && c->backdrop[0]) ? c->backdrop : NULL;
    GLuint tex = arte ? tex_obter(arte) : 0;
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
    for (k = 0; k < 3; k++) {
      float fase = agora / 1000.0f * 3.0f - k * 0.6f;
      float br = 0.45f + 0.55f * (0.5f + 0.5f * sinf(fase));
      GfxRect pt = { NV_TELA_W * 0.5f - 34 + k * 26, NV_TELA_H * 0.5f - 7, 14, 14 };
      gfx_cor(pt, 7.0f, 1, 1, 1, br * entrada);
    }
    { TxtLinha lc = txt_linha(TXT_CALLOUT, "Abrindo fonte", 236, 237, 242, 255);
      txt_desenhar_alpha(lc, NV_TELA_W * 0.5f - lc.w * 0.5f,
                         NV_TELA_H * 0.5f + 34, 0.85f * entrada); }
  }

  float a = anim * entrada;
  if (a <= 0.005f) return;   // tocando limpo: nada por cima da imagem

  // Dois degrades, como no web: .player-controls-gradient-top (150px, 0.7 -> 0)
  // e .player-controls-gradient-bottom (200px, 0 -> 0.8). O de baixo sustenta o
  // titulo e a barra; o de cima existe porque os selos e a classificacao ficam
  // no alto e sem ele sumiriam sobre cena clara. Ambos acompanham a animacao
  // dos controles: fixos, deixariam sombra permanente em toda cena.
  GfxRect veu = { 0, NV_TELA_H - PLR_GRAD_BAIXO, NV_TELA_W, PLR_GRAD_BAIXO };
  gfx_rect(veu, 0, GFX_VEU, 0, 0, 0, 0.0f, 0, 0, 0, 0.80f * a);
  { GfxRect topo = { 0, 0, NV_TELA_W, PLR_GRAD_TOPO };
    gfx_rect(topo, 0, GFX_VEU_TOPO, 0, 0, 0, 0.0f, 0, 0, 0, 0.70f * a); }

  // O bloco inteiro desliza junto: titulo, barra e icones sao UM objeto que
  // sobe. Animar cada linha por conta propria produz um escalonamento que o
  // aparelho nao tem.
  float desce = (1.0f - anim) * PLR_DESLIZE;

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
  float bx = PLR_PAD_X, bw = NV_TELA_W - PLR_PAD_X * 2;
  float frac = duracaoSeg > 0.0f ? anim_clamp(posSeg / duracaoSeg, 0.0f, 1.0f) : 0.0f;
  GfxRect trilho = { bx, yBarra, bw, PLR_TRILHO_H };
  GfxRect andado = { bx, yBarra, bw * frac, PLR_TRILHO_H };
  // rgba(255,255,255,0.3) do .player-progress-track.
  gfx_cor(trilho, PLR_TRILHO_R, 1, 1, 1, 0.30f * a);
  // O buffer do pipeline, entre o andado e o fim: e o que mostra que o video
  // esta a frente do relogio. Sem dado do pipeline o segmento nao existe —
  // inventar "quase todo carregado" seria pior que a barra simples. No web ele
  // e a MESMA cor do preenchimento a 0.35 (.player-progress-buffered).
  { float bufFrac = duracaoSeg > 0.0f ? anim_clamp(video_buffer_fim() / duracaoSeg, 0.0f, 1.0f) : 0.0f;
    if (bufFrac > frac + 0.004f) {
      GfxRect buf = { bx + bw * frac, yBarra, bw * (bufFrac - frac), PLR_TRILHO_H };
      gfx_cor(buf, PLR_TRILHO_R, PLR_FILL_C, PLR_FILL_C, PLR_FILL_C, 0.35f * a);
    } }
  if (andado.w > 1.0f)
    gfx_cor(andado, PLR_TRILHO_R, PLR_FILL_C, PLR_FILL_C, PLR_FILL_C, a);

  // --- nome do titulo: o LOGO quando o catalogo tem, texto como reserva ---
  // Mesma regra do resto do app: o app da Apple desenha a marca da producao, e
  // e ela que da identidade a tela. So sem logo o nome vira texto.
  // No web a coluna e titulo, DEPOIS subtitulo (.player-subtitle vem em seguida
  // no fluxo, com margin-top 2), e so entao a barra. Aqui estava ao contrario —
  // a linha do episodio ficava ACIMA do titulo, que e o arranjo do app da
  // Apple. Por isso o bloco e medido de baixo para cima: a base do subtitulo
  // encosta nos 12px acima da barra e o titulo sobe a partir dele.
  float hSub = 0.0f;
  if (linhaEp[0]) {
    TxtLinha lep = txt_linha(TXT_PLR_CORPO, linhaEp, 255, 255, 255, 230);
    hSub = (float)lep.h + 2.0f;
    txt_desenhar_alpha(lep, bx, yBarra - PLR_GAP_BARRA - (float)lep.h, a * 0.9f);
  }
  float yMetaBase = yBarra - PLR_GAP_BARRA - hSub;

  const char *arqLogo = (c && c->logo[0]) ? c->logo : NULL;
  GLuint texLogo = arqLogo ? tex_obter(arqLogo) : 0;
  float hTit, yTit;
  if (texLogo) {
    float asp = tex_aspecto(arqLogo);
    hTit = NV_LOGO_CAB_H;
    float w = asp > 0.0f ? hTit * asp : NV_LOGO_CAB_MAX_W;
    if (w > NV_LOGO_CAB_MAX_W) { w = NV_LOGO_CAB_MAX_W; hTit = asp > 0.0f ? w / asp : hTit; }
    yTit = yMetaBase - hTit;
    GfxRect r = { bx, yTit, w, hTit };
    gfx_tex_aspect_atual = 0.0f;   // o logo ja vem na proporcao certa
    gfx_rect(r, texLogo, GFX_TEXTO, 0, 0, 0, 0.0f, 1, 1, 1, a);
  } else {
    TxtLinha lt = txt_linha(TXT_PLR_TITULO, (c && c->titulo[0]) ? c->titulo : "Reproduzindo",
                            255, 255, 255, 255);
    hTit = (float)lt.h;
    yTit = yMetaBase - hTit;
    txt_desenhar_alpha(lt, bx, yTit, a);
  }

  // --- fileira de BOTOES: o transporte do aparelho --------------------------
  // Tres circulos centrados («10, pausar/retomar, 10») e, nos cantos onde o
  // app da Apple os poe, CC e audio. O foco anda pelos cinco; o glifo troca de
  // tinta com o foco, e o circulo sobe 9% de escala na mola.
  {
    // .player-controls-row e space-between: o grupo de botoes a ESQUERDA, com
    // gap de 4px entre eles, e o rotulo de tempo empurrado para a direita por
    // margin-left:auto. Nao e o transporte centralizado do app da Apple.
    float passo = PLR_BTN_D + PLR_BTN_GAP;
    float x0    = bx + PLR_BTN_D * 0.5f;
    const float cxs[PLR_NBTNS] = { x0, x0 + passo, x0 + passo * 2,
                                   x0 + passo * 3, x0 + passo * 4 };
    for (int i = 0; i < PLR_NBTNS; i++) {
      float f = focoB[i];
      int sel = (botao == i);
      botaoCirculo(cxs[i], cyBotoes, f, a, sel);
      float lum = sel ? 0.13f : 0.94f;
      switch (i) {
        case PLR_VOLTAR:  iconeSalto(cxs[i], cyBotoes, a, 0, lum); break;
        case PLR_PLAY:    iconePlayPause(cxs[i], cyBotoes, a, tocando, lum); break;
        case PLR_AVANCAR: iconeSalto(cxs[i], cyBotoes, a, 1, lum); break;
        case PLR_CC:      iconeLegendas(cxs[i], cyBotoes, a, lum); break;
        default:          iconeAudio(cxs[i], cyBotoes, a, lum); break;
      }
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
      txt_desenhar_alpha(l, bx + bw - l.w,
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
    if (video_tem_dolby_vision()) selos[nSelos++] = "Dolby Vision";
    if (video_tem_atmos())        selos[nSelos++] = "Dolby Atmos";
    { float sy = PLR_PAD_Y + desce;
      int i;
      for (i = 0; i < nSelos; i++) {
        TxtLinha l = txt_linha(TXT_MINI, selos[i], 236, 237, 242, 255);
        txt_desenhar_alpha(l, NV_TELA_W - PLR_PAD_X - l.w, sy, a * 0.85f);
        sy += l.h + 6.0f;
      } }
  }

  // Classificacao com o motivo ao lado, tambem conferido na foto: o badge
  // sozinho nao diz por que, e no aparelho ele vem sempre acompanhado.
  if (c && c->classificacao[0]) {
    char cl[8]; snprintf(cl, sizeof cl, "A%s", c->classificacao);
    TxtLinha lb = txt_linha(TXT_CAPTION2, cl, 255, 255, 255, 255);
    float by = PLR_PAD_Y + desce;
    GfxRect bg = { PLR_PAD_X, by, lb.w + 18, lb.h + 8 };
    gfx_cor(bg, 0.16f, 0.85f, 0.36f, 0.10f, 0.95f * a);
    txt_desenhar_alpha(lb, bg.x + 9, by + 4, a);
    // O motivo vem do genero do titulo — "Violencia" fixo aparecia em comedia.
    if (c->genero[0]) {
      const char *g = strrchr(c->genero, '\xb7');
      const char *rot = g ? g + 1 : c->genero;
      while (*rot == ' ') rot++;
      { TxtLinha lm = txt_linha(TXT_MINI, rot, 226, 228, 236, 255);
        txt_desenhar_alpha(lm, bg.x + bg.w + 12, by + (bg.h - lm.h) * 0.5f, a * 0.85f); }
    }
  }
}
