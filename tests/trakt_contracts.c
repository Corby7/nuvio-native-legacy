#include "trakt.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>

extern int trakt_operation_state(int kind);
extern int trakt_watchlist_kind(const char *imdb, const char *kind, int add);
extern int trakt_watched_kind(const char *imdb, const char *kind, int mark);

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static char ultimaUrl[160], lastBody[320];
static int calls, nextStatus;

char *net_post_st(const char *url, int seconds, const char *const *header,
                     const char *body, int *status) {
  (void)seconds; (void)header;
  pthread_mutex_lock(&lock);
  snprintf(ultimaUrl, sizeof ultimaUrl, "%s", url);
  snprintf(lastBody, sizeof lastBody, "%s", body);
  calls++;
  if (status) *status = nextStatus;
  pthread_cond_broadcast(&cond);
  pthread_mutex_unlock(&lock);
  return strdup("");
}

int cat_index_por_imdb(const char *imdb) { return !strcmp(imdb, "tt1234567") ? 0 : -1; }
void cat_set_na_list(int index_, int naList) { (void)index_; (void)naList; }
const char *cat_kind_por_imdb(const char *imdb) { (void)imdb; return "movie"; }
void cat_history_set_id(const char *imdb, const char *kind, int watched) {
  (void)imdb; (void)kind; (void)watched;
}

static void waits(int expected) {
  pthread_mutex_lock(&lock);
  while (calls < expected) pthread_cond_wait(&cond, &lock);
  pthread_mutex_unlock(&lock);
}

int main(void) {
  trakt_set("token", "client");

  nextStatus = 400;
  assert(trakt_watchlist_kind("tt1234567:2:4", "series", 1));
  waits(1);
  while (trakt_operation_state(1) == 1) {}
  assert(trakt_operation_state(1) == 3);
  assert(strstr(ultimaUrl, "/sync/watchlist"));
  assert(strstr(lastBody, "shows") && !strstr(lastBody, "movies"));

  nextStatus = 204;
  while (!trakt_watchlist_kind("tt1234567", "series", 1)) sched_yield();
  waits(2);
  while (trakt_operation_state(1) == 1) {}
  assert(trakt_operation_state(1) == 2);

  nextStatus = 201;
  while (!trakt_watched_kind("tt7654321", "movie", 1)) sched_yield();
  waits(3);
  while (trakt_operation_state(2) == 1) {}
  assert(trakt_operation_state(2) == 2);
  assert(strstr(ultimaUrl, "/sync/history"));
  assert(strstr(lastBody, "movies") && !strstr(lastBody, "shows"));
  puts("trakt contracts: PASS (intent, scope, failure and retry confirmed)");
  return 0;
}
