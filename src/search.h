// The Search screen, in the shape of the Apple TV app: an on-screen keyboard on
// the left, a grid of posters on the right filtering as you type.
//
// The screen exists because a TV has no physical keyboard: ALL text entry goes
// through the D-pad, which is why the choice of keyboard shape (a grid against
// tvOS's single line) changes the experience more than any visual detail. The
// reasoning behind the choice is at the top of search.c.
#ifndef NV_SEARCH_H
#define NV_SEARCH_H
#include <SDL2/SDL.h>
#include "home.h"

int  search_start(void);
void search_event(const SDL_Event *e);
void search_update(float dt, Uint32 now);
void search_draw(Uint32 now);
// 1 when Back should close the screen. Back only reaches here after exhausting
// what it has to undo INSIDE the search (leaving the results back to the
// keyboard); closing straight from the middle of the results loses the typed
// text without the user having asked for that.
int  search_wants_exit(void);
void search_shutdown(void);

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
