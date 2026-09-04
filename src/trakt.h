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
#include "profile.h"
#include <stddef.h>

int  trakt_load(const char *dirArt);   // 1 quando ha credencial

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
int  trakt_resume(CatItem *output, int max);

// The recent activity of the owner's FRIENDS. It uses Trakt's official social
// feed (/users/me/friends/activities), keeping the normal title and art in the
// CatItem and the social data in the presentation fields: `country` = the
// friend's name, `provName` = the action and `directing` = the episode's
// context. BLOCKS.
int  trakt_social(CatItem *output, int max);

// The monthly snapshot used by the Profile and Stats screen. It makes its calls
// on the app's worker thread and must never run in the draw loop.
int  trakt_profile(ProfileData *output);

// Reports where the owner stopped. `imdb` may carry an episode ("tt123:4:9").
// Until now the app only READ Trakt; without this, watching here did not move
// the "continue watching" on their other devices. Does not block: it goes out on
// a thread.
void trakt_mark(const char *imdb, double posSeg, double durationSeg);

// Watchlist ("Minha Lista") e colecao ("Comprados") do dono. `qual` e
// "watchlist" ou "collection". BLOQUEIA — chamar do fio de descoberta.
int  trakt_list(const char *which, CatItem *output, int max);

// Adds the title to, or removes it from, the owner's WATCHLIST. Does not block.
// The read state already arrives in CatItem.inList, filled in by trakt_list
// during discovery — what was missing was writing back: the "+" button only
// touched a local array and the list on the other devices never knew.
void trakt_watchlist(const char *imdb, int add);

// Marks (or unmarks) the title as WATCHED in /sync/history.
//
// NOT to be confused with trakt_mark, which is /scrobble/pause ("I stopped
// here") and serves the player. The eye button means "I have seen this one", and
// it used trakt_mark with a duration of 1.0 — a value that function's own guard
// rejects, so nothing ever reached Trakt.
void trakt_watched(const char *imdb, int mark);

#endif
