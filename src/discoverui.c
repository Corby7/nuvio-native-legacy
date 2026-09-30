#include "pointer.h"
#include "app.h"
#include "discoverui.h"
#include "gfx.h"
#include "text.h"
#include "tex_cache.h"
#include "anim.h"
#include "layout.h"
#include "gridsize.h"
#include "catalog.h"
#include "discover.h"
#include "settings.h"
#include "dropdown.h"
#include "hold.h"
#include "ctxmenu.h"
#include <string.h>
#include <stdio.h>

// The three pickers, left to right, and the two places the focus can be.
enum { PICK_TYPE, PICK_CATALOG, PICK_GENRE, PICK_N };
// ZONE_HEAD is the grid-size button on the title row, above the pickers.
enum { ZONE_HEAD, ZONE_PICKERS, ZONE_GRID };

// "movie" and "series" are the two the rest of the app has screens for — the
// same pair the manifest sweep restricts search to, and for the same reason.
static const char *const KINDS[]  = { "movie", "series" };
static const char *const KIND_LABEL[] = { "Movie", "Series" };
#define KIND_N 2

// How many catalogues the Catalog picker will offer. See the divergence note in
// discoverui.h: these are the rows the home already built, not every catalogue
// the addons declare.
#define DUI_CATS 48

static int   kindSel;
static int   catSel;
static int   genreSel;         // 0 = Default (no genre parameter at all)
static int   pickSel;
// The OPEN dropdown. `menuOpen` is the picker whose list is expanded (-1 when
// none) and `menuFocus` the row inside it — which is NOT the picker's current
// value: the list opens ON that value, and moving inside it must not change
// anything until OK. The same split the detail screen's season picker uses.
static int   menuOpen = -1;
static int   menuFocus;
static int   zone;
static int   focus;            // index into the grid
static int   wantsExit;
static int   requestMenu;   // LEFT off the screen's left edge
static int   request = -1;
static float scrollY, scrollTarget, scrollV;
static ScrollBar bar;
// Whether the grid's scroll follows the focus. A card the Magic Remote's pointer
// focused leaves it where it is: snapping that card's row to the top would slide
// another under a pointer that had not moved. Any arrow, the wheel's included,
// hands it back.
static int   follow = 1;
static float animPick[PICK_N];
static float animCard;         // one spring: only ever one card is focused
static float animHead;         // the grid-size button
static HomeItem itemFocus;
// Holding OK on a card opens the poster menu (hold.h).
static Hold hold;
static int   hasItemFocus;

// The catalogues matching the chosen type, as indices into cat_row().
static int   cats[DUI_CATS];
static int   nCats;

static int gridN(void) { return disc_seeall_n(); }

static const CatRow *currentCat(void) {
  if (catSel < 0 || catSel >= nCats) return NULL;
  return cat_row(cats[catSel]);
}

static int genreN(void) {
  const CatRow *r = currentCat();
  if (!r) return 0;
  return disc_genres_n(r->base, r->kind, r->catId);
}

// The label the Genre picker shows. Index 0 is "Default", which is not a genre
// but the ABSENCE of one: the catalogue is asked without the parameter, and
// that is a different request from any genre it lists.
static const char *genreLabel(int i) {
  const CatRow *r = currentCat();
  if (i <= 0 || !r) return "Default";
  return disc_genre_at(r->base, r->kind, r->catId, i - 1);
}

// How many options each picker offers, and what option `i` of it reads as. One
// pair of functions rather than three, because the dropdown draws and navigates
// all three the same way and only the contents differ.
static int optionsN(int p) {
  switch (p) {
    case PICK_TYPE:    return KIND_N;
    case PICK_CATALOG: return nCats;
    case PICK_GENRE:   return genreN() + 1;   // +1 for Default
    default: return 0;
  }
}

static const char *optionLabel(int p, int i) {
  switch (p) {
    case PICK_TYPE:
      return (i >= 0 && i < KIND_N) ? KIND_LABEL[i] : "";
    case PICK_CATALOG: {
      const CatRow *row = (i >= 0 && i < nCats) ? cat_row(cats[i]) : NULL;
      return row ? row->title : "";
    }
    case PICK_GENRE:   return genreLabel(i);
    default: return "";
  }
}

static int optionCurrent(int p) {
  switch (p) {
    case PICK_TYPE:    return kindSel;
    case PICK_CATALOG: return catSel;
    case PICK_GENRE:   return genreSel;
    default: return 0;
  }
}

// Rebuilds the catalogue list for the chosen type and fires the fetch. Called
// whenever any picker moves, because all three feed one request.
static void rebuild(int keepCatalogue) {
  const CatRow *r;
  int n = cat_n_rows(), i;
  char wanted[96] = "";
  if (keepCatalogue && (r = currentCat()) != NULL)
    snprintf(wanted, sizeof wanted, "%s", r->catId);

  nCats = 0;
  for (i = 0; i < n && nCats < DUI_CATS; i++) {
    const CatRow *row = cat_row(i);
    if (!row) break;
    // A row with no address cannot be paged — it came from the packaged
    // catalogue, not from an addon — so it would be an option that opens an
    // empty grid. See the note in discoverui.h about options that do nothing.
    if (!row->base[0] || !row->catId[0]) continue;
    if (strcmp(row->kind, KINDS[kindSel])) continue;
    cats[nCats++] = i;
  }
  // Keep the owner on the same catalogue across a type change where that makes
  // sense; otherwise start at the first.
  catSel = 0;
  if (wanted[0])
    for (i = 0; i < nCats; i++) {
      const CatRow *row = cat_row(cats[i]);
      if (row && !strcmp(row->catId, wanted)) { catSel = i; break; }
    }
  if (genreSel > genreN()) genreSel = 0;

  focus = 0;
  scrollY = scrollTarget = scrollV = 0.0f;
  animCard = 0.0f;
  r = currentCat();
  if (r) disc_seeall_filter(r->base, r->kind, r->catId,
                            genreSel > 0 ? genreLabel(genreSel) : "");
}

int dui_start(void) {
  kindSel = genreSel = pickSel = 0;
  zone = ZONE_PICKERS;
  wantsExit = 0; request = -1;
  hasItemFocus = 0;
  memset(animPick, 0, sizeof animPick);
  rebuild(0);
  // Nothing to browse. The caller must not switch to a screen that can only
  // show an empty grid and a picker with no options in it.
  if (!nCats) {
    // Try the other type before giving up: a fresh install whose addons serve
    // only series would otherwise be told there is nothing at all.
    kindSel = 1;
    rebuild(0);
    if (!nCats) { kindSel = 0; return 0; }
  }
  return 1;
}

void dui_shutdown(void) { hasItemFocus = 0; }
int  dui_wants_exit(void) { return wantsExit; }
int  dui_requested_menu(void) { int v = requestMenu; requestMenu = 0; return v; }

int dui_requested_open(int *indexCatalog) {
  if (request < 0) return 0;
  if (indexCatalog) *indexCatalog = request;
  request = -1;
  return 1;
}

int dui_item_focused(HomeItem *out) {
  if (!hasItemFocus || !out) return 0;
  *out = itemFocus;
  return 1;
}

// Commits option `i` of picker `p`. The dropdown's OK lands here; nothing else
// changes a picker's value, so there is one place where a choice takes effect
// and one place that decides what a choice invalidates.
static void chooseOption(int p, int i) {
  switch (p) {
    case PICK_TYPE:
      if (i == kindSel) return;
      kindSel = i;
      // The genres belong to the CATALOGUE, and changing type changes which
      // catalogue is selected, so the old genre means nothing against the new
      // one. Carrying it over would send a genre the catalogue has never heard
      // of and get an empty grid back.
      genreSel = 0;
      rebuild(1);
      break;
    case PICK_CATALOG:
      if (i == catSel) return;
      catSel = i;
      genreSel = 0;
      // Not rebuild(1): the catalogue was just chosen BY HAND, and "keep the
      // one you were on" would undo the choice.
      { const CatRow *r = currentCat();
        focus = 0; scrollY = scrollTarget = scrollV = 0.0f; animCard = 0.0f;
        if (r) disc_seeall_filter(r->base, r->kind, r->catId, ""); }
      break;
    case PICK_GENRE:
      if (i == genreSel) return;
      genreSel = i;
      rebuild(1);
      break;
    default: break;
  }
}

// The focused card as a catalogue index. The item is NOT in the global catalogue
// — it came from a page only this screen read — so it goes in through cat_append
// and the detail and the hold menu open it by index, which is how the whole app
// works. Same as seeall.c. -1 when there is no card.
static int focusedIndex(void) {
  CatItem it;
  int idx;
  if (focus < 0 || focus >= gridN() || !disc_seeall_item(focus, &it)) return -1;
  idx = it.imdb[0] ? cat_index_by_imdb(it.imdb) : -1;
  if (idx < 0) idx = cat_append(&it);
  return idx;
}

// The pointer's setters, each doing what the keys that reach the same place do.
static void pointHead(int a, int b) {
  (void)a; (void)b;
  if (menuOpen < 0) zone = ZONE_HEAD;
}
static void pointPick(int p, int unused) {
  (void)unused;
  if (menuOpen < 0) { zone = ZONE_PICKERS; pickSel = p; }
}
static void pointOption(int c, int unused) {
  (void)unused;
  if (menuOpen >= 0 && c >= 0 && c < optionsN(menuOpen)) menuFocus = c;
}
static void pointOffMenu(int a, int b) { (void)a; (void)b; menuOpen = -1; }
static void pointCard(int i, int unused) {
  (void)unused;
  if (menuOpen >= 0 || i < 0 || i >= gridN()) return;
  zone = ZONE_GRID; focus = i; follow = 0;
}

// THE WALL: the focused card leaning towards a press that had nowhere to go (anim.h).
static AnimBump nudge[2];

void dui_event(const SDL_Event *e) {
  int n, row, lastRow;
  if (e->type == SDL_QUIT) { wantsExit = 1; return; }
  // OK over a card is a tap or a hold, and only the release can tell which.
  { int tap;
    if (hold_event(&hold, e, zone == ZONE_GRID && menuOpen < 0 &&
                             focus >= 0 && focus < gridN(), &tap)) {
      if (tap) { int idx = focusedIndex(); if (idx >= 0) request = idx; }
      return;
    } }
  if (e->type != SDL_KEYDOWN) return;
  SDL_Keycode k = e->key.keysym.sym;
  if (k == SDLK_UP || k == SDLK_DOWN || k == SDLK_LEFT || k == SDLK_RIGHT) follow = 1;
  n = gridN();

  // AN OPEN LIST OWNS THE WHOLE D-PAD. It is a question standing in front of the
  // screen, and nothing behind it should answer a key — the same rule the source
  // sheet and the season picker already follow.
  if (menuOpen >= 0) {
    int count = optionsN(menuOpen);
    switch (k) {
      case SDLK_UP:    if (menuFocus > 0) menuFocus--; break;
      case SDLK_DOWN:  if (menuFocus + 1 < count) menuFocus++; break;
      case SDLK_AC_BACK: case SDLK_ESCAPE: case SDLK_BACKSPACE:
      case SDLK_LEFT: case SDLK_RIGHT:
        menuOpen = -1;
        break;
      case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: {
        int p = menuOpen;
        menuOpen = -1;
        chooseOption(p, menuFocus);
        break;
      }
      default: break;
    }
    return;
  }

  if (k == SDLK_AC_BACK || k == SDLK_ESCAPE || k == SDLK_BACKSPACE) {
    // From the grid, Back climbs to the pickers — the reverse of the move that
    // got there. Only from the pickers does it leave the screen.
    if (zone == ZONE_GRID) { zone = ZONE_PICKERS; return; }
    wantsExit = 1;
    return;
  }

  if (zone == ZONE_HEAD) {
    // OK steps the size; the focused title keeps its index, so the grid
    // re-flows around it and the scroll follows its new row.
    if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) grid_cycle();
    else if (k == SDLK_DOWN) zone = ZONE_PICKERS;
    else if (k == SDLK_LEFT) requestMenu = 1;
    return;
  }

  if (zone == ZONE_PICKERS) {
    switch (k) {
      case SDLK_UP:    zone = ZONE_HEAD; break;
      case SDLK_LEFT:  if (pickSel > 0) pickSel--; else requestMenu = 1; break;
      case SDLK_RIGHT: if (pickSel < PICK_N - 1) pickSel++; break;
      case SDLK_DOWN:  if (n > 0) { zone = ZONE_GRID; focus = 0; } break;
      case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE:
        // The list opens ON the current value, which is how the owner sees
        // which one they are on without a tick beside it.
        if (optionsN(pickSel) > 1) {
          menuOpen = pickSel;
          menuFocus = optionCurrent(pickSel);
        }
        break;
      default: break;
    }
    return;
  }

  row     = focus / grid_cols();
  lastRow = n > 0 ? (n - 1) / grid_cols() : 0;
  switch (k) {
    case SDLK_LEFT:
      if (focus % grid_cols()) focus--;
      else requestMenu = 1;
      break;
    case SDLK_RIGHT: {
      int was = focus;
      if (focus + 1 < n && (focus + 1) % grid_cols()) focus++;
      anim_nudge(nudge, 1, 0, focus != was, e->key.repeat, settings_animations_reduced(), NV_EDGE_BUMP_PX, NV_EDGE_BUMP_W);
      break; }
    case SDLK_UP:
      if (row == 0) zone = ZONE_PICKERS;
      else focus -= grid_cols();
      break;
    case SDLK_DOWN: {
      int was = focus;
      if (row < lastRow) {
        focus += grid_cols();
        if (focus >= n) focus = n - 1;
      }
      anim_nudge(nudge, 0, 1, focus != was, e->key.repeat, settings_animations_reduced(), NV_EDGE_BUMP_PX, NV_EDGE_BUMP_W);
      break; }
    // OK on a card is handled above, by the hold.
    default: break;
  }
  // Nearing the end, ask for the next page — before the owner sees the empty
  // space, not once they are already staring at it.
  //
  // FOUR ROWS AHEAD, not two. The page takes a network round trip and then its
  // posters take their own; two rows gave the page time to land but not its art,
  // and the new rows came up as skeletons. Four leaves room for both, and
  // grid_warm starts on the posters as soon as the page is in.
  if (focus >= n - grid_cols() * 4) disc_seeall_more();
}

void dui_update(float dt, Uint32 now) {
  int i, n = gridN();
  hold_animate(&hold, dt, now);
  anim_nudge_step(nudge, dt, NV_EDGE_BUMP_W);
  if (hold_fired(&hold, now) && zone == ZONE_GRID) {
    int idx = focusedIndex();
    if (idx >= 0) {
      // The rect is the focused card's even when hasItemFocus is 0 (a title
      // not yet in the catalogue): the draw writes it for every focused card.
      if (itemFocus.rect.w > 0.0f) ctx_set_anchor(itemFocus.rect, NV_DSC_POSTER_R);
      ctx_open(idx);
    }
  }
  for (i = 0; i < PICK_N; i++) {
    float target = (zone == ZONE_PICKERS && pickSel == i) ? 1.0f : 0.0f;
    animPick[i] = anim_spring(animPick[i], target, dt,
                              target > animPick[i] ? NV_SPRING_FOCUS : NV_SPRING_BLUR);
  }
  { float target = (zone == ZONE_HEAD) ? 1.0f : 0.0f;
    animHead = anim_spring(animHead, target, dt,
                           target > animHead ? NV_SPRING_FOCUS : NV_SPRING_BLUR); }
  { float target = (zone == ZONE_GRID) ? 1.0f : 0.0f;
    animCard = anim_spring(animCard, target, dt,
                           target > animCard ? NV_SPRING_FOCUS : NV_SPRING_BLUR); }

  // THE SCROLL SNAPS TO A ROW, and that is what fixes the top edge.
  //
  // Before, it scrolled by whatever the focused card needed, so the row at the
  // top of the screen was left sliced through the middle of its posters — a
  // hard horizontal line across six pictures, which reads as a rendering fault
  // and not as an edge. That was "the top cutoff looks weird".
  //
  // Snapping to a multiple of the row step means the first visible row is
  // ALWAYS flush under the pickers: at rest nothing is cut at the top at all,
  // and the only cut is at the bottom, where the screen's own edge is and where
  // the eye already expects the picture to stop.
  //
  // IT SNAPS TO THE FOCUSED ROW rather than keeping a row above it, and that is
  // arithmetic and not preference: a card is 483 tall with its focus scale and
  // its name, the band under the pickers is 784, and two rows are 972. The
  // second row cannot be made to fit, so the focused one goes to the top.
  //
  // No clamp at the end of the list: stopping the scroll early would put the
  // last rows back where a partial row sits at the top, which is the thing this
  // exists to avoid. The list simply ends with space under it.
  if (!follow) {
    /* the pointer's: hold where it is */
  } else if (zone == ZONE_GRID && n > 0) {
    scrollTarget = (float)(focus / grid_cols()) * grid_line_step();
  } else {
    scrollTarget = 0.0f;
  }
  // THE POINTER RESTING ON THE GRID'S EDGE scrolls it; the focus stays put.
  { float d = app_screen_in_front() && menuOpen < 0 && n > 0
            ? pointer_edge_scroll(0.0f, NV_SCREEN_W, NV_DSC_CLIP_TOP, NV_SCREEN_H, dt) : 0.0f;
    if (d != 0.0f) {
      float max = (float)((n - 1) / grid_cols()) * grid_line_step();
      scrollTarget = anim_clamp(scrollTarget + d, 0.0f, max);
      follow = 0;
    } }
  if (scrollTarget < 0.0f) scrollTarget = 0.0f;
  scrollY = anim_spring2_reduced(&scrollV, scrollY, scrollTarget, dt, NV_SPRING2_PAGE,
                                settings_animations_reduced());
}

// --- Drawing -----------------------------------------------------------------
// The pickers and the list they open are dropdown.c's — the same control the
// collection grid draws, so the two cannot drift. Only which picker is where,
// and what its label and value read as, belong to this screen.
static const char *menuLabel(void *ctx, int i) {
  return optionLabel(*(int *)ctx, i);
}

static GfxRect pickRect(int p) {
  GfxRect r = { NV_DSC_X + p * NV_DSC_PICK_STEP, NV_DSC_PICK_Y,
                NV_DSC_PICK_W, NV_DSC_PICK_H };
  return r;
}

static void drawPicker(int p) {
  const CatRow *cat = currentCat();
  const char *label = (p == PICK_TYPE) ? "Type"
                    : (p == PICK_CATALOG) ? "Catalog" : "Genre";
  const char *value;

  if (p == PICK_TYPE) value = KIND_LABEL[kindSel];
  else if (p == PICK_CATALOG) value = cat ? cat->title : "None";
  else value = genreLabel(genreSel);

  // All three are live at once here: they are three halves of one request, not
  // alternatives to each other. See dd_pill's `active`.
  dd_pill(pickRect(p), label, value, animPick[p], 1, 1.0f);
}

static void drawMenu(void) {
  int p = menuOpen;
  if (p < 0) return;
  pointer_zone_click(0, 0, NV_SCREEN_W, NV_SCREEN_H, pointOffMenu, 0, 0);
  dd_menu_point(pointOption);
  dd_menu(pickRect(p), optionsN(p), menuFocus, menuLabel, &menuOpen, 1.0f);
}

// The grid. 252-wide posters, six across, the focused one scaled 1.05 from its
// TOP edge with the 4px border on the INSIDE — "Android TV uses the inside
// focus border, not an outer halo", says the stylesheet's own comment.
// THE GRID'S ENTRANCE when it fills with something new — a catalogue, type or genre picked, whose page arrives into an empty grid — and never when
// the same list merely redraws. The cards arrive in a diagonal wave from the top
// left of what is on screen (anim_stagger), each fading in and rising the last
// NV_STAGGER_RISE into place. 0 is "no entrance running".
static Uint32 fillAt;
static float entranceOf(int i, Uint32 now) {
  if (!fillAt || settings_animations_reduced()) return 1.0f;
  int firstRow = (int)(scrollY / grid_line_step());
  int order = (i / grid_cols() - firstRow) + i % grid_cols();
  float p = anim_stagger((float)(now - fillAt), order, NV_STAGGER_MS,
                         NV_STAGGER_MAX, NV_STAGGER_DUR_MS);
  if (now - fillAt > NV_STAGGER_MS * NV_STAGGER_MAX + NV_STAGGER_DUR_MS) fillAt = 0;
  return p;
}

static void drawGrid(Uint32 now) {
  int n = gridN(), i;
  hasItemFocus = 0;
  { static int drawnN;
    if (n && !drawnN) fillAt = now ? now : 1;
    drawnN = n; }

  if (!n) {
    const char *l1 = disc_seeall_loading() ? "Fetching titles…"
                                           : "Nothing in this catalogue";
    const char *l2 = disc_seeall_loading()
        ? "The catalogue is answering."
        : "Try another catalogue, or a different genre.";
    TxtLine t1 = txt_line(TXT_TITLE2, l1, 255, 255, 255, 255);
    TxtLine t2 = txt_line(TXT_SRCH_EMPTY, l2, 179, 179, 179, 255);
    float cx = NV_DSC_X + NV_DSC_W * 0.5f;
    float y  = NV_DSC_GRID_Y + 150.0f;
    txt_draw_alpha(t1, cx - (float)t1.w * 0.5f, y, 0.96f);
    txt_draw_alpha(t2, cx - (float)t2.w * 0.5f, y + (float)t1.h + 18.0f, 0.85f);
    return;
  }

  // Clipped just under the pickers and run to the bottom of the SCREEN. It used
  // to stop 60px short, which is the other half of the cut-off-with-room-left
  // report above.
  gfx_crop(0.0f, NV_DSC_CLIP_TOP, NV_SCREEN_W, NV_DSC_GRID_BOTTOM - NV_DSC_CLIP_TOP);
  pointer_clip(0.0f, NV_DSC_CLIP_TOP, NV_SCREEN_W, NV_DSC_GRID_BOTTOM - NV_DSC_CLIP_TOP);
  // TWO PASSES: the focused card scales up and must sit over its neighbours,
  // or the poster beside it clips the focus border.
  for (int pass = 0; pass < 2; pass++)
    for (i = 0; i < n; i++) {
      CatItem it;
      int isFocus = (zone == ZONE_GRID && i == focus);
      float f = isFocus ? animCard : 0.0f;
      float top = NV_DSC_GRID_Y + (float)(i / grid_cols()) * grid_line_step() - scrollY;
      float left = NV_DSC_X + (float)(i % grid_cols()) * grid_card_step();
      // THE ROW DISSOLVES AS IT LEAVES, the home's top-edge mask and the
      // collection grid's. The clip alone guillotines a poster against an
      // invisible line; this has it gone before it gets there. `top` already
      // carries the scroll and this screen has no entrance offset, so it IS the
      // resting y that anim_edge wants.
      float edge = anim_edge(top, NV_DSC_CLIP_TOP, NV_DSC_FADE);
      if ((pass == 1) != (f > 0.01f)) continue;
      if (top > NV_SCREEN_H + 40.0f || top + grid_poster_h() < -40.0f) {
        // Only rows within reach: the grid can run to hundreds of titles, and
        // copying every one out of the page cache per frame to learn it is far
        // away would cost more than the warming saves.
        if (top < NV_SCREEN_H + grid_line_step() * 4.0f &&
            top + grid_poster_h() > -grid_line_step() * 1.5f &&
            disc_seeall_item(i, &it))
          grid_warm(it.poster[0] ? it.poster : it.backdrop, top,
                    grid_card_w(), grid_line_step());
        continue;
      }
      // `&& !isFocus`: navigating UPWARDS the newly focused row descends INTO
      // the fold, so it starts at zero — and skipping it there would drop
      // itemFocus for those frames and blink the backdrop this screen feeds.
      // Drawn at zero it costs nothing and the bookkeeping below still runs.
      if (edge <= 0.004f && !isFocus) continue;
      if (!disc_seeall_item(i, &it)) continue;
      // The entrance moves the card but not its mask: `edge` above is already
      // read from the resting top, which is what anim_edge wants.
      { float in = entranceOf(i, now);
        gfx_opacity_group = edge * in;
        top += (1.0f - in) * NV_STAGGER_RISE; }

      { float scale = anim_blend(1.0f, 1.0f + NV_DSC_FOCUS_SCALE, f);
        float w = grid_card_w() * scale, h = grid_poster_h() * scale;
        // `transform-origin: top`: the top edge stays on the row and the growth
        // goes downwards, so only x is re-centred.
        GfxRect card = { left - (w - grid_card_w()) * 0.5f, top, w, h };
        pointer_zone(card.x, card.y, card.w, card.h, pointCard, i, 0);
        // Held: pressed in about its centre, the glow behind it (hold.h).
        if (isFocus) {
          card = hold_card(&hold, card);
          card.x += nudge[0].x; card.y += nudge[1].x;
          hold_glow(&hold, card, NV_DSC_POSTER_R);
        }
        float radius = NV_DSC_POSTER_R / card.h;
        const char *art = it.poster[0] ? it.poster
                        : (it.backdrop[0] ? it.backdrop : NULL);
        // The RESTING width, not the animated one: tex_cache re-decodes an exact
        // request whose width moves, and a poster that re-decodes through a focus
        // spring is a poster that is missing for the length of it.
        GLuint tex = art ? tex_get_width(art, grid_card_w()) : 0;
        if (tex) {
          // Fresh art comes in over its skeleton rather than replacing it (tex_appear).
          float in = tex_appear(tex);
          if (in < 1.0f) gfx_skeleton(card, radius, NV_COLOR_SKELETON_R, NV_COLOR_SKELETON_G, NV_COLOR_SKELETON_B, 1.0f);
          gfx_tex_aspect_current = tex_aspect(art);
          gfx_rect(card, tex, GFX_CARD, f, 0.0f, 0.0f, radius, 0, 0, 0, in);
          gfx_tex_aspect_current = 0.0f;
        } else {
          gfx_skeleton(card, radius, NV_COLOR_SKELETON_R, NV_COLOR_SKELETON_G,
                       NV_COLOR_SKELETON_B, 1.0f);
        }
        // The ring, with the hold's sweep over it while OK is held.
        hold_ring(&hold, card, NV_DSC_BORDER / card.h, radius,
                  0.961f, 0.961f, 0.961f, f);
        if (isFocus) hold_track(&hold, card, NV_DSC_POSTER_R);

        if (settings_labels_poster()) {
          TxtLine tl = txt_line_trim(TXT_CALLOUT, it.title, 255, 255, 255, 255,
                                     grid_card_w());
          txt_draw_alpha(tl, card.x, top + h + NV_DSC_TITLE_GAP, 0.98f);
        }

        // PUT IT BACK before anything else is drawn: a group opacity left set
        // bleeds onto the pickers and the open menu above.
        gfx_opacity_group = 1.0f;

        if (isFocus) {
          int idx = it.imdb[0] ? cat_index_by_imdb(it.imdb) : -1;
          itemFocus.index_ = idx;
          itemFocus.rect   = card;
          itemFocus.art    = it.backdrop[0] ? it.backdrop : it.poster;
          itemFocus.title  = it.title;
          itemFocus.genre  = it.genre;
          itemFocus.meta   = it.meta;
          hasItemFocus = idx >= 0;
        }
      }
    }
  gfx_opacity_group = 1.0f;
  grid_bar_draw(&bar, (int)(scrollTarget / grid_line_step() + 0.5f),
                (n + grid_cols() - 1) / grid_cols(), grid_line_step(),
                NV_DSC_GRID_Y, NV_DSC_GRID_BOTTOM - NV_DSC_FADE, 1.0f, now);
  gfx_no_crop();
  pointer_no_clip();
}

void dui_draw(Uint32 now) {
  int p;
  // No background fill: main.c has already cleared to #0d0d0d, and a
  // full-screen layer thrown away per frame is the dominant cost on this GPU.
  // See the note in gfx.c.
  txt_tracking(TXT_TITLE3, "Discover", 255, 255, 255,
               NV_DSC_X, NV_DSC_Y, 1.0f, NV_DSC_TITLE_LS);
  // `.library-page-source`: the context line, right-aligned against the title.
  // It says WHICH catalogue is on screen, which the picker below also says —
  // but the picker says it as a control and this says it as a heading, and the
  // web app carries both.
  { const CatRow *cat = currentCat();
    char line[160];
    snprintf(line, sizeof line, "%s%s%s",
             KIND_LABEL[kindSel],
             cat ? "  ·  " : "",
             cat ? cat->title : "");
    // The grid-size button takes the right end of the title row, and the
    // context line moves in to sit beside it.
    { GfxRect b = grid_button_draw(NV_DSC_X + NV_DSC_W, NV_DSC_Y + (NV_DSC_TITLE_H - 64.0f) * 0.5f, animHead, 1.0f);
      pointer_zone(b.x, b.y, b.w, b.h, pointHead, 0, 0);
      float w = txt_tracking(TXT_SRCH_NAME, line, 128, 128, 128, -1.0f, 0.0f, 0.0f, 4.0f);
      txt_tracking(TXT_SRCH_NAME, line, 128, 128, 128,
                   b.x - 32.0f - w, NV_DSC_Y + 10.0f, 0.95f, 4.0f); } }

  for (p = 0; p < PICK_N; p++) {
    GfxRect r = pickRect(p);
    drawPicker(p);
    pointer_zone(r.x, r.y, r.w, r.h, pointPick, p, 0);
  }
  drawGrid(now);
  // LAST, over everything: an open list covers the grid and the pickers beside
  // it, and it carries a shadow that has to fall on them.
  drawMenu();
}
