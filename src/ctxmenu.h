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

// THE CATALOGUE THE CARD'S ROW CAME FROM. It travels with the card because the
// menu offers to open the WHOLE row as a grid, and a catalogue cannot be deduced
// from an item: the same title sits in several rows at once.
typedef struct { char title[96], base[600], kind[8], catId[96]; } CtxCatalog;

// `index` is the position in the global catalogue.
// The long-press integration lives in home.c: it measures NV_HOLD_MS on KEYUP
// and calls ctx_open only once the threshold is reached. This module does not
// time the key and does not open on KEYDOWN; that way a short press still opens
// the detail, and the modal only takes D-pad focus once it is visible.
void ctx_open(int index_);
// The same, with the row's catalogue, which adds the "browse the whole row"
// option. A NULL or empty `base`/`catId` simply leaves that option out — which is
// what "Continue watching", the collections and the friends' feed get, none of
// them having a catalogue to open.
void ctx_open_row(int index_, const CtxCatalog *row);
// The row the owner asked to see in full, or 0. Consumed once: the router reads
// it, opens the grid and the modal is already closed.
int  ctx_requested_seeall(CtxCatalog *out);
int  ctx_is_open(void);
void ctx_event(const SDL_Event *e);
void ctx_update(float dt, Uint32 now);
void ctx_draw(Uint32 now);
// Index of the title whose detail the owner asked for, or -1. Consumed once.
int  ctx_requested_details(void);
#endif
