#include "mark.h"
#include "data.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

// WHERE THE FILE GOES. /tmp on the Mac; on the TV, the data folder, for the same
// reason as main.c's devPath: since webOS 11 the app has a private /tmp that the
// Developer Mode ssh user cannot read, and a timeline nobody can read measures
// nothing.
//
// The clock starts BEFORE data_start has chosen that folder, so the lines are
// kept in memory and the whole file is rewritten on every mark. It is a few dozen
// lines per session; the rewrite costs nothing that matters.
static Uint32 t0;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char lines[16384];
static size_t used;

static void flush(void) {
  char path[600];
  FILE *f;
#ifdef __APPLE__
  snprintf(path, sizeof path, "/tmp/nuvio-marks.txt");
#else
  if (!data_path(path, sizeof path, "nuvio-marks.txt")) return;
#endif
  f = fopen(path, "w");
  if (f) { fwrite(lines, 1, used, f); fclose(f); }
}

void mark_start(void) {
  t0 = SDL_GetTicks();
  pthread_mutex_lock(&lock);
  used = (size_t)snprintf(lines, sizeof lines, "ms\tevent\n");
  pthread_mutex_unlock(&lock);
}

void mark(const char *name) {
  Uint32 ms;
  int n;
  if (!name) return;
  // SDL_GetTicks is thread safe; what needs the lock is the buffer and the file,
  // so two threads do not interleave half a line each.
  ms = SDL_GetTicks() - t0;
  pthread_mutex_lock(&lock);
  n = snprintf(lines + used, sizeof lines - used, "%u\t%s\n", (unsigned)ms, name);
  if (n > 0 && used + (size_t)n < sizeof lines) used += (size_t)n;
  flush();
  pthread_mutex_unlock(&lock);
  printf("[t] %u %s\n", (unsigned)ms, name);
  fflush(stdout);
}
