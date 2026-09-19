// "SEE ALL" SCREEN: a whole catalogue in a grid, opened from the card at the
// end of a home row.
//
// It exists because the row shows 12 items and the catalogue has hundreds — on
// the web this is `catalogSeeAllScreen`, opened by the `openCatalogSeeAll`
// card. Without it the rest of each catalogue was unreachable: there was NO
// path in the app to see item 13 of a row.
//
// The grid is 5 columns at 1920, measured from the web (.seeall-card is 248px
// wide, and the body reserves 368px for the side panel). The detail panel the
// web has on the right is NOT in this version — it repeats what the title
// screen already shows, and the grid alone solves what was missing.
#ifndef NV_SEEALL_H
#define NV_SEEALL_H
#include <SDL2/SDL.h>
#include "collections.h"
// Opens a COLLECTION. The view grows out of the collection card that was focused
// when it was pressed and carries the folder's wordmark up into this screen's
// header; both rects are read from the home at this moment, before it stops being
// drawn. A folder reached with no card on screen falls back to a plain fade.
void seeall_collection(const ColFolder *folder);
// 1 once the window has reached every edge and the ground behind it is opaque: the
// screen underneath is invisible and need not be drawn. NOT the same question as
// seeall_is_open, which is true from the first frame of the opening.
int  seeall_covers_screen(void);
// 1 while the folder's MARK is in the air between the home's hero and this header
// — the wordmark, or the title as type for a folder that has no wordmark. The home
// reads it to stand its own copy down for the duration. False through the close,
// which is a plain fade with nothing travelling.
int  seeall_owns_mark(void);

// Opens with the catalogue behind a home row.
void seeall_open(const char *base, const char *kind, const char *catId,
                   const char *title);
int  seeall_is_open(void);
void seeall_event(const SDL_Event *e);
void seeall_update(float dt, Uint32 now);
void seeall_draw(Uint32 now);
// Index in the global catalogue of the title the owner opened, or -1. Consumed
// once: the router calls, opens the detail and the screen closes.
int  seeall_requested_open(void);
#endif
