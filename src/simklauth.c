#include "simklauth.h"
#include "cloud.h"
#include "data.h"
#include "net.h"
#include "sync.h"
#include "js.h"
#include "jsw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define SMK_FILE  "simkl.txt"
#define SMK_BASE "https://api.simkl.com"
// O Simkl nao devolve `interval`; 5s e o passo que o app web usa.
#define SMK_POLL_MS 5000u

static SmkState state = SMK_STOPPED;
static char userCode[48];
static char url[200];
static char error[200];
static char token[300];
static unsigned nextPoll, beganMs, limitMs = 900000u;

static pthread_t thread;
static int threadAlive, threadReady, tokenNew;

// Todo pedido leva client_id, app-name e app-version na QUERY — nao em
// cabecalho. Sem eles o Simkl responde erro sem dizer o que faltou.
static char *take(const char *path, int *status) {
  char complete[500], cid[200], name[120];
  const char *header[2];
  cloud_url_escape(cloud_simkl_client(), cid, sizeof cid);
  cloud_url_escape(cloud_simkl_app()[0] ? cloud_simkl_app() : "nuvio", name, sizeof name);
  snprintf(complete, sizeof complete,
           "%s%s?client_id=%s&app-name=%s&app-version=1.0.1", SMK_BASE, path, cid, name);
  header[0] = "Accept: application/json";
  header[1] = NULL;
  return net_download_st(complete, 20, header, status);
}

// ---------------------------------------------------------------- disco

static void save(void) {
  char buf[400];
  snprintf(buf, sizeof buf, "%s\n", token);
  data_write(SMK_FILE, buf);
}

int simklauth_load(void) {
  char *b = data_read(SMK_FILE);
  if (!b) return 0;
  { char *end = b + strlen(b);
    while (end > b && (end[-1] == '\n' || end[-1] == '\r')) *--end = 0; }
  if (b[0]) { snprintf(token, sizeof token, "%s", b); state = SMK_ON; }
  free(b);
  return token[0] != 0;
}

void simklauth_forget(void) {
  token[0] = userCode[0] = url[0] = error[0] = 0;
  state = SMK_STOPPED;
  data_erase(SMK_FILE);
}

// ---------------------------------------------------------------- fluxo

static void *threadRequest(void *u) {
  char *r;
  int st = 0;
  (void)u;
  error[0] = userCode[0] = 0;

  if (!cloud_simkl_client()[0]) {
    snprintf(error, sizeof error, "package has no Simkl key");
    state = SMK_ERROR;
    threadReady = 1;
    return NULL;
  }

  r = take("/oauth/pin", &st);
  if (r && st >= 200 && st < 300) {
    const char *end = r + strlen(r);
    double expires;
    js_text(r, end, "user_code", userCode, sizeof userCode);
    // O campo aparece nas duas grafias na documentacao; aceitar as duas evita
    // uma tela vazia por causa de um "i" a menos.
    if (!js_text(r, end, "verification_url", url, sizeof url))
      js_text(r, end, "verification_uri", url, sizeof url);
    expires = js_num(r, end, "expires_in", 0);
    if (expires > 30.0 && expires < 3600.0) limitMs = (unsigned)(expires * 1000.0);
  }
  if (!userCode[0]) {
    snprintf(error, sizeof error, "could not request the code from Simkl (HTTP %d)", st);
    state = SMK_ERROR;
  } else {
    if (!url[0]) snprintf(url, sizeof url, "https://simkl.com/pin");
    state = SMK_WAITING;
  }
  free(r);
  threadReady = 1;
  return NULL;
}

static void *threadPoll(void *u) {
  char path[120], *r;
  int st = 0;
  (void)u;
  snprintf(path, sizeof path, "/oauth/pin/%s", userCode);
  r = take(path, &st);
  if (r && st >= 200 && st < 300) {
    char res[16], t[300];
    const char *end = r + strlen(r);
    js_text(r, end, "result", res, sizeof res);
    if (!strcmp(res, "KO")) {
      /* ainda nao autorizado */
    } else if (js_text(r, end, "access_token", t, sizeof t) && t[0]) {
      snprintf(token, sizeof token, "%s", t);
      tokenNew = 1;
      state = SMK_ON;
    } else {
      // Resposta que nao e KO nem traz token: o Simkl invalidou este PIN.
      snprintf(error, sizeof error, "Simkl invalidated this code");
      state = SMK_ERROR;
    }
  } else if (st) {
    snprintf(error, sizeof error, "failed to query Simkl (HTTP %d)", st);
    state = SMK_ERROR;
  }
  free(r);
  threadReady = 1;
  return NULL;
}

static void release(void *(*routine)(void *)) {
  if (threadAlive) return;
  threadReady = 0;
  if (pthread_create(&thread, NULL, routine, NULL) == 0) { pthread_detach(thread); threadAlive = 1; }
  else { snprintf(error, sizeof error, "no thread to talk to Simkl"); state = SMK_ERROR; }
}

void simklauth_begin(void) {
  if (state == SMK_REQUESTING || state == SMK_WAITING) return;
  error[0] = 0;
  beganMs = 0;
  state = SMK_REQUESTING;
  release(threadRequest);
}

void simklauth_step(unsigned nowMs) {
  if (threadAlive && threadReady) { threadAlive = 0; threadReady = 0; }
  if (threadAlive) return;

  if (tokenNew) {
    Jsw c;
    tokenNew = 0;
    save();
    jsw_start(&c);
    jsw_obj_start(&c);
    jsw_cs(&c, "access_token", token);
    jsw_obj_end(&c);
    sync_push_credential("simkl", jsw_text_final(&c));
    jsw_free(&c);
    printf("[simkl] vinculado nesta TV\n");
    fflush(stdout);
  }

  if (state != SMK_WAITING) return;
  if (!beganMs) beganMs = nowMs;
  if (nowMs - beganMs > limitMs) {
    snprintf(error, sizeof error, "the code expired");
    state = SMK_ERROR;
    return;
  }
  if (nowMs >= nextPoll) {
    nextPoll = nowMs + SMK_POLL_MS;
    release(threadPoll);
  }
}

void simklauth_cancel(void) {
  if (state == SMK_REQUESTING || state == SMK_WAITING || state == SMK_ERROR)
    state = token[0] ? SMK_ON : SMK_STOPPED;
}

SmkState   simklauth_state(void) { return state; }
const char *simklauth_code(void) { return userCode; }
const char *simklauth_url(void)    { return url; }
const char *simklauth_error(void)   { return error; }
