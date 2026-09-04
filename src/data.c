#include "data.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

static char dir[512];
static char clientId[64];

// Tries to create the folder and write in it. Creating is not enough: at
// several points in the device's filesystem the mkdir succeeds and the open
// fails afterwards, and a test that only looks at the mkdir would choose a
// folder where nothing gets written.
static int serve(const char *candidate) {
  char teste[600];
  FILE *f;
  if (!candidate || !*candidate) return 0;
  mkdir(candidate, 0755);   // ja existir nao e erro para o que interessa aqui
  snprintf(teste, sizeof teste, "%s/.write-test", candidate);
  f = fopen(teste, "w");
  if (!f) return 0;
  if (fputs("ok\n", f) < 0) { fclose(f); return 0; }
  if (fclose(f) != 0) return 0;
  remove(teste);
  return 1;
}

static void migrateLegacyNames(void);

void data_start(const char *dirArt) {
  char lar[512];
  const char *env = getenv("NUVIO_DATA");
  const char *home = getenv("HOME");
  const char *candidates[5];
  int n = 0, i;

  if (env && *env) candidates[n++] = env;
  if (home && *home) {
    snprintf(lar, sizeof lar, "%s/.nuvio", home);
    candidates[n++] = lar;
  }
  // The working folder of webOS developer mode. It exists and is writable on
  // the devices this app runs on today; on a shop-bought device it may not
  // exist, which is why it is a candidate and not an answer.
  candidates[n++] = "/media/developer/temp/nuvio";
  if (dirArt && *dirArt) candidates[n++] = dirArt;

  for (i = 0; i < n; i++) {
    if (serve(candidates[i])) {
      snprintf(dir, sizeof dir, "%s", candidates[i]);
      printf("[data] writing to %s\n", dir);
      fflush(stdout);
      migrateLegacyNames();
      return;
    }
    printf("[data] rejected %s\n", candidates[i]);
  }
  dir[0] = 0;
  printf("[data] NO writable folder: session and settings will not survive "
         "the next start\n");
  fflush(stdout);
}

// The user's files were named in Portuguese until 1.0.1. Renaming them without
// this would silently orphan the session, the active profile and the watch
// progress of everyone who already installed the package: the app would open,
// look signed out, and the old files would sit there unread. Rename on the way
// in, and only when the new name is not already present.
static void migrateLegacyNames(void) {
  static const struct { const char *old, *new; } T[] = {
    { "cliente.txt",   "client.txt"   }, { "sessao.txt",   "session.txt"  },
    { "perfil.txt",    "profile.txt"  }, { "progresso.txt","progress.txt" },
    { "ajustes.txt",   "settings.txt" },
  };
  char from[600], to[600];
  size_t i;
  if (!dir[0]) return;
  for (i = 0; i < sizeof T / sizeof *T; i++) {
    if (!data_path(from, sizeof from, T[i].old)) continue;
    if (!data_path(to, sizeof to, T[i].new)) continue;
    if (access(from, F_OK) != 0 || access(to, F_OK) == 0) continue;
    if (rename(from, to) == 0) {
      printf("[data] migrated %s -> %s\n", T[i].old, T[i].new);
      fflush(stdout);
    }
  }
}

const char *data_dir(void) { return dir; }

char *data_path(char *dst, unsigned size, const char *name) {
  if (!dir[0] || !name || !*name) return NULL;
  snprintf(dst, size, "%s/%s", dir, name);
  return dst;
}

int data_write(const char *name, const char *content) {
  char path[600], tmp[600];
  FILE *f;
  size_t n;
  if (!data_path(path, sizeof path, name)) return 0;
  snprintf(tmp, sizeof tmp, "%s.tmp", path);
  f = fopen(tmp, "w");
  if (!f) return 0;
  n = content ? strlen(content) : 0;
  if (n && fwrite(content, 1, n, f) != n) { fclose(f); remove(tmp); return 0; }
  if (fclose(f) != 0) { remove(tmp); return 0; }
  if (rename(tmp, path) != 0) { remove(tmp); return 0; }
  return 1;
}

char *data_read(const char *name) {
  char path[600];
  FILE *f;
  long n;
  char *buf;
  if (!data_path(path, sizeof path, name)) return NULL;
  f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n < 0) { fclose(f); return NULL; }
  buf = (char *)malloc((size_t)n + 1);
  if (!buf) { fclose(f); return NULL; }
  n = (long)fread(buf, 1, (size_t)n, f);
  fclose(f);
  buf[n] = 0;
  return buf;
}

int data_erase(const char *name) {
  char path[600];
  if (!data_path(path, sizeof path, name)) return 0;
  return remove(path) == 0;
}

void data_uuid(char *dst, unsigned size) {
  static const char *hex = "0123456789abcdef";
  static int seeded;
  int i;
  if (size < 37) { if (size) dst[0] = 0; return; }
  if (!seeded) {
    srand((unsigned)time(NULL) ^ (unsigned)getpid() ^ (unsigned)(size_t)dst);
    seeded = 1;
  }
  for (i = 0; i < 36; i++) {
    if (i == 8 || i == 13 || i == 18 || i == 23) { dst[i] = '-'; continue; }
    if (i == 14) { dst[i] = '4'; continue; }              // versao
    if (i == 19) { dst[i] = hex[8 + (rand() & 3)]; continue; }  // variante
    dst[i] = hex[rand() & 15];
  }
  dst[36] = 0;
}

const char *data_client_id(void) {
  char *lido;
  if (clientId[0]) return clientId;

  lido = data_read("client.txt");
  if (lido) {
    char *end = lido + strlen(lido);
    while (end > lido && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ')) *--end = 0;
    if (lido[0]) snprintf(clientId, sizeof clientId, "%s", lido);
    free(lido);
    if (clientId[0]) return clientId;
  }

  // UUID v4 format because that is what the server receives from Android and
  // from the web; the randomness does not need to be cryptographic — this number
  // identifies a device so it does not echo its own write, it protects
  // nothing.
  data_uuid(clientId, sizeof clientId);

  { char line[64];
    snprintf(line, sizeof line, "%s\n", clientId);
    data_write("client.txt", line); }
  return clientId;
}
