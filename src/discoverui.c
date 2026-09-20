#include "discoverui.h"
#include "gfx.h"
#include "text.h"
#include "tex_cache.h"
#include "anim.h"
#include "layout.h"
#include "catalog.h"
#include "discover.h"
#include "settings.h"
#include "dropdown.h"
#include <string.h>
#include <stdio.h>

// The three pickers, left to right, and the two places the focus can be.
enum { PICK_TYPE, PICK_CATALOG, PICK_GENRE, PICK_N };
enum { ZONE_PICKERS, ZONE_GRID };

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
static int   request = -1;
static float scrollY, scrollTarget;
static float animPick[PICK_N];
static float animCard;         // one spring: only ever one card is focused
static HomeItem itemFocus;
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
  scrollY = scrollTarget = 0.0f;
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
        focus = 0; scrollY = scrollTarget = 0.0f; animCard = 0.0f;
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

void dui_event(const SDL_Event *e) {
  int n, row, lastRow;
  if (e->type == SDL_QUIT) { wantsExit = 1; return; }
  if (e->type != SDL_KEYDOWN) return;
  SDL_Keycode k = e->key.keysym.sym;
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

  if (zone == ZONE_PICKERS) {
    switch (k) {
      case SDLK_LEFT:  if (pickSel > 0) pickSel--; break;
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

  row     = focus / NV_DSC_COLUMNS;
  lastRow = n > 0 ? (n - 1) / NV_DSC_COLUMNS : 0;
  switch (k) {
    case SDLK_LEFT:
      if (focus % NV_DSC_COLUMNS) focus--;
      break;
    case SDLK_RIGHT:
      if (focus + 1 < n && (focus + 1) % NV_DSC_COLUMNS) focus++;
      break;
    case SDLK_UP:
      if (row == 0) zone = ZONE_PICKERS;
      else focus -= NV_DSC_COLUMNS;
      break;
    case SDLK_DOWN:
      if (row < lastRow) {
        focus += NV_DSC_COLUMNS;
        if (focus >= n) focus = n - 1;
      }
      break;
    case SDLK_RETURN: case SDLK_KP_ENTER: {
      // The item is NOT in the global catalogue — it came from a page only this
      // screen read — so it goes in through cat_append and the detail opens it
      // by index, which is how the whole app works. Same as seeall.c.
      CatItem it;
      if (focus >= 0 && focus < n && disc_seeall_item(focus, &it)) {
        int idx = it.imdb[0] ? cat_index_by_imdb(it.imdb) : -1;
        if (idx < 0) idx = cat_append(&it);
        if (idx >= 0) request = idx;
      }
      break;
    }
    default: break;
  }
  // Nearing the end, ask for the next page — before the owner sees the empty
  // space, not once they are already staring at it.
  if (focus >= n - NV_DSC_COLUMNS * 2) disc_seeall_more();
}

void dui_update(float dt, Uint32 now) {
  int i, n = gridN();
  (void)now;
  for (i = 0; i < PICK_N; i++) {
    float target = (zone == ZONE_PICKERS && pickSel == i) ? 1.0f : 0.0f;
    animPick[i] = anim_spring(animPick[i], target, dt,
                              target > animPick[i] ? NV_SPRING_FOCUS : NV_SPRING_BLUR);
  }
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
  if (zone == ZONE_GRID && n > 0) {
    scrollTarget = (float)(focus / NV_DSC_COLUMNS) * NV_DSC_LINE_STEP;
  } else {
    scrollTarget = 0.0f;
  }
  if (scrollTarget < 0.0f) scrollTarget = 0.0f;
  scrollY = anim_spring(scrollY, scrollTarget, dt, NV_SPRING_GRID);
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
  dd_menu(pickRect(p), optionsN(p), menuFocus, menuLabel, &menuOpen, 1.0f);
}

// The grid. 252-wide posters, six across, the focused one scaled 1.05 from its
// TOP edge with the 4px border on the INSIDE — "Android TV uses the inside
// focus border, not an outer halo", says the stylesheet's own comment.
static void drawGrid(void) {
  int n = gridN(), i;
  hasItemFocus = 0;

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
  // TWO PASSES: the focused card scales up and must sit over its neighbours,
  // or the poster beside it clips the focus border.
  for (int pass = 0; pass < 2; pass++)
    for (i = 0; i < n; i++) {
      CatItem it;
      int isFocus = (zone == ZONE_GRID && i == focus);
      float f = isFocus ? animCard : 0.0f;
      float top = NV_DSC_GRID_Y + (float)(i / NV_DSC_COLUMNS) * NV_DSC_LINE_STEP - scrollY;
      float left = NV_DSC_X + (float)(i % NV_DSC_COLUMNS) * NV_DSC_CARD_STEP;
      // THE ROW DISSOLVES AS IT LEAVES, the home's top-edge mask and the
      // collection grid's. The clip alone guillotines a poster against an
      // invisible line; this has it gone before it gets there. `top` already
      // carries the scroll and this screen has no entrance offset, so it IS the
      // resting y that anim_edge wants.
      float edge = anim_edge(top, NV_DSC_CLIP_TOP, NV_DSC_FADE);
      if ((pass == 1) != (f > 0.01f)) continue;
      if (top > NV_SCREEN_H + 40.0f || top + NV_DSC_POSTER_H < -40.0f) continue;
      // `&& !isFocus`: navigating UPWARDS the newly focused row descends INTO
      // the fold, so it starts at zero — and skipping it there would drop
      // itemFocus for those frames and blink the backdrop this screen feeds.
      // Drawn at zero it costs nothing and the bookkeeping below still runs.
      if (edge <= 0.004f && !isFocus) continue;
      if (!disc_seeall_item(i, &it)) continue;
      gfx_opacity_group = edge;

      { float scale = anim_blend(1.0f, 1.0f + NV_DSC_FOCUS_SCALE, f);
        float w = NV_DSC_CARD_W * scale, h = NV_DSC_POSTER_H * scale;
        // `transform-origin: top`: the top edge stays on the row and the growth
        // goes downwards, so only x is re-centred.
        GfxRect card = { left - (w - NV_DSC_CARD_W) * 0.5f, top, w, h };
        float radius = NV_DSC_POSTER_R / card.h;
        const char *art = it.poster[0] ? it.poster
                        : (it.backdrop[0] ? it.backdrop : NULL);
        // The RESTING width, not the animated one: tex_cache re-decodes an exact
        // request whose width moves, and a poster that re-decodes through a focus
        // spring is a poster that is missing for the length of it.
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
          TxtLine tl = txt_line_trim(TXT_CALLOUT, it.title, 255, 255, 255, 255,
                                     NV_DSC_CARD_W);
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
  gfx_no_crop();

}

void dui_draw(Uint32 now) {
  int p;
  (void)now;
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
    { float w = txt_tracking(TXT_SRCH_NAME, line, 128, 128, 128, -1.0f, 0.0f, 0.0f, 4.0f);
      txt_tracking(TXT_SRCH_NAME, line, 128, 128, 128,
                   NV_DSC_X + NV_DSC_W - w, NV_DSC_Y + 10.0f, 0.95f, 4.0f); } }

  for (p = 0; p < PICK_N; p++) drawPicker(p);
  drawGrid();
  // LAST, over everything: an open list covers the grid and the pickers beside
  // it, and it carries a shadow that has to fall on them.
  drawMenu();
}
