#include "listadd.h"
#include "traktlists.h"
#include "homerows.h"
#include "discover.h"
#include "settings.h"
#include "gfx.h"
#include "text.h"
#include "tex_cache.h"
#include "pointer.h"
#include "anim.h"
#include "layout.h"
#include "ime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>

// --- the screen's geometry ------------------------------------------------------
//
// The header and the controls stand still; the result rows scroll beneath them,
// and the focused one comes to rest at ROWS_TOP the way the Home's focused row
// rests at its shelf line. Rows above it fade out over ROWS_FADE rather than
// being cut.
#define CTRL_Y    136.0f
#define CTRL_H     64.0f
#define CTRL_PADX  28.0f
#define CTRL_ICON  26.0f
#define CTRL_GAPI  14.0f
#define CTRL_GAP   18.0f
#define SEARCH_W  620.0f
#define ROWS_TOP  256.0f
#define ROWS_FADE  28.0f   // ends below the status line, so no poster runs into it
// A row's heading band: the list's name and its button, which is the controls'
// pill above (CTRL_H tall, same type and padding), then HEAD_GAP down to the cards.
#define HEAD_H     CTRL_H
#define HEAD_GAP   28.0f
#define FADE_MS   180.0f

// --- previews ----------------------------------------------------------------------
//
// A result row shows the list's first PV_N titles. They are fetched when the row
// comes near the focus, on threads of their own (at most PV_THREADS at once), and
// kept for the session in a small cache keyed by source and id, so walking back up
// the results or reopening the screen does not ask again. Only what a card draws
// is kept: a whole CatItem is kilobytes of cast and synopsis nobody here reads.
#define PV_N       20
#define PV_CACHE   48
#define PV_THREADS 3
enum { PV_IDLE, PV_LOADING, PV_READY, PV_FAILED };
typedef struct { char poster[512], backdrop[512], title[96]; } PvItem;
typedef struct {
  int src; long id;
  int state, n;
  unsigned used;
  PvItem items[PV_N];
} Pv;
static Pv cache[PV_CACHE];
static int nCache, inFlight;
static unsigned useClock;
static pthread_mutex_t pvLock = PTHREAD_MUTEX_INITIALIZER;

// The entry for a list, made (by evicting the least recently drawn one that is not
// in flight) when there is none. Main thread; NULL only when every slot is loading.
static Pv *pvFind(int src, long id) {
  int i, victim = -1;
  for (i = 0; i < nCache; i++)
    if (cache[i].src == src && cache[i].id == id) { cache[i].used = ++useClock; return &cache[i]; }
  pthread_mutex_lock(&pvLock);
  if (nCache < PV_CACHE) victim = nCache++;
  else
    for (i = 0; i < nCache; i++)
      if (cache[i].state != PV_LOADING && (victim < 0 || cache[i].used < cache[victim].used))
        victim = i;
  if (victim >= 0) {
    memset(&cache[victim], 0, sizeof cache[victim]);
    cache[victim].src = src;
    cache[victim].id = id;
    cache[victim].used = ++useClock;
  }
  pthread_mutex_unlock(&pvLock);
  return victim >= 0 ? &cache[victim] : NULL;
}

static void *threadPreview(void *u) {
  Pv *e = (Pv *)u;
  CatItem *buf = calloc(PV_N, sizeof *buf);
  int n = buf ? tl_items(e->src, e->id, buf, PV_N) : 0, i;
  pthread_mutex_lock(&pvLock);
  // A loading entry is never evicted, so `e` is still this list's.
  for (i = 0; i < n; i++) {
    snprintf(e->items[i].poster, sizeof e->items[i].poster, "%s", buf[i].poster);
    snprintf(e->items[i].backdrop, sizeof e->items[i].backdrop, "%s", buf[i].backdrop);
    snprintf(e->items[i].title, sizeof e->items[i].title, "%s", buf[i].title);
  }
  e->n = n;
  // An empty answer from a list that HAS titles is a failure (no network, a
  // refused key); one that is truly empty says so in the row either way.
  e->state = n > 0 ? PV_READY : PV_FAILED;
  inFlight--;
  pthread_mutex_unlock(&pvLock);
  free(buf);
  return NULL;
}

static void pvWant(const TlList *l) {
  Pv *e = pvFind(l->src, l->id);
  pthread_t th;
  if (!e || e->state != PV_IDLE) return;
  pthread_mutex_lock(&pvLock);
  if (inFlight >= PV_THREADS) { pthread_mutex_unlock(&pvLock); return; }
  inFlight++;
  e->state = PV_LOADING;
  pthread_mutex_unlock(&pvLock);
  if (pthread_create(&th, NULL, threadPreview, e) == 0) pthread_detach(th);
  else threadPreview(e);
}

// The entry's state under the lock, so a card never reads items mid-copy.
static int pvState(const Pv *e) {
  int s;
  pthread_mutex_lock(&pvLock);
  s = e->state;
  pthread_mutex_unlock(&pvLock);
  return s;
}

// --- state ----------------------------------------------------------------------------

enum { C_TRAKT, C_MDB, C_SEARCH, C_N };
static int isOpen, closing, dirty;
static float shown;                 // 0..1, the fade in and out
static int src = TL_TRAKT;
static char query[96];
// The focus: row -1 is the controls (column ctrlCol), 0.. the results.
static int frow = -1, ctrlCol = C_TRAKT;
static int colOf[TL_BROWSE_MAX];
static float scrollY, velY;
static float scrollX[TL_BROWSE_MAX], velX[TL_BROWSE_MAX];
static float ctrlAnim[C_N], rowAnim[TL_BROWSE_MAX];
static char flash[160];
static Uint32 flashUntil;

static int landscape(void) { return settings_posters_landscape(); }
static float cardW(void) { return landscape() ? NV_CARD_LAND_W : NV_CARD_W; }
static float cardH(void) { return landscape() ? NV_CARD_LAND_H : NV_CARD_H; }
static float rowStep(void) {
  return HEAD_H + HEAD_GAP + cardH() + (landscape() ? NV_ROW_GAP_LAND : NV_ROW_GAP);
}

// The results on screen belong to the tab only when they came from its source.
// They stay up while a new search for the same source is in flight, so the rows
// do not blink out on every query.
static int results(void) {
  const TlList *first = tl_browse_item(0);
  return first && first->src == src ? tl_browse_n() : 0;
}

static void say(const char *msg) {
  snprintf(flash, sizeof flash, "%s", msg);
  flashUntil = SDL_GetTicks() + 2600;
}

static void browse(void) {
  if (!tl_source_usable(src)) return;
  tl_browse(src, query);
}

static void resetFocus(void) {
  frow = -1;
  memset(colOf, 0, sizeof colOf);
  memset(scrollX, 0, sizeof scrollX);
  memset(velX, 0, sizeof velX);
  memset(rowAnim, 0, sizeof rowAnim);
}

void listadd_open(void) {
  isOpen = 1; closing = 0; dirty = 0;
  shown = settings_animations_reduced() ? 1.0f : 0.0f;
  ctrlCol = C_SEARCH;
  resetFocus();
  scrollY = velY = 0.0f;
  flash[0] = 0;
  // What the tab shows is fetched once per visit; the Settings finder may have
  // left another source's results, or a different query, in the shared slot.
  if (tl_browse_src() != src || tl_browse_state() != TLB_READY ||
      strcmp(tl_browse_query(), query))
    browse();
  printf("[listadd] open (%s)\n", tl_source_name(src));
  fflush(stdout);
}

int listadd_is_open(void) { return isOpen; }
int listadd_covers_screen(void) { return isOpen && !closing && shown >= 0.999f; }

static void startClose(void) {
  if (ime_is_open()) ime_close();
  closing = 1;
  // One rebuild for the whole visit, as Settings does on leaving its finder.
  if (dirty) { dirty = 0; disc_rebuild(); }
}

static void setSource(int s) {
  if (s == src) return;
  src = s;
  resetFocus();
  scrollY = velY = 0.0f;
  if (!tl_source_usable(src)) {
    say(src == TL_MDB ? "MDBList needs an mdbList API key on your Nuvio account"
                      : "This build has no Trakt client id");
    return;
  }
  browse();
}

static void submitSearch(void) {
  size_t a = 0, b = strlen(query);
  while (query[a] == ' ') a++;
  while (b > a && query[b - 1] == ' ') b--;
  memmove(query, query + a, b - a);
  query[b - a] = 0;
  resetFocus();
  browse();
}

static void toggle(int i) {
  const TlList *l = tl_browse_item(i);
  char key[48], msg[160];
  if (!l) return;
  tl_key(l->src, l->id, key, sizeof key);
  if (tl_has(l->src, l->id)) {
    tl_remove(l->src, l->id);
    snprintf(msg, sizeof msg, "Removed \xe2\x80\x9c%s\xe2\x80\x9d from your Home", l->name);
  } else if (tl_add(l)) {
    homerows_list_place(key);
    snprintf(msg, sizeof msg, "Added \xe2\x80\x9c%s\xe2\x80\x9d to the top of your Home", l->name);
  } else {
    say("Ten lists is the most the Home takes. Remove one first.");
    return;
  }
  dirty = 1;
  say(msg);
}

static GfxRect searchRect(void);

static void pointCtrl(int c, int unused) { (void)unused; frow = -1; ctrlCol = c; }
static void pointCard(int r, int c) { frow = r; colOf[r] = c; }

void listadd_event(const SDL_Event *e) {
  SDL_Keycode k;
  int ok, back, n = results();
  if (!isOpen || closing) return;
  if (ime_is_open()) {
    int len = (int)strlen(query);
    if (ime_edit(e, query, &len, (int)sizeof query)) return;
  }
  if (e->type != SDL_KEYDOWN) return;
  k = e->key.keysym.sym;
  ok = k == SDLK_RETURN || k == SDLK_KP_ENTER;
  back = k == SDLK_ESCAPE || k == SDLK_AC_BACK;
#ifdef __APPLE__
  if (k == SDLK_BACKSPACE && !ime_is_open()) back = 1;
#endif
  if (ime_is_open()) {
    int arrow = k == SDLK_UP || k == SDLK_DOWN || k == SDLK_LEFT || k == SDLK_RIGHT;
    // The LG keyboard can go down by itself; from then on the arrows are ours.
    if (arrow && !ime_shown()) ime_close();
    else {
      if (back) ime_close();
      else if (ok) { ime_close(); submitSearch(); }
      return;
    }
  }
  if (back) { if (!e->key.repeat) startClose(); return; }

  if (frow < 0) {
    if (k == SDLK_LEFT && ctrlCol > 0) ctrlCol--;
    else if (k == SDLK_RIGHT && ctrlCol < C_N - 1) ctrlCol++;
    else if (k == SDLK_DOWN && n > 0) frow = 0;
    else if (ok) {
      if (ctrlCol == C_TRAKT) setSource(TL_TRAKT);
      else if (ctrlCol == C_MDB) setSource(TL_MDB);
      else if (!tl_source_usable(src)) setSource(src == TL_MDB ? TL_TRAKT : TL_MDB);
      else if (ime_usable()) ime_open(searchRect());
      else if (tl_browse_state() == TLB_FAILED) browse();
    }
    return;
  }
  if (frow >= n) frow = n - 1;
  if (frow < 0) return;
  if (k == SDLK_UP) frow--;
  else if (k == SDLK_DOWN && frow < n - 1) frow++;
  else if (k == SDLK_LEFT && colOf[frow] > 0) colOf[frow]--;
  else if (k == SDLK_RIGHT) {
    const TlList *l = tl_browse_item(frow);
    Pv *pv = l ? pvFind(l->src, l->id) : NULL;
    if (pv && pvState(pv) == PV_READY && colOf[frow] < pv->n - 1) colOf[frow]++;
  }
  else if (ok && !e->key.repeat) toggle(frow);
}

void listadd_update(float dt, Uint32 now) {
  int reduced = settings_animations_reduced(), n, i, c;
  (void)now;
  if (!isOpen) return;
  shown = reduced ? (closing ? 0.0f : 1.0f)
                  : anim_ramp(shown, closing ? 0.0f : 1.0f, dt, FADE_MS);
  if (closing && shown <= 0.001f) { isOpen = 0; closing = 0; return; }
  if (ime_is_open()) ime_pump();
  tl_browse_poll();
  n = results();
  if (frow >= n) frow = n - 1;

  // The rows near the focus ask for their previews: the one in focus first, then
  // the ones it is about to walk onto.
  { int order[5] = { 0, 1, 2, -1, 3 }, base = frow < 0 ? 0 : frow;
    for (i = 0; i < 5; i++) {
      int r = base + order[i];
      if (r >= 0 && r < n) pvWant(tl_browse_item(r));
    } }

  for (c = 0; c < C_N; c++) {
    float t = frow < 0 && ctrlCol == c ? 1.0f : 0.0f;
    ctrlAnim[c] = reduced ? t : anim_spring(ctrlAnim[c], t, dt, NV_SPRING_FOCUS);
  }
  for (i = 0; i < n && i < TL_BROWSE_MAX; i++) {
    float t = i == frow ? 1.0f : 0.0f, step = cardW() + NV_CARD_GAP, left, goal;
    float util = NV_SCREEN_W - settings_content_x() - NV_HOME_SAFE_RIGHT;
    rowAnim[i] = reduced ? t : anim_spring(rowAnim[i], t, dt, NV_SPRING_FOCUS);
    // The Home's own rules: "First slot" (Settings > Poster focus) pins the focused
    // card to the row's start; otherwise scroll only as far as it needs to fit.
    left = colOf[i] * step;
    goal = scrollX[i];
    if (settings_row_first_slot()) goal = left;
    else {
      if (left + cardW() - goal > util) goal = left + cardW() - util;
      if (left < goal) goal = left;
    }
    if (goal < 0) goal = 0;
    scrollX[i] = anim_spring2_reduced(&velX[i], scrollX[i], goal, dt, NV_SPRING2_SCROLL, reduced);
  }
  scrollY = anim_spring2_reduced(&velY, scrollY, frow < 0 ? 0.0f : frow * rowStep(), dt,
                                 NV_SPRING2_SCROLL, reduced);
}

// --- drawing ------------------------------------------------------------------------

// The title screen's buttons (detail.c): #222 at rest; focus turns the fill light
// and adds a 4px white ring. `f` is the focus, 0..1; `on` keeps a tab lit.
static void pill(GfxRect r, float f, int on, float *ink) {
  float fill = on ? 0.32f : 0.133f;
  fill += (0.961f - fill) * f;
  *ink = 1.0f + (0.067f - 1.0f) * f;
  if (f > 0.01f)
    gfx_color((GfxRect){ r.x - 4, r.y - 4, r.w + 8, r.h + 8 }, NV_RADIUS_PILL, 1, 1, 1, f);
  gfx_color(r, NV_RADIUS_PILL, fill, fill, fill, 1.0f);
}

static float tabW(int c) {
  return roundf(CTRL_PADX * 2 + txt_width(TXT_CALLOUT, c == C_TRAKT ? "Trakt" : "MDBList"));
}
static GfxRect searchRect(void) {
  float x = settings_content_x() + tabW(C_TRAKT) + tabW(C_MDB) + CTRL_GAP * 2;
  return (GfxRect){ x, CTRL_Y, SEARCH_W, CTRL_H };
}

static void drawControls(void) {
  float x = settings_content_x(), ink;
  int c, k;
  for (c = C_TRAKT; c <= C_MDB; c++) {
    const char *label = c == C_TRAKT ? "Trakt" : "MDBList";
    GfxRect r = { x, CTRL_Y, tabW(c), CTRL_H };
    TxtLine l;
    pill(r, ctrlAnim[c], (c == C_MDB) == (src == TL_MDB), &ink);
    k = (int)(ink * 255.0f + 0.5f);
    l = txt_line(TXT_CALLOUT, label, k, k, k, 255);
    txt_draw(l, r.x + (r.w - l.w) * 0.5f, r.y + (r.h - l.h) * 0.5f);
    pointer_zone(r.x, r.y, r.w, r.h, pointCtrl, c, 0);
    x += r.w + CTRL_GAP;
  }
  { GfxRect r = searchRect();
    int typing = ime_is_open();
    char text[160];
    TxtLine l;
    float f = typing ? 1.0f : ctrlAnim[C_SEARCH];
    pill(r, f, 0, &ink);
    if (query[0] || typing) snprintf(text, sizeof text, "%s%s", query, typing ? "|" : "");
    else snprintf(text, sizeof text, "Search %s lists", tl_source_name(src));
    k = (int)(ink * 255.0f + 0.5f);
    if (!query[0] && !typing) k = (int)(k * 0.62f + (f > 0.5f ? 40 : 0));
    gfx_icon((GfxRect){ r.x + CTRL_PADX, r.y + (CTRL_H - CTRL_ICON) * 0.5f, CTRL_ICON, CTRL_ICON },
             "lists_search", ink, ink, ink, 1.0f);
    l = txt_line_trim(TXT_CALLOUT, text, k, k, k, 255,
                      r.w - CTRL_PADX * 2 - CTRL_ICON - CTRL_GAPI);
    txt_draw(l, r.x + CTRL_PADX + CTRL_ICON + CTRL_GAPI, r.y + (CTRL_H - l.h) * 0.5f);
    pointer_zone(r.x, r.y, r.w, r.h, pointCtrl, C_SEARCH, 0);
  }
}

// The context line, top right against the title, set as Library sets its own
// (library.c): grey with wide tracking. It says what the rows ARE ("MDBList · Top
// lists"), and in their place, while there is something to tell, what is going
// on: loading, a failure, nothing found, or the list just added or removed.
static void drawContext(void) {
  char line[200];
  int st = tl_browse_src() == src ? tl_browse_state() : TLB_LOADING, warn = 0;
  float w;
  if (flash[0] && SDL_GetTicks() < flashUntil) snprintf(line, sizeof line, "%s", flash);
  else if (!tl_source_usable(src)) {
    warn = 1;
    snprintf(line, sizeof line, src == TL_MDB
             ? "MDBList needs an mdbList API key on your Nuvio account"
             : "This build has no Trakt client id");
  } else if (st == TLB_LOADING || st == TLB_IDLE)
    snprintf(line, sizeof line, "%s  \xc2\xb7  %s", tl_source_name(src),
             query[0] ? "Searching\xe2\x80\xa6" : "Loading\xe2\x80\xa6");
  else if (st == TLB_FAILED) {
    warn = 1;
    snprintf(line, sizeof line, "Couldn't reach %s", tl_source_name(src));
  } else if (!results())
    snprintf(line, sizeof line, "%s  \xc2\xb7  Nothing found", tl_source_name(src));
  else if (tl_browse_query()[0])
    snprintf(line, sizeof line, "%s  \xc2\xb7  %d %s", tl_source_name(src), results(),
             results() == 1 ? "result" : "results");
  else
    snprintf(line, sizeof line, "%s  \xc2\xb7  %s", tl_source_name(src),
             src == TL_MDB ? "Top lists" : "Popular lists");
  { int r = warn ? 236 : 128, g = warn ? 170 : 128, bl = warn ? 140 : 128;
    w = txt_tracking(TXT_SRCH_NAME, line, r, g, bl, -1.0f, 0.0f, 0.0f, 4.0f);
    txt_tracking(TXT_SRCH_NAME, line, r, g, bl,
                 NV_SCREEN_W - NV_HOME_SAFE_RIGHT - w, NV_SEARCH_TOP + 10.0f, 0.95f, 4.0f); }
}

// The Home's framed poster card (home.c, the ROW_NORMAL path): the picture sits
// NV_CARD_PAD inside the card's box, so the gap from the row's heading to the
// first visible edge is the Home's. No poster takes a focus ring here: OK acts on
// the row's button, and a ringed poster would claim that it is the target. The
// focused row's posters only lift out of the dim (`lit`), as previews.
static GfxRect inset(GfxRect r, float in) {
  return (GfxRect){ r.x + in, r.y + in, r.w - in * 2, r.h - in * 2 };
}

static void drawCard(GfxRect r, const PvItem *it, float lit) {
  float rpx = settings_radius_poster_px(), scale = r.h / cardH();
  float pad = roundf(NV_CARD_PAD * scale), artR;
  GfxRect art = inset(r, pad);
  const char *path = it ? (landscape() && it->backdrop[0] ? it->backdrop : it->poster) : NULL;
  GLuint tex = path && path[0]
             ? tex_get_width(path, (cardW() - 2.0f * NV_CARD_PAD) * (1.0f + NV_FOCUS_SCALE_POSTER))
             : 0;
  float dim = NV_DIM_UNFOCUSED + (1.0f - NV_DIM_UNFOCUSED) * lit;
  artR = (rpx - NV_FRAME_BORDER > 0.0f ? rpx - NV_FRAME_BORDER : 0.0f) / art.h;
  if (tex) {
    gfx_tex_aspect_current = tex_aspect(path);
    gfx_rect(art, tex, GFX_CARD, 0, 0, 0, artR, dim, dim, dim, 1.0f);
    gfx_tex_aspect_current = 0.0f;
  } else if (!it || (path && path[0] && !tex_failed(path))) {
    gfx_skeleton(art, artR, NV_POSTER_BG_R, NV_POSTER_BG_G, NV_POSTER_BG_B, 1.0f);
  } else {
    // No art to be had: the plate and the title, as the Home does for a missing poster.
    gfx_color(art, artR, NV_POSTER_BG_R * dim, NV_POSTER_BG_G * dim, NV_POSTER_BG_B * dim, 1.0f);
    txt_block_trim(TXT_CAPTION, it->title, 200, 202, 208, art.x + 18, art.y + 18, art.w - 36, 30, 1, 4);
  }
}

// The heading as the Home would show it, plus what the Home does not: who made the
// list, how long it is, and whether it is already on the Home.
static void drawRowHead(int i, const TlList *l, float y) {
  int on = tl_has(l->src, l->id);
  float x = settings_content_x(), right = NV_SCREEN_W - NV_HOME_SAFE_RIGHT, f = rowAnim[i];
  char meta[200], chip[48];
  TxtLine tc, tl, tm;
  float cw, ink;
  GfxRect cr;
  // THE BUTTON IS THE FOCUS. Walking onto a row lands on this pill — OK acts on
  // it, whichever poster the arrows have scrolled to — so it is drawn as the
  // tabs and the search field above are (pill(): the 4px white ring and the fill
  // turning light), at their height and in their type. A list already on the Home
  // keeps the selected tab's lighter fill and a tick; under the focus it offers
  // what OK will do, a minus and "Remove from Home".
  if (on) snprintf(chip, sizeof chip, f > 0.5f ? "Remove from Home" : "On your Home");
  else    snprintf(chip, sizeof chip, "Add to Home");
  { int k;
    tc = txt_line(TXT_CALLOUT, chip, 0, 0, 0, 255);
    cw = roundf(CTRL_PADX * 2 + CTRL_ICON + CTRL_GAPI + tc.w);
    cr = (GfxRect){ right - cw, y, cw, HEAD_H };
    pill(cr, f, on, &ink);
    k = (int)(ink * 255.0f + 0.5f);
    tc = txt_line(TXT_CALLOUT, chip, k, k, k, 255); }
  gfx_icon((GfxRect){ cr.x + CTRL_PADX, cr.y + (HEAD_H - CTRL_ICON) * 0.5f, CTRL_ICON, CTRL_ICON },
           on ? (f > 0.5f ? "lists_remove" : "lists_added") : "lists_add", ink, ink, ink, 1.0f);
  txt_draw(tc, cr.x + CTRL_PADX + CTRL_ICON + CTRL_GAPI, cr.y + (HEAD_H - tc.h) * 0.5f);

  tl = txt_line_trim(TXT_ROW_TITLE, l->name, 245, 246, 249, 255, (right - cw - 40 - x) * 0.6f);
  txt_draw(tl, x, y + (HEAD_H - tl.h) * 0.5f);
  { size_t w = 0;
    meta[0] = 0;
    if (l->user[0]) w += (size_t)snprintf(meta + w, sizeof meta - w, "by %s  \xc2\xb7  ", l->user);
    w += (size_t)snprintf(meta + w, sizeof meta - w, l->items == 1 ? "%d title" : "%d titles", l->items);
    if (l->likes > 0 && w < sizeof meta)
      snprintf(meta + w, sizeof meta - w, "  \xc2\xb7  %d likes", l->likes); }
  tm = txt_line_trim(TXT_HERO_META, meta, 160, 165, 176, 255, right - cw - 40 - x - tl.w - 22);
  txt_draw(tm, x + tl.w + 22, y + (HEAD_H - tm.h) * 0.5f);
}

static void drawRows(void) {
  int n = results(), i, c;
  float y0 = ROWS_TOP - scrollY, step = rowStep(), w = cardW(), h = cardH();
  float clipTop = ROWS_TOP - ROWS_FADE;
  gfx_crop(0, clipTop, NV_SCREEN_W, NV_SCREEN_H - clipTop);
  pointer_clip(0, clipTop, NV_SCREEN_W, NV_SCREEN_H - clipTop);
  for (i = 0; i < n; i++) {
    const TlList *l = tl_browse_item(i);
    float y = y0 + i * step, cardY = y + HEAD_H + HEAD_GAP, fade, base = gfx_opacity_group;
    Pv *pv;
    int st;
    if (!l || y > NV_SCREEN_H || y + step < clipTop) continue;
    fade = anim_clamp((y - clipTop) / ROWS_FADE, 0.0f, 1.0f);
    gfx_opacity_group = base * fade * fade * (3 - 2 * fade);
    drawRowHead(i, l, y);
    pv = pvFind(l->src, l->id);
    st = pv ? pvState(pv) : PV_LOADING;
    if (st == PV_FAILED && pv && !pv->n) {
      txt_draw(txt_line(TXT_HERO_META,
                        l->items > 0 ? "Couldn't load this list's titles" : "This list is empty",
                        150, 152, 158, 255),
               settings_content_x(), cardY + 20);
    } else {
      int cards = st == PV_READY ? pv->n : 8;
      for (c = 0; c < cards && c < PV_N; c++) {
        float cx = settings_content_x() + c * (w + NV_CARD_GAP) - scrollX[i] + w * 0.5f;
        GfxRect r;
        if (cx < -w || cx > NV_SCREEN_W + w) continue;
        r = (GfxRect){ roundf(cx - w * 0.5f), roundf(cardY), w, h };
        drawCard(r, st == PV_READY ? &pv->items[c] : NULL, rowAnim[i]);
        if (st == PV_READY) pointer_zone(r.x, r.y, r.w, r.h, pointCard, i, c);
      }
    }
    gfx_opacity_group = base;
  }
  gfx_no_crop();
  pointer_no_clip();
}

void listadd_draw(Uint32 now) {
  float base = gfx_opacity_group;
  (void)now;
  if (!isOpen) return;
  gfx_opacity_group = base * anim_smooth(shown);
  gfx_color((GfxRect){ 0, 0, NV_SCREEN_W, NV_SCREEN_H }, 0.0f, 0.051f, 0.051f, 0.051f, 1.0f);
  // The page title as Search and Library set theirs: 48/600, 1px letter-spacing.
  txt_tracking(TXT_TITLE3, "Add lists", 255, 255, 255, settings_content_x(), NV_SEARCH_TOP,
               1.0f, NV_SEARCH_TITLE_LS);
  drawContext();
  drawControls();
  drawRows();
  gfx_opacity_group = base;
}
