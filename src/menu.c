// The fixed side rail of Nuvio 1.0.1 legacy, with an expandable overlay for
// D-pad navigation.
//
// Two independent animations, and the separation is what gives the right
// movement:
//   - `slides` takes the bar off the left edge (position);
//   - `expands` swaps the width from "icon only" to "icon + label".
// On tvOS the collapsed bar shows only the icons and widens only when it takes
// focus. Here it is born off screen, so the two happen almost together — but
// with springs of different stiffness, so that the width LAGS behind the entry.
// It is that lag that produces the reading "it came in and then it opened"; with
// a single spring the bar appears already at its final size and the effect is gone.
//
// Icons derived from the sidebar's original SVGs, with real alpha and cut-outs.
#include "menu.h"
#include "profiles.h"
#include "tex_cache.h"
#include "gfx.h"
#include "text.h"
#include "anim.h"
#include "layout.h"
#include "settings.h"

// Widths: the collapsed one fits only the icon; the open one is the tvOS bar's,
// wide enough that the longest label ("Profile and Stats") does not touch the edge.
#define NV_MENU_W_ICON   NV_LEGACY_RAIL_W
#define NV_MENU_W_IS_OPEN  392.0f
#define NV_MENU_LINE_H    96.0f
#define NV_MENU_ICON      38.0f
// The icon's centre is the same at both widths: on the device the icon does NOT
// move when the bar opens, only the label comes in beside it. If the icon slid
// along, the opening would become a sideways shove instead of a reveal.
#define NV_MENU_ICON_CX  (NV_MENU_W_ICON * 0.5f)
#define NV_MENU_LABEL_X  112.0f
#define NV_MENU_PILL_PAD   20.0f
#define NV_MENU_RADIUS_PILL  0.20f
// How much the content on the right darkens with the bar open. Without it the
// menu competes for attention with the hero's art, which is bright and fills the
// whole screen.
#define NV_MENU_VEIL        0.58f
// OPENING AND CLOSING TIME, with a clock of its own.
//
// The reference has no side bar at all on the home — LEFT and UP from the first
// card go up to the hero's buttons and stop there; there is no rail to measure.
// The only comparable overlay I managed to open there was the MENU key's context
// sheet, and it gives the timing and the SHAPE of the veil:
//
//   closing (a clean measurement, 10 consecutive frames, no drops):
//     16ms 0.00 | 33 0.09 | 50 0.19 | 66 0.29 | 83 0.38 | 100 0.49
//     117 0.60 | 133 0.73 | 151 0.84 | 166 0.98
//   that is, a STRAIGHT RAMP, ~0.10 every 17 ms, finishing at ~150 ms of travel.
//   opening: the same straight ramp, ~0.0044/ms, which gives ~230 ms of travel.
//
// Two findings the spring did not reproduce: the veil is LINEAR (no spring is),
// and CLOSING is much faster than OPENING. NV_SPRING_SCREEN (9.0) gave a
// symmetric 333 ms with the fastest part at the start, which is the opposite of a ramp.
#define NV_MENU_OPEN_MS  230.0f
#define NV_MENU_CLOSE_MS 150.0f
// The width still LAGS behind the entry — it is the "it came in and then it
// opened" effect described at the top of the file. There is no measurement from
// the reference for it (this bar does not exist there); what changed was only
// the total time, now tied to the same clock instead of a loose spring stiffness.
#define NV_MENU_EXP_LENTO  1.6f

// Labels and order checked against the reference. "Search" is the noun, not the
// verb: the other entries are nouns too, and a verb among them read as odd.
static const char *LABELS[MENU_N] = { "Home", "Search", "Library", "Profile and Stats", "Settings" };

// FOOTER: who is using the app, and the door to switching. It is one EXTRA focus
// item, at index MENU_N — it deliberately did not go into the enum, because
// switching profile is not a tab of the app and nobody should be able to
// "navigate" to it as a destination.
#define NV_MENU_FOOTER_H   112.0f
#define NV_MENU_AVATAR      56.0f
#define NV_MENU_FOCUSES      (MENU_N + 1)
#define MENU_FOOTER         MENU_N

static int   requestedSwap = 0;
static int   is_open  = 0;
static int   destination = MENU_START;
static int   line   = MENU_START;   // the highlight; it only becomes the destination on choosing
static int   changed   = 0;
static float slides = 0.0f;
static float expands = 0.0f;
static float animFocus[NV_MENU_FOCUSES];
static void icon(int d, float cx, float cy, float s, float r, float g, float b, float a);
static void drawFooter(float px, float w, float alpha, float focus);

// Legacy keeps the 144px rail always visible. The expanded menu is an extra
// layer; we do not shift the content when it closes.
static void drawRailFixed(void) {
  GfxRect panel = { 0, 0, NV_LEGACY_RAIL_W, NV_SCREEN_H };
  gfx_color(panel, 0.0f, 0.055f, 0.058f, 0.064f, 1.0f);
  float y = (NV_SCREEN_H - MENU_N * NV_MENU_LINE_H) * 0.5f;
  for (int i = 0; i < MENU_N; i++, y += NV_MENU_LINE_H) {
    int current = (i == destination);
    float luma = current ? 1.0f : 0.60f;
    if (current) {
      GfxRect brand = { 18.0f, y + 12.0f, NV_LEGACY_RAIL_W - 36.0f,
                        NV_MENU_LINE_H - 24.0f };
      gfx_color(brand, NV_MENU_RADIUS_PILL, 0.20f, 0.22f, 0.25f, 0.70f);
    }
    icon(i, NV_MENU_ICON_CX, y + NV_MENU_LINE_H * 0.5f,
          NV_MENU_ICON, luma, luma, luma, 0.95f);
  }
  drawFooter(0.0f, NV_LEGACY_RAIL_W, 0.95f, 0.0f);
}

int menu_start(void) {
  is_open = 0; destination = MENU_START; line = MENU_START; changed = 0;
  slides = 0.0f; expands = 0.0f;
  for (int i = 0; i < MENU_N; i++) animFocus[i] = 0.0f;
  return 1;
}

void menu_open(void) {
  if (is_open) return;
  // The highlight always starts on the destination in force, never where it was
  // left last time: the bar is a map of where you are, and opening it with the
  // highlight on another item would tell the user they had already changed screen.
  line = destination;
  is_open = 1;
}
void menu_close(void) { is_open = 0; line = destination; }

int menu_is_open(void)  { return is_open; }
int menu_visible(void) { return 1; }
int menu_destination(void) { return destination; }
void menu_set_destination(int d) {
  if (d < 0 || d >= MENU_N) return;
  destination = d;
  if (!is_open) line = d;
}
int menu_changed_destination(void) { int m = changed; changed = 0; return m; }
const char *menu_label(int d) {
  return (d >= 0 && d < MENU_N) ? LABELS[d] : "";
}

// Confirms the highlight and collapses. RIGHT also comes through here: on the
// device the bar does not "cancel" when you leave to the right — the highlighted
// item is the one the user is looking at, and undoing the choice on the way back
// would be a surprise.
static void choose(void) {
  if (line == MENU_FOOTER) {
    // The footer does not change destination: it asks for the profile picker screen.
    requestedSwap = 1;
    is_open = 0;
    line = destination;
    return;
  }
  if (line != destination) { destination = line; changed = 1; }
  is_open = 0;
}

int menu_requested_swap(void) { int p = requestedSwap; requestedSwap = 0; return p; }

void menu_event(const SDL_Event *e) {
  if (!is_open || e->type != SDL_KEYDOWN) return;
  SDL_Keycode k = e->key.keysym.sym;

  // The same set of "back" keys the detail screen accepts: on the remote it is
  // Back, on a keyboard everyone reaches a different one.
  if (k == SDLK_AC_BACK || k == SDLK_ESCAPE || k == SDLK_BACKSPACE ||
      k == SDLK_DELETE) { menu_close(); return; }

  if (k == SDLK_RIGHT || k == SDLK_RETURN || k == SDLK_KP_ENTER) { choose(); return; }
  // No wrap-around at the ends: the bar is short and the user sees all four rows
  // at once, so wrapping at the end of the list reads as a fault, not a shortcut.
  if (k == SDLK_DOWN && line < NV_MENU_FOCUSES - 1) line++;
  else if (k == SDLK_UP && line > 0)       line--;
  // LEFT dies here on purpose: the bar is already the edge of the screen.
}

void menu_update(float dt, Uint32 now) {
  (void)now;
  // Collapsed and settled costs nothing: no spring, no loop over the destinations.
  if (!is_open && slides < 0.002f) {
    if (slides != 0.0f) { slides = 0.0f; expands = 0.0f; }
    return;
  }
  float target = is_open ? 1.0f : 0.0f;
  float ms   = is_open ? NV_MENU_OPEN_MS : NV_MENU_CLOSE_MS;
  slides = anim_ramp(slides, target, dt, ms);
  expands = anim_ramp(expands, target, dt, ms * NV_MENU_EXP_LENTO);
  for (int i = 0; i < NV_MENU_FOCUSES; i++) {
    float a = (is_open && i == line) ? 1.0f : 0.0f;
    animFocus[i] = anim_spring(animFocus[i], a, dt,
                            a > animFocus[i] ? NV_SPRING_FOCUS : NV_SPRING_BLUR);
  }
}

// Mesmos vetores do sidebar oficial, rasterizados no build e tintados pelo shader.
static void icon(int d, float cx, float cy, float s, float r, float g, float b, float a) {
  static const char *names[MENU_N] = {"menu_home", "menu_search", "menu_library", "menu_profile", "menu_settings"};
  if (d < 0 || d >= MENU_N) return;
  gfx_icon((GfxRect){cx-s*.5f, cy-s*.5f, s, s}, names[d], r, g, b, a);
}


// The avatar's colour from the "#RRGGBB" the account stores. With no readable
// colour, the web app's default blue.
static void colorAvatar(const char *hex, float *r, float *g, float *b) {
  unsigned v = 0;
  *r = 0.12f; *g = 0.53f; *b = 0.90f;
  if (!hex || hex[0] != '#' || strlen(hex) < 7) return;
  if (sscanf(hex + 1, "%6x", &v) != 1) return;
  *r = ((v >> 16) & 255) / 255.0f;
  *g = ((v >> 8) & 255) / 255.0f;
  *b = (v & 255) / 255.0f;
}

// The name's INITIAL, respecting UTF-8: a name starting with an accent is two
// bytes, and cutting at the first draws rubbish.
static void initialOf(const char *name, char *dst, size_t size) {
  if (size < 3) { if (size) dst[0] = 0; return; }
  dst[0] = (name && name[0]) ? name[0] : '?';
  dst[1] = 0;
  if (name && (unsigned char)name[0] >= 0xC0 && name[1]) { dst[1] = name[1]; dst[2] = 0; }
}

// Footer: who is using it, and the door to switching. It draws at BOTH widths —
// collapsed shows only the avatar (the only thing that fits in 144px), open
// shows the name and the action.
static void drawFooter(float px, float w, float alpha, float focus) {
  const AccountProfile *p = profiles_item_active();
  // Above the safe area, not stuck to the bottom: on a TV the last 60px may be
  // off the panel (overscan), and the user's name is precisely what disappears first.
  float y = NV_SCREEN_H - NV_MARGIN_Y - NV_MENU_FOOTER_H;
  float cx = px + NV_MENU_ICON_CX;
  float cy = y + NV_MENU_FOOTER_H * 0.5f;
  float cr, cg, cb;
  char start[4];
  GfxRect av;

  if (alpha <= 0.01f) return;

  if (focus > 0.01f) {
    GfxRect pill = { px + NV_MENU_PILL_PAD, y + 8.0f,
                     w - NV_MENU_PILL_PAD * 2.0f, NV_MENU_FOOTER_H - 16.0f };
    GfxRect ring = { pill.x - NV_RING_FOCUS, pill.y - NV_RING_FOCUS,
                     pill.w + NV_RING_FOCUS * 2, pill.h + NV_RING_FOCUS * 2 };
    float radius = (pill.h * NV_MENU_RADIUS_PILL + NV_RING_FOCUS) / ring.h;
    gfx_color(ring, radius, 1, 1, 1, focus * alpha);
    gfx_color(pill, NV_MENU_RADIUS_PILL, NV_COLOR_FOCUS_R, NV_COLOR_FOCUS_G,
            NV_COLOR_FOCUS_B, focus * alpha);
  }

  av.x = cx - NV_MENU_AVATAR * 0.5f;
  av.y = cy - NV_MENU_AVATAR * 0.5f;
  av.w = av.h = NV_MENU_AVATAR;

  // A PHOTO when the account has one; otherwise the circle with the initial,
  // which is what the web app shows when `avatar_url` is null — and on this
  // account it is.
  { GLuint tex = (p && p->avatarUrl[0]) ? tex_get(p->avatarUrl) : 0;
    if (tex) {
      gfx_tex_aspect_current = 1.0f;
      gfx_rect(av, tex, GFX_CARD, 0, 0, 0, 0.5f, 0, 0, 0, alpha);
    } else {
      colorAvatar(p ? p->colorHex : NULL, &cr, &cg, &cb);
      gfx_color(av, 0.5f, cr, cg, cb, alpha);
      initialOf(p ? p->name : NULL, start, sizeof start);
      { TxtLine l = txt_line(TXT_HEADLINE, start, 255, 255, 255, 255);
        txt_draw_alpha(l, av.x + (av.w - l.w) * 0.5f,
                           av.y + (av.h - l.h) * 0.5f, alpha); } } }

  // The name and the action only appear with the bar open: 144px does not fit
  // text, and squeezing the name in there would be worse than not showing it.
  { float aText = expands * expands * alpha;
    if (aText > 0.01f) {
      int c = (int)(anim_blend(0.72f, 1.0f, focus) * 255.0f + 0.5f);
      TxtLine name = txt_line_trim(TXT_BODY, p ? p->name : "Your account",
                                      c, c, c, 255,
                                      NV_MENU_W_IS_OPEN - NV_MENU_LABEL_X - 28.0f);
      TxtLine action = txt_line(TXT_CAPTION, "Switch user", 150, 152, 160, 255);
      txt_draw_alpha(name, px + NV_MENU_LABEL_X, cy - name.h - 2.0f, aText);
      txt_draw_alpha(action, px + NV_MENU_LABEL_X, cy + 4.0f, aText);
    } }
}

void menu_draw(Uint32 now) {
  (void)now;
  // The fixed rail is always present, as in the legacy shell. The expanded
  // overlay only comes on stage when the menu has been asked for.
  // `collapseSidebar`: with the bar COLLAPSED the web app draws no rail at all —
  // `.home-nav-list` has width 0 and takes no flow; it only appears as a layer
  // when it takes focus. The port already moved the content to 104 in that case
  // (settings_content_x), but went on painting the rail's 144px underneath it: a
  // dark band under the first card, with nothing on top.
  if (!is_open && slides < .002f && !settings_rail_collapsed()) drawRailFixed();
  if (!is_open && slides < 0.002f) return;

  float w = anim_blend(NV_MENU_W_ICON, NV_MENU_W_IS_OPEN, anim_smooth(expands));
  // The VEIL uses the RAW ramp: the reference's measurement is a straight line
  // (see NV_MENU_OPEN_MS). The panel's POSITION uses the same ramp, smoothed — a
  // block that size stopping dead at the end of its travel reads as a cut, and
  // the reference starts slowly on everything that slides (see anim_spring2 in anim.h).
  float entry = anim_smooth(slides);
  float px = -w * (1.0f - entry);

  GfxRect screen = { 0, 0, NV_SCREEN_W, NV_SCREEN_H };
  gfx_color(screen, 0.0f, 0, 0, 0, NV_MENU_VEIL * slides);

  // A near-opaque panel, a little darker than NV_COLOR_BACKGROUND: sitting
  // against the home's background it needs an edge of its own, otherwise the bar
  // looks like a piece of the screen that darkened by itself.
  GfxRect panel = { px, 0, w, NV_SCREEN_H };
  gfx_color(panel, 0.0f, 0.075f, 0.078f, 0.086f, 0.97f * entry);

  // Everything from here down is clipped to the panel. Without the clip the
  // label — which is drawn at the text's fixed x — leaks into the content while
  // the bar is still narrow, and you see the word appearing outside it.
  gfx_crop(px, 0, w, NV_SCREEN_H);

  float y = (NV_SCREEN_H - MENU_N * NV_MENU_LINE_H) * 0.5f;
  for (int i = 0; i < MENU_N; i++, y += NV_MENU_LINE_H) {
    float f = animFocus[i];
    float cy = y + NV_MENU_LINE_H * 0.5f;

    if (i == destination && f < .99f) {
      GfxRect current = {px + NV_MENU_PILL_PAD, y + 9, w - NV_MENU_PILL_PAD*2, NV_MENU_LINE_H - 18};
      gfx_color(current, NV_MENU_RADIUS_PILL, .16f, .17f, .19f, .6f * (1-f) * slides);
    }
    if (f > 0.01f) {
      GfxRect pill = { px + NV_MENU_PILL_PAD, y + 7.0f,
                       w - NV_MENU_PILL_PAD * 2.0f, NV_MENU_LINE_H - 14.0f };
      // A DARK PILL with a white ring, not a light pill with dark text.
      // MEASURED against the reference: a focused item has background #303030 and
      // text #FFFFFF — the --focus-bg token, which the web app's CSS also
      // declares. Ours inverted it (background #E4E4E9, dark text), and #E4E4E9
      // was no system colour at all: neither white, nor the #F5F5F5 of
      // --secondary-color.
      { GfxRect ring = { pill.x - NV_RING_FOCUS, pill.y - NV_RING_FOCUS,
                         pill.w + NV_RING_FOCUS * 2, pill.h + NV_RING_FOCUS * 2 };
        float radius = (pill.h * NV_MENU_RADIUS_PILL + NV_RING_FOCUS) / ring.h;
        gfx_color(ring, radius, 1, 1, 1, f * slides); }
      gfx_color(pill, NV_MENU_RADIUS_PILL, NV_COLOR_FOCUS_R, NV_COLOR_FOCUS_G,
              NV_COLOR_FOCUS_B, f * slides);
    }

    // Three states, and all three need to exist: focused (WHITE text over the
    // dark pill), the destination in force (white, so the user can find where
    // they are without moving the focus) and the rest (grey). With only two
    // states, opening the menu erases the indication of where you were.
    int current = (i == destination);
    float luma = anim_blend(current ? 1.0f : 0.62f, 1.0f, f);
    float alpha = slides * anim_blend(current ? 1.0f : 0.85f, 1.0f, f);

    icon(i, px + NV_MENU_ICON_CX, cy, NV_MENU_ICON, luma, luma, luma, alpha);

    // The label comes in with the width, not before it: `expands` squared holds
    // the word back until the bar really has room, otherwise it is born squashed
    // against the icon.
    float aRot = expands * expands * entry;
    if (aRot > 0.01f) {
      int c = (int)(luma * 255.0f + 0.5f);
      TxtLine l = txt_line_trim(TXT_BODY, LABELS[i], c, c, c, 255,
                                   NV_MENU_W_IS_OPEN - NV_MENU_LABEL_X - 28);
      txt_draw_alpha(l, px + NV_MENU_LABEL_X, cy - l.h * 0.5f, aRot);
    }
  }

  drawFooter(px, w, entry, animFocus[MENU_FOOTER]);

  gfx_no_crop();
}
