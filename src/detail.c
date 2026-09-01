// Detalhe do titulo, no formato do app da Apple (conferido em dois videos do
// aparelho, 2026-08-30). Sao TRES comportamentos, e cada um deles eu tinha
// errado antes de ver o video:
//
//   1. O detalhe nao e uma tela: e um CARTAO que vem para a frente, com margem
//      por onde a home aparece atras. Ele entra com um leve estouro de escala —
//      a sensacao que o dono descreveu como "sair da TV".
//   2. Os titulos VIZINHOS existem como cartoes ao lado, cortados pelas bordas.
//      Esquerda/direita troca de titulo deslizando o carrossel; nao ha volta a
//      home no meio do caminho.
//   3. Descer NAO rola a pagina: ESTICA o cartao para a tela inteira. A arte
//      perde os metadados sobrepostos, vira fundo, e entram as secoes.
#include "detail.h"
#include "streams.h"
#include "descoberta.h"
#include "gfx.h"
#include "text.h"
#include "tex_cache.h"
#include "focus.h"
#include "anim.h"
#include "layout.h"
#include "catalogo.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

#define N_SUG      8
#define N_SECOES   7
// 8, nao 3. Era um numero de exemplo: FROM tem 4 temporadas e a quarta nao
// cabia, entao a serie parecia ter menos do que tem. 8 e o teto do vetor de
// animacoes (N_SUG), que e o limite real.
// Posicao da fileira de similares no vetor SECOES. Fica como constante porque
// o desenho precisa distinguir essa fileira da de trailers, e as duas sao
// SEC_CARDS.
#define SEC_SIMILARES 4

#define N_TEMPORADAS 8
#define N_ELENCO   6

static HomeItem item;
static int  aberto = 0, saindo = 0;
static int  idx = 0;                 // titulo atual dentro do acervo
static float t = 0.0f;               // 0 = card na home, 1 = cartao aberto
// Tres niveis, medidos no app da Apple: o primeiro DESCER nao abre a pagina —
// ele so tira a moldura e leva a arte a tela cheia. So o segundo traz os cards
// e desfoca o fundo. Juntar os dois num passo so foi o que fez os cards
// parecerem "aparecer por cima" da foto em vez de continuar o movimento.
static int  nivel = 0;
static int  botao = 0;      // botao em foco quando o cartao esta em tela cheia
static int  pedReproduzir = 0, pedMarcar = 0, pedFontes = 0;
// Instante em que o OK foi pressionado. Segurar abre a lista de fontes; um
// toque toca no automatico. Sem guardar o instante nao ha como distinguir os
// dois, porque o D-pad so avisa "desceu" e "subiu".
static Uint32 okDesceEm = 0;               // 0 cartao, 1 tela cheia, 2 pagina
static float pg = 0.0f;              // 0..1: tela cheia -> pagina com cards
static Foco foco;                    // so vale no estado expandido
static float animFoco[N_SECOES][N_SUG];  // N_SUG e o maior n de qualquer secao
static float scrollSec[N_SECOES];
static float scrollY = 0.0f;
static const char *arteBorrada = NULL;  // de quem e o borrao que esta no alvo
// Qual temporada esta escolhida. Diferente do foco: a pilula da temporada
// escolhida continua la depois que o foco desce para os episodios — no app da
// Apple da para ver as duas coisas ao mesmo tempo.
static int temporada = 0;

static const char *TITULOS[] = {
  "Eternidade", "Falando a Real", "Ruptura", "Silo", "Ted Lasso",
  "Foundation", "For All Mankind", "Servant", "Invasao", "Shrinking"
};
static const char *GENEROS[] = {
  "Filme  \xc2\xb7  Romance  \xc2\xb7  Comedia",
  "Programa de TV  \xc2\xb7  Comedia",
  "Programa de TV  \xc2\xb7  Suspense",
  "Programa de TV  \xc2\xb7  Ficcao cientifica"
};
static const char *FICHAS[] = {
  "2025  \xc2\xb7  1 h 54 min", "2025  \xc2\xb7  T3, 10 episodios",
  "2024  \xc2\xb7  T2, 10 episodios", "2025  \xc2\xb7  1 h 38 min"
};
// A pagina do titulo nao e uma pilha de fileiras iguais: cada secao tem forma
// propria. Abas de temporada sao pills; episodio e um card alto com miniatura e
// texto embaixo; elenco sao circulos. Desenhar tudo como card 16:9 foi o que
// deixou a pagina parecendo uma home dentro do detalhe.
typedef enum { SEC_ABAS, SEC_EPISODIOS, SEC_CARDS, SEC_ELENCO, SEC_ONDE, SEC_SOBRE } TipoSecao;
// Quantos itens a secao mostra AGORA. So a de episodios varia: uma serie de 6
// episodios listando 8 repetiria os dois primeiros, e o vetor fixo de exemplo
// esconderia que a lista real acabou.
static int secaoN(int r);
static int temporadaEm(int c);

// --- onde assistir: tiers de verdade, na ordem do app da Apple ---------------
// Assinatura primeiro, aluguel, compra. Cada tier so existe com nome do
// provedor; sem dado, o card NAO aparece e a secao encolhe.
static const char *ONDE_SUB[3] = { "Assinatura", "Alugar", "Comprar" };

static int ondeContar(const CatItem *ci) {
  const char *nomes[3] = { ci->provNome, ci->alugNome, ci->compNome };
  int n = 0;
  for (int k = 0; k < 3 && n < 2; k++) if (nomes[k][0]) n++;
  return n;
}
static int ondeCard(const CatItem *ci, int c, const char **nome, const char **sub,
                    const char **logo) {
  const char *nomes[3] = { ci->provNome, ci->alugNome, ci->compNome };
  const char *logos[3] = { ci->provLogo, ci->alugLogo, ci->compLogo };
  for (int k = 0; k < 3; k++) {
    if (!nomes[k][0]) continue;
    if (c == 0) { *nome = nomes[k]; *sub = ONDE_SUB[k]; *logo = logos[k]; return 1; }
    c--;
  }
  return 0;
}

static const struct { const char *titulo; TipoSecao tipo; int n; } SECOES[N_SECOES] = {
  { NULL,                      SEC_ABAS,      N_TEMPORADAS },
  { NULL,                      SEC_EPISODIOS, N_SUG },
  { "Trailers",                SEC_CARDS,     N_SUG },
  { "Elenco e equipe",         SEC_ELENCO,    N_ELENCO },
  { "Voce tambem pode gostar", SEC_CARDS,     N_SUG },   // indice SEC_SIMILARES
  { "Como assistir",           SEC_ONDE,      2 },
  { "Sobre",                   SEC_SOBRE,     1 },
};
static const char *SOBRE[] = {
  "Uma serie de comedia dramatica sobre luto, amizade e os limites",
  "do que um terapeuta pode fazer quando decide dizer a verdade.",
  "Vencedora de tres premios da critica.",
};
static int secaoN(int r) {
  if (r < 0 || r >= N_SECOES) return 0;
  if (SECOES[r].tipo == SEC_EPISODIOS) {
    int q = cat_n_episodios(idx);
    if (q > 0) return q < N_SUG ? q : N_SUG;   // N_SUG e o teto das animacoes
  }
  if (SECOES[r].tipo == SEC_ELENCO) {
    const CatItem *ce = cat_item(idx);
    if (ce && ce->nElenco > 0) return ce->nElenco;
  }
  if (SECOES[r].tipo == SEC_ABAS) {
    // Filme nao tem temporada: a secao some em vez de mostrar abas que nao
    // levam a lugar nenhum.
    const CatItem *ci = cat_item(idx);
    if (ci && ci->tipo[0] && strcmp(ci->tipo, "series")) return 0;
    if (ci && ci->nTemporadas > 0)
      return ci->nTemporadas < N_TEMPORADAS ? ci->nTemporadas : N_TEMPORADAS;
  }
  if (SECOES[r].tipo == SEC_ONDE) {
    const CatItem *ci = cat_item(idx);
    // "Como assistir" sem dado nenhum NAO existe: um titulo vazio seria uma
    // promessa sem card. E com so um tier, e UM card — nao completo o par com
    // o "Comprar R$ 29,90" inventado que estava aqui.
    return ci ? ondeContar(ci) : 0;
  }
  return SECOES[r].n;
}

static const char *EPISODIOS[][3] = {
  { "Cara ou coroa",        "38 min", "27/01/2023" },
  { "Fortaleza da solidao", "32 min", "27/01/2023" },
  { "Quinze minutos",       "31 min", "03/02/2023" },
  { "Batatas",              "33 min", "10/02/2023" },
  { "Pai ausente",          "29 min", "17/02/2023" },
  { "Boop",                 "34 min", "24/02/2023" },
  { "Terapia de choque",    "30 min", "03/03/2023" },
  { "Depois da tempestade", "36 min", "10/03/2023" },
};
static const char *SIN_EP =
  "Jimmy, um terapeuta em luto pela esposa, assume uma abordagem mais "
  "assertiva com os pacientes, ajudando-os enquanto tenta se recompor.";
static const char *ELENCO[][2] = {
  { "Jason Segel",    "Jimmy" },  { "Harrison Ford",  "Dr. Paul Rhoades" },
  { "Jessica Williams","Gaby" },  { "Christa Miller", "Liz" },
  { "Luke Tennie",    "Sean" },   { "Michael Urie",   "Brian" },
};

static float alturaSecao(int r);
static float larguraSecao(TipoSecao t);
static float passoSecao(TipoSecao t);

// Texto real quando ha catalogo; as listas fixas ficam como reserva para quando
// o app roda so com uma pasta de imagens solta.
static const char *tituloDe(int i) {
  const CatItem *c = cat_item(i);
  return (c && c->titulo[0]) ? c->titulo : TITULOS[((i % 10) + 10) % 10];
}
static const char *generoDe(int i) {
  const CatItem *c = cat_item(i);
  return (c && c->genero[0]) ? c->genero : GENEROS[((i % 4) + 4) % 4];
}
static const char *fichaDe(int i) {
  const CatItem *c = cat_item(i);
  return (c && c->meta[0]) ? c->meta : FICHAS[((i % 4) + 4) % 4];
}
static const char *sinopseDe(int i) {
  const CatItem *c = cat_item(i);
  return (c && c->sinopse[0]) ? c->sinopse : NULL;
}
static const char *logoDe(int i) {
  const CatItem *c = cat_item(i);
  return (c && c->logo[0]) ? c->logo : NULL;
}
static const char *arteDe(int i) {
  const CatItem *c = cat_item(i);
  if (c && c->backdrop[0]) return c->backdrop;
  int n = home_n_artes();
  return n ? home_arte(((i % n) + n) % n) : NULL;
}

static float suave(float x) {
  x = anim_clamp(x, 0.0f, 1.0f);
  return 1.0f - (1.0f - x) * (1.0f - x) * (1.0f - x);
}
static float fase2(void) { return suave((t - 0.45f) / 0.55f); }

void detail_abrir(const HomeItem *it) {
  item = *it;
  aberto = 1; saindo = 0; nivel = 0; botao = 0; arteBorrada = NULL;
  t = 0.0f; pg = 0.0f; scrollY = 0.0f;
  // O titulo aberto e o do card, nao o primeiro do catalogo. Estava fixo em 0,
  // e por isso todo filme e serie abria a mesma pagina.
  idx = it->indice;
  // A aba marcada tem de ser a da temporada que os episodios trazem. Comecando
  // sempre em 0, FROM abria mostrando os episodios da 4 com "Temporada 1"
  // aceso — o rotulo desmentia a lista logo abaixo.
  temporada = 0;
  { const CatEp *e0 = cat_episodio(idx, 0);
    const CatItem *ci0 = cat_item(idx);
    if (e0 && ci0) {
      int k;
      for (k = 0; k < ci0->nTemporadas; k++)
        if (ci0->temporadas[k] == e0->temporada) { temporada = k; break; }
    } }
  int cols[N_SECOES]; for (int i = 0; i < N_SECOES; i++) cols[i] = secaoN(i);
  focus_iniciar(&foco, N_SECOES, cols);
  memset(animFoco, 0, sizeof animFoco);
  memset(scrollSec, 0, sizeof scrollSec);
}

int detail_aberto(void) { return aberto; }

int detail_assentado(void) {
  return aberto && !saindo && t > 0.985f && nivel == 0;
}

int detail_cobre_tela(void) {
  // O backdrop e FULL-BLEED: assim que ele termina de crescer, nao sobra um
  // pixel da tela anterior. Desenhar a home por baixo disso custava um quadro
  // inteiro de preenchimento a toa — medido em 42 ms no pior quadro.
  return aberto && suave(t) > 0.995f;
}

void detail_evento(const SDL_Event *e) {
  if (saindo) return;

  // O OK so decide o que fazer quando SOBE: ate la nao se sabe se foi toque ou
  // pressao longa.
  if (e->type == SDL_KEYDOWN && (e->key.keysym.sym == SDLK_RETURN ||
                                 e->key.keysym.sym == SDLK_KP_ENTER)) {
    if (!okDesceEm) okDesceEm = SDL_GetTicks();
    return;
  }
  if (e->type == SDL_KEYUP && (e->key.keysym.sym == SDLK_RETURN ||
                               e->key.keysym.sym == SDLK_KP_ENTER)) {
    Uint32 dur = okDesceEm ? SDL_GetTicks() - okDesceEm : 0;
    okDesceEm = 0;
    if (nivel == 0 && botao == 0) {
      if (dur >= NV_HOLD_MS) pedFontes = 1; else pedReproduzir = 1;
    } else if (nivel == 0 && botao == 1) {
      pedMarcar = 1;
    } else if (nivel == 0 && botao == 2) {
      // O botao "..." e o outro caminho para a lista de fontes, para quem nao
      // descobre que dava para segurar.
      pedFontes = 1;
    } else if (nivel >= 1 && SECOES[foco.fileira].tipo == SEC_ABAS) {
      // Trocar de aba BUSCA a temporada. Antes so mudava o realce e a lista
      // continuava a mesma, o que fazia a aba parecer quebrada.
      temporada = foco.coluna;
      desc_episodios(idx, temporadaEm(temporada));
    }
    return;
  }

  if (e->type != SDL_KEYDOWN) return;
  SDL_Keycode k = e->key.keysym.sym;

  // Varias teclas contam como "voltar": no aparelho e o Back do controle, e no
  // teclado cada um alcanca uma diferente — Esc, Delete ou a seta de apagar.
  if (k == SDLK_ESCAPE || k == SDLK_AC_BACK || k == SDLK_BACKSPACE ||
      k == SDLK_DELETE) {
    // Back desfaz UM nivel por vez; so na base ele volta para a home. Fechar
    // direto de dentro da pagina perderia o lugar.
    if (nivel > 0) nivel--; else saindo = 1;
    return;
  }
  // Dois niveis, nao tres. O nivel intermediario "cartao vira tela cheia" so
  // fazia sentido enquanto havia cartao: no web a tela JA nasce cheia e com o
  // "Reproduzir" em foco, entao o primeiro DESCER tem de levar direto as
  // secoes. E LEFT/RIGHT anda entre os botoes desde o comeco — o carrossel de
  // titulos vizinhos era invencao do port, o web nao tem.
  if (nivel == 0) {
    if (k == SDLK_DOWN)       nivel = 1;
    else if (k == SDLK_RIGHT) { if (botao < 2) botao++; }
    else if (k == SDLK_LEFT)  { if (botao > 0) botao--; }
    return;
  }
  if (k == SDLK_RIGHT)      focus_mover(&foco, 1, 0);
  else if (k == SDLK_LEFT)  focus_mover(&foco, -1, 0);
  else if (k == SDLK_DOWN)  focus_mover(&foco, 0, 1);
  else if (k == SDLK_UP)    { if (!focus_mover(&foco, 0, -1)) nivel = 0; }
}

void detail_atualizar(float dt, Uint32 agora) {
  (void)agora;
  if (!aberto) return;
  t  = anim_mola(t,  saindo ? 0.0f : 1.0f, dt, NV_MOLA_TELA);
  // Rigidez propria, e nao a de troca de tela: o web leva 0.8s para apagar o
  // backdrop (cubic-bezier .4,0,.2,1), e a mola de NV_MOLA_TELA assenta em
  // ~330ms. exp(-3.8*0.8) = 0.05, ou seja 95% do caminho em 800ms.
  pg = anim_mola(pg, nivel >= 1 ? 1.0f : 0.0f, dt, NV_MOLA_PAGINA);
  if (saindo && t < 0.02f) { aberto = 0; saindo = 0; t = 0.0f; return; }

  for (int r = 0; r < N_SECOES; r++) {
    for (int c = 0; c < secaoN(r); c++) {
      float alvo = (nivel >= 1 && focus_indice(&foco, r, c)) ? 1.0f : 0.0f;
      animFoco[r][c] = anim_mola(animFoco[r][c], alvo, dt,
                                 alvo > animFoco[r][c] ? NV_MOLA_FOCO : NV_MOLA_DESFOCO);
    }
    if (r == foco.fileira) {
      float passo = passoSecao(SECOES[r].tipo);
      float alvo = foco.coluna * passo - larguraSecao(SECOES[r].tipo) * 0.25f;
      if (alvo < 0) alvo = 0;
      scrollSec[r] = anim_mola(scrollSec[r], alvo, dt, NV_MOLA_SCROLL);
    }
  }
  // Rola so o necessario para a secao focada caber na area util. Alinhar a
  // secao focada ao topo, como estava, empurrava as abas para fora da tela
  // assim que o foco descia para os episodios — no app da Apple elas continuam
  // visiveis, porque ele so rola quando o conteudo nao cabe mais.
  float topoSecao = NV_PG_TOPO + 42.0f + NV_PG_TIT_ABAS;
  for (int r = 0; r < foco.fileira; r++)
    topoSecao += alturaSecao(r) +
                 (((r + 1 < N_SECOES) && SECOES[r + 1].titulo) ? NV_PG_ENTRE_SEC : 26.0f);
  float baseSecao = topoSecao + alturaSecao(foco.fileira);
  float limiteTopo = NV_PG_TOPO + 78.0f;      // abaixo do cabecalho fixo
  float alvoY = scrollY;
  if (baseSecao - alvoY > NV_TELA_H - 40.0f) alvoY = baseSecao - (NV_TELA_H - 40.0f);
  if (topoSecao - alvoY < limiteTopo)        alvoY = topoSecao - limiteTopo;
  if (alvoY < 0.0f) alvoY = 0.0f;
  scrollY = anim_mola(scrollY, nivel >= 1 ? alvoY : 0.0f, dt, NV_MOLA_SCROLL);
}

// Bloco de conteudo da tela de detalhe, no layout do APP WEB.
//
// Nada aqui e sobreposicao num cartao: a tela e full-bleed, a coluna comeca em
// x=72 e a pilha e ancorada na BASE (`.detail-hero-section` e um flex column
// com `justify-content: flex-end`). Empilhar de cima para baixo, como a versao
// anterior fazia, faz o bloco inteiro subir e descer conforme o tamanho da
// sinopse — no web ele fica preso na base e so o topo se move.
//
// A ordem, medida: logo -> botoes -> "Diretor: ..." -> sinopse -> generos e ano
// -> duracao. Nao ha badge de classificacao, nem nota, nem selos de formato,
// nem coluna de creditos a direita: esses vieram do app da Apple TV.
static void desenhaBotao(GfxRect r, const char *rot, int icone, int focado, float a) {
  // Foco no web NAO e escala nem sombra: e um anel branco de 4px por fora
  // (box-shadow 0 0 0 4px #fff) e a troca de cor do circulo. Medido nas duas
  // classes; o `transform` continua `none` nos dois estados.
  if (focado) {
    GfxRect anel = { r.x - NV_DETW_ANEL, r.y - NV_DETW_ANEL,
                     r.w + NV_DETW_ANEL * 2, r.h + NV_DETW_ANEL * 2 };
    gfx_cor(anel, NV_RAIO_PILL, 1, 1, 1, a);
  }
  int circular = (rot == NULL);
  if (circular) {
    // #222 sem foco, #f5f5f5 com foco.
    float lum = focado ? 0.961f : 0.133f;
    gfx_cor(r, NV_RAIO_PILL, lum, lum, lum, a);
    // Sem glifo: o "+" e o "..." sao desenhados, porque a familia embarcada
    // nao garante os simbolos e um quadrado no lugar e pior que nada.
    float ic = focado ? 0.067f : 1.0f;   // #111 com foco, branco sem
    float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
    if (icone == 1) {                    // "+"
      // O raio do SDF e uma fracao da ALTURA, nao do menor lado: num retangulo
      // 5x44 pedir 0.5 faz `b.x` ficar negativo e a forma colapsa — foi o que
      // deixou a barra vertical do "+" reduzida a um toco na primeira captura.
      // O teto e 0.5 * (w/h) quando o retangulo e mais alto que largo.
      GfxRect h = { cx - 22, cy - 2.5f, 44, 5 };
      GfxRect v = { cx - 2.5f, cy - 22, 5, 44 };
      gfx_cor(h, 0.5f, ic, ic, ic, a);
      gfx_cor(v, 0.5f * (v.w / v.h), ic, ic, ic, a);
    } else {                             // "..."
      for (int k = -1; k <= 1; k++) {
        GfxRect p = { cx + k * 15.0f - 3.5f, cy - 3.5f, 7, 7 };
        gfx_cor(p, 0.5f, ic, ic, ic, a);
      }
    }
    return;
  }
  // Primario: branco com texto preto nos DOIS estados — o foco so acrescenta o
  // anel. Isto foi conferido tirando a classe `focused` no web: a cor nao muda.
  gfx_cor(r, NV_RAIO_PILL, 1, 1, 1, a);
  TxtLinha l = txt_linha(TXT_DET_BOTAO, rot, 0, 0, 0, 255);
  float x = r.x + NV_DETW_BTN_PADX;
  GfxRect tri = { x + NV_DETW_BTN_ICONE * 0.16f,
                  r.y + (r.h - NV_DETW_BTN_ICONE) * 0.5f,
                  NV_DETW_BTN_ICONE * 0.72f, NV_DETW_BTN_ICONE * 0.84f };
  gfx_rect(tri, 0, GFX_PLAY, 0, 0, 0, 0.0f, 0, 0, 0, a);
  txt_desenhar_alpha(l, x + NV_DETW_BTN_ICONE + NV_DETW_BTN_GAPI,
                     r.y + (r.h - l.h) * 0.5f, a);
}

// Largura do botao primario: padding 48 + icone 36 + gap 16 + texto. Medido
// 298 para "Reproduzir"; a conta e a do CSS, entao um rotulo maior cresce a
// pilula em vez de estourar por baixo do texto.
static float larguraPrimario(const char *rot) {
  TxtLinha l = txt_linha(TXT_DET_BOTAO, rot, 0, 0, 0, 255);
  return NV_DETW_BTN_PADX * 2 + NV_DETW_BTN_ICONE + NV_DETW_BTN_GAPI + l.w;
}

// Ano solto do campo `meta` ("2025 · 1 h 54 min" -> "2025" e "1 h 54 min").
// O web tem os dois campos separados; aqui eles chegam numa linha so, e cortar
// no primeiro separador e o que permite empurrar o ano para a direita como la.
static void partirMeta(const char *meta, char *ano, size_t na, char *resto, size_t nr) {
  ano[0] = 0; resto[0] = 0;
  if (!meta || !meta[0]) return;
  const char *sep = strstr(meta, "\xc2\xb7");        // U+00B7
  if (!sep) { snprintf(ano, na, "%s", meta); return; }
  size_t n = (size_t)(sep - meta);
  while (n && (meta[n-1] == ' ')) n--;
  if (n >= na) n = na - 1;
  memcpy(ano, meta, n); ano[n] = 0;
  const char *r = sep + 2;
  while (*r == ' ') r++;
  snprintf(resto, nr, "%s", r);
}

static void heroWeb(float a) {
  if (a <= 0.005f) return;
  const CatItem *ci = cat_item(idx);

  char ano[32], dur[64];
  partirMeta(fichaDe(idx), ano, sizeof ano, dur, sizeof dur);

  char sup[192] = "";
  if (ci && ci->direcao[0]) snprintf(sup, sizeof sup, "Diretor: %s", ci->direcao);

  const char *sin = sinopseDe(idx);

  // --- empilhamento de BAIXO para cima, como o flex-end do web ---------------
  float yMeta2 = NV_DETW_BASE - 45.0f;                       // 1003
  float yMeta1 = yMeta2 - NV_DETW_META_GAP - 49.0f;          // 928
  float hSin = 0.0f;
  if (sin) hSin = txt_bloco(TXT_DET_SIN, sin, 255, 255, 255, -1.0f, 0.0f,
                            NV_DETW_TEXTO_W, NV_DETW_LD_SIN, 0.0f,
                            NV_DETW_SIN_LINHAS);
  float ySin = yMeta1 - NV_DETW_GAP_SIN - hSin;              // 787
  float ySup = sup[0] ? ySin - NV_DETW_GAP_SUP - NV_DETW_LD_SUP : ySin;  // 727
  // A linha de retomada so ocupa espaco quando existe; sem ela a pilha e a
  // mesma de um titulo nunca aberto.
  float temRetom = (ci && ci->progresso > 0) ? 1.0f : 0.0f;
  float yRetom = ySup - NV_DETW_GAP_SUP - NV_DETW_RETOM_H;
  float yAcoes = (temRetom ? yRetom - NV_DETW_GAP_RETOM
                           : ySup - NV_DETW_GAP_ACOES) - NV_DETW_ACOES_H;

  // Sobe alguns pixels enquanto entra: continua o movimento da arte em vez de
  // aparecer pronto no lugar.
  float sobe = (1.0f - a) * 26.0f;
  yMeta2 += sobe; yMeta1 += sobe; ySin += sobe; ySup += sobe;
  yRetom += sobe; yAcoes += sobe;

  // --- logo -----------------------------------------------------------------
  const char *arqLogo = logoDe(idx);
  GLuint texLogo = arqLogo ? tex_obter(arqLogo) : 0;
  if (texLogo) {
    float asp = tex_aspecto(arqLogo);
    if (asp <= 0.0f) asp = 2.5f;
    float h = NV_DETW_LOGO_H, w = h * asp;
    if (w > NV_DETW_LOGO_MAXW) { w = NV_DETW_LOGO_MAXW; h = w / asp; }
    GfxRect r = { NV_DETW_X, yAcoes - NV_DETW_LOGO_GAP - h, w, h };
    gfx_tex_aspect_atual = 0.0f;   // o logo ja vem na proporcao certa
    gfx_rect(r, texLogo, GFX_TEXTO, 0, 0, 0, 0.0f, 1, 1, 1, a);
  } else {
    // Sem logo, o NOME. A altura da caixa continua sendo a do logo, para que a
    // linha de botoes nao pule de lugar entre um titulo com logo e outro sem.
    TxtLinha t2 = txt_linha_corta(TXT_TITULO1, tituloDe(idx), 255, 255, 255, 255,
                                  NV_DETW_LOGO_MAXW);
    txt_desenhar_alpha(t2, NV_DETW_X,
                       yAcoes - NV_DETW_LOGO_GAP - NV_DETW_LOGO_H
                              + (NV_DETW_LOGO_H - t2.h) * 0.5f, a);
  }

  // --- botoes ---------------------------------------------------------------
  // Em FLUXO, com 63px entre vizinhos. As posicoes 439/586/734 que eu tinha
  // fixado nao sao constantes do desenho: sao o resultado dessa conta com o
  // rotulo "Reproduzir". Num titulo em progresso o rotulo vira "Retomar T2E3",
  // o primario passa de 298 para 334, e tudo a direita anda 36px — medido na
  // sessao logada, com Silo.
  //
  // O web tem tres circulares (lista, "nao me interessa", trailer). Aqui sao
  // DOIS: lista e fontes. O terceiro nao entra porque nao existe reprodutor de
  // trailer neste app, e um botao que nao faz nada e pior que a ausencia dele.
  float yBtn  = yAcoes + 6.0f;
  float yCirc = yAcoes + 12.0f;
  // "Retomar" e o rotulo do web quando ha progresso; nele o icone continua o
  // mesmo triangulo, so o texto muda.
  const char *rot = (ci && ci->progresso > 0) ? "Retomar" : "Reproduzir";
  float bx = NV_DETW_X + 6.0f;
  GfxRect rp = { bx, yBtn, larguraPrimario(rot), NV_DETW_BTN_H };
  desenhaBotao(rp, rot, 0, nivel == 0 && botao == 0, a);
  bx += rp.w + NV_DETW_BTN_GAP;
  for (int k = 0; k < 2; k++) {
    GfxRect rc = { bx, yCirc, NV_DETW_CIRC, NV_DETW_CIRC };
    desenhaBotao(rc, NULL, k + 1, nivel == 0 && botao == k + 1, a);
    bx += NV_DETW_CIRC + NV_DETW_BTN_GAP;
  }

  // --- linha de retomada ----------------------------------------------------
  // So existe com progresso, e e ela que empurra tudo o que vem acima. Medida
  // no web logado: "Retomada disponivel · 45% · Episodio S2E3 · 30m restantes",
  // 22.66/400 em rgba(255,255,255,0.82).
  if (ci && ci->progresso > 0) {
    char ln[160];
    if (ci->temporada > 0)
      snprintf(ln, sizeof ln, "Retomada disponivel   %d%%   Episodio T%dE%d",
               ci->progresso, ci->temporada, ci->episodio);
    else
      snprintf(ln, sizeof ln, "Retomada disponivel   %d%%", ci->progresso);
    TxtLinha l = txt_linha(TXT_CAPTION, ln, 255, 255, 255, 255);
    txt_desenhar_alpha(l, NV_DETW_X, yRetom + (NV_DETW_RETOM_H - l.h) * 0.5f,
                       a * 0.82f);
  }

  // --- "Diretor: ..." -------------------------------------------------------
  if (sup[0]) {
    TxtLinha l = txt_linha_corta(TXT_DET_META, sup, 179, 179, 179, 255,
                                 NV_DETW_TEXTO_W);
    txt_desenhar_alpha(l, NV_DETW_X, ySup + (NV_DETW_LD_SUP - l.h) * 0.5f, a);
  }

  // --- sinopse --------------------------------------------------------------
  if (sin) txt_bloco(TXT_DET_SIN, sin, 255, 255, 255, NV_DETW_X, ySin,
                     NV_DETW_TEXTO_W, NV_DETW_LD_SIN, a, NV_DETW_SIN_LINHAS);

  // --- generos a esquerda, ano empurrado a direita --------------------------
  {
    TxtLinha lg = txt_linha_corta(TXT_DET_META, generoDe(idx), 179, 179, 179, 255,
                                  NV_DETW_DIR - NV_DETW_X - 240.0f);
    txt_desenhar_alpha(lg, NV_DETW_X, yMeta1 + (NV_DETW_LD_META - lg.h) * 0.5f, a);
    if (ano[0]) {
      TxtLinha la = txt_linha(TXT_DET_META, ano, 179, 179, 179, 255);
      float xa = NV_DETW_DIR - la.w;
      txt_desenhar_alpha(la, xa, yMeta1 + (NV_DETW_LD_META - la.h) * 0.5f, a);
      // O ponto separador do web: 1x14, a 24px de folga de cada lado.
      GfxRect pt = { xa - NV_DETW_META_SEP - 1.0f,
                     yMeta1 + (NV_DETW_LD_META - 14.0f) * 0.5f, 1, 14 };
      gfx_cor(pt, 0.0f, 0.70f, 0.70f, 0.70f, 0.55f * a);
    }
  }

  // --- duracao (o web tambem poe o pais; o catalogo daqui nao tem) ----------
  if (dur[0]) {
    TxtLinha ld = txt_linha(TXT_DET_META2, dur, 255, 255, 255, 255);
    txt_desenhar_alpha(ld, NV_DETW_X, yMeta2 + (NV_DETW_LD_META2 - ld.h) * 0.5f, a);
  }
}

// --- medidas de cada tipo de secao ---
// Recebe o INDICE da secao e nao o tipo porque uma secao pode encolher a zero
// sem dados (SEC_ONDE sem provedor nenhum): a secao some do layout em vez de
// deixar um vazio com o titulo dela pendurado.
static float alturaSecao(int r) {
  TipoSecao t = SECOES[r].tipo;
  switch (t) {
    case SEC_ABAS:      return NV_ABA_H;
    case SEC_EPISODIOS: return NV_EP_H;
    case SEC_ELENCO:    return NV_AVATAR + 92.0f;
    case SEC_ONDE:      return secaoN(r) > 0 ? NV_ONDE_H : 0.0f;
    case SEC_SOBRE:     return NV_SOBRE_H;
    default:            return NV_CARD_H;
  }
}
static float larguraSecao(TipoSecao t) {
  switch (t) {
    case SEC_ABAS:      return NV_ABA_W;
    case SEC_EPISODIOS: return NV_CARD_W;
    case SEC_ELENCO:    return NV_AVATAR;
    case SEC_ONDE:      return NV_ONDE_W;
    case SEC_SOBRE:     return NV_TELA_W - NV_MARGEM_X * 2;
    default:            return NV_CARD_W;
  }
}
static float passoSecao(TipoSecao t) {
  if (t == SEC_ABAS) return NV_ABA_PITCH;          // medido texto a texto
  return larguraSecao(t) + (t == SEC_ELENCO ? 34.0f : NV_CARD_GAP);
}

// Numero REAL da temporada na posicao `c`. Serie que comeca na 2 (o que
// acontece quando o Cinemeta nao tem a 1) mostrava "Temporada 1" apontando
// para a 2, e a lista abaixo nao batia com o rotulo.
static int temporadaEm(int c) {
  const CatItem *ci = cat_item(idx);
  if (ci && ci->nTemporadas > 0)
    return (c >= 0 && c < ci->nTemporadas) ? ci->temporadas[c] : ci->temporadas[0];
  return c + 1;
}

static void desenhaAba(GfxRect r, int c, float f, float a) {
  char rot[32]; snprintf(rot, sizeof rot, "Temporada %d", temporadaEm(c));
  int sel = (c == temporada);
  // Escolhida sem foco = pilula translucida; com foco = pilula clara e texto
  // escuro. Sem o estado de escolha, o usuario perde de vista em que temporada
  // esta assim que desce para a lista.
  float base = sel ? 0.30f : 0.0f;
  float lum = 0.62f + 0.34f * f;
  gfx_cor(r, NV_RAIO_PILL, lum, lum, lum, (base + 0.66f * f) * a);
  int cor = f > 0.55f ? 24 : (sel ? 250 : 196);
  TxtLinha l = txt_linha(TXT_BODY, rot, cor, cor, cor, 255);
  txt_desenhar_alpha(l, r.x + (r.w - l.w) * 0.5f, r.y + (r.h - l.h) * 0.5f, a);
}

// Card de episodio: miniatura em cima, texto embaixo. A duracao fica DENTRO da
// miniatura, no canto, como no app da Apple — fora dela o card ganha uma linha
// so para isso e a coluna estica.
static void desenhaEpisodio(GfxRect r, int c, float f, float a, Uint32 agora) {
  // Episodio REAL do titulo aberto (Cinemeta), quando existe. O vetor fixo
  // continua de reserva: filme nao tem episodio, e serie que o Cinemeta nao
  // conhece cairia numa secao vazia, pior que uma lista de exemplo.
  const CatEp *ep = cat_episodio(idx, c);
  float thumbH = r.w * 9.0f / 16.0f;
  GfxRect th = { r.x, r.y, r.w, thumbH };
  // Still do proprio episodio quando o Cinemeta tem; senao a arte do titulo.
  // Um retangulo cinza no lugar da miniatura seria pior que a arte repetida.
  const char *arte = (ep && ep->thumb[0]) ? ep->thumb : home_arte(c * 2 + 3);
  GLuint t2 = arte ? tex_obter(arte) : 0;
  if (t2) {
    gfx_tex_aspect_atual = tex_aspecto(arte);
    float fase = agora / 1000.0f + c * 0.6f;
    gfx_rect(th, t2, GFX_CARD, f, sinf(fase) * 0.010f * f, 0, NV_RAIO_CARD, 0, 0, 0, a);
    gfx_tex_aspect_atual = 0.0f;
  } else gfx_cor(th, NV_RAIO_CARD, 0.14f, 0.14f, 0.16f, a);

  // Painel claro atras do texto quando o episodio esta em foco. Medido no
  // aparelho: 428x284 para uma coluna de 410 (ou seja, sangra ~9px para cada
  // lado), branco a ~15%. E o que amarra miniatura e texto num objeto so.
  if (f > 0.01f) {
    // O painel cobre TODO o bloco de texto, inclusive a data e o selo — no
    // app da Apple nada do texto fica de fora dele.
    GfxRect pn = { r.x - 12, r.y + thumbH + 6, r.w + 24, r.h - thumbH - 6 };
    gfx_cor(pn, 0.06f, 1, 1, 1, 0.19f * f * a);
  }

  int e = c % (int)(sizeof EPISODIOS / sizeof *EPISODIOS);
  const char *epNome = ep && ep->nome[0] ? ep->nome : EPISODIOS[e][0];
  const char *epDur  = ep && ep->duracao[0] ? ep->duracao : EPISODIOS[e][1];
  const char *epData = ep && ep->data[0] ? ep->data : EPISODIOS[e][2];
  const char *epSin  = ep && ep->sinopse[0] ? ep->sinopse : SIN_EP;
  int epNum = ep ? ep->episodio : c + 1;

  // Duracao sobre a arte, SEM pilula de fundo — o app da Apple usa so o texto
  // com o triangulo e, quando ha progresso, uma barrinha entre os dois. A
  // pilula que eu tinha posto pesava o canto e nao existe no original.
  TxtLinha ld = txt_linha(TXT_CAPTION2, epDur, 255, 255, 255, 255);
  float dx = th.x + 20, dy = th.y + thumbH - ld.h - 18;
  GfxRect tri = { dx, dy + ld.h * 0.5f - 8, 14, 16 };
  gfx_rect(tri, 0, GFX_PLAY, 0, 0, 0, 0.0f, 1, 1, 1, 0.96f * a);
  float px2 = dx + 26;
  if (c == 0) {   // so o episodio comecado mostra progresso
    GfxRect tr = { px2, dy + ld.h * 0.5f - 2, 46, 4 };
    GfxRect at = { px2, dy + ld.h * 0.5f - 2, 17, 4 };
    gfx_cor(tr, 0.5f, 1, 1, 1, 0.34f * a);
    gfx_cor(at, 0.5f, 1, 1, 1, 0.96f * a);
    px2 += 60;
  }
  txt_desenhar_alpha(ld, px2, dy, a);

  char cab[24]; snprintf(cab, sizeof cab, "EPISODIO %d", epNum);
  float y = th.y + thumbH + NV_EP_THUMB_GAP;
  // Sobre o painel claro do foco, o cinza do texto perde contraste — no app da
  // Apple o bloco focado fica visivelmente mais legivel, nao mais apagado.
  int cz = (int)(178 + 46 * f), cb2 = (int)(188 + 40 * f);
  TxtLinha lc = txt_linha(TXT_CAPTION2, cab, cz, cz + 2, cb2, 255);
  txt_desenhar_alpha(lc, r.x, y, a * 0.95f); y += lc.h + 6;
  TxtLinha ln = txt_linha(TXT_BODY, epNome, 255, 255, 255, 255);
  txt_desenhar_alpha(ln, r.x, y, a); y += ln.h + 8;
  int cs = (int)(198 + 42 * f);
  y += txt_bloco(TXT_CAPTION2, epSin, cs, cs + 2, cs + 10, r.x, y, r.w,
                 NV_LD_CAPTION2, a * 0.92f, 5);
  y += 8;
  TxtLinha lf = txt_linha(TXT_CAPTION2, epData, cz, cz + 2, cb2, 255);
  txt_desenhar_alpha(lf, r.x, y, a * 0.9f);
  // Badge de classificacao indicativa ao lado da data, em laranja — no app da
  // Apple ele fecha a linha e e o unico ponto de cor do bloco.
  TxtLinha lb = txt_linha(TXT_MINI, "A14", 255, 255, 255, 255);
  // Menor e menos saturado que eu tinha: no original ele e um selo discreto ao
  // lado da data, nao um rotulo que disputa atencao com o titulo.
  GfxRect bg = { r.x + lf.w + 11, y + (lf.h - lb.h) * 0.5f, lb.w + 6, lb.h };
  gfx_cor(bg, 0.20f, 0.72f, 0.36f, 0.12f, 0.85f * a);
  txt_desenhar_alpha(lb, bg.x + 3, bg.y, a * 0.95f);
}

// Elenco: retrato recortado em circulo (raio 0.5 no SDF), nome e papel abaixo.
// "Como assistir": cartao com o nome do provedor e o tipo de acesso embaixo.
static void desenhaOnde(GfxRect r, int c, float f, float a) {
  const CatItem *ci = cat_item(idx);
  const char *nome = "", *sub = "", *logo = "";
  // Tier DE VERDADE, na ordem assinatura > aluguel > compra. Sem dado nesta
  // posicao a secao nem chega aqui (secaoN/alturaSecao ja encolheram), mas o
  // guard e o que impede um card vazio se os dados chegarem parcialmente.
  if (!ci || !ondeCard(ci, c, &nome, &sub, &logo)) return;
  float lum = 0.18f + 0.72f * f;
  gfx_cor(r, 0.10f, lum, lum, lum, (0.22f + 0.72f * f) * a);
  int cor = f > 0.5f ? 24 : 236;
  TxtLinha ln = txt_linha(TXT_BODY, nome, cor, cor, cor, 255);
  TxtLinha lt = txt_linha(TXT_CAPTION2, sub,
                          f > 0.5f ? 90 : 168, f > 0.5f ? 90 : 170, f > 0.5f ? 96 : 178, 255);
  float tx = r.x + 30;
  if (logo[0]) {
    GLuint tl = tex_obter(logo);
    if (tl) {
      GfxRect rl = { r.x + 22, r.y + r.h * 0.5f - 20, 40, 40 };
      gfx_tex_aspect_atual = 1.0f;   // as logos do TMDB sao quadradas
      gfx_rect(rl, tl, GFX_CARD, 0, 0, 0, 9.0f, 0, 0, 0, a);
      gfx_tex_aspect_atual = 0.0f;
      tx = r.x + 74;
    }
  }
  txt_desenhar_alpha(ln, tx, r.y + r.h * 0.5f - ln.h + 2, a);
  txt_desenhar_alpha(lt, tx, r.y + r.h * 0.5f + 6, a * 0.95f);
}

// "Sobre": bloco de texto corrido, sem card. Ele existe para fechar a pagina —
// no app da Apple e a ultima secao antes do fim do scroll.
static void desenhaSobre(GfxRect r, float a) {
  // A sinopse do titulo aberto, nao tres frases fixas que falavam de uma serie
  // de comedia dramatica em cima de qualquer filme.
  const CatItem *ci = cat_item(idx);
  if (ci && ci->sinopse[0]) {
    txt_bloco(TXT_CAPTION, ci->sinopse, 202, 204, 212, r.x, r.y,
              NV_TELA_W - NV_MARGEM_X * 2, NV_LD_CAPTION, a * 0.92f, 6);
    return;
  }
  { float y = r.y;
    int k;
    for (k = 0; k < (int)(sizeof SOBRE / sizeof *SOBRE); k++) {
      TxtLinha l = txt_linha(TXT_CAPTION, SOBRE[k], 202, 204, 212, 255);
      txt_desenhar_alpha(l, r.x, y, a * 0.92f);
      y += NV_LD_CAPTION;
    } }
}

static void desenhaElenco(GfxRect r, int c, float f, float a) {
  const CatItem *ci = cat_item(idx);
  const char *nome = NULL, *papel = NULL, *foto = NULL;
  if (ci && c < ci->nElenco) {
    nome = ci->elenco[c].nome;
    papel = ci->elenco[c].papel;
    if (ci->elenco[c].foto[0]) foto = ci->elenco[c].foto;
  }
  // Havendo elenco de verdade, mostrar SO ele. Completar com o vetor de exemplo
  // punha tres atores reais ao lado de tres inventados na mesma fileira, o que
  // e pior que uma fileira curta.
  { const CatItem *ce = cat_item(idx);
    if (ce && ce->nElenco > 0 && c >= ce->nElenco) return; }
  if (!nome) { int e = c % N_ELENCO; nome = ELENCO[e][0]; papel = ELENCO[e][1]; }

  GLuint t2 = foto ? tex_obter(foto) : 0;
  if (t2) {
    gfx_tex_aspect_atual = tex_aspecto(foto);
    gfx_rect(r, t2, GFX_CARD, f, 0, 0, 0.5f, 0, 0, 0, a);
    gfx_tex_aspect_atual = 0.0f;
  } else {
    // Sem foto, um circulo com a inicial — melhor que um buraco cinza, e e o
    // que a propria Apple mostra quando o ator nao tem retrato.
    gfx_cor(r, 0.5f, 0.20f, 0.20f, 0.23f, a);
    char ini[5] = {0};
    for (int k = 0; k < 4 && nome[k] && (unsigned char)nome[k] >= 0x20; k++) {
      ini[k] = nome[k];
      if ((nome[k] & 0xC0) != 0x80) { if (k) { ini[k] = 0; break; } }
    }
    TxtLinha li = txt_linha(TXT_TITULO3, ini, 210, 212, 220, 255);
    txt_desenhar_alpha(li, r.x + (r.w - li.w) * 0.5f, r.y + (r.h - li.h) * 0.5f, a * 0.9f);
  }
  TxtLinha ln = txt_linha(TXT_CAPTION2, nome, 255, 255, 255, 255);
  TxtLinha lp = txt_linha(TXT_CAPTION2, papel && papel[0] ? papel : " ", 165, 167, 175, 255);
  txt_desenhar_alpha(ln, r.x + (r.w - ln.w) * 0.5f, r.y + r.h + 14, a);
  txt_desenhar_alpha(lp, r.x + (r.w - lp.w) * 0.5f, r.y + r.h + 14 + ln.h + 4, a * 0.9f);
}

static void secao(int r, float y, float a, Uint32 agora) {
  TipoSecao tipo = SECOES[r].tipo;
  // Secao sem itens: o titulo desenharia uma promessa sem card embaixo dela.
  if (tipo == SEC_ONDE && secaoN(r) == 0) return;
  // O que sobe para baixo do cabecalho fixo desaparece antes de cruza-lo. Sem
  // isso, as abas subiam ate a linha do titulo e os dois textos se liam um
  // sobre o outro — a faixa escura sozinha nao resolvia.
  float somem = anim_clamp((y - (NV_PG_TOPO + 34.0f)) / 90.0f, 0.0f, 1.0f);
  a *= somem;
  if (a <= 0.005f) return;
  float lh = alturaSecao(r), lw = larguraSecao(tipo), passo = passoSecao(tipo);
  if (y > NV_TELA_H || y + lh < -80) return;

  if (SECOES[r].titulo) {
    TxtLinha tl = txt_linha(TXT_HEADLINE, SECOES[r].titulo, 255, 255, 255, 255);
    txt_desenhar_alpha(tl, NV_MARGEM_X, y - tl.h - NV_PG_SEC_CARDS, a);
  }
  for (int passe = 0; passe < 2; passe++)
    for (int c = 0; c < secaoN(r); c++) {
      float f = animFoco[r][c];
      if (passe == 0 && f < 0.01f) continue;
      // "Sobre" e um bloco de texto, nao um objeto: crescer com o foco empurra
      // a coluna inteira para fora da margem esquerda, porque a largura dela e
      // a tela inteira menos as margens.
      float esc = (tipo == SEC_SOBRE) ? 1.0f
                : 1.0f + (tipo == SEC_ELENCO ? NV_FOCO_ESCALA_P : NV_FOCO_ESCALA) * f;
      float w = lw * esc;
      float cx = NV_MARGEM_X + c * passo - scrollSec[r] + lw * 0.5f;
      if (cx < -lw || cx > NV_TELA_W + lw) continue;
      // Altura do OBJETO focavel, que nem sempre e a altura da secao: no
      // episodio so a miniatura recebe foco e sombra, nao o bloco de texto
      // embaixo dela. Usar a altura da secao aqui punha a sombra atras do texto.
      float hObj;
      switch (tipo) {
        case SEC_ABAS:      hObj = NV_ABA_H * esc;        break;
        case SEC_ELENCO:    hObj = lw * esc;              break;   // circulo
        case SEC_ONDE:      hObj = NV_ONDE_H * esc;       break;
        case SEC_EPISODIOS: hObj = w * 9.0f / 16.0f;      break;   // so a miniatura
        case SEC_SOBRE:     hObj = NV_SOBRE_H;            break;
        default:            hObj = NV_CARD_H * esc;       break;
      }
      // O item em foco sobe alguns px: no tvOS o objeto nao so cresce, ele se
      // levanta em direcao ao espectador, e a sombra cai por baixo.
      float px = cx - w * 0.5f, py = y - NV_FOCO_LIFT * f;
      GfxRect r2 = { px, py, w, hObj };
      // NENHUMA secao da pagina usa sombra. A sombra e feita para separar um
      // card claro de um fundo escuro — que e o caso da home. Aqui o fundo e a
      // arte desfocada, quase sempre clara, e a mancha escura em volta do item
      // focado le como halo sujo. O foco se marca por escala, pelo painel do
      // episodio e pelo brilho do proprio card.
      if (passe == 0) continue;

      switch (tipo) {
        case SEC_ABAS:      desenhaAba(r2, c, f, a); break;
        case SEC_EPISODIOS: { GfxRect e = { px, py, w, lh }; desenhaEpisodio(e, c, f, a, agora); break; }
        case SEC_ELENCO:    desenhaElenco(r2, c, f, a); break;
        case SEC_ONDE:      desenhaOnde(r2, c, f, a); break;
        case SEC_SOBRE:     desenhaSobre(r2, a); break;
        default: {
          // "Voce tambem pode gostar" mostra titulos DE VERDADE parecidos com
          // o aberto; "Trailers" continua com arte do acervo, porque nao ha
          // trailer nenhum para mostrar e fingir um seria pior.
          const char *caminho;
          const CatItem *simItem = NULL;
          if (r == SEC_SIMILARES) {
            static int sims[N_SUG];
            static int nSims, simDe = -1;
            if (simDe != idx) { nSims = cat_similares(idx, sims, N_SUG); simDe = idx; }
            if (c < nSims) simItem = cat_item(sims[c]);
          }
          caminho = (simItem && simItem->backdrop[0]) ? simItem->backdrop
                  : home_arte(r * 5 + c * 3);
          GLuint tc = caminho ? tex_obter(caminho) : 0;
          if (tc) {
            float fase = agora / 1000.0f + (r * 3 + c) * 0.6f;
            gfx_tex_aspect_atual = tex_aspecto(caminho);
            gfx_rect(r2, tc, GFX_CARD, f, sinf(fase) * 0.010f * f,
                     cosf(fase * 0.8f) * 0.006f * f, NV_RAIO_CARD, 0, 0, 0, a);
            gfx_tex_aspect_atual = 0.0f;
          } else gfx_cor(r2, NV_RAIO_CARD, 0.14f, 0.14f, 0.16f, a);
          // Logo do titulo sobre a arte, como na home: sem ele o card de
          // similar e uma imagem sem nome, e o dono nao sabe o que esta vendo.
          // O veu na base e o que mantem o logo legivel em arte clara.
          if (simItem && simItem->logo[0]) {
            GLuint tl = tex_obter(simItem->logo);
            if (tl) {
              float lw2 = w * 0.52f, lh2 = hObj * 0.30f;
              GfxRect rl = { r2.x + 16, r2.y + hObj - lh2 - 14, lw2, lh2 };
              gfx_rect(r2, 0, GFX_VEU, 0, 0, 0, NV_RAIO_CARD, 0, 0, 0, 0.5f * a);
              gfx_tex_aspect_atual = 0.0f;   // o logo ja vem na proporcao certa
              gfx_rect(rl, tl, GFX_TEXTO, 0, 0, 0, 0.0f, 1, 1, 1, a);
            }
          }
        }
      }
    }
}

void detail_desenhar(Uint32 agora) {
  if (!aberto) return;
  float s = suave(t), a2 = fase2();

  // O que cerca a tela ENQUANTO ela cresce. Assim que o backdrop cobre tudo
  // isto some, e a home nem chega a ser desenhada (detail_cobre_tela).
  if (!detail_cobre_tela()) {
    GfxRect tela = { 0, 0, NV_TELA_W, NV_TELA_H };
    gfx_cor(tela, 0.0f, 0.051f, 0.051f, 0.051f, s);   // #0d0d0d, o fundo do web
  }
  gfx_sem_recorte();

  // --- backdrop full-bleed --------------------------------------------------
  // A tela de detalhe do web e uma imagem de 1920x1080 em (0,0) com a vinheta
  // horizontal por cima; nao ha cartao, nem moldura, nem titulos vizinhos. O
  // unico movimento que sobra do port anterior e a ORIGEM: o retangulo cresce
  // a partir do card que estava em foco na home, o que da continuidade a troca
  // de tela. No repouso ele e exatamente a tela inteira.
  GfxRect cheia = { 0, 0, NV_TELA_W, NV_TELA_H };
  GfxRect alvo = {
    anim_mistura(item.rect.x, cheia.x, s), anim_mistura(item.rect.y, cheia.y, s),
    anim_mistura(item.rect.w, cheia.w, s), anim_mistura(item.rect.h, cheia.h, s),
  };
  const char *arte = arteDe(idx);
  GLuint tex = arte ? tex_obter(arte) : 0;
  // Ao rolar para as secoes, o web NAO desfoca a arte: ele a APAGA. MEDIDO na
  // folha, em `.series-detail-shell.detail-scrolled` — o backdrop vai a
  // `opacity: 0.15` e a vinheta a 0, ambos em 0.8s cubic-bezier(.4,0,.2,1). O
  // desfoque gaussiano com escurecimento que estava aqui e do app da Apple TV,
  // custa duas passadas de tela cheia por quadro, e some com a cor da arte —
  // que no web continua la, so que fraca.
  if (pg > 0.01f) {
    GfxRect tela = { 0, 0, NV_TELA_W, NV_TELA_H };
    gfx_cor(tela, 0.0f, 0.051f, 0.051f, 0.051f, pg);
  }
  if (tex) {
    gfx_tex_aspect_atual = tex_aspecto(arte);
    gfx_rect(alvo, tex, GFX_DETALHE, 0, 0, 0, 0.0f, 0, 0, 0,
             1.0f - 0.85f * pg);
    gfx_tex_aspect_atual = 0.0f;
  } else {
    gfx_cor(alvo, 0.0f, 0.051f, 0.051f, 0.051f, 1.0f);
  }
  (void)arteBorrada;

  // Coluna de conteudo. Sai de cena quando a pagina toma o lugar.
  heroWeb(a2 * (1.0f - pg));

  if (pg <= 0.01f) return;

  // --- pagina esticada: titulo centralizado no topo e as secoes ---
  // Faixa escura no topo, sob o cabecalho. O cabecalho e fixo e o conteudo
  // rola por baixo dele; sem a faixa, as abas subiam e cruzavam o titulo, e as
  // duas coisas ficavam ilegiveis uma sobre a outra.
  // A faixa so precisa existir quando algo esta subindo por baixo do
  // cabecalho. Fixa e opaca, como estava, ela punha uma barra preta no topo de
  // uma pagina que no original e clara ali.
  float precisaFaixa = anim_clamp(scrollY / 120.0f, 0.0f, 1.0f);
  if (precisaFaixa > 0.01f) {
    GfxRect faixa = { 0, 0, NV_TELA_W, NV_PG_TOPO + 78.0f };
    gfx_rect(faixa, 0, GFX_VEU_TOPO, 0, 0, 0, 0.0f, 0, 0, 0, pg * 0.80f * precisaFaixa);
  }

  // Cabecalho da pagina: o LOGO do titulo, centralizado — igual ao cartao. O
  // texto em maiusculas com tracking so entra quando o titulo nao tem logo; ele
  // imita a forma, mas nao a identidade de cada producao.
  const char *logoCab = logoDe(idx);
  GLuint texCab = logoCab ? tex_obter(logoCab) : 0;
  if (texCab) {
    float asp = tex_aspecto(logoCab);
    if (asp <= 0.0f) asp = 4.0f;
    float h = NV_LOGO_CAB_H, w = h * asp;
    if (w > NV_LOGO_CAB_MAX_W) { w = NV_LOGO_CAB_MAX_W; h = w / asp; }
    GfxRect r = { (NV_TELA_W - w) * 0.5f, NV_PG_TOPO, w, h };
    gfx_tex_aspect_atual = 0.0f;
    gfx_rect(r, texCab, GFX_TEXTO, 0, 0, 0, 0.0f, 1, 1, 1, pg);
  } else {
    char cab[64]; int k = 0;
    for (const char *q = tituloDe(idx); *q && k < 62; q++, k++)
      cab[k] = (*q >= 'a' && *q <= 'z') ? (char)(*q - 32) : *q;
    cab[k] = 0;
    float lcab = txt_tracking(TXT_TITULO2, cab, 255, 255, 255, -1, 0, 0, NV_TRACKING_CAB);
    txt_tracking(TXT_TITULO2, cab, 255, 255, 255,
                 (NV_TELA_W - lcab) * 0.5f, NV_PG_TOPO, pg, NV_TRACKING_CAB);
  }
  // As secoes entram subindo: continuam o movimento do cartao esticando, em
  // vez de piscarem no lugar final ja formadas.
  float y = NV_PG_TOPO + 42.0f + NV_PG_TIT_ABAS + (1.0f - pg) * 300.0f - scrollY;
  for (int r = 0; r < N_SECOES; r++) {
    secao(r, y, pg, agora);
    // O espaco grande pertence a secao SEGUINTE (e o respiro antes do
    // cabecalho dela), nao a atual. Medindo pela atual, a fileira de episodios
    // — que nao tem cabecalho proprio — colava no titulo "Trailers".
    int proxTemTitulo = (r + 1 < N_SECOES) && SECOES[r + 1].titulo;
    y += alturaSecao(r) + (proxTemTitulo ? NV_PG_ENTRE_SEC : 26.0f);
  }
}

int detail_indice(void) { return idx; }
int detail_pediu_reproduzir(void) { int v = pedReproduzir; pedReproduzir = 0; return v; }
int detail_pediu_marcar(void)     { int v = pedMarcar;     pedMarcar = 0;     return v; }

int detail_pediu_fontes(void) { int v = pedFontes; pedFontes = 0; return v; }
