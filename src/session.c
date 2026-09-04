#include "session.h"
#include "cloud.h"
#include "data.h"
#include "js.h"
#include "jsw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#define FILE_SESSION "session.txt"
// MEDIDO: a propria resposta do start traz `poll_interval_seconds` (3 no
// servidor de hoje). Este valor e so o padrao de quando ela nao vier — o
// intervalo real vem do servidor, que e quem sabe o custo que ele aguenta.
#define POLL_MS      3000
// Depois disto o codigo do servidor expira de qualquer jeito; continuar
// perguntando so gasta bateria do aparelho e mostra um codigo morto na tela.
#define LOGIN_LIMIT_MS 600000

static char access_[3000];
static char refresh[3000];
static char sub[80];
static int  anon;              // 1 quando o token e da sessao anonima
static long expiresIn;             // `exp` do JWT, em segundos

static SessState state = SESS_LOGGEDOUT;
static char code[64];
static char urlLogin[400];
static char nonce[64];
static char error[240];

static unsigned pollMs = POLL_MS;

static pthread_t thread;
static int threadAlive;
static int stepReady;           // o fio terminou; a proxima etapa pode ir
static unsigned nextPoll;
static unsigned loginBeganMs;

// ---------------------------------------------------------------- JWT

// base64url -> bytes. So o necessario para ler o payload do JWT: nem valida
// assinatura nem tenta ser geral. Um JWT invalido aqui vira "sem sub e sem
// exp", que os chamadores ja tratam.
static int b64url(const char *s, size_t n, char *dst, size_t size) {
  static const char *tab =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  unsigned acc = 0;
  int bits = 0;
  size_t i, w = 0;
  for (i = 0; i < n; i++) {
    const char *p = strchr(tab, s[i]);
    if (!p) continue;
    acc = (acc << 6) | (unsigned)(p - tab);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (w + 1 >= size) return 0;
      dst[w++] = (char)((acc >> bits) & 0xFF);
    }
  }
  dst[w] = 0;
  return 1;
}

// Le `sub` e `exp` do payload. Sem isto o app nao sabe de quem e a sessao nem
// quando ela vence, e so descobriria pelo 401 — depois de a operacao ja ter
// falhado uma vez.
static void readJwt(const char *token) {
  const char *p1, *p2;
  char payload[2200];
  sub[0] = 0;
  expiresIn = 0;
  if (!token || !*token) return;
  p1 = strchr(token, '.');
  if (!p1) return;
  p2 = strchr(p1 + 1, '.');
  if (!p2) return;
  if (!b64url(p1 + 1, (size_t)(p2 - p1 - 1), payload, sizeof payload)) return;
  { const char *end = payload + strlen(payload);
    js_text(payload, end, "sub", sub, sizeof sub);
    expiresIn = (long)js_num(payload, end, "exp", 0); }
}

// Folga de 30s, igual a do web: um token que vence durante a requisicao volta
// como 401 e custa a viagem inteira.
static int expired(void) {
  if (!access_[0]) return 1;
  if (!expiresIn) return 0;   // sem exp legivel, so o servidor pode dizer
  return expiresIn <= (long)time(NULL) + 30;
}

// ---------------------------------------------------------------- disco

static void writeSession(void) {
  char buf[6400];
  snprintf(buf, sizeof buf, "%s\n%s\n%d\n", access_, refresh, anon ? 1 : 0);
  data_write(FILE_SESSION, buf);
}

static void clear(void) {
  access_[0] = refresh[0] = sub[0] = 0;
  anon = 0;
  expiresIn = 0;
}

// ---------------------------------------------------------------- respostas

// Guarda os tokens de uma resposta de autenticacao. Serve para os tres
// formatos que aparecem: /auth/v1/signup, /auth/v1/token e a funcao de troca —
// todos trazem access_token/refresh_token, uns na raiz, outros dentro de
// "session".
static int storeTokens(const char *body, int isAnon) {
  const char *end, *sess;
  char a[3000], r[3000];
  if (!body) return 0;
  end = body + strlen(body);
  a[0] = r[0] = 0;
  js_text(body, end, "access_token", a, sizeof a);
  js_text(body, end, "refresh_token", r, sizeof r);
  if (!a[0]) {
    sess = strstr(body, "\"session\"");
    if (sess) {
      js_text(sess, end, "access_token", a, sizeof a);
      js_text(sess, end, "refresh_token", r, sizeof r);
    }
  }
  if (!a[0]) return 0;
  snprintf(access_, sizeof access_, "%s", a);
  snprintf(refresh, sizeof refresh, "%s", r);
  anon = isAnon ? 1 : 0;
  readJwt(access_);
  writeSession();
  return 1;
}

// ---------------------------------------------------------------- anonima

static int sessionAnon(void) {
  char *resp;
  int st = 0;
  // Mesmo corpo do web: o `data` marca de onde veio a sessao, e o servidor usa
  // isso nos relatorios dele.
  resp = cloud_post("/auth/v1/signup",
                    "{\"data\":{\"tv_client\":\"webos\"}}", NULL, &st);
  if (resp && st >= 200 && st < 300 && storeTokens(resp, 1)) { free(resp); return 1; }
  free(resp);
  // O signup anonimo pode estar desligado no projeto; ai o caminho e o grant
  // dedicado. O web tenta os dois na mesma ordem.
  resp = cloud_post("/auth/v1/token?grant_type=anonymous", "{}", NULL, &st);
  if (resp && st >= 200 && st < 300 && storeTokens(resp, 1)) { free(resp); return 1; }
  if (resp) {
    snprintf(error, sizeof error, "anonymous session refused (HTTP %d)", st);
    free(resp);
  } else {
    snprintf(error, sizeof error, "no network when opening the session");
  }
  return 0;
}

// ---------------------------------------------------------------- renovacao

static int refreshToken(void) {
  Jsw w;
  char *resp;
  int st = 0, ok;
  if (!refresh[0]) return 0;
  jsw_start(&w);
  jsw_obj_start(&w);
  jsw_cs(&w, "refresh_token", refresh);
  jsw_obj_end(&w);
  resp = cloud_post("/auth/v1/token?grant_type=refresh_token",
                    jsw_text_final(&w), NULL, &st);
  jsw_free(&w);
  ok = (resp && st >= 200 && st < 300 && storeTokens(resp, anon));
  free(resp);
  if (!ok) {
    // Renovacao recusada e o fim da sessao, nao um erro transitorio: insistir
    // com um refresh token invalido devolve 400 para sempre.
    printf("[session] refresh refused (HTTP %d): signing out\n", st);
    clear();
    data_erase(FILE_SESSION);
    state = SESS_LOGGEDOUT;
  }
  return ok;
}

// ---------------------------------------------------------------- RPC

static char *call(const char *path, const char *body, int *status) {
  char *resp;
  int st = 0;
  if (!cloud_ready()) { if (status) *status = 0; return NULL; }
  if (expired() && refresh[0]) refreshToken();
  resp = cloud_post(path, body, access_, &st);
  if (st == 401 && refresh[0]) {
    free(resp);
    if (!refreshToken()) { if (status) *status = 401; return NULL; }
    resp = cloud_post(path, body, access_, &st);
  }
  if (status) *status = st;
  if (!resp || st == 0 || st >= 500) cloud_failed();
  else cloud_ok();
  return resp;
}

char *session_rpc(const char *func, const char *bodyJson, int *status) {
  char path[300];
  if (!func || !*func) return NULL;
  snprintf(path, sizeof path, "/rest/v1/rpc/%s", func);
  return call(path, bodyJson, status);
}

char *session_table(const char *table, const char *query, int *status) {
  char *resp;
  int st = 0;
  if (!cloud_ready()) { if (status) *status = 0; return NULL; }
  if (expired() && refresh[0]) refreshToken();
  resp = cloud_table(table, query, access_, &st);
  if (st == 401 && refresh[0]) {
    free(resp);
    if (!refreshToken()) { if (status) *status = 401; return NULL; }
    resp = cloud_table(table, query, access_, &st);
  }
  if (status) *status = st;
  if (!resp || st == 0 || st >= 500) cloud_failed();
  else cloud_ok();
  return resp;
}

char *session_func(const char *name, const char *bodyJson, int *status) {
  char path[300];
  if (!name || !*name) return NULL;
  snprintf(path, sizeof path, "/functions/v1/%s", name);
  return call(path, bodyJson, status);
}

// ---------------------------------------------------------------- login

// Passo 1+2, no fio: sessao anonima e pedido do codigo.
static void *threadRequest(void *u) {
  Jsw w;
  char *resp;
  int st = 0;
  (void)u;
  error[0] = 0;
  code[0] = 0;
  urlLogin[0] = 0;

  if (!access_[0] || (anon && expired())) {
    if (!sessionAnon()) { state = SESS_ERROR; stepReady = 1; return NULL; }
  }

  // MEDIDO contra o servidor: `p_redirect_base_url` vazia devolve 400 com
  // "Invalid TV login redirect base URL". Nao ha login sem essa configuracao, e
  // tentar assim mesmo so produziria um erro que a pessoa nao pode resolver.
  if (!cloud_base_login()[0]) {
    snprintf(error, sizeof error, "package has no login address configured");
    state = SESS_ERROR;
    stepReady = 1;
    return NULL;
  }

  // MEDIDO: o nonce TEM de ser um UUID. Com um identificador proprio, mesmo
  // unico, o servidor devolve 400 "Invalid device nonce".
  data_uuid(nonce, sizeof nonce);

  jsw_start(&w);
  jsw_obj_start(&w);
  jsw_cs(&w, "p_device_nonce", nonce);
  jsw_cs(&w, "p_redirect_base_url", cloud_base_login());
  jsw_cs(&w, "p_device_name", "LG webOS (Nuvio nativo)");
  jsw_obj_end(&w);
  resp = cloud_rpc_com("start_tv_login_session", jsw_text_final(&w), access_, &st);

  // Servidor antigo recusa o p_device_name; o web repete sem ele. Sem esta
  // segunda tentativa o login simplesmente nao existe nesses projetos.
  if (resp && cloud_error_missing(resp)) {
    free(resp);
    jsw_free(&w);
    jsw_start(&w);
    jsw_obj_start(&w);
    jsw_cs(&w, "p_device_nonce", nonce);
    jsw_cs(&w, "p_redirect_base_url", cloud_base_login());
    jsw_obj_end(&w);
    resp = cloud_rpc_com("start_tv_login_session", jsw_text_final(&w), access_, &st);
  }
  jsw_free(&w);

  if (resp && st >= 200 && st < 300) {
    const char *end = resp + strlen(resp);
    double interval;
    js_text(resp, end, "code", code, sizeof code);
    // MEDIDO: a resposta ja traz a URL COMPLETA, com o codigo na query. Montar
    // "base + ?code=" a mao daria o mesmo resultado hoje e quebraria no dia em
    // que o servidor mudar o formato — usar o que ele mandou e de graca.
    js_text(resp, end, "web_url", urlLogin, sizeof urlLogin);
    interval = js_num(resp, end, "poll_interval_seconds", 0);
    if (interval >= 1.0 && interval <= 60.0) pollMs = (unsigned)(interval * 1000.0);
  }
  if (!code[0]) {
    // O corpo do erro do PostgREST tem a mensagem util ("Invalid device nonce",
    // "Invalid TV login redirect base URL"); jogar fora e ficar so com o numero
    // do HTTP transformaria um defeito de configuracao em mistério.
    char msg[160];
    msg[0] = 0;
    if (resp) js_text(resp, resp + strlen(resp), "message", msg, sizeof msg);
    if (msg[0]) snprintf(error, sizeof error, "o servidor recusou: %s", msg);
    else        snprintf(error, sizeof error, "could not request the code (HTTP %d)", st);
    state = SESS_ERROR;
  } else {
    if (!urlLogin[0])
      snprintf(urlLogin, sizeof urlLogin, "%s?code=%s", cloud_base_login(), code);
    state = SESS_WAITING;
  }
  free(resp);
  stepReady = 1;
  return NULL;
}

// Passo 3: pergunta se ja autorizaram; quando sim, troca pelo token.
static void *threadPoll(void *u) {
  Jsw w;
  char *resp;
  int st = 0;
  char status[48];
  (void)u;

  jsw_start(&w);
  jsw_obj_start(&w);
  jsw_cs(&w, "p_code", code);
  jsw_cs(&w, "p_device_nonce", nonce);
  jsw_obj_end(&w);
  resp = cloud_rpc_com("poll_tv_login_session", jsw_text_final(&w), access_, &st);
  jsw_free(&w);

  status[0] = 0;
  if (resp && st >= 200 && st < 300)
    js_text(resp, resp + strlen(resp), "status", status, sizeof status);
  free(resp);

  // "approved"/"authorized"/"ready": o servidor nao publica a lista, entao
  // qualquer coisa que NAO seja pendente/expirado tenta a troca. Uma troca
  // recusada nao custa nada; um estado desconhecido tratado como pendente
  // deixaria o usuario preso numa tela que ja podia ter passado.
  if (status[0] && strcmp(status, "pending") && strcmp(status, "expired") &&
      strcmp(status, "cancelled")) {
    state = SESS_SWITCHING;
    jsw_start(&w);
    jsw_obj_start(&w);
    jsw_cs(&w, "code", code);
    jsw_cs(&w, "device_nonce", nonce);
    jsw_obj_end(&w);
    resp = cloud_post("/functions/v1/tv-logins-exchange", jsw_text_final(&w),
                      access_, &st);
    jsw_free(&w);
    if (resp && st >= 200 && st < 300 && storeTokens(resp, 0)) {
      state = SESS_LOGGEDIN;
      code[0] = 0;
      printf("[session] signed in as %s\n", sub[0] ? sub : "(no sub)");
    } else {
      snprintf(error, sizeof error, "code exchange refused (HTTP %d)", st);
      state = SESS_ERROR;
    }
    free(resp);
  } else if (!strcmp(status, "expired") || !strcmp(status, "cancelled")) {
    snprintf(error, sizeof error, "the code expired");
    state = SESS_ERROR;
  }
  stepReady = 1;
  return NULL;
}

static void release(void *(*routine)(void *)) {
  if (threadAlive) return;
  stepReady = 0;
  if (pthread_create(&thread, NULL, routine, NULL) == 0) {
    pthread_detach(thread);
    threadAlive = 1;
  } else {
    snprintf(error, sizeof error, "no thread to talk to the server");
    state = SESS_ERROR;
  }
}

void session_start(void) {
  char *buf = data_read(FILE_SESSION);
  char *l1, *l2, *l3;
  if (!buf) return;
  l1 = buf;
  l2 = strchr(l1, '\n');
  if (l2) { *l2++ = 0; l3 = strchr(l2, '\n'); if (l3) *l3++ = 0; } else l3 = NULL;
  snprintf(access_, sizeof access_, "%s", l1);
  snprintf(refresh, sizeof refresh, "%s", l2 ? l2 : "");
  anon = (l3 && atoi(l3) == 1);
  free(buf);
  readJwt(access_);
  // Sessao anonima gravada NAO conta como conta: ela existe so como degrau para
  // pedir o codigo, e tratar isso como "logado" faria o app pular a tela de
  // login e depois falhar em todo sync com 401 sem explicar nada.
  if (access_[0] && !anon) {
    state = SESS_LOGGEDIN;
    printf("[session] session restored (%s)\n", sub[0] ? sub : "no sub");
  }
}

SessState   session_state(void)   { return state; }
// Ter conta e uma questao de TOKEN, nao do estado da tela: se a pessoa abre a
// tela de login estando logada e a tentativa falha, o estado vira SES_ERRO mas
// a sessao anterior continua boa. Amarrar isto ao estado deslogaria alguem por
// causa de um erro que nao tocou na sessao dela.
int         session_loggedin(void)   { return access_[0] != 0 && !anon; }
const char *session_code(void)   { return code; }
const char *session_url_login(void){ return urlLogin; }
const char *session_error(void)     { return error; }
const char *session_user(void)  { return sub; }

void session_login_begin(void) {
  if (!cloud_ready()) {
    snprintf(error, sizeof error, "app has no server configuration");
    state = SESS_ERROR;
    return;
  }
  if (state == SESS_REQUESTING || state == SESS_WAITING || state == SESS_SWITCHING) return;
  error[0] = 0;
  code[0] = 0;
  state = SESS_REQUESTING;
  loginBeganMs = 0;
  release(threadRequest);
}

void session_step(unsigned nowMs) {
  if (threadAlive && stepReady) { threadAlive = 0; stepReady = 0; }
  if (threadAlive) return;

  if (state == SESS_WAITING) {
    if (!loginBeganMs) loginBeganMs = nowMs;
    if (nowMs - loginBeganMs > LOGIN_LIMIT_MS) {
      snprintf(error, sizeof error, "the code expired");
      state = SESS_ERROR;
      code[0] = 0;
      return;
    }
    if (nowMs >= nextPoll) {
      nextPoll = nowMs + pollMs;
      release(threadPoll);
    }
  }
}

void session_cancel(void) {
  if (state == SESS_REQUESTING || state == SESS_WAITING ||
      state == SESS_SWITCHING || state == SESS_ERROR) {
    code[0] = 0;
    error[0] = 0;
    state = session_loggedin() ? SESS_LOGGEDIN : SESS_LOGGEDOUT;
  }
}

void session_exit(void) {
  clear();
  data_erase(FILE_SESSION);
  code[0] = 0;
  error[0] = 0;
  state = SESS_LOGGEDOUT;
  printf("[session] session ended and erased from disk\n");
}
