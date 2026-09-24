// Search, ported from NuvioWeb's screen (read off the live stylesheet,
// 2026-09-19).
//
// ------------------------------------------------------------------------
// WHAT CHANGED, AND WHY
//
// This screen used to state, at the top of this very file, that the web app's
// arrangement "CANNOT be ported: this app is pure SDL and there is no IME to
// call — with no on-screen keyboard there is no way to type". That was the
// reason a 6x7 grid of letters occupied the left third of the screen while the
// web app put the results there.
//
// It is no longer true, and the whole shape of the screen follows from that.
// SDL has a text-input API, and where the platform has a keyboard behind it —
// which on this TV means LG's own, the one the web build already gets — the app
// does not have to draw one. See ime.h. So:
//
//   1. The results are now where the web app puts them: full width, starting
//      under the field, one horizontal row per addon catalogue.
//   2. The field is a real text field. OK on it raises the TV's keyboard, with
//      the owner's layout, their typing history and the remote's microphone —
//      none of which a grid drawn here could ever offer.
//   3. The empty state is the web app's: "Recent searches" as chips, saved
//      across sessions, or "No Results" once something has been typed.
//   4. A row that filled up ends with the web app's round "See All" button,
//      which opens the catalogue behind it.
//
// CONFIRMED ON THE C3: the keyboard comes up, the typing arrives, the search
// runs. The measurement and its evidence are at the top of ime.h.
//
// THE GRID IS STILL HERE ANYWAY, and deleting it would be the mistake — the
// reason has simply changed from "we do not know yet" to "not every device is
// this one". ime_usable() answers at RUNTIME and it can answer no in two ways:
// the backend says it has no screen keyboard, or it says it has one and then
// never shows it (ime.c times that out). In either case this screen falls back
// to the grid, in the band the web app leaves empty on the left, and the
// results move right — one branch, two numbers, and a search that still works.
// NUVIO_NO_IME=1 is how you look at that half without owning such a device.
//
// DESIGN NOTE kept from the grid's own history, because it still applies to the
// fallback: it is a GRID and not tvOS's single line. The horizontal strip is
// handsome and fits in little height, but it costs dearly on a D-pad: 38 keys in
// ONE dimension, so the average distance between two letters is ~13 presses and
// the worst case is over 37. The 6x7 grid puts the same key at 5+6 presses at
// most and ~5 on average.
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
#include "seeall.h"
#include "data.h"
#include "ime.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>

// --- The fallback keyboard (see the top) -------------------------------------
#define SEARCH_KEY_W     74.0f
#define SEARCH_KEY_GAP   12.0f
#define SEARCH_KB_COLS      6
#define SEARCH_KB_ROWS      7        // 6 rows of A-Z/0-9 + 1 of space/delete/clear
#define SEARCH_KB_STEP   (SEARCH_KEY_W + SEARCH_KEY_GAP)
#define SEARCH_KB_W       (SEARCH_KB_COLS * SEARCH_KEY_W + (SEARCH_KB_COLS - 1) * SEARCH_KEY_GAP)
// How much the focused key grows. Smaller than a poster's on purpose: the key is
// small and an immediate neighbour of the others, and at 14% it invades the 12px gap.
#define SEARCH_KEY_SCALE 0.10f

#define SEARCH_MAX_QUERY 48
#define SEARCH_MAX_ROWS FOCUS_MAX_ROWS
#define SEARCH_MAX_PER_FILTER  12
// The chips wrap, and six of them at 1776 usable will not need more than this.
// It is a ceiling for the focus grid, not a layout decision.
#define SEARCH_HIST_ROWS  3

#define SEARCH_HIST_FILE "search.txt"

// The four places the focus can be. FIELD and KEYS are mutually exclusive — one
// of the two exists depending on ime_usable() — and HIST and RES never both
// carry content, because the chips ARE the empty state.
enum { PANEL_FIELD, PANEL_KEYS, PANEL_HIST, PANEL_RES };

static Focus  focusKb;
static Focus  focusRes;
static Focus  focusHist;
static int   panel;
// Which of the header's two controls has the focus: 0 the field, 1 the voice
// button. It is not a Focus because a row of two needs no column memory.
static int   headCol;
static char  query[SEARCH_MAX_QUERY];
static int   nQuery = 0;
static char queryFiltered[SEARCH_MAX_QUERY];

// Results grouped by CATALOGUE, as in the web app: one row per catalogue that
// had at least one matching title. The items are indices into the global
// catalogue.
//
// The strings are COPIED and not pointed at. They used to be `const char *`
// into discover.c's target table and into the CatRow array, and the CatRow one
// is a live hazard: cat_append_batch reallocates the catalogue, this very
// function calls it, and any pointer taken before that call is dangling after
// it. 22 KB of static buffer is the cheap side of that trade.
static struct {
  char title[96];       // the catalogue's name ("Top 100 Today - Film")
  char origin[64];      // the addon it came from; empty when not known
  char base[600];       // the catalogue itself, for the "See All" button …
  char kind[8];         // … which needs all three, or it cannot be drawn
  char catId[96];
  int items[SEARCH_MAX_PER_FILTER];
  int n;
  int seeAll;           // 1 when the row ends with the "See All" button
} filter[SEARCH_MAX_ROWS];
static int nFilter = 0;
static int wantsExit = 0;
static int request = -1;             // the chosen catalogue index, -1 = none
static int requestDiscover = 0;      // the compass was pressed
static int requestMenu = 0;          // LEFT off the screen's left edge
static float animKey[SEARCH_KB_ROWS][SEARCH_KB_COLS];
// +1 column for the "See All" button at the end of the row.
static float animRes[SEARCH_MAX_ROWS][SEARCH_MAX_PER_FILTER + 1];
static float animField;
static float animDiscover;
static float animChip[NV_SEARCH_HIST_MAX];
static float scrollY = 0.0f, scrollTarget = 0.0f;
static float scrollX[SEARCH_MAX_ROWS];
static HomeItem itemFocus;
static int   hasItemFocus = 0;

// The owner's recent terms, newest first — localStorage's `nuvio_search_history`
// on the web, a file here. Saved on ENTER only, like the web app: saving on
// every keystroke would fill the list with the prefixes of one word.
static char hist[NV_SEARCH_HIST_MAX][SEARCH_MAX_QUERY];
static int  nHist = 0;
// Where each chip landed this frame. Filled by layoutChips(), read by both the
// drawing and the focus, so the two cannot disagree about which pill is under
// the cursor.
static GfxRect chipRect[NV_SEARCH_HIST_MAX];
static int     chipRow[NV_SEARCH_HIST_MAX];
static int     chipColumn[NV_SEARCH_HIST_MAX];
static int     chipRows = 0;
static int     chipCols[SEARCH_HIST_ROWS];

static const int KB_COLUMNS[SEARCH_KB_ROWS] = { 6, 6, 6, 6, 6, 6, 3 };
// Lower case as on the device: the field shows what was typed, and a query in
// capitals reads as shouting. The comparison ignores case either way.
static const char *KEYS =
  "abcdefghijklmnopqrstuvwxyz0123456789";   // 36 = 6 rows x 6 columns

// --- Where the content starts ------------------------------------------------
// The ONE branch the fallback keyboard costs. With the TV's keyboard the screen
// is the web app's: everything at 64. Without it, the grid takes the band the
// web app leaves empty and the rows start to its right, one header gap away.
static float contentX(void) {
  if (ime_usable()) return NV_SEARCH_X;
  return NV_SEARCH_X + SEARCH_KB_W + NV_SEARCH_HEAD_GAP;
}

// The header is a flex row: the field takes what is left, then a 24 gap, then
// the 100x100 Discover button. `.search-voice-btn` is NOT drawn — see the note
// on drawDiscover for which of the web app's two buttons this port carries and
// why the other one cannot exist here.
static GfxRect rectDiscover(void) {
  GfxRect v = { NV_SEARCH_RIGHT - NV_SEARCH_BTN, NV_SEARCH_HEAD_Y,
                NV_SEARCH_BTN, NV_SEARCH_BTN };
  return v;
}

static GfxRect rectField(void) {
  GfxRect f = { NV_SEARCH_X, NV_SEARCH_HEAD_Y,
                NV_SEARCH_RIGHT - NV_SEARCH_X - NV_SEARCH_BTN - NV_SEARCH_BTN_GAP,
                NV_SEARCH_HEAD_H };
  return f;
}

// WHERE A LINE OF TEXT GOES SO THAT IT READS CENTRED IN `h`.
//
// Not (h - l.h) * 0.5. A TxtLine is a box the height of the FONT — ascent plus
// descent — and its centre is not where the eye puts the middle of a word:
// Inter carries far more descent than a lowercase word ever uses, so centring
// the BOX pushes the ink down. MEASURED on the C3, in this very field: the
// magnifier's ink centred on 177.5 (the field's own centre is 178.0) while the
// x-height band of the query sat at 181.0 — three pixels of disagreement
// between an icon and the word beside it, which is what "doesn't look properly
// aligned" was.
//
// So the CAP BAND is centred instead, cap-top to baseline, which is the band
// the eye actually reads. txt_cap_inset and txt_baseline exist for exactly this
// and are measured off the face, so this stays right if the font is ever
// changed. A word of pure lowercase still sits a shade low against a capital —
// that is true of every typeface and the browser does it too — but the gap to
// the icon closes.
static float textCenterY(TxtStyle style, float top, float h) {
  return top + h * 0.5f - (txt_cap_inset(style) + txt_baseline(style)) * 0.5f;
}

// --- Normalisation -----------------------------------------------------------
// Folds an accented Latin letter (the second byte of a UTF-8 sequence starting
// with 0xC3) onto the matching ASCII letter. Without this, searching for
// "fundacao" does not find "Fundação" — and with the TV's keyboard that matters
// MORE than it did with the grid, not less: the grid had no cedilla to press, so
// the query was always plain; the LG keyboard has the whole layout, so now both
// sides of the comparison can arrive accented.
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

// --- Recent searches ---------------------------------------------------------
// One term per line. A plain text file and not JSON because that is all the
// shape needs, and because a half-written line is then one lost term rather than
// a parse failure that loses the lot.
static void historyLoad(void) {
  char *buf = data_read(SEARCH_HIST_FILE);
  char *p = buf;
  nHist = 0;
  if (!buf) return;
  while (*p && nHist < NV_SEARCH_HIST_MAX) {
    char *end = strchr(p, '\n');
    size_t n = end ? (size_t)(end - p) : strlen(p);
    if (n > 0 && n < sizeof hist[0]) {
      memcpy(hist[nHist], p, n);
      hist[nHist][n] = 0;
      nHist++;
    }
    if (!end) break;
    p = end + 1;
  }
  free(buf);
}

static void historySave(void) {
  char buf[NV_SEARCH_HIST_MAX * SEARCH_MAX_QUERY + 8];
  int k = 0, i;
  buf[0] = 0;
  for (i = 0; i < nHist; i++)
    k += snprintf(buf + k, sizeof buf - (size_t)k, "%s\n", hist[i]);
  data_write(SEARCH_HIST_FILE, buf);
}

// Newest first, no repeats, at most NV_SEARCH_HIST_MAX — saveSearchHistory in
// searchScreen.js, including its "shorter than 2 characters is not a search"
// rule, which is the same threshold the filter itself uses.
static void historyAdd(const char *term) {
  char norm[SEARCH_MAX_QUERY * 2], other[SEARCH_MAX_QUERY * 2];
  int i, w;
  if (!term || (int)strlen(term) < 2) return;
  normalize(term, norm, sizeof norm);
  for (i = 0, w = 0; i < nHist; i++) {
    normalize(hist[i], other, sizeof other);
    if (strcmp(norm, other)) {
      if (w != i) snprintf(hist[w], sizeof hist[w], "%s", hist[i]);
      w++;
    }
  }
  nHist = w;
  if (nHist > NV_SEARCH_HIST_MAX - 1) nHist = NV_SEARCH_HIST_MAX - 1;
  for (i = nHist; i > 0; i--) snprintf(hist[i], sizeof hist[i], "%s", hist[i - 1]);
  snprintf(hist[0], sizeof hist[0], "%s", term);
  nHist++;
  historySave();
}

// BACK TO THE HEADER, from wherever the focus was. One function because it is
// two facts, not one: which panel, and that the landing place inside it is the
// FIELD and never the microphone. Leaving headCol alone would drop the focus on
// the voice button whenever the owner came back up from a poster, which is not
// where they were when they left.
static void toHeader(void) {
  panel = ime_usable() ? PANEL_FIELD : PANEL_KEYS;
  headCol = 0;
}

// --- Filter ------------------------------------------------------------------
// One row per CATALOGUE, exactly as the web app builds `.search-results-row`.
static void refilter(void) {
  char target[SEARCH_MAX_QUERY * 2];
  int previous = -1, sameQuery = !strcmp(queryFiltered, query);
  if (sameQuery && panel == PANEL_RES && focusRes.row < nFilter &&
      focusRes.column < filter[focusRes.row].n)
    previous = filter[focusRes.row].items[focusRes.column];
  snprintf(queryFiltered, sizeof queryFiltered, "%s", query);
  normalize(query, target, sizeof target);
  nFilter = 0;
  // Fewer than 2 characters = the empty state, like the web app. Searching with
  // one letter returns the whole collection and does not help.
  if ((int)strlen(target) < 2) {
    if (panel == PANEL_RES) toHeader();
    return;
  }

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
          posNew[nNew] = found++;   // reserve the place; the index comes later
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
        snprintf(filter[nFilter].title, sizeof filter[nFilter].title, "%s",
                 disc_search_target_title(targetIdx));
        snprintf(filter[nFilter].origin, sizeof filter[nFilter].origin, "%s",
                 disc_search_target_addon(targetIdx));
        snprintf(filter[nFilter].base, sizeof filter[nFilter].base, "%s",
                 disc_search_target_base(targetIdx));
        snprintf(filter[nFilter].kind, sizeof filter[nFilter].kind, "%s",
                 disc_search_target_kind(targetIdx));
        snprintf(filter[nFilter].catId, sizeof filter[nFilter].catId, "%s",
                 disc_search_target_id(targetIdx));
        filter[nFilter].n = found;
        // THE BUTTON APPEARS ONLY ON A FULL ROW, and that is a guess dressed up
        // honestly rather than a fact. The web app knows whether there is more
        // (`result.data.hasMore`); nothing on this side does — disc_search_*
        // caps at SEARCH_PER_TARGET and reports the cap, not the total. A row
        // that came back at its ceiling almost certainly has more behind it; a
        // row of three does not. Offering "See All" on the row of three would be
        // a button that opens a screen showing the same three.
        filter[nFilter].seeAll = (found >= SEARCH_MAX_PER_FILTER &&
                                  filter[nFilter].base[0] && filter[nFilter].catId[0]);
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
    snprintf(filter[nFilter].title, sizeof filter[nFilter].title, "%s", cf->title);
    // `catalogAddonNameEnabled` decides the "from <addon>" line under the row's
    // title. The data does NOT exist on this side: `CatRow` stores the key, the
    // title, the type and the window into the array — the addon's name lives in
    // addons.c and the row does not carry it. Until discover.c passes that field
    // along, the line is not drawn; writing the TYPE ("movie") there instead would
    // be worse than its absence, because it would read as the origin.
    filter[nFilter].origin[0] = 0;
    snprintf(filter[nFilter].base,  sizeof filter[nFilter].base,  "%s", cf->base);
    snprintf(filter[nFilter].kind,  sizeof filter[nFilter].kind,  "%s", cf->kind);
    snprintf(filter[nFilter].catId, sizeof filter[nFilter].catId, "%s", cf->catId);
    filter[nFilter].n = found;
    // These rows are a FILTER over what the home already holds, so `found` is
    // bounded by what is in memory and never says anything about the catalogue's
    // real length. The button still means what it means everywhere else — "the
    // catalogue behind this row" — and here it is the only way to reach the rest
    // of it, so a full row gets one for the same reason as above.
    filter[nFilter].seeAll = (found >= SEARCH_MAX_PER_FILTER &&
                              filter[nFilter].base[0] && filter[nFilter].catId[0]);
    nFilter++;
  }

  int cols[SEARCH_MAX_ROWS];
  for (int i = 0; i < nFilter; i++) cols[i] = filter[i].n + filter[i].seeAll;
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
  if (nFilter == 0 && panel == PANEL_RES) toHeader();
  memset(animRes, 0, sizeof animRes);
  if (!sameQuery) {
    memset(scrollX, 0, sizeof scrollX);
    scrollY = scrollTarget = 0.0f;
  }
}

// --- Keys --------------------------------------------------------------------
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
  r.y = NV_SEARCH_BODY_Y + row * (SEARCH_KEY_W + SEARCH_KEY_GAP);
  r.h = SEARCH_KEY_W;
  if (row < SEARCH_KB_ROWS - 1) {
    r.x = NV_SEARCH_X + column * SEARCH_KB_STEP;
    r.w = SEARCH_KEY_W;
  } else {
    r.w = (SEARCH_KB_W - 2 * SEARCH_KEY_GAP) / 3;
    r.x = NV_SEARCH_X + column * (r.w + SEARCH_KEY_GAP);
  }
  return r;
}

// --- The chips ---------------------------------------------------------------
// `display: flex; flex-wrap: wrap; gap: 24px` on a block as wide as the content.
// Laid out once per frame into chipRect, and the focus grid is rebuilt from the
// SAME pass — a chip the eye sees on the second line and the focus thinks is on
// the first is the defect this avoids.
static void layoutChips(void) {
  float x0 = contentX();
  float wide = NV_SEARCH_RIGHT - x0;
  float y = NV_SEARCH_BODY_Y + NV_LD_SRCH_NAME + NV_SEARCH_HIST_GAP;
  float x = x0;
  int i, row = 0, column = 0;
  chipRows = 0;
  for (i = 0; i < SEARCH_HIST_ROWS; i++) chipCols[i] = 0;
  for (i = 0; i < nHist; i++) {
    TxtLine l = txt_line(TXT_CALLOUT, hist[i], 255, 255, 255, 255);
    float w = NV_SEARCH_CHIP_PADX * 2 + NV_SEARCH_CHIP_BORDER * 2 +
              NV_SEARCH_CHIP_ICON + NV_SEARCH_CHIP_ICOGAP + (float)l.w;
    if (column > 0 && x + w > x0 + wide) {
      if (row + 1 >= SEARCH_HIST_ROWS) break;
      row++; column = 0;
      x = x0;
      y += NV_SEARCH_CHIP_H + NV_SEARCH_CHIP_GAP;
    }
    chipRect[i].x = x; chipRect[i].y = y;
    chipRect[i].w = w; chipRect[i].h = NV_SEARCH_CHIP_H;
    chipRow[i] = row; chipColumn[i] = column;
    chipCols[row] = ++column;
    x += w + NV_SEARCH_CHIP_GAP;
    chipRows = row + 1;
  }
}

static int chipAt(int row, int column) {
  int i;
  for (i = 0; i < nHist; i++)
    if (chipRow[i] == row && chipColumn[i] == column) return i;
  return -1;
}

// The chips are the empty state, so they only exist while there is nothing else
// in their place.
static int histShown(void) { return nHist > 0 && nFilter == 0 && nQuery < 2; }

static void applyHistory(int i) {
  if (i < 0 || i >= nHist) return;
  snprintf(query, sizeof query, "%s", hist[i]);
  nQuery = (int)strlen(query);
  historyAdd(query);
  refilter();
  if (nFilter > 0) panel = PANEL_RES; else toHeader();
}

// --- Life cycle --------------------------------------------------------------
int search_start(void) {
  focus_start(&focusKb, SEARCH_KB_ROWS, KB_COLUMNS);
  toHeader();
  wantsExit = 0; request = -1; requestDiscover = 0; requestMenu = 0;
  nQuery = 0; query[0] = 0;
  queryFiltered[0] = 0;
  scrollY = scrollTarget = 0.0f;
  hasItemFocus = 0;
  animField = 0.0f;
  animDiscover = 0.0f;
  headCol = 0;
  memset(animKey, 0, sizeof animKey);
  memset(animRes, 0, sizeof animRes);
  memset(animChip, 0, sizeof animChip);
  memset(scrollX, 0, sizeof scrollX);
  historyLoad();
  { int one = 1; focus_start(&focusHist, 1, &one); }
  refilter();
  return 1;
}

void search_resume(void) {
  wantsExit = 0; request = -1; requestDiscover = 0; requestMenu = 0;
  hasItemFocus = 0;
  // A keyboard left up by the last visit is not the viewer's: text input is
  // global to the window (ime.h), and it would eat the D-pad on the way in.
  ime_close();
  historyLoad();
  // The results hold CATALOGUE INDICES, and a rebuild while the screen was away
  // moves them. Filtering the same query again keeps the focused title (refilter
  // looks for it) and points every card at the right one.
  refilter();
}

void search_shutdown(void) {
  hasItemFocus = 0;
  // The keyboard must not outlive the screen. See ime.h: text input is global to
  // the window, and left on, the home's D-pad presses can arrive twice.
  ime_close();
}

int  search_wants_exit(void) { return wantsExit; }
int  search_requested_menu(void) { int v = requestMenu; requestMenu = 0; return v; }

int search_requested_discover(void) {
  int v = requestDiscover; requestDiscover = 0; return v;
}

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

// Everything Back has to undo before it is allowed to close the screen. The
// order is the reverse of the moves that got here, which is the only order that
// does not lose the typed text without being asked to.
static void goBack(void) {
  if (ime_is_open())          { ime_close(); return; }
  if (panel == PANEL_RES)     { toHeader(); return; }
  if (panel == PANEL_HIST)    { toHeader(); return; }
  wantsExit = 1;
}

// ENTER on the field: the term joins the history and the focus moves to the
// results, which is what runSearchFromInput does on the web with
// `autoFocusResults: true`.
static void submit(void) {
  ime_close();
  historyAdd(query);
  refilter();
  if (nFilter > 0) panel = PANEL_RES;
}

void search_event(const SDL_Event *e) {
  if (e->type == SDL_QUIT) { wantsExit = 1; return; }

  // THE TV'S KEYBOARD TYPES HERE. SDL_TEXTINPUT arrives whenever text input is
  // on, whatever raised it, and it is the ONLY path by which an accented or
  // non-Latin character can reach the query — SDL_KEYDOWN carries a keycode,
  // not a character. It is taken before the key handling below so a backspace
  // from the system keyboard deletes a CHARACTER and not a byte.
  if (ime_is_open() && ime_edit(e, query, &nQuery, SEARCH_MAX_QUERY)) {
    refilter();
    return;
  }

  if (e->type != SDL_KEYDOWN) return;
  SDL_Keycode k = e->key.keysym.sym;

  if (k == SDLK_AC_BACK || k == SDLK_ESCAPE) { goBack(); return; }

  // An arrow that REACHES the app with the keyboard asked for but not on screen
  // means the system keyboard is no longer holding the D-pad. Lower it here and
  // let the arrow do what it does on the field — down to the results, right to
  // the compass, left to the menu — instead of swallowing it until Back.
  if (ime_is_open() && !ime_shown() &&
      (k == SDLK_UP || k == SDLK_DOWN || k == SDLK_LEFT || k == SDLK_RIGHT)) {
    printf("[search] arrow with the keyboard down: leaving the field\n");
    ime_close();
  }

  if (panel == PANEL_FIELD) {
    switch (k) {
      case SDLK_RETURN: case SDLK_KP_ENTER:
        // OK on a field that is not yet taking text RAISES the keyboard; OK
        // again, with it up, is the submit. Two meanings for one key, and they
        // cannot be confused because only one of the two states is ever current.
        if (headCol == 1) {
          // The keyboard must come DOWN before another screen takes over, or it
          // stays up over a grid that has no field in it. See ime.h.
          ime_close();
          requestDiscover = 1;
        }
        else if (!ime_is_open()) ime_open(rectField());
        else                     submit();
        break;
      case SDLK_BACKSPACE:
        // Reachable only with the keyboard DOWN — with it up, ime_edit above has
        // already taken it.
        if (headCol == 0 && nQuery > 0) {
          ime_edit(e, query, &nQuery, SEARCH_MAX_QUERY); refilter();
        }
        break;
      case SDLK_RIGHT:
        if (!ime_is_open()) headCol = 1;
        break;
      case SDLK_LEFT:
        // From the field itself LEFT is the edge of the screen: the side menu.
        if (!ime_is_open()) { if (headCol == 0) requestMenu = 1; headCol = 0; }
        break;
      case SDLK_DOWN:
        if (ime_is_open()) break;          // the keyboard owns the D-pad
        if (nFilter > 0)      panel = PANEL_RES;
        else if (histShown()) { panel = PANEL_HIST; focusHist.row = focusHist.column = 0; }
        break;
      default: break;
    }
    return;
  }

  if (panel == PANEL_KEYS) {
    if (k == SDLK_BACKSPACE) {
      if (nQuery > 0) { query[--nQuery] = 0; refilter(); }
      return;
    }
    // A REAL KEYBOARD, on the Mac preview and on anything the owner plugs in.
    // It is not the fallback's reason for existing, but refusing it here would
    // make the preview unusable for the one screen that takes text.
    if (!(e->key.keysym.mod & (KMOD_CTRL | KMOD_ALT | KMOD_GUI)) &&
        ((k >= SDLK_a && k <= SDLK_z) || (k >= SDLK_0 && k <= SDLK_9) || k == SDLK_SPACE)) {
      if (nQuery + 1 < SEARCH_MAX_QUERY && (k != SDLK_SPACE || nQuery)) {
        query[nQuery++] = (char)k; query[nQuery] = 0; refilter();
      }
      return;
    }
    switch (k) {
      case SDLK_LEFT:
        if (focusKb.column == 0) requestMenu = 1;
        else focus_move_grid(&focusKb, -1, 0);
        break;
      case SDLK_RIGHT:
        // Going past the keyboard's LAST column enters whatever is to the right.
        // It is the only bridge out of the grid, and that is why it must not fail
        // silently: with nothing over there, the focus stays where it is.
        if (focusKb.column >= KB_COLUMNS[focusKb.row] - 1) {
          if (nFilter > 0)      panel = PANEL_RES;
          else if (histShown()) { panel = PANEL_HIST; focusHist.row = focusHist.column = 0; }
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

  if (panel == PANEL_HIST) {
    switch (k) {
      case SDLK_LEFT:
        if (focusHist.column == 0) {
          if (ime_usable()) requestMenu = 1; else panel = PANEL_KEYS;
        }
        else focus_move_grid(&focusHist, -1, 0);
        break;
      case SDLK_RIGHT: focus_move_grid(&focusHist, 1, 0); break;
      case SDLK_UP:
        if (focusHist.row == 0) toHeader();
        else focus_move_grid(&focusHist, 0, -1);
        break;
      case SDLK_DOWN:  focus_move_grid(&focusHist, 0, 1); break;
      case SDLK_RETURN: case SDLK_KP_ENTER:
        applyHistory(chipAt(focusHist.row, focusHist.column));
        break;
      case SDLK_BACKSPACE: goBack(); break;
      default: break;
    }
    return;
  }

  switch (k) {
    case SDLK_LEFT:
      // Going back from the results' first column returns the focus to whatever
      // is on the left — the grid, or nothing but the field above.
      if (focusRes.column == 0) toHeader();
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
    case SDLK_UP:
      // Off the top of the first row is the field, exactly as the web app's
      // focus engine leaves the track upwards into the header.
      if (focusRes.row == 0) toHeader();
      else focus_move(&focusRes, 0, -1);
      break;
    case SDLK_DOWN: focus_move(&focusRes, 0,  1); break;
    case SDLK_BACKSPACE: goBack(); break;
    case SDLK_RETURN: case SDLK_KP_ENTER:
      if (focusRes.row < nFilter) {
        int c = focusRes.column;
        if (c < filter[focusRes.row].n) request = filter[focusRes.row].items[c];
        else if (filter[focusRes.row].seeAll)
          seeall_open(filter[focusRes.row].base, filter[focusRes.row].kind,
                      filter[focusRes.row].catId, filter[focusRes.row].title);
      }
      break;
    default: break;
  }
}

void search_update(float dt, Uint32 now) {
  (void)now;
  ime_pump();
  // ime_pump may have just given up on a keyboard that never came (see ime.c).
  // The field then has nothing behind it, so the focus has to move somewhere it
  // can still type — otherwise the owner is left on a dead control.
  if (panel == PANEL_FIELD && !ime_usable()) panel = PANEL_KEYS;

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

  { float wantField = (panel == PANEL_FIELD && headCol == 0) ? 1.0f : 0.0f;
    float wantVoice  = (panel == PANEL_FIELD && headCol == 1) ? 1.0f : 0.0f;
    animField = anim_spring(animField, wantField, dt,
                            wantField > animField ? NV_SPRING_FOCUS : NV_SPRING_BLUR);
    animDiscover = anim_spring(animDiscover, wantVoice, dt,
                            wantVoice > animDiscover ? NV_SPRING_FOCUS : NV_SPRING_BLUR); }
  for (int f = 0; f < SEARCH_KB_ROWS; f++)
    for (int c = 0; c < KB_COLUMNS[f]; c++) {
      float target = (panel == PANEL_KEYS && focus_index(&focusKb, f, c)) ? 1.0f : 0.0f;
      animKey[f][c] = anim_spring(animKey[f][c], target, dt,
                                  target > animKey[f][c] ? NV_SPRING_FOCUS : NV_SPRING_BLUR);
    }
  for (int i = 0; i < nHist; i++) {
    float target = (panel == PANEL_HIST && chipRow[i] == focusHist.row &&
                    chipColumn[i] == focusHist.column) ? 1.0f : 0.0f;
    animChip[i] = anim_spring(animChip[i], target, dt,
                              target > animChip[i] ? NV_SPRING_FOCUS : NV_SPRING_BLUR);
  }
  for (int r = 0; r < SEARCH_MAX_ROWS; r++)
    for (int c = 0; c < SEARCH_MAX_PER_FILTER + 1; c++) {
      float target = (panel == PANEL_RES && focus_index(&focusRes, r, c)) ? 1.0f : 0.0f;
      animRes[r][c] = anim_spring(animRes[r][c], target, dt,
                                target > animRes[r][c] ? NV_SPRING_FOCUS : NV_SPRING_BLUR);
    }

  // The focus grid for the chips is rebuilt from the LAST laid-out frame: the
  // wrap depends on the measured width of each term, which only the drawing
  // knows. One frame of lag on a list that changes when the owner presses ENTER
  // is not observable; measuring the text twice per frame would be.
  if (chipRows > 0) {
    int saveRow = focusHist.row, saveColumn = focusHist.column;
    focus_start(&focusHist, chipRows, chipCols);
    focusHist.row = saveRow < chipRows ? saveRow : chipRows - 1;
    focusHist.column = saveColumn < chipCols[focusHist.row]
                     ? saveColumn : chipCols[focusHist.row] - 1;
    if (focusHist.column < 0) focusHist.column = 0;
  }

  // Scrolls only as far as needed for the focused row to fit whole in the usable
  // area — scrolling in proportion to the index would hide the first row before
  // the user had got to it.
  float areaH = NV_SCREEN_H - NV_SEARCH_TOP - NV_SEARCH_BODY_Y;
  if (panel == PANEL_RES && nFilter > 0) {
    float top = focusRes.row * NV_SEARCH_ROW_STEP;
    float base = top + NV_SEARCH_ROW_RAIL + NV_SEARCH_CARD_H;
    if (top - scrollTarget < 0.0f)          scrollTarget = top;
    if (base - scrollTarget > areaH)        scrollTarget = base - areaH;

    // Horizontal scrolling of the focused row, the same rule as the home's.
    int r = focusRes.row;
    float x0 = contentX() + NV_SEARCH_TRACK_X;
    float util = NV_SCREEN_W - x0;
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
// "Search", .library-page-title: 48/600 with a 1px letter-spacing. The spacing
// is not decoration — txt_tracking exists because SDL_ttf has none, and without
// it the title sits noticeably tighter than the same string in the web app.
static void drawTitle(void) {
  txt_tracking(TXT_TITLE3, "Search", 255, 255, 255,
               NV_SEARCH_X, NV_SEARCH_TOP, 1.0f, NV_SEARCH_TITLE_LS);
}

// A FILLED PILL WITH A 1px EDGE OF ANOTHER COLOUR, drawn as TWO SHAPES and not
// as a stroke.
//
// WHY, and it is worth the extra rect. A 1px border asked for with
// GFX_RING_INSET is a band one pixel wide, and one pixel is not enough room to
// antialias: the shader's ramp gets clamped to half the band (see the note on
// GFX_RING_INSET in gfx.c) and the result still lands differently on each pixel
// as the contour turns. MEASURED on the C3, on the unfocused voice button —
// 51 along the straight run, 49, 42, then 37 at the 45-degree diagonal, against
// an ideal of 51. A bright hairline that fades out four times around a circle
// is exactly what reads as "pixelated".
//
// Two filled shapes have no such band. The outer one is painted in the edge
// colour, the inner one inset by a pixel in the fill colour, and the "border"
// is simply what is left showing between them. Each shape gets edgeAA's full
// 1.25px ramp against its own neighbour, which is the case that shader is good
// at. It is also what the browser does with `border: 1px solid` — the border is
// a region, not a line.
//
// The cost is one extra rounded rect per control. These are the header's two
// and the See All circle; none of them is large, and the fill-rate note in
// gfx.c is about FULL-screen layers.
static void pillFilled(GfxRect r, float edge, float fill, float a) {
  gfx_color(r, NV_SEARCH_PILL, edge, edge, edge, a);
  { GfxRect in = { r.x + 1.0f, r.y + 1.0f, r.w - 2.0f, r.h - 2.0f };
    gfx_color(in, NV_SEARCH_PILL, fill, fill, fill, a); }
}

// The field. A PILL with the magnifier inside it, the text at 88 from the edge,
// and a cross on the right once there is something to clear.
static void drawField(Uint32 now) {
  GfxRect field = rectField();
  float f = animField;
  (void)0;
  float cx = field.x + NV_SEARCH_ICON_X + NV_SEARCH_ICON * 0.5f;
  float cy = field.y + field.h * 0.5f;
  float tx = field.x + NV_SEARCH_FIELD_PADX;
  float textW = field.w - 2 * NV_SEARCH_FIELD_PADX;

  // #222 body inside a 1px #333 edge. The edge does NOT animate: on focus the
  // 3px inset white band below is drawn over the same outer shape and covers it
  // completely, so there is nothing to cross-fade.
  pillFilled(field, 0.2f, 0.133f, 1.0f);   // --border-color over --card-bg
  // The resting border is 1px #333; focused it becomes white at 10% and an INSET
  // 3px white band takes over as the mark.
  //
  // INSET, and this is the correction the old screen needed. There used to be a
  // gfx_color over `field + 2px` here, and gfx_color FILLS: white at 22% washed
  // out the whole field. The arithmetic matches what was measured on screen:
  // 0.133 x 0.78 + 0.961 x 0.22 = 0.315, that is #505050 in place of the #222222
  // the line above had just painted. The field read as a DISABLED control, and
  // the placeholder almost vanished inside it. The live sheet has since moved to
  // `box-shadow: inset 0 0 0 3px` for the same reason.
  //
  // THREE pixels is wide enough for the ramp, so this one stays a stroke — it
  // measured 246 evenly right around the cap. It is only the 1px case that has
  // to be drawn as two shapes; see pillFilled.
  if (f > 0.01f)
    gfx_rect(field, 0, GFX_RING_INSET, 0, NV_SEARCH_RING / field.h, 0,
             NV_SEARCH_PILL, 1.0f, 1.0f, 1.0f, 0.96f * f);

  // --text-tertiary at rest, --text-secondary focused: the sheet transitions the
  // icon's colour with the field's, so it is part of the same cue and not a
  // separate ornament.
  { float luma = anim_blend(0.502f, 0.702f, f);
    GfxRect ic = { cx - NV_SEARCH_ICON * 0.5f, cy - NV_SEARCH_ICON * 0.5f,
                   NV_SEARCH_ICON, NV_SEARCH_ICON };
    gfx_icon(ic, "search_glass", luma, luma, luma, 1.0f); }

  if (nQuery) {
    // rgb(179), not white: `.search-input-field` takes its colour from
    // --addons-text-secondary, which resolves to --text-secondary. It looks like
    // an oversight in the sheet and is not — the field is a readout of a term,
    // not a heading, and the placeholder one step below it at rgb(128) needs the
    // room underneath.
    TxtLine l = txt_line_trim(TXT_CALLOUT, query, 179, 179, 179, 255,
                              textW - NV_SEARCH_CLEAR - 12.0f);
    txt_draw(l, tx, textCenterY(TXT_CALLOUT, field.y, field.h));
    tx += (float)l.w + 6.0f;
    // .search-clear-btn, shown by `.search-input-field.has-value`. It is drawn
    // and NOT focusable, exactly as on the web (`tabindex="-1"`): clearing is
    // what the keyboard's own delete does, and a target the D-pad can land on
    // between the field and the results would be one more stop on the way down.
    { float luma = anim_blend(0.502f, 0.702f, f);
      GfxRect x = { field.x + field.w - NV_SEARCH_CLEAR_X - NV_SEARCH_CLEAR,
                    cy - NV_SEARCH_CLEAR * 0.5f, NV_SEARCH_CLEAR, NV_SEARCH_CLEAR };
      gfx_icon(x, "search_clear", luma, luma, luma, 1.0f); }
  } else {
    // The web app's placeholder, verbatim, at --text-tertiary.
    TxtLine l = txt_line(TXT_CALLOUT, "Search movies & series", 128, 128, 128, 255);
    txt_draw(l, tx, textCenterY(TXT_CALLOUT, field.y, field.h));
  }

  // The caret. It blinks only while the field HAS the focus and the system
  // keyboard is up — a caret in a field nothing is typing into is a lie about
  // where the next key will go, and on the fallback layout the next key goes to
  // the grid.
  if (panel == PANEL_FIELD && headCol == 0 && ime_is_open() && (now / 500) % 2 == 0) {
    GfxRect cur = { tx, field.y + 26.0f, 3.0f, field.h - 52.0f };
    gfx_color(cur, 0.0f, 1.0f, 1.0f, 1.0f, 0.85f);
  }
}

// THE DISCOVER BUTTON. `.search-discover-btn`, the compass beside the field: it
// opens the Discover screen, where a catalogue is browsed by hand instead of
// searched by name. See discoverui.h.
//
// IT IS THE ONE OF THE WEB APP'S TWO HEADER BUTTONS THAT THIS PORT CAN CARRY.
// The other is the microphone, and it is gone on purpose. It drives
// window.SpeechRecognition, which LG really does implement — libcbe.so carries
// their own webos_speech_recognition_manager.cc — but every route to it is shut
// to us. MEASURED, from inside this app:
//
//     {"returnValue":false,"errorCode":-1,
//      "errorText":"Not permitted to send to com.webos.service.voiceconductor."}
//
// and the reason is in the hub's own configuration:
//
//     com.webos.service.voiceconductor.groups.json
//       "voiceconductor.operation": [ "oem" ]
//       "voiceconductor.query":     [ "oem" ]
//
// Those groups go to LG's own first-party clients and nowhere else — only
// com.webos.app.buddy, their assistant, holds them. Developer mode does not
// help: its certificate grants ["ares.webos.cli", "public"]. It is the same
// wall recorded in video.h and plane.h for com.webos.service.tv.display.
//
// NOR DOES IT WORK IN NUVIOWEB, which is the obvious objection. That code runs
// inside WebAppMgr, and WebAppMgr is granted twelve LS2 groups, none of them
// voice — it would hit the same refusal. What does work there, and works here
// too, is the SYSTEM microphone: the one on the Magic Remote and on the LG
// keyboard, performed by the IME (trustLevel "oem") and inserted into whatever
// text field has focus. With the keyboard up, this screen's field IS that
// field, and the words arrive as SDL_TEXTINPUT like any other typing. A drawn
// microphone button would have added nothing to that and implied a great deal.
static void drawDiscover(void) {
  GfxRect v = rectDiscover();
  float f = animDiscover;
  float ink = anim_blend(1.0f, 0.055f, f);
  GfxRect ic = { v.x + (v.w - NV_SEARCH_BTN_ICON) * 0.5f,
                 v.y + (v.h - NV_SEARCH_BTN_ICON) * 0.5f,
                 NV_SEARCH_BTN_ICON, NV_SEARCH_BTN_ICON };
  // The same pair of states as the field: #222 with a 1px #333 edge at rest,
  // going to a filled white disc on focus. The edge travels WITH the fill so
  // there is never a stale hairline sitting on top of the white.
  pillFilled(v, anim_blend(0.2f, 1.0f, f), anim_blend(0.133f, 1.0f, f), 1.0f);
  gfx_icon_at(ic, "search_discover", NV_SEARCH_BTN_ICON, ink, ink, ink, 1.0f);
  // `.search-discover-btn::after` — the label rides ABOVE the button, 16 clear
  // of it, and only while it has the focus.
  if (f > 0.01f) {
    TxtLine l = txt_line(TXT_DETWEB_TIP, "Discover", 255, 255, 255, 255);
    txt_draw_alpha(l, v.x + (v.w - (float)l.w) * 0.5f,
                   v.y - 16.0f - (float)l.h, 0.92f * f);
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
      txt_draw(l, t.x + (t.w - (float)l.w) * 0.5f, t.y + (t.h - (float)l.h) * 0.5f);
    }
  }
  float y = NV_SEARCH_BODY_Y + SEARCH_KB_ROWS * SEARCH_KB_STEP + 24;
  TxtLine hint = txt_line_trim(TXT_SRCH_META,
      nFilter ? "Right: results   •   Back: menu" : "OK: type   •   Back: menu",
      179, 183, 190, 255, SEARCH_KB_W);
  txt_draw(hint, NV_SEARCH_X, y);
}

// "RECENT SEARCHES" and the chips. The web app's idle state, and the reason the
// screen is worth opening with nothing typed at all.
static void drawHistory(void) {
  float x0 = contentX();
  int i;
  // 24/500, uppercase, tracked 1, --text-tertiary. Uppercasing is done here
  // rather than in the stored term: `text-transform` is a display rule on the
  // web too, and the chip below shows the term as it was typed.
  { char up[SEARCH_MAX_QUERY];
    const char *src = "Recent searches";
    int k = 0;
    while (src[k] && k + 1 < (int)sizeof up) { up[k] = (char)toupper((unsigned char)src[k]); k++; }
    up[k] = 0;
    txt_tracking(TXT_SRCH_NAME, up, 128, 128, 128,
                 x0, NV_SEARCH_BODY_Y, 1.0f, NV_SEARCH_HIST_LS); }

  for (i = 0; i < nHist; i++) {
    float f = animChip[i];
    GfxRect b = chipRect[i];
    // scale 1.06 from the CENTRE (the sheet sets no transform-origin, so it is
    // the default), which is why both axes are inset by half the growth.
    float scale = anim_blend(1.0f, NV_SEARCH_CHIP_FOCUS, f);
    GfxRect r = { b.x - b.w * (scale - 1.0f) * 0.5f,
                  b.y - b.h * (scale - 1.0f) * 0.5f,
                  b.w * scale, b.h * scale };
    float radius = 0.5f;                       // border-radius 999
    float luma = anim_blend(1.0f, 0.063f, f);  // #fff ink -> #10151f on white
    float icx = r.x + NV_SEARCH_CHIP_PADX + NV_SEARCH_CHIP_BORDER;
    gfx_color(r, radius, anim_blend(0.133f, 1.0f, f), anim_blend(0.133f, 1.0f, f),
              anim_blend(0.133f, 1.0f, f), 1.0f);
    gfx_rect(r, 0, GFX_RING_INSET, 0, NV_SEARCH_CHIP_BORDER / r.h, 0, radius,
             anim_blend(0.2f, 1.0f, f), anim_blend(0.2f, 1.0f, f),
             anim_blend(0.2f, 1.0f, f), anim_blend(1.0f, 0.4f, f));
    { GfxRect ic = { icx, r.y + (r.h - NV_SEARCH_CHIP_ICON) * 0.5f,
                     NV_SEARCH_CHIP_ICON, NV_SEARCH_CHIP_ICON };
      gfx_icon_at(ic, "search_clock", NV_SEARCH_CHIP_ICON, luma, luma, luma, 1.0f); }
    { int tom = (int)(luma * 255.0f + 0.5f);
      TxtLine l = txt_line(TXT_CALLOUT, hist[i], tom, tom, tom, 255);
      txt_draw(l, icx + NV_SEARCH_CHIP_ICON + NV_SEARCH_CHIP_ICOGAP,
               r.y + (r.h - (float)l.h) * 0.5f); }
  }
}

// "No Results". The web app's `.search-empty-state-results`: a 56/600 heading
// and a 24/400 line under it, the block centred horizontally and its own two
// lines left-aligned inside.
static void drawEmpty(void) {
  const char *t1 = "No Results";
  const char *t2 = "Try searching with different keywords";
  TxtLine l1 = txt_line(TXT_TITLE2, t1, 255, 255, 255, 255);
  TxtLine l2 = txt_line(TXT_SRCH_EMPTY, t2, 179, 179, 179, 255);
  float wide = (float)(l1.w > l2.w ? l1.w : l2.w);
  float x0 = contentX();
  float x = x0 + ((NV_SEARCH_RIGHT - x0) - wide) * 0.5f;
  float y = NV_SEARCH_BODY_Y + (NV_SEARCH_EMPTY_H - (float)(l1.h + l2.h)) * 0.5f;
  txt_draw(l1, x, y + NV_SEARCH_EMPTY_TOP);
  txt_draw(l2, x, y + NV_SEARCH_EMPTY_TOP + (float)l1.h + NV_SEARCH_EMPTY_GAP);
}

// The round "See All" at the end of a full row. The same 100px circle as the
// header's buttons; focused it fills white and the arrow swaps to the filled
// glyph, which is a different FILE and not the same shape recoloured.
static void drawSeeAll(float x, float y, float f) {
  GfxRect c = { x, y, NV_SEARCH_SEEALL, NV_SEARCH_SEEALL };
  float ink = anim_blend(1.0f, 0.055f, f);
  GfxRect ic = { x + (NV_SEARCH_SEEALL - NV_SEARCH_SEEALL_ICO) * 0.5f,
                 y + (NV_SEARCH_SEEALL - NV_SEARCH_SEEALL_ICO) * 0.5f,
                 NV_SEARCH_SEEALL_ICO, NV_SEARCH_SEEALL_ICO };
  pillFilled(c, anim_blend(0.2f, 1.0f, f), anim_blend(0.133f, 1.0f, f), 1.0f);
  if (f > 0.01f) {
    GfxRect halo = { x - 3.0f, y - 3.0f, NV_SEARCH_SEEALL + 6.0f, NV_SEARCH_SEEALL + 6.0f };
    gfx_rect(halo, 0, GFX_RING, 0, 3.0f / halo.h, 0, 0.5f, 1.0f, 1.0f, 1.0f, 0.25f * f);
  }
  gfx_icon_at(ic, f > 0.5f ? "search_seeall_fill" : "search_seeall",
              NV_SEARCH_SEEALL_ICO, ink, ink, ink, 1.0f);
}

static void drawResults(Uint32 now) {
  (void)now;
  float x0 = contentX();
  float trackX = x0 + NV_SEARCH_TRACK_X;
  hasItemFocus = 0;
  if (nFilter == 0) {
    if (histShown()) drawHistory();
    else if (nQuery >= 2) drawEmpty();
    return;
  }

  // The track really does run to the screen's edge: `.search-content` has no
  // right padding and the row is clipped by the viewport, which is what makes a
  // row read as continuing past the edge instead of ending there.
  gfx_crop(x0 - 8.0f, NV_SEARCH_BODY_Y - 30.0f,
           (NV_SCREEN_W - x0) + 8.0f,
           (NV_SCREEN_H - NV_SEARCH_TOP) - NV_SEARCH_BODY_Y + 30.0f);

  for (int r = 0; r < nFilter; r++) {
    float ry = NV_SEARCH_BODY_Y + r * NV_SEARCH_ROW_STEP - scrollY;
    if (ry > NV_SCREEN_H + 100.0f || ry + NV_SEARCH_ROW_STEP < -100.0f) continue;

    // The catalogue's name in 28/600 and the origin in 20/400 just below.
    TxtLine tt = txt_line_trim(TXT_ROW_TITLE, filter[r].title, 255, 255, 255, 255,
                               NV_SEARCH_RIGHT - x0);
    txt_draw(tt, x0, ry);
    if (filter[r].origin[0]) {
      char org[96];
      snprintf(org, sizeof org, "from %s", filter[r].origin);
      TxtLine ts = txt_line_trim(TXT_SRCH_META, org, 179, 179, 179, 255,
                                 NV_SEARCH_RIGHT - x0);
      txt_draw(ts, x0, ry + NV_SEARCH_ROW_SUB);
    }

    float cardY = ry + NV_SEARCH_ROW_RAIL;
    // Two passes: the focused item has to sit ON TOP of its neighbours, otherwise
    // the poster beside it clips the focus ring.
    for (int pass = 0; pass < 2; pass++) {
      for (int c = 0; c < filter[r].n; c++) {
        float f = animRes[r][c];
        if ((pass == 1) != (f > 0.01f)) continue;
        const CatItem *ci = cat_item(filter[r].items[c]);
        if (!ci) continue;

        float px = trackX + c * NV_SEARCH_CARD_STEP - scrollX[r];
        if (px > NV_SCREEN_W || px + NV_SEARCH_CARD_W < trackX - NV_SEARCH_CARD_W) continue;
        // `transform: scale(1.05)` with `transform-origin: top` on the WHOLE
        // card — poster, name and year — so the top edge stays on the rail and
        // the growth goes down. It did not scale in the sheet this screen was
        // first ported from; it does now.
        float scale = anim_blend(1.0f, NV_SEARCH_CARD_FOCUS, f);
        float cw = NV_SEARCH_CARD_W * scale;
        float cardX = px - (cw - NV_SEARCH_CARD_W) * 0.5f;
        GfxRect poster = { cardX, cardY, cw, NV_SEARCH_POSTER_H * scale };
        // The SDF's radius is a fraction of the HEIGHT, not of the smaller side:
        // `p = (uv-0.5)*vec2(asp,1.0)` makes one SDF unit h pixels on both axes.
        // Dividing by the width rounded this poster half again too much. See the
        // note on radiusInset in home.c.
        float radius = NV_SEARCH_POSTER_R / poster.h;
        if (f > 0.01f) {
          // A 2px border INSIDE the wrap plus `box-shadow 0 0 0 2px` outside it:
          // four pixels of --secondary-color in all, which is why both halves
          // are drawn and not just the outer one.
          GfxRect b = { poster.x - 2.0f, poster.y - 2.0f,
                        poster.w + 4.0f, poster.h + 4.0f };
          gfx_color(b, NV_SEARCH_POSTER_R / b.h, 0.961f, 0.961f, 0.961f, f);
        }

        const char *art = ci->poster[0] ? ci->poster
                         : (ci->backdrop[0] ? ci->backdrop : NULL);
        // The RESTING width, not the animated one: tex_cache re-decodes an exact
        // request whose width moves, and a poster that re-decodes through a
        // focus spring is a poster that is missing for the length of it.
        GLuint tex = art ? tex_get_width(art, NV_SEARCH_CARD_W) : 0;
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
        if (f > 0.01f)
          gfx_rect(poster, 0, GFX_RING_INSET, 0, 2.0f / poster.h, 0, radius,
                   0.961f, 0.961f, 0.961f, f);

        // The name in 24/500 at 16 from the poster; the year in 20/400 rgb(179)
        // at 4 from the name.
        TxtLine tn = txt_line_trim(TXT_SRCH_NAME, ci->title, 255, 255, 255, 255, cw);
        float ny = poster.y + poster.h + NV_SEARCH_NAME_GAP * scale;
        txt_draw_alpha(tn, poster.x, ny, anim_blend(0.82f, 1.0f, f));
        if (ci->meta[0]) {
          TxtLine td = txt_line_trim(TXT_SRCH_META, ci->meta, 179, 179, 179, 255, cw);
          txt_draw_alpha(td, poster.x, ny + (float)tn.h + NV_SEARCH_DATE_GAP, 0.92f);
        }

        if (panel == PANEL_RES && focus_index(&focusRes, r, c)) {
          itemFocus.index_ = filter[r].items[c];
          itemFocus.rect   = poster;
          itemFocus.art    = ci->backdrop[0] ? ci->backdrop : ci->poster;
          itemFocus.title  = ci->title;
          itemFocus.genre  = ci->genre;
          itemFocus.meta   = ci->meta;
          hasItemFocus = 1;
        }
      }
      if (filter[r].seeAll) {
        int c = filter[r].n;
        float f = animRes[r][c];
        if ((pass == 1) == (f > 0.01f)) {
          float px = trackX + c * NV_SEARCH_CARD_STEP - scrollX[r]
                   + (NV_SEARCH_SEEALL_GAP - (NV_SEARCH_CARD_STEP - NV_SEARCH_CARD_W));
          drawSeeAll(px, cardY + NV_SEARCH_SEEALL_Y, f);
        }
      }
    }
  }
  gfx_no_crop();
}

void search_draw(Uint32 now) {
  // Background #0d0d0d, .search-screen-shell's --bg-color. The screen has
  // already been cleared with THIS VERY COLOUR by glClearColor/glClear in main.c
  // before app_draw. Painting over it was one full-screen layer thrown away per
  // frame — and the dominant cost on this GPU is fill rate (gfx.c records that
  // TWO full-screen layers dropped the Mali-G71 to ~40fps). Do not put it back
  // without first changing the clear colour.
  // Only when they are on screen: layoutChips measures every term, and the
  // rasteriser has a per-frame budget that the results have first call on.
  if (histShown()) layoutChips(); else chipRows = 0;
  drawTitle();
  drawField(now);
  drawDiscover();
  if (!ime_usable()) drawKeyboard();
  drawResults(now);
}
