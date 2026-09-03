#include "sync.h"
#include "sessao.h"
#include "nuvem.h"
#include "perfis.h"
#include "dados.h"
#include "addons.h"
#include "trakt.h"
#include "catalogo.h"
#include "ajustes.h"
#include "descoberta.h"
#include "extras.h"
#include "js.h"
#include "jsw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define SY_ADD_MAX   16
#define SY_PROG_MAX 240
#define ARQ_PROG "progresso.txt"

static pthread_t fio;
static int fioVivo, fioPronto;
static SyncEstado estado = SYNC_PARADO;
static char resumo[220] = "sem sincronizar";
static unsigned ultimoOk;
static int sujoProgresso, sujoAddons;

// O fio NAO toca no app: ele so preenche estas caixas, e sync_passo aplica no
// laco principal. Sem essa separacao, uma resposta de rede reescreveria a lista
// de addons no meio de um quadro que ja estava lendo dela.
static AddonRemoto addonsRem[SY_ADD_MAX];
static int nAddonsRem, temAddonsRem;

static char traktTok[300];
static int  temTraktRem;

// Chaves de servico que a conta guarda e o app lia de arquivo do dono.
// MEDIDO na conta real: os provedores presentes sao animeskip, debrid:*,
// introdb, mdblist e tmdb — e NAO ha "trakt". O leitor de trakt continua aqui
// porque a RPC e a mesma e a linha aparece assim que o app web a escrever.
static char tmdbKey[120], mdbKey[120];
static int  temTmdb, temMdb;

typedef struct { char imdb[40]; double pos, dur; int temp, ep; } ProgItem;
static ProgItem progRem[SY_PROG_MAX];
static int nProgRem;

// Contagens do que foi puxado mas o app ainda nao consome. Elas existem para o
// resumo poder dizer a verdade em vez de "sincronizado" sem qualificar.
static int cVistos, cBiblio, cSalvos, cColecoes, temAjustesPerfil, temCatHome;

// Blob de ajustes do perfil, cru, esperando ser aplicado no fio principal.
// `aplicarAjustes` comeca ligado: no arranque nao ha mudanca local para
// preservar, e e ai que a conta tem de mandar.
//
// ALOCADO, e nao um vetor fixo. MEDIDO na TV com uma conta de verdade: o blob
// nao coube em 4096 bytes e o app recusou aplicar — a recusa estava certa
// (aplicar metade das opcoes traria metade da conta e metade do padrao), mas o
// efeito era o recurso simplesmente nao funcionar. O app web guarda no mesmo
// objeto muito mais chaves do que este app conhece, e escolher um teto aqui e
// escolher uma conta que nao vai funcionar.
static char *ajustesBlob;
static int  temAjustesBlob;
static int  aplicarAjustes = 1;

// ---------------------------------------------------------------- utilitarios

static int ok2xx(const char *r, int st) { return r && st >= 200 && st < 300; }

// ---------------------------------------------------------------- addons

static void puxarAddons(void) {
  char consulta[400], dono[80];
  char *r;
  int st = 0, k = 0;
  const char *p;

  if (!perfis_dono()[0]) return;
  nuvem_url_escapar(perfis_dono(), dono, sizeof dono);
  // MEDIDO: `sync_pull_addons` NAO EXISTE neste servidor (PGRST202), e a
  // tabela `tv_addons` tambem nao (PGRST205). O unico caminho que responde e a
  // tabela `addons`, que e exatamente o caminho feliz do app web.
  snprintf(consulta, sizeof consulta,
           "user_id=eq.%s&profile_id=eq.%d&select=*&order=sort_order.asc",
           dono, perfis_ativo());
  // Com a chave anonima o RLS responde 401 "permission denied for table
  // addons": ler as linhas de alguem exige o token de quem esta pedindo.
  r = sessao_tabela("addons", consulta, &st);
  if (!ok2xx(r, st)) {
    if (r && nuvem_erro_ausente(r)) printf("[sync] tabela addons ausente\n");
    else if (st) printf("[sync] leitura de addons: HTTP %d\n", st);
    free(r);
    return;
  }
  for (p = js_raiz_array(r); p && k < SY_ADD_MAX; p = js_prox(js_fim(p))) {
    const char *f = js_fim(p);
    char b[16];
    memset(&addonsRem[k], 0, sizeof addonsRem[k]);
    if (!js_texto(p, f, "url", addonsRem[k].url, sizeof addonsRem[k].url)) continue;
    js_texto(p, f, "name", addonsRem[k].nome, sizeof addonsRem[k].nome);
    // Ausente conta como LIGADO: e assim que o web le, e um addon que some por
    // causa de um campo que o servidor nao mandou e pior que um a mais.
    addonsRem[k].ativo = js_bruto(p, f, "enabled", b, sizeof b)
                         ? (strcmp(b, "false") != 0) : 1;
    k++;
  }
  free(r);
  nAddonsRem = k;
  temAddonsRem = 1;
}

static void empurrarAddons(void) {
  AddonRemoto atuais[SY_ADD_MAX];
  Jsw w;
  char *r;
  int st = 0, n, i;

  n = addons_exportar(atuais, SY_ADD_MAX);
  // Lista local vazia NAO vira push. Um push vazio apaga os addons da pessoa em
  // todos os aparelhos dela, e "ainda nao carreguei nada" e indistinguivel de
  // "o usuario removeu tudo" deste lado.
  if (n <= 0) return;

  jsw_iniciar(&w);
  jsw_obj_ini(&w);
  jsw_ci(&w, "p_profile_id", perfis_ativo());
  jsw_chave(&w, "p_addons");
  jsw_arr_ini(&w);
  for (i = 0; i < n; i++) {
    jsw_obj_ini(&w);
    jsw_cs(&w, "url", atuais[i].url);
    jsw_ci(&w, "sort_order", i);
    jsw_cb(&w, "enabled", atuais[i].ativo);
    if (atuais[i].nome[0]) jsw_cs(&w, "name", atuais[i].nome);
    jsw_obj_fim(&w);
  }
  jsw_arr_fim(&w);
  jsw_obj_fim(&w);
  r = sessao_rpc("sync_push_addons", jsw_texto_final(&w), &st);
  jsw_livre(&w);
  if (!ok2xx(r, st)) printf("[sync] push de addons falhou (HTTP %d)\n", st);
  else sujoAddons = 0;
  free(r);
}

// ---------------------------------------------------------------- credenciais

static void puxarCredenciais(void) {
  Jsw w;
  char *r;
  int st = 0;
  const char *p;

  jsw_iniciar(&w);
  jsw_obj_ini(&w);
  jsw_ci(&w, "p_profile_id", perfis_ativo());
  jsw_obj_fim(&w);
  r = sessao_rpc("sync_pull_provider_credentials", jsw_texto_final(&w), &st);
  jsw_livre(&w);
  if (!ok2xx(r, st)) { free(r); return; }

  // Trakt, debrid e mdblist compartilham as MESMAS RPC, separados so pelo campo
  // `provider`. Uma leitura serve para os tres.
  for (p = js_raiz_array(r); p; p = js_prox(js_fim(p))) {
    const char *f = js_fim(p);
    char prov[48], cred[900];
    if (!js_texto(p, f, "provider", prov, sizeof prov)) continue;
    if (!js_bruto(p, f, "credential_json", cred, sizeof cred)) continue;
    if (!strcmp(prov, "trakt")) {
      // O credential_json pode vir como OBJETO ou como string JSON — o web
      // trata os dois. Aqui basta procurar a chave dentro do texto cru.
      char tk[300];
      if (js_texto(cred, cred + strlen(cred), "access_token", tk, sizeof tk)) {
        snprintf(traktTok, sizeof traktTok, "%s", tk);
        temTraktRem = 1;
      }
    }
    else if (!strcmp(prov, "tmdb")) {
      if (js_texto(cred, cred + strlen(cred), "api_key", tmdbKey, sizeof tmdbKey))
        temTmdb = 1;
    }
    else if (!strcmp(prov, "mdblist")) {
      if (js_texto(cred, cred + strlen(cred), "api_key", mdbKey, sizeof mdbKey))
        temMdb = 1;
    }
    // debrid:* NAO e aplicado hoje de proposito: as chaves de debrid que este
    // app usa ja vem embutidas na URL do addon (ver addons.h), entao aplicar a
    // chave solta nao mudaria nada e daria a impressao falsa de que o app fala
    // com o provedor por conta propria.
  }
  free(r);
}

// ---------------------------------------------------------------- progresso

static void puxarProgresso(void) {
  Jsw w;
  char *r;
  int st = 0, k = 0;
  const char *p;

  jsw_iniciar(&w);
  jsw_obj_ini(&w);
  jsw_ci(&w, "p_profile_id", perfis_ativo());
  jsw_obj_fim(&w);
  r = sessao_rpc("sync_pull_watch_progress", jsw_texto_final(&w), &st);
  jsw_livre(&w);
  if (!ok2xx(r, st)) { free(r); return; }

  for (p = js_raiz_array(r); p && k < SY_PROG_MAX; p = js_prox(js_fim(p))) {
    const char *f = js_fim(p);
    double pos, dur;
    int temp, ep;
    char id[40];
    if (!js_texto(p, f, "content_id", id, sizeof id)) continue;
    // O web aceita position_ms/duration_ms e position/duration; os primeiros
    // ganham quando existem, porque os segundos ja vem em milissegundos nesta
    // RPC e misturar as duas unidades produz progresso de 100% em tudo.
    pos = js_num(p, f, "position_ms", -1.0);
    dur = js_num(p, f, "duration_ms", -1.0);
    if (pos < 0) pos = js_num(p, f, "position", 0);
    if (dur < 0) dur = js_num(p, f, "duration", 0);
    pos /= 1000.0;
    dur /= 1000.0;
    if (dur <= 1.0) continue;
    temp = (int)js_num(p, f, "season", 0);
    ep   = (int)js_num(p, f, "episode", 0);
    if (temp > 0 && ep > 0)
      snprintf(progRem[k].imdb, sizeof progRem[k].imdb, "%s:%d:%d", id, temp, ep);
    else
      snprintf(progRem[k].imdb, sizeof progRem[k].imdb, "%s", id);
    progRem[k].pos = pos;
    progRem[k].dur = dur;
    progRem[k].temp = temp;
    progRem[k].ep = ep;
    k++;
  }
  free(r);
  // Vazio nao apaga nada: quem consome so aplica o que veio.
  nProgRem = k;
}

// Le o progresso que ESTE aparelho gravou. E a unica superficie em que o app
// nativo tem informacao propria de verdade — por isso e a unica, junto dos
// addons, que ele empurra.
static int lerProgressoLocal(ProgItem *saida, int max) {
  char *buf, *linha, *ctx;
  int k = 0;
  buf = dados_ler(ARQ_PROG);
  if (!buf) return 0;
  for (linha = strtok_r(buf, "\n", &ctx); linha && k < max;
       linha = strtok_r(NULL, "\n", &ctx)) {
    char id[40];
    double pos, dur;
    if (sscanf(linha, "%39s %lf %lf", id, &pos, &dur) != 3) continue;
    if (dur <= 1.0) continue;
    snprintf(saida[k].imdb, sizeof saida[k].imdb, "%s", id);
    saida[k].pos = pos;
    saida[k].dur = dur;
    k++;
  }
  free(buf);
  return k;
}

static void empurrarProgresso(void) {
  ProgItem local[SY_PROG_MAX];
  Jsw w;
  char *r;
  int n, i, st = 0;

  n = lerProgressoLocal(local, SY_PROG_MAX);
  if (n <= 0) return;   // vazio nunca vira push; delecao tem RPC propria

  jsw_iniciar(&w);
  jsw_obj_ini(&w);
  jsw_ci(&w, "p_profile_id", perfis_ativo());
  jsw_cs(&w, "p_origin_client_id", dados_cliente_id());
  jsw_chave(&w, "p_entries");
  jsw_arr_ini(&w);
  for (i = 0; i < n; i++) {
    // "tt123:4:9" carrega temporada e episodio; o servidor quer os tres campos
    // separados, e mandar o id composto em content_id faria cada episodio
    // virar um titulo diferente na conta.
    char id[40];
    int temp = 0, ep = 0;
    char *dp;
    snprintf(id, sizeof id, "%s", local[i].imdb);
    dp = strchr(id, ':');
    if (dp) { sscanf(dp + 1, "%d:%d", &temp, &ep); *dp = 0; }

    jsw_obj_ini(&w);
    jsw_cs(&w, "content_id", id);
    jsw_cs(&w, "content_type", (temp > 0) ? "series" : "movie");
    jsw_ci(&w, "position", (long long)(local[i].pos * 1000.0));
    jsw_ci(&w, "duration", (long long)(local[i].dur * 1000.0));
    if (temp > 0) { jsw_ci(&w, "season", temp); jsw_ci(&w, "episode", ep); }
    else          { jsw_chave(&w, "season"); jsw_nulo(&w);
                    jsw_chave(&w, "episode"); jsw_nulo(&w); }
    jsw_cs(&w, "progress_key", local[i].imdb);
    jsw_obj_fim(&w);
  }
  jsw_arr_fim(&w);
  jsw_obj_fim(&w);
  r = sessao_rpc("sync_push_watch_progress", jsw_texto_final(&w), &st);
  jsw_livre(&w);
  if (!ok2xx(r, st)) printf("[sync] push de progresso falhou (HTTP %d)\n", st);
  else sujoProgresso = 0;
  free(r);
}

// ---------------------------------------------------------------- so leitura

// Conta os itens de uma RPC que devolve array. Estas superficies sao puxadas
// mas ainda nao consumidas: o app nativo nao tem tela propria para elas, e
// EMPURRAR sem ter a tela mandaria lista vazia — que apaga o dado nos outros
// aparelhos da pessoa. Contar e dizer no resumo e o comportamento honesto ate
// a tela existir.
// RPC que o servidor nao tem NAO e perguntada de novo. MEDIDO:
// `sync_pull_saved_library` nao existe neste servidor, e sem esta lista o app
// gastaria uma viagem por ciclo, para sempre, contra um 404 que nunca muda.
#define SY_AUSENTES 8
static const char *ausentes[SY_AUSENTES];
static int nAusentes;

static int jaAusente(const char *funcao) {
  int i;
  for (i = 0; i < nAusentes; i++)
    if (!strcmp(ausentes[i], funcao)) return 1;
  return 0;
}

static int contarRpc(const char *funcao, const char *corpo) {
  char *r;
  int st = 0, k = 0;
  const char *p;
  if (jaAusente(funcao)) return -1;
  r = sessao_rpc(funcao, corpo, &st);
  if (!ok2xx(r, st)) {
    if (r && nuvem_erro_ausente(r)) {
      printf("[sync] %s nao existe neste servidor\n", funcao);
      if (nAusentes < SY_AUSENTES) ausentes[nAusentes++] = funcao;
    }
    free(r);
    return -1;
  }
  for (p = js_raiz_array(r); p; p = js_prox(js_fim(p))) k++;
  free(r);
  return k;
}

// O blob de ajustes NAO e contado, e lido: ele e o layout da pessoa. Ate agora
// esta RPC so alimentava um numero no resumo, e as ~40 preferencias vinham dos
// padroes transcritos a mao do perfil de quem montou o pacote.
static int puxarAjustesPerfil(const char *corpo) {
  char *r;
  int st = 0, ok = 0;
  const char *p;
  if (!aplicarAjustes) return temAjustesPerfil;   // nada a fazer nesta volta
  r = sessao_rpc("sync_pull_profile_settings_blob", corpo, &st);
  if (!ok2xx(r, st)) { free(r); return 0; }
  // A resposta e [{ "settings_json": { ... } }]; o que interessa e o objeto de
  // dentro, cru e INTEIRO.
  p = js_raiz_array(r);
  if (p) {
    const char *fimObj = js_fim(p);
    const char *k = strstr(p, "\"settings_json\"");
    if (k && k < fimObj) {
      const char *v = strchr(k, ':');
      if (v) {
        v++;
        while (*v && (unsigned char)*v <= ' ') v++;
        if (*v == '{') {
          const char *f = js_fim(v);
          size_t n = (size_t)(f - v);
          char *novo = (char *)malloc(n + 1);
          if (novo) {
            memcpy(novo, v, n);
            novo[n] = 0;
            free(ajustesBlob);
            ajustesBlob = novo;
            temAjustesBlob = 1;
            ok = 1;
            printf("[sync] blob de ajustes: %d bytes\n", (int)n);
          }
        } else {
          // O web aceita o blob tambem como STRING JSON serializada. Este
          // servidor devolve objeto; se um dia devolver string, o certo e
          // dizer, nao aplicar um pedaco.
          printf("[sync] blob de ajustes nao veio como objeto; nao aplicado\n");
        }
      }
    }
  }
  free(r);
  return ok;
}

static void puxarSoLeitura(void) {
  char corpo[160];
  int perfil = perfis_ativo();

  snprintf(corpo, sizeof corpo, "{\"p_profile_id\":%d}", perfil);
  cBiblio   = contarRpc("sync_pull_library", corpo);
  cColecoes = contarRpc("sync_pull_collections", corpo);

  // MEDIDO: `p_page` comeca em 1. Com 0 o servidor responde 400 "OFFSET must
  // not be negative" — a conta dele e (p_page - 1) * p_page_size.
  snprintf(corpo, sizeof corpo,
           "{\"p_profile_id\":%d,\"p_page\":1,\"p_page_size\":200}", perfil);
  cVistos = contarRpc("sync_pull_watched_items", corpo);

  snprintf(corpo, sizeof corpo,
           "{\"p_profile_id\":%d,\"p_limit\":200,\"p_offset\":0}", perfil);
  cSalvos = contarRpc("sync_pull_saved_library", corpo);

  snprintf(corpo, sizeof corpo,
           "{\"p_profile_id\":%d,\"p_platform\":\"tv\"}", perfil);
  temAjustesPerfil = puxarAjustesPerfil(corpo);

  snprintf(corpo, sizeof corpo,
           "{\"p_profile_id\":%d,\"p_platform\":\"home_catalog_shared\"}", perfil);
  temCatHome = contarRpc("sync_pull_home_catalog_settings", corpo) > 0;
}

// ---------------------------------------------------------------- ciclo

static void *rodar(void *u) {
  (void)u;
  perfis_puxar();
  puxarAddons();
  puxarCredenciais();
  puxarProgresso();
  puxarSoLeitura();
  // Empurrar DEPOIS de puxar, como o startupSyncService do web: puxar depois
  // de empurrar faria o aparelho sobrescrever com o que ele mesmo mandou.
  if (sujoAddons)    empurrarAddons();
  if (sujoProgresso) empurrarProgresso();

  snprintf(resumo, sizeof resumo,
           "%d addons · %d progressos · %d vistos · %d na lista · %d coleções%s",
           nAddonsRem, nProgRem, cVistos < 0 ? 0 : cVistos,
           cBiblio < 0 ? 0 : cBiblio, cColecoes < 0 ? 0 : cColecoes,
           temTraktRem ? " · Trakt" : "");
  estado = SYNC_PRONTO;
  fioPronto = 1;
  return NULL;
}

void sync_iniciar(void) {
  if (fioVivo || !sessao_logada()) return;
  if (nuvem_freio_ativo()) return;
  estado = SYNC_RODANDO;
  fioPronto = 0;
  if (pthread_create(&fio, NULL, rodar, NULL) == 0) { pthread_detach(fio); fioVivo = 1; }
  else { estado = SYNC_FALHOU; snprintf(resumo, sizeof resumo, "sem fio para sincronizar"); }
}

// Um ciclo automatico, se ja passou o intervalo. Devolve 1 quando disparou.
// Separado de sync_passo porque quem chama sabe se a hora e boa: durante a
// reproducao NAO e — uma rajada de HTTP no meio do video disputa CPU e rede
// com o decodificador, e um engasgo de imagem custa mais que 5 minutos de
// atraso no progresso.
int sync_periodico(unsigned agoraMs) {
  if (!sessao_logada() || fioVivo) return 0;
  if (nuvem_freio_ativo()) return 0;
  // Sem nenhum ciclo bem-sucedido ainda, quem manda e quem chamou sync_iniciar
  // — nao adianta insistir por cima de uma falha que o freio ja esta segurando.
  if (!ultimoOk) return 0;
  if (agoraMs - ultimoOk < SYNC_INTERVALO_MS) return 0;
  sync_iniciar();
  return 1;
}

void sync_passo(unsigned agoraMs) {
  if (!fioVivo || !fioPronto) return;
  fioVivo = 0;
  fioPronto = 0;

  if (temAddonsRem) { addons_definir_lista(addonsRem, nAddonsRem); temAddonsRem = 0; }
  if (temTraktRem)  { trakt_definir(traktTok, nuvem_trakt_cliente()); temTraktRem = 0; }
  if (temTmdb)      { desc_tmdb_definir(tmdbKey);   temTmdb = 0; }
  if (temMdb)       { extras_definir_chave(mdbKey); temMdb = 0; }
  if (temAjustesBlob && ajustesBlob) {
    ajustes_aplicar_blob(ajustesBlob);
    free(ajustesBlob);
    ajustesBlob = NULL;
    temAjustesBlob = 0;
    aplicarAjustes = 0;   // daqui para frente, o que a pessoa mudar na TV fica
  }
  if (nProgRem) {
    int i, aplicados = 0;
    for (i = 0; i < nProgRem; i++) {
      int idx = cat_indice_por_imdb(progRem[i].imdb);
      if (idx < 0) continue;
      // O catalogo deste projeto sabe gravar progresso POR EPISODIO. Usar a
      // versao sem temporada/episodio perderia em qual episodio a pessoa
      // parou, que e a informacao que faz a fileira "continue assistindo"
      // valer alguma coisa numa serie.
      cat_salvar_progresso_ep(idx, progRem[i].pos, progRem[i].dur,
                              progRem[i].temp, progRem[i].ep);
      aplicados++;
    }
    printf("[sync] %d de %d progressos casaram com o catalogo\n", aplicados, nProgRem);
    nProgRem = 0;
  }
  if (estado == SYNC_PRONTO) ultimoOk = agoraMs;
}

SyncEstado  sync_estado(void)      { return estado; }
const char *sync_resumo(void)      { return resumo; }
unsigned    sync_ultimo_ok(void)   { return ultimoOk; }
void        sync_sujar_progresso(void) { sujoProgresso = 1; }
void        sync_sujar_addons(void)    { sujoAddons = 1; }
void sync_empurrar_credencial(const char *provider, const char *credJson) {
  Jsw w;
  char *r;
  int st = 0;
  if (!sessao_logada() || !provider || !*provider || !credJson || !*credJson) return;
  jsw_iniciar(&w);
  jsw_obj_ini(&w);
  jsw_ci(&w, "p_profile_id", perfis_ativo());
  jsw_cs(&w, "p_origin_client_id", dados_cliente_id());
  jsw_chave(&w, "p_credentials");
  jsw_arr_ini(&w);
  jsw_obj_ini(&w);
  jsw_cs(&w, "provider", provider);
  jsw_chave(&w, "credential_json");
  jsw_bruto(&w, credJson);
  jsw_obj_fim(&w);
  jsw_arr_fim(&w);
  jsw_obj_fim(&w);
  r = sessao_rpc("sync_push_provider_credentials", jsw_texto_final(&w), &st);
  jsw_livre(&w);
  if (!ok2xx(r, st)) printf("[sync] push de credencial %s falhou (HTTP %d)\n", provider, st);
  else printf("[sync] credencial %s guardada na conta\n", provider);
  free(r);
}

void sync_reaplicar_ajustes(void) { aplicarAjustes = 1; }

void sync_esquecer_usuario(void) {
  // A ordem importa pouco, mas o CONJUNTO nao: cada linha aqui corresponde a
  // uma coisa que sobrevivia ao logout.
  addons_esquecer();
  trakt_esquecer();
  perfis_esquecer();
  dados_apagar(ARQ_PROG);

  // As caixas que o fio preenche tambem: um ciclo que terminou logo antes do
  // logout aplicaria os addons da conta anterior no proximo sync_passo.
  memset(addonsRem, 0, sizeof addonsRem);
  nAddonsRem = 0; temAddonsRem = 0;
  traktTok[0] = 0; temTraktRem = 0;
  memset(tmdbKey, 0, sizeof tmdbKey); temTmdb = 0;
  memset(mdbKey, 0, sizeof mdbKey);   temMdb = 0;
  nProgRem = 0;
  cVistos = cBiblio = cSalvos = cColecoes = 0;
  temAjustesPerfil = temCatHome = 0;
  estado = SYNC_PARADO;
  ultimoOk = 0;
  sujoProgresso = 0; sujoAddons = 0;
  free(ajustesBlob);
  ajustesBlob = NULL;
  temAjustesBlob = 0;
  aplicarAjustes = 1;
  snprintf(resumo, sizeof resumo, "sem conta");
  printf("[sync] dados do usuario apagados deste aparelho\n");
}

void        sync_encerrar(void)    { }
