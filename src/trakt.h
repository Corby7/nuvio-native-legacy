// Continue Watching, from Trakt.
//
// The history used to be a snapshot exported from the web app
// (localStorage's watchProgressItems) and froze at the moment of export. The
// owner already uses Trakt as their progress source
// (traktSettings.watchProgressSource = "trakt"), so asking it directly is what
// keeps the row alive — and what makes the native app agree with the web app
// without the two writing to each other.
//
// THE CREDENTIALS live in art/trakt.txt ("token<TAB>clientId"), the owner's
// file: treat it as a secret, do not commit it. Without the file the module
// simply does nothing and the row falls back to what came in the package.
#ifndef NV_TRAKT_H
#define NV_TRAKT_H
#include "catalog.h"
#include "watchedep.h"
#include <stddef.h>

int  trakt_load(const char *dirArt);   // 1 when a credential is present

// Builds the three headers EVERY Trakt request requires (the token, the API
// version and the application key) into `header`, which needs 4 slots — the last
// receives NULL. Returns 0 when no credential is loaded.
//
// It exists because this same block was copied into every function in trakt.c,
// and extras.c would have been the fourth copy. The `auth` and `key` buffers
// belong to the caller: the headers point into them and they have to live until
// the request finishes.
int  trakt_headers(const char **header, char *auth, size_t nAuth,
                      char *key, size_t nKey);
int  trakt_active(void);

// 1 once a Trakt call has come back 401 in this session — the credential is
// loaded but the server refuses it, which is what an expired access token looks
// like from here. Cleared by trakt_set, i.e. by a renewal or a fresh pairing.
// traktauth_step is what acts on it.
int  trakt_refused(void);

// The credential from the ACCOUNT, in place of the file. The token comes from
// sync_pull_provider_credentials (provider "trakt"); the clientId belongs to the
// APPLICATION, not the person, and is compiled in (-DNV_TRAKT_CLIENT_ID,
// generated from local.properties). Until this existed, the native app's Trakt
// link was the package owner's — for everyone who installed it.
int  trakt_set(const char *token, const char *clientId);

// Forgets the credential. Called on SIGN-OUT: a Trakt token that survives the
// sign-out keeps WRITING (trakt_mark) into the departing person's account,
// with whatever the next person watches.
void trakt_forget(void);

// Fills up to `max` "continue watching" items, with the art already resolved.
// BLOCKS — call from the discovery thread. Returns how many it filled.
//
// Only what is genuinely IN PROGRESS comes back: /sync/playback keeps every
// resume point any Trakt client ever recorded, so the 1%-to-90% window is
// applied here, and each item is stamped with the `paused_at` instant.
int  trakt_resume(CatItem *output, int max);

// Deletes every Trakt resume point (/sync/playback) of the WORK `imdb` names —
// all of a series' episodes. This is NOT the watch history: nothing is unmarked,
// the title just stops being "in progress". BLOCKS; 1 when every delete landed
// (or Trakt is not connected, so there was nothing to delete), 0 otherwise.
int  trakt_playback_remove(const char *imdb);

// Resolves art, synopsis and duration for `n` items that carry only an id and a
// kind, in parallel, and returns how many survived — Cinemeta does not know
// everything, and the ones it does not know are compacted out.
//
// The local "Continue watching" list is the second caller: it is built from
// progress.txt, which stores a position and nothing a screen can draw. NOT
// reentrant (the task queue is file-static); callers serialise on discover.c's
// lock.
int  trakt_decorate_batch(CatItem *output, int n);

// The words only — synopsis, "year · runtime", score — for items whose art is
// already right (the Trakt watchlist and recommendations rows). Keeps every item,
// known to Cinemeta or not. BLOCKS; safe beside trakt_decorate_batch.
int  trakt_describe_batch(CatItem *output, int n);

// Reports where the owner stopped. `imdb` may carry an episode ("tt123:4:9").
// Until now the app only READ Trakt; without this, watching here did not move
// the "continue watching" on their other devices. Does not block: it goes out on
// a thread.
void trakt_mark(const char *imdb, double posSeg, double durationSeg);
// "Watching now" (/scrobble/start) and "paused here" (/scrobble/pause), sent by
// the player as playback starts, pauses and resumes. Same id form and the same
// non-blocking queue as trakt_mark, which is the closing message of the three.
void trakt_scrobble_start(const char *imdb, double posSeg, double durationSeg);
void trakt_scrobble_pause(const char *imdb, double posSeg, double durationSeg);

// The owner's watchlist ("My List") and collection ("Purchased"). `which` is
// "watchlist" or "collection". BLOCKS — call from the discovery thread.
int  trakt_list(const char *which, CatItem *output, int max);

// The owner's personalized recommendations (/recommendations), movies and shows
// taken in turn, up to `max` in all. Titles already on the watchlist or in the
// collection are left out by Trakt. BLOCKS — call from the discovery thread.
int  trakt_recommendations(CatItem *output, int max);

// Adds the title to, or removes it from, the owner's WATCHLIST. Does not block.
// The read state already arrives in CatItem.inList, filled in by trakt_list
// during discovery — what was missing was writing back: the "+" button only
// touched a local array and the list on the other devices never knew.
void trakt_watchlist(const char *imdb, int add);

// Marks or unmarks a BATCH OF EPISODES in /sync/history — one POST for the whole
// batch, which is what makes "up to here" and "the whole season" a single
// request instead of one per episode.
//
// SYNCHRONOUS and it can take the full 20 s timeout: the caller has already
// applied the local effect (watchedep_mark_batch), so a caller on the draw
// thread must hand this to a thread of its own or it freezes the TV.
int trakt_mark_episodes(const char *imdb, const WatchedPair *pairs, int count,
                        int watched);

// Marks (or unmarks) the title as WATCHED in /sync/history.
//
// NOT to be confused with trakt_mark, which is /scrobble/pause ("I stopped
// here") and serves the player. The eye button means "I have seen this one", and
// it used trakt_mark with a duration of 1.0 — a value that function's own guard
// rejects, so nothing ever reached Trakt.
void trakt_watched(const char *imdb, int mark);

#endif
