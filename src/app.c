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
#include "login.h"
#include "session.h"
#include "profiles.h"
#include "profile_select.h"
#include "sync.h"
#include "traktauth.h"
#include "simklauth.h"
#include "text.h"
#include "seeall.h"
#include "ctxmenu.h"
#include "mark.h"
#include <string.h>
#include "home.h"
#include "detail.h"
#include "menu.h"
#include "search.h"
#include "library.h"
#include "profile.h"
#include "social.h"
#include "settings.h"
#include "player.h"
#include "streams.h"
#include "video.h"
#include "addons.h"
#include "discover.h"
#include "trakt.h"
#include "tracks.h"
#include "episodes.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

// Link de debrid expira em minutos; um minuto e folga suficiente para o usuario
// apertar Reproduzir logo depois de abrir o titulo sem pagar uma busca a mais.
#define NV_LINK_VALID_MS 60000

static int waitingSource;
static pthread_t threadSource;
static _Atomic int sourceChosen = -2;   // release/acquire entre verificacao e UI

static ProfileData profilePending;
static int profileSuccess;
static _Atomic int profileLoad; // 0=ocioso, 1=rede, 2=snapshot pronto
static _Atomic unsigned profileGeneration = 1;
static pthread_mutex_t profileLock = PTHREAD_MUTEX_INITIALIZER;
typedef struct { unsigned generation; int profile; char account[96]; } ProfileRequest;
static void *loadProfile(void *u) {
  ProfileRequest *request=u;
  ProfileData new={0};
  int success=trakt_profile(&new);
  pthread_mutex_lock(&profileLock);
  // A troca de conta/perfil invalida a resposta. O worker termina, mas nunca
  // publica uma identidade antiga nem deixa um snapshot obsoleto na fila.
  if(request->generation==atomic_load_explicit(&profileGeneration,memory_order_acquire) &&
     atomic_load_explicit(&profileLoad,memory_order_relaxed)==1 && session_loggedin() &&
     profiles_active()==request->profile && !strcmp(session_user(),request->account)){
    profileSuccess=success;
    profilePending=new;
    atomic_store_explicit(&profileLoad,2,memory_order_release);
  }
  pthread_mutex_unlock(&profileLock);
  free(request);
  return NULL;
}
static void invalidateProfile(void) {
  atomic_fetch_add_explicit(&profileGeneration,1,memory_order_acq_rel);
  atomic_store_explicit(&profileLoad,0,memory_order_release);
  pthread_mutex_lock(&profileLock);memset(&profilePending,0,sizeof profilePending);profileSuccess=0;pthread_mutex_unlock(&profileLock);
  profile_set_data(NULL);
}
static void requestProfile(void) {
  pthread_t t;
  int expected = 0;
  ProfileRequest *request;
  profile_set_loading(1);
  if (!atomic_compare_exchange_strong(&profileLoad, &expected, 1)) return;
  request=calloc(1,sizeof *request);
  if(!request){atomic_store(&profileLoad,0);profile_set_error("Could not start the query. Try again.");return;}
  request->generation=atomic_load_explicit(&profileGeneration,memory_order_acquire);
  request->profile=profiles_active();
  snprintf(request->account,sizeof request->account,"%s",session_user());
  if (pthread_create(&t, NULL, loadProfile, request) == 0) pthread_detach(t);
  else { free(request); atomic_store(&profileLoad,0); profile_set_error("Could not start the query. Try again."); }
}

// A verificacao faz uma requisicao por fonte candidata e bloqueia; num fio
// proprio a tela segue em 60fps mostrando "Abrindo fonte".
static void *chooseSource(void *u) {
  (void)u;
  // Ate 8: numa lista tipica de 12, as primeiras costumam ser do mesmo
  // provedor e falham juntas quando o arquivo nao esta em cache. Testar poucas
  // devolvia "nenhuma fonte serve" com fontes boas logo adiante.
  sourceChosen = stream_first_good(8);
  return NULL;
}
#include "catalog.h"
#include "gfx.h"
#include "catalog.h"
#include "layout.h"
#include <stdio.h>

static Screen screen = SCREEN_HOME;
static int sair = 0;

// O detalhe precisa do retangulo REAL de onde o card saiu para o voo comecar
// dali. Cada tela que abre um titulo entrega o seu; quando nenhuma entrega
// (caso do menu ou de um indice vindo de fora), cai para a tela inteira.
static void openTitle(const HomeItem *it) {
  if (it && it->art) detail_open(it);
}

static void openByIndex(int i) {
  const CatItem *c = cat_item(i);
  if (!c || (!c->backdrop[0] && !c->poster[0])) return;
  HomeItem it;
  GfxRect all = { 0, 0, NV_SCREEN_W, NV_SCREEN_H };
  // O `it` e da PILHA e esta funcao preenchia todos os campos MENOS o indice —
  // que ia como lixo. Como a biblioteca e a busca abrem por aqui, qualquer
  // titulo escolhido nelas levava ao mesmo filme. A home nao sofria porque ela
  // entrega o HomeItem inteiro, ja com o indice.
  //
  // O campo existe exatamente por causa deste defeito, e o comentario dele em
  // home.h ja avisava: "faltava, e por isso o detalhe abria sempre o item 0".
  // Zerar a struct antes garante que o proximo campo novo nasca definido em vez
  // de repetir a historia.
  memset(&it, 0, sizeof it);
  it.index_ = i;
  it.rect = all;
  it.art = c->backdrop[0] ? c->backdrop : c->poster;
  it.title = c->title;
  it.genre = c->genre;
  it.meta = c->meta;
  openTitle(&it);
}

// Monta o id que os addons esperam. Para serie e "tt1234567:temporada:episodio";
// sem os dois numeros a resposta volta VAZIA com HTTP 200, e era por isso que o
// addons_buscar cravava ":1:1" — o que fazia toda a serie mostrar as fontes do
// episodio 1, qualquer que fosse o escolhido.
static void idOfTarget(const CatItem *ci, char *dst, size_t n) {
  int t = 0, e = 0;
  if (!ci) { if (n) dst[0] = 0; return; }
  if (!strcmp(ci->kind, "series") && detail_ep_focus(&t, &e) && t > 0 && e > 0)
    snprintf(dst, n, "%.*s:%d:%d", (int)strcspn(ci->imdb,":"),ci->imdb, t, e);
  else
    snprintf(dst, n, "%s", ci->imdb);
}

static void swapScreen(Screen new) {
  if (new == screen) return;
  screen = new;
  // Cada tela zera o proprio estado ao ser aberta: voltar para a busca com o
  // texto de duas navegacoes atras seria lixo, nao memoria util.
  switch (screen) {
    case SCREEN_SEARCH:      search_start();      break;
    case SCREEN_LIBRARY: library_start(); break;
    case SCREEN_PROFILE:     profile_open(); requestProfile(); break;
    case SCREEN_SETTINGS:    settings_start();    break;
    default: break;
  }
}

static void targetPlayer(char *target, size_t size) {
  const CatItem *c = cat_item(player_index());
  int t, e;
  player_episode_current(&t, &e);
  if (!c) { target[0] = 0; return; }
  if (t > 0 && e > 0) snprintf(target,size,"%.*s:%d:%d",(int)strcspn(c->imdb,":"),c->imdb,t,e);
  else snprintf(target,size,"%s",c->imdb);
}
static void fetchForPlayer(void) {
  const CatItem *c = cat_item(player_index());
  char target[64]; targetPlayer(target,sizeof target);
  if (c && target[0]) {
    addons_fetch(target,c->kind);
    addons_fetch_subtitles(target,c->kind);
  }
}
static void episodeOfDetail(void) {
  int t=0,e=0;
  detail_ep_focus(&t,&e);
  player_set_episode(t,e);
}

// A home carregou? Sem arte no pacote ela nao carrega, e ate agora isso
// DERRUBAVA o app: app_iniciar devolvia 0 e o main saia com codigo 1. Num
// pacote de dono isso nunca acontecia porque a arte ia junto; num pacote
// distribuivel, que nao pode levar arte nem credencial de ninguem, esse era o
// comportamento da PRIMEIRA execucao de todo mundo — o app abria e fechava,
// antes mesmo da tela de login.
static int homeReady;

int app_start(const char *dirArt) {
  homeReady = home_start(dirArt);
  if (!homeReady)
    printf("[app] no art in the package: home only appears after the first sync\n");
  menu_start();
  profile_start();
  // Sem conta, o app abre no login. Com sessao gravada ele nem passa por ela —
  // pedir o codigo de novo a cada arranque seria o mesmo que nao ter gravado.
  if (session_loggedin()) {
    screen = SCREEN_HOME;
    // Com sessao gravada o ciclo comeca no arranque: e ele que traz os addons
    // e o Trakt da pessoa, sem os quais a home mostra so o que veio no pacote.
    sync_start();
  } else {
    screen = SCREEN_LOGIN;
    login_start();
  }
  return 1;
}

void app_event(const SDL_Event *e) {
  if (e->type == SDL_QUIT) { sair = 1; return; }

  // O login vem antes de tudo, inclusive do player: enquanto nao ha conta o
  // resto do app nao tem dado nenhum para operar. A escolha de perfil vem logo
  // depois, porque e ela que define para QUEM o resto do app vai sincronizar.
  if (screen == SCREEN_LOGIN)          { login_event(e);     return; }
  if (screen == SCREEN_CHOICE_PROFILE) { profilesel_event(e); return; }

  if(e->type==SDL_KEYDOWN && !e->key.repeat &&
     (e->key.keysym.sym==SDLK_s || e->key.keysym.scancode==NV_SCANCODE_BLUE) &&
     screen==SCREEN_HOME && !player_is_open() && !detail_is_open() && !seeall_is_open() && !menu_is_open()) {
    if(profile_is_open() && profile_side())profile_close();
    else {profile_open_side();requestProfile();}
    return;
  }

  // A folha de fontes fica acima de tudo: ela e uma pergunta, e enquanto ela
  // esta em pe nada mais deve responder ao D-pad.
  if (tracks_is_open()) { tracks_event(e); return; }
  if (episodes_is_open()) { episodes_event(e); return; }
  if (stream_sheet_is_open()) { stream_sheet_event(e); return; }
  if (player_is_open()) { player_event(e); return; }
  if (detail_is_open()) { detail_event(e); return; }
  if (profile_is_open() && profile_side()) { profile_event(e); return; }
  if (menu_is_open())   { menu_event(e);   return; }
  // "Ver tudo" fica ENTRE a home e o detalhe: ela cobre a home e o detalhe
  // cobre ela. Por isso vem depois do detalhe e antes do roteamento por tela.
  // O menu do cartaz fica ACIMA de tudo que a home mostra: ele e modal.
  if (ctx_is_open())     { ctx_event(e);     return; }
  if (seeall_is_open()) { seeall_event(e); return; }

  switch (screen) {
    case SCREEN_SEARCH:      search_event(e);      break;
    case SCREEN_LIBRARY: library_event(e); break;
    case SCREEN_PROFILE:     profile_event(e);     break;
    case SCREEN_SOCIAL:     social_event(e);     break;
    case SCREEN_SETTINGS:    settings_event(e);    break;
    default:              home_event(e);       break;
  }

  // O menu abre AQUI, no mesmo evento que o pediu, e nao no proximo
  // app_atualizar. Diferido por um quadro, as teclas que vierem logo depois do
  // ESQUERDA — e num controle elas vem — sao entregues a tela de tras, que
  // ainda acha que e a dona do foco.
  if (screen == SCREEN_HOME && home_requested_menu()) menu_open();
}

// A tela de detalhe pode pedir para abrir OUTRO titulo (um credito da
// filmografia de um ator, um item de "Mais como este"). Quem troca e aqui, e
// nao ela: reabrir a si mesma no meio do proprio desenho e o tipo de coisa que
// quebra em silencio, e o roteador ja e o unico lugar que sabe abrir titulo.
// O botao do olho: marcar como ASSISTIDO. Grava progresso cheio no arquivo do
// app e avisa o Trakt, que e a fonte que o dono usa nos outros aparelhos. Fica
// no roteador pelo mesmo motivo de tudo mais: e ele que conhece catalogo e
// Trakt, e a tela de detalhe nao precisa conhecer nenhum dos dois.
static void markWatchedIfRequested(void) {
  const CatItem *c;
  int i;
  if (!detail_requested_watched()) return;
  i = detail_index();
  c = cat_item(i);
  if (!c) return;
  // ALTERNA, e manda para o HISTORICO do Trakt.
  //
  // Estava chamando trakt_marcar (que e /scrobble/pause) com duracao 1.0 — e
  // aquela funcao comeca com `durSeg <= 1.0 -> return`. O botao mudava so o
  // espelho local e o Trakt NUNCA era informado: parecia funcionar e nao
  // funcionava. Agora vai por /sync/history, que e o endpoint de "assisti".
  //
  // E alterna em vez de so marcar: o icone ja mostra os dois estados, entao um
  // botao que so soma nao teria como desfazer um toque errado.
  { int watched = (c->progress >= 90);
    cat_save_progress(i, watched ? 0.0 : 1.0, 1.0);
    if (c->imdb[0]) trakt_watched(c->imdb, !watched);
    printf("[app] watched %s: %s\n", watched ? "unmarked" : "marked",
           c->title); fflush(stdout); }
}

static void swapOfTitleIfRequested(void) {
  int target = detail_requested_open();
  if (target >= 0) { openByIndex(target); return; }
  // Titulo que veio de FORA do catalogo: a descoberta buscou o meta num fio e
  // avisa aqui quando ele entrou. Abrir no fio da rede seria mexer na tela de
  // outro fio; este e o unico lugar que abre titulo.
  { int new = disc_title_ready();
    if (new >= 0) openByIndex(new); }
}

void app_update(float dt, Uint32 now) {
  if (screen == SCREEN_LOGIN) {
    login_update(dt, now);
    // A troca so acontece AQUI, quando a sessao existe de verdade — nao no
    // instante em que o servidor respondeu. Assim a home nunca abre com uma
    // sessao pela metade.
    if (login_done()) {
      // Logo apos entrar, o primeiro ciclo de sync: e ele que descobre quantos
      // perfis a conta tem, e sem isso a tela de escolha nao teria o que
      // mostrar.
      sync_reapply_settings();
      sync_start();
      screen = SCREEN_CHOICE_PROFILE;
      profilesel_start();
      menu_set_destination(MENU_START);
    }
    return;
  }

  // Sessao perdida no meio do uso (renovacao recusada): voltar ao login e a
  // unica saida honesta. Continuar na home mostraria o catalogo de exemplo do
  // pacote como se fosse o da pessoa.
  if (!session_loggedin()) {
    invalidateProfile();
    screen = SCREEN_LOGIN;
    login_start();
    return;
  }

  if (screen == SCREEN_CHOICE_PROFILE) {
    sync_step((unsigned)now);
  // Os vinculos de Trakt e Simkl tambem avancam aqui: os dois fazem poll e
  // precisam de um passo por quadro, como o login da conta.
  traktauth_step((unsigned)now);
  simklauth_step((unsigned)now);
    profilesel_update(dt, now);
    if (profilesel_requested_retry()) { sync_start(); return; }
    if (profilesel_wants_exit()) {
      // Sem uma escolha confirmada, voltar nao pode escolher o perfil 1 por
      // acidente. A tela continua visivel e aguarda uma escolha explicita.
      if (!profiles_needs_choose()) { screen = SCREEN_HOME; menu_set_destination(MENU_START); }
      return;
    }
    if (profilesel_done()) {
      // O perfil mudou o destino do sync: rodar de novo traz os addons e o
      // progresso DESTE perfil, e nao os do perfil 1 que o primeiro ciclo
      // pegou por falta de escolha.
      invalidateProfile();
      sync_reapply_settings();
      sync_start();
      screen = SCREEN_HOME;
    }
    return;
  }

  // Um ciclo por vez, e so quando a conta existe. O passo e barato: sem fio
  // terminado ele nao faz nada.
  sync_step((unsigned)now);

  // Conta com mais de um perfil e nenhum escolhido NESTA instalacao: perguntar.
  // Isto vale tambem para quem abriu o app com sessao ja gravada — o caminho
  // comum depois do primeiro dia. Sem isto o app assumia o perfil 1 para
  // sempre, e `perfis_precisa_escolher()` era codigo morto.
  if (screen == SCREEN_HOME && !player_is_open() && !detail_is_open() &&
      profiles_needs_choose()) {
    screen = SCREEN_CHOICE_PROFILE;
    profilesel_start();
    return;
  }
  // E o ciclo automatico — nunca com o player aberto: rajada de HTTP no meio
  // do video disputa CPU e rede com o decodificador.
  if (!player_is_open()) sync_periodic((unsigned)now);

  // Durante a verificacao nao substituir a lista que os workers consultam.
  if (waitingSource != 2) addons_state();
  swapOfTitleIfRequested();
  markWatchedIfRequested();
  if (atomic_load_explicit(&profileLoad, memory_order_acquire) == 2) {
    ProfileData snapshot; int success;
    pthread_mutex_lock(&profileLock); snapshot=profilePending; success=profileSuccess; pthread_mutex_unlock(&profileLock);
    if (success) profile_set_data(&snapshot);
    else if (!trakt_active()) profile_set_state(PROFILE_STATE_DISCONNECTED,
                                                    "Trakt disconnected. Link the account to see your profile.");
    else profile_set_state(PROFILE_STATE_UNAVAILABLE,
                               "Profile unavailable. Your last summary is still safe, if there is one.");
    atomic_store_explicit(&profileLoad, 0, memory_order_release);
  }
  if ((screen == SCREEN_PROFILE || profile_side()) && profile_requested_update()) requestProfile();
  if (profile_requested_complete()) swapScreen(SCREEN_PROFILE);
  if (screen==SCREEN_HOME) {
    CatItem person;
    if(home_requested_person_social(&person)) {
      social_open(&person);swapScreen(SCREEN_SOCIAL);
    }
  }
  if (screen==SCREEN_HOME && home_requested_social()) {
    swapScreen(SCREEN_SETTINGS);menu_set_destination(MENU_SETTINGS);
  }
  // Fora da home, o Back tem para onde voltar: a home. So nela ele fecha o app.
  if (screen != SCREEN_HOME) {
    int shouldClose = (screen == SCREEN_SEARCH      && search_wants_exit())
              || (screen == SCREEN_LIBRARY && library_wants_exit())
              || (screen == SCREEN_PROFILE      && profile_wants_exit())
              || (screen == SCREEN_SOCIAL      && social_wants_exit())
              || (screen == SCREEN_SETTINGS    && settings_wants_exit());
    if (shouldClose) { swapScreen(SCREEN_HOME); menu_set_destination(MENU_START); }
  } else if (home_wants_exit()) {
    sair = 1;
  }

  // Trocar de usuario, pedido pelo rodape da barra lateral. Vem ANTES do
  // destino: as duas coisas saem do mesmo menu, e quem pediu troca nao quer
  // mudar de aba.
  if (menu_requested_swap()) {
    invalidateProfile();
    screen = SCREEN_CHOICE_PROFILE;
    profilesel_start();
    return;
  }

  if (menu_changed_destination()) {
    switch (menu_destination()) {
      case MENU_FETCH:     swapScreen(SCREEN_SEARCH);      break;
      case MENU_LIBRARY: swapScreen(SCREEN_LIBRARY); break;
      case MENU_PROFILE:     profile_open_side(); requestProfile(); break;
      case MENU_SETTINGS:    swapScreen(SCREEN_SETTINGS);    break;
      default:              swapScreen(SCREEN_HOME);       break;
    }
  }
  // Pedidos de abrir um titulo, vindos de qualquer tela.
  if (!detail_is_open() && !player_is_open()) {
    int idx = -1;
    HomeItem it;
    if (screen == SCREEN_HOME && home_requested_open()) {
      if (home_item_focused(&it)) openTitle(&it);
    } else if (screen == SCREEN_SEARCH && search_requested_open(&idx)) {
      if (search_item_focused(&it)) openTitle(&it); else openByIndex(idx);
    } else if (screen == SCREEN_LIBRARY && library_requested_open(&idx)) {
      openByIndex(idx);
    } else if (screen == SCREEN_PROFILE) {
      ProfileHighlight p;
      if (profile_item_selected(&p) && p.id[0]) {
        idx = cat_index_by_imdb(p.id);
        if (idx >= 0) openByIndex(idx); else disc_request_title(p.id);
      }
    } else if (screen == SCREEN_SOCIAL) {
      SocialItemSelected s;
      if (social_item_selected(&s) && s.imdb[0]) {
        idx=cat_index_by_imdb(s.imdb);
        if(idx>=0)openByIndex(idx); else disc_request_title(s.imdb);
      }
    }
  }

  // Botoes do detalhe: quem sabe que existe player e biblioteca e o roteador,
  // nao a tela de detalhe.
  if (detail_is_open()) {
    // Reproduzir sem escolher = modo automatico: a regra do stream_automatico
    // (MP4 4K Dolby Vision primeiro, senao o primeiro da lista) decide sozinha.
    // Sem lista, o player abre sem video em vez de nao abrir — a tela dizendo
    // que nao ha fonte e melhor que um botao que parece nao responder.
    // Ao abrir um titulo, perguntar as fontes JA — a busca leva segundos e
    // esperar o usuario apertar Reproduzir para so entao comecar faria a
    // primeira reproducao parecer travada.
    // O gatilho e o ID, nao o indice do titulo. Com o indice, mudar de EPISODIO
    // nao repetia a busca e a lista continuava a do episodio anterior — meia
    // correcao seria pior que nenhuma, porque a tela mostraria fontes de um
    // episodio com o nome de outro.
    { static char lastTarget[32] = "";
      int i = detail_index();
      const CatItem *ci = cat_item(i);
      char target[32];
      idOfTarget(ci, target, sizeof target);
      if (!player_is_open() && waitingSource != 2 && ci && ci->imdb[0] && strcmp(target, lastTarget)) {
        snprintf(lastTarget, sizeof lastTarget, "%s", target);
        { addons_fetch(target, ci->kind); }
        // Episodios do titulo aberto, na temporada onde o dono parou. Sai da
        // rede na hora: guardar a lista de episodios de 40 titulos no pacote
        // envelhecia a cada temporada nova.
        // No FILME o mesmo fio busca o /meta/movie quando o catalogo ainda nao
        // tem elenco: e de la que saem atores, direcao e generos da pagina.
        if (!strcmp(ci->kind, "series") || ci->nCast == 0) disc_episodes(i, 0);
        // Legendas do OpenSubtitles junto: sao dezenas por titulo e a busca
        // leva segundos. Pedir so quando o dono abre a folha de faixas faria
        // ele esperar de olho numa lista vazia.
        addons_fetch_subtitles(target, ci->kind);
      } }
    if (detail_requested_play() && waitingSource != 2) {
      // A tela abre JA, no estado "abrindo fonte", e a escolha acontece depois.
      // Escolher antes deixaria o botao sem resposta por segundos, e escolher
      // sem verificar entregava o video de aviso do debrid — que toca normal e
      // por isso passa por sucesso.
      const CatItem *ci = cat_item(detail_index());
      player_open(detail_index(), NULL);
      episodeOfDetail();
      // O episodio so fica definitivo DEPOIS de abrir o player. Refaça sempre
      // o pedido de legenda nesse ponto; a busca de prefetch pode ter comecado
      // no episodio anteriormente focado e o worker agora troca para o pedido
      // mais recente sem publicar resultados velhos.
      if (ci && ci->imdb[0]) {
        char targetSub[64]; targetPlayer(targetSub, sizeof targetSub);
        addons_fetch_subtitles(targetSub, ci->kind);
      }
      if (stream_age_ms() > NV_LINK_VALID_MS && ci && ci->imdb[0]) {
        char target[32]; idOfTarget(ci, target, sizeof target);
        printf("source: list %ums old, refreshing (%s)\n",
               (unsigned)stream_age_ms(), target);
        addons_fetch(target, ci->kind);
      }
      waitingSource = 1;
    }
    if (detail_requested_mark()) {
      // Alterna no Trakt E no espelho local. O estado de partida vem de
      // ci->naLista, que a descoberta preencheu com a watchlist de verdade;
      // sem ele o botao adicionava de novo um titulo que ja estava la.
      int i = detail_index();
      const CatItem *c = cat_item(i);
      library_toggle_list(i);
      if (c && c->imdb[0]) trakt_watchlist(c->imdb, !c->inList);
      if (c) cat_set_in_list(i, !c->inList);
    }
    if (detail_requested_sources())     stream_sheet_open();
  }
  // Escolher uma fonte na folha inicia a reproducao DELA. Trocar de fonte com o
  // player ja aberto tambem vale: fecha a sessao atual e abre na nova, senao
  // duas ficariam presas no mesmo pipeline.
  // A busca disparada por Reproduzir terminou: agora VERIFICA as fontes, em
  // ordem, ate achar uma que leve ao arquivo — e so entao liga o video.
  if (waitingSource == 1 && addons_state() != ADD_SEARCHING) {
    waitingSource = 2;
    sourceChosen = -2;
    if (pthread_create(&threadSource, NULL, chooseSource, NULL) != 0) {
      waitingSource = 0; player_error_source();
    }
  }
  if (waitingSource == 2 && sourceChosen != -2) {
    pthread_join(threadSource,NULL);
    const Stream *s = sourceChosen >= 0 ? stream_item(sourceChosen) : NULL;
    waitingSource = 0;
    printf("automatico (verificado): %s\n", s ? s->label : "(no usable source)");
    // A afirmacao de HDR/DV vai ANTES do tocar: e ela que o bind do ACB
    // descreve ao tv.display. Sem isto o C9 exibe tudo mapeado em SDR.
    if (s) video_set_dv(s->dolbyVision);
    // Anuncia o CONTENTOR pelo mesmo caminho: e o que dispensa a sonda de
    // Matroska num arquivo que nunca teria um cabecalho desses.
    if (s) video_set_mp4(s->mp4 || strstr(s->url, ".mp4") != NULL);
    mark(s ? "source chosen" : "no usable source");
    if (player_is_open() && !player_wants_exit()) {
      stream_set_current(sourceChosen);
      if (s) player_set_source(s->url); else player_error_source();
    }
  }

  int source;
  if (waitingSource != 2 && stream_sheet_chose(&source)) {
    const Stream *s = stream_item(source);
    printf("source chosen: %s\n", s ? s->label : "?");
    if (s) video_set_dv(s->dolbyVision);
    if (s) {
      waitingSource=0;
      int title=player_is_open()?player_index():detail_index(), t=0,e=0;
      if (player_is_open()) { player_episode_current(&t,&e); player_shutdown(); }
      else detail_ep_focus(&t,&e);
      player_open(title,NULL);
      player_set_episode(t,e);
      stream_set_current(source);
      player_set_source(s->url);
    }
  }
  if (waitingSource != 2 && player_requested_sources()) {
    stream_sheet_context(player_line_episode());
    stream_sheet_open();
  }
  if (waitingSource != 2 && stream_sheet_reload()) {
    if (player_is_open()) fetchForPlayer();
    else {
      const CatItem *ci=cat_item(detail_index()); char id[64];
      idOfTarget(ci,id,sizeof id);
      if (ci) addons_fetch(id,ci->kind);
    }
  }
  { int t,e;
    if (waitingSource != 2 && episodes_chose(&t,&e) && player_is_open()) {
      int title=player_index();
      player_shutdown(); player_open(title,NULL);
      player_set_episode(t,e);
      mark("fetching sources"); fetchForPlayer(); waitingSource=1;
    }
  }
  { int t,e;
    if (waitingSource != 2 && player_requested_next(&t,&e) && player_is_open()) {
      int title=player_index();
      player_shutdown(); player_open(title,NULL); player_set_episode(t,e);
      mark("next episode: fetching sources"); fetchForPlayer(); waitingSource=1;
    }
  }
  episodes_update(dt);
  // Prazo do recuo de Dolby Vision: se a declaracao nao render imagem, o video
  // recarrega sozinho sem ela. Precisa bater todo quadro (ver video.h).
  video_pump();
  // Remontagem pedida quando a lista de addons da conta chegou. Aqui, e nao no
  // sync_step, porque a primeira montagem pode ainda estar rodando naquele
  // instante e o pedido precisa sobreviver ate ela terminar.
  disc_step();
  // O player devolve 1 para a coluna de audio e 2 para a de legenda.
  { int q = player_requested_tracks();
    if (q) tracks_open_em(q == 2 ? 1 : 0); }
  tracks_update(dt, now);
  stream_sheet_update(dt, now);

  if (player_wants_exit() && !player_is_open()) player_shutdown();

  player_update(dt, now);
  detail_update(dt, now);
  menu_update(dt, now);
  seeall_update(dt, now);
  ctx_update(dt, now);
  { int i = ctx_requested_details();
    if (i >= 0) {
      const CatItem *ci = cat_item(i);
      HomeItem it;
      memset(&it, 0, sizeof it);
      it.index_ = i;
      it.rect = (GfxRect){ NV_SCREEN_W * 0.5f - 124.0f, NV_SCREEN_H * 0.5f - 186.0f,
                           248.0f, 372.0f };
      it.art   = ci ? (ci->poster[0] ? ci->poster : ci->backdrop) : NULL;
      it.title = ci ? ci->title : NULL;
      it.genre = ci ? ci->genre : NULL;
      it.meta   = ci ? ci->meta : NULL;
      detail_open(&it);
    } }
  // Titulo escolhido na grade: abre o detalhe, como se tivesse vindo da home.
  { int idx = seeall_requested_open();
    if (idx >= 0) {
      const CatItem *ci = cat_item(idx);
      // A grade nao tem retangulo de origem para a transicao crescer a partir
      // dele: o card fica na tela que esta saindo. Entra centrado, do tamanho
      // de um cartaz — o detalhe cobre a tela em seguida de qualquer forma.
      HomeItem it;
      memset(&it, 0, sizeof it);
      it.index_ = idx;
      it.rect = (GfxRect){ NV_SCREEN_W * 0.5f - 124.0f, NV_SCREEN_H * 0.5f - 186.0f,
                           248.0f, 372.0f };
      it.art   = ci ? (ci->poster[0] ? ci->poster : ci->backdrop) : NULL;
      it.title = ci ? ci->title : NULL;
      it.genre = ci ? ci->genre : NULL;
      it.meta   = ci ? ci->meta : NULL;
      detail_open(&it);
    } }
  switch (screen) {
    case SCREEN_SEARCH:      search_update(dt, now);      break;
    case SCREEN_LIBRARY: library_update(dt, now); break;
    case SCREEN_PROFILE:     break;
    case SCREEN_SETTINGS:    settings_update(dt, now);    break;
    default:              home_update(dt, now);       break;
  }
  profile_update(dt, now);
  if(screen==SCREEN_SOCIAL) social_update(dt, now);
}

void app_draw(Uint32 now) {
  if (screen == SCREEN_LOGIN)          { login_draw(now);     return; }
  if (screen == SCREEN_CHOICE_PROFILE) { profilesel_draw(now); return; }

  // Estado vazio de verdade, em vez de uma tela preta que parece travamento.
  if (!homeReady && screen == SCREEN_HOME && !player_is_open() && !detail_is_open()) {
    GfxRect background = { 0, 0, NV_SCREEN_W, NV_SCREEN_H };
    TxtLine t, sb;
    gfx_color(background, 0.0f, NV_COLOR_BACKGROUND_R, NV_COLOR_BACKGROUND_G, NV_COLOR_BACKGROUND_B, 1.0f);
    t = txt_line(TXT_TITLE2, "Preparing your catalogue…", 255, 255, 255, 255);
    txt_draw(t, (NV_SCREEN_W - t.w) * 0.5f, 460.0f);
    sb = txt_line(TXT_BODY,
                   sync_state() == SYNC_RUNNING
                     ? "Fetching your addons and what you were watching."
                     : "If this does not move on, check your addons in the account.",
                   160, 162, 170, 255);
    txt_draw(sb, (NV_SCREEN_W - sb.w) * 0.5f, 546.0f);
    if (menu_visible()) menu_draw(now);
    return;
  }

  // O player cobre tudo; desenhar o que esta atras dele e trabalho jogado fora
  // — a mesma conta que ja valia para o cartao de detalhe esticado.
  if (!player_is_open()) {
    // "Ver tudo" cobre a tela de tras por completo (fundo opaco), entao a home
    // nao precisa ser desenhada por baixo — a mesma conta do detail_cobre_tela.
    if (!detail_covers_screen() && !seeall_is_open()) {
      switch (screen) {
        case SCREEN_SEARCH:      search_draw(now);      break;
        case SCREEN_LIBRARY: library_draw(now); break;
        case SCREEN_PROFILE:     profile_draw(now);     break;
        case SCREEN_SOCIAL:     social_draw(now);     break;
        case SCREEN_SETTINGS:    settings_draw(now);    break;
        default:              home_draw(now);       break;
      }
    }
    if (!detail_covers_screen()) seeall_draw(now);
    ctx_draw(now);
    detail_draw(now);
    // A rail NAO existe na tela de detalhe do app web: ela e full-bleed e a
    // coluna de conteudo comeca em x=72, ou seja, DENTRO do que a rail ocuparia.
    // Com a rail por cima, o logo, o botao "Reproduzir" e a linha de duracao
    // ficavam cortados pela faixa preta de 144px — foi o primeiro defeito que
    // apareceu na captura do aparelho depois do port.
    // A rail some com o detalhe aberto (o web nao a tem nessa tela) e some
    // tambem quando `collapseSidebar` esta ligado, que e o estado do perfil do
    // dono. Recolhida ela nao ocupa largura nenhuma: o conteudo passa a comecar
    // em 104, e quem devolve esse x e ajustes_conteudo_x().
    // A guarda de `collapseSidebar` NAO entra aqui. Ela ja existe DENTRO do
    // menu_desenhar, e la ela pula so a RAIL FIXA — que e o correto: recolhida,
    // a barra nao ocupa largura, mas continua abrindo como CAMADA ao ganhar
    // foco, exatamente como o web faz.
    //
    // Com a guarda tambem neste ponto, o menu_desenhar nunca era chamado no
    // perfil do dono (collapseSidebar ligado): o menu abria, engolia as teclas
    // e nao desenhava nada. Ficava sem menu e sem caminho para os Ajustes — foi
    // o defeito relatado como "nao ta mostrando o menu e nao tem os ajustes".
    // Guarda repetida em dois lugares para a mesma regra: no de dentro ela
    // significa "nao pinte a faixa", no de fora significava "nao exista".
    if (menu_visible() && !detail_is_open())
      menu_draw(now);
    if(profile_side() && !detail_is_open()) profile_draw(now);
  }
  player_draw(now);
  episodes_draw();
  stream_sheet_draw(now);
  tracks_draw(now);
}

int app_wants_exit(void) { return sair; }

void app_shutdown(void) {
  if (waitingSource == 2) pthread_join(threadSource, NULL);
  waitingSource = 0;
  player_shutdown();
  settings_shutdown();
  library_shutdown();
  profile_shutdown();
  search_shutdown();
  home_shutdown();
}
