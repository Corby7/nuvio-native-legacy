// The Magic Remote's pointer, as one more way to put the focus somewhere.
//
// Every screen already has a complete, tested D-pad contract — OK down and up,
// the hold that opens the poster's menu, the release guards. The pointer does
// NOT get a second set of actions. It does two things:
//
//   HOVER MOVES THE FOCUS. Each screen registers, while it draws, the rect of
//   every focusable thing together with a setter that focuses it. Moving the
//   pointer onto a rect calls the setter — the same state the arrows change, so
//   the scroll, the growth and the tooltips follow as they do for the remote.
//
//   A CLICK IS OK. Button down and up become RETURN down and up, PUSHED onto
//   SDL's queue so they reach everything a real key does — ctxmenu's event
//   watch included — and a held click opens the poster's menu like a held OK.
//
// MEASURED ON THE C3 (webOS 11, 2026-09-26), which is what shaped the rules:
// - motion arrives in 1920x1080 window coordinates, the design space itself;
// - a click is mouse button 1 ONLY: no RETURN comes with it, so there is no
//   double OK to guard against;
// - the wheel is SDL_MOUSEWHEEL with y = -1 rolling down, +1 up, in bursts;
// - the pointer showing and hiding arrive as KEYS with sym 0 and webOS
//   scancodes 484 (shown) and 485 (hidden) — there is no window LEAVE. They
//   are swallowed here: on the home any other KEYDOWN ends an OK hold.
#ifndef NV_POINTER_H
#define NV_POINTER_H
#include <SDL2/SDL.h>

// Focuses thing (a, b) of the screen that registered it — a row and a column,
// or an index and 0. It must do what the arrows would have done to get there.
typedef void (*PointerFocus)(int a, int b);

// Called by main.c whenever the surface changes: the letterbox inside the
// drawable, and how many drawable pixels one window point is (2 on a retina Mac,
// 1 on the TV). Mouse coordinates are in window points.
void pointer_set_box(int bx, int by, int bw, int bh, float pixelsPerPoint);

// Called once per frame before drawing: the zones drawn last frame become the
// ones pointed at, and a fresh list starts filling.
void pointer_frame_begin(void);

// Whether the layer about to draw is the one that owns the keys. app.c decides,
// with the same order app_event routes in; zones from any other layer are
// dropped, so pointing at the home through an open title does nothing.
void pointer_accept(int yes);

// Zones are clipped to this rect until pointer_no_clip — a row scrolled under
// the hero's block must not be clickable where it is not visible.
void pointer_clip(float x, float y, float w, float h);
void pointer_no_clip(void);

// Registers a focusable rect in design coordinates (1920x1080). The LAST one
// registered wins where two overlap, which is drawing order.
void pointer_zone(float x, float y, float w, float h, PointerFocus focus, int a, int b);

// A zone that only HOVERS: pointing at it calls `focus`, clicking it sends no
// OK. For areas that mean "away from here" — the content beside an open menu —
// where an OK would land on whatever the screen behind had focused.
void pointer_zone_hover(float x, float y, float w, float h, PointerFocus focus, int a, int b);

// A zone that only CLICKS: pointing at it focuses nothing, clicking it calls
// `click` instead of sending OK. For controls that exist only for the pointer —
// the arrows that page a row — and have no place in the D-pad's focus.
void pointer_zone_click(float x, float y, float w, float h, PointerFocus click, int a, int b);

// Hover focuses like pointer_zone, but a click calls `act` instead of sending OK
// — the player's progress bar: pointing at it lights it, clicking it seeks.
void pointer_zone_act(float x, float y, float w, float h, PointerFocus focus,
                      PointerFocus act, int a, int b);

// Takes every mouse event, and the pointer's show/hide keys. Returns 1 when it
// consumed the event; the keys it stands for are pushed onto SDL's queue.
int  pointer_event(const SDL_Event *e);

// 1 while the pointer is on screen AND over the zone registered with this
// (focus, a, b) — for a control to draw its own hover state. Answered from last
// frame's zones, like every hit test here.
int  pointer_over(PointerFocus focus, int a, int b);

// Where the pointer is, in design coordinates — for a control whose click means a
// position, like the progress bar.
float pointer_x(void);
float pointer_y(void);
// 1 while the left button (the Magic Remote's OK) is held down: a drag.
int  pointer_held(void);
// SDL_GetTicks of the last motion, 0 before any: the player wakes its controls on
// it, as it does on any key.
Uint32 pointer_moved_at(void);

// EDGE SCROLLING: how many pixels a list whose visible band is [top, bottom],
// across [x0, x1], should scroll THIS FRAME because the pointer rests near one
// of its edges — positive down, negative up, 0 when the pointer is elsewhere,
// hidden or dragging. It quickens with how far into the edge band the pointer
// sits, from a quarter of full speed at the band's inner line to full at the edge.
// The caller adds it to its scroll goal and clamps it to its own ends.
float pointer_edge_scroll(float x0, float x1, float top, float bottom, float dt);

// 1 while the pointer is on screen. An arrow key or the hide key ends it.
int  pointer_active(void);

#endif
