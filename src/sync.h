// Synchronisation with the account: this is what makes the native app behave
// like the official one on somebody else's TV.
//
// WHAT IT REPLACES: today the addons and the Trakt token are text files inside
// the package (art/addons.txt, art/trakt.txt), which makes the .ipk
// undistributable — it hands over the credentials of whoever built it. After
// this module, both come from the account of whoever signed in.
//
// WHERE IT WRITES AND WHERE IT DOES NOT — the most important distinction in this
// file:
//
//   PULLS AND PUSHES (the app holds the real information):
//     addons, credentials (Trakt, TMDB, mdblist), playback progress.
//
// MEASURED on the owner's account: sync_pull_provider_credentials returns
// animeskip, debrid:premiumize, debrid:realdebrid, debrid:torbox, introdb,
// mdblist and tmdb — and NO "trakt". The trakt reader stays here because the RPC
// is the same and the row will appear on its own as soon as the web app writes
// it; until then this app's Trakt link still comes from art/trakt.txt, which
// CANNOT go in the package.
//   ONLY PULLS (the app reads, but has no local editing to push):
//     profiles, watched, library, saved, collections, profile settings, home
//     catalogues.
//
// This is NOT laziness, it is safety rule number 2 from section 1.6 of the plan.
// Pushing a surface the app does not edit would mean sending an EMPTY list to
// the server, and an empty list erases what exists on the person's other
// devices. Pushing only what the app actually owns is the only safe way to take
// part in a two-way sync without having every screen.
//
// Every network call happens on a thread of its own. The UI only queries
// state.
#ifndef NV_SYNC_H
#define NV_SYNC_H

typedef enum {
  SYNC_STOPPED = 0,
  SYNC_RUNNING,
  SYNC_READY,
  SYNC_FAILED
} SyncState;

// Fires a full cycle (pulls and, where it makes sense, pushes). Returns
// immediately. Idempotent while a cycle is in progress.
void sync_start(void);

SyncState  sync_state(void);
const char *sync_summary(void);   // uma linha para a tela de ajustes

// Marks a surface dirty: the next cycle pushes it. Call when the user changes
// something locally.
void sync_dirty_progress(void);
void sync_dirty_addons(void);

// The last instant a cycle finished successfully (SDL_GetTicks); 0 if never.
unsigned sync_last_ok(void);

// The interval between automatic cycles. Until now the sync only ran at
// startup, after login and when switching profile — so stopping an episode on
// the phone did not show up on the TV without closing and reopening the app,
// which is the opposite of what the account promises.
//
// 5 minutes, and not 30 seconds: the cycle is ~8 requests (profiles, locks,
// addons, credentials, progress and the read-only ones). The TV stays ON for
// hours on the same screen, so a short interval becomes a constant hammer on the
// backend — and this project already had a quota-overrun episode where the side
// effect (login impossible, anonymous session, syncing the wrong account) looked
// like a bug in the app. The cycle also only runs with the app in use, never
// during playback.
#define SYNC_INTERVAL_MS 300000u

// Call once per frame. Does not block: it only collects the thread's result and
// applies it to the app (addon list, Trakt credential, progress).
void sync_step(unsigned nowMs);

// Fires a cycle if SYNC_INTERVAL_MS has passed since the last successful one.
// Does not run with a cycle in progress, with the brake on, or before the first
// success. 1 when it fired.
int  sync_periodic(unsigned nowMs);

// Erases from the device everything belonging to whoever was signed in. Call it
// TOGETHER with session_exit() — the session alone is not enough.
//
// The defect this fixes: signing out deleted the token and nothing else. The
// addon list stayed in memory (with the debrid keys embedded in the URLs), the
// Trakt token stayed valid and kept WRITING whatever the next person watched
// into the departing person's account, the active profile stayed saved — so the
// next account's first sync would write progress under the previous
// `p_profile_id` — and the previous person's progress.txt was still on disk.
//
// On a living-room TV, "sign out" is the only barrier between two people. It has
// to erase for real.
void sync_forget_user(void);

// Makes the NEXT cycle reapply the settings that came from the account. Call it
// on sign-in and on switching profile.
//
// Why not apply on EVERY cycle: the native app reads the profile's settings but
// does not write them back. Always reapplying would undo, on the next pass,
// anything the person changed on the TV itself — they would touch an option and
// watch it revert on its own. Applying only on the first pass after signing in
// (or switching profile), the account sets the starting point and the local
// change holds for the rest of the session.
void sync_reapply_settings(void);

// Sends a service credential to the ACCOUNT, so the person's other devices
// inherit the link. `credJson` is the finished object (the server stores
// whatever arrives). BLOCKS — call from a thread, or accept the cost of one
// round trip.
//
// It exists because of Trakt: the owner's account had no `trakt` row, so linking
// on the TV did not help the phone. Linking here now WRITES into the account,
// which is what the web app does.
void sync_push_credential(const char *provider, const char *credJson);

void sync_shutdown(void);

#endif
