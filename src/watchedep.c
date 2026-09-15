#include "watchedep.h"
#include "js.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

// A CEILING. Somebody following 100 series of 50 episodes each has 5000
// entries; at 24 bytes that is 120 KB, which fits comfortably. The ceiling
// exists so an absurd response cannot turn into unbounded memory, not because
// 8000 is a special number. Once it is hit the map STOPS GROWING and says so in
// the log — what is already in it stays valid, because half a map beats none.
#define WEP_MAX 8000

typedef struct { char id[16]; short season, episode; unsigned char watched; } Mark;
static Mark *map;
static int n, cap, warnedCeiling;

// A LOCK, because the two ends of this map are on different threads: the Trakt
// reader fills it on the extras worker thread while the episode list reads it
// from the draw loop. Without one, the realloc that grows the map frees the
// array a drawing thread is walking — a crash that would surface as "the app
// closes when I open a series", which is the worst kind to chase.
//
// The cost is a lock per query on an uncontended mutex, next to nothing beside
// the rasterising each row already does. The PUBLIC functions take it; the
// static helpers below assume it is already held, and the names say so.
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

// The title's id only. "tt123:2:8" and "tt123" have to match: the first is the
// form CatItem.imdb carries on a "Continue watching" item, and a caller does not
// always know which of the two it is holding.
static void base(const char *origin, char *dst, size_t size) {
  size_t k = 0;
  if (!dst || !size) return;
  dst[0] = 0;
  if (!origin) return;
  while (origin[k] && origin[k] != ':' && k + 1 < size) { dst[k] = origin[k]; k++; }
  dst[k] = 0;
}

static int find(const char *id, int s, int e) {
  int i;
  for (i = 0; i < n; i++)
    if (map[i].season == s && map[i].episode == e && !strcmp(map[i].id, id)) return i;
  return -1;
}

// Assumes the lock is held.
static void setLocked(const char *imdb, int season, int episode, int watched) {
  char id[16];
  int i;
  base(imdb, id, sizeof id);
  if (!id[0] || season < 0 || episode < 1) return;
  i = find(id, season, episode);
  if (i >= 0) { map[i].watched = watched ? 1 : 0; return; }
  if (n >= WEP_MAX) {
    if (!warnedCeiling) {
      warnedCeiling = 1;
      printf("[watchedep] ceiling of %d episodes; the map stops growing\n", WEP_MAX);
      fflush(stdout);
    }
    return;
  }
  if (n == cap) {
    int want = cap ? cap * 2 : 256;
    Mark *m;
    if (want > WEP_MAX) want = WEP_MAX;
    m = (Mark *)realloc(map, (size_t)want * sizeof *m);
    if (!m) return;
    map = m; cap = want;
  }
  memset(&map[n], 0, sizeof map[n]);
  snprintf(map[n].id, sizeof map[n].id, "%s", id);
  map[n].season = (short)season;
  map[n].episode = (short)episode;
  map[n].watched = watched ? 1 : 0;
  n++;
}

void watchedep_set(const char *imdb, int season, int episode, int watched) {
  pthread_mutex_lock(&lock);
  setLocked(imdb, season, episode, watched);
  pthread_mutex_unlock(&lock);
}

// Assumes the lock is held — the batch helper reads and writes inside a single
// critical section rather than taking the lock once per episode.
static int stateLocked(const char *imdb, int season, int episode) {
  char id[16];
  int i;
  base(imdb, id, sizeof id);
  if (!id[0]) return -1;
  i = find(id, season, episode);
  return i < 0 ? -1 : (int)map[i].watched;
}

int watchedep_state(const char *imdb, int season, int episode) {
  int r;
  pthread_mutex_lock(&lock);
  r = stateLocked(imdb, season, episode);
  pthread_mutex_unlock(&lock);
  return r;
}

int watchedep_count(const char *imdb) {
  char id[16];
  int i, k = 0;
  base(imdb, id, sizeof id);
  if (!id[0]) return 0;
  pthread_mutex_lock(&lock);
  for (i = 0; i < n; i++) if (map[i].watched && !strcmp(map[i].id, id)) k++;
  pthread_mutex_unlock(&lock);
  return k;
}

int watchedep_known(const char *imdb) {
  char id[16];
  int i, found = 0;
  base(imdb, id, sizeof id);
  if (!id[0]) return 0;
  pthread_mutex_lock(&lock);
  for (i = 0; i < n; i++) if (!strcmp(map[i].id, id)) { found = 1; break; }
  pthread_mutex_unlock(&lock);
  return found;
}

int watchedep_mark_batch(const char *imdb, const WatchedPair *pairs, int count,
                         int watched) {
  int i, changed = 0;
  if (!pairs) return 0;
  // ONE critical section for the whole batch: "the whole season" is dozens of
  // episodes, and the list redraws from another thread between them. Half a
  // marked season on screen for a frame is exactly the flicker this avoids.
  pthread_mutex_lock(&lock);
  for (i = 0; i < count; i++) {
    if (stateLocked(imdb, pairs[i].season, pairs[i].episode) == (watched ? 1 : 0))
      continue;
    setLocked(imdb, pairs[i].season, pairs[i].episode, watched);
    changed++;
  }
  pthread_mutex_unlock(&lock);
  return changed;
}

// EPISODE ORDER IS (season, number), in that order — not the number on its own.
// Season 0, the specials, comes BEFORE season 1, which is where Trakt puts it
// too; marking "up to here" on season 2 episode 3 must not drag season 3 in
// merely because its episode number is smaller.
static int beforeOrEqual(int s, int e, int sTarget, int eTarget) {
  if (s != sTarget) return s < sTarget;
  return e <= eTarget;
}

int watchedep_up_to_here(const char *imdb, int season, int episode,
                         WatchedPair *out, int max) {
  char id[16];
  int i, k = 0;
  base(imdb, id, sizeof id);
  if (!id[0]) return 0;
  pthread_mutex_lock(&lock);
  // `out` NULL IS COUNT MODE, and `max` is ignored in it — see the header. The
  // screen needs the NUMBER before it decides whether to offer the action, and
  // without this it would have to allocate an array just to learn the size, or,
  // worse, pass max=0 and always be told zero.
  for (i = 0; i < n; i++) {
    if (strcmp(map[i].id, id)) continue;
    if (!beforeOrEqual(map[i].season, map[i].episode, season, episode)) continue;
    if (out) {
      if (k >= max) break;
      out[k].season = map[i].season;
      out[k].episode = map[i].episode;
    }
    k++;
  }
  pthread_mutex_unlock(&lock);
  return k;
}

int watchedep_season(const char *imdb, int season, WatchedPair *out, int max) {
  char id[16];
  int i, k = 0;
  base(imdb, id, sizeof id);
  if (!id[0]) return 0;
  pthread_mutex_lock(&lock);
  for (i = 0; i < n; i++) {                 // out NULL = count, see above
    if (map[i].season != season || strcmp(map[i].id, id)) continue;
    if (out) {
      if (k >= max) break;
      out[k].season = map[i].season;
      out[k].episode = map[i].episode;
    }
    k++;
  }
  pthread_mutex_unlock(&lock);
  return k;
}

int watchedep_n(void) {
  int r;
  pthread_mutex_lock(&lock);
  r = n;
  pthread_mutex_unlock(&lock);
  return r;
}

void watchedep_forget(void) {
  pthread_mutex_lock(&lock);
  free(map); map = NULL; n = 0; cap = 0; warnedCeiling = 0;
  pthread_mutex_unlock(&lock);
}

// ------------------------------------------------------------------- Trakt

// /shows/<id>/progress/watched:
//   { aired, completed, seasons: [ { number, aired, completed,
//       episodes: [ { number, completed, last_watched_at } ] } ] }
//
// HERE `completed` IS EXPLICIT, which is why this reader writes 0 AS WELL as 1.
// That is the difference from a map assembled out of "what was played": there,
// the absence of an episode proves nothing; here the response enumerates the
// whole series and says yes or no for every line. After this read,
// watchedep_state only answers -1 for a series that was never asked about — and
// the screen can trust the 0.
int watchedep_read_progress(const char *imdb, const char *json) {
  const char *seasons;
  const char *end;
  int total = 0;
  if (!imdb || !imdb[0] || !json) return -1;
  end = json + strlen(json);
  seasons = js_array(json, end, "seasons");
  if (!seasons) {
    printf("[watchedep] %s: response with no \"seasons\"\n", imdb);
    fflush(stdout);
    return -1;
  }
  for (; seasons && *seasons == '{'; seasons = js_next(js_end(seasons))) {
    const char *seasonEnd = js_end(seasons), *eps;
    int sn = (int)js_num(seasons, seasonEnd, "number", -1.0);
    if (!seasonEnd || sn < 0) break;
    eps = js_array(seasons, seasonEnd, "episodes");
    for (; eps && *eps == '{'; eps = js_next(js_end(eps))) {
      const char *epEnd = js_end(eps);
      int en, done;
      if (!epEnd) break;
      en = (int)js_num(eps, epEnd, "number", -1.0);
      // js_flag, and not a hand-rolled search for the literal "true": this layer
      // already has a boolean reader that respects the object's bounds, and a
      // substring search does not — it would happily find the `true` of a
      // NEIGHBOURING field.
      done = js_flag(eps, epEnd, "completed", 0);
      if (en >= 1) { watchedep_set(imdb, sn, en, done); total++; }
      if (epEnd >= seasonEnd) break;
    }
    if (seasonEnd >= end) break;
  }
  printf("[watchedep] %s: %d episodes in the map (%d watched)\n",
         imdb, total, watchedep_count(imdb));
  fflush(stdout);
  return total;
}
