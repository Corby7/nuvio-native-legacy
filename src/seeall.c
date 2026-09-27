#include "seeall.h"
#include "home.h"   // the rects the opening grows and flies from
#include "badges.h"
#include "discover.h"
#include "catalog.h"
#include "gfx.h"
#include "text.h"
#include "tex_cache.h"
#include "layout.h"
#include "gridsize.h"
#include "anim.h"
#include "settings.h"
#include "director.h"
#include "dropdown.h"
#include "hold.h"
#include "ctxmenu.h"
#include "pointer.h"
#include "app.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <unistd.h>

// THE GRID IS THE APP'S GRID, WHICH IS THE LIBRARY'S. See the note on NV_LIB_*
// in layout.h.
//
// It used to be this screen's own measurement — a 248-wide poster 16 from its
// neighbour with a 20px row gap, read off `.seeall-card` — and the result was
// that opening a collection out of the Library landed the viewer on a visibly
// tighter wall than the one they had just left. Same posters, same app, two
// different grids. The card, the gaps and the row step are the Library's now,
// and the only thing this screen decides for itself is how many columns fit
// beside its detail panel.
// SIX COLUMNS, THE FULL WIDTH, AND NO DETAIL PANEL.
//
// There used to be a 336-wide column on the right holding one item's still,
// synopsis and credits, which is where 440 of these 1920 pixels went and why the
// grid could only fit five narrow columns. It is gone. A grid screen's job is the
// grid; the title screen is one press away and says all of that proprly, with
// room to say it.
//
// Everything else is still the Library's: its column count (the viewer's, from
// the grid-size button — see gridsize.h), 24 between them, 16 down to the
// title, 85.8 from one poster's foot to the next one's head.
#define SEEALL_GRID_COLS grid_cols()
#define SEEALL_COLS    (timeline ? 1 : SEEALL_GRID_COLS)
#define SEEALL_CARD_W  gridCardW()
#define SEEALL_CARD_H  (timeline ? 236.0f : SEEALL_CARD_W * 1.5f)
#define SEEALL_GAP_X   NV_LIB_CARD_GAP
// The Directors timeline is not a poster grid and keeps its own step; its rows
// are 236-tall stills with the synopsis beside them.
#define SEEALL_GAP_Y   (timeline ? 60.0f : (NV_LIB_LINE_STEP - NV_LIB_POSTER_H))
// THE CLEARANCE UNDER THE HEADER IS THE DISSOLVE BAND, which is why it is
// SEEALL_FADE and not a spacing number chosen by eye: a card has to have reached
// zero by the time it arrives at the picker row, or it is drawn over the
// dropdowns on its way past. So the grid begins exactly one band below whatever
// the header ends with — the picker row when there is one, the title when there
// is not (a home row's "See all" has no sources to pick between).
#define SEEALL_TOP     (nPicks ? SEEALL_PICK_Y + NV_DD_PICK_H + SEEALL_FADE \
                               : 244.0f)
// And the clip sits at the top of that band, where nothing is left to show.
#define SEEALL_CLIP_TOP (SEEALL_TOP - SEEALL_FADE)
// THE PICKER ROW. Two dropdowns at most, and unlike Discover's three they do not
// stretch across the screen: the detail panel owns the right of this one.
#define SEEALL_PICK_Y  212.0f
#define SEEALL_PICK_W  360.0f
#define SEEALL_PICK_GAP 12.0f
// THE BAND A CARD DISSOLVES ACROSS as it goes under the header.
//
// IT IS ALSO THE GAP between the picker row and the first row of posters, and
// that is not a coincidence — see SEEALL_TOP. So this number cannot be chosen
// for the dissolve alone: at the home's 80 the dissolve was lovely and the gap
// under the dropdowns was visibly too deep. 48 is the compromise, and it is
// Discover's clearance rather than a number invented here.
//
// It cannot go to zero. A card still partly opaque when it reaches the dropdowns
// is drawn behind them, and the header is not a solid band — it is a wordmark
// and two pills over the collection's own art, so a ghost would show through
// beside them.
#define SEEALL_FADE     48.0f


static int   is_open, focus, reqOpen = -1;
static float anim, animV, scrollY, scrollV;
// WHERE THE VIEW GREW FROM: the collection card that was focused when OK was
// pressed. The grid does not simply appear — it opens out of that card, so the
// screen the viewer gets is visibly the thing they chose. 0 when the grid was
// reached some other way (a home row's "See all"), and then it fades in place.
static GfxRect fromCard;
static int     fromValid = 0;
// The window's rectangle for THIS frame, computed once at the top of the draw.
// A file static because anything that takes a clip of its own while drawing the
// header has to stay inside the window as well.
static GfxRect gView = { 0, 0, NV_SCREEN_W, NV_SCREEN_H };
static char  title[96];
static const ColFolder *collection;
static int source, timeline, ranked;
static int order[SEEALL_MAX], orderN=-1;
static char catalogId[96];

// THE TWO PICKERS. A collection's sources are each a catalogue of ONE media
// type — "Netflix Popular" (movie), "Netflix New" (series) — and they used to be
// a row of pills, one per source, that scrolled sideways once there were more
// than six. That row made the viewer read every option end to end to find the
// two things they actually wanted to say: which kind, and which list.
//
// Two dropdowns say it in that order. Movies and Series, each offering the
// sources of its type, and only the types the folder HAS get a picker — a
// collection of films alone shows one dropdown and no empty Series beside it.
//
// The control is dropdown.c's, the same one the Discover screen draws.
enum { TY_MOVIE, TY_SERIES, TY_N };
static const char *const TY_LABEL[TY_N] = { "Movies", "Series" };
// The sources of each type, as indices into collection->sources.
static int   srcOf[TY_N][COL_SOURCE_MAX], nSrcOf[TY_N];
// The chosen option WITHIN a type's list. Kept per type so that moving from
// Series back to Movies returns to the movie list the viewer had, rather than
// resetting to the first one.
static int   selOf[TY_N];
// The types that have a picker, left to right, and which of them the focus is
// on. `pickFocus` is the old `tabFocus`: 1 when the row has the focus and not
// the grid.
static int   picks[TY_N], nPicks, pickSel, pickFocus;
static float pickAnim[TY_N];
// The OPEN list: the TYPE whose options are expanded (-1 when none), and the
// row inside it — which is NOT selOf: the list opens on the current value and
// moving inside it must change nothing until OK. The same split Discover and
// the detail screen's season picker use.
static int   menuOpen = -1, menuFocus;

// WHETHER THE SCROLL FOLLOWS THE FOCUS. The grid aims the focused row at 30% of
// the screen; with the Magic Remote's pointer on a lower row that slid another
// row under a pointer that had not moved, which focused it, which scrolled
// again. So a focus set by the POINTER leaves the scroll where it is (goalY),
// and any arrow — the wheel's included, which is how a pointer scrolls — hands
// it back to the focus.
static int followFocus = 1;
static float goalY;
// One spring for the grid's focus, because only ever one card is focused. The
// card it belongs to is `focus`; when that moves the scale stays put and the
// new card simply has it. Discover does the same.
static float animCard;
// THE GRID-SIZE BUTTON sits at the right end of the picker row and is reached
// like one more picker: `pickSel == nPicks` is the button. With no pickers (a
// home row's "See all") it is the only thing in that row. The Directors
// timeline is one column by design and has none.
#define HAS_GRID_BTN (!timeline)
static float animBtn;

// HOW WIDE A CARD HAS TO BE for six of them to fit the width.
//
// IT DEPENDS ON THE RAIL, which is why it is read off settings_content_x rather
// than fixed at the Library's 268. The Library hardcodes x=96 and a 268 card;
// this screen cannot, because with the fixed rail on (the owner's profile) the
// content genuinely starts at 248 and six 268s would need 1728 more — 56 past
// the right edge of the screen. Fitted, the card is about 241 with the rail and
// about 265 without it, which is the Library's to within a couple of pixels.
//
// Never wider than the Library's card at the same column count: that is the
// app's poster, and a screen with nothing on either side of it should not grow
// past it.
static float gridCardW(void) {
  float avail = NV_SCREEN_W - settings_content_x() - NV_CONTENT_PAD;
  float w = (avail - (SEEALL_GRID_COLS - 1) * NV_LIB_CARD_GAP)
          / (float)SEEALL_GRID_COLS;
  if (w < 120.0f) w = 120.0f;
  return w > grid_card_w() ? grid_card_w() : w;
}
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

static int yearOf(const CatItem *it) {
  for(const char *s=it->meta;*s;s++)if((*s=='1'||*s=='2')&&strlen(s)>=4&&s[1]>='0'&&s[1]<='9'&&s[2]>='0'&&s[2]<='9'&&s[3]>='0'&&s[3]<='9')return atoi(s);
  return 9999;
}
static int viewItem(int i,CatItem *out) {return disc_seeall_item(timeline&&i<orderN?order[i]:i,out);}

static int typeOf(const ColSource *s) {
  return !strcmp(s->type, "series") ? TY_SERIES : TY_MOVIE;
}
// Which picker is in effect: the type of the source the grid is showing. The
// other one is drawn dim, because with two pickers over one grid, two white
// values would say both are live and neither would say which list this is.
static int activeType(void) {
  if (!collection || source < 0 || source >= collection->nSources) return TY_MOVIE;
  return typeOf(&collection->sources[source]);
}
// THE CATALOGUE'S REAL NAME, out of the addon's manifest.
//
// NOT ONE of the owner's collection sources carries a title — checked against
// the real account, all 129 of them across Genres, Streaming Services and
// Discover arrive with title="" — so every option on this screen is named by
// this function or not at all. Untouched, they read "mdblist.91211", which is
// the catalogue's id and tells nobody anything.
//
// THE ADDONS NAME THEIR CATALOGUES, in the manifests the app has already read.
// This used to ask cat_row for that name, and cat_row is the wrong list: it
// holds the catalogues the HOME is showing, around two dozen of them, ordered
// and filtered by the owner's preferences. That is why only Streaming Services
// came out named — those few catalogues happen to be on the home — while every
// genre folder, pointing at mdblist lists the owner has no home row for, fell
// straight through to the id. disc_catalog_title reads the whole DECLARED set
// instead, which is every catalogue of every installed addon.
//
// It comes back as "Top Rated - Movie", because the home's rows want the type on
// the end of the line (formatTitle, discover.c). Here the picker's own label
// says Movies directly above it, so the suffix comes off again rather than being
// printed twice on one row.
static const char *catalogName(const ColSource *s) {
  static char out[96];
  const char *base = col_source_base(s);
  const char *found;
  if (!base || !*base) return "";
  found = disc_catalog_title(base, s->type, s->catId);
  if (!found || !*found) return "";
  snprintf(out, sizeof out, "%s", found);
  // The LAST " - ", not the first: a catalogue genuinely called
  // "Top - Rated - Movie" must lose only the type.
  { char *cut = NULL, *p = out;
    for (; *p; p++) if (p[0]==' ' && p[1]=='-' && p[2]==' ') cut = p;
    if (cut && (!strcasecmp(cut + 3, "Movie") || !strcasecmp(cut + 3, "Series")))
      *cut = 0; }
  return out;
}
// WHAT AN OPTION READS AS. A collection's sources carry the folder's name in
// their own titles — "Netflix Popular", "Netflix New Releases" — and the header
// directly above already says Netflix in letters an inch tall. So the name comes
// off and what is left is the actual choice: "Popular", "New Releases". A source
// titled exactly like its folder has no choice in it and keeps its own name.
static const char *sourceOption(int idx) {
  static char out[128];
  const ColSource *s;
  const char *t;
  size_t n;
  if (!collection || idx < 0 || idx >= collection->nSources) return "";
  s = &collection->sources[idx];
  t = s->title[0] ? s->title : catalogName(s);
  if (!t[0]) t = s->catId;
  n = strlen(collection->title);
  if (n && !strncasecmp(t, collection->title, n)) {
    const char *rest = t + n;
    while (*rest == ' ' || *rest == '-' || *rest == ':') rest++;
    if (*rest) t = rest;
  }
  snprintf(out, sizeof out, "%s", t);
  return out;
}
// dropdown.c asks for its rows through this; `ctx` is the type being listed.
static const char *pickOption(void *ctx, int i) {
  int t = *(int *)ctx;
  return (t >= 0 && t < TY_N && i >= 0 && i < nSrcOf[t])
       ? sourceOption(srcOf[t][i]) : "";
}
// The pickers follow whatever source is live, so a grid reached by ANY route —
// the collection card on the home, a home row's "See all" pointing at one
// catalogue of the folder — opens with the right picker lit and the right
// option under it.
static void syncPickers(void) {
  int t, i;
  if (!collection || source < 0 || source >= collection->nSources) return;
  t = typeOf(&collection->sources[source]);
  for (i = 0; i < nSrcOf[t]; i++) if (srcOf[t][i] == source) selOf[t] = i;
  for (i = 0; i < nPicks; i++) if (picks[i] == t) pickSel = i;
}
static void buildPickers(void) {
  int t, i;
  nPicks = 0; pickSel = 0; pickFocus = 0;
  menuOpen = -1; menuFocus = 0;
  memset(pickAnim, 0, sizeof pickAnim);
  memset(nSrcOf, 0, sizeof nSrcOf);
  memset(selOf, 0, sizeof selOf);
  if (!collection) return;
  for (i = 0; i < collection->nSources; i++) {
    int ty = typeOf(&collection->sources[i]);
    if (nSrcOf[ty] < COL_SOURCE_MAX) srcOf[ty][nSrcOf[ty]++] = i;
  }
  for (t = 0; t < TY_N; t++) if (nSrcOf[t]) picks[nPicks++] = t;
  syncPickers();
}
static void openSource(void);
// Commits option `i` of the type-`t` list. The dropdown's OK lands here, and
// nothing else changes which source is on screen.
static void chooseSource(int t, int i) {
  if (t < 0 || t >= TY_N || i < 0 || i >= nSrcOf[t]) return;
  selOf[t] = i;
  if (srcOf[t][i] == source) return;   /* already the one showing */
  source = srcOf[t][i];
  openSource();
}
static void openSource(void) {
  const ColSource *s=&collection->sources[source];
  snprintf(catalogId,sizeof catalogId,"%s",s->catId);
  ranked=strstr(s->catId,"top100")||strstr(s->catId,"top250")||strstr(s->catId,"top10");
  focus=0;scrollY=scrollV=0;orderN=-1;
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
  anim = 0.0f; animV = 0.0f; scrollY = scrollV = 0.0f; focus = 0;
  animCard = 0.0f;
  collection=folder;source=0;is_open=1;reqOpen=-1;followFocus=1;
  timeline=!strcmp(folder->group,"Directors");
  snprintf(title,sizeof title,"%s",folder->title);openSource();
  buildPickers();
  // The row starts with the focus only when there is a choice to make in it.
  // One source is one grid, and landing on a dropdown that cannot open is a
  // press wasted before the viewer has seen a single poster.
  pickFocus = folder->nSources > 1;
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
  seeall_search(base, kind, catId, heading, "");
}

void seeall_search(const char *base, const char *kind, const char *catId,
                   const char *heading, const char *term) {
  // A home row's "See all" card is not a collection card and has no wordmark to
  // carry: that path keeps the plain fade it has always had.
  fromValid = 0;
  is_open = 1; focus = 0; scrollY = scrollV = 0.0f; reqOpen = -1; followFocus = 1;
  animCard = 0.0f;
  snprintf(title, sizeof title, "%s", heading ? heading : "");
  collection=col_by_catalog(base,kind,catId);timeline=collection&&!strcmp(collection->group,"Directors");
  source=0;orderN=-1;
  if(collection) {
    for(int i=0;i<collection->nSources;i++)if(!strcmp(collection->sources[i].catId,catId)&&!strcmp(collection->sources[i].type,kind)){source=i;break;}
    snprintf(title,sizeof title,"%s",collection->title);
  }
  // AFTER the source is resolved, so syncPickers lights the picker that matches
  // the catalogue this was opened on and not the folder's first one.
  buildPickers();
  snprintf(catalogId,sizeof catalogId,"%s",catId);
  ranked=strstr(catId,"top100")||strstr(catId,"top250")||strstr(catId,"top10");
  if (term && term[0]) disc_seeall_search(base, kind, catId, term);
  else disc_seeall_open(base, kind, catId);
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

// The pointer's setters. Each does what the keys that reach the same place do.
static void pointCard(int i, int unused) {
  (void)unused;
  if (!is_open || menuOpen >= 0 || i < 0 || i >= nItems()) return;
  pickFocus = 0;
  focus = i;
  followFocus = 0;
  if (focus >= nItems() - SEEALL_COLS * 4) disc_seeall_more();
}
static void pointPick(int i, int unused) {
  (void)unused;
  if (!is_open || menuOpen >= 0 || i < 0 || i >= nPicks + HAS_GRID_BTN) return;
  if (!pickFocus) { pickFocus = 1; syncPickers(); }
  pickSel = i;
  followFocus = 0;
}
static void pointOption(int c, int unused) {
  (void)unused;
  if (menuOpen >= 0 && c >= 0 && c < nSrcOf[menuOpen]) menuFocus = c;
}
static void pointOffMenu(int a, int b) { (void)a; (void)b; menuOpen = -1; }

// Holding OK on a card opens the poster menu (hold.h). The card it opens beside
// is the one drawn focused on the last frame.
static Hold hold;
static GfxRect holdCard;
static float holdCardR;
static int hasHoldCard;

// The focused card as a catalogue index. The grid item is NOT in the global
// catalogue — it came from a page only this screen read. It goes in through
// cat_append so the title screen and the hold menu can open it by index, which
// is how the whole app works. -1 when there is no card.
static int focusedIndex(void) {
  CatItem it;
  int idx;
  if (focus < 0 || focus >= nItems() || !viewItem(focus, &it)) return -1;
  idx = it.imdb[0] ? cat_index_by_imdb(it.imdb) : -1;
  if (idx < 0) idx = cat_append(&it);
  return idx;
}

void seeall_event(const SDL_Event *e) {
  int n = nItems(), k;
  if (!is_open) return;
  // OK over a card is a tap or a hold, and only the release can tell which.
  { int tap;
    if (hold_event(&hold, e, menuOpen < 0 && !pickFocus && n > 0 &&
                             !disc_seeall_error(), &tap)) {
      // A tap keeps the list and the position on the way back.
      if (tap) { int idx = focusedIndex(); if (idx >= 0) reqOpen = idx; }
      return;
    } }
  if (e->type != SDL_KEYDOWN) return;
  k = e->key.keysym.sym;
  if (k == SDLK_UP || k == SDLK_DOWN || k == SDLK_LEFT || k == SDLK_RIGHT) followFocus = 1;
  { int back = (k == SDLK_AC_BACK || k == SDLK_ESCAPE || k == SDLK_BACKSPACE ||
                e->key.keysym.scancode == NV_SCANCODE_BACK);
    // AN OPEN LIST OWNS THE WHOLE D-PAD, Back included: it is a question
    // standing in front of the screen, and Back answers the question rather
    // than leaving the grid. Nothing behind it sees a key. The same rule
    // Discover and the detail screen's season picker follow — which is why this
    // stands ABOVE the Back that closes the screen and not below it.
    if (menuOpen >= 0) {
      int t = menuOpen;
      if (back || k == SDLK_LEFT || k == SDLK_RIGHT) menuOpen = -1;
      else if (k == SDLK_UP)   { if (menuFocus > 0) menuFocus--; }
      else if (k == SDLK_DOWN) { if (menuFocus + 1 < nSrcOf[t]) menuFocus++; }
      else if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) {
        menuOpen = -1;
        chooseSource(t, menuFocus);
      }
      return;
    }
    if (back) { is_open = 0; return; } }
  if((nPicks||HAS_GRID_BTN)&&pickFocus) {
    if(k==SDLK_LEFT&&pickSel>0)pickSel--;
    else if(k==SDLK_RIGHT&&pickSel+1<nPicks+HAS_GRID_BTN)pickSel++;
    // The button: OK steps the size, and the grid re-flows around the title
    // the focus was on.
    else if((k==SDLK_RETURN||k==SDLK_KP_ENTER||k==SDLK_SPACE)&&pickSel>=nPicks)
      grid_cycle();
    else if(k==SDLK_RETURN||k==SDLK_KP_ENTER||k==SDLK_SPACE) {
      // A list with one option in it is not a question. The picker is still
      // drawn — it says which list is on screen — but OK does not open a sheet
      // whose only row is the one already showing.
      int t = picks[pickSel];
      if (nSrcOf[t] > 1) { menuOpen = t; menuFocus = selOf[t]; }
    }
    else if(k==SDLK_DOWN&&n>0)pickFocus=0;
    return;
  }
  if(k==SDLK_UP&&focus<SEEALL_COLS&&(nPicks||HAS_GRID_BTN)){pickFocus=1;syncPickers();return;}
  if((k==SDLK_RETURN||k==SDLK_KP_ENTER)&&disc_seeall_error()){disc_seeall_more();return;}
  if (n < 1) return;
  if (k == SDLK_RIGHT && focus + 1 < n) focus++;
  else if (k == SDLK_LEFT && focus > 0) focus--;
  else if (k == SDLK_DOWN) { if (focus + SEEALL_COLS < n) focus += SEEALL_COLS;
                             else focus = n - 1; }
  else if (k == SDLK_UP) { if (focus >= SEEALL_COLS) focus -= SEEALL_COLS; }
  // OK on a card is handled above, by the hold.
  // Nearing the end, ask for the next page. Before the owner sees the empty
  // space, not once they are already staring at it.
  // Four rows ahead and not two, for Discover's reason: the page has to land AND
  // its posters have to arrive before the viewer gets there.
  if (focus >= n - SEEALL_COLS * 4) disc_seeall_more();
}

void seeall_update(float dt, Uint32 now) {
  float target, maxY;
  int n = nItems(), lines;
  hold_animate(&hold, dt, now);
  if (hold_fired(&hold, now) && is_open && !pickFocus) {
    int idx = focusedIndex();
    if (idx >= 0) {
      if (hasHoldCard) ctx_set_anchor(holdCard, holdCardR);
      ctx_open(idx);
    }
  }
  // Critically damped, for the reason anim.h records at anim_spring2 and the
  // detail screen's flight documents at NV_SPRING2_SCREEN: a first-order spring
  // leaves at maximum speed, which on this size of movement is a cut.
  anim = anim_spring2(&animV, anim, is_open ? 1.0f : 0.0f, dt,
                      is_open ? NV_SPRING2_GRID : NV_SPRING2_GRID_OUT);
  if (!is_open) return;
  for (int t = 0; t < TY_N; t++) {
    float targetPick = pickFocus && pickSel < nPicks && picks[pickSel] == t ? 1.0f : 0.0f;
    pickAnim[t] = anim_spring(pickAnim[t], targetPick, dt,
                           targetPick > pickAnim[t] ? NV_SPRING_FOCUS : NV_SPRING_BLUR);
  }
  { float targetBtn = HAS_GRID_BTN && pickFocus && pickSel >= nPicks ? 1.0f : 0.0f;
    animBtn = anim_spring(animBtn, targetBtn, dt,
                          targetBtn > animBtn ? NV_SPRING_FOCUS : NV_SPRING_BLUR); }
  // The grid's own focus. It is the CARD scale, so it drops to 0 while the
  // picker row has the focus: a poster still swollen under a dropdown the viewer
  // has moved up to reads as two things focused at once.
  { float targetCard = (!pickFocus && n > 0) ? 1.0f : 0.0f;
    animCard = anim_spring(animCard, targetCard, dt,
                           targetCard > animCard ? NV_SPRING_FOCUS : NV_SPRING_BLUR); }
  if(timeline&&n!=orderN) {
    int old=orderN>0&&focus<orderN?order[focus]:-1;int years[SEEALL_MAX];
    for(int i=0;i<n;i++){CatItem it;order[i]=i;years[i]=disc_seeall_item(i,&it)?yearOf(&it):9999;}
    for(int i=1;i<n;i++){int v=order[i],j=i;while(j>0&&years[order[j-1]]>years[v]){order[j]=order[j-1];j--;}order[j]=v;}
    orderN=n;if(old>=0)for(int i=0;i<n;i++)if(order[i]==old){focus=i;break;}
  }
  lines = (n + SEEALL_COLS - 1) / SEEALL_COLS;
  // Aims the focused row at 30% of the usable height, as the rest of the app does.
  { float row = (float)(focus / SEEALL_COLS) * (SEEALL_CARD_H + SEEALL_GAP_Y);
    target = SEEALL_TOP + row - NV_SCREEN_H * 0.30f;
    // BUT NEVER PAST THE TOP OF THE GRID, and this is what was slicing the
    // posters. 30% of 1080 is 324 and the grid begins at 360, so the aim alone
    // put the focused row's head at y=324 — twelve pixels ABOVE the line the
    // grid is clipped at — and the top 24px of every focused card was cut off
    // by the header band. It was true from the very first row, because the aim
    // is 36px of scroll even at row 0.
    //
    // Clamping to `row` lands that row exactly on SEEALL_TOP: flush under the
    // header, nothing cut. Where the header is shallow enough for the 30% aim
    // to sit below it the aim still wins, so this is a floor and not a
    // replacement for it. Discover reached the same place from the other
    // direction — see the snap note in discoverui.c.
    if (target > row) target = row; }
  if(pickFocus)target=0;
  if (!followFocus) target = goalY;
  goalY = target;
  maxY = SEEALL_TOP + (float)lines * (SEEALL_CARD_H + SEEALL_GAP_Y) - NV_SCREEN_H + 120.0f;
  if (maxY < 0.0f) maxY = 0.0f;
  if (target < 0.0f) target = 0.0f;
  if (target > maxY) target = maxY;
  // THE POINTER RESTING ON THE GRID'S EDGE scrolls it; the focus stays put.
  { float d = app_seeall_in_front() && menuOpen < 0 && n > 0
            ? pointer_edge_scroll(0.0f, NV_SCREEN_W, SEEALL_CLIP_TOP, NV_SCREEN_H, dt) : 0.0f;
    if (d != 0.0f) { target = goalY = anim_clamp(goalY + d, 0.0f, maxY); followFocus = 0; } }
  scrollY = anim_spring2_reduced(&scrollV, scrollY, target, dt, NV_SPRING2_PAGE,
                                settings_animations_reduced());
}

// THE RIGHT-HAND PANEL IS GONE, and this is where it was.
//
// It showed the focused item's art, synopsis, score and credits in a 336-wide
// column on the right, and it was a port of the web app's `.seeall-detail`. Two
// things were wrong with it on a television. It opened by drawing the focused
// POSTER — the very card the viewer was looking at, at nearly the same size, a
// few inches to its left — so the first half of the column was a copy of
// something already on screen. And the column cost 440 of the 1920 pixels here,
// which is what held the grid down to five narrow posters.
//
// Replacing the poster with the title's still fixed the duplication and bought
// the synopsis four more lines, but not the width: a browse screen was still
// spending a quarter of itself on one item. The title screen is ONE PRESS away
// and gives that item the whole display. So the column went, the grid took the
// width, and the posters are the Library's again.

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

// Where picker `i` of the row sits. `dy` is the content's rise, so the row
// arrives with the copy above it rather than being pinned while everything
// around it moves.
static GfxRect pickRect(int i, float x0, float dy) {
  GfxRect r = { x0 + i * (SEEALL_PICK_W + SEEALL_PICK_GAP), SEEALL_PICK_Y + dy,
                SEEALL_PICK_W, NV_DD_PICK_H };
  return r;
}

static void themeHeader(float a,float x0,float dy) {
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
  // NO COUNT AND NO STRAPLINE. This line used to read "20 titles loaded  ·  A
  // selection from your catalogue", and neither half earned its place: the
  // viewer can see how many posters there are, "loaded" exposed the pager as a
  // fact about the fetch rather than about the list, and the strapline was a
  // sentence generated per group ("Curated by theme and mood") that said nothing
  // the folder's own name had not already said.
  //
  // What is left is the three things the grid genuinely CANNOT say for itself:
  // that it is still arriving, that it is empty, or that it failed — and those
  // are drawn WHERE THE GRID WOULD BE, not as a strapline under the title. The
  // caption's old slot at 192 is now 20px above the picker row, but that is not
  // why it moved: a line saying the list is empty belongs in the empty space,
  // which is what Discover's own empty state does.
  //
  // AND NOT "Loading titles…" ANY MORE: the skeleton row below draws in exactly
  // this space while the fetch is out, and a word for it on top of the blocks
  // that already mean it is one message twice, printed over itself.
  { int n = nItems();
    const char *msg = disc_seeall_error() ? "Could not load. OK to try again."
                    : (n || disc_seeall_loading()) ? NULL
                                            : "No titles in this list.";
    if (msg) {
      TxtLine sub = txt_line_trim(TXT_DET_META2, msg, 196, 202, 213, 255, 960);
      txt_draw_alpha(sub, x0, SEEALL_TOP + 12.0f + dy, a);
    } }
  // THE PICKERS. The pill per source that used to be here is in the history at
  // the head of this file's picker block; these are dropdown.c's, the same
  // control Discover draws.
  for (int i = 0; i < nPicks; i++) {
    int t = picks[i];
    dd_pill(pickRect(i, x0, dy), TY_LABEL[t], sourceOption(srcOf[t][selOf[t]]),
            pickAnim[t], t == activeType(), a);
    { GfxRect pr = pickRect(i, x0, dy); pointer_zone(pr.x, pr.y, pr.w, pr.h, pointPick, i, 0); }
  }
  // Right-aligned on the picker row, centred on its pills. With no picker row
  // the grid starts high, so it moves up beside the wordmark instead.
  if (HAS_GRID_BTN) {
    GfxRect b = grid_button_draw(NV_SCREEN_W - NV_CONTENT_PAD,
                     (nPicks ? SEEALL_PICK_Y + (NV_DD_PICK_H - 64.0f) * 0.5f
                             : 105.0f) + dy, animBtn, a);
    pointer_zone(b.x, b.y, b.w, b.h, pointPick, nPicks, 0);
  }
}

static void timelineCard(int i,float cy,float a,float x0) {
  CatItem it;if(!viewItem(i,&it))return;
  int sel=i==focus&&!pickFocus;float r,g,b;colorCollection(&r,&g,&b);
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
  hasHoldCard = 0;
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
  cropBoth((GfxRect){ 0.0f, SEEALL_CLIP_TOP, NV_SCREEN_W,
                      NV_SCREEN_H - SEEALL_CLIP_TOP }, view);
  a = ac;   /* the grid and the copy below come in on the content's ramp */
  // TWO PASSES: the focused card GROWS, and a card that grows has to be drawn
  // over its neighbours or the poster beside it clips its border. The Library
  // and Discover both do this, and for the same reason.
  pointer_clip(0.0f, SEEALL_CLIP_TOP, NV_SCREEN_W, NV_SCREEN_H - SEEALL_CLIP_TOP);
  for (int pass = 0; pass < 2; pass++)
  for (i = 0; i < n; i++) {
    float cx = x0 + (float)(i % SEEALL_COLS) * (SEEALL_CARD_W + SEEALL_GAP_X);
    float cy = SEEALL_TOP + (float)(i / SEEALL_COLS) * (SEEALL_CARD_H + SEEALL_GAP_Y)
             - scrollY + rise;
    CatItem it;
    GLuint t;
    int sel = (i == focus && !pickFocus);
    // CARDS DISSOLVE AS THEY LEAVE, exactly as the home's rows do.
    //
    // The clip alone removes them, but a poster that is simply chopped off by an
    // invisible line reads as a rendering fault — which is what this screen did:
    // a row slid up and was guillotined against the header. The home answers it
    // with an 80px band above the fold in which a row ramps to nothing (see the
    // top-edge mask in home_draw), so what crosses the boundary has already gone.
    // Same band, same smoothstep, same clock here.
    //
    // MEASURED AT REST, not at the animated position: `cy` carries the opening
    // window's rise, and reading the ramp through that faded the whole grid in
    // from the top on every entrance. The home's note records the same trap.
    float edge = anim_edge(cy - rise, SEEALL_CLIP_TOP, SEEALL_FADE);
    // THE FOCUSED POSTER SCALES, which it did not before: it got a white ring
    // laid 4px outside it and stayed exactly the size of its neighbours. Every
    // other grid in this app grows the card instead — `.library-grid-card.focused
    // { transform: scale(1.02) }` with the origin at the TOP, and the 4px border
    // on the INSIDE rather than as a halo outside. A ring where the rest of the
    // app has a lift is what made this screen feel like a different app.
    float f = (sel && !timeline) ? animCard : 0.0f;
    float scale = 1.0f + NV_LIB_FOCUS_SCALE * f;
    float cw = SEEALL_CARD_W * scale, chh = SEEALL_CARD_H * scale;
    // `transform-origin: top`: the top edge stays on its row and the growth goes
    // downwards, so only x is re-centred.
    GfxRect r = { cx - (cw - SEEALL_CARD_W) * 0.5f, cy, cw, chh };
    // Held: pressed in about its centre (hold.h). The glow goes behind it below.
    if (sel && !timeline) r = hold_card(&hold, r);
    // The SAME radius as the home's posters: `posterCardCornerRadiusDp` (12dp x
    // 2 = 24px), a fraction of the SMALLER side because the shader's SDF is
    // normalised. The fixed NV_RADIUS_CARD that used to be here gave a corner
    // different from the rest of the app, and the grid read as another screen.
    // The SDF's radius is a fraction of the HEIGHT, not of the smaller side:
    // `p = (uv-0.5)*vec2(asp,1.0)` makes one SDF unit h pixels on both axes.
    // Dividing by the width rounded this poster half again too much. See the
    // note on radiusInset in home.c.
    float radius = settings_radius_poster_px() / r.h;
    if (sel && !timeline) { holdCard = r; holdCardR = radius * r.h; hasHoldCard = 1; }
    if (cy > NV_SCREEN_H || cy + SEEALL_CARD_H + 40.0f < SEEALL_CLIP_TOP) {
      // Off screen: warm the rows within reach so a DOWN brings up posters and
      // not skeletons (gridsize.h). Only those rows, because a folder runs to
      // hundreds of titles and each copy out of the page cache costs.
      float step = SEEALL_CARD_H + SEEALL_GAP_Y;
      if (!timeline && pass == 0 && cy < NV_SCREEN_H + step * 4.0f &&
          cy + step > -step * 1.5f && viewItem(i, &it))
        grid_warm(it.poster[0] ? it.poster : it.backdrop, cy, SEEALL_CARD_W, step);
      continue;
    }
    // The Directors timeline scrolls in the same viewport and dissolves with it.
    if(timeline){
      if(!pass&&edge>0.004f){
        gfx_opacity_group=edge;timelineCard(i,cy,a,x0);gfx_opacity_group=1.0f;
      }
      continue;}
    if (pass == 0) pointer_zone(r.x, r.y, r.w, r.h, pointCard, i, 0);
    /* pass 0 lays the row, pass 1 puts the one that grew back on top of it */
    if ((pass == 1) != (f > 0.01f)) continue;
    if (!viewItem(i, &it)) continue;
    if (edge <= 0.004f) continue;
    gfx_opacity_group = edge;
    {
      // The RESTING width, not the animated one: tex_cache re-decodes an exact
      // request whose width moves, and a poster that re-decodes through a focus
      // spring is a poster that is missing for the length of it.
      if (sel) hold_glow(&hold, r, radius * r.h);
      t = it.poster[0] ? tex_get_width(it.poster, SEEALL_CARD_W)
        : (it.backdrop[0] ? tex_get_width(it.backdrop, SEEALL_CARD_W) : 0);
      if (t) {
        gfx_tex_aspect_current = tex_aspect(it.poster[0] ? it.poster : it.backdrop);
        gfx_rect(r, t, GFX_CARD, f, 0, 0, radius, 0, 0, 0, a);
        gfx_tex_aspect_current = 0.0f;
      } else {
        // A skeleton while the art has not arrived — the same colour as the rest of
        // the app, and on the same sweep of light (gfx_skeleton).
        gfx_skeleton(r, radius, NV_COLOR_SKELETON_R, NV_COLOR_SKELETON_G,
                NV_COLOR_SKELETON_B, a);
      }
      // The focus border, 4px ON THE INSIDE — "Android TV uses the inside focus
      // border, not an outer halo", says the stylesheet.
      //
      // GFX_RING_INSET AND NOT GFX_RING, and this is what made the corners look
      // wrong. GFX_RING centres its band on the shape's contour, so half the
      // stroke falls OUTSIDE the rounded rectangle. Along the straight sides
      // that half lands outside the quad and is simply never rasterised, giving
      // a 3px line; at the CORNERS it lands in the square quad's leftover
      // triangle, where there IS a fragment to paint, so the full 6px got drawn
      // and the border visibly swelled and bulged at each corner. Thin sides,
      // fat corners, on every focused poster.
      //
      // GFX_RING_INSET puts the band strictly between the contour and
      // `thickness` inside it, so it is the same weight the whole way round.
      //
      // AND /r.h, NOT /r.w: the SDF is normalised to the HEIGHT — `p =
      // (uv-0.5)*vec2(asp,1.0)` makes one unit h pixels on both axes — so a
      // thickness divided by the width comes out at w/h of what was asked for.
      // The ring, with the hold's sweep over it while OK is held.
      hold_ring(&hold, r, NV_LIB_POSTER_BORDER / r.h, radius,
                0.961f, 0.961f, 0.961f, f * a);
      if (sel) hold_track(&hold, r, radius * r.h); }
    // The title stays on the UNSCALED column, as the Library's does: a name
    // that slid sideways under a poster that grew would be two movements where
    // the card only made one.
    { int c = sel ? 255 : 214;
      TxtLine l = txt_line_trim(TXT_CALLOUT, it.title, c, c, c, 255,
                                   SEEALL_CARD_W);
      txt_draw_alpha(l, cx, cy + SEEALL_CARD_H + NV_LIB_TITLE_GAP,
                     a * (sel ? 1.0f : 0.86f)); }
    if(ranked) {
      char rank[8];snprintf(rank,sizeof rank,"%d",i+1);
      TxtLine mark=txt_line(TXT_RANK,rank,234,236,241,255),ink=txt_line(TXT_RANK,rank,17,18,22,255);
      float x=r.x-10,y=r.y+r.h-mark.h;
      for(int dx=-2;dx<=2;dx+=2)for(int dy=-2;dy<=2;dy+=2)txt_draw_alpha(mark,x+dx,y+dy,a);
      txt_draw_alpha(ink,x,y,a);
    }
    // PUT IT BACK. A group opacity left set bleeds onto everything drawn after
    // it — here that would be the header and the wordmark.
    gfx_opacity_group = 1.0f;
  }
  gfx_opacity_group = 1.0f;
  pointer_no_clip();
  if(!n&&disc_seeall_loading())for(int i=0;i<SEEALL_COLS;i++)
    gfx_color((GfxRect){x0+i*(SEEALL_CARD_W+SEEALL_GAP_X),SEEALL_TOP,
                        SEEALL_CARD_W,SEEALL_CARD_H},.06f,.12f,.13f,.15f,a);
  // THE HEADER above the clip, so it never competes with the art — but still
  // inside the window, which is what makes it part of the thing that opened.
  cropBoth((GfxRect){ 0, 0, NV_SCREEN_W, NV_SCREEN_H }, view);
  themeHeader(a,x0,rise);

  gfx_no_crop();
  // OUTSIDE the window: the wordmark is travelling INTO the header from a place
  // the window has not reached yet, so clipping it to the window would cut it in
  // half for the first part of the journey.
  flyingLogo(x0);
  flyingTitle(x0);
  flyingGroup(x0);
  // LAST OF EVERYTHING, and uncropped: an open list covers the grid, the panel
  // and the picker beside it, and it carries a shadow that has to fall on them.
  if (menuOpen >= 0) {
    int t = menuOpen, i;
    for (i = 0; i < nPicks && picks[i] != t; i++) ;
    if (i < nPicks) {
      // A click anywhere off the list closes it, as Back does.
      pointer_zone_click(0, 0, NV_SCREEN_W, NV_SCREEN_H, pointOffMenu, 0, 0);
      dd_menu_point(pointOption);
      dd_menu(pickRect(i, x0, rise), nSrcOf[t], menuFocus, pickOption, &t, a);
    }
  }
  }
}
