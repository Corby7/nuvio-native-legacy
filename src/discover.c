#include "discover.h"
#include "mark.h"
#include <SDL2/SDL.h>
#include "catalog.h"
#include "addons.h"
#include "net.h"
#include "js.h"
#include "trakt.h"
#include "settings.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

#define CINEMETA "https://v3-cinemeta.strem.io"
#define TMDB     "https://api.themoviedb.org/3"

// "2026-07-29" -> "29 July 2026". The web app's format, which uses
// `toLocaleDateString(undefined, {month:"long", day:"numeric", year:"numeric"})`
// (metaDetailsScreen.js:1387); the numeric format that used to be here was the
// port's invention. Input that does not match the ISO pattern comes out as it
// arrived, and not empty: better to show the raw date than to swallow the data.
void disc_date_long(const char *iso, char *dst, size_t size) {
  static const char *MONTH[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
  };
  if (!iso || !dst || size == 0) { if (dst && size) dst[0] = 0; return; }
  if (strlen(iso) >= 10 && iso[4] == '-') {
    int month = (iso[5] - '0') * 10 + (iso[6] - '0');
    int day = (iso[8] - '0') * 10 + (iso[9] - '0');
    if (month >= 1 && month <= 12) {
      snprintf(dst, size, "%d %s %c%c%c%c",
               day, MONTH[month - 1], iso[0], iso[1], iso[2], iso[3]);
      return;
    }
    snprintf(dst, size, "%c%c%c%c", iso[0], iso[1], iso[2], iso[3]);
    return;
  }
  snprintf(dst, size, "%s", iso);
}


// The TMDB key, in art/tmdb.txt. The owner's SECRET (it came out of the web app's
// dist/nuvio.env.js) — do not commit it. Without it the cast has names only.
static char tmdbKey[64];
static char dirArtDisc[512];
// 1 once a COMPLETE catalogue has reached the screen this session.
//
// The partial publishes below exist to fill an EMPTY-ish home while the network
// works, and they were gated on cat_do_cache() alone. But cat_cache_replaced()
// clears that flag at the first complete publish, so the SECOND build — the
// rebuild disc_rebuild() asks for when the account's addons arrive from the
// sync — published in pieces over a home that was already fully populated.
// Measured on the TV: the home stood complete at 2335 ms, collapsed to the
// single "Continue watching" row at 3201 ms (the hero blank, its art a CDN URL
// still downloading) and only filled again at 5576 ms. That is the black flash
// between the packaged catalogue and the real screen.
//
// Once something complete is on screen, a rebuild only ever swaps in another
// complete result.
static int completeOnScreen;
// May a HALF-BUILT catalogue go on screen? Only while what is showing is the
// packaged fallback: replacing 40 strangers' titles with one real row is a
// gain, replacing the owner's own home with it is the regression above. Both
// the cache and a previous complete build count as the owner's.
static int partialAllowed(void) { return !cat_do_cache() && !completeOnScreen; }

void disc_tmdb_set(const char *key) {
  if (!key || !*key) return;
  snprintf(tmdbKey, sizeof tmdbKey, "%s", key);
  printf("[disc] tmdb: key from the account\n");
  fflush(stdout);
}

void disc_tmdb(const char *dirArt) {
  char path[600];
  FILE *f;
  snprintf(dirArtDisc, sizeof dirArtDisc, "%s", dirArt ? dirArt : ".");
  snprintf(path, sizeof path, "%s/tmdb.txt", dirArt ? dirArt : ".");
  f = fopen(path, "r");
  if (!f) return;
  if (fgets(tmdbKey, sizeof tmdbKey, f)) {
    char *end = tmdbKey + strlen(tmdbKey);
    while (end > tmdbKey && (end[-1] == '\n' || end[-1] == '\r')) *--end = 0;
  }
  fclose(f);
  printf("[disc] tmdb %s\n", tmdbKey[0] ? "ok" : "missing");
}

const char *disc_key_tmdb(void) { return tmdbKey; }

// A strstr that does NOT go past `end`. The BR region's object ends before the
// other regions in TMDB's response; looking for rent/buy in the whole body would
// pick another region's provider when BR did not have one.
// Canonical genre label. Cinemeta and the addons return genres in English, but
// not consistently: the same genre arrives as "Sci-Fi" from one source and
// "Science Fiction" from another, and the hyphenated forms ("Film-Noir",
// "Talk-Show") read as identifiers rather than labels. Both spellings landing
// in the same row is visible on every card and every detail screen.
//
// A table and not a lookup: the Stremio genre set is closed and small, and a
// network round trip per title to tidy two words would be absurd. A genre
const char *disc_genre_label(const char *g) {
  static const struct { const char *from, *to; } T[] = {
    { "Sci-Fi",     "Science Fiction" }, { "Film-Noir", "Film Noir" },
    { "Game-Show",  "Game Show" },       { "Talk-Show", "Talk Show" },
    { "Reality-TV", "Reality" },
  };
  size_t i;
  if (!g || !*g) return "";
  for (i = 0; i < sizeof T / sizeof *T; i++)
    if (!strcasecmp(g, T[i].from)) return T[i].to;
  return g;
}

static const char *ate(const char *start, const char *end, const char *needle) {
  size_t n = strlen(needle);
  for (; start && start + n <= end; start++)
    if (*start == needle[0] && memcmp(start, needle, n) == 0) return start;
  return NULL;
}

// The first provider of the `key` array within [start,end): the name and the logo
// in TMDB's w92 format. flatrate/rent/buy are arrays of providers; the first is
// the main one in practice (TMDB orders by local relevance).
static int providerBetween(const char *start, const char *end, const char *key,
                         char *name, size_t nName, char *logo, size_t nLogo) {
  const char *k = ate(start, end, key);
  const char *item = k ? strchr(k, '{') : NULL;
  if (!item || item >= end) return 0;
  const char *fi = js_end(item);
  if (fi > end) fi = end;
  char path[128] = "";
  if (!js_text(item, fi, "provider_name", name, nName)) return 0;
  if (js_text(item, fi, "logo_path", path, sizeof path) && path[0] == '/')
    snprintf(logo, nLogo, "https://image.tmdb.org/t/p/w92%s", path);
  return 1;
}

// Fills in the cast's photo and character. Cinemeta gives only the NAME; the
// character and the portrait come from TMDB, which needs two round trips: finding
// its id from the IMDb id and only then asking for the credits.
static void photosOfCast(CatItem *d, const char *imdbSeries, int series) {
  char url[400], *body;
  long idTmdb = 0;
  if (!tmdbKey[0] || d->nCast < 1) return;
  snprintf(url, sizeof url, "%s/find/%s?api_key=%s&external_source=imdb_id",
           TMDB, imdbSeries, tmdbKey);
  body = net_download(url, 20);
  if (!body) return;
  { const char *vet = series ? "tv_results" : "movie_results";
    const char *p = js_array(body, NULL, vet);
    if (p) idTmdb = (long)js_num(p, js_end(p), "id", 0); }
  free(body);
  if (!idTmdb) return;
  d->tmdb = idTmdb;

  snprintf(url, sizeof url, "%s/%s/%ld/credits?api_key=%s",
           TMDB, series ? "tv" : "movie", idTmdb, tmdbKey);
  body = net_download(url, 20);
  if (!body) return;
  { const char *p = js_array(body, NULL, "cast");
    int k = 0;
    (void)0;
    while (p && k < d->nCast) {
      const char *f = js_end(p);
      char pathPhoto[128] = "";
      js_text(p, f, "character", d->cast[k].role, sizeof d->cast[k].role);
      d->cast[k].tmdb = (long)js_num(p, f, "id", 0.0);
      if (js_text(p, f, "profile_path", pathPhoto, sizeof pathPhoto) &&
          pathPhoto[0] == '/')
        snprintf(d->cast[k].photo, sizeof d->cast[k].photo,
                 "https://image.tmdb.org/t/p/w185%s", pathPhoto);
      // TMDB returns the cast in the same order of importance as Cinemeta, so
      // matching by position is right in practice; matching by name would fail on
      // accents and on names spelled differently between the two databases.
      k++;
      p = js_next(f);
    } }
  free(body);

  // Where to watch. The providerLogo/providerName fields existed in CatItem and
  // were NEVER filled in on the dynamic path — the streaming badge was empty on
  // every title. TMDB answers by region; BR is the owner's.
  snprintf(url, sizeof url, "%s/%s/%ld/watch/providers?api_key=%s",
           TMDB, series ? "tv" : "movie", idTmdb, tmdbKey);
  body = net_download(url, 20);
  if (body) {
    const char *br = strstr(body, "\"BR\"");
    if (br) {
      const char *brObj = strchr(br, '{');
      const char *brEnd = brObj ? js_end(brObj) : NULL;
      if (brObj && brEnd && brEnd > brObj) {
        // flatrate = included in the subscription; rent = rental; buy = purchase.
        // If the title is not on streaming here, the badge stays empty ON PURPOSE,
        // instead of advertising a rental as though it were catalogue.
        providerBetween(brObj, brEnd, "\"flatrate\"",
                      d->providerName, sizeof d->providerName,
                      d->providerLogo, sizeof d->providerLogo);
        providerBetween(brObj, brEnd, "\"rent\"",
                      d->rentName, sizeof d->rentName,
                      d->rentLogo, sizeof d->rentLogo);
        providerBetween(brObj, brEnd, "\"buy\"",
                      d->compName, sizeof d->compName,
                      d->compLogo, sizeof d->compLogo);
      }
    }
    free(body);
  }
}

// Defined further down, along with the rest of the Stremio meta parse; declared
// here because the search, just below, builds a CatItem from the same response.
static int ofMeta(const char *start, const char *end, const char *kind, CatItem *d);

// --- SEARCH BY TITLE ---------------------------------------------------------
//
// The search screen only filtered what was already in memory (a strstr over the
// home's rows), so looking for anything outside the first ~12 lines of each
// catalogue found nothing — and the owner saw that as "it isn't searching
// everything". It was true: there was no network query at all.
//
// The Stremio protocol exposes search on the same catalogue endpoint, with the
// filter in the path: <base>/catalog/<type>/<id>/search=<term>.json. Cinemeta,
// which is the official catalogue and does not depend on the owner's addons,
// answers on both types — and that is why it is the source used here: a search
// that only worked with the installed addons would fail differently on every machine.
//
// It runs on a THREAD OF ITS OWN because it blocks (two round trips), and the
// search screen cannot freeze between one keypress and the next.
static char     searchTerm[96];     // the term ALREADY QUERIED
static char     searchRequest[96];    // the term the threads should query
static pthread_mutex_t searchLock = PTHREAD_MUTEX_INITIALIZER;

// Escapes the term to fit in a URL path. Without this a space or an accent breaks
// the request, and "the invite" — two words, the normal case — would never reach
// the server.
static void urlEscape(const char *s, char *dst, size_t size) {
  static const char *HEX = "0123456789ABCDEF";
  size_t o = 0;
  for (; *s && o + 4 < size; s++) {
    unsigned char c = (unsigned char)*s;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      dst[o++] = (char)c;
    } else {
      dst[o++] = '%'; dst[o++] = HEX[c >> 4]; dst[o++] = HEX[c & 15];
    }
  }
  dst[o] = 0;
}

static int readSearch(const char *kind, const char *term, CatItem *output,
                    int max) {
  char url[500], esc[300];
  char *body;
  const char *p;
  int n = 0;
  urlEscape(term, esc, sizeof esc);
  snprintf(url, sizeof url, "%s/catalog/%s/top/search=%s.json",
           CINEMETA, kind, esc);
  body = net_download(url, 20);
  if (!body) return 0;
  p = js_array(body, NULL, "metas");
  while (p && n < max) {
    const char *f = js_end(p);
    if (ofMeta(p, f, kind, &output[n])) n++;
    p = js_next(f);
  }
  free(body);
  return n;
}

// --- SEARCH TARGETS ----------------------------------------------------------
//
// A "target" is a catalogue that accepts search. There are Cinemeta's 2 (which
// always exist, independent of the owner's addons) plus whatever the manifests
// declare. On the owner's addons that is 8: Xperience (film/series), AIOStreams
// TMDB and TVDB (film/series each) and Akashi TV (film/series).
//
// Before, only Cinemeta was queried, and that is what the owner saw as "it isn't
// searching all the catalogues" — because it genuinely was not.
#define SEARCH_TARGETS  16
#define SEARCH_PER_TARGET 12          // one row per target, 12 fit on screen
#define SEARCH_THREADS    3            // how many targets in flight at once

typedef struct {
  char base[300];
  char kind[8];
  char id[96];
  char title[96];
  char addon[64];
} TargetSearch;

static TargetSearch targets[SEARCH_TARGETS];
static int       nTargets;

// The result PER TARGET, with the generation it was obtained in. Storing it per
// target (and not in a single list) is what allows one row per catalogue, with the
// origin, and what lets the screen show the first one to answer without waiting
// for the slowest.
static struct {
  CatItem items[SEARCH_PER_TARGET];
  int     n;
  int     generation;
} resTarget[SEARCH_TARGETS];

static int  generation;            // goes up with every new term
static int  nextTarget;        // the work queue: the next index to query
static int  threadsAlive;

void disc_targets_search_reset(void) {
  pthread_mutex_lock(&searchLock);
  // Cinemeta goes in ALWAYS and first: it is the only source that depends on no
  // addon, so the search goes on working on a clean installation.
  nTargets = 0;
  { int t; const char *tt[2] = { "movie", "series" };
    const char *rot[2] = { "Films", "Series" };
    for (t = 0; t < 2; t++) {
      TargetSearch *a = &targets[nTargets++];
      snprintf(a->base,  sizeof a->base,  "%s", CINEMETA);
      snprintf(a->kind,  sizeof a->kind,  "%s", tt[t]);
      snprintf(a->id,    sizeof a->id,    "%s", "top");
      snprintf(a->title,sizeof a->title,"%s", rot[t]);
      snprintf(a->addon, sizeof a->addon, "%s", "Cinemeta");
    } }
  memset(resTarget, 0, sizeof resTarget);
  pthread_mutex_unlock(&searchLock);
}

void disc_target_search(const char *base, const char *kind, const char *id,
                     const char *title, const char *addon) {
  pthread_mutex_lock(&searchLock);
  if (nTargets < SEARCH_TARGETS) {
    TargetSearch *a = &targets[nTargets++];
    snprintf(a->base,   sizeof a->base,   "%s", base ? base : "");
    snprintf(a->kind,   sizeof a->kind,   "%s", kind ? kind : "");
    snprintf(a->id,     sizeof a->id,     "%s", id ? id : "");
    snprintf(a->title, sizeof a->title, "%s", title ? title : "");
    snprintf(a->addon,  sizeof a->addon,  "%s", addon ? addon : "");
  }
  pthread_mutex_unlock(&searchLock);
}

// Queries ONE target. Returns how many items it read.
static int queryTarget(const TargetSearch *a, const char *term,
                         CatItem *output, int max) {
  char url[600], esc[300];
  char *body;
  const char *p;
  int n = 0;
  urlEscape(term, esc, sizeof esc);
  snprintf(url, sizeof url, "%s/catalog/%s/%s/search=%s.json",
           a->base, a->kind, a->id, esc);
  // 6 s per target, like the web app (SEARCH_CATALOG_TIMEOUT 6500). A slow addon
  // does not freeze the screen: its row only appears when it arrives, and the
  // others are already there.
  body = net_download(url, 6);
  if (!body) return 0;
  p = js_array(body, NULL, "metas");
  while (p && n < max) {
    const char *f = js_end(p);
    if (ofMeta(p, f, a->kind, &output[n])) n++;
    p = js_next(f);
  }
  free(body);
  return n;
}

static void *threadSearch(void *arg) {
  (void)arg;
  for (;;) {
    TargetSearch a;
    char term[96];
    int mine, g;
    CatItem found[SEARCH_PER_TARGET];
    int n;

    pthread_mutex_lock(&searchLock);
    if (nextTarget >= nTargets || !searchRequest[0]) {
      threadsAlive--;
      pthread_mutex_unlock(&searchLock);
      return NULL;
    }
    mine = nextTarget++;
    a = targets[mine];
    g = generation;
    snprintf(term, sizeof term, "%s", searchRequest);
    pthread_mutex_unlock(&searchLock);

    n = queryTarget(&a, term, found, SEARCH_PER_TARGET);

    pthread_mutex_lock(&searchLock);
    // An old generation = the owner typed something else while this was coming
    // back. The result was born stale; discarding is cheaper than showing and
    // swapping.
    if (g == generation) {
      memcpy(resTarget[mine].items, found, sizeof(CatItem) * (size_t)n);
      resTarget[mine].n = n;
      resTarget[mine].generation = g;
      snprintf(searchTerm, sizeof searchTerm, "%s", term);
    }
    pthread_mutex_unlock(&searchLock);
  }
}

void disc_fetch(const char *term) {
  int k, missing;
  if (!term) return;
  pthread_mutex_lock(&searchLock);
  if (!strcmp(term, searchRequest)) { pthread_mutex_unlock(&searchLock); return; }
  snprintf(searchRequest, sizeof searchRequest, "%s", term);
  generation++;
  nextTarget = 0;
  // It zeroes the count, not the items: the screen may be drawing the current
  // frame, and reading half an item would be worse than one row fewer.
  for (k = 0; k < SEARCH_TARGETS; k++) resTarget[k].n = 0;
  missing = SEARCH_THREADS - threadsAlive;
  pthread_mutex_unlock(&searchLock);

  // Threads on demand: the ones already alive pick up the new targets themselves,
  // because they read `nextTarget` under the lock on every round.
  for (k = 0; k < missing; k++) {
    pthread_t t;
    pthread_mutex_lock(&searchLock); threadsAlive++; pthread_mutex_unlock(&searchLock);
    if (pthread_create(&t, NULL, threadSearch, NULL) != 0) {
      pthread_mutex_lock(&searchLock); threadsAlive--; pthread_mutex_unlock(&searchLock);
    } else {
      pthread_detach(t);
    }
  }
}

int disc_search_generation(void) {
  int g;
  pthread_mutex_lock(&searchLock);
  g = generation;
  pthread_mutex_unlock(&searchLock);
  return g;
}

int disc_search_n_targets(void) { return nTargets; }

int disc_search_target_n(int target, const char *term) {
  int n = 0;
  pthread_mutex_lock(&searchLock);
  if (target >= 0 && target < nTargets && term && !strcmp(term, searchTerm) &&
      resTarget[target].generation == generation)
    n = resTarget[target].n;
  pthread_mutex_unlock(&searchLock);
  return n;
}

const char *disc_search_target_title(int target) {
  return (target >= 0 && target < nTargets) ? targets[target].title : "";
}
const char *disc_search_target_addon(int target) {
  return (target >= 0 && target < nTargets) ? targets[target].addon : "";
}

int disc_search_target_item(int target, int i, CatItem *dst) {
  int ok = 0;
  pthread_mutex_lock(&searchLock);
  if (dst && target >= 0 && target < nTargets && i >= 0 && i < resTarget[target].n) {
    memcpy(dst, &resTarget[target].items[i], sizeof *dst);
    ok = 1;
  }
  pthread_mutex_unlock(&searchLock);
  return ok;
}

// Compatibility for anyone still asking "how many in total".
int disc_search_n(const char *term) {
  int k, t = 0;
  for (k = 0; k < nTargets; k++) t += disc_search_target_n(k, term);
  return t;
}


static int searching;
static pthread_t thread, threadEp;
static int epItem = -1, epTemp, threadEpAlive;

int disc_searching(void) {
  int v;
  pthread_mutex_lock(&searchLock);
  v = (threadsAlive > 0);
  pthread_mutex_unlock(&searchLock);
  return v;
}

// A catalogue item built from a Stremio meta. Returns 1 if it was usable (it needs
// a name and some art).
static int ofMeta(const char *start, const char *end, const char *kind, CatItem *d) {
  char v[900];
  memset(d, 0, sizeof *d);
  if (!js_text(start, end, "name", d->title, sizeof d->title)) return 0;
  // The poster is the only mandatory one: without it the card is a grey rectangle.
  if (!js_text(start, end, "poster", d->poster, sizeof d->poster)) return 0;
  js_text(start, end, "background", d->backdrop, sizeof d->backdrop);
  js_text(start, end, "logo", d->logo, sizeof d->logo);
  // Off TMDB's `original` and onto w1280 — the measurement that says why, and the
  // reason this is no longer written out here, are in catalog.h. Done by rewriting
  // the URL and not by asking for another field because Cinemeta only returns this
  // one; TMDB's ladder is w300/w780/w1280/original.
  cat_backdrop_shrink(d->backdrop, sizeof d->backdrop);
  if (!d->backdrop[0]) snprintf(d->backdrop, sizeof d->backdrop, "%s", d->poster);

  if (!js_text(start, end, "imdb_id", d->imdb, sizeof d->imdb))
    js_text(start, end, "id", d->imdb, sizeof d->imdb);
  snprintf(d->kind, sizeof d->kind, "%s", kind);

  { // genre: "Film · Action · Drama"
    const char *g = js_array(start, end, "genres");
    char g1[48] = "", g2[48] = "";
    if (g) {
      const char *f1 = js_end(g);
      (void)f1;
      // text elements: copy straight out of the array
      { const char *p = g; int k = 0;
        while (p && k < 2) {
          char tmp[48]; size_t n = 0;
          if (*p != '"') break;
          p++;
          while (*p && *p != '"' && n + 1 < sizeof tmp) tmp[n++] = *p++;
          tmp[n] = 0;
          // Normalise HERE, on the way in: CatItem's `genre` field is used by
          // several screens and all of them would show the raw spelling if the
          // normalisation lived in the drawing.
          if (k == 0) snprintf(g1, sizeof g1, "%s", disc_genre_label(tmp));
          else        snprintf(g2, sizeof g2, "%s", disc_genre_label(tmp));
          k++;
          p++;
          while (*p == ' ') p++;
          if (*p != ',') break;
          p++;
          while (*p == ' ') p++;
        } }
    }
    snprintf(d->genre, sizeof d->genre, "%s%s%s%s%s",
             strcmp(kind, "series") ? "Film" : "TV Show",
             g1[0] ? "  \xc2\xb7  " : "", g1,
             g2[0] ? "  \xc2\xb7  " : "", g2);
  }
  v[0] = 0;
  js_text(start, end, "releaseInfo", v, sizeof v);
  { char duration[24] = "";
    js_text(start, end, "runtime", duration, sizeof duration);
    // "2024–" becomes "2024": the dash of an ongoing series clutters the line.
    // BOTH spellings, because the catalogue is not all one addon: Cinemeta writes
    // the EN DASH ("2024\xe2\x80\x93", and "44 min" with a space), the TMDB addon
    // writes a plain ASCII hyphen ("2024-", and "44min" without one). Cutting only
    // the en dash left the hyphen on screen on every TMDB-sourced row.
    { char *tr = strstr(v, "\xe2\x80\x93"); if (!tr) tr = strchr(v, '-'); if (tr) *tr = 0; }
    snprintf(d->meta, sizeof d->meta, "%.20s%s%.20s", v,
             (v[0] && duration[0]) ? "  \xc2\xb7  " : "", duration); }
  js_text(start, end, "description", d->synopsis, sizeof d->synopsis);
  // DO NOT INVENT AN AGE RATING. There used to be a hard-coded `"14"` here, and the
  // effect was that EVERY title coming from the network showed a "14" badge —
  // Cinemeta does not send an age rating, and the fallback value became a constant
  // disguised as data, drawn with the same confidence as a real field.
  //
  // Empty is the honest answer: drawMetaBadge is already guarded by
  // `age_rating[0]` in the caller (detail.c), so the badge simply does not appear
  // while there is no value. What really fills it is TMDB's fact sheet in extras.c
  // (release_dates -> certification), which arrives later.
  d->age_rating[0] = 0;
  { double score = js_num(start, end, "imdbRating", 0.0);
    d->score = (int)(score * 10.0 + 0.5) / 1; }
  if (d->score > 99) d->score /= 10;
  return 1;
}

// Reads a catalogue (movie|series) from an addon and appends to the array.
static int readCatalog(const char *base, const char *kind, const char *id,
                       CatItem *output, int max, int count) {
  char url[900];
  char *body;
  const char *p;
  int n = 0;
  snprintf(url, sizeof url, "%s/catalog/%s/%s.json", base, kind, id);
  // 8 s and not 25: an addon that is down held one of the three threads for 25 s,
  // and its row delays ALL the following ones because the assembly walks in order.
  // It is the same lesson already recorded in the texture cache — there the timeout
  // dropped from 25 to 8 for the same reason, with two dead URLs blocking both
  // decode threads. A catalogue that does not answer in 8 s is not going to answer.
  body = net_download(url, 8);
  if (!body) return 0;
  p = js_array(body, NULL, "metas");
  while (p && n < max && n < count) {
    const char *f = js_end(p);
    if (ofMeta(p, f, kind, &output[n])) n++;
    p = js_next(f);
  }
  free(body);
  return n;
}

// --- home rows: catalogues declared by the addons ----------------------------
// This replaces the fixed PREF list of four catalogues. The web app has no fixed
// row: each row is a catalogue declared in an addon's manifest, and the
// order/visibility/name come from `homeCatalogPrefs`. See the long comment in
// catalog.h, which carries the sortAndFilterRowsInternal algorithm.

// A ceiling on the catalogues declared across ALL the addons.
//
// It was 64, and Xperience alone declares 64 — the loop's `nDecl < DECL_MAX`
// stopped there, and AIOStreams and Akashi TV never even had their manifests read.
// Neither their rows appeared on the home nor did their search catalogues exist:
// the app behaved as though the owner had installed a single addon. The symptom
// that arrived first was the search ("it doesn't search all the catalogues"), but
// the ceiling was cutting everything.
#define DECL_MAX 256
// How many items each row shows. The home draws at most MAX_CARDS (12) and
// fetching more is traffic nobody sees.
#define MAX_PER_ROW 12

static CatRow filtersBuilt[CAT_FILTER_MAX];
static int nRowsBuilt;

typedef struct {
  char key[192];      // homeCatalogKey:        <addonId>_<tipo>_<catalogoId>
  char disable[352];  // homeCatalogDisableKey: <base>_<tipo>_<catalogoId>_<nome>
  char title[96];
  char kind[8];
  char id[96];
  const char *base;
  // 1 when the catalogue accepts SEARCH. The manifest declares that in
  // `extra: [{name:"search"}]` (the new format) or `extraSupported: ["search"]`
  // (the old one) — the owner's addons use both.
  int searchable;
  char nameAddon[64];   // "Xperience", for the "from <addon>" line in the result
} Decl;

// The owner's preferences, the local equivalent of `homeCatalogPrefs`. A text file
// because the web app's is another process's localStorage — the same reason that
// already held for progress: that file belongs to whoever keeps it open, and
// writing to it from outside would corrupt its state.
//
//   order    <key>
//   off      <key-or-disable-key>
//   title    <key><TAB><title>
// 64 was not enough: an account with AIOMetadata alone declares 151 catalogues,
// and the saved order covers ALL of them. Cut at 64, the owner's order applied
// only to the start of the list and the rest fell back to the manifest's order —
// which showed up as "the home ignores what I configured".
#define PREF_MAX 256
static char prefOrder[PREF_MAX][192];   static int nPrefOrder;
static char prefOff[PREF_MAX][352];     static int nPrefOff;
static struct { char key[192], title[96]; } prefTitle[PREF_MAX];
static int nPrefTitle;

// 1 when the preferences came from the ACCOUNT. The account wins over the local
// file, as on the web, where the dedicated blob wins over the copy that travels
// inside the profile blob.
static int prefsFromAccount;

void disc_prefs_begin(void) {
  nPrefOrder = nPrefOff = nPrefTitle = 0;
  prefsFromAccount = 0;
}

void disc_prefs_add(const char *key, int enabled, const char *customTitle) {
  if (!key || !*key) return;
  // THE ORDER IS THE ARRIVAL ORDER. The server already sends the items in the
  // sequence the person chose (the `order` field is their index), so sorting
  // again here would only create a second opinion about the same thing.
  if (nPrefOrder < PREF_MAX) snprintf(prefOrder[nPrefOrder++], 192, "%s", key);
  if (!enabled && nPrefOff < PREF_MAX) snprintf(prefOff[nPrefOff++], 352, "%s", key);
  if (customTitle && *customTitle && nPrefTitle < PREF_MAX) {
    snprintf(prefTitle[nPrefTitle].key, 192, "%s", key);
    snprintf(prefTitle[nPrefTitle].title, 96, "%s", customTitle);
    nPrefTitle++;
  }
}

// Reading the preferences, for whoever assembles the rows.
//
// The account's ordered list mixes CATALOGUES and COLLECTIONS, and this file can
// only resolve catalogues — collections exist only for home.c. So the order is
// exposed raw instead of applied here: the home is what can match both kinds,
// and it needs the whole sequence, gaps and all.
int disc_prefs_n(void) { return nPrefOrder; }

const char *disc_prefs_key(int i) {
  return (i >= 0 && i < nPrefOrder) ? prefOrder[i] : "";
}

int disc_prefs_hidden(const char *key) {
  int i;
  if (!key || !*key) return 0;
  for (i = 0; i < nPrefOff; i++) if (!strcmp(prefOff[i], key)) return 1;
  return 0;
}

const char *disc_prefs_title(const char *key) {
  int i;
  if (!key || !*key) return NULL;
  for (i = 0; i < nPrefTitle; i++)
    if (!strcmp(prefTitle[i].key, key)) return prefTitle[i].title;
  return NULL;
}

void disc_prefs_end(void) {
  prefsFromAccount = nPrefOrder > 0;
  printf("[disc] account row prefs: %d ordered, %d disabled, %d renamed\n",
         nPrefOrder, nPrefOff, nPrefTitle);
  fflush(stdout);
}

static void readPrefs(void) {
  char path[600], line[600];
  FILE *f;
  // THE ACCOUNT WINS OVER THE FILE. Clearing here would wipe what the sync just
  // delivered, and the home would fall back to the manifest's order with nothing
  // to show for it.
  if (prefsFromAccount) return;
  nPrefOrder = nPrefOff = nPrefTitle = 0;
  if (!dirArtDisc[0]) return;
  snprintf(path, sizeof path, "%s/rows.txt", dirArtDisc);
  f = fopen(path, "r");
  if (!f) return;
  while (fgets(line, sizeof line, f)) {
    char *end = line + strlen(line);
    char *arg;
    while (end > line && (end[-1] == '\n' || end[-1] == '\r')) *--end = 0;
    if (!line[0] || line[0] == '#') continue;
    arg = strchr(line, ' ');
    if (!arg) continue;
    *arg++ = 0;
    while (*arg == ' ') arg++;
    if (!strcmp(line, "order") && nPrefOrder < PREF_MAX) {
      snprintf(prefOrder[nPrefOrder++], 192, "%s", arg);
    } else if (!strcmp(line, "off") && nPrefOff < PREF_MAX) {
      snprintf(prefOff[nPrefOff++], 352, "%s", arg);
    } else if (!strcmp(line, "title") && nPrefTitle < PREF_MAX) {
      char *tab = strchr(arg, '\t');
      if (!tab) continue;
      *tab++ = 0;
      snprintf(prefTitle[nPrefTitle].key, 192, "%s", arg);
      snprintf(prefTitle[nPrefTitle].title, 96, "%s", tab);
      nPrefTitle++;
    }
  }
  fclose(f);
  printf("[disc] row prefs: %d ordered, %d disabled, %d renamed\n",
         nPrefOrder, nPrefOff, nPrefTitle);
}

// The check is against TWO keys, as in the web app: whoever turns it off from the
// settings screen writes the disable key (which carries the base URL and the
// name), and whoever turns it off from the ordering writes the short key.
static int off(const Decl *d) {
  int i;
  for (i = 0; i < nPrefOff; i++)
    if (!strcmp(prefOff[i], d->key) || !strcmp(prefOff[i], d->disable)) return 1;
  return 0;
}

// formatCatalogRowTitle (js/ui/screens/home/homeUtils.js:62): a capital first
// letter and, if the name does NOT already end with the type's label, " - <type>".
// That is why the home shows "For You - Film" and not "for you".
static void formatTitle(const char *name, const char *kind, char *dst, size_t size) {
  const char *label = strcmp(kind, "series") ? "Film" : "Series";
  const char *raw    = strcmp(kind, "series") ? "Movie" : "Series";
  size_t ln = strlen(name), lr = strlen(label), lc = strlen(raw);
  int alreadyHas = 0;
  if (!name[0]) { snprintf(dst, size, "%s", label); return; }
  if (ln >= lr && !strcasecmp(name + ln - lr, label)) alreadyHas = 1;
  if (ln >= lc && !strcasecmp(name + ln - lc, raw))    alreadyHas = 1;
  if (alreadyHas) snprintf(dst, size, "%s", name);
  else       snprintf(dst, size, "%s - %s", name, label);
  if (dst[0] >= 'a' && dst[0] <= 'z') dst[0] = (char)(dst[0] - 32);
}

// The host of a URL, so it can be LOGGED without leaking a secret. An addon's
// path carries the debrid key and the owner's token; the host does not. A log
// nobody can show to anyone ends up being a log nobody reads.
static void hostOf(const char *url, char *dst, size_t size) {
  const char *a = strstr(url ? url : "", "://");
  const char *b;
  size_t n;
  a = a ? a + 3 : (url ? url : "");
  b = strchr(a, '/');
  n = b ? (size_t)(b - a) : strlen(a);
  if (n >= size) n = size - 1;
  memcpy(dst, a, n);
  dst[n] = 0;
}

// Le <base>/manifest.json e acrescenta os catalogos declarados.
static int readManifest(const char *base, Decl *output, int max) {
  char url[900], addonId[96] = "", name[96], kind[8], id[96];
  char host[128];
  char *body;
  const char *p, *end;
  int n = 0;
  snprintf(url, sizeof url, "%s/manifest.json", base);
  hostOf(url, host, sizeof host);
  body = net_download(url, 20);
  // WHY THIS IS LOGGED. An addon that does not answer, or answers something with
  // no "catalogs", produced exactly the same visible result as an addon that
  // simply has no catalogues: the home fell back to the packaged catalogue and
  // nobody could tell which of the two had happened. "0 catalogues declared" is
  // the total; these lines say WHO contributed zero, and why.
  if (!body) {
    printf("[disc] manifest %s: NO RESPONSE (network, TLS or 404)\n", host);
    fflush(stdout);
    return 0;
  }
  end = body + strlen(body);
  js_text(body, end, "id", addonId, sizeof addonId);
  // The id is REMEMBERED on the addon. This is the only place in the app where a
  // base and an id meet, and it is how the owner's collection sources — which
  // reference catalogues by addonId — find the address to fetch from.
  addons_note_id(base, addonId);
  p = js_array(body, end, "catalogs");
  // No `n < max` in the condition: the row array may fill up, but the sweep carries
  // on to the end of the manifest because the SEARCH catalogues tend to be at the
  // end of it (Xperience puts its at 603/604 of 605). What stops recording is the
  // `if (n < max)` inside.
  while (p) {
    const char *f = js_end(p);
    kind[0] = id[0] = name[0] = 0;
    js_text(p, f, "type", kind, sizeof kind);
    js_text(p, f, "id",   id,   sizeof id);
    js_text(p, f, "name", name, sizeof name);
    // With no type or no id there is no way to build the catalogue's URL; and a
    // catalogue that does not answer is worse than one row fewer.
    if (kind[0] && id[0]) {
      Decl local, *d;
      // The array is full: use a scratch Decl only to decide/register the search.
      d = (n < max) ? &output[n] : &local;
      memset(d, 0, sizeof *d);
      d->base = base;
      // SEARCH: it looks for "search" inside THIS catalogue's `extra`/
      // `extraSupported` block (the range [p,f) is its own object, so it does not
      // leak into its neighbour).
      //
      // Films and series only. Akashi declares search on `event` and `channel` too,
      // and AIOStreams on `collections` — types this app has no screen for. Querying
      // them would be traffic that turns into nothing.
      { const char *ex = strstr(p, "\"extra\"");
        if (!ex || ex >= f) ex = strstr(p, "\"extraSupported\"");
        if (ex && ex < f) {
          const char *sc = strstr(ex, "\"search\"");
          if (sc && sc < f) d->searchable = 1;
        }
        if (strcmp(kind, "movie") && strcmp(kind, "series")) d->searchable = 0;
        // Register HERE, and not afterwards by sweeping the Decl array.
        //
        // Xperience declares 605 catalogues and puts its two SEARCH ones in the
        // LAST two positions (603 and 604). Any ceiling on the home's row array —
        // 64, 256, whatever number — cuts exactly the catalogues the search cares
        // about. The two subjects have no reason to share a limit: it is 16 search
        // targets against hundreds of rows.
        if (d->searchable) {
          char label[96], nameAddon[96] = "";
          js_text(body, end, "name", nameAddon, sizeof nameAddon);
          formatTitle(name, kind, label, sizeof label);
          disc_target_search(base, kind, id, label,
                          nameAddon[0] ? nameAddon
                                       : (addonId[0] ? addonId : "addon"));
        } }
      snprintf(d->kind, sizeof d->kind, "%s", kind);
      snprintf(d->id,   sizeof d->id,   "%s", id);
      snprintf(d->key, sizeof d->key, "%s_%s_%s",
               addonId[0] ? addonId : base, kind, id);
      snprintf(d->disable, sizeof d->disable, "%s_%s_%s_%s", base, kind, id, name);
      formatTitle(name, kind, d->title, sizeof d->title);
      // A readable addon name, for the "from <addon>" line under the results row's
      // title. The manifest has `name`; without it the id stands in.
      { char an[96] = "";
        js_text(body, end, "name", an, sizeof an);
        snprintf(d->nameAddon, sizeof d->nameAddon, "%s",
                 an[0] ? an : (addonId[0] ? addonId : "addon")); }
      if (n < max) n++;
    }
    p = js_next(f);
  }
  { size_t bytes = strlen(body);
    const char *has = js_array(body, end, "catalogs");
    if (!has)
      printf("[disc] manifest %s: %u bytes, NO \"catalogs\" array\n",
             host, (unsigned)bytes);
    else
      printf("[disc] manifest %s: %u bytes, %d catalogue(s) usable\n",
             host, (unsigned)bytes, n);
    fflush(stdout); }
  free(body);
  return n;
}

// --- READING THE CATALOGUES IN PARALLEL --------------------------------------
//
// It used to be up to 16 GETs IN SERIES, with a 25 s timeout each. Measured on the
// Mac: 8.7 s between the first row appearing (4.0 s) and the catalogue being
// complete (12.8 s), and on the TV it is worse. They are INDEPENDENT requests —
// nothing in readCatalog/ofMeta touches shared state (the genre table is const),
// and net_download already runs on three threads in the search.
//
// THE ROW ORDER HAS TO BE PRESERVED: it comes from art/rows.txt and is the owner's
// preference. So the threads each write into THEIR OWN bucket and whoever
// assembles walks in order, waiting for bucket k to be ready. The result is the
// same order as before, with the time of the LARGEST request instead of the SUM.
#define CAT_THREADS 3

typedef struct {
  const Decl *d;
  CatItem items[MAX_PER_ROW];
  int  n;
  int  ready;
  // The assembly rebuilds the catalogue from every ready bucket on each pass,
  // so a row would otherwise be logged once per pass. This keeps one line per
  // row, which is what the log is read for.
  int  logged;
} TaskCat;

static TaskCat *tasks;
static int  nTasks, nextTask;
static pthread_mutex_t catLock = PTHREAD_MUTEX_INITIALIZER;
// Signalled for every bucket that becomes ready. It replaces the SDL_Delay(10)
// the assembly used to wait for row k with: that loop slept even when the row
// had already arrived and — worse — only ever looked at row k, so one slow row
// held back every row already finished behind it.
static pthread_cond_t  catCond = PTHREAD_COND_INITIALIZER;

static void *threadCatalog(void *u) {
  (void)u;
  for (;;) {
    int mine, got;
    const Decl *d;
    pthread_mutex_lock(&catLock);
    if (nextTask >= nTasks) { pthread_mutex_unlock(&catLock); return NULL; }
    mine = nextTask++;
    d = tasks[mine].d;
    pthread_mutex_unlock(&catLock);

    got = readCatalog(d->base, d->kind, d->id, tasks[mine].items,
                      MAX_PER_ROW, MAX_PER_ROW);

    pthread_mutex_lock(&catLock);
    tasks[mine].n = got;
    tasks[mine].ready = 1;
    pthread_cond_signal(&catCond);
    pthread_mutex_unlock(&catLock);
  }
}

// They were read one at a time, on the assembly thread, before any catalogue:
// with the owner's addons that is several GETs in series, and NOTHING else
// happened until the last one answered. They are independent requests against
// different hosts, and readManifest touches no shared state without a lock:
// disc_target_search already takes searchLock, and addons_note_id writes the
// entry matched by `base`, which is unique per addon.
//
// THE ADDON ORDER HAS TO BE PRESERVED: it is what defines the default row order
// before preferences are applied. So each thread fills its OWN bucket and the
#define MANIFEST_THREADS 4

typedef struct {
  const char *base;
  Decl *out;    // DECL_MAX entries; see the note on the size in manifestsStart
  int n;
} TaskManifest;

static TaskManifest *manTasks;
static int nManTasks, nextManTask, nManThreads;
static pthread_t manThreads[MANIFEST_THREADS];
static pthread_mutex_t manLock = PTHREAD_MUTEX_INITIALIZER;

static void *threadManifest(void *u) {
  (void)u;
  for (;;) {
    int mine;
    pthread_mutex_lock(&manLock);
    if (nextManTask >= nManTasks) { pthread_mutex_unlock(&manLock); return NULL; }
    mine = nextManTask++;
    pthread_mutex_unlock(&manLock);
    if (manTasks[mine].out)
      manTasks[mine].n = readManifest(manTasks[mine].base, manTasks[mine].out,
                                      DECL_MAX);
  }
}

// Fires the manifest reads off and RETURNS. The caller goes on with other work
// and calls manifestsJoin afterwards.
static void manifestsStart(void) {
  int i, q;
  nManTasks = 0; nextManTask = 0; nManThreads = 0;
  manTasks = calloc((size_t)(addons_n() > 0 ? addons_n() : 1), sizeof(TaskManifest));
  if (!manTasks) return;
  for (i = 0; i < addons_n(); i++) {
    if (!addons_has_catalog(i)) continue;
    // The bucket holds DECL_MAX and not a share of the total because ONE addon
    // can declare more catalogues than the whole array (Xperience declares
    // 605), and the cut has to happen at the join, in addon order, exactly as
    // it did when the reading was serial. That is ~200 KB per addon, alive only
    // while the manifests are being read.
    manTasks[nManTasks].base = addons_base(i);
    manTasks[nManTasks].out  = malloc(sizeof(Decl) * DECL_MAX);
    nManTasks++;
  }
  for (q = 0; q < MANIFEST_THREADS && q < nManTasks; q++)
    if (pthread_create(&manThreads[nManThreads], NULL, threadManifest, NULL) == 0)
      nManThreads++;
}

// Waits for the manifests and joins the buckets IN ADDON ORDER, applying the
// same ceiling the serial loop applied. Returns how many Decl it wrote.
static int manifestsJoin(Decl *output, int max) {
  int n = 0, k, q;
  if (!manTasks) return 0;
  // With NO thread at all (pthread_create failed on every one), read serially
  // on this thread: worse performance, same result.
  if (!nManThreads && nManTasks > 0) threadManifest(NULL);
  for (q = 0; q < nManThreads; q++) pthread_join(manThreads[q], NULL);
  for (k = 0; k < nManTasks; k++) {
    int got = manTasks[k].n;
    if (got > max - n) got = max - n;
    if (got > 0) {
      memcpy(output + n, manTasks[k].out, sizeof(Decl) * (size_t)got);
      n += got;
    }
    free(manTasks[k].out);
  }
  free(manTasks); manTasks = NULL;
  nManTasks = 0; nManThreads = 0; nextManTask = 0;
  return n;
}

// --- "CONTINUE WATCHING": THE TWO SOURCES, MERGED ----------------------------
//
// THE DEFECT. This row used to be built exclusively from Trakt:
//     nResume = trakt_resume(lote, 8);
// so with Trakt linked, the progress of the NUVIO ACCOUNT — which is what
// arrives from the person's phone, lands in progress.txt through sync.c, and is
// the only record this TV keeps of what was watched HERE — was simply never
// read. Both halves of the report are that one line:
//   "I watch on my phone and it does not show up in Continue watching on the TV"
//     -> the record existed on disk and nothing ever looked at it;
//   "films and series I never watched appear there"
//     -> those came from Trakt's /sync/playback, which keeps every resume point
//        any Trakt client ever recorded, and this path applied none of the
//        1%-to-90% limits the local side always did.
//
// WHY MERGING IS THE RIGHT ANSWER rather than picking a source: the two answer
// the same question about DIFFERENT universes. Trakt knows what was watched in
// any Trakt client; the Nuvio account knows what was watched on Nuvio devices.
// Picking one throws away half the history, in whichever direction. Dedupe by
// work and order by the most recent instant, and the row becomes what was
// actually being watched, wherever it was watched.
//
// WHEN THE TWO DISAGREE ABOUT ONE TITLE, THE MORE RECENT WINS — and both sides
// now know their instant: the Trakt item from `paused_at`, the local one from
// the last column of progress.txt. An entry from before that column existed
// reports 0 and orders after everything that knows its own instant, which is
// the honest placement: unknown is not the same as old.
#define CONT_MAX 8

// The window the local path always had and the Trakt one never did. Below 1% it
// was not started; from 90% on it is finished. "Continue" is neither.
static int inProgress(int pct) { return pct >= 1 && pct < 90; }

// 1 when the two items are the SAME WORK. Only the part before the ':' counts:
// "tt123:4:9" and "tt123:1:2" are two episodes of one series, and the row shows
// the series once.
static int sameWork(const char *a, const char *b) {
  size_t la, lb;
  const char *ca, *cb;
  if (!a || !b || !*a || !*b) return 0;
  ca = strchr(a, ':'); cb = strchr(b, ':');
  la = ca ? (size_t)(ca - a) : strlen(a);
  lb = cb ? (size_t)(cb - b) : strlen(b);
  return la && la == lb && !strncmp(a, b, la);
}

// "Continue watching" from the LOCAL progress record, in the same shape
// trakt_resume returns: id (with the episode composed in for a series), kind,
// percentage, season/episode — and the art comes from Cinemeta through the same
// decorator. Most recent first, which is the order cat_progress_read gives.
static int resumeLocal(CatItem *output, int max) {
  static CatProgress regs[CAT_PROGRESS_MAX];
  int k, i, n = 0;
  k = cat_progress_read(regs, CAT_PROGRESS_MAX);
  for (i = 0; i < k && n < max; i++) {
    const CatProgress *r = &regs[i];
    CatItem *d;
    int pct, j, repeated = 0;
    if (r->durationSeg < 60.0) continue;
    pct = (int)(100.0 * r->posSeg / r->durationSeg);
    if (!inProgress(pct)) continue;
    // A series with several episodes recorded enters ONCE, at the most recent —
    // and the list is already ordered, so the first one seen is that one.
    for (j = 0; j < n; j++)
      if (sameWork(output[j].imdb, r->imdb)) { repeated = 1; break; }
    if (repeated) continue;
    d = &output[n];
    memset(d, 0, sizeof *d);
    d->progress = pct;
    d->remainingMin = (int)((r->durationSeg - r->posSeg) / 60.0 + 0.5);
    // The instant is already in the record: stamping it here saves a lookup
    // later and, more to the point, survives the compaction inside
    // trakt_decorate_batch. See resumedMs in catalog.h.
    d->resumedMs = r->lastWatchedMs;
    if (r->episode > 0) {
      d->season = r->season;
      d->episode = r->episode;
      snprintf(d->imdb, sizeof d->imdb, "%s:%d:%d", r->imdb,
               r->season ? r->season : 1, r->episode);
      snprintf(d->kind, sizeof d->kind, "series");
    } else {
      snprintf(d->imdb, sizeof d->imdb, "%s", r->imdb);
      snprintf(d->kind, sizeof d->kind, "movie");
    }
    n++;
  }
  n = trakt_decorate_batch(output, n);
  if (n) printf("[disc] continue watching, local: %d\n", n);
  return n;
}

// One candidate for the row, with what is known about WHEN it happened. `ord`
// keeps the position its own source gave it, so items with no instant keep that
// relative order instead of being shuffled by an unstable sort.
typedef struct { CatItem *item; long long ms; int ord; } Cand;

static int candNewestFirst(const void *a, const void *b) {
  const Cand *x = a, *y = b;
  if (x->ms > y->ms) return -1;
  if (x->ms < y->ms) return 1;
  return x->ord - y->ord;
}

static int buildResume(CatItem *output, int max) {
  // static: two batches of 8 CatItem are over 50 KB, and this runs once, on one
  // thread — the same reason the Decl array below is static.
  static CatItem fromTrakt[CONT_MAX], fromAccount[CONT_MAX];
  static Cand joined[CONT_MAX * 2];
  int nT, nL, nJ = 0, i, j, n = 0, dropped = 0;

  nT = trakt_active() ? trakt_resume(fromTrakt, CONT_MAX) : 0;
  nL = resumeLocal(fromAccount, CONT_MAX);

  for (i = 0; i < nT && nJ < CONT_MAX * 2; i++) {
    if (!inProgress(fromTrakt[i].progress)) { dropped++; continue; }
    joined[nJ].item = &fromTrakt[i];
    joined[nJ].ms = fromTrakt[i].resumedMs;
    joined[nJ].ord = nJ;
    nJ++;
  }
  for (i = 0; i < nL && nJ < CONT_MAX * 2; i++) {
    joined[nJ].item = &fromAccount[i];
    joined[nJ].ms = fromAccount[i].resumedMs;
    joined[nJ].ord = nJ;
    nJ++;
  }

  if (nJ > 1) qsort(joined, (size_t)nJ, sizeof *joined, candNewestFirst);

  // Deduplicate AFTER ordering, so the copy that survives is the most recent
  // one — which is also the one whose season and episode are right for a series
  // the person watched on two devices.
  for (i = 0; i < nJ && n < max; i++) {
    int repeated = 0;
    for (j = 0; j < n; j++)
      if (sameWork(output[j].imdb, joined[i].item->imdb)) { repeated = 1; break; }
    if (repeated) continue;
    output[n++] = *joined[i].item;
  }
  printf("[disc] continue watching: %d trakt + %d account -> %d shown"
         " (%d outside 1-90%%)\n", nT, nL, n, dropped);
  return n;
}

static void *build(void *u) {
  // The batch grows too: it used to be sized by CAT_MAX and so inherited the same
  // arbitrary ceiling.
  int cap = 128;
  CatItem *lote = malloc(sizeof(CatItem) * (size_t)cap);
  int n = 0;
  int nResume = 0, nSocial = 0;
  (void)u;
  if (!lote) { searching = 0; return NULL; }

  // "Continue watching" comes FIRST, and from BOTH sources — Trakt and the Nuvio
  // account's own progress. See the header on buildResume. The home uses the
  // catalogue's first positions in that row, so the order here is what decides
  // what appears there, and the history has to beat the recommendations.
  mark("build: start");
  // THE MANIFESTS LEAVE FIRST AND RUN UNDERNEATH THE TRAKT CALLS.
  //
  // They depend on nothing from Trakt, and Trakt is expensive: /sync/playback
  // and /sync/history are two GETs of up to 25 s each, and the social feed is
  // one more. While that ran, the network sat idle as far as the addons were
  // concerned. Now the two happen at once and the join below usually waits for
  // nothing at all.
  //
  // The search-target reset comes along because every searchable catalogue
  // registers itself INSIDE readManifest — clearing it afterwards would erase
  // what the threads had just written. readPrefs does NOT move: it reads the
  // owner's preference and the account can arrive between one moment and the
  // next, so it stays exactly where it was, next to the ordering that uses it.
  disc_targets_search_reset();
  manifestsStart();
  nResume = buildResume(lote, 8);
  n += nResume;
  mark("continue watching, both sources");
  // The official social feed is a row of its own, right after the return to what
  // was being watched. It comes early so as not to depend on the addons' manifests,
  // and it uses the same Trakt credential already loaded.
  // With the row turned off in Settings there is nobody to show it to, and the
  // feed is a third Trakt GET on the critical path — so it is not fetched at
  // all, instead of fetched and dropped later.
  nSocial = settings_social_row() ? trakt_social(lote + n, 8) : 0;
  n += nSocial;
  mark("trakt friend activity");
  // Trakt's history is the home's FIRST row and arrives ~1.6 s before the
  // manifests. Publishing here puts content on screen at that moment instead of
  // holding everything back to the end.
  // It builds straight into filtersBuilt: the local `filter` array only exists
  // further down, and creating one here just to copy from would be wasted work.
  if (n > 0 && partialAllowed()) {
    int nf = 0;
    if (nResume > 0) {
      CatRow *f0 = &filtersBuilt[nf++];
      memset(f0, 0, sizeof *f0);
      snprintf(f0->key,  sizeof f0->key,  "continue_watching");
      snprintf(f0->title, sizeof f0->title, "Continue watching");
      snprintf(f0->kind,   sizeof f0->kind,   "movie");
      f0->start = 0; f0->n = nResume;
    }
    if (nSocial > 0) {
      CatRow *fs = &filtersBuilt[nf++];
      memset(fs, 0, sizeof *fs);
      snprintf(fs->key, sizeof fs->key, "social_activity");
      snprintf(fs->title, sizeof fs->title, "Friends watching");
      snprintf(fs->kind, sizeof fs->kind, "social");
      fs->start = nResume; fs->n = nSocial;
    }
    nRowsBuilt = nf;
    cat_set_all(lote, n, filtersBuilt, nf);
    mark("continue watching on screen");
  }
#define ENSURES(count) do { \
    if (n + (count) > cap) { \
      int newCap = cap; \
      CatItem *larger; \
      while (newCap < n + (count)) newCap *= 2; \
      larger = realloc(lote, sizeof(CatItem) * (size_t)newCap); \
      if (larger) { lote = larger; cap = newCap; } \
    } } while (0)

  // The rows come from the CATALOGUES declared in the addons' manifests, and not
  // from a fixed list. The order, what is left out and the names follow the web
  // app's algorithm (sortAndFilterRowsInternal), with the preferences read from
  // art/rows.txt.
  {
    // static: 256 entries pass 200 KB, and that does not fit comfortably on a
    // thread's stack. build() runs once and on a single thread, so there is no
    // reentrancy for this to break.
    static Decl decls[DECL_MAX];
    int nDecl = 0, k;
    CatRow filter[CAT_FILTER_MAX];
    int nFilter = 0;
    // Row 0 is "Continue watching", which was assembled above. It is SYNTHETIC: it
    // is not in the web app's order and cannot be switched off by key — in the app
    // it exists whenever there is progress.
    if (nResume > 0) {
      CatRow *f0 = &filter[nFilter++];
      memset(f0, 0, sizeof *f0);
      snprintf(f0->key,  sizeof f0->key,  "continue_watching");
      snprintf(f0->title, sizeof f0->title, "Continue watching");
      snprintf(f0->kind,   sizeof f0->kind,   "movie");
      f0->start = 0; f0->n = nResume;
    }
    if (nSocial > 0) {
      CatRow *fs = &filter[nFilter++];
      memset(fs, 0, sizeof *fs);
      snprintf(fs->key, sizeof fs->key, "social_activity");
      snprintf(fs->title, sizeof fs->title, "Friends watching");
      snprintf(fs->kind, sizeof fs->kind, "social");
      fs->start = nResume; fs->n = nSocial;
    }

    readPrefs();
    // The manifests have been in flight since the top of build(); this only
    // waits for whatever has not landed yet. Every addon with catalogues is
    // read, even with the array full: it is the cut at the JOIN that decides
    // what gets in, not the reading. With the ceiling applied at READ time the
    // next addon's manifest was not even downloaded, and AIOStreams and Akashi
    // TV were invisible to the whole app just because Xperience, read earlier,
    // declares 605 catalogues — including invisible to SEARCH, which registers
    // during the read and does not depend on this array.
    nDecl = manifestsJoin(decls, DECL_MAX);
    printf("[disc] %d catalogues declared by the addons\n", nDecl);

    // SEARCH TARGETS. They are independent of the order/filtering of the home's
    // ROWS: a catalogue may be disabled on the home and still be good to search
    // (Akashi only has search, no row worth having).
    printf("[disc] %d search targets\n", disc_search_n_targets());
    mark("manifests read");

    // ensureOrderKeysWithPrefs: the saved order first, and the NEW keys appended at
    // the end. A catalogue the addon has started declaring today goes last, not in
    // the middle — which is what stops the home reorganising itself.
    {
      int order[DECL_MAX];
      int nOrder = 0, j;
      char watched[DECL_MAX];
      memset(watched, 0, sizeof watched);
      for (k = 0; k < nPrefOrder; k++)
        for (j = 0; j < nDecl; j++)
          if (!watched[j] && !strcmp(decls[j].key, prefOrder[k])) {
            order[nOrder++] = j; watched[j] = 1; break;
          }
      for (j = 0; j < nDecl; j++) if (!watched[j]) order[nOrder++] = j;

      int markedFirst = 0;
      // STAGE 1 — choose and ORDER the rows that will be read. The filters (off,
      // custom title) are local and cheap; doing this first leaves the threads with
      // only the expensive part, which is the network.
      nTasks = 0; nextTask = 0;
      tasks = calloc(CAT_FILTER_MAX, sizeof(TaskCat));
      for (k = 0; k < nOrder && nTasks < CAT_FILTER_MAX; k++) {
        Decl *d = &decls[order[k]];
        int t;
        if (off(d)) continue;
        // customTitles ganha do nome do manifesto.
        for (t = 0; t < nPrefTitle; t++)
          if (!strcmp(prefTitle[t].key, d->key)) {
            snprintf(d->title, sizeof d->title, "%s", prefTitle[t].title);
            break;
          }
        if (tasks) tasks[nTasks++].d = d;
      }

      // STAGE 2 — CAT_THREADS working the queue. If the calloc fails or there is
      // nothing to read, nTasks stays 0 and the assembly loop below does not run:
      // the home carries on with what has already been published, with no error
      // path of its own.
      { pthread_t threads[CAT_THREADS];
        int created = 0, q;
        for (q = 0; q < CAT_THREADS && nTasks > 0; q++)
          if (pthread_create(&threads[created], NULL, threadCatalog, NULL) == 0) created++;
        // With NO threads at all (pthread_create failed on all of them), read in
        // series on this very thread: worse performance, same result. Better than an
        // empty home.
        if (!created && nTasks > 0) threadCatalog(NULL);

        // ETAPA 3 — PUBLISH AS IT ARRIVES, not in reading order.
        //
        // The assembly used to wait for bucket k in order to publish bucket k.
        // One slow row held back every row already finished behind it: with an
        // 8 s ceiling per catalogue, a bad addon hid the rest of the home for a
        // full 8 s.
        //
        // Now any bucket that becomes ready wakes the assembly, and it REBUILDS
        // the catalogue from EVERY ready bucket, walking preference order and
        // skipping the ones that have not landed. What that does on screen is
        // something the home already knows how to handle: row 5 shows up before
        // row 3, and when row 3 arrives it goes in ABOVE row 5. home.c re-finds
        // the focus and the scroll by the row's KEY on every publish, precisely
        // because "a new row can enter in the middle".
        //
        // Rebuilding rather than appending: the rows are windows (start,n) into
        // one array, so inserting in the middle means rewriting the array from
        // there on. It costs a memcpy of ~100 items on the discovery thread,
        // not on the drawing one.
        int nBase = n, nFilterBase = nFilter;
        int done = 0;
        Uint32 lastPublish = 0;
        while (done < nTasks) {
          int anyReady;
          pthread_mutex_lock(&catLock);
          for (;;) {
            anyReady = 0;
            for (k = 0; k < nTasks; k++) if (tasks[k].ready) anyReady++;
            if (anyReady > done) break;
            pthread_cond_wait(&catCond, &catLock);
          }
          pthread_mutex_unlock(&catLock);
          done = anyReady;

          // REBUILD from scratch, starting after what Trakt already put in the
          // array.
          n = nBase; nFilter = nFilterBase;
          for (k = 0; k < nTasks && nFilter < CAT_FILTER_MAX; k++) {
            const Decl *d = tasks[k].d;
            int got, isReady;
            pthread_mutex_lock(&catLock);
            isReady = tasks[k].ready;
            got = tasks[k].n;
            pthread_mutex_unlock(&catLock);
            if (!isReady) continue;
            if (!got) continue;   // an empty row does not become a dangling title
            ENSURES(MAX_PER_ROW + 2);
            if (got > cap - n) got = cap - n;
            if (got <= 0) continue;
            memcpy(lote + n, tasks[k].items, sizeof(CatItem) * (size_t)got);
            {
              CatRow *f = &filter[nFilter++];
              memset(f, 0, sizeof *f);
              snprintf(f->key,  sizeof f->key,  "%s", d->key);
              snprintf(f->title, sizeof f->title, "%s", d->title);
              snprintf(f->kind,   sizeof f->kind,   "%s", d->kind);
              snprintf(f->base,   sizeof f->base,   "%s", d->base ? d->base : "");
              snprintf(f->catId,  sizeof f->catId,  "%s", d->id);
              f->start = n; f->n = got;
            }
            n += got;
            // ARRIVAL, not final position. The index a row lands at changes on
            // every pass — a later row inserts above it — so printing one here
            // would be a different number each time for the same row. The
            // ordered list is printed once, after the loop.
            if (!tasks[k].logged) {
              tasks[k].logged = 1;
              printf("[disc] row ready: %s (%d)\n", d->title, got);
            }
          }
          nRowsBuilt = nFilter;
          memcpy(filtersBuilt, filter, sizeof(CatRow) * (size_t)nFilter);

          // ONE PUBLISH PER FRAME, at most.
          //
          // Two buckets finishing together gave two publishes back to back, and
          // cat_set_all keeps only ONE garbage block: the second would free a
          // block the drawing thread may still be walking. It also re-reads
          // progress.txt on every call. Nothing is lost by skipping one: the
          // next bucket publishes what this one left, and the final publish
          // comes after the loop.
          //
          // Only publishes in pieces when the screen is showing the PACKAGED
          // catalogue — see partialAllowed. Over anything of the owner's (the
          // cache, or a complete build from earlier this session) it would be a
          // visible regression: 16 rows become 1.
          { Uint32 now = SDL_GetTicks();
            if (partialAllowed() && (done == nTasks || now - lastPublish >= 16)) {
              lastPublish = now;
              cat_set_all(lote, n, filtersBuilt, nRowsBuilt);
              // A flag of its own: `nFilter == 1` never happens here because
              // "Continue watching" already took position 0.
              if (!markedFirst && nFilter > nFilterBase) {
                markedFirst = 1;
                mark("first network row on screen");
              }
            } }
        }
        for (q = 0; q < created; q++) pthread_join(threads[q], NULL);
        // THE FINAL ORDER, as one list. This is what gets compared between two
        // versions to prove the owner's preference order did not change; the
        // arrival lines above are in NETWORK order, which differs every launch.
        for (k = nFilterBase; k < nFilter; k++)
          printf("[disc] row %d: %s (%d)\n", k, filter[k].title, filter[k].n);
      }
      free(tasks); tasks = NULL; nTasks = 0;
      nRowsBuilt = nFilter;
      memcpy(filtersBuilt, filter, sizeof(CatRow) * (size_t)nFilter);
    }
  }

  // The watchlist and the collection go in AFTER the recommendations, not before.
  // The home uses the catalogue's FIRST positions in its rows; with Trakt's lists
  // in front (and they run to over 60 items each) the rows became the whole
  // watchlist and the recommendations never appeared. The library sweeps the whole
  // catalogue looking for the marks, so for it their position makes no difference.
  ENSURES(400);
  n += trakt_list("watchlist",  lote + n, cap - n);
  ENSURES(400);
  n += trakt_list("collection", lote + n, cap - n);
#undef ENSURES

  if (n) {
    cat_set_all(lote, n, filtersBuilt, nRowsBuilt);
    cat_cache_replaced();
    completeOnScreen = 1;
    mark("network catalog published");
    printf("[disc] catalog built with %d titles\n", n);
    // It only writes the COMPLETE result, not the partial publications: a cache
    // with three rows would make the next opening start half-built and only
    // complete when the network answered — exactly what the cache exists to avoid.
    cat_write_cache(dirArtDisc);
  } else {
    printf("[disc] nothing came from the network; using the packaged catalog\n");
  }
  fflush(stdout);
  free(lote);
  searching = 0;
  return NULL;
}

void disc_start(void) {
  if (searching) return;
  searching = 1;
  if (pthread_create(&thread, NULL, build, NULL) != 0) searching = 0;
  else pthread_detach(thread);
}

// A REQUEST to rebuild, not the rebuild itself.
//
// The case this fixes: the addon list comes from the ACCOUNT and arrives with the
// sync, well after the home has already been assembled. The first build ran with
// zero addons, read zero manifests, found zero catalogues and fell back to the
// packaged catalogue — and nothing ever reconsidered it. The owner saw the
// packager's catalogue instead of their own, on every launch, because the list is
// not kept between sessions either.
//
// It stays a REQUEST because the first build may still be running when the sync
// answers: disc_start gives up silently while one is in flight, and the call
// would be lost in exactly the race this code exists to resolve.
static volatile int rebuildWanted;

void disc_rebuild(void) { rebuildWanted = 1; }

void disc_step(void) {
  if (!rebuildWanted || searching) return;
  rebuildWanted = 0;
  printf("[disc] rebuilding the rows with the account's addons\n");
  fflush(stdout);
  disc_start();
}

// --- episodes on demand ------------------------------------------------------

// AN LRU CACHE OF THE SERIES' /meta.
//
// ONE Cinemeta response carries ALL the seasons: `videos` comes whole and the
// per-season filtering happens down here, for free. Even so, every season change
// re-downloaded the whole body — on a long series that is hundreds of kilobytes of
// JSON per pill pressed, and that is what the owner felt as "it takes ages to
// update when you change season".
//
// Keeping only the last series meant going back to the previous title repeated the
// whole transfer. Four responses cover normal back-and-forth navigation without
// letting memory use grow without limit.
#define META_CACHE_N 4
static struct { char id[24]; char *body; unsigned usage; } metaCache[META_CACHE_N];
static unsigned metaClock;
static pthread_mutex_t metaLock = PTHREAD_MUTEX_INITIALIZER;

static char *metaCacheGet(const char *id) {
  char *r = NULL;
  pthread_mutex_lock(&metaLock);
  for (int i = 0; i < META_CACHE_N; i++)
    if (metaCache[i].body && !strcmp(metaCache[i].id, id)) {
      metaCache[i].usage = ++metaClock;
      r = strdup(metaCache[i].body); /* the thread works on a stable copy */
      break;
    }
  pthread_mutex_unlock(&metaLock);
  return r;
}

static void metaCacheStore(const char *id, const char *body) {
  int slot = 0;
  char *copy = strdup(body);
  if (!copy) return;
  pthread_mutex_lock(&metaLock);
  for (int i = 0; i < META_CACHE_N; i++) {
    if (metaCache[i].body && !strcmp(metaCache[i].id, id)) { slot = i; break; }
    if (!metaCache[i].body || metaCache[i].usage < metaCache[slot].usage) slot = i;
  }
  free(metaCache[slot].body);
  metaCache[slot].body = copy;
  metaCache[slot].usage = ++metaClock;
  snprintf(metaCache[slot].id, sizeof metaCache[slot].id, "%s", id);
  pthread_mutex_unlock(&metaLock);
}

// THE EPISODE CARDS' IMDb SCORES.
//
// They do NOT come from Cinemeta, and that is the trap: `videos[].rating` is present
// and reads "0" for a great many series — every Fallout, Game of Thrones and Stranger
// Things episode does — so a port that reads it looks like it works and quietly shows
// nothing. The web app tries this API FIRST and only falls back to the addon's field
// (fetchSeriesRatingsBySeason, metaDetailsScreen.js), which is why the same Fallout
// page shows 8.2 there and nothing here.
//
// <base>/api/shows/<tmdbId>/season-ratings returns a BARE ARRAY of seasons, each with
// an `episodes` array carrying `episode_number` and `imdb_rating` (`vote_average`
// repeats it). Checked against Fallout 106379: S1E1 "The End" = 8.2 over 23197 votes.
//
// It runs AFTER publishEpisodes, so the row is already on screen and the numbers
// arrive into it — cat_set_ep_score fills them in place rather than republishing the
// list, which would restart every thumbnail.
#ifndef NV_IMDB_RATINGS
#define NV_IMDB_RATINGS ""
#endif
static void episodeScores(long tmdb, int targetItem) {
  char url[512], *body;
  if (tmdb <= 0 || !NV_IMDB_RATINGS[0]) return;
  { const char *base = NV_IMDB_RATINGS;
    size_t n = strlen(base);
    // The property is written with and without a trailing slash across checkouts.
    snprintf(url, sizeof url, "%s%sapi/shows/%ld/season-ratings",
             base, (n && base[n - 1] == '/') ? "" : "/", tmdb); }
  body = net_download(url, 12);
  if (!body) return;
  { int filled = 0, seen = 0;
    const char *season = js_root_array(body);
    while (season) {
      const char *sEnd = js_end(season);
      int sn = (int)js_num(season, sEnd, "season_number", -1);
      const char *ep = js_array(season, sEnd, "episodes");
      while (ep) {
        const char *eEnd = js_end(ep);
        int en = (int)js_num(ep, eEnd, "episode_number", -1);
        // imdb_rating is the one the badge names; vote_average carries the same
        // number and is read only when the first is absent.
        double v = js_num(ep, eEnd, "imdb_rating", -1.0);
        if (v < 0.0) v = js_num(ep, eEnd, "vote_average", -1.0);
        if (sn >= 0 && en > 0 && v > 0.0 && v <= 10.0) {
          seen++;
          filled += cat_set_ep_score(targetItem, sn, en, (int)(v * 10.0 + 0.5));
        }
        ep = js_next(eEnd);
      }
      season = js_next(sEnd);
    }
    printf("[disc] episode scores: %d of %d matched an episode on screen\n",
           filled, seen); }
  free(body);
}

// It publishes the critical part before any optional enrichment. That way the
// episodes row appears after the first response, without waiting for the two TMDB
// round trips used for the cast's photo and character.
static int publishEpisodes(const char *body, int targetItem, const char *title) {
#define VIDEOS_MAX 600
  CatEp *eps = malloc(sizeof(CatEp) * VIDEOS_MAX);
  int n = 0;
  if (!eps) return 0;
  const char *p = js_array(body, NULL, "videos");
  while (p && n < VIDEOS_MAX) {
    const char *f = js_end(p);
    int t = (int)js_num(p, f, "season", -1);
    if (t > 0) {
      CatEp *e = &eps[n];
      char d[24] = "";
      memset(e, 0, sizeof *e);
      e->season = t;
      e->episode = (int)js_num(p, f, "episode", 0);
      js_text(p, f, "name", e->name, sizeof e->name);
      js_text(p, f, "overview", e->synopsis, sizeof e->synopsis);
      js_text(p, f, "thumbnail", e->thumb, sizeof e->thumb);
      js_text(p, f, "released", d, sizeof d);
      disc_date_long(d, e->date, sizeof e->date);
      // THE EPISODE'S IMDb SCORE, in tenths. Cinemeta sends it as a STRING ("8.5"),
      // under "rating" on the videos entries and "imdbRating" on the meta itself —
      // both are read, because which one is present varies by addon.
      { char r[16] = "";
        js_text(p, f, "imdbRating", r, sizeof r);
        if (!r[0]) js_text(p, f, "rating", r, sizeof r);
        if (r[0]) {
          double v = atof(r);
          if (v > 0.0 && v <= 10.0) e->imdb = (int)(v * 10.0 + 0.5);
        } }
      n++;
    }
    p = js_next(f);
  }
  for (int i = 1; i < n; i++) {
    CatEp k = eps[i];
    int j;
    for (j = i - 1; j >= 0 &&
         (eps[j].season > k.season ||
          (eps[j].season == k.season && eps[j].episode > k.episode)); j--)
      eps[j + 1] = eps[j];
    eps[j + 1] = k;
  }
  if (n) cat_set_episodes(targetItem, eps, n);
  free(eps);
  mark("episodes on screen");
  printf("[disc] %s: %d episodes published before the extras\n", title, n);
  fflush(stdout);
  return n;
}

static void *fetchEps(void *u) {
  int targetItem = epItem;
  const CatItem *orig = cat_item(targetItem);
  CatItem base;
  const CatItem *it;
  char url[600], *body = NULL;
  char series[24];
  (void)u;
  if (!orig || !orig->imdb[0]) { threadEpAlive = 0; return NULL; }
  // A FILM COMES THROUGH HERE TOO. /meta/movie carries cast, directing, genres and
  // score — before, only the titles enriched in the catalogue had a cast, and the
  // film's page opened without the row. What is series-only (episodes, seasons) is
  // skipped below.
  int isMovie = strcmp(orig->kind, "series") != 0;
  base = *orig;
  it = &base;
  { const char *dp;
    snprintf(series, sizeof series, "%s", it->imdb);
    dp = strchr(series, ':');
    if (dp) *(char *)dp = 0;
    snprintf(url, sizeof url, "%s/meta/%s/%s.json", CINEMETA,
             isMovie ? "movie" : "series", series); }

  body = metaCacheGet(series);
  mark(body ? "episodes: meta from cache" : "episodes: downloading meta");

  if (!body) {
    body = net_download(url, 25);
    if (!body) { threadEpAlive = 0; return NULL; }
    metaCacheStore(series, body);
  }
  if (!isMovie) publishEpisodes(body, targetItem, it->title);
  // The SAME response carries the cast, the directing and the season list. Fetching
  // again for each would be three round trips to the same place.
  {
    CatItem edit = *it;
    const char *c = js_array(body, NULL, "cast");
    int k = 0;
    while (c && k < 6) {
      size_t n2 = 0;
      const char *p2 = c;
      if (*p2 != '"') break;
      p2++;
      while (*p2 && *p2 != '"' && n2 + 1 < sizeof edit.cast[k].name)
        edit.cast[k].name[n2++] = *p2++;
      edit.cast[k].name[n2] = 0;
      edit.cast[k].role[0] = 0;   // Cinemeta does not give the character
      edit.cast[k].photo[0] = 0;
      k++;
      p2++;
      while (*p2 == ' ') p2++;
      c = (*p2 == ',') ? p2 + 1 : NULL;
      while (c && *c == ' ') c++;
    }
    edit.nCast = k;
    { const char *dr = js_array(body, NULL, "director");
      if (dr && *dr == '"') {
        size_t n2 = 0;
        dr++;
        while (*dr && *dr != '"' && n2 + 1 < sizeof edit.directing)
          edit.directing[n2++] = *dr++;
        edit.directing[n2] = 0;
      } }
    // GENRES, SCORE AND COUNTRY. They used to come only from the CATALOGUE, and
    // Cinemeta's catalogue carries none of the three: the meta line ended up with
    // the TYPE ("TV Show") in place of the genres, with no IMDb badge and no
    // country. /meta carries all three, and this function already has the response
    // in hand — not reading it was wasting a round trip already paid for.
    { const char *g = js_array(body, NULL, "genres");
      char list[160]; size_t n3 = 0;
      list[0] = 0;
      while (g && *g == '"' && n3 + 1 < sizeof list) {
        const char *p2 = g + 1;
        if (n3) { // separador do web: espaco, ponto medio, espaco
          if (n3 + 4 >= sizeof list) break;
          list[n3++] = ' '; list[n3++] = '\xc2'; list[n3++] = '\xb7'; list[n3++] = ' ';
        }
        while (*p2 && *p2 != '"' && n3 + 1 < sizeof list) list[n3++] = *p2++;
        list[n3] = 0;
        if (*p2 == '"') p2++;
        while (*p2 == ' ') p2++;
        g = (*p2 == ',') ? p2 + 1 : NULL;
        while (g && *g == ' ') g++;
      }
      if (list[0]) snprintf(edit.genre, sizeof edit.genre, "%s", list); }
    { double score = js_num(body, NULL, "imdbRating", 0.0);
      // The field arrives as "8.1" (a string or a number); we store it times 10 so
      // it fits in an int without losing the decimal place, as the rest of the
      // catalogue already does.
      if (score > 0.0) {
        int n10 = (int)(score * 10.0 + 0.5);
        if (n10 > 99) n10 /= 10;      // it already came multiplied
        edit.score = n10;
      } }
    js_text(body, NULL, "country", edit.country, sizeof edit.country);
    // The seasons present, without repeats and in order.
    { const char *v = js_array(body, NULL, "videos");
      edit.nSeasons = 0;
      while (v) {
        const char *fv = js_end(v);
        int t2 = (int)js_num(v, fv, "season", -1);
        if (t2 > 0) {
          int j, found = 0;
          for (j = 0; j < edit.nSeasons; j++)
            if (edit.seasons[j] == t2) { found = 1; break; }
          if (!found && edit.nSeasons < 12) edit.seasons[edit.nSeasons++] = t2;
        }
        v = js_next(fv);
      }
      { int i2, j2, tmp;
        for (i2 = 0; i2 < edit.nSeasons; i2++)
          for (j2 = i2 + 1; j2 < edit.nSeasons; j2++)
            if (edit.seasons[j2] < edit.seasons[i2]) {
              tmp = edit.seasons[i2];
              edit.seasons[i2] = edit.seasons[j2];
              edit.seasons[j2] = tmp;
            } } }
    // It publishes text, genres and seasons before the image enrichment.
    cat_update_item(targetItem, &edit);
    mark("detail: basic meta on screen");
    { char idBase[24];
      const char *dp;
      snprintf(idBase, sizeof idBase, "%s", it->imdb);
      dp = strchr(idBase, ':');
      if (dp) *(char *)dp = 0;
      photosOfCast(&edit, idBase, !strcmp(it->kind, "series")); }
    // AFTER photosOfCast, because that is what resolves the TMDB id, and the ratings
    // API is keyed by it. Called before this, `edit.tmdb` is still 0 and the fetch
    // returned without a word — which is exactly how it failed the first time.
    if (!isMovie) episodeScores(edit.tmdb, targetItem);
    cat_update_item(targetItem, &edit);
    printf("[disc] %s: %d actors, dir='%s', %d seasons\n",
           edit.title, edit.nCast, edit.directing, edit.nSeasons);
    fflush(stdout);
  }

  free(body);
  threadEpAlive = 0;
  return NULL;
}

// A request NOT YET SERVED, when one arrives with a thread in flight. This used to
// be `if (threadEpAlive) return;` — the request was dropped on the floor, and
// changing season while the previous one loaded left the list on the WRONG season
// forever, with no retry. Keeping the last one (rather than queueing them all) is
// right: the owner wants the season they STOPPED on, not the ones they passed through.
static int pendingItem = -1, pendingTemp;

// --- SEE ALL -----------------------------------------------------------------
//
// A list SEPARATE from the home's catalogue, on purpose: the home keeps 12 per row
// and it is the home that the library and the search sweep. Dumping 200 items of a
// catalogue in there would change what those two screens see because of a piece of
// navigation the owner may close a second later.
#define SEEALL_STEP 100        // the web app's default `skipStep` when the addon does not say

static CatItem  seeallItems[SEEALL_MAX];
static int      seeallN;
static char     seeallBase[600], seeallKind[8], seeallCat[96], seeallGenre[96];
static int      seeallPage, seeallEnd, seeallThreadAlive, seeallError;
static unsigned seeallGeneration;
static pthread_mutex_t seeallLock = PTHREAD_MUTEX_INITIALIZER;

static void *threadSeeAll(void *u) {
  (void)u;
  for (;;) {
  char url[1600], base[600], type[8], id[96], genre[96], encoded[290], *body;
  int raw=0, skip, cap;unsigned generation;
  pthread_mutex_lock(&seeallLock);
  skip=seeallPage;generation=seeallGeneration;
  snprintf(base,sizeof base,"%s",seeallBase);snprintf(type,sizeof type,"%s",seeallKind);
  snprintf(id,sizeof id,"%s",seeallCat);snprintf(genre,sizeof genre,"%s",seeallGenre);
  pthread_mutex_unlock(&seeallLock);
  int z=0;
  for(const unsigned char *c=(const unsigned char *)genre;*c&&z<(int)sizeof encoded-4;c++) {
    if((*c>='a'&&*c<='z')||(*c>='A'&&*c<='Z')||(*c>='0'&&*c<='9')||*c=='-'||*c=='_')encoded[z++]=*c;
    else {snprintf(encoded+z,4,"%%%02X",*c);z+=3;}
  }encoded[z]=0;
  if(genre[0])snprintf(url,sizeof url,"%s/catalog/%s/%s/genre=%s&skip=%d.json",base,type,id,encoded,skip);
  else if(skip)snprintf(url,sizeof url,"%s/catalog/%s/%s/skip=%d.json",base,type,id,skip);
  else snprintf(url,sizeof url,"%s/catalog/%s/%s.json",base,type,id);
  body=net_download(url,10);
  cap=strstr(id,"top100")?100:strstr(id,"top250")?250:SEEALL_MAX;
  pthread_mutex_lock(&seeallLock);
  if(generation!=seeallGeneration){pthread_mutex_unlock(&seeallLock);free(body);continue;}
  pthread_mutex_unlock(&seeallLock);
  const char *first=body?js_array(body,NULL,"metas"):NULL;
  int valid=body&&strstr(body,"\"metas\"");
  int added=0;
  for(const char *p=first;p;p=js_next(js_end(p))) {
    const char *f=js_end(p);raw++;
    CatItem it;
    if (ofMeta(p, f, type, &it)) {
      pthread_mutex_lock(&seeallLock);
      int duplicate=0;
      for(int i=0;i<seeallN;i++)if(it.imdb[0]&&!strcmp(seeallItems[i].imdb,it.imdb)&&!strcmp(seeallItems[i].kind,it.kind)){duplicate=1;break;}
      if(generation==seeallGeneration&&seeallN<cap&&!duplicate){seeallItems[seeallN++]=it;added++;}
      pthread_mutex_unlock(&seeallLock);
    }
  }
  free(body);
  pthread_mutex_lock(&seeallLock);
  if(generation!=seeallGeneration){pthread_mutex_unlock(&seeallLock);continue;}
  seeallError=!valid;
  // The skip uses the quantity received, not a presumed 100. Many addons deliver
  // 20/50 per page. A repetition with no new ids also ends the pagination.
  if(valid){seeallPage+=raw;if(!raw||!added||seeallN>=cap)seeallEnd=1;}
  seeallThreadAlive=0;
  pthread_mutex_unlock(&seeallLock);
  return NULL;
  }
}

static void seeallFire(void) {
  pthread_t t;
  pthread_mutex_lock(&seeallLock);
  if (seeallThreadAlive || seeallEnd || !seeallBase[0]) {pthread_mutex_unlock(&seeallLock);return;}
  seeallThreadAlive = 1;
  seeallError=0;
  if (pthread_create(&t, NULL, threadSeeAll, NULL) != 0) seeallThreadAlive = 0;
  else pthread_detach(t);
  pthread_mutex_unlock(&seeallLock);
}

void disc_seeall_open(const char *base, const char *kind, const char *catId) {
  disc_seeall_filter(base,kind,catId,"");
}
void disc_seeall_filter(const char *base, const char *kind, const char *catId,const char *genre) {
  if (!base || !kind || !catId) return;
  pthread_mutex_lock(&seeallLock);
  // The same catalogue that is already open: it keeps what has been read instead of
  // starting from scratch (the owner may have gone back and come in again).
  if (!strcmp(seeallBase, base) && !strcmp(seeallKind, kind) && !strcmp(seeallCat, catId)
      && !strcmp(seeallGenre,genre?genre:"") && seeallN > 0) {
    pthread_mutex_unlock(&seeallLock);
    return;
  }
  snprintf(seeallBase, sizeof seeallBase, "%s", base);
  snprintf(seeallKind, sizeof seeallKind, "%s", kind);
  snprintf(seeallCat,  sizeof seeallCat,  "%s", catId);
  snprintf(seeallGenre,sizeof seeallGenre,"%s",genre?genre:"");
  seeallN = 0; seeallPage = 0; seeallEnd = 0;seeallError=0;seeallGeneration++;
  pthread_mutex_unlock(&seeallLock);
  seeallFire();
}

void disc_seeall_more(void) { seeallFire(); }
int  disc_seeall_n(void) { pthread_mutex_lock(&seeallLock);int n=seeallN;pthread_mutex_unlock(&seeallLock);return n; }
int  disc_seeall_loading(void) { pthread_mutex_lock(&seeallLock);int n=seeallThreadAlive;pthread_mutex_unlock(&seeallLock);return n; }
int  disc_seeall_end(void) { pthread_mutex_lock(&seeallLock);int n=seeallEnd;pthread_mutex_unlock(&seeallLock);return n; }
int  disc_seeall_error(void) { pthread_mutex_lock(&seeallLock);int n=seeallError;pthread_mutex_unlock(&seeallLock);return n; }
void disc_seeall_close(void) { /* it keeps what it read; see disc_seeall_open */ }

int disc_seeall_item(int i, CatItem *dst) {
  int ok = 0;
  pthread_mutex_lock(&seeallLock);
  if (dst && i >= 0 && i < seeallN) { memcpy(dst, &seeallItems[i], sizeof *dst); ok = 1; }
  pthread_mutex_unlock(&seeallLock);
  return ok;
}

void disc_episodes(int indexItem, int season) {
  if (threadEpAlive) { pendingItem = indexItem; pendingTemp = season; return; }
  // The list is now SINGLE and covers every season, so having any episode of this
  // title is already enough — changing tab asks for nothing.
  (void)season;
  if (cat_n_episodes(indexItem) > 0) return;
  epItem = indexItem; epTemp = season;
  threadEpAlive = 1;
  if (pthread_create(&threadEp, NULL, fetchEps, NULL) != 0) threadEpAlive = 0;
  else pthread_detach(threadEp);
}

int disc_episodes_loading(int indexItem) {
  return (threadEpAlive && epItem == indexItem) || pendingItem == indexItem;
}

// Called every frame by whoever draws, so the stored request can go out as soon as
// the previous thread frees up.
void disc_episodes_pending(void) {
  int i, t;
  if (threadEpAlive || pendingItem < 0) return;
  i = pendingItem; t = pendingTemp;
  pendingItem = -1;
  disc_episodes(i, t);
}

// Opening a credit from an actor's filmography, or a "More like this" item, needs
// the meta of a title the owner's catalogue does NOT have. Before, those items were
// greyed out and would not open, which left the filmography decorative.
//
// The meta comes from Cinemeta, the same source as the rest of the catalogue, and
// the item goes in at the END of the array (cat_append). The type is not known in
// advance — TMDB says "movie"/"tv" in the credit, but Trakt's related item does not
// — so it tries film and, failing that, series. Two calls in the worst case, one in
// the common one.
static char sobId[24];
static long sobTmdb;          // when > 0, the IMDb id still has to be resolved
static char sobKind[8];
static int  sobIndex = -1;   // the result, consumed by disc_title_ready
static int  sobThreadAlive;
static pthread_t sobThread;

static void *fetchTitle(void *arg) {
  char url[200], id[24], *body;
  int found = -1, step;
  (void)arg;
  snprintf(id, sizeof id, "%s", sobId);

  // An actor's credit arrives with the TMDB id, not the IMDb one — combined_credits
  // does not carry imdb_id. `external_ids` does the translation, and it is a single
  // call, made only when the owner opens the credit.
  if (sobTmdb > 0) {
    const char *key = disc_key_tmdb();
    id[0] = 0;
    if (key && key[0]) {
      snprintf(url, sizeof url, "%s/%s/%ld/external_ids?api_key=%s", TMDB,
               strcmp(sobKind, "tv") ? "movie" : "tv", sobTmdb, key);
      body = net_download(url, 15);
      if (body) { js_text(body, NULL, "imdb_id", id, sizeof id); free(body); }
    }
    if (!id[0] || id[0] != 't') {
      printf("[disc] on demand tmdb %ld -> no imdb\n", sobTmdb); fflush(stdout);
      sobIndex = -1; sobThreadAlive = 0; return NULL;
    }
    // Already have it? Then just open it.
    { int j = cat_index_by_imdb(id);
      if (j >= 0) { sobIndex = j; sobThreadAlive = 0; return NULL; } }
  }

  for (step = 0; step < 2 && found < 0; step++) {
    const char *kind = step ? "series" : "movie";
    snprintf(url, sizeof url, "%s/meta/%s/%s.json", CINEMETA, kind, id);
    body = net_download(url, 20);
    if (!body) continue;
    { const char *m = strstr(body, "\"meta\"");
      CatItem it;
      if (m && ofMeta(m, NULL, kind, &it)) {
        // The request's own id wins: Cinemeta sometimes returns the field empty,
        // and without it the title would go into the catalogue with no key and
        // could neither be reopened nor matched with progress.
        if (!it.imdb[0]) snprintf(it.imdb, sizeof it.imdb, "%s", id);
        found = cat_append(&it);
      } }
    free(body);
  }
  printf("[disc] on demand %s -> index %d\n", id, found); fflush(stdout);
  sobIndex = found;
  sobThreadAlive = 0;
  return NULL;
}

void disc_request_title_tmdb(long tmdbId, const char *kind) {
  if (tmdbId <= 0 || sobThreadAlive) return;
  sobTmdb = tmdbId;
  snprintf(sobKind, sizeof sobKind, "%s", kind ? kind : "movie");
  sobId[0] = 0;
  sobIndex = -1;
  sobThreadAlive = 1;
  if (pthread_create(&sobThread, NULL, fetchTitle, NULL) != 0) sobThreadAlive = 0;
  else pthread_detach(sobThread);
}

void disc_request_title(const char *imdb) {
  char id[24];
  const char *dp;
  if (!imdb || imdb[0] != 't' || sobThreadAlive) return;
  // Cuts the episode suffix, if it comes: the meta belongs to the TITLE.
  dp = strchr(imdb, ':');
  if (dp) { size_t k = (size_t)(dp - imdb);
            if (k >= sizeof id) k = sizeof id - 1;
            memcpy(id, imdb, k); id[k] = 0; }
  else snprintf(id, sizeof id, "%s", imdb);
  if (cat_index_by_imdb(id) >= 0) return;   // ja temos
  snprintf(sobId, sizeof sobId, "%s", id);
  sobTmdb = 0;
  sobIndex = -1;
  sobThreadAlive = 1;
  if (pthread_create(&sobThread, NULL, fetchTitle, NULL) != 0) sobThreadAlive = 0;
  else pthread_detach(sobThread);
}

int disc_title_ready(void) { int v = sobIndex; sobIndex = -1; return v; }
int disc_title_searching(void) { return sobThreadAlive; }
