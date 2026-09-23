// "REMOVE FROM CONTINUE WATCHING", from the home's hold menu.
//
// A title in the row can come from three places, and a removal that misses one
// of them is undone by the next build:
//
//   - progress.txt, this device's own resume points     -> dropped at once
//   - the Nuvio account's copy (sync_pull_watch_progress) -> sync_delete_progress
//   - Trakt's /sync/playback                            -> trakt_playback_remove
//
// On top of those it records the removal (cat_cw_dismiss), which is what keeps a
// series from coming straight back as a next-up card out of its watch history,
// and what hides the card even when a remote delete fails. The home's row is
// rebuilt at once; the remote half runs on a thread of its own.
#ifndef NV_CWREMOVE_H
#define NV_CWREMOVE_H

// Starts the removal of the WORK behind catalogue item `index_` (a series goes
// whole, every episode). 1 when it started; 0 when there was nothing to remove or
// a removal is still running.
int cw_remove(int index_);

// 0 none, 1 in flight, 2 every remote delete landed, 3 at least one failed — the
// same numbering as trakt_operation_state, so the menu reads both alike. The
// card is gone from the row in every case except 0.
int cw_remove_state(void);

#endif
