#include "traktlists.h"
#include "trakt.h"
#include "net.h"
#include "js.h"
#include "data.h"
#include "profiles.h"
#include "extras.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

// --- the added lists --------------------------------------------------------------
//
// One line per list: "<id>\t<items>\t<likes>\t<user>\t<name>". The name goes
// last because it is the only field that can hold anything; a tab inside it is
// turned into a space on the way in. An MDBList list's id carries an "m" in
// front ("m2194"); a bare number is Trakt's, which is every line written before
// MDBList existed here.
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
    l->src = f[0][0] == 'm' ? TL_MDB : TL_TRAKT;
    l->id = strtol(f[0] + (l->src == TL_MDB), NULL, 10);
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
    w += (size_t)snprintf(text + w, sizeof text - w, "%s%ld\t%d\t%d\t%s\t%s\n",
                          added[i].src == TL_MDB ? "m" : "", added[i].id,
                          added[i].items, added[i].likes, u, t);
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

static int findLocked(int src, long id) {
  int i;
  for (i = 0; i < nAdded; i++) if (added[i].src == src && added[i].id == id) return i;
  return -1;
}

int tl_has(int src, long id) {
  int v;
  pthread_mutex_lock(&lock);
  loadLocked();
  v = findLocked(src, id) >= 0;
  pthread_mutex_unlock(&lock);
  return v;
}

int tl_add(const TlList *l) {
  int ok = 1;
  pthread_mutex_lock(&lock);
  loadLocked();
  if (findLocked(l->src, l->id) < 0) {
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
  if (ok) { printf("[lists] added %s %ld \"%s\"\n", tl_source_name(l->src), l->id, l->name); fflush(stdout); }
  return ok;
}

void tl_remove(int src, long id) {
  int k;
  pthread_mutex_lock(&lock);
  loadLocked();
  k = findLocked(src, id);
  if (k >= 0) {
    memmove(added + k, added + k + 1, sizeof(TlList) * (size_t)(nAdded - k - 1));
    nAdded--;
    saveLocked();
    gen++;
  }
  pthread_mutex_unlock(&lock);
  if (k >= 0) { printf("[lists] removed %s %ld\n", tl_source_name(src), id); fflush(stdout); }
}

unsigned tl_generation(void) {
  pthread_mutex_lock(&lock);
  loadLocked();   // a profile switch is a new set too
  pthread_mutex_unlock(&lock);
  return gen;
}

void tl_key(int src, long id, char *dst, size_t n) {
  snprintf(dst, n, src == TL_MDB ? "mdblist_%ld" : "traktlist_%ld", id);
}

long tl_id_of_key(const char *key, int *src) {
  int s;
  long id;
  if (!key) return 0;
  if (!strncmp(key, "traktlist_", 10)) { s = TL_TRAKT; id = strtol(key + 10, NULL, 10); }
  else if (!strncmp(key, "mdblist_", 8)) { s = TL_MDB; id = strtol(key + 8, NULL, 10); }
  else return 0;
  if (src) *src = s;
  return id;
}

// --- the sources ----------------------------------------------------------------------

const char *tl_source_name(int src) { return src == TL_MDB ? "MDBList" : "Trakt"; }

int tl_source_usable(int src) {
  if (src == TL_MDB) return extras_mdb_key()[0] != 0;
  { const char *header[4];
    char auth[200], key[140];
    return trakt_public_headers(header, auth, sizeof auth, key, sizeof key); }
}

// --- browsing -----------------------------------------------------------------------
//
// The thread fills a fresh array and hands it over under the lock; the main
// thread adopts it in tl_browse_poll. A request number travels with each fetch,
// so a slow search that lands after a newer one was asked for is thrown away.
typedef struct { TlList items[TL_BROWSE_MAX]; int n, failed, src; unsigned req; char query[96]; } Batch;

static pthread_mutex_t lockBrowse = PTHREAD_MUTEX_INITIALIZER;
static Batch *landed;          // finished, not yet adopted
static unsigned reqLatest;
static int state = TLB_IDLE, stateSrc = TL_TRAKT;
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

// MDBList's answer is the lists themselves in a root array, flat: {"id",
// "name", "user_name", "items", "likes", "description", "private", ...}. A
// wrapped answer ({"lists": [...]}) is read too, in case the API ever adds one.
static int parseMdbLists(const char *body, TlList *out, int max) {
  const char *p, *end;
  int n = 0;
  if (!body) return 0;
  p = body;
  while (*p && (unsigned char)*p <= ' ') p++;
  if (*p == '{') { end = js_end(p); p = js_array(p, end, "lists"); }
  else p = js_root_array(body);
  for (; p && n < max; p = js_next(js_end(p))) {
    const char *f = js_end(p);
    TlList *d = &out[n];
    if (*p != '{') break;
    memset(d, 0, sizeof *d);
    d->src = TL_MDB;
    d->id = (long)js_num(p, f, "id", 0);
    js_text(p, f, "name", d->name, sizeof d->name);
    js_text(p, f, "user_name", d->user, sizeof d->user);
    if (!d->user[0]) js_text(p, f, "username", d->user, sizeof d->user);
    js_text(p, f, "description", d->desc, sizeof d->desc);
    d->items = (int)js_num(p, f, "items", 0);
    d->likes = (int)js_num(p, f, "likes", 0);
    if (d->id > 0 && d->items > 0 && d->name[0] && !js_flag(p, f, "private", 0)) n++;
  }
  return n;
}

static void *threadBrowse(void *u) {
  Batch *b = (Batch *)u;
  const char *header[4];
  char auth[200], key[140], url[400], q[200], *body = NULL;
  if (b->src == TL_MDB) {
    const char *k = extras_mdb_key();
    if (k[0]) {
      if (b->query[0]) {
        encode(q, sizeof q, b->query);
        snprintf(url, sizeof url, "https://api.mdblist.com/lists/search?query=%s&limit=%d&apikey=%s",
                 q, TL_BROWSE_MAX, k);
      } else {
        snprintf(url, sizeof url, "https://api.mdblist.com/lists/top?limit=%d&apikey=%s",
                 TL_BROWSE_MAX, k);
      }
      body = net_download(url, 20);
    }
    b->failed = !body;
    b->n = body ? parseMdbLists(body, b->items, TL_BROWSE_MAX) : 0;
  } else if (trakt_public_headers(header, auth, sizeof auth, key, sizeof key)) {
    if (b->query[0]) {
      encode(q, sizeof q, b->query);
      snprintf(url, sizeof url, "https://api.trakt.tv/search/list?query=%s&limit=%d",
               q, TL_BROWSE_MAX);
    } else {
      snprintf(url, sizeof url, "https://api.trakt.tv/lists/popular?limit=%d", TL_BROWSE_MAX);
    }
    body = net_download_headers(url, 20, header);
    b->failed = !body;
    b->n = body ? parseLists(body, b->items, TL_BROWSE_MAX) : 0;
  } else b->failed = 1;
  free(body);
  printf("[lists] %s %s \"%s\": %d\n", tl_source_name(b->src),
         b->query[0] ? "search" : "popular", b->query, b->n);
  fflush(stdout);
  pthread_mutex_lock(&lockBrowse);
  if (b->req == reqLatest) { free(landed); landed = b; b = NULL; }
  pthread_mutex_unlock(&lockBrowse);
  free(b);
  return NULL;
}

void tl_browse(int src, const char *query) {
  Batch *b = calloc(1, sizeof *b);
  pthread_t th;
  if (!b) return;
  b->src = src == TL_MDB ? TL_MDB : TL_TRAKT;
  stateSrc = b->src;
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
int tl_browse_src(void) { return state == TLB_LOADING ? stateSrc : shown.src; }

// --- a list's titles --------------------------------------------------------------

typedef struct { int rank; CatItem *d; } Ranked;
static int byRank(const void *a, const void *b) {
  return ((const Ranked *)a)->rank - ((const Ranked *)b)->rank;
}

// One array of MDBList items: {"rank", "title", "imdb_id", "mediatype",
// "release_year", ...}. Without an IMDb id there is nothing to draw or open.
static int mdbItemsOf(const char *p, CatItem *out, int *rank, int max) {
  int n = 0;
  for (; p && n < max; p = js_next(js_end(p))) {
    const char *f = js_end(p);
    char imdb[24] = "", type[16] = "";
    if (*p != '{') break;
    js_text(p, f, "imdb_id", imdb, sizeof imdb);
    if (strncmp(imdb, "tt", 2)) continue;
    js_text(p, f, "mediatype", type, sizeof type);
    memset(&out[n], 0, sizeof out[n]);
    js_text(p, f, "title", out[n].title, sizeof out[n].title);
    out[n].year = (int)js_num(p, f, "release_year", 0);
    trakt_list_art(&out[n], imdb, !strcmp(type, "show"));
    rank[n] = (int)js_num(p, f, "rank", 1000000 + n);
    n++;
  }
  return n;
}

static int mdbItems(long id, CatItem *output, int max) {
  const char *k = extras_mdb_key(), *p;
  char url[300], *body;
  CatItem *tmp;
  int *rank, n = 0, i;
  Ranked *order;
  if (!k[0] || id <= 0 || max < 1) return 0;
  // Movies and shows come back as two arrays, so `limit` bounds each. Each array
  // is already in the order the list's owner chose to show it (often newest
  // first, not by rank), so a list of one kind keeps it as it came; only a mixed
  // list is merged, by rank, which is the one order both arrays share.
  snprintf(url, sizeof url, "https://api.mdblist.com/lists/%ld/items?limit=%d&apikey=%s",
           id, max, k);
  body = net_download(url, 25);
  if (!body) { printf("[lists] MDBList %ld: no answer\n", id); fflush(stdout); return 0; }
  tmp = calloc((size_t)max * 2, sizeof *tmp);
  rank = calloc((size_t)max * 2, sizeof *rank);
  order = calloc((size_t)max * 2, sizeof *order);
  if (tmp && rank && order) {
    p = body;
    while (*p && (unsigned char)*p <= ' ') p++;
    int movies = 0, mixed = 0;
    if (*p == '[') n = mdbItemsOf(js_root_array(body), tmp, rank, max * 2);
    else {
      const char *end = js_end(p);
      n = movies = mdbItemsOf(js_array(p, end, "movies"), tmp, rank, max);
      n += mdbItemsOf(js_array(p, end, "shows"), tmp + n, rank + n, max * 2 - n);
      mixed = movies > 0 && n > movies;
    }
    for (i = 0; i < n; i++) { order[i].rank = mixed ? rank[i] : i; order[i].d = &tmp[i]; }
    if (mixed) qsort(order, (size_t)n, sizeof *order, byRank);
    if (n > max) n = max;
    for (i = 0; i < n; i++) output[i] = *order[i].d;
  }
  free(tmp); free(rank); free(order); free(body);
  printf("[lists] MDBList %ld: %d\n", id, n);
  fflush(stdout);
  return n;
}

int tl_items(int src, long id, CatItem *output, int max) {
  return src == TL_MDB ? mdbItems(id, output, max) : trakt_public_list(id, output, max);
}
