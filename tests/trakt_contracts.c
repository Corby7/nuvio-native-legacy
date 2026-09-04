#include "trakt.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>

extern int trakt_operacao_estado(int tipo);
extern int trakt_watchlist_tipo(const char *imdb, const char *tipo, int adicionar);
extern int trakt_assistido_tipo(const char *imdb, const char *tipo, int marcar);

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static char ultimaUrl[160], ultimoCorpo[320];
static int chamadas, proximoStatus;

char *rede_postar_st(const char *url, int segundos, const char *const *cab,
                     const char *corpo, int *status) {
  (void)segundos; (void)cab;
  pthread_mutex_lock(&lock);
  snprintf(ultimaUrl, sizeof ultimaUrl, "%s", url);
  snprintf(ultimoCorpo, sizeof ultimoCorpo, "%s", corpo);
  chamadas++;
  if (status) *status = proximoStatus;
  pthread_cond_broadcast(&cond);
  pthread_mutex_unlock(&lock);
  return strdup("");
}

int cat_indice_por_imdb(const char *imdb) { return !strcmp(imdb, "tt1234567") ? 0 : -1; }
void cat_definir_na_lista(int indice, int naLista) { (void)indice; (void)naLista; }
const char *cat_tipo_por_imdb(const char *imdb) { (void)imdb; return "movie"; }
void cat_historico_definir_id(const char *imdb, const char *tipo, int visto) {
  (void)imdb; (void)tipo; (void)visto;
}

static void espera(int esperado) {
  pthread_mutex_lock(&lock);
  while (chamadas < esperado) pthread_cond_wait(&cond, &lock);
  pthread_mutex_unlock(&lock);
}

int main(void) {
  trakt_definir("token", "client");

  proximoStatus = 400;
  assert(trakt_watchlist_tipo("tt1234567:2:4", "series", 1));
  espera(1);
  while (trakt_operacao_estado(1) == 1) {}
  assert(trakt_operacao_estado(1) == 3);
  assert(strstr(ultimaUrl, "/sync/watchlist"));
  assert(strstr(ultimoCorpo, "shows") && !strstr(ultimoCorpo, "movies"));

  proximoStatus = 204;
  while (!trakt_watchlist_tipo("tt1234567", "series", 1)) sched_yield();
  espera(2);
  while (trakt_operacao_estado(1) == 1) {}
  assert(trakt_operacao_estado(1) == 2);

  proximoStatus = 201;
  while (!trakt_assistido_tipo("tt7654321", "movie", 1)) sched_yield();
  espera(3);
  while (trakt_operacao_estado(2) == 1) {}
  assert(trakt_operacao_estado(2) == 2);
  assert(strstr(ultimaUrl, "/sync/history"));
  assert(strstr(ultimoCorpo, "movies") && !strstr(ultimoCorpo, "shows"));
  puts("trakt contracts: PASS (intenção, escopo, falha e retry confirmados)");
  return 0;
}
