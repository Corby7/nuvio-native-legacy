#include "traktlists.h"
#include "trakt.h"
#include "net.h"
#include "js.h"
#include "data.h"
#include "profiles.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

// --- the added lists --------------------------------------------------------------
//
// One line per list: "<id>\t<items>\t<likes>\t<user>\t<name>". The name goes
// last because it is the only field that can hold anything; a tab inside it is
// turned into a space on the way in.
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static TlList added[TL_MAX];
static int nAdded, loadedProfile = -1;
static volatile unsigned gen;

static void fileName(char *dst, size_t n) {
  snprintf(dst, n, "trakt-lists-p%d.txt", profiles_active());
}

static void loadLocked(void) {
  char name[64], *body, *line, *save = NULL;
  if (loadedProfile == profiles_active()) return;
  loadedProfile = profiles_active();
  nAdded = 0;
  gen++;
  fileName(name, sizeof name);
  body = data_read(name);
  if (!body) return;
  for (line = strtok_r(body, "\n", &save); line && nAdded < TL_MAX;
       line = strtok_r(NULL, "\n", &save)) {
    TlList *l = &added[nAdded];
    char *f[5], *p = line;
    int k;
    for (k = 0; k < 5 && p; k++) {
      f[k] = p;
      p = k < 4 ? strchr(p, '\t') : NULL;
      if (p) *p++ = 0;
    }
    if (k < 5) continue;
    memset(l, 0, sizeof *l);
    l->id = strtol(f[0], NULL, 10);
    l->items = atoi(f[1]);
    l->likes = atoi(f[2]);
    snprintf(l->user, sizeof l->user, "%s", f[3]);
    snprintf(l->name, sizeof l->name, "%s", f[4]);
    if (l->id > 0) nAdded++;
  }
  free(body);
}

static void clean(char *dst, size_t n, const char *s) {
  size_t i;
  snprintf(dst, n, "%s", s);
  for (i = 0; dst[i]; i++) if (dst[i] == '\t' || dst[i] == '\n' || dst[i] == '\r') dst[i] = ' ';
}

static void saveLocked(void) {
  char name[64], text[TL_MAX * 220 + 1], u[64], t[96];
  size_t w = 0;
  int i;
  text[0] = 0;
  for (i = 0; i < nAdded && w < sizeof text; i++) {
    clean(u, sizeof u, added[i].user);
    clean(t, sizeof t, added[i].name);
    w += (size_t)snprintf(text + w, sizeof text - w, "%ld\t%d\t%d\t%s\t%s\n",
                          added[i].id, added[i].items, added[i].likes, u, t);
  }
  fileName(name, sizeof name);
  data_write(name, text);
}

int tl_n(void) {
  int n;
  pthread_mutex_lock(&lock);
  loadLocked();
  n = nAdded;
  pthread_mutex_unlock(&lock);
  return n;
}

int tl_get(int i, TlList *out) {
  int ok;
  pthread_mutex_lock(&lock);
  loadLocked();
  ok = i >= 0 && i < nAdded;
  if (ok) *out = added[i];
  pthread_mutex_unlock(&lock);
  return ok;
}

static int findLocked(long id) {
  int i;
  for (i = 0; i < nAdded; i++) if (added[i].id == id) return i;
  return -1;
}

int tl_has(long id) {
  int v;
  pthread_mutex_lock(&lock);
  loadLocked();
  v = findLocked(id) >= 0;
  pthread_mutex_unlock(&lock);
  return v;
}

int tl_add(const TlList *l) {
  int ok = 1;
  pthread_mutex_lock(&lock);
  loadLocked();
  if (findLocked(l->id) < 0) {
    if (nAdded >= TL_MAX) ok = 0;
    else {
      added[nAdded] = *l;
      added[nAdded].desc[0] = 0;
      nAdded++;
      saveLocked();
      gen++;
    }
  }
  pthread_mutex_unlock(&lock);
  if (ok) { printf("[lists] added %ld \"%s\"\n", l->id, l->name); fflush(stdout); }
  return ok;
}

void tl_remove(long id) {
  int k;
  pthread_mutex_lock(&lock);
  loadLocked();
  k = findLocked(id);
  if (k >= 0) {
    memmove(added + k, added + k + 1, sizeof(TlList) * (size_t)(nAdded - k - 1));
    nAdded--;
    saveLocked();
    gen++;
  }
  pthread_mutex_unlock(&lock);
  if (k >= 0) { printf("[lists] removed %ld\n", id); fflush(stdout); }
}

unsigned tl_generation(void) {
  pthread_mutex_lock(&lock);
  loadLocked();   // a profile switch is a new set too
  pthread_mutex_unlock(&lock);
  return gen;
}

void tl_key(long id, char *dst, size_t n) { snprintf(dst, n, "traktlist_%ld", id); }

long tl_id_of_key(const char *key) {
  if (!key || strncmp(key, "traktlist_", 10)) return 0;
  return strtol(key + 10, NULL, 10);
}

// --- browsing -----------------------------------------------------------------------
//
// The thread fills a fresh array and hands it over under the lock; the main
// thread adopts it in tl_browse_poll. A request number travels with each fetch,
// so a slow search that lands after a newer one was asked for is thrown away.
typedef struct { TlList items[TL_BROWSE_MAX]; int n, failed; unsigned req; char query[96]; } Batch;

static pthread_mutex_t lockBrowse = PTHREAD_MUTEX_INITIALIZER;
static Batch *landed;          // finished, not yet adopted
static unsigned reqLatest;
static int state = TLB_IDLE;
static Batch shown;            // main thread only

static void encode(char *dst, size_t n, const char *s) {
  static const char HEX[] = "0123456789ABCDEF";
  size_t w = 0;
  for (; *s && w + 4 < n; s++) {
    unsigned char c = (unsigned char)*s;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~') dst[w++] = (char)c;
    else { dst[w++] = '%'; dst[w++] = HEX[c >> 4]; dst[w++] = HEX[c & 15]; }
  }
  dst[w] = 0;
}

// Both endpoints wrap each list in an object holding it under "list" — the
// search adds a score, the popular page a like count — so one parse serves both.
static int parseLists(const char *body, TlList *out, int max) {
  const char *p = body ? strchr(body, '[') : NULL;
  int n = 0;
  p = p ? p + 1 : NULL;
  while (p && *p && n < max) {
    const char *f, *l;
    while (*p && (unsigned char)*p <= ' ') p++;
    if (*p != '{') break;
    f = js_end(p);
    l = strstr(p, "\"list\":");
    if (l && l < f && strchr(l, '{') && strchr(l, '{') < f) {
      const char *lo = strchr(l, '{'), *lf = js_end(lo), *u;
      TlList *d = &out[n];
      char privacy[16] = "";
      memset(d, 0, sizeof *d);
      js_text(lo, lf, "name", d->name, sizeof d->name);
      js_text(lo, lf, "description", d->desc, sizeof d->desc);
      js_text(lo, lf, "privacy", privacy, sizeof privacy);
      d->items = (int)js_num(lo, lf, "item_count", 0);
      d->likes = (int)js_num(lo, lf, "likes", 0);
      // The list's own id comes before the user block, and Trakt leaves the
      // user's numeric id out of public answers, so the first "trakt" is the list's.
      d->id = (long)js_num(lo, lf, "trakt", 0);
      u = strstr(lo, "\"user\":");
      if (u && u < lf) js_text(u, lf, "username", d->user, sizeof d->user);
      if (d->id > 0 && d->items > 0 && d->name[0] &&
          (!privacy[0] || !strcmp(privacy, "public"))) n++;
    }
    p = js_next(f);
  }
  return n;
}

static void *threadBrowse(void *u) {
  Batch *b = (Batch *)u;
  const char *header[4];
  char auth[200], key[140], url[300], q[200], *body = NULL;
  if (trakt_public_headers(header, auth, sizeof auth, key, sizeof key)) {
    if (b->query[0]) {
      encode(q, sizeof q, b->query);
      snprintf(url, sizeof url, "https://api.trakt.tv/search/list?query=%s&limit=%d",
               q, TL_BROWSE_MAX);
    } else {
      snprintf(url, sizeof url, "https://api.trakt.tv/lists/popular?limit=%d", TL_BROWSE_MAX);
    }
    body = net_download_headers(url, 20, header);
  }
  b->failed = !body;
  b->n = body ? parseLists(body, b->items, TL_BROWSE_MAX) : 0;
  free(body);
  printf("[lists] %s \"%s\": %d\n", b->query[0] ? "search" : "popular", b->query, b->n);
  fflush(stdout);
  pthread_mutex_lock(&lockBrowse);
  if (b->req == reqLatest) { free(landed); landed = b; b = NULL; }
  pthread_mutex_unlock(&lockBrowse);
  free(b);
  return NULL;
}

void tl_browse(const char *query) {
  Batch *b = calloc(1, sizeof *b);
  pthread_t th;
  if (!b) return;
  snprintf(b->query, sizeof b->query, "%s", query ? query : "");
  pthread_mutex_lock(&lockBrowse);
  b->req = ++reqLatest;
  pthread_mutex_unlock(&lockBrowse);
  state = TLB_LOADING;
  if (pthread_create(&th, NULL, threadBrowse, b) == 0) pthread_detach(th);
  else threadBrowse(b);
}

void tl_browse_poll(void) {
  Batch *b;
  pthread_mutex_lock(&lockBrowse);
  b = landed; landed = NULL;
  pthread_mutex_unlock(&lockBrowse);
  if (!b) return;
  shown = *b;
  state = b->failed ? TLB_FAILED : TLB_READY;
  free(b);
}

int tl_browse_state(void) { return state; }
int tl_browse_n(void) { return shown.n; }
const TlList *tl_browse_item(int i) { return i >= 0 && i < shown.n ? &shown.items[i] : NULL; }
const char *tl_browse_query(void) { return shown.query; }
