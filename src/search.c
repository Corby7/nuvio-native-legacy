// Search, aligned with the web app's screen (MEASURED live, on the owner's
// profile).
//
// ------------------------------------------------------------------------
// WHAT CHANGED, AND WHY
//
// The port had a GRID keyboard on the left and a 4-wide GRID of posters on the
// right. Measuring the web app's screen, neither of those matches:
//
//   1. The web app has no results grid. It has horizontal ROWS, one per addon
//      catalogue, with the catalogue's name in 48/600 and the origin
//      ("from Xperience") in 20/400 just below. A 248-wide card, a 248x372
//      poster, the name in 28/500 and the year in 20/400 underneath; a step of
//      280 between cards and 562.4 between rows.
//   2. The web app has no keyboard at all: it has a wide <input> at the top, and
//      what raises the keyboard is the TV's SYSTEM.
//
// (1) was ported whole. (2) CANNOT be ported: this app is pure SDL and there is
// no IME to call — with no on-screen keyboard there is no way to type, and a
// search you cannot type into is not a search. The keyboard stayed, now BELOW
// the header and on the left, occupying the band where the web app draws its
// empty state; the result rows run to the right of it. It is this screen's only
// deliberate divergence, and it is recorded here so as not to be mistaken for
// carelessness.
//
// DESIGN DECISION — the keyboard is a GRID, not tvOS's single line.
// The tvOS horizontal strip is handsome and fits in little height, but it costs
// dearly on a D-pad: 38 keys in ONE dimension, so the average distance between
// two letters is ~13 presses and the worst case is over 37. The 6x7 grid puts
// the same key at 5+6 presses at most and ~5 on average.
#include "search.h"
#include "gfx.h"
#include "text.h"
#include "tex_cache.h"
#include "focus.h"
#include "anim.h"
#include "layout.h"
#include "settings.h"
#include "catalog.h"
#include "discover.h"
#include <string.h>
#include <stdio.h>

// --- Header: geometry MEASURED in the web app --------------------------------
// .search-header y=22 h=110, side padding 104.
//   .search-discover-btn 110x110 at (104,22)   bg #222, 1px #333 border, radius 22
//   .search-voice-btn    110x110 at (262,22)   -> step 158 (gap 48)
//   .search-input-field  1396x110 at (420,22)  bg #222, radius 22, 34/500,
//                        side padding 32, placeholder "Search films and series"
//
// Voice and Discover do not appear as buttons: there is no audio capture and no
// discovery action on this native screen. The field takes the full available width.
#define SEARCH_HEAD_Y     NV_SEARCH_HEAD_Y
#define SEARCH_HEAD_H     NV_SEARCH_HEAD_H
#define SEARCH_DIR       (NV_SCREEN_W - NV_CONTENT_PAD)   // 1816

// --- Keyboard (a deliberate divergence; see the top) -------------------------
#define SEARCH_KEY_W     74.0f
#define SEARCH_KEY_GAP   12.0f
#define SEARCH_KB_COLS      6
#define SEARCH_KB_ROWS  7            // 6 rows of A-Z/0-9 + 1 of space/delete
#define SEARCH_KB_STEP   (SEARCH_KEY_W + SEARCH_KEY_GAP)
#define SEARCH_KB_W       (SEARCH_KB_COLS * SEARCH_KEY_W + (SEARCH_KB_COLS - 1) * SEARCH_KEY_GAP)
#define SEARCH_KB_Y       NV_SEARCH_EMPTY_Y   // 148: the web app's empty-state band
// How much the focused key grows. Smaller than the poster's on purpose: the key
// is small and an immediate neighbour of the others, and at 14% it invades the
// 12px gap.
#define SEARCH_KEY_SCALE 0.10f
#define SEARCH_MAX_QUERY 48

// --- Result rows (the web app's geometry) ------------------------------------
#define SEARCH_RES_X       (SEARCH_KB_X + SEARCH_KB_W + 64.0f)
#define SEARCH_RES_Y       NV_SEARCH_EMPTY_Y
#define SEARCH_RES_AREA_H  (NV_SCREEN_H - NV_MARGIN_Y - SEARCH_RES_Y)
#define SEARCH_MAX_ROWS FOCUS_MAX_ROWS
#define SEARCH_MAX_PER_FILTER  12

#define SEARCH_KB_X        NV_CONTENT_PAD

// --- Estado ------------------------------------------------------------------
static Focus  focusKb;
static Focus  focusRes;
static int   panel = 0;            // 0 = keyboard, 1 = results
static char  query[SEARCH_MAX_QUERY];
static int   nQuery = 0;
static char queryFiltered[SEARCH_MAX_QUERY];
// Results grouped by CATALOGUE, as in the web app: one row per catalogue that
// had at least one matching title. We store indices into the global catalogue.
static struct {
  const char *title;      // the catalogue's name ("Top 100 Today - Film")
  const char *origin;      // "from <addon>"; empty when it is not known
  int items[SEARCH_MAX_PER_FILTER];
  int n;
} filter[SEARCH_MAX_ROWS];
static int nFilter = 0;
static int wantsExit = 0;
static int request = -1;             // the chosen catalogue index, -1 = none
static float animKey[SEARCH_KB_ROWS][SEARCH_KB_COLS];
static float animRes[SEARCH_MAX_ROWS][SEARCH_MAX_PER_FILTER];
static float scrollY = 0.0f, scrollTarget = 0.0f;
static float scrollX[SEARCH_MAX_ROWS];
static HomeItem itemFocus;
static int   hasItemFocus = 0;

static const int KB_COLUMNS[SEARCH_KB_ROWS] = { 6, 6, 6, 6, 6, 6, 3 };
// Lower case as on the device: the field shows what was typed, and a query in
// capitals reads as shouting. The comparison ignores case either way.
static const char *KEYS =
  "abcdefghijklmnopqrstuvwxyz0123456789";   // 36 = 6 rows x 6 columns

// --- Normalisation -----------------------------------------------------------
// Folds an accented Latin letter (the second byte of a UTF-8 sequence starting
// with 0xC3) onto the matching ASCII letter. Without this, searching for
// "fundacao" does not find "Fundação" — the screen's most obvious use case,
// since nobody types a cedilla on a D-pad keyboard.
static char foldLatin(unsigned char second) {
  unsigned cp = (unsigned)second + 0x40u;
  if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) cp += 0x20;
  if (cp >= 0xE0 && cp <= 0xE6) return 'a';
  if (cp == 0xE7)               return 'c';
  if (cp >= 0xE8 && cp <= 0xEB) return 'e';
  if (cp >= 0xEC && cp <= 0xEF) return 'i';
  if (cp == 0xF0)               return 'd';
  if (cp == 0xF1)               return 'n';
  if ((cp >= 0xF2 && cp <= 0xF6) || cp == 0xF8) return 'o';
  if (cp >= 0xF9 && cp <= 0xFC) return 'u';
  if (cp == 0xFD || cp == 0xFF) return 'y';
  return ' ';
}

static void normalize(const char *s, char *destination, size_t size) {
  size_t k = 0;
  const unsigned char *p = (const unsigned char *)s;
  while (*p && k + 1 < size) {
    unsigned char c = *p++;
    char output;
    if (c < 0x80) {
      output = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c;
    } else if (c == 0xC3 && *p) {
      output = foldLatin(*p++);
    } else {
      while ((*p & 0xC0) == 0x80) p++;
      output = ' ';
    }
    destination[k++] = output;
  }
  destination[k] = 0;
}

// --- Filter ------------------------------------------------------------------
// One row per CATALOGUE, exactly as the web app builds `.search-results-row`.
// This used to be a flat list of the whole collection, which lost the
// information of WHERE each result was found — and that information is what the
// "from <addon>" subtitle shows.
static void refilter(void) {
  char target[SEARCH_MAX_QUERY * 2];
  int previous = -1, sameQuery = !strcmp(queryFiltered, query);
  if (sameQuery && panel == 1 && focusRes.row < nFilter &&
      focusRes.column < filter[focusRes.row].n)
    previous = filter[focusRes.row].items[focusRes.column];
  snprintf(queryFiltered, sizeof queryFiltered, "%s", query);
  normalize(query, target, sizeof target);
  nFilter = 0;
  // Fewer than 2 characters = the empty state, like the web app ("Type at least
  // 2 characters"). Searching with one letter returns the whole collection and
  // does not help.
  if ((int)strlen(target) < 2) { panel = 0; return; }

  // A NETWORK SEARCH. The screen only filtered what was already in memory — the
  // first ~12 rows of each of the home's catalogues — so any title outside that
  // simply did not exist as far as the search was concerned. It fires and
  // returns at once; the result appears on its own when it arrives, because
  // refiltering runs on every keypress and disc_search_n only answers for the
  // current term.
  disc_fetch(target);

  // ONE ROW PER CATALOGUE QUERIED, with the origin below it — just like the web
  // app, which builds one `.search-results-row` per catalogue instead of a single
  // list.
  //
  // Before, only Cinemeta was queried and everything fell into one "Search
  // results" row. With ten targets in a single list the owner had no way to know
  // where anything came from, and the slow addon's results seemed never to arrive
  // (they did; they sat at the end of a row of 12 already full of Cinemeta).
  //
  // The items go into the global catalogue via cat_append_batch because the screen
  // opens a title by catalogue INDEX — a result that lived only here would not be
  // openable.
  { int targetIdx, nTargets = disc_search_n_targets();
    for (targetIdx = 0; targetIdx < nTargets && nFilter < SEARCH_MAX_ROWS; targetIdx++) {
      int nRemote = disc_search_target_n(targetIdx, target), i;
      // TWO PASSES, and the separation is the fix: first gather those not yet in
      // the catalogue, then append them ALL in a single block swap.
      //
      // It used to be cat_append per result, and each call copies the whole
      // catalogue: with 300 titles, ~2.3 MB per copy, up to 40 times, on the
      // DRAWING thread, on every key pressed. That is why the search stuttered.
      CatItem new[SEARCH_MAX_PER_FILTER];
      int idxNew[SEARCH_MAX_PER_FILTER];
      int found = 0, nNew = 0;
      int posNew[SEARCH_MAX_PER_FILTER];   // where each new one goes in filter[].items
      if (nRemote <= 0) continue;
      for (i = 0; i < nRemote && found < SEARCH_MAX_PER_FILTER; i++) {
        CatItem it;
        int idx;
        if (!disc_search_target_item(targetIdx, i, &it)) continue;
        // Already in the catalogue? Reuse the index instead of duplicating the card.
        idx = it.imdb[0] ? cat_index_by_imdb(it.imdb) : -1;
        if (idx >= 0) {
          filter[nFilter].items[found++] = idx;
        } else if (nNew < SEARCH_MAX_PER_FILTER) {
          new[nNew] = it;
          posNew[nNew] = found++;   // reserva o lugar; o indice vem depois
          nNew++;
        }
      }
      if (nNew > 0) {
        int entered = cat_append_batch(new, nNew, idxNew);
        for (i = 0; i < nNew; i++)
          filter[nFilter].items[posNew[i]] = (i < entered) ? idxNew[i] : -1;
        // Whatever did not fit (catalogue at its ceiling) becomes -1 and is
        // COMPACTED out. Just lowering the count would leave holes in the MIDDLE
        // of the row, and the hole's card would point at the wrong item — worse
        // than a missing card.
        if (entered < nNew) {
          int r = 0, w = 0;
          for (r = 0; r < found; r++)
            if (filter[nFilter].items[r] >= 0) filter[nFilter].items[w++] = filter[nFilter].items[r];
          found = w;
        }
      }
      if (found > 0) {
        filter[nFilter].title = disc_search_target_title(targetIdx);
        filter[nFilter].origin = disc_search_target_addon(targetIdx);
        filter[nFilter].n = found;
        nFilter++;
      }
    } }

  int nCat = cat_n_rows();
  for (int r = 0; r < nCat && nFilter < SEARCH_MAX_ROWS; r++) {
    const CatRow *cf = cat_row(r);
    if (!cf) break;
    int found = 0;
    for (int i = 0; i < cf->n && found < SEARCH_MAX_PER_FILTER; i++) {
      const CatItem *ci = cat_item(cf->start + i);
      if (!ci) continue;
      char title[320];
      normalize(ci->title, title, sizeof title);
      if (strstr(title, target)) filter[nFilter].items[found++] = cf->start + i;
    }
    if (!found) continue;
    filter[nFilter].title = cf->title;
    // `catalogAddonNameEnabled` decides the "from <addon>" line under the row's
    // title. The data does NOT exist on this side: `CatRow` stores the key, the
    // title, the type and the window into the array — the addon's name lives in
    // addons.c and the row does not carry it. Until discover.c passes that field
    // along, the line is not drawn; writing the TYPE ("movie") there instead would
    // be worse than its absence, because it would read as the origin.
    filter[nFilter].origin = NULL;
    filter[nFilter].n = found;
    nFilter++;
  }

  int cols[SEARCH_MAX_ROWS];
  for (int i = 0; i < nFilter; i++) cols[i] = filter[i].n;
  focus_start(&focusRes, nFilter > 0 ? nFilter : 1, nFilter > 0 ? cols : (int[]){ 1 });
  if (previous >= 0) {
    int found = 0;
    for (int r = 0; r < nFilter && !found; r++)
      for (int c = 0; c < filter[r].n; c++)
        if (filter[r].items[c] == previous) {
          focusRes.row = r; focusRes.column = c;
          focusRes.columnRemembered[r] = c;
          found = 1; break;
        }
  }
  if (nFilter == 0) panel = 0;
  memset(animRes, 0, sizeof animRes);
  if (!sameQuery) {
    memset(scrollX, 0, sizeof scrollX);
    scrollY = scrollTarget = 0.0f;
  }
}

// --- Teclas ------------------------------------------------------------------
static void applyKey(void) {
  if (focusKb.row < SEARCH_KB_ROWS - 1) {
    int k = focusKb.row * SEARCH_KB_COLS + focusKb.column;
    if (nQuery + 1 < SEARCH_MAX_QUERY) query[nQuery++] = KEYS[k];
  } else if (focusKb.column == 0) {
    // a space at the start does not go in: it does not change the filter and only
    // accumulates rubbish in the field
    if (nQuery > 0 && nQuery + 1 < SEARCH_MAX_QUERY) query[nQuery++] = ' ';
  } else if (focusKb.column == 1) {
    if (nQuery > 0) nQuery--;
  } else {
    nQuery = 0;
  }
  query[nQuery] = 0;
  refilter();
}

static GfxRect rectKey(int row, int column) {
  GfxRect r;
  r.y = SEARCH_KB_Y + row * (SEARCH_KEY_W + SEARCH_KEY_GAP);
  r.h = SEARCH_KEY_W;
  if (row < SEARCH_KB_ROWS - 1) {
    r.x = SEARCH_KB_X + column * SEARCH_KB_STEP;
    r.w = SEARCH_KEY_W;
  } else {
    r.w = (SEARCH_KB_W - 2 * SEARCH_KEY_GAP) / 3;
    r.x = SEARCH_KB_X + column * (r.w + SEARCH_KEY_GAP);
  }
  return r;
}

// --- Life cycle --------------------------------------------------------------
int search_start(void) {
  focus_start(&focusKb, SEARCH_KB_ROWS, KB_COLUMNS);
  panel = 0; wantsExit = 0; request = -1;
  nQuery = 0; query[0] = 0;
  queryFiltered[0] = 0;
  scrollY = scrollTarget = 0.0f;
  hasItemFocus = 0;
  memset(animKey, 0, sizeof animKey);
  memset(animRes, 0, sizeof animRes);
  memset(scrollX, 0, sizeof scrollX);
  refilter();
  return 1;
}

void search_shutdown(void) { hasItemFocus = 0; }
int  search_wants_exit(void) { return wantsExit; }

int search_requested_open(int *indexCatalog) {
  if (request < 0) return 0;
  if (indexCatalog) *indexCatalog = request;
  request = -1;
  return 1;
}

int search_item_focused(HomeItem *out) {
  if (!hasItemFocus || !out) return 0;
  *out = itemFocus;
  return 1;
}

void search_event(const SDL_Event *e) {
  if (e->type == SDL_QUIT) { wantsExit = 1; return; }
  if (e->type != SDL_KEYDOWN) return;
  SDL_Keycode k = e->key.keysym.sym;

  if (k == SDLK_BACKSPACE && panel == 0) {
    if (nQuery > 0) { query[--nQuery] = 0; refilter(); }
    return;
  }
  if (k == SDLK_AC_BACK || k == SDLK_ESCAPE || k == SDLK_BACKSPACE) {
    // In the results, Back goes back to the keyboard: it is the reverse of the
    // move that led there. Only from the keyboard does it close the screen.
    if (panel == 1) panel = 0; else wantsExit = 1;
    return;
  }

  if (panel == 0) {
    if (!(e->key.keysym.mod & (KMOD_CTRL | KMOD_ALT | KMOD_GUI)) &&
        ((k >= SDLK_a && k <= SDLK_z) || (k >= SDLK_0 && k <= SDLK_9) || k == SDLK_SPACE)) {
      if (nQuery + 1 < SEARCH_MAX_QUERY && (k != SDLK_SPACE || nQuery)) {
        query[nQuery++] = (char)k; query[nQuery] = 0; refilter();
      }
      return;
    }
    if (k == SDLK_TAB && nFilter > 0) { panel = 1; return; }
    switch (k) {
      case SDLK_LEFT:  focus_move_grid(&focusKb, -1, 0); break;
      case SDLK_RIGHT:
        // Going past the keyboard's LAST column enters the results. It is the
        // only bridge between the two panels, and that is why it must not fail
        // silently: with no results at all, the focus stays where it is.
        if (focusKb.column >= KB_COLUMNS[focusKb.row] - 1) {
          if (nFilter > 0) panel = 1;
        } else focus_move_grid(&focusKb, 1, 0);
        break;
      // A GRID, not rows: see focus_move_grid. This is where "it jumps to a
      // random letter" came from — the per-row column memory sent the cursor to
      // wherever it had last been in the destination row instead of keeping the
      // column it was standing in.
      case SDLK_UP:     focus_move_grid(&focusKb, 0, -1); break;
      case SDLK_DOWN:   focus_move_grid(&focusKb, 0,  1); break;
      case SDLK_RETURN: case SDLK_KP_ENTER: applyKey(); break;
      default: break;
    }
    return;
  }

  switch (k) {
    case SDLK_TAB: panel = 0; break;
    case SDLK_LEFT:
      // Going back from the results' first column returns the focus to the keyboard.
      if (focusRes.column == 0) panel = 0;
      else focus_move(&focusRes, -1, 0);
      break;
    case SDLK_RIGHT:
      // `fastHorizontalNavigationEnabled`: the web app jumps three at a time
      // within the row when the preference is on. In a row of 12 cards, 4 presses
      // instead of 12 to reach the end.
      focus_move(&focusRes, 1, 0);
      if (settings_navigation_horizontal_fast()) {
        focus_move(&focusRes, 1, 0);
        focus_move(&focusRes, 1, 0);
      }
      break;
    case SDLK_UP:   focus_move(&focusRes, 0, -1); break;
    case SDLK_DOWN: focus_move(&focusRes, 0,  1); break;
    case SDLK_RETURN: case SDLK_KP_ENTER:
      if (focusRes.row < nFilter && focusRes.column < filter[focusRes.row].n)
        request = filter[focusRes.row].items[focusRes.column];
      break;
    default: break;
  }
}

void search_update(float dt, Uint32 now) {
  (void)now;
  // THE NETWORK RESULT ARRIVES AFTER THE KEYPRESS. refilter() only runs when the
  // owner types, so without this Cinemeta's answer arrived, sat there and NEVER
  // appeared — the screen went on showing the local filter from the moment the
  // last letter was pressed. Here the current term's count is watched per frame,
  // and a change rebuilds the list exactly once.
  { char target[SEARCH_MAX_QUERY * 2];
    static int lastRemote = -1;
    normalize(query, target, sizeof target);
    if ((int)strlen(target) >= 2) {
      int n = disc_search_n(target);
      if (n != lastRemote) { lastRemote = n; refilter(); }
    } else {
      lastRemote = -1;
    } }
  for (int f = 0; f < SEARCH_KB_ROWS; f++)
    for (int c = 0; c < KB_COLUMNS[f]; c++) {
      float target = (panel == 0 && focus_index(&focusKb, f, c)) ? 1.0f : 0.0f;
      animKey[f][c] = anim_spring(animKey[f][c], target, dt,
                                  target > animKey[f][c] ? NV_SPRING_FOCUS : NV_SPRING_BLUR);
    }
  for (int r = 0; r < SEARCH_MAX_ROWS; r++)
    for (int c = 0; c < SEARCH_MAX_PER_FILTER; c++) {
      float target = (panel == 1 && focus_index(&focusRes, r, c)) ? 1.0f : 0.0f;
      animRes[r][c] = anim_spring(animRes[r][c], target, dt,
                                target > animRes[r][c] ? NV_SPRING_FOCUS : NV_SPRING_BLUR);
    }

  // Scrolls only as far as needed for the focused row to fit whole in the usable
  // area — scrolling in proportion to the index would hide the first row before
  // the user had got to it.
  if (panel == 1 && nFilter > 0) {
    float top = focusRes.row * NV_SEARCH_ROW_STEP;
    float base = top + NV_SEARCH_ROW_RAIL + NV_SEARCH_POSTER_H + 70.0f;
    if (top - scrollTarget < 0.0f)             scrollTarget = top;
    if (base - scrollTarget > SEARCH_RES_AREA_H)    scrollTarget = base - SEARCH_RES_AREA_H;

    // Horizontal scrolling of the focused row, the same rule as the home's.
    int r = focusRes.row;
    float util = SEARCH_DIR - SEARCH_RES_X;
    float left = focusRes.column * NV_SEARCH_CARD_STEP;
    float dir = left + NV_SEARCH_CARD_W;
    float targetX = scrollX[r];
    if (dir - targetX > util) targetX = dir - util;
    if (left - targetX < 0.0f) targetX = left;
    if (targetX < 0.0f) targetX = 0.0f;
    scrollX[r] = anim_spring(scrollX[r], targetX, dt, NV_SPRING_SCROLL);
  } else {
    scrollTarget = 0.0f;
  }
  if (scrollTarget < 0.0f) scrollTarget = 0.0f;
  scrollY = anim_spring(scrollY, scrollTarget, dt, NV_SPRING_SCROLL);
}

// --- Drawing -----------------------------------------------------------------
// The query field: no decorative button that cannot take focus.
static void drawHeader(Uint32 now) {
  float x = NV_CONTENT_PAD;
  float radius = NV_SEARCH_RADIUS / (NV_SEARCH_HEAD_H * 0.5f) * 0.5f;  // 22 over 110

  GfxRect field = { x, SEARCH_HEAD_Y, SEARCH_DIR - x, NV_SEARCH_HEAD_H };
  gfx_color(field, radius, 0.133f, 0.133f, 0.133f, 1.0f);
  // The field's outline — a RING, not a filled rectangle.
  //
  // There used to be a gfx_color over `field + 2px` here, and gfx_color FILLS:
  // white at 22% washed out the whole field. The arithmetic matches what was
  // measured on screen: 0.133 x 0.78 + 0.961 x 0.22 = 0.315, that is #505050 in
  // place of the #222222 the line above had just painted. The field read as a
  // DISABLED control, and the example text almost vanished inside it.
  //
  // GFX_RING draws only the outline (the middle stays intact), which was the
  // intention written in the old comment.
  { GfxRect halo = { field.x - 2.0f, field.y - 2.0f,
                     field.w + 4.0f, field.h + 4.0f };
    gfx_rect(halo, 0, GFX_RING, 0, 2.0f / halo.h, 0, radius,
             0.961f, 0.961f, 0.961f, panel == 0 ? 0.55f : 0.16f); }

  float tx = field.x + NV_SEARCH_FIELD_PADX;
  if (nQuery) {
    TxtLine l = txt_line_trim(TXT_HEADLINE, query, 245, 246, 250, 255,
                                field.w - 2 * NV_SEARCH_FIELD_PADX - 12);
    txt_draw(l, tx, field.y + (field.h - l.h) * 0.5f);
    tx += l.w + 6.0f;
  } else {
    // The same text as the web app's placeholder.
    TxtLine l = txt_line(TXT_HEADLINE, "Search films and series", 255, 255, 255, 255);
    txt_draw_alpha(l, tx, field.y + (field.h - l.h) * 0.5f, 0.40f);
  }
  // The blinking cursor is the only sign that the field is active.
  if (panel == 0 && (now / 500) % 2 == 0) {
    GfxRect cur = { tx, field.y + 24.0f, 3.0f, field.h - 48.0f };
    gfx_color(cur, 0.0f, 1.0f, 1.0f, 1.0f, 0.85f);
  }
}

static void drawKeyboard(void) {
  char label[8];
  for (int f = 0; f < SEARCH_KB_ROWS; f++) {
    for (int c = 0; c < KB_COLUMNS[f]; c++) {
      float k = animKey[f][c];
      GfxRect base = rectKey(f, c);
      float scale = 1.0f + SEARCH_KEY_SCALE * k;
      GfxRect t = { base.x - base.w * (scale - 1.0f) * 0.5f,
                    base.y - base.h * (scale - 1.0f) * 0.5f,
                    base.w * scale, base.h * scale };
      // The focused key INVERTS (light background, dark glyph) instead of merely
      // lighting up: at sofa distance the inversion is the only contrast you can
      // see at a glance in a grid of 38 identical targets.
      gfx_color(t, NV_RADIUS_CARD, 1.0f, 1.0f, 1.0f, anim_blend(0.09f, 1.0f, k));
      const char *s;
      if (f < SEARCH_KB_ROWS - 1) {
        label[0] = KEYS[f * SEARCH_KB_COLS + c]; label[1] = 0;
        s = label;
      } else s = (c == 0) ? "space" : (c == 1 ? "delete" : "clear");
      int tom = (int)anim_blend(236.0f, 26.0f, k);
      TxtStyle st = (f < SEARCH_KB_ROWS - 1) ? TXT_TITLE3 : TXT_HEADLINE;
      TxtLine l = txt_line(st, s, tom, tom, tom, 255);
      txt_draw(l, t.x + (t.w - l.w) * 0.5f, t.y + (t.h - l.h) * 0.5f);
    }
  }
  float y = SEARCH_KB_Y + SEARCH_KB_ROWS * SEARCH_KB_STEP + 24;
  TxtLine hint = txt_line_trim(TXT_CAPTION2,
      nFilter ? "Right: results   •   Back: menu" : "OK: type   •   Back: menu",
      179, 183, 190, 255, SEARCH_KB_W);
  txt_draw(hint, SEARCH_KB_X, y);
}

// The web app's empty state: a 56/600 title and 24/400 rgb(179,179,179) support
// text. Here it sits on the RIGHT, in place of the rows, because the central band
// is taken by the keyboard.
static void drawEmpty(void) {
  const char *t1 = nQuery >= 2 ? "No titles received" : "What are we watching?";
  const char *t2 = nQuery >= 2 ? "Results from your addons appear here."
                             : "Type at least 2 letters of a film or series.";
  TxtLine l1 = txt_line(TXT_TITLE2, t1, 255, 255, 255, 255);
  TxtLine l2 = txt_line(TXT_BODY, t2, 179, 179, 179, 255);
  float cx = SEARCH_RES_X + (SEARCH_DIR - SEARCH_RES_X) * 0.5f;
  float y = SEARCH_RES_Y + 180.0f;
  txt_draw_alpha(l1, cx - l1.w * 0.5f, y, 0.96f);
  txt_draw_alpha(l2, cx - l2.w * 0.5f, y + l1.h + 18.0f, 0.85f);
  if (nQuery >= 2) {
    TxtLine help = txt_line(TXT_CAPTION2,
        "If they do not, check the connection or try another name.", 179, 183, 190, 255);
    txt_draw(help, cx - help.w * 0.5f, y + l1.h + l2.h + 42);
  }
}

static void drawResults(Uint32 now) {
  (void)now;
  hasItemFocus = 0;
  if (nFilter == 0) { drawEmpty(); return; }

  gfx_crop(SEARCH_RES_X - 8.0f, SEARCH_RES_Y - 30.0f,
              (SEARCH_DIR - SEARCH_RES_X) + 16.0f, SEARCH_RES_AREA_H + 30.0f);

  for (int r = 0; r < nFilter; r++) {
    float ry = SEARCH_RES_Y + r * NV_SEARCH_ROW_STEP - scrollY;
    if (ry > NV_SCREEN_H + 100.0f || ry + NV_SEARCH_ROW_STEP < -100.0f) continue;

    // The catalogue's title in 48/600 and the origin in 20/400 just below
    // (margin-top 4).
    TxtLine tt = txt_line_trim(TXT_TITLE3, filter[r].title, 255, 255, 255, 255,
                                  SEARCH_DIR - SEARCH_RES_X);
    txt_draw(tt, SEARCH_RES_X, ry);
    if (filter[r].origin) {
      char org[96];
      snprintf(org, sizeof org, "de %s", filter[r].origin);
      TxtLine ts = txt_line_trim(TXT_CAPTION2, org, 179, 179, 179, 255,
                                   SEARCH_DIR - SEARCH_RES_X);
      txt_draw_alpha(ts, SEARCH_RES_X, ry + NV_SEARCH_ROW_SUB, 0.95f);
    }

    float cardY = ry + NV_SEARCH_ROW_RAIL;
    // Two passes: the focused item has to sit ON TOP of its neighbours, otherwise
    // the poster beside it clips the focus ring.
    for (int passe = 0; passe < 2; passe++)
      for (int c = 0; c < filter[r].n; c++) {
        float f = animRes[r][c];
        if ((passe == 1) != (f > 0.01f)) continue;
        const CatItem *ci = cat_item(filter[r].items[c]);
        if (!ci) continue;

        float px = SEARCH_RES_X + c * NV_SEARCH_CARD_STEP - scrollX[r];
        if (px > SEARCH_DIR || px + NV_SEARCH_CARD_W < SEARCH_RES_X - NV_SEARCH_CARD_W) continue;
        GfxRect poster = { px, cardY, NV_SEARCH_CARD_W, NV_SEARCH_POSTER_H };
        // The web app's card does NOT scale on focus: it marks with a 2px border,
        // like the home.
        // The SDF's radius is a fraction of the HEIGHT, not of the smaller side:
        // `p = (uv-0.5)*vec2(asp,1.0)` makes one SDF unit h pixels on both axes.
        // Dividing by the width rounded this poster half again too much. See the
        // note on radiusInset in home.c.
        float radius = NV_SEARCH_RADIUS / NV_SEARCH_POSTER_H;
        if (f > 0.01f) {
          GfxRect b = { poster.x - 2.0f, poster.y - 2.0f,
                        poster.w + 4.0f, poster.h + 4.0f };
          gfx_color(b, radius, 0.961f, 0.961f, 0.961f, f);
        }

        const char *art = ci->poster[0] ? ci->poster
                         : (ci->backdrop[0] ? ci->backdrop : NULL);
        GLuint tex = art ? tex_get_width(art, poster.w) : 0;
        if (tex) {
          // Without the aspect ratio the 2:3 art stretches; and the poster is
          // exactly where that jumps out, because they all sit side by side.
          gfx_tex_aspect_current = tex_aspect(art);
          gfx_rect(poster, tex, GFX_CARD, f, 0.0f, 0.0f, radius, 0, 0, 0, 1);
          gfx_tex_aspect_current = 0.0f;
        } else {
          // A VISIBLE skeleton, the same as the home's: #2C2C2C. See the note
            // there — a placeholder in the background's tone reads as a broken
            // card, not as loading.
            gfx_skeleton(poster, radius, NV_COLOR_SKELETON_R, NV_COLOR_SKELETON_G,
                  NV_COLOR_SKELETON_B, 1.0f);
        }

        // The name in 28/500 white at 8 from the poster; the year in 20/400
        // rgb(179) at 4 from the name.
        TxtLine tn = txt_line_trim(TXT_CALLOUT, ci->title, 255, 255, 255, 255,
                                      NV_SEARCH_CARD_W);
        float ny = poster.y + poster.h + NV_SEARCH_NAME_GAP;
        txt_draw_alpha(tn, poster.x, ny, anim_blend(0.82f, 1.0f, f));
        if (ci->meta[0]) {
          TxtLine td = txt_line_trim(TXT_CAPTION2, ci->meta, 179, 179, 179, 255,
                                        NV_SEARCH_CARD_W);
          txt_draw_alpha(td, poster.x, ny + tn.h + NV_SEARCH_DATE_GAP, 0.92f);
        }

        if (panel == 1 && focus_index(&focusRes, r, c)) {
          itemFocus.index_ = filter[r].items[c];
          itemFocus.rect   = poster;
          itemFocus.art   = ci->backdrop[0] ? ci->backdrop : ci->poster;
          itemFocus.title = ci->title;
          itemFocus.genre = ci->genre;
          itemFocus.meta   = ci->meta;
          hasItemFocus = 1;
        }
      }
  }
  gfx_no_crop();
}

void search_draw(Uint32 now) {
  // Background #0d0d0d, measured from the web app's .search-screen-shell — darker
  // than the home's grey, and the web app uses the same tone on both.
  GfxRect screen = { 0, 0, NV_SCREEN_W, NV_SCREEN_H };
  // The screen has already been cleared with THIS VERY COLOUR by
  // glClearColor/glClear in main.c before app_draw. Painting over it was one
  // full-screen layer thrown away per frame — and the dominant cost on this GPU
  // is fill rate (gfx.c records that TWO full-screen layers dropped the Mali-G71
  // to ~40fps). Do not put it back without first changing the clear colour.
  (void)screen;
  drawHeader(now);
  drawKeyboard();
  drawResults(now);
}
