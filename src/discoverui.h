// THE DISCOVER SCREEN: browse a whole catalogue by hand.
//
// The web app's `discoverScreen.js`, reached from the compass button in the
// search header. Three pickers across the top — Type, Catalog, Genre — and the
// chosen catalogue's posters in a grid under them, paging as you reach the
// bottom.
//
// WHY THE FILE IS NOT CALLED discover.c. That name is taken, by the module that
// assembles the CATALOGUE at runtime (`disc_*`), and the two are not the same
// thing at all: that one is the data layer this screen reads FROM. Renaming it
// would touch a dozen includes for no gain, so the screen carries the longer
// name and the `dui_` prefix. If you are looking for where the catalogues come
// from, it is discover.h.
//
// WHAT IT REUSES, because nearly all of it already existed:
//   - disc_seeall_filter / disc_seeall_more / disc_seeall_n — the paged fetch
//     of one catalogue, with a genre, written for the "See all" screen. The
//     Discover grid is the same request with the parameters chosen by picker
//     rather than inherited from a row.
//   - the Library's picker and grid geometry (NV_DSC_* in layout.h, measured
//     off the same stylesheet rules).
//
// ONE DELIBERATE DIVERGENCE, recorded so it is not mistaken for an oversight.
// The web app's Catalog picker lists EVERY non-search catalogue the installed
// addons declare. On the owner's addons that is over six hundred — Xperience
// alone declares 605 — which is a dropdown on a mouse and an ordeal on a D-pad.
// This picker offers the catalogues the app already holds (cat_row), which is
// what the home built, bounded by its row limit, and every entry of which is
// known to answer. An option that returns nothing is worse than an option that
// is not there.
#ifndef NV_DISCOVERUI_H
#define NV_DISCOVERUI_H
#include <SDL2/SDL.h>
#include "home.h"

// Opens on the first catalogue of the first type that has one. Returns 0 when
// there is nothing to browse at all (no catalogues loaded yet), and the caller
// should not switch to the screen.
int  dui_start(void);
void dui_event(const SDL_Event *e);
void dui_update(float dt, Uint32 now);
void dui_draw(Uint32 now);
// 1 when Back should close the screen. Back first closes an open picker, then
// climbs from the grid to the pickers, and only then leaves.
int  dui_wants_exit(void);
int  dui_requested_menu(void);   // LEFT at the left edge: calls up the menu
void dui_shutdown(void);

// OK pressed on a poster: 1 ONCE, with the index into the global catalogue, the
// same contract as home_requested_open and search_requested_open.
int  dui_requested_open(int *indexCatalog);

// The focused poster and the rectangle it occupies THIS frame, for the detail
// screen's fly-in. 0 until a frame has been drawn with the focus in the grid.
int  dui_item_focused(HomeItem *out);

#endif
