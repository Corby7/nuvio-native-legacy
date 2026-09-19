#include "seeall.h"
#include "home.h"   // the rects the opening grows and flies from
#include "badges.h"
#include "discover.h"
#include "catalog.h"
#include "gfx.h"
#include "text.h"
#include "tex_cache.h"
#include "layout.h"
#include "anim.h"
#include "settings.h"
#include "director.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

// MEASURED from the web app (catalogSeeAllScreen, .seeall-card): a 248-wide
// poster with radius 12. At 1920, 5 columns fit with the screen gutter on both sides.
#define SEEALL_COLS      (timeline ? 1 : 5)
#define SEEALL_CARD_W  248.0f
#define SEEALL_CARD_H  (timeline ? 236.0f : SEEALL_CARD_W * 1.5f)
#define SEEALL_GAP_X    16.0f                 // .seeall-grid: gap 20px 16px
#define SEEALL_GAP_Y    (20.0f + 40.0f)       // gap + the title line under the poster
#define SEEALL_TOP    (collection ? 332.0f : 244.0f)

// DETAIL PANEL, on the right. Measurements from the web app (.seeall-detail),
// anchored explicitly to the real 1920x1080 screen: top 170, right 104, w 336.
//
// FIXED and not scrolling along: the CSS itself records that `position: sticky`
// does not exist in the TV's Chromium 53 and that without `fixed` the panel
// vanished as soon as the owner scrolled. There is no flow here at all.
#define SEEALL_PAN_W   336.0f
#define SEEALL_PAN_X   (NV_SCREEN_W - 104.0f - SEEALL_PAN_W)
#define SEEALL_PAN_Y   SEEALL_TOP
#define SEEALL_PAN_ART_H 330.0f
#define SEEALL_PAN_ART_W 220.0f

static int   is_open, focus, reqOpen = -1;
static float anim, animV, scrollY, velY;
// WHERE THE VIEW GREW FROM: the collection card that was focused when OK was
// pressed. The grid does not simply appear — it opens out of that card, so the
// screen the viewer gets is visibly the thing they chose. 0 when the grid was
// reached some other way (a home row's "See all"), and then it fades in place.
static GfxRect fromCard;
static int     fromValid = 0;
// The window's rectangle for THIS frame, computed once at the top of the draw.
// A file static because the header clips inside itself (the source tabs), and a
// clip taken there has to stay inside the window as well.
static GfxRect gView = { 0, 0, NV_SCREEN_W, NV_SCREEN_H };
static char  title[96];
static const ColFolder *collection;
static int source, tabFocus, tabCursor, timeline, ranked;
static float tabAnim[COL_SOURCE_MAX];
static int order[SEEALL_MAX], orderN=-1;
static char catalogId[96];
static int group(const char *name) {
  return collection && !strcmp(collection->group, name);
}
static void colorCollection(float *r,float *g,float *b) {
  col_color(collection,r,g,b);
  // Services keep their own brand. Editorial families without a brand of their
  // own use colour only for selection/state, keeping the surfaces neutral.
  if (group("Genres")) {*r=.075f;*g=.34f;*b=.285f;}
  else if (group("Themes")) {*r=.34f;*g=.13f;*b=.38f;}
  else if (group("Film Collections") || group("TV Collections"))
    {*r=.12f;*g=.27f;*b=.40f;}
}
// THE FOLDER'S OWN GROUP, WORD FOR WORD — "GENRES", "STREAMING", "DIRECTORS".
//
// This used to translate the group into a wording of its own: "Genres" became
// "GENRE", "Streaming" stayed but everything unrecognised became "COLLECTION". The
// home's collection hero shows the group itself, so opening a folder swapped the
// word out from under the viewer at the same moment the title beneath it was
// travelling into place — the one line that should have proved the two screens are
// the same screen was the one line that changed.
//
// col_group_label (collections.c) is the shared wording, so neither side can drift.
// The fallbacks below are only for a grid with no collection behind it at all: a
// home row's "See all", which has a catalogue and no folder.
static const char *labelGroup(void) {
  static char lbl[80];
  if (collection) { col_group_label(collection, lbl, sizeof lbl); return lbl; }
  if (ranked) return "RANKING";
  return "CATALOGUE";
}
static const char *subtitleGroup(void) {
  if (timeline) return "Filmography in chronological order";
  if (group("Awards")) return ranked ? "Ranking in its original order" : "Awards list configured by the user";
  if (group("Genres")) return "Titles of this genre in your catalogue";
  if (group("Themes")) return "Curated by theme and mood";
  if (group("Streaming")) return "Catalogue organised by service";
  return ranked ? "Original ranking order" : "A selection from your catalogue";
}

static int yearOf(const CatItem *it) {
  for(const char *s=it->meta;*s;s++)if((*s=='1'||*s=='2')&&strlen(s)>=4&&s[1]>='0'&&s[1]<='9'&&s[2]>='0'&&s[2]<='9'&&s[3]>='0'&&s[3]<='9')return atoi(s);
  return 9999;
}
static int viewItem(int i,CatItem *out) {return disc_seeall_item(timeline&&i<orderN?order[i]:i,out);}
static void openSource(void) {
  const ColSource *s=&collection->sources[source];
  snprintf(catalogId,sizeof catalogId,"%s",s->catId);
  ranked=strstr(s->catId,"top100")||strstr(s->catId,"top250")||strstr(s->catId,"top10");
  focus=0;scrollY=velY=0;orderN=-1;
  // col_source_base and not s->base: a collection from the account stores the
  // addon's ID, and the address comes from the INSTALLED addon with that id.
  { const char *base=col_source_base(s);
    if(!base||!*base) {
      // This source's addon is not installed (or its manifest was never read).
      // With no address there is nothing to fetch, and an empty grid with no
      // explanation sends you looking for a fault where there is none.
      printf("[seeall] '%s': addon '%s' has no address; not fetching\n",
             s->title[0]?s->title:s->catId, s->addonId);
      fflush(stdout);
      return;
    }
    disc_seeall_filter(base,s->type,s->catId,s->genre); }
}
void seeall_collection(const ColFolder *folder) {
  if(!folder||!folder->nSources)return;
  // The card this is opening out of, and the wordmark that will travel with it.
  // Asked for HERE and not while drawing: home_draw stops running the moment the
  // grid covers the screen, and by then the rect would be a frame out of date.
  fromValid = home_collection_card_rect(&fromCard.x, &fromCard.y,
                                        &fromCard.w, &fromCard.h);
  anim = 0.0f; animV = 0.0f; scrollY = 0.0f; velY = 0.0f; focus = 0;
  memset(tabAnim, 0, sizeof tabAnim);
  collection=folder;source=tabCursor=0;tabFocus=folder->nSources>1;is_open=1;reqOpen=-1;
  timeline=!strcmp(folder->group,"Directors");
  snprintf(title,sizeof title,"%s",folder->title);openSource();
}

// The parameter MUST NOT be called `title`: there is a `static char title[96]`
// at the top of this file, and the parameter shadowed it. The effect was double
// and silent —
//
//   snprintf(title, sizeof title, "%s", title ? title : "");
//
// wrote INTO the parameter, a `const char *` pointing at the caller's memory,
// with `sizeof title` being 8 (the size of a pointer) instead of 96; and the
// real buffer was never filled, so the "see all" screen showed the previous
// title.
void seeall_open(const char *base, const char *kind, const char *catId,
                   const char *heading) {
  // A home row's "See all" card is not a collection card and has no wordmark to
  // carry: that path keeps the plain fade it has always had.
  fromValid = 0;
  is_open = 1; focus = 0; scrollY = 0.0f; velY = 0.0f; reqOpen = -1;
  memset(tabAnim, 0, sizeof tabAnim);
  snprintf(title, sizeof title, "%s", heading ? heading : "");
  collection=col_by_catalog(base,kind,catId);timeline=collection&&!strcmp(collection->group,"Directors");
  source=tabCursor=tabFocus=0;orderN=-1;
  if(collection) {
    for(int i=0;i<collection->nSources;i++)if(!strcmp(collection->sources[i].catId,catId)&&!strcmp(collection->sources[i].type,kind)){source=tabCursor=i;break;}
    snprintf(title,sizeof title,"%s",collection->title);
  }
  snprintf(catalogId,sizeof catalogId,"%s",catId);
  ranked=strstr(catId,"top100")||strstr(catId,"top250")||strstr(catId,"top10");
  disc_seeall_open(base, kind, catId);
}

int seeall_is_open(void) { return is_open; }

// THE VIEW'S RECTANGLE THIS FRAME: the card at 0, the whole screen at 1.
//
// Everything the grid draws lives inside it, so the screen is revealed THROUGH a
// window that opens out of the card rather than fading up over the home. The
// background does not take part — see the note in seeall_draw.
// THE EDGES GO FIRST. The window is an app opening out of an icon: the four sides
// reach the screen well before the list inside has finished arriving, and the top
// edge is what carries the mark and its label up with it.
//
// Front-loading it is deliberate here, and it is the one place in this app where
// easing a spring a second time is the right answer rather than the defect the
// detail screen's notes describe: there the composition tripled an opening that
// was already a cut, here the spring is the quick one (NV_SPRING2_GRID) and the
// curve is quadratic, not cubic. The edges are ~75% of the way at the spring's
// halfway point and settled around 210ms.
static float edgeProgress(void) {
  float x = anim_clamp(anim, 0.0f, 1.0f);
  if (!fromValid || !is_open) return x;   /* nothing to open out of, or closing */
  return 1.0f - (1.0f - x) * (1.0f - x);
}

// WHERE THE MARK IS IN ITS JOURNEY. 1 = this screen's header, 0 = the home's hero.
//
// Opening it rides the window's edge, so the mark is pushed up by the rectangle
// that is carrying it. Closing it WAITS: it holds at the header until the list has
// finished fading (contentAlpha reaches 0 at 0.45) and only then comes back down,
// over a background with nothing else happening on it.
//
// That ordering is the whole point of the pair — mark first then content on the
// way in, content first then mark on the way out. Both at once is what put the
// wordmark on top of the fade.
static float markProgress(void) {
  if (!is_open) return anim_clamp(anim / 0.45f, 0.0f, 1.0f);
  return edgeProgress();
}

static GfxRect viewRect(void) {
  GfxRect all = { 0, 0, NV_SCREEN_W, NV_SCREEN_H };
  // THE WINDOW ONLY OPENS. It does not close again, and that asymmetry is the
  // point rather than an omission.
  //
  // Opening, the window IS the gesture: one rectangle growing out of the card the
  // viewer pressed, with the list still faint inside it. Closing, the list is
  // already there in full — a hundred posters, a header, the source tabs — and
  // dragging a shrinking clip across all of it sets every one of those edges
  // moving at once. Too busy to read, and it buries the one thing that should be
  // followed: the wordmark travelling back down to the hero.
  //
  // So on the way out the window stays where it is and the content simply fades,
  // leaving the marks as the only thing in motion. They still fly, on the raw
  // curve — see seeall_owns_mark, which is true in both directions.
  if (!is_open) return all;
  if (!fromValid || anim >= 0.999f) return all;
  { float g = edgeProgress(); GfxRect r;
    r.x = fromCard.x + (all.x - fromCard.x) * g;
    r.y = fromCard.y + (all.y - fromCard.y) * g;
    r.w = fromCard.w + (all.w - fromCard.w) * g;
    r.h = fromCard.h + (all.h - fromCard.h) * g;
    return r; }
}

// The copy and the cards come in AFTER the window has opened far enough to hold
// them: laid into a card-sized rectangle they would be a page of text seen through
// a letterbox. Nothing is lost by waiting — the window is what carries the first
// half of the movement.
// THE SAME DELAY AT BOTH ENDS, so the close is the open's SEQUENCE reversed and
// not merely its direction. Coming in, the list waits until the window has opened
// far enough to hold it; going out, it is gone again by the time the curve has
// fallen back to that same point — which leaves the second half of the close clear
// for the mark to travel through.
//
// It used to fall back to the raw curve on the close, so the list was still fading
// while the wordmark crossed over it. A solid mark moving across a half-dissolved
// screen is two things competing for the same pixels, and it read exactly as badly
// as that sounds.
static float contentAlpha(void) {
  if (!fromValid) return anim;
  return anim_smooth((anim - 0.45f) / 0.55f);
}

// AND IT RISES as it arrives, by NV_SEEALL_RISE, because the wordmark above it is
// already travelling up. Everything moving one way reads as a single screen
// assembling; the copy fading in place under a mark that is climbing reads as two
// unrelated things happening at once.
// ONE FORMULA, BOTH DIRECTIONS, which is what makes the close the entrance run
// backwards without any code that knows it is closing.
//
// Coming in, contentAlpha climbs and the offset falls to nothing: the list rises
// the last NV_SEEALL_RISE into place. Going out it climbs again, so the list sinks
// back down the way it came while it fades.
//
// This used to return 0 on the close and the list simply dissolved where it stood.
// That is the part that read as odd: it had ARRIVED with a movement, and leaving
// without one makes the screen look like it was switched off rather than left.
// Fade paired with a short translate is the ordinary way to dismiss a full view —
// the same pairing the entrance uses, pointed the other way.
static float contentRise(void) {
  return (1.0f - contentAlpha()) * NV_SEEALL_RISE;
}

// 1 once the window has reached all four edges AND the ground behind it is opaque:
// the home underneath is invisible and app.c can stop drawing it. While the window
// is still opening the home IS the background and has to stay.
int seeall_covers_screen(void) {
  if (!is_open) return 0;
  { GfxRect v = viewRect();
    return anim >= 0.995f && v.x <= 0.5f && v.y <= 0.5f &&
           v.x + v.w >= NV_SCREEN_W - 0.5f && v.y + v.h >= NV_SCREEN_H - 0.5f; }
}

// The folder's wordmark is the grid's while it travels, so the home stops drawing
// its copy. Only true when there is actually a flight to carry it.
// THE MARK IS THE GRID'S while it travels, whichever of the two it is, so the home
// stands its own copy down. TRUE IN BOTH DIRECTIONS: the mark that rose into this
// header on the way in comes back down to the hero on the way out, so the journey
// is reversible rather than a one-way trip that ends in a fade.
//
// The lower bound matches seeall_draw's own early-out (a < 0.01). Below it this
// screen draws nothing, so the home has to have its mark back by then or there
// would be a frame or two with no wordmark anywhere. At that point the flight has
// arrived within 1% of the hero's rect, so the handover is invisible.
int seeall_owns_mark(void) {
  return fromValid && anim >= 0.01f && anim < 0.999f;
}
int seeall_requested_open(void) { int v = reqOpen; reqOpen = -1; return v; }

static int nItems(void) { return disc_seeall_n(); }

void seeall_event(const SDL_Event *e) {
  int n = nItems(), k;
  if (!is_open || e->type != SDL_KEYDOWN) return;
  k = e->key.keysym.sym;
  if (k == SDLK_AC_BACK || k == SDLK_ESCAPE || k == SDLK_BACKSPACE ||
      e->key.keysym.scancode == NV_SCANCODE_BACK) { is_open = 0; return; }
  if(collection&&tabFocus) {
    if(k==SDLK_LEFT&&tabCursor>0)tabCursor--;
    if(k==SDLK_RIGHT&&tabCursor+1<collection->nSources)tabCursor++;
    if(k==SDLK_RETURN||k==SDLK_KP_ENTER){source=tabCursor;openSource();tabFocus=0;}
    if(k==SDLK_DOWN&&n>0)tabFocus=0;
    return;
  }
  if(k==SDLK_UP&&focus<SEEALL_COLS&&collection){tabFocus=1;tabCursor=source;return;}
  if((k==SDLK_RETURN||k==SDLK_KP_ENTER)&&disc_seeall_error()){disc_seeall_more();return;}
  if (n < 1) return;
  if (k == SDLK_RIGHT && focus + 1 < n) focus++;
  else if (k == SDLK_LEFT && focus > 0) focus--;
  else if (k == SDLK_DOWN) { if (focus + SEEALL_COLS < n) focus += SEEALL_COLS;
                             else focus = n - 1; }
  else if (k == SDLK_UP) { if (focus >= SEEALL_COLS) focus -= SEEALL_COLS; }
  else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
    // The grid item is NOT in the global catalogue — it came from a page only
    // this screen read. It goes in through cat_append so the title screen can
    // open it by index, which is how the whole app works.
    CatItem it;
    if (viewItem(focus, &it)) {
      int idx = it.imdb[0] ? cat_index_by_imdb(it.imdb) : -1;
      if (idx < 0) idx = cat_append(&it);
      if (idx >= 0) { reqOpen = idx; } // keeps the list and the position on the way back
    }
  }
  // Nearing the end, ask for the next page. Before the owner sees the empty
  // space, not once they are already staring at it.
  if (focus >= n - SEEALL_COLS * 2) disc_seeall_more();
}

void seeall_update(float dt, Uint32 now) {
  float target, maxY;
  int n = nItems(), lines;
  (void)now;
  // Critically damped, for the reason anim.h records at anim_spring2 and the
  // detail screen's flight documents at NV_SPRING2_SCREEN: a first-order spring
  // leaves at maximum speed, which on this size of movement is a cut.
  anim = anim_spring2(&animV, anim, is_open ? 1.0f : 0.0f, dt,
                      is_open ? NV_SPRING2_GRID : NV_SPRING2_GRID_OUT);
  if (!is_open) return;
  for (int i = 0; i < COL_SOURCE_MAX; i++) {
    float targetTab = collection && tabFocus && i == tabCursor ? 1.0f : 0.0f;
    tabAnim[i] = anim_spring(tabAnim[i], targetTab, dt,
                           targetTab > tabAnim[i] ? NV_SPRING_FOCUS : NV_SPRING_BLUR);
  }
  if(timeline&&n!=orderN) {
    int old=orderN>0&&focus<orderN?order[focus]:-1;int years[SEEALL_MAX];
    for(int i=0;i<n;i++){CatItem it;order[i]=i;years[i]=disc_seeall_item(i,&it)?yearOf(&it):9999;}
    for(int i=1;i<n;i++){int v=order[i],j=i;while(j>0&&years[order[j-1]]>years[v]){order[j]=order[j-1];j--;}order[j]=v;}
    orderN=n;if(old>=0)for(int i=0;i<n;i++)if(order[i]==old){focus=i;break;}
  }
  lines = (n + SEEALL_COLS - 1) / SEEALL_COLS;
  // Aims the focused row at 30% of the usable height, as the rest of the app does.
  target = SEEALL_TOP + (float)(focus / SEEALL_COLS) * (SEEALL_CARD_H + SEEALL_GAP_Y)
       - NV_SCREEN_H * 0.30f;
  if(tabFocus)target=0;
  maxY = SEEALL_TOP + (float)lines * (SEEALL_CARD_H + SEEALL_GAP_Y) - NV_SCREEN_H + 120.0f;
  if (maxY < 0.0f) maxY = 0.0f;
  if (target < 0.0f) target = 0.0f;
  if (target > maxY) target = maxY;
  scrollY = anim_spring2(&velY, scrollY, target, dt, NV_SPRING2_SCROLL);
}

// THE RIGHT-HAND PANEL: what the grid alone does not say — synopsis, genres, score.
//
// Without it the owner sees 50 posters and no information; it was what the
// screen needed to stop being just a wall of images.
static void panel(float a) {
  CatItem it;
  float y = SEEALL_PAN_Y;
  if (!viewItem(focus, &it)) return;

  // The side context uses the poster, never repeating the timeline's still.
  // It keeps the 2:3 geometry even without art so the metadata does not shift.
  { GfxRect r = { SEEALL_PAN_X, y, SEEALL_PAN_ART_W, SEEALL_PAN_ART_H };
    const char *art = it.poster[0] ? it.poster : it.backdrop;
    float radius = 12.0f / SEEALL_PAN_ART_H;   // fraction of the HEIGHT
    GLuint t = art[0] ? tex_get_width(art, SEEALL_PAN_ART_W) : 0;
    gfx_color(r, radius, 1, 1, 1, 0.05f * a);
    if (t) {
      gfx_tex_aspect_current = tex_aspect(art);
      gfx_rect(r, t, GFX_CARD, 0, 0, 0, radius, 0, 0, 0, a);
      gfx_tex_aspect_current = 0.0f;
    } else {
      TxtLine missing = txt_line_trim(TXT_HERO_META, "No poster", 185, 191, 204, 255, r.w-24);
      txt_draw_alpha(missing, r.x+(r.w-missing.w)*.5f, r.y+(r.h-missing.h)*.5f, a);
    } }
  y += SEEALL_PAN_ART_H + 18.0f;
  float badgeW=badges_draw(badges_provider(it.providerName),SEEALL_PAN_X,y,SEEALL_PAN_W,28,a);
  if(badgeW>0)y+=40;

  // THE LOGO in place of the title where there is one (max 264x82 in the web
  // app); the name set in the interface font only when there is no logo.
  { GLuint tl = it.logo[0] ? tex_get_width(it.logo, 264.0f) : 0;
    float ap = it.logo[0] ? tex_aspect(it.logo) : 0.0f;
    if (tl && ap > 0.0f) {
      float wL = 264.0f, hL = wL / ap;
      if (hL > 82.0f) { hL = 82.0f; wL = hL * ap; }
      { GfxRect rl = { SEEALL_PAN_X, y, wL, hL };
        GfxMode m = tex_brand_dark(it.logo) ? GFX_BRAND : GFX_TEXT;
        gfx_tex_aspect_current = 0.0f;
        gfx_rect(rl, tl, m, 0, 0, 0, 0.0f, 1, 1, 1, a); }
      y += hL + 4.0f;
    } else {
      TxtLine t = txt_line_trim(TXT_HEADLINE, it.title, 245, 245, 245, 255,
                                   SEEALL_PAN_W);
      txt_draw_alpha(t, SEEALL_PAN_X, y, a);
      y += t.h + 6.0f;
    } }

  if (it.genre[0]) {
    TxtLine t = txt_line_trim(TXT_CAPTION, it.genre, 220, 231, 244, 255,
                                 SEEALL_PAN_W);
    txt_draw_alpha(t, SEEALL_PAN_X, y, a * 0.72f);
    y += t.h + 6.0f;
  }
  // The score pill, in the IMDb yellow the web app uses (245,197,24).
  if (it.score > 0) {
    char n[16];
    snprintf(n, sizeof n, "%.1f", it.score / 10.0f);
    { TxtLine t = txt_line(TXT_CAPTION, n, 23, 19, 10, 255);
      GfxRect r = { SEEALL_PAN_X, y + 8.0f, t.w + 26.0f, t.h + 8.0f };
      gfx_color(r, 8.0f / (t.h + 8.0f), 0.961f, 0.773f, 0.094f, 0.92f * a);
      txt_draw_alpha(t, SEEALL_PAN_X + 13.0f, y + 12.0f, a);
      y += r.h + 14.0f; }
  }
  if (it.meta[0]) {
    TxtLine t = txt_line_trim(TXT_DET_META2, it.meta, 240, 240, 240, 255,
                                 SEEALL_PAN_W);
    txt_draw_alpha(t, SEEALL_PAN_X, y, a * 0.92f);
    y += t.h + 12.0f;
  }
  if (it.synopsis[0]) {
    // As far as it fits without passing the usable bottom (the web app cuts at
    // max-height: 100% - 210).
    int lines = (int)((NV_SCREEN_H - 48.0f - y) / 34.0f);
    if (lines > 8) lines = 8;
    if (lines > 0)
      txt_block(TXT_DET_META2, it.synopsis, 236, 236, 236,
                SEEALL_PAN_X, y, SEEALL_PAN_W, 34.0f, a * 0.86f, lines);
  }
}

static const char *portraitLocal(const ColFolder *folder) {
  static char path[700];
  if (!folder || !folder->frameDir[0]) return "";
  snprintf(path, sizeof path, "%s/portrait.png", folder->frameDir);
  if (access(path, R_OK) == 0) return path;
  snprintf(path, sizeof path, "%s/portrait.jpg", folder->frameDir);
  return access(path, R_OK) == 0 ? path : "";
}

// A single full-width piece of art, dissolving into the body colour. Directors
// uses the local vertical portrait when the package already has it; that
// collection's horizontal hero is a neutral placeholder and only adds a layer
// with no information. It uses the existing shaders and cache, with no blur.
static void themeBackground(float a) {
  if(collection) {
    if(collection->editorial) {
      GLuint art=tex_get_hero(collection->detailHero);
      /* The content starts at 332; the separate detail illustration ends at 320. */
      if(art)gfx_rect((GfxRect){0,0,1920,320},art,GFX_TEXT,0,0,0,0,1,1,1,a);
      return;
    }
    if(group("Directors")) {
      const char *photo=portraitLocal(collection);
      if(!photo[0]) {
        director_request(collection->title);
        photo=director_photo(collection->title);
      }
      GLuint tp=photo[0]?tex_get_width(photo,260.0f):0;
      if(tp) {
        // The selected item's panel starts at VT_PAN_Y. The portrait occupies
        // only the header and ends before it, without crossing the poster or
        // the synopsis as a second layer.
        GfxRect rp={1660,18,260,300};
        gfx_tex_aspect_current=tex_aspect(photo);
        gfx_rect(rp,tp,GFX_PORTRAIT,
                 0,0,0,0,0,0,0,a*.88f);
        gfx_tex_aspect_current=0;
      }
    } else {
      // THE ART KEEPS THE HOME HERO'S RECTANGLE, and that is the whole of "the
      // background stays in the same place".
      //
      // It used to be drawn in a box of its own, 1920x620 across the top. The file
      // is the same one the home's collection hero is showing — but cover-cropped
      // into a different shape, so on opening a collection the picture JUMPED to a
      // different framing at the same moment it dimmed. Standing still was not
      // enough; it has to stand still in the same rectangle.
      //
      // Asking the home for that rect rather than copying its arithmetic also
      // keeps the two honest across the hero's three layouts (full screen, band,
      // top band) — heroRectFor owns that decision and this follows it.
      //
      // What DOES change is the treatment, which is the point: 38% and this
      // screen's gradient instead of the hero's full-strength one.
      const char *art=collection->hero[0]?collection->hero:collection->cover;
      GLuint tex=art[0]?tex_get_hero(art):0;
      if(tex) {
        GfxRect hr;
        home_hero_rect(&hr.x,&hr.y,&hr.w,&hr.h);
        if(hr.w<1.0f||hr.h<1.0f)hr=(GfxRect){0,0,NV_SCREEN_W,620};
        gfx_tex_aspect_current=tex_aspect(art);
        gfx_rect(hr,tex,GFX_HERO_FULL,0,0,0,0,0,0,0,a*.38f);
        gfx_tex_aspect_current=0;
      }
    }
  }
}

// The intersection of two rectangles, as the clip. The grid already clips itself
// below the header; while the window is opening it must ALSO stay inside the
// window, and gfx_crop takes one rect, not two.
static void cropBoth(GfxRect a, GfxRect b) {
  float x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
  float x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w;
  float y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
  if (x1 < x0) x1 = x0;
  if (y1 < y0) y1 = y0;
  gfx_crop(x0, y0, x1 - x0, y1 - y0);
}

// The wordmark's box in THIS screen's header. One arithmetic, read twice: by the
// header that draws it at rest and by the flight that lands on it.
static int headerLogo(float x0, GfxRect *box, GLuint *tex) {
  int isDirector = collection && !strcasecmp(collection->group, "Directors");
  float aspect, w = 560.0f, h;
  if (!collection || isDirector || collection->editorial || !collection->logo[0])
    return 0;
  aspect = tex_aspect(collection->logo);
  if (aspect <= 0.0f) return 0;
  h = w / aspect;
  if (h > 108.0f) { h = 108.0f; w = h * aspect; }
  *tex = tex_get_exact(collection->logo, w);
  if (!*tex) return 0;
  *box = (GfxRect){ x0, 83.0f, w, h };
  return 1;
}

// THE WORDMARK TRAVELS, it does not crossfade. The home's collection hero and this
// header draw the SAME file, so the mark the viewer was reading on the home is the
// one that ends up in the header — it moves UP, from y=326 to y=83, while the view
// opens underneath it.
//
// Drawn OUTSIDE the window's clip and at full alpha, for the reason the detail's
// logo records: the fade is for copy that has nowhere to come from, and fading
// something that is moving only makes the movement harder to follow.
static void flyingLogo(float x0) {
  GfxRect dst, from, r; GLuint tex;
  if (!seeall_owns_mark()) return;
  if (!headerLogo(x0, &dst, &tex)) return;
  if (!home_collection_logo_rect(&from.x, &from.y, &from.w, &from.h)) return;
  { float g = markProgress();
    r.x = from.x + (dst.x - from.x) * g;
    r.y = from.y + (dst.y - from.y) * g;
    r.w = from.w + (dst.w - from.w) * g;
    r.h = from.h + (dst.h - from.h) * g; }
  gfx_rect(r, tex, tex_brand_dark(collection->logo) ? GFX_BRAND : GFX_TEXT,
           0, 0, 0, 0, .96f, .97f, .98f, 1.0f);
}

// THE SAME JOURNEY FOR A FOLDER THAT HAS NO WORDMARK. Only the streaming services
// carry one; Genres, Awards and the rest are type, and while just the marks flew,
// everything else appeared in the header having never crossed the screen.
//
// It is a MOVE and never a scale, which is the only reason rasterised text can do
// this at all: the home sets the name in TXT_TITLE1 and so does this header, so
// the two differ in POSITION and nothing else. Scaling it would mean re-rasterising
// every frame, which is the one cost text.c is built to avoid.
static int titleFlying(float x0, float *tx, float *ty) {
  GfxRect from; GfxRect dst;  GLuint tex;
  if (!seeall_owns_mark()) return 0;
  if (headerLogo(x0, &dst, &tex)) return 0;      /* the wordmark has it */
  if (!home_collection_title_rect(&from.x, &from.y, &from.w, &from.h)) return 0;
  { float g = markProgress();
    *tx = from.x + (x0 - from.x) * g;
    *ty = from.y + (80.0f - from.y) * g; }
  return 1;
}

// THE LABEL ABOVE THE MARK TRAVELS TOO. It is the same string on both screens now,
// set in the same face, so — like the title — this is a move and never a scale.
//
// Without it the line would fade in place while the title climbed past it, which is
// the "kinda teleports" the mark's own flight was added to stop.
static int groupFlying(float x0, float *tx, float *ty) {
  GfxRect from;
  if (!seeall_owns_mark()) return 0;
  if (!home_collection_group_rect(&from.x, &from.y, &from.w, &from.h)) return 0;
  { float g = markProgress();
    *tx = from.x + (x0 - from.x) * g;
    *ty = from.y + (40.0f - from.y) * g; }
  return 1;
}

static void flyingGroup(float x0) {
  float tx, ty;
  if (!groupFlying(x0, &tx, &ty)) return;
  { TxtLine l = txt_line(TXT_HERO_META, labelGroup(), 201, 206, 218, 255);
    txt_draw_alpha(l, tx, ty, 1.0f); }
}

static void flyingTitle(float x0) {
  float tx, ty;
  if (!titleFlying(x0, &tx, &ty)) return;
  { TxtLine line = txt_line_trim(TXT_TITLE1, title, 242, 243, 247, 255, 940);
    txt_draw_alpha(line, tx, ty, 1.0f); }
}

static void themeHeader(float a,float x0,float dy) {
  float r,g,b;colorCollection(&r,&g,&b);
  // THE HOME'S COLOUR, not one of its own: same words, same face, same ink, so
  // the line genuinely does not change when the folder opens. It is drawn here
  // only when it is NOT in the air — see flyingGroup.
  TxtLine eyebrow=txt_line(TXT_HERO_META,labelGroup(),201,206,218,255);
  { float gx, gy;
    if (!groupFlying(x0, &gx, &gy)) txt_draw_alpha(eyebrow,x0,40+dy,a); }
  int isDirector=collection&&!strcasecmp(collection->group,"Directors");
  // A director collection's wordmark may contain a head or composed lettering.
  // In the filmography header, the textual name and the clean portrait keep the
  // identity legible without duplicating the same visual information.
  // TEX_GET_EXACT: the header wordmark is furniture at a size this layout already
  // knows, and tex_get_width would leave the texture 1.25x-1.6x the drawn size, on
  // either side of GL_LINEAR_MIPMAP_NEAREST's 1.414 snap — undersampled below it,
  // halved and magnified above it. See the measured note in home.c's collection hero,
  // which is the same wordmark drawn from the same file.
  //
  // The two screens never overlap (app.c skips home_draw while seeall_is_open), so the
  // two exact widths cost one re-decode when the screen changes and nothing after.
  //
  // So the ASPECT first and the request second: before anything has decoded tex_aspect
  // answers 0 and the box is asked for at its full width, and the frame it lands the
  // width becomes the real one.
  int hasLogo=!isDirector&&collection&&!collection->editorial&&collection->logo[0];
  float aspect=hasLogo?tex_aspect(collection->logo):0;
  // The official wordmark, large enough to read at a distance. The
  // transparent PNG is imported at up to 800px, so 560px neither interpolates
  // upwards nor loses the brand's original silhouette.
  float w=560.0f,h=0.0f;
  if(aspect>0){h=w/aspect;if(h>108){h=108;w=h*aspect;}}
  GLuint logo=hasLogo?tex_get_exact(collection->logo,w):0;
  if(logo&&aspect>0&&seeall_owns_mark()) {
    /* in the air: flyingLogo has it, outside the clip and at full alpha */
  } else if(logo&&aspect>0) {
    gfx_rect((GfxRect){x0,83+dy,w,h},logo,tex_brand_dark(collection->logo)?GFX_BRAND:GFX_TEXT,0,0,0,0,.96f,.97f,.98f,a);
  } else {
    float tx, ty;
    if (!titleFlying(x0, &tx, &ty)) {
      TxtLine line=txt_line_trim(TXT_TITLE1,title,242,243,247,255,940);
      txt_draw_alpha(line,x0,80+dy,a);
    }
  }
  char caption[180];int n=nItems();
  if(disc_seeall_error())snprintf(caption,sizeof caption,"Could not load. OK to try again.");
  else if(!n)snprintf(caption,sizeof caption,"%s",disc_seeall_loading()?"Loading titles…":"No titles in this list.");
  else snprintf(caption,sizeof caption,"%d titles%s  ·  %s",n,disc_seeall_end()?"":" loaded",subtitleGroup());
  TxtLine sub=txt_line_trim(TXT_DET_META2,caption,196,202,213,255,960);txt_draw_alpha(sub,x0,192+dy,a);
  if(collection&&collection->nSources>1) {
    int first=tabCursor>3?tabCursor-3:0;
    cropBoth((GfxRect){x0-6,244+dy,NV_SCREEN_W-x0-90,72}, gView);
    for(int i=first;i<collection->nSources&&i<first+6;i++) {
      float x=x0+(i-first)*322.0f;const ColSource *s=&collection->sources[i];
      int f=tabFocus&&tabCursor==i, selected=source==i;
      float fa=tabAnim[i], scale=1.0f+.025f*fa;
      GfxRect pill={x-(304*scale-304)*.5f,250+dy-(58*scale-58)*.5f,
                    304*scale,58*scale};
      gfx_color(pill,.28f,f?.94f:selected?r*.82f:.09f,
              f?.95f:selected?g*.82f:.10f,
              f?.97f:selected?b*.82f:.12f,a);
      if(!f) gfx_rect(pill,0,GFX_RING,0,1.5f/pill.h,0,.28f,
                      selected?.86f:.36f,selected?.88f:.38f,
                      selected?.92f:.43f,a*(selected?.72f:.35f));
      if(selected&&!f)
        gfx_color((GfxRect){pill.x+18,pill.y+pill.h-4,pill.w-36,3},.5f,
                .84f+r*.16f,.84f+g*.16f,.84f+b*.16f,a);
      char label[180];snprintf(label,sizeof label,"%s · %s",s->title,!strcmp(s->type,"series")?"Series":"Movies");
      TxtLine t=txt_line_trim(TXT_HERO_META,label,f?22:238,f?24:240,f?28:245,255,276);
      txt_draw_alpha(t,pill.x+(pill.w-t.w)*.5f,pill.y+(pill.h-t.h)*.5f,a);
    }cropBoth((GfxRect){0,0,NV_SCREEN_W,NV_SCREEN_H}, gView);
  }
}

static void timelineCard(int i,float cy,float a,float x0) {
  CatItem it;if(!viewItem(i,&it))return;
  int sel=i==focus&&!tabFocus;float r,g,b;colorCollection(&r,&g,&b);
  // The line organises the chronology; it is not a decorative card border.
  gfx_color((GfxRect){x0+109,cy-30,2,SEEALL_CARD_H+SEEALL_GAP_Y},0,.48f,.47f,.46f,a*.6f);
  gfx_color((GfxRect){x0+102,cy+24,16,16},.5f,sel?.95f:r,sel?.95f:g,sel?.97f:b,a);
  char year[16];int y=yearOf(&it);if(y==9999)snprintf(year,sizeof year,"—");else snprintf(year,sizeof year,"%d",y);
  TxtLine yr=txt_line(TXT_CW_TITLE,year,219,210,195,255);txt_draw_alpha(yr,x0,cy+14,a);
  float x=x0+158;GfxRect card={x,cy,1000,SEEALL_CARD_H};
  if(sel)gfx_color((GfxRect){x-4,cy-4,1008,SEEALL_CARD_H+8},.045f,.94f,.95f,.97f,a);
  gfx_color(card,.04f,.09f,.095f,.105f,a);
  const char *art=it.backdrop[0]?it.backdrop:it.poster;GLuint tex=art[0]?tex_get_width(art,390):0;
  if(tex){gfx_tex_aspect_current=tex_aspect(art);gfx_rect((GfxRect){x+12,cy+12,376,212},tex,GFX_CARD,0,0,0,.04f,0,0,0,a);gfx_tex_aspect_current=0;}
  else { gfx_color((GfxRect){x+12,cy+12,376,212},.04f,.16f,.16f,.18f,a);
         txt_draw_alpha(txt_line(TXT_MINI,"No art",184,188,198,255),
                            x+158,y+104,a*.9f); }
  TxtLine name=txt_line_trim(TXT_CW_TITLE,it.title,242,243,247,255,550);txt_draw_alpha(name,x+418,cy+22,a);
  TxtLine genre=txt_line_trim(TXT_HERO_META,it.genre,187,194,207,255,550);txt_draw_alpha(genre,x+418,cy+64,a);
  txt_block(TXT_DET_META2,it.synopsis,209,214,225,x+418,cy+106,545,30,a,3);
}

void seeall_draw(Uint32 now) {
  float a = anim, x0 = settings_content_x();
  int n = nItems(), i;
  (void)now;
  if (a < 0.01f) return;

  // THE BACKGROUND DOES NOT TAKE PART IN THE OPENING, and that is deliberate.
  //
  // It stays exactly where it is and simply changes treatment: the ground comes up
  // and themeBackground lays this screen's own gradient over it — the collection's
  // art across the top at 38%, which is what the grid looks like at rest. Nothing
  // about it slides or grows. The home's hero is the same folder's art in the same
  // place, so what the eye sees is the picture dimming into the grid's palette
  // rather than one screen being replaced by another.
  //
  // What MOVES is the window below and the wordmark above it.
  { GfxRect screen = { 0, 0, NV_SCREEN_W, NV_SCREEN_H };
    gfx_color(screen, 0.0f, NV_COLOR_BACKGROUND_R, NV_COLOR_BACKGROUND_G, NV_COLOR_BACKGROUND_B, a); }
  themeBackground(a);

  // THE VIEW OPENS OUT OF THE CARD. Everything from here down is drawn inside a
  // rectangle that starts on the collection card the viewer pressed and grows to
  // the screen, so the grid is revealed THROUGH it — the card becomes the view
  // instead of being replaced by it. The copy and the cards wait for the window to
  // be big enough to hold them (contentAlpha).
  { GfxRect view = viewRect();
    float ac = contentAlpha(), rise = contentRise();
    gView = view;

  // THE GRID IS CLIPPED BELOW THE HEADER.
  //
  // The header was already drawn at a fixed position, but the posters passed
  // BEHIND it when scrolling — the title sat over moving imagery and turned
  // into "background". Clipping solves it without needing an opaque band: what
  // rises above the top simply is not drawn.
  cropBoth((GfxRect){ 0.0f, SEEALL_TOP - 12.0f, NV_SCREEN_W,
                      NV_SCREEN_H - SEEALL_TOP + 12.0f }, view);
  a = ac;   /* the grid and the copy below come in on the content's ramp */
  for (i = 0; i < n; i++) {
    float cx = x0 + (float)(i % SEEALL_COLS) * (SEEALL_CARD_W + SEEALL_GAP_X);
    float cy = SEEALL_TOP + (float)(i / SEEALL_COLS) * (SEEALL_CARD_H + SEEALL_GAP_Y)
             - scrollY + rise;
    CatItem it;
    GLuint t;
    // The SAME radius as the home's posters: `posterCardCornerRadiusDp` (12dp x
    // 2 = 24px), a fraction of the SMALLER side because the shader's SDF is
    // normalised. The fixed NV_RADIUS_CARD that used to be here gave a corner
    // different from the rest of the app, and the grid read as another screen.
    // The SDF's radius is a fraction of the HEIGHT, not of the smaller side:
    // `p = (uv-0.5)*vec2(asp,1.0)` makes one SDF unit h pixels on both axes.
    // Dividing by the width rounded this poster half again too much. See the
    // note on radiusInset in home.c.
    float radius = settings_radius_poster_px() / SEEALL_CARD_H;
    int sel = (i == focus);
    if (cy > NV_SCREEN_H || cy + SEEALL_CARD_H + 40.0f < SEEALL_TOP - 12.0f) continue;
    if(timeline){timelineCard(i,cy,a,x0);continue;}
    if (!viewItem(i, &it)) continue;
    { GfxRect r = { cx, cy, SEEALL_CARD_W, SEEALL_CARD_H };
      if (sel && !tabFocus) {
        GfxRect ring = { cx - 4, cy - 4, SEEALL_CARD_W + 8, SEEALL_CARD_H + 8 };
        gfx_color(ring, settings_radius_poster_px() / (SEEALL_CARD_W + 8.0f), 1, 1, 1, a);
      }
      t = it.poster[0] ? tex_get_width(it.poster, SEEALL_CARD_W)
        : (it.backdrop[0] ? tex_get_width(it.backdrop, SEEALL_CARD_W) : 0);
      if (t) {
        gfx_tex_aspect_current = tex_aspect(it.poster[0] ? it.poster : it.backdrop);
        gfx_rect(r, t, GFX_CARD, sel ? 1.0f : 0.0f, 0, 0, radius, 0, 0, 0, a);
        gfx_tex_aspect_current = 0.0f;
      } else {
        // A skeleton while the art has not arrived — the same colour as the rest of
        // the app, and on the same sweep of light (gfx_skeleton).
        gfx_skeleton(r, radius, NV_COLOR_SKELETON_R, NV_COLOR_SKELETON_G,
                NV_COLOR_SKELETON_B, a);
      } }
    { int c = sel ? 255 : 214;
      TxtLine l = txt_line_trim(TXT_DET_META2, it.title, c, c, c, 255,
                                   SEEALL_CARD_W);
      txt_draw_alpha(l, cx, cy + SEEALL_CARD_H + 10.0f, a * (sel ? 1.0f : 0.86f)); }
    if(ranked) {
      char rank[8];snprintf(rank,sizeof rank,"%d",i+1);
      TxtLine edge=txt_line(TXT_RANK,rank,234,236,241,255),ink=txt_line(TXT_RANK,rank,17,18,22,255);
      float x=cx-10,y=cy+SEEALL_CARD_H-edge.h;
      for(int dx=-2;dx<=2;dx+=2)for(int dy=-2;dy<=2;dy+=2)txt_draw_alpha(edge,x+dx,y+dy,a);
      txt_draw_alpha(ink,x,y,a);
    }
  }
  if(!n&&disc_seeall_loading())for(int i=0;i<5;i++)
    gfx_color((GfxRect){x0+i*264,SEEALL_TOP,248,372},.06f,.12f,.13f,.15f,a);
  // THE HEADER above the clip, so it never competes with the art — but still
  // inside the window, which is what makes it part of the thing that opened.
  cropBoth((GfxRect){ 0, 0, NV_SCREEN_W, NV_SCREEN_H }, view);
  themeHeader(a,x0,rise);

  // RE-APPLIED, not assumed: themeHeader clips inside itself for the source tabs,
  // so the window's clip is not necessarily the one still in force when it
  // returns. It used to end with gfx_no_crop(), which left the panel below
  // unclipped and drawing outside the window that is supposed to contain it.
  cropBoth((GfxRect){ 0, 0, NV_SCREEN_W, NV_SCREEN_H }, view);
  if (n > 0) panel(a);
  gfx_no_crop();
  // OUTSIDE the window: the wordmark is travelling INTO the header from a place
  // the window has not reached yet, so clipping it to the window would cut it in
  // half for the first part of the journey.
  flyingLogo(x0);
  flyingTitle(x0);
  flyingGroup(x0);
  }
}
