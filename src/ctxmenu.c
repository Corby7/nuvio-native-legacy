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

// MEASURED on bundle 1.0.4: the dialog is 37.5vw wide (720 px at 1920).
#define CTX_W      720.0f
#define CTX_PAD     44.0f
#define CTX_LINE   86.0f     // height of each button
#define CTX_GAP     12.0f
#define CTX_HEADER    148.0f     // title, states and group label
#define CTX_STATUS_H 34.0f
#define CTX_FOOTER  70.0f

static int   is_open, idx = -1, focus, reqDetails = -1;
static float anim;
static int   operation, intent, stateOperation;
static int   mirrorApplied;
static char  operationImdb[16];
static volatile int holdActive, holdCancelled, holdReady;
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
  }
  return 0;
}

// Up to three: details, library and — only on films/series — watched.
#define CTX_MAX 3
static struct { const char *rot; int action; } ops[CTX_MAX];
static int nOps;
static float focusAnim[CTX_MAX];
static int holdObserver;
enum { OP_DETAILS, OP_LIST, OP_WATCHED };

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
  nOps = 0;
  if (!ci) return;
  ops[nOps].rot = "See details";        ops[nOps].action = OP_DETAILS;  nOps++;
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
    ops[nOps].action = OP_WATCHED; nOps++;
  }
}

void ctx_open(int index_) {
  if (holdCancelled) {
    holdCancelled = 0;
    holdReady = 0;
    return;
  }
  if (index_ < 0 || index_ >= cat_n() || !cat_item(index_)) return;
  // The long press has already consumed the gesture on the home. Clearing the
  // sentinel here stops the following KEYUP being reused as a selection inside
  // the modal.
  holdReady = 0;
  idx = index_; focus = 0; is_open = 1; reqDetails = -1;
  operation = CTX_OP_NONE; intent = 0; stateOperation = 0;
  mirrorApplied = 0;
  operationImdb[0] = 0;
  memset(focusAnim, 0, sizeof focusAnim);
  build();
}

int ctx_is_open(void) { return is_open; }
int ctx_requested_details(void) { int v = reqDetails; reqDetails = -1; return v; }

static void apply(void) {
  int current = indexCurrent();
  const CatItem *ci = current >= 0 ? cat_item(current) : NULL;
  int action;
  if (!ci || focus < 0 || focus >= nOps) return;
  action = ops[focus].action;
  if (action != OP_DETAILS && operation != CTX_OP_NONE &&
      stateOperation != CTX_FAILURE) return;
  switch (action) {
    case OP_DETAILS: reqDetails = idx; break;
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
  if (action == OP_DETAILS) is_open = 0;
}

void ctx_event(const SDL_Event *e) {
  int k;
  if (!is_open) return;
  if (e->type != SDL_KEYDOWN) return;
  k = e->key.keysym.sym;
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

void ctx_draw(Uint32 now) {
  const CatItem *ci;
  const char *states[2];
  const char *message = NULL;
  float a = anim, height, x, y;
  int i, nStates = 1;
  (void)now;
  if (!is_open && holdActive) {
    float p = (float)(SDL_GetTicks() - holdSince) / (float)NV_HOLD_MS;
    TxtLine t;
    if (p > 1.0f) p = 1.0f;
    t = txt_line(TXT_CAPTION2,
                  p >= 1.0f ? "Release to open options" : "Hold OK for options",
                  220, 224, 232, 255);
    txt_draw_alpha(t, (NV_SCREEN_W - t.w) * 0.5f, NV_SCREEN_H - 124.0f, 0.94f);
    gfx_color((GfxRect){ (NV_SCREEN_W - 420.0f) * 0.5f, NV_SCREEN_H - 82.0f,
                       420.0f, 8.0f }, 4.0f, 0.18f, 0.2f, 0.23f, 0.96f);
    gfx_color((GfxRect){ (NV_SCREEN_W - 420.0f) * 0.5f, NV_SCREEN_H - 82.0f,
                       420.0f * p, 8.0f }, 4.0f, 0.78f, 0.84f, 0.96f, 0.98f);
  }
  if (a < 0.01f) return;
  ci = indexCurrent() >= 0 ? cat_item(indexCurrent()) : NULL;
  if (!ci) return;

  if (stateOperation == CTX_PENDING)
    message = operation == CTX_OP_LIST ? "Updating library..."
                                        : (intent ? "Marking as watched..."
                                                    : "Unmarking as watched...");
  else if (stateOperation == CTX_CONFIRMED)
    message = operation == CTX_OP_LIST ? "Library updated"
                                        : (intent ? "Marked as watched"
                                                    : "Unmarked as watched");
  else if (stateOperation == CTX_FAILURE)
    message = "Could not update. Try again.";

  states[0] = ci->inList ? "In library" : "Not in library";
  if (!strcmp(ci->kind, "movie") || !strcmp(ci->kind, "series")) {
    { int history = cat_history_state_item(indexCurrent());
      states[1] = history == 1 ? "Watched"
                   : history == 0 ? "Not watched"
                   : ci->progress > 0 ? "Progress saved"
                   : "History not checked"; }
    nStates = 2;
  }

  { GfxRect screen = { 0, 0, NV_SCREEN_W, NV_SCREEN_H };
    gfx_color(screen, 0.0f, 0, 0, 0, 0.72f * a); }

  height = CTX_PAD * 2.0f + CTX_HEADER +
        (float)nOps * (CTX_LINE + CTX_GAP) - CTX_GAP + CTX_FOOTER;
  x = (NV_SCREEN_W - CTX_W) * 0.5f;
  y = (NV_SCREEN_H - height) * 0.5f;
  // Rises from the bottom as it appears, like the app's other sheets.
  y += (1.0f - a) * 40.0f;

  { GfxRect p = { x, y, CTX_W, height };
    gfx_color(p, 0.06f, 0.11f, 0.11f, 0.13f, 0.98f * a); }

  { TxtLine t = txt_line(TXT_CAPTION2, "SELECTED TITLE", 174, 178, 188, 255);
    txt_draw_alpha(t, x + CTX_PAD, y + CTX_PAD, a * 0.95f); }
  { TxtLine t = txt_line_trim(TXT_HEADLINE, ci->title, 245, 248, 255, 255,
                                 CTX_W - CTX_PAD * 2.0f);
    txt_draw_alpha(t, x + CTX_PAD, y + CTX_PAD + 28.0f, a); }
  { const char *subtitle = message ? message : "Title options";
    TxtLine t = txt_line(TXT_DET_META2, subtitle, 150, 154, 163, 255);
    txt_draw_alpha(t, x + CTX_PAD, y + CTX_PAD + 70.0f, a * 0.9f); }

  { float sx = x + CTX_PAD;
    float sy = y + CTX_PAD + 104.0f;
    for (i = 0; i < nStates; i++) {
      TxtLine t = txt_line(TXT_CAPTION2, states[i], 215, 218, 225, 255);
      float sw = t.w + 24.0f;
      gfx_color((GfxRect){ sx, sy, sw, CTX_STATUS_H }, 0.5f,
              0.16f, 0.17f, 0.19f, 0.96f * a);
      txt_draw_alpha(t, sx + 12.0f,
                         sy + (CTX_STATUS_H - t.h) * 0.5f, a);
      sx += sw + CTX_GAP;
    } }

  for (i = 0; i < nOps; i++) {
    float by = y + CTX_PAD + CTX_HEADER + (float)i * (CTX_LINE + CTX_GAP);
    GfxRect r = { x + CTX_PAD, by, CTX_W - CTX_PAD * 2.0f, CTX_LINE };
    float f = focusAnim[i];
    // The same language as the pills: the focused one INVERTS (light
    // background, dark text), instead of a white ring over a light fill.
    float luma = anim_blend(0.176f, 0.961f, f);
    int color = i == focus ? 17 : 240;
    gfx_color(r, 14.0f / CTX_LINE, luma, luma, luma, a);
    { TxtLine t = txt_line(TXT_PLR_BODY, ops[i].rot, color, color, color, 255);
      txt_draw_alpha(t, r.x + 44.0f,
                         by + (CTX_LINE - t.h) * 0.5f, a); }
    if (f > 0.02f) {
      TxtLine seta = txt_line(TXT_CAPTION2, "▸", color, color, color, 255);
      txt_draw_alpha(seta, r.x + 16.0f,
                         by + (CTX_LINE - seta.h) * 0.5f, a * f);
    }
  }

  { const char *footer = stateOperation == CTX_PENDING
                           ? "Back Close   Please wait..."
                           : operation != CTX_OP_NONE
                           ? "↑ ↓ Navigate   OK Close   Back Close"
                           : "↑ ↓ Navigate   OK Select   Back Close";
    TxtLine t = txt_line(TXT_CAPTION2, footer,
                           155, 159, 169, 255);
    txt_draw_alpha(t, x + CTX_PAD,
                       y + height - CTX_PAD - t.h, a * 0.86f); }
}
