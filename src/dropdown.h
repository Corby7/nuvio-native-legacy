#ifndef NV_DROPDOWN_H
#define NV_DROPDOWN_H
#include "gfx.h"

// THE APP'S DROPDOWN: an anchor pill with a small grey label over a large white
// value and a chevron, and the list it opens underneath.
//
// It was the Discover screen's, drawn inline there. The collection grid then
// needed the same control, and the choice was to copy sixty lines of drawing or
// to move them. This is the move — one implementation, so the two screens cannot
// drift, and any screen that needs a picker from here on asks for this one.
// The geometry is NV_DD_* in layout.h.
//
// THE WIDGET DRAWS AND NOTHING ELSE. Which picker is open, where the focus sits
// inside its list and what OK does stay with the SCREEN, because those genuinely
// differ: Discover has three pickers feeding one request, the collection grid has
// one per media type with only one of them in effect at a time.

// How a screen names option `index` of the list being drawn. `ctx` is whatever
// the caller passed; the widget only hands it back.
typedef const char *(*DdLabel)(void *ctx, int index);

// The anchor.
//   focus  0..1 focus spring — the fill lifts and an inset ring comes in.
//   active 0 for a picker whose value is NOT what the screen is showing, which
//          dims the value to the label's grey. The collection grid's Series
//          picker while its Movies picker is in effect; Discover passes 1.
//   alpha  multiplies everything, for a header that fades in as a whole.
void dd_pill(GfxRect r, const char *label, const char *value,
             float focus, int active, float alpha);

// THE SELECT: the title page's season picker as an anchor — one line, the value
// in 30/500 white with `tail` after a dot in grey (NULL or "" for none), and the
// chevron. `focus` 0..1 lifts the ground and brings the inset ring in, which is
// also how it should be drawn while its menu is down. dd_menu opens under it.
void dd_select(GfxRect r, const char *value, const char *tail, float focus,
               float alpha);
// The width a select needs to hold the WIDEST of `n` values without the anchor
// resizing as the choice changes — the season picker's rule.
float dd_select_width(int n, DdLabel label, void *ctx);

// The open list: hung NV_DD_MENU_GAP under `anchor`, matching its width, and
// scrolling once the options run past NV_DD_OPT_VIS rows.
//
// DRAW IT LAST of everything on the screen. It is a question standing in front
// of the page — it covers the grid and the pickers beside it, and it carries a
// shadow that has to fall on them.
void dd_menu(GfxRect anchor, int n, int focus, DdLabel label, void *ctx,
             float alpha);
#endif
