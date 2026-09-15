// WHICH EPISODES HAVE BEEN WATCHED — the one thing the app never knew.
//
// catalog.c's history is per TITLE: id_base() truncates the id at the ':' on
// purpose, so "tt123:2:8" and "tt123" are the same thing to it. That is enough
// for "mark the series as watched" and enough for nothing per episode.
//
// WHY A MODULE OF ITS OWN rather than a field on CatEp: the episode list is
// loaded ON DEMAND and thrown away when the title changes (cat_set_all zeroes
// nEps), while what has been watched holds for the whole session and arrives
// from the network before any list exists. Keeping it on the episode would make
// the data be born and die with the screen that shows it.
//
// TWO SOURCES, and both write here:
//   - the Nuvio account, through the watched items the sync already pulls, which
//     carries `season` and `episode` on every row — the episode was being read
//     and thrown away;
//   - Trakt, through /shows/<id>/progress/watched, which returns the COMPLETE
//     map of seasons and episodes. /sync/history does not serve: it is paginated
//     over recent plays and answers "what was watched lately", not "what is
//     watched".
//
// THE KEY IS THE TITLE'S ID, always truncated at the ':' — a caller may pass
// "tt123" or "tt123:2:8", and the second is the form CatItem.imdb carries on an
// item of "Continue watching".
#ifndef NV_WATCHEDEP_H
#define NV_WATCHEDEP_H

// -1 not known (never asked, or the title is not in the map)
//  0 known NOT to have been watched
//  1 watched
int  watchedep_state(const char *imdb, int season, int episode);

// Marks one episode. `watched` is 0 or 1. Called both by the network reader and
// by the person's action on the TV — the local effect is immediate, and talking
// to the server is the caller's job.
void watchedep_set(const char *imdb, int season, int episode, int watched);

// How many episodes of a title are marked watched. It serves the list's caption
// ("12 of 20") without making the screen walk the map.
int  watchedep_count(const char *imdb);

// Does the title HAVE a map? It separates "a series with no episode watched"
// from "we never knew anything about this series", which is the difference
// between drawing a zero and drawing nothing.
int  watchedep_known(const char *imdb);

// One episode, for the batches. The three gestures the screen offers — this
// episode, up to here, the whole season — are the SAME batch at different sizes,
// which is why there is one function instead of three.
typedef struct { short season, episode; } WatchedPair;

// Marks a batch at once, LOCALLY. Talking to the server is the caller's job: the
// local effect has to be immediate (the list redraws in the same frame) and the
// network takes seconds. Returns how many actually changed state.
int  watchedep_mark_batch(const char *imdb, const WatchedPair *pairs, int n,
                          int watched);

// Builds the "up to here" batch: every episode of THIS series in the map at a
// position less than or equal to (season, episode), in order. Returns how many
// fitted in `out`; `max` limits it. It comes from the MAP and not the catalogue
// because the map is what knows which episodes exist as far as Trakt is
// concerned — an episode the catalogue has and Trakt does not cannot be marked
// there.
//
// `out` NULL IS COUNT MODE, and `max` is ignored in it: the screen needs the
// NUMBER before it can decide whether to offer the action ("Up to here (7
// episodes)").
int  watchedep_up_to_here(const char *imdb, int season, int episode,
                          WatchedPair *out, int max);
// The same for a whole season.
int  watchedep_season(const char *imdb, int season, WatchedPair *out, int max);

// Reads the body of Trakt's /shows/<id>/progress/watched, which enumerates the
// WHOLE series with `completed` per episode — so this reader writes 0 as well as
// 1. Returns how many episodes went in, or -1 on an invalid body.
int  watchedep_read_progress(const char *imdb, const char *json);

int  watchedep_n(void);         // episodes in the map, for the log and the tests
void watchedep_forget(void);    // sign-out

#endif
