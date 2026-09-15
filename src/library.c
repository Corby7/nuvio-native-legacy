// The Library, aligned with the web app's screen (MEASURED live, on the owner's
// profile).
//
// ------------------------------------------------------------------------
// WHAT CHANGED, AND WHY
//
// The port had three CENTRED pills ("My List" / "Purchased" / "Genres") and a
// 6-column grid of 212. Measuring the web app's screen, the structure is
// different and has FOUR bands, all left-aligned at x=96:
//
//   .library-page-title    "Library" 56/600, letter-spacing 1, at (96,48)
//   .library-page-source   a "NUVIO" badge 28/500 rgb(128,128,128) ls 4, on the RIGHT
//   .library-view-mode-row y=136: 150x56 pills, radius 999, 21/400 — "Saved" and
//                          "Cloud"; the chosen one bg #303030 border 2px #fff,
//                          the others bg #222 border 2px #333
//   .library-picker-row    y=212: TWO 840x110 pickers, radius 36 — "Type" and
//                          "Sort" —, each with a 19/500 rgb(128) label and a
//                          30/500 white value below it, and an arrow on the right
//   .library-grid          6 columns of 268 (auto-fill with a minimum of 252 over
//                          the 1728 usable, gutter 24), 2:3 poster = 268x402
//                          radius 24 with a 4px border ON THE INSIDE, title
//                          32/500 at 16 from the poster; row step 487.8
//
// The web app's two dimensions ("Saved/Cloud" and the Type filter) replace the
// three invented tabs. "Genres" does not exist in the web app and has gone.
//
// Two decisions that came out of mistakes already made on other screens of this app:
//
//   1. The CHOSEN pill stays marked when the focus moves down into the grid. It
//      was the same problem as the detail screen's season tabs: without the
//      chosen state kept separate from the focus, the user loses sight of where
//      they are.
//   2. Scrolling moves the MINIMUM needed for the focused row to fit. Aligning
//      the focused row to the top pushes the header off screen on the first move
//      down.
#include "library.h"
#include "trakt.h"
#include "gfx.h"
#include "text.h"
#include "tex_cache.h"
#include "focus.h"
#include "anim.h"
#include "layout.h"
#include "settings.h"
#include "catalog.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

// Focus rows: 0 = modes (Saved/Cloud), 1 = pickers (Type/Sort), 2.. = grid.
#define LIB_FILTER_MODE   0
#define LIB_FILTER_PICK   1
#define LIB_FILTER_GRID  2
#define LIB_MAX_LINES (FOCUS_MAX_ROWS - LIB_FILTER_GRID)
#define LIB_GRID_BASE (NV_SCREEN_H - NV_MARGIN_Y)
// How many px a poster takes to disappear as it rises under the header. A
// scissor clip would solve it, but gfx_crop assumes a 1:1 target with the screen
// and a retina Mac delivers double; the fade does not depend on the drawable.
#define LIB_FADE       90.0f

// "Saved" is the device's own list; "Cloud" is what came from Trakt.
enum { MODE_SAVED, MODE_CLOUD, LIB_N_MODES };
static const char *ROT_MODE[LIB_N_MODES] = { "Saved", "Collection" };

// Seletor "Tipo": os mesmos valores do web.
enum { KIND_ALL, KIND_MOVIE, KIND_SERIES, LIB_N_KINDS };
static const char *ROT_KIND[LIB_N_KINDS] = { "All", "Films", "Series" };
// The "Sort" picker.
enum { ORDER_ADDED, ORDER_TITLE, ORDER_YEAR, LIB_N_ORDER };
static const char *ROT_ORDER[LIB_N_ORDER] = { "List order", "Title: A to Z", "Year: newest first" };

static int mode = MODE_SAVED;
static int kind = KIND_ALL;
static int order = ORDER_ADDED;
static int pickSel = 0;          // which of the two pickers has the focus

static int filter[CAT_MAX];      // visible catalogue indices
static int nFilter = 0;
static int totalMode = 0;
static Focus focus;
static float animMode[LIB_N_MODES];
static float animPick[2];
static float animFocus[LIB_MAX_LINES][NV_LIB_COLUMNS];
static float scrollY = 0.0f;
static int wantsExit = 0, request = -1;

// Account state. The real list is Trakt's, which marks ci->inList/inCollection
// ON THE ITEM — there is no per-index table here any more: the catalogue is
// rebuilt from the network and a stored index points at a different title on the
// next round. `bought` survives because there is no source for it yet.
static char bought[CAT_MAX];

static float heightLine(void) {
  // poster + gap + titulo (32/500, lh 1.18 -> 37.8)
  return NV_LIB_POSTER_H + NV_LIB_TITLE_GAP + 37.8f;
}
static float stepLine(void)  { return NV_LIB_LINE_STEP; }
static float stepColumn(void) { return NV_LIB_CARD_W + NV_LIB_CARD_GAP; }
static int   nLines(void)     { return (nFilter + NV_LIB_COLUMNS - 1) / NV_LIB_COLUMNS; }

static int isSeries(const CatItem *ci) {
  return ci && (!strcmp(ci->kind, "series") || ci->nSeasons > 0
                || ci->season > 0);
}

// Rebuilds the visible list and the focus map. Called on every change of mode,
// type or order because the number of columns in the last row changes with the
// filter, and a focus pointing at a column that no longer exists draws an empty
// rectangle.
static void rebuild(void) {
  int n = cat_n();
  if (n > CAT_MAX) n = CAT_MAX;
  nFilter = 0;
  totalMode = 0;
  for (int i = 0; i < n; i++) {
    const CatItem *ci = cat_item(i);
    if (!ci) continue;
    // The two modes showed almost the SAME list: both included ci->inList, so
    // switching pill changed practically nothing and the two had no reason to
    // exist. The split is now Trakt's own, which separates the watchlist (what
    // you intend to watch) from the collection (what you own) — they are
    // different questions and each pill answers one.
    //
    // And it reads from the ITEM, no longer from an inList[] array indexed by
    // position. That array was a known and documented mistake: the catalogue is
    // REBUILT from the network on every discovery, so today's position 3 is a
    // different title tomorrow — the "saved" mark migrated by itself to a film
    // nobody saved. The mark has to live on the item, and it does
    // (CatItem.inList / .inCollection, filled from Trakt's real list).
    int enters = (mode == MODE_SAVED) ? ci->inList
                                      : (ci->inCollection || bought[i]);
    if (!enters) continue;
    totalMode++;
    if (kind == KIND_MOVIE && isSeries(ci)) continue;
    if (kind == KIND_SERIES && !isSeries(ci)) continue;
    // `hideUnreleasedContent`: with no year in `meta` the title has not been
    // released as far as the catalogue is concerned, and the preference says to hide it.
    if (settings_hide_unreleased() && !ci->meta[0]) continue;
    filter[nFilter++] = i;
  }

  // Insertion sort — there are a few dozen items, once per change.
  if (order != ORDER_ADDED) {
    for (int i = 1; i < nFilter; i++) {
      int v = filter[i], j = i - 1;
      while (j >= 0) {
        const CatItem *a = cat_item(filter[j]), *b = cat_item(v);
        int larger;
        if (order == ORDER_TITLE) larger = a && b && strcmp(a->title, b->title) > 0;
        else /* ORDER_YEAR, descending */
          larger = a && b && strcmp(a->meta, b->meta) < 0;
        if (!larger) break;
        filter[j + 1] = filter[j]; j--;
      }
      filter[j + 1] = v;
    }
  }

  int lines = nLines();
  if (lines > LIB_MAX_LINES) lines = LIB_MAX_LINES;
  int cols[FOCUS_MAX_ROWS];
  cols[LIB_FILTER_MODE] = LIB_N_MODES;
  cols[LIB_FILTER_PICK] = 2;
  for (int r = 0; r < lines; r++) {
    int rest = nFilter - r * NV_LIB_COLUMNS;
    cols[LIB_FILTER_GRID + r] = rest > NV_LIB_COLUMNS ? NV_LIB_COLUMNS : rest;
  }
  focus_start(&focus, LIB_FILTER_GRID + lines, cols);
  scrollY = 0.0f;
  memset(animFocus, 0, sizeof animFocus);
}

static int started;
int library_start(void) {
  // Zeroed ONCE per process, and not on every entry.
  if (!started) {
    memset(bought, 0, sizeof bought);
    started = 1;
  }
  mode = MODE_SAVED; kind = KIND_ALL; order = ORDER_ADDED;
  pickSel = 0;
  memset(animMode, 0, sizeof animMode);
  memset(animPick, 0, sizeof animPick);
  wantsExit = 0; request = -1;
  rebuild();
  // The focus is born on the mode bar: whoever comes in is still choosing the slice.
  focus.row = LIB_FILTER_MODE;
  focus.column = mode;
  return 1;
}

void library_shutdown(void) { }

int library_in_list(int i) {
  // The truth is the mark ON THE ITEM, which discovery fills from Trakt's
  // watchlist. The per-index array that used to answer here pointed at a
  // different title as soon as the catalogue was rebuilt.
  const CatItem *c = cat_item(i);
  return c ? c->inList : 0;
}
int library_bought(int i) { return (i >= 0 && i < CAT_MAX) ? bought[i] : 0; }
void library_toggle_list(int i) {
  // It only rebuilds the list. What flips the mark is cat_set_in_list, at the
  // same point that talks to Trakt (app.c) — having TWO owners of the same state
  // was what left the library disagreeing with the detail screen's "+" button.
  (void)i;
  rebuild();
}

int library_wants_exit(void) { return wantsExit; }

int library_requested_open(int *indexCatalog) {
  if (request < 0) return 0;
  if (indexCatalog) *indexCatalog = request;
  request = -1;
  return 1;
}

void library_event(const SDL_Event *e) {
  if (e->type != SDL_KEYDOWN) return;
  SDL_Keycode k = e->key.keysym.sym;
  if (k == SDLK_ESCAPE || k == SDLK_AC_BACK || k == SDLK_BACKSPACE ||
      k == SDLK_DELETE) { wantsExit = 1; return; }

  // Mode bar: left/right SWAPS the mode, and swapping rebuilds the focus map.
  // That is why the mode changes HERE and not through focus_move — calling the
  // two in the wrong order returned the focus to column 0 on every move.
  if (focus.row == LIB_FILTER_MODE) {
    if (k == SDLK_RIGHT && mode < LIB_N_MODES - 1) {
      mode++; rebuild(); focus.row = LIB_FILTER_MODE; focus.column = mode; return;
    }
    if (k == SDLK_LEFT && mode > 0) {
      mode--; rebuild(); focus.row = LIB_FILTER_MODE; focus.column = mode; return;
    }
    if (k == SDLK_DOWN || k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) {
      focus.row = LIB_FILTER_PICK; focus.column = pickSel;
    }
    return;
  }

  // Picker row: left/right moves BETWEEN the two; OK cycles the value of the one
  // in focus. The web app opens a dropdown; on a D-pad, cycling in the picker
  // itself saves the round trip down to the list and back.
  if (focus.row == LIB_FILTER_PICK) {
    if (k == SDLK_RIGHT && pickSel == 0) { pickSel = 1; focus.column = 1; return; }
    if (k == SDLK_LEFT  && pickSel == 1) { pickSel = 0; focus.column = 0; return; }
    if (k == SDLK_UP)   { focus.row = LIB_FILTER_MODE; focus.column = mode; return; }
    if (k == SDLK_DOWN) { if (nFilter) focus_move_grid(&focus, 0, 1); return; }
    if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) {
      if (pickSel == 0) kind = (kind + 1) % LIB_N_KINDS;
      else              order = (order + 1) % LIB_N_ORDER;
      rebuild();
      focus.row = LIB_FILTER_PICK; focus.column = pickSel;
    }
    return;
  }

  if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) {
    int i = (focus.row - LIB_FILTER_GRID) * NV_LIB_COLUMNS + focus.column;
    if (i >= 0 && i < nFilter) request = filter[i];
    return;
  }
  // THE POSTER GRID, and a grid is what it is: see focus_move_grid. Dropping a
  // row here used to land on whichever column the focus had last held in that
  // row, which in a grid of equal-length rows is simply the wrong poster.
  if (k == SDLK_RIGHT)     focus_move_grid(&focus, 1, 0);
  else if (k == SDLK_LEFT) focus_move_grid(&focus, -1, 0);
  else if (k == SDLK_DOWN) focus_move_grid(&focus, 0, 1);
  else if (k == SDLK_UP) {
    if (focus.row == LIB_FILTER_GRID) { focus.row = LIB_FILTER_PICK; focus.column = pickSel; }
    else focus_move_grid(&focus, 0, -1);
  }
}

void library_update(float dt, Uint32 now) {
  (void)now;
  for (int a = 0; a < LIB_N_MODES; a++) {
    float target = (focus.row == LIB_FILTER_MODE && focus.column == a) ? 1.0f : 0.0f;
    animMode[a] = anim_spring(animMode[a], target, dt,
                            target > animMode[a] ? NV_SPRING_FOCUS : NV_SPRING_BLUR);
  }
  for (int p = 0; p < 2; p++) {
    float target = (focus.row == LIB_FILTER_PICK && focus.column == p) ? 1.0f : 0.0f;
    animPick[p] = anim_spring(animPick[p], target, dt,
                            target > animPick[p] ? NV_SPRING_FOCUS : NV_SPRING_BLUR);
  }
  int lines = nLines();
  if (lines > LIB_MAX_LINES) lines = LIB_MAX_LINES;
  for (int r = 0; r < lines; r++)
    for (int c = 0; c < NV_LIB_COLUMNS; c++) {
      float target = focus_index(&focus, LIB_FILTER_GRID + r, c) ? 1.0f : 0.0f;
      animFocus[r][c] = anim_spring(animFocus[r][c], target, dt,
                                 target > animFocus[r][c] ? NV_SPRING_FOCUS : NV_SPRING_BLUR);
    }

  // Scrolls the MINIMUM for the focused row to fit whole in the usable area. With
  // the focus on the header the target is 0 — going back to the top is part of
  // going back to the bar.
  float target = scrollY;
  if (focus.row >= LIB_FILTER_GRID) {
    float top = NV_LIB_GRID_Y + (focus.row - LIB_FILTER_GRID) * stepLine();
    float base = top + heightLine();
    if (base - target > LIB_GRID_BASE)  target = base - LIB_GRID_BASE;
    if (top - target < NV_LIB_GRID_Y)  target = top - NV_LIB_GRID_Y;
  } else {
    target = 0.0f;
  }
  if (target < 0.0f) target = 0.0f;
  scrollY = anim_spring(scrollY, target, dt, NV_SPRING_SCROLL);
}

// A mode pill. Chosen but unfocused it gets background #303030 and a white
// border; focused it lightens and the text darkens. The two readings have to
// stay distinct — that is the mistake the detail screen's season tabs already made.
static void drawMode(int a, float f) {
  GfxRect r = { NV_LIB_X + a * NV_LIB_MODE_STEP, NV_LIB_MODE_Y,
                NV_LIB_MODE_W, NV_LIB_MODE_H };
  int sel = (a == mode);
  float radius = NV_RADIUS_PILL;
  if (f > 0.02f) {
    GfxRect b = { r.x - 2.0f, r.y - 2.0f, r.w + 4.0f, r.h + 4.0f };
    gfx_color(b, radius, 0.961f, 0.961f, 0.961f, f);
  }
  float luma = sel ? 0.188f : 0.133f;        // #303030 contra #222
  gfx_color(r, radius, luma, luma, luma, 1.0f);
  if (sel && f < 0.98f)
    gfx_rect(r, 0, GFX_RING, 0, 1.0f / r.h, 0, radius,
             0.70f, 0.70f, 0.72f, 1.0f - f);
  int color = 245;
  TxtLine l = txt_line(TXT_CAPTION2, ROT_MODE[a], color, color, color, 255);
  txt_draw_alpha(l, r.x + (r.w - l.w) * 0.5f, r.y + (r.h - l.h) * 0.5f,
                     sel ? 1.0f : 0.82f);
}

// The "Type" / "Sort" picker: a small grey label and a large white value, with
// the arrow up against the right edge.
static void drawPicker(int p, float f) {
  GfxRect r = { NV_LIB_X + p * NV_LIB_PICK_STEP, NV_LIB_PICK_Y,
                NV_LIB_PICK_W, NV_LIB_PICK_H };
  float radius = NV_LIB_PICK_RADIUS / (NV_LIB_PICK_H * 0.5f) * 0.5f;
  if (f > 0.02f) {
    GfxRect b = { r.x - 2.0f, r.y - 2.0f, r.w + 4.0f, r.h + 4.0f };
    gfx_color(b, radius, 0.961f, 0.961f, 0.961f, f);
  }
  float luma = anim_blend(0.133f, 0.188f, f);   // #222 -> #303030 no foco
  gfx_color(r, radius, luma, luma, luma, 1.0f);

  const char *rot = (p == 0) ? "Type" : "Sort";
  const char *val = (p == 0) ? ROT_KIND[kind] : ROT_ORDER[order];
  // 19/500 rgb(128,128,128) on top, 30/500 white below with 4 of slack.
  TxtLine tr = txt_line(TXT_MINI, rot, 179, 179, 179, 255);
  TxtLine tv = txt_line(TXT_CALLOUT, val, 255, 255, 255, 255);
  float tx = r.x + NV_LIB_PICK_PADX;
  float ty = r.y + NV_LIB_PICK_PADY;
  txt_draw_alpha(tr, tx, ty, 0.95f);
  txt_draw_alpha(tv, tx, ty + tr.h + 4.0f, 1.0f);

  TxtLine seta = txt_line(TXT_CAPTION2, "OK: change", 196, 197, 202, 255);
  txt_draw_alpha(seta, r.x + r.w - NV_LIB_PICK_PADX - seta.w,
                     r.y + (r.h - seta.h) * 0.5f, 0.85f);
}

// Empty state: 46/500 white and 28/400 rgb(179,179,179), centred in the usable
// width. A blank grid looks like a broken screen.
static void drawEmpty(void) {
  const char *l1 = totalMode ? "No titles under this filter"
      : mode == MODE_CLOUD ? "Your collection appears here" : "Your next session starts here";
  const char *l2 = totalMode
      ? "Under Type, choose All. Check the filters in Settings too."
      : mode == MODE_CLOUD
        ? "The films and series in your Trakt collection are gathered in this tab."
        : "Open a film or series and choose Add to list to keep it.";
  TxtLine t1 = txt_line(TXT_TITLE2, l1, 255, 255, 255, 255);
  TxtLine t2 = txt_line(TXT_CALLOUT, l2, 179, 179, 179, 255);
  float cx = NV_LIB_X + NV_LIB_W * 0.5f;
  float y = NV_LIB_EMPTY_Y + 190.0f;
  gfx_icon((GfxRect){cx - 32.0f, y - 100.0f, 64.0f, 64.0f},
             "menu_library", 0.70f, 0.70f, 0.72f, 1.0f);
  txt_draw_alpha(t1, cx - t1.w * 0.5f, y, 0.96f);
  txt_draw_alpha(t2, cx - t2.w * 0.5f, y + t1.h + 18.0f, 0.85f);
  TxtLine hint = txt_line(TXT_CAPTION2,
      "↑ Back to filters   ·   Back: menu", 179, 179, 179, 255);
  txt_draw(hint, cx - hint.w * 0.5f, y + t1.h + t2.h + 58.0f);
}

void library_draw(Uint32 now) {
  (void)now;
  // An opaque background of its own: the library covers the whole screen and
  // cannot rely on whoever drew before it.
  GfxRect screen = { 0, 0, NV_SCREEN_W, NV_SCREEN_H };
  // The screen has already been cleared with THIS VERY COLOUR by
  // glClearColor/glClear in main.c before app_draw. Painting over it was one
  // full-screen layer thrown away per frame — and the dominant cost on this GPU
  // is fill rate (gfx.c records that TWO full-screen layers dropped the Mali-G71
  // to ~40fps). Do not put it back without first changing the clear colour.
  (void)screen;

  TxtLine title = txt_line(TXT_TITLE2, "Library", 255, 255, 255, 255);
  txt_draw(title, NV_LIB_X, NV_LIB_Y);
  // The source badge, aligned to the right of the usable area. Deliberately
  // tracked out: in the web app it has letter-spacing 4 and reads as a label,
  // not as a word.
  //
  // THE BADGE STATES THE REAL SOURCE. It used to be hard-coded to "NUVIO", which
  // is the app's name and not where the data comes from — the list comes from
  // TRAKT, and the log confirms it ("[trakt] credential loaded", "[trakt]
  // watchlist: 118"). A source badge that does not reflect the source belongs to
  // the same family as the hard-coded "14" age rating and the demo cast:
  // invented information wearing the face of data.
  //
  // Without a Trakt credential the library is local, and the badge says so.
  { const char *source = trakt_active() ? "TRAKT" : "LOCAL";
    float wBadge = txt_tracking(TXT_CALLOUT, source, 128, 128, 128,
                               -1.0f, 0.0f, 0.0f, 4.0f);
    txt_tracking(TXT_CALLOUT, source, 128, 128, 128,
                 NV_LIB_DIR - wBadge, NV_LIB_Y + 10.0f, 0.9f, 4.0f); }

  for (int a = 0; a < LIB_N_MODES; a++) drawMode(a, animMode[a]);
  {
    char summary[160];
    snprintf(summary, sizeof summary, "%d %s   ·   %s", nFilter,
             nFilter == 1 ? "title" : "titles",
             mode == MODE_SAVED ? "Your watchlist" : "Your Trakt collection");
    TxtLine info = txt_line(TXT_CAPTION2, summary, 179, 179, 179, 255);
    txt_draw(info, NV_LIB_DIR - info.w,
                 NV_LIB_MODE_Y + (NV_LIB_MODE_H - info.h) * 0.5f);
  }
  for (int p = 0; p < 2; p++)           drawPicker(p, animPick[p]);

  if (nFilter == 0) { drawEmpty(); return; }

  int lines = nLines();
  if (lines > LIB_MAX_LINES) lines = LIB_MAX_LINES;
  float stepC = stepColumn(), stepL = stepLine();

  // Two passes: the focused item scales by 2% and has to be drawn LAST,
  // otherwise its right-hand neighbour clips its border.
  for (int passe = 0; passe < 2; passe++)
    for (int r = 0; r < lines; r++) {
      float top = NV_LIB_GRID_Y + r * stepL - scrollY;
      if (top > NV_SCREEN_H || top + heightLine() < -80.0f) continue;
      // Whatever rises under the header fades out before crossing it: without
      // the fade, poster and picker read one on top of the other.
      float a = anim_clamp((top - (NV_LIB_PICK_Y + NV_LIB_PICK_H * 0.5f)) / LIB_FADE,
                           0.0f, 1.0f);
      if (a <= 0.005f) continue;

      for (int c = 0; c < NV_LIB_COLUMNS; c++) {
        int i = r * NV_LIB_COLUMNS + c;
        if (i >= nFilter) break;
        float f = animFocus[r][c];
        if ((passe == 0) == (f > 0.01f)) continue;

        // `.library-grid-card.focused { transform: scale(1.02) }` with the
        // origin at the TOP — it is the only focus scale the web app has, and it
        // is 2%, not the 14% that used to be here (a number from the tvOS Top
        // Shelf tables).
        float scale = 1.0f + NV_LIB_FOCUS_SCALE * f;
        float bw = NV_LIB_CARD_W * scale, bh = NV_LIB_POSTER_H * scale;
        float bx = NV_LIB_X + c * stepC - (bw - NV_LIB_CARD_W) * 0.5f;
        GfxRect card = { bx, top, bw, bh };
        // The SDF's radius is a fraction of the HEIGHT, not of the smaller side:
        // `p = (uv-0.5)*vec2(asp,1.0)` makes one SDF unit h pixels on both axes.
        // Dividing by the width rounded this poster half again too much. See the
        // note on radiusInset in home.c.
        float radius = 24.0f / NV_LIB_POSTER_H;

        const CatItem *ci = cat_item(filter[i]);
        const char *art = (ci && ci->poster[0]) ? ci->poster : NULL;
        GLuint tex = art ? tex_get(art) : 0;
        if (tex) {
          gfx_tex_aspect_current = tex_aspect(art);
          gfx_rect(card, tex, GFX_CARD, f, 0.0f, 0.0f, radius, 0, 0, 0, a);
          gfx_tex_aspect_current = 0.0f;
        } else {
          // A placeholder in the cards' colour: the poster is still decoding on
          // another thread and the grid must not flash a hole.
          // A VISIBLE skeleton, the same as the home's: #2C2C2C. See the note
          // there — a placeholder in the background's tone reads as a broken
          // card, not as loading.
            gfx_color(card, radius, NV_COLOR_SKELETON_R, NV_COLOR_SKELETON_G,
                  NV_COLOR_SKELETON_B, a);
        }
        // The web app's focus border is 4px ON THE INSIDE of the poster (the card
        // already reserves `border: 4px solid transparent`), and not a halo on
        // the outside — "Android TV uses the inside focus border, not an outer
        // halo", says the stylesheet's own comment.
        if (f > 0.01f) {
          gfx_rect(card, 0, GFX_RING, 0, NV_LIB_POSTER_BORDER / card.w,
                   0, radius, 0.961f, 0.961f, 0.961f, f * a);
        }

        // Title 32/500, one line, cut with an ellipsis — the web app uses
        // white-space:nowrap + text-overflow:ellipsis.
        if (ci) {
          TxtLine tl = txt_line_trim(TXT_CALLOUT, ci->title, 255, 255, 255, 255,
                                        NV_LIB_CARD_W);
          txt_draw_alpha(tl, NV_LIB_X + c * stepC,
                             top + NV_LIB_POSTER_H + NV_LIB_TITLE_GAP, a * 0.98f);
        }
      }
    }
}
