// Home rows: the order of the Home and which rows show, edited on the TV.
//
// THE LIST IS THE ACCOUNT'S. The order, the hidden rows and the custom names live
// in the account's home_catalog_shared blob — the one the web app's "Reorder home
// catalogs" edits and sync.c already reads. Editing it here writes the same blob
// back (sync_push_home_catalog_settings), so the TV and the web agree. A local-only
// order would lose to the next pull, which is why there is none.
//
// Two rows are NOT in that blob: the Trakt watchlist and Trakt recommendations.
// The web keeps them device-local too (homeCatalogSettingsSyncService re-inserts
// them after every pull), and this does the same: their position and visibility
// live in home-rows-p<profile>.txt and are merged into the account's order
// whenever it is applied. "Continue watching" and "Among friends" are not in the
// list: the first is always the top row, the second has its own switch.
//
// Everything here runs on the main thread except the Trakt getters, which the
// discovery thread calls during a build.
#ifndef NV_HOMEROWS_H
#define NV_HOMEROWS_H

// Builds the editable list from the account's order, the catalogues the addons
// declare and the collections. Called when the Settings section opens.
void homerows_open(void);

int homerows_n(void);                       // rows shown in the list
const char *homerows_title(int i);
const char *homerows_kind(int i);           // "Catalogue", "Collection", "Trakt"
const char *homerows_source(int i);         // the addon's name, or ""
int  homerows_enabled(int i);
// 1 when the row is shown but falls past the Home's row cap, so it will not
// appear. Only catalogue and Trakt rows count against the cap.
int  homerows_over_cap(int i);

void homerows_toggle(int i);
// Moves row `i` one place up (dir < 0) or down; returns its new index.
int  homerows_move(int i, int dir);

// Applies the edited list to the Home, stores it and sends it to the account.
// Does nothing when nothing changed. Called when the section closes.
void homerows_commit(void);

// Whether the opt-in Trakt rows are shown. Any thread.
int  homerows_trakt_watchlist(void);
int  homerows_trakt_recs(void);

// Inserts the Trakt rows into the order discover is being given, at their saved
// positions. sync.c calls it after applying the account's order, before
// disc_prefs_end.
void homerows_merge_trakt(void);

// A counter bumped on every commit. A pull of the account's order that STARTED
// under an older value, or that lands while a push is still in flight, carries
// the order from before the edit and must not be applied.
unsigned homerows_generation(void);
int      homerows_push_in_flight(void);

#endif
