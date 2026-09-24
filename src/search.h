// The Search screen, in the shape of NuvioWeb's: a wide text field at the top
// and horizontal rows of results under it, one row per addon catalogue.
//
// A TV has no physical keyboard, so ALL text entry goes through something. The
// web app on this same TV hands that job to the SYSTEM — it puts an <input> on
// screen and the LG keyboard comes up over it — and this screen now does the
// same through ime.h. What it draws when the platform has no keyboard to offer,
// and why it still draws anything at all, is at the top of search.c.
#ifndef NV_SEARCH_H
#define NV_SEARCH_H
#include <SDL2/SDL.h>
#include "home.h"

int  search_start(void);
// COMING BACK to the screen within a few minutes: what the viewer left — the
// typed text, the results, the focus and the scroll — stays, and only the
// requests the last visit left pending are cleared. app.c decides which of the
// two a visit is (swapScreen).
void search_resume(void);
void search_event(const SDL_Event *e);
void search_update(float dt, Uint32 now);
void search_draw(Uint32 now);
// 1 when Back should close the screen. Back only reaches here after exhausting
// what it has to undo INSIDE the search — lowering the TV's keyboard, then
// leaving the results or the recent-search chips back to the field. Closing
// straight from the middle of the results loses the typed text without the user
// having asked for that.
int  search_wants_exit(void);
int  search_requested_menu(void);   // LEFT at the left edge: calls up the menu
void search_shutdown(void);

// The compass in the header was pressed: 1 ONCE, and the router should open the
// Discover screen. Consumed like the others.
int  search_requested_discover(void);

// OK pressed on a poster: returns 1 ONCE and writes the index into
// `catalogIndex` for cat_item(). Consumes the request, like
// home_requested_open.
int  search_requested_open(int *indexCatalog);

// The focused poster with the rectangle it occupies on screen THIS frame, in
// the same shape the home delivers — it is what detail_open needs for the card
// to fly from there instead of appearing out of nowhere. 0 if no frame has yet
// been drawn with the focus on the results.
int  search_item_focused(HomeItem *out);

#endif
