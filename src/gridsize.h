// THE POSTER GRID'S SIZE, shared by Library and Discover.
//
// Both screens draw the same grid (NV_DSC_* in layout.h) and it was fixed at six
// posters a row. Six is the web app's number for a desktop window; across a room
// some want bigger posters and fewer of them, and a long library reads faster
// with more. The size is the viewer's to choose, from a button in each screen's
// header, and it is ONE setting: a grid that changed size between the two
// screens that look identical would read as a fault.
//
// Everything that depended on the column count — the card width, the poster
// height, the row step — is derived from it here rather than from macros, so a
// change takes effect on the next frame. Stored on the device, not the account:
// it answers "how far is the sofa from THIS screen".
#ifndef NV_GRIDSIZE_H
#define NV_GRIDSIZE_H
#include "gfx.h"

#define GRID_COLS_MIN 4
#define GRID_COLS_MAX 8

int   grid_cols(void);
float grid_card_w(void);      // NV_DSC_CARD_W at this column count
float grid_card_step(void);   // card + gap
float grid_poster_h(void);    // 2:3
float grid_line_step(void);   // poster + title + row gap

// WARMS the poster of a card that is OFF screen, by how far away it is: the row
// or so below the fold is decoded now, so it is ready the moment a DOWN brings
// it up; the few after that are only downloaded to disk, which costs no texture
// memory. `top` is the card's y with the scroll applied, `width` the card's
// resting width (the decode size) and `step` the grid's row pitch. Call it for
// every card the grid skips drawing.
//
// WHY: both grids asked for a poster only once its card was on screen, and one
// row fits under the header. Every DOWN brought up a row whose art had not even
// been requested — a row of skeletons, then the posters arriving one by one.
void  grid_warm(const char *art, float top, float width, float step);

// The next size, wrapping from the largest count back to the smallest, and
// written to disk at once.
void  grid_cycle(void);

// The header button: a pill with the grid glyph and "N per row", drawn
// right-aligned so its RIGHT edge sits at `right`. `focus` 0..1 fills it white;
// `alpha` fades all of it, for a header that comes in as a whole. Returns its
// rectangle, so the screen can place what sits beside it.
GfxRect grid_button_draw(float right, float y, float focus, float alpha);
#endif
