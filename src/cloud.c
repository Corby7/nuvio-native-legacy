#include "cloud.h"
#include "net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef NV_SUPABASE_URL
#define NV_SUPABASE_URL ""
#endif
#ifndef NV_SUPABASE_ANON_KEY
#define NV_SUPABASE_ANON_KEY ""
#endif
#ifndef NV_TV_LOGIN_BASE
#define NV_TV_LOGIN_BASE ""
#endif
#ifndef NV_TRAKT_CLIENT_ID
#define NV_TRAKT_CLIENT_ID ""
#endif
#ifndef NV_TRAKT_CLIENT_SECRET
#define NV_TRAKT_CLIENT_SECRET ""
#endif
#ifndef NV_SIMKL_CLIENT_ID
#define NV_SIMKL_CLIENT_ID ""
#endif
#ifndef NV_SIMKL_APP
#define NV_SIMKL_APP "nuvio"
#endif

static char url[300]      = NV_SUPABASE_URL;
static char anon[1200]    = NV_SUPABASE_ANON_KEY;
static char baseLogin[300] = NV_TV_LOGIN_BASE;
static char traktClient[128] = NV_TRAKT_CLIENT_ID;
static char traktSecret[128] = NV_TRAKT_CLIENT_SECRET;
static char simklClient[200] = NV_SIMKL_CLIENT_ID;
static char simklApp[80] = NV_SIMKL_APP;

// The brake: the instant (in seconds) until which nobody tries, and how many
// consecutive failures have happened. The interval doubles on each failure up to
// the cap — the same design as the web's syncBackoffPolicy.
#define BRAKE_MIN   5
#define BRAKE_MAX 300
static long brakeAte = 0;
static int  failures = 0;

static void stripEnd(char *s) {
  char *f = s + strlen(s);
  while (f > s && (f[-1] == '\n' || f[-1] == '\r' || f[-1] == ' ' || f[-1] == '\t')) *--f = 0;
}

int cloud_configure(const char *dirArt) {
  char path[600], line[1200];
  FILE *f;
  if (dirArt && *dirArt) {
    snprintf(path, sizeof path, "%s/cloud.txt", dirArt);
    f = fopen(path, "r");
    if (f) {
      if (fgets(line, sizeof line, f)) { stripEnd(line); if (line[0]) snprintf(url, sizeof url, "%s", line); }
      if (fgets(line, sizeof line, f)) { stripEnd(line); if (line[0]) snprintf(anon, sizeof anon, "%s", line); }
      if (fgets(line, sizeof line, f)) { stripEnd(line); if (line[0]) snprintf(baseLogin, sizeof baseLogin, "%s", line); }
      fclose(f);
      printf("[cloud] configuration from art/cloud.txt\n");
    }
  }
  // A trailing slash would break every concatenation with the RPC path: the web
  // normalises the base the same way.
  { char *end = url + strlen(url);
    while (end > url && end[-1] == '/') *--end = 0; }
  { char *end = baseLogin + strlen(baseLogin);
    while (end > baseLogin && end[-1] == '/') *--end = 0; }

  if (!url[0] || !anon[0]) {
    // Saying this loudly matters: with no configuration the app is not merely
    // "without sync", it is without LOGIN, and the screen will look broken
    // without explaining why.
    printf("[cloud] NOT CONFIGURED (url=%s key=%s): login and sync are off\n",
           url[0] ? "ok" : "empty", anon[0] ? "ok" : "empty");
    return 0;
  }
  printf("[cloud] %s\n", url);
  return 1;
}

int         cloud_ready(void)     { return url[0] && anon[0]; }
const char *cloud_url(void)        { return url; }
const char *cloud_anon(void)       { return anon; }
const char *cloud_base_login(void) { return baseLogin; }
const char *cloud_trakt_client(void) { return traktClient; }
const char *cloud_trakt_secret(void) { return traktSecret; }
const char *cloud_simkl_client(void) { return simklClient; }
const char *cloud_simkl_app(void) { return simklApp; }

char *cloud_post(const char *path, const char *bodyJson,
                 const char *bearer, int *status) {
  char complete[900];
  char headerApi[1300], headerAuth[1300];
  const char *header[3];
  if (status) *status = 0;
  if (!cloud_ready() || !path) return NULL;

  snprintf(complete, sizeof complete, "%s%s%s", url,
           path[0] == '/' ? "" : "/", path);
  snprintf(headerApi, sizeof headerApi, "apikey: %s", anon);
  snprintf(headerAuth, sizeof headerAuth, "Authorization: Bearer %s",
           (bearer && *bearer) ? bearer : anon);
  header[0] = headerApi;
  header[1] = headerAuth;
  header[2] = NULL;
  // Content-Type: application/json is already set by net_post.
  return net_post_st(complete, 25, header, bodyJson ? bodyJson : "{}", status);
}

char *cloud_rpc_com(const char *func, const char *bodyJson,
                    const char *bearer, int *status) {
  char path[300];
  if (!func || !*func) return NULL;
  snprintf(path, sizeof path, "/rest/v1/rpc/%s", func);
  return cloud_post(path, bodyJson, bearer, status);
}

void cloud_url_escape(const char *value, char *dst, unsigned size) {
  static const char *hex = "0123456789ABCDEF";
  unsigned w = 0;
  const unsigned char *p;
  if (!dst || size == 0) return;
  dst[0] = 0;
  for (p = (const unsigned char *)(value ? value : ""); *p; p++) {
    int safe = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                 (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' ||
                 *p == '.' || *p == '~';
    if (safe) {
      if (w + 2 > size) break;
      dst[w++] = (char)*p;
    } else {
      if (w + 4 > size) break;
      dst[w++] = '%';
      dst[w++] = hex[*p >> 4];
      dst[w++] = hex[*p & 15];
    }
  }
  dst[w] = 0;
}

char *cloud_table(const char *table, const char *query,
                   const char *bearer, int *status) {
  char complete[1200], headerApi[1300], headerAuth[1300];
  const char *header[3];
  if (status) *status = 0;
  if (!cloud_ready() || !table) return NULL;
  snprintf(complete, sizeof complete, "%s/rest/v1/%s%s%s", url, table,
           (query && *query) ? "?" : "", (query && *query) ? query : "");
  snprintf(headerApi, sizeof headerApi, "apikey: %s", anon);
  snprintf(headerAuth, sizeof headerAuth, "Authorization: Bearer %s",
           (bearer && *bearer) ? bearer : anon);
  header[0] = headerApi;
  header[1] = headerAuth;
  header[2] = NULL;
  return net_download_st(complete, 25, header, status);
}

int cloud_error_missing(const char *bodyError) {
  if (!bodyError) return 0;
  return strstr(bodyError, "PGRST202") != NULL ||
         strstr(bodyError, "PGRST205") != NULL ||
         strstr(bodyError, "Could not find the function") != NULL ||
         strstr(bodyError, "Could not find the table") != NULL;
}

void cloud_failed(void) {
  int waits;
  if (failures < 30) failures++;
  waits = BRAKE_MIN;
  { int i;
    for (i = 1; i < failures && waits < BRAKE_MAX; i++) waits *= 2; }
  if (waits > BRAKE_MAX) waits = BRAKE_MAX;
  brakeAte = (long)time(NULL) + waits;
  printf("[cloud] backing off for %ds (failure %d)\n", waits, failures);
}

void cloud_ok(void) {
  failures = 0;
  brakeAte = 0;
}

int cloud_brake_active(void) {
  return brakeAte > (long)time(NULL);
}
