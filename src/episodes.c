#include "episodes.h"
#include "catalog.h"
#include "discover.h"
#include "extras.h"
#include "gfx.h"
#include "tex_cache.h"
#include "text.h"
#include "layout.h"
#include "anim.h"
#include <stdio.h>

#define EP_W 720.0f
#define EP_ROW 172.0f
#define EP_TOP 216.0f
static int is_open, title, currentT, currentE, season, focus, group;
static int requestT, requestE;
static float anim, scroll;
static int locateCurrent;

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
  season = focus = 0; group = 1; requestE = 0; scroll = 0;
  locateCurrent = 1;
  for (int i = 0; i < nSeasons(); i++) if (numSeason(i) == t) season = i;
  for (int i = 0; i < nLines(); i++) if (epLine(i)->episode == e) focus = i;
  disc_episodes(title, t);
}
int episodes_is_open(void) { return is_open; }
void episodes_close(void) { is_open = 0; }
int episodes_chose(int *t, int *e) {
  if (!requestE) return 0;
  *t = requestT; *e = requestE; requestE = 0; return 1;
}
void episodes_event(const SDL_Event *ev) {
  if (!is_open || ev->type != SDL_KEYDOWN) return;
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
    else {
      const CatEp *ep = epLine(focus);
      if (ep) {
        if (ep->season != currentT || ep->episode != currentE) {
          requestT = ep->season; requestE = ep->episode;
        }
        is_open = 0;
      }
    }
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
    char num[40];snprintf(num,sizeof num,"T%dE%d",ep->season,ep->episode);
    gfx_color((GfxRect){tr.x+8,tr.y+92,72,30},.15f,.025f,.025f,.03f,.9f*anim);
    txt_draw_alpha(txt_line(TXT_MINI,num,240,240,242,255),tr.x+15,tr.y+97,anim);
    float tx=x+260, w=EP_W-310;
    txt_draw_alpha(txt_line_trim(TXT_PANEL_ITEM,ep->name[0]?ep->name:num,242,243,245,255,w),tx,y+16,anim);
    int current=ep->season==currentT && ep->episode==currentE;
    int watched=extras_ep_watched(ep->season,ep->episode);
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
  }
}
