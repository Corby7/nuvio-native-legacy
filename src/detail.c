// Tela de detalhe do titulo, no layout do APP WEB (sessao LOGADA).
//
// O port comecou copiando o app da Apple TV, e cada pedaco dessa heranca foi
// sendo devolvido a medida que o web era MEDIDO. O que restava dela ate agora
// era tudo o que fica abaixo da dobra — pilulas de temporada de 236x63, card de
// episodio com o texto ABAIXO da miniatura, secoes "Trailers", "Como assistir"
// e "Sobre". Nada disso existe no web. O que existe, medido em 1920x1080 na
// serie "Silo" com o perfil do dono:
//
//   1. A tela e UM documento rolavel de 2144px de altura. O hero ocupa os
//      primeiros 1080 e ROLA junto: nao ha cabecalho fixo nem logo centralizado
//      no topo. Descer nao "estica" nada — apenas rola.
//   2. As secoes sao quatro: abas de temporada (269x80), fileira de episodios
//      (cards 640x422 com o texto DENTRO da miniatura), abas de informacao
//      ("Criador e elenco | Avaliacoes | Mais como este | Trailer") e a fileira
//      de elenco (avatar 140 redondo).
//   3. Rolar leva o topo do grupo focado a 33% da altura util (40% nas abas de
//      informacao). Isto esta no fonte do web (DETAIL_ROW_FOCUS_TARGET) e foi
//      conferido medindo o scrollTop nos quatro grupos.
//   4. Ao rolar, a arte de fundo NAO desfoca: ela vai a 15% de opacidade em
//      0.8s. O desfoque gaussiano era do app da Apple TV.
#include "detail.h"
#include "home.h"
#include "extras.h"
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

// Teto de itens por secao. 24 e nao 8: uma temporada de "Silo" tem 10
// episodios e o vetor de 8 escondia os dois ultimos — a lista parecia menor do
// que a serie e.
#define N_ITENS    24
#define N_SECOES    4
#define N_ELENCO    6

static HomeItem item;
static int  aberto = 0, saindo = 0;
static int  idx = 0;                 // titulo atual dentro do acervo
static float t = 0.0f;               // 0 = card na home, 1 = tela cheia
// Dois estados, nao tres: o hero (nivel 0) e a pagina rolada (nivel 1). O
// nivel intermediario "cartao vira tela cheia" so fazia sentido enquanto havia
// cartao; no web a tela ja nasce cheia.
static int  nivel = 0;
static int  botao = 0;      // botao em foco no hero
static int  pedReproduzir = 0, pedMarcar = 0, pedFontes = 0;
static int  pedDoInicio = 0;         // botao "Reproduzir desde o inicio"
static Uint32 okDesceEm = 0;
static float pg = 0.0f;              // 0..1: hero -> pagina rolada
static Foco foco;
static float animFoco[N_SECOES][N_ITENS];
static float scrollSec[N_SECOES];    // rolagem HORIZONTAL de cada fileira
static float scrollY = 0.0f;         // rolagem VERTICAL do documento
static int temporada = 0;            // temporada ESCOLHIDA (nao a focada)
static int abaInfo = 0;              // aba de informacao escolhida

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

// As quatro secoes do web, com o topo do GRUPO em coordenada de documento — e
// nao uma pilha de alturas somadas, que era o modelo do app da Apple TV. As
// posicoes sao fixas porque no web tambem sao: o documento tem tamanho
// conhecido e a rolagem so muda o quanto dele se enxerga.
typedef enum { SEC_TEMPORADAS, SEC_EPISODIOS, SEC_ABAS_INFO, SEC_ELENCO } TipoSecao;
static const struct { float grupo, alvo; } SECOES[N_SECOES] = {
  { NV_DETP_G_TEMP,   NV_DETP_ALVO_FILEIRA },
  { NV_DETP_G_EP,     NV_DETP_ALVO_FILEIRA },
  { NV_DETP_G_ABAS,   NV_DETP_ALVO_ABAS    },
  { NV_DETP_G_ELENCO, NV_DETP_ALVO_FILEIRA },
};
// As abas sao DINAMICAS, como no web: renderSeriesInsightSection
// (metaDetailsScreen.js:3751) so acrescenta "Mais como este", "Trailer" e
// "Colecao" quando a lista correspondente tem itens, e esconde a barra inteira
// quando sobra uma aba so. O port cravava as quatro e as tres ultimas caiam
// todas em "Sem informacao para esta aba." — que e exatamente o que o web
// evita nao mostrando a aba.
//
// Aqui existem duas: elenco (sempre) e avaliacoes (quando ha nota). Similares
// e trailer nao tem fonte neste port; quando tiverem, entram nesta tabela.
typedef enum { ABA_ELENCO, ABA_AVALIACOES, ABA_RELACIONADOS, ABA_COMENTARIOS,
               ABA_NFIXAS } AbaInfoId;
static const char *ABA_ROTULO[ABA_NFIXAS] = {
  "Criador e elenco", "Avaliacoes", "Mais como este", "Comentarios"
};

// Nota do IMDb do titulo aberto, 0 quando nao ha.
static int notaDe(int i) {
  const CatItem *ci = cat_item(i);
  return ci ? ci->nota : 0;
}
static int abaDisponivel(int id) {
  switch (id) {
    case ABA_ELENCO:       return 1;
    // Basta UMA das notas para a aba valer a pena; o cartao que faltar mostra
    // "-", que e o que o web faz.
    case ABA_AVALIACOES:   return notaDe(idx) > 0 || extras_nota_trakt() > 0;
    case ABA_RELACIONADOS: return extras_n_relacionados() > 0;
    case ABA_COMENTARIOS:  return extras_n_comentarios() > 0;
    default:               return 0;
  }
}
// Traduz a posicao visivel `c` para o id da aba.
static int abaIdDe(int c) {
  for (int id = 0, v = 0; id < ABA_NFIXAS; id++)
    if (abaDisponivel(id) && v++ == c) return id;
  return ABA_ELENCO;
}
static int nAbasInfo(void) {
  int n = 0;
  for (int id = 0; id < ABA_NFIXAS; id++) if (abaDisponivel(id)) n++;
  return n;
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

static int  secaoN(int r);
static int  temporadaEm(int c);
static float larguraTemporada(int c);
static float larguraAbaInfo(int i);

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
static int ehSerie(void) {
  const CatItem *ci = cat_item(idx);
  if (!ci) return 0;
  if (ci->tipo[0]) return strcmp(ci->tipo, "series") == 0;
  return cat_n_episodios(idx) > 0;
}

static float suave(float x) {
  x = anim_clamp(x, 0.0f, 1.0f);
  return 1.0f - (1.0f - x) * (1.0f - x) * (1.0f - x);
}
static float fase2(void) { return suave((t - 0.45f) / 0.55f); }

// A Inter embarcada so tem Regular, Medium e Bold, e a pagina pede 500, 600 e
// 800 em corpos (32, 26, 21) que so existem em Regular na tabela de text.c —
// que e arquivo de outro agente nesta sessao. Engrossar redesenhando a mesma
// linha com deslocamentos sub-pixel e o que sobra, e e o que os rasterizadores
// chamam de "faux bold": custa uma textura so, porque a linha vem do cache.
static void txt_peso(TxtLinha l, float x, float y, float a, float grossura) {
  txt_desenhar_alpha(l, x, y, a);
  if (grossura > 0.05f) txt_desenhar_alpha(l, x + grossura * 0.5f, y, a);
  if (grossura > 0.9f)  txt_desenhar_alpha(l, x + grossura, y, a);
}

void detail_abrir(const HomeItem *it) {
  item = *it;
  aberto = 1; saindo = 0; nivel = 0; botao = 0;
  t = 0.0f; pg = 0.0f; scrollY = 0.0f; abaInfo = 0;
  idx = it->indice;
  // Nota do Trakt, comentarios e relacionados. Pedido na ABERTURA e nao no
  // desenho: as abas so aparecem depois que o dado chega, e pedir no desenho
  // faria a barra de abas surgir com o titulo ja na tela.
  { const CatItem *ci = cat_item(idx);
    if (ci && ci->imdb[0]) extras_pedir(ci->imdb, ehSerie()); }
  // A aba marcada tem de ser a da temporada que os episodios trazem. Comecando
  // sempre em 0, uma serie cujo primeiro episodio carregado e da 4 abria com
  // "Temporada 1" aceso — o rotulo desmentia a lista logo abaixo.
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

// 0..1 de quanto o detalhe ja tomou a tela. A home le isto para DESCER as
// fileiras enquanto ele entra: e o movimento que o dono descreve como "so os
// posters descem". Fica aqui e nao numa variavel compartilhada porque a mola
// que o produz e a mesma do desenho — dois relogios diferentes descasariam.
float detail_progresso(void) { return aberto ? suave(t) : 0.0f; }

// Temporada e episodio EM FOCO, para quem for pedir fonte.
//
// Sem isto o addons_buscar recebia so o imdb da serie e cravava ":1:1" — e por
// isso as fontes eram sempre as do episodio 1, qualquer que fosse o escolhido.
// Devolve 0 quando o foco nao esta na fileira de episodios; nesse caso quem
// chama cai no primeiro episodio da temporada em exibicao, que e o que a tela
// mostra em cima.
int detail_ep_foco(int *temp, int *epis) {
  const CatEp *ep = NULL;
  if (!aberto) return 0;
  if (foco.fileira == SEC_EPISODIOS) ep = cat_episodio(idx, foco.coluna);
  if (!ep) ep = cat_episodio(idx, 0);
  if (!ep) return 0;
  if (temp) *temp = ep->temporada;
  if (epis) *epis = ep->episodio;
  return 1;
}

int detail_assentado(void) {
  return aberto && !saindo && t > 0.985f && nivel == 0;
}

int detail_cobre_tela(void) {
  // O backdrop e FULL-BLEED: assim que ele termina de crescer, nao sobra um
  // pixel da tela anterior. Desenhar a home por baixo custava um quadro inteiro
  // de preenchimento a toa — medido em 42 ms no pior quadro.
  return aberto && suave(t) > 0.995f;
}

static int secaoN(int r) {
  const CatItem *ci = cat_item(idx);
  switch (r) {
    case SEC_TEMPORADAS:
      // Filme nao tem temporada: a fileira SOME em vez de mostrar abas que nao
      // levam a lugar nenhum. E o que o web faz — a `.series-season-row` so
      // existe no layout de serie.
      if (!ehSerie()) return 0;
      if (ci && ci->nTemporadas > 0)
        return ci->nTemporadas < N_ITENS ? ci->nTemporadas : N_ITENS;
      return 0;
    case SEC_EPISODIOS: {
      int q = cat_n_episodios(idx);
      if (q <= 0) return 0;
      return q < N_ITENS ? q : N_ITENS;
    }
    // Uma aba so = barra escondida, como o `tabItems.length > 1` do web.
    case SEC_ABAS_INFO: { int n = nAbasInfo(); return n > 1 ? n : 0; }
    case SEC_ELENCO:
      if (ci && ci->nElenco > 0) return ci->nElenco;
      return N_ELENCO;
  }
  return 0;
}

// O botao primario e UM SO, e ele TROCA DE ROTULO conforme o estado:
// "Reproduzir" quando nunca foi aberto, "Retomar TxEy" quando ha progresso.
//
// Havia um segundo botao ("Reproduzir desde o inicio") que aparecia junto da
// linha de retomada. Saiu por decisao do dono: "quando ja tiver comecado nao use
// outro botao para resumir, use o mesmo botao de reproduzir, so troque ele". E o
// que a referencia mostra tambem — primario + TRES circulares (+, ja assisti,
// trailer), sem segundo botao de texto.
static int nBotoes(void) { return 4; }   // primario + 3 circulares

void detail_evento(const SDL_Event *e) {
  if (saindo) return;

  if (e->type == SDL_KEYDOWN && (e->key.keysym.sym == SDLK_RETURN ||
                                 e->key.keysym.sym == SDLK_KP_ENTER)) {
    if (!okDesceEm) okDesceEm = SDL_GetTicks();
    return;
  }
  if (e->type == SDL_KEYUP && (e->key.keysym.sym == SDLK_RETURN ||
                               e->key.keysym.sym == SDLK_KP_ENTER)) {
    Uint32 dur;
    // SOLTAR sem ter PRESSIONADO nao e clique. Sem esta guarda o detalhe
    // reproduzia sozinho ao ser aberto: o OK apertado na home entrega o KEYDOWN
    // a home (que abre o detalhe) e o KEYUP JA CHEGA AQUI, com nivel 0 e botao
    // 0 — que e exatamente "Reproduzir". Da para ver como o dono descreveu:
    // "clica num titulo e ele ja clica duas vezes e inicia".
    //
    // Antes isto nao aparecia porque o botao morava no nivel 1 e o KEYUP orfao
    // caia em nenhum caso. Passar os botoes para o nivel 0 (que e onde o web os
    // poe) descobriu o defeito que ja existia.
    if (!okDesceEm) return;
    dur = SDL_GetTicks() - okDesceEm;
    okDesceEm = 0;
    if (nivel == 0) {
      // Ordem FIXA agora que o botao de texto extra saiu: primario, adicionar a
      // lista, marcar como visto, trailer.
      if (botao == 0) {
        if (dur >= NV_HOLD_MS) pedFontes = 1; else pedReproduzir = 1;
      } else if (botao == 1) {
        pedMarcar = 1;
      } else {
        pedFontes = 1;
      }
    } else if (foco.fileira == SEC_TEMPORADAS) {
      // Trocar de aba BUSCA a temporada. Antes so mudava o realce e a lista
      // continuava a mesma, o que fazia a aba parecer quebrada.
      temporada = foco.coluna;
      desc_episodios(idx, temporadaEm(temporada));
    } else if (foco.fileira == SEC_ABAS_INFO) {
      abaInfo = foco.coluna;
    } else if (foco.fileira == SEC_EPISODIOS) {
      // No web e `openEpisodeStreams`. Aqui a folha de fontes ainda e a do
      // titulo: `stream_folha_abrir()` nao recebe episodio. Melhor abrir a
      // folha que existe do que nao responder ao OK.
      pedFontes = 1;
    }
    return;
  }

  if (e->type != SDL_KEYDOWN) return;
  SDL_Keycode k = e->key.keysym.sym;

  if (k == SDLK_ESCAPE || k == SDLK_AC_BACK || k == SDLK_BACKSPACE ||
      k == SDLK_DELETE) {
    if (nivel > 0) nivel = 0; else saindo = 1;
    return;
  }
  if (nivel == 0) {
    if (k == SDLK_DOWN) {
      // Descer do hero cai na PRIMEIRA fileira que existe. Num filme nao ha
      // temporadas nem episodios, e parar numa fileira vazia deixava o D-pad
      // sem resposta.
      for (int r = 0; r < N_SECOES; r++)
        if (secaoN(r) > 0) { foco.fileira = r; foco.coluna = 0; nivel = 1; break; }
    }
    else if (k == SDLK_RIGHT) { if (botao < nBotoes() - 1) botao++; }
    else if (k == SDLK_LEFT)  { if (botao > 0) botao--; }
    return;
  }
  // Com uma aba sem conteudo escolhida nao ha fileira de elenco para descer:
  // sem esta guarda o foco caia em avatares que a secao nem desenha mais.
  if (k == SDLK_DOWN && foco.fileira == SEC_ABAS_INFO && abaInfo != 0) return;
  if (k == SDLK_RIGHT)      focus_mover(&foco, 1, 0);
  else if (k == SDLK_LEFT)  focus_mover(&foco, -1, 0);
  else if (k == SDLK_DOWN)  focus_mover(&foco, 0, 1);
  else if (k == SDLK_UP)    { if (!focus_mover(&foco, 0, -1)) nivel = 0; }
}

// Largura do item e passo horizontal de cada fileira. Temporada e aba de
// informacao tem largura VARIAVEL (saem do texto), e por isso o passo delas nao
// e uma constante como a do episodio.
static float larguraItem(int r, int c) {
  switch (r) {
    case SEC_TEMPORADAS:  return larguraTemporada(c);
    case SEC_EPISODIOS:   return NV_DETP_EP_W;
    case SEC_ABAS_INFO:   return larguraAbaInfo(c);
    default:              return NV_DETP_EL_W;
  }
}
// x do item `c` DENTRO da fileira (antes da rolagem horizontal).
static float xItem(int r, int c) {
  float x = NV_DETP_X;
  for (int k = 0; k < c; k++) {
    if (r == SEC_EPISODIOS) { x += NV_DETP_EP_PASSO; continue; }
    if (r == SEC_ELENCO)    { x += NV_DETP_EL_PASSO; continue; }
    if (r == SEC_TEMPORADAS) x += larguraTemporada(k) + NV_DETP_TEMP_GAP;
    else x += larguraAbaInfo(k) + NV_DETP_ABA_SEP * 2 + 9.0f;  // 9 = largura do "|"
  }
  return x;
}

// Reconta as colunas de cada secao a cada quadro.
//
// O focus_iniciar do detail_abrir congela nColunas com o que EXISTE NA HORA da
// abertura — e os episodios, as temporadas e o elenco chegam DA REDE, segundos
// depois. Com a contagem parada em zero o focus_mover recusa qualquer passo
// lateral (`novo < nColunas[fileira]` nunca passa), que e o defeito relatado:
// "a lista de episodios nao mexe para os lados".
//
// Sai cedo quando nada mudou, entao custa N comparacoes de inteiro. Mesmo
// padrao do sincronizarFileiras() da home, pela mesma razao: quem preenche o
// catalogo e outro fio.
static void sincronizarColunas(void) {
  int r, mudou = 0;
  for (r = 0; r < N_SECOES; r++) {
    int n = secaoN(r);
    if (foco.nColunas[r] != n) { foco.nColunas[r] = n; mudou = 1; }
  }
  if (!mudou) return;
  // A coluna corrente pode ter ficado fora da faixa (a lista encolheu ao trocar
  // de temporada). Puxar para dentro evita desenhar foco em item inexistente.
  if (foco.coluna >= foco.nColunas[foco.fileira])
    foco.coluna = foco.nColunas[foco.fileira] > 0
                ? foco.nColunas[foco.fileira] - 1 : 0;
}

void detail_atualizar(float dt, Uint32 agora) {
  (void)agora;
  if (!aberto) return;
  sincronizarColunas();
  t  = anim_mola(t,  saindo ? 0.0f : 1.0f, dt, NV_MOLA_TELA);
  // Rigidez propria: o web leva 0.8s para apagar o backdrop (cubic-bezier
  // .4,0,.2,1), e a mola de NV_MOLA_TELA assenta em ~330ms.
  pg = anim_mola(pg, nivel >= 1 ? 1.0f : 0.0f, dt, NV_MOLA_PAGINA);
  if (saindo && t < 0.02f) { aberto = 0; saindo = 0; t = 0.0f; return; }

  for (int r = 0; r < N_SECOES; r++)
    for (int c = 0; c < secaoN(r) && c < N_ITENS; c++) {
      float alvo = (nivel >= 1 && focus_indice(&foco, r, c)) ? 1.0f : 0.0f;
      animFoco[r][c] = anim_mola(animFoco[r][c], alvo, dt,
                                 alvo > animFoco[r][c] ? NV_MOLA_FOCO : NV_MOLA_DESFOCO);
    }

  // --- rolagem HORIZONTAL da fileira focada ---------------------------------
  // Duas regras, as duas do fonte do web (`getHorizontalTrackScrollLeft`): a
  // fileira de episodios ENCOSTA o card focado na margem esquerda; as demais so
  // rolam o necessario, com 24px de folga nas bordas.
  { int r = foco.fileira;
    if (r >= 0 && r < N_SECOES && secaoN(r) > 0) {
      float x = xItem(r, foco.coluna) - NV_DETP_X;
      float w = larguraItem(r, foco.coluna);
      float vista = NV_TELA_W - NV_DETP_X * 2;
      float alvo = scrollSec[r];
      if (r == SEC_EPISODIOS) alvo = x;
      else {
        if (foco.coluna == 0) alvo = 0.0f;
        else if (x + w > alvo + vista - 24.0f) alvo = x + w - vista + 24.0f;
        else if (x < alvo + 24.0f)             alvo = x - 24.0f;
      }
      if (alvo < 0.0f) alvo = 0.0f;
      scrollSec[r] = anim_mola(scrollSec[r], alvo, dt, NV_MOLA_SCROLL);
    } }

  // --- rolagem VERTICAL -----------------------------------------------------
  // O topo do grupo focado vai para 33% da altura util (40% nas abas). E a
  // regra do web, e nao um "rola o necessario": conferida nos quatro grupos.
  float alvoY = 0.0f;
  if (nivel >= 1 && foco.fileira >= 0 && foco.fileira < N_SECOES) {
    alvoY = SECOES[foco.fileira].grupo - NV_TELA_H * SECOES[foco.fileira].alvo;
    float maxY = NV_DETP_FIM - NV_TELA_H;
    if (alvoY > maxY) alvoY = maxY;
    if (alvoY < 0.0f) alvoY = 0.0f;
  }
  scrollY = anim_mola(scrollY, alvoY, dt, NV_MOLA_SCROLL);
}

// ---------------------------------------------------------------------------
// HERO
// ---------------------------------------------------------------------------
// Nada aqui e sobreposicao num cartao: a tela e full-bleed, a coluna comeca em
// x=72 e a pilha e ancorada na BASE (`.detail-hero-section` e um flex column
// com `justify-content: flex-end`). Empilhar de cima para baixo faz o bloco
// inteiro subir e descer conforme o tamanho da sinopse; no web ele fica preso
// na base e so o topo se move.
static void desenhaBotao(GfxRect r, const char *rot, int icone, int focado, float a) {
  // Foco no web NAO e escala nem sombra: e um anel branco de 4px por fora
  // (box-shadow 0 0 0 4px #fff) e a troca de cor do fundo. `transform` continua
  // `none` nos dois estados.
  if (focado) {
    GfxRect anel = { r.x - NV_DETW_ANEL, r.y - NV_DETW_ANEL,
                     r.w + NV_DETW_ANEL * 2, r.h + NV_DETW_ANEL * 2 };
    gfx_cor(anel, NV_RAIO_PILL, 1, 1, 1, a);
  }
  int circular = (rot == NULL);
  if (circular) {
    float lum = focado ? 0.961f : 0.133f;   // #f5f5f5 / #222
    gfx_cor(r, NV_RAIO_PILL, lum, lum, lum, a);
    // Os tres glifos do web: biblioteca (+), assistido (olho) e trailer
    // (placa do YouTube). Sao SVG la e nao existem na familia embarcada, entao
    // vem do shader — ver GFX_OLHO e GFX_TRAILER. Antes eram "+" e dois "...",
    // que nao diziam o que os botoes faziam.
    float ic = focado ? 0.067f : 1.0f;      // #111 com foco, branco sem
    float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
    // Proporcao glifo/circulo MEDIDA na referencia do dono (circulo de 96 na
    // captura dele): "+" 0.36, olho 0.45, placa do YouTube 0.69 de largura por
    // 0.48 de altura. Sobre o circulo de 84 desta tela dao 30, 38 e 58x40 — a
    // conta pelo font-size (32) acertava o olho e deixava a placa do YouTube
    // pequena demais, porque no SVG ela e mais larga que a caixa da fonte.
    float g = NV_DETW_CIRC;
    if (icone == 1) {                       // biblioteca: "+"
      // O raio do SDF e fracao da ALTURA: num retangulo 5x44 pedir 0.5 faz
      // `b.x` ficar negativo e a forma colapsa.
      float b = g * 0.36f;
      GfxRect h = { cx - b * 0.5f, cy - 2.5f, b, 5 };
      GfxRect v = { cx - 2.5f, cy - b * 0.5f, 5, b };
      gfx_cor(h, 0.5f, ic, ic, ic, a);
      gfx_cor(v, 0.5f * (v.w / v.h), ic, ic, ic, a);
    } else if (icone == 2) {                // assistido: olho, com risco
      // A caixa e mais larga que alta porque a amendoa e deitada; o shader
      // desenha dentro dela e o risco vem em `parx`.
      GfxRect o = { cx - g * 0.225f, cy - g * 0.16f, g * 0.45f, g * 0.32f };
      gfx_rect(o, 0, GFX_OLHO, 0, 1.0f, 0, 0.0f, ic, ic, ic, a);
    } else {                                // trailer: glifo do YouTube
      GfxRect y = { cx - g * 0.345f, cy - g * 0.24f, g * 0.69f, g * 0.48f };
      gfx_rect(y, 0, GFX_TRAILER, 0, 0, 0, 0.0f, ic, ic, ic, a);
    }
    return;
  }
  // Primario: branco com texto preto nos DOIS estados — o foco so acrescenta o
  // anel de 4px (components.css:17610, `box-shadow: 0 0 0 4px #fff`). A cor de
  // fundo e a do texto sao as mesmas focado ou nao, conferido na regra final
  // (22646): background #ffffff, color #000000.
  //
  // O anel FALTAVA: o bloco de foco existia so no secundario, entao o botao
  // principal nao dava sinal nenhum de estar selecionado.
  if (focado) {
    GfxRect anel = { r.x - NV_DETW_ANEL, r.y - NV_DETW_ANEL,
                     r.w + NV_DETW_ANEL * 2, r.h + NV_DETW_ANEL * 2 };
    gfx_cor(anel, NV_RAIO_PILL, 1, 1, 1, a);
  }
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

// Botao secundario: 345x96, raio 64, fundo #222 e texto branco; focado, fundo
// #f5f5f5 e texto #111, com o mesmo anel de 4px. Nao tem icone — no web e so o
// rotulo, e por isso a largura sai de `texto + 2 x 34` e nao da conta do
// primario.
static void desenhaSecundario(GfxRect r, const char *rot, int focado, float a) {
  if (focado) {
    GfxRect anel = { r.x - NV_DETW_ANEL, r.y - NV_DETW_ANEL,
                     r.w + NV_DETW_ANEL * 2, r.h + NV_DETW_ANEL * 2 };
    gfx_cor(anel, NV_RAIO_PILL, 1, 1, 1, a);
  }
  float lum = focado ? 0.961f : 0.133f;
  gfx_cor(r, NV_RAIO_PILL, lum, lum, lum, a);
  int cor = focado ? 17 : 255;
  TxtLinha l = txt_linha(TXT_DET_BOTAO, rot, cor, cor, cor, 255);
  txt_desenhar_alpha(l, r.x + (r.w - l.w) * 0.5f, r.y + (r.h - l.h) * 0.5f, a);
}

// Largura do botao primario: padding 48 + icone 36 + vao 34 + texto. A conta e
// a do CSS, entao um rotulo maior cresce a pilula em vez de estourar por baixo
// do texto — e e o que faz "Retomar T2E3" (334) e "Reproduzir" (298) baterem.
static float larguraPrimario(const char *rot) {
  TxtLinha l = txt_linha(TXT_DET_BOTAO, rot, 0, 0, 0, 255);
  return NV_DETW_BTN_PADX * 2 + NV_DETW_BTN_ICONE + NV_DETW_BTN_GAPI + l.w;
}
static float larguraSecundario(const char *rot) {
  TxtLinha l = txt_linha(TXT_DET_BOTAO, rot, 255, 255, 255, 255);
  return NV_DETW_BTN2_PADX * 2 + l.w;
}

// Ano solto do campo `meta` ("2025 · 1 h 54 min" -> "2025" e "1 h 54 min").
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

// Selo do IMDb: 109x60, sem fundo — o logo amarelo de 60x60 e a nota a direita,
// em 20.7/400 rgb(179,179,179). O logo do web e `assets/icons/imdb_logo_2016.svg`
// e este app nao empacota SVG (nem daria para acrescentar o arquivo sem
// reinstalar o ipk de 166MB), entao ele e DESENHADO: retangulo amarelo #f5c518
// com "IMDb" preto dentro, que e a forma da marca. Fica anotado como o unico
// ponto do selo que nao e 1:1.
static float desenhaSeloImdb(float xDir, float yCentro, int nota, float a) {
  if (nota <= 0) return 0.0f;
  char txt[8];
  snprintf(txt, sizeof txt, "%.1f", nota / 10.0f);
  TxtLinha l = txt_linha(TXT_CAPTION2, txt, 179, 179, 179, 255);
  float larg = NV_DETW_IMDB_H + 20.0f + l.w;
  float x = xDir - larg;
  GfxRect marca = { x, yCentro - 15.0f, NV_DETW_IMDB_H, 30.0f };
  gfx_cor(marca, 0.14f, 0.961f, 0.773f, 0.094f, a);   // #f5c518
  TxtLinha lm = txt_linha(TXT_MINI, "IMDb", 10, 10, 10, 255);
  txt_peso(lm, marca.x + (marca.w - lm.w) * 0.5f,
           marca.y + (marca.h - lm.h) * 0.5f, a, 0.8f);
  txt_desenhar_alpha(l, xDir - l.w, yCentro - l.h * 0.5f, a);
  return larg;
}

// Selo de texto da segunda linha de meta ("RETURNING SERIES", classificacao
// indicativa): 45 de altura, raio 8, SEM fundo, com borda de 1px
// rgba(179,179,179,0.55) e texto branco 23/400. A borda e desenhada como um
// retangulo de fundo 1px maior, porque o shader nao tem contorno.
static float desenhaSeloMeta(float x, float y, const char *txt, float a) {
  TxtLinha l = txt_linha(TXT_DET_META2, txt, 255, 255, 255, 255);
  float w = l.w + NV_DETW_SELO_PADX * 2;
  float raio = 8.0f / NV_DETW_SELO_H;
  GfxRect borda = { x - 1, y - 1, w + 2, NV_DETW_SELO_H + 2 };
  gfx_cor(borda, raio, 0.70f, 0.70f, 0.70f, 0.55f * a);
  GfxRect dentro = { x, y, w, NV_DETW_SELO_H };
  gfx_cor(dentro, raio, 0.051f, 0.051f, 0.051f, 0.0f);   // furo: so a borda
  txt_desenhar_alpha(l, x + NV_DETW_SELO_PADX,
                     y + (NV_DETW_SELO_H - l.h) * 0.5f, a);
  return w;
}

// O ponto separador do web: 1x14 em rgba(179,179,179,0.55), com 38 de folga de
// cada lado (medido borda a borda em seis ocorrencias).
static void desenhaPonto(float x, float yCentro, float a) {
  GfxRect pt = { x, yCentro - 7.0f, 1, 14 };
  gfx_cor(pt, 0.0f, 0.70f, 0.70f, 0.70f, 0.55f * a);
}

static void heroWeb(float a, float desloc) {
  if (a <= 0.005f) return;
  const CatItem *ci = cat_item(idx);

  char ano[32], dur[64];
  partirMeta(fichaDe(idx), ano, sizeof ano, dur, sizeof dur);

  // Em serie o web escreve "Roteirista:"/"Criador:"; em filme, "Diretor:".
  char sup[192] = "";
  if (ci && ci->direcao[0])
    snprintf(sup, sizeof sup, "%s: %s", ehSerie() ? "Roteirista" : "Diretor",
             ci->direcao);

  const char *sin = sinopseDe(idx);

  // --- empilhamento de BAIXO para cima, como o flex-end do web ---------------
  float yMeta2 = NV_DETW_BASE - NV_DETW_SELO_H - 14.0f;       // 989
  float yMeta1 = yMeta2 - NV_DETW_META_GAP - 74.0f;           // 889
  float hSin = 0.0f;
  if (sin) hSin = txt_bloco(TXT_DET_SIN, sin, 255, 255, 255, -1.0f, 0.0f,
                            NV_DETW_TEXTO_W, NV_DETW_LD_SIN, 0.0f,
                            NV_DETW_SIN_LINHAS);
  float ySin = yMeta1 - NV_DETW_GAP_SIN - hSin;              // 748
  float ySup = sup[0] ? ySin - NV_DETW_GAP_SUP - NV_DETW_LD_SUP : ySin;  // 688
  // A linha de retomada continua ligada ao PROGRESSO, nao ao botao que saiu.
  float temRetom = (ci && ci->progresso > 0) ? 1.0f : 0.0f;
  float yRetom = ySup - NV_DETW_GAP_SUP - NV_DETW_RETOM_H;
  float yAcoes = (temRetom ? yRetom - NV_DETW_GAP_RETOM
                           : ySup - NV_DETW_GAP_ACOES) - NV_DETW_ACOES_H;

  // Sobe alguns pixels enquanto entra: continua o movimento da arte em vez de
  // aparecer pronto no lugar. `desloc` e a rolagem do documento.
  float sobe = (1.0f - a) * 26.0f + desloc;
  yMeta2 += sobe; yMeta1 += sobe; ySin += sobe; ySup += sobe;
  yRetom += sobe; yAcoes += sobe;

  // --- logo -----------------------------------------------------------------
  const char *arqLogo = logoDe(idx);
  // O logo e desenhado com 261 de largura mas a arte de origem costuma vir bem
  // maior; o teto de 960 ja bastaria, mas quando a mesma arte tambem serve ao
  // hero o item e promovido — por isso passa pelo mesmo caminho.
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
    // linha de botoes nao pule entre um titulo com logo e outro sem.
    TxtLinha t2 = txt_linha_corta(TXT_TITULO1, tituloDe(idx), 255, 255, 255, 255,
                                  NV_DETW_LOGO_MAXW);
    txt_desenhar_alpha(t2, NV_DETW_X,
                       yAcoes - NV_DETW_LOGO_GAP - NV_DETW_LOGO_H
                              + (NV_DETW_LOGO_H - t2.h) * 0.5f, a);
  }

  // --- botoes ---------------------------------------------------------------
  // Em FLUXO, com 63px entre vizinhos. As posicoes x=439/586/734 que o arquivo
  // de medidas trazia nao sao constantes do desenho: sao o resultado dessa
  // conta com o rotulo "Reproduzir" e SEM o botao secundario.
  //
  // O web tem ate tres circulares (lista, "assistido", trailer). Aqui sao DOIS:
  // lista e fontes. O de trailer nao entra porque este app nao tem reprodutor
  // de trailer, e um botao que nao faz nada e pior que a ausencia dele.
  float yBtn  = yAcoes + 6.0f;
  float yCirc = yAcoes + 12.0f;
  // Dois estados do rotulo, medidos: "Retomar T2E3" quando ha progresso,
  // "Reproduzir" quando nao ha. (O web tem um terceiro, "Proximo T2E4", que sai
  // do proximo episodio nao assistido — o catalogo nativo nao guarda quais
  // episodios ja foram vistos, entao esse estado nao tem de onde vir.)
  char rot[48];
  if (ci && ci->progresso > 0 && ci->temporada > 0)
    snprintf(rot, sizeof rot, "Retomar T%dE%d", ci->temporada, ci->episodio);
  else if (ci && ci->progresso > 0) snprintf(rot, sizeof rot, "Retomar");
  else snprintf(rot, sizeof rot, "Reproduzir");

  int nb = 0;
  float bx = NV_DETW_X + 6.0f;
  GfxRect rp = { bx, yBtn, larguraPrimario(rot), NV_DETW_BTN_H };
  desenhaBotao(rp, rot, 0, nivel == 0 && botao == nb, a);
  bx += rp.w + NV_DETW_BTN_GAP; nb++;
  // TRES circulares, como na referencia: adicionar a lista, marcar como visto e
  // trailer. Antes eram dois, e entre eles cabia o botao de texto que saiu.
  for (int k = 0; k < 3; k++, nb++) {
    GfxRect rc = { bx, yCirc, NV_DETW_CIRC, NV_DETW_CIRC };
    desenhaBotao(rc, NULL, k + 1, nivel == 0 && botao == nb, a);
    bx += NV_DETW_CIRC + NV_DETW_BTN_GAP;
  }

  // --- linha de retomada ----------------------------------------------------
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

  // --- "Roteirista: ..." / "Diretor: ..." ------------------------------------
  if (sup[0]) {
    TxtLinha l = txt_linha_corta(TXT_DET_META, sup, 179, 179, 179, 255,
                                 NV_DETW_TEXTO_W);
    txt_desenhar_alpha(l, NV_DETW_X, ySup + (NV_DETW_LD_SUP - l.h) * 0.5f, a);
  }

  // --- sinopse --------------------------------------------------------------
  if (sin) txt_bloco(TXT_DET_SIN, sin, 255, 255, 255, NV_DETW_X, ySin,
                     NV_DETW_TEXTO_W, NV_DETW_LD_SIN, a, NV_DETW_SIN_LINHAS);

  // --- meta 1: generos a esquerda; ano e selo IMDb empurrados a direita ------
  // A linha cresce de 49 para 74 de altura quando o selo IMDb existe — foi ele
  // que empurrou a pilha inteira para cima na sessao logada.
  {
    float yc = yMeta1 + 37.0f;
    float xDir = NV_DETW_DIR;
    float usado = desenhaSeloImdb(xDir, yc, ci ? ci->nota : 0, a);
    if (usado > 0.0f) {
      xDir -= usado + NV_DETP_SEP;
      desenhaPonto(xDir, yc, a);
      xDir -= NV_DETP_SEP;
    }
    if (ano[0]) {
      TxtLinha la = txt_linha(TXT_DET_META, ano, 179, 179, 179, 255);
      txt_desenhar_alpha(la, xDir - la.w, yc - la.h * 0.5f, a);
      xDir -= la.w + NV_DETP_SEP;
      desenhaPonto(xDir, yc, a);
      xDir -= NV_DETP_SEP;
    }
    TxtLinha lg = txt_linha_corta(TXT_DET_META, generoDe(idx), 179, 179, 179, 255,
                                  xDir - NV_DETW_X);
    txt_desenhar_alpha(lg, NV_DETW_X, yc - lg.h * 0.5f, a);
  }

  // --- meta 2: selo, duracao, pais ------------------------------------------
  // O web abre a linha com o selo de STATUS ("RETURNING SERIES") e/ou o de
  // classificacao indicativa. Status nao existe no CatItem — o catalogo nativo
  // nao traz esse campo do Cinemeta —, entao aqui o selo e o de classificacao,
  // que existe e ocupa o mesmo lugar na mesma linha do web.
  {
    float x = NV_DETW_X, yc = yMeta2 + NV_DETW_SELO_H * 0.5f;
    if (ci && ci->classificacao[0]) {
      x += desenhaSeloMeta(x, yMeta2, ci->classificacao, a) + NV_DETP_SEP;
      desenhaPonto(x, yc, a);
      x += NV_DETP_SEP;
    }
    if (dur[0]) {
      TxtLinha ld = txt_linha(TXT_DET_META2, dur, 255, 255, 255, 255);
      txt_desenhar_alpha(ld, x, yc - ld.h * 0.5f, a);
    }
  }
}

// ---------------------------------------------------------------------------
// PAGINA: temporadas, episodios, abas de informacao, elenco
// ---------------------------------------------------------------------------

// Numero REAL da temporada na posicao `c`. Serie que comeca na 2 (o que
// acontece quando o Cinemeta nao tem a 1) mostrava "Temporada 1" apontando para
// a 2, e a lista abaixo nao batia com o rotulo.
static int temporadaEm(int c) {
  const CatItem *ci = cat_item(idx);
  if (ci && ci->nTemporadas > 0)
    return (c >= 0 && c < ci->nTemporadas) ? ci->temporadas[c] : ci->temporadas[0];
  return c + 1;
}
static void rotuloTemporada(int c, char *dst, size_t n) {
  int s = temporadaEm(c);
  if (s == 0) snprintf(dst, n, "Especiais");
  else snprintf(dst, n, "Temporada %d", s);
}
static float larguraTemporada(int c) {
  char rot[32]; rotuloTemporada(c, rot, sizeof rot);
  TxtLinha l = txt_linha(TXT_PLR_CORPO, rot, 255, 255, 255, 255);
  return l.w + NV_DETP_TEMP_PADX * 2;
}
static float larguraAbaInfo(int i) {
  TxtLinha l = txt_linha(TXT_PLR_CORPO, ABA_ROTULO[abaIdDe(i)], 255, 255, 255, 255);
  return l.w;
}

// Aba de temporada: 80 de altura, raio 40 (pilula), borda de 1px
// rgba(255,255,255,0.16). Tres estados MEDIDOS, e nao dois:
//   normal      #222     texto rgb(179,179,179)
//   escolhida   #2d2d2d  texto branco
//   com foco    #f5f5f5  texto #111, sem borda
// Sem o estado do meio, o usuario perde de vista em que temporada esta assim
// que o foco desce para a lista.
static void desenhaTemporada(GfxRect r, int c, float f, float a) {
  char rot[32]; rotuloTemporada(c, rot, sizeof rot);
  int sel = (c == temporada);
  float raio = NV_RAIO_PILL;
  GfxRect borda = { r.x - 1, r.y - 1, r.w + 2, r.h + 2 };
  gfx_cor(borda, raio, 1, 1, 1, 0.16f * a * (1.0f - f));
  float base = sel ? 0.176f : 0.133f;         // #2d2d2d / #222
  float lum  = base + (0.961f - base) * f;    // -> #f5f5f5 com foco
  gfx_cor(r, raio, lum, lum, lum, a);
  int alvo = sel ? 255 : 179;
  int cor = (int)(alvo + (17 - alvo) * f);    // -> #111 com foco
  TxtLinha l = txt_linha(TXT_PLR_CORPO, rot, cor, cor, cor, 255);
  // 500 de peso na Inter Regular: uma segunda passada meio pixel a direita.
  txt_peso(l, r.x + (r.w - l.w) * 0.5f, r.y + (r.h - l.h) * 0.5f, a, 0.5f);
}

// O degrade do `.series-episode-overlay`: linear vertical de rgba(0,0,0,0.06)
// a 0.95, com paradas em 22% (0.18), 52% (0.62) e 82% (0.86). O shader nao tem
// modo para ele — gfx.c e arquivo de outro agente — e o GFX_VEU que existe
// escurece TAMBEM a esquerda, o que aqui apagaria a metade do card.
//
// Sai em faixas ancoradas na BASE: cada faixa e um retangulo arredondado que
// vai de uma altura ate o fim da miniatura, com o mesmo raio absoluto. Assim os
// cantos de baixo acompanham a miniatura (uma faixa de cantos retos poria dois
// dentes escuros fora do arredondamento) e o empilhamento reproduz a rampa,
// porque compor N camadas de alfa `d` da 1-(1-d)^n.
static void veuEpisodio(GfxRect th, float a) {
  static const float PARADA[5] = { 0.00f, 0.22f, 0.52f, 0.82f, 1.00f };
  static const float ALFA[5]   = { 0.06f, 0.18f, 0.62f, 0.86f, 0.95f };
  const int PASSOS = 14;
  float acum = 0.0f;
  for (int i = 0; i <= PASSOS; i++) {
    float u = (float)i / PASSOS;
    // Alvo interpolado linearmente por partes, como o `linear-gradient`.
    float alvo = ALFA[4];
    for (int k = 0; k < 4; k++)
      if (u <= PARADA[k + 1]) {
        float d = PARADA[k + 1] - PARADA[k];
        alvo = ALFA[k] + (ALFA[k + 1] - ALFA[k]) * (d > 0 ? (u - PARADA[k]) / d : 0);
        break;
      }
    // Quanto ESTA faixa precisa acrescentar para que o acumulado bata no alvo.
    float d = (alvo - acum) / (1.0f - acum);
    if (d <= 0.001f) continue;
    acum = alvo;
    float topo = th.y + th.h * u;
    GfxRect faixa = { th.x, topo, th.w, th.y + th.h - topo };
    if (faixa.h < 2.0f) continue;
    float raio = NV_DETP_EP_RAIO / (faixa.w < faixa.h ? faixa.w : faixa.h);
    if (raio > 0.5f) raio = 0.5f;
    gfx_cor(faixa, raio, 0, 0, 0, d * a);
  }
}

// Card de episodio: 640x422, com a miniatura de 640x414 e TODO o texto dentro
// dela, sobre o degrade. E a diferenca estrutural com o que estava aqui antes
// (miniatura em cima, texto embaixo, que e o app da Apple TV).
static void desenhaEpisodio(GfxRect r, int c, float f, float a, Uint32 agora) {
  (void)agora;
  const CatEp *ep = cat_episodio(idx, c);
  GfxRect th = { r.x, r.y, r.w, NV_DETP_EP_THUMB_H };
  float raioTh = NV_DETP_EP_RAIO / NV_DETP_EP_THUMB_H;

  // Anel de foco: no web e um box-shadow na MINIATURA, nao no card, e nao ha
  // escala nenhuma (`transform: none`).
  if (f > 0.01f) {
    GfxRect anel = { th.x - NV_DETP_ANEL, th.y - NV_DETP_ANEL,
                     th.w + NV_DETP_ANEL * 2, th.h + NV_DETP_ANEL * 2 };
    gfx_cor(anel, raioTh, 1, 1, 1, f * a);
  }

  const char *arte = (ep && ep->thumb[0]) ? ep->thumb : home_arte(c * 2 + 3);
  GLuint t2 = arte ? tex_obter(arte) : 0;
  if (t2) {
    gfx_tex_aspect_atual = tex_aspecto(arte);
    gfx_rect(th, t2, GFX_CARD, 0, 0, 0, raioTh, 0, 0, 0, a);
    gfx_tex_aspect_atual = 0.0f;
  } else gfx_cor(th, raioTh, 0.133f, 0.133f, 0.133f, a);
  veuEpisodio(th, a);

  int e = c % (int)(sizeof EPISODIOS / sizeof *EPISODIOS);
  const char *epNome = ep && ep->nome[0] ? ep->nome : EPISODIOS[e][0];
  const char *epDur  = ep && ep->duracao[0] ? ep->duracao : EPISODIOS[e][1];
  const char *epData = ep && ep->data[0] ? ep->data : EPISODIOS[e][2];
  const char *epSin  = ep && ep->sinopse[0] ? ep->sinopse : SIN_EP;
  int epNum = ep ? ep->episodio : c + 1;

  float tx = r.x + NV_DETP_EP_PAD;

  // O circulo TRACEJADO de "ainda nao assistido" (48x48 em (24,24), borda de
  // 2px rgba(179,179,179,0.9)) NAO e desenhado. Um anel exige furar o meio, e o
  // shader so sabe preencher: a primeira tentativa saiu um disco cinza chapado
  // no canto da miniatura, que le como defeito e nao como selo. O meio nao pode
  // ser pintado da cor do fundo porque ali o veu esta em 0.06 — o que aparece
  // atras e a propria arte do episodio. Fica de fora ate gfx.c ganhar um modo
  // de anel, e gfx.c e arquivo de outro agente nesta sessao.

  // Selo "EPISODIO n": 163x44, raio 12, fundo rgba(0,0,0,0.42), 20/600 com
  // caixa alta.
  { char cab[24]; snprintf(cab, sizeof cab, "EPISODIO %d", epNum);
    TxtLinha l = txt_linha(TXT_CAPTION2, cab, 255, 255, 255, 255);
    float w = l.w + NV_DETP_EP_SELO_PADX * 2;
    GfxRect s = { tx, r.y + NV_DETP_EP_SELO_Y, w, NV_DETP_EP_SELO_H };
    gfx_cor(s, 12.0f / NV_DETP_EP_SELO_H, 0, 0, 0, 0.42f * a);
    txt_peso(l, s.x + NV_DETP_EP_SELO_PADX,
             s.y + (NV_DETP_EP_SELO_H - l.h) * 0.5f, a, 1.0f); }

  // Titulo: 32/800. O 800 nao existe na familia embarcada e o 32 so existe em
  // Regular na tabela de estilos, entao vem de tres passadas.
  { TxtLinha l = txt_linha_corta(TXT_PLR_CORPO, epNome, 255, 255, 255, 255,
                                 NV_DETP_EP_TEXTO_W);
    txt_peso(l, tx, r.y + NV_DETP_EP_TIT_Y, a, 1.4f); }

  // Sinopse: 28/400 em rgba(255,255,255,0.9), duas linhas.
  txt_bloco(TXT_DET_SIN, epSin, 255, 255, 255, tx, r.y + NV_DETP_EP_SIN_Y,
            NV_DETP_EP_TEXTO_W, NV_DETP_EP_LD_SIN, a * 0.9f, 2);

  // Meta: relogio + duracao + data, 20/400 rgb(179,179,179), com 38 de folga
  // entre os dois blocos.
  { float x = tx, y = r.y + NV_DETP_EP_META_Y;
    // Relogio de 28x28. O glifo do web e um disco CHEIO em rgb(179,179,179) com
    // os ponteiros VAZADOS — o `path` do SVG recorta o L do ponteiro do disco.
    // Aqui o vazado sai pintando os ponteiros de preto por cima: naquele ponto
    // da miniatura o veu ja esta em 0.95, entao o que estaria atras do recorte e
    // praticamente preto. Desenhar os ponteiros na MESMA cor do disco, como
    // estava, some com eles e deixa so uma bolinha cinza.
    float cx = x + NV_DETP_EP_ICONE * 0.5f, cy = y + NV_DETP_EP_ICONE * 0.5f;
    GfxRect aro = { x, y, NV_DETP_EP_ICONE, NV_DETP_EP_ICONE };
    gfx_cor(aro, 0.5f, 0.70f, 0.70f, 0.70f, a);
    GfxRect pv = { cx - 1.5f, cy - 8, 3, 9.5f };
    GfxRect ph = { cx - 1.5f, cy - 1.5f, 8, 3 };
    gfx_cor(pv, 0.0f, 0.0f, 0.0f, 0.0f, 0.92f * a);
    gfx_cor(ph, 0.0f, 0.0f, 0.0f, 0.0f, 0.92f * a);
    x += NV_DETP_EP_ICONE + 8.0f;
    TxtLinha ld = txt_linha(TXT_CAPTION2, epDur, 179, 179, 179, 255);
    txt_desenhar_alpha(ld, x, y, a);
    x += ld.w + NV_DETP_SEP;
    TxtLinha lf = txt_linha(TXT_CAPTION2, epData, 179, 179, 179, 255);
    txt_desenhar_alpha(lf, x, y, a); }

  // Barra de progresso: 576x8 a 16px da base da miniatura, trilho
  // rgba(0,0,0,0.45) e preenchimento rgb(158,158,158). So aparece entre 2% e
  // 98% — e o mesmo intervalo do web, e e o que faz um episodio recem-comecado
  // nao ganhar uma barra de largura zero.
  { int prog = 0;
    const CatItem *ci = cat_item(idx);
    if (ci && ci->progresso > 0 && ep && ci->temporada == ep->temporada &&
        ci->episodio == ep->episodio) prog = ci->progresso;
    if (prog > 2 && prog < 98) {
      GfxRect tr = { tx, r.y + NV_DETP_EP_BARRA_Y, NV_DETP_EP_TEXTO_W,
                     NV_DETP_EP_BARRA_H };
      GfxRect at = { tr.x, tr.y, tr.w * (prog / 100.0f), tr.h };
      gfx_cor(tr, 0.5f, 0, 0, 0, 0.45f * a);
      gfx_cor(at, 0.5f, 0.62f, 0.62f, 0.62f, a);
    } }
}

// Abas de informacao: texto puro, sem pilula. Escolhida (ou focada) em branco,
// as outras em #808080; o divisor "|" e 32/700 #808080. O foco no web e
// `transform: scale(1.03)` — o unico lugar desta tela que escala.
static void desenhaAbaInfo(float x, float y, int i, float f, float a) {
  int sel = (i == abaInfo);
  int base = sel ? 255 : 128;
  int cor = (int)(base + (255 - base) * f);
  TxtLinha l = txt_linha(TXT_PLR_CORPO, ABA_ROTULO[abaIdDe(i)], cor, cor, cor, 255);
  txt_peso(l, x, y + (NV_DETP_ABA_H - l.h) * 0.5f, a, 0.5f + f * 0.6f);
}

// Elenco: avatar redondo de 140 ALINHADO A ESQUERDA do card de 220 (nao
// centralizado, que era o desenho anterior), nome 26/500 rgb(179,179,179) e
// papel 21/400 rgb(128,128,128) abaixo dele.
static void desenhaElenco(float x, float y, int c, float f, float a) {
  const CatItem *ci = cat_item(idx);
  const char *nome = NULL, *papel = NULL, *foto = NULL;
  if (ci && c < ci->nElenco) {
    nome = ci->elenco[c].nome;
    papel = ci->elenco[c].papel;
    if (ci->elenco[c].foto[0]) foto = ci->elenco[c].foto;
  }
  if (ci && ci->nElenco > 0 && c >= ci->nElenco) return;
  if (!nome) { int e = c % N_ELENCO; nome = ELENCO[e][0]; papel = ELENCO[e][1]; }

  GfxRect av = { x, y, NV_DETP_EL_AVATAR, NV_DETP_EL_AVATAR };
  if (f > 0.01f) {
    GfxRect anel = { av.x - NV_DETP_ANEL, av.y - NV_DETP_ANEL,
                     av.w + NV_DETP_ANEL * 2, av.h + NV_DETP_ANEL * 2 };
    gfx_cor(anel, 0.5f, 1, 1, 1, f * a);
  }
  GLuint t2 = foto ? tex_obter(foto) : 0;
  if (t2) {
    gfx_tex_aspect_atual = tex_aspecto(foto);
    gfx_rect(av, t2, GFX_CARD, 0, 0, 0, 0.5f, 0, 0, 0, a);
    gfx_tex_aspect_atual = 0.0f;
  } else {
    // Sem foto, a inicial sobre #222 (#303030 com foco) — e o que o web faz
    // com `.movie-cast-avatar-fallback`.
    float lum = 0.133f + 0.055f * f;
    gfx_cor(av, 0.5f, lum, lum, lum, a);
    char ini[5] = {0};
    for (int k = 0; k < 4 && nome[k] && (unsigned char)nome[k] >= 0x20; k++) {
      ini[k] = nome[k];
      if ((nome[k] & 0xC0) != 0x80) { if (k) { ini[k] = 0; break; } }
    }
    TxtLinha li = txt_linha(TXT_TITULO3, ini, 210, 212, 220, 255);
    txt_desenhar_alpha(li, av.x + (av.w - li.w) * 0.5f,
                       av.y + (av.h - li.h) * 0.5f, a * 0.9f);
  }
  float yn = y + NV_DETP_EL_AVATAR + NV_DETP_EL_NOME_DY;
  TxtLinha ln = txt_linha_corta(TXT_CALLOUT, nome, 179, 179, 179, 255, NV_DETP_EL_W);
  txt_desenhar_alpha(ln, x, yn, a);
  if (papel && papel[0]) {
    TxtLinha lp = txt_linha_corta(TXT_CAPTION2, papel, 128, 128, 128, 255,
                                  NV_DETP_EL_W);
    txt_desenhar_alpha(lp, x, yn + NV_DETP_EL_PAPEL_DY, a * 0.95f);
  }
}

// Aba "Avaliacoes". No web (metaDetailsScreen.js:3699) sao dois cartoes lado a
// lado, IMDb e TMDB: .movie-rating-card de 160x120, raio 14, fundo
// rgba(18,23,31,.9) com borda de 1px a 16%, logo de 56x28 em cima e o valor em
// 34/800 embaixo; quando o dado falta o cartao mostra "-".
//
// DIVERGENCIA ANOTADA: em SERIE o web troca isto por um painel de avaliacoes
// POR EPISODIO (renderSeriesRatingsPanel), com seletor de temporada. Este port
// nao tem nota por episodio em fonte nenhuma — o Cinemeta nao devolve — entao
// serie mostra os mesmos dois cartoes do filme. Nao e a tela do web; e o que o
// dado permite, e mostrar dois cartoes certos e melhor que uma grade vazia.
#define AVAL_CARD_W  160.0f
#define AVAL_CARD_H  120.0f
#define AVAL_CARD_GAP 16.0f
static void cartaoNota(float x, float y, const char *fonte, const char *valor,
                       float a) {
  GfxRect card = { x, y, AVAL_CARD_W, AVAL_CARD_H };
  gfx_cor(card, 14.0f, 0.071f, 0.090f, 0.122f, 0.90f * a);
  // Borda de 1px a 16%: quatro faixas, que e como o resto desta tela desenha
  // contorno (nao ha helper de borda no gfx).
  float b = 0.16f * a;
  gfx_cor((GfxRect){ x, y, AVAL_CARD_W, 1 }, 0, 1, 1, 1, b);
  gfx_cor((GfxRect){ x, y + AVAL_CARD_H - 1, AVAL_CARD_W, 1 }, 0, 1, 1, 1, b);
  gfx_cor((GfxRect){ x, y, 1, AVAL_CARD_H }, 0, 1, 1, 1, b);
  gfx_cor((GfxRect){ x + AVAL_CARD_W - 1, y, 1, AVAL_CARD_H }, 0, 1, 1, 1, b);

  // O SVG das duas marcas nao esta empacotado; o nome em maiusculas ocupa o
  // mesmo lugar do logo de 56x28 e diz a mesma coisa.
  TxtLinha lf = txt_linha(TXT_CAPTION2, fonte, 200, 205, 214, 255);
  TxtLinha lv = txt_linha(TXT_TITULO3, valor, 245, 248, 255, 255);
  float hBloco = lf.h + 8.0f + lv.h;
  float yb = y + (AVAL_CARD_H - hBloco) * 0.5f;
  txt_desenhar_alpha(lf, x + (AVAL_CARD_W - lf.w) * 0.5f, yb, a * 0.9f);
  txt_desenhar_alpha(lv, x + (AVAL_CARD_W - lv.w) * 0.5f, yb + lf.h + 8.0f, a);
}

static void desenhaAvaliacoes(float x, float y, float a) {
  char imdb[8] = "-", trakt[8] = "-";
  int n = notaDe(idx), t = extras_nota_trakt();
  if (n > 0) snprintf(imdb, sizeof imdb, "%.1f", n / 10.0f);
  if (t > 0) snprintf(trakt, sizeof trakt, "%.1f", t / 10.0f);
  cartaoNota(x, y, "IMDb", imdb, a);
  // Trakt entra na MESMA fileira, como no renderExternalRatingsRow do web
  // (metaDetailsScreen.js:3410), que lista trakt, imdb, tmdb e o resto lado a
  // lado. O TMDB continua "-": a nota dele nao vem no catalogo.
  cartaoNota(x + (AVAL_CARD_W + AVAL_CARD_GAP), y, "Trakt", trakt, a);
  cartaoNota(x + (AVAL_CARD_W + AVAL_CARD_GAP) * 2, y, "TMDB", "-", a);
}

// Aba "Mais como este": /related do Trakt. Uma coluna de titulos com o ano, e
// nao os posteres do web — o related do Trakt devolve identificador e nome, e
// buscar poster para doze titulos so para pintar esta aba custaria doze
// pedidos de rede a cada abertura. O que a aba precisa responder e "o que mais
// se parece com isto", e o nome responde.
static void desenhaRelacionados(float x, float y, float a) {
  int n = extras_n_relacionados(), i;
  for (i = 0; i < n && i < 8; i++) {
    float yl = y + i * 52.0f;
    TxtLinha lt = txt_linha_corta(TXT_DET_META, extras_relacionado_titulo(i),
                                  235, 238, 245, 255, 900.0f);
    txt_desenhar_alpha(lt, x, yl, a);
    { const char *ano = extras_relacionado_ano(i);
      if (ano[0]) {
        TxtLinha la = txt_linha(TXT_DET_META2, ano, 150, 154, 163, 255);
        txt_desenhar_alpha(la, x + lt.w + 18.0f, yl + 2.0f, a * 0.9f);
      } }
  }
}

// Aba "Comentarios": /comments/likes do Trakt, os mais curtidos primeiro. Uma
// linha com o usuario e as curtidas, e o texto quebrado embaixo.
static void desenhaComentarios(float x, float y, float a) {
  int n = extras_n_comentarios(), i;
  float yl = y;
  // TRES e nao quatro: com quatro o cabecalho do ultimo cabia na tela e o texto
  // dele nao, e sobrava um "usuario / N curtidas" solto no rodape.
  for (i = 0; i < n && i < 3; i++) {
    char cab[80];
    snprintf(cab, sizeof cab, "%s   %d curtidas", extras_comentario_usuario(i),
             extras_comentario_curtidas(i));
    { TxtLinha lc = txt_linha(TXT_DET_META2, cab, 150, 154, 163, 255);
      txt_desenhar_alpha(lc, x, yl, a * 0.9f); }
    yl += 34.0f;
    yl += txt_bloco(TXT_DET_META, extras_comentario_texto(i), 226, 230, 238,
                    x, yl, 1100.0f, 34.0f, a, 2);
    yl += 26.0f;
  }
}

static void desenhaSecao(int r, float a, Uint32 agora) {
  int n = secaoN(r);
  // Aba de informacao que nao seja "Criador e elenco": o web TROCA o conteudo
  // da secao (avaliacoes por episodio, fileira de similares, trailer). Nenhum
  // desses dados existe no catalogo nativo, e o web mostra exatamente esta
  // linha quando o dado falta (`.series-insight-empty`).
  //
  // Trocar, e nao sobrepor: na primeira captura do aparelho a mensagem saia POR
  // CIMA dos avatares do elenco, e as duas coisas ficavam ilegiveis.
  { int aba = abaIdDe(abaInfo);
    float yAba = NV_DETP_EL_Y - scrollY + 40.0f;
    if (r == SEC_ELENCO && aba == ABA_AVALIACOES) {
      desenhaAvaliacoes(NV_DETP_X, yAba, a); return;
    }
    if (r == SEC_ELENCO && aba == ABA_RELACIONADOS) {
      desenhaRelacionados(NV_DETP_X, yAba, a); return;
    }
    if (r == SEC_ELENCO && aba == ABA_COMENTARIOS) {
      desenhaComentarios(NV_DETP_X, yAba, a); return;
    } }
  if (n <= 0) return;
  float y;
  switch (r) {
    case SEC_TEMPORADAS: y = NV_DETP_TEMP_Y; break;
    case SEC_EPISODIOS:  y = NV_DETP_EP_Y;   break;
    case SEC_ABAS_INFO:  y = NV_DETP_ABA_Y;  break;
    default:             y = NV_DETP_EL_Y;   break;
  }
  y -= scrollY;
  float alt = (r == SEC_EPISODIOS) ? NV_DETP_EP_H
            : (r == SEC_ELENCO)    ? 257.0f
            : NV_DETP_TEMP_H;
  if (y > NV_TELA_H || y + alt < -40.0f) return;

  for (int c = 0; c < n && c < N_ITENS; c++) {
    float f = animFoco[r][c];
    float x = xItem(r, c) - scrollSec[r];
    float w = larguraItem(r, c);
    if (x > NV_TELA_W || x + w < -w) continue;
    switch (r) {
      case SEC_TEMPORADAS: {
        GfxRect b = { x, y, w, NV_DETP_TEMP_H };
        desenhaTemporada(b, c, f, a); break;
      }
      case SEC_EPISODIOS: {
        GfxRect b = { x, y, NV_DETP_EP_W, NV_DETP_EP_H };
        desenhaEpisodio(b, c, f, a, agora); break;
      }
      case SEC_ABAS_INFO: {
        desenhaAbaInfo(x, y, c, f, a);
        if (c + 1 < n) {
          TxtLinha d = txt_linha(TXT_PLR_CORPO, "|", 128, 128, 128, 255);
          txt_peso(d, x + w + NV_DETP_ABA_SEP,
                   y + (NV_DETP_ABA_H - d.h) * 0.5f, a, 1.4f);
        }
        break;
      }
      default: desenhaElenco(x, y, c, f, a); break;
    }
  }

}

void detail_desenhar(Uint32 agora) {
  if (!aberto) return;
  float s = suave(t), a2 = fase2();

  if (!detail_cobre_tela()) {
    GfxRect tela = { 0, 0, NV_TELA_W, NV_TELA_H };
    gfx_cor(tela, 0.0f, 0.051f, 0.051f, 0.051f, s);   // #0d0d0d, o fundo do web
  }
  gfx_sem_recorte();

  // --- backdrop full-bleed --------------------------------------------------
  // A tela de detalhe e uma imagem de 1920x1080 em (0,0) com a vinheta por cima;
  // nao ha cartao, nem moldura, nem titulos vizinhos.
  //
  // O BACKDROP NAO CRESCE A PARTIR DO CARD. Era o ultimo resto do voo do app da
  // Apple: o retangulo saia de item.rect e se abria ate a tela. O dono descreveu
  // o comportamento certo — "so os posters descem e mantem o background, e o
  // background e a arte do filme selecionado" — e voar o retangulo e o oposto
  // disso: a arte entra pequena e cresce, em vez de ja estar la.
  //
  // Agora a arte ocupa a tela desde o primeiro quadro e so ganha opacidade. Quem
  // se move sao as fileiras da home, que descem (ver home_desenhar, que le o
  // detail_progresso).
  // O FUNDO NAO TROCA: ele CONTINUA. O hero da home ja mostrava a arte deste
  // mesmo titulo, entao o backdrop do detalhe nasce no rect exato em que ela
  // estava e cresce dali ate a tela cheia, sem piscar e sem crossfade — com o
  // hero em tela cheia os dois rects sao praticamente o mesmo e o olho nao ve
  // movimento nenhum, so o texto se rearranjando. Antes a arte entrava do zero
  // ganhando opacidade sobre a arte identica que ja estava la, o que dava um
  // clarao no meio da transicao.
  GfxRect cheia = { 0, 0, NV_TELA_W, NV_TELA_H };
  GfxRect de;
  home_hero_rect(&de.x, &de.y, &de.w, &de.h);
  GfxRect alvo = { de.x + (cheia.x - de.x) * s, de.y + (cheia.y - de.y) * s,
                   de.w + (cheia.w - de.w) * s, de.h + (cheia.h - de.h) * s };
  const char *arte = arteDe(idx);
  // Backdrop em tela cheia: pede o teto de 1920. Com o teto comum de 960 a arte
  // era decodificada com metade da resolucao e ampliada ao dobro na tela.
  GLuint tex = arte ? tex_obter_hero(arte) : 0;
  // Ao rolar, o web NAO desfoca a arte: ele a APAGA. Medido em
  // `.series-detail-shell.detail-scrolled` — o backdrop vai a `opacity: 0.15` e
  // a vinheta a 0, ambos em 0.8s cubic-bezier(.4,0,.2,1).
  if (pg > 0.01f) {
    GfxRect tela = { 0, 0, NV_TELA_W, NV_TELA_H };
    gfx_cor(tela, 0.0f, 0.051f, 0.051f, 0.051f, pg);
  }
  if (tex) {
    gfx_tex_aspect_atual = tex_aspecto(arte);
    // Opacidade sobe RAPIDO (nao com `s`): a arte por baixo e a mesma, entao o
    // que a rampa faz e so trocar a vinheta do hero pela do detalhe. Esticada
    // ao longo de toda a mola ela viraria um esmaecimento visivel do fundo.
    float aEntrada = anim_clamp(s * 3.0f, 0.0f, 1.0f);
    gfx_rect(alvo, tex, GFX_DETALHE, 0, 0, 0, 0.0f, 0, 0, 0,
             aEntrada * (1.0f - 0.85f * pg));
    gfx_tex_aspect_atual = 0.0f;
  } else {
    gfx_cor(alvo, 0.0f, 0.051f, 0.051f, 0.051f, 1.0f);
  }

  // O hero ROLA com o documento: ele nao some nem e substituido por um
  // cabecalho fixo. Era isso que fazia a pagina do port parecer outra tela em
  // vez da mesma tela rolada.
  // O conteudo SOBE para o lugar enquanto aparece, no lugar de so surgir: e a
  // contraparte do texto da home, que desce e apaga. Junto, le como um bloco
  // trocando de arranjo, que e o que o dono pediu.
  heroWeb(a2, -scrollY + (1.0f - a2) * NV_TELA_H * 0.05f);

  if (pg <= 0.01f && scrollY < 1.0f) return;
  for (int r = 0; r < N_SECOES; r++) desenhaSecao(r, pg, agora);
}

int detail_indice(void) { return idx; }
int detail_pediu_reproduzir(void) { int v = pedReproduzir; pedReproduzir = 0; return v; }
int detail_pediu_marcar(void)     { int v = pedMarcar;     pedMarcar = 0;     return v; }
int detail_pediu_fontes(void)     { int v = pedFontes;     pedFontes = 0;     return v; }
// "Reproduzir desde o inicio" ainda cai no mesmo caminho do primario: o
// roteador so sabe abrir o player no ponto salvo. Consumir o pedido aqui evita
// que ele fique pendurado.
int detail_pediu_do_inicio(void)  { int v = pedDoInicio;   pedDoInicio = 0;   return v; }
