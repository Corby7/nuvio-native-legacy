// The fixed side rail of Nuvio 1.0.1 legacy, with an expandable overlay for
// D-pad navigation.
//
// IT IS THE WEB APP'S SIDEBAR, drawn in GL. Everything below that carries a
// number was read off `.home-sidebar` and its children in css/components.css:
// the 104 row, the 44 icon at x=36, the label at 112, the stadium pill in
// --focus-bg, the Phosphor glyph set with its filled variant on the selected
// row, the brand lockup at the top, and the frosted plate the whole thing sits
// on. Where the two apps disagree it is noted at the spot, and there are only
// three places: the icon column (further into the pill), the footer (the
// web puts the account at the TOP of the list, this app keeps it at the bottom
// where the D-pad expects it) and the fixed rail (in the web the collapsed bar
// takes no width at all, here it is a permanent 144 band).
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
#include "menu.h"
#include "profiles.h"
#include "tex_cache.h"
#include "gfx.h"
#include "text.h"
#include "anim.h"
#include "layout.h"
#include "settings.h"
#include "pointer.h"

// Widths: the collapsed one fits only the icon; the open one is in layout.h,
// because main.c sizes the backdrop's grab from it.
#define NV_MENU_W_ICON   NV_LEGACY_RAIL_W
// The row, MEASURED: --legacy-sidebar-item-height 104 and --legacy-sidebar-item-gap 12.
#define NV_MENU_LINE_H   116.0f
#define NV_MENU_PILL_H   104.0f
// --sidebar-icon-size, 44. It used to be 38, and next to the Phosphor glyphs —
// which are drawn thin, on a 256 grid — 38 read as a smaller icon than the web's,
// not merely a smaller box.
#define NV_MENU_ICON      44.0f
// THE RAIL'S MIDPOINT, and a DELIBERATE departure from the web: `.home-nav-item`
// there has `padding: 0 36px` and a 44 icon, so the glyph sits at 36..80, left of
// centre — which is invisible in the browser, where the collapsed bar has width 0
// and the icons are only ever seen inside the open panel. Here the rail is a
// permanent 144 band and every glyph in it read as pushed against the left edge.
// The glyphs and the footer's avatar hang off this one axis.
#define NV_MENU_ICON_CX  (NV_MENU_W_ICON * 0.5f)
// THE OPEN PANEL'S AXIS. At the rail's 72 the glyph sat only 26 inside the pill,
// hugging its left end; the owner's design has it ~40 in, which puts the centre at
// 24 + 42 + 22. The icons drift from one axis to the other with `expands`, so the
// move is part of the opening rather than a jump.
#define NV_MENU_ICON_CX_OPEN  88.0f
// The gap after the glyph, from whichever axis the glyph is on — a label that did
// not travel with its icon would close the gap and sit against it. 36 and not the
// web's 32: the owner's design measures ~37, and at 28px type the web's 32 read
// as the word leaning on the glyph.
#define NV_MENU_LABEL_GAP  (NV_MENU_ICON * 0.5f + 36.0f)
#define NV_MENU_LABEL_W    (NV_MENU_W_IS_OPEN - NV_MENU_ICON_CX_OPEN - NV_MENU_LABEL_GAP - 28.0f)
// --legacy-sidebar-padding is `48px 24px`: 24 each side, and the pill fills what
// is left of the box.
#define NV_MENU_PILL_PAD   24.0f
// THE WEB'S STADIUM. --legacy-sidebar-item-radius is 64 on a 104 row — past half
// the height, so a browser caps it into a full pill. The port had a 0.20 rounded
// rectangle with a white ring around it; the owner's design is the plain #303030
// stadium, no ring, and with the bar at 340 it no longer reads as a lozenge.
#define NV_MENU_RADIUS_PILL  0.50f
// ONE PILL THAT GLIDES. The focus is a single stadium that travels to the row you
// move to, the way the selection moves in Apple's sidebars, instead of one pill
// fading out while the next fades in. anim_spring2 for the travel: it leaves
// softly and lands without overshoot, and it chases a held key without
// restarting. At 24 rad/s one row's step is essentially settled in ~250 ms.
#define NV_MENU_GLIDE_W      24.0f
// A hairline of light around the focused pill, so the stadium reads as a lit
// surface rather than a flat grey cut-out. Faint on purpose: the white ring the
// port had was this at full strength, and that one shouted.
#define NV_MENU_EDGE_PX      1.5f
#define NV_MENU_EDGE_FOCUS_A 0.14f
// How much the content on the right darkens with the bar open. MEASURED on
// `.home-shell::before`: rgba(0,0,0,0.5).
#define NV_MENU_VEIL        0.50f
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

// THE GLASS. `.home-sidebar::before` is
//   backdrop-filter: blur(52px) saturate(160%) brightness(0.85)
//   background: linear-gradient(180deg, rgba(255,255,255,.06) 0%, transparent 18%)
//               top / 100% 100px no-repeat, rgba(8,12,24,.62)
//   border-right: 1px solid rgba(255,255,255,.09)
// The filter chain and the plate live in GFX_BACKDROP; what is left here is the
// sheen's height (18% of the gradient's own 100px box) and the hairline.
#define NV_MENU_SHEEN_H     18.0f
#define NV_MENU_EDGE_A      0.09f
// The plate GFX_BACKDROP paints, repeated for the frames where there is no glass
// at all (no FBO on the device, or the strip has not been grabbed yet). Without a
// fallback the bar would be a hole in the screen rather than merely flat.
#define NV_MENU_PLATE_R     0.031f
#define NV_MENU_PLATE_G     0.047f
#define NV_MENU_PLATE_B     0.094f
#define NV_MENU_PLATE_A     0.62f

// THE BRAND. `.home-brand-wrap` is a ROW, 80 tall: a 54x54 mark, a 32 gap and a
// 54-tall wordmark, all `object-fit: contain` under --legacy-sidebar-padding's 48
// of top inset. The row is CENTRED in the bar rather than pinned to the web's 36
// left inset, on the owner's call.
//
// NV_MENU_BRAND_WORD is the wordmark's CAP HEIGHT, not a box, because the file was
// trimmed to its ink: it ships 221x108 with the letters in y30..70, so the web's
// 54-tall contain box actually draws 41/108 of it — 20.5 of cap. Same number, said
// in the units the trimmed file needs.
#define NV_MENU_BRAND_Y     48.0f
#define NV_MENU_BRAND_H     80.0f
#define NV_MENU_BRAND       54.0f
#define NV_MENU_BRAND_WORD  20.5f
#define NV_MENU_BRAND_GAP   32.0f

// Labels and order checked against the reference. "Search" is the noun, not the
// verb: the other entries are nouns too, and a verb among them read as odd.
static const char *LABELS[MENU_N] = { "Home", "Search", "Library", "Profile", "Settings" };

// FOOTER: who is using the app, and the door to switching. It is one EXTRA focus
// item, at index MENU_N — it deliberately did not go into the enum, because
// switching profile is not a tab of the app and nobody should be able to
// "navigate" to it as a destination.
// The same row as any other, so the avatar lines up with the column of icons
// above it: `.home-profile-pill` is --legacy-sidebar-item-height (104) with a
// --sidebar-leading-visual-size (44) avatar, the same 44 the glyphs use.
#define NV_MENU_FOOTER_H   NV_MENU_PILL_H
#define NV_MENU_AVATAR      44.0f
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
static float  pillY = 0.0f, pillV = 0.0f;  // the gliding pill's top, and its speed
static int    pillSnap = 1;                // place it without travel on the next update
static void icon(int d, int filled, float cx, float cy, float s, float a);
static void drawFooter(float px, float w, float alpha, float focus);
static void drawGlass(float w, float alpha);
static void pointRow(int row, int unused);

// The top of the pill on focus `row`: a nav row, or the footer, which sits apart
// at the bottom of the bar — the pill glides the whole gap to reach it.
static float pillTop(int row) {
  if (row == MENU_FOOTER) return NV_SCREEN_H - NV_MARGIN_Y - NV_MENU_FOOTER_H;
  return (NV_SCREEN_H - MENU_N * NV_MENU_LINE_H) * 0.5f
       + row * NV_MENU_LINE_H + (NV_MENU_LINE_H - NV_MENU_PILL_H) * 0.5f;
}

// THE FOCUS PILL: the faint lit edge and the #303030 stadium over it.
static void drawFocusPill(GfxRect pill, float a) {
  GfxRect edge = { pill.x - NV_MENU_EDGE_PX, pill.y - NV_MENU_EDGE_PX,
                   pill.w + NV_MENU_EDGE_PX * 2, pill.h + NV_MENU_EDGE_PX * 2 };
  if (a <= 0.01f) return;
  gfx_color(edge, NV_MENU_RADIUS_PILL, 1, 1, 1, NV_MENU_EDGE_FOCUS_A * a);
  gfx_color(pill, NV_MENU_RADIUS_PILL, NV_COLOR_FOCUS_R, NV_COLOR_FOCUS_G,
            NV_COLOR_FOCUS_B, a);
}

// Where the icon column is right now: the rail's midpoint collapsed, the open
// panel's axis expanded, and the same smoothing the width uses in between.
static float iconAxis(void) {
  return anim_blend(NV_MENU_ICON_CX, NV_MENU_ICON_CX_OPEN, anim_smooth(expands));
}

// The frosted plate the bar sits on, `.home-sidebar::before` in GL. The blur is
// LIVE — it is whatever app_draw has already put behind this strip — so it has to
// be drawn before anything of the bar's own.
//
// The hairline down the right edge is drawn here and not in the shader: at this
// size it is a single layout pixel, and a `step()` in the fragment would land on
// a different pixel depending on the panel's sub-pixel position, flickering while
// the bar slides.
static void drawGlass(float w, float alpha) {
  GfxRect plate = { 0, 0, w, NV_SCREEN_H };
  GfxRect edge  = { w - 1.0f, 0, 1.0f, NV_SCREEN_H };
  if (w <= 1.0f || alpha <= 0.004f) return;
  // The plate goes down FIRST and the glass over it. On the frames where the
  // backdrop is unavailable the plate is all there is, and the bar is flat rather
  // than transparent; where the glass does draw, it carries the same plate inside
  // its own filter chain, so the two agree instead of stacking.
  gfx_color(plate, 0.0f, NV_MENU_PLATE_R, NV_MENU_PLATE_G, NV_MENU_PLATE_B,
            NV_MENU_PLATE_A * alpha);
  gfx_backdrop(plate, alpha, NV_MENU_SHEEN_H / NV_SCREEN_H);
  gfx_color(edge, 0.0f, 1.0f, 1.0f, 1.0f, NV_MENU_EDGE_A * alpha);
}

// Legacy keeps the 144px rail always visible. The expanded menu is an extra
// layer; we do not shift the content when it closes.
//
// NO PILL on the current row. The web app draws a background on `.home-nav-item`
// only when the bar is EXPANDED (`.home-sidebar.content-expanded .home-nav-item
// .focused`); collapsed, the whole indication is the icon — the filled variant of
// the glyph, at full white, against outlines at 50%. The port had a grey pill
// here, and it was the one thing on the rail with no counterpart in the web at all.
static void drawRailFixed(Uint32 now) {
  // The picture first, the bar over it: gfx_backdrop_grab reads the frame buffer,
  // and by this point in app_draw it holds the content and nothing of the menu.
  gfx_backdrop_grab((unsigned)now);
  drawGlass(NV_LEGACY_RAIL_W, 1.0f);
  float y = (NV_SCREEN_H - MENU_N * NV_MENU_LINE_H) * 0.5f;
  for (int i = 0; i < MENU_N; i++, y += NV_MENU_LINE_H) {
    // --text-color at opacity 1 for the row you are on, at 0.5 for the rest:
    // `.home-nav-icon-wrap { opacity: .5 }` and 1 on `.selected`. The colour is
    // white in both cases — it is the OPACITY that carries the state, which is
    // also what keeps the glyph from having to be re-tinted.
    int current = (i == destination);
    icon(i, current, NV_MENU_ICON_CX, y + NV_MENU_LINE_H * 0.5f,
         NV_MENU_ICON, current ? 1.0f : 0.5f);
  }
  drawFooter(0.0f, NV_LEGACY_RAIL_W, 1.0f, 0.0f);
  y = (NV_SCREEN_H - MENU_N * NV_MENU_LINE_H) * 0.5f;
  for (int i = 0; i < MENU_N; i++, y += NV_MENU_LINE_H)
    pointer_zone_hover(0, y, NV_LEGACY_RAIL_W, NV_MENU_LINE_H, pointRow, i, 0);
  pointer_zone_hover(0, pillTop(MENU_FOOTER), NV_LEGACY_RAIL_W, NV_MENU_FOOTER_H,
                     pointRow, MENU_FOOTER, 0);
}

// THE POINTER ON THE BAR. Collapsed, landing on the rail opens the bar on the
// row under the pointer — the web's hover-to-expand. Open, it moves the
// highlight, as UP and DOWN would. `away` is the content beside the open bar:
// pointing back at it closes the bar, the way RIGHT does.
static void pointRow(int row, int unused) {
  (void)unused;
  if (row < 0 || row >= NV_MENU_FOCUSES) return;
  if (!is_open) menu_open();
  line = row;
}
static void pointAway(int a, int b) { (void)a; (void)b; if (is_open) menu_close(); }
// With the rail collapsed there is nothing at the edge to land on, so the strip
// itself opens the bar, on the destination in force as the LEFT key does.
static void pointEdge(int a, int b) { (void)a; (void)b; if (!is_open) menu_open(); }

int menu_start(void) {
  is_open = 0; destination = MENU_START; line = MENU_START; changed = 0;
  slides = 0.0f; expands = 0.0f;
  for (int i = 0; i < MENU_N; i++) animFocus[i] = 0.0f;
  return 1;
}

void menu_open(void) {
  if (is_open) return;
  pillSnap = 1;   // it appears ON the current row; gliding in from the last one
                  // the bar was closed on would read as a stale focus moving
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

// Confirms the highlight and collapses. Only OK comes through here: RIGHT used to
// as well, and on the remote that meant scrolling past a row and stepping back to
// the content opened whatever row the highlight happened to rest on.
//
// It reports a choice even when the row IS the destination in force: the bar
// opens from sub-screens too (Discover under Search, a person's activity), and
// picking "Search" from Discover has to go to Search. The app ignores a choice of
// the screen it is already on.
static void choose(void) {
  if (line == MENU_FOOTER) {
    // The footer does not change destination: it asks for the profile picker screen.
    requestedSwap = 1;
    is_open = 0;
    line = destination;
    return;
  }
  destination = line; changed = 1;
  is_open = 0;
}

int menu_requested_swap(void) { int p = requestedSwap; requestedSwap = 0; return p; }

void menu_event(const SDL_Event *e) {
  if (!is_open || e->type != SDL_KEYDOWN) return;
  SDL_Keycode k = e->key.keysym.sym;

  // The same set of "back" keys the detail screen accepts: on the remote it is
  // Back, on a keyboard everyone reaches a different one.
  // RIGHT is the way back to the content, so it closes like Back does.
  if (k == SDLK_AC_BACK || k == SDLK_ESCAPE || k == SDLK_BACKSPACE ||
      k == SDLK_DELETE || k == SDLK_RIGHT) { menu_close(); return; }

  if (k == SDLK_RETURN || k == SDLK_KP_ENTER) { choose(); return; }
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
  if (pillSnap) { pillY = pillTop(line); pillV = 0.0f; pillSnap = 0; }
  else pillY = anim_spring2(&pillV, pillY, pillTop(line), dt, NV_MENU_GLIDE_W);
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

// THE WEB APP'S OWN GLYPHS, both variants. ROOT_SIDEBAR_ITEMS in
// sidebarNavigation.js carries an `iconMarkup` and a `filledIconMarkup` per
// entry, and the CSS swaps one for the other on `.selected` — that swap is the
// selected row's main signal, and drawing the outline in both states loses it.
// The paths were taken from that file and rasterised into art/icons; only
// "Profile" had to be chosen, because the web has no such row (its
// account entry is the avatar itself, which is this bar's footer).
//
// The colour is always WHITE and the state rides on the alpha: the CSS does the
// same (`.home-nav-icon-wrap { color: var(--text-color); opacity: .5 }`), and its
// comment says why — on webOS a change of fill re-rasterises the SVG, an opacity
// is composited. Here the reason is smaller but points the same way: one tint
// means the two variants cannot drift apart in colour.
static void icon(int d, int filled, float cx, float cy, float s, float a) {
  static const char *names[MENU_N] = {"menu_home", "menu_search", "menu_library", "menu_profile", "menu_settings"};
  static const char *fills[MENU_N] = {"menu_home_fill", "menu_search_fill", "menu_library_fill", "menu_profile_fill", "menu_settings_fill"};
  if (d < 0 || d >= MENU_N) return;
  gfx_icon((GfxRect){cx-s*.5f, cy-s*.5f, s, s}, filled ? fills[d] : names[d],
           1.0f, 1.0f, 1.0f, a);
}

// THE BRAND LOCKUP, `.home-brand-wrap`: mark and wordmark side by side on one
// row, the row centred in the bar (see the constants).
//
// The two go through different modes. GFX_CARD would drop the mark's alpha and
// paint a black box behind it; GFX_BRAND would flatten its gradient into one
// colour — GFX_TEXT is the only mode that keeps a texture's RGB AND honours its
// alpha. The wordmark, being a single white, is what GFX_BRAND is for, and takes
// its colour from here.
//
// Both are `object-fit: contain`, so the widths come from the files' real aspects:
// hard-coding them would deform the logo if the art were ever swapped. And BOTH
// have to be loaded before either is drawn — the row is centred on their combined
// width, so drawing the mark while the wordmark is still decoding would centre it
// alone and then shunt it left when the word arrived.
//
// `w` is the panel's CURRENT width, not its open one: the row is centred in the
// bar, so while the bar is widening the logo travels with it. Centring on the open
// width instead would park it off the right edge for the first half of the
// animation and then have it appear already in place, which reads as a pop.
static void drawBrand(float px, float w, float alpha) {
  GLuint tMark = 0, tWord = 0;
  float apMark = 0.0f, apWord = 0.0f;
  float cy = NV_MENU_BRAND_Y + NV_MENU_BRAND_H * 0.5f;
  float wMark, wWord, x;

  if (alpha <= 0.01f) return;

  // Each path is used and finished with INSIDE its own block. gfx_icon_path hands
  // back ONE static buffer, so holding two at a time would leave the first
  // pointing at the second's file — harmless in this order today, and a silent
  // swap of the two logos the moment anyone reorders the lines.
  //
  // TEX_GET_EXACT, and the size asked for is the size drawn — which is a small
  // circularity, because that width comes from the aspect and the aspect only
  // exists once the file has been decoded. So the FIRST request is made at the
  // box's own height, and the one that follows it, on the frame the art lands,
  // asks for the real width and re-decodes once. It settles there: tex_aspect
  // answers from the FILE's shape, so the width it produces does not move with the
  // texture underneath it.
  { const char *cam = gfx_icon_path("brand_mark");
    apMark = tex_aspect(cam);
    tMark = tex_get_exact(cam, apMark > 0.0f && apMark < 1.0f
                               ? NV_MENU_BRAND * apMark : NV_MENU_BRAND);
    if (!tMark) apMark = 0.0f; }
  { const char *cam = gfx_icon_path("brand_wordmark");
    apWord = tex_aspect(cam);
    tWord = tex_get_exact(cam, apWord > 0.0f ? NV_MENU_BRAND_WORD * apWord
                                             : NV_MENU_BRAND_WORD * 6.0f);
    if (!tWord) apWord = 0.0f; }
  if (apMark <= 0.0f || apWord <= 0.0f) return;

  // "contain" in a 54 box: the taller-than-wide mark keeps the height and gives up
  // width, and a mark that were wider than tall would do the opposite.
  wMark = (apMark < 1.0f) ? NV_MENU_BRAND * apMark : NV_MENU_BRAND;
  wWord = NV_MENU_BRAND_WORD * apWord;
  x = px + (w - (wMark + NV_MENU_BRAND_GAP + wWord)) * 0.5f;

  gfx_tex_aspect_current = 0.0f;
  gfx_rect((GfxRect){x, cy - NV_MENU_BRAND * 0.5f, wMark, NV_MENU_BRAND},
           tMark, GFX_TEXT, 0, 0, 0, 0.0f, 1, 1, 1, alpha);
  x += wMark + NV_MENU_BRAND_GAP;
  gfx_icon((GfxRect){x, cy - NV_MENU_BRAND_WORD * 0.5f, wWord, NV_MENU_BRAND_WORD},
           "brand_wordmark", 1, 1, 1, alpha);
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
  float cx = px + iconAxis();
  float cy = y + NV_MENU_FOOTER_H * 0.5f;
  float cr, cg, cb;
  char start[4];
  GfxRect av;

  if (alpha <= 0.01f) return;

  av.x = cx - NV_MENU_AVATAR * 0.5f;
  av.y = cy - NV_MENU_AVATAR * 0.5f;
  av.w = av.h = NV_MENU_AVATAR;

  // A PHOTO when the account has one; otherwise the circle with the initial,
  // which is what the web app shows when `avatar_url` is null — and on this
  // account it is.
  // THE SAME PICTURE THE PICKER SHOWS, resolved the same way: this used to read
  // p->avatarUrl on its own, so a profile whose picture is one of Nuvio's own
  // avatars (avatar_id, no url) fell through to the initial here while the picker
  // two screens back showed the photograph. profiles_avatar() knows both routes.
  { char url[420];
    int has = p ? profiles_avatar(p, url, sizeof url) : 0;
    GLuint tex = has ? tex_get_width(url, av.w) : 0;
    // The colour goes DOWN FIRST, under the photo — these avatars are PNGs on a
    // transparent ground and without it the cut-out sits on the frosted glass
    // with nothing behind it. The picker does the same, and for the same reason.
    if (tex) {
      colorAvatar(p ? p->colorHex : NULL, &cr, &cg, &cb);
      gfx_rect(av, 0, GFX_DISK, 0, 0, 0, 0.5f, cr, cg, cb, alpha);
    }
    if (tex) {
      // THE FILE'S OWN ASPECT, and GFX_AVATAR. This was GFX_CARD with
      // gfx_tex_aspect_current pinned to 1.0 — a claim that every avatar is
      // square. The "cover" then has nothing to crop and maps the whole image
      // onto the disc, so a photo that is not square came out squashed into the
      // circle. It is the one number the mode cannot guess and the only one that
      // was wrong.
      //
      // GFX_AVATAR rather than a rounded GFX_CARD for the reason gfx.h gives: at
      // 44px the rectangle SDF's corner rounding leaves burrs on the rim, and the
      // disc's own radial mask does not. The same pair social.c and profile.c use.
      gfx_tex_aspect_current = tex_aspect(url);
      gfx_rect(av, tex, GFX_AVATAR, 0, 0, 0, 0.0f, 1, 1, 1, alpha);
      gfx_tex_aspect_current = 0.0f;
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
      // --text-secondary (#b3b3b3) resting, --text-color on focus; the action
      // line sits at --text-tertiary (#808080), the same grey the unselected rows
      // use on the rail. The port had 150,152,160 here — a blue-tinted grey that
      // is in none of the theme's tokens.
      int c = (int)(anim_blend(0.702f, 1.0f, focus) * 255.0f + 0.5f);
      TxtLine name = txt_line_trim(TXT_MENU_ITEM, p ? p->name : "Your account",
                                      c, c, c, 255,
                                      NV_MENU_LABEL_W);
      // The name alone, CENTRED on the row. There used to be a "Switch user"
      // caption under it, which is why the name sat a line high; with the caption
      // gone that offset would leave it hanging above the avatar it belongs to.
      txt_draw_alpha(name, cx + NV_MENU_LABEL_GAP, cy - name.h * 0.5f, aText);
    } }
}

void menu_draw(Uint32 now) {
  // The fixed rail is always present, as in the legacy shell. The expanded
  // overlay only comes on stage when the menu has been asked for.
  // `collapseSidebar`: with the bar COLLAPSED the web app draws no rail at all —
  // `.home-nav-list` has width 0 and takes no flow; it only appears as a layer
  // when it takes focus. The port already moved the content to 104 in that case
  // (settings_content_x), but went on painting the rail's 144px underneath it: a
  // dark band under the first card, with nothing on top.
  if (!is_open && slides < .002f && !settings_rail_collapsed()) drawRailFixed(now);
  if (!is_open && settings_rail_collapsed())
    pointer_zone_hover(0, 0, NV_MENU_EDGE_ZONE_W, NV_SCREEN_H, pointEdge, 0, 0);
  if (!is_open && slides < 0.002f) return;

  float w = anim_blend(NV_MENU_W_ICON, NV_MENU_W_IS_OPEN, anim_smooth(expands));
  // The VEIL uses the RAW ramp: the reference's measurement is a straight line
  // (see NV_MENU_OPEN_MS). The panel's POSITION uses the same ramp, smoothed — a
  // block that size stopping dead at the end of its travel reads as a cut, and
  // the reference starts slowly on everything that slides (see anim_spring2 in anim.h).
  float entry = anim_smooth(slides);
  float px = -w * (1.0f - entry);

  // BEFORE the veil, for the reason gfx_backdrop_grab spells out: everything from
  // here on is the menu, and the glass must not photograph itself.
  gfx_backdrop_grab((unsigned)now);

  GfxRect screen = { 0, 0, NV_SCREEN_W, NV_SCREEN_H };
  gfx_color(screen, 0.0f, 0, 0, 0, NV_MENU_VEIL * slides);

  // THE GLASS DOES NOT SLIDE. `backdrop-filter` samples what is under the element
  // AT THE POSITION IT IS IN, so while the panel travels in from the left the
  // frosted strip is already at rest against the screen's edge and only grows
  // wider. Drawing it at the panel's own `px` would drag the blurred picture
  // along with the bar, and the content behind would appear to move sideways.
  drawGlass(px + w, entry);

  // Everything from here down is clipped to the panel. Without the clip the
  // label — which is drawn at the text's fixed x — leaks into the content while
  // the bar is still narrow, and you see the word appearing outside it.
  gfx_crop(px, 0, w, NV_SCREEN_H);

  // The brand comes in with the width, like the labels do: `.home-brand-mark` and
  // `.home-brand-wordmark` are both opacity 0 until `.content-expanded`.
  drawBrand(px, w, expands * expands * entry);

  // ONE pill, on the FOCUSED row, drawn before any row so every label sits over
  // it as it passes. The web gives `.selected` no background of its own —
  // `.home-sidebar.content-expanded .home-nav-item.selected` sets colour and
  // weight, nothing more, and the only rule that paints --focus-bg wants
  // `.focused` (or a hover). What tells you where you are is the FILLED glyph plus
  // white text, exactly as in the browser.
  //
  // A DARK PILL, not a light pill with dark text. MEASURED against the reference:
  // a focused item has background #303030 and text #FFFFFF — the --focus-bg token,
  // which the web app's CSS also declares.
  drawFocusPill((GfxRect){ px + NV_MENU_PILL_PAD, pillY,
                           w - NV_MENU_PILL_PAD * 2.0f, NV_MENU_PILL_H }, slides);

  float y = (NV_SCREEN_H - MENU_N * NV_MENU_LINE_H) * 0.5f;
  for (int i = 0; i < MENU_N; i++, y += NV_MENU_LINE_H) {
    float f = animFocus[i];
    float cy = y + NV_MENU_LINE_H * 0.5f;
    int current = (i == destination);

    // Three states, and all three exist in the web app too: focused (white over
    // the dark pill), the destination in force (white, so the user can find where
    // they are without moving the focus) and the rest (--text-secondary). The
    // GLYPH carries the same three on its alpha alone — 0.5 for the rest, 1 for
    // the other two — plus the fill/outline swap for the destination.
    float luma  = anim_blend(current ? 1.0f : 0.702f, 1.0f, f);
    float aIcon = slides * anim_blend(current ? 1.0f : 0.5f, 1.0f, f);

    icon(i, current, px + iconAxis(), cy, NV_MENU_ICON, aIcon);

    // The label comes in with the width, not before it: `expands` squared holds
    // the word back until the bar really has room, otherwise it is born squashed
    // against the icon.
    float aRot = expands * expands * entry;
    if (aRot > 0.01f) {
      int c = (int)(luma * 255.0f + 0.5f);
      // `.home-nav-item.selected` is `font-weight: bold`; the rest inherit 400.
      TxtLine l = txt_line_trim(current ? TXT_MENU_SEL : TXT_MENU_ITEM, LABELS[i],
                                   c, c, c, 255,
                                   NV_MENU_LABEL_W);
      txt_draw_alpha(l, px + iconAxis() + NV_MENU_LABEL_GAP, cy - l.h * 0.5f, aRot);
    }
  }

  drawFooter(px, w, entry, animFocus[MENU_FOOTER]);

  gfx_no_crop();

  // THE ZONES TAKE THE BAR'S RESTING WIDTH, not the one on screen. While it
  // slides in, the drawn width starts near zero, and `away` measured from it
  // covered the whole screen: the first motion after the bar opened closed it.
  if (is_open) {
    float wOpen = NV_MENU_W_IS_OPEN;
    pointer_zone_hover(wOpen, 0, NV_SCREEN_W - wOpen, NV_SCREEN_H, pointAway, 0, 0);
    y = (NV_SCREEN_H - MENU_N * NV_MENU_LINE_H) * 0.5f;
    for (int i = 0; i < MENU_N; i++, y += NV_MENU_LINE_H)
      pointer_zone(0, y, wOpen, NV_MENU_LINE_H, pointRow, i, 0);
    pointer_zone(0, pillTop(MENU_FOOTER), wOpen, NV_MENU_FOOTER_H, pointRow, MENU_FOOTER, 0);
  }
}
