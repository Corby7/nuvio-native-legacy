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
// rebuilt once the remote half, on a thread of its own, has answered.
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

// Call every frame from the UI thread. Once the remote deletes have answered it
// does the local half — the dismissal, progress.txt, the rebuild — and the card
// leaves the row.
void cw_remove_step(void);

// The work id (tt…, no episode suffix) whose removal is still in flight, or NULL.
// Its card stays in the row meanwhile and the home circles its ring — until the
// home reports the card gone (cw_remove_gone), not merely the deletes done.
const char *cw_remove_busy(void);
// The home found no card for the busy work in "Continue watching". Ignored
// while the local half is still owed: the card is SUPPOSED to be there then.
void cw_remove_gone(void);

#endif
