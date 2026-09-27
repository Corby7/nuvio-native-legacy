#include "trakt.h"
#include "net.h"
#include "js.h"
#include "watchedep.h"
#include "jsw.h"
#include "discover.h"
#include "acclib.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <sys/stat.h>
#include "data.h"

#define CINEMETA "https://v3-cinemeta.strem.io"

static char token[128], client[80];
static int  on;

// The state of the last write started from the menu. A POST's body is no proof
// of success: Trakt returns a body on 4xx too. The consumer uses this state so
// it only mirrors the local intent after an HTTP 2xx.
enum { TK_OP_NONE, TK_OP_PENDING, TK_OP_CONFIRMED, TK_OP_FAILURE };
enum { TK_OP_LIST = 1, TK_OP_HISTORY = 2 };
static volatile int listState, historyState;

// Keeps the old contract (IMDb only) without losing the type when the item is
// already in the catalogue. The episode suffix stays as a fallback for older
// calls made before the catalogue was assembled.
extern const char *cat_kind_by_imdb(const char *imdb);
extern void cat_history_set_id(const char *imdb, const char *kind, int watched);

static const char *kind_item(const char *kind, const char *imdb) {
  if (kind && (!strcmp(kind, "series") || !strcmp(kind, "show"))) return "series";
  if (kind && !strcmp(kind, "movie")) return "movie";
  return cat_kind_by_imdb(imdb);
}
static pthread_mutex_t lockList = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t lockHistory = PTHREAD_MUTEX_INITIALIZER;

static void stateWrite(volatile int *state, int value) {
  __atomic_store_n(state, value, __ATOMIC_RELEASE);
}

static int stateRead(const volatile int *state) {
  return __atomic_load_n(state, __ATOMIC_ACQUIRE);
}

// A small API, internal to the port: the declaration lives in the consumer
// because the historical public contract of trakt_watchlist/trakt_watched is
// still void.
int trakt_operation_state(int kind) {
  if (kind == TK_OP_LIST) return stateRead(&listState);
  if (kind == TK_OP_HISTORY) return stateRead(&historyState);
  return TK_OP_NONE;
}

int trakt_active(void) { return on; }

// THE CREDENTIAL WAS REFUSED (HTTP 401).
//
// The 401 used to reach the log and nowhere else: the screen carried on saying
// "connected" and the owner only found out when "Continue watching" stopped
// arriving. The network layer is what tells us (net_notify_401), because the
// 401 can come back from any call — resume, history, extras. traktauth_step is
// what ACTS: it renews from the stored refresh token and, if the refresh has
// died too, drops the state to invalid so the screen finally says so.
//
// Written on a NETWORK thread and read on the UI thread, so it is only ever a
// flag — never a place to make a request from.
static volatile int credRefused;

static void warn401(const char *url) {
  // /oauth/* answers 401 for a bad APPLICATION credential, which is the
  // package's problem and not this person's session.
  if (!url || !strstr(url, "api.trakt.tv") || strstr(url, "/oauth/")) return;
  if (on && !credRefused) {
    credRefused = 1;
    printf("[trakt] HTTP 401: credential refused — renewal pending\n");
    fflush(stdout);
  }
}

int trakt_refused(void) { return credRefused; }

void trakt_forget(void) {
  token[0] = 0;
  client[0] = 0;
  on = 0;
  printf("[trakt] credential forgotten (signed out)\n");
}

int trakt_set(const char *tk, const char *cli) {
  if (!tk || !*tk) return 0;
  snprintf(token, sizeof token, "%s", tk);
  if (cli && *cli) snprintf(client, sizeof client, "%s", cli);
  on = token[0] && client[0];
  // A NEW token clears the refusal mark — this is the same door the renewal
  // (traktauth) and a fresh pairing both come through. Registering the listener
  // here rather than at startup means it is armed exactly when there is a
  // credential that can be refused.
  credRefused = 0;
  net_notify_401(warn401);
  printf("[trakt] credential from the account: %s\n",
         on ? "active" : "no application client id (see tools/env.sh)");
  return on;
}


int trakt_headers(const char **header, char *auth, size_t nAuth,
                     char *key, size_t nKey) {
  if (!on) return 0;
  snprintf(auth, nAuth, "Authorization: Bearer %s", token);
  snprintf(key, nKey, "trakt-api-key: %s", client);
  header[0] = auth; header[1] = "trakt-api-version: 2"; header[2] = key; header[3] = NULL;
  return 1;
}

int trakt_load(const char *dirArt) {
  char path[600], line[300], *tab;
  FILE *f;
  snprintf(path, sizeof path, "%s/trakt.txt", dirArt ? dirArt : ".");
  f = fopen(path, "r");
  if (!f) { printf("[trakt] no %s\n", path); return 0; }
  if (fgets(line, sizeof line, f)) {
    char *end;
    tab = strchr(line, '\t');
    if (tab) {
      *tab = 0;
      snprintf(client, sizeof client, "%s", tab + 1);
      end = client + strlen(client);
      while (end > client && (end[-1] == '\n' || end[-1] == '\r')) *--end = 0;
    }
    snprintf(token, sizeof token, "%s", line);
  }
  fclose(f);
  on = token[0] && client[0];
  printf("[trakt] %s\n", on ? "credential loaded" : "credential incomplete");
  return on;
}

// The WORDS of a Cinemeta /meta body — name, synopsis, score, "year · runtime" —
// into `d`, leaving its art alone. decorate() takes the art as well; describe()
// takes only this.
static void wordsOf(CatItem *d, const char *body, const char *kind) {
  if (!d->title[0]) js_text(body, NULL, "name", d->title, sizeof d->title);
  js_text(body, NULL, "description", d->synopsis, sizeof d->synopsis);
  // THE GENRES, off the same body, in the shape every reader expects: the type
  // first, then each genre through disc_genre_label, joined by "  \xc2\xb7  ".
  // Without them a Trakt card reached the hero as the bare "Movie", and the
  // detail prefetch filled "Movie \xe2\x80\xa2 Action" in 400 ms into a rest on the
  // card: the meta line grew under the eye on every card of these rows.
  { const char *g = js_array(body, NULL, "genres");
    char list[sizeof d->genre];
    snprintf(list, sizeof list, "%s", strcmp(kind, "series") ? "Movie" : "TV Show");
    while (g && *g == '"') {
      const char *p = g + 1;
      char raw[48]; size_t n = 0, used = strlen(list);
      while (*p && *p != '"' && n + 1 < sizeof raw) raw[n++] = *p++;
      raw[n] = 0;
      if (raw[0])
        snprintf(list + used, sizeof list - used, "  \xc2\xb7  %s", disc_genre_label(raw));
      while (*p && *p != '"') p++;
      if (*p == '"') p++;
      while (*p == ' ') p++;
      g = (*p == ',') ? p + 1 : NULL;
      while (g && *g == ' ') g++;
    }
    snprintf(d->genre, sizeof d->genre, "%s", list); }
  // THE IMDb SCORE, off the same body. It was never read here, so every Continue
  // watching (and Library) item reached the hero with score 0 and the mark and
  // number simply did not draw — while the same title in any catalogue row had
  // them. Stored in tenths like the rest of the catalogue; see discover.c.
  { double score = js_num(body, NULL, "imdbRating", 0.0);
    if (score > 0.0 && d->score <= 0) {
      int n10 = (int)(score * 10.0 + 0.5);
      if (n10 > 99) n10 /= 10;      // it already came multiplied
      d->score = n10;
    } }
  { char r[24] = "", year[24] = "";
    js_text(body, NULL, "runtime", r, sizeof r);
    js_text(body, NULL, "releaseInfo", year, sizeof year);
    // Both dash spellings, as in discover.c (ofMeta): the en dash Cinemeta writes
    // and the ASCII hyphen other addons do.
    { char *tr = strstr(year, "\xe2\x80\x93"); if (!tr) tr = strchr(year, '-'); if (tr) *tr = 0; }
    snprintf(d->meta, sizeof d->meta, "%.20s%s%.20s", year,
             (year[0] && r[0]) ? "  \xc2\xb7  " : "", r);
    // Minutes remaining, for the card's caption. Trakt gives the percentage and
    // Cinemeta the duration; crossing the two is the only way to have this
    // without downloading the file.
    if (d->progress > 0 && d->progress < 100) {
      int total = atoi(r);
      if (total > 0) d->remainingMin = total - (total * d->progress) / 100;
    } else if (d->progress == 0) {
      d->remainingMin = atoi(r);
    } }
}

// Art and synopsis by IMDb id. Trakt returns only identifiers and progress; the
// one with images is Cinemeta, which is the same index the addons use — so what
// appears on screen is what a source can be requested for.
static int decorate(CatItem *d, const char *kind) {
  char url[300], *body;
  char series[24];
  const char *dp;
  int ok = 0;
  snprintf(series, sizeof series, "%s", d->imdb);
  dp = strchr(series, ':');
  if (dp) *(char *)dp = 0;
  snprintf(url, sizeof url, "%s/meta/%s/%s.json", CINEMETA, kind, series);
  // 8 s and not 20: there are up to EIGHT of these in series (one per history
  // item) before the home's first row exists. Measured on the Mac: 2.1 s in the
  // good case; with one slow item it was 20 s of screen with no content at all.
  // Through the shared cache: the detail page and Next Up read this same body.
  body = net_download_cached(url, 8, DISC_META_TTL_S);
  if (!body) return 0;
  ok = js_text(body, NULL, "poster", d->poster, sizeof d->poster);
  js_text(body, NULL, "background", d->backdrop, sizeof d->backdrop);
  js_text(body, NULL, "logo", d->logo, sizeof d->logo);
  wordsOf(d, body, kind);
  // The same rewrite discover.c does on this same field, and it was missing here:
  // these are the history and watchlist items, which fill Continue watching and
  // the Library — the rows the hero sits above. See cat_backdrop_shrink.
  cat_backdrop_shrink(d->backdrop, sizeof d->backdrop, CAT_BACKDROP_HERO_W);
  if (!d->backdrop[0]) snprintf(d->backdrop, sizeof d->backdrop, "%s", d->poster);
  // THE EPISODE'S STILL, FOR THE CONTINUE WATCHING CARD, AT NO EXTRA REQUEST.
  //
  // The body already in hand carries `videos`, one entry per episode, each with
  // its own `thumbnail` — the same array publishEpisodes reads in discover.c.
  // Reading it here is what separates the card from the hero: both resolved to
  // this item's `backdrop`, so the row drew the picture already filling the top
  // of the screen and said nothing about the episode it resumes.
  //
  // The url is scanned for on the season and number the caller already put on
  // the item: trakt_resume takes them from the `episode` block of
  // /sync/playback, resumeLocal from progress.txt. A film leaves both at 0 and
  // never enters here.
  //
  // AND THE EPISODE'S NAME COMES OFF THE SAME ENTRY, which is what the local
  // half of Continue watching was missing. `videos` carries `name` beside
  // `thumbnail` (publishEpisodes in discover.c reads exactly that field), so
  // this costs nothing — the body is already in hand. Without it the card built
  // from progress.txt drew the still and the "S1 E4" kicker and then a blank
  // where the title goes, because resumeLocal knows an id, a position and a
  // season/episode and nothing else: the ONLY producer that ever filled
  // nameEpisode was trakt_resume, out of /sync/playback's `episode.title`. A
  // series watched on a Nuvio device therefore had a name on the web app and
  // none here.
  //
  // Only when it is still EMPTY: Trakt's own title arrived with the item and is
  // the person's own language, so Cinemeta does not get to overwrite it.
  if (d->season > 0 && d->episode > 0) {
    const char *v = js_array(body, NULL, "videos");
    while (v) {
      const char *fv = js_end(v);
      if ((int)js_num(v, fv, "season",  -1) == d->season &&
          (int)js_num(v, fv, "episode", -1) == d->episode) {
        js_text(v, fv, "thumbnail", d->thumbEp, sizeof d->thumbEp);
        if (!d->nameEpisode[0])
          js_text(v, fv, "name", d->nameEpisode, sizeof d->nameEpisode);
        break;
      }
      v = js_next(fv);
    }
    // Same rewrite the backdrop gets, and for the same reason: a still served
    // from TMDB's /original is a decode this card has no use for. A SMALLER RUNG
    // than the backdrop's, though — this one is drawn at 640, not 1920.
    cat_backdrop_shrink(d->thumbEp, sizeof d->thumbEp, CAT_BACKDROP_THUMB_W);
  }
  snprintf(d->age_rating, sizeof d->age_rating, "14");
  free(body);
  return ok;
}

// Only the words, for items whose ART must stay as it is. The Trakt lists build
// their posters from metahub's /medium/ (see listArt — /small/ is WebP, which this
// TV cannot decode), and Cinemeta's own `poster` is exactly that /small/ URL, so
// decorate() would trade a picture that draws for one that does not. Always 1:
// a title Cinemeta does not know keeps its card, just without a synopsis.
// THE WORDS, KEPT ON DISK BETWEEN LAUNCHES.
//
// describe() runs over the whole watchlist — up to 200 cards — on every build of
// the home, and every launch builds at least once: 200 Cinemeta requests to learn
// a synopsis, genres, a score and a runtime that change on the scale of weeks,
// and a long series' /meta is ~400 KB for them. So the few fields wordsOf reads
// are kept per title in data_dir()/words, and a launch within WORDS_TTL_S reads
// them from there.
//
// The copy is the RAW JSON of exactly those keys, taken with js_raw from the same
// body wordsOf reads, so wordsOf finds the same first occurrence of each key in
// either — the two cannot drift into two parses.
#define WORDS_TTL_S (3 * 86400)
static const char *const WORDS_KEY[] = {
  "name", "description", "genres", "imdbRating", "runtime", "releaseInfo"
};

static int wordsName(char *dst, size_t size, const char *kind, const char *id) {
  if (!data_dir()[0] || strspn(id, "tt0123456789") != strlen(id)) return 0;
  snprintf(dst, size, "words/%s-%s.json", kind, id);
  return 1;
}

static char *wordsLoad(const char *name) {
  char path[600];
  struct stat st;
  if (!data_path(path, sizeof path, name) || stat(path, &st) != 0) return NULL;
  if (time(NULL) - st.st_mtime > WORDS_TTL_S) return NULL;
  return data_read(name);
}

static void wordsStore(const char *name, const char *body) {
  static int dirMade;
  char out[8192], raw[4096];
  size_t o = 0, i;
  if (!dirMade) {
    char dir[600];
    if (data_path(dir, sizeof dir, "words")) mkdir(dir, 0755);
    dirMade = 1;
  }
  out[o++] = '{';
  for (i = 0; i < sizeof WORDS_KEY / sizeof *WORDS_KEY; i++) {
    if (!js_raw(body, NULL, WORDS_KEY[i], raw, sizeof raw)) continue;
    { int k = snprintf(out + o, sizeof out - o, "%s\"%s\":%s",
                       o > 1 ? "," : "", WORDS_KEY[i], raw);
      if (k < 0 || (size_t)k >= sizeof out - o - 1) return;   // would not fit: skip
      o += (size_t)k; }
  }
  out[o++] = '}'; out[o] = 0;
  data_write(name, out);
}

static int describe(CatItem *d, const char *kind) {
  char url[300], series[24], name[80], *body, *dp;
  int stored;
  snprintf(series, sizeof series, "%s", d->imdb);
  dp = strchr(series, ':');
  if (dp) *dp = 0;
  stored = wordsName(name, sizeof name, kind, series);
  if (stored && (body = wordsLoad(name)) != NULL) {
    wordsOf(d, body, kind);
    free(body);
    return 1;
  }
  snprintf(url, sizeof url, "%s/meta/%s/%s.json", CINEMETA, kind, series);
  body = net_download_cached(url, 8, DISC_META_TTL_S);
  if (!body) return 1;
  wordsOf(d, body, kind);
  if (stored) wordsStore(name, body);
  free(body);
  return 1;
}

// There are up to 8 GETs to Cinemeta, one per history item, and they used to be
// done IN SERIES inside the reading loop. Measured on the Mac: 2.1 s before the
// home had any network content — and this is the FIRST row, the one the owner
// sees first.
//
// Each `decorate` only writes into its own CatItem and touches no shared state,
// so parallelising is straightforward. The history's order is preserved because
// each thread writes into the slot that was already its own.
#define TK_THREADS 3

typedef struct { CatItem *d; char kind[8]; int ok; } TaskDecorate;
static int (*decorateFn)(CatItem *, const char *) = decorate;
static TaskDecorate *decorateTasks;
static int decorateN, decorateNext;
static pthread_mutex_t decorateLock = PTHREAD_MUTEX_INITIALIZER;

static void *threadDecorate(void *u) {
  (void)u;
  for (;;) {
    int mine;
    pthread_mutex_lock(&decorateLock);
    if (decorateNext >= decorateN) { pthread_mutex_unlock(&decorateLock); return NULL; }
    mine = decorateNext++;
    pthread_mutex_unlock(&decorateLock);
    decorateTasks[mine].ok = decorateFn(decorateTasks[mine].d, decorateTasks[mine].kind);
  }
}

// DECORATE `n` items across TK_THREADS threads, and only then compact:
// `decorate` fails for an item Cinemeta does not know, and the compaction is
// what removes those, preserving the order of whatever was handed in.
//
// PUBLIC because there is now a SECOND producer of bare items that need art and
// a synopsis: the local "Continue watching" list, built in discover.c out of
// progress.txt, which knows an id and a position and nothing else. It used to be
// the tail of trakt_resume; leaving it there would have meant a second copy of
// the thread pool and the compaction, and the compaction is precisely the step
// whose subtlety cost us `resumedMs` travelling on the item.
//
// It is NOT reentrant — the task queue is file-static, for the same reason the
// rest of this module's batches are — so the callers hold discover.c's lock.
// The queue above is file-static, so one batch runs at a time. The Trakt rows
// are described on discover's SECOND Trakt thread while the first may be
// decorating Continue watching; this lock is what lets them share the queue.
static pthread_mutex_t batchLock = PTHREAD_MUTEX_INITIALIZER;

static int runBatch(CatItem *output, int n, int (*fn)(CatItem *, const char *));

int trakt_decorate_batch(CatItem *output, int n) {
  return runBatch(output, n, decorate);
}

int trakt_describe_batch(CatItem *output, int n) {
  return runBatch(output, n, describe);
}

static int runBatch(CatItem *output, int n, int (*fn)(CatItem *, const char *)) {
  if (n <= 0) return 0;
  pthread_mutex_lock(&batchLock);
  decorateFn = fn;
  decorateTasks = calloc((size_t)n, sizeof(TaskDecorate));
  if (decorateTasks) {
    pthread_t threads[TK_THREADS];
    int created = 0, q, r, w;
    for (q = 0; q < n; q++) {
      decorateTasks[q].d = &output[q];
      snprintf(decorateTasks[q].kind, sizeof decorateTasks[q].kind, "%s", output[q].kind);
    }
    decorateN = n; decorateNext = 0;
    for (q = 0; q < TK_THREADS; q++)
      if (pthread_create(&threads[created], NULL, threadDecorate, NULL) == 0) created++;
    if (!created) threadDecorate(NULL);      // no threads: in series, same result
    for (q = 0; q < created; q++) pthread_join(threads[q], NULL);

    for (r = 0, w = 0; r < n; r++)
      if (decorateTasks[r].ok) { if (w != r) output[w] = output[r]; w++; }
    n = w;
    free(decorateTasks); decorateTasks = NULL; decorateN = 0;
  } else {
    // No memory for the queue: in series, on this very thread.
    int r, w;
    for (r = 0, w = 0; r < n; r++)
      if (fn(&output[r], output[r].kind)) { if (w != r) output[w] = output[r]; w++; }
    n = w;
  }
  pthread_mutex_unlock(&batchLock);
  return n;
}

// MARKS OR UNMARKS A BATCH OF EPISODES ON TRAKT.
//
// ONE POST, not one per episode: /sync/history and /sync/history/remove accept
// shows[{ids:{imdb}, seasons:[{number, episodes:[{number}]}]}], and it is that
// shape which makes "up to here" and "the whole season" a single request.
// Marking 20 episodes one at a time would be 20 round trips and 20 chances of
// finishing half done.
//
// SYNCHRONOUS, on the caller's thread. The screen has already applied the local
// effect before reaching here (watchedep_mark_batch), so what is expected here
// is the confirmation, not the drawing. A caller on the draw thread has to hand
// this to a thread of its own.
//
// The batch arrives ordered by season, but that is not assumed: the loop groups
// by looking each season up once, which for the handful of episodes in a gesture
// costs less than sorting.
int trakt_mark_episodes(const char *imdb, const WatchedPair *pairs, int count,
                        int watched) {
  const char *header[4];
  char auth[200], keyHeader[140], id[24], url[64];
  Jsw w;
  char *r;
  int i, j, st = 0, ok;
  char done[64];
  if (!on || !imdb || imdb[0] != 't' || !pairs || count < 1) return 0;
  if (count > (int)sizeof done) count = (int)sizeof done;
  for (i = 0; imdb[i] && imdb[i] != ':' && i < (int)sizeof id - 1; i++) id[i] = imdb[i];
  id[i] = 0;
  if (!id[0]) return 0;
  memset(done, 0, sizeof done);

  jsw_start(&w);
  jsw_obj_start(&w);
  jsw_key(&w, "shows");
  jsw_arr_start(&w);
  jsw_obj_start(&w);
  jsw_key(&w, "ids");
  jsw_obj_start(&w);
  jsw_cs(&w, "imdb", id);
  jsw_obj_end(&w);
  jsw_key(&w, "seasons");
  jsw_arr_start(&w);
  for (i = 0; i < count; i++) {
    if (done[i]) continue;
    jsw_obj_start(&w);
    jsw_ci(&w, "number", pairs[i].season);
    jsw_key(&w, "episodes");
    jsw_arr_start(&w);
    for (j = i; j < count; j++) {
      if (done[j] || pairs[j].season != pairs[i].season) continue;
      done[j] = 1;
      jsw_obj_start(&w);
      jsw_ci(&w, "number", pairs[j].episode);
      jsw_obj_end(&w);
    }
    jsw_arr_end(&w);
    jsw_obj_end(&w);
  }
  jsw_arr_end(&w);
  jsw_obj_end(&w);
  jsw_arr_end(&w);
  jsw_obj_end(&w);

  snprintf(auth, sizeof auth, "Authorization: Bearer %s", token);
  snprintf(keyHeader, sizeof keyHeader, "trakt-api-key: %s", client);
  header[0] = auth;
  header[1] = "trakt-api-version: 2";
  header[2] = keyHeader;
  header[3] = NULL;
  snprintf(url, sizeof url, "https://api.trakt.tv/sync/history%s",
           watched ? "" : "/remove");
  r = net_post_st(url, 20, header, jsw_text_final(&w), &st);
  jsw_free(&w);
  ok = st >= 200 && st < 300;
  free(r);
  printf("[trakt] %s %d episode(s) of %s -> %s (HTTP %d)\n",
         watched ? "mark" : "unmark", count, id, ok ? "ok" : "failed", st);
  fflush(stdout);
  return ok;
}

// The resume bar comes from /sync/playback and does not say whether the title
// was marked as watched. We query the real history once in the same discovery
// cycle so the modal does not treat high progress as proof of having watched.
// For series, records with `episode` are deliberately ignored: having seen one
// episode does not mean the whole series was marked as watched.
static void loadHistoryReal(const char *const *header) {
  char *body = net_download_headers("https://api.trakt.tv/sync/history?limit=100&extended=full", 25, header);
  const char *p;
  if (!body) return;
  p = strchr(body, '[');
  p = p ? p + 1 : NULL;
  while (p && *p) {
    const char *f, *obj;
    char id[24] = "";
    const char *kind = NULL;
    while (*p && (unsigned char)*p <= ' ') p++;
    if (*p != '{') break;
    f = js_end(p);
    if (strstr(p, "\"episode\"") && strstr(p, "\"episode\"") < f) {
      p = js_next(f);
      continue;
    }
    obj = strstr(p, "\"movie\"");
    if (obj && obj < f) kind = "movie";
    else {
      obj = strstr(p, "\"show\"");
      if (obj && obj < f) kind = "series";
    }
    if (obj && kind) {
      const char *fo = js_end(strchr(obj, '{'));
      js_text(obj, fo, "imdb", id, sizeof id);
      if (id[0]) cat_history_set_id(id, kind, 1);
    }
    p = js_next(f);
  }
  free(body);
}

int trakt_playback_remove(const char *imdb) {
  const char *header[4];
  char auth[200], key[140], work[24], url[96];
  char *body;
  const char *p;
  int ok = 1, found = 0;
  if (!on) return 1;
  if (!imdb || imdb[0] != 't') return 0;
  snprintf(work, sizeof work, "%.*s", (int)strcspn(imdb, ":"), imdb);
  snprintf(auth, sizeof auth, "Authorization: Bearer %s", token);
  snprintf(key, sizeof key, "trakt-api-key: %s", client);
  header[0] = auth;
  header[1] = "trakt-api-version: 2";
  header[2] = key;
  header[3] = NULL;
  body = net_download_headers("https://api.trakt.tv/sync/playback", 25, header);
  if (!body) { printf("[trakt] playback remove %s: no response\n", work); return 0; }
  // Every resume point of the WORK goes: all of a series' episodes, not only the
  // one on the card, or the next build would surface the one before it.
  p = strchr(body, '[');
  p = p ? p + 1 : NULL;
  while (p && *p) {
    const char *f, *block;
    char id[24] = "";
    while (*p && (unsigned char)*p <= ' ') p++;
    if (*p != '{') break;
    f = js_end(p);
    block = strstr(p, "\"show\"");
    if (!block || block > f) block = strstr(p, "\"movie\"");
    if (block && block < f) {
      const char *fb = js_end(strchr(block, '{'));
      js_text(block, fb, "imdb", id, sizeof id);
    }
    if (id[0] && !strcmp(id, work)) {
      // The playback's own id sits at the top of the entry; the nested ones are
      // under "ids", which the quoted key does not match.
      long long pid = (long long)js_num(p, f, "id", 0.0);
      if (pid > 0) {
        int status = 0;
        char *r;
        snprintf(url, sizeof url, "https://api.trakt.tv/sync/playback/%lld", pid);
        r = net_delete_st(url, 20, header, &status);
        free(r);
        found++;
        // 404 is "already gone", which is what was asked for.
        if (!(status >= 200 && status < 300) && status != 404) ok = 0;
        printf("[trakt] playback remove %s #%lld -> HTTP %d\n", work, pid, status);
      }
    }
    p = js_next(f);
  }
  free(body);
  if (!found) printf("[trakt] playback remove %s: nothing on Trakt\n", work);
  fflush(stdout);
  return ok;
}

int trakt_resume(CatItem *output, int max) {
  const char *header[4];
  char auth[200], key[140];
  char *body;
  const char *p;
  int n = 0;
  if (!on) return 0;
  snprintf(auth, sizeof auth, "Authorization: Bearer %s", token);
  snprintf(key, sizeof key, "trakt-api-key: %s", client);
  header[0] = auth;
  header[1] = "trakt-api-version: 2";
  header[2] = key;
  header[3] = NULL;
  body = net_download_headers("https://api.trakt.tv/sync/playback?extended=full", 25, header);
  if (!body) { printf("[trakt] no response\n"); return 0; }
  // The body is an array at the root; js_array searches by key, so we walk by hand.
  p = strchr(body, '[');
  p = p ? p + 1 : NULL;
  while (p && *p && n < max) {
    const char *f;
    while (*p && (unsigned char)*p <= ' ') p++;
    if (*p != '{') break;
    f = js_end(p);
    {
      CatItem *d = &output[n];
      const char *ep = strstr(p, "\"episode\"");
      int series = ep && ep < f;
      char imdb[24] = "";
      memset(d, 0, sizeof *d);
      d->progress = (int)js_num(p, f, "progress", 0.0);
      // THE 1%-TO-90% WINDOW, WHICH THIS PATH NEVER APPLIED. The local path
      // always did, and /sync/playback is a much dirtier source: it keeps every
      // resume point any Trakt client ever recorded, which is where the reports
      // of "films and series I never watched" in Continue watching came from —
      // entries sitting at 0% that were opened once by something else, and
      // entries at 98% that are simply finished. Below 1% it was not started,
      // from 90% on it is over, and "continue" is neither.
      if (d->progress < 1 || d->progress >= 90) { p = js_next(f); continue; }
      // WHEN it was paused. Without it every Trakt item entered with an unknown
      // instant and the row fell back to the order the API answered in, so a
      // title watched last month could sit in front of one watched an hour ago.
      // It is stamped on the ITEM because the batch gets compacted after
      // decorating — see resumedMs in catalog.h.
      { char pausedAt[40] = "";
        js_text(p, f, "paused_at", pausedAt, sizeof pausedAt);
        d->resumedMs = js_ms_iso(pausedAt); }
      // The "movie"/"show" block holds the title and the ids; the "episode" one
      // carries the season and number. Looking for "imdb" across the whole range
      // would pick the episode's, which the addons also accept but which does
      // not identify the work.
      { const char *block = strstr(p, series ? "\"show\"" : "\"movie\"");
        if (block && block < f) {
          const char *fb = js_end(strchr(block, '{'));
          js_text(block, fb, "title", d->title, sizeof d->title);
          js_text(block, fb, "imdb", imdb, sizeof imdb);
          // Trakt's `ids` carry the TMDB id too: with it on the item the detail
          // page's cast and fact sheet need no lookup of their own.
          d->tmdb = (long)js_num(block, fb, "tmdb", 0);
        } }
      if (!imdb[0]) { p = js_next(f); continue; }
      if (series) {
        const char *fe = js_end(strchr(ep, '{'));
        d->season = (int)js_num(ep, fe, "season", 0);
        d->episode  = (int)js_num(ep, fe, "number", 0);
        js_text(ep, fe, "title", d->nameEpisode, sizeof d->nameEpisode);
        snprintf(d->imdb, sizeof d->imdb, "%s:%d:%d", imdb,
                 d->season ? d->season : 1, d->episode ? d->episode : 1);
        snprintf(d->kind, sizeof d->kind, "series");
      } else {
        snprintf(d->imdb, sizeof d->imdb, "%s", imdb);
        snprintf(d->kind, sizeof d->kind, "movie");
      }
      // Decorating is left for AFTER the loop, in parallel. Here the item is
      // already assembled: only the art and the synopsis are missing, and those
      // come from the network.
      n++;
    }
    p = js_next(f);
  }
  free(body);
  loadHistoryReal(header);

  n = trakt_decorate_batch(output, n);

  printf("[trakt] %d in progress\n", n);
  fflush(stdout);
  return n;
}

// The fields every Trakt card shares, from the IMDb id alone.
static void listArt(CatItem *d, const char *imdb, int series) {
  snprintf(d->imdb, sizeof d->imdb, "%s", imdb);
  snprintf(d->kind, sizeof d->kind, "%s", series ? "series" : "movie");
  // Art WITHOUT a query: the metahub URLs are deterministic from the
  // IMDb id (verified, 200 on everything tested). One query per item cost
  // ~0.3 s and limited the list to ten; this way it can be as long as the
  // owner's list, and the image is only downloaded when it appears on
  // screen — tex_cache already does that.
  snprintf(d->poster, sizeof d->poster,
           // "medium" and not "small", and the difference is NOT size:
           // metahub serves poster/small as image/WEBP and poster/medium
           // as image/jpeg. THIS TV's libSDL2_image loads libjpeg,
           // libpng16 and libtiff through dlopen and does NOT load
           // libwebp — the only format error string inside it is "WEBP
           // images are not supported". (libwebp.so.7 does exist on the
           // system; it is SDL2_image that was not built against it.)
           //
           // The effect of small: EVERY card coming from Trakt
           // (watchlist, collection, the whole Library) never decoded —
           // and worse, the cache does not store failure, so every frame
           // tried again and burned a decode slot. It was the biggest
           // cause of "not all the posters show up".
           //
           // Cost: 105 KB against 31 KB. Cheap, for the art to exist.
           "https://images.metahub.space/poster/medium/%s/img", imdb);
  snprintf(d->backdrop, sizeof d->backdrop,
           "https://images.metahub.space/background/medium/%s/img", imdb);
  snprintf(d->logo, sizeof d->logo,
           "https://images.metahub.space/logo/medium/%s/img", imdb);
  snprintf(d->genre, sizeof d->genre, "%s",
           series ? "TV Show" : "Movie");
  snprintf(d->age_rating, sizeof d->age_rating, "14");
}

int trakt_list(const char *which, CatItem *output, int max) {
  const char *header[4];
  char auth[200], key[140], url[160], *body;
  const char *p;
  int n = 0, step;
  if (!on) return 0;
  snprintf(auth, sizeof auth, "Authorization: Bearer %s", token);
  snprintf(key, sizeof key, "trakt-api-key: %s", client);
  header[0] = auth; header[1] = "trakt-api-version: 2"; header[2] = key; header[3] = NULL;

  // Films and series come from separate endpoints; mixing the two lists into the
  // same row is what the owner sees as "My List".
  for (step = 0; step < 2 && n < max; step++) {
    const char *kind = step ? "shows" : "movies";
    snprintf(url, sizeof url, "https://api.trakt.tv/sync/%s/%s", which, kind);
    body = net_download_headers(url, 25, header);
    if (!body) continue;
    p = strchr(body, '[');
    p = p ? p + 1 : NULL;
    while (p && *p && n < max) {
      const char *f;
      while (*p && (unsigned char)*p <= ' ') p++;
      if (*p != '{') break;
      f = js_end(p);
      {
        CatItem *d = &output[n];
        const char *block = strstr(p, step ? "\"show\"" : "\"movie\"");
        char imdb[24] = "";
        memset(d, 0, sizeof *d);
        if (block && block < f) {
          const char *fb = js_end(strchr(block, '{'));
          js_text(block, fb, "title", d->title, sizeof d->title);
          js_text(block, fb, "imdb", imdb, sizeof imdb);
          d->year = (int)js_num(block, fb, "year", 0);
          d->tmdb = (long)js_num(block, fb, "tmdb", 0);
        }
        // The date on the list item itself, outside the movie/show block. A
        // collected show has no `collected_at`, only `last_collected_at`.
        { char when[40] = "";
          const char *key = !strcmp(which, "watchlist") ? "listed_at"
                          : step ? "last_collected_at" : "collected_at";
          if (js_text(p, f, key, when, sizeof when)) d->added = js_ms_iso(when); }
        if (imdb[0]) {
          if (!strcmp(which, "watchlist")) d->inList = 1;
          else                            d->inCollection = 1;
          listArt(d, imdb, step);
          n++;
        }
      }
      p = js_next(f);
    }
    free(body);
  }
  printf("[trakt] %s: %d\n", which, n);
  fflush(stdout);
  return n;
}

int trakt_recommendations(CatItem *output, int max) {
  const char *header[4];
  char auth[200], key[140], url[200], *body;
  CatItem *byKind[2];
  int got[2] = { 0, 0 }, per, step, i, n = 0;
  if (!on || max < 1) return 0;
  if (!trakt_headers(header, auth, sizeof auth, key, sizeof key)) return 0;
  per = max / 2 > 0 ? max / 2 : 1;
  byKind[0] = calloc((size_t)per, sizeof(CatItem));
  byKind[1] = calloc((size_t)per, sizeof(CatItem));
  if (!byKind[0] || !byKind[1]) { free(byKind[0]); free(byKind[1]); return 0; }

  // What the owner already listed or owns is left out by Trakt itself, as on the
  // web: a recommendation for something already on the watchlist row says nothing.
  for (step = 0; step < 2; step++) {
    const char *p;
    snprintf(url, sizeof url,
             "https://api.trakt.tv/recommendations/%s?limit=%d"
             "&ignore_collected=true&ignore_watchlisted=true",
             step ? "shows" : "movies", per);
    body = net_download_headers(url, 25, header);
    if (!body) continue;
    p = strchr(body, '[');
    p = p ? p + 1 : NULL;
    // Unlike /sync/watchlist, each element IS the movie or show — there is no
    // wrapper holding it under a "movie"/"show" key.
    while (p && *p && got[step] < per) {
      const char *f;
      char imdb[24] = "";
      CatItem *d = &byKind[step][got[step]];
      while (*p && (unsigned char)*p <= ' ') p++;
      if (*p != '{') break;
      f = js_end(p);
      memset(d, 0, sizeof *d);
      js_text(p, f, "title", d->title, sizeof d->title);
      js_text(p, f, "imdb", imdb, sizeof imdb);
      d->year = (int)js_num(p, f, "year", 0);
      d->tmdb = (long)js_num(p, f, "tmdb", 0);
      if (imdb[0]) { listArt(d, imdb, step); got[step]++; }
      p = js_next(f);
    }
    free(body);
  }
  // Trakt ranks each type on its own, so both rankings are kept by taking them in
  // turn rather than one type after the other.
  for (i = 0; i < per && n < max; i++) {
    if (i < got[0]) output[n++] = byKind[0][i];
    if (i < got[1] && n < max) output[n++] = byKind[1][i];
  }
  free(byKind[0]); free(byKind[1]);
  printf("[trakt] recommendations: %d\n", n);
  fflush(stdout);
  return n;
}

// --- recording progress: the scrobble -----------------------------------------
//
// THREE MESSAGES, ONE ORDERED QUEUE. /scrobble/start says "watching now" — what
// Trakt shows on the profile and what the owner's other devices see while this TV
// plays. /scrobble/pause keeps the point reached. /scrobble/stop at 90% or more
// records the watch. The player sends start on play, pause on pause, and the
// closing pause-or-stop on the way out (trakt_mark).
//
// A queue and not the single thread-with-a-flag that was here: with only the
// closing call that flag was enough, but a start still in flight when the viewer
// pauses would have DROPPED the pause, and a pause in flight on exit would have
// dropped the stop — the one message that marks the episode watched. Events are
// sent one at a time, in the order they happened. Four slots: they come seconds
// apart, and when they do pile up the oldest is the one that no longer matters.
enum { SCROBBLE_START, SCROBBLE_PAUSE, SCROBBLE_END };
#define BRAND_QUEUE 4
typedef struct { char id[64]; double pos, duration; int action; } Brand;
static pthread_mutex_t lockBrand = PTHREAD_MUTEX_INITIALIZER;
static Brand brandQueue[BRAND_QUEUE];
static int nBrand, threadBrandAlive;

static void sendOneBrand(const Brand *b) {
  const char *header[4], *path, *verb;
  char auth[200], key[140], body[400], *r;
  char id[24];
  int t = 0, e = 0;
  const char *dp;
  double pct;
  snprintf(id, sizeof id, "%s", b->id);
  dp = strchr(id, ':');
  if (dp) { sscanf(dp + 1, "%d:%d", &t, &e); *(char *)dp = 0; }
  pct = 100.0 * b->pos / b->duration;
  if (pct < 0.0) pct = 0.0;
  if (pct > 100.0) pct = 100.0;

  snprintf(auth, sizeof auth, "Authorization: Bearer %s", token);
  snprintf(key, sizeof key, "trakt-api-key: %s", client);
  header[0] = auth; header[1] = "trakt-api-version: 2"; header[2] = key; header[3] = NULL;

  // Pause preserves the point; stop records completion. We keep this client's
  // conservative 90% threshold. Pause on its own never completes the episode.
  if (t > 0 && e > 0)
    snprintf(body, sizeof body,
             "{\"show\":{\"ids\":{\"imdb\":\"%s\"}},"
             "\"episode\":{\"season\":%d,\"number\":%d},\"progress\":%.2f}",
             id, t, e, pct);
  else
    snprintf(body, sizeof body,
             "{\"movie\":{\"ids\":{\"imdb\":\"%s\"}},\"progress\":%.2f}",
             id, pct);

  if (b->action == SCROBBLE_START) { path = "start"; verb = "start"; }
  else if (b->action == SCROBBLE_END && pct >= 90) { path = "stop"; verb = "stop"; }
  else { path = "pause"; verb = "pause"; }
  { char url[64];
    snprintf(url, sizeof url, "https://api.trakt.tv/scrobble/%s", path);
    r = net_post(url, 20, header, body); }
  printf("[trakt] %s %s %.1f%% -> %s\n", verb, b->id, pct, r ? "ok" : "failed");
  fflush(stdout);
  free(r);
}

static void *sendBrand(void *u) {
  (void)u;
  for (;;) {
    Brand b;
    pthread_mutex_lock(&lockBrand);
    if (!nBrand) { threadBrandAlive = 0; pthread_mutex_unlock(&lockBrand); return NULL; }
    b = brandQueue[0];
    memmove(brandQueue, brandQueue + 1, (size_t)(nBrand - 1) * sizeof *brandQueue);
    nBrand--;
    pthread_mutex_unlock(&lockBrand);
    // Checked per message, not per batch: a sign-out between two of them must
    // stop the rest from going to the departing account.
    if (on) sendOneBrand(&b);
  }
}

static void scrobble(const char *imdb, double posSeg, double durationSeg, int action) {
  pthread_t thread;
  if (!on || !imdb || !*imdb || durationSeg <= 1.0) return;
  pthread_mutex_lock(&lockBrand);
  if (nBrand == BRAND_QUEUE) {
    memmove(brandQueue, brandQueue + 1, (size_t)(BRAND_QUEUE - 1) * sizeof *brandQueue);
    nBrand--;
  }
  snprintf(brandQueue[nBrand].id, sizeof brandQueue[nBrand].id, "%s", imdb);
  brandQueue[nBrand].pos = posSeg;
  brandQueue[nBrand].duration = durationSeg;
  brandQueue[nBrand].action = action;
  nBrand++;
  if (!threadBrandAlive) {
    threadBrandAlive = 1;
    if (pthread_create(&thread, NULL, sendBrand, NULL) != 0) { threadBrandAlive = 0; nBrand = 0; }
    else pthread_detach(thread);
  }
  pthread_mutex_unlock(&lockBrand);
}

void trakt_mark(const char *imdb, double posSeg, double durationSeg) {
  scrobble(imdb, posSeg, durationSeg, SCROBBLE_END);
}
void trakt_scrobble_start(const char *imdb, double posSeg, double durationSeg) {
  scrobble(imdb, posSeg, durationSeg, SCROBBLE_START);
}
void trakt_scrobble_pause(const char *imdb, double posSeg, double durationSeg) {
  scrobble(imdb, posSeg, durationSeg, SCROBBLE_PAUSE);
}

// --- WATCHLIST: writing and reading -------------------------------------------
//
// The title screen's "+" button only touched a local array (library.c), so the
// owner's list on their other devices never knew. Now it talks to Trakt, which
// is already the source of truth for the rest of the app.
//
// The STATE matters too: without reading back, the button showed "+" even for a
// title that was already on the list, and a second press would add it again.
// ci->inList is already filled in by trakt_list during discovery; what was
// missing was keeping that field up to date after a write of our own.
static char targetList[24];
static char targetListKind[8];
static int  targetAdd, threadListAlive;
static pthread_t threadList;

static void *sendList(void *u) {
  const char *header[4];
  char auth[200], key[140], url[120], body[200], id[24], kindItemBuf[8];
  char *resp;
  int status = 0, confirmed;
  int add;
  (void)u;
  pthread_mutex_lock(&lockList);
  snprintf(id, sizeof id, "%s", targetList);
  snprintf(kindItemBuf, sizeof kindItemBuf, "%s", targetListKind);
  add = targetAdd;
  pthread_mutex_unlock(&lockList);
  if (!trakt_headers(header, auth, sizeof auth, key, sizeof key)) {
    stateWrite(&listState, TK_OP_FAILURE);
    pthread_mutex_lock(&lockList); threadListAlive = 0; pthread_mutex_unlock(&lockList);
    return NULL;
  }
  // The type is part of the intent: sending film and series together lets the API
  // resolve the IMDb id in the wrong scope and makes the confirmation ambiguous.
  if (!strcmp(kindItemBuf, "series"))
    snprintf(body, sizeof body, "{\"shows\":[{\"ids\":{\"imdb\":\"%s\"}}]}", id);
  else
    snprintf(body, sizeof body, "{\"movies\":[{\"ids\":{\"imdb\":\"%s\"}}]}", id);
  snprintf(url, sizeof url, "https://api.trakt.tv/sync/watchlist%s",
           add ? "" : "/remove");
  resp = net_post_st(url, 20, header, body, &status);
  confirmed = status >= 200 && status < 300;
  stateWrite(&listState, confirmed ? TK_OP_CONFIRMED : TK_OP_FAILURE);
  printf("[trakt] watchlist %s %s (%s) -> %s (HTTP %d)\n",
         add ? "add" : "del", id, kindItemBuf,
         confirmed ? "confirmed" : "failed", status);
  fflush(stdout);
  free(resp);
  pthread_mutex_lock(&lockList); threadListAlive = 0; pthread_mutex_unlock(&lockList);
  return NULL;
}

// A DIFFERENT endpoint from trakt_mark: that one is /scrobble/pause ("I stopped
// here"), which the player uses on the way out. This one is /sync/history ("I
// watched it"), which is what the eye button means.
//
// trakt_mark could not be reused: it keeps `durationSeg <= 1.0 -> return` so as
// not to send a scrobble with an invalid duration, and the eye's caller passed
// exactly dur=1.0 — the function returned on its first line and NOTHING was
// sent. The button looked like it worked (the local mirror changed) and Trakt
// never knew.
static pthread_t threadHistory;
static int       threadHistoryAlive, historyAdd;
static char      targetHistory[24];
static char      targetHistoryKind[8];

static void *sendHistory(void *u) {
  const char *header[4];
  char auth[200], key[140], url[120], body[200], id[24], kindItemBuf[8];
  char *resp;
  int status = 0, confirmed;
  int mark;
  (void)u;
  pthread_mutex_lock(&lockHistory);
  snprintf(id, sizeof id, "%s", targetHistory);
  snprintf(kindItemBuf, sizeof kindItemBuf, "%s", targetHistoryKind);
  mark = historyAdd;
  pthread_mutex_unlock(&lockHistory);
  if (!trakt_headers(header, auth, sizeof auth, key, sizeof key)) {
    stateWrite(&historyState, TK_OP_FAILURE);
    pthread_mutex_lock(&lockHistory); threadHistoryAlive = 0; pthread_mutex_unlock(&lockHistory);
    return NULL;
  }
  // The command's scope is explicit. For a series the target is the show, not an
  // episode derived from progress, and not a second array of the opposite type.
  if (!strcmp(kindItemBuf, "series"))
    snprintf(body, sizeof body, "{\"shows\":[{\"ids\":{\"imdb\":\"%s\"}}]}", id);
  else
    snprintf(body, sizeof body, "{\"movies\":[{\"ids\":{\"imdb\":\"%s\"}}]}", id);
  snprintf(url, sizeof url, "https://api.trakt.tv/sync/history%s",
           mark ? "" : "/remove");
  resp = net_post_st(url, 20, header, body, &status);
  confirmed = status >= 200 && status < 300;
  stateWrite(&historyState, confirmed ? TK_OP_CONFIRMED : TK_OP_FAILURE);
  if (confirmed) cat_history_set_id(id, kindItemBuf, mark);
  printf("[trakt] history %s %s (%s) -> %s (HTTP %d)\n",
         mark ? "add" : "del", id, kindItemBuf,
         confirmed ? "confirmed" : "failed", status);
  fflush(stdout);
  free(resp);
  pthread_mutex_lock(&lockHistory); threadHistoryAlive = 0; pthread_mutex_unlock(&lockHistory);
  return NULL;
}

int trakt_watched_kind(const char *imdb, const char *kind, int mark) {
  const char *dp;
  // WITHOUT TRAKT the mark belongs to the Nuvio account (acclib.h), the way the
  // web app keeps it when Trakt is not the source. The local mirror changes now
  // and the account hears of it on the light cycle that follows, so the answer
  // here is already "confirmed" — there is no request for the modal to wait on.
  if (!on && acclib_active() && imdb && !strncmp(imdb, "tt", 2)) {
    char id[24];
    size_t k = strcspn(imdb, ":");
    if (k >= sizeof id) k = sizeof id - 1;
    memcpy(id, imdb, k); id[k] = 0;
    acclib_watched(id, kind_item(kind, imdb), 0, 0, mark);
    cat_history_set_id(id, kind_item(kind, imdb), mark);
    stateWrite(&historyState, TK_OP_CONFIRMED);
    return 1;
  }
  if (!on || !imdb || imdb[0] != 't') {
    stateWrite(&historyState, TK_OP_FAILURE);
    return 0;
  }
  pthread_mutex_lock(&lockHistory);
  if (threadHistoryAlive) {
    pthread_mutex_unlock(&lockHistory);
    return 0;
  }
  // "tt123:2:5" (an episode) becomes "tt123": the history belongs to the TITLE.
  dp = strchr(imdb, ':');
  { size_t k = dp ? (size_t)(dp - imdb) : strlen(imdb);
    if (k >= sizeof targetHistory) k = sizeof targetHistory - 1;
    memcpy(targetHistory, imdb, k); targetHistory[k] = 0; }
  snprintf(targetHistoryKind, sizeof targetHistoryKind, "%s", kind_item(kind, imdb));
  historyAdd = mark;
  stateWrite(&historyState, TK_OP_PENDING);
  threadHistoryAlive = 1;
  pthread_mutex_unlock(&lockHistory);
  if (pthread_create(&threadHistory, NULL, sendHistory, NULL) != 0) {
    pthread_mutex_lock(&lockHistory); threadHistoryAlive = 0; pthread_mutex_unlock(&lockHistory);
    stateWrite(&historyState, TK_OP_FAILURE);
    return 0;
  }
  else pthread_detach(threadHistory);
  return 1;
}

int trakt_watchlist_kind(const char *imdb, const char *kind, int add) {
  const char *dp;
  // Without Trakt, the account's own library — see trakt_watched_kind.
  if (!on && acclib_active() && imdb && !strncmp(imdb, "tt", 2)) {
    acclib_list(imdb, kind_item(kind, imdb), add);
    stateWrite(&listState, TK_OP_CONFIRMED);
    return 1;
  }
  if (!on || !imdb || imdb[0] != 't') {
    stateWrite(&listState, TK_OP_FAILURE);
    return 0;
  }
  pthread_mutex_lock(&lockList);
  if (threadListAlive) {
    pthread_mutex_unlock(&lockList);
    return 0;
  }
  dp = strchr(imdb, ':');
  { size_t k = dp ? (size_t)(dp - imdb) : strlen(imdb);
    if (k >= sizeof targetList) k = sizeof targetList - 1;
    memcpy(targetList, imdb, k); targetList[k] = 0; }
  snprintf(targetListKind, sizeof targetListKind, "%s", kind_item(kind, imdb));
  targetAdd = add;
  stateWrite(&listState, TK_OP_PENDING);
  threadListAlive = 1;
  pthread_mutex_unlock(&lockList);
  if (pthread_create(&threadList, NULL, sendList, NULL) != 0) {
    pthread_mutex_lock(&lockList); threadListAlive = 0; pthread_mutex_unlock(&lockList);
    stateWrite(&listState, TK_OP_FAILURE);
    return 0;
  }
  else pthread_detach(threadList);
  return 1;
}

void trakt_watched(const char *imdb, int mark) {
  (void)trakt_watched_kind(imdb, cat_kind_by_imdb(imdb), mark);
}

void trakt_watchlist(const char *imdb, int add) {
  (void)trakt_watchlist_kind(imdb, cat_kind_by_imdb(imdb), add);
}
