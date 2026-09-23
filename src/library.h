// Library screen: the owner's Saved and Collection sets in Discover's page — a
// tab strip between the two sets, Type and Sort dropdowns, and Discover's grid.
//
// It is the only screen in the app where the content is the user's own SET, not
// an editorial shelf, so it has to stay readable when that set is empty.
#ifndef NV_LIBRARY_H
#define NV_LIBRARY_H
#include <SDL2/SDL.h>
#include "home.h"

int  library_start(void);
void library_event(const SDL_Event *e);
void library_update(float dt, Uint32 now);
void library_draw(Uint32 now);
int  library_wants_exit(void);   // 1 when Back should close the screen
int  library_requested_menu(void);   // LEFT at the left edge: calls up the menu
void library_shutdown(void);

// OK pressed on a poster: consumes the request and returns 1, writing into
// *catalogIndex the item's index IN THE CATALOG (not its position in the grid —
// the grid is filtered, and whoever opens the detail needs the real item).
int  library_requested_open(int *indexCatalog);
// The focused poster and the rectangle it occupies THIS frame, for the detail
// screen's fly-in. 0 until a frame has been drawn with the focus in the grid.
int  library_item_focused(HomeItem *out);

// Account state, in memory. Exposed because the thing that marks a title is the
// detail screen (the "+" button), not the library: without this the "My List"
// tab could only be fed from inside this module, and the detail's "+" would
// have nowhere to write.
int  library_in_list(int indexCatalog);
void library_toggle_list(int indexCatalog);
int  library_bought(int indexCatalog);

#endif
