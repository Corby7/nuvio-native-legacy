#include "net.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

// Constantes da libcurl escritas a mao: nao ha curl.h no SDK do aparelho, e
// puxar o header inteiro so por meia duzia de numeros nao se paga. Os valores
// sao estaveis desde sempre (CURLOPTTYPE_OBJECTPOINT = 10000 etc).
#define OPT_URL             10002
#define OPT_WRITEFUNCTION   20011
#define OPT_WRITEDATA       10001
#define OPT_TIMEOUT            13
#define OPT_FOLLOWLOCATION     52
#define OPT_SSL_VERIFYPEER     64
#define OPT_SSL_VERIFYHOST     81
#define OPT_USERAGENT       10018
#define OPT_ACCEPT_ENCODING 10102
#define OPT_NOSIGNAL          99
#define OPT_HTTPHEADER      10023
#define OPT_NOBODY             44
#define OPT_RANGE           10007
#define INFO_URL_FINAL    1048577
#define OPT_POSTFIELDS      10015
#define OPT_POST               47
// CURLOPT_CONNECTTIMEOUT. Separate from OPT_TIMEOUT because the two measure
// different things: a DEAD host spent the whole transfer budget on a connection
// that was never going to complete, and that was the same number giving a slow
// but LIVE server room to answer. With the connect ceiling on its own, an addon
// that is down fails in CONNECT_SECONDS and the slow one keeps its full 8 s.
#define OPT_CONNECTTIMEOUT     78
// CURLINFO_RESPONSE_CODE = CURLINFO_LONG (0x200000) + 2.
#define INFO_RESPONSE_CODE   2097154

// Seconds to ESTABLISH the connection (DNS + TCP + TLS), not the ceiling for
// the transfer. MEASURED on the Mac: a good connection completes the whole
// handshake in under 300 ms; 3 s covers the TV, which is slower, without coming
// near the 8 s a catalogue has to answer in full.
#define CONNECT_SECONDS 3L

static void *(*curl_init)(void);
static int   (*curl_setopt)(void *, int, ...);
static int   (*curl_perform)(void *);
static void  (*curl_cleanup)(void *);
static void  (*curl_reset)(void *);
static int   (*curl_global)(long);
static void *(*slist_append)(void *, const char *);
static void  (*slist_free)(void *);
static int   (*curl_getinfo)(void *, int, ...);
static int    ready;

// `cap`: optional byte ceiling for THIS transfer; 0 = no ceiling. It exists
// because a server that IGNORES the Range header answers 200 with the whole
// file, and in that case the header it was asked for limits nothing.
//
// It lives in the bucket and not in a global because `receive` is the receiver
// for EVERY transfer in the app, on any thread. While it was global, the 320 KB
// ceiling mkv.c uses to read a film's header also applied to whatever the four
// artwork threads were downloading at that instant — and a truncated image
// passes tex_cache's signature check (the magic bytes are intact) and goes to
// the disk cache, which has neither expiry nor eviction.
typedef struct { char *p; size_t n; long cap; } Bucket;

static char *net_download_internal(const char *url, int seconds, long *size,
                                 const char *const *header, long cap);
static char *net_download_internal2(const char *url, int seconds, long *size,
                                  const char *const *header, int *status,
                                  long cap);

static size_t receive(void *data, size_t size, size_t count, void *u) {
  Bucket *b = (Bucket *)u;
  size_t bytes = size * count;
  char *new;
  if (b->cap > 0 && b->n >= (size_t)b->cap) return 0;   // corta a conexao
  if (b->cap > 0 && b->n + bytes > (size_t)b->cap)
    bytes = (size_t)b->cap - b->n;
  new = realloc(b->p, b->n + bytes + 1);
  if (!new) return 0;              // devolver 0 aborta a transferencia
  b->p = new;
  memcpy(b->p + b->n, data, bytes);
  b->n += bytes;
  b->p[b->n] = 0;
  return bytes;
}

// --- ONE HANDLE PER THREAD, REUSED -------------------------------------------
//
// Every request used to open and close its own handle, and everything libcurl
// keeps INSIDE the handle went with it: the open connection, the DNS cache and
// the TLS session. Startup makes ~32 requests and the four artwork threads make
// hundreds, nearly all against the same few hosts (v3-cinemeta.strem.io,
// api.trakt.tv, api.themoviedb.org, images.metahub.space) — and each one paid
// for DNS, TCP and a handshake again. On a 2019 TV over Wi-Fi that is the cost
// that dominates.
//
// The handle lives on the THREAD and not in a shared pool because a libcurl
// easy handle is NOT safe across threads; one per thread buys the reuse without
// a single lock.
//
// The key's destructor is what closes the handle when the thread dies. It is
// required, not a detail: half the threads here are detached (search, director,
// parental guide, intro, subtitle, the login pollers) and nobody joins them, so
// there is no outside moment at which to clean up.
static pthread_key_t handleKey;
static int handleKeyReady;

static void handleGone(void *h) {
  if (h && curl_cleanup) curl_cleanup(h);
}

// This thread's handle, cleared and ready to take the options. `own` comes back
// 1 when the handle is disposable and the caller has to close it; 0 when it
// belongs to the thread and outlives the request.
static void *handleTake(int *own) {
  void *h;
  *own = 1;
  if (!handleKeyReady) return curl_init();
  h = pthread_getspecific(handleKey);
  if (h) {
    // Clears the options and KEEPS the connection, the DNS cache and the TLS
    // session — which is exactly what we are here to keep. Without this reset
    // the handle would reach the next request still carrying net_url_final's
    // OPT_RANGE, or the headers of a Trakt call.
    curl_reset(h);
    *own = 0;
    return h;
  }
  h = curl_init();
  if (!h) return NULL;
  // A fresh handle is already clean and needs no reset. If there is nowhere to
  // store it, it stays disposable and the request happens all the same.
  if (pthread_setspecific(handleKey, h) == 0) *own = 0;
  return h;
}

// Closes the request. `list` is the header list, which libcurl does NOT own: it
// only holds a pointer to it, so it has to be unhooked from the handle BEFORE
// being freed. With a disposable handle that was implicit, because the handle
// died first; with a handle that survives the request it is not, and the next
// curl_easy_reset would be the only thing standing between a dangling pointer
// and a request that reuses it.
static void handleGive(void *h, int own, void *list) {
  if (h && list && curl_setopt) curl_setopt(h, OPT_HTTPHEADER, (void *)0);
  if (list && slist_free) slist_free(list);
  if (own && h && curl_cleanup) curl_cleanup(h);
}

// CARREGAMENTO DA LIBCURL, UMA VEZ SO E COM TRAVA.
//
// `curl_global_init` NAO e seguro entre fios — e a propria libcurl documenta
// isso. Isto aqui era uma bandeira simples, e enquanto so a descoberta e dois
// fios de decode chamavam, a corrida quase nunca acontecia. Ao acrescentar
// QUATRO fios de rede para as artes, todos partindo no arranque, ela passou a
// acontecer: dois fios entram com `pronto == 0`, os dois fazem dlopen e os dois
// chamam curl_global_init ao mesmo tempo. O estado global fica corrompido e
// TODO download passa a falhar — catalogos, addons e artes de uma vez, que foi
// exatamente o que o dono viu depois do ultimo deploy.
//
// A trava e estatica e sem inicializacao dinamica de proposito: ela precisa
// existir ANTES do primeiro fio, e um PTHREAD_MUTEX_INITIALIZER garante isso
// sem depender de ninguem chamar nada primeiro.
static pthread_mutex_t openLock = PTHREAD_MUTEX_INITIALIZER;

static int openHandle(void) {
  void *h;
  int r;
  // Leitura rapida sem trava para o caso comum (ja carregado). Escrita de int
  // e atomica nas arquiteturas em que este app roda; o que precisa de trava e a
  // SEQUENCIA dlopen+global_init, nao a bandeira.
  if (ready) return ready > 0;
  pthread_mutex_lock(&openLock);
  if (ready) { r = ready > 0; pthread_mutex_unlock(&openLock); return r; }
  ready = -1;
  h = dlopen("libcurl.so.5", RTLD_NOW);
  if (!h) h = dlopen("libcurl.so.4", RTLD_NOW);
  if (!h) h = dlopen("libcurl.4.dylib", RTLD_NOW);   // Mac
  if (!h) h = dlopen("libcurl.dylib", RTLD_NOW);
  if (!h) { printf("[net] no libcurl: %s\n", dlerror());
            pthread_mutex_unlock(&openLock); return 0; }
  *(void **)(&curl_init)    = dlsym(h, "curl_easy_init");
  *(void **)(&curl_setopt)  = dlsym(h, "curl_easy_setopt");
  *(void **)(&curl_perform) = dlsym(h, "curl_easy_perform");
  *(void **)(&curl_cleanup) = dlsym(h, "curl_easy_cleanup");
  *(void **)(&curl_reset)   = dlsym(h, "curl_easy_reset");
  *(void **)(&curl_global)  = dlsym(h, "curl_global_init");
  *(void **)(&slist_append) = dlsym(h, "curl_slist_append");
  *(void **)(&slist_free)   = dlsym(h, "curl_slist_free_all");
  *(void **)(&curl_getinfo) = dlsym(h, "curl_easy_getinfo");
  if (!curl_init || !curl_setopt || !curl_perform) {
    printf("[net] libcurl is missing the expected symbols\n");
    pthread_mutex_unlock(&openLock);
    return 0;
  }
  if (curl_global) curl_global(3 /* CURL_GLOBAL_DEFAULT */);
  // The per-thread handle depends on curl_easy_reset. Without it there is no
  // way to return the handle to a clean state between one request and the next,
  // and reusing it would carry the previous request's options along — a Trakt
  // header on an artwork request, one request's Range into the following one.
  // In that case the module goes back to what it did before: a handle per
  // request. Slower, and correct.
  if (curl_reset && curl_cleanup &&
      pthread_key_create(&handleKey, handleGone) == 0)
    handleKeyReady = 1;
  else
    printf("[net] no curl_easy_reset: one handle per request\n");
  ready = 1;
  pthread_mutex_unlock(&openLock);
  return 1;
}

void net_prepare(void) { openHandle(); }

char *net_download_bin(const char *url, int seconds, long *size) {
  return net_download_internal(url, seconds, size, NULL, 0);
}

char *net_download(const char *url, int seconds) {
  return net_download_internal(url, seconds, NULL, NULL, 0);
}

char *net_download_chunk(const char *url, int seconds, long start, long end,
                         long *size) {
  char track[80];
  const char *header[2];
  // Range e um cabecalho comum, entao o caminho com cabecalhos ja existente
  // serve. Nao ha modo "binario com cabecalhos" separado porque
  // rede_baixar_interno ja devolve o tamanho quando `tam` e passado — quem
  // pediu texto e que ignora esse campo.
  snprintf(track, sizeof track, "Range: bytes=%ld-%ld", start, end);
  header[0] = track; header[1] = NULL;
  // TETO DE VERDADE, e nao so o cabecalho. MEDIDO: um servidor que ignora o
  // Range responde 200 com o arquivo INTEIRO — no teste vieram 31 MB para um
  // pedido de 2 MB. Sem o teto, ler o cabecalho de um filme de 20 GB baixaria
  // o filme. O corte e no recebedor, entao a conexao morre no limite em vez de
  // esperar o fim.
  return net_download_internal(url, seconds, size, header, end - start + 1);
}

char *net_download_com(const char *url, int seconds, const char *const *header) {
  return net_download_internal(url, seconds, NULL, header, 0);
}

char *net_download_st(const char *url, int seconds, const char *const *header,
                     int *status) {
  return net_download_internal2(url, seconds, NULL, header, status, 0);
}

static char *net_download_internal(const char *url, int seconds, long *size,
                                 const char *const *header, long cap) {
  return net_download_internal2(url, seconds, size, header, NULL, cap);
}

static char *net_download_internal2(const char *url, int seconds, long *size,
                                  const char *const *header, int *status,
                                  long cap) {
  Bucket b = { NULL, 0, 0 };
  void *c, *list = NULL;
  int r, own;
  if (status) *status = 0;
  if (!url || !*url || !openHandle()) return NULL;
  b.cap = cap;
  c = handleTake(&own);
  if (!c) return NULL;
  curl_setopt(c, OPT_URL, url);
  curl_setopt(c, OPT_WRITEFUNCTION, receive);
  curl_setopt(c, OPT_WRITEDATA, &b);
  curl_setopt(c, OPT_FOLLOWLOCATION, (long)1);
  curl_setopt(c, OPT_TIMEOUT, (long)(seconds > 0 ? seconds : 30));
  curl_setopt(c, OPT_CONNECTTIMEOUT, CONNECT_SECONDS);
  // O app roda com fios; sem NOSIGNAL a libcurl usa alarmes para o timeout de
  // DNS e pode derrubar o processo inteiro a partir de um fio secundario.
  curl_setopt(c, OPT_NOSIGNAL, (long)1);
  // Os addons sao servidos por hosts com cadeias que este aparelho de 2019 nao
  // conhece; o pacote de CAs dele e de fabrica e nao se atualiza. Verificar
  // recusaria fontes legitimas do dono. O conteudo e midia publica e a escolha
  // esta escrita aqui de proposito.
  curl_setopt(c, OPT_SSL_VERIFYPEER, (long)0);
  curl_setopt(c, OPT_SSL_VERIFYHOST, (long)0);
  curl_setopt(c, OPT_USERAGENT, "Nuvio/1.0 (webOS)");
  curl_setopt(c, OPT_ACCEPT_ENCODING, "");   // "" = todas as que a lib suporta
  if (header && slist_append) {
    int k;
    for (k = 0; header[k]; k++) list = slist_append(list, header[k]);
    if (list) curl_setopt(c, OPT_HTTPHEADER, list);
  }
  r = curl_perform(c);
  // STATUS HTTP, e nao so o codigo de erro da libcurl. MEDIDO: numa navegacao
  // da home o log tinha 93 "decode falhou" e ZERO "[rede] falha" — ou seja, o
  // curl_easy_perform devolvia 0 (sucesso de TRANSPORTE) para respostas que nao
  // eram a imagem. Um 404, um 403 ou um 429 e uma transferencia bem-sucedida
  // para a libcurl; quem tem de olhar o status e quem chama.
  //
  // Sem isto o erro chegava sem nome ao tex_cache, que so via "corpo curto" e
  // devolvia 0 em silencio — e o unico sintoma era card sem arte. A assinatura
  // de imagem que ja existe la pega o 404 com pagina de erro GRANDE; esta
  // conferencia pega o resto, e diz QUAL foi o codigo.
  //
  // A EXCECAO e quem pediu `status`: para o Supabase, um 4xx nao e falha, e a
  // resposta. O corpo do 404 diz QUAL funcao ou tabela nao existe (PGRST202 /
  // PGRST205), e e essa string que distingue "servidor antigo" de "parametro
  // errado". Jogar o corpo fora aqui apagaria a unica pista.
  { long http = 0;
    if (curl_getinfo) curl_getinfo(c, INFO_RESPONSE_CODE, &http);
    if (status) *status = (int)http;
    if (!r && http >= 400 && !status) {
      handleGive(c, own, list);
      free(b.p);
      printf("[net] HTTP %ld on %.60s\n", http, url);
      fflush(stdout);
      return NULL;
    } }
  handleGive(c, own, list);
  // 23 = CURLE_WRITE_ERROR. Quando ha teto, ele e o resultado ESPERADO: o
  // recebedor devolve menos bytes de proposito para cortar a conexao assim que
  // enche. Nesse caso o que ja veio e exatamente o que se queria — tratar como
  // falha jogaria fora o cabecalho inteiro que acabamos de baixar.
  if (r == 23 && b.cap > 0 && b.n > 0) r = 0;
  if (r != 0) { free(b.p); printf("[net] failure %d on %.60s\n", r, url); return NULL; }
  if (size) *size = (long)b.n;
  return b.p;
}

int net_url_final(const char *url, int seconds, char *dst, unsigned size) {
  Bucket b = { NULL, 0, 0 };
  void *c;
  char *end = NULL;
  int r, own;
  if (!url || !*url || !openHandle() || !curl_getinfo) return 0;
  c = handleTake(&own);
  if (!c) return 0;
  curl_setopt(c, OPT_URL, url);
  curl_setopt(c, OPT_WRITEFUNCTION, receive);
  curl_setopt(c, OPT_WRITEDATA, &b);
  curl_setopt(c, OPT_FOLLOWLOCATION, (long)1);
  curl_setopt(c, OPT_TIMEOUT, (long)(seconds > 0 ? seconds : 20));
  curl_setopt(c, OPT_CONNECTTIMEOUT, CONNECT_SECONDS);
  curl_setopt(c, OPT_NOSIGNAL, (long)1);
  curl_setopt(c, OPT_SSL_VERIFYPEER, (long)0);
  curl_setopt(c, OPT_SSL_VERIFYHOST, (long)0);
  curl_setopt(c, OPT_USERAGENT, "Nuvio/1.0 (webOS)");
  // Um pedaco minusculo em vez de HEAD: varios servidores de debrid respondem
  // HEAD com 405 ou mentem no redirecionamento, mas honram Range.
  curl_setopt(c, OPT_RANGE, "0-64");
  r = curl_perform(c);
  if (!r) curl_getinfo(c, INFO_URL_FINAL, &end);
  // The address comes out of the handle, so it has to be COPIED before the
  // handle is given back: on a reused handle the next curl_easy_reset frees it.
  if (!r && end) snprintf(dst, size, "%s", end);
  handleGive(c, own, NULL);
  free(b.p);
  return (!r && end) ? 1 : 0;
}

char *net_post(const char *url, int seconds, const char *const *header,
                  const char *body) {
  return net_post_st(url, seconds, header, body, NULL);
}

char *net_post_st(const char *url, int seconds, const char *const *header,
                     const char *body, int *status) {
  Bucket b = { NULL, 0, 0 };
  void *c, *list = NULL;
  int r, own;
  if (status) *status = 0;
  if (!url || !*url || !openHandle()) return NULL;
  c = handleTake(&own);
  if (!c) return NULL;
  curl_setopt(c, OPT_URL, url);
  curl_setopt(c, OPT_WRITEFUNCTION, receive);
  curl_setopt(c, OPT_WRITEDATA, &b);
  curl_setopt(c, OPT_TIMEOUT, (long)(seconds > 0 ? seconds : 20));
  curl_setopt(c, OPT_CONNECTTIMEOUT, CONNECT_SECONDS);
  curl_setopt(c, OPT_NOSIGNAL, (long)1);
  curl_setopt(c, OPT_SSL_VERIFYPEER, (long)0);
  curl_setopt(c, OPT_SSL_VERIFYHOST, (long)0);
  curl_setopt(c, OPT_USERAGENT, "Nuvio/1.0 (webOS)");
  curl_setopt(c, OPT_POST, (long)1);
  curl_setopt(c, OPT_POSTFIELDS, body ? body : "");
  if (slist_append) {
    int k;
    list = slist_append(list, "Content-Type: application/json");
    for (k = 0; header && header[k]; k++) list = slist_append(list, header[k]);
    if (list) curl_setopt(c, OPT_HTTPHEADER, list);
  }
  r = curl_perform(c);
  // O codigo sai ANTES do cleanup: depois dele a alca nao existe mais.
  if (status && !r && curl_getinfo) {
    long code = 0;
    curl_getinfo(c, INFO_RESPONSE_CODE, &code);
    *status = (int)code;
  }
  handleGive(c, own, list);
  // Falha de TRANSPORTE (r != 0) continua sendo NULL — ai nao houve resposta
  // nenhuma. O corpo de um 4xx, ao contrario, e devolvido: e nele que o
  // PostgREST explica o que faltou.
  if (r != 0) { free(b.p); return NULL; }
  return b.p ? b.p : strdup("");
}
