// HOLD OK ON A GRID CARD: the poster menu, outside the home.
//
// The home measures its own long press (home.c). The poster grids — Library,
// Discover, and "See all", which is also what a streaming service's folder opens
// into — acted on OK's KEYDOWN, so a hold there was just a slow tap and the menu
// was unreachable. This is the gesture on its own, for any screen with a
// focused card:
//
//   - OK DOWN over a card only ARMS it. Nothing opens yet.
//   - Released before NV_HOLD_MS: a tap — the screen does what OK always did.
//   - Still held at NV_HOLD_MS: hold_fired() says so ONCE, the screen opens
//     the menu, and the release that follows is swallowed.
//   - An arrow or Back while held cancels it: the release does nothing.
//
// A release with no press seen here is not a tap either. The sidebar and the
// open lists act on the KEYDOWN, and the KEYUP of that same press would
// otherwise reach the grid behind them and open whatever card has focus.
//
// THE FEEDBACK IS THE HOME'S, value for value (NV_HOLD_* in layout.h): the
// focus ring steps back and a white sweep refills it, eased; the card presses
// in about its centre and, when the menu opens, springs back past its size on an
// under-damped spring while a glow widens and fades behind it. Let go early and
// the sweep drains back. The first version here only shrank the card linearly
// from its top edge and snapped back when the menu opened, and next to the
// home's it looked broken.
#ifndef NV_HOLD_H
#define NV_HOLD_H
#include <SDL2/SDL.h>
#include "gfx.h"

typedef struct {
  int    pressing;         // OK is down and armed over a card
  int    fired;            // the menu opened; the release is swallowed
  Uint32 since;
  // The feedback, as home.c names it: how far round the sweep is, how far the
  // ring has stepped back, the confirming glow, and the press spring.
  float  shown, dim, pulse, scale, scaleV;
} Hold;

// Feed every event. `armable`: whether an OK press NOW would be over a card
// (the grid has focus and no list is open); it only matters on the KEYDOWN.
// Returns 1 when the event was this gesture's and the screen must not handle
// it; `*tap` comes back 1 on the release of a short press.
int   hold_event(Hold *h, const SDL_Event *e, int armable, int *tap);
// 1 exactly once, on the frame the press reaches NV_HOLD_MS. Starts the glow.
int   hold_fired(Hold *h, Uint32 now);
// Steps the feedback. Call every frame, from the screen's update.
void  hold_animate(Hold *h, float dt, Uint32 now);

// THE DRAWING, all for the focused card only.
// The press: `card` scaled about its centre (the focus growth pins the top; a
// push does not).
GfxRect hold_card(const Hold *h, GfxRect card);
// The confirming glow. BEFORE the card, so it shows round it.
void  hold_glow(const Hold *h, GfxRect card, float radiusPx);
// The focus ring with the sweep, in place of the screen's own inset ring:
// `thickness` and `radius` normalised to the card's height, as GFX_RING_INSET
// takes them; `f` the focus amount the ring was drawn at.
void  hold_ring(const Hold *h, GfxRect card, float thickness, float radius,
                float r, float g, float b, float f);
// While the menu is open: keeps its scrim's hole on the card as it springs, and
// feathered as far as the glow reaches.
void  hold_track(const Hold *h, GfxRect card, float radiusPx);

#endif
