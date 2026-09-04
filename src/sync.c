#include "sync.h"
#include "session.h"
#include "cloud.h"
#include "profiles.h"
#include "data.h"
#include "addons.h"
#include "trakt.h"
#include "catalog.h"
#include "settings.h"
#include "discover.h"
#include "extras.h"
#include "js.h"
#include "jsw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define SY_ADD_MAX   16
#define SY_PROGRESS_MAX 240
#define FILE_PROGRESS "progress.txt"

static pthread_t thread;
static int threadAlive, threadReady;
static SyncState state = SYNC_STOPPED;
static char summary[220] = "not synced";
static unsigned lastOk;
static int dirtyProgress, dirtyAddons;

// O fio NAO toca no app: ele so preenche estas caixas, e sync_passo aplica no
// laco principal. Sem essa separacao, uma resposta de rede reescreveria a lista
// de addons no meio de um quadro que ja estava lendo dela.
static AddonRemote addonsRemote[SY_ADD_MAX];
static int nAddonsRemote, hasAddonsRemote;

static char traktToken[300];
static int  hasTraktRemote;

// Chaves de servico que a conta guarda e o app lia de arquivo do dono.
// MEDIDO na conta real: os provedores presentes sao animeskip, debrid:*,
// introdb, mdblist e tmdb — e NAO ha "trakt". O leitor de trakt continua aqui
// porque a RPC e a mesma e a linha aparece assim que o app web a escrever.
static char tmdbKey[120], mdbKey[120];
static int  hasTmdb, hasMdb;

typedef struct { char imdb[40]; double pos, duration; int temp, ep; } ProgressItem;
static ProgressItem progressRemote[SY_PROGRESS_MAX];
static int nProgressRemote;

// Contagens do que foi puxado mas o app ainda nao consome. Elas existem para o
// resumo poder dizer a verdade em vez de "sincronizado" sem qualificar.
static int cWatched, cLib, cSaved, cCollections, hasSettingsProfile, hasCatHome;

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
static char *settingsBlob;
static int  hasSettingsBlob;
static int  applySettings = 1;

// ---------------------------------------------------------------- utilitarios

static int ok2xx(const char *r, int st) { return r && st >= 200 && st < 300; }

// ---------------------------------------------------------------- addons

static void pullAddons(void) {
  char query[400], owner[80];
  char *r;
  int st = 0, k = 0;
  const char *p;

  if (!profiles_owner()[0]) return;
  cloud_url_escape(profiles_owner(), owner, sizeof owner);
  // MEDIDO: `sync_pull_addons` NAO EXISTE neste servidor (PGRST202), e a
  // tabela `tv_addons` tambem nao (PGRST205). O unico caminho que responde e a
  // tabela `addons`, que e exatamente o caminho feliz do app web.
  snprintf(query, sizeof query,
           "user_id=eq.%s&profile_id=eq.%d&select=*&order=sort_order.asc",
           owner, profiles_active());
  // Com a chave anonima o RLS responde 401 "permission denied for table
  // addons": ler as linhas de alguem exige o token de quem esta pedindo.
  r = session_table("addons", query, &st);
  if (!ok2xx(r, st)) {
    if (r && cloud_error_missing(r)) printf("[sync] addons table missing\n");
    else if (st) printf("[sync] addon read: HTTP %d\n", st);
    free(r);
    return;
  }
  for (p = js_root_array(r); p && k < SY_ADD_MAX; p = js_next(js_end(p))) {
    const char *f = js_end(p);
    char b[16];
    memset(&addonsRemote[k], 0, sizeof addonsRemote[k]);
    if (!js_text(p, f, "url", addonsRemote[k].url, sizeof addonsRemote[k].url)) continue;
    js_text(p, f, "name", addonsRemote[k].name, sizeof addonsRemote[k].name);
    // Ausente conta como LIGADO: e assim que o web le, e um addon que some por
    // causa de um campo que o servidor nao mandou e pior que um a mais.
    addonsRemote[k].active = js_raw(p, f, "enabled", b, sizeof b)
                         ? (strcmp(b, "false") != 0) : 1;
    k++;
  }
  free(r);
  nAddonsRemote = k;
  hasAddonsRemote = 1;
}

static void pushAddons(void) {
  AddonRemote current[SY_ADD_MAX];
  Jsw w;
  char *r;
  int st = 0, n, i;

  n = addons_export(current, SY_ADD_MAX);
  // Lista local vazia NAO vira push. Um push vazio apaga os addons da pessoa em
  // todos os aparelhos dela, e "ainda nao carreguei nada" e indistinguivel de
  // "o usuario removeu tudo" deste lado.
  if (n <= 0) return;

  jsw_start(&w);
  jsw_obj_start(&w);
  jsw_ci(&w, "p_profile_id", profiles_active());
  jsw_key(&w, "p_addons");
  jsw_arr_start(&w);
  for (i = 0; i < n; i++) {
    jsw_obj_start(&w);
    jsw_cs(&w, "url", current[i].url);
    jsw_ci(&w, "sort_order", i);
    jsw_cb(&w, "enabled", current[i].active);
    if (current[i].name[0]) jsw_cs(&w, "name", current[i].name);
    jsw_obj_end(&w);
  }
  jsw_arr_end(&w);
  jsw_obj_end(&w);
  r = session_rpc("sync_push_addons", jsw_text_final(&w), &st);
  jsw_free(&w);
  if (!ok2xx(r, st)) printf("[sync] addon push failed (HTTP %d)\n", st);
  else dirtyAddons = 0;
  free(r);
}

// ---------------------------------------------------------------- credenciais

static void pullCredentials(void) {
  Jsw w;
  char *r;
  int st = 0;
  const char *p;

  jsw_start(&w);
  jsw_obj_start(&w);
  jsw_ci(&w, "p_profile_id", profiles_active());
  jsw_obj_end(&w);
  r = session_rpc("sync_pull_provider_credentials", jsw_text_final(&w), &st);
  jsw_free(&w);
  if (!ok2xx(r, st)) { free(r); return; }

  // Trakt, debrid e mdblist compartilham as MESMAS RPC, separados so pelo campo
  // `provider`. Uma leitura serve para os tres.
  for (p = js_root_array(r); p; p = js_next(js_end(p))) {
    const char *f = js_end(p);
    char provider[48], cred[900];
    if (!js_text(p, f, "provider", provider, sizeof provider)) continue;
    if (!js_raw(p, f, "credential_json", cred, sizeof cred)) continue;
    if (!strcmp(provider, "trakt")) {
      // O credential_json pode vir como OBJETO ou como string JSON — o web
      // trata os dois. Aqui basta procurar a chave dentro do texto cru.
      char tk[300];
      if (js_text(cred, cred + strlen(cred), "access_token", tk, sizeof tk)) {
        snprintf(traktToken, sizeof traktToken, "%s", tk);
        hasTraktRemote = 1;
      }
    }
    else if (!strcmp(provider, "tmdb")) {
      if (js_text(cred, cred + strlen(cred), "api_key", tmdbKey, sizeof tmdbKey))
        hasTmdb = 1;
    }
    else if (!strcmp(provider, "mdblist")) {
      if (js_text(cred, cred + strlen(cred), "api_key", mdbKey, sizeof mdbKey))
        hasMdb = 1;
    }
    // debrid:* NAO e aplicado hoje de proposito: as chaves de debrid que este
    // app usa ja vem embutidas na URL do addon (ver addons.h), entao aplicar a
    // chave solta nao mudaria nada e daria a impressao falsa de que o app fala
    // com o provedor por conta propria.
  }
  free(r);
}

// ---------------------------------------------------------------- progresso

static void pullProgress(void) {
  Jsw w;
  char *r;
  int st = 0, k = 0;
  const char *p;

  jsw_start(&w);
  jsw_obj_start(&w);
  jsw_ci(&w, "p_profile_id", profiles_active());
  jsw_obj_end(&w);
  r = session_rpc("sync_pull_watch_progress", jsw_text_final(&w), &st);
  jsw_free(&w);
  if (!ok2xx(r, st)) { free(r); return; }

  for (p = js_root_array(r); p && k < SY_PROGRESS_MAX; p = js_next(js_end(p))) {
    const char *f = js_end(p);
    double pos, duration;
    int temp, ep;
    char id[40];
    if (!js_text(p, f, "content_id", id, sizeof id)) continue;
    // O web aceita position_ms/duration_ms e position/duration; os primeiros
    // ganham quando existem, porque os segundos ja vem em milissegundos nesta
    // RPC e misturar as duas unidades produz progresso de 100% em tudo.
    pos = js_num(p, f, "position_ms", -1.0);
    duration = js_num(p, f, "duration_ms", -1.0);
    if (pos < 0) pos = js_num(p, f, "position", 0);
    if (duration < 0) duration = js_num(p, f, "duration", 0);
    pos /= 1000.0;
    duration /= 1000.0;
    if (duration <= 1.0) continue;
    temp = (int)js_num(p, f, "season", 0);
    ep   = (int)js_num(p, f, "episode", 0);
    if (temp > 0 && ep > 0)
      snprintf(progressRemote[k].imdb, sizeof progressRemote[k].imdb, "%s:%d:%d", id, temp, ep);
    else
      snprintf(progressRemote[k].imdb, sizeof progressRemote[k].imdb, "%s", id);
    progressRemote[k].pos = pos;
    progressRemote[k].duration = duration;
    progressRemote[k].temp = temp;
    progressRemote[k].ep = ep;
    k++;
  }
  free(r);
  // Vazio nao apaga nada: quem consome so aplica o que veio.
  nProgressRemote = k;
}

// Le o progresso que ESTE aparelho gravou. E a unica superficie em que o app
// nativo tem informacao propria de verdade — por isso e a unica, junto dos
// addons, que ele empurra.
static int readProgressLocal(ProgressItem *output, int max) {
  char *buf, *line, *ctx;
  int k = 0;
  buf = data_read(FILE_PROGRESS);
  if (!buf) return 0;
  for (line = strtok_r(buf, "\n", &ctx); line && k < max;
       line = strtok_r(NULL, "\n", &ctx)) {
    char id[40];
    double pos, duration;
    if (sscanf(line, "%39s %lf %lf", id, &pos, &duration) != 3) continue;
    if (duration <= 1.0) continue;
    snprintf(output[k].imdb, sizeof output[k].imdb, "%s", id);
    output[k].pos = pos;
    output[k].duration = duration;
    k++;
  }
  free(buf);
  return k;
}

static void pushProgress(void) {
  ProgressItem local[SY_PROGRESS_MAX];
  Jsw w;
  char *r;
  int n, i, st = 0;

  n = readProgressLocal(local, SY_PROGRESS_MAX);
  if (n <= 0) return;   // vazio nunca vira push; delecao tem RPC propria

  jsw_start(&w);
  jsw_obj_start(&w);
  jsw_ci(&w, "p_profile_id", profiles_active());
  jsw_cs(&w, "p_origin_client_id", data_client_id());
  jsw_key(&w, "p_entries");
  jsw_arr_start(&w);
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

    jsw_obj_start(&w);
    jsw_cs(&w, "content_id", id);
    jsw_cs(&w, "content_type", (temp > 0) ? "series" : "movie");
    jsw_ci(&w, "position", (long long)(local[i].pos * 1000.0));
    jsw_ci(&w, "duration", (long long)(local[i].duration * 1000.0));
    if (temp > 0) { jsw_ci(&w, "season", temp); jsw_ci(&w, "episode", ep); }
    else          { jsw_key(&w, "season"); jsw_null(&w);
                    jsw_key(&w, "episode"); jsw_null(&w); }
    jsw_cs(&w, "progress_key", local[i].imdb);
    jsw_obj_end(&w);
  }
  jsw_arr_end(&w);
  jsw_obj_end(&w);
  r = session_rpc("sync_push_watch_progress", jsw_text_final(&w), &st);
  jsw_free(&w);
  if (!ok2xx(r, st)) printf("[sync] progress push failed (HTTP %d)\n", st);
  else dirtyProgress = 0;
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
#define SY_MISSING 8
static const char *missing[SY_MISSING];
static int nMissing;

static int alreadyMissing(const char *func) {
  int i;
  for (i = 0; i < nMissing; i++)
    if (!strcmp(missing[i], func)) return 1;
  return 0;
}

static int countRpc(const char *func, const char *body) {
  char *r;
  int st = 0, k = 0;
  const char *p;
  if (alreadyMissing(func)) return -1;
  r = session_rpc(func, body, &st);
  if (!ok2xx(r, st)) {
    if (r && cloud_error_missing(r)) {
      printf("[sync] %s does not exist on this server\n", func);
      if (nMissing < SY_MISSING) missing[nMissing++] = func;
    }
    free(r);
    return -1;
  }
  for (p = js_root_array(r); p; p = js_next(js_end(p))) k++;
  free(r);
  return k;
}

// O blob de ajustes NAO e contado, e lido: ele e o layout da pessoa. Ate agora
// esta RPC so alimentava um numero no resumo, e as ~40 preferencias vinham dos
// padroes transcritos a mao do perfil de quem montou o pacote.
static int pullSettingsProfile(const char *body) {
  char *r;
  int st = 0, ok = 0;
  const char *p;
  if (!applySettings) return hasSettingsProfile;   // nada a fazer nesta volta
  r = session_rpc("sync_pull_profile_settings_blob", body, &st);
  if (!ok2xx(r, st)) { free(r); return 0; }
  // A resposta e [{ "settings_json": { ... } }]; o que interessa e o objeto de
  // dentro, cru e INTEIRO.
  p = js_root_array(r);
  if (p) {
    const char *endObj = js_end(p);
    const char *k = strstr(p, "\"settings_json\"");
    if (k && k < endObj) {
      const char *v = strchr(k, ':');
      if (v) {
        v++;
        while (*v && (unsigned char)*v <= ' ') v++;
        if (*v == '{') {
          const char *f = js_end(v);
          size_t n = (size_t)(f - v);
          char *new = (char *)malloc(n + 1);
          if (new) {
            memcpy(new, v, n);
            new[n] = 0;
            free(settingsBlob);
            settingsBlob = new;
            hasSettingsBlob = 1;
            ok = 1;
            printf("[sync] settings blob: %d bytes\n", (int)n);
          }
        } else {
          // O web aceita o blob tambem como STRING JSON serializada. Este
          // servidor devolve objeto; se um dia devolver string, o certo e
          // dizer, nao aplicar um pedaco.
          printf("[sync] settings blob did not arrive as an object; not applied\n");
        }
      }
    }
  }
  free(r);
  return ok;
}

static void pullSoRead(void) {
  char body[160];
  int profile = profiles_active();

  snprintf(body, sizeof body, "{\"p_profile_id\":%d}", profile);
  cLib   = countRpc("sync_pull_library", body);
  cCollections = countRpc("sync_pull_collections", body);

  // MEDIDO: `p_page` comeca em 1. Com 0 o servidor responde 400 "OFFSET must
  // not be negative" — a conta dele e (p_page - 1) * p_page_size.
  snprintf(body, sizeof body,
           "{\"p_profile_id\":%d,\"p_page\":1,\"p_page_size\":200}", profile);
  cWatched = countRpc("sync_pull_watched_items", body);

  snprintf(body, sizeof body,
           "{\"p_profile_id\":%d,\"p_limit\":200,\"p_offset\":0}", profile);
  cSaved = countRpc("sync_pull_saved_library", body);

  snprintf(body, sizeof body,
           "{\"p_profile_id\":%d,\"p_platform\":\"tv\"}", profile);
  hasSettingsProfile = pullSettingsProfile(body);

  snprintf(body, sizeof body,
           "{\"p_profile_id\":%d,\"p_platform\":\"home_catalog_shared\"}", profile);
  hasCatHome = countRpc("sync_pull_home_catalog_settings", body) > 0;
}

// ---------------------------------------------------------------- ciclo

static void *run(void *u) {
  (void)u;
  profiles_pull();
  pullAddons();
  pullCredentials();
  pullProgress();
  pullSoRead();
  // Empurrar DEPOIS de puxar, como o startupSyncService do web: puxar depois
  // de empurrar faria o aparelho sobrescrever com o que ele mesmo mandou.
  if (dirtyAddons)    pushAddons();
  if (dirtyProgress) pushProgress();

  snprintf(summary, sizeof summary,
           "%d addons · %d progress · %d watched · %d in list · %d collections%s",
           nAddonsRemote, nProgressRemote, cWatched < 0 ? 0 : cWatched,
           cLib < 0 ? 0 : cLib, cCollections < 0 ? 0 : cCollections,
           hasTraktRemote ? " · Trakt" : "");
  state = SYNC_READY;
  threadReady = 1;
  return NULL;
}

void sync_start(void) {
  if (threadAlive || !session_loggedin()) return;
  if (cloud_brake_active()) return;
  state = SYNC_RUNNING;
  threadReady = 0;
  if (pthread_create(&thread, NULL, run, NULL) == 0) { pthread_detach(thread); threadAlive = 1; }
  else { state = SYNC_FAILED; snprintf(summary, sizeof summary, "no thread to sync with"); }
}

// Um ciclo automatico, se ja passou o intervalo. Devolve 1 quando disparou.
// Separado de sync_passo porque quem chama sabe se a hora e boa: durante a
// reproducao NAO e — uma rajada de HTTP no meio do video disputa CPU e rede
// com o decodificador, e um engasgo de imagem custa mais que 5 minutos de
// atraso no progresso.
int sync_periodic(unsigned nowMs) {
  if (!session_loggedin() || threadAlive) return 0;
  if (cloud_brake_active()) return 0;
  // Sem nenhum ciclo bem-sucedido ainda, quem manda e quem chamou sync_iniciar
  // — nao adianta insistir por cima de uma falha que o freio ja esta segurando.
  if (!lastOk) return 0;
  if (nowMs - lastOk < SYNC_INTERVAL_MS) return 0;
  sync_start();
  return 1;
}

void sync_step(unsigned nowMs) {
  if (!threadAlive || !threadReady) return;
  threadAlive = 0;
  threadReady = 0;

  if (hasAddonsRemote) { addons_set_list(addonsRemote, nAddonsRemote); hasAddonsRemote = 0; }
  if (hasTraktRemote)  { trakt_set(traktToken, cloud_trakt_client()); hasTraktRemote = 0; }
  if (hasTmdb)      { disc_tmdb_set(tmdbKey);   hasTmdb = 0; }
  if (hasMdb)       { extras_set_key(mdbKey); hasMdb = 0; }
  if (hasSettingsBlob && settingsBlob) {
    settings_apply_blob(settingsBlob);
    free(settingsBlob);
    settingsBlob = NULL;
    hasSettingsBlob = 0;
    applySettings = 0;   // daqui para frente, o que a pessoa mudar na TV fica
  }
  if (nProgressRemote) {
    int i, applied = 0;
    for (i = 0; i < nProgressRemote; i++) {
      int idx = cat_index_by_imdb(progressRemote[i].imdb);
      if (idx < 0) continue;
      // O catalogo deste projeto sabe gravar progresso POR EPISODIO. Usar a
      // versao sem temporada/episodio perderia em qual episodio a pessoa
      // parou, que e a informacao que faz a fileira "continue assistindo"
      // valer alguma coisa numa serie.
      cat_save_progress_ep(idx, progressRemote[i].pos, progressRemote[i].duration,
                              progressRemote[i].temp, progressRemote[i].ep);
      applied++;
    }
    printf("[sync] %d of %d progress entries matched the catalog\n", applied, nProgressRemote);
    nProgressRemote = 0;
  }
  if (state == SYNC_READY) lastOk = nowMs;
}

SyncState  sync_state(void)      { return state; }
const char *sync_summary(void)      { return summary; }
unsigned    sync_last_ok(void)   { return lastOk; }
void        sync_dirty_progress(void) { dirtyProgress = 1; }
void        sync_dirty_addons(void)    { dirtyAddons = 1; }
void sync_push_credential(const char *provider, const char *credJson) {
  Jsw w;
  char *r;
  int st = 0;
  if (!session_loggedin() || !provider || !*provider || !credJson || !*credJson) return;
  jsw_start(&w);
  jsw_obj_start(&w);
  jsw_ci(&w, "p_profile_id", profiles_active());
  jsw_cs(&w, "p_origin_client_id", data_client_id());
  jsw_key(&w, "p_credentials");
  jsw_arr_start(&w);
  jsw_obj_start(&w);
  jsw_cs(&w, "provider", provider);
  jsw_key(&w, "credential_json");
  jsw_raw(&w, credJson);
  jsw_obj_end(&w);
  jsw_arr_end(&w);
  jsw_obj_end(&w);
  r = session_rpc("sync_push_provider_credentials", jsw_text_final(&w), &st);
  jsw_free(&w);
  if (!ok2xx(r, st)) printf("[sync] credential push %s failed (HTTP %d)\n", provider, st);
  else printf("[sync] credential %s stored in the account\n", provider);
  free(r);
}

void sync_reapply_settings(void) { applySettings = 1; }

void sync_forget_user(void) {
  // A ordem importa pouco, mas o CONJUNTO nao: cada linha aqui corresponde a
  // uma coisa que sobrevivia ao logout.
  addons_forget();
  trakt_forget();
  profiles_forget();
  data_erase(FILE_PROGRESS);

  // As caixas que o fio preenche tambem: um ciclo que terminou logo antes do
  // logout aplicaria os addons da conta anterior no proximo sync_passo.
  memset(addonsRemote, 0, sizeof addonsRemote);
  nAddonsRemote = 0; hasAddonsRemote = 0;
  traktToken[0] = 0; hasTraktRemote = 0;
  memset(tmdbKey, 0, sizeof tmdbKey); hasTmdb = 0;
  memset(mdbKey, 0, sizeof mdbKey);   hasMdb = 0;
  nProgressRemote = 0;
  cWatched = cLib = cSaved = cCollections = 0;
  hasSettingsProfile = hasCatHome = 0;
  state = SYNC_STOPPED;
  lastOk = 0;
  dirtyProgress = 0; dirtyAddons = 0;
  free(settingsBlob);
  settingsBlob = NULL;
  hasSettingsBlob = 0;
  applySettings = 1;
  snprintf(summary, sizeof summary, "no account");
  printf("[sync] user data erased from this device\n");
}

void        sync_shutdown(void)    { }
