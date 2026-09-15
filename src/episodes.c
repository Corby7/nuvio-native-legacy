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

#define EP_W 720.0f
#define EP_ROW 172.0f
#define EP_TOP 216.0f
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
  if (k == SDLK_UP) { if (group == 1 && focus > 0) focus--; else group--; }
  if (k == SDLK_DOWN) { if (group < 1) group++; else if (focus < n - 1) focus++; }
  if (group < -1) group = -1;
  if (group == 0 && (k == SDLK_LEFT || k == SDLK_RIGHT)) {
    int new = season + (k == SDLK_RIGHT ? 1 : -1);
    if (new >= 0 && new < nt) {
      season = new; focus = 0; scroll = 0;
      locateCurrent = 0;
      disc_episodes(title, numSeason(season));
    }
  }
  if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
    if (group == -1) is_open = 0;
    else if (group == 0) {
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
  float area = NV_SCREEN_H - EP_TOP - 36;
  float max = n * EP_ROW - area;
  float target = scroll;
  if(focus*EP_ROW<scroll) target=focus*EP_ROW;
  if((focus+1)*EP_ROW>scroll+area) target=(focus+1)*EP_ROW-area;
  if (target > max) target = max;
  if (target < 0) target = 0;
  scroll = anim_spring(scroll, target, dt, NV_SPRING_SCROLL);
}
void episodes_draw(void) {
  if (anim < .005f) return;
  float x = NV_SCREEN_W - EP_W + (1 - anim) * EP_W;
  gfx_color((GfxRect){0,0,NV_SCREEN_W,NV_SCREEN_H},0,.02f,.02f,.025f,.35f*anim);
  gfx_color((GfxRect){x,0,EP_W,NV_SCREEN_H},.025f,.095f,.095f,.10f,anim);
  txt_draw_alpha(txt_line(TXT_PANEL_TITLE,"Episodes",240,241,243,255),x+40,44,anim);
  gfx_color((GfxRect){x+EP_W-146,44,110,50},.3f,group==-1?.94f:.14f,group==-1?.94f:.14f,group==-1?.95f:.15f,anim);
  int color = group == -1 ? 25 : 230;
  txt_draw_alpha(txt_line(TXT_PG_LABEL,"Close",color,color,color,255),x+EP_W-130,55,anim);
  gfx_crop(x+36,120,EP_W-72,64);
  int first = season > 1 ? season - 1 : 0;
  for (int i = first; i < nSeasons() && i < first+3; i++) {
    float tx = x+40+(i-first)*212;
    int sel = i == season;
    gfx_color((GfxRect){tx,126,196,52},.5f,sel?.94f:.14f,sel?.94f:.14f,sel?.95f:.15f,anim);
    char s[48]; snprintf(s,sizeof s,"Season %d",numSeason(i));
    int b=sel?24:210;
    TxtLine l=txt_line(TXT_PG_LABEL,s,b,b,b,255);
    txt_draw_alpha(l,tx+(196-l.w)*.5f,138,anim);
    if (sel && group==0) gfx_color((GfxRect){tx+30,184,136,2},0,.94f,.94f,.95f,anim);
  }
  gfx_no_crop();
  gfx_crop(x+36,EP_TOP,EP_W-72,NV_SCREEN_H-EP_TOP-32);
  int n=nLines();
  for (int i=0;i<n;i++) {
    float y=EP_TOP+i*EP_ROW-scroll;
    if (y+EP_ROW<EP_TOP || y>NV_SCREEN_H-32) continue;
    const CatEp *ep=epLine(i);
    int sel=group==1 && i==focus;
    GfxRect r={x+40,y,EP_W-80,EP_ROW-14};
    if(sel) gfx_color(r,.13f,.94f,.94f,.95f,anim);
    r.x+=2; r.y+=2; r.w-=4; r.h-=4;
    gfx_color(r,.12f,.135f,.135f,.14f,anim);
    const CatItem *ci=cat_item(title);
    const char *art=ep->thumb[0]?ep->thumb:(ci?ci->backdrop:"");
    GLuint tex=tex_get_width(art,184);
    GfxRect tr={x+54,y+14,184,130};
    gfx_color(tr,.10f,.19f,.19f,.20f,anim);
    if(tex){gfx_tex_aspect_current=tex_aspect(art);gfx_rect(tr,tex,GFX_CARD,0,0,0,.10f,0,0,0,anim);gfx_tex_aspect_current=0;}
    char num[40];snprintf(num,sizeof num,"S%dE%d",ep->season,ep->episode);
    gfx_color((GfxRect){tr.x+8,tr.y+92,72,30},.15f,.025f,.025f,.03f,.9f*anim);
    txt_draw_alpha(txt_line(TXT_MINI,num,240,240,242,255),tr.x+15,tr.y+97,anim);
    float tx=x+260, w=EP_W-310;
    txt_draw_alpha(txt_line_trim(TXT_PANEL_ITEM,ep->name[0]?ep->name:num,242,243,245,255,w),tx,y+16,anim);
    int current=ep->season==currentT && ep->episode==currentE;
    // THE SESSION MAP FIRST, the per-title grid second. extras.c's grid is
    // bounded at 20 seasons by 40 episodes and silently loses a mark past
    // either edge — a long-running series showed unwatched episodes it had
    // every right to tick. watchedep has no ceiling and answers -1 only when
    // nothing is known about the series, which is exactly when the old grid's
    // zero was a guess anyway.
    const CatItem *cim=cat_item(title);
    int mapped=cim?watchedep_state(cim->imdb,ep->season,ep->episode):-1;
    int watched=mapped>=0?mapped:extras_ep_watched(ep->season,ep->episode);
    char state[96];
    if(current) snprintf(state,sizeof state,"Now playing");
    else if(watched) snprintf(state,sizeof state,"✓ Watched%s%s",ep->duration[0]?" · ":"",ep->duration);
    else snprintf(state,sizeof state,"%s%s%s",ep->date,ep->date[0]&&ep->duration[0]?" · ":"",ep->duration);
    txt_draw_alpha(txt_line_trim(TXT_PG_END,state,current?236:180,current?237:182,current?240:188,255,w),tx,y+48,anim);
    txt_block(TXT_PG_END,ep->synopsis,186,188,194,tx,y+78,w,25,anim,3);
  }
  if(!n) txt_block(TXT_PG_END,disc_episodes_loading(title)?
    "Loading episodes…":"Episodes unavailable. Select the season and press OK to try again.",
    196,198,204,x+56,EP_TOP+40,EP_W-112,28,anim,4);
  gfx_no_crop();
  if(n) {
    char counter[48];snprintf(counter,sizeof counter,"%d of %d episodes",focus+1,n);
    txt_draw_alpha(txt_line(TXT_MINI,counter,166,168,174,255),x+40,NV_SCREEN_H-26,anim);
    // THE HINT, because a long press is invisible otherwise. The gesture is the
    // only way to reach the marking menu, and an unhinted gesture is a feature
    // nobody finds.
    if(!wmOpen) {
      TxtLine h=txt_line(TXT_MINI,"Hold OK to mark as watched",150,152,158,255);
      txt_draw_alpha(h,x+EP_W-40-h.w,NV_SCREEN_H-26,anim);
    }
  }

  // THE MARKING MENU, drawn LAST so it sits above the list it belongs to.
  if(wmOpen) {
    int opts=wmOptions();
    float mw=560, mh=112+opts*74, mx=x+(EP_W-mw)*.5f, my=(NV_SCREEN_H-mh)*.5f;
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
