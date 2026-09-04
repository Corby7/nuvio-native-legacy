#include "mark.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <pthread.h>

static Uint32 t0;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

void mark_start(void) {
  FILE *f;
  t0 = SDL_GetTicks();
  f = fopen("/tmp/nuvio-marcos.txt", "w");
  if (f) { fprintf(f, "ms\tevento\n"); fclose(f); }
}

void mark(const char *name) {
  FILE *f;
  Uint32 ms;
  if (!name) return;
  // SDL_GetTicks is thread safe; what needs the lock is the file, so two
  // threads do not interleave half a line each.
  ms = SDL_GetTicks() - t0;
  pthread_mutex_lock(&lock);
  f = fopen("/tmp/nuvio-marcos.txt", "a");
  if (f) { fprintf(f, "%u\t%s\n", (unsigned)ms, name); fclose(f); }
  pthread_mutex_unlock(&lock);
  printf("[t] %u %s\n", (unsigned)ms, name);
  fflush(stdout);
}
