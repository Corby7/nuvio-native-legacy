#include "traktauth.h"
#include "cloud.h"
#include "data.h"
#include "net.h"
#include "trakt.h"
#include "sync.h"
#include "js.h"
#include "jsw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#define TRA_FILE  "trakt.txt"
#define TRA_STREAM "trakt-flow.txt"
#define TRA_BASE "https://api.trakt.tv"
// Quando o Trakt nao manda `interval`, 5s e o que a documentacao dele sugere.
#define TRA_POLL_DFLT 5000u

static TraState state = TRA_STOPPED;
static char deviceCode[128];
static char userCode[32];
static char url[160];
static char error[200];
static char token[300], refresh[300];
static unsigned pollMs = TRA_POLL_DFLT;
static unsigned nextPoll, beganMs, limitMs;
// Prazo em RELOGIO DE PAREDE, nao em ticks: o pedido tem de sobreviver a um
// reinicio do app, e SDL_GetTicks zera junto com o processo.
static long expiresIn;

static pthread_t thread;
static int threadAlive, threadReady;
// 1 quando o fio acabou de conseguir o token e o laco principal ainda nao o
// aplicou. Aplicar dentro do fio mexeria em trakt.c enquanto a UI le dele.
static int tokenNew;

static char *post(const char *path, const char *body, int *status) {
  char complete[300];
  const char *header[2];
  snprintf(complete, sizeof complete, "%s%s", TRA_BASE, path);
  // O Trakt exige o cabecalho de versao da API; sem ele responde 412.
  header[0] = "trakt-api-version: 2";
  header[1] = NULL;
  return net_post_st(complete, 20, header, body, status);
}

// ---------------------------------------------------------------- disco

// O PEDIDO PENDENTE vai para o disco. Sem isto o codigo do dispositivo vivia so
// na memoria: bastava o app reiniciar — ou o proprio deploy — para a
// autorizacao feita no celular nao ter mais ninguem perguntando por ela. Foi
// exatamente o que aconteceu: o dono autorizou e o app "nao atualizou", porque
// a instancia que tinha pedido o codigo ja nao existia. O app web guarda o
// mesmo estado (TraktAuthStore.saveDeviceFlow).
static void writeStream(void) {
  char buf[600];
  snprintf(buf, sizeof buf, "%s\t%s\t%s\t%ld\n", deviceCode, userCode, url, expiresIn);
  data_write(TRA_STREAM, buf);
}

static void forgetStream(void) {
  deviceCode[0] = userCode[0] = 0;
  expiresIn = 0;
  data_erase(TRA_STREAM);
}

static void save(void) {
  char buf[400];
  // Mesmo formato do art/trakt.txt de antes ("token<TAB>clientId"), para o
  // arquivo continuar legivel por quem ja conhecia o de la. A diferenca e o
  // LUGAR: aqui e a pasta da instalacao, nao o pacote.
  snprintf(buf, sizeof buf, "%s\t%s\n", token, cloud_trakt_client());
  data_write(TRA_FILE, buf);
}

int traktauth_load(void) {
  char *b = data_read(TRA_FILE);
  char *tab;
  if (!b) return 0;
  tab = strchr(b, '\t');
  if (tab) *tab = 0;
  { char *end = b + strlen(b);
    while (end > b && (end[-1] == '\n' || end[-1] == '\r')) *--end = 0; }
  if (b[0]) {
    snprintf(token, sizeof token, "%s", b);
    trakt_set(token, cloud_trakt_client());
    state = TRA_ON;
  }
  free(b);
  if (token[0]) return 1;

  // Sem token, mas pode haver um pedido em andamento de antes do reinicio.
  { char *f = data_read(TRA_STREAM);
    if (f) {
      char *c[4] = { f, NULL, NULL, NULL };
      int i;
      for (i = 1; i < 4 && c[i - 1]; i++) {
        c[i] = strchr(c[i - 1], '\t');
        if (c[i]) *c[i]++ = 0;
      }
      { char *end = f + strlen(f);
        while (end > f && (end[-1] == '\n' || end[-1] == '\r')) *--end = 0; }
      if (c[3]) {
        long now = (long)time(NULL);
        long ate = atol(c[3]);
        if (ate > now + 5) {
          snprintf(deviceCode, sizeof deviceCode, "%s", c[0]);
          snprintf(userCode, sizeof userCode, "%s", c[1]);
          snprintf(url, sizeof url, "%s", c[2]);
          expiresIn = ate;
          state = TRA_WAITING;
          printf("[trakt] resuming the pending request (%lds left)\n", ate - now);
        } else {
          data_erase(TRA_STREAM);
        }
      }
      free(f);
    } }
  return 0;
}

void traktauth_forget(void) {
  token[0] = refresh[0] = url[0] = error[0] = 0;
  state = TRA_STOPPED;
  data_erase(TRA_FILE);
  forgetStream();
}

// ---------------------------------------------------------------- fluxo

static void *threadRequest(void *u) {
  Jsw w;
  char *r;
  int st = 0;
  (void)u;
  error[0] = userCode[0] = deviceCode[0] = 0;

  if (!cloud_trakt_client()[0] || !cloud_trakt_secret()[0]) {
    // Caso de COMPILACAO, nao do usuario: o pacote saiu sem as chaves do
    // aplicativo. Dizer isso evita a pessoa tentar de novo para sempre.
    snprintf(error, sizeof error, "package has no Trakt keys");
    state = TRA_ERROR;
    threadReady = 1;
    return NULL;
  }

  jsw_start(&w);
  jsw_obj_start(&w);
  jsw_cs(&w, "client_id", cloud_trakt_client());
  jsw_obj_end(&w);
  r = post("/oauth/device/code", jsw_text_final(&w), &st);
  jsw_free(&w);

  if (r && st >= 200 && st < 300) {
    const char *end = r + strlen(r);
    double interval, expires;
    js_text(r, end, "device_code", deviceCode, sizeof deviceCode);
    js_text(r, end, "user_code", userCode, sizeof userCode);
    js_text(r, end, "verification_url", url, sizeof url);
    interval = js_num(r, end, "interval", 0);
    expires = js_num(r, end, "expires_in", 0);
    if (interval >= 1.0 && interval <= 60.0) pollMs = (unsigned)(interval * 1000.0);
    limitMs = (expires > 30.0 && expires < 3600.0) ? (unsigned)(expires * 1000.0) : 600000u;
  }
  if (!deviceCode[0] || !userCode[0]) {
    if (st == 429) snprintf(error, sizeof error, "Trakt asked us to wait; try again shortly");
    else snprintf(error, sizeof error, "could not request the code from Trakt (HTTP %d)", st);
    state = TRA_ERROR;
  } else {
    if (!url[0]) snprintf(url, sizeof url, "https://trakt.tv/activate");
    expiresIn = (long)time(NULL) + (long)(limitMs / 1000u);
    writeStream();
    state = TRA_WAITING;
  }
  free(r);
  threadReady = 1;
  return NULL;
}

static void *threadPoll(void *u) {
  Jsw w;
  char *r;
  int st = 0;
  (void)u;

  jsw_start(&w);
  jsw_obj_start(&w);
  jsw_cs(&w, "code", deviceCode);
  jsw_cs(&w, "client_id", cloud_trakt_client());
  jsw_cs(&w, "client_secret", cloud_trakt_secret());
  jsw_obj_end(&w);
  r = post("/oauth/device/token", jsw_text_final(&w), &st);
  jsw_free(&w);

  if (r && st >= 200 && st < 300) {
    char t[300];
    if (js_text(r, r + strlen(r), "access_token", t, sizeof t)) {
      snprintf(token, sizeof token, "%s", t);
      // O refresh vai junto para a conta: sem ele, o vinculo morre no dia em
      // que o access token vencer e o app web nao teria como renovar.
      if (!js_text(r, r + strlen(r), "refresh_token", refresh, sizeof refresh))
        refresh[0] = 0;
      tokenNew = 1;
      forgetStream();
      state = TRA_ON;
    } else {
      snprintf(error, sizeof error, "Trakt answered without a token");
      state = TRA_ERROR;
    }
  } else if (st == 400) {
    /* ainda nao autorizado: seguir perguntando */
  } else if (st == 429) {
    // O Trakt mandou ir mais devagar. Subir o intervalo, com teto.
    pollMs += 5000u;
    if (pollMs > 60000u) pollMs = 60000u;
  } else if (st == 409) {
    snprintf(error, sizeof error, "this code has already been used");
    forgetStream();
    state = TRA_ERROR;
  } else if (st == 410) {
    snprintf(error, sizeof error, "the code expired");
    forgetStream();
    state = TRA_ERROR;
  } else if (st == 418) {
    snprintf(error, sizeof error, "authorisation denied by Trakt");
    forgetStream();
    state = TRA_ERROR;
  } else if (st) {
    snprintf(error, sizeof error, "failed to exchange the code (HTTP %d)", st);
    state = TRA_ERROR;
  }
  free(r);
  threadReady = 1;
  return NULL;
}

static void release(void *(*routine)(void *)) {
  if (threadAlive) return;
  threadReady = 0;
  if (pthread_create(&thread, NULL, routine, NULL) == 0) { pthread_detach(thread); threadAlive = 1; }
  else { snprintf(error, sizeof error, "no thread to talk to Trakt"); state = TRA_ERROR; }
}

void traktauth_begin(void) {
  if (state == TRA_REQUESTING || state == TRA_WAITING) return;
  error[0] = 0;
  beganMs = 0;
  pollMs = TRA_POLL_DFLT;
  state = TRA_REQUESTING;
  release(threadRequest);
}

void traktauth_step(unsigned nowMs) {
  if (threadAlive && threadReady) { threadAlive = 0; threadReady = 0; }
  if (threadAlive) return;

  // Aplicar o token no LACO PRINCIPAL, nunca no fio: trakt.c e lido pela UI.
  if (tokenNew) {
    tokenNew = 0;
    trakt_set(token, cloud_trakt_client());
    save();
    // E manda para a CONTA, para os outros aparelhos da pessoa herdarem o
    // vinculo — e a linha `trakt` que hoje nao existe la.
    { // A forma do credential_json e a que o app web grava, para os dois lados
      // lerem a mesma coisa.
      Jsw c;
      jsw_start(&c);
      jsw_obj_start(&c);
      jsw_cs(&c, "access_token", token);
      if (refresh[0]) jsw_cs(&c, "refresh_token", refresh);
      jsw_cs(&c, "token_type", "bearer");
      jsw_obj_end(&c);
      sync_push_credential("trakt", jsw_text_final(&c));
      jsw_free(&c); }
    printf("[trakt] vinculado nesta TV\n");
    fflush(stdout);
  }

  if (state != TRA_WAITING) return;
  if (!beganMs) beganMs = nowMs;
  if (expiresIn && (long)time(NULL) >= expiresIn) {
    snprintf(error, sizeof error, "the code expired");
    forgetStream();
    state = TRA_ERROR;
    return;
  }
  if (nowMs >= nextPoll) {
    nextPoll = nowMs + pollMs;
    release(threadPoll);
  }
}

void traktauth_cancel(void) {
  if (state == TRA_REQUESTING || state == TRA_WAITING || state == TRA_ERROR)
    state = token[0] ? TRA_ON : TRA_STOPPED;
}

TraState   traktauth_state(void) { return state; }
const char *traktauth_code(void) { return userCode; }
const char *traktauth_url(void)    { return url; }
const char *traktauth_error(void)   { return error; }
