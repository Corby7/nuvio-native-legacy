#include "trailers.h"
#include "net.h"
#include "js.h"
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>

typedef struct {
  char name[96], kind[16], thumb[240], url[900];
  int  seconds;
} Trailer;

static Trailer list[TR_MAX];
static int     nList;
static char    idRequest[24], idLoaded[24];
static int     threadAlive;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

// THE LAST FEW TITLES' LISTS, kept in memory. The home prefetches a card's trailers
// while it rests under the focus (prefetchDetail), and sweeping back to a card seen a
// moment ago should not cost IMDb another round trip — nor the title page another
// late button. Only lists with something in them are kept: an empty answer may have
// been the network, and is asked again. The playback URLs are signed for 24 hours;
// an entry is dropped long before that (TR_CACHE_SECS), and never reaches the disk.
#define TR_CACHE      8
#define TR_CACHE_SECS (3 * 60 * 60)
static struct {
  char id[24];
  time_t at;
  int n;
  Trailer list[TR_MAX];
} cache[TR_CACHE];

// Under the lock. The entry for `id` still fresh, or -1.
static int cacheFind(const char *id) {
  time_t now = time(NULL);
  for (int i = 0; i < TR_CACHE; i++)
    if (cache[i].id[0] && !strcmp(cache[i].id, id) && now - cache[i].at < TR_CACHE_SECS)
      return i;
  return -1;
}
// Under the lock. Replaces the oldest entry.
static void cacheStore(const char *id, const Trailer *found, int n) {
  int slot = 0;
  if (n <= 0) return;
  for (int i = 1; i < TR_CACHE; i++) if (cache[i].at < cache[slot].at) slot = i;
  snprintf(cache[slot].id, sizeof cache[slot].id, "%s", id);
  cache[slot].at = time(NULL);
  cache[slot].n = n;
  memcpy(cache[slot].list, found, n * sizeof *found);
}

// The fields are asked for in the order they are read below; the parse finds
// each one by searching forward inside the node, so the order is what keeps
// "value" from being read out of the wrong object.
#define TR_QUERY \
  "query($id:ID!){ title(id:$id){ primaryVideos(first:12){ edges{ node{ " \
  "id contentType{displayName{value}} name{value} runtime{value} " \
  "thumbnail{url} playbackURLs{ url videoMimeType videoDefinition } } } } } }"

// Case-insensitive substring. strcasestr is a GNU extension and the Mac build
// does not see it without feature macros.
static int contains(const char *s, const char *what) {
  size_t n = strlen(what);
  for (; *s; s++) {
    size_t k = 0;
    while (k < n && s[k] && tolower((unsigned char)s[k]) == what[k]) k++;
    if (k == n) return 1;
  }
  return 0;
}

// The first `key` after `from` that is still inside [from, end).
static const char *inside(const char *from, const char *end, const char *key) {
  const char *p = strstr(from, key);
  return (p && p < end) ? p : NULL;
}

// Higher is better. 240p is below SD on purpose: IMDb's "SD" is 360p or 480p.
static int rankOf(const char *def) {
  if (!strcmp(def, "DEF_1080p")) return 5;
  if (!strcmp(def, "DEF_720p"))  return 4;
  if (!strcmp(def, "DEF_480p"))  return 3;
  if (!strcmp(def, "DEF_SD"))    return 2;
  return 1;
}

// "…@._V1_.jpg" is the full-size original (600 KB on Dune: Part Two); the same
// path with "_QL75_UX520_" is resized by the image CDN to the card's width
// (15 KB). Any other shape is kept as it came.
static void thumbForCard(const char *src, char *dst, size_t size) {
  const char *v = strstr(src, "._V1_.jpg");
  if (v) snprintf(dst, size, "%.*s._V1_QL75_UX520_.jpg", (int)(v - src), src);
  else snprintf(dst, size, "%s", src);
}

static int parse(const char *body, Trailer *out) {
  const char *end = body + strlen(body);
  const char *e = js_array(body, end, "edges");
  int n = 0;
  while (e && n < TR_MAX) {
    const char *f = js_end(e), *p;
    Trailer t;
    int best = 0;
    memset(&t, 0, sizeof t);
    if ((p = inside(e, f, "\"contentType\"")))
      js_text(p, f, "value", t.kind, sizeof t.kind);
    if ((p = inside(e, f, "\"name\"")))
      js_text(p, f, "value", t.name, sizeof t.name);
    if ((p = inside(e, f, "\"runtime\"")))
      t.seconds = (int)js_num(p, f, "value", 0.0);
    if ((p = inside(e, f, "\"thumbnail\""))) {
      char raw[240] = "";
      js_text(p, f, "url", raw, sizeof raw);
      thumbForCard(raw, t.thumb, sizeof t.thumb);
    }
    // Only MP4: the HLS master would work too, but a single progressive file is
    // what the player's MP4 path is tuned for (video_set_mp4).
    { const char *u = js_array(e, f, "playbackURLs");
      while (u) {
        const char *uf = js_end(u);
        char mime[16] = "", def[16] = "", url[900] = "";
        js_text(u, uf, "videoMimeType", mime, sizeof mime);
        js_text(u, uf, "videoDefinition", def, sizeof def);
        js_text(u, uf, "url", url, sizeof url);
        if (!strcmp(mime, "MP4") && url[0] && rankOf(def) > best) {
          best = rankOf(def);
          snprintf(t.url, sizeof t.url, "%s", url);
        }
        u = js_next(uf);
      } }
    // IMDb mixes clips, interviews and awards reels into the same list.
    if (t.url[0] && (!strcmp(t.kind, "Trailer") || !strcmp(t.kind, "Teaser"))) {
      if (!t.name[0]) snprintf(t.name, sizeof t.name, "%s", t.kind);
      out[n++] = t;
    }
    e = js_next(f);
  }
  return n;
}

// Moves the main trailer to the front (see trailers.h for the rule).
static void mainFirst(Trailer *t, int n) {
  int pick = -1, i;
  for (i = 0; i < n && pick < 0; i++)
    if (contains(t[i].name, "official trailer") && t[i].seconds >= 60) pick = i;
  for (i = 0; i < n && pick < 0; i++)
    if (!strcmp(t[i].kind, "Trailer") && t[i].seconds >= 60) pick = i;
  if (pick > 0) {
    Trailer m = t[pick];
    memmove(t + 1, t, pick * sizeof *t);
    t[0] = m;
  }
}

static void *fetch(void *arg) {
  static const char *const header[] = { "x-imdb-client-name: imdb-web-next", NULL };
  char id[24], payload[600];
  (void)arg;
  for (;;) {
    Trailer found[TR_MAX];
    int n = 0;
    char *body;
    pthread_mutex_lock(&lock);
    snprintf(id, sizeof id, "%s", idRequest);
    pthread_mutex_unlock(&lock);
    snprintf(payload, sizeof payload,
             "{\"query\":\"%s\",\"variables\":{\"id\":\"%s\"}}", TR_QUERY, id);
    body = net_post("https://api.graphql.imdb.com/", 15, header, payload);
    if (body) { n = parse(body, found); free(body); }
    mainFirst(found, n);
    printf("[trailers] %s: %d from IMDb%s\n", id, n, body ? "" : " (no answer)");
    fflush(stdout);
    pthread_mutex_lock(&lock);
    cacheStore(id, found, n);
    if (!strcmp(id, idRequest)) {
      // Copied while nList is still 0, then published: the drawing reads without
      // the lock and only ever sees a whole list or none.
      memcpy(list, found, n * sizeof *found);
      nList = n;
      snprintf(idLoaded, sizeof idLoaded, "%s", id);
      threadAlive = 0;
      pthread_mutex_unlock(&lock);
      return NULL;
    }
    // Another title was opened during the request: fetch that one instead —
    // unless the cache already answered it.
    if (!strcmp(idLoaded, idRequest)) {
      threadAlive = 0;
      pthread_mutex_unlock(&lock);
      return NULL;
    }
    pthread_mutex_unlock(&lock);
  }
}

void trailers_request(const char *imdb) {
  char id[24];
  pthread_t t;
  size_t n;
  if (!imdb || imdb[0] != 't') return;
  n = strcspn(imdb, ":");
  if (n >= sizeof id) n = sizeof id - 1;
  memcpy(id, imdb, n); id[n] = 0;
  pthread_mutex_lock(&lock);
  // An EMPTY answer is asked again on the next visit: it may have been the
  // network, and a title with no trailers costs one small POST per opening.
  if (!strcmp(id, idRequest) && (threadAlive || nList > 0)) {
    pthread_mutex_unlock(&lock); return;
  }
  snprintf(idRequest, sizeof idRequest, "%s", id);
  nList = 0; idLoaded[0] = 0;
  // Already fetched: published at once, the same way the thread publishes — the
  // list copied while nList is 0, then the count. A thread still in flight for
  // another title sees idRequest changed and simply stores what it gets.
  { int c = cacheFind(id);
    if (c >= 0) {
      memcpy(list, cache[c].list, cache[c].n * sizeof *list);
      nList = cache[c].n;
      snprintf(idLoaded, sizeof idLoaded, "%s", id);
      pthread_mutex_unlock(&lock);
      return;
    } }
  if (threadAlive) { pthread_mutex_unlock(&lock); return; }
  threadAlive = 1;
  pthread_mutex_unlock(&lock);
  if (pthread_create(&t, NULL, fetch, NULL) != 0) {
    pthread_mutex_lock(&lock); threadAlive = 0; idRequest[0] = 0;
    pthread_mutex_unlock(&lock);
  } else pthread_detach(t);
}

int trailers_n(void) { return nList; }
#define FIELD(i, f) ((i) >= 0 && (i) < nList ? list[i].f : "")
const char *trailers_name(int i)  { return FIELD(i, name); }
const char *trailers_kind(int i)  { return FIELD(i, kind); }
const char *trailers_thumb(int i) { return FIELD(i, thumb); }
const char *trailers_url(int i)   { return FIELD(i, url); }
int trailers_seconds(int i) { return (i >= 0 && i < nList) ? list[i].seconds : 0; }
const char *trailers_title(void) { return idLoaded; }
