#include "rede.h"
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

static void *(*curl_init)(void);
static int   (*curl_setopt)(void *, int, ...);
static int   (*curl_perform)(void *);
static void  (*curl_cleanup)(void *);
static int   (*curl_global)(long);
static void *(*slist_append)(void *, const char *);
static void  (*slist_free)(void *);
static int   (*curl_getinfo)(void *, int, ...);
static int    pronto;

typedef struct { char *p; size_t n; } Balde;

static char *rede_baixar_interno(const char *url, int segundos, long *tam,
                                 const char *const *cab);

static size_t receber(void *dados, size_t tam, size_t qtd, void *u) {
  Balde *b = (Balde *)u;
  size_t bytes = tam * qtd;
  char *novo = realloc(b->p, b->n + bytes + 1);
  if (!novo) return 0;              // devolver 0 aborta a transferencia
  b->p = novo;
  memcpy(b->p + b->n, dados, bytes);
  b->n += bytes;
  b->p[b->n] = 0;
  return bytes;
}

static int abrir(void) {
  void *h;
  if (pronto) return pronto > 0;
  pronto = -1;
  h = dlopen("libcurl.so.5", RTLD_NOW);
  if (!h) h = dlopen("libcurl.so.4", RTLD_NOW);
  if (!h) h = dlopen("libcurl.4.dylib", RTLD_NOW);   // Mac
  if (!h) h = dlopen("libcurl.dylib", RTLD_NOW);
  if (!h) { printf("[rede] sem libcurl: %s\n", dlerror()); return 0; }
  *(void **)(&curl_init)    = dlsym(h, "curl_easy_init");
  *(void **)(&curl_setopt)  = dlsym(h, "curl_easy_setopt");
  *(void **)(&curl_perform) = dlsym(h, "curl_easy_perform");
  *(void **)(&curl_cleanup) = dlsym(h, "curl_easy_cleanup");
  *(void **)(&curl_global)  = dlsym(h, "curl_global_init");
  *(void **)(&slist_append) = dlsym(h, "curl_slist_append");
  *(void **)(&slist_free)   = dlsym(h, "curl_slist_free_all");
  *(void **)(&curl_getinfo) = dlsym(h, "curl_easy_getinfo");
  if (!curl_init || !curl_setopt || !curl_perform) {
    printf("[rede] libcurl sem os simbolos esperados\n");
    return 0;
  }
  if (curl_global) curl_global(3 /* CURL_GLOBAL_DEFAULT */);
  pronto = 1;
  return 1;
}

char *rede_baixar_bin(const char *url, int segundos, long *tam) {
  return rede_baixar_interno(url, segundos, tam, NULL);
}

char *rede_baixar(const char *url, int segundos) {
  return rede_baixar_interno(url, segundos, NULL, NULL);
}

char *rede_baixar_com(const char *url, int segundos, const char *const *cab) {
  return rede_baixar_interno(url, segundos, NULL, cab);
}

static char *rede_baixar_interno(const char *url, int segundos, long *tam,
                                 const char *const *cab) {
  Balde b = { NULL, 0 };
  void *c, *lista = NULL;
  int r;
  if (!url || !*url || !abrir()) return NULL;
  c = curl_init();
  if (!c) return NULL;
  curl_setopt(c, OPT_URL, url);
  curl_setopt(c, OPT_WRITEFUNCTION, receber);
  curl_setopt(c, OPT_WRITEDATA, &b);
  curl_setopt(c, OPT_FOLLOWLOCATION, (long)1);
  curl_setopt(c, OPT_TIMEOUT, (long)(segundos > 0 ? segundos : 30));
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
  if (cab && slist_append) {
    int k;
    for (k = 0; cab[k]; k++) lista = slist_append(lista, cab[k]);
    if (lista) curl_setopt(c, OPT_HTTPHEADER, lista);
  }
  r = curl_perform(c);
  curl_cleanup(c);
  if (lista && slist_free) slist_free(lista);
  if (r != 0) { free(b.p); printf("[rede] falha %d em %.60s\n", r, url); return NULL; }
  if (tam) *tam = (long)b.n;
  return b.p;
}

int rede_url_final(const char *url, int segundos, char *dst, unsigned tam) {
  Balde b = { NULL, 0 };
  void *c;
  char *fim = NULL;
  int r;
  if (!url || !*url || !abrir() || !curl_getinfo) return 0;
  c = curl_init();
  if (!c) return 0;
  curl_setopt(c, OPT_URL, url);
  curl_setopt(c, OPT_WRITEFUNCTION, receber);
  curl_setopt(c, OPT_WRITEDATA, &b);
  curl_setopt(c, OPT_FOLLOWLOCATION, (long)1);
  curl_setopt(c, OPT_TIMEOUT, (long)(segundos > 0 ? segundos : 20));
  curl_setopt(c, OPT_NOSIGNAL, (long)1);
  curl_setopt(c, OPT_SSL_VERIFYPEER, (long)0);
  curl_setopt(c, OPT_SSL_VERIFYHOST, (long)0);
  curl_setopt(c, OPT_USERAGENT, "Nuvio/1.0 (webOS)");
  // Um pedaco minusculo em vez de HEAD: varios servidores de debrid respondem
  // HEAD com 405 ou mentem no redirecionamento, mas honram Range.
  curl_setopt(c, OPT_RANGE, "0-64");
  r = curl_perform(c);
  if (!r) curl_getinfo(c, INFO_URL_FINAL, &fim);
  if (!r && fim) snprintf(dst, tam, "%s", fim);
  curl_cleanup(c);
  free(b.p);
  return (!r && fim) ? 1 : 0;
}

char *rede_postar(const char *url, int segundos, const char *const *cab,
                  const char *corpo) {
  Balde b = { NULL, 0 };
  void *c, *lista = NULL;
  int r;
  if (!url || !*url || !abrir()) return NULL;
  c = curl_init();
  if (!c) return NULL;
  curl_setopt(c, OPT_URL, url);
  curl_setopt(c, OPT_WRITEFUNCTION, receber);
  curl_setopt(c, OPT_WRITEDATA, &b);
  curl_setopt(c, OPT_TIMEOUT, (long)(segundos > 0 ? segundos : 20));
  curl_setopt(c, OPT_NOSIGNAL, (long)1);
  curl_setopt(c, OPT_SSL_VERIFYPEER, (long)0);
  curl_setopt(c, OPT_SSL_VERIFYHOST, (long)0);
  curl_setopt(c, OPT_USERAGENT, "Nuvio/1.0 (webOS)");
  curl_setopt(c, OPT_POST, (long)1);
  curl_setopt(c, OPT_POSTFIELDS, corpo ? corpo : "");
  if (slist_append) {
    int k;
    lista = slist_append(lista, "Content-Type: application/json");
    for (k = 0; cab && cab[k]; k++) lista = slist_append(lista, cab[k]);
    if (lista) curl_setopt(c, OPT_HTTPHEADER, lista);
  }
  r = curl_perform(c);
  curl_cleanup(c);
  if (lista && slist_free) slist_free(lista);
  if (r != 0) { free(b.p); return NULL; }
  return b.p ? b.p : strdup("");
}
