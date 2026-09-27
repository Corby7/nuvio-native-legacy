// The account's library and watched list. The contract and the reasons are in
// acclib.h; the short version is that sync_push_library REPLACES the list, so
// the library is only ever pushed as a complete pull with the pending edits
// replayed on top.
#include "acclib.h"
#include "session.h"
#include "profiles.h"
#include "trakt.h"
#include "data.h"
#include "watchedep.h"
#include "sync.h"
#include "js.h"
#include "jsw.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// catalog.c's watched mirror for whole titles; not in its header, the same
// extern ctxmenu.c uses.
extern void cat_history_set_id(const char *imdb, const char *kind, int watched);

// Pages as the web and Android clients ask for them, and a ceiling so a server
// that ignores the limit cannot keep the thread paging forever.
#define AL_LIB_PAGE     500
#define AL_WATCH_PAGE   900
#define AL_MAX_PAGES     20

typedef struct {
  char id[24], kind[8];
  char title[160], poster[512], background[512], release[32];
  char genre[160], synopsis[900];
  long long added;
} LibEntry;

typedef struct { char id[24], kind[8]; int season, episode; } WatchRow;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
// The library as the main thread sees it: the last pull with the pending edits
// replayed. Read by discovery's thread too, hence the lock.
static LibEntry *lib;
static int nLib, capLib, libKnown;
// What the last pull brought, set on the sync thread so the cycle's summary line
// (written on that thread too) can say it.
static int nLibPulled = -1, nWatchedKnown = -1;

// What the sync thread pulled, waiting for acclib_step.
static LibEntry *newLib;
static int nNewLib, hasNewLib;
static WatchRow *newWatch;
static int nNewWatch, hasNewWatch;

static long long nowMs(void) { return (long long)time(NULL) * 1000LL; }

static void baseId(const char *in, char *out, size_t size) {
  size_t k = 0;
  if (!size) return;
  while (in && in[k] && in[k] != ':' && k + 1 < size) { out[k] = in[k]; k++; }
  out[k] = 0;
}

static const char *kindOf(const char *kind) {
  return kind && (!strcmp(kind, "series") || !strcmp(kind, "show")) ? "series" : "movie";
}

static void fileName(char *dst, size_t size, int profile, const char *what) {
  snprintf(dst, size, "account-p%d-%s", profile, what);
}

int acclib_active(void) { return session_loggedin() && !trakt_active(); }

// ---------------------------------------------------------------- pending edits
//
// One line per edit, tab-separated so titles keep their spaces:
//   library  "+\t<id>\t<kind>\t<added ms>\t<title>\t<poster>\t<background>\t<release>"
//            "-\t<id>\t<kind>"
//   watched  "+\t<id>\t<kind>\t<season>\t<episode>\t<watched ms>\t<title>"
//            "-\t<id>\t<kind>\t<season>\t<episode>"
// Replaying them in order is idempotent (adding a present title or removing an
// absent one changes nothing), which is what lets acclib_step replay whatever
// is still in the file on top of every fresh pull.

static void clean(char *s) {
  for (; *s; s++) if (*s == '\t' || *s == '\n' || *s == '\r') *s = ' ';
}

static void opAppend(const char *what, const char *line) {
  char name[64], *old, *all;
  size_t a, b;
  fileName(name, sizeof name, profiles_active(), what);
  pthread_mutex_lock(&lock);
  old = data_read(name);
  a = old ? strlen(old) : 0;
  b = strlen(line);
  all = malloc(a + b + 2);
  if (all) {
    if (a) memcpy(all, old, a);
    memcpy(all + a, line, b);
    all[a + b] = '\n';
    all[a + b + 1] = 0;
    data_write(name, all);
  }
  pthread_mutex_unlock(&lock);
  free(old);
  free(all);
}

// Drops the first `k` lines — the ones a push just delivered — and keeps any the
// person added while it was in flight.
static void opDrop(const char *what, int profile, int k) {
  char name[64], *all, *p;
  fileName(name, sizeof name, profile, what);
  pthread_mutex_lock(&lock);
  all = data_read(name);
  if (all) {
    for (p = all; k > 0 && *p; k--) {
      char *nl = strchr(p, '\n');
      p = nl ? nl + 1 : p + strlen(p);
    }
    if (*p) data_write(name, p); else data_erase(name);
  }
  pthread_mutex_unlock(&lock);
  free(all);
}

static int splitTabs(char *line, char **field, int max) {
  int n = 0;
  char *p = line;
  while (n < max) {
    field[n++] = p;
    p = strchr(p, '\t');
    if (!p) break;
    *p++ = 0;
  }
  return n;
}

// ---------------------------------------------------------------- the library list

static int libFind(const LibEntry *list, int n, const char *id) {
  int i;
  for (i = 0; i < n; i++) if (!strcmp(list[i].id, id)) return i;
  return -1;
}

static int libGrow(LibEntry **list, int *cap, int want) {
  LibEntry *bigger;
  int c = *cap ? *cap : 64;
  if (want <= *cap) return 1;
  while (c < want) c *= 2;
  bigger = realloc(*list, sizeof(LibEntry) * (size_t)c);
  if (!bigger) return 0;
  *list = bigger;
  *cap = c;
  return 1;
}

// Replays library edit lines onto `list` (caller holds the lock or owns it).
static void libReplay(LibEntry **list, int *n, int *cap, const char *ops) {
  char *copy, *line, *ctx;
  if (!ops) return;
  copy = strdup(ops);
  if (!copy) return;
  for (line = strtok_r(copy, "\n", &ctx); line; line = strtok_r(NULL, "\n", &ctx)) {
    char *f[8];
    int nf = splitTabs(line, f, 8), at;
    if (nf < 3 || !f[1][0]) continue;
    at = libFind(*list, *n, f[1]);
    if (f[0][0] == '-') {
      if (at >= 0) { memmove(*list + at, *list + at + 1, sizeof(LibEntry) * (size_t)(*n - at - 1)); (*n)--; }
    } else if (f[0][0] == '+' && at < 0 && nf >= 8 && libGrow(list, cap, *n + 1)) {
      LibEntry *e = &(*list)[*n];
      // Newest first, as the server orders it: a title just added goes on top.
      memmove(*list + 1, *list, sizeof(LibEntry) * (size_t)*n);
      e = &(*list)[0];
      memset(e, 0, sizeof *e);
      snprintf(e->id, sizeof e->id, "%s", f[1]);
      snprintf(e->kind, sizeof e->kind, "%s", kindOf(f[2]));
      e->added = atoll(f[3]);
      snprintf(e->title, sizeof e->title, "%s", f[4]);
      snprintf(e->poster, sizeof e->poster, "%s", f[5]);
      snprintf(e->background, sizeof e->background, "%s", f[6]);
      snprintf(e->release, sizeof e->release, "%s", f[7]);
      (*n)++;
    }
  }
  free(copy);
}

// A row of sync_pull_library -> an entry.
static void libFromRow(const char *p, const char *f, LibEntry *e) {
  const char *g;
  memset(e, 0, sizeof *e);
  js_text(p, f, "content_id", e->id, sizeof e->id);
  { char k[16] = ""; js_text(p, f, "content_type", k, sizeof k);
    snprintf(e->kind, sizeof e->kind, "%s", kindOf(k)); }
  js_text(p, f, "name", e->title, sizeof e->title);
  js_text(p, f, "poster", e->poster, sizeof e->poster);
  js_text(p, f, "background", e->background, sizeof e->background);
  js_text(p, f, "release_info", e->release, sizeof e->release);
  js_text(p, f, "description", e->synopsis, sizeof e->synopsis);
  e->added = (long long)js_num(p, f, "added_at", 0);
  // The genres in the shape every card reader expects: the type first, then the
  // genres, joined by a middle dot.
  snprintf(e->genre, sizeof e->genre, "%s", strcmp(e->kind, "series") ? "Movie" : "TV Show");
  for (g = js_array(p, f, "genres"); g && g < f && *g == '"'; ) {
    const char *q = g + 1;
    size_t len = strlen(e->genre);
    char word[48];
    size_t k = 0;
    while (*q && *q != '"' && k + 1 < sizeof word) { if (*q == '\\' && q[1]) q++; word[k++] = *q++; }
    word[k] = 0;
    while (*q && *q != '"') q++;
    if (*q) q++;
    if (word[0] && len + k + 8 < sizeof e->genre)
      snprintf(e->genre + len, sizeof e->genre - len, "  \xc2\xb7  %s", word);
    while (*q == ' ' || *q == '\n' || *q == '\r' || *q == '\t') q++;
    if (*q != ',') break;
    q++;
    while (*q == ' ' || *q == '\n' || *q == '\r' || *q == '\t') q++;
    g = q;
  }
}

static void libSave(int profile, const LibEntry *list, int n) {
  char name[64], *buf;
  size_t cap = (size_t)n * 2600 + 16, k = 0;
  int i;
  fileName(name, sizeof name, profile, "library.txt");
  buf = malloc(cap);
  if (!buf) return;
  buf[0] = 0;
  for (i = 0; i < n; i++) {
    LibEntry e = list[i];
    clean(e.title); clean(e.poster); clean(e.background); clean(e.release);
    clean(e.genre); clean(e.synopsis);
    k += (size_t)snprintf(buf + k, cap - k, "%s\t%s\t%lld\t%s\t%s\t%s\t%s\t%s\t%s\n",
                          e.id, e.kind, e.added, e.title, e.poster, e.background,
                          e.release, e.genre, e.synopsis);
    if (k >= cap) break;
  }
  if (n) data_write(name, buf); else data_write(name, "\n");
  free(buf);
}

void acclib_restore(void) {
  char name[64], *buf, *line, *ctx, *ops;
  if (!session_loggedin()) return;
  fileName(name, sizeof name, profiles_active(), "library.txt");
  buf = data_read(name);
  pthread_mutex_lock(&lock);
  nLib = 0;
  libKnown = 0;
  if (buf) {
    for (line = strtok_r(buf, "\n", &ctx); line; line = strtok_r(NULL, "\n", &ctx)) {
      char *f[9];
      LibEntry *e;
      if (splitTabs(line, f, 9) < 9 || !f[0][0] || !libGrow(&lib, &capLib, nLib + 1)) continue;
      e = &lib[nLib++];
      memset(e, 0, sizeof *e);
      snprintf(e->id, sizeof e->id, "%s", f[0]);
      snprintf(e->kind, sizeof e->kind, "%s", f[1]);
      e->added = atoll(f[2]);
      snprintf(e->title, sizeof e->title, "%s", f[3]);
      snprintf(e->poster, sizeof e->poster, "%s", f[4]);
      snprintf(e->background, sizeof e->background, "%s", f[5]);
      snprintf(e->release, sizeof e->release, "%s", f[6]);
      snprintf(e->genre, sizeof e->genre, "%s", f[7]);
      snprintf(e->synopsis, sizeof e->synopsis, "%s", f[8]);
    }
    libKnown = 1;
  }
  pthread_mutex_unlock(&lock);
  free(buf);
  fileName(name, sizeof name, profiles_active(), "library-ops.txt");
  ops = data_read(name);
  pthread_mutex_lock(&lock);
  libReplay(&lib, &nLib, &capLib, ops);
  pthread_mutex_unlock(&lock);
  free(ops);
}

int acclib_has(const char *imdb) {
  char id[24];
  int r;
  baseId(imdb, id, sizeof id);
  if (!id[0]) return 0;
  pthread_mutex_lock(&lock);
  r = libFind(lib, nLib, id) >= 0;
  pthread_mutex_unlock(&lock);
  return r;
}

int acclib_items(CatItem *out, int max) {
  int i, n = 0;
  pthread_mutex_lock(&lock);
  for (i = 0; i < nLib && n < max; i++) {
    const LibEntry *e = &lib[i];
    CatItem *d = &out[n];
    if (strncmp(e->id, "tt", 2)) continue;   // the art and the addons key on IMDb ids
    memset(d, 0, sizeof *d);
    snprintf(d->imdb, sizeof d->imdb, "%s", e->id);
    snprintf(d->kind, sizeof d->kind, "%s", e->kind);
    snprintf(d->title, sizeof d->title, "%s", e->title);
    // The account's own art when the row has it, metahub's otherwise — the same
    // deterministic urls, and the same medium JPEG, trakt.c's listArt uses.
    if (e->poster[0]) snprintf(d->poster, sizeof d->poster, "%s", e->poster);
    else snprintf(d->poster, sizeof d->poster, "https://images.metahub.space/poster/medium/%s/img", e->id);
    if (e->background[0]) snprintf(d->backdrop, sizeof d->backdrop, "%s", e->background);
    else snprintf(d->backdrop, sizeof d->backdrop, "https://images.metahub.space/background/medium/%s/img", e->id);
    cat_backdrop_shrink(d->backdrop, sizeof d->backdrop, CAT_BACKDROP_HERO_W);
    snprintf(d->logo, sizeof d->logo, "https://images.metahub.space/logo/medium/%s/img", e->id);
    snprintf(d->genre, sizeof d->genre, "%s", e->genre[0] ? e->genre
             : strcmp(e->kind, "series") ? "Movie" : "TV Show");
    snprintf(d->synopsis, sizeof d->synopsis, "%s", e->synopsis);
    d->year = atoi(e->release);
    if (d->year > 1800) snprintf(d->meta, sizeof d->meta, "%d", d->year);
    snprintf(d->age_rating, sizeof d->age_rating, "14");
    d->inList = 1;
    d->added = e->added;
    n++;
  }
  pthread_mutex_unlock(&lock);
  return n;
}

// ---------------------------------------------------------------- gestures

void acclib_list(const char *imdb, const char *kind, int add) {
  char id[24], line[1600];
  baseId(imdb, id, sizeof id);
  if (!id[0]) return;
  if (add) {
    char title[160] = "", poster[512] = "", background[512] = "", release[32] = "";
    int ci = cat_index_by_imdb(id);
    const CatItem *c = ci >= 0 ? cat_item(ci) : NULL;
    if (c) {
      snprintf(title, sizeof title, "%s", c->title);
      snprintf(poster, sizeof poster, "%s", c->poster);
      snprintf(background, sizeof background, "%s", c->backdrop);
      if (c->year > 0) snprintf(release, sizeof release, "%d", c->year);
      else snprintf(release, sizeof release, "%.4s", c->meta);
      if (!kind || !kind[0]) kind = c->kind;
    }
    clean(title); clean(poster); clean(background); clean(release);
    snprintf(line, sizeof line, "+\t%s\t%s\t%lld\t%s\t%s\t%s\t%s", id, kindOf(kind),
             nowMs(), title, poster, background, release);
  } else {
    snprintf(line, sizeof line, "-\t%s\t%s", id, kindOf(kind));
  }
  opAppend("library-ops.txt", line);
  pthread_mutex_lock(&lock);
  libReplay(&lib, &nLib, &capLib, line);
  pthread_mutex_unlock(&lock);
  // The card's own mark is the caller's (app.c and ctxmenu.c flip it after this
  // returns — flipping it here too would cancel theirs). Other copies of the
  // title follow on the next cycle, when acclib_step re-marks the catalogue.
  printf("[acclib] library %s %s (pending push)\n", add ? "add" : "remove", id);
  sync_soon();
}

void acclib_watched(const char *imdb, const char *kind, int season, int episode,
                    int watched) {
  char id[24], line[400];
  baseId(imdb, id, sizeof id);
  if (!id[0]) return;
  if (season < 0) season = 0;
  if (episode < 0) episode = 0;
  if (watched) {
    char title[160] = "";
    int ci = cat_index_by_imdb(id);
    const CatItem *c = ci >= 0 ? cat_item(ci) : NULL;
    if (c) snprintf(title, sizeof title, "%s", c->title);
    clean(title);
    snprintf(line, sizeof line, "+\t%s\t%s\t%d\t%d\t%lld\t%s", id,
             season > 0 ? "series" : kindOf(kind), season, episode, nowMs(), title);
  } else {
    snprintf(line, sizeof line, "-\t%s\t%s\t%d\t%d", id, kindOf(kind), season, episode);
  }
  opAppend("watched-ops.txt", line);
  printf("[acclib] watched %s %s s%d e%d (pending push)\n",
         watched ? "mark" : "unmark", id, season, episode);
  sync_soon();
}

// ---------------------------------------------------------------- the sync thread

static int ok2xx(const char *r, int st) { return r && st >= 200 && st < 300; }

typedef struct { const char *raw; size_t len; char id[24]; int drop; } RawRow;

// Every page of the library. Returns the bodies (caller frees each) and fills
// `rows`; -1 when any page failed, because a push built on a partial pull would
// delete the pages that did not arrive.
static int pullLibrary(char **bodies, int *nBodies, RawRow **rows, int *nRows) {
  int page, cap = 0;
  *nBodies = 0; *nRows = 0; *rows = NULL;
  for (page = 0; page < AL_MAX_PAGES; page++) {
    char body[128], *r;
    const char *p;
    int st = 0, got = 0;
    snprintf(body, sizeof body, "{\"p_profile_id\":%d,\"p_limit\":%d,\"p_offset\":%d}",
             profiles_active(), AL_LIB_PAGE, page * AL_LIB_PAGE);
    r = session_rpc("sync_pull_library", body, &st);
    if (!ok2xx(r, st)) {
      printf("[acclib] library pull failed (HTTP %d)\n", st);
      free(r);
      return -1;
    }
    bodies[(*nBodies)++] = r;
    for (p = js_root_array(r); p; p = js_next(js_end(p))) {
      const char *f = js_end(p);
      RawRow *row;
      got++;
      if (*nRows >= cap) {
        RawRow *bigger = realloc(*rows, sizeof(RawRow) * (size_t)(cap ? cap * 2 : 128));
        if (!bigger) return -1;
        *rows = bigger;
        cap = cap ? cap * 2 : 128;
      }
      row = &(*rows)[(*nRows)++];
      memset(row, 0, sizeof *row);
      row->raw = p;
      row->len = (size_t)(f - p);
      js_text(p, f, "content_id", row->id, sizeof row->id);
    }
    if (got < AL_LIB_PAGE) return 0;
  }
  return 0;
}

// The library cycle: complete pull, replay, push when anything is pending, and
// the pulled list handed to the main thread.
static void syncLibrary(int profile) {
  char *bodies[AL_MAX_PAGES], name[64], *ops, *copy, *line, *ctx;
  RawRow *rows = NULL;
  int nBodies = 0, nRows = 0, i, nOps = 0, changed = 0;
  LibEntry *pulled = NULL;
  int nPulled = 0, capPulled = 0;
  Jsw w;

  if (pullLibrary(bodies, &nBodies, &rows, &nRows) < 0) goto done;

  // The pulled list itself, before any replay: acclib_step replays whatever is
  // still pending on top of it, so a gesture made while this cycle runs is not
  // lost when the list is swapped.
  if (nRows && libGrow(&pulled, &capPulled, nRows))
    for (i = 0; i < nRows; i++) {
      libFromRow(rows[i].raw, rows[i].raw + rows[i].len, &pulled[nPulled]);
      if (pulled[nPulled].id[0]) nPulled++;
    }

  fileName(name, sizeof name, profile, "library-ops.txt");
  pthread_mutex_lock(&lock);
  ops = data_read(name);
  pthread_mutex_unlock(&lock);

  if (ops && (copy = strdup(ops))) {
    // Adds that are not on the server yet, in the order they were made.
    char *added[256];
    int nAdded = 0;
    for (line = strtok_r(copy, "\n", &ctx); line; line = strtok_r(NULL, "\n", &ctx)) {
      char *f[8], keep[1600];
      int nf, j;
      snprintf(keep, sizeof keep, "%s", line);
      nf = splitTabs(line, f, 8);
      nOps++;
      if (nf < 3 || !f[1][0]) continue;
      if (f[0][0] == '-') {
        for (j = 0; j < nRows; j++) if (!strcmp(rows[j].id, f[1]) && !rows[j].drop) { rows[j].drop = 1; changed = 1; }
        for (j = 0; j < nAdded; j++)
          if (!strncmp(added[j] + 2, f[1], strlen(f[1])) && added[j][2 + strlen(f[1])] == '\t') {
            free(added[j]); added[j] = added[--nAdded]; changed = 1; break;
          }
      } else if (f[0][0] == '+' && nf >= 8) {
        int present = 0;
        for (j = 0; j < nRows; j++) if (!rows[j].drop && !strcmp(rows[j].id, f[1])) present = 1;
        for (j = 0; j < nAdded; j++)
          if (!strncmp(added[j] + 2, f[1], strlen(f[1])) && added[j][2 + strlen(f[1])] == '\t') present = 1;
        if (!present && nAdded < 256 && (added[nAdded] = strdup(keep))) { nAdded++; changed = 1; }
      }
    }
    free(copy);

    if (nOps && changed) {
      char *r;
      int st = 0, kept = 0;
      jsw_start(&w);
      jsw_obj_start(&w);
      jsw_ci(&w, "p_profile_id", profile);
      jsw_cs(&w, "p_origin_client_id", data_client_id());
      jsw_key(&w, "p_items");
      jsw_arr_start(&w);
      // THE ACCOUNT'S ROWS GO BACK AS THEY CAME. The server's upsert only writes
      // a column that differs, so an untouched row stays untouched — and the
      // columns this app never reads (poster_shape, genres, imdb_rating,
      // addon_base_url) are not flattened to defaults on every device.
      for (i = 0; i < nRows; i++) {
        char *one;
        if (rows[i].drop) continue;
        one = malloc(rows[i].len + 1);
        if (!one) { w.error = 1; break; }
        memcpy(one, rows[i].raw, rows[i].len);
        one[rows[i].len] = 0;
        jsw_raw(&w, one);
        free(one);
        kept++;
      }
      for (i = 0; i < nAdded; i++) {
        char *f[8];
        splitTabs(added[i], f, 8);
        jsw_obj_start(&w);
        jsw_cs(&w, "content_id", f[1]);
        jsw_cs(&w, "content_type", f[2]);
        jsw_cs(&w, "name", f[4][0] ? f[4] : "Untitled");
        if (f[5][0]) jsw_cs(&w, "poster", f[5]); else { jsw_key(&w, "poster"); jsw_null(&w); }
        jsw_cs(&w, "poster_shape", "POSTER");
        if (f[6][0]) jsw_cs(&w, "background", f[6]); else { jsw_key(&w, "background"); jsw_null(&w); }
        jsw_cs(&w, "description", "");
        jsw_cs(&w, "release_info", f[7]);
        jsw_key(&w, "imdb_rating"); jsw_null(&w);
        jsw_key(&w, "genres"); jsw_arr_start(&w); jsw_arr_end(&w);
        jsw_key(&w, "addon_base_url"); jsw_null(&w);
        jsw_ci(&w, "added_at", atoll(f[3]));
        jsw_obj_end(&w);
        kept++;
      }
      jsw_arr_end(&w);
      jsw_obj_end(&w);
      if (w.error) {
        printf("[acclib] library push not built (out of memory); edits kept\n");
      } else {
        r = session_rpc("sync_push_library", jsw_text_final(&w), &st);
        if (ok2xx(r, st)) {
          opDrop("library-ops.txt", profile, nOps);
          nOps = 0;
          data_log("sync.log", "library pushed: %d item(s)", kept);
          printf("[acclib] library pushed: %d item(s)\n", kept);
          // What was pushed is what the server now holds: the handed-over list
          // becomes the pushed one, so the screen does not flicker back to the
          // pre-edit pull until the next cycle.
          libReplay(&pulled, &nPulled, &capPulled, ops);
        } else {
          data_log("sync.log", "library push FAILED (HTTP %d); %d edit(s) kept", st, nOps);
          printf("[acclib] library push failed (HTTP %d)%s%.200s\n", st, r ? ": " : "", r ? r : "");
        }
        free(r);
      }
      jsw_free(&w);
    } else if (nOps) {
      // Every edit was already true on the server (made on another device too,
      // or a push whose answer was lost). Nothing to send; clear them.
      opDrop("library-ops.txt", profile, nOps);
    }
    for (i = 0; i < nAdded; i++) free(added[i]);
  }
  free(ops);

  pthread_mutex_lock(&lock);
  free(newLib);
  newLib = pulled;
  nNewLib = nPulled;
  hasNewLib = 1;
  nLibPulled = nPulled;
  pulled = NULL;
  pthread_mutex_unlock(&lock);

done:
  free(pulled);
  free(rows);
  for (i = 0; i < nBodies; i++) free(bodies[i]);
}

static void syncWatched(int profile) {
  char name[64], *ops, *copy, *line, *ctx;
  WatchRow *rows = NULL;
  int nRows = 0, cap = 0, page, nOps = 0, failed = 0;

  // Pushes FIRST here, unlike the library: sync_push_watched_items only upserts,
  // so there is nothing to lose by sending before reading, and the pull that
  // follows then already includes what this TV marked.
  fileName(name, sizeof name, profile, "watched-ops.txt");
  pthread_mutex_lock(&lock);
  ops = data_read(name);
  pthread_mutex_unlock(&lock);
  if (ops && (copy = strdup(ops))) {
    // The last word per episode wins: marked then unmarked is one delete.
    typedef struct { char id[24], kind[8], title[160]; int season, episode, mark; long long ms; } Final;
    Final *fin = NULL;
    int nFin = 0, capFin = 0, i, nAdd = 0, nDel = 0;
    for (line = strtok_r(copy, "\n", &ctx); line; line = strtok_r(NULL, "\n", &ctx)) {
      char *f[7];
      int nf = splitTabs(line, f, 7), s, e, j;
      nOps++;
      if (nf < 5 || !f[1][0]) continue;
      s = atoi(f[3]); e = atoi(f[4]);
      for (j = 0; j < nFin; j++)
        if (!strcmp(fin[j].id, f[1]) && fin[j].season == s && fin[j].episode == e) break;
      if (j == nFin) {
        if (nFin >= capFin) {
          Final *bigger = realloc(fin, sizeof(Final) * (size_t)(capFin ? capFin * 2 : 32));
          if (!bigger) continue;
          fin = bigger;
          capFin = capFin ? capFin * 2 : 32;
        }
        memset(&fin[nFin], 0, sizeof fin[nFin]);
        nFin++;
      }
      snprintf(fin[j].id, sizeof fin[j].id, "%s", f[1]);
      snprintf(fin[j].kind, sizeof fin[j].kind, "%s", f[2]);
      fin[j].season = s; fin[j].episode = e;
      fin[j].mark = f[0][0] == '+';
      fin[j].ms = nf >= 6 ? atoll(f[5]) : 0;
      if (nf >= 7) snprintf(fin[j].title, sizeof fin[j].title, "%s", f[6]);
    }
    free(copy);
    for (i = 0; i < nFin; i++) { if (fin[i].mark) nAdd++; else nDel++; }
    if (nAdd) {
      Jsw w;
      char *r;
      int st = 0;
      jsw_start(&w);
      jsw_obj_start(&w);
      jsw_ci(&w, "p_profile_id", profile);
      jsw_cs(&w, "p_origin_client_id", data_client_id());
      jsw_key(&w, "p_items");
      jsw_arr_start(&w);
      for (i = 0; i < nFin; i++) {
        if (!fin[i].mark) continue;
        jsw_obj_start(&w);
        jsw_cs(&w, "content_id", fin[i].id);
        jsw_cs(&w, "content_type", fin[i].kind);
        jsw_cs(&w, "title", fin[i].title);
        // The web's key: an episode carries both numbers, a film or a whole
        // series neither — never a 0, which the server would store as season 0.
        if (fin[i].season > 0 && fin[i].episode > 0) {
          jsw_ci(&w, "season", fin[i].season); jsw_ci(&w, "episode", fin[i].episode);
        } else {
          jsw_key(&w, "season"); jsw_null(&w); jsw_key(&w, "episode"); jsw_null(&w);
        }
        jsw_ci(&w, "watched_at", fin[i].ms > 0 ? fin[i].ms : nowMs());
        jsw_obj_end(&w);
      }
      jsw_arr_end(&w);
      jsw_obj_end(&w);
      r = session_rpc("sync_push_watched_items", jsw_text_final(&w), &st);
      jsw_free(&w);
      if (!ok2xx(r, st)) {
        failed = 1;
        data_log("sync.log", "watched push FAILED (HTTP %d)", st);
        printf("[acclib] watched push failed (HTTP %d)%s%.200s\n", st, r ? ": " : "", r ? r : "");
      } else {
        data_log("sync.log", "watched pushed: %d item(s)", nAdd);
      }
      free(r);
    }
    if (nDel && !failed) {
      Jsw w;
      char *r;
      int st = 0;
      jsw_start(&w);
      jsw_obj_start(&w);
      jsw_ci(&w, "p_profile_id", profile);
      jsw_cs(&w, "p_origin_client_id", data_client_id());
      jsw_key(&w, "p_keys");
      jsw_arr_start(&w);
      for (i = 0; i < nFin; i++) {
        if (fin[i].mark) continue;
        jsw_obj_start(&w);
        jsw_cs(&w, "content_id", fin[i].id);
        if (fin[i].season > 0 && fin[i].episode > 0) {
          jsw_ci(&w, "season", fin[i].season); jsw_ci(&w, "episode", fin[i].episode);
        }
        jsw_obj_end(&w);
      }
      jsw_arr_end(&w);
      jsw_obj_end(&w);
      r = session_rpc("sync_delete_watched_items", jsw_text_final(&w), &st);
      jsw_free(&w);
      if (!ok2xx(r, st)) {
        failed = 1;
        data_log("sync.log", "watched delete FAILED (HTTP %d)", st);
        printf("[acclib] watched delete failed (HTTP %d)%s%.200s\n", st, r ? ": " : "", r ? r : "");
      } else {
        data_log("sync.log", "watched deleted: %d item(s)", nDel);
      }
      free(r);
    }
    // Both halves are idempotent, so a retry after a partial failure is safe.
    if (!failed) opDrop("watched-ops.txt", profile, nOps);
    free(fin);
  }
  free(ops);

  for (page = 1; page <= AL_MAX_PAGES; page++) {
    char body[128], *r;
    const char *p;
    int st = 0, got = 0;
    snprintf(body, sizeof body, "{\"p_profile_id\":%d,\"p_page\":%d,\"p_page_size\":%d}",
             profile, page, AL_WATCH_PAGE);
    r = session_rpc("sync_pull_watched_items", body, &st);
    if (!ok2xx(r, st)) { free(r); free(rows); return; }
    for (p = js_root_array(r); p; p = js_next(js_end(p))) {
      const char *f = js_end(p);
      WatchRow *row;
      char k[16] = "";
      got++;
      if (nRows >= cap) {
        WatchRow *bigger = realloc(rows, sizeof(WatchRow) * (size_t)(cap ? cap * 2 : 256));
        if (!bigger) break;
        rows = bigger;
        cap = cap ? cap * 2 : 256;
      }
      row = &rows[nRows];
      memset(row, 0, sizeof *row);
      if (!js_text(p, f, "content_id", row->id, sizeof row->id)) continue;
      js_text(p, f, "content_type", k, sizeof k);
      snprintf(row->kind, sizeof row->kind, "%s", kindOf(k));
      row->season = (int)js_num(p, f, "season", 0);
      row->episode = (int)js_num(p, f, "episode", 0);
      nRows++;
    }
    free(r);
    if (got < AL_WATCH_PAGE) break;
  }
  pthread_mutex_lock(&lock);
  free(newWatch);
  newWatch = rows;
  nNewWatch = nRows;
  hasNewWatch = 1;
  nWatchedKnown = nRows;
  pthread_mutex_unlock(&lock);
}

void acclib_sync(void) {
  int profile = profiles_active();
  if (!acclib_active()) return;
  syncLibrary(profile);
  syncWatched(profile);
}

// ---------------------------------------------------------------- main thread

int acclib_step(void) {
  char name[64], *ops;
  int changed = 0, i;
  LibEntry *pulled = NULL;
  WatchRow *watch = NULL;
  int nPulled = 0, nWatch = 0, gotLib, gotWatch;

  pthread_mutex_lock(&lock);
  gotLib = hasNewLib; gotWatch = hasNewWatch;
  if (gotLib) { pulled = newLib; nPulled = nNewLib; newLib = NULL; hasNewLib = 0; }
  if (gotWatch) { watch = newWatch; nWatch = nNewWatch; newWatch = NULL; hasNewWatch = 0; }
  pthread_mutex_unlock(&lock);
  // Trakt linked while the cycle ran: what it pulled is no longer the source.
  if (!acclib_active()) { free(pulled); free(watch); return 0; }

  if (gotLib) {
    int profile = profiles_active(), cap = nPulled;
    libSave(profile, pulled, nPulled);
    fileName(name, sizeof name, profile, "library-ops.txt");
    ops = data_read(name);
    libReplay(&pulled, &nPulled, &cap, ops);
    free(ops);
    pthread_mutex_lock(&lock);
    // A rebuild is only worth it when the SET of titles changed: the catalogue
    // then lacks a card the Library should show.
    if (!libKnown || nPulled != nLib) changed = 1;
    else for (i = 0; i < nPulled && !changed; i++) if (libFind(lib, nLib, pulled[i].id) < 0) changed = 1;
    free(lib);
    lib = pulled;
    nLib = nPulled;
    capLib = cap;
    libKnown = 1;
    pthread_mutex_unlock(&lock);
    // The marks on the catalogue follow the list: a title removed on the phone
    // loses its "saved" state here without waiting for a rebuild.
    for (i = 0; i < cat_n(); i++) {
      const CatItem *c = cat_item(i);
      int want;
      if (!c || !c->imdb[0]) continue;
      want = acclib_has(c->imdb);
      if (c->inList != want) cat_set_in_list(i, want);
    }
    printf("[acclib] library: %d title(s)%s\n", nLib, changed ? ", changed" : "");
  }

  if (gotWatch) {
    for (i = 0; i < nWatch; i++) {
      const WatchRow *r = &watch[i];
      if (r->season > 0 && r->episode > 0) watchedep_set(r->id, r->season, r->episode, 1);
      else cat_history_set_id(r->id, r->kind, 1);
    }
    // The edits the server has not taken yet still stand on this screen.
    fileName(name, sizeof name, profiles_active(), "watched-ops.txt");
    ops = data_read(name);
    if (ops) {
      char *line, *ctx;
      for (line = strtok_r(ops, "\n", &ctx); line; line = strtok_r(NULL, "\n", &ctx)) {
        char *f[7];
        int nf = splitTabs(line, f, 7), s, e;
        if (nf < 5) continue;
        s = atoi(f[3]); e = atoi(f[4]);
        if (s > 0 && e > 0) watchedep_set(f[1], s, e, f[0][0] == '+');
        else cat_history_set_id(f[1], f[2], f[0][0] == '+');
      }
      free(ops);
    }
    printf("[acclib] watched: %d item(s)\n", nWatch);
    free(watch);
  }
  return changed;
}

int acclib_count_library(void) {
  int n;
  pthread_mutex_lock(&lock);
  n = nLibPulled;
  pthread_mutex_unlock(&lock);
  return n;
}
int acclib_count_watched(void) {
  int n;
  pthread_mutex_lock(&lock);
  n = nWatchedKnown;
  pthread_mutex_unlock(&lock);
  return n;
}

void acclib_forget(void) {
  static const char *const WHAT[] = { "library.txt", "library-ops.txt", "watched-ops.txt" };
  char name[64];
  int p, k;
  pthread_mutex_lock(&lock);
  free(lib); lib = NULL; nLib = capLib = 0; libKnown = 0;
  free(newLib); newLib = NULL; nNewLib = 0; hasNewLib = 0;
  free(newWatch); newWatch = NULL; nNewWatch = 0; hasNewWatch = 0;
  nWatchedKnown = -1; nLibPulled = -1;
  for (p = 0; p <= 16; p++)
    for (k = 0; k < 3; k++) { fileName(name, sizeof name, p, WHAT[k]); data_erase(name); }
  pthread_mutex_unlock(&lock);
}
