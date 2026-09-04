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
// CURLINFO_RESPONSE_CODE = CURLINFO_LONG (0x200000) + 2.
#define INFO_RESPONSE_CODE   2097154

static void *(*curl_init)(void);
static int   (*curl_setopt)(void *, int, ...);
static int   (*curl_perform)(void *);
static void  (*curl_cleanup)(void *);
static int   (*curl_global)(long);
static void *(*slist_append)(void *, const char *);
static void  (*slist_free)(void *);
static int   (*curl_getinfo)(void *, int, ...);
static int    ready;

typedef struct { char *p; size_t n; } Bucket;

static char *net_download_internal(const char *url, int seconds, long *size,
                                 const char *const *header);
static char *net_download_internal2(const char *url, int seconds, long *size,
                                  const char *const *header, int *status);

// Teto opcional de bytes para a proxima transferencia; 0 = sem teto. Existe
// porque servidor que IGNORA o cabecalho Range responde 200 com o arquivo
// inteiro, e nesse caso o cabecalho pedido nao limita nada.
long net_cap = 0;

static size_t receive(void *data, size_t size, size_t count, void *u) {
  Bucket *b = (Bucket *)u;
  size_t bytes = size * count;
  char *new;
  if (net_cap > 0 && b->n >= (size_t)net_cap) return 0;   // corta a conexao
  if (net_cap > 0 && b->n + bytes > (size_t)net_cap)
    bytes = (size_t)net_cap - b->n;
  new = realloc(b->p, b->n + bytes + 1);
  if (!new) return 0;              // devolver 0 aborta a transferencia
  b->p = new;
  memcpy(b->p + b->n, data, bytes);
  b->n += bytes;
  b->p[b->n] = 0;
  return bytes;
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
  ready = 1;
  pthread_mutex_unlock(&openLock);
  return 1;
}

void net_prepare(void) { openHandle(); }

char *net_download_bin(const char *url, int seconds, long *size) {
  return net_download_internal(url, seconds, size, NULL);
}

char *net_download(const char *url, int seconds) {
  return net_download_internal(url, seconds, NULL, NULL);
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
  net_cap = end - start + 1;
  { char *r = net_download_internal(url, seconds, size, header);
    net_cap = 0;
    return r; }
}

char *net_download_com(const char *url, int seconds, const char *const *header) {
  return net_download_internal(url, seconds, NULL, header);
}

char *net_download_st(const char *url, int seconds, const char *const *header,
                     int *status) {
  return net_download_internal2(url, seconds, NULL, header, status);
}

static char *net_download_internal(const char *url, int seconds, long *size,
                                 const char *const *header) {
  return net_download_internal2(url, seconds, size, header, NULL);
}

static char *net_download_internal2(const char *url, int seconds, long *size,
                                  const char *const *header, int *status) {
  Bucket b = { NULL, 0 };
  void *c, *list = NULL;
  int r;
  if (status) *status = 0;
  if (!url || !*url || !openHandle()) return NULL;
  c = curl_init();
  if (!c) return NULL;
  curl_setopt(c, OPT_URL, url);
  curl_setopt(c, OPT_WRITEFUNCTION, receive);
  curl_setopt(c, OPT_WRITEDATA, &b);
  curl_setopt(c, OPT_FOLLOWLOCATION, (long)1);
  curl_setopt(c, OPT_TIMEOUT, (long)(seconds > 0 ? seconds : 30));
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
      curl_cleanup(c);
      if (list && slist_free) slist_free(list);
      free(b.p);
      printf("[net] HTTP %ld on %.60s\n", http, url);
      fflush(stdout);
      return NULL;
    } }
  curl_cleanup(c);
  if (list && slist_free) slist_free(list);
  // 23 = CURLE_WRITE_ERROR. Quando ha teto, ele e o resultado ESPERADO: o
  // recebedor devolve menos bytes de proposito para cortar a conexao assim que
  // enche. Nesse caso o que ja veio e exatamente o que se queria — tratar como
  // falha jogaria fora o cabecalho inteiro que acabamos de baixar.
  if (r == 23 && net_cap > 0 && b.n > 0) r = 0;
  if (r != 0) { free(b.p); printf("[net] failure %d on %.60s\n", r, url); return NULL; }
  if (size) *size = (long)b.n;
  return b.p;
}

int net_url_final(const char *url, int seconds, char *dst, unsigned size) {
  Bucket b = { NULL, 0 };
  void *c;
  char *end = NULL;
  int r;
  if (!url || !*url || !openHandle() || !curl_getinfo) return 0;
  c = curl_init();
  if (!c) return 0;
  curl_setopt(c, OPT_URL, url);
  curl_setopt(c, OPT_WRITEFUNCTION, receive);
  curl_setopt(c, OPT_WRITEDATA, &b);
  curl_setopt(c, OPT_FOLLOWLOCATION, (long)1);
  curl_setopt(c, OPT_TIMEOUT, (long)(seconds > 0 ? seconds : 20));
  curl_setopt(c, OPT_NOSIGNAL, (long)1);
  curl_setopt(c, OPT_SSL_VERIFYPEER, (long)0);
  curl_setopt(c, OPT_SSL_VERIFYHOST, (long)0);
  curl_setopt(c, OPT_USERAGENT, "Nuvio/1.0 (webOS)");
  // Um pedaco minusculo em vez de HEAD: varios servidores de debrid respondem
  // HEAD com 405 ou mentem no redirecionamento, mas honram Range.
  curl_setopt(c, OPT_RANGE, "0-64");
  r = curl_perform(c);
  if (!r) curl_getinfo(c, INFO_URL_FINAL, &end);
  if (!r && end) snprintf(dst, size, "%s", end);
  curl_cleanup(c);
  free(b.p);
  return (!r && end) ? 1 : 0;
}

char *net_post(const char *url, int seconds, const char *const *header,
                  const char *body) {
  return net_post_st(url, seconds, header, body, NULL);
}

char *net_post_st(const char *url, int seconds, const char *const *header,
                     const char *body, int *status) {
  Bucket b = { NULL, 0 };
  void *c, *list = NULL;
  int r;
  if (status) *status = 0;
  if (!url || !*url || !openHandle()) return NULL;
  c = curl_init();
  if (!c) return NULL;
  curl_setopt(c, OPT_URL, url);
  curl_setopt(c, OPT_WRITEFUNCTION, receive);
  curl_setopt(c, OPT_WRITEDATA, &b);
  curl_setopt(c, OPT_TIMEOUT, (long)(seconds > 0 ? seconds : 20));
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
  curl_cleanup(c);
  if (list && slist_free) slist_free(list);
  // Falha de TRANSPORTE (r != 0) continua sendo NULL — ai nao houve resposta
  // nenhuma. O corpo de um 4xx, ao contrario, e devolvido: e nele que o
  // PostgREST explica o que faltou.
  if (r != 0) { free(b.p); return NULL; }
  return b.p ? b.p : strdup("");
}
