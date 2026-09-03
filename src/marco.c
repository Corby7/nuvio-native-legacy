#include "marco.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <pthread.h>

static Uint32 t0;
static pthread_mutex_t trava = PTHREAD_MUTEX_INITIALIZER;

void marco_iniciar(void) {
  FILE *f;
  t0 = SDL_GetTicks();
  f = fopen("/tmp/nuvio-marcos.txt", "w");
  if (f) { fprintf(f, "ms\tevento\n"); fclose(f); }
}

void marco(const char *nome) {
  FILE *f;
  Uint32 ms;
  if (!nome) return;
  // SDL_GetTicks e seguro entre fios; o que precisa de trava e o arquivo, para
  // dois fios nao intercalarem meia linha cada.
  ms = SDL_GetTicks() - t0;
  pthread_mutex_lock(&trava);
  f = fopen("/tmp/nuvio-marcos.txt", "a");
  if (f) { fprintf(f, "%u\t%s\n", (unsigned)ms, nome); fclose(f); }
  pthread_mutex_unlock(&trava);
  printf("[t] %u %s\n", (unsigned)ms, nome);
  fflush(stdout);
}
