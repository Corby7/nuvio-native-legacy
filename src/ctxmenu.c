#include "ctxmenu.h"
#include "catalog.h"
#include "trakt.h"
#include "extras.h"
#include "gfx.h"
#include "text.h"
#include "tex_cache.h"
#include "layout.h"
#include "anim.h"
#include "settings.h"
#include "cwremove.h"
#include <stdio.h>
#include <string.h>

// Keeps Trakt's public header stable: these reads are the internal contract
// between the modal and the port's own asynchronous writes.
extern int trakt_operation_state(int kind);
extern int trakt_watchlist_kind(const char *imdb, const char *kind, int add);
extern int trakt_watched_kind(const char *imdb, const char *kind, int mark);
extern int cat_history_state_item(int index_);
extern void cat_history_set_id(const char *imdb, const char *kind, int watched);

enum { CTX_OP_NONE, CTX_OP_LIST = 1, CTX_OP_HISTORY = 2 };
enum { CTX_PENDING = 1, CTX_CONFIRMED = 2, CTX_FAILURE = 3 };

// THE OWNER'S DESIGN, not the web's centred dialog: a panel that opens BESIDE the
// card that was held, so the thing the options act on stays in view. Measured off
// the mock (2000px wide, scaled to 1920). Radii are in PIXELS here; gfx_color
// takes a fraction of the shorter side, and pxRadius converts.
#define CTX_W        560.0f
#define CTX_PAD       20.0f
#define CTX_BOTTOM    20.0f     // below the last row
#define CTX_LINE      64.0f     // height of each row
#define CTX_GAP        7.0f
#define CTX_RADIUS    20.0f
#define CTX_ICON      28.0f
#define CTX_ICON_X    26.0f     // icon's left edge inside the row
#define CTX_LABEL_X   84.0f     // label's left edge inside the row
#define CTX_META_GAP  10.0f     // title to meta line
#define CTX_HEAD_GAP  20.0f     // meta line to the first row
#define CTX_CARD_GAP  32.0f     // card edge to panel edge
#define CTX_BELOW     24.0f     // how far the panel runs past the card's bottom
#define CTX_MARGIN    48.0f     // nearest the panel comes to a screen edge
#define CTX_SLIDE     24.0f     // travel away from the card while it appears
#define CTX_SCRIM      0.80f

static float pxRadius(GfxRect r, float px) {
  float m = r.w < r.h ? r.w : r.h;
  return m > 0.0f ? px / m : 0.0f;
}

static int   is_open, idx = -1, focus, reqDetails = -1;
// Resume / Start from the beginning: the title to play, and whether from 0.
static int   reqPlay = -1, reqFromStart;
// The row the card was in, and whether the owner asked to see all of it.
static CtxCatalog row;
// The card the panel sits beside. `anchorNext` is what the home handed over for
// the NEXT open; `anchor` is what the open in progress took from it.
static GfxRect anchor, anchorNext;
static float   anchorR, anchorRNext;
// Where the card is drawn now (ctx_track_card); the scrim's hole, not the panel's.
static GfxRect hole;
static float   holeR, holeFeather;
static int     hasAnchor, hasAnchorNext;
static int   reqSeeAll;
static float anim;
static int   operation, intent, stateOperation;
static int   mirrorApplied;
static char  operationImdb[16];
static volatile int holdActive, holdCancelled, holdReady;
// OK was still physically down when the modal opened — see the guard in ctx_event.
static volatile int swallowOk;
static Uint32 holdSince;

static int keyOk(SDL_Keycode k) {
  return k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE;
}

// The home is what knows the focused item, so it goes on deciding which index
// to hand to ctx_open on KEYUP. This observer provides the feedback during the
// hold and arms the long window; arrows/Back invalidate the gesture before the
// home can turn it into an action.
static int observeHold(void *u, SDL_Event *e) {
  (void)u;
  if (e->type == SDL_KEYDOWN) {
    SDL_Keycode k = e->key.keysym.sym;
    if (keyOk(k) && !e->key.repeat) {
      holdActive = 1;
      holdCancelled = 0;
      holdReady = 0;
      holdSince = SDL_GetTicks();
    } else if (holdActive &&
               (k == SDLK_UP || k == SDLK_DOWN || k == SDLK_LEFT ||
                k == SDLK_RIGHT || k == SDLK_AC_BACK || k == SDLK_ESCAPE ||
                k == SDLK_BACKSPACE || e->key.keysym.scancode == NV_SCANCODE_BACK)) {
      holdCancelled = 1;
    }
  } else if (e->type == SDL_KEYUP && keyOk(e->key.keysym.sym)) {
    if (holdActive && !holdCancelled && SDL_GetTicks() - holdSince >= NV_HOLD_MS)
      holdReady = 1;
    holdActive = 0;
    swallowOk = 0;
  }
  return 0;
}

// Up to seven: resume and start over on a title with progress, details, library,
// — only on films/series — watched, the row itself when it came from a catalogue,
// and leaving Continue watching.
#define CTX_MAX 7
// `hint` is drawn at the row's right edge, dimmer: the episode Resume will play.
static struct { const char *rot, *icon, *hint; int action; } ops[CTX_MAX];
static int nOps;
static float focusAnim[CTX_MAX];
static int holdObserver;
enum { OP_DETAILS, OP_LIST, OP_WATCHED, OP_SEEALL, OP_RESUME, OP_START_OVER,
       OP_REMOVE_CW };

// IN PROGRESS by the player's own measure: player_set_episode resumes only between
// 1% and 90%, so outside that window "Resume" would be a lie.
static int inProgress(const CatItem *ci) {
  return ci->progress > 0 && ci->progress < 90;
}

static void addOp(const char *rot, const char *icon, int action) {
  if (nOps >= CTX_MAX) return;
  ops[nOps].rot = rot; ops[nOps].icon = icon; ops[nOps].hint = NULL;
  ops[nOps].action = action;
  nOps++;
}

static int indexCurrent(void) {
  int n = cat_n();
  int found;
  if (n < 1 || idx < 0 || idx >= n) return -1;
  if (operationImdb[0]) {
    found = cat_index_by_imdb(operationImdb);
    // The answer may arrive after discovery has swapped the block out. Never
    // reuse `idx` in that case, because it may be a different title.
    return found;
  }
  return idx;
}

static void build(void) {
  int i = indexCurrent();
  const CatItem *ci = i >= 0 ? cat_item(i) : NULL;
  int going;
  nOps = 0;
  memset(ops, 0, sizeof ops);
  if (!ci) return;
  going = inProgress(ci);
  // A TITLE WITH PROGRESS LEADS WITH PLAYING IT, in the owner's mock order:
  // Resume, Start from the beginning, the library and watched toggles, then the
  // details. Anything else keeps details first, as it always had.
  if (going) {
    static char episode[24];
    addOp("Resume", "ctx_play", OP_RESUME);
    if (!strcmp(ci->kind, "series") && ci->season > 0 && ci->episode > 0) {
      snprintf(episode, sizeof episode, "S%d E%d", ci->season, ci->episode);
      ops[nOps - 1].hint = episode;
    }
    addOp("Start from the beginning", "ctx_restart", OP_START_OVER);
  } else {
    addOp("See details", "ctx_info", OP_DETAILS);
  }
  // Without an IMDb id there is no supported remote endpoint for this action.
  // Do not offer a button that would only look like it works and would invent
  // local state.
  if (ci->imdb[0]) {
    if (stateOperation == CTX_PENDING && operation == CTX_OP_LIST)
      ops[nOps].rot = intent ? "Adding to library..."
                               : "Removing from library...";
    else
      ops[nOps].rot = ci->inList ? "Remove from library"
                                  : "Add to library";
    ops[nOps].icon = ci->inList ? "ctx_minus" : "ctx_plus";
    ops[nOps].action = OP_LIST; nOps++;
  }
  // The web app only offers "watched" on films and series — not on channels or
  // events, which are types the owner's addons also declare.
  if (ci->imdb[0] && (!strcmp(ci->kind, "movie") || !strcmp(ci->kind, "series"))) {
    if (stateOperation == CTX_PENDING && operation == CTX_OP_HISTORY)
      ops[nOps].rot = intent ? "Marking as watched..."
                               : "Unmarking as watched...";
    else
      ops[nOps].rot = cat_history_state_item(i) == 1
                        ? "Unmark as watched"
                        : "Mark as watched";
    ops[nOps].icon = cat_history_state_item(i) == 1 ? "ctx_x" : "ctx_check";
    ops[nOps].action = OP_WATCHED; nOps++;
  }
  if (going) addOp("See details", "ctx_info", OP_DETAILS);
  // THE ROW, LAST. It is the only option here that does not act on the title in
  // the header, so it goes below the three that do rather than between them.
  //
  // It carries the row's NAME because "Browse this list" would be the one line in
  // this modal that does not say what it will do: the header names the title, and
  // nothing on screen would name the list being opened.
  if (row.base[0] && row.catId[0]) {
    static char label[96];
    if (row.title[0]) {
      // 28 IS A WIDTH, not a round number. The button draws its text with
      // txt_line and no trimming, so anything too long runs out past the modal's
      // edge instead of being cut. At NV_FT_PLR_BODY (32px) the ~588px of button
      // left of the text takes about 34 characters, and "Browse " is 7 of them.
      // Every row name that exists today fits ("Popular - Movie", "Oscars 2026 -
      // Movie"); a renamed catalogue is what this is here for.
      char cut[28];
      snprintf(cut, sizeof cut, "%s", row.title);
      if (strlen(row.title) >= sizeof cut) {
        // Room for the ellipsis AND its terminator, and never mid-character: cut
        // between the bytes of a three-byte glyph and the line draws a
        // replacement box. Back up to the first byte of whatever sits there.
        size_t k = sizeof cut - 4;
        while (k > 0 && (cut[k] & 0xC0) == 0x80) k--;
        snprintf(cut + k, sizeof cut - k, "\xe2\x80\xa6");
      }
      snprintf(label, sizeof label, "Browse %s", cut);
    } else {
      snprintf(label, sizeof label, "Browse the whole row");
    }
    ops[nOps].rot = label; ops[nOps].action = OP_SEEALL;
    ops[nOps].icon = "ctx_grid"; nOps++;
  }
  // LAST, and set apart by being the one that takes something away. It clears
  // the resume points here, on the account and on Trakt — see cwremove.h.
  if (going) addOp("Remove from Continue watching", "ctx_hide", OP_REMOVE_CW);
}

void ctx_open(int index_) { ctx_open_row(index_, NULL); }

void ctx_track_card(GfxRect card, float radiusPx, float glowPx) {
  hole = card; holeR = radiusPx; holeFeather = glowPx;
}

void ctx_set_anchor(GfxRect card, float radiusPx) {
  anchorNext = card; anchorRNext = radiusPx; hasAnchorNext = 1;
}

void ctx_open_row(int index_, const CtxCatalog *from) {
  // Taken whether or not this open goes ahead, so a stale card never anchors a
  // later open that did not set one.
  int anchored = hasAnchorNext;
  hasAnchorNext = 0;
  if (holdCancelled) {
    holdCancelled = 0;
    holdReady = 0;
    return;
  }
  if (index_ < 0 || index_ >= cat_n() || !cat_item(index_)) return;
  hasAnchor = anchored;
  if (anchored) {
    anchor = anchorNext; anchorR = anchorRNext;
    hole = anchor; holeR = anchorR; holeFeather = 0.0f;
  }
  // The long press has already consumed the gesture on the home. Clearing the
  // sentinel here stops the following KEYUP being reused as a selection inside
  // the modal.
  holdReady = 0;
  swallowOk = holdActive;
  idx = index_; focus = 0; is_open = 1; reqDetails = -1; reqPlay = -1;
  memset(&row, 0, sizeof row);
  if (from) row = *from;
  reqSeeAll = 0;
  operation = CTX_OP_NONE; intent = 0; stateOperation = 0;
  mirrorApplied = 0;
  operationImdb[0] = 0;
  memset(focusAnim, 0, sizeof focusAnim);
  build();
}

int ctx_is_open(void) { return is_open; }
int ctx_requested_details(void) { int v = reqDetails; reqDetails = -1; return v; }
int ctx_requested_play(int *fromStart) {
  int v = reqPlay;
  reqPlay = -1;
  if (fromStart) *fromStart = reqFromStart;
  return v;
}
int ctx_requested_seeall(CtxCatalog *out) {
  int v = reqSeeAll;
  reqSeeAll = 0;
  if (v && out) *out = row;
  return v;
}

static void apply(void) {
  int current = indexCurrent();
  const CatItem *ci = current >= 0 ? cat_item(current) : NULL;
  int action;
  if (!ci || focus < 0 || focus >= nOps) return;
  action = ops[focus].action;
  // Only the WRITES wait on one another; playing, opening and browsing never do.
  if ((action == OP_LIST || action == OP_WATCHED || action == OP_REMOVE_CW) &&
      operation != CTX_OP_NONE && stateOperation != CTX_FAILURE) return;
  switch (action) {
    case OP_DETAILS: reqDetails = idx; break;
    case OP_SEEALL:  reqSeeAll = 1;    break;
    case OP_RESUME:      reqPlay = idx; reqFromStart = 0; break;
    case OP_START_OVER:  reqPlay = idx; reqFromStart = 1; break;
    // The card leaves the row at once (the local half of cw_remove is
    // synchronous), so there is nothing left for the menu to show: it closes, and
    // the remote deletes finish on their own thread.
    case OP_REMOVE_CW:   cw_remove(current); break;
    case OP_LIST:
      // Capture the intent BEFORE any write. The same value goes on to the
      // POST and only reaches the local mirror after a 2xx response.
      intent = !ci->inList;
      snprintf(operationImdb, sizeof operationImdb, "%s", ci->imdb);
      operation = CTX_OP_LIST;
      mirrorApplied = 0;
      stateOperation = CTX_PENDING;
      if (!trakt_watchlist_kind(ci->imdb, ci->kind, intent))
        stateOperation = CTX_FAILURE;
      build();
      break;
    case OP_WATCHED:
      // Progress and resume position, not history. Only a confirmed history
      // snapshot may flip the action to "unmark".
      intent = cat_history_state_item(current) == 1 ? 0 : 1;
      snprintf(operationImdb, sizeof operationImdb, "%s", ci->imdb);
      operation = CTX_OP_HISTORY;
      mirrorApplied = 0;
      stateOperation = CTX_PENDING;
      if (!trakt_watched_kind(ci->imdb, ci->kind, intent))
        stateOperation = CTX_FAILURE;
      build();
      break;
  }
  if (action == OP_DETAILS || action == OP_SEEALL || action == OP_RESUME ||
      action == OP_START_OVER || action == OP_REMOVE_CW) is_open = 0;
}

void ctx_event(const SDL_Event *e) {
  int k;
  if (!is_open) return;
  if (e->type != SDL_KEYDOWN) return;
  k = e->key.keysym.sym;
  // OK IS STILL DOWN FROM THE GESTURE THAT OPENED THIS MODAL. It opens at the 500ms
  // mark, with the button not yet released, and the remote goes on sending repeats
  // of that same press — which arrived here and reached apply(), firing "Details".
  // A long press therefore landed on the detail screen, the very place a TAP lands,
  // with the modal flashing on the way through: the two gestures looked identical
  // and neither seemed to do what it said. The first OK this modal accepts is the
  // first one PRESSED after it opened.
  if (keyOk(k) && (e->key.repeat || swallowOk)) return;
  if (k == SDLK_AC_BACK || k == SDLK_ESCAPE || k == SDLK_BACKSPACE ||
      e->key.keysym.scancode == NV_SCANCODE_BACK) { is_open = 0; return; }
      // While the request is in flight, OK does not repeat the write. Focus
      // stays inside the modal and Back can always cancel the visual wait.
  if (operation != CTX_OP_NONE) {
    if (stateOperation == CTX_PENDING) return;
    if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) {
      if (stateOperation == CTX_FAILURE) apply();
      else is_open = 0;
    }
    if (k != SDLK_UP && k != SDLK_DOWN) return;
  }
  if (k == SDLK_UP)   { if (focus > 0) focus--; return; }
  if (k == SDLK_DOWN) { if (focus + 1 < nOps) focus++; return; }
  if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) { apply(); return; }
}

void ctx_update(float dt, Uint32 now) {
  int i;
  int current;
  if (!holdObserver) {
    SDL_AddEventWatch(observeHold, NULL);
    holdObserver = 1;
  }
  if (holdActive && now - holdSince >= NV_HOLD_MS) holdReady = 1;
  if (settings_animations_reduced())
    anim = is_open ? 1.0f : 0.0f;
  else
    anim = anim_spring(anim, is_open ? 1.0f : 0.0f, dt, NV_SPRING_SCREEN);
  for (i = 0; i < CTX_MAX; i++)
    focusAnim[i] = settings_animations_reduced()
      ? (is_open && focus == i ? 1.0f : 0.0f)
      : anim_spring(focusAnim[i], is_open && focus == i ? 1.0f : 0.0f,
                  dt, NV_SPRING_FOCUS);

  current = indexCurrent();
  if (is_open && current < 0) { is_open = 0; return; }

  if (operation != CTX_OP_NONE && stateOperation == CTX_PENDING) {
    int new = trakt_operation_state(operation);
    if (new == CTX_CONFIRMED || new == CTX_FAILURE) {
      stateOperation = new;
      if (!mirrorApplied && current >= 0) {
        const CatItem *ci = cat_item(current);
        if (ci && new == CTX_CONFIRMED) {
          if (operation == CTX_OP_LIST) {
            cat_set_in_list(current, intent);
          } else {
            cat_history_set_id(ci->imdb, ci->kind, intent);
          }
        }
        mirrorApplied = 1;
        build();
      }
    }
  }
}

// The meta line under the title: "Series · 2026 · S1 E3 · 55 min left". The time
// left is the one piece drawn bright, as the thing a held card is most often held
// for. Returns how many pieces it filled.
static int metaPieces(const CatItem *ci, char out[4][32], int *bright) {
  int n = 0, year = ci->year;
  *bright = -1;
  if (!strcmp(ci->kind, "series"))     snprintf(out[n++], 32, "Series");
  else if (!strcmp(ci->kind, "movie")) snprintf(out[n++], 32, "Movie");
  // A catalogue item carries its year only at the head of `meta` ("2022 · 3
  // seasons"); `year` is filled for Trakt list items alone.
  if (year <= 0 && ci->meta[0] >= '1' && ci->meta[0] <= '2' &&
      ci->meta[1] >= '0' && ci->meta[1] <= '9' &&
      ci->meta[2] >= '0' && ci->meta[2] <= '9' &&
      ci->meta[3] >= '0' && ci->meta[3] <= '9')
    year = (ci->meta[0] - '0') * 1000 + (ci->meta[1] - '0') * 100 +
           (ci->meta[2] - '0') * 10 + (ci->meta[3] - '0');
  if (year > 0) snprintf(out[n++], 32, "%d", year);
  if (ci->season > 0 && ci->episode > 0)
    snprintf(out[n++], 32, "S%d E%d", ci->season, ci->episode);
  if (ci->remainingMin > 0) {
    *bright = n;
    snprintf(out[n++], 32, "%d min left", ci->remainingMin);
  }
  return n;
}

void ctx_draw(Uint32 now) {
  const CatItem *ci;
  const char *message = NULL;
  float a = anim, height, x, y, cy;
  int i;
  (void)now;
  // THE HINT USED TO BE DRAWN HERE TOO, centred at the bottom of the screen. ctx_draw
  // runs on EVERY screen, and this observer sees OK on every screen, so "Hold OK for
  // options" appeared over the detail and the episode list — where holding OK picks a
  // source and marks an episode watched, not this modal — and, on the home, next to
  // the card's own rail, the two bars filling at different rates. The home's rail is
  // the only feedback left, and it is a bar with no words.
  if (a < 0.01f) return;
  ci = indexCurrent() >= 0 ? cat_item(indexCurrent()) : NULL;
  if (!ci) return;

  // The labels already say "Adding to library..." while a write is in flight, so
  // the meta line only gives way to the outcome.
  if (stateOperation == CTX_CONFIRMED)
    message = operation == CTX_OP_LIST ? "Library updated"
                                        : (intent ? "Marked as watched"
                                                    : "Unmarked as watched");
  else if (stateOperation == CTX_FAILURE)
    message = "Could not update. Try again.";

  { TxtLine title = txt_line_trim(TXT_PANEL_TITLE, ci->title, 245, 246, 249, 255,
                                  CTX_W - CTX_PAD * 2.0f);
    TxtLine probe = txt_line(TXT_HERO_META, "Series", 150, 154, 163, 255);
    float headH = (float)title.h + CTX_META_GAP + (float)probe.h + CTX_HEAD_GAP;
    height = CTX_PAD + headH + CTX_BOTTOM +
             (float)nOps * (CTX_LINE + CTX_GAP) - CTX_GAP;

    // Beside the card, on its right; on its left when the right runs out of screen.
    // Bottom-aligned just past the card, so the panel grows UP beside it, and kept
    // on screen whichever row the card is in.
    if (hasAnchor) {
      int left = anchor.x + anchor.w + CTX_CARD_GAP + CTX_W > NV_SCREEN_W - CTX_MARGIN;
      x = left ? anchor.x - CTX_CARD_GAP - CTX_W
               : anchor.x + anchor.w + CTX_CARD_GAP;
      if (x < CTX_MARGIN) x = CTX_MARGIN;
      y = anchor.y + anchor.h + CTX_BELOW - height;
      if (y > NV_SCREEN_H - CTX_MARGIN - height) y = NV_SCREEN_H - CTX_MARGIN - height;
      if (y < CTX_MARGIN) y = CTX_MARGIN;
      // Slides out from the card as it appears.
      x += (left ? 1.0f : -1.0f) * (1.0f - a) * CTX_SLIDE;

      // The scrim goes round the card, not over it: the held card stays lit and
      // everything else steps back. One quad with a rounded hole cut exactly at
      // the ring's outer edge (home.c hands over that edge and its corner).
      gfx_scrim_hole(hole, holeR, holeFeather, CTX_SCRIM * a);
    } else {
      x = (NV_SCREEN_W - CTX_W) * 0.5f;
      y = (NV_SCREEN_H - height) * 0.5f + (1.0f - a) * CTX_SLIDE;
      gfx_color((GfxRect){ 0, 0, NV_SCREEN_W, NV_SCREEN_H }, 0.0f, 0, 0, 0, CTX_SCRIM * a);
    }

    // A hairline border: the same panel one pixel larger and a step lighter, under it.
    { GfxRect panel = { x, y, CTX_W, height };
      GfxRect edge = { x - 1.0f, y - 1.0f, CTX_W + 2.0f, height + 2.0f };
      gfx_drop_shadow(panel, CTX_RADIUS, 40.0f, 12.0f, 0.5f * a);
      gfx_color(edge, pxRadius(edge, CTX_RADIUS + 1.0f), 0.14f, 0.15f, 0.17f, a);
      gfx_color(panel, pxRadius(panel, CTX_RADIUS), 0.055f, 0.059f, 0.071f, a); }

    cy = y + CTX_PAD;
    txt_draw_alpha(title, x + CTX_PAD, cy, a);
    cy += (float)title.h + CTX_META_GAP;

    if (message) {
      int fail = stateOperation == CTX_FAILURE;
      TxtLine t = txt_line_trim(TXT_HERO_META, message,
                                fail ? 255 : 225, fail ? 138 : 228, fail ? 128 : 235, 255,
                                CTX_W - CTX_PAD * 2.0f);
      txt_draw_alpha(t, x + CTX_PAD, cy, a);
    } else {
      char pieces[4][32];
      int bright, n = metaPieces(ci, pieces, &bright);
      float mx = x + CTX_PAD, maxX = x + CTX_W - CTX_PAD;
      for (i = 0; i < n; i++) {
        int on = i == bright;
        TxtLine t = txt_line(TXT_HERO_META, pieces[i],
                             on ? 236 : 150, on ? 238 : 154, on ? 242 : 162, 255);
        if (i > 0) {
          TxtLine dot = txt_line(TXT_HERO_META, "\xc2\xb7", 96, 100, 108, 255);
          mx += 14.0f;
          txt_draw_alpha(dot, mx, cy, a);
          mx += (float)dot.w + 14.0f;
        }
        if (mx + (float)t.w > maxX) break;
        txt_draw_alpha(t, mx, cy, a);
        mx += (float)t.w;
      }
    }
    cy = y + CTX_PAD + headH;
  }

  for (i = 0; i < nOps; i++) {
    float by = cy + (float)i * (CTX_LINE + CTX_GAP);
    GfxRect r = { x + CTX_PAD, by, CTX_W - CTX_PAD * 2.0f, CTX_LINE };
    float f = focusAnim[i];
    // The focused row INVERTS into a full pill, like every other button in the
    // app: a light capsule with dark ink.
    // The others sit bare on the panel.
    float ink = anim_blend(0.95f, 0.07f, f);
    int c = (int)(ink * 255.0f + 0.5f);
    if (f > 0.01f)
      gfx_color(r, 0.5f, 0.945f, 0.949f, 0.957f, a * f);
    if (ops[i].icon)
      gfx_icon((GfxRect){ r.x + CTX_ICON_X, by + (CTX_LINE - CTX_ICON) * 0.5f,
                          CTX_ICON, CTX_ICON },
               ops[i].icon, ink, ink, ink, a);
    { float room = r.w - CTX_LABEL_X - CTX_ICON_X;
      if (ops[i].hint) {
        int hc = (int)(anim_blend(0.62f, 0.36f, f) * 255.0f + 0.5f);
        TxtLine h = txt_line(TXT_HERO_META, ops[i].hint, hc, hc, hc, 255);
        float hx = r.x + r.w - CTX_ICON_X - (float)h.w;
        txt_draw_alpha(h, hx, by + (CTX_LINE - h.h) * 0.5f, a);
        room -= (float)h.w + 16.0f;
      }
      { TxtLine t = txt_line_trim(TXT_DET_BUTTON, ops[i].rot, c, c, c, 255, room);
        txt_draw_alpha(t, r.x + CTX_LABEL_X, by + (CTX_LINE - t.h) * 0.5f, a); } }
  }
}
