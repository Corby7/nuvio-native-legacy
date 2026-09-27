#include "pointer.h"
#include <string.h>

// webOS keys that carry the pointer's visibility, measured on the C3 (see
// pointer.h). sym is 0 for both.
#define NV_SCANCODE_POINTER_SHOWN  484
#define NV_SCANCODE_POINTER_HIDDEN 485

#define NV_ZONES_MAX 512
// Edge scrolling: the band inside a list's top and bottom edges that scrolls it,
// and the speed at the very edge, in design pixels per second — about five
// poster rows a second. The first measurements (120px, and 1100px/s through a
// squared ramp, with a top band half as deep) were too slow and the top band too
// small to find from the sofa.
#define NV_POINTER_EDGE_BAND  160.0f
#define NV_POINTER_EDGE_SPEED 2600.0f
// The speed on entering the band, as a share of the full speed: the ramp is
// linear from here, so the band is never a dead strip.
#define NV_POINTER_EDGE_FLOOR 0.25f
// Stamped on the keys this module pushes, so that the wheel's arrows coming back
// through pointer_event are not mistaken for the remote's and end pointer mode.
#define NV_POINTER_SYNTHETIC 0x9017E4u

// kind: ZONE_FOCUS (hover focuses, click is OK), ZONE_HOVER (hover only),
// ZONE_CLICK (click calls `focus` as an action, hover does nothing) or ZONE_ACT
// (hover focuses, click calls `act`).
enum { ZONE_FOCUS, ZONE_HOVER, ZONE_CLICK, ZONE_ACT };
typedef struct { float x, y, w, h; PointerFocus focus, act; int a, b, kind; } Zone;

// Two lists: the one the current frame is filling, and the last complete one,
// which is what the pointer is really over. Events are handled BEFORE the frame
// draws, so hit-testing the list being filled would see half a screen.
static Zone building[NV_ZONES_MAX], ready[NV_ZONES_MAX];
static int nBuilding, nReady;
static int accepting = 1;
static int clipOn;
static float clipX, clipY, clipW, clipH;

static int boxX, boxY, boxW = 1920, boxH = 1080;
static float perPoint = 1.0f;

static int active;
static float px, py;
// The zone the pointer last focused, so that jitter inside one card does not
// call its setter every frame — and, more to the point, so a still pointer never
// refocuses anything (see pointer_event).
static int hovered = -1;
static PointerFocus hoveredFocus;
static int hoveredA, hoveredB;
// Whether the RETURN down went out, so its up follows it and nothing else.
static int okDown;
static int held;
static Uint32 movedAt;

void pointer_set_box(int bx, int by, int bw, int bh, float pixelsPerPoint) {
  boxX = bx; boxY = by;
  boxW = bw > 0 ? bw : 1; boxH = bh > 0 ? bh : 1;
  perPoint = pixelsPerPoint > 0 ? pixelsPerPoint : 1.0f;
}

void pointer_frame_begin(void) {
  memcpy(ready, building, sizeof(Zone) * nBuilding);
  nReady = nBuilding;
  nBuilding = 0;
  accepting = 1;
  clipOn = 0;
}

void pointer_accept(int yes) { accepting = yes; }

void pointer_clip(float x, float y, float w, float h) {
  clipOn = 1; clipX = x; clipY = y; clipW = w; clipH = h;
}

void pointer_no_clip(void) { clipOn = 0; }

static void addZone(float x, float y, float w, float h, PointerFocus focus,
                    PointerFocus act, int a, int b, int kind) {
  if (!accepting || !focus || nBuilding >= NV_ZONES_MAX) return;
  if (clipOn) {
    float x2 = x + w, y2 = y + h;
    if (x < clipX) x = clipX;
    if (y < clipY) y = clipY;
    if (x2 > clipX + clipW) x2 = clipX + clipW;
    if (y2 > clipY + clipH) y2 = clipY + clipH;
    w = x2 - x; h = y2 - y;
  }
  if (w <= 0 || h <= 0) return;
  building[nBuilding++] = (Zone){ x, y, w, h, focus, act, a, b, kind };
}

void pointer_zone(float x, float y, float w, float h, PointerFocus focus, int a, int b) {
  addZone(x, y, w, h, focus, NULL, a, b, ZONE_FOCUS);
}

void pointer_zone_hover(float x, float y, float w, float h, PointerFocus focus, int a, int b) {
  addZone(x, y, w, h, focus, NULL, a, b, ZONE_HOVER);
}

void pointer_zone_click(float x, float y, float w, float h, PointerFocus click, int a, int b) {
  addZone(x, y, w, h, click, NULL, a, b, ZONE_CLICK);
}

void pointer_zone_act(float x, float y, float w, float h, PointerFocus focus,
                      PointerFocus act, int a, int b) {
  if (act) addZone(x, y, w, h, focus, act, a, b, ZONE_ACT);
}

// From window points to design space, through the letterbox.
static void toDesign(int wx, int wy) {
  px = ((float)wx * perPoint - boxX) * 1920.0f / boxW;
  py = ((float)wy * perPoint - boxY) * 1080.0f / boxH;
}

static int hit(void) {
  for (int i = nReady - 1; i >= 0; i--) {
    const Zone *z = &ready[i];
    if (px >= z->x && px < z->x + z->w && py >= z->y && py < z->y + z->h) return i;
  }
  return -1;
}

static int sameAsHovered(int i) {
  return hovered >= 0 && ready[i].focus == hoveredFocus &&
         ready[i].a == hoveredA && ready[i].b == hoveredB;
}

// Remembers the zone as the one under the pointer and, unless it is a click-only
// control, focuses it. A click zone is still REMEMBERED, so moving back from it
// onto the card it was covering focuses that card again.
static void focusZone(int i) {
  hovered = i;
  hoveredFocus = ready[i].focus; hoveredA = ready[i].a; hoveredB = ready[i].b;
  if (ready[i].kind != ZONE_CLICK) ready[i].focus(ready[i].a, ready[i].b);
}

static void forgetHover(void) { hovered = -1; hoveredFocus = NULL; }

static void sendKey(Uint32 type, SDL_Keycode k) {
  SDL_Event ev; SDL_zero(ev);
  ev.type = type;
  ev.key.windowID = NV_POINTER_SYNTHETIC;
  ev.key.keysym.sym = k;
  ev.key.state = type == SDL_KEYDOWN ? SDL_PRESSED : SDL_RELEASED;
  SDL_PushEvent(&ev);
}

int pointer_event(const SDL_Event *e) {
  switch (e->type) {
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
      int sc = e->key.keysym.scancode;
      if (sc == NV_SCANCODE_POINTER_SHOWN || sc == NV_SCANCODE_POINTER_HIDDEN) {
        if (e->type == SDL_KEYDOWN) {
          active = sc == NV_SCANCODE_POINTER_SHOWN;
          forgetHover();
        }
        return 1;
      }
      // AN ARROW HANDS THE FOCUS BACK TO THE REMOTE. The hover is forgotten, so
      // pointing at the same card again afterwards focuses it again.
      if (e->type == SDL_KEYDOWN && e->key.windowID != NV_POINTER_SYNTHETIC) {
        SDL_Keycode k = e->key.keysym.sym;
        if (k == SDLK_UP || k == SDLK_DOWN || k == SDLK_LEFT || k == SDLK_RIGHT) {
          active = 0;
          forgetHover();
        }
      }
      return 0;
    }

    case SDL_MOUSEMOTION: {
      active = 1;
      movedAt = SDL_GetTicks();
      if (!movedAt) movedAt = 1;
      toDesign(e->motion.x, e->motion.y);
      // ONLY MOTION MOVES THE FOCUS. A row scrolls when its edge card takes the
      // focus, and a new card slides under a pointer that has not moved; if that
      // refocused, the row would run away on its own. So the test is here, on
      // the event, and never on a frame.
      // NOT WHILE THE BUTTON IS HELD. A held click is either the long press that
      // opens a poster's menu — moving onto the next card would cancel it on the
      // wrong one — or a drag along the player's bar, which owns the pointer
      // until it is let go.
      int i = held ? -1 : hit();
      if (i >= 0 && !sameAsHovered(i)) focusZone(i);
      return 1;
    }

    case SDL_MOUSEBUTTONDOWN: {
      if (e->button.button != SDL_BUTTON_LEFT) return 1;
      active = 1;
      held = 1;
      toDesign(e->button.x, e->button.y);
      // A CLICK ON NOTHING IS NOT AN OK. Without this, a click on the empty hero
      // would open whatever card the focus happened to be on.
      int i = hit();
      if (i < 0) return 1;
      // Focus first, even with no motion in between: the click acts on what is
      // under the pointer, not on where the focus was left.
      if (ready[i].kind == ZONE_CLICK) {
        hovered = i; hoveredFocus = ready[i].focus;
        hoveredA = ready[i].a; hoveredB = ready[i].b;
        ready[i].focus(ready[i].a, ready[i].b);
        return 1;
      }
      if (!sameAsHovered(i)) focusZone(i);
      if (ready[i].kind == ZONE_HOVER) return 1;
      if (ready[i].kind == ZONE_ACT) { ready[i].act(ready[i].a, ready[i].b); return 1; }
      okDown = 1;
      sendKey(SDL_KEYDOWN, SDLK_RETURN);
      return 1;
    }

    case SDL_MOUSEBUTTONUP:
      if (e->button.button != SDL_BUTTON_LEFT) return 1;
      held = 0;
      if (okDown) { okDown = 0; sendKey(SDL_KEYUP, SDLK_RETURN); }
      return 1;

    case SDL_MOUSEWHEEL: {
      int y = e->wheel.y;
      if (e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED) y = -y;
      if (!y) return 1;
      // One arrow per notch. The hover is forgotten so that, once the page has
      // moved, the next motion refocuses whatever is under the pointer.
      SDL_Keycode k = y < 0 ? SDLK_DOWN : SDLK_UP;
      int n = y < 0 ? -y : y;
      for (int j = 0; j < n; j++) {
        sendKey(SDL_KEYDOWN, k);
        sendKey(SDL_KEYUP, k);
      }
      forgetHover();
      return 1;
    }
  }
  return 0;
}

float pointer_edge_scroll(float x0, float x1, float top, float bottom, float dt) {
  float depth = 0.0f;
  if (!active || held || px < x0 || px > x1) return 0.0f;
  if (py > bottom - NV_POINTER_EDGE_BAND && py <= bottom + NV_POINTER_EDGE_BAND)
    depth = (py - (bottom - NV_POINTER_EDGE_BAND)) / NV_POINTER_EDGE_BAND;
  // The TOP band starts exactly at the edge and runs down into the list: above
  // it sit the screen's own controls — tabs, pickers — which must never scroll it.
  else if (py >= top && py < top + NV_POINTER_EDGE_BAND)
    depth = -((top + NV_POINTER_EDGE_BAND) - py) / NV_POINTER_EDGE_BAND;
  if (depth > 1.0f) depth = 1.0f;
  if (depth < -1.0f) depth = -1.0f;
  if (depth == 0.0f) return 0.0f;
  { float m = depth < 0 ? -depth : depth;
    m = NV_POINTER_EDGE_FLOOR + (1.0f - NV_POINTER_EDGE_FLOOR) * m;
    return (depth < 0 ? -m : m) * NV_POINTER_EDGE_SPEED * dt; }
}

int pointer_active(void) { return active; }
float pointer_x(void) { return px; }
float pointer_y(void) { return py; }
int pointer_held(void) { return held; }
Uint32 pointer_moved_at(void) { return movedAt; }

int pointer_over(PointerFocus focus, int a, int b) {
  if (!active) return 0;
  int i = hit();
  return i >= 0 && ready[i].focus == focus && ready[i].a == a && ready[i].b == b;
}
