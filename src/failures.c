#include "failures.h"
#include "data.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#define FAILURES_FILE  "playback-failures.log"
#define FAILURES_OLD   "playback-failures.old.log"
#define FAILURES_MAX   (256L * 1024L)

void failure_log(const char *line) {
  char path[600], old[600], stamp[32];
  struct stat st;
  time_t now = time(NULL);
  struct tm lt;
  FILE *f;
  if (!line || !data_path(path, sizeof path, FAILURES_FILE)) return;
  if (stat(path, &st) == 0 && st.st_size > FAILURES_MAX &&
      data_path(old, sizeof old, FAILURES_OLD))
    rename(path, old);
  localtime_r(&now, &lt);
  strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &lt);
  f = fopen(path, "a");
  if (!f) return;
  fprintf(f, "%s | %s\n", stamp, line);
  fclose(f);
  printf("[failure] %s\n", line);
  fflush(stdout);
}
