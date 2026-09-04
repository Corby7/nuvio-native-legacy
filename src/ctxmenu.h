// Poster CONTEXT MENU, opened by HOLDING OK over a home card.
//
// This is the web app's `posterHoldMenu`, and the options are its own, measured
// in bundle 1.0.4 (getPosterHoldMenuOptions): "See details", "Add to/Remove
// from library" and, only on films and series, "Mark as watched/unwatched".
//
// It exists because the two library actions only had a path INSIDE the title
// screen: to mark a film as seen you had to open the detail, wait for the
// network and scroll down to the button. Holding OK over the poster is two
// presses.
#ifndef NV_CTXMENU_H
#define NV_CTXMENU_H
#include <SDL2/SDL.h>

// `index` is the position in the global catalogue.
// The long-press integration lives in home.c: it measures NV_HOLD_MS on KEYUP
// and calls ctx_open only once the threshold is reached. This module does not
// time the key and does not open on KEYDOWN; that way a short press still opens
// the detail, and the modal only takes D-pad focus once it is visible.
void ctx_open(int index_);
int  ctx_is_open(void);
void ctx_event(const SDL_Event *e);
void ctx_update(float dt, Uint32 now);
void ctx_draw(Uint32 now);
// Index of the title whose detail the owner asked for, or -1. Consumed once.
int  ctx_requested_details(void);
#endif
