#include "person.h"
#include "discover.h"
#include "net.h"
#include "js.h"
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define TMDB "https://api.themoviedb.org/3"

// ---------------------------------------------------------------------------
// THE CAST LIST
// ---------------------------------------------------------------------------
typedef struct {
  char name[64], role[64], photo[200];
  long person;
  int eps;
} Member;
// FOUR TITLES' casts, not one. A title opened from "More like this" or "Also in"
// replaced the only list, so Back to the title before it fell to the catalogue's six
// until TMDB answered again — and a face further down than six was clamped away,
// with the page it restored. Four covers the Back chain anyone walks in practice.
#define CAST_SLOTS 4
typedef struct {
  long title;            // 0 = empty
  unsigned used;
  int n;
  Member m[CAST_MAX];
} CastList;
static CastList lists[CAST_SLOTS];
static unsigned castClock;
// The title asked for, and whether it is a series. The thread loops until the
// wanted title is in a slot, so a title asked for while another is in flight is
// fetched next rather than dropped.
static long castWanted;
static int castSeries, castThread;
static pthread_mutex_t castLock = PTHREAD_MUTEX_INITIALIZER;
// The wanted title's slot, or NULL. Only the main thread reads it, and the fetch
// thread never evicts the wanted title's slot, so it stays valid.
static const CastList *castCur;

static int castFind(long id) {
  int i;
  for (i = 0; i < CAST_SLOTS; i++) if (id > 0 && lists[i].title == id) return i;
  return -1;
}

static void *castFetch(void *arg) {
  (void)arg;
  for (;;) {
    char url[400], *body;
    CastList *got;
    long id;
    int series, k = 0;
    const char *key = disc_key_tmdb();

    pthread_mutex_lock(&castLock);
    id = castWanted; series = castSeries;
    if (id <= 0 || castFind(id) >= 0 || !key || !key[0]) {
      castThread = 0; pthread_mutex_unlock(&castLock); return NULL;
    }
    pthread_mutex_unlock(&castLock);

    // AGGREGATE credits on a series: /tv/<id>/credits is only the LATEST season's
    // cast, so a series that recast lost everyone who had left. The aggregate is the
    // whole run, and it carries each actor's episode count.
    snprintf(url, sizeof url, "%s/%s/%ld/%s?api_key=%s&language=en-US", TMDB,
             series ? "tv" : "movie", id,
             series ? "aggregate_credits" : "credits", key);
    body = net_download(url, 20);
    got = calloc(1, sizeof *got);
    if (body && got) {
      // The first "cast" of the document is the cast array; "crew" comes after it.
      const char *p = js_array(body, NULL, "cast");
      while (p && k < CAST_MAX) {
        const char *end = js_end(p);
        Member *mb = &got->m[k];
        char path[128] = "";
        js_text(p, end, "name", mb->name, sizeof mb->name);
        // On a series the character lives inside `roles`; the first match of the
        // key is the first role, which is the one the actor is credited as.
        js_text(p, end, "character", mb->role, sizeof mb->role);
        mb->person = (long)js_num(p, end, "id", 0.0);
        mb->eps = series ? (int)js_num(p, end, "total_episode_count", 0.0) : 0;
        if (js_text(p, end, "profile_path", path, sizeof path) && path[0] == '/')
          snprintf(mb->photo, sizeof mb->photo,
                   "https://image.tmdb.org/t/p/w185%s", path);
        if (mb->name[0]) k++;
        p = js_next(end);
      }
    }
    pthread_mutex_lock(&castLock);
    // Stored even when it failed, as an empty list: the page then keeps the
    // catalogue's six and this title is not asked again on every frame.
    if (got) {
      int i, slot = -1;
      for (i = 0; i < CAST_SLOTS; i++) {
        if (lists[i].title == castWanted && castWanted != id) continue;
        if (slot < 0 || lists[i].used < lists[slot].used) slot = i;
      }
      if (slot >= 0) {
        got->title = id; got->n = k; got->used = ++castClock;
        lists[slot] = *got;
      }
    }
    pthread_mutex_unlock(&castLock);
    printf("[cast] %ld -> %d members\n", id, k); fflush(stdout);
    free(got);
    free(body);
  }
}

void cast_request(long titleTmdb, int series) {
  if (titleTmdb <= 0) return;
  pthread_mutex_lock(&castLock);
  if (castWanted == titleTmdb) { pthread_mutex_unlock(&castLock); return; }
  castWanted = titleTmdb;
  castSeries = series;
  { int i = castFind(titleTmdb);
    if (i >= 0) { lists[i].used = ++castClock; pthread_mutex_unlock(&castLock); return; } }
  // The running thread loops and picks the new title up when it finishes.
  if (castThread) { pthread_mutex_unlock(&castLock); return; }
  castThread = 1;
  pthread_mutex_unlock(&castLock);
  { pthread_t th;
    if (pthread_create(&th, NULL, castFetch, NULL) != 0) {
      pthread_mutex_lock(&castLock); castThread = 0; pthread_mutex_unlock(&castLock);
    } else pthread_detach(th); }
}

int cast_n(long titleTmdb) {
  int i;
  castCur = NULL;
  if (titleTmdb <= 0 || titleTmdb != castWanted) return 0;
  pthread_mutex_lock(&castLock);
  i = castFind(titleTmdb);
  if (i >= 0) castCur = &lists[i];
  pthread_mutex_unlock(&castLock);
  return castCur ? castCur->n : 0;
}
#define MEM(i, field, none) \
  ((castCur && (i) >= 0 && (i) < castCur->n) ? castCur->m[i].field : (none))
const char *cast_name(int i)  { return MEM(i, name, ""); }
const char *cast_role(int i)  { return MEM(i, role, ""); }
const char *cast_photo(int i) { return MEM(i, photo, ""); }
long cast_person(int i)       { return MEM(i, person, 0); }
int  cast_episodes(int i)     { return MEM(i, eps, 0); }

// ---------------------------------------------------------------------------
// THE PERSON
// ---------------------------------------------------------------------------
// Named because sorting by popularity needs a temporary variable of the same
// type, and an anonymous struct does not let you declare another.
typedef struct {
  char t[120], p[80], y[8], po[160], im[16];
  // A combined_credits credit has NO imdb_id — that field only exists on the
  // title's detailed endpoint. Without keeping the TMDB id and the type, there
  // was no way to translate the credit into anything Cinemeta understands.
  long tmdb;
  char kind[8];          // "movie" or "tv"
  double pop;
} Credit;

typedef struct {
  long id;
  unsigned used;         // the LRU clock: the entry with the smallest goes first
  char photo[200], bio[1400], born[120];
  Credit cred[PES_MAX];
  int nCred;
} Person;

// EIGHT PEOPLE, because the cast page asks for one on every step down the list and
// the viewer walks back up it as often as down. A step onto someone already seen
// then draws at once instead of waiting on TMDB again.
#define PES_CACHE 8
static Person cache[PES_CACHE];
static unsigned clockLru;
static long idRequest, idFailed;
static int threadAlive;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
// The current person's entry, or NULL. Only the main thread reads it, and the fetch
// thread never evicts the entry idRequest names (see store), so it stays valid.
static const Person *cur;

static int find(long id) {
  int i;
  for (i = 0; i < PES_CACHE; i++) if (id > 0 && cache[i].id == id) return i;
  return -1;
}

static void parse(const char *body, Person *o) {
  char dob[16] = "", place[96] = "";
  int k = 0;
  js_text(body, NULL, "biography", o->bio, sizeof o->bio);
  { char path[128] = "";
    if (js_text(body, NULL, "profile_path", path, sizeof path) && path[0] == '/')
      snprintf(o->photo, sizeof o->photo, "https://image.tmdb.org/t/p/w342%s", path); }
  // The biography's stand-in: the year and the place, as much as TMDB has.
  js_text(body, NULL, "birthday", dob, sizeof dob);
  js_text(body, NULL, "place_of_birth", place, sizeof place);
  if (strlen(dob) >= 4 && place[0])
    snprintf(o->born, sizeof o->born, "Born %.4s in %s", dob, place);
  else if (strlen(dob) >= 4) snprintf(o->born, sizeof o->born, "Born %.4s", dob);
  else if (place[0])         snprintf(o->born, sizeof o->born, "Born in %s", place);

  // `combined_credits.cast` — js_array finds the document's first "cast". The root
  // object has no other "cast", so this is the one.
  { const char *p = js_array(body, NULL, "cast");
    while (p && k < PES_MAX) {
      const char *end = js_end(p);
      Credit *c = &o->cred[k];
      char date[16] = "", path[128] = "";
      memset(c, 0, sizeof *c);
      // A film has "title"/"release_date"; a series has "name"/"first_air_date".
      if (!js_text(p, end, "title", c->t, sizeof c->t))
        js_text(p, end, "name", c->t, sizeof c->t);
      js_text(p, end, "character", c->p, sizeof c->p);
      if (!js_text(p, end, "release_date", date, sizeof date))
        js_text(p, end, "first_air_date", date, sizeof date);
      if (strlen(date) >= 4) { memcpy(c->y, date, 4); c->y[4] = 0; }
      if (js_text(p, end, "poster_path", path, sizeof path) && path[0] == '/')
        snprintf(c->po, sizeof c->po, "https://image.tmdb.org/t/p/w342%s", path);
      js_text(p, end, "imdb_id", c->im, sizeof c->im);
      c->tmdb = (long)js_num(p, end, "id", 0.0);
      js_text(p, end, "media_type", c->kind, sizeof c->kind);
      c->pop = js_num(p, end, "popularity", 0.0);
      // A talk-show appearance or an award night is a credit with no poster, and a
      // row of grey cards says nothing: "Also in" is for work you can open.
      if (c->t[0] && c->po[0]) k++;
      p = js_next(end);
    } }

  // By POPULARITY, like the web (`right.popularity - left.popularity`). The order
  // TMDB returns is chronological, and with it the work the person is known for
  // ends up at the end of the list.
  { int i, j;
    for (i = 1; i < k; i++) {
      Credit tmp = o->cred[i];
      for (j = i; j > 0 && o->cred[j-1].pop < tmp.pop; j--) o->cred[j] = o->cred[j-1];
      o->cred[j] = tmp;
    } }
  o->nCred = k;
}

// Under the lock. The least recently used entry goes, but never the one the page is
// showing: a step back onto a cached face while another is in flight must not have
// that face evicted from under the drawing.
static void store(const Person *p) {
  int i, slot = find(p->id);
  if (slot < 0)
    for (i = 0; i < PES_CACHE; i++) {
      if (cache[i].id == idRequest && idRequest != p->id) continue;
      if (slot < 0 || cache[i].used < cache[slot].used) slot = i;
    }
  if (slot < 0) return;
  cache[slot] = *p;
  cache[slot].used = ++clockLru;
}

static void *fetch(void *arg) {
  (void)arg;
  // It LOOPS: while one person is in flight the viewer may have stepped on to three
  // more, and only the last of them matters. The old single-shot thread dropped every
  // request that arrived while it was busy, so a quick walk down the cast left the
  // panel on whoever was asked for first.
  for (;;) {
    char url[400], *body;
    Person *p;
    long id;
    const char *key = disc_key_tmdb();

    pthread_mutex_lock(&lock);
    id = idRequest;
    if (id <= 0 || find(id) >= 0 || id == idFailed || !key || !key[0]) {
      threadAlive = 0; pthread_mutex_unlock(&lock); return NULL;
    }
    pthread_mutex_unlock(&lock);

    // The SAME request as the web's (castDetailScreen.js:204): the profile and the
    // combined credits in a single call.
    snprintf(url, sizeof url,
             "%s/person/%ld?api_key=%s&language=en-US"
             "&append_to_response=combined_credits", TMDB, id, key);
    body = net_download(url, 20);
    p = body ? calloc(1, sizeof *p) : NULL;
    if (p) { p->id = id; parse(body, p); }
    pthread_mutex_lock(&lock);
    // A failure is remembered so the loop does not hammer TMDB for the same face;
    // stepping to anyone else clears it.
    if (p) store(p); else idFailed = id;
    pthread_mutex_unlock(&lock);
    if (p) { printf("[person] %ld -> %d credits\n", id, p->nCred); fflush(stdout); }
    free(p);
    free(body);
  }
}

void person_request(long tmdbId) {
  if (tmdbId <= 0) return;
  pthread_mutex_lock(&lock);
  if (idRequest != tmdbId) idFailed = 0;
  idRequest = tmdbId;
  { int i = find(tmdbId);
    if (i >= 0) { cache[i].used = ++clockLru; pthread_mutex_unlock(&lock); return; } }
  if (threadAlive) { pthread_mutex_unlock(&lock); return; }
  threadAlive = 1;
  pthread_mutex_unlock(&lock);
  { pthread_t th;
    if (pthread_create(&th, NULL, fetch, NULL) != 0) {
      pthread_mutex_lock(&lock); threadAlive = 0; pthread_mutex_unlock(&lock);
    } else pthread_detach(th); }
}

int person_ready(long tmdbId) {
  int i;
  cur = NULL;
  if (tmdbId <= 0 || tmdbId != idRequest) return 0;
  pthread_mutex_lock(&lock);
  i = find(tmdbId);
  if (i >= 0) cur = &cache[i];
  pthread_mutex_unlock(&lock);
  return cur != NULL;
}
const char *person_photo(void) { return cur ? cur->photo : ""; }
const char *person_bio(void)   { return cur ? cur->bio : ""; }
const char *person_born(void)  { return cur ? cur->born : ""; }

int person_n_credits(void) { return cur ? cur->nCred : 0; }
#define CRED(i, field, none) \
  ((cur && (i) >= 0 && (i) < cur->nCred) ? cur->cred[i].field : (none))
const char *person_credit_title(int i)  { return CRED(i, t, ""); }
const char *person_credit_role(int i)   { return CRED(i, p, ""); }
const char *person_credit_year(int i)   { return CRED(i, y, ""); }
const char *person_credit_poster(int i) { return CRED(i, po, ""); }
const char *person_credit_imdb(int i)   { return CRED(i, im, ""); }
long person_credit_tmdb(int i)          { return CRED(i, tmdb, 0); }
const char *person_credit_kind(int i)   { return CRED(i, kind, ""); }
