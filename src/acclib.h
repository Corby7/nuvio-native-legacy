// THE ACCOUNT'S OWN LIBRARY AND WATCHED LIST, for whoever has not linked Trakt.
//
// With Trakt linked, the Library is Trakt's watchlist and collection and the
// watched marks are Trakt's history (trakt.c) — the web app does the same when
// Trakt is the library source. WITHOUT Trakt the Library screen was simply
// empty and "Add to library" failed: the app pulled the account's library and
// watched items and only counted them. This module reads them, shows them, and
// writes the TV's own changes back, so a title saved on the phone appears here
// and one saved here appears on the phone.
//
// THE SERVER'S CONTRACT, read in NuvioMedia/self-host (baseline migration) and
// not guessed from the clients, because the two RPCs are NOT alike:
//
//   sync_push_library       REPLACES the list. It upserts every item sent and
//                           DELETES every row of the profile that was not sent.
//                           A partial push erases the rest of the person's
//                           library on every device.
//   sync_push_watched_items UPSERTS only. A batch of one is safe.
//   sync_delete_watched_items deletes by (content_id, season, episode).
//
// So the library is never pushed from what the TV happens to hold. Every push
// is built on the thread, in the same cycle, from a COMPLETE pull (every page)
// with the TV's pending edits replayed on top — and the rows that came from the
// account go back byte for byte, so fields this app does not know (poster
// shape, genres, the addon base url) survive the round trip untouched.
//
// PENDING EDITS live on disk (account-p<N>-library-ops.txt and
// -watched-ops.txt) until the server accepts them: an edit made just before the
// TV is switched off still reaches the account on the next start.
#ifndef NV_ACCLIB_H
#define NV_ACCLIB_H
#include "catalog.h"

// 1 when the account (and not Trakt) is where the library and watched marks
// live: signed in, Trakt not linked.
int  acclib_active(void);

// The person's gesture, on the main thread. The local effect is the caller's
// (cat_set_in_list, cat_history_set_id, watchedep_set); this records the edit
// and asks the sync for a short cycle. `imdb` may carry an episode suffix.
void acclib_list(const char *imdb, const char *kind, int add);
void acclib_watched(const char *imdb, const char *kind, int season, int episode,
                    int watched);

// Is the title in the account library as last pulled, with the pending edits
// applied? Any thread.
int  acclib_has(const char *imdb);

// The library as catalogue items (inList set, `added` from added_at), for
// discovery. Any thread. Returns how many went into `out`.
int  acclib_items(CatItem *out, int max);

// The sync thread's share: pull both lists, push what is pending. Blocking.
void acclib_sync(void);
// The main thread's share: apply what acclib_sync pulled. Returns 1 when the
// set of library titles changed, which is when the catalogue needs a rebuild to
// carry the new ones.
int  acclib_step(void);
// Reads the last pulled library of the active profile from disk. Main thread,
// before the first build.
void acclib_restore(void);

// "N in library · M watched", for the settings summary. -1 when never pulled.
int  acclib_count_library(void);
int  acclib_count_watched(void);

// Sign-out: the lists, the pending edits and the files, for every profile.
void acclib_forget(void);

#endif
