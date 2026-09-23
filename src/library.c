// The Library: the owner's own titles, in Discover's page.
//
// ------------------------------------------------------------------------
// THE LAYOUT, AS OF 2026-09-23
//
//   "Library"            the page title where Discover's sits, with the source
//                        and the count right-aligned against it
//   Saved   Collection  the player sheets' tab strip at page size (tab_page_draw):
//                        words, and a rule under the chosen one, lit while the
//                        cursor is on it — not the two pills that were here
//   [Type v] [Sort v]    the title page's dropdowns (dd_select + dd_menu), sized
//                        to their widest option like the season picker
//   the grid             Discover's: its card, its gaps, its 1.05 focus from the
//                        top edge, its row-snapped scroll and its dissolve under
//                        the header
//
// The strip is a switch between two SETS, not a filter on one, which is why it
// is words above the controls rather than one more control beside them. The
// dropdowns filter and order whichever set is on screen.
//
// THE FOCUS MOVES LIKE DISCOVER'S. Three zones, top to bottom; Back climbs one
// zone at a time and only leaves from the strip; LEFT off the left edge of any
// zone calls up the menu; an open dropdown owns the whole D-pad.
#include "library.h"
#include "trakt.h"
#include "gfx.h"
#include "text.h"
#include "tex_cache.h"
#include "anim.h"
#include "layout.h"
#include "settings.h"
#include "catalog.h"
#include "dropdown.h"
#include "tabs.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

// "Saved" is Trakt's watchlist; "Collection" is Trakt's collection.
enum { MODE_SAVED, MODE_COLLECTION, LIB_N_MODES };
static const char *MODE_LABEL[LIB_N_MODES] = { "Saved", "Collection" };

// The dropdowns carry no label over the value, so each value says what it is.
enum { KIND_ALL, KIND_MOVIE, KIND_SERIES, LIB_N_KINDS };
static const char *KIND_LABEL[LIB_N_KINDS] = { "All types", "Movies", "Series" };
// "Added" is when the title went onto the Trakt list; "release" is its year.
enum { ORDER_RECENT, ORDER_FIRST, ORDER_NEWEST, ORDER_OLDEST, ORDER_TITLE,
       LIB_N_ORDER };
static const char *ORDER_LABEL[LIB_N_ORDER] = {
  "Recently added", "First added", "Newest releases", "Oldest releases",
  "Title A\xe2\x80\x93Z" };

enum { PICK_KIND, PICK_ORDER, PICK_N };
enum { ZONE_TABS, ZONE_PICKERS, ZONE_GRID };

static int mode = MODE_SAVED;
static int kind = KIND_ALL;
static int order = ORDER_RECENT;
static int zone;
static int pickSel;
// The open dropdown and the row inside it, which is NOT the value until OK —
// the split Discover and the season picker use.
static int menuOpen = -1;
static int menuFocus;
static int focus;                // index into filter[]

static int filter[CAT_MAX];      // visible catalogue indices
static int nFilter = 0;
static int totalMode = 0;
static float animPick[PICK_N];
static float animCard;           // one spring: only ever one card is focused
static float animTabs;           // the cursor on the Saved / Collection strip
static float scrollY;
static int wantsExit = 0, request = -1, requestMenu = 0;
static HomeItem itemFocus;
static int hasItemFocus;

// Account state. The real list is Trakt's, which marks ci->inList/inCollection
// ON THE ITEM — there is no per-index table here any more: the catalogue is
// rebuilt from the network and a stored index points at a different title on the
// next round. `bought` survives because there is no source for it yet.
static char bought[CAT_MAX];

static int isSeries(const CatItem *ci) {
  return ci && (!strcmp(ci->kind, "series") || ci->nSeasons > 0
                || ci->season > 0);
}

// The release year: Trakt's own for a list item, else the year `meta` opens with
// ("2022 · 3 seasons"). 0 when neither says.
static int yearOf(const CatItem *ci) {
  if (ci->year > 0) return ci->year;
  if (ci->meta[0] >= '0' && ci->meta[0] <= '9') return atoi(ci->meta);
  return 0;
}

// 1 when `a` belongs AFTER `b` in the chosen order. A title with no date or no
// year goes to the end in either direction: it has no place in the order, and
// putting it first would bury the ones that do.
static int after(const CatItem *a, const CatItem *b) {
  if (!a || !b) return 0;
  switch (order) {
    case ORDER_RECENT: case ORDER_FIRST: {
      long long x = a->added, y = b->added;
      if (!x || !y) return !x && y;
      return order == ORDER_RECENT ? x < y : x > y;
    }
    case ORDER_NEWEST: case ORDER_OLDEST: {
      int x = yearOf(a), y = yearOf(b);
      if (!x || !y) return !x && y;
      return order == ORDER_NEWEST ? x < y : x > y;
    }
    default: return strcasecmp(a->title, b->title) > 0;
  }
}

// Rebuilds the visible list. Called on every change of mode, type or order, and
// it puts the grid's focus back on the first poster: the old index points at a
// different title in the new list.
static void rebuild(void) {
  int n = cat_n();
  if (n > CAT_MAX) n = CAT_MAX;
  nFilter = 0;
  totalMode = 0;
  for (int i = 0; i < n; i++) {
    const CatItem *ci = cat_item(i);
    if (!ci) continue;
    // Trakt's own split: the watchlist (what you intend to watch) against the
    // collection (what you own). Read from the ITEM — see `bought` above.
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

  // Insertion sort — there are a few dozen items, once per change. Stable, so
  // ties keep Trakt's own order.
  for (int i = 1; i < nFilter; i++) {
    int v = filter[i], j = i - 1;
    while (j >= 0 && after(cat_item(filter[j]), cat_item(v))) {
      filter[j + 1] = filter[j]; j--;
    }
    filter[j + 1] = v;
  }

  focus = 0;
  animCard = 0.0f;
  // An empty grid cannot hold the focus.
  if (!nFilter && zone == ZONE_GRID) zone = ZONE_PICKERS;
}

static int started;
int library_start(void) {
  // Zeroed ONCE per process, and not on every entry.
  if (!started) {
    memset(bought, 0, sizeof bought);
    started = 1;
  }
  mode = MODE_SAVED; kind = KIND_ALL; order = ORDER_RECENT;
  zone = ZONE_TABS; pickSel = 0; menuOpen = -1;
  memset(animPick, 0, sizeof animPick);
  animTabs = 1.0f;   // born on the strip, already lit
  scrollY = 0.0f;
  wantsExit = 0; request = -1;
  hasItemFocus = 0;
  rebuild();
  return 1;
}

void library_shutdown(void) { hasItemFocus = 0; }

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
int library_requested_menu(void) { int v = requestMenu; requestMenu = 0; return v; }

int library_requested_open(int *indexCatalog) {
  if (request < 0) return 0;
  if (indexCatalog) *indexCatalog = request;
  request = -1;
  return 1;
}

int library_item_focused(HomeItem *out) {
  if (!hasItemFocus || !out) return 0;
  *out = itemFocus;
  return 1;
}

// What option `i` of picker `p` reads as — the dropdown's label callback.
static const char *optionLabel(void *ctx, int i) {
  int p = *(int *)ctx;
  if (p == PICK_KIND)  return (i >= 0 && i < LIB_N_KINDS) ? KIND_LABEL[i] : "";
  return (i >= 0 && i < LIB_N_ORDER) ? ORDER_LABEL[i] : "";
}
static int optionsN(int p) { return p == PICK_KIND ? LIB_N_KINDS : LIB_N_ORDER; }
static int optionCurrent(int p) { return p == PICK_KIND ? kind : order; }

static int isOk(SDL_Keycode k) {
  return k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE;
}

void library_event(const SDL_Event *e) {
  if (e->type != SDL_KEYDOWN) return;
  SDL_Keycode k = e->key.keysym.sym;
  int back = k == SDLK_ESCAPE || k == SDLK_AC_BACK || k == SDLK_BACKSPACE ||
             k == SDLK_DELETE;

  // AN OPEN LIST OWNS THE WHOLE D-PAD, as on Discover.
  if (menuOpen >= 0) {
    int count = optionsN(menuOpen);
    if (k == SDLK_UP && menuFocus > 0) menuFocus--;
    else if (k == SDLK_DOWN && menuFocus + 1 < count) menuFocus++;
    else if (back || k == SDLK_LEFT || k == SDLK_RIGHT) menuOpen = -1;
    else if (isOk(k)) {
      int p = menuOpen;
      menuOpen = -1;
      if (menuFocus != optionCurrent(p)) {
        if (p == PICK_KIND) kind = menuFocus; else order = menuFocus;
        rebuild();
      }
    }
    return;
  }

  if (back) {
    // Back climbs to the strip, one zone at a time, and only leaves from there.
    if (zone != ZONE_TABS) zone--;
    else wantsExit = 1;
    return;
  }

  if (zone == ZONE_TABS) {
    // Left/right SWAP the set, like the sources sheet's strip; LEFT past the
    // first word calls up the menu.
    if (k == SDLK_RIGHT && mode < LIB_N_MODES - 1) { mode++; rebuild(); }
    else if (k == SDLK_LEFT) {
      if (mode > 0) { mode--; rebuild(); } else requestMenu = 1;
    }
    else if (k == SDLK_DOWN || isOk(k)) zone = ZONE_PICKERS;
    return;
  }

  if (zone == ZONE_PICKERS) {
    if (k == SDLK_LEFT) { if (pickSel > 0) pickSel--; else requestMenu = 1; }
    else if (k == SDLK_RIGHT) { if (pickSel < PICK_N - 1) pickSel++; }
    else if (k == SDLK_UP) zone = ZONE_TABS;
    else if (k == SDLK_DOWN) { if (nFilter) { zone = ZONE_GRID; focus = 0; } }
    else if (isOk(k)) {
      // The list opens ON the current value.
      menuOpen = pickSel;
      menuFocus = optionCurrent(pickSel);
    }
    return;
  }

  // THE GRID, moved exactly as Discover's.
  int row = focus / NV_DSC_COLUMNS;
  int lastRow = nFilter > 0 ? (nFilter - 1) / NV_DSC_COLUMNS : 0;
  switch (k) {
    case SDLK_LEFT:
      if (focus % NV_DSC_COLUMNS) focus--;
      else requestMenu = 1;
      break;
    case SDLK_RIGHT:
      if (focus + 1 < nFilter && (focus + 1) % NV_DSC_COLUMNS) focus++;
      break;
    case SDLK_UP:
      if (row == 0) zone = ZONE_PICKERS;
      else focus -= NV_DSC_COLUMNS;
      break;
    case SDLK_DOWN:
      if (row < lastRow) {
        focus += NV_DSC_COLUMNS;
        if (focus >= nFilter) focus = nFilter - 1;
      }
      break;
    case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE:
      if (focus >= 0 && focus < nFilter) request = filter[focus];
      break;
    default: break;
  }
}

void library_update(float dt, Uint32 now) {
  (void)now;
  for (int p = 0; p < PICK_N; p++) {
    float target = ((zone == ZONE_PICKERS && pickSel == p) || menuOpen == p)
                   ? 1.0f : 0.0f;
    animPick[p] = anim_spring(animPick[p], target, dt,
                              target > animPick[p] ? NV_SPRING_FOCUS : NV_SPRING_BLUR);
  }
  { float target = (zone == ZONE_TABS) ? 1.0f : 0.0f;
    animTabs = anim_spring(animTabs, target, dt,
                           target > animTabs ? NV_SPRING_FOCUS : NV_SPRING_BLUR); }
  { float target = (zone == ZONE_GRID) ? 1.0f : 0.0f;
    animCard = anim_spring(animCard, target, dt,
                           target > animCard ? NV_SPRING_FOCUS : NV_SPRING_BLUR); }

  // THE SCROLL SNAPS TO THE FOCUSED ROW — Discover's rule, for Discover's reason:
  // one row of cards fits under the header, so the focused one goes to the top
  // and nothing is ever sliced there. See dui_update.
  float target = (zone == ZONE_GRID && nFilter > 0)
                 ? (float)(focus / NV_DSC_COLUMNS) * NV_DSC_LINE_STEP : 0.0f;
  scrollY = anim_spring(scrollY, target, dt, NV_SPRING_GRID);
}

// --- Drawing -----------------------------------------------------------------
static GfxRect pickRect(int p) {
  int kindCtx = PICK_KIND, orderCtx = PICK_ORDER;
  float wKind = dd_select_width(LIB_N_KINDS, optionLabel, &kindCtx);
  float wOrder = dd_select_width(LIB_N_ORDER, optionLabel, &orderCtx);
  if (p == PICK_KIND) return (GfxRect){ NV_DSC_X, NV_LIB_SEL_Y, wKind, NV_DD_SEL_H };
  return (GfxRect){ NV_DSC_X + wKind + NV_LIB_SEL_GAP, NV_LIB_SEL_Y, wOrder,
                    NV_DD_SEL_H };
}

// Empty state, centred under the header. A blank grid looks like a broken screen.
static void drawEmpty(void) {
  const char *l1 = totalMode ? "No titles under this filter"
      : mode == MODE_COLLECTION ? "Your collection appears here"
                                : "Your next session starts here";
  const char *l2 = totalMode
      ? "Choose All types, or check the filters in Settings."
      : mode == MODE_COLLECTION
        ? "The movies and series in your Trakt collection are gathered here."
        : "Open a movie or series and choose Add to list to keep it.";
  TxtLine t1 = txt_line(TXT_TITLE2, l1, 255, 255, 255, 255);
  TxtLine t2 = txt_line(TXT_SRCH_EMPTY, l2, 179, 179, 179, 255);
  float cx = NV_DSC_X + NV_DSC_W * 0.5f;
  float y  = NV_LIB_GRID_Y + 150.0f;
  gfx_icon((GfxRect){ cx - 32.0f, y - 100.0f, 64.0f, 64.0f },
           "menu_library", 0.70f, 0.70f, 0.72f, 1.0f);
  txt_draw_alpha(t1, cx - (float)t1.w * 0.5f, y, 0.96f);
  txt_draw_alpha(t2, cx - (float)t2.w * 0.5f, y + (float)t1.h + 18.0f, 0.85f);
}

// Discover's grid, card for card — see drawGrid in discoverui.c for why each
// piece is the way it is. Only the data source differs.
static void drawGrid(void) {
  hasItemFocus = 0;
  if (!nFilter) { drawEmpty(); return; }

  gfx_crop(0.0f, NV_LIB_CLIP_TOP, NV_SCREEN_W, NV_DSC_GRID_BOTTOM - NV_LIB_CLIP_TOP);
  // TWO PASSES: the focused card scales up and must sit over its neighbours.
  for (int pass = 0; pass < 2; pass++)
    for (int i = 0; i < nFilter; i++) {
      int isFocus = (zone == ZONE_GRID && i == focus);
      float f = isFocus ? animCard : 0.0f;
      float top = NV_LIB_GRID_Y + (float)(i / NV_DSC_COLUMNS) * NV_DSC_LINE_STEP - scrollY;
      float left = NV_DSC_X + (float)(i % NV_DSC_COLUMNS) * NV_DSC_CARD_STEP;
      float edge = anim_edge(top, NV_LIB_CLIP_TOP, NV_LIB_FADE);
      const CatItem *ci;
      if ((pass == 1) != (f > 0.01f)) continue;
      if (top > NV_SCREEN_H + 40.0f || top + NV_DSC_POSTER_H < -40.0f) continue;
      if (edge <= 0.004f && !isFocus) continue;
      if (!(ci = cat_item(filter[i]))) continue;
      gfx_opacity_group = edge;

      { float scale = anim_blend(1.0f, 1.0f + NV_DSC_FOCUS_SCALE, f);
        float w = NV_DSC_CARD_W * scale, h = NV_DSC_POSTER_H * scale;
        // `transform-origin: top`: only x is re-centred.
        GfxRect card = { left - (w - NV_DSC_CARD_W) * 0.5f, top, w, h };
        float radius = NV_DSC_POSTER_R / card.h;
        const char *art = ci->poster[0] ? ci->poster
                        : (ci->backdrop[0] ? ci->backdrop : NULL);
        // The RESTING width, so the focus spring does not re-decode the poster.
        GLuint tex = art ? tex_get_width(art, NV_DSC_CARD_W) : 0;
        if (tex) {
          gfx_tex_aspect_current = tex_aspect(art);
          gfx_rect(card, tex, GFX_CARD, f, 0.0f, 0.0f, radius, 0, 0, 0, 1);
          gfx_tex_aspect_current = 0.0f;
        } else {
          gfx_skeleton(card, radius, NV_COLOR_SKELETON_R, NV_COLOR_SKELETON_G,
                       NV_COLOR_SKELETON_B, 1.0f);
        }
        if (f > 0.01f)
          gfx_rect(card, 0, GFX_RING_INSET, 0, NV_DSC_BORDER / card.h, 0,
                   radius, 0.961f, 0.961f, 0.961f, f);

        if (settings_labels_poster()) {
          TxtLine tl = txt_line_trim(TXT_CALLOUT, ci->title, 255, 255, 255, 255,
                                     NV_DSC_CARD_W);
          txt_draw_alpha(tl, card.x, top + h + NV_DSC_TITLE_GAP, 0.98f);
        }
        gfx_opacity_group = 1.0f;

        if (isFocus) {
          itemFocus.index_ = filter[i];
          itemFocus.rect   = card;
          itemFocus.art    = ci->backdrop[0] ? ci->backdrop : ci->poster;
          itemFocus.title  = ci->title;
          itemFocus.genre  = ci->genre;
          itemFocus.meta   = ci->meta;
          hasItemFocus = 1;
        }
      }
    }
  gfx_opacity_group = 1.0f;
  gfx_no_crop();
}

void library_draw(Uint32 now) {
  (void)now;
  // No background fill: main.c has already cleared to #0d0d0d, and a
  // full-screen layer thrown away per frame is the dominant cost on this GPU.
  txt_tracking(TXT_TITLE3, "Library", 255, 255, 255,
               NV_DSC_X, NV_DSC_Y, 1.0f, NV_DSC_TITLE_LS);
  // The context line, right-aligned against the title as on Discover. It names
  // the REAL source — the list comes from Trakt, and without a Trakt credential
  // it is local — and how many titles are on screen.
  { char line[96];
    snprintf(line, sizeof line, "%s  \xc2\xb7  %d %s",
             trakt_active() ? "Trakt" : "Local", nFilter,
             nFilter == 1 ? "title" : "titles");
    float w = txt_tracking(TXT_SRCH_NAME, line, 128, 128, 128, -1.0f, 0.0f, 0.0f, 4.0f);
    txt_tracking(TXT_SRCH_NAME, line, 128, 128, 128,
                 NV_DSC_X + NV_DSC_W - w, NV_DSC_Y + 10.0f, 0.95f, 4.0f); }

  { float x = NV_DSC_X;
    for (int m = 0; m < LIB_N_MODES; m++)
      x += tab_page_draw(x, NV_LIB_TABS_Y, MODE_LABEL[m], m == mode, animTabs, 1.0f); }

  dd_select(pickRect(PICK_KIND), KIND_LABEL[kind], NULL, animPick[PICK_KIND], 1.0f);
  dd_select(pickRect(PICK_ORDER), ORDER_LABEL[order], NULL, animPick[PICK_ORDER], 1.0f);

  drawGrid();
  // LAST, over everything: an open list covers the grid under it.
  if (menuOpen >= 0)
    dd_menu(pickRect(menuOpen), optionsN(menuOpen), menuFocus, optionLabel,
            &menuOpen, 1.0f);
}
