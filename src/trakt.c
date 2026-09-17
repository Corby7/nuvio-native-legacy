#include "trakt.h"
#include "net.h"
#include "js.h"
#include "watchedep.h"
#include "jsw.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>

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
  body = net_download(url, 8);
  if (!body) return 0;
  ok = js_text(body, NULL, "poster", d->poster, sizeof d->poster);
  js_text(body, NULL, "background", d->backdrop, sizeof d->backdrop);
  js_text(body, NULL, "logo", d->logo, sizeof d->logo);
  if (!d->title[0]) js_text(body, NULL, "name", d->title, sizeof d->title);
  js_text(body, NULL, "description", d->synopsis, sizeof d->synopsis);
  // The same rewrite discover.c does on this same field, and it was missing here:
  // these are the history and watchlist items, which fill Continue watching and
  // the Library — the rows the hero sits above. See cat_backdrop_shrink.
  cat_backdrop_shrink(d->backdrop, sizeof d->backdrop);
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
    // from TMDB's /original is a decode this card has no use for.
    cat_backdrop_shrink(d->thumbEp, sizeof d->thumbEp);
  }
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
  snprintf(d->genre, sizeof d->genre, "%s",
           strcmp(kind, "series") ? "Film" : "TV Show");
  snprintf(d->age_rating, sizeof d->age_rating, "14");
  free(body);
  return ok;
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
    decorateTasks[mine].ok = decorate(decorateTasks[mine].d, decorateTasks[mine].kind);
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
int trakt_decorate_batch(CatItem *output, int n) {
  if (n <= 0) return 0;
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
      if (decorate(&output[r], output[r].kind)) { if (w != r) output[w] = output[r]; w++; }
    n = w;
  }
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

// Some clients get a 401 only on the aggregated feed. The graph and the
// profiles' public history stay reachable with the same credential.
static char *socialByFollowed(const char *const *header, int max) {
  char *list=net_download_headers("https://api.trakt.tv/users/me/following?extended=full",10,header);
  if(!list)return NULL;
  char *out=calloc(1,262144);size_t used=1;int n=0,queried=0;
  if(!out){free(list);return NULL;}out[0]='[';
  const char *p=strchr(list,'[');p=p?p+1:NULL;
  while(p&&*p&&n<max&&queried<8) {
    while(*p&&(unsigned char)*p<=' ')p++;
    if(*p!='{')break;
    const char *f=js_end(p),*u=strstr(p,"\"user\"");
    if(!u||u>=f){p=js_next(f);continue;}
    u=strchr(u,'{');const char *uf=js_end(u);char id[128]="",url[400];
    js_text(u,uf,"slug",id,sizeof id);
    if(!id[0] || strspn(id,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")!=strlen(id)){p=js_next(f);continue;}
    queried++;
    snprintf(url,sizeof url,"https://api.trakt.tv/users/%s/watching?extended=full",id);
    char *body=net_download_headers(url,8,header);int now=body&&strchr(body,'{');
    if(!now){free(body);snprintf(url,sizeof url,"https://api.trakt.tv/users/%s/history?limit=1&extended=full",id);body=net_download_headers(url,8,header);}
    const char *b=body?strchr(body,'{'):NULL,*bf=b?js_end(b):NULL;
    if(b&&bf&&bf>b+1) {
      size_t un=(size_t)(uf-u),bn=(size_t)(bf-b-2);
      if(used+un+bn+80<262144){
        int k=snprintf(out+used,262144-used,"%s{\"user\":%.*s,%s%.*s}",n?",":"",(int)un,u,
            now?"\"action\":\"watching\",":"",(int)bn,b+1);
        used+=(size_t)k;n++;
      }
    }
    free(body);p=js_next(f);
  }
  out[used++]=']';out[used]=0;free(list);
  printf("[trakt] social: %d follows queried, %d activities\n",queried,n);
  return out;
}

int trakt_social(CatItem *output, int max) {
  const char *header[4];
  char auth[200], key[140], *body;
  const char *p;
  int n = 0;
  if (!on || max < 1) return 0;
  if (!trakt_headers(header, auth, sizeof auth, key, sizeof key)) return 0;
  body = net_download_headers(
    "https://api.trakt.tv/users/me/friends/activities?extended=full&page=1&limit=12",
    20, header);
  // Accounts without the new social scope may get a 401 on the `friends` graph,
  // even though the token is still valid for history. `following` is the honest
  // fallback: they are still people the owner chose, never global activity.
  if (!body) body = net_download_headers(
    "https://api.trakt.tv/users/me/following/activities?extended=full&page=1&limit=12",
    20, header);
  if (!body) body=socialByFollowed(header,max);
  if (!body) { printf("[trakt] social feed unavailable\n"); return 0; }
  p = strchr(body, '['); p = p ? p + 1 : NULL;
  while (p && *p && n < max) {
    const char *f, *bu, *bm, *bs, *be, *fb;
    CatItem *d;
    char imdb[24] = "", person[64] = "", action[32] = "";
    while (*p && (unsigned char)*p <= ' ') p++;
    if (*p != '{') break;
    f = js_end(p);
    bu = strstr(p, "\"user\"");
    bm = strstr(p, "\"movie\"");
    bs = strstr(p, "\"show\"");
    be = strstr(p, "\"episode\"");
    if (!bu || bu >= f || ((!bm || bm >= f) && (!bs || bs >= f))) {
      p = js_next(f); continue;
    }
    d = &output[n]; memset(d, 0, sizeof *d);
    fb = js_end(strchr(bu, '{'));
    js_text(bu, fb, "name", person, sizeof person);
    if (!person[0]) js_text(bu, fb, "username", person, sizeof person);
    js_text(bu, fb, "slug", d->socialSlug, sizeof d->socialSlug);
    const char *avatar = strstr(bu, "\"avatar\"");
    if (avatar && avatar < fb) js_text(avatar, fb, "full", d->socialAvatar, sizeof d->socialAvatar);
    snprintf(d->socialName, sizeof d->socialName, "%s", person[0] ? person : "Friend");
    js_text(p, f, "action", action, sizeof action);
    snprintf(d->country, sizeof d->country, "%s", person[0] ? person : "Friend");
    snprintf(d->providerName, sizeof d->providerName, "%s",
             !strcmp(action,"watching") ? "watching now" :
             !strcmp(action, "watch") || !strcmp(action, "scrobble") ? "watched" :
             !strcmp(action, "checkin") ? "checked in" :
             !strcmp(action, "rating") ? "rated" : "recent activity");
    snprintf(d->socialAction, sizeof d->socialAction, "%s", d->providerName);
    if (bs && bs < f) {
      fb = js_end(strchr(bs, '{'));
      js_text(bs, fb, "title", d->title, sizeof d->title);
      js_text(bs, fb, "imdb", imdb, sizeof imdb);
      snprintf(d->kind, sizeof d->kind, "series");
      if (be && be < f) {
        const char *fe = js_end(strchr(be, '{'));
        d->season = (int)js_num(be, fe, "season", 0);
        d->episode = (int)js_num(be, fe, "number", 0);
        js_text(be, fe, "title", d->nameEpisode, sizeof d->nameEpisode);
        snprintf(d->directing, sizeof d->directing, "S%dE%d%s%s", d->season,
                 d->episode, d->nameEpisode[0] ? "  \xc2\xb7  " : "",
                 d->nameEpisode);
      }
    } else {
      fb = js_end(strchr(bm, '{'));
      js_text(bm, fb, "title", d->title, sizeof d->title);
      js_text(bm, fb, "imdb", imdb, sizeof imdb);
      snprintf(d->kind, sizeof d->kind, "movie");
      snprintf(d->directing, sizeof d->directing, "Film");
    }
    if (!imdb[0]) { p = js_next(f); continue; }
    snprintf(d->imdb, sizeof d->imdb, "%s", imdb);
    n++;
    p = js_next(f);
  }
  free(body);
  // The feed already arrives ordered most-recent-first. The art is resolved in
  // parallel, with the same limit of three connections Continue Watching uses.
  if (n > 0) {
    TaskDecorate *tasksSoc = calloc((size_t)n, sizeof *tasksSoc);
    if (tasksSoc) {
      pthread_t threads[TK_THREADS]; int created = 0, q;
      decorateTasks = tasksSoc; decorateN = n; decorateNext = 0;
      for (q = 0; q < n; q++) {
        tasksSoc[q].d = &output[q];
        snprintf(tasksSoc[q].kind, sizeof tasksSoc[q].kind, "%s", output[q].kind);
      }
      for (q = 0; q < TK_THREADS; q++)
        if (pthread_create(&threads[created], NULL, threadDecorate, NULL) == 0) created++;
      if (!created) threadDecorate(NULL);
      for (q = 0; q < created; q++) pthread_join(threads[q], NULL);
      // Unavailable art must not erase a real person from the feed.
      free(tasksSoc); decorateTasks = NULL; decorateN = 0;
    }
  }
  printf("[trakt] %d friend activities\n", n); fflush(stdout);
  return n;
}

static void profileGenre(ProfileData *d, const char *name) {
  int i;
  if (!name || !*name) return;
  static const char *en[]={"drama","science-fiction","comedy","crime","thriller","action","mystery","history","fantasy","horror","adventure","romance","documentary","animation"};
  // Trakt sends lowercase slugs ("science-fiction"); these are the labels the
  // interface shows.
  static const char *label[]={"Drama","Science Fiction","Comedy","Crime","Thriller","Action","Mystery","History","Fantasy","Horror","Adventure","Romance","Documentary","Animation"};
  for (unsigned k=0;k<sizeof en/sizeof *en;k++) if(!strcmp(name,en[k])) {name=label[k];break;}
  for (i=0;i<d->nGenres;i++) if (!strcmp(d->genres[i].name,name)) {
    d->genres[i].count++; return;
  }
  if (d->nGenres < PROFILE_MAX_GENRES) {
    ProfileGenre *g=&d->genres[d->nGenres++];
    snprintf(g->name,sizeof g->name,"%s",name); g->count=1;
  }
}

static void profileGenresJson(ProfileData *d, const char *b, const char *f) {
  const char *g = strstr(b,"\"genres\"");
  if (!g || g >= f || !(g=strchr(g,'[')) || g>=f) return;
  g++;
  while (g < f) {
    char name[40]; size_t n=0;
    while (g<f && *g!='\"' && *g!=']') g++;
    if (g>=f || *g==']') break;
    g++;
    while (g<f && *g!='\"' && n+1<sizeof name) name[n++]=*g++;
    name[n]=0; profileGenre(d,name);
    if (g<f) g++;
  }
}

int trakt_profile(ProfileData *d) {
  const char *header[4]; char auth[200],key[140],url[360],*body;
  time_t now=time(NULL); struct tm tmv=*localtime(&now);
  char start[48]; int daysInMonth;
  ProfileHighlight *ranking;
  int nRanking = 0;
  if (!d || !on) return 0;
  memset(d,0,sizeof *d);
  if (!trakt_headers(header,auth,sizeof auth,key,sizeof key)) return 0;
  static const char *months[]={"January","February","March","April","May","June","July","August","September","October","November","December"};
  snprintf(d->period,sizeof d->period,"%s %d",months[tmv.tm_mon],tmv.tm_year+1900);
  struct tm first=tmv;
  first.tm_mday=1;first.tm_hour=first.tm_min=first.tm_sec=0;first.tm_isdst=-1;
  time_t limit=mktime(&first);struct tm utc;
  gmtime_r(&limit,&utc);
  strftime(start,sizeof start,"%Y-%m-%dT%H%%3A%M%%3A%SZ",&utc);
  // Profile and avatar. The avatar may be WebP on newer Trakt; the renderer only
  // asks for it if the firmware accepts it, and the screen is still complete without it.
  body=net_download_headers("https://api.trakt.tv/users/settings?extended=full",15,header);
  if(body){ const char *u=strstr(body,"\"user\""); const char *fu=u?js_end(strchr(u,'{')):NULL;
    if(u&&fu){js_text(u,fu,"name",d->name,sizeof d->name);js_text(u,fu,"username",d->user,sizeof d->user);
      js_text(u,fu,"full",d->avatar,sizeof d->avatar);} free(body); }
  snprintf(url,sizeof url,
    "https://api.trakt.tv/users/me/history?start_at=%s&extended=full&page=1&limit=100",start);
  body=net_download_headers(url,25,header);
  if (!body || !strchr(body, '[')) { free(body); return 0; }
  ranking = calloc(100, sizeof *ranking);
  if (!ranking) { free(body); return 0; }
  d->partial = 1;
  snprintf(d->warning, sizeof d->warning,
           "A slice of the 100 most recent plays this month. Runtimes reported by Trakt.");
  { const char *p=strchr(body,'['); p=p?p+1:NULL;
    while(p&&*p){
      const char *f,*bm,*bs,*be,*obj,*fo; char watched[32]="",imdb[24]="",title[128]="";
      int runtime=0,t=0,e=0,hi=-1;
      while(*p&&(unsigned char)*p<=' ')p++; if(*p!='{')break; f=js_end(p);
      js_text(p,f,"watched_at",watched,sizeof watched);
      bm=strstr(p,"\"movie\""); bs=strstr(p,"\"show\""); be=strstr(p,"\"episode\"");
      obj=(bs&&bs<f)?bs:((bm&&bm<f)?bm:NULL); if(!obj){p=js_next(f);continue;}
      fo=js_end(strchr(obj,'{')); js_text(obj,fo,"title",title,sizeof title); js_text(obj,fo,"imdb",imdb,sizeof imdb);
      runtime=(int)js_num(obj,fo,"runtime",0); profileGenresJson(d,obj,fo);
      if(be&&be<f){const char *fe=js_end(strchr(be,'{'));t=(int)js_num(be,fe,"season",0);e=(int)js_num(be,fe,"number",0);
        {int re=(int)js_num(be,fe,"runtime",0);if(re>0)runtime=re;} d->episodes++;}
      else d->movies++;
      d->plays++; if (runtime > 0) d->minutes += runtime;
      // js_ms_iso and not a parse of its own: this was the third copy of the
      // same ISO reading in the app. See the note on it in js.h.
      { long long wms = js_ms_iso(watched);
        if (wms) {
          struct tm local; time_t stamp = (time_t)(wms / 1000);
          localtime_r(&stamp,&local);
          if(local.tm_year==tmv.tm_year&&local.tm_mon==tmv.tm_mon&&local.tm_mday>=1&&local.tm_mday<=31)
            d->activity[local.tm_mday-1]++;
        }
      }
      for(int i=0;i<nRanking;i++)if(imdb[0]&&!strcmp(ranking[i].id,imdb)){hi=i;break;}
      if(hi<0&&imdb[0]&&nRanking<100){hi=nRanking++;ProfileHighlight *h=&ranking[hi];
        snprintf(h->id,sizeof h->id,"%s",imdb);snprintf(h->title,sizeof h->title,"%s",title);
        if(t>0&&e>0)snprintf(h->detail,sizeof h->detail,"S%dE%d",t,e);else snprintf(h->detail,sizeof h->detail,"Film");
        if(imdb[0]){snprintf(h->poster,sizeof h->poster,"https://images.metahub.space/poster/medium/%s/img",imdb);
          snprintf(h->backdrop,sizeof h->backdrop,"https://images.metahub.space/background/medium/%s/img",imdb);}}
      if(hi>=0){ranking[hi].plays++;if(runtime>0)ranking[hi].minutes+=runtime;}
      p=js_next(f);
    }
  }
  free(body);
  for(int i=0;i<nRanking;i++)for(int j=i+1;j<nRanking;j++)
    if(ranking[j].plays>ranking[i].plays){ProfileHighlight x=ranking[i];ranking[i]=ranking[j];ranking[j]=x;}
  d->nHighlights=nRanking<PROFILE_MAX_HIGHLIGHTS?nRanking:PROFILE_MAX_HIGHLIGHTS;
  memcpy(d->highlights,ranking,d->nHighlights*sizeof *ranking);
  free(ranking);
  daysInMonth=31; if(tmv.tm_mon==1) daysInMonth=((tmv.tm_year+1900)%4==0)?29:28;
  else if(tmv.tm_mon==3||tmv.tm_mon==5||tmv.tm_mon==8||tmv.tm_mon==10)daysInMonth=30;
  d->nDays=daysInMonth;
  {struct tm first=tmv;first.tm_mday=1;mktime(&first);d->firstDayWeek=first.tm_wday;}
  for(int i=0;i<d->nDays;i++)if(d->activity[i])d->daysActiveMonth++;
  // A monthly slice does not prove annual activity.
  d->daysActiveYear=0;
  for(int i=tmv.tm_mday-1;i>=0&&i<d->nDays;i--){if(!d->activity[i])break;d->streakCurrent++;}
  // Sorts highlights and genres by volume so the visual reading is honest.
  for(int i=0;i<d->nHighlights;i++)for(int j=i+1;j<d->nHighlights;j++)if(d->highlights[j].plays>d->highlights[i].plays){ProfileHighlight x=d->highlights[i];d->highlights[i]=d->highlights[j];d->highlights[j]=x;}
  for(int i=0;i<d->nGenres;i++)for(int j=i+1;j<d->nGenres;j++)if(d->genres[j].count>d->genres[i].count){ProfileGenre x=d->genres[i];d->genres[i]=d->genres[j];d->genres[j]=x;}
  printf("[trakt] profile: %d plays, %d min, %d highlights\n",d->plays,d->minutes,d->nHighlights);fflush(stdout);
  return 1;
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
        }
        if (imdb[0]) {
          snprintf(d->imdb, sizeof d->imdb, "%s", imdb);
          snprintf(d->kind, sizeof d->kind, "%s", step ? "series" : "movie");
          if (!strcmp(which, "watchlist")) d->inList = 1;
          else                            d->inCollection = 1;
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
                   step ? "TV Show" : "Film");
          snprintf(d->age_rating, sizeof d->age_rating, "14");
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

// --- gravar progresso -------------------------------------------------------

static char brandId[64];
static double brandPos, brandDuration;
static pthread_t threadBrand;
static int threadBrandAlive;

static void *sendBrand(void *u) {
  const char *header[4];
  char auth[200], key[140], body[400], *r;
  char id[24];
  int t = 0, e = 0;
  const char *dp;
  double pct;
  (void)u;
  snprintf(id, sizeof id, "%s", brandId);
  dp = strchr(id, ':');
  if (dp) { sscanf(dp + 1, "%d:%d", &t, &e); *(char *)dp = 0; }
  pct = 100.0 * brandPos / brandDuration;
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

  r = net_post(pct >= 90 ? "https://api.trakt.tv/scrobble/stop" :
                             "https://api.trakt.tv/scrobble/pause", 20, header, body);
  printf("[trakt] %s %s %.1f%% -> %s\n", pct>=90?"stop":"pause",brandId,pct,r?"ok":"failed");
  fflush(stdout);
  free(r);
  threadBrandAlive = 0;
  return NULL;
}

void trakt_mark(const char *imdb, double posSeg, double durationSeg) {
  if (!on || !imdb || !*imdb || durationSeg <= 1.0 || threadBrandAlive) return;
  snprintf(brandId, sizeof brandId, "%s", imdb);
  brandPos = posSeg; brandDuration = durationSeg;
  threadBrandAlive = 1;
  if (pthread_create(&threadBrand, NULL, sendBrand, NULL) != 0) threadBrandAlive = 0;
  else pthread_detach(threadBrand);
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
