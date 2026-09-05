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
#include "badges.h"
#include "mark.h"
#include "settings.h"
#include "home.h"
#include "extras.h"
#include "person.h"
#include "streams.h"
#include "discover.h"
#include "director.h"
#include "gfx.h"
#include "text.h"
#include "tex_cache.h"
#include "focus.h"
#include "anim.h"
#include "layout.h"
#include "catalog.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

// Teto de itens por secao. 24 e nao 8: uma temporada de "Silo" tem 10
// episodios e o vetor de 8 escondia os dois ultimos — a lista parecia menor do
// que a serie e.
#define N_ITEMS    24
// SEIS secoes, mas nenhum titulo usa as seis: serie acende as quatro primeiras
// e filme acende as tres ultimas. As que nao valem para o tipo devolvem 0 em
// secaoN, e focus_mover PULA fileira vazia — entao a ordem do enum ja entrega a
// ordem visual certa nos dois casos, sem tabela de tradusao no meio:
//   serie -> Temporadas, Episodios, Abas, Elenco
//   filme -> Elenco, Trailers, Detalhes
// Cartao de "Recomendacoes" e de "Comentarios". Ficavam junto das funcoes que
// os desenham, la embaixo; subiram porque larguraItem e xItem, no topo,
// precisam deles para posicionar as SECOES novas.
#define REL_CARD_W   212.0f
#define REL_CARD_H   318.0f
#define REL_CARD_GAP  32.0f
// MEDIDO na referencia (TCL, 1920x1080): cartao 722x466, vao 25, canto 20.
#define COM_CARD_W   722.0f
#define COM_CARD_H   466.0f
#define COM_DFLT       28.0f
#define COM_CARD_GAP  25.0f   // MEDIDO

#define N_SECTIONS    8
#define N_CAST    6

static HomeItem item;
static int  is_open = 0, exiting = 0;
static int  idx = 0;                 // titulo atual dentro do acervo
static float t = 0.0f;               // 0 = card na home, 1 = tela cheia
// Dois estados, nao tres: o hero (nivel 0) e a pagina rolada (nivel 1). O
// nivel intermediario "cartao vira tela cheia" so fazia sentido enquanto havia
// cartao; no web a tela ja nasce cheia.
static int  level = 0;
// Ficha da pessoa por cima da tela de titulo. Nao e um `nivel` a mais porque
// nao e um estado da MESMA pagina: e outra tela, que aparece e sai inteira.
static int  personIs_open;
static int  personFocus;
// Primeira LINHA visivel da filmografia. A grade tem 6 por linha e cabem duas
// linhas na tela; sem isto o resto dos creditos era cortado sem aviso.
static int  personLine;
static int  reqOpen = -1;
// Foco DENTRO da aba "Mais como este", que e uma lista vertical propria e nao
// uma das fileiras horizontais do focus.c.
static int  relFocus;
// Temporada escolhida no painel de notas por episodio (indice em extras).
static int  ratTemp;
#define PES_PHOTO_W   280.0f
#define PES_PHOTO_H   420.0f
#define PES_COL_X    (NV_DETP_X + PES_PHOTO_W + 56.0f)
#define PES_CARD_W   212.0f
#define PES_CARD_H   318.0f
#define PES_CARD_GAP  32.0f
#define PES_PER_LINE  6

static int  button = 0;      // botao em foco no hero
static int  reqPlay = 0, reqMark = 0, reqSources = 0;
// Marcar como ASSISTIDO. Separado de pedMarcar, que e "adicionar a lista".
static int  reqWatched = 0;
static int  reqOfStart = 0;         // botao "Reproduzir desde o inicio"
static Uint32 okScrolldownIn = 0;
static float pg = 0.0f;              // 0..1: hero -> pagina rolada
static Focus focus;
static float animFocus[N_SECTIONS][N_ITEMS];
static float scrollSec[N_SECTIONS];    // rolagem HORIZONTAL de cada fileira
static float scrollY = 0.0f;         // rolagem VERTICAL do documento
static int season = 0;            // temporada ESCOLHIDA (nao a focada)
// Repouso do foco sobre a fileira de temporadas, para trocar de temporada ao
// PARAR numa pilula em vez de a cada pilula por que se passa.
static int    tempPending = 0;
static Uint32 tempSince = 0;
// Comentarios: 0 = da SERIE, 1 = do EPISODIO. E o seletor que a referencia poe
// sob "Avaliações do Trakt". Em filme nao existe e fica cravado em 0.
static int commentEp = 0;
static int tabInfo = 0;              // aba de informacao escolhida

// As quatro secoes do web, com o topo do GRUPO em coordenada de documento — e
// nao uma pilha de alturas somadas, que era o modelo do app da Apple TV. As
// posicoes sao fixas porque no web tambem sao: o documento tem tamanho
// conhecido e a rolagem so muda o quanto dele se enxerga.
typedef enum { SEC_SEASONS, SEC_EPISODES, SEC_TABS_INFO, SEC_CAST,
               SEC_TRAILERS, SEC_RELATED, SEC_COMMENTS,
               SEC_DETAILS } KindSection;
// Definida adiante, junto do resto das consultas ao catalogo; declarada aqui
// porque recalcularLayout, cabecalhoDe e nAvaliaveis, todas acima dela,
// precisam separar serie de filme.
static int isSeries(void);
static float heightHeaderComments(void);
static int seasonIn(int c);
static float baseOfTabActive(void);
// A fileira de comentarios e definida junto do desenho dela, la embaixo, mas a
// contagem de colunas e a largura de item — que ficam aqui em cima — precisam
// perguntar quantas pilulas e quantos cartoes ela tem.
#define COM_PILL_GAP  16.0f
static const char *COM_ROT[2];
static int   nPillsCom(void);
static int   nCardsCom(void);
static float widthPilulaCom(const char *rot);
static int  sectionN(int r);
static float heightSection(int r);

// LAYOUT DO DOCUMENTO, recalculado a cada quadro.
//
// Duas leis diferentes, e de proposito:
//
// SERIE — coordenadas ABSOLUTAS medidas no aparelho (NV_DETP_G_*). Nao viram
// fluxo. O comentario em detail.h:70 registra o que aconteceu quando alguem
// tentou deduzi-las por soma de alturas: a rolagem batia no teto cedo demais e
// a fileira de elenco parava meio ecra fora do lugar.
//
// FILME — EMPILHADO. As secoes de filme (Elenco, Trailers, Detalhes) tem altura
// que depende do conteudo, e nao ha medida de aparelho para copiar. Aqui o topo
// de cada uma e a soma do que veio antes, que e como o web se comporta de fato:
// o bloco que nao existe nao ocupa altura.
//
// `topoSec` e o topo do GRUPO (a linha do cabecalho). O conteudo comeca em
// `conteudoSec`, e e ELE o alvo da rolagem — o web mira o topo do TRILHO, nao
// o do grupo (focusInList, metaDetailsScreen.js:7936).
static float topSec[N_SECTIONS], contentSec[N_SECTIONS], targetSec[N_SECTIONS];
static float docEnd = NV_DETP_END;

// Cabecalho de secao: so o filme tem. Na serie o rotulo "Temporadas" e desenhado
// pelo caminho antigo, e "Elenco" ficaria repetindo a aba "Criador e elenco"
// logo acima (ver detail.c:1611).
static const char *headerOf(int r) {
  if (isSeries()) return NULL;
  switch (r) {
    case SEC_CAST:   return "Cast";
    case SEC_TRAILERS:     return "Trailers";
    case SEC_RELATED: return "Recommendations";
    // Sem cabecalho de secao: a propria secao ja abre com "trakt Comentários" e
    // o subtitulo "Avaliações do Trakt". Com os dois saiam DOIS titulos
    // empilhados dizendo a mesma coisa.
    case SEC_COMMENTS:  return NULL;
    case SEC_DETAILS:     return "Film Details";
    default:           return NULL;
  }
}

// A ABA DE TEMPORADA E UM ATALHO DE ROLAGEM, nao um filtro.
//
// Proposta do dono, e melhor que o que estava: a lista de episodios passou a
// ser UNICA (todas as temporadas, ordenadas), e a aba apenas leva o foco ao
// PRIMEIRO episodio daquela temporada. Nao ha recarga, nao ha rede, nao ha
// reconstrucao — a demora ao trocar de aba deixa de existir porque a troca
// deixa de acontecer.
static void goToSeason(int c) {
  int target = seasonIn(c), n = cat_n_episodes(idx), i;
  if (target <= 0 || n < 1) return;
  for (i = 0; i < n; i++) {
    const CatEp *e = cat_episode(idx, i);
    if (e && e->season == target) {
      // Mover o FOCO e nao so a rolagem: a fileira de episodios ja tem a regra
      // de encostar o card focado na margem esquerda, e reusa-la deixa a aba e
      // o D-pad concordando sobre onde o dono esta.
      if (i < focus.nColumns[SEC_EPISODES]) {
        focus.row = SEC_EPISODES;
        focus.column = i;
      }
      return;
    }
  }
}

// Filme sem elenco ainda, com o meta em voo. E o unico caso em que uma secao
// vazia ocupa altura (ver recalcularLayout) e recebe esqueleto.
static int castLoading(void) {
  const CatItem *ci = cat_item(idx);
  return !isSeries() && ci && ci->nCast == 0 && disc_episodes_loading(idx);
}

static void recomputeLayout(void) {
  int r;
  if (isSeries()) {
    // As quatro primeiras vem de MEDIDA ABSOLUTA na referencia; nao sao um
    // empilhamento. As de baixo (comentarios) sim: elas ficam depois do elenco,
    // cuja altura e conhecida.
    static const float G[N_SECTIONS] = {
      NV_DETP_G_TEMP, NV_DETP_G_EP, NV_DETP_G_TABS, NV_DETP_G_CAST, 0, 0
    };
    float y;
    for (r = 0; r < N_SECTIONS; r++) {
      topSec[r] = contentSec[r] = G[r];
      targetSec[r] = (r == SEC_TABS_INFO) ? NV_DETP_TARGET_TABS
                                        : NV_DETP_TARGET_ROW;
    }
    docEnd = NV_DETP_END;
    // SECAO DO TRAKT NA SERIE: empilhada abaixo do elenco, como na referencia.
    // Era o "falta a secao do trakt na de series" — ela existia so em filme.
    //
    // A base do ELENCO sai das medidas da SERIE, nao de NV_DETF_EL_ALT (193),
    // que e a altura da fileira de elenco do FILME — foi o que eu usei antes e
    // por isso a secao do Trakt caiu POR CIMA dos avatares.
    //
    // O empilhamento e: topo da fileira (EL_Y) + avatar + o vao ate o nome + o
    // vao do nome ate o papel + a linha do papel. Mais UMA linha de folga
    // porque nome comprido quebra em duas ("Geneva Robertson-Dworet" na propria
    // captura do dono) e empurra o papel para baixo.
    y = baseOfTabActive() + NV_DETP_EL_GAP_TRAKT;
    topSec[SEC_COMMENTS] = contentSec[SEC_COMMENTS] = y;
    if (sectionN(SEC_COMMENTS) > 0) {
      float end = y + heightSection(SEC_COMMENTS) + NV_DETF_DFLT_END;
      if (end > docEnd) docEnd = end;
    }
    return;
  }
  { float y = NV_DETF_HERO_END;
    for (r = 0; r < N_SECTIONS; r++) {
      float h;
      topSec[r] = contentSec[r] = y;
      targetSec[r] = NV_DETP_TARGET_ROW;
      // Secao ausente nao ocupa altura — salvo o ELENCO enquanto o meta do
      // filme carrega: a fileira reserva o lugar e recebe o esqueleto, para a
      // pagina nao pular quando os atores chegarem.
      if (sectionN(r) <= 0 && !(r == SEC_CAST && castLoading())) continue;
      if (headerOf(r)) {
        contentSec[r] = y + NV_DETF_HEADER_H + NV_DETF_HEADER_GAP;
        y = contentSec[r];
      }
      h = heightSection(r);
      y += h + NV_DETF_SEC_GAP;
    }
    // Fim REAL do documento, nao os 2473 da serie: um filme e bem mais curto e
    // copiar aquele numero deixaria a pagina rolar para muito depois do fim.
    docEnd = y - NV_DETF_SEC_GAP + NV_DETF_DFLT_END;
    if (docEnd < NV_SCREEN_H) docEnd = NV_SCREEN_H; }
}

// As abas sao DINAMICAS, como no web: renderSeriesInsightSection
// (metaDetailsScreen.js:3751) so acrescenta "Mais como este", "Trailer" e
// "Colecao" quando a lista correspondente tem itens, e esconde a barra inteira
// quando sobra uma aba so. O port cravava as quatro e as tres ultimas caiam
// todas em "Sem informacao para esta aba." — que e exatamente o que o web
// evita nao mostrando a aba.
//
// Aqui existem duas: elenco (sempre) e avaliacoes (quando ha nota). Similares
// e trailer nao tem fonte neste port; quando tiverem, entram nesta tabela.
typedef enum { TAB_CAST, TAB_RATINGS, TAB_RELATED, TAB_COLLECTION,
               TAB_COMMENTS, TAB_NFIXAS } TabInfoId;
// OS ROTULOS SAO OS DO APARELHO, e nao os do web. Lido na barra da serie
// "Furious" na TCL: "Direção e Elenco | Avaliações | Recomendações | Trailer".
// "Criador e elenco" e "Mais como este" vinham do NuvioWeb e nao existem la.
//
// "Coleção" e "Comentários" ficam: sao dados que este port TEM e que a barra da
// referencia nao mostrava naquele titulo (ela some as abas sem conteudo, e a
// serie medida nao tinha nem colecao nem comentarios). Tirar as duas seria
// esconder o que o app ja sabe mostrar.
static const char *TAB_LABEL[TAB_NFIXAS] = {
  "Cast and Crew", "Ratings", "Recommendations", "Collection", "Comments"
};

// Nota do IMDb do titulo aberto, 0 quando nao ha.
static int scoreOf(int i) {
  const CatItem *ci = cat_item(i);
  return ci ? ci->score : 0;
}
// Quantos itens a aba de Avaliacoes tem para focar: as temporadas, em serie; os
// cartoes de nota, em filme. Serve so a navegacao — o desenho ja sabe o que
// mostrar em cada caso.
static int nRateable(void) {
  int i, n = 0;
  if (isSeries() && extras_n_seasons() > 0) return extras_n_seasons();
  for (i = 0; i < EX_NSOURCES; i++) {
    int v = extras_score(i);
    if (i == EX_IMDB && !v) v = scoreOf(idx);
    if (v) n++;
  }
  return n;
}

static int tabAvailable(int id) {
  switch (id) {
    case TAB_CAST:       return 1;
    // Basta UMA das notas para a aba valer a pena; o cartao que faltar mostra
    // "-", que e o que o web faz.
    case TAB_RATINGS:   return scoreOf(idx) > 0 || extras_score_trakt() > 0;
    case TAB_RELATED: return extras_n_related() > 0;
    case TAB_COLLECTION:      return extras_n_collection() > 1;
    // Sem aba de comentarios: na referencia eles sao uma SECAO empilhada, e as
    // abas medidas na TCL sao so Direção e Elenco / Avaliações / Recomendações.
    // Deixar as duas coisas mostraria o mesmo conteudo em dois lugares.
    case TAB_COMMENTS:  return 0;
    default:               return 0;
  }
}
// Traduz a posicao visivel `c` para o id da aba.
static int tabIdOf(int c) {
  for (int id = 0, v = 0; id < TAB_NFIXAS; id++)
    if (tabAvailable(id) && v++ == c) return id;
  return TAB_CAST;
}
static int nTabsInfo(void) {
  int n = 0;
  for (int id = 0; id < TAB_NFIXAS; id++) if (tabAvailable(id)) n++;
  return n;
}


static int  sectionN(int r);
static int  sectionColumns(int r);
static int  seasonIn(int c);
static float widthSeason(int c);
static float widthTabInfo(int i);

// NULL quando nao se sabe — nao uma lista de reserva. As listas fixas que
// ficavam aqui existiam para o app rodar so com uma pasta de imagens solta, mas
// o preco era um titulo REAL sem nome carregado aparecer chamado "Ruptura" ou
// "Silo", indistinguivel de dado verdadeiro. Quem desenha omite o que vier NULL.
static const char *titleOf(int i) {
  const CatItem *c = cat_item(i);
  return (c && c->title[0]) ? c->title : NULL;
}
static const char *genreOf(int i) {
  const CatItem *c = cat_item(i);
  return (c && c->genre[0]) ? c->genre : NULL;
}
// Vazio quando nao se sabe. A lista de reserva que ficava aqui carimbava
// "2025 · 1 h 54 min" num titulo cujo metadado ainda nao chegou — ano e duracao
// INVENTADOS, na linha de meta do hero, ao lado de dados verdadeiros.
// partirMeta ja lida com string vazia e devolve os dois campos vazios; quem
// desenha omite cada um deles.
static const char *profileOf(int i) {
  const CatItem *c = cat_item(i);
  return (c && c->meta[0]) ? c->meta : "";
}
static const char *synopsisOf(int i) {
  const CatItem *c = cat_item(i);
  return (c && c->synopsis[0]) ? c->synopsis : NULL;
}
static const char *logoOf(int i) {
  const CatItem *c = cat_item(i);
  return (c && c->logo[0]) ? c->logo : NULL;
}
static const char *artOf(int i) {
  const CatItem *c = cat_item(i);
  // Um detalhe nunca pode herdar a arte de outra posicao do catalogo. Quando
  // o backdrop do proprio titulo falta, o renderer mostra o estado neutro e
  // preserva o layout, aguardando eventual enriquecimento do mesmo item.
  if (c && c->backdrop[0]) return c->backdrop;
  // Poster do próprio título é a reserva segura. O desenho trata-o como arte
  // contida, não como cover 16:9, para preservar rosto, lettering e proporção.
  if (c && c->poster[0]) return c->poster;
  return NULL;
}

static int artDetailIsPoster(int i) {
  const CatItem *c = cat_item(i);
  return c && !c->backdrop[0] && c->poster[0];
}

static void drawArtDetail(GfxRect target, GLuint tex, const char *art,
                               int poster, float alpha, float pg) {
  if (!tex) {
    gfx_color(target, 0.0f, 0.051f, 0.051f, 0.051f, alpha);
    return;
  }
  gfx_tex_aspect_current = tex_aspect(art);
  if (!poster) {
    gfx_rect(target, tex, GFX_DETAIL, 1.0f - pg, 0, 0, 0.0f, 0, 0, 0,
             alpha);
  } else {
    float ap = gfx_tex_aspect_current > 0.05f ? gfx_tex_aspect_current : (2.0f / 3.0f);
    float h = target.h * 0.90f, w = h * ap, maxW = target.w * 0.42f;
    if (w > maxW) { w = maxW; h = w / ap; }
    GfxRect r = { target.x + target.w - w - 72.0f,
                  target.y + (target.h - h) * 0.5f, w, h };
    gfx_rect(r, tex, GFX_HERO, 0, 0, 0, 0, 0, 0, 0, alpha);
  }
  gfx_tex_aspect_current = 0.0f;
}
static int isSeries(void) {
  const CatItem *ci = cat_item(idx);
  if (!ci) return 0;
  if (ci->kind[0]) return strcmp(ci->kind, "series") == 0;
  return cat_n_episodes(idx) > 0;
}

static float smooth(float x) {
  x = anim_clamp(x, 0.0f, 1.0f);
  return 1.0f - (1.0f - x) * (1.0f - x) * (1.0f - x);
}
static float phase2(void) { return smooth((t - 0.45f) / 0.55f); }

// A Inter embarcada so tem Regular, Medium e Bold, e a pagina pede 500, 600 e
// 800 em corpos (32, 26, 21) que so existem em Regular na tabela de text.c —
// que e arquivo de outro agente nesta sessao. Engrossar redesenhando a mesma
// linha com deslocamentos sub-pixel e o que sobra, e e o que os rasterizadores
// chamam de "faux bold": custa uma textura so, porque a linha vem do cache.
static void txt_weight(TxtLine l, float x, float y, float a, float thickness) {
  txt_draw_alpha(l, x, y, a);
  if (thickness > 0.05f) txt_draw_alpha(l, x + thickness * 0.5f, y, a);
  if (thickness > 0.9f)  txt_draw_alpha(l, x + thickness, y, a);
}

void detail_open(const HomeItem *it) {
  mark("detail_open");
  item = *it;
  is_open = 1; exiting = 0; level = 0; button = 0;
  t = 0.0f; pg = 0.0f; scrollY = 0.0f; tabInfo = 0; personIs_open = 0;
  relFocus = 0; reqOpen = -1; ratTemp = 0;
  idx = it->index_;
  // Nota do Trakt, comentarios e relacionados. Pedido na ABERTURA e nao no
  // desenho: as abas so aparecem depois que o dado chega, e pedir no desenho
  // faria a barra de abas surgir com o titulo ja na tela.
  { const CatItem *ci = cat_item(idx);
    if (ci && ci->imdb[0]) extras_request(ci->imdb, isSeries(), ci->tmdb); }
  // A aba marcada tem de ser a da temporada que os episodios trazem. Comecando
  // sempre em 0, uma serie cujo primeiro episodio carregado e da 4 abria com
  // "Temporada 1" aceso — o rotulo desmentia a lista logo abaixo.
  season = 0;
  { const CatEp *e0 = cat_episode(idx, 0);
    const CatItem *ci0 = cat_item(idx);
    if (e0 && ci0) {
      int k;
      for (k = 0; k < ci0->nSeasons; k++)
        if (ci0->seasons[k] == e0->season) { season = k; break; }
    } }
  tempPending = season; tempSince = 0;
  int cols[N_SECTIONS]; for (int i = 0; i < N_SECTIONS; i++) cols[i] = sectionColumns(i);
  focus_start(&focus, N_SECTIONS, cols);
  memset(animFocus, 0, sizeof animFocus);
  memset(scrollSec, 0, sizeof scrollSec);
}

int detail_is_open(void) { return is_open; }

// 0..1 de quanto o detalhe ja tomou a tela. A home le isto para DESCER as
// fileiras enquanto ele entra: e o movimento que o dono descreve como "so os
// posters descem". Fica aqui e nao numa variavel compartilhada porque a mola
// que o produz e a mesma do desenho — dois relogios diferentes descasariam.
float detail_progress(void) { return is_open ? smooth(t) : 0.0f; }

// Temporada e episodio EM FOCO, para quem for pedir fonte.
//
// Sem isto o addons_buscar recebia so o imdb da serie e cravava ":1:1" — e por
// isso as fontes eram sempre as do episodio 1, qualquer que fosse o escolhido.
// Devolve 0 quando o foco nao esta na fileira de episodios; nesse caso quem
// chama cai no primeiro episodio da temporada em exibicao, que e o que a tela
// mostra em cima.
// ONDE O DONO PAROU, ou onde ele deve comecar.
//
// Tres fontes, nesta ordem:
//   1. o episodio EM FOCO, quando ele esta na fileira de episodios — ali a
//      escolha e explicita e ganha de qualquer historico;
//   2. o episodio em ANDAMENTO (progresso do Trakt no proprio CatItem), que e o
//      "Retomar";
//   3. o PRIMEIRO NAO ASSISTIDO, varrendo as temporadas em ordem com o
//      /shows/<id>/progress/watched que extras.c ja le — o "Proximo".
//
// O comentario que existia aqui dizia que o catalogo nativo "nao guarda quais
// episodios ja foram vistos". Nao guarda mesmo, mas extras_ep_visto() sabe
// desde que o painel de notas por episodio foi feito — a afirmacao ficou velha
// e o botao seguiu apontando para T1E1 em serie ja comecada, que foi o que o
// dono relatou.
//
// `origem` (opcional) devolve 1 = foco, 2 = retomar, 3 = proximo, 0 = primeiro.
static int episodeTarget(int *temp, int *eps, int *origin) {
  const CatItem *ci = cat_item(idx);
  const CatEp *ep = NULL;
  if (origin) *origin = 0;

  if (focus.row == SEC_EPISODES) ep = cat_episode(idx, focus.column);
  if (ep) {
    if (temp) *temp = ep->season;
    if (eps) *eps = ep->episode;
    if (origin) *origin = 1;
    return 1;
  }
  // Em andamento: o item do "Continuar assistindo" traz temporada e episodio.
  if (ci && ci->progress > 0 && ci->progress < 90 && ci->season > 0 && ci->episode > 0 &&
      !extras_ep_watched(ci->season, ci->episode)) {
    if (temp) *temp = ci->season;
    if (eps) *eps = ci->episode;
    if (origin) *origin = 2;
    return 1;
  }
  // Primeiro nao assistido. So vale quando o Trakt ja respondeu; sem dado
  // nenhum extras_n_temporadas() e 0 e cai no primeiro episodio, como antes.
  { int t, e;
    if (extras_next_episode(&t, &e)) {
      if (temp) *temp = t;
      if (eps) *eps = e;
      if (origin) *origin = 3;
      return 1;
    }
  }
  { int t, i, nt = extras_progress_ready() ? extras_n_seasons() : 0;
    for (t = 0; t < nt; t++) {
      int tn = extras_season_number(t), ne = extras_n_eps(t);
      for (i = 0; i < ne; i++) {
        int en = extras_ep_number(t, i);
        if (en > 0 && !extras_ep_watched(tn, en)) {
          if (temp) *temp = tn;
          if (eps) *eps = en;
          if (origin) *origin = 3;
          return 1;
        }
      }
    } }
  ep = cat_episode(idx, 0);
  if (!ep) return 0;
  if (temp) *temp = ep->season;
  if (eps) *eps = ep->episode;
  return 1;
}

int detail_ep_focus(int *temp, int *eps) {
  if (!is_open) return 0;
  return episodeTarget(temp, eps, NULL);
}

int detail_settled(void) {
  return is_open && !exiting && t > 0.985f && level == 0;
}

// Retangulo do backdrop NESTE quadro e a opacidade com que ele sai. Uma conta
// so, usada por detail_cobre_tela e por detail_desenhar — se as duas
// divergirem, a home some um quadro antes de a arte cobrir e a tela pisca.
static void backdropRect(GfxRect *r, float *opacity) {
  float s = smooth(t);
  GfxRect de;
  home_hero_rect(&de.x, &de.y, &de.w, &de.h);
  r->x = de.x + (0.0f - de.x) * s;
  r->y = de.y + (0.0f - de.y) * s;
  r->w = de.w + (NV_SCREEN_W - de.w) * s;
  r->h = de.h + (NV_SCREEN_H - de.h) * s;
  // Sobe RAPIDO (s*3, nao s): a arte por baixo e a mesma, entao a rampa so
  // troca a vinheta do hero pela do detalhe.
  *opacity = anim_clamp(s * 3.0f, 0.0f, 1.0f);
}

int detail_covers_screen(void) {
  // O backdrop e FULL-BLEED: assim que ele termina de crescer, nao sobra um
  // pixel da tela anterior. Desenhar a home por baixo custava um quadro inteiro
  // de preenchimento a toa — medido em 42 ms no pior quadro.
  //
  // A CONDICAO ERA `suave(t) > 0.995`, que so e verdade em t > 0,83: a home
  // continuava sendo desenhada em 83% da abertura, e e nessa janela que estava
  // o jank medido no aparelho (clr=38,3ms com CPU ociosa — GPU afogada por
  // preenchimento, home + fundo chapado + backdrop, tres camadas de tela cheia
  // ou mais).
  //
  // Agora a pergunta e a certa: o retangulo do backdrop ja alcancou as quatro
  // bordas E ja esta opaco? Com o hero em tela cheia ele nasce praticamente do
  // tamanho da tela, entao a resposta chega em t ~ 0,13 — a home sai seis vezes
  // mais cedo. Quando a origem NAO cobre (hero em faixa, ou o detalhe aberto da
  // busca), a conta responde `nao` e a home continua desenhada: e por isso que
  // isto e uma medida de cobertura e nao um limiar novo em `t`.
  if (!is_open) return 0;
  { GfxRect r; float opacity;
    backdropRect(&r, &opacity);
    if (opacity < 0.999f) return 0;
    return r.x <= 0.5f && r.y <= 0.5f &&
           r.x + r.w >= NV_SCREEN_W - 0.5f && r.y + r.h >= NV_SCREEN_H - 0.5f;
  }
}

// --- tabela "Detalhes do Filme" ---------------------------------------------
//
// Uma linha por campo COM VALOR. Campo vazio nao vira linha com traco: some.
// Essa e a mesma regra que desenhaAvaliacoes ja usa para fonte sem nota, e e o
// que impede a tabela de virar um formulario meio preenchido quando o TMDB nao
// tem o dado.
typedef struct { const char *key; char value[168]; } LineDet;

// "111" -> "1h 51m"; "47" -> "47min". O TMDB manda minutos crus.
static void durationText(int min, char *dst, size_t size) {
  if (min <= 0) { dst[0] = 0; return; }
  if (min < 60) { snprintf(dst, size, "%dmin", min); return; }
  if (min % 60) snprintf(dst, size, "%dh %dmin", min / 60, min % 60);
  else          snprintf(dst, size, "%dh", min / 60);
}

static int buildDetails(LineDet *o, int max) {
  int n = 0;
  const CatItem *ci = cat_item(idx);
  const char *v;

  #define DET_PLACE(K, S) do {                                   \
    if ((n) < (max) && (S) && (S)[0]) {                        \
      o[n].key = (K);                                        \
      snprintf(o[n].value, sizeof o[n].value, "%s", (S));      \
      n++;                                                     \
    } } while (0)

  DET_PLACE("Status", extras_profile_status());
  { char dt[48]; disc_date_long(extras_profile_release(), dt, sizeof dt);
    DET_PLACE("Release", dt); }
  { char d[32]; durationText(extras_profile_duration(), d, sizeof d);
    DET_PLACE("Runtime", d); }
  // Classificacao: a da ficha do TMDB e a boa. A do catalogo serve de reserva,
  // e desde que o "14" cravado saiu de descoberta.c ela so tem valor quando
  // veio do arquivo de catalogo, que e dado de verdade.
  v = extras_profile_age_rating();
  if (!v || !v[0]) v = (ci && ci->age_rating[0]) ? ci->age_rating : NULL;
  DET_PLACE("Rating", v);
  // Pais: a lista completa do TMDB quando ha; senao o unico que o Cinemeta da.
  v = extras_profile_countries();
  if (!v || !v[0]) v = (ci && ci->pais[0]) ? ci->pais : NULL;
  DET_PLACE("Country of Origin", v);
  DET_PLACE("Directing", (ci && ci->directing[0]) ? ci->directing : NULL);

  #undef DET_PLACE
  return n;
}

static int nLinesDetail(void) {
  LineDet l[NV_DETF_DET_MAXL];
  return buildDetails(l, NV_DETF_DET_MAXL);
}

// Altura do CONTEUDO de uma secao (sem o cabecalho). Serve ao empilhamento do
// filme e ao culling. Antes cada numero destes vivia cravado no meio do
// desenho, e uma secao nova herdava a altura do elenco em silencio.
static float heightSection(int r) {
  switch (r) {
    case SEC_SEASONS: return NV_DETP_TEMP_H;
    case SEC_EPISODES:  return NV_DETP_EP_H;
    case SEC_TABS_INFO:  return NV_DETP_TAB_H;
    case SEC_CAST:     return NV_DETF_EL_HEIGHT;
    case SEC_TRAILERS:     return NV_DETF_TR_HEIGHT;
    case SEC_RELATED: return 318.0f + 46.0f;   // cartaz + titulo/ano
    // + o cabecalho: sem ele a secao seguinte ("Detalhes do Filme") era
    // empilhada usando so a altura dos cartoes e saia POR CIMA deles.
    case SEC_COMMENTS:  return heightHeaderComments() + COM_CARD_H;
    case SEC_DETAILS:     return nLinesDetail() * NV_DETF_DET_LINE;
  }
  return 0.0f;
}

static int sectionN(int r) {
  const CatItem *ci = cat_item(idx);
  switch (r) {
    case SEC_SEASONS:
      // Filme nao tem temporada: a fileira SOME em vez de mostrar abas que nao
      // levam a lugar nenhum. E o que o web faz — a `.series-season-row` so
      // existe no layout de serie.
      if (!isSeries()) return 0;
      if (ci && ci->nSeasons > 0)
        return ci->nSeasons < N_ITEMS ? ci->nSeasons : N_ITEMS;
      return 0;
    case SEC_EPISODES: {
      int q = cat_n_episodes(idx);
      if (q <= 0) return 0;
      return q < N_ITEMS ? q : N_ITEMS;
    }
    // Uma aba so = barra escondida, como o `tabItems.length > 1` do web.
    // FILME NAO TEM ABAS: a pagina de filme empilha as secoes com cabecalho
    // proprio, entao a barra de abas nao entra. Sem esta guarda o filme ficava
    // com as duas coisas ao mesmo tempo — a barra E os cabecalhos.
    case SEC_TABS_INFO: {
      int n;
      if (!isSeries()) return 0;
      n = nTabsInfo();
      return n > 1 ? n : 0;
    }
    // A FILEIRA DE BAIXO E A ABA ESCOLHIDA, nao "o elenco". Este slot desenha
    // elenco, cartazes de "Mais como este", cartoes de nota, a colecao ou os
    // comentarios — desenhaSecao troca o conteudo no lugar. Se a contagem
    // continuasse sendo so a do elenco, escolher outra aba deixava a fileira com
    // o numero errado de colunas, e uma guarda no evento BLOQUEAVA descer para
    // ela por completo: dava para mexer nas abas e em mais nada.
    //
    // Sem elenco a secao nao existe — nao ha reserva. O `N_ELENCO` que ficava
    // aqui como padrao enchia a fileira com seis nomes de demonstracao mesmo num
    // titulo que o app nao sabe quem estrela.
    case SEC_CAST: {
      int n;
      switch (tabIdOf(tabInfo)) {
        case TAB_RATINGS:   n = nRateable();           break;
        case TAB_RELATED: n = extras_n_related(); break;
        case TAB_COLLECTION:      n = extras_n_collection();      break;
        // O cartao de comentario nao se escolhe um a um; o que RECEBE foco sao
        // as duas pilulas do seletor "Série | Episódio". Em filme nao ha
        // episodio: sobra uma coluna so, para o foco poder pousar na fileira e
        // a pagina rolar ate os cartoes.
        case TAB_COMMENTS:  n = isSeries() ? 2 : 1; break;
        default:               n = (ci && ci->nCast > 0) ? ci->nCast : 0;
      }
      return n < NV_DETF_EL_MAX ? n : NV_DETF_EL_MAX;
    }
    // Trailers, Recomendacoes, Comentarios e Detalhes so existem em FILME —
    // na serie o mesmo conteudo vive atras das ABAS.
    //
    // Estas duas ultimas eram justamente o que se perdeu ao tirar as abas do
    // filme: os dados sempre estiveram la (o log mostra "coment=8 rel=12"),
    // mas sem aba e sem secao nao havia como chegar neles.
    case SEC_TRAILERS:
      if (isSeries()) return 0;
      return extras_n_trailers();
    case SEC_RELATED: {
      int n;
      if (isSeries()) return 0;
      n = extras_n_related();
      return n < N_ITEMS ? n : N_ITEMS;
    }
    // Comentario nao se escolhe um a um: UMA coluna, so para o foco pousar e a
    // pagina rolar ate os cartoes.
    // COMENTARIOS EXISTEM NOS DOIS. Na referencia a secao do Trakt fica
    // EMPILHADA abaixo da fileira de elenco tambem na serie — nao e uma aba.
    // Aqui ela so existia em filme, e na serie vivia atras de uma aba que a
    // referencia nao tem; o dono viu isso como "falta a secao do trakt na de
    // series".
    //
    // Colunas: as duas pilulas do seletor "Série | Episódio" na serie; em filme
    // nao ha episodio, entao sobra uma coluna so para o foco pousar.
    case SEC_COMMENTS: {
      int nc = nCardsCom();
      if (nc <= 0 && extras_n_comments() <= 0) return 0;
      return nPillsCom() + nc;
    }
    // A tabela e UMA coluna focavel, nao uma por linha: o D-pad desce ate ela,
    // ela rola para a tela e pronto. Zero colunas faria focus_mover PULA-LA
    // (focus.c:25) e a secao viraria inalcancavel — logo, tambem irrolavel.
    case SEC_DETAILS:
      if (isSeries()) return 0;
      return nLinesDetail() > 0 ? 1 : 0;
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
// QUANTOS CIRCULARES, e a resposta depende do tipo. MEDIDO nas duas capturas
// do aparelho: o FILME ("Ma") tem tres — mais, olho de "ja assisti" e trailer —
// e a SERIE ("Lioness") tem DOIS, sem o olho. Faz sentido e nao e descuido da
// referencia: "assistido" numa serie e por episodio, e a lista de episodios
// logo abaixo ja marca isso um a um; um olho no hero teria de significar "a
// serie inteira", que nao e coisa que o Trakt guarde por titulo.
//
// Este arquivo desenhava TRES nos dois casos.
static int nButtons(void) { return isSeries() ? 3 : 4; }

// Que ACAO esta na posicao `n` da linha. As acoes tem numeros fixos (0
// primario, 1 lista, 2 assistido, 3 fontes) porque detail_evento decide por
// eles; o que muda com o tipo e quais posicoes existem. Sem esta traducao, na
// serie o segundo circular (que e o de fontes) dispararia "marcar assistido".
enum { ACTION_PRIMARY = 0, ACTION_LIST = 1, ACTION_WATCHED = 2, ACTION_SOURCES = 3 };
static int actionIn(int n) {
  if (n >= 2 && isSeries()) return n + 1;   // serie pula o olho
  return n;
}

void detail_event(const SDL_Event *e) {
  if (exiting) return;

  // A FICHA DA PESSOA come os eventos enquanto esta aberta. Ela e outra tela e
  // nao uma secao desta: deixar a tela de titulo continuar respondendo por
  // baixo faria a seta mover duas coisas ao mesmo tempo.
  //
  // O `return` no fim deste bloco e o que faz isso valer. Ele ja existia, mas a
  // chave que o abria englobava TAMBEM os dois blocos abaixo — as setas em
  // "Avaliações" e a navegacao/OK de "Mais como este" e "Coleção" estavam
  // dentro de `if (pessoaAberta)` exigindo `!pessoaAberta`, ou seja, nunca
  // rodavam. Era por isso que nao dava para andar nem abrir nada nas
  // recomendacoes: o codigo estava escrito e era inalcancavel.
  if (personIs_open) {
    if (e->type != SDL_KEYDOWN) return;
    { int n = person_n_credits();
      switch (e->key.keysym.sym) {
        case SDLK_LEFT:  if (personFocus > 0) personFocus--; return;
        case SDLK_RIGHT: if (personFocus + 1 < n) personFocus++; return;
        case SDLK_UP:
          if (personFocus >= PES_PER_LINE) personFocus -= PES_PER_LINE;
          if (personFocus / PES_PER_LINE < personLine) personLine--;
          return;
        case SDLK_DOWN:
          if (personFocus + PES_PER_LINE < n) personFocus += PES_PER_LINE;
          // A grade ROLA quando o foco passa da segunda linha visivel. Duas
          // linhas cabem na tela; a terceira em diante entra empurrando.
          if (personFocus / PES_PER_LINE > personLine + 1) personLine++;
          return;
        case SDLK_AC_BACK: personIs_open = 0; return;
        case SDLK_RETURN:
        case SDLK_KP_ENTER: {
          // Abre o titulo, quando ele for um dos que o catalogo ja tem meta.
          // Quem troca de fato e o roteador (app.c) — daqui so sai o pedido.
          //
          // Um credito que NAO esta no catalogo nao abre nada, de proposito:
          // sem meta nao ha episodios, elenco nem fonte, e uma tela de detalhe
          // vazia e pior que o botao nao responder. Buscar meta sob demanda e
          // trabalho a parte.
          const char *id = person_credit_imdb(personFocus);
          int target = id[0] ? cat_index_by_imdb(id) : -1;
          if (target >= 0) { reqOpen = target; personIs_open = 0; }
          // Nao esta no catalogo: busca o meta e abre quando chegar. Quem
          // termina o trabalho e o roteador, que ja acompanha o resultado.
          // O credito quase nunca traz imdb_id, entao o caminho normal e pelo
          // id do TMDB.
          else if (id[0]) { disc_request_title(id); personIs_open = 0; }
          else if (person_credit_tmdb(personFocus) > 0) {
            disc_request_title_tmdb(person_credit_tmdb(personFocus),
                                   person_credit_kind(personFocus));
            personIs_open = 0;
          }
          return; }
        default: break;
      } }
    if (e->key.keysym.scancode == NV_SCANCODE_BACK) personIs_open = 0;
    return;
  }
    // A aba "Mais como este" e uma LISTA VERTICAL dentro da fileira do elenco.
  // Enquanto ela estiver aberta, cima/baixo andam nela em vez de trocar de
  // fileira — e o mesmo que o web faz, onde a lista tem foco proprio.
  // No painel de notas por episodio, esquerda/direita trocam de TEMPORADA.
  if (e->type == SDL_KEYDOWN && focus.row == SEC_CAST && !personIs_open &&
      tabIdOf(tabInfo) == TAB_RATINGS && isSeries() &&
      extras_n_seasons() > 0) {
    int nt = extras_n_seasons();
    if (e->key.keysym.sym == SDLK_RIGHT && ratTemp + 1 < nt) { ratTemp++; return; }
    if (e->key.keysym.sym == SDLK_LEFT  && ratTemp > 0)      { ratTemp--; return; }
  }

  // "Mais como este" e "Colecao" sao a MESMA lista vertical, so muda a fonte.
  if (e->type == SDL_KEYDOWN && focus.row == SEC_CAST && !personIs_open &&
      (tabIdOf(tabInfo) == TAB_RELATED || tabIdOf(tabInfo) == TAB_COLLECTION)) {
    int col = (tabIdOf(tabInfo) == TAB_COLLECTION);
    int n = col ? extras_n_collection() : extras_n_related();
    if (n > 7) n = 7;
    switch (e->key.keysym.sym) {
      // "Mais como este" e uma fileira de cartazes: anda na HORIZONTAL. A
      // colecao continua em lista vertical.
      case SDLK_RIGHT: if (!col && relFocus + 1 < n) { relFocus++; return; } break;
      case SDLK_LEFT:  if (!col && relFocus > 0)     { relFocus--; return; } break;
      case SDLK_DOWN: if (col && relFocus + 1 < n) { relFocus++; return; } break;
      case SDLK_UP:   if (col && relFocus > 0)     { relFocus--; return; } break;
      case SDLK_RETURN:
      case SDLK_KP_ENTER: {
        if (col) {
          // A parte da colecao traz so o id do TMDB; o caminho e o mesmo do
          // credito de um ator.
          long t = extras_collection_tmdb(relFocus);
          if (t > 0) disc_request_title_tmdb(t, "movie");
        } else {
          const char *id = extras_related_imdb(relFocus);
          int target = cat_index_by_imdb(id);
          if (target >= 0) reqOpen = target;
          else if (id[0]) disc_request_title(id);
        }
        return; }
      default: break;
    }
    // CIMA no primeiro item e BAIXO no ultimo caem no comportamento normal e
    // saem da lista — senao o foco fica preso nela.
  }



  if (e->type == SDL_KEYDOWN && (e->key.keysym.sym == SDLK_RETURN ||
                                 e->key.keysym.sym == SDLK_KP_ENTER)) {
    if (!okScrolldownIn) okScrolldownIn = SDL_GetTicks();
    return;
  }
  if (e->type == SDL_KEYUP && (e->key.keysym.sym == SDLK_RETURN ||
                               e->key.keysym.sym == SDLK_KP_ENTER)) {
    Uint32 duration;
    // SOLTAR sem ter PRESSIONADO nao e clique. Sem esta guarda o detalhe
    // reproduzia sozinho ao ser aberto: o OK apertado na home entrega o KEYDOWN
    // a home (que abre o detalhe) e o KEYUP JA CHEGA AQUI, com nivel 0 e botao
    // 0 — que e exatamente "Reproduzir". Da para ver como o dono descreveu:
    // "clica num titulo e ele ja clica duas vezes e inicia".
    //
    // Antes isto nao aparecia porque o botao morava no nivel 1 e o KEYUP orfao
    // caia em nenhum caso. Passar os botoes para o nivel 0 (que e onde o web os
    // poe) descobriu o defeito que ja existia.
    if (!okScrolldownIn) return;
    duration = SDL_GetTicks() - okScrolldownIn;
    okScrolldownIn = 0;
    if (level == 0) {
      // Ordem FIXA: primario, adicionar a lista, marcar como visto, fontes.
      //
      // O botao do olho caia no `else` e abria a folha de FONTES — ele nunca
      // marcou nada, apesar do icone. Agora tem pedido proprio.
      int action = actionIn(button);
      if (action == ACTION_PRIMARY) {
        if (duration >= NV_HOLD_MS) reqSources = 1; else reqPlay = 1;
      } else if (action == ACTION_LIST) {
        reqMark = 1;
      } else if (action == ACTION_WATCHED) {
        reqWatched = 1;
      } else {
        reqSources = 1;
      }
    } else if (focus.row == SEC_RELATED) {
      // FILME: "Mais como este" e secao propria. Mesmo destino do caminho de
      // serie — abre do catalogo quando ja temos meta, senao pede e o roteador
      // termina quando chegar.
      const char *id = extras_related_imdb(focus.column);
      int target = id[0] ? cat_index_by_imdb(id) : -1;
      if (target >= 0) reqOpen = target;
      else if (id[0]) disc_request_title(id);
    } else if (focus.row == SEC_SEASONS) {
      // Trocar de aba BUSCA a temporada. Antes so mudava o realce e a lista
      // continuava a mesma, o que fazia a aba parecer quebrada.
      season = focus.column;
      tempPending = season; tempSince = 0;
      goToSeason(season);
    } else if (focus.row == SEC_CAST && tabIdOf(tabInfo) == TAB_CAST) {
      // OK num rosto abre a FILMOGRAFIA da pessoa. E o `openCastDetail` do web
      // (metaDetailsScreen.js:6165); aqui o OK no elenco nao fazia nada.
      const CatItem *ci = cat_item(idx);
      if (ci && focus.column < ci->nCast && ci->cast[focus.column].tmdb > 0) {
        person_request(ci->cast[focus.column].tmdb,
                     ci->cast[focus.column].name,
                     ci->cast[focus.column].photo);
        personIs_open = 1;
        personFocus = 0;
        personLine = 0;
      }
    } else if (focus.row == SEC_TABS_INFO) {
      tabInfo = focus.column;
    } else if (focus.row == SEC_EPISODES) {
      // No web e `openEpisodeStreams`. Aqui a folha de fontes ainda e a do
      // titulo: `stream_folha_abrir()` nao recebe episodio. Melhor abrir a
      // folha que existe do que nao responder ao OK.
      reqSources = 1;
    }
    return;
  }

  if (e->type != SDL_KEYDOWN) return;
  SDL_Keycode k = e->key.keysym.sym;

  if (k == SDLK_ESCAPE || k == SDLK_AC_BACK || k == SDLK_BACKSPACE ||
      k == SDLK_DELETE) {
    if (level > 0) level = 0; else exiting = 1;
    return;
  }
  if (level == 0) {
    if (k == SDLK_DOWN) {
      // Descer do hero cai na primeira fileira FOCAVEL. Num filme nao ha
      // temporadas nem episodios, e parar numa fileira vazia deixava o D-pad
      // sem resposta. Tem de ser secaoColunas e nao secaoN: os trailers sao
      // DESENHADOS mas nao aceitam foco, e um filme sem elenco pousaria neles.
      for (int r = 0; r < N_SECTIONS; r++)
        if (sectionColumns(r) > 0) { focus.row = r; focus.column = 0; level = 1; break; }
    }
    else if (k == SDLK_RIGHT) { if (button < nButtons() - 1) button++; }
    else if (k == SDLK_LEFT)  { if (button > 0) button--; }
    return;
  }
  // A guarda que existia aqui bloqueava DESCER das abas sempre que a aba
  // escolhida nao fosse "Criador e elenco" — e com isso trancava o acesso a
  // "Mais como este", "Coleção" e "Comentários", cujo codigo de navegacao ja
  // estava escrito logo acima e nunca era alcancado.
  //
  // Nao e mais preciso: secaoN devolve a contagem DA ABA ATIVA, entao a fileira
  // ou tem colunas de verdade (e o foco pousa no que esta desenhado) ou tem
  // zero, e focus_mover pula sozinho.
  if (k == SDLK_RIGHT)      focus_mover(&focus, 1, 0);
  else if (k == SDLK_LEFT)  focus_mover(&focus, -1, 0);
  else if (k == SDLK_DOWN)  focus_mover(&focus, 0, 1);
  else if (k == SDLK_UP)    { if (!focus_mover(&focus, 0, -1)) level = 0; }
}

// Largura do item e passo horizontal de cada fileira. Temporada e aba de
// informacao tem largura VARIAVEL (saem do texto), e por isso o passo delas nao
// e uma constante como a do episodio.
static float widthItem(int r, int c) {
  switch (r) {
    case SEC_SEASONS:  return widthSeason(c);
    case SEC_EPISODES:   return NV_DETP_EP_W;
    case SEC_TABS_INFO:   return widthTabInfo(c);
    case SEC_TRAILERS:     return NV_DETF_TR_W;
    case SEC_RELATED: return REL_CARD_W;
    case SEC_COMMENTS:
      return (c < nPillsCom()) ? widthPilulaCom(COM_ROT[c]) : COM_CARD_W;
    // A tabela e um bloco so, da largura da divisoria. Cair no `default` daria
    // a ela a largura de um avatar de elenco, e o culling horizontal cortaria
    // a tabela fora da tela.
    case SEC_DETAILS:    return NV_DETF_DET_W;
    default:              return NV_DETP_EL_W;
  }
}
// x do item `c` DENTRO da fileira (antes da rolagem horizontal).
static float xItem(int r, int c) {
  float x = NV_DETP_X;
  for (int k = 0; k < c; k++) {
    if (r == SEC_EPISODES) { x += NV_DETP_EP_STEP; continue; }
    if (r == SEC_CAST)    { x += NV_DETP_EL_STEP; continue; }
    if (r == SEC_TRAILERS)  { x += NV_DETF_TR_STEP;  continue; }
    if (r == SEC_RELATED) { x += REL_CARD_W + REL_CARD_GAP; continue; }
    if (r == SEC_COMMENTS) {
      // As pilulas somam largura + vao; os CARTOES recomecam em NV_DETP_X
      // porque ficam numa LINHA de baixo. xItem deixa de ser monotonico nesta
      // fileira, e nao ha problema: a rolagem horizontal so consulta a coluna
      // FOCADA, nunca a sequencia inteira.
      int np = nPillsCom();
      if (c <= np) x += widthPilulaCom(COM_ROT[k]) + COM_PILL_GAP;
      else if (k >= np) x = NV_DETP_X + (float)(c - np) * (COM_CARD_W + COM_CARD_GAP);
      continue;
    }
    if (r == SEC_DETAILS)  { continue; }   // coluna unica: sempre em NV_DETP_X
    if (r == SEC_SEASONS) x += widthSeason(k) + NV_DETP_TEMP_GAP;
    else x += widthTabInfo(k) + NV_DETP_TAB_SEP * 2 + 9.0f;  // 9 = largura do "|"
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
// Quantas colunas da secao aceitam FOCO. Nem sempre e o mesmo que secaoN, que
// diz quantas se DESENHA.
//
// Trailers e o caso: os cards aparecem, mas nao recebem foco. Este port nao tem
// reprodutor de YouTube, e a regra ja escrita duas vezes neste codigo — o botao
// de trailer removido do hero, o glifo do YouTube trocado no terceiro circular
// — e que um controle que promete o que nao cumpre e pior que a ausencia dele.
// Pular a fileira nao esconde nada: ao descer do Elenco para os Detalhes a
// rolagem passa por cima dos trailers e eles ficam visiveis no caminho.
static int sectionColumns(int r) {
  // TRAILERS SAO FOCAVEIS. Eles ficaram fora do foco por um tempo, pelo
  // argumento de que este port nao toca YouTube e um controle que promete o
  // que nao cumpre e pior que a ausencia dele — a mesma regra que tirou o botao
  // de trailer do hero.
  //
  // O dono pediu o contrario, e tem razao no caso: pular a fileira inteira
  // impede ate de PERCORRER os trailers para ler os nomes, e "nao consigo
  // navegar nos trailers" e um defeito maior que um OK sem efeito. O card
  // continua sem acao ao apertar OK enquanto nao houver reprodutor.
  return sectionN(r);
}

static void syncColumns(void) {
  int r, changed = 0;
  for (r = 0; r < N_SECTIONS; r++) {
    int n = sectionColumns(r);
    if (focus.nColumns[r] != n) { focus.nColumns[r] = n; changed = 1; }
  }
  if (!changed) return;
  // A coluna corrente pode ter ficado fora da faixa (a lista encolheu ao trocar
  // de temporada). Puxar para dentro evita desenhar foco em item inexistente.
  if (focus.column >= focus.nColumns[focus.row])
    focus.column = focus.nColumns[focus.row] > 0
                ? focus.nColumns[focus.row] - 1 : 0;
}

void detail_update(float dt, Uint32 now) {
  if (!is_open) return;
  syncColumns();
  // Solta o pedido de episodios que ficou guardado por ter chegado com outro
  // carregamento em voo.
  disc_episodes_pending();

  // TROCA DE TEMPORADA PELO MOVIMENTO DO FOCO, nao pelo OK.
  //
  // A fileira de temporadas e um SELETOR na referencia: andar com o direcional
  // ja troca a lista de episodios. Aqui a troca so acontecia dentro do OK, e o
  // dono, passando pelas pilulas, via a lista NAO mudar — o que ele descreveu
  // como "demora para atualizar quando troca de temporada". Nao demorava: nao
  // acontecia.
  //
  // Com REPOUSO, pela mesma razao do heroi (NV_HERO_REPOUSO_MS): varrer quatro
  // temporadas de ponta a ponta dispararia quatro consultas das quais so a
  // ultima interessa. Espera o foco parar e so entao troca.
  if (level >= 1 && focus.row == SEC_SEASONS) {
    if (focus.column != tempPending) { tempPending = focus.column; tempSince = now; }
    else if (tempPending != season && tempSince &&
             now - tempSince >= NV_HERO_IDLE_MS) {
      season = tempPending;
      goToSeason(season);
      tempSince = 0;
    }
  } else {
    tempPending = season;
    tempSince = 0;
  }

  // SELETOR DE COMENTARIOS, pela mesma regra: mover o foco ja troca a fonte.
  // Sem repouso — sao duas pilulas, e a da serie ja esta em memoria; so a do
  // episodio custa uma viagem, e ela e disparada uma vez por episodio.
  if (level >= 1 && focus.row == SEC_COMMENTS && isSeries()) {
    if (focus.column != commentEp) commentEp = focus.column;
    if (commentEp) {
      const CatItem *ci = cat_item(idx);
      int t = 0, ep = 0;
      if (ci && detail_ep_focus(&t, &ep) && t > 0 && ep > 0)
        extras_request_comments_ep(ci->imdb, t, ep);
    }
  }

  // DEPOIS de sincronizarColunas, nao antes: o empilhamento pergunta a secaoN
  // quem tem conteudo, e secaoN olha dados que chegam da rede. Recalcular com a
  // contagem do quadro anterior deixaria o layout um quadro atrasado — visivel
  // como um tranco quando o elenco ou os trailers chegam.
  recomputeLayout();
  t  = anim_spring(t,  exiting ? 0.0f : 1.0f, dt, NV_SPRING_SCREEN);
  // Rigidez propria: o web leva 0.8s para apagar o backdrop (cubic-bezier
  // .4,0,.2,1), e a mola de NV_MOLA_TELA assenta em ~330ms.
  pg = anim_spring(pg, level >= 1 ? 1.0f : 0.0f, dt, NV_SPRING_PAGE);
  if (exiting && t < 0.02f) { is_open = 0; exiting = 0; t = 0.0f; return; }

  for (int r = 0; r < N_SECTIONS; r++)
    for (int c = 0; c < sectionN(r) && c < N_ITEMS; c++) {
      float target = (level >= 1 && focus_index(&focus, r, c)) ? 1.0f : 0.0f;
      animFocus[r][c] = anim_spring(animFocus[r][c], target, dt,
                                 target > animFocus[r][c] ? NV_SPRING_FOCUS : NV_SPRING_BLUR);
    }

  // --- rolagem HORIZONTAL da fileira focada ---------------------------------
  // Duas regras, as duas do fonte do web (`getHorizontalTrackScrollLeft`): a
  // fileira de episodios ENCOSTA o card focado na margem esquerda; as demais so
  // rolam o necessario, com 24px de folga nas bordas.
  { int r = focus.row;
    if (r >= 0 && r < N_SECTIONS && sectionN(r) > 0) {
      float x = xItem(r, focus.column) - NV_DETP_X;
      float w = widthItem(r, focus.column);
      float vista = NV_SCREEN_W - NV_DETP_X * 2;
      float target = scrollSec[r];
      if (r == SEC_EPISODES) target = x;
      else {
        // Pilula focada: a fileira volta ao inicio. As pilulas nao rolam junto
        // com os cartoes (elas ficam numa linha propria, fixa), entao deixar o
        // scroll de um cartao antigo pendurado esconderia o primeiro cartao
        // assim que o foco subisse para o seletor.
        if (r == SEC_COMMENTS && focus.column < nPillsCom()) target = 0.0f;
        else if (focus.column == 0) target = 0.0f;
        else if (x + w > target + vista - 24.0f) target = x + w - vista + 24.0f;
        else if (x < target + 24.0f)             target = x - 24.0f;
      }
      if (target < 0.0f) target = 0.0f;
      scrollSec[r] = anim_spring(scrollSec[r], target, dt, NV_SPRING_SCROLL);
    } }

  // --- rolagem VERTICAL -----------------------------------------------------
  // O topo do grupo focado vai para 33% da altura util (40% nas abas). E a
  // regra do web, e nao um "rola o necessario": conferida nos quatro grupos.
  float targetY = 0.0f;
  if (level >= 1 && focus.row >= 0 && focus.row < N_SECTIONS) {
    // Mira o topo do CONTEUDO (o trilho), nao o do grupo: o cabecalho da secao
    // fica acima e entra na tela junto, de graca. E o que focusInList faz no
    // web — `target.closest(".movie-cast-track, ...")`.
    float maxY = docEnd - NV_SCREEN_H;
    targetY = contentSec[focus.row] - NV_SCREEN_H * targetSec[focus.row];
    if (targetY > maxY) targetY = maxY;
    if (targetY < 0.0f) targetY = 0.0f;
  }
  scrollY = anim_spring(scrollY, targetY, dt, NV_SPRING_SCROLL);
}

// ---------------------------------------------------------------------------
// HERO
// ---------------------------------------------------------------------------
// Nada aqui e sobreposicao num cartao: a tela e full-bleed, a coluna comeca em
// x=72 e a pilha e ancorada na BASE (`.detail-hero-section` e um flex column
// com `justify-content: flex-end`). Empilhar de cima para baixo faz o bloco
// inteiro subir e descer conforme o tamanho da sinopse; no web ele fica preso
// na base e so o topo se move.
// Quanto do titulo ja foi assistido, 0..100. 0 quando nunca comecou.
static int progressOf(int i) {
  const CatItem *c = cat_item(i);
  return c ? c->progress : 0;
}

static void drawButton(GfxRect r, const char *rot, int icon, int focused, float a) {
  // FOCO E TAMANHO, NAO ANEL.
  //
  // Aqui havia duas marcas de foco empilhadas e as duas falhavam no botao
  // primario: primeiro um gfx_cor 8px maior — que PREENCHE, nao contorna — e
  // logo depois o GFX_ANEL. Num botao que ja e branco a pilula preenchida se
  // funde com ele e o resultado e um botao branco 8px maior, que nao le como
  // "selecionado" e sim como "o botao mudou de forma". Era o defeito relatado.
  //
  // O aparelho resolve por escala: o item focado cresce com o centro parado, os
  // fatores estao em NV_DETW2_FOCO_*, e nao ha anel nenhum em captura alguma.
  // No circular a escala vem acompanhada da inversao de cor, que ja existia.
  int circular = (rot == NULL);
  if (focused) {
    float sx = circular ? NV_DETW2_FOCUS_SY : NV_DETW2_FOCUS_SX;
    float sy = NV_DETW2_FOCUS_SY;
    float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
    r.w *= sx; r.h *= sy;
    r.x = cx - r.w * 0.5f; r.y = cy - r.h * 0.5f;
  }
  if (circular) {
    float luma = focused ? 0.961f : 0.133f;   // #f5f5f5 / #222
    gfx_color(r, NV_RADIUS_PILL, luma, luma, luma, a);
    // Os tres glifos do web: biblioteca (+), assistido (olho) e trailer
    // (placa do YouTube). Sao SVG la e nao existem na familia embarcada, entao
    // vem do shader — ver GFX_OLHO e GFX_FONTES. Antes eram "+" e dois "...",
    // que nao diziam o que os botoes faziam.
    float ic = focused ? 0.067f : 1.0f;      // #111 com foco, branco sem
    float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
    // ICONES DE VERDADE, do art/icones (SVG do app web rasterizados). Antes
    // cada glifo era desenhado a mao no shader — um "+" de dois retangulos, um
    // olho de dois discos, tres barras — e cada um era uma aproximacao do
    // original. Agora e o arquivo, e a cor vem daqui pelo GFX_MARCA.
    //
    // Proporcao glifo/circulo MEDIDA no aparelho: o "+" mede 32 dentro do
    // circulo de 96 em repouso e 36 dentro do de 110 focado — 0,333 nos dois.
    // Estava 0,45, de uma captura solta, e o glifo quase encostava na borda.
    // Sai de `r` (ja escalado) para que o icone cresca junto com o botao.
    float g = r.w * NV_DETW2_CIRC_GLYPH;
    GfxRect ig = { cx - g * 0.5f, cy - g * 0.5f, g, g };
    if (icon == 1) {
      // O botao MOSTRA O ESTADO: com o titulo ja na watchlist o "+" some e
      // entra o olho aberto — nao adianta convidar a adicionar o que ja esta la.
      // O estado vem de ci->naLista, que a descoberta preenche com a lista de
      // verdade do Trakt.
      const CatItem *ci = cat_item(idx);
      gfx_icon(ig, (ci && ci->inList) ? "watched" : "more", ic, ic, ic, a);
    } else if (icon == 2) {
      // ASSISTIDO: olho aberto quando ja viu, olho riscado quando nao. Antes o
      // icone era sempre o mesmo e nao dizia estado nenhum — era so um enfeite
      // que o dono nao conseguia ler ("avisar o que foi visto").
      gfx_icon(ig, progressOf(idx) >= 90 ? "watched" : "unwatched", ic, ic, ic, a);
    } else {
      gfx_icon(ig, "sources", ic, ic, ic, a);
    }
    return;
  }
  // Primario: branco com texto preto nos DOIS estados. Conferido nas duas
  // capturas do aparelho — o miolo mede (255,255,255) focado e em repouso, e o
  // que muda entre eles e so o tamanho (321x94 -> 357,6x107,8), ja aplicado em
  // `r` la em cima.
  // PILULA BRANCA LIMPA. Aqui o botao INTEIRO era a barra de progresso: a parte
  // que faltava assistir recebia um veu preto a 30%, recortado no ponto do
  // progresso. A intencao era boa e o recorte estava certo, mas o resultado
  // lia como BOTAO DESABILITADO — uma pilula branca com dois tercos apagados
  // parece controle inativo, nao "16% assistido". Foi o que o dono viu: "esse
  // retomar ta muito feio, nem parece o mesmo app".
  //
  // A referencia nao faz isso: o botao e branco limpo e o progresso vive na
  // LINHA DE TEXTO logo acima, que este arquivo ja desenha ("Retomada
  // disponivel  16%  Episodio T2E1"). O veu era redundante alem de feio —
  // dizia com tinta o que a linha ja diz com palavra.
  gfx_color(r, NV_RADIUS_PILL, 1, 1, 1, a);

  // Triangulo 28x30 e vao de 21 ate a tinta do rotulo, medidos no aparelho
  // (x=150..177 e rotulo em 200, dentro da pilula 96..417).
  //
  // O grupo icone+rotulo e CENTRADO na pilula em vez de ancorado no padding
  // esquerdo: focada, a pilula cresce e o rotulo nao — SDL_ttf rasteriza num
  // corpo fixo e nao ha estilo de 28pt na tabela de text.c, que e arquivo de
  // outro agente. Ancorado a esquerda, o texto ficaria visivelmente fora de
  // centro no estado focado; centrado, a folga sobra igual dos dois lados.
  { float s = r.h / NV_DETW2_BTN_H;
    float iw = NV_DETW2_BTN_ICON_W * s, ih = NV_DETW2_BTN_ICON_H * s;
    TxtLine l = txt_line(TXT_DET_BUTTON, rot, 0, 0, 0, 255);
    float group = iw + NV_DETW2_BTN_GAPI * s + l.w;
    float x = r.x + (r.w - group) * 0.5f;
    GfxRect tri = { x, r.y + (r.h - ih) * 0.5f, iw, ih };
    gfx_rect(tri, 0, GFX_PLAY, 0, 0, 0, 0.0f, 0, 0, 0, a);
    txt_draw_alpha(l, x + iw + NV_DETW2_BTN_GAPI * s,
                       r.y + (r.h - l.h) * 0.5f, a); }
}

// Botao secundario: 345x96, raio 64, fundo #222 e texto branco; focado, fundo
// #f5f5f5 e texto #111, com o mesmo anel de 4px. Nao tem icone — no web e so o
// rotulo, e por isso a largura sai de `texto + 2 x 34` e nao da conta do
// primario.
static void drawSecondary(GfxRect r, const char *rot, int focused, float a) {
  if (focused) {
    GfxRect ring = { r.x - NV_DETW_RING, r.y - NV_DETW_RING,
                     r.w + NV_DETW_RING * 2, r.h + NV_DETW_RING * 2 };
    gfx_color(ring, NV_RADIUS_PILL, 1, 1, 1, a);
  }
  float luma = focused ? 0.961f : 0.133f;
  gfx_color(r, NV_RADIUS_PILL, luma, luma, luma, a);
  int color = focused ? 17 : 255;
  TxtLine l = txt_line(TXT_DET_BUTTON, rot, color, color, color, 255);
  txt_draw_alpha(l, r.x + (r.w - l.w) * 0.5f, r.y + (r.h - l.h) * 0.5f, a);
}

// Largura do botao primario: padding 54 + icone 28 + vao 21 + texto. A conta e
// a mesma do aparelho e fecha na medida: com "Assistir T1:E1" (tinta 162) da
// 319 contra os 321 lidos na captura. Um rotulo maior cresce a pilula em vez de
// estourar por baixo do texto.
static float widthPrimary(const char *rot) {
  TxtLine l = txt_line(TXT_DET_BUTTON, rot, 0, 0, 0, 255);
  return NV_DETW2_BTN_PADX * 2 + NV_DETW2_BTN_ICON_W + NV_DETW2_BTN_GAPI + l.w;
}
static float widthSecondary(const char *rot) {
  TxtLine l = txt_line(TXT_DET_BUTTON, rot, 255, 255, 255, 255);
  return NV_DETW_BTN2_PADX * 2 + l.w;
}

// Ano solto do campo `meta` ("2025 · 1 h 54 min" -> "2025" e "1 h 54 min").
static void fromMeta(const char *meta, char *year, size_t na, char *rest, size_t nr) {
  year[0] = 0; rest[0] = 0;
  if (!meta || !meta[0]) return;
  const char *sep = strstr(meta, "\xc2\xb7");        // U+00B7
  if (!sep) { snprintf(year, na, "%s", meta); return; }
  size_t n = (size_t)(sep - meta);
  while (n && (meta[n-1] == ' ')) n--;
  if (n >= na) n = na - 1;
  memcpy(year, meta, n); year[n] = 0;
  const char *r = sep + 2;
  while (*r == ' ') r++;
  snprintf(rest, nr, "%s", r);
}

// Selo do IMDb: 60x30, raio 4, amarelo #f6c700 com "IMDb" preto dentro; a nota
// vem 8px depois, em rgb(179,179,179) — a MESMA cor do resto da linha, e nao
// branca. Medido nas duas capturas do aparelho (selo em x=628..687 na serie e
// 714..773 no filme, sempre y=938..967).
//
// NAO e o 109x60 que este arquivo trazia do web: la o logo tem 60 de ALTURA e
// aqui o selo inteiro tem 30. Com o valor do web o selo ficava do dobro do
// tamanho da linha em que vive.
//
// A marca continua DESENHADA (retangulo + texto) e nao rasterizada do SVG: o
// app nao empacota SVG e o arquivo nao entra sem reinstalar o ipk. E o unico
// ponto do selo que nao e 1:1.
//
// A nota sai com VIRGULA decimal ("7,8"), como na referencia — o "%.1f" do C
// escreve ponto e a linha inteira e em portugues.
static float drawBadgeImdb(float x, float yCenter, int score, float a) {
  if (score <= 0) return 0.0f;
  char txt[8];
  snprintf(txt, sizeof txt, "%d,%d", score / 10, score % 10);
  TxtLine l = txt_line(TXT_DET_SIN, txt, 179, 179, 179, 255);
  GfxRect brand = { x, yCenter - NV_DETW2_IMDB_H * 0.5f,
                    NV_DETW2_IMDB_W, NV_DETW2_IMDB_H };
  gfx_color(brand, NV_DETW2_IMDB_R / NV_DETW2_IMDB_H,
          0.965f, 0.780f, 0.0f, a);                       // #f6c700
  TxtLine lm = txt_line(TXT_MINI, "IMDb", 10, 10, 10, 255);
  txt_weight(lm, brand.x + (brand.w - lm.w) * 0.5f,
           brand.y + (brand.h - lm.h) * 0.5f, a, 0.8f);
  txt_draw_alpha(l, x + NV_DETW2_IMDB_W + NV_DETW2_IMDB_GAP,
                     yCenter - l.h * 0.5f, a);
  return NV_DETW2_IMDB_W + NV_DETW2_IMDB_GAP + l.w;
}

// Selo de CONTORNO da segunda linha de meta. Ele carrega DUAS coisas dentro da
// mesma caixa — classificacao indicativa e status da producao —, separadas por
// uma barra vertical: "TV-MA | RENOVADA" na serie, "R | LANÇADO" no filme. A
// classificacao sai em rgb(179,179,179) e o status em BRANCO, o que faz o olho
// ler primeiro o que interessa.
//
// Antes eram duas coisas soltas na linha e o contorno era falsificado com um
// retangulo cheio 1px maior por baixo — que so funciona sobre fundo chapado.
// Aqui e GFX_ANEL, que desenha contorno de verdade e deixa a arte aparecer no
// miolo, como no aparelho.
//
// `dir` pode ser NULL: sem status o selo tem so a classificacao e NENHUMA
// divisoria. Nao ha valor de reserva — o CatItem nao tem campo de status, e
// carimbar "LANÇADO" em tudo seria dado inventado, que ja custou caro aqui.
static float drawBadgeMeta(float x, float y, const char *left, const char *dir,
                             float a) {
  TxtLine le = txt_line(TXT_DET_META2, left, 179, 179, 179, 255);
  TxtLine ld = { 0, 0, 0 };
  float w = NV_DETW2_BADGE_PADX * 2 + le.w;
  if (dir && dir[0]) {
    ld = txt_line(TXT_DET_META2, dir, 255, 255, 255, 255);
    w += NV_DETW2_DIV_DFLT * 2 + NV_DETW2_DIV_W + ld.w;
  }
  GfxRect box = { x, y, w, NV_DETW2_BADGE_H };
  gfx_rect(box, 0, GFX_RING, 0, NV_DETW2_BADGE_BORDER / NV_DETW2_BADGE_H, 0,
           NV_DETW2_BADGE_R / NV_DETW2_BADGE_H, 0.42f, 0.42f, 0.42f, a);
  float cx = x + NV_DETW2_BADGE_PADX;
  float cy = y + NV_DETW2_BADGE_H * 0.5f;
  txt_draw_alpha(le, cx, cy - le.h * 0.5f, a);
  cx += le.w;
  if (dir && dir[0]) {
    GfxRect bar = { cx + NV_DETW2_DIV_DFLT, cy - NV_DETW2_DIV_H * 0.5f,
                    NV_DETW2_DIV_W, NV_DETW2_DIV_H };
    gfx_color(bar, 0.0f, 0.42f, 0.42f, 0.42f, a);
    cx += NV_DETW2_DIV_DFLT * 2 + NV_DETW2_DIV_W;
    txt_draw_alpha(ld, cx, cy - ld.h * 0.5f, a);
  }
  return w;
}

// Ponto separador: disco de 6, e nao a barrinha de 1x14 do web. Sao dois usos
// com a MESMA forma e cores diferentes, e a diferenca de cor e o que agrupa a
// linha: entre generos ele e rgb(179,179,179) (a cor do proprio texto, porque
// ali ele e um "•" da frase) e entre GRUPOS e rgb(128,128,128), mais apagado.
static void drawDot(float x, float yCenter, float luma, float a) {
  GfxRect pt = { x, yCenter - NV_DETW2_DOT_D * 0.5f,
                 NV_DETW2_DOT_D, NV_DETW2_DOT_D };
  gfx_color(pt, 0.5f, luma, luma, luma, a);
}

static void heroWeb(float a, float offset) {
  if (a <= 0.005f) return;
  const CatItem *ci = cat_item(idx);

  char year[32], duration[64];
  fromMeta(profileOf(idx), year, sizeof year, duration, sizeof duration);

  // Em serie o web escreve "Roteirista:"/"Criador:"; em filme, "Diretor:".
  char sup[192] = "";
  if (ci && ci->directing[0])
    snprintf(sup, sizeof sup, "%s: %s", isSeries() ? "Writer" : "Director",
             ci->directing);

  const char *sin = synopsisOf(idx);

  // --- ORDEM DA COLUNA, como a referencia do dono -----------------------------
  //
  // De cima para baixo: logo, linha de meta (ano, temporadas, classificacao),
  // generos, quem dirigiu, sinopse, linha de retomada e por fim os BOTOES.
  //
  // Antes os botoes vinham logo abaixo do logo e generos/classificacao caiam no
  // rodape, o que separava a informacao do titulo em dois blocos com a acao no
  // meio. Na referencia tudo que DESCREVE o titulo vem junto e a acao fecha o
  // bloco — foi isso que o dono pediu ao comparar as duas telas.
  //
  // Continua ancorado na BASE: a sinopse muda de altura conforme o texto, e
  // ancorar no topo faria o botao dancar de titulo para titulo.
  // A ACAO VEM LOGO ABAIXO DO LOGO, e todo o texto que DESCREVE o titulo vem
  // junto, embaixo dela.
  //
  // Estava ao contrario: as acoes fechavam o bloco, com ~500 px de texto acima
  // delas — o unico alvo interativo da tela era o mais distante do topo. MEDIDO
  // na referencia (TCL, 1920x1080): logo 311..473, ACOES 509..608, apoio
  // 640..685, sinopse 700..900, meta 938..970, classificacao/pais 1003..1045.
  //
  // Isto NAO reintroduz o defeito que motivou a ordem antiga. Aquele era a
  // informacao PARTIDA EM DOIS — parte acima da acao, parte no rodape. Aqui a
  // acao sobe e o texto desce INTEIRO, num bloco so. As proprias constantes
  // deste arquivo ja descreviam esta ordem (NV_DETW_GAP_ACOES e literalmente
  // "acoes -> Diretor:"); o codigo e que tinha derivado do token sheet.
  //
  // Continua empilhando DE BAIXO PARA CIMA e ancorado na base: a sinopse muda
  // de altura com o texto, e ancorar no topo faria o bloco inteiro dancar de
  // titulo para titulo. Isso ficou CONFIRMADO no aparelho: entre a serie (5
  // linhas de sinopse) e o filme (4) as duas linhas de meta caem exatamente nos
  // mesmos y (938 e 999) e a ULTIMA linha de sinopse tambem — o que cresce para
  // cima e o resto da pilha.
  //
  // A GRADE NOVA, medida na TCL: acoes 512..606, apoio 649..675, sinopse em
  // passo de 40 terminando com a tinta em 890, meta 1 (generos/data/IMDb)
  // 938..968 e meta 2 (selo de contorno + pais) 999..1048.
  //
  // A LINHA DE GENEROS SOLTA DEIXOU DE EXISTIR: no aparelho os generos abrem a
  // primeira linha de meta, e ano e nota vem depois deles, separados por ponto.
  // Aqui eram duas linhas — uma so com generos, outra com ano e duracao — e o
  // selo do IMDb ficava sozinho encostado na borda direita da tela, a meio
  // metro do bloco de texto a que pertence.
  float hasResume = (ci && ci->progress > 0) ? 1.0f : 0.0f;
  float hSin = 0.0f;
  if (sin) hSin = txt_block(TXT_DET_SIN, sin, 255, 255, 255, -1.0f, 0.0f,
                            NV_DETW2_TEXT_W, NV_DETW2_LD_SIN, 0.0f,
                            NV_DETW2_SIN_LINES);
  float yMeta2 = NV_DETW2_BASE - NV_DETW2_BADGE_H;
  float yMeta1 = yMeta2 - NV_DETW2_META_GAP - NV_DETW2_M1_H;
  float ySin   = yMeta1 - NV_DETW2_GAP_SIN - hSin;
  float ySup   = sup[0] ? ySin - NV_DETW2_GAP_SUP : ySin;
  float yActions = ySup - NV_DETW2_GAP_ACTIONS - NV_DETW2_BTN_H;
  // A linha de retomada explica o BOTAO, entao fica colada nele — logo acima.
  float yResume = yActions - NV_DETW_GAP_RESUME - NV_DETW_RESUME_H;

  // Sobe alguns pixels enquanto entra: continua o movimento da arte em vez de
  // aparecer pronto no lugar. `desloc` e a rolagem do documento.
  float rises = (1.0f - a) * 26.0f + offset;
  yMeta2 += rises; yMeta1 += rises; ySin += rises; ySup += rises;
  yResume += rises; yActions += rises;

  // --- logo -----------------------------------------------------------------
  const char *fileLogo = logoOf(idx);
  // O logo e desenhado com 261 de largura mas a arte de origem costuma vir bem
  // maior; o teto de 960 ja bastaria, mas quando a mesma arte tambem serve ao
  // hero o item e promovido — por isso passa pelo mesmo caminho.
  GLuint texLogo = fileLogo ? tex_get(fileLogo) : 0;
  if (texLogo) {
    float aspect = tex_aspect(fileLogo);
    if (aspect <= 0.0f) aspect = 2.5f;
    float h = NV_DETW_LOGO_H, w = h * aspect;
    if (w > NV_DETW_LOGO_MAXW) { w = NV_DETW_LOGO_MAXW; h = w / aspect; }
      // O logo assenta acima do que vier primeiro: a linha de retomada quando ha
    // progresso, senao a propria linha de acoes.
    float baseLogo = (hasResume ? yResume : yActions) - NV_DETW_LOGO_GAP;
    GfxRect r = { NV_DETW2_X, baseLogo - h, w, h };
    gfx_tex_aspect_current = 0.0f;   // o logo ja vem na proporcao certa
    // LOGO PRETO VIRA BRANCO. O TMDB serve a mesma marca em versao clara e
    // escura e NAO diz qual e qual — nao ha campo para isso, e o ranking do
    // proprio app web ordena so por idioma e nota. Quando cai a escura, ela
    // aparece preta sobre um backdrop escuro e o titulo some da tela: foi o que
    // aconteceu com "The Invite".
    //
    // A decisao e por MEDIDA, nao por regra fixa: tex_luminancia devolve a
    // media dos pixels opacos, calculada uma vez na thread de decode. So a arte
    // realmente escura e tingida; logo claro ou COLORIDO (o dourado, o
    // vermelho) passa intacto pelo GFX_TEXTO, porque chapa-lo de branco seria
    // trocar um defeito por outro.
    //
    // -1 = ainda carregando: trata como clara e nao tinge. Errar para o lado de
    // nao mexer na arte e o certo enquanto nao se sabe.
    { GfxMode m = tex_brand_dark(fileLogo) ? GFX_BRAND : GFX_TEXT;
      gfx_rect(r, texLogo, m, 0, 0, 0, 0.0f, 1, 1, 1, a); }
  } else {
    // Sem logo, o NOME. A altura da caixa continua sendo a do logo, para que a
    // linha de botoes nao pule entre um titulo com logo e outro sem.
    const char *name = titleOf(idx);
    if (name) {
      TxtLine t2 = txt_line_trim(TXT_TITLE1, name, 255, 255, 255, 255,
                                    NV_DETW_LOGO_MAXW);
      // Mesma ancora do logo: acima da retomada quando ha, senao das acoes. A
      // altura da CAIXA continua sendo a do logo, para que a linha de acoes nao
      // pule entre um titulo com logo e outro sem.
      float baseLogo = (hasResume ? yResume : yActions) - NV_DETW_LOGO_GAP;
      txt_draw_alpha(t2, NV_DETW2_X,
                         baseLogo - NV_DETW_LOGO_H
                                  + (NV_DETW_LOGO_H - t2.h) * 0.5f, a);
    }
  }

  // --- botoes ---------------------------------------------------------------
  // Em FLUXO, com 24px entre vizinhos — nao os 63 que vinham do web. MEDIDO no
  // aparelho: pilula 96..417, circulos com centro em 488,5 e 608,5 (passo 120,
  // diametro 96), o que da 23,5 e 24 de vao. Os circulos sao 96 e nao 84, e
  // ficam 1px mais altos que a pilula (511..606 contra 512..606), o que na
  // pratica e o mesmo centro vertical — e assim que ficam alinhados aqui.
  //
  // Dois estados do rotulo, medidos: "Retomar T2E3" quando ha progresso,
  // "Reproduzir" quando nao ha. (O web tem um terceiro, "Proximo T2E4", que sai
  // do proximo episodio nao assistido — o catalogo nativo nao guarda quais
  // episodios ja foram vistos, entao esse estado nao tem de onde vir.)
  // TRES estados, como o web: "Retomar TxEy" (em andamento), "Próximo TxEy"
  // (primeiro nao assistido) e "Reproduzir" (nunca aberto). O terceiro estado
  // era dado como impossivel aqui; e possivel desde que extras_ep_visto existe.
  char rot[48];
  { int t = 0, e = 0, de = 0;
    if (isSeries() && episodeTarget(&t, &e, &de) && t > 0 && e > 0 && de >= 2)
      snprintf(rot, sizeof rot, "%s T%dE%d",
               de == 2 ? "Resume" : "Next", t, e);
    else if (ci && ci->progress > 0) snprintf(rot, sizeof rot, "Resume");
    else snprintf(rot, sizeof rot, "Play"); }

  { float cyBtn = yActions + NV_DETW2_BTN_H * 0.5f;
    int nb = 0, n = nButtons();
    // Trocar de titulo com o foco no ultimo circular de um FILME e cair numa
    // serie deixaria `botao` = 3 numa linha de 3 botoes: nenhum apareceria
    // focado e o OK nao acharia acao. Fixa aqui, no desenho, que e por onde
    // todo quadro passa.
    if (button >= n) button = n - 1;
    float bx = NV_DETW2_X;
    GfxRect rp = { bx, yActions, widthPrimary(rot), NV_DETW2_BTN_H };
    drawButton(rp, rot, 0, level == 0 && button == nb, a);
    bx += rp.w + NV_DETW2_BTN_GAP; nb++;
    for (; nb < n; nb++) {
      GfxRect rc = { bx, cyBtn - NV_DETW2_CIRC * 0.5f,
                     NV_DETW2_CIRC, NV_DETW2_CIRC };
      drawButton(rc, NULL, actionIn(nb), level == 0 && button == nb, a);
      bx += NV_DETW2_CIRC + NV_DETW2_BTN_GAP;
    } }

  // --- linha de retomada ----------------------------------------------------
  if (ci && ci->progress > 0) {
    char ln[160];
    if (ci->season > 0)
      snprintf(ln, sizeof ln, "Resume available   %d%%   Episode S%dE%d",
               ci->progress, ci->season, ci->episode);
    else
      snprintf(ln, sizeof ln, "Resume available   %d%%", ci->progress);
    TxtLine l = txt_line(TXT_CAPTION, ln, 255, 255, 255, 255);
    txt_draw_alpha(l, NV_DETW2_X, yResume + (NV_DETW_RESUME_H - l.h) * 0.5f,
                       a * 0.82f);
  }

  // --- "Roteirista: ..." / "Diretor: ..." ------------------------------------
  // Mesmo CORPO da sinopse, e nao um menor: na referencia o "R" de "Roteirista"
  // e o "C" da sinopse medem os mesmos 20 de altura de caixa alta. Estava em
  // TXT_DET_META (25) contra TXT_DET_SIN (26) por uma medida do web, onde as
  // duas linhas de fato divergem.
  if (sup[0]) {
    TxtLine l = txt_line_trim(TXT_DET_SIN, sup, 179, 179, 179, 255,
                                 NV_DETW2_TEXT_W);
    txt_draw_alpha(l, NV_DETW2_X, ySup, a);
  }

  // --- sinopse --------------------------------------------------------------
  if (sin) txt_block(TXT_DET_SIN, sin, 255, 255, 255, NV_DETW2_X, ySin,
                     NV_DETW2_TEXT_W, NV_DETW2_LD_SIN, a, NV_DETW2_SIN_LINES);

  // --- meta linha 1: generos • generos  ·  ano  ·  [IMDb] nota ---------------
  //
  // Uma linha so, na ordem do aparelho. O selo do IMDb entra AQUI, no fim dos
  // grupos, e nao encostado na borda direita da tela: ele estava orfao, a mais
  // de 1000px do texto de que faz parte, porque o valor herdado era
  // NV_DETW_DIR.
  //
  // Dois pontos separadores diferentes, e a diferenca de cor e o que agrupa a
  // linha — ver desenhaPonto.
  {
    float x = NV_DETW2_X, yc = yMeta1 + NV_DETW2_M1_H * 0.5f;
    const CatItem *badgeItem=cat_item(idx);
    if(badgeItem)x+=badges_draw(badges_provider(badgeItem->providerName),x,yc-14,150,28,a);
    int something = 0;
    // GENEROS sem o primeiro campo. `genero` vem do catalogo como
    // "Programa de TV · Ação · Aventura" e o primeiro trecho e sempre o TIPO
    // (ver catalogo.c:539) — a referencia nao o mostra na linha de meta, so os
    // generos. Cada um vira um trecho proprio com o "•" entre eles.
    const char *g = genreOf(idx);
    if (g) {
      const char *p = strstr(g, "\xc2\xb7");
      while (p) {
        char term[80]; size_t n;
        p += 2; while (*p == ' ') p++;
        const char *end = strstr(p, "\xc2\xb7");
        n = end ? (size_t)(end - p) : strlen(p);
        while (n && p[n-1] == ' ') n--;
        if (n && n < sizeof term) {
          memcpy(term, p, n); term[n] = 0;
          if (something) {
            drawDot(x + NV_DETW2_BULLET_SEP, yc, 0.702f, a);   // 179
            x += NV_DETW2_BULLET_SEP * 2 + NV_DETW2_DOT_D;
          }
          TxtLine lt = txt_line(TXT_DET_SIN, term, 179, 179, 179, 255);
          txt_draw_alpha(lt, x, yc - lt.h * 0.5f, a);
          x += lt.w; something = 1;
        }
        p = end;
      }
    }
    // ANO. Em serie a referencia escreve "2023-" e em filme a data cheia; o
    // catalogo so guarda o ano nos dois casos (descoberta.c corta o travessao
    // da serie de proposito), entao sai o ano. Vazio quando o metadado ainda
    // nao chegou — e ai o grupo inteiro some, sem valor de reserva.
    if (year[0]) {
      if (something) { drawDot(x + NV_DETW2_SEP, yc, 0.502f, a);    // 128
                  x += NV_DETW2_SEP * 2 + NV_DETW2_DOT_D; }
      TxtLine la = txt_line(TXT_DET_SIN, year, 179, 179, 179, 255);
      txt_draw_alpha(la, x, yc - la.h * 0.5f, a);
      x += la.w; something = 1;
    }
    if (ci && ci->score > 0) {
      if (something) { drawDot(x + NV_DETW2_SEP, yc, 0.502f, a);
                  x += NV_DETW2_SEP * 2 + NV_DETW2_DOT_D; }
      x += drawBadgeImdb(x, yc, ci->score, a);
    }
    const int sources[] = { EX_TOMATOES, EX_TRAKT };
    for(int i=0;i<2;i++) {
      int n=extras_score(sources[i]);
      if(n<=0) continue;
      // Rotten Tomatoes: tomate FRESCO de 60% para cima, o RESPINGO verde
      // abaixo — e a convencao do proprio site, e o icone e que diz o
      // veredito antes do numero. Trakt: o WORDMARK (nome), nao o icone.
      const char *brand;
      if(sources[i]==EX_TOMATOES) brand=extras_path_brand_name(n>=600?"tomatoes_fresh":"tomatoes_rotten");
      else brand=extras_path_brand_name("trakt_wordmark");
      GLuint logo=tex_get(brand);
      char value[20];snprintf(value,sizeof value,"%d%%",n/10);
      TxtLine lv=txt_line(TXT_DET_META2,value,220,220,225,255);
      float mh=sources[i]==EX_TRAKT?22.0f:32.0f,mw=mh;
      if(logo){float ap=tex_aspect(brand);if(ap>0)mw=mh*ap;if(mw>110)mw=110;}
      if(x+24+mw+10+lv.w>NV_DETW2_X+NV_HERO_SIN_W)break;
      x+=24;
      // GFX_TEXTO e nao GFX_SNAP: o SNAP ignora o alfa da textura e o tomate saia
      // com um quadrado escuro em volta. O TEXTO preserva o RGB e usa o alfa.
      // O wordmark do Trakt e escuro: vai por GFX_MARCA, que tinge o alfa.
      if(logo){GfxMode m=sources[i]==EX_TRAKT&&tex_brand_dark(brand)?GFX_BRAND:GFX_TEXT;
        gfx_rect((GfxRect){x,yc-mh*.5f,mw,mh},logo,m,0,0,0,0,.93f,.94f,.96f,a);}
      else {TxtLine label=txt_line(TXT_MINI,extras_source_brand(sources[i]),200,200,205,255);
        txt_draw_alpha(label,x,yc-label.h*.5f,a);mw=label.w;}
      x+=mw+10;txt_draw_alpha(lv,x,yc-lv.h*.5f,a);x+=lv.w;
    }
  }

  // --- meta linha 2: [classificacao | status]  ·  duracao  ·  pais -----------
  //
  // A classificacao indicativa e o status da producao vivem DENTRO do mesmo
  // selo de contorno, com uma divisoria entre eles ("TV-MA | RENOVADA",
  // "R | LANÇADO"). Aqui a classificacao era um selo solto na linha de cima.
  //
  // STATUS: o CatItem nao tem o campo. Fica NULL, e o selo sai so com a
  // classificacao — sem divisoria e sem texto de reserva. Carimbar "LANÇADO"
  // em tudo seria repetir o erro que ja tirou a classificacao "14" fixa e o
  // elenco de demonstracao daqui.
  //
  // DURACAO so em FILME. Em serie o segundo campo de `meta` e a contagem de
  // temporadas ("3 temporadas"), e a referencia nao a mostra no hero — quem
  // conta temporadas sao as abas logo abaixo da dobra, que esta tela ja
  // desenha. Repetir a informacao aqui seria acrescentar o que o aparelho
  // tirou.
  {
    float x = NV_DETW2_X, yc = yMeta2 + NV_DETW2_BADGE_H * 0.5f;
    int something = 0;
    const char *status=NULL,*raw=extras_profile_status();
    if(isSeries()) {
      if(!strcmp(raw,"canceled")||!strcmp(raw,"Canceled"))status="CANCELLED";
      else if(!strcmp(raw,"ended")||!strcmp(raw,"Ended"))status="ENDED";
      else if(!strcmp(raw,"returning series"))status="NOW SHOWING";
      else if(!strcmp(raw,"renewed"))status="RENEWED";
    }
    if ((ci && ci->age_rating[0]) || status) {
      x += drawBadgeMeta(x, yMeta2, ci && ci->age_rating[0] ? ci->age_rating : status,
                           ci && ci->age_rating[0] ? status : NULL, a);
      something = 1;
    }
    if (!isSeries() && duration[0]) {
      if (something) { drawDot(x + NV_DETW2_SEP, yc, 0.502f, a);
                  x += NV_DETW2_SEP * 2 + NV_DETW2_DOT_D; }
      TxtLine ld = txt_line(TXT_DET_META2, duration, 255, 255, 255, 255);
      txt_draw_alpha(ld, x, yc - ld.h * 0.5f, a);
      x += ld.w; something = 1;
    }
    if (ci && ci->pais[0]) {
      if (something) { drawDot(x + NV_DETW2_SEP, yc, 0.502f, a);
                  x += NV_DETW2_SEP * 2 + NV_DETW2_DOT_D; }
      TxtLine lp = txt_line(TXT_DET_META2, ci->pais, 255, 255, 255, 255);
      txt_draw_alpha(lp, x, yc - lp.h * 0.5f, a);
    }
  }
}

// ---------------------------------------------------------------------------
// PAGINA: temporadas, episodios, abas de informacao, elenco
// ---------------------------------------------------------------------------

// Numero REAL da temporada na posicao `c`. Serie que comeca na 2 (o que
// acontece quando o Cinemeta nao tem a 1) mostrava "Temporada 1" apontando para
// a 2, e a lista abaixo nao batia com o rotulo.
static int seasonIn(int c) {
  const CatItem *ci = cat_item(idx);
  if (ci && ci->nSeasons > 0)
    return (c >= 0 && c < ci->nSeasons) ? ci->seasons[c] : ci->seasons[0];
  return c + 1;
}
static void labelSeason(int c, char *dst, size_t n) {
  int s = seasonIn(c);
  if (s == 0) snprintf(dst, n, "Specials");
  else snprintf(dst, n, "Season %d", s);
}
static float widthSeason(int c) {
  char rot[32]; labelSeason(c, rot, sizeof rot);
  TxtLine l = txt_line(TXT_PLR_BODY, rot, 255, 255, 255, 255);
  return l.w + NV_DETP_TEMP_PADX * 2;
}
static float widthTabInfo(int i) {
  TxtLine l = txt_line(TXT_PLR_BODY, TAB_LABEL[tabIdOf(i)], 255, 255, 255, 255);
  return l.w;
}

// Aba de temporada: 80 de altura, raio 40 (pilula), borda de 1px
// rgba(255,255,255,0.16). Tres estados MEDIDOS, e nao dois:
//   normal      #222     texto rgb(179,179,179)
//   escolhida   #2d2d2d  texto branco
//   com foco    #f5f5f5  texto #111, sem borda
// Sem o estado do meio, o usuario perde de vista em que temporada esta assim
// que o foco desce para a lista.
static void drawSeason(GfxRect r, int c, float f, float a) {
  char rot[32]; labelSeason(c, rot, sizeof rot);
  int sel = (c == season);
  float radius = NV_RADIUS_PILL;
  // TODA temporada e um CHIP, escolhida ou nao. Antes so a escolhida tinha
  // container e as outras eram texto solto sobre o fundo — nao liam como um
  // grupo de botoes, e nao havia como adivinhar que eram clicaveis.
  //
  // MEDIDO na referencia: chips #2D2D2D de 285x83; a ESCOLHIDA se distingue
  // pelo TEXTO branco (o fundo continua #2D2D2D), e a FOCADA INVERTE — fundo
  // quase branco, texto escuro, SEM anel.
  //
  // O que estava aqui punha um anel branco em volta e deixava o miolo em
  // #2D2D2D com texto cinza 170: o item focado virava o MAIS APAGADO da
  // fileira, lido na TV como "desabilitado". A inversao e a mesma linguagem de
  // foco dos botoes circulares do heroi, medida na mesma referencia — foco e
  // "escolhido" deixam de colidir sem precisar de dois tons de cinza que a
  // 3 metros ninguem separa.
  { float base = sel ? 0.21f : 0.133f;
    float luma  = base + (0.961f - base) * f;   // -> #F5F5F5 no foco
    gfx_color(r, radius, luma, luma, luma, a); }
  if (sel && f < 0.99f)
    gfx_rect(r, 0, GFX_RING, 0, 1.5f / r.h, 0, radius,
             0.76f, 0.77f, 0.79f, 0.5f * (1 - f) * a);
  // Texto: cinza quando so existe, branco quando escolhido, ESCURO quando
  // focado. Interpolado por `f` para acompanhar a mola em vez de estalar.
  { float claro = sel ? 255.0f : 179.0f;
    float v = claro + (17.0f - claro) * f;     // -> #111 no foco
    int color = (int)(v + 0.5f);
    TxtLine l = txt_line(TXT_PLR_BODY, rot, color, color, color, 255);
    // 500 de peso na Inter Regular: uma segunda passada meio pixel a direita.
    txt_weight(l, r.x + (r.w - l.w) * 0.5f, r.y + (r.h - l.h) * 0.5f, a, 0.5f); }
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
static void veilEpisode(GfxRect th, float a) {
  static const float STOPPED[5] = { 0.00f, 0.22f, 0.52f, 0.82f, 1.00f };
  static const float ALFA[5]   = { 0.06f, 0.18f, 0.62f, 0.86f, 0.95f };
  const int STEPS = 14;
  float accum = 0.0f;
  for (int i = 0; i <= STEPS; i++) {
    float u = (float)i / STEPS;
    // Alvo interpolado linearmente por partes, como o `linear-gradient`.
    float target = ALFA[4];
    for (int k = 0; k < 4; k++)
      if (u <= STOPPED[k + 1]) {
        float d = STOPPED[k + 1] - STOPPED[k];
        target = ALFA[k] + (ALFA[k + 1] - ALFA[k]) * (d > 0 ? (u - STOPPED[k]) / d : 0);
        break;
      }
    // Quanto ESTA faixa precisa acrescentar para que o acumulado bata no alvo.
    float d = (target - accum) / (1.0f - accum);
    if (d <= 0.001f) continue;
    accum = target;
    float top = th.y + th.h * u;
    GfxRect track = { th.x, top, th.w, th.y + th.h - top };
    if (track.h < 2.0f) continue;
    float radius = NV_DETP_EP_RADIUS / (track.w < track.h ? track.w : track.h);
    if (radius > 0.5f) radius = 0.5f;
    gfx_color(track, radius, 0, 0, 0, d * a);
  }
}

// Card de episodio: 640x422, com a miniatura de 640x414 e TODO o texto dentro
// dela, sobre o degrade. E a diferenca estrutural com o que estava aqui antes
// (miniatura em cima, texto embaixo, que e o app da Apple TV).
static void drawEpisode(GfxRect r, int c, float f, float a, Uint32 now) {
  (void)now;
  const CatEp *ep = cat_episode(idx, c);
  GfxRect th = { r.x, r.y, r.w, NV_DETP_EP_THUMB_H };
  float radiusTh = NV_DETP_EP_RADIUS / NV_DETP_EP_THUMB_H;

  // Anel de foco: no web e um box-shadow na MINIATURA, nao no card, e nao ha
  // escala nenhuma (`transform: none`).
  if (f > 0.01f) {
    GfxRect ring = { th.x - NV_DETP_RING, th.y - NV_DETP_RING,
                     th.w + NV_DETP_RING * 2, th.h + NV_DETP_RING * 2 };
    gfx_color(ring, radiusTh, 1, 1, 1, f * a);
  }

  const CatItem *series = cat_item(idx);
  const char *art = (ep && ep->thumb[0]) ? ep->thumb
                     : (series && series->backdrop[0] ? series->backdrop : NULL);
  GLuint t2 = art ? tex_get_width(art, th.w) : 0;
  if (t2) {
    gfx_tex_aspect_current = tex_aspect(art);
    gfx_rect(th, t2, GFX_CARD, 0, 0, 0, radiusTh, 0, 0, 0, a);
    gfx_tex_aspect_current = 0.0f;
  } else gfx_color(th, radiusTh, 0.133f, 0.133f, 0.133f, a);
  veilEpisode(th, a);

  // EPISODIO JA ASSISTIDO, segundo o Trakt: mascara escura sobre a miniatura e
  // um check no canto. Pedido do dono, e resolve uma pergunta que a lista nao
  // respondia — onde ele parou.
  //
  // A mascara vem DEPOIS do veu de texto de proposito: ela precisa cobrir a
  // miniatura inteira, inclusive a parte ja escurecida, senao o card visto e o
  // nao visto ficam parecidos justo em cima do texto.
  if (ep && extras_ep_watched(ep->season, ep->episode)) {
    float d = 36.0f;
    GfxRect badge = { th.x + th.w - d - 16.0f, th.y + 16.0f, d, d };
    gfx_color(th, radiusTh, 0.0f, 0.0f, 0.0f, 0.22f * a);
    gfx_color(badge, 0.5f, 1, 1, 1, 0.92f * a);
    // O check e feito de dois tracos; sem rotacao no gfx, dois retangulos finos
    // em degraus dao a mesma leitura no tamanho de um selo.
    { float cx2 = badge.x + d * 0.5f, cy2 = badge.y + d * 0.5f;
      int k;
      for (k = 0; k < 4; k++)
        gfx_color((GfxRect){ cx2 - 9.0f + k * 2.0f, cy2 - 1.0f + k * 2.0f, 3, 3 },
                0.4f, 0.05f, 0.05f, 0.05f, a);
      for (k = 0; k < 6; k++)
        gfx_color((GfxRect){ cx2 - 1.0f + k * 2.0f, cy2 + 5.0f - k * 2.0f, 3, 3 },
                0.4f, 0.05f, 0.05f, 0.05f, a); }
  }

  // NADA DE RESERVA INVENTADA. Aqui as quatro linhas caiam numa tabela de
  // demonstracao (nome, duracao, data e sinopse de "Shrinking"), entao um
  // episodio sem dado nao aparecia vazio: aparecia com o TEXTO DE OUTRA SERIE,
  // indistinguivel de informacao real. Campo ausente agora fica ausente, e o
  // desenho abaixo ja omite cada pedaco que vier vazio.
  const char *epName = (ep && ep->name[0])    ? ep->name    : NULL;
  const char *epDuration  = (ep && ep->duration[0]) ? ep->duration : NULL;
  const char *epDate = (ep && ep->date[0])    ? ep->date    : NULL;
  const char *epSin  = (ep && ep->synopsis[0]) ? ep->synopsis : NULL;
  int epNum = ep ? ep->episode : c + 1;

  float tx = r.x + NV_DETP_EP_DFLT;

  // Ausencia de check nao afirma que o historico ja chegou. Evita um selo
  // "nao assistido" inventado enquanto o Trakt ainda esta consultando.

  // Selo "EPISÓDIO n": caixa de 43 de altura, raio 12, fundo escuro
  // translucido, texto em caixa alta.
  //
  // Voltou a ser "EPISÓDIO n" e nao "T2E4". A forma curta entrou por uma
  // captura antiga do dono, mas a referencia no aparelho escreve "EPISÓDIO 1"
  // por extenso — e o argumento de que a forma curta "diz de que temporada e"
  // nao se sustenta: o card so aparece dentro da aba da temporada escolhida,
  // que esta desenhada logo acima dele.
  { char header[24];
    snprintf(header, sizeof header, "EPISODE %d", epNum);
    TxtLine l = txt_line(TXT_CAPTION2, header, 255, 255, 255, 255);
    float w = l.w + NV_DETP_EP_BADGE_PADX * 2;
    GfxRect s = { tx, r.y + NV_DETP_EP_BADGE_Y, w, NV_DETP_EP_BADGE_H };
    gfx_color(s, 12.0f / NV_DETP_EP_BADGE_H, 0.05f, 0.05f, 0.06f, 0.78f * a);
    txt_weight(l, s.x + NV_DETP_EP_BADGE_PADX,
             s.y + (NV_DETP_EP_BADGE_H - l.h) * 0.5f, a, 1.0f); }

  // Titulo: 32/800. O 800 nao existe na familia embarcada e o 32 so existe em
  // Regular na tabela de estilos, entao vem de tres passadas.
  // Sem nome do episodio, "Episodio N" — que e um rotulo VERDADEIRO, deduzido
  // do numero, e nao o titulo de outra serie.
  { char fallback[32];
    const char *name = epName;
    if (!name) { snprintf(fallback, sizeof fallback, "Episode %d", epNum);
                 name = fallback; }
    TxtLine l = txt_line_trim(TXT_PLR_BODY, name, 255, 255, 255, 255,
                                 NV_DETP_EP_TEXT_W);
    txt_weight(l, tx, r.y + NV_DETP_EP_TITLE_Y, a, 1.4f); }

  // Sinopse: tres linhas, como a referencia, com truncamento do bloco.
  // Sem sinopse o espaco
  // fica vazio: melhor um card com menos texto que um card com texto errado.
  if (epSin)
    txt_block(TXT_DET_SIN, epSin, 255, 255, 255, tx, r.y + NV_DETP_EP_SIN_Y,
              NV_DETP_EP_TEXT_W, NV_DETP_EP_LD_SIN, a * 0.9f, 3);

  // Meta: relogio + duracao + data, 20/400 rgb(179,179,179), com 38 de folga
  // entre os dois blocos.
  { float x = tx, y = r.y + NV_DETP_EP_META_Y;
    // Relogio de 28x28. O glifo do web e um disco CHEIO em rgb(179,179,179) com
    // os ponteiros VAZADOS — o `path` do SVG recorta o L do ponteiro do disco.
    // Aqui o vazado sai pintando os ponteiros de preto por cima: naquele ponto
    // da miniatura o veu ja esta em 0.95, entao o que estaria atras do recorte e
    // praticamente preto. Desenhar os ponteiros na MESMA cor do disco, como
    // estava, some com eles e deixa so uma bolinha cinza.
    // O relogio so entra COM a duracao ao lado. Sozinho ele nao e um icone, e
    // um rotulo sem valor: um disco cinza solto no canto do card, que le como
    // defeito de desenho.
    if (epDuration) {
      float cx = x + NV_DETP_EP_ICON * 0.5f, cy = y + NV_DETP_EP_ICON * 0.5f;
      GfxRect aro = { x, y, NV_DETP_EP_ICON, NV_DETP_EP_ICON };
      GfxRect pv = { cx - 1.5f, cy - 8, 3, 9.5f };
      GfxRect ph = { cx - 1.5f, cy - 1.5f, 8, 3 };
      gfx_rect(aro, 0, GFX_RING, 0, 2.0f / NV_DETP_EP_ICON, 0, 0.5f,
               0.76f, 0.77f, 0.79f, a);
      gfx_color(pv, 0.0f, 0.76f, 0.77f, 0.79f, a);
      gfx_color(ph, 0.0f, 0.76f, 0.77f, 0.79f, a);
      x += NV_DETP_EP_ICON + 8.0f;
      { TxtLine ld = txt_line(TXT_CAPTION2, epDuration, 179, 179, 179, 255);
        txt_draw_alpha(ld, x, y, a); x += ld.w + 16; }
    }
    // Extras fornece avaliacao Trakt por episodio, nao IMDb. Nunca usar
    // a nota da serie ou o selo de outro provedor neste rodape.
    int score = 0;
    if (ep) for (int st = 0; st < extras_n_seasons(); st++) {
      if (extras_season_number(st) != ep->season) continue;
      for (int ei = 0; ei < extras_n_eps(st); ei++)
        if (extras_ep_number(st, ei) == ep->episode) {
          score = extras_ep_score(st, ei); break;
        }
      break;
    }
    if (score > 0) {
      char value[32]; snprintf(value, sizeof value, "Trakt %d.%d", score / 10, score % 10);
      TxtLine ln = txt_line(TXT_CAPTION2, value, 229, 231, 236, 255);
      GfxRect badge = { x, y - 3, ln.w + 16, NV_DETP_EP_ICON + 6 };
      gfx_color(badge, 0.18f, 0.15f, 0.15f, 0.17f, 0.94f * a);
      txt_draw_alpha(ln, x + 8, y, a);
      x += badge.w + 16;
    }
    // A DATA vai para a direita do card, como na referencia: a esquerda fica so
    // a duracao, e as duas deixam de disputar a mesma linha corrida.
    if (epDate) {
      const char *date = epDate;
      size_t nDate = strlen(epDate);
      if (!settings_date_full() && nDate >= 4) date = epDate + nDate - 4;
      float available = r.x + r.w - NV_DETP_EP_DFLT - x;
      if (available > 48) {
        TxtLine lf = txt_line_trim(TXT_CAPTION2, date, 179, 179, 179, 255, available);
        txt_draw_alpha(lf, r.x + r.w - NV_DETP_EP_DFLT - lf.w, y, a);
      }
    } }

  // Barra de progresso: 576x8 a 16px da base da miniatura, trilho
  // rgba(0,0,0,0.45) e preenchimento rgb(158,158,158). So aparece entre 2% e
  // 98% — e o mesmo intervalo do web, e e o que faz um episodio recem-comecado
  // nao ganhar uma barra de largura zero.
  { int progress = 0;
    const CatItem *ci = cat_item(idx);
    if (ci && ci->progress > 0 && ep && ci->season == ep->season &&
        ci->episode == ep->episode) progress = ci->progress;
    if (progress > 2 && progress < 98) {
      GfxRect tr = { tx, r.y + NV_DETP_EP_BAR_Y, NV_DETP_EP_TEXT_W,
                     NV_DETP_EP_BAR_H };
      GfxRect at = { tr.x, tr.y, tr.w * (progress / 100.0f), tr.h };
      gfx_color(tr, 0.5f, 0, 0, 0, 0.45f * a);
      gfx_color(at, 0.5f, 0.62f, 0.62f, 0.62f, a);
    } }
}

// Abas de informacao: texto puro, sem pilula. Escolhida (ou focada) em branco,
// as outras em #808080; o divisor "|" e 32/700 #808080. O foco no web e
// `transform: scale(1.03)` — o unico lugar desta tela que escala.
static void drawTabInfo(float x, float y, int i, float f, float a) {
  int sel = (i == tabInfo);
  int base = sel ? 255 : 128;
  int color = (int)(base + (255 - base) * f);
  TxtLine l = txt_line(TXT_PLR_BODY, TAB_LABEL[tabIdOf(i)], color, color, color, 255);
  txt_weight(l, x, y + (NV_DETP_TAB_H - l.h) * 0.5f, a, 0.5f + f * 0.6f);
}

// Elenco: avatar redondo de 140 ALINHADO A ESQUERDA do card de 220 (nao
// centralizado, que era o desenho anterior), nome 26/500 rgb(179,179,179) e
// papel 21/400 rgb(128,128,128) abaixo dele.
// --- card de TRAILER ---------------------------------------------------------
//
// Miniatura 520x292 raio 24, selo de play ao centro, nome embaixo e o tipo em
// cinza. A miniatura vem de img.youtube.com por URL previsivel, e tex_obter
// baixa e cacheia sozinho — nao ha codigo de rede aqui.
//
// NAO E FOCAVEL, e isso e decisao, nao pendencia: este app nao tem reprodutor
// de YouTube. A mesma regra ja tirou o botao de trailer do hero (detail.c) e o
// glifo do YouTube do terceiro circular (gfx.c) — um controle que promete o que
// nao cumpre e pior que a ausencia dele. O card entra na composicao para a
// pagina nao mentir sobre o que o filme tem; abrir, nao abre.
static void drawTrailer(float x, float y, int c, float a) {
  const char *mini = extras_trailer_thumb(c);
  GfxRect v = { x, y, NV_DETF_TR_W, NV_DETF_TR_VIDEO_H };
  float radius = NV_DETF_TR_RADIUS / NV_DETF_TR_VIDEO_H;   // fracao do MENOR lado
  GLuint tex = (mini && mini[0]) ? tex_get_width(mini, NV_DETF_TR_W) : 0;

  if (tex) {
    gfx_tex_aspect_current = tex_aspect(mini);
    gfx_rect(v, tex, GFX_CARD, 0, 0, 0, radius, 1, 1, 1, a);
    gfx_tex_aspect_current = 0.0f;
  } else {
    gfx_color(v, radius, 0.13f, 0.13f, 0.13f, a);
  }

  // Selo de play: disco escuro e o triangulo por cima, centrados na miniatura.
  { float d = NV_DETF_TR_PLAY_D;
    GfxRect disk = { x + (NV_DETF_TR_W - d) * 0.5f,
                      y + (NV_DETF_TR_VIDEO_H - d) * 0.5f, d, d };
    GfxRect tri   = { disk.x + d * 0.34f, disk.y + d * 0.28f,
                      d * 0.36f, d * 0.44f };
    gfx_color(disk, 0.5f, 0.0f, 0.0f, 0.0f, a * 0.48f);
    gfx_rect(tri, 0, GFX_PLAY, 0, 0, 0, 0.0f, 1, 1, 1, a); }

  { TxtLine ln = txt_line_trim(TXT_ROW_TITLE, extras_trailer_name(c),
                                  245, 248, 255, 255, NV_DETF_TR_W);
    txt_draw_alpha(ln, x, y + NV_DETF_TR_NAME_DY, a); }
  { TxtLine lt = txt_line(TXT_CAPTION2, "YouTube", 179, 179, 179, 255);
    txt_draw_alpha(lt, x, y + NV_DETF_TR_KIND_DY, a * 0.9f); }
}

// --- tabela "Detalhes do Filme" ---------------------------------------------
//
// Duas colunas: chave em cinza a esquerda, valor em branco numa coluna FIXA.
// A coluna do valor nao segue a largura da chave — se seguisse, cada linha
// comecaria num x diferente e a tabela serrilharia. Divisoria de 1px sob cada
// linha menos a ultima, como na referencia.
//
// Recebe `f` so para saber se a secao esta focada: a tabela nao tem item a
// item, entao o foco nela e a propria secao, e o realce e sutil de proposito —
// nao ha o que escolher aqui, so o que ler.
static void drawDetails(float x, float y, float f, float a) {
  LineDet l[NV_DETF_DET_MAXL];
  int n = buildDetails(l, NV_DETF_DET_MAXL), i;
  for (i = 0; i < n; i++) {
    float ly = y + i * NV_DETF_DET_LINE;
    float yc = ly + NV_DETF_DET_LINE * 0.5f;
    TxtLine lk = txt_line(TXT_DET_META2, l[i].key, 150, 154, 163, 255);
    TxtLine lv = txt_line_trim(TXT_DET_META, l[i].value, 235, 238, 245, 255,
                                  NV_DETF_DET_W - NV_DETF_DET_KEY_W);
    txt_draw_alpha(lk, x, yc - lk.h * 0.5f, a * 0.9f);
    txt_draw_alpha(lv, x + NV_DETF_DET_KEY_W, yc - lv.h * 0.5f, a);
    if (i < n - 1) {
      GfxRect d = { x, ly + NV_DETF_DET_LINE - 1.0f, NV_DETF_DET_W, 1.0f };
      gfx_color(d, 0.0f, 1, 1, 1, a * (0.10f + 0.06f * f));
    }
  }
}

static void drawCast(float x, float y, int c, float f, float a) {
  const CatItem *ci = cat_item(idx);
  const char *name = NULL, *role = NULL, *photo = NULL;
  if (ci && c < ci->nCast) {
    name = ci->cast[c].name;
    role = ci->cast[c].role;
    if (ci->cast[c].photo[0]) photo = ci->cast[c].photo;
  }
  if (ci && ci->nCast > 0 && c >= ci->nCast) return;
  // SEM ELENCO NAO SE INVENTA ELENCO. Aqui havia uma reserva cravada
  // (`ELENCO[c % N_ELENCO]`) que preenchia a fileira com o elenco de
  // "Shrinking" — e o resultado era o Homem-Aranha creditando Jason Segel e
  // Harrison Ford, com cara de dado real. Mesmo defeito do "14" que estava
  // cravado em descoberta.c: valor de demonstracao exibido como informacao.
  //
  // A fileira nem chega aqui sem dado, porque secaoN devolve 0 (e focus_mover
  // pula fileira vazia). Este `return` e a segunda tranca.
  if (!name || !name[0]) return;

  GfxRect av = { x, y, NV_DETP_EL_AVATAR, NV_DETP_EL_AVATAR };
  if (f > 0.01f) {
    GfxRect ring = { av.x - NV_DETP_RING, av.y - NV_DETP_RING,
                     av.w + NV_DETP_RING * 2, av.h + NV_DETP_RING * 2 };
    gfx_color(ring, 0.5f, 1, 1, 1, f * a);
  }
  GLuint t2 = photo ? tex_get_width(photo, NV_DETP_EL_AVATAR) : 0;
  if (t2) {
    gfx_tex_aspect_current = tex_aspect(photo);
    gfx_rect(av, t2, GFX_CARD, 0, 0, 0, 0.5f, 0, 0, 0, a);
    gfx_tex_aspect_current = 0.0f;
  } else {
    // Sem foto, a inicial sobre #222 (#303030 com foco) — e o que o web faz
    // com `.movie-cast-avatar-fallback`.
    float luma = 0.133f + 0.055f * f;
    gfx_color(av, 0.5f, luma, luma, luma, a);
    char start[5] = {0};
    for (int k = 0; k < 4 && name[k] && (unsigned char)name[k] >= 0x20; k++) {
      start[k] = name[k];
      if ((name[k] & 0xC0) != 0x80) { if (k) { start[k] = 0; break; } }
    }
    TxtLine li = txt_line(TXT_TITLE3, start, 210, 212, 220, 255);
    txt_draw_alpha(li, av.x + (av.w - li.w) * 0.5f,
                       av.y + (av.h - li.h) * 0.5f, a * 0.9f);
  }
  float yn = y + NV_DETP_EL_AVATAR + NV_DETP_EL_NAME_DY;
  TxtLine ln = txt_line_trim(TXT_CALLOUT, name, 179, 179, 179, 255, NV_DETP_EL_W);
  txt_draw_alpha(ln, x, yn, a);
  if (role && role[0]) {
    TxtLine lp = txt_line_trim(TXT_CAPTION2, role, 128, 128, 128, 255,
                                  NV_DETP_EL_W);
    txt_draw_alpha(lp, x, yn + NV_DETP_EL_ROLE_DY, a * 0.95f);
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
#define RATING_CARD_W  160.0f
#define RATING_CARD_H  120.0f
#define RATING_CARD_GAP 16.0f
// Contorno de 1px a 16%, em quatro faixas: nao ha helper de borda no gfx e
// este mesmo desenho serve o cartao de nota e o de comentario.
// `raio` em PIXEIS; a conversao para a fracao do menor lado que o gfx espera e
// feita aqui. Passar 14 direto (o raio do CSS) fazia o SDF saturar e o cartao
// saia de canto reto — o valor do gfx e fracao, nao pixel.
// RAIO DE CARTAZ, em fracao do menor lado — o SDF do shader e normalizado.
//
// Existe porque cinco pontos deste arquivo faziam `NV_RAIO_CARD / largura`, e
// NV_RAIO_CARD JA E UMA FRACAO (0,055). Dividir de novo pela largura dava
// ~0,0003, ou seja canto reto: era por isso que os cartazes de "Recomendações"
// e as fotos da filmografia saiam quadrados enquanto os da home eram
// arredondados. O valor vem do mesmo `posterCardCornerRadiusDp` da home, para
// as duas telas terem o mesmo canto.
static float radiusPoster(float w, float h) {
  float smaller = w < h ? w : h;
  if (smaller <= 0.0f) return NV_RADIUS_CARD;
  return settings_radius_poster_px() / smaller;
}

static void frame(GfxRect r, float radius, float a) {
  float smaller = r.w < r.h ? r.w : r.h;
  radius = smaller > 0.0f ? radius / smaller : 0.0f;
  // CINZA NEUTRO, o mesmo #2D2D2D das pilulas de temporada e do resto dos
  // componentes. O azul-escuro que estava aqui (#12171F) era o unico tom da
  // familia nesta tela: o cartao de comentario lia como peca de outro app.
  gfx_color(r, radius, 0.176f, 0.176f, 0.176f, 0.94f * a);
  // A BORDA SEGUE O CANTO. Eram QUATRO RETANGULOS RETOS de 1 px, um por lado —
  // eles cruzavam por fora do arredondamento e desenhavam bico nos quatro
  // cantos, que e o que o dono viu como "borda nao arredondada". GFX_ANEL usa o
  // mesmo SDF do preenchimento, entao o traco acompanha o raio.
  gfx_rect(r, 0, GFX_RING, 0, 1.0f / (smaller > 0.0f ? smaller : 1.0f), 0, radius,
           1, 1, 1, 0.14f * a);
}

// Cartao de nota: a MARCA em cima e o valor embaixo, como o .movie-rating-card
// do web (logo 56x28, valor 34/800). As marcas sao os proprios arquivos do app
// web convertidos para PNG em art/marcas — desenhar um retangulo colorido com
// as iniciais, que era o que estava aqui, fica com cara de esboco ao lado de
// componentes que usam arte de verdade.
static void cardScore(float x, float y, const char *brand, const char *value,
                       float a) {
  GfxRect card = { x, y, RATING_CARD_W, RATING_CARD_H };
  const char *cam = brand;
  GLuint t;
  frame(card, 14.0f, a);
  t = tex_get(cam);
  { TxtLine lv = txt_line(TXT_TITLE3, value, 245, 248, 255, 255);
    float hLogo = 28.0f, hBlock = hLogo + 12.0f + lv.h;
    float yb = y + (RATING_CARD_H - hBlock) * 0.5f;
    if (t) {
      float ap = tex_aspect(cam);
      float w;
      if (ap <= 0.0f) ap = 2.0f;
      w = hLogo * ap;
      if (w > 96.0f) { w = 96.0f; hLogo = w / ap; }
      { GfxRect rl = { x + (RATING_CARD_W - w) * 0.5f, yb, w, hLogo };
        // GFX_CARD e nao GFX_TEXTO: o modo de texto pinta a forma com a COR
        // dada e joga fora o RGB da textura — o logo do IMDb sairia como uma
        // silhueta branca. Aqui a marca tem de manter a cor dela.
        gfx_tex_aspect_current = 0.0f;
        gfx_rect(rl, t, GFX_CARD, 0, 0, 0, 0.0f, 0, 0, 0, a); }
    }
    txt_draw_alpha(lv, x + (RATING_CARD_W - lv.w) * 0.5f, yb + 28.0f + 12.0f, a);
  }
}

// A FILEIRA de notas, na ordem do web: trakt, imdb, tmdb, tomatoes, audience,
// metacritic, letterboxd. So entra a fonte que TEM nota — o web faz o mesmo
// (`.filter(([,,value]) => value != null)`), e uma fileira de "-" nao informa
// nada. IMDb vem do catalogo quando o mdbList nao respondeu por ele.
// PASTILHA DE NOTA DE EPISODIO. As cores e as faixas sao as do web
// (ratingToneClass, metaDetailsScreen.js:912, e as regras
// .series-episode-rating-chip.*): >=9 excelente, >=8 otimo, >=7.5 bom,
// >=7 misto, >=6 ruim, >0 pessimo. O numero e a nota do Trakt, nao do IMDb.
static void colorOfScore(int scoreDec, float *r, float *g, float *b, float *tx) {
  float rr, gg, bb, t;
  if      (scoreDec >= 90) { rr=0.078f; gg=0.643f; bb=0.302f; t=0.97f; }  /* #14a44d */
  else if (scoreDec >= 80) { rr=0.180f; gg=0.733f; bb=0.404f; t=0.97f; }  /* #2ebb67 */
  else if (scoreDec >= 75) { rr=0.243f; gg=0.722f; bb=0.400f; t=0.97f; }  /* #3eb866 */
  else if (scoreDec >= 70) { rr=0.906f; gg=0.706f; bb=0.196f; t=0.10f; }  /* #e7b432 */
  else if (scoreDec >= 60) { rr=0.906f; gg=0.298f; bb=0.235f; t=0.97f; }  /* #e74c3c */
  else if (scoreDec >  0)  { rr=0.388f; gg=0.224f; bb=0.455f; t=0.97f; }  /* #633974 */
  else                    { rr=0.925f; gg=0.816f; bb=0.239f; t=0.09f; }  /* #ecd03d */
  *r = rr; *g = gg; *b = bb; *tx = t;
}

#define RAT_PIL_W    86.0f
#define RAT_PIL_H    62.0f
#define RAT_PIL_GAP  10.0f
#define RAT_TEMP_H   38.0f
#define RAT_TEMP_GAP 10.0f

// Painel de SERIE: fileira de temporadas e a grade de pastilhas por episodio.
// E o renderSeriesRatingsPanel do web, que ate agora nao tinha fonte aqui — a
// aba de serie caia nos mesmos cartoes do filme.
static void drawScoresEpisode(float x, float y, float a) {
  int nt = extras_n_seasons(), t, i, ne;
  if (ratTemp >= nt) ratTemp = 0;
  for (t = 0; t < nt; t++) {
    char rot[8];
    float bx = x + t * (58.0f + RAT_TEMP_GAP);
    GfxRect r = { bx, y, 58.0f, RAT_TEMP_H };
    int sel = (t == ratTemp);
    snprintf(rot, sizeof rot, "T%d", extras_season_number(t));
    gfx_color(r, 0.5f, 1, 1, 1, (sel ? 0.28f : 0.14f) * a);
    { TxtLine l = txt_line(TXT_DET_META2, rot, 241, 247, 254, 255);
      txt_draw_alpha(l, bx + (58.0f - l.w) * 0.5f,
                         y + (RAT_TEMP_H - l.h) * 0.5f, a); }
  }
  ne = extras_n_eps(ratTemp);
  { float gy = y + RAT_TEMP_H + 18.0f;
    for (i = 0; i < ne; i++) {
      float gx = x + i * (RAT_PIL_W + RAT_PIL_GAP);
      int nd = extras_ep_score(ratTemp, i);
      float cr, cg, cb, tx;
      char ep[8], nv[8];
      if (gx + RAT_PIL_W > NV_SCREEN_W - NV_DETP_X) break;
      colorOfScore(nd, &cr, &cg, &cb, &tx);
      gfx_color((GfxRect){ gx, gy, RAT_PIL_W, RAT_PIL_H }, 14.0f / RAT_PIL_H,
              cr, cg, cb, a);
      snprintf(ep, sizeof ep, "E%d", extras_ep_number(ratTemp, i));
      if (nd > 0) snprintf(nv, sizeof nv, "%.1f", nd / 10.0f);
      else        snprintf(nv, sizeof nv, "-");
      // 14/700 no rotulo e 28/800 no valor, do web
      // (.series-episode-rating-ep e .series-episode-rating-val). TXT_TITULO3 e
      // 48 e estourava a pastilha de 62 — o "E1" era empurrado para fora dela.
      { int c = (int)(tx * 255.0f);
        TxtLine le = txt_line(TXT_MINI, ep, c, c, c, 255);
        TxtLine lv = txt_line(TXT_ROW_TITLE, nv, c, c, c, 255);
        float h = le.h + 2.0f + lv.h;
        float yb = gy + (RAT_PIL_H - h) * 0.5f;
        txt_draw_alpha(le, gx + (RAT_PIL_W - le.w) * 0.5f, yb, a);
        txt_draw_alpha(lv, gx + (RAT_PIL_W - lv.w) * 0.5f, yb + le.h + 2.0f, a); }
    } }
}

static void drawRatings(float x, float y, float a) {
  int i, col = 0;
  for (i = 0; i < EX_NSOURCES; i++) {
    int v = extras_score(i);
    char txt[8];
    // Sem mdbList o IMDb ainda vem do catalogo, que guarda 0..100; no vetor a
    // escala e "cru x 10", e para o imdb o cru e 0..10.
    if (i == EX_IMDB && !v) v = scoreOf(idx);
    if (!v) continue;
    if (extras_source_percentual(i))
      snprintf(txt, sizeof txt, "%d%%", (v + 5) / 10);
    else
      snprintf(txt, sizeof txt, "%.1f", v / 10.0f);
    cardScore(x + col * (RATING_CARD_W + RATING_CARD_GAP), y,
               extras_path_brand(i), txt, a);
    col++;
  }
}

// Aba "Mais como este": /related do Trakt. Uma coluna de titulos com o ano, e
// nao os posteres do web — o related do Trakt devolve identificador e nome, e
// buscar poster para doze titulos so para pintar esta aba custaria doze
// pedidos de rede a cada abertura. O que a aba precisa responder e "o que mais
// se parece com isto", e o nome responde.
// "Mais como este" em CARTAZES, e nao em lista de texto: e assim que o web
// mostra (renderPreviewRail) e e o que o dono pediu ao ver a lista crua. O
// poster vem do proprio Trakt, com `extended=images` no /related — buscar arte
// noutro servico seria um pedido por titulo so para pintar esta aba.
// "Mais como este" aparece por DOIS caminhos e eles nao sao o mesmo estado:
//   SERIE  -> e uma ABA, desenhada no slot de SEC_ELENCO, com foco proprio
//             (`relFoco`), porque a fileira do elenco tem outra contagem.
//   FILME  -> e uma SECAO propria, SEC_RELACIONADOS, e quem manda e `foco.coluna`.
//
// So o primeiro caso estava tratado. No filme a fileira RECEBIA foco (secaoN
// devolve a contagem certa) mas nada acendia e o OK nao respondia — parecia que
// a secao inteira nao existia para o D-pad. Este par resolve os dois de uma vez.
static int relInList(void) {
  return focus.row == SEC_CAST || focus.row == SEC_RELATED;
}
static int relIndex(void) {
  return (focus.row == SEC_RELATED) ? focus.column : relFocus;
}

static void drawRelated(float x, float y, float a) {
  int n = extras_n_related(), i;
  int inList = relInList();
  int foc = relIndex();
  for (i = 0; i < n && i < 7; i++) {
    float cx = x + i * (REL_CARD_W + REL_CARD_GAP);
    GfxRect r = { cx, y, REL_CARD_W, REL_CARD_H };
    int lit = inList && i == foc;
    const char *po = extras_related_poster(i);
    GLuint t = po[0] ? tex_get_width(po, REL_CARD_W) : 0;
    float radius = radiusPoster(REL_CARD_W, REL_CARD_H);
    if (cx + REL_CARD_W > NV_SCREEN_W - NV_DETP_X) break;
    if (lit) {
      GfxRect ring = { r.x - 4, r.y - 4, r.w + 8, r.h + 8 };
      gfx_color(ring, radius, 1, 1, 1, a);
    }
    if (t) {
      gfx_tex_aspect_current = tex_aspect(po);
      gfx_rect(r, t, GFX_CARD, lit ? 1.0f : 0.0f, 0, 0, radius, 0, 0, 0, a);
      gfx_tex_aspect_current = 0.0f;
    } else {
      gfx_color(r, radius, 0.133f, 0.133f, 0.133f, a);
    }
    { int c = lit ? 255 : 225;
      TxtLine lt = txt_line_trim(TXT_DET_META2, extras_related_title(i),
                                    c, c, c, 255, REL_CARD_W);
      txt_draw_alpha(lt, cx, y + REL_CARD_H + 12.0f, a);
      { const char *year = extras_related_year(i);
        if (year[0]) {
          TxtLine la = txt_line(TXT_MINI, year, 140, 144, 153, 255);
          txt_draw_alpha(la, cx, y + REL_CARD_H + 12.0f + lt.h + 6.0f,
                             a * 0.9f);
        } } }
  }
}

// Aba da COLECAO: as partes da franquia, na ordem que o TMDB devolve. Mesma
// lista vertical de "Mais como este" — o que muda e a fonte e o cabecalho com
// o nome da colecao.
static void drawCollection(float x, float y, float a) {
  int n = extras_n_collection(), i;
  float y0 = y;
  if (extras_collection_name()[0]) {
    TxtLine ln = txt_line_trim(TXT_DET_META2, extras_collection_name(),
                                  150, 154, 163, 255, 900.0f);
    txt_draw_alpha(ln, x, y0, a * 0.9f);
    y0 += ln.h + 16.0f;
  }
  for (i = 0; i < n && i < 7; i++) {
    float yl = y0 + i * 52.0f;
    int lit = (focus.row == SEC_CAST) && i == relFocus;
    int c = lit ? 255 : 225;
    if (lit) {
      GfxRect track = { x - 16.0f, yl - 8.0f, 940.0f, 48.0f };
      gfx_color(track, 10.0f / 48.0f, 1, 1, 1, 0.12f * a);
    }
    { TxtLine lt = txt_line_trim(TXT_DET_META, extras_collection_title(i),
                                    c, c, c, 255, 900.0f);
      txt_draw_alpha(lt, x, yl, a);
      { const char *year = extras_collection_year(i);
        if (year[0]) {
          TxtLine la = txt_line(TXT_DET_META2, year, 150, 154, 163, 255);
          txt_draw_alpha(la, x + lt.w + 18.0f, yl + 2.0f, a * 0.9f);
        } } }
  }
}

// Aba "Comentarios": /comments/likes do Trakt, os mais curtidos primeiro. Uma
// linha com o usuario e as curtidas, e o texto quebrado embaixo.
// Comentarios em CARTOES lado a lado, com a mesma moldura dos cartoes de nota,
// para nao ficarem como texto solto no meio de uma tela feita de componentes.
// Cartao de comentario NO FORMATO DA REFERENCIA. MEDIDO na TCL: 722x466, vao
// de 25, canto ~20. O que havia aqui era 560x240 com o nome e as curtidas na
// MESMA linha — o cartao cabia tres linhas de texto e cortava o resto, e a nota
// de quem comentou nao aparecia.
//
// A referencia separa em tres blocos, e a ordem importa: NOME sozinho no topo,
// TEXTO no meio ocupando o que sobra, e um rodape "10/10  17 curtidas" colado
// na base. Ler o nome, decidir se interessa e so entao ler — nessa ordem.
// Cabecalho da secao, como na referencia: o WORDMARK do trakt, "Comentários" ao
// lado, "Avaliações do Trakt" abaixo, e o seletor "Série | Episódio".
//
// Nada disto existia — os cartoes apareciam soltos, sem dizer de onde vinham
// nem que havia dois conjuntos. O seletor nao e enfeite: comentario de EPISODIO
// e outra consulta no Trakt, e sem ele metade do conteudo era inalcancavel.
#define COM_PILL_H    64.0f
#define COM_PILL_DFLT  34.0f
// Altura do cabecalho, medida do mesmo jeito que cabecalhoComentarios a
// percorre: 46 do titulo + 44 do subtitulo + as pilulas (so em serie) + 28 de
// respiro. Vem de uma funcao e nao de uma constante justamente porque a de
// filme e menor — cravar um numero so faria uma das duas ficar errada.
// BASE DA ABA ATIVA na serie: o y ABSOLUTO onde o conteudo do slot de
// SEC_ELENCO termina.
//
// Existe porque esse slot desenha coisas de alturas MUITO diferentes conforme a
// aba: o elenco tem ~230, mas "Mais como este" tem cartaz de 318 mais o rotulo.
// A secao do Trakt era empilhada a partir da altura do ELENCO sempre, entao ao
// escolher "Recomendações" os cartazes desciam por cima dela. Nao da para usar
// uma altura so: usar a maior afastaria o Trakt do elenco sem motivo, e usar a
// menor e o defeito que o dono viu.
static float baseOfTabActive(void) {
  // Elenco e desenhado no proprio NV_DETP_EL_Y; as outras abas em EL_Y + 40
  // (o `yAba` de desenhaSecao). Sao dois pontos de partida diferentes.
  switch (tabIdOf(tabInfo)) {
    case TAB_RELATED:
      return NV_DETP_EL_Y + 40.0f + REL_CARD_H + 12.0f
           + NV_DETP_EL_LINE * 2.0f;          // titulo + ano sob o cartaz
    case TAB_RATINGS:
      if (isSeries() && extras_n_seasons() > 0)
        return NV_DETP_EL_Y + 40.0f + RAT_TEMP_H + 18.0f + RAT_PIL_H;
      return NV_DETP_EL_Y + 40.0f + RATING_CARD_H;
    case TAB_COLLECTION: {
      int n = extras_n_collection();
      if (n > 7) n = 7;
      return NV_DETP_EL_Y + 40.0f + 30.0f + (float)n * 52.0f;
    }
    default:
      return NV_DETP_EL_Y + NV_DETP_EL_AVATAR + NV_DETP_EL_NAME_DY
           + NV_DETP_EL_ROLE_DY + NV_DETP_EL_LINE * 2.0f;
  }
}

static float heightHeaderComments(void) {
  return 46.0f + 44.0f + (isSeries() ? COM_PILL_H : 0.0f) + 28.0f;
}

// Rotulos do seletor, em escopo de arquivo: a contagem de colunas e a largura
// de item precisam deles fora do desenho.
static const char *COM_ROT[2] = { "Series", "Episode" };

// A fileira de comentarios tem DUAS naturezas em sequencia: as pilulas do
// seletor (so em serie) e, depois delas, os CARTOES.
//
// Os cartoes precisavam virar colunas: eles eram desenhados tres e ponto, sem
// foco, entao os outros cinco que o Trakt manda (EX_COMENT_MAX = 8) eram
// inalcancaveis — foi o "nao tava dando pra navegar nos comentarios".
static int nPillsCom(void) { return isSeries() ? 2 : 0; }
static int nCardsCom(void) {
  int n = (isSeries() && commentEp) ? extras_n_comments_ep()
                                 : extras_n_comments();
  return n > EX_COMMENT_MAX ? EX_COMMENT_MAX : n;
}

static float widthPilulaCom(const char *rot) {
  TxtLine l = txt_line(TXT_PLR_BODY, rot, 255, 255, 255, 255);
  return l.w + COM_PILL_DFLT * 2;
}

// Desenha o cabecalho e devolve o Y onde os CARTOES comecam.
static float headerComments(float x, float y, float a) {
  float yy = y;
  // Wordmark. A marca ja esta em art/marcas/trakt.png, a mesma que a fileira de
  // notas usa — nao ha texto "trakt" desenhado com fonte, porque o logotipo tem
  // desenho proprio e escrever a palavra sairia diferente da referencia.
  //
  // GFX_CARD e nao GFX_MARCA/GFX_TEXTO, pelo mesmo motivo do cartao de nota: os
  // modos de forma pintam com a cor dada e descartam o RGB da textura, e o
  // wordmark viraria uma silhueta. gfx_icone tambem nao serve — ele monta o
  // caminho a partir de art/icones/, e a marca mora em art/marcas/.
  //
  // art/marcas/trakt_wordmark.png (282x106, com alfa) — o wordmark de verdade,
  // fornecido pelo dono. Antes eu desenhava aqui o LOGOMARK circular
  // (trakt.png, 96x96) esticado ate a largura de um wordmark, e saia um selo
  // vermelho deformado que nao era nem uma coisa nem outra.
  //
  // GFX_MARCA, e nao GFX_CARD: o modo de cartao IGNORA O ALFA da textura e
  // pinta o retangulo inteiro, entao saia uma CAIXA atras das letras — com o
  // arquivo antigo (captura de tela, fundo chapado) e com o vetorial tambem,
  // porque ali o fundo e transparente e o RGB por baixo e preto.
  //
  // GFX_MARCA existe exatamente para isto: a forma vem do ALFA e a cor vem de
  // uCor. Serve porque o wordmark e de UMA COR SO. Nao serviria para o selo do
  // IMDb, que e amarelo e preto e precisa do RGB do arquivo — e por isso o
  // cartao de nota continua em GFX_CARD.
  //
  // A altura manda e a largura sai do aspecto REAL do arquivo — cravar a
  // largura deformaria o desenho se a arte for trocada.
  float widthBrand = 0.0f;
  { const char *cam = extras_path_brand_name("trakt_wordmark");
    GLuint t = tex_get_width(cam, 160.0f);
    if (t) {
      float ap = tex_aspect(cam);
      float h = 34.0f;
      if (ap <= 0.0f) ap = 282.0f / 106.0f;
      widthBrand = h * ap;
      { GfxRect m = { x, yy + 6.0f, widthBrand, h };
        gfx_tex_aspect_current = 0.0f;
        gfx_rect(m, t, GFX_BRAND, 0, 0, 0, 0.0f, 1, 1, 1, a); }
      widthBrand += 14.0f;
    } }
  // Sem a palavra "Comentários" ao lado do wordmark: o logo do trakt ja diz de
  // quem sao, e o subtitulo logo abaixo ja diz o que sao. Eram tres rotulos
  // para uma coisa so.
  (void)widthBrand;
  yy += 46.0f;
  { TxtLine ls = txt_line(TXT_DET_META2, "Trakt ratings", 179, 179, 179, 255);
    txt_draw_alpha(ls, x, yy, a * 0.95f); }
  yy += 44.0f;

  // As duas pilulas. Em FILME so existe a da serie — nao ha episodio —, entao a
  // fileira inteira some em vez de mostrar um controle morto.
  if (isSeries()) {
    float px = x;
    int k;
    int row = SEC_COMMENTS;
    for (k = 0; k < 2; k++) {
      float w = widthPilulaCom(COM_ROT[k]);
      GfxRect r = { px, yy, w, COM_PILL_H };
      // MEDIDO na referencia: a pilula ESCOLHIDA e BRANCA com texto escuro, e a
      // outra e #2D2D2D com texto branco. E o oposto das pilulas de temporada,
      // onde a escolhida continua escura e so o texto embranquece — sao dois
      // componentes com regras proprias, e eu tinha aplicado a regra errada
      // aqui.
      //
      // Por isso o FOCO nao pode ser a inversao: a inversao ja e o estado
      // "escolhida". Fica o anel branco, que e a outra linguagem de foco do app
      // e nao colide com nada.
      float f = (level >= 1 && focus.row == row && focus.column == k)
                ? animFocus[row][k] : 0.0f;
      int sel = (commentEp == k);
      // FOCO: anel branco na pilula ESCURA, CRESCIMENTO na pilula branca.
      //
      // Anel branco em volta de preenchimento branco deixa uma folga escura
      // entre os dois, e essa folga e o "halo estranho" — dois brancos
      // separados por uma linha preta, que nao le como foco nem como selecao.
      // Na pilula ja invertida o foco se marca pelo TAMANHO, que e a mesma
      // linguagem medida nos botoes circulares do heroi.
      { float grows = sel ? (1.0f + 0.07f * f) : 1.0f;
        float dw = r.w * (grows - 1.0f), dh = r.h * (grows - 1.0f);
        GfxRect rc = { r.x - dw * 0.5f, r.y - dh * 0.5f, r.w + dw, r.h + dh };
        float luma = sel ? 0.961f : 0.176f;      // #F5F5F5 / #2D2D2D
        gfx_color(rc, NV_RADIUS_PILL, luma, luma, luma, a);
        r = rc; }
      if (f > 0.01f && !sel) {
        GfxRect ring = { r.x - NV_RING_FOCUS, r.y - NV_RING_FOCUS,
                         r.w + NV_RING_FOCUS * 2, r.h + NV_RING_FOCUS * 2 };
        gfx_rect(ring, 0, GFX_RING, 0, NV_RING_FOCUS / ring.h, 0, NV_RADIUS_PILL,
                 1, 1, 1, f * a);
      }
      { int color = sel ? 17 : 255;
        TxtLine l = txt_line(TXT_PLR_BODY, COM_ROT[k], color, color, color, 255);
        txt_weight(l, r.x + (r.w - l.w) * 0.5f, r.y + (r.h - l.h) * 0.5f, a, 0.5f); }
      px += w + COM_PILL_GAP;
    }
    yy += COM_PILL_H;
  }
  return yy + 28.0f;
}

static void drawComments(float x, float y, float a) {
  int ofSeries = !(isSeries() && commentEp);
  int n = ofSeries ? extras_n_comments() : extras_n_comments_ep();
  int i;
  y = headerComments(x, y, a);
  // Carregando e "nao ha" sao a MESMA lista vazia; sem separar os dois o
  // episodio parecia nunca ter comentario nenhum.
  if (n == 0) {
    const char *msg = (!ofSeries && extras_comments_ep_loading())
                    ? "Loading comments…"
                    : "No comments yet.";
    TxtLine l = txt_line(TXT_DET_META2, msg, 150, 154, 163, 255);
    txt_draw_alpha(l, x, y, a * 0.9f);
    return;
  }
  // TODOS os cartoes, nao tres: os outros que o Trakt manda ficavam
  // inalcancaveis. Quem limita o que aparece e o recorte lateral abaixo, e quem
  // traz os de fora da tela e a rolagem horizontal da fileira.
  for (i = 0; i < n; i++) {
    float cx = x + i * (COM_CARD_W + COM_CARD_GAP) - scrollSec[SEC_COMMENTS];
    GfxRect card = { cx, y, COM_CARD_W, COM_CARD_H };
    int foc = (level >= 1 && focus.row == SEC_COMMENTS &&
               focus.column - nPillsCom() == i);
    float px, width;
    // Fora da tela dos dois lados: nem desenha. Sao ate 8 cartoes de 722 px, e
    // pintar os que ninguem ve custa preenchimento num aparelho onde ele e o
    // recurso escasso.
    if (cx > NV_SCREEN_W || cx + COM_CARD_W < 0.0f) continue;
    char footer[64];
    px = cx + COM_DFLT; width = COM_CARD_W - COM_DFLT * 2;
    frame(card, 20.0f, a);
    if (foc) {
      GfxRect ring = { card.x - NV_RING_FOCUS, card.y - NV_RING_FOCUS,
                       card.w + NV_RING_FOCUS * 2, card.h + NV_RING_FOCUS * 2 };
      // Raio EXTERNO = raio do cartao + espessura do anel, senao o canto do
      // anel fica mais quadrado que o do cartao e as duas curvas descasam.
      gfx_rect(ring, 0, GFX_RING, 0, NV_RING_FOCUS / ring.h, 0,
               (20.0f + NV_RING_FOCUS) / ring.h, 1, 1, 1, a);
    }

    { TxtLine lu = txt_line_trim(TXT_ROW_TITLE,
                                    ofSeries ? extras_comment_user(i)
                                            : extras_comment_ep_user(i),
                                    245, 248, 255, 255, width);
      txt_draw_alpha(lu, px, y + COM_DFLT, a); }

    // O texto para ANTES do rodape: sem o teto de linhas ele passava por cima
    // das curtidas. 5 linhas e o que cabe entre o nome e o rodape com o leading
    // de 34.
    txt_block(TXT_DET_META2, ofSeries ? extras_comment_text(i)
                                     : extras_comment_ep_text(i),
              200, 205, 214,
              px, y + COM_DFLT + 46.0f, width, 34.0f, a * 0.95f, 5);

    { int score = ofSeries ? extras_comment_score(i)
                         : extras_comment_ep_score(i);
      int cur  = ofSeries ? extras_comment_likes(i)
                         : extras_comment_ep_likes(i);
      if (score > 0)
        snprintf(footer, sizeof footer, "%d/10   %d likes", score, cur);
      else
        snprintf(footer, sizeof footer, "%d likes", cur);
      { TxtLine lr = txt_line(TXT_CAPTION2, footer, 150, 154, 163, 255);
        txt_draw_alpha(lr, px, y + COM_CARD_H - COM_DFLT - lr.h, a * 0.9f); } }
  }
}

static void drawSection(int r, float a, Uint32 now) {
  int n = sectionN(r);
  // Aba de informacao que nao seja "Criador e elenco": o web TROCA o conteudo
  // da secao (avaliacoes por episodio, fileira de similares, trailer). Nenhum
  // desses dados existe no catalogo nativo, e o web mostra exatamente esta
  // linha quando o dado falta (`.series-insight-empty`).
  //
  // Trocar, e nao sobrepor: na primeira captura do aparelho a mensagem saia POR
  // CIMA dos avatares do elenco, e as duas coisas ficavam ilegiveis.
  { int tab = tabIdOf(tabInfo);
    float yTab = NV_DETP_EL_Y - scrollY + 40.0f;
    if (r == SEC_CAST && tab == TAB_RATINGS) {
      // Serie com notas por episodio mostra o painel do web; o resto (filme, ou
      // serie sem essa fonte) cai nos cartoes de nota.
      if (isSeries() && extras_n_seasons() > 0)
        drawScoresEpisode(NV_DETP_X, yTab, a);
      else
        drawRatings(NV_DETP_X, yTab, a);
      return;
    }
    if (r == SEC_CAST && tab == TAB_RELATED) {
      drawRelated(NV_DETP_X, yTab, a); return;
    }
    if (r == SEC_CAST && tab == TAB_COLLECTION) {
      drawCollection(NV_DETP_X, yTab, a); return;
    }
    if (r == SEC_CAST && tab == TAB_COMMENTS) {
      drawComments(NV_DETP_X, yTab, a); return;
    } }
  if (n <= 0) return;
  // FILME le o layout empilhado; SERIE mantem as coordenadas medidas. Note que
  // na serie o topo do GRUPO e o y de DESENHO sao numeros diferentes (o grupo
  // de temporadas comeca em 1080 e a pilula e desenhada em 1160), por isso as
  // duas constantes coexistem em vez de uma sair da outra.
  float y;
  if (!isSeries()) y = contentSec[r];
  else switch (r) {
    case SEC_SEASONS: y = NV_DETP_TEMP_Y; break;
    case SEC_EPISODES:  y = NV_DETP_EP_Y;   break;
    case SEC_TABS_INFO:  y = NV_DETP_TAB_Y;  break;
    // A SECAO DO TRAKT E EMPILHADA, nao medida: ela vem DEPOIS do elenco e a
    // altura do elenco varia (nome comprido quebra em duas linhas). O `default`
    // abaixo mandava ela para NV_DETP_EL_Y, que e o y do PROPRIO elenco — por
    // isso ela era desenhada por cima dos avatares.
    //
    // Consertar o recalcularLayout nao bastou: aquilo governa foco e rolagem, e
    // este switch e quem escolhe onde DESENHAR. Eram dois numeros para o mesmo
    // lugar, e so um deles tinha sido corrigido.
    case SEC_COMMENTS: y = contentSec[r]; break;
    default:             y = NV_DETP_EL_Y;   break;
  }
  y -= scrollY;

  // TITULO DA SECAO ("Temporadas", "Elenco"), como a referencia. O port nao
  // tinha cabecalho nenhum e as fileiras apareciam soltas, sem dizer o que
  // eram. Fica ACIMA da fileira e some junto com ela na rolagem.
  // So "Temporadas". A fileira de elenco ja e rotulada pela ABA acima dela
  // ("Criador e elenco"), e um cabecalho "Elenco" logo abaixo dela dizia a
  // mesma coisa duas vezes — na primeira tentativa os dois ainda se
  // sobrepunham.
  // Na SERIE so "Temporadas": a fileira de elenco ja e rotulada pela aba
  // "Criador e elenco" logo acima, e um cabecalho "Elenco" abaixo dela dizia a
  // mesma coisa duas vezes. No FILME nao ha abas, entao cada secao carrega o
  // proprio nome — que e o que torna a pagina legivel sem a barra.
  //
  // Na SERIE nao ha cabecalho NENHUM. Havia "Temporadas" acima da fileira de
  // pilulas; a referencia no aparelho nao tem: a pilula "Temporada 1" ja diz o
  // que a fileira e, e o rotulo acima dela repetia a palavra duas vezes em
  // linhas seguidas. No FILME cada secao continua carregando o proprio nome,
  // porque la nao existe a barra de abas para dizer o que e o que.
  { const char *header = isSeries() ? NULL : headerOf(r);
    if (header) {
      TxtLine lc = txt_line(TXT_HEADLINE, header, 245, 248, 255, 255);
      txt_draw_alpha(lc, NV_DETP_X, y - lc.h - NV_DETF_HEADER_GAP, a);
    } }
  { float height = heightSection(r);
    if (y > NV_SCREEN_H || y + height < -40.0f) return; }

  for (int c = 0; c < n && c < N_ITEMS; c++) {
    float f = animFocus[r][c];
    float x = xItem(r, c) - scrollSec[r];
    float w = widthItem(r, c);
    if (x > NV_SCREEN_W || x + w < -w) continue;
    switch (r) {
      case SEC_SEASONS: {
        GfxRect b = { x, y, w, NV_DETP_TEMP_H };
        drawSeason(b, c, f, a); break;
      }
      case SEC_EPISODES: {
        GfxRect b = { x, y, NV_DETP_EP_W, NV_DETP_EP_H };
        drawEpisode(b, c, f, a, now); break;
      }
      case SEC_TABS_INFO: {
        drawTabInfo(x, y, c, f, a);
        if (c + 1 < n) {
          TxtLine d = txt_line(TXT_PLR_BODY, "|", 128, 128, 128, 255);
          txt_weight(d, x + w + NV_DETP_TAB_SEP,
                   y + (NV_DETP_TAB_H - d.h) * 0.5f, a, 1.4f);
        }
        break;
      }
      case SEC_TRAILERS: drawTrailer(x, y, c, a); break;
      // Reaproveitam o desenho que ja servia as ABAS da serie: e o mesmo
      // conteudo, so que agora numa secao propria em vez de atras de uma aba.
      case SEC_RELATED: if (c == 0) drawRelated(NV_DETP_X, y, a); break;
      case SEC_COMMENTS:  drawComments(NV_DETP_X, y, a); break;
      case SEC_DETAILS: drawDetails(x, y, f, a); break;
      default: drawCast(x, y, c, f, a); break;
    }
  }

}

// A estrutura da pagina aparece enquanto o Cinemeta responde. Nao entra em
// secaoN(): esqueleto nao recebe foco nem inventa itens. Ele ocupa exatamente
// as coordenadas finais de temporadas/episodios, de modo que a resposta apenas
// preenche os blocos e nao desloca a pagina sob o controle remoto.
static void drawSkeletonEpisodes(float a) {
  int c;
  float yt, ye;
  if (!isSeries() || cat_n_episodes(idx) > 0 ||
      !disc_episodes_loading(idx)) return;
  yt = NV_DETP_TEMP_Y - scrollY;
  ye = NV_DETP_EP_Y - scrollY;

  if (yt < NV_SCREEN_H && yt + NV_DETP_TEMP_H > 0) {
    for (c = 0; c < 3; c++) {
      float w = c == 0 ? 238.0f : 214.0f;
      GfxRect p = { NV_DETP_X + c * 276.0f, yt, w, NV_DETP_TEMP_H };
      gfx_color(p, 0.5f, 0.17f, 0.18f, 0.20f, a * 0.62f);
    }
  }
  if (ye < NV_SCREEN_H && ye + NV_DETP_EP_H > 0) {
    for (c = 0; c < 3; c++) {
      float x = NV_DETP_X + c * NV_DETP_EP_STEP;
      GfxRect card = { x, ye, NV_DETP_EP_W, NV_DETP_EP_H };
      GfxRect badge = { x + NV_DETP_EP_DFLT, ye + NV_DETP_EP_BADGE_Y,
                       108.0f, NV_DETP_EP_BADGE_H };
      GfxRect title = { x + NV_DETP_EP_DFLT, ye + NV_DETP_EP_TITLE_Y,
                         292.0f, 25.0f };
      GfxRect sin1 = { x + NV_DETP_EP_DFLT, ye + NV_DETP_EP_SIN_Y,
                       NV_DETP_EP_TEXT_W, 18.0f };
      GfxRect sin2 = { sin1.x, sin1.y + NV_DETP_EP_LD_SIN, 420.0f, 18.0f };
      gfx_color(card, NV_DETP_EP_RADIUS / NV_DETP_EP_H,
              0.105f, 0.11f, 0.12f, a * 0.82f);
      gfx_color(badge, 0.48f, 0.19f, 0.20f, 0.22f, a * 0.70f);
      gfx_color(title, 0.5f, 0.25f, 0.26f, 0.28f, a * 0.62f);
      gfx_color(sin1, 0.5f, 0.20f, 0.21f, 0.23f, a * 0.52f);
      gfx_color(sin2, 0.5f, 0.20f, 0.21f, 0.23f, a * 0.52f);
    }
  }
}

// Mesma ideia para o ELENCO do filme: seis avatares e as duas linhas de texto
// nas coordenadas finais, com o cabecalho "Elenco" no lugar dele.
static void drawSkeletonCast(float a) {
  if (!castLoading()) return;
  float y = contentSec[SEC_CAST] - scrollY;
  if (y > NV_SCREEN_H || y + NV_DETF_EL_HEIGHT < -40.0f) return;
  { TxtLine lc = txt_line(TXT_HEADLINE, "Cast", 245, 248, 255, 255);
    txt_draw_alpha(lc, NV_DETP_X, y - lc.h - NV_DETF_HEADER_GAP, a); }
  for (int c = 0; c < 6; c++) {
    float x = NV_DETP_X + c * NV_DETP_EL_STEP;
    GfxRect av = { x, y, NV_DETP_EL_AVATAR, NV_DETP_EL_AVATAR };
    GfxRect name = { x, y + NV_DETP_EL_AVATAR + NV_DETP_EL_NAME_DY + 4.0f,
                     c % 2 ? 150.0f : 184.0f, 20.0f };
    GfxRect role = { x, name.y + NV_DETP_EL_ROLE_DY, 110.0f, 16.0f };
    gfx_color(av, 0.5f, 0.17f, 0.18f, 0.20f, a * 0.62f);
    gfx_color(name, 0.5f, 0.22f, 0.23f, 0.25f, a * 0.55f);
    gfx_color(role, 0.5f, 0.20f, 0.21f, 0.23f, a * 0.45f);
  }
}

// FICHA DA PESSOA — a tela que o web chama de castDetailScreen. Ocupa a tela
// inteira sobre um fundo opaco, com a foto e a bio a esquerda e a filmografia
// em cartoes de poster a direita. Nao ha layout medido do web para copiar aqui
// (a tela do web e uma pagina rolavel de largura fluida), entao as medidas
// seguem as que esta tela ja usa: gutter de 96, poster de 212x318, cartao com
// o mesmo raio dos outros.

static void drawPerson(float a) {
  GfxRect screen = { 0, 0, NV_SCREEN_W, NV_SCREEN_H };
  gfx_color(screen, 0.0f, 0.051f, 0.051f, 0.051f, a);

  { GLuint t = person_photo()[0] ? tex_get(person_photo()) : 0;
    GfxRect r = { NV_DETP_X, 96.0f, PES_PHOTO_W, PES_PHOTO_H };
    if (t) {
      gfx_tex_aspect_current = tex_aspect(person_photo());
      gfx_rect(r, t, GFX_CARD, 0, 0, 0, radiusPoster(PES_PHOTO_W, PES_PHOTO_H), 0, 0, 0, a);
      gfx_tex_aspect_current = 0.0f;
    } else {
      gfx_color(r, radiusPoster(PES_PHOTO_W, PES_PHOTO_H), 0.13f, 0.13f, 0.13f, a);
    } }

  { float y = 96.0f + PES_PHOTO_H + 32.0f;
    TxtLine ln = txt_line_trim(TXT_TITLE3, person_name(), 245, 248, 255, 255,
                                  PES_PHOTO_W);
    txt_draw_alpha(ln, NV_DETP_X, y, a);
    y += ln.h + 10.0f;
    if (person_area()[0]) {
      TxtLine la = txt_line(TXT_DET_META2, person_area(), 150, 154, 163, 255);
      txt_draw_alpha(la, NV_DETP_X, y, a * 0.9f);
      y += la.h + 18.0f;
    }
    if (person_bio()[0])
      txt_block(TXT_DET_META2, person_bio(), 190, 195, 205, NV_DETP_X, y,
                PES_PHOTO_W, 32.0f, a * 0.9f, 6);
  }

  { int n = person_n_credits(), i;
    float x0 = PES_COL_X;
    TxtLine lt = txt_line(TXT_HEADLINE, "Filmography", 245, 248, 255, 255);
    txt_draw_alpha(lt, x0, 96.0f, a);
    for (i = 0; i < n; i++) {
      int col = i % PES_PER_LINE, lin = i / PES_PER_LINE;
      float x = x0 + col * (PES_CARD_W + PES_CARD_GAP);
      float y = 96.0f + lt.h + 28.0f + (lin - personLine) * (PES_CARD_H + 92.0f);
      if (lin < personLine) continue;
      GfxRect r = { x, y, PES_CARD_W, PES_CARD_H };
      const char *po = person_credit_poster(i);
      GLuint t = po[0] ? tex_get_width(po, PES_CARD_W) : 0;
      if (y + PES_CARD_H > NV_SCREEN_H - 24.0f) break;
      if (i == personFocus) {
        GfxRect ring = { r.x - 4, r.y - 4, r.w + 8, r.h + 8 };
        gfx_color(ring, radiusPoster(PES_CARD_W, PES_CARD_H), 1, 1, 1, a);
      }
      if (t) {
        gfx_tex_aspect_current = tex_aspect(po);
        gfx_rect(r, t, GFX_CARD, i == personFocus ? 1.0f : 0.0f, 0, 0,
                 radiusPoster(PES_CARD_W, PES_CARD_H), 0, 0, 0, a);
        gfx_tex_aspect_current = 0.0f;
      } else {
        gfx_color(r, radiusPoster(PES_CARD_W, PES_CARD_H), 0.13f, 0.13f, 0.13f, a);
      }
      { TxtLine lc = txt_line_trim(TXT_DET_META2, person_credit_title(i),
                                      230, 234, 242, 255, PES_CARD_W);
        txt_draw_alpha(lc, x, y + PES_CARD_H + 12.0f, a);
        { const char *year = person_credit_year(i);
          const char *pap = person_credit_role(i);
          char sub[96];
          snprintf(sub, sizeof sub, "%s%s%s", year,
                   (year[0] && pap[0]) ? "  \xc2\xb7  " : "", pap);
          if (sub[0]) {
            TxtLine ls = txt_line_trim(TXT_MINI, sub, 140, 144, 153, 255,
                                          PES_CARD_W);
            txt_draw_alpha(ls, x, y + PES_CARD_H + 12.0f + lc.h + 6.0f,
                               a * 0.9f);
          } } }
    } }
}

// O retrato do diretor pertence ao detalhe, não ao hero da home. A foto do
// TMDB entra sobre o backdrop com o mesmo tratamento editorial do renderer e
// recua quando o documento rola; a arte horizontal continua sendo a base.
static void drawDirectorDetail(const CatItem *ci, float a, float pg) {
  const char *photo;
  GLuint tex;
  GfxRect r;
  if (!ci || isSeries() || !ci->directing[0] || a <= 0.005f) return;
  director_request(ci->directing);
  photo = director_photo(ci->directing);
  if (!photo[0]) return;
  // O retrato ocupa menos que um hero full-bleed, mas e exibido grande na TV.
  // Pedir pelo tamanho da coluna evita ampliar um w500 borrado sem reservar
  // os ~2K de uma capa horizontal.
  tex = tex_get_width(photo, 960.0f);
  if (!tex) return;
  // Caixa vertical real: a proporcao da foto fica sob responsabilidade do
  // shader GFX_RETRATO, que ancora a imagem na direita e dissolve as bordas.
  // A caixa mais alta deixa o rosto respirar e evita a aparencia de retrato
  // comprimido dentro de um banner largo.
  r = (GfxRect){ 1080.0f, 0.0f, 840.0f, 930.0f };
  gfx_tex_aspect_current = tex_aspect(photo);
  gfx_rect(r, tex, GFX_PORTRAIT, 0, 0, 0, 0, 0, 0, 0,
           a * (1.0f - 0.82f * pg));
  gfx_tex_aspect_current = 0.0f;
}

void detail_draw(Uint32 now) {
  if (!is_open) return;
  float s = smooth(t), a2 = phase2();

  if (!detail_covers_screen()) {
    GfxRect screen = { 0, 0, NV_SCREEN_W, NV_SCREEN_H };
    gfx_color(screen, 0.0f, 0.051f, 0.051f, 0.051f, s);   // #0d0d0d, o fundo do web
  }
  gfx_no_crop();

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
  GfxRect target; float aEntry;
  backdropRect(&target, &aEntry);
  const char *art = artOf(idx);
  int artPoster = artDetailIsPoster(idx);
  // Backdrop em tela cheia: pede o teto de 1920. Com o teto comum de 960 a arte
  // era decodificada com metade da resolucao e ampliada ao dobro na tela.
  GLuint tex = art ? tex_get_hero(art) : 0;
  // Ao rolar, o web NAO desfoca a arte: ele a APAGA. Medido em
  // `.series-detail-shell.detail-scrolled` — o backdrop vai a `opacity: 0.15` e
  // a vinheta a 0, ambos em 0.8s cubic-bezier(.4,0,.2,1).
  //
  // O VEU E DE TELA CHEIA e custava caro numa GPU que ja estava afogada em
  // preenchimento (medido: clr=38,3ms com a CPU ociosa). Mas ele pinta
  // #0d0d0d — que e EXATAMENTE a cor com que main.c limpa o quadro
  // (NV_COR_FUNDO_*). Com a tela ja coberta pelo detalhe, embaixo dele nao ha
  // home nem outra tela: ha o glClear. Pintar #0d0d0d sobre #0d0d0d nao muda
  // um pixel, e a camada inteira sai.
  //
  // Fica quando a tela NAO esta coberta: ai embaixo ha a home, e o veu e o que
  // a apaga.
  if (pg > 0.01f && !detail_covers_screen()) {
    GfxRect screen = { 0, 0, NV_SCREEN_W, NV_SCREEN_H };
    gfx_color(screen, 0.0f, 0.051f, 0.051f, 0.051f, pg);
  }
  // 4o parametro = forca da VINHETA, nao "foco". Vai a 0 junto com a rolagem,
  // que e o par que faltava: o web apaga a arte para 15% E some com a vinheta
  // ao mesmo tempo. Poster reserva usa composição contida, sem crop de capa.
  drawArtDetail(target, tex, art, artPoster,
                     tex ? aEntry * (1.0f - 0.85f * pg) : 1.0f, pg);

  drawDirectorDetail(cat_item(idx), aEntry, pg);

  // O hero ROLA com o documento: ele nao some nem e substituido por um
  // cabecalho fixo. Era isso que fazia a pagina do port parecer outra tela em
  // vez da mesma tela rolada.
  // O conteudo SOBE para o lugar enquanto aparece, no lugar de so surgir: e a
  // contraparte do texto da home, que desce e apaga. Junto, le como um bloco
  // trocando de arranjo, que e o que o dono pediu.
  heroWeb(a2, -scrollY + (1.0f - a2) * NV_SCREEN_H * 0.05f);


  if (pg <= 0.01f && scrollY < 1.0f) {
    if (personIs_open) drawPerson(s);
    return;
  }
  drawSkeletonEpisodes(pg);
  drawSkeletonCast(pg);
  for (int r = 0; r < N_SECTIONS; r++) drawSection(r, pg, now);
  // POR CIMA de tudo: a ficha e outra tela, nao uma secao desta.
  if (personIs_open) drawPerson(s);
}

int detail_index(void) { return idx; }
int detail_requested_play(void) { int v = reqPlay; reqPlay = 0; return v; }
int detail_requested_open(void) { int v = reqOpen; reqOpen = -1; return v; }
int detail_requested_watched(void) { int v = reqWatched; reqWatched = 0; return v; }
int detail_requested_mark(void)     { int v = reqMark;     reqMark = 0;     return v; }
int detail_requested_sources(void)     { int v = reqSources;     reqSources = 0;     return v; }
// "Reproduzir desde o inicio" ainda cai no mesmo caminho do primario: o
// roteador so sabe abrir o player no ponto salvo. Consumir o pedido aqui evita
// que ele fique pendurado.
int detail_requested_do_start(void)  { int v = reqOfStart;   reqOfStart = 0;   return v; }
