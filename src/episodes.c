#include "episodes.h"
#include "catalog.h"
#include "discover.h"
#include "extras.h"
#include "gfx.h"
#include "tex_cache.h"
#include "text.h"
#include "layout.h"
#include "anim.h"
#include "watchedep.h"
#include "trakt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

// THE PANEL'S OWN HEIGHT, built from the pieces below it so the sheet is exactly
// as tall as what it holds. The measurements are in layout.h under NV_ERAIL_*.
//
//   72 top padding
//  + the detail block (meta 27 + 8 + title 48 + 12 + overview 62 = 157)
//  + 32 margin under it
//  + the viewport (16 + card + 16)
//  + 48 bottom padding
#define EP_CARD_H   (NV_ERAIL_CARD_W * 9.0f / 16.0f)   /* the thumb is 16:9 */
#define EP_CTITLE_H  42.0f                             /* 12 margin + a 24px line */
#define EP_ITEM_H   (EP_CARD_H + EP_CTITLE_H)
#define EP_DETAIL_H 157.0f
#define EP_VIEW_H   (EP_ITEM_H + NV_ERAIL_VIEW_PAD * 2)
#define EP_SEASONS_H 64.0f                             /* the season tabs row */
#define EP_H        (NV_ERAIL_PAD_TOP + EP_SEASONS_H + EP_DETAIL_H + \
                     NV_ERAIL_DETAIL_MB + EP_VIEW_H + NV_ERAIL_PAD_BOT)
// The pitch one card occupies along the rail.
#define EP_PITCH    (NV_ERAIL_CARD_W + NV_ERAIL_GAP)
static int is_open, title, currentT, currentE, season, focus, group;
static int requestT, requestE;
static float anim, scroll;
static int locateCurrent;

// --- MARKING EPISODES AS WATCHED, FROM THE TV --------------------------------
//
// The app could always READ which episodes were watched (extras.c fills the grid
// this list draws the tick from) and could never WRITE it: marking was only
// available per TITLE, from the poster's menu, which on a series meant "all of
// it" and nothing finer. The three gestures below are the same batch at three
// sizes — this episode, everything up to it, the whole season — which is why
// they share one applier.
enum { WM_THIS = 0, WM_UP_TO, WM_SEASON, WM_N };
static int wmOpen, wmFocus, wmT, wmE;
// The SENSE of the gesture, decided when the menu opens: somebody looking at an
// episode already marked wants to unmark it. Unknown (-1) counts as unwatched.
static int wmWatched;
// The hold that opens the menu, and the guard that stops a release the list
// never saw from counting as a press — the same guard home.c, detail.c and
// ctxmenu.c carry, for the same defect.
static int wmHolding;
static Uint32 wmSince;
static char wmName[160];

// THE SEND GOES TO A THREAD. trakt_mark_episodes is synchronous by design and
// can spend its full 20 s timeout; on the draw thread that freezes the TV.
// The LOCAL effect has already happened before the thread is born, so the list
// redraws in the same frame and this thread only carries the news to the server.
typedef struct { char imdb[24]; WatchedPair pairs[64]; int n, watched; } Send;
static void *sendWatched(void *u) {
  Send *e = (Send *)u;
  trakt_mark_episodes(e->imdb, e->pairs, e->n, e->watched);
  free(e);
  return NULL;
}

static int nSeasons(void) {
  const CatItem *c = cat_item(title);
  return c && c->nSeasons > 0 ? c->nSeasons : 1;
}
static int numSeason(int i) {
  const CatItem *c = cat_item(title);
  return c && c->nSeasons > 0 ? c->seasons[i] : currentT;
}
static const CatEp *epLine(int line) {
  int n = cat_n_episodes(title);
  for (int i = 0, j = 0; i < n; i++) {
    const CatEp *ep = cat_episode(title, i);
    if (ep && ep->season == numSeason(season) && j++ == line) return ep;
  }
  return NULL;
}
static int nLines(void) {
  int n = 0;
  for (int i=0;i<cat_n_episodes(title);i++) {
    const CatEp *e=cat_episode(title,i);
    if(e && e->season==numSeason(season)) n++;
  }
  return n;
}
void episodes_open(int idx, int t, int e) {
  title = idx; currentT = t; currentE = e; is_open = 1;
  wmOpen = 0; wmHolding = 0;
  season = focus = 0; group = 1; requestE = 0; scroll = 0;
  locateCurrent = 1;
  for (int i = 0; i < nSeasons(); i++) if (numSeason(i) == t) season = i;
  for (int i = 0; i < nLines(); i++) if (epLine(i)->episode == e) focus = i;
  disc_episodes(title, t);
}
int episodes_is_open(void) { return is_open; }
void episodes_close(void) { is_open = 0; wmOpen = 0; wmHolding = 0; }
int episodes_chose(int *t, int *e) {
  if (!requestE) return 0;
  *t = requestT; *e = requestE; requestE = 0; return 1;
}
// How many episodes each gesture would touch. WM_THIS is always 1; the other two
// come out of the MAP, in count mode (a NULL batch) — the label has to say the
// number before the person commits, and the map is also what decides whether the
// option is worth offering at all.
static int wmCount(int mode) {
  const CatItem *ci = cat_item(title);
  if (!ci || !ci->imdb[0]) return 0;
  if (mode == WM_THIS) return 1;
  if (mode == WM_UP_TO) return watchedep_up_to_here(ci->imdb, wmT, wmE, NULL, 0);
  return watchedep_season(ci->imdb, wmT, NULL, 0);
}

static void wmLabel(int mode, char *dst, size_t size) {
  int k = wmCount(mode);
  const char *verb = wmWatched ? "Mark" : "Unmark";
  if (mode == WM_THIS)  snprintf(dst, size, "%s this episode", verb);
  else if (mode == WM_UP_TO) snprintf(dst, size, "%s everything up to here (%d)", verb, k);
  else snprintf(dst, size, "%s the whole season (%d)", verb, k);
}

// Applies the gesture: builds the batch, changes the local state at once, and
// hands the rest to a thread. Returns 0 when there is nothing to do — which is
// the case of marking what is already marked, and must not spend a request.
static int wmApply(int mode) {
  const CatItem *ci = cat_item(title);
  WatchedPair batch[64];
  int n = 0, changed;
  if (!ci || !ci->imdb[0]) return 0;
  if (mode == WM_THIS) {
    batch[0].season = (short)wmT;
    batch[0].episode = (short)wmE;
    n = 1;
  } else if (mode == WM_UP_TO) {
    n = watchedep_up_to_here(ci->imdb, wmT, wmE, batch, 64);
  } else {
    // THE EPISODE'S season, not the selected tab's. Inside this sheet they are
    // the same; the distinction costs nothing and survives a caller that has no
    // tab at all.
    n = watchedep_season(ci->imdb, wmT, batch, 64);
  }
  if (n < 1) return 0;
  changed = watchedep_mark_batch(ci->imdb, batch, n, wmWatched);
  if (!changed) return 0;
  { Send *send = (Send *)calloc(1, sizeof *send);
    pthread_t thread;
    if (!send) return changed;
    snprintf(send->imdb, sizeof send->imdb, "%s", ci->imdb);
    memcpy(send->pairs, batch, sizeof(WatchedPair) * (size_t)n);
    send->n = n; send->watched = wmWatched;
    if (pthread_create(&thread, NULL, sendWatched, send) == 0) pthread_detach(thread);
    else free(send); }
  return changed;
}

static void wmOpenFor(const CatEp *ep) {
  const CatItem *ci = cat_item(title);
  if (!ci || !ci->imdb[0] || !ep) return;
  wmT = ep->season;
  wmE = ep->episode;
  snprintf(wmName, sizeof wmName, "%s", ep->name);
  wmWatched = watchedep_state(ci->imdb, wmT, wmE) == 1 ? 0 : 1;
  wmOpen = 1;
  wmFocus = 0;
}

// How many options this menu shows. "Up to here" and "the whole season" are
// hidden when the map cannot name a single episode for them: an option that
// would silently do nothing is worse than its absence.
static int wmOptions(void) {
  int n = 1;
  if (wmCount(WM_UP_TO) > 0) n++;
  if (wmCount(WM_SEASON) > 0) n++;
  return n;
}

// The option at a focus position, once the hidden ones are skipped.
static int wmModeAt(int slot) {
  int i, k = 0;
  for (i = 0; i < WM_N; i++) {
    if (i != WM_THIS && wmCount(i) < 1) continue;
    if (k++ == slot) return i;
  }
  return WM_THIS;
}

static void wmEvent(const SDL_Event *ev) {
  SDL_Keycode k = ev->key.keysym.sym;
  if (ev->type != SDL_KEYDOWN) return;
  if (k == SDLK_UP)   { if (wmFocus > 0) wmFocus--; return; }
  if (k == SDLK_DOWN) { if (wmFocus < wmOptions() - 1) wmFocus++; return; }
  if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) {
    wmApply(wmModeAt(wmFocus));
    wmOpen = 0;
    return;
  }
  wmOpen = 0;   // any other key closes it
}

void episodes_event(const SDL_Event *ev) {
  if (!is_open) return;

  // THE MENU EATS THE KEY WHILE IT IS UP. The same rule as the poster's menu:
  // it is the most recent thing on screen and the one the person is looking at.
  if (wmOpen) { wmEvent(ev); return; }

  // A LONG PRESS ON AN EPISODE ROW opens the watched menu. Only in the list
  // group: over the season tabs or the header there is no episode to mark.
  { SDL_Keycode ko = ev->key.keysym.sym;
    int isOk = (ko == SDLK_RETURN || ko == SDLK_KP_ENTER || ko == SDLK_SPACE);
    if (isOk && group == 1) {
      if (ev->type == SDL_KEYDOWN && !ev->key.repeat) {
        wmHolding = 1; wmSince = SDL_GetTicks(); return;
      }
      if (ev->type == SDL_KEYUP) {
        // THE GUARD ASKS WHETHER THE PRESS HAPPENED HERE, not how long it was.
        // A release without a press in this layer is not a click — the layer
        // above may have closed on the KEYDOWN and let the KEYUP leak through.
        //
        // TESTING `duration != 0` INSTEAD WOULD SWALLOW A FAST TAP: down and up
        // in the same millisecond give 0, which is legitimate.
        int wasHere = wmHolding;
        Uint32 duration = wasHere ? SDL_GetTicks() - wmSince : 0;
        wmHolding = 0;
        if (!wasHere) return;
        if (duration >= NV_HOLD_MS) { wmOpenFor(epLine(focus)); return; }
        // A SHORT TAP OPENS THE EPISODE, and it happens HERE rather than in the
        // OK branch below: the KEYUP is the only moment that knows the
        // DURATION, and it is the duration that separates opening from holding.
        { const CatEp *ep = epLine(focus);
          if (ep) {
            if (ep->season != currentT || ep->episode != currentE) {
              requestT = ep->season; requestE = ep->episode;
            }
            is_open = 0;
          } }
        return;
      }
      return;
    }
    // OK anywhere else keeps acting on the KEYDOWN, and the stray KEYUP that
    // follows must not fall through to the handler below.
    if (isOk && ev->type == SDL_KEYUP) { wmHolding = 0; return; }
  }

  if (ev->type != SDL_KEYDOWN) return;
  SDL_Keycode k = ev->key.keysym.sym;
  if(k==SDLK_r) { disc_episodes(title,numSeason(season)); return; }
  if (k == SDLK_ESCAPE || k == SDLK_BACKSPACE || k == SDLK_DELETE || k == SDLK_AC_BACK) {
    is_open = 0; return;
  }
  int nt = nSeasons(), n = nLines();
  // THE RAIL IS HORIZONTAL NOW, so the two axes have swapped jobs: up/down moves
  // between the sheet's rows (close / seasons / rail) and left/right moves WITHIN
  // whichever row holds the focus. In the old drawer both of those were up/down on
  // one axis, which is what a vertical list of rows wanted.
  if (k == SDLK_UP)   { if (group > -1) group--; }
  if (k == SDLK_DOWN) { if (group < 1) group++; }
  // 0 is the floor now: the sheet has no header, so there is no Close row above
  // the seasons to walk up into. BACK is the way out, as it already was.
  if (group < 0) group = 0;
  if (group == 1 && k == SDLK_LEFT  && focus > 0)     focus--;
  if (group == 1 && k == SDLK_RIGHT && focus < n - 1) focus++;
  if (group == 0 && (k == SDLK_LEFT || k == SDLK_RIGHT)) {
    int new = season + (k == SDLK_RIGHT ? 1 : -1);
    if (new >= 0 && new < nt) {
      season = new; focus = 0; scroll = 0;
      locateCurrent = 0;
      disc_episodes(title, numSeason(season));
    }
  }
  if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
    if (group == 0) {
      group = 1;
      if (!n) disc_episodes(title,numSeason(season));
    }
    // group == 1 is handled on the KEYUP above, which is the only place that
    // knows whether this was a tap or a hold.
  }
}
void episodes_update(float dt) {
  anim = anim_spring(anim, is_open ? 1 : 0, dt, NV_SPRING_SCREEN);
  if(!is_open && anim<.005f) return;
  disc_episodes_pending();
  int n = nLines();
  if(locateCurrent && n) {
    for(int i=0;i<n;i++) if(epLine(i)->episode==currentE) focus=i;
    locateCurrent=0;
  }
  if (focus >= n) focus = n > 0 ? n - 1 : 0;
  // The rail's offset, in pixels along X. The focused card is pinned to the
  // gutter rather than merely kept in view: on a rail of large cards the eye goes
  // to a fixed place, and a card that sometimes sits left and sometimes right of
  // centre makes every move feel like a different distance. The track moves, the
  // selection does not.
  { float max = n * EP_PITCH - NV_ERAIL_GAP - (NV_SCREEN_W - NV_ERAIL_GUTTER * 2);
    float target = focus * EP_PITCH;
    if (max < 0) max = 0;
    if (target > max) target = max;
    if (target < 0) target = 0;
    scroll = anim_spring(scroll, target, dt, NV_SPRING_SCROLL); }
}
// One episode card: a 16:9 still with the code burnt into its base, the "Playing"
// pill when it is the one on screen, and the name under it.
static void cardDraw(int i, float x, float y, int sel, float a) {
  const CatEp *ep = epLine(i);
  const CatItem *ci = cat_item(title);
  float w = NV_ERAIL_CARD_W, h = EP_CARD_H;
  float r, cx, cy;
  char num[40];
  const char *art;
  GLuint tex;
  int current;

  if (!ep) return;
  // The selected card GROWS, about its own centre, and nothing else changes: no
  // fill, no border on the item itself. The ring below rides on the thumbnail.
  if (sel) {
    float g = (NV_ERAIL_SCALE - 1.0f) * 0.5f;
    x -= w * g; y -= h * g; w *= NV_ERAIL_SCALE; h *= NV_ERAIL_SCALE;
  }
  r = NV_ERAIL_THUMB_R / h;
  snprintf(num, sizeof num, "S%dE%d", ep->season, ep->episode);

  gfx_color((GfxRect){ x, y, w, h }, r, 1, 1, 1, 0.07f * a);
  art = ep->thumb[0] ? ep->thumb : (ci ? ci->backdrop : "");
  tex = art && art[0] ? tex_get_width(art, (int)NV_ERAIL_CARD_W) : 0;
  if (tex) {
    gfx_tex_aspect_current = tex_aspect(art);
    gfx_rect((GfxRect){ x, y, w, h }, tex, GFX_CARD, 0, 0, 0, r, 0, 0, 0, a);
    gfx_tex_aspect_current = 0;
  }
  // The code has to stay readable over whatever the still happens to be, so the
  // base of the card is shaded rather than the code given a chip of its own —
  // the same reasoning as GFX_CORNER_SCRIM: a container reads as a control.
  gfx_rect((GfxRect){ x, y, w, h }, 0, GFX_EP_SCRIM, 0, 0, 0, r, 0, 0, 0, a);

  if (sel) {
    // A 4px white border INSIDE the thumbnail's edge. Inset because the ring
    // belongs to the picture — outside it would collide with the neighbouring
    // card across the 24px gap once this one is scaled up.
    gfx_rect((GfxRect){ x, y, w, h }, 0, GFX_RING_INSET, 0,
             NV_ERAIL_RING / h, 0, r, 1, 1, 1, 0.98f * a);
  }

  cx = x + 12.0f; cy = y + h - 12.0f;
  { TxtLine c = txt_line(TXT_ERAIL_CODE, num, 255, 255, 255, 255);
    txt_draw_alpha(c, cx, cy - (float)c.h, a); }

  current = ep->season == currentT && ep->episode == currentE;
  if (current) {
    TxtLine p = txt_line(TXT_ERAIL_PILL, "PLAYING", 11, 13, 16, 255);
    float pw = (float)p.w + 24.0f, ph = (float)p.h + 12.0f;
    GfxRect pr = { x + w - 12.0f - pw, y + 12.0f, pw, ph };
    gfx_color(pr, 8.0f / ph, 0.961f, 0.961f, 0.961f, a);
    txt_draw_alpha(p, pr.x + 12.0f, pr.y + 6.0f, a);
  }

  { int c = sel ? 255 : 153;   /* rgba(255,255,255,0.6) resting */
    txt_draw_alpha(txt_line_trim(TXT_ERAIL_CTITLE, ep->name[0] ? ep->name : num,
                                 c, c, c, 255, w),
                   x, y + h + 12.0f, a); }
}

void episodes_draw(void) {
  if (anim < .005f) return;
  int n = nLines(), i;
  // The sheet slides UP and fades, the counterpart of the right-hand menus'
  // sideways travel. 14% of its own height, as the web app has it.
  float top = NV_SCREEN_H - EP_H + (1 - anim) * EP_H * 0.14f;
  float y = top + NV_ERAIL_PAD_TOP;
  float railY, trackX;

  // The panel supplies its OWN gradient, so the full-screen scrim behind it only
  // has to take the edge off the exposed frame rather than dim the whole picture.
  gfx_color((GfxRect){0,0,NV_SCREEN_W,NV_SCREEN_H},0,.02f,.02f,.025f,.18f*anim);
  gfx_rect((GfxRect){ 0, top, NV_SCREEN_W, EP_H }, 0, GFX_ERAIL_SCRIM,
           0, 0, 0, 0, 0.0314f, 0.0392f, 0.0510f, anim);

  // --- the season tabs ------------------------------------------------------
  { int first = season > 1 ? season - 1 : 0;
    for (i = first; i < nSeasons() && i < first + 4; i++) {
      float tx = NV_ERAIL_GUTTER + (i - first) * 212.0f;
      int sel = i == season, b = sel ? 24 : 210;
      char s[48];
      TxtLine l;
      gfx_color((GfxRect){ tx, y, 196, 52 }, .5f,
                sel ? .94f : .14f, sel ? .94f : .14f, sel ? .95f : .15f, anim);
      snprintf(s, sizeof s, "Season %d", numSeason(i));
      l = txt_line(TXT_PG_LABEL, s, b, b, b, 255);
      txt_draw_alpha(l, tx + (196 - l.w) * .5f, y + 12, anim);
      if (sel && group == 0)
        gfx_color((GfxRect){ tx + 30, y + 58, 136, 2 }, 0, .94f, .94f, .95f, anim);
    } }
  y += EP_SEASONS_H;

  // --- the selected episode's detail, ABOVE the rail ------------------------
  // This is the whole reason the picker stopped being a list of rows: the title
  // and synopsis get room to be read once, here, instead of being clamped into
  // every row of a column 400px wide.
  { const CatEp *ep = n ? epLine(focus) : NULL;
    char meta[96];
    const CatItem *cim = cat_item(title);
    int watched = 0, mapped;
    if (ep) {
      mapped = cim ? watchedep_state(cim->imdb, ep->season, ep->episode) : -1;
      watched = mapped >= 0 ? mapped : extras_ep_watched(ep->season, ep->episode);
      snprintf(meta, sizeof meta, "S%dE%d%s%s%s%s", ep->season, ep->episode,
               ep->duration[0] ? " · " : "", ep->duration,
               watched ? " · " : "", watched ? "WATCHED" : "");
      txt_tracking(TXT_ERAIL_META, meta, 245, 245, 245,
                   NV_ERAIL_GUTTER, y, anim, NV_ERAIL_META_TRACK);
      txt_draw_alpha(txt_line_trim(TXT_ERAIL_TITLE,
                                   ep->name[0] ? ep->name : meta,
                                   255, 255, 255, 255, NV_ERAIL_DETAIL_W),
                     NV_ERAIL_GUTTER, y + 35.0f, anim);
      txt_block(TXT_ERAIL_OVER, ep->synopsis, 158, 159, 162,
                NV_ERAIL_GUTTER, y + 95.0f, NV_ERAIL_DETAIL_W,
                NV_LD_ERAIL_OVER, anim, 2);
    } else {
      txt_block(TXT_ERAIL_OVER, disc_episodes_loading(title)
                  ? "Loading episodes…"
                  : "Episodes unavailable. Select the season and press OK to try again.",
                196, 198, 204, NV_ERAIL_GUTTER, y + 35.0f,
                NV_ERAIL_DETAIL_W, NV_LD_ERAIL_OVER, anim, 2);
    } }
  y += EP_DETAIL_H + NV_ERAIL_DETAIL_MB;

  // --- the rail -------------------------------------------------------------
  railY = y + NV_ERAIL_VIEW_PAD;
  trackX = NV_ERAIL_GUTTER - scroll;
  // Cropped to the sheet, then FEATHERED at both ends: cards should run past the
  // edge rather than stop dead, and the part-visible card on the left should
  // dissolve instead of being sliced down its middle.
  gfx_crop(0, y, NV_SCREEN_W, EP_VIEW_H);
  for (i = 0; i < n; i++) {
    float cx = trackX + i * EP_PITCH;
    if (cx + NV_ERAIL_CARD_W < -40 || cx > NV_SCREEN_W + 40) continue;
    cardDraw(i, cx, railY, group == 1 && i == focus, anim);
  }
  gfx_no_crop();
  { float ink = 0.0314f, g = 0.0392f, b = 0.0510f;
    // The rail's ground at this height is the panel gradient's own value there —
    // 0.96 of the ink — so the feather dissolves into the sheet, not onto video.
    gfx_rect((GfxRect){ 0, y, NV_ERAIL_GUTTER, EP_VIEW_H }, 0, GFX_MENU_FEATHER,
             0, 1.0f, 1.0f, 0, ink, g, b, 0.96f * anim);
    gfx_rect((GfxRect){ NV_SCREEN_W - 120.0f, y, 120.0f, EP_VIEW_H }, 0,
             GFX_MENU_FEATHER, 0, 1.0f, 0.0f, 0, ink, g, b, 0.96f * anim); }

  if (n) {
    char counter[48];
    snprintf(counter, sizeof counter, "%d of %d episodes", focus + 1, n);
    txt_draw_alpha(txt_line(TXT_MINI, counter, 166, 168, 174, 255),
                   NV_ERAIL_GUTTER, NV_SCREEN_H - 30, anim);
    // THE HINT, because a long press is invisible otherwise. The gesture is the
    // only way to reach the marking menu, and an unhinted gesture is a feature
    // nobody finds.
    if (!wmOpen) {
      TxtLine h = txt_line(TXT_MINI, "Hold OK to mark as watched", 150, 152, 158, 255);
      txt_draw_alpha(h, NV_SCREEN_W - NV_ERAIL_GUTTER - h.w, NV_SCREEN_H - 30, anim);
    }
  }

  // THE MARKING MENU, drawn LAST so it sits above the list it belongs to.
  if(wmOpen) {
    int opts=wmOptions();
    float mw=560, mh=112+opts*74, mx=(NV_SCREEN_W-mw)*.5f, my=(NV_SCREEN_H-mh)*.5f;
    char head[200];
    gfx_color((GfxRect){0,0,NV_SCREEN_W,NV_SCREEN_H},0,.02f,.02f,.025f,.55f*anim);
    gfx_color((GfxRect){mx,my,mw,mh},.03f,.115f,.115f,.125f,anim);
    snprintf(head,sizeof head,"S%dE%d%s%s",wmT,wmE,wmName[0]?" · ":"",wmName);
    txt_draw_alpha(txt_line_trim(TXT_PANEL_ITEM,head,242,243,245,255,mw-72),
                   mx+36,my+30,anim);
    txt_draw_alpha(txt_line(TXT_MINI,wmWatched?"Mark as watched":"Remove the watched mark",
                            168,170,176,255),mx+36,my+66,anim);
    for(int i=0;i<opts;i++) {
      int mode=wmModeAt(i), sel=i==wmFocus;
      float ry=my+104+i*74;
      char label[160];
      int c=sel?24:222;
      gfx_color((GfxRect){mx+28,ry,mw-56,60},.10f,sel?.94f:.155f,sel?.94f:.155f,sel?.95f:.165f,anim);
      wmLabel(mode,label,sizeof label);
      txt_draw_alpha(txt_line_trim(TXT_PG_LABEL,label,c,c,c,255,mw-104),mx+52,ry+18,anim);
    }
  }
}
