// Roteador de telas.
//
// Antes disto o main.c decidia entre home e detalhe com um if. Com menu, busca,
// biblioteca, ajustes e player, esse if viraria um emaranhado onde cada tela
// precisa saber das outras — e a regra de "quem come a tecla" ficaria espalhada
// por seis arquivos. Aqui existe uma tela CORRENTE e uma unica ordem de
// prioridade, escrita num lugar so.
//
// Ordem de quem recebe o D-pad, de cima para baixo:
//   1. player  — cobre a tela inteira
//   2. detalhe — camada sobre a tela corrente
//   3. menu    — camada sobre a tela corrente
//   4. a tela corrente (home, busca, biblioteca ou ajustes)
#include "app.h"
#include <string.h>
#include "home.h"
#include "detail.h"
#include "menu.h"
#include "busca.h"
#include "biblioteca.h"
#include "ajustes.h"
#include "player.h"
#include "streams.h"
#include "video.h"
#include "addons.h"
#include "descoberta.h"
#include "faixas.h"
#include <pthread.h>

// Link de debrid expira em minutos; um minuto e folga suficiente para o usuario
// apertar Reproduzir logo depois de abrir o titulo sem pagar uma busca a mais.
#define NV_LINK_VALIDO_MS 60000

static int aguardandoFonte;
static pthread_t fioFonte;
static int fonteEscolhida = -2;   // -2 = trabalhando, -1 = nenhuma serve

// A verificacao faz uma requisicao por fonte candidata e bloqueia; num fio
// proprio a tela segue em 60fps mostrando "Abrindo fonte".
static void *escolherFonte(void *u) {
  (void)u;
  // Ate 8: numa lista tipica de 12, as primeiras costumam ser do mesmo
  // provedor e falham juntas quando o arquivo nao esta em cache. Testar poucas
  // devolvia "nenhuma fonte serve" com fontes boas logo adiante.
  fonteEscolhida = stream_primeira_boa(8);
  return NULL;
}
#include "catalogo.h"
#include "gfx.h"
#include "catalogo.h"
#include "layout.h"
#include <stdio.h>

static Tela tela = TELA_HOME;
static int sair = 0;

// O detalhe precisa do retangulo REAL de onde o card saiu para o voo comecar
// dali. Cada tela que abre um titulo entrega o seu; quando nenhuma entrega
// (caso do menu ou de um indice vindo de fora), cai para a tela inteira.
static void abrirTitulo(const HomeItem *it) {
  if (it && it->arte) detail_abrir(it);
}

static void abrirPorIndice(int i) {
  const CatItem *c = cat_item(i);
  if (!c || !c->backdrop[0]) return;
  HomeItem it;
  GfxRect tudo = { 0, 0, NV_TELA_W, NV_TELA_H };
  it.rect = tudo;
  it.arte = c->backdrop;
  it.titulo = c->titulo;
  it.genero = c->genero;
  it.meta = c->meta;
  abrirTitulo(&it);
}

static void trocarTela(Tela nova) {
  if (nova == tela) return;
  tela = nova;
  // Cada tela zera o proprio estado ao ser aberta: voltar para a busca com o
  // texto de duas navegacoes atras seria lixo, nao memoria util.
  switch (tela) {
    case TELA_BUSCA:      busca_iniciar();      break;
    case TELA_BIBLIOTECA: biblioteca_iniciar(); break;
    case TELA_AJUSTES:    ajustes_iniciar();    break;
    default: break;
  }
}

int app_iniciar(const char *dirArte) {
  if (!home_iniciar(dirArte)) return 0;
  menu_iniciar();
  tela = TELA_HOME;
  return 1;
}

void app_evento(const SDL_Event *e) {
  if (e->type == SDL_QUIT) { sair = 1; return; }

  // A folha de fontes fica acima de tudo: ela e uma pergunta, e enquanto ela
  // esta em pe nada mais deve responder ao D-pad.
  if (faixas_aberta()) { faixas_evento(e); return; }
  if (stream_folha_aberta()) { stream_folha_evento(e); return; }
  if (player_aberto()) { player_evento(e); return; }
  if (detail_aberto()) { detail_evento(e); return; }
  if (menu_aberto())   { menu_evento(e);   return; }

  switch (tela) {
    case TELA_BUSCA:      busca_evento(e);      break;
    case TELA_BIBLIOTECA: biblioteca_evento(e); break;
    case TELA_AJUSTES:    ajustes_evento(e);    break;
    default:              home_evento(e);       break;
  }

  // O menu abre AQUI, no mesmo evento que o pediu, e nao no proximo
  // app_atualizar. Diferido por um quadro, as teclas que vierem logo depois do
  // ESQUERDA — e num controle elas vem — sao entregues a tela de tras, que
  // ainda acha que e a dona do foco.
  if (tela == TELA_HOME && home_pediu_menu()) menu_abrir();
}

void app_atualizar(float dt, Uint32 agora) {
  // Fora da home, o Back tem para onde voltar: a home. So nela ele fecha o app.
  if (tela != TELA_HOME) {
    int fechar = (tela == TELA_BUSCA      && busca_quer_sair())
              || (tela == TELA_BIBLIOTECA && biblioteca_quer_sair())
              || (tela == TELA_AJUSTES    && ajustes_quer_sair());
    if (fechar) { trocarTela(TELA_HOME); menu_definir_destino(MENU_INICIO); }
  } else if (home_quer_sair()) {
    sair = 1;
  }

  if (menu_mudou_destino()) {
    switch (menu_destino()) {
      case MENU_BUSCAR:     trocarTela(TELA_BUSCA);      break;
      case MENU_BIBLIOTECA: trocarTela(TELA_BIBLIOTECA); break;
      case MENU_AJUSTES:    trocarTela(TELA_AJUSTES);    break;
      default:              trocarTela(TELA_HOME);       break;
    }
  }
  // Pedidos de abrir um titulo, vindos de qualquer tela.
  if (!detail_aberto() && !player_aberto()) {
    int idx = -1;
    HomeItem it;
    if (tela == TELA_HOME && home_pediu_abrir()) {
      if (home_item_focado(&it)) abrirTitulo(&it);
    } else if (tela == TELA_BUSCA && busca_pediu_abrir(&idx)) {
      if (busca_item_focado(&it)) abrirTitulo(&it); else abrirPorIndice(idx);
    } else if (tela == TELA_BIBLIOTECA && biblioteca_pediu_abrir(&idx)) {
      abrirPorIndice(idx);
    }
  }

  // Botoes do detalhe: quem sabe que existe player e biblioteca e o roteador,
  // nao a tela de detalhe.
  if (detail_aberto()) {
    // Reproduzir sem escolher = modo automatico: a regra do stream_automatico
    // (MP4 4K Dolby Vision primeiro, senao o primeiro da lista) decide sozinha.
    // Sem lista, o player abre sem video em vez de nao abrir — a tela dizendo
    // que nao ha fonte e melhor que um botao que parece nao responder.
    // Ao abrir um titulo, perguntar as fontes JA — a busca leva segundos e
    // esperar o usuario apertar Reproduzir para so entao comecar faria a
    // primeira reproducao parecer travada.
    { static int ultimoConsultado = -1;
      int i = detail_indice();
      const CatItem *ci = cat_item(i);
      if (i != ultimoConsultado && ci && ci->imdb[0]) {
        ultimoConsultado = i;
        addons_buscar(ci->imdb, ci->tipo);
        // Episodios do titulo aberto, na temporada onde o dono parou. Sai da
        // rede na hora: guardar a lista de episodios de 40 titulos no pacote
        // envelhecia a cada temporada nova.
        if (!strcmp(ci->tipo, "series")) desc_episodios(i, 0);
        // Legendas do OpenSubtitles junto: sao dezenas por titulo e a busca
        // leva segundos. Pedir so quando o dono abre a folha de faixas faria
        // ele esperar de olho numa lista vazia.
        addons_buscar_legendas(ci->imdb, ci->tipo);
      } }
    if (detail_pediu_reproduzir()) {
      // A tela abre JA, no estado "abrindo fonte", e a escolha acontece depois.
      // Escolher antes deixaria o botao sem resposta por segundos, e escolher
      // sem verificar entregava o video de aviso do debrid — que toca normal e
      // por isso passa por sucesso.
      const CatItem *ci = cat_item(detail_indice());
      player_abrir(detail_indice(), NULL);
      if (stream_idade_ms() > NV_LINK_VALIDO_MS && ci && ci->imdb[0]) {
        printf("fonte: lista com %ums, renovando\n", (unsigned)stream_idade_ms());
        addons_buscar(ci->imdb, ci->tipo);
      }
      aguardandoFonte = 1;
    }
    if (detail_pediu_marcar())     biblioteca_alternar_lista(detail_indice());
    if (detail_pediu_fontes())     stream_folha_abrir();
  }
  // Escolher uma fonte na folha inicia a reproducao DELA. Trocar de fonte com o
  // player ja aberto tambem vale: fecha a sessao atual e abre na nova, senao
  // duas ficariam presas no mesmo pipeline.
  // A busca disparada por Reproduzir terminou: agora VERIFICA as fontes, em
  // ordem, ate achar uma que leve ao arquivo — e so entao liga o video.
  if (aguardandoFonte == 1 && addons_estado() != ADD_BUSCANDO) {
    aguardandoFonte = 2;
    fonteEscolhida = -2;
    if (pthread_create(&fioFonte, NULL, escolherFonte, NULL) == 0) pthread_detach(fioFonte);
    else aguardandoFonte = 0;
  }
  if (aguardandoFonte == 2 && fonteEscolhida != -2) {
    const Stream *s = fonteEscolhida >= 0 ? stream_item(fonteEscolhida) : NULL;
    aguardandoFonte = 0;
    printf("automatico (verificado): %s\n", s ? s->rotulo : "(nenhuma fonte serve)");
    // A afirmacao de HDR/DV vai ANTES do tocar: e ela que o bind do ACB
    // descreve ao tv.display. Sem isto o C9 exibe tudo mapeado em SDR.
    if (s) video_definir_dv(s->dolbyVision);
    if (s) player_definir_fonte(s->url);
  }

  int fonte;
  if (stream_folha_escolheu(&fonte)) {
    const Stream *s = stream_item(fonte);
    printf("fonte escolhida: %s\n", s ? s->rotulo : "?");
    if (s) video_definir_dv(s->dolbyVision);
    if (player_aberto()) player_encerrar();
    player_abrir(detail_indice(), s ? s->url : NULL);
  }
  if (player_pediu_faixas()) faixas_abrir();
  faixas_atualizar(dt, agora);
  stream_folha_atualizar(dt, agora);

  if (player_quer_sair() && !player_aberto()) player_encerrar();

  player_atualizar(dt, agora);
  detail_atualizar(dt, agora);
  menu_atualizar(dt, agora);
  switch (tela) {
    case TELA_BUSCA:      busca_atualizar(dt, agora);      break;
    case TELA_BIBLIOTECA: biblioteca_atualizar(dt, agora); break;
    case TELA_AJUSTES:    ajustes_atualizar(dt, agora);    break;
    default:              home_atualizar(dt, agora);       break;
  }
}

void app_desenhar(Uint32 agora) {
  // O player cobre tudo; desenhar o que esta atras dele e trabalho jogado fora
  // — a mesma conta que ja valia para o cartao de detalhe esticado.
  if (!player_aberto()) {
    if (!detail_cobre_tela()) {
      switch (tela) {
        case TELA_BUSCA:      busca_desenhar(agora);      break;
        case TELA_BIBLIOTECA: biblioteca_desenhar(agora); break;
        case TELA_AJUSTES:    ajustes_desenhar(agora);    break;
        default:              home_desenhar(agora);       break;
      }
    }
    detail_desenhar(agora);
    if (menu_visivel()) menu_desenhar(agora);
  }
  player_desenhar(agora);
  stream_folha_desenhar(agora);
  faixas_desenhar(agora);
}

int app_quer_sair(void) { return sair; }

void app_encerrar(void) {
  player_encerrar();
  ajustes_encerrar();
  biblioteca_encerrar();
  busca_encerrar();
  home_encerrar();
}
