// The title's detail screen, in the WEB APP's layout (SIGNED-IN session).
//
// The port started out copying the Apple TV app, and every piece of that
// inheritance has been given back as the web app was MEASURED. What was left of it
// until now was everything below the fold — 236x63 season pills, an episode card
// with the text BELOW the thumbnail, "Trailers", "How to watch" and "About"
// sections. None of that exists in the web app. What does exist, measured at
// 1920x1080 on the series "Silo" with the owner's profile:
//
//   1. The screen is ONE scrollable document 2144px tall. The hero takes the first
//      1080 and SCROLLS with it: there is no fixed header and no logo centred at
//      the top. Going down does not "stretch" anything — it just scrolls.
//   2. There are four sections: season tabs (269x80), the episode row (640x422
//      cards with the text INSIDE the thumbnail), information tabs
//      ("Creator and cast | Ratings | More like this | Trailer") and the cast row
//      (a round 140 avatar).
//   3. Scrolling takes the top of the focused group to 33% of the usable height
//      (40% on the information tabs). This is in the web app's source
//      (DETAIL_ROW_FOCUS_TARGET) and was checked by measuring scrollTop on all four
//      groups.
//   4. On scrolling, the background art does NOT blur: it goes to 15% opacity over
//      0.8s. The gaussian blur was the Apple TV app's.
#include "detail.h"
#include "badges.h"
#include "mark.h"
#include "settings.h"
#include "home.h"
#include "extras.h"
#include "person.h"
#include "streams.h"
#include "discover.h"
#include "gfx.h"
#include "text.h"
#include "tex_cache.h"
#include "focus.h"
#include "anim.h"
#include "layout.h"
#include "catalog.h"
#include "trakt.h"   // trakt_active(), for the library tooltip's wording
#include <stdio.h>
#include <string.h>
#include <math.h>

// A ceiling of items per section. 24 and not 8: a season of "Silo" has 10 episodes
// and the array of 8 hid the last two — the list looked smaller than the series is.
//
// 40 AND NOT 24, for the same defect one layer up: the season picker is a section
// too, and nSeasonsOf() clamps to this number. With CAT_MAX_SEASONS raised to 40
// and this left at 24, South Park's 29 seasons would have come out of the
// catalogue whole and then been cut to 24 here — the same silent truncation
// moved, not fixed. The two have to be read together, so keep this >= that.
//
// It lifts the EPISODE ceiling with it, which is the original bug's own class: a
// season of more than 24 episodes (most of anime, and the long US network runs)
// was losing its tail exactly as "Silo" lost its last two.
#define N_ITEMS    40
// SIX sections, but no title uses all six: a series lights up the first four and a
// film the last three. The ones that do not apply to the type return 0 in sectionN,
// and focus_move SKIPS an empty row — so the enum's order already gives the right
// visual order in both cases, with no translation table in between:
//   series -> Seasons, Episodes, Tabs, Cast
//   film   -> Cast, Trailers, Details
// The "Recommendations" and "Comments" cards. They used to sit next to the functions
// that draw them, further down; they moved up because widthItem and xItem, at the
// top, need them to position the new SECTIONS.
#define REL_CARD_W   212.0f
#define REL_CARD_H   318.0f
#define REL_CARD_GAP  32.0f
// MEASURED against the reference (TCL, 1920x1080): a 722x466 card, a gap of 25, a corner of 20.
#define COM_CARD_W   722.0f
#define COM_CARD_H   466.0f
#define COM_PAD       28.0f
#define COM_CARD_GAP  25.0f   // MEDIDO

#define N_SECTIONS    8
#define N_CAST    6

static HomeItem item;
static int  is_open = 0, exiting = 0;
static int  idx = 0;                 // the current title within the collection
static float t = 0.0f;               // 0 = the card on the home, 1 = full screen
// The flight's VELOCITY. The spring that drives `t` is second-order (see
// NV_SPRING2_SCREEN in layout.h), so the position alone does not describe its
// state — and without the velocity kept here the movement would leave at full
// speed on its first frame, which is exactly the cut this change removes.
static float velT = 0.0f;
// Whether this opening has a hero on screen behind it to continue FROM. Set by the
// router at detail_open; see the note there. It selects the whole shape of the
// transition, not a detail of it: with an origin the backdrop flies out of the
// hero's rect and the logo crosses the screen, without one both would start from
// coordinates nothing has ever been drawn at.
static int sharedOrigin = 0;
// Two states, not three: the hero (level 0) and the scrolled page (level 1). The
// intermediate "the card becomes full screen" only made sense while there was a
// card; in the web app the screen is born full.
static int  level = 0;
// THE THREE PLACEHOLDERS on the hero, one per field that arrives late and has
// something to its right. 0 = the bar is showing, 1 = the real thing is. They
// crossfade IN PLACE, because the slot was already the right size. See the
// NV_DETW2_SKEL_* block in detail.h for why these three and not the others.
static float skelProv, skelSup, skelStat;
// When the current title's clock started, which NV_DETW2_SKEL_MS counts against.
static Uint32 skelSince;
// Declared here because detail_update drives the crossfades and sits above the
// drawing that defines them. They are one question asked from two places — "will
// there be something here, and is it worth holding the space" — and a second copy of
// the answer in the update would be the kind that drifts from the one in the draw.
static const char *statusWord(void);
static int pendingMeta(void);
static int pendingExtras(void);
static int holdFor(int have, int pending);

// A person's card over the title screen. It is not one more `level` because it is
// not a state of the SAME page: it is another screen, which appears and leaves whole.
static int  personIs_open;
static int  personFocus;
// The first visible ROW of the filmography. The grid has 6 per row and two rows fit
// on screen; without this the rest of the credits was cut off with no warning.
static int  personLine;
static int  reqOpen = -1;
// The focus INSIDE the "More like this" tab, which is a vertical list of its own and
// not one of focus.c's horizontal rows.
static int  relFocus;
// The season chosen in the per-episode ratings panel (an index into extras).
static int  ratTemp;
#define PES_PHOTO_W   280.0f
#define PES_PHOTO_H   420.0f
#define PES_COL_X    (NV_DETP_X + PES_PHOTO_W + 56.0f)
#define PES_CARD_W   212.0f
#define PES_CARD_H   318.0f
#define PES_CARD_GAP  32.0f
#define PES_PER_LINE  6

static int  button = 0;      // the focused button on the hero
// --- the region recorder, see detail.h --------------------------------------
#define NV_REG_MAX 10
static struct { const char *name; GfxRect r; } regions[NV_REG_MAX];
static int nRegions;
static void regionReset(void) { nRegions = 0; }
static void regionAdd(const char *name, GfxRect r) {
  if (nRegions < NV_REG_MAX) { regions[nRegions].name = name;
                               regions[nRegions].r = r; nRegions++; }
}
void detail_regions(void) {
  if (!is_open) { printf("[rect] the detail screen is not open\n"); fflush(stdout); return; }
  for (int i = 0; i < nRegions; i++) {
    GfxRect r = regions[i].r;
    printf("[rect] %-9s %d,%d %dx%d\n", regions[i].name,
           (int)(r.x + 0.5f), (int)(r.y + 0.5f), (int)(r.w + 0.5f), (int)(r.h + 0.5f));
  }
  fflush(stdout);
}
// The most buttons the row ever has: the primary plus three circles, which is what a
// FILM shows (see nButtons).
#define N_BUTTONS 4
// The tooltip's `transition: opacity 140ms` — one value per button, because moving
// the focus CROSSFADES: the label you are leaving fades out while the new one fades
// in, and a single shared opacity would make it blink to zero and back instead.
// Index 0 is the primary, which never has a tooltip, and is simply never raised.
static float tipA[N_BUTTONS];
static int  reqPlay = 0, reqMark = 0, reqSources = 0;
// Mark as WATCHED. Separate from reqMark, which is "add to the list".
static int  reqWatched = 0;
static int  reqOfStart = 0;         // the "Play from the start" button
static Uint32 okPressedAt = 0;
static float pg = 0.0f;              // 0..1: hero -> scrolled page
static Focus focus;
static float animFocus[N_SECTIONS][N_ITEMS];
static float scrollSec[N_SECTIONS];    // each row's HORIZONTAL scroll
static float scrollY = 0.0f;         // the document's VERTICAL scroll
// THE VELOCITY OF BOTH SCROLLS, because anim_spring2 keeps position AND velocity.
//
// This page scrolled on the FIRST-ORDER spring while the home rows were moved to the
// second-order one (home.c, scrollX/velX). anim.h spells out why that shape is wrong
// for a track that slides: the first-order spring leaves at MAXIMUM speed and only
// decelerates, so every press of RIGHT snapped the row to full speed from wherever it
// had got to. Holding the key — which is how anyone walks an episode list — made that
// a velocity discontinuity per repeat, and a row that lurches once per keypress is
// exactly the "jittery" the owner saw. The frame was never the problem: measured on
// the device mid-scroll, 60fps, janks=0, text=0.0ms/0.
//
// The second-order spring starts at zero velocity, accelerates, and decays with the
// tail that was measured on the reference — and it retargets mid-flight without a
// discontinuity, which is the whole point when the key is held down.
static float velSec[N_SECTIONS];
static float velY = 0.0f;
static int season = 0;            // the CHOSEN season (not the focused one)
// The season picker's dropdown. `seasonMenuOpen` is the listbox being expanded and
// `seasonMenuFocus` the row inside it — which is NOT `season`: the list opens on the
// chosen one and moving through it changes nothing until OK.
static int seasonMenuOpen = 0;
// The unfocused episode rows' opacity, NV_DETEP_DIM or NV_DETEP_REST (see update).
static float epListA = NV_DETEP_REST;
static int seasonMenuFocus = 0;
// Where the anchor landed this frame. The list is drawn in a pass of its own after
// every section, because it hangs over the episode row below it — drawn in place it
// would be painted over by the very cards it covers.
static GfxRect seasonMenuAt;
// Comments: 0 = the SERIES', 1 = the EPISODE's. It is the selector the reference
// puts under "Trakt ratings". On a film it does not exist and it stays at 0.
static int commentEp = 0;
static int tabInfo = 0;              // the chosen information tab

// The web app's four sections, with the GROUP's top in document coordinates — and
// not a stack of summed heights, which was the Apple TV app's model. The positions
// are fixed because in the web app they are too: the document has a known size and
// scrolling only changes how much of it you see.
typedef enum { SEC_SEASONS, SEC_EPISODES, SEC_TABS_INFO, SEC_CAST,
               SEC_TRAILERS, SEC_RELATED, SEC_COMMENTS,
               SEC_DETAILS } KindSection;
// Defined further down, along with the rest of the catalogue queries; declared here
// because recalcLayout, headerOf and nRateable, all above it, need to tell a series
// from a film.
static int isSeries(void);
static float heightHeaderComments(void);
static int seasonIn(int c);
// sectionN answers 1 for the season row (it is one dropdown); this is the number of
// SEASONS behind it, and sectionN itself has to ask.
static int nSeasonsOf(void);
// The catalogue keeps every season's episodes in ONE flat list. These map a
// season-relative index onto it, and they are declared here because sectionN, the event
// handler and detail_ep_focus — all above their definitions — index episodes.
static int nEpsOfSeason(int s2);
static const CatEp *epOfSeason(int s2, int i);
static int seasonNow(void);
static float baseOfTabActive(void);
// The focused episode row's grown height, which the scroll in detail_update needs to
// know the bottom of the list by; defined with the rest of the list.
static float episodeFullH(int c);
// The comments row is defined next to the drawing of it, further down, but the
// column count and the item width — which live up here — need to ask how many pills
// and how many cards it has.
#define COM_PILL_GAP  16.0f
static const char *COM_LABEL[2];
static int   nPillsCom(void);
static int   nCardsCom(void);
static float widthPillCom(const char *rot);
static int  sectionN(int r);
static float heightSection(int r);

// A SERIES — ABSOLUTE coordinates measured on the device (NV_DETP_G_*). They do not
// become flow. The comment in detail.h:70 records what happened when someone tried
// to deduce them from a sum of heights: the scrolling hit its ceiling too early and
// the cast row stopped half a screen out of place.
//
// A FILM — STACKED. A film's sections (Cast, Trailers, Details) have a height that
// depends on the content, and there is no device measurement to copy. Here each
// one's top is the sum of what came before, which is how the web app actually
// behaves: a block that does not exist takes up no height.
//
// `topSec` is the GROUP's top (the header's line). The content starts at
// `contentSec`, and it is THAT which the scrolling targets — the web app aims at the
// TRACK's top, not the group's (focusInList, metaDetailsScreen.js:7936).
static float topSec[N_SECTIONS], contentSec[N_SECTIONS], targetSec[N_SECTIONS];
// WHERE THE FOCUSED ROW PARKS THE DOCUMENT, absolute, or -1 to fall back on
// targetSec's fraction.
//
// It is what makes the episode page a page, and it exists because the fraction cannot
// express it: "put 1080 at the top of the screen" for a group whose top is 1320 is not
// a percentage of anything. A series sets it on the four groups that live on pages 2
// and 3 (see detail.h); a film leaves it at -1 throughout and keeps the 33% rule,
// because a film's sections are a stack whose heights come from the content and there
// are no pages to snap to.
static float snapSec[N_SECTIONS];
static float docEnd = NV_DETP_P3 + NV_SCREEN_H;

// A section header: only a film has one. On a series the "Seasons" label is drawn by
// the old path, and "Cast" would be repeating the "Creator and cast" tab just above
// it (see detail.c:1611).
static const char *headerOf(int r) {
  if (isSeries()) return NULL;
  switch (r) {
    case SEC_CAST:   return "Cast";
    case SEC_TRAILERS:     return "Trailers";
    case SEC_RELATED: return "Recommendations";
    // No section header: the section itself already opens with "trakt Comments" and
    // the subtitle "Trakt ratings". With both, TWO stacked titles came out saying the
    // same thing.
    case SEC_COMMENTS:  return NULL;
    case SEC_DETAILS:     return "Movie Details";
    default:           return NULL;
  }
}

// THE SEASON TAB IS A SCROLL SHORTCUT, not a filter.
//
// The owner's proposal, and better than what was there: the episode list became
// SINGLE (every season, in order), and the tab merely takes the focus to that
// season's FIRST episode. There is no reload, no network, no rebuild — the delay on
// changing tab stops existing because the change stops happening.
static void goToSeason(int c) {
  // The row now HOLDS one season, so choosing a season does not hunt for an offset
  // inside a flat list any more — it starts that season's row at its first card. It
  // used to walk every episode of every season looking for the first with a matching
  // number, which is what a single continuous track needs and this no longer is.
  (void)c;
  scrollSec[SEC_EPISODES] = 0.0f; velSec[SEC_EPISODES] = 0.0f;
  if (focus.row == SEC_EPISODES) focus.column = 0;
}

// A film with no cast yet, with the meta in flight. It is the only case where an
// empty section takes up height (see recalcLayout) and gets a skeleton.
static int castLoading(void) {
  const CatItem *ci = cat_item(idx);
  return !isSeries() && ci && ci->nCast == 0 && disc_episodes_loading(idx);
}

static void recomputeLayout(void) {
  int r;
  if (isSeries()) {
    // The first four come from an ABSOLUTE MEASUREMENT against the reference; they
    // are not a stack. The ones below (comments) are: they sit after the cast, whose
    // height is known.
    static const float G[N_SECTIONS] = {
      NV_DETP_G_TEMP, NV_DETP_G_EP, NV_DETP_G_TABS, NV_DETP_G_CAST, 0, 0
    };
    float y;
    for (r = 0; r < N_SECTIONS; r++) {
      topSec[r] = contentSec[r] = G[r];
      targetSec[r] = (r == SEC_TABS_INFO) ? NV_DETP_TARGET_TABS
                                        : NV_DETP_TARGET_ROW;
      snapSec[r] = -1.0f;
    }
    // The two pages. Both groups on a page share its top, so moving the focus from the
    // picker to a card, or from the tab strip to the cast, does not move the document
    // at all — only the focus travels, which is what makes them read as one screen
    // rather than as two rows that happen to be near each other.
    snapSec[SEC_SEASONS]   = snapSec[SEC_EPISODES] = NV_DETP_P2;
    snapSec[SEC_TABS_INFO] = snapSec[SEC_CAST]     = NV_DETP_P3;
    // A page model needs a whole page to scroll INTO: the ceiling below is
    // docEnd - NV_SCREEN_H, so a document that stopped at the cast would clamp the
    // last snap short of its own page and the tab strip would never reach the top.
    //
    // A SERIES WITH NO EPISODES (the catalogue answered with nothing, and is not still
    // loading) therefore has an empty page 2 and DOWN from Play travels 2160 to reach
    // page 3. Collapsing the pages for that case would mean the tab and cast DRAWING
    // coordinates stop being constants — NV_DETP_EL_Y feeds eight call sites through
    // baseOfTabActive — and it is not worth that to tidy up a title that is already
    // broken. While the episodes are in flight drawSkeletonEpisodes fills the page.
    docEnd = NV_DETP_P3 + NV_SCREEN_H;
    // THE TRAKT SECTION ON A SERIES: stacked below the cast, as in the reference.
    // It was the "the trakt section is missing on series" — it existed only on films.
    //
    // The cast's base comes from the SERIES' measurements, not from NV_DETF_EL_HEIGHT
    // (193), which is the FILM's cast row height — that is what I used before and why
    // the Trakt section landed ON TOP of the avatars.
    //
    // The stacking is: the row's top (EL_Y) + the avatar + the gap to the name + the
    // gap from the name to the role + the role's line. Plus ONE line of slack because
    // a long name wraps onto two ("Geneva Robertson-Dworet" in the owner's own
    // capture) and pushes the role down.
    y = baseOfTabActive() + NV_DETP_EL_GAP_TRAKT;
    topSec[SEC_COMMENTS] = contentSec[SEC_COMMENTS] = y;
    if (sectionN(SEC_COMMENTS) > 0) {
      float end = y + heightSection(SEC_COMMENTS) + NV_DETF_PAD_END;
      if (end > docEnd) docEnd = end;
    }
    return;
  }
  { float y = NV_DETF_HERO_END;
    for (r = 0; r < N_SECTIONS; r++) {
      float h;
      topSec[r] = contentSec[r] = y;
      targetSec[r] = NV_DETP_TARGET_ROW;
      snapSec[r] = -1.0f;
      // A missing section takes up no height — except the CAST while the film's meta
      // is loading: the row reserves its place and gets the skeleton, so the page does
      // not jump when the actors arrive.
      if (sectionN(r) <= 0 && !(r == SEC_CAST && castLoading())) continue;
      if (headerOf(r)) {
        contentSec[r] = y + NV_DETF_HEADER_H + NV_DETF_HEADER_GAP;
        y = contentSec[r];
      }
      h = heightSection(r);
      y += h + NV_DETF_SEC_GAP;
    }
    // The REAL end of the document, not the series' 2473: a film is much shorter and
    // copying that number would let the page scroll far past its end.
    docEnd = y - NV_DETF_SEC_GAP + NV_DETF_PAD_END;
    if (docEnd < NV_SCREEN_H) docEnd = NV_SCREEN_H; }
}

// The tabs are DYNAMIC, as in the web app: renderSeriesInsightSection
// (metaDetailsScreen.js:3751) only adds "More like this", "Trailer" and "Collection"
// when the corresponding list has items, and hides the whole bar when a single tab is
// left. The port hard-coded all four and the last three all fell into "No information
// for this tab." — which is exactly what the web app avoids by not showing the tab.
//
// Here there are two: cast (always) and ratings (when there is a score). Similar
// titles and trailers have no source in this port; when they do, they go into this table.
typedef enum { TAB_CAST, TAB_RATINGS, TAB_RELATED, TAB_COLLECTION,
               TAB_COMMENTS, TAB_NFIXAS } TabInfoId;
// THE LABELS ARE THE DEVICE'S, and not the web app's. Read off the bar of the series
// "Furious" on the TCL: "Direction and Cast | Ratings | Recommendations | Trailer".
// "Creator and cast" and "More like this" came from NuvioWeb and do not exist there.
//
// "Collection" and "Comments" stay: they are data this port HAS and that the
// reference's bar did not show on that title (it hides tabs with no content, and the
// series measured had neither a collection nor comments). Removing the two would be
// hiding what the app already knows how to show.
static const char *TAB_LABEL[TAB_NFIXAS] = {
  "Cast and Crew", "Ratings", "Recommendations", "Collection", "Comments"
};

// The open title's IMDb score, 0 when there is none.
static int scoreOf(int i) {
  const CatItem *ci = cat_item(i);
  return ci ? ci->score : 0;
}
// How many items the Ratings tab has to focus: the seasons, on a series; the score
// cards, on a film. It serves navigation only — the drawing already knows what to
// show in each case.
static int nRateable(void) {
  int i, n = 0;
  if (isSeries() && extras_n_seasons() > 0) return extras_n_seasons();
  for (i = 0; i < EX_NSOURCES; i++) {
    int v = extras_score(i);
    if (i == EX_IMDB && !v) v = scoreOf(idx);
    if (v) n++;
  }
  return n;
}

static int tabAvailable(int id) {
  switch (id) {
    case TAB_CAST:       return 1;
    // ONE of the scores is enough for the tab to be worth it; whichever card is
    // missing shows "-", which is what the web app does.
    case TAB_RATINGS:   return scoreOf(idx) > 0 || extras_score_trakt() > 0;
    case TAB_RELATED: return extras_n_related() > 0;
    case TAB_COLLECTION:      return extras_n_collection() > 1;
    // No comments tab: in the reference they are a stacked SECTION, and the tabs
    // measured on the TCL are only Direction and Cast / Ratings / Recommendations.
    // Keeping both would show the same content in two places.
    case TAB_COMMENTS:  return 0;
    default:               return 0;
  }
}
// Translates the visible position `c` into the tab's id.
static int tabIdOf(int c) {
  for (int id = 0, v = 0; id < TAB_NFIXAS; id++)
    if (tabAvailable(id) && v++ == c) return id;
  return TAB_CAST;
}
static int nTabsInfo(void) {
  int n = 0;
  for (int id = 0; id < TAB_NFIXAS; id++) if (tabAvailable(id)) n++;
  return n;
}


static int  sectionN(int r);
static int  sectionColumns(int r);
static int  seasonIn(int c);
static float widthSeason(int c);
static float widthTabInfo(int i);

// NULL when it is not known — not a fallback list. The fixed lists that used to be
// here existed so the app could run with only a loose folder of images, but the price
// was a REAL title with no name loaded appearing as "Severance" or "Silo",
// indistinguishable from real data. Whoever draws omits anything that comes back NULL.
static const char *titleOf(int i) {
  const CatItem *c = cat_item(i);
  return (c && c->title[0]) ? c->title : NULL;
}
static const char *genreOf(int i) {
  const CatItem *c = cat_item(i);
  return (c && c->genre[0]) ? c->genre : NULL;
}
// Empty when it is not known. The fallback list that used to be here stamped
// "2025 · 1 h 54 min" on a title whose metadata had not arrived — an INVENTED year
// and duration, on the hero's meta line, beside real data.
// splitMeta already handles an empty string and returns both fields empty; whoever
// draws omits each of them.
static const char *profileOf(int i) {
  const CatItem *c = cat_item(i);
  return (c && c->meta[0]) ? c->meta : "";
}
static const char *synopsisOf(int i) {
  const CatItem *c = cat_item(i);
  return (c && c->synopsis[0]) ? c->synopsis : NULL;
}
static const char *logoOf(int i) {
  const CatItem *c = cat_item(i);
  return (c && c->logo[0]) ? c->logo : NULL;
}
static const char *artOf(int i) {
  const CatItem *c = cat_item(i);
  // A detail screen can never inherit the art of another catalogue position. When the
  // title's own backdrop is missing, the renderer shows the neutral state and
  // preserves the layout, waiting for the same item to be enriched later.
  if (c && c->backdrop[0]) return c->backdrop;
  // The title's own poster is the safe fallback. The drawing treats it as contained
  // art, not as a 16:9 cover, to preserve the face, the lettering and the proportions.
  if (c && c->poster[0]) return c->poster;
  return NULL;
}

static int artDetailIsPoster(int i) {
  const CatItem *c = cat_item(i);
  return c && !c->backdrop[0] && c->poster[0];
}

static void drawArtDetail(GfxRect target, GLuint tex, const char *art,
                               int poster, float alpha, float pg, float zoom,
                               GfxRect cell) {
  if (!tex) {
    gfx_color(target, 0.0f, 0.051f, 0.051f, 0.051f, alpha);
    return;
  }
  gfx_tex_aspect_current = tex_aspect(art);
  // IT KEEPS THE VIGNETTE ALL THE WAY HOME, and that is not a compromise.
  //
  // Drawing the returning card through GFX_CARD took the scrim off, and the left
  // of the picture — which this screen's gradient holds at nearly #0d0d0d — jumped
  // to full brightness the moment the close began. The vignette IS the look: take
  // it away and the image stops being the title screen's backdrop and becomes a
  // photograph.
  //
  // Anchored to the RECT, it also does the job the missing scrim left undone. The
  // ramp reaches #0d0d0d at the card's own left edge, and the page ground behind it
  // is that same colour (the veil in detail_draw, which is why that veil stays on
  // the way out), so the edge the eye would otherwise follow is drawn in the colour
  // of the thing it sits on. The picture is pushed back down to the hero's size
  // INSIDE its own gradient rather than against the home.
  if (!poster) {
    // uPar.x carries the closing's zoom, uCell the opening's framing; see the
    // note on the mode in gfx.c. RESET after the call — the header on
    // gfx_tex_cell_current warns that a cell left set crops everything drawn next.
    gfx_tex_cell_current = cell;
    gfx_rect(target, tex, GFX_DETAIL, 1.0f - pg, zoom, 0, 0.0f, 0, 0, 0,
             alpha);
    gfx_tex_cell_current = (GfxRect){ 0.0f, 0.0f, 1.0f, 1.0f };
  } else {
    float ap = gfx_tex_aspect_current > 0.05f ? gfx_tex_aspect_current : (2.0f / 3.0f);
    float h = target.h * 0.90f, w = h * ap, maxW = target.w * 0.42f;
    if (w > maxW) { w = maxW; h = w / ap; }
    GfxRect r = { target.x + target.w - w - 72.0f,
                  target.y + (target.h - h) * 0.5f, w, h };
    gfx_rect(r, tex, GFX_HERO, 0, 0, 0, 0, 0, 0, 0, alpha);
  }
  gfx_tex_aspect_current = 0.0f;
}
static int isSeries(void) {
  const CatItem *ci = cat_item(idx);
  if (!ci) return 0;
  if (ci->kind[0]) return strcmp(ci->kind, "series") == 0;
  return cat_n_episodes(idx) > 0;
}

static float smooth(float x) {
  x = anim_clamp(x, 0.0f, 1.0f);
  return 1.0f - (1.0f - x) * (1.0f - x) * (1.0f - x);
}
static float phase2(void) { return smooth((t - 0.45f) / 0.55f); }

// The embedded Inter has only Regular, Medium and Bold, and the page asks for 500,
// 600 and 800 at sizes (32, 26, 21) that only exist in Regular in text.c's table —
// which is another agent's file in this session. Thickening by redrawing the same
// line with sub-pixel offsets is what is left, and it is what rasterisers call
// "faux bold": it costs a single texture, because the line comes from the cache.
static void txt_weight(TxtLine l, float x, float y, float a, float thickness) {
  txt_draw_alpha(l, x, y, a);
  if (thickness > 0.05f) txt_draw_alpha(l, x + thickness * 0.5f, y, a);
  if (thickness > 0.9f)  txt_draw_alpha(l, x + thickness, y, a);
}

void detail_open(const HomeItem *it, int shared) {
  mark("detail_open");
  item = *it;
  sharedOrigin = shared;
  is_open = 1; exiting = 0; level = 0; button = 0;
  t = 0.0f; velT = 0.0f; pg = 0.0f; scrollY = 0.0f; velY = 0.0f; tabInfo = 0; personIs_open = 0;
  relFocus = 0; reqOpen = -1; ratTemp = 0;
  // Every held slot starts closed for the new title, and the clock on them starts
  // HERE rather than at the first draw — a title opened from another title would
  // otherwise inherit a wait the previous one had already served.
  skelProv = skelSup = skelStat = 0.0f;
  skelSince = SDL_GetTicks();
  idx = it->index_;
  // The Trakt score, comments and related titles. Requested on OPENING and not while
  // drawing: the tabs only appear once the data arrives, and requesting while drawing
  // would make the tab bar appear with the title already on screen.
  { const CatItem *ci = cat_item(idx);
    if (ci && ci->imdb[0]) extras_request(ci->imdb, isSeries(), ci->tmdb); }
  // The marked tab has to be the season the episodes carry. Always starting at 0, a
  // series whose first loaded episode is from season 4 opened with "Season 1" lit —
  // the label contradicted the list just below it.
  season = 0;
  { const CatEp *e0 = epOfSeason(seasonNow(), 0);
    const CatItem *ci0 = cat_item(idx);
    if (e0 && ci0) {
      int k;
      for (k = 0; k < ci0->nSeasons; k++)
        if (ci0->seasons[k] == e0->season) { season = k; break; }
    } }
  int cols[N_SECTIONS]; for (int i = 0; i < N_SECTIONS; i++) cols[i] = sectionColumns(i);
  focus_start(&focus, N_SECTIONS, cols);
  memset(animFocus, 0, sizeof animFocus);
  memset(tipA, 0, sizeof tipA);
  memset(scrollSec, 0, sizeof scrollSec);
  // The velocity goes with it: a position reset that leaves the spring still moving
  // carries the old row's momentum into the new one.
  memset(velSec, 0, sizeof velSec);
}

int detail_is_open(void) { return is_open; }
int detail_shared_origin(void) { return is_open && sharedOrigin; }
int detail_is_exiting(void) { return is_open && exiting; }

// 0..1 of how much the detail has already taken over the screen. The home reads this
// to push the rows DOWN as it comes in: it is the movement the owner describes as
// "only the posters go down". It lives here and not in a shared variable because the
// spring that produces it is the drawing's own — two different clocks would drift.
// REMAPPED ON THE WAY OUT so it reaches 0 exactly where the screen is let go of,
// and not at a value the home then has to snap away from.
//
// The home multiplies this by 8% of the screen to slide its rows down, and by the
// hero copy's alpha. detail_update releases at NV_DETAIL_EXIT_CUT, so with the raw
// curve the last frame handed over 0.06 — 5px of row offset and 6% of brightness
// that vanished in one frame, on every row at once. That step is what the owner
// saw as a flicker at the end of the close. It was harmless while the cut sat at
// 0.006 (half a pixel) and stopped being harmless when the cut moved up to where
// the backdrop's fade actually ends.
//
// Opening is untouched: `exiting` only becomes 1 once the screen is on its way
// out, and at that moment t is ~1, where the remap is the identity anyway.
float detail_progress(void) {
  if (!is_open) return 0.0f;
  if (!exiting) return t;
  return anim_clamp((t - NV_DETAIL_EXIT_CUT) / (1.0f - NV_DETAIL_EXIT_CUT),
                    0.0f, 1.0f);
}

// The season and episode IN FOCUS, for whoever is going to ask for a source.
//
// Without this, addons_fetch received only the series' imdb and hard-coded ":1:1" —
// and that is why the sources were always episode 1's, whichever one was chosen.
// It returns 0 when the focus is not on the episode row; in that case the caller
// falls back to the first episode of the season on display, which is what the screen
// shows at the top.
// WHERE THE OWNER STOPPED, or where they should start.
//
// Three sources, in this order:
//   1. the episode IN FOCUS, when it is on the episode row — there the choice is
//      explicit and beats any history;
//   2. the episode IN PROGRESS (Trakt's progress in the CatItem itself), which is
//      the "Resume";
//   3. the FIRST UNWATCHED, sweeping the seasons in order with the
//      /shows/<id>/progress/watched that extras.c already reads — the "Next".
//
// The comment that used to be here said the native catalogue "does not store which
// episodes have been watched". It does not, but extras_ep_watched() has known since
// the per-episode ratings panel was built — the claim went stale and the button went
// on pointing at S1E1 on a series already started, which is what the owner reported.
//
// `origin` (optional) returns 1 = focus, 2 = resume, 3 = next, 0 = first.
static int episodeTarget(int *temp, int *eps, int *origin) {
  const CatItem *ci = cat_item(idx);
  const CatEp *ep = NULL;
  if (origin) *origin = 0;

  if (focus.row == SEC_EPISODES) ep = epOfSeason(seasonNow(), focus.column);
  if (ep) {
    if (temp) *temp = ep->season;
    if (eps) *eps = ep->episode;
    if (origin) *origin = 1;
    return 1;
  }
  // In progress: the "Continue watching" item carries the season and episode.
  //
  // AND SO DOES A NEXT-UP ONE, at 0% — which is why the lower bound is gone. The
  // row now offers the episode that FOLLOWS the one that finished, and the card
  // names it ("S12 E15"); with `progress > 0` required here, opening that card
  // fell through to extras_next_episode below and played whatever Trakt's map
  // said was next, which is a different question with a different answer
  // whenever Trakt has not answered yet or the person watched out of order. A
  // card that names an episode has to play that episode.
  //
  // A season and an episode on the item is the discriminator: an untouched
  // catalogue item carries 0/0, because the only writer of these fields is the
  // progress record.
  if (ci && ci->progress < 90 && ci->season > 0 && ci->episode > 0 &&
      !extras_ep_watched(ci->season, ci->episode)) {
    if (temp) *temp = ci->season;
    if (eps) *eps = ci->episode;
    if (origin) *origin = 2;
    return 1;
  }
  // The first unwatched. It only counts once Trakt has answered; with no data at all
  // extras_n_seasons() is 0 and it falls back to the first episode, as before.
  { int t, e;
    if (extras_next_episode(&t, &e)) {
      if (temp) *temp = t;
      if (eps) *eps = e;
      if (origin) *origin = 3;
      return 1;
    }
  }
  { int t, i, nt = extras_progress_ready() ? extras_n_seasons() : 0;
    for (t = 0; t < nt; t++) {
      int tn = extras_season_number(t), ne = extras_n_eps(t);
      for (i = 0; i < ne; i++) {
        int en = extras_ep_number(t, i);
        if (en > 0 && !extras_ep_watched(tn, en)) {
          if (temp) *temp = tn;
          if (eps) *eps = en;
          if (origin) *origin = 3;
          return 1;
        }
      }
    } }
  ep = epOfSeason(seasonNow(), 0);
  if (!ep) return 0;
  if (temp) *temp = ep->season;
  if (eps) *eps = ep->episode;
  return 1;
}

int detail_ep_focus(int *temp, int *eps) {
  if (!is_open) return 0;
  return episodeTarget(temp, eps, NULL);
}

int detail_settled(void) {
  return is_open && !exiting && t > 0.985f && level == 0;
}

// The backdrop's rectangle THIS FRAME and the opacity it comes in with. One piece of
// arithmetic, used by detail_covers_screen and by detail_draw — if the two diverged,
// the home would disappear one frame before the art covers it and the screen would flash.
// The flight's geometry and the two ramps that go with it. The two DIRECTIONS are
// deliberately not the same shape, and that is the conclusion of trying every
// version where they were.
//
// OPENING: THE RECT FLIES, born exactly on the home hero's rect and growing until
// it is the screen. The background does not SWAP, it CONTINUES — the hero was
// already showing this title's art — and with a full-screen hero the two rects are
// the same, so the eye sees only the text rearranging. This is the movement to
// keep; everything below exists so as not to spoil it.
//
// CLOSING: THE RECT DOES NOT FLY. It stays full-bleed and dissolves. A rectangle
// shrinking back across the home puts four travelling edges on screen, and outside
// them is the home's rows on flat #0d0d0d (drawBackground is a no-op) while inside
// is a bright picture — that contrast is a frame, and the eye follows the shape.
// Three ways of hiding it were built and rejected: an alpha feather (alpha reveals
// the rows behind, so the boundary stayed), a perimeter ramp to the page colour
// with the ground brought down to meet it, and removing the flight from BOTH ends
// (which hid the edges by deleting the transition). What is left is the honest
// answer: do not put edges on screen on the way out. The picture pulls back
// through the zoom instead, inside a frame that never appears.
static void backdropRect(GfxRect *r, float *opacity, float *zoom, GfxRect *cell) {
  float s = t;
  GfxRect de;
  home_hero_rect(&de.x, &de.y, &de.w, &de.h);
  // No origin, no flight: from the search, Discover, a grid or the context menu
  // there is no hero rect on screen, so the backdrop is full-bleed from the first
  // frame and simply fades up, and fades straight back down again on the way out.
  //
  // WITH an origin it flies BOTH WAYS. The first attempt at a returning flight was
  // abandoned and the reasons were all fixable, which is why it is back:
  //
  //   - it carried GFX_DETAIL's vignette, anchored to the rect, so a hard black
  //     edge travelled down the left of a shrinking rectangle. It now goes home as
  //     a plain CARD, with no scrim baked in;
  //   - it went semi-transparent early, so that scrim was seen over the home as a
  //     dark square. It is opaque until it is nearly back on the hero;
  //   - its corners were square, which reads as a crop rather than an object. They
  //     round as it shrinks;
  //   - the home was veiled to black behind it, so the rectangle was a bright shape
  //     on a dark field instead of a card settling onto a screen that is already
  //     there. The veil is gone on the way out.
  GfxRect flight;
  if (!sharedOrigin) {
    flight = (GfxRect){ 0.0f, 0.0f, NV_SCREEN_W, NV_SCREEN_H };
  } else {
    flight.x = de.x + (0.0f - de.x) * s;
    flight.y = de.y + (0.0f - de.y) * s;
    flight.w = de.w + (NV_SCREEN_W - de.w) * s;
    flight.h = de.h + (NV_SCREEN_H - de.h) * s;
  }

  // DRAWN AT THE RECT, both ways, with the identity cell.
  //
  // The opening was briefly drawn as a viewport-sized quad with the flight carried
  // in uCell, so the uncovered L could be filled by the texture rather than left
  // bare — first by clamping the edge texel across it, then by folding. Both
  // failed for the same reason, and it was not the filling: THE HOME HAS ITS OWN
  // EDGE THERE. On Top band the hero is 80% of the screen anchored top-right, and
  // that band's boundary is stationary — it never grows out, it is simply covered
  // when the backdrop turns opaque. Whatever the backdrop puts in the L, a hard
  // line sits behind it in the layer underneath.
  //
  // So the fix is in home.c: the hero's art now grows on this same curve, and the
  // two rects agree at every frame. The only edge in the frame is the one the home
  // already has at rest, and it leaves the screen instead of being covered.
  *r = flight;
  if (cell) *cell = (GfxRect){ 0.0f, 0.0f, 1.0f, 1.0f };

  // OPENING it comes up FAST (front-loaded, opaque at s = 0.126, ~50 ms): the art
  // underneath is the same picture, so the ramp only swaps the hero's gradient for
  // the detail's, and fading one copy in over the other dips the brightness in the
  // middle.
  //
  // CLOSING it stays OPAQUE for the whole flight and dissolves only at the end,
  // where the card is already sitting on the hero's rect drawing the same
  // photograph the home has there. The two are aligned by then, so the dissolve is
  // a gradient swap and not a double image — the same argument as the opening,
  // run the other way. Clear at CUT, which is where detail_update lets go.
  *opacity = exiting
    ? anim_clamp((s - NV_DETAIL_EXIT_CUT) / NV_DETAIL_EXIT_FADE, 0.0f, 1.0f)
    : anim_clamp(smooth(s) * 3.0f, 0.0f, 1.0f);

  // THE ZOOM is the closing's movement, and only the closing's — opening, the rect
  // is doing the moving and this stays at 1.0. Its size is not chosen here: the
  // home's hero cover-crops this art into ITS rect, and a wider rect crops harder,
  // so the banded hero (1421x670, aspect 2.12) shows a 16:9 backdrop magnified by
  // 2.12/1.78 = 1.19 against the full screen's 1.0. Pulling back to that ratio is
  // the same content movement the flight performs in the other direction, which is
  // why the two ends read as one gesture despite being built differently. A
  // full-screen hero gives 1.0 and nothing moves, which is right for that layout.
  if (zoom) {
    float ah = de.h > 1.0f ? de.w / de.h : (NV_SCREEN_W / NV_SCREEN_H);
    float z0 = anim_clamp(ah / (NV_SCREEN_W / NV_SCREEN_H), 1.0f, 1.25f);
    // Only where there is no rect to move: with an origin the flight IS the
    // movement, and zooming the picture as well would be two of them.
    *zoom = (exiting && !sharedOrigin) ? 1.0f + (z0 - 1.0f) * (1.0f - s) : 1.0f;
  }

}

int detail_covers_screen(void) {
  // The backdrop is FULL-BLEED: as soon as it has finished growing, not a pixel of
  // the previous screen is left. Drawing the home underneath cost a whole frame of
  // fill for nothing — measured at 42 ms on the worst frame.
  //
  // THE CONDITION WAS `smooth(t) > 0.995`, which is only true at t > 0.83: the home
  // was still being drawn at 83% of the opening, and that window is where the jank
  // measured on the device was (clr=38.3ms with the CPU idle — the GPU drowned in
  // fill, home + flat background + backdrop, three full-screen layers or more).
  //
  // Now the question is the right one: has the backdrop's rectangle already reached
  // all four edges AND is it already opaque?
  //
  // Now the question is the right one: has the backdrop's rectangle already reached
  // all four edges AND is it already opaque? With the hero full screen it is born
  // practically screen-sized, so the answer arrives at t ~ 0.13. When the origin
  // does NOT cover (a banded hero, or the detail opened from the search) the
  // arithmetic answers `no` and the home is still drawn: which is why this is a
  // coverage measurement and not a threshold on `t`. CLOSING, the rect is
  // full-bleed from the start, so what answers is the dissolve alone.
  if (!is_open) return 0;
  { GfxRect r; float opacity;
    backdropRect(&r, &opacity, NULL, NULL);
    // THE HOME IS NOT SKIPPED ON THE WAY OUT, even though the page's ground is
    // solid over it and it cannot be seen. Skipping it was added here as free fill
    // and was not free: home_draw is what ASKS for the hero's art every frame, and
    // with the detail holding a 1920 texture of its own the cache drops an
    // unrequested one inside the ~200ms the ground is opaque. The home then comes
    // back with nothing decoded and paints its placeholder for a frame or two —
    // the black flash in the backdrop at the end of the close.
    //
    // The opening does not have the problem: there the home is on its way out and
    // a frame of placeholder behind an arriving screen is never seen. Here it is
    // the thing being arrived AT.
    if (opacity < 0.999f) return 0;
    return r.x <= 0.5f && r.y <= 0.5f &&
           r.x + r.w >= NV_SCREEN_W - 0.5f && r.y + r.h >= NV_SCREEN_H - 0.5f;
  }
}

// --- the "Movie Details" table ----------------------------------------------
//
// One row per field WITH A VALUE. An empty field does not become a row with a dash:
// it disappears. That is the same rule drawRatings already uses for a source with no
// score, and it is what stops the table becoming a half-filled form when TMDB does
// not have the data.
typedef struct { const char *key; char value[168]; } LineDet;

// "111" -> "1h 51m"; "47" -> "47min". TMDB sends raw minutes.
static void durationText(int min, char *dst, size_t size) {
  if (min <= 0) { dst[0] = 0; return; }
  if (min < 60) { snprintf(dst, size, "%dmin", min); return; }
  if (min % 60) snprintf(dst, size, "%dh %dmin", min / 60, min % 60);
  else          snprintf(dst, size, "%dh", min / 60);
}

static int buildDetails(LineDet *o, int max) {
  int n = 0;
  const CatItem *ci = cat_item(idx);
  const char *v;

  #define DET_PLACE(K, S) do {                                   \
    if ((n) < (max) && (S) && (S)[0]) {                        \
      o[n].key = (K);                                        \
      snprintf(o[n].value, sizeof o[n].value, "%s", (S));      \
      n++;                                                     \
    } } while (0)

  DET_PLACE("Status", extras_profile_status());
  { char dt[48]; disc_date_long(extras_profile_release(), dt, sizeof dt);
    DET_PLACE("Release", dt); }
  { char d[32]; durationText(extras_profile_duration(), d, sizeof d);
    DET_PLACE("Runtime", d); }
  // The rating: TMDB's fact sheet is the good one. The catalogue's serves as a
  // fallback, and since the hard-coded "14" left discover.c it only has any value
  // when it came from the catalogue file, which is real data.
  v = extras_profile_age_rating();
  if (!v || !v[0]) v = (ci && ci->age_rating[0]) ? ci->age_rating : NULL;
  DET_PLACE("Rating", v);
  // Country: TMDB's full list where there is one; otherwise the single one Cinemeta gives.
  v = extras_profile_countries();
  if (!v || !v[0]) v = (ci && ci->country[0]) ? ci->country : NULL;
  DET_PLACE("Country of Origin", v);
  DET_PLACE("Directing", (ci && ci->directing[0]) ? ci->directing : NULL);

  #undef DET_PLACE
  return n;
}

static int nLinesDetail(void) {
  LineDet l[NV_DETF_DET_MAXL];
  return buildDetails(l, NV_DETF_DET_MAXL);
}

// The height of a section's CONTENT (without the header). It serves the film's
// stacking and the culling. Each of these numbers used to live hard-coded in the
// middle of the drawing, and a new section silently inherited the cast's height.
static float heightSection(int r) {
  switch (r) {
    case SEC_SEASONS: return NV_DETWEB_SEA_H;
    // The list's WINDOW, not its rows: they scroll inside it.
    case SEC_EPISODES:  return NV_DETEP_LIST_H;
    case SEC_TABS_INFO:  return NV_DETP_TAB_H;
    case SEC_CAST:     return NV_DETF_EL_HEIGHT;
    case SEC_TRAILERS:     return NV_DETF_TR_HEIGHT;
    case SEC_RELATED: return 318.0f + 46.0f;   // poster + title/year
    // + the header: without it the next section ("Movie Details") was stacked using
    // only the cards' height and came out ON TOP of them.
    case SEC_COMMENTS:  return heightHeaderComments() + COM_CARD_H;
    case SEC_DETAILS:     return nLinesDetail() * NV_DETF_DET_LINE;
  }
  return 0.0f;
}

static int sectionN(int r) {
  const CatItem *ci = cat_item(idx);
  switch (r) {
    // ONE COLUMN, because the row is now a single DROPDOWN and not a chip per season.
    // The seasons themselves are counted by nSeasonsOf(); this is what the D-pad walks,
    // and with the old per-season count RIGHT from the picker stepped onto invisible
    // siblings before it reached the episode row.
    case SEC_SEASONS:
      // A film has no seasons: the row DISAPPEARS instead of a control that leads
      // nowhere. It is what the web app does — `.series-season-row` only exists in
      // the series layout.
      if (!isSeries()) return 0;
      return nSeasonsOf() > 0 ? 1 : 0;
    // ONE SEASON'S episodes, not every season's. See nEpsOfSeason.
    case SEC_EPISODES: {
      int q = nEpsOfSeason(seasonNow());
      if (q <= 0) return 0;
      return q < N_ITEMS ? q : N_ITEMS;
    }
    // A single tab = the bar hidden, like the web app's `tabItems.length > 1`.
    // A FILM HAS NO TABS: the film page stacks the sections with headers of their own,
    // so the tab bar does not come in. Without this guard a film ended up with both at
    // once — the bar AND the headers.
    case SEC_TABS_INFO: {
      int n;
      if (!isSeries()) return 0;
      n = nTabsInfo();
      return n > 1 ? n : 0;
    }
    // THE BOTTOM ROW IS THE CHOSEN TAB, not "the cast". This slot draws the cast,
    // "More like this" posters, score cards, the collection or the comments —
    // drawSection swaps the content in place. If the count stayed the cast's alone,
    // choosing another tab left the row with the wrong number of columns, and a guard
    // in the event handler BLOCKED going down to it entirely: you could move on the
    // tabs and nothing else.
    //
    // With no cast the section does not exist — there is no fallback. The `N_CAST`
    // that used to be here as a default filled the row with six demo names even on a
    // title the app does not know who stars in.
    case SEC_CAST: {
      int n;
      switch (tabIdOf(tabInfo)) {
        case TAB_RATINGS:   n = nRateable();           break;
        case TAB_RELATED: n = extras_n_related(); break;
        case TAB_COLLECTION:      n = extras_n_collection();      break;
        // A comment card is not chosen one by one; what TAKES focus is the two pills
        // of the "Series | Episode" selector. On a film there is no episode: a single
        // column is left, so the focus can land on the row and the page can scroll to
        // the cards.
        case TAB_COMMENTS:  n = isSeries() ? 2 : 1; break;
        default:               n = (ci && ci->nCast > 0) ? ci->nCast : 0;
      }
      return n < NV_DETF_EL_MAX ? n : NV_DETF_EL_MAX;
    }
    // Trailers, Recommendations, Comments and Details only exist on a FILM — on a
    // series the same content lives behind the TABS.
    //
    // These last two were precisely what was lost when the tabs were taken off the
    // film: the data was always there (the log shows "comments=8 rel=12"), but with
    // no tab and no section there was no way to reach it.
    case SEC_TRAILERS:
      if (isSeries()) return 0;
      return extras_n_trailers();
    case SEC_RELATED: {
      int n;
      if (isSeries()) return 0;
      n = extras_n_related();
      return n < N_ITEMS ? n : N_ITEMS;
    }
    // A comment is not chosen one by one: ONE column, just so the focus can land and
    // the page can scroll to the cards.
    // COMMENTS EXIST ON BOTH. In the reference the Trakt section sits STACKED below
    // the cast row on a series too — it is not a tab. Here it existed only on a film,
    // and on a series it lived behind a tab the reference does not have; the owner saw
    // that as "the trakt section is missing on series".
    //
    // Columns: the two pills of the "Series | Episode" selector on a series; on a film
    // there is no episode, so a single column is left for the focus to land on.
    case SEC_COMMENTS: {
      int nc = nCardsCom();
      if (nc <= 0 && extras_n_comments() <= 0) return 0;
      return nPillsCom() + nc;
    }
    // The table is ONE focusable column, not one per row: the D-pad goes down to it,
    // it scrolls into view and that is that. Zero columns would make focus_move SKIP
    // IT (focus.c:25) and the section would become unreachable — and therefore
    // unscrollable too.
    case SEC_DETAILS:
      if (isSeries()) return 0;
      return nLinesDetail() > 0 ? 1 : 0;
  }
  return 0;
}

// The primary button is a SINGLE one, and it CHANGES ITS LABEL according to the
// state: "Play" when it has never been opened, "Resume SxEy" when there is progress.
//
// There used to be a second button ("Play from the start") that appeared alongside
// the resume line. It went by the owner's decision: "when it's already started don't
// use another button to resume, use the same play button, just change it". It is what
// the reference shows too — a primary plus THREE circular buttons (+, already
// watched, trailer), with no second text button.
// HOW MANY CIRCULAR ONES, and the answer depends on the type. MEASURED on the two
// captures from the device: the FILM ("Ma") has three — plus, the "already watched"
// eye and the trailer — and the SERIES ("Lioness") has TWO, without the eye. It makes
// sense and is not an oversight in the reference: "watched" on a series is per
// episode, and the episode list just below already marks that one by one; an eye on
// the hero would have to mean "the whole series", which is not something Trakt stores
// per title.
//
// This file drew THREE in both cases.
static int nButtons(void) { return isSeries() ? 3 : 4; }

// Which ACTION sits at position `n` in the row. The actions have fixed numbers (0
// primary, 1 list, 2 watched, 3 sources) because detail_event decides by them; what
// changes with the type is which positions exist. Without this translation, on a
// series the second circular button (which is the sources one) would fire "mark as
// watched".
enum { ACTION_PRIMARY = 0, ACTION_LIST = 1, ACTION_WATCHED = 2, ACTION_SOURCES = 3 };
static int actionIn(int n) {
  if (n >= 2 && isSeries()) return n + 1;   // a series skips the eye
  return n;
}

void detail_event(const SDL_Event *e) {
  if (exiting) return;

  // THE PERSON'S CARD eats the events while it is open. It is another screen and not a
  // section of this one: letting the title screen carry on responding underneath would
  // make the arrow move two things at once.
  //
  // The `return` at the end of this block is what makes that hold. It already existed,
  // but the brace that opened it enclosed the two blocks below TOO — the arrows in
  // "Ratings" and the navigation/OK of "More like this" and "Collection" sat inside
  // `if (personIs_open)` requiring `!personIs_open`, that is, they never ran. That is
  // why you could neither move nor open anything in the recommendations: the code was
  // written and was unreachable.
  // THE SEASON LIST eats the events while it is expanded, the same way the person's
  // card does. It is a listbox over the page: letting the arrows reach the page
  // underneath would scroll the document behind an open menu.
  if (seasonMenuOpen) {
    if (e->type != SDL_KEYDOWN) return;
    { int n = nSeasonsOf();
      switch (e->key.keysym.sym) {
        case SDLK_UP:   if (seasonMenuFocus > 0) seasonMenuFocus--; return;
        case SDLK_DOWN: if (seasonMenuFocus + 1 < n) seasonMenuFocus++; return;
        case SDLK_ESCAPE:
        case SDLK_AC_BACK:
        case SDLK_BACKSPACE:
        case SDLK_DELETE:
          // Closing WITHOUT choosing leaves `season` alone. That is the point of
          // keeping the menu's focus separate from it.
          seasonMenuOpen = 0; return;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
          seasonMenuOpen = 0;
          if (seasonMenuFocus != season) {
            season = seasonMenuFocus;
            goToSeason(season);
          }
          return;
        default: return;
      } }
  }

  if (personIs_open) {
    if (e->type != SDL_KEYDOWN) return;
    { int n = person_n_credits();
      switch (e->key.keysym.sym) {
        case SDLK_LEFT:  if (personFocus > 0) personFocus--; return;
        case SDLK_RIGHT: if (personFocus + 1 < n) personFocus++; return;
        case SDLK_UP:
          if (personFocus >= PES_PER_LINE) personFocus -= PES_PER_LINE;
          if (personFocus / PES_PER_LINE < personLine) personLine--;
          return;
        case SDLK_DOWN:
          if (personFocus + PES_PER_LINE < n) personFocus += PES_PER_LINE;
          // The grid SCROLLS when the focus passes the second visible row. Two rows fit
          // on screen; the third onwards comes in pushing.
          if (personFocus / PES_PER_LINE > personLine + 1) personLine++;
          return;
        case SDLK_AC_BACK: personIs_open = 0; return;
        case SDLK_RETURN:
        case SDLK_KP_ENTER: {
          // Opens the title, when it is one the catalogue already has meta for.
          // What actually swaps is the router (app.c) — only the request leaves here.
          //
          // A credit that is NOT in the catalogue opens nothing, on purpose: with no
          // meta there are no episodes, no cast and no source, and an empty detail
          // screen is worse than the button not responding. Fetching meta on demand is
          // separate work.
          const char *id = person_credit_imdb(personFocus);
          int target = id[0] ? cat_index_by_imdb(id) : -1;
          if (target >= 0) { reqOpen = target; personIs_open = 0; }
          // Not in the catalogue: it fetches the meta and opens when it arrives. What
          // finishes the job is the router, which already follows the result.
          // The credit almost never carries an imdb_id, so the normal route is through
          // the TMDB id.
          else if (id[0]) { disc_request_title(id); personIs_open = 0; }
          else if (person_credit_tmdb(personFocus) > 0) {
            disc_request_title_tmdb(person_credit_tmdb(personFocus),
                                   person_credit_kind(personFocus));
            personIs_open = 0;
          }
          return; }
        default: break;
      } }
    if (e->key.keysym.scancode == NV_SCANCODE_BACK) personIs_open = 0;
    return;
  }
    // The "More like this" tab is a VERTICAL LIST inside the cast row.
  // While it is open, up/down move within it instead of changing row — it is the same
  // as the web app does, where the list has focus of its own.
  // In the per-episode ratings panel, left/right change SEASON.
  if (e->type == SDL_KEYDOWN && focus.row == SEC_CAST && !personIs_open &&
      tabIdOf(tabInfo) == TAB_RATINGS && isSeries() &&
      extras_n_seasons() > 0) {
    int nt = extras_n_seasons();
    if (e->key.keysym.sym == SDLK_RIGHT && ratTemp + 1 < nt) { ratTemp++; return; }
    if (e->key.keysym.sym == SDLK_LEFT  && ratTemp > 0)      { ratTemp--; return; }
  }

  // "More like this" and "Collection" are the SAME vertical list, only the source differs.
  if (e->type == SDL_KEYDOWN && focus.row == SEC_CAST && !personIs_open &&
      (tabIdOf(tabInfo) == TAB_RELATED || tabIdOf(tabInfo) == TAB_COLLECTION)) {
    int col = (tabIdOf(tabInfo) == TAB_COLLECTION);
    int n = col ? extras_n_collection() : extras_n_related();
    if (n > 7) n = 7;
    switch (e->key.keysym.sym) {
      // "More like this" is a row of posters: it moves HORIZONTALLY. The collection
      // stays a vertical list.
      case SDLK_RIGHT: if (!col && relFocus + 1 < n) { relFocus++; return; } break;
      case SDLK_LEFT:  if (!col && relFocus > 0)     { relFocus--; return; } break;
      case SDLK_DOWN: if (col && relFocus + 1 < n) { relFocus++; return; } break;
      case SDLK_UP:   if (col && relFocus > 0)     { relFocus--; return; } break;
      case SDLK_RETURN:
      case SDLK_KP_ENTER: {
        if (col) {
          // The collection's part carries only the TMDB id; the route is the same as
          // an actor's credit.
          long t = extras_collection_tmdb(relFocus);
          if (t > 0) disc_request_title_tmdb(t, "movie");
        } else {
          const char *id = extras_related_imdb(relFocus);
          int target = cat_index_by_imdb(id);
          if (target >= 0) reqOpen = target;
          else if (id[0]) disc_request_title(id);
        }
        return; }
      default: break;
    }
    // UP on the first item and DOWN on the last fall through to the normal behaviour
    // and leave the list — otherwise the focus is trapped in it.
  }



  if (e->type == SDL_KEYDOWN && (e->key.keysym.sym == SDLK_RETURN ||
                                 e->key.keysym.sym == SDLK_KP_ENTER)) {
    if (!okPressedAt) okPressedAt = SDL_GetTicks();
    return;
  }
  if (e->type == SDL_KEYUP && (e->key.keysym.sym == SDLK_RETURN ||
                               e->key.keysym.sym == SDLK_KP_ENTER)) {
    Uint32 duration;
    // RELEASING without having PRESSED is not a click. Without this guard the detail
    // screen played by itself on being opened: the OK pressed on the home delivers the
    // KEYDOWN to the home (which opens the detail) and the KEYUP ARRIVES HERE, with
    // level 0 and button 0 — which is exactly "Play". You can see it as the owner
    // described: "you click a title and it clicks twice and starts".
    //
    // Before, this did not show because the button lived on level 1 and the orphan
    // KEYUP fell into no case. Moving the buttons to level 0 (which is where the web
    // app puts them) uncovered a defect that already existed.
    if (!okPressedAt) return;
    duration = SDL_GetTicks() - okPressedAt;
    okPressedAt = 0;
    if (level == 0) {
      // A FIXED order: primary, add to the list, mark as watched, sources.
      //
      // The eye button fell into the `else` and opened the SOURCES sheet — it never
      // marked anything, despite the icon. Now it has a request of its own.
      int action = actionIn(button);
      if (action == ACTION_PRIMARY) {
        if (duration >= NV_HOLD_MS) reqSources = 1; else reqPlay = 1;
      } else if (action == ACTION_LIST) {
        reqMark = 1;
      } else if (action == ACTION_WATCHED) {
        reqWatched = 1;
      } else {
        reqSources = 1;
      }
    } else if (focus.row == SEC_RELATED) {
      // A FILM: "More like this" is a section of its own. The same destination as the
      // series path — it opens from the catalogue when we already have meta, otherwise
      // it asks and the router finishes when it arrives.
      const char *id = extras_related_imdb(focus.column);
      int target = id[0] ? cat_index_by_imdb(id) : -1;
      if (target >= 0) reqOpen = target;
      else if (id[0]) disc_request_title(id);
    } else if (focus.row == SEC_SEASONS) {
      // OK EXPANDS THE LIST; it no longer switches season on its own, because there is
      // no longer one control per season to switch to. The list opens with its focus on
      // the season already chosen, which is how the web shows you where you are.
      seasonMenuOpen = 1;
      seasonMenuFocus = season;
    } else if (focus.row == SEC_CAST && tabIdOf(tabInfo) == TAB_CAST) {
      // OK on a face opens the person's FILMOGRAPHY. It is the web app's
      // `openCastDetail` (metaDetailsScreen.js:6165); here OK on the cast did nothing.
      const CatItem *ci = cat_item(idx);
      if (ci && focus.column < ci->nCast && ci->cast[focus.column].tmdb > 0) {
        person_request(ci->cast[focus.column].tmdb,
                     ci->cast[focus.column].name,
                     ci->cast[focus.column].photo);
        personIs_open = 1;
        personFocus = 0;
        personLine = 0;
      }
    } else if (focus.row == SEC_TABS_INFO) {
      tabInfo = focus.column;
    } else if (focus.row == SEC_EPISODES) {
      // The focused row carries a Resume / Play pill, so OK does what it says: it plays
      // THIS episode (episodeTarget answers the focused row first). Held, it opens the
      // sources sheet — the same split as the hero's primary button.
      if (duration >= NV_HOLD_MS) reqSources = 1; else reqPlay = 1;
    }
    return;
  }

  if (e->type != SDL_KEYDOWN) return;
  SDL_Keycode k = e->key.keysym.sym;

  if (k == SDLK_ESCAPE || k == SDLK_AC_BACK || k == SDLK_BACKSPACE ||
      k == SDLK_DELETE) {
    if (level > 0) level = 0; else exiting = 1;
    return;
  }
  if (level == 0) {
    if (k == SDLK_DOWN) {
      // Going down from the hero lands on the first FOCUSABLE row. On a film there are
      // no seasons and no episodes, and stopping on an empty row left the D-pad
      // unresponsive. It has to be sectionColumns and not sectionN: the trailers are
      // DRAWN but take no focus, and a film with no cast would land on them.
      for (int r = 0; r < N_SECTIONS; r++)
        if (sectionColumns(r) > 0) { focus.row = r; focus.column = 0; level = 1; break; }
    }
    else if (k == SDLK_RIGHT) { if (button < nButtons() - 1) button++; }
    else if (k == SDLK_LEFT)  { if (button > 0) button--; }
    return;
  }
  // The guard that used to be here blocked GOING DOWN from the tabs whenever the
  // chosen tab was not "Creator and cast" — and with it locked access to "More like
  // this", "Collection" and "Comments", whose navigation code was already written just
  // above and was never reached.
  //
  // It is no longer needed: sectionN returns the count OF THE ACTIVE TAB, so the row
  // either has real columns (and the focus lands on what is drawn) or has zero, and
  // focus_move skips it by itself.
  // THE EPISODE LIST IS VERTICAL: its "columns" are rows, so UP and DOWN walk it and
  // only leave it past either end — UP from the first row to the picker, DOWN from the
  // last to the tabs. LEFT and RIGHT have nowhere to go.
  if (focus.row == SEC_EPISODES) {
    if (k == SDLK_DOWN && focus.column + 1 < focus.nColumns[SEC_EPISODES]) {
      focus.column++; return;
    }
    if (k == SDLK_UP && focus.column > 0) { focus.column--; return; }
    if (k == SDLK_LEFT || k == SDLK_RIGHT) return;
  }
  if (k == SDLK_RIGHT)      focus_move(&focus, 1, 0);
  else if (k == SDLK_LEFT)  focus_move(&focus, -1, 0);
  else if (k == SDLK_DOWN)  focus_move(&focus, 0, 1);
  else if (k == SDLK_UP)    { if (!focus_move(&focus, 0, -1)) level = 0; }
}

// The item's width and each row's horizontal step. A season and an information tab
// have a VARIABLE width (it comes from the text), which is why their step is not a
// constant like the episode's.
static float widthItem(int r, int c) {
  switch (r) {
    case SEC_SEASONS:  return widthSeason(c);
    case SEC_EPISODES:   return NV_SCREEN_W;
    case SEC_TABS_INFO:   return widthTabInfo(c);
    case SEC_TRAILERS:     return NV_DETF_TR_W;
    case SEC_RELATED: return REL_CARD_W;
    case SEC_COMMENTS:
      return (c < nPillsCom()) ? widthPillCom(COM_LABEL[c]) : COM_CARD_W;
    // The table is one block, the width of the divider. Falling into the `default`
    // would give it the width of a cast avatar, and the horizontal culling would cut
    // the table off screen.
    case SEC_DETAILS:    return NV_DETF_DET_W;
    default:              return NV_DETP_EL_W;
  }
}
// The x of item `c` WITHIN the row (before the horizontal scroll).
static float xItem(int r, int c) {
  float x = NV_DETP_X;
  for (int k = 0; k < c; k++) {
    if (r == SEC_EPISODES) continue;   // a vertical list: every row at the gutter
    if (r == SEC_CAST)    { x += NV_DETP_EL_STEP; continue; }
    if (r == SEC_TRAILERS)  { x += NV_DETF_TR_STEP;  continue; }
    if (r == SEC_RELATED) { x += REL_CARD_W + REL_CARD_GAP; continue; }
    if (r == SEC_COMMENTS) {
      // The pills add width + gap; the CARDS start again at NV_DETP_X because they sit
      // on a lower LINE. xItem stops being monotonic on this row, and that is fine: the
      // horizontal scroll only ever consults the FOCUSED column, never the whole
      // sequence.
      int np = nPillsCom();
      if (c <= np) x += widthPillCom(COM_LABEL[k]) + COM_PILL_GAP;
      else if (k >= np) x = NV_DETP_X + (float)(c - np) * (COM_CARD_W + COM_CARD_GAP);
      continue;
    }
    if (r == SEC_DETAILS)  { continue; }   // a single column: always at NV_DETP_X
    if (r == SEC_SEASONS) continue;   // one column: the picker is the row
    else x += widthTabInfo(k) + NV_DETP_TAB_SEP * 2 + 9.0f;  // 9 = the "|"'s width
  }
  return x;
}

// Recounts each section's columns every frame.
//
// detail_open's focus_start freezes nColumns with what EXISTS AT THE MOMENT of
// opening — and the episodes, the seasons and the cast come FROM THE NETWORK, seconds
// later. With the count stuck at zero, focus_move refuses any sideways step
// (`new < nColumns[row]` never passes), which is the reported defect: "the episode
// list won't move sideways".
//
// It returns early when nothing has changed, so it costs N integer comparisons. The
// same pattern as the home's syncRows(), for the same reason: what fills the catalogue
// is another thread.
// How many of a section's columns accept FOCUS. It is not always the same as sectionN,
// which says how many are DRAWN.
//
// Trailers are the case: the cards appear, but take no focus. This port has no YouTube
// player, and the rule already written twice in this code — the trailer button removed
// from the hero, the YouTube glyph swapped on the third circular button — is that a
// control which promises what it cannot deliver is worse than its absence.
// Skipping the row hides nothing: going down from the Cast to the Details, the
// scrolling passes over the trailers and they are visible on the way.
static int sectionColumns(int r) {
  // TRAILERS ARE FOCUSABLE. They were left out of the focus for a while, on the
  // argument that this port does not play YouTube and a control that promises what it
  // cannot deliver is worse than its absence — the same rule that removed the trailer
  // button from the hero.
  //
  // The owner asked for the opposite, and is right in this case: skipping the whole row
  // stops you even WALKING THROUGH the trailers to read their names, and "I can't
  // navigate the trailers" is a bigger defect than an OK with no effect. The card still
  // has no action on OK while there is no player.
  return sectionN(r);
}

static void syncColumns(void) {
  int r, changed = 0;
  for (r = 0; r < N_SECTIONS; r++) {
    int n = sectionColumns(r);
    if (focus.nColumns[r] != n) { focus.nColumns[r] = n; changed = 1; }
  }
  if (!changed) return;
  // The current column may have fallen out of range (the list shrank when the season
  // changed). Pulling it back in avoids drawing focus on a non-existent item.
  if (focus.column >= focus.nColumns[focus.row])
    focus.column = focus.nColumns[focus.row] > 0
                ? focus.nColumns[focus.row] - 1 : 0;
}

void detail_update(float dt, Uint32 now) {
  if (!is_open) return;
  syncColumns();
  // It releases the episode request that was held because it arrived with another load
  // in flight.
  disc_episodes_pending();

  // THE SEASON IS CHOSEN IN THE DROPDOWN, and nowhere else.
  //
  // There used to be a rest period here: the season row was a PILL PER SEASON, the
  // focus resting on a pill for NV_HERO_IDLE_MS committed that pill's column as the
  // season, and that is how walking the row swapped the episode list.
  //
  // The row became a SINGLE DROPDOWN (sectionN returns 1 column for SEC_SEASONS), so
  // `focus.column` there is ALWAYS 0 — and this block went on reading it as a season
  // index. Choosing season 2 from the list set `season` to 1, the focus stayed on the
  // dropdown where it had been, the rest period expired against column 0, and the
  // screen dropped straight back to season 1. Committing on OK is the whole of it now.

  // THE COMMENTS SELECTOR, by the same rule: moving the focus already swaps the source.
  // With no rest period — there are two pills, and the series' one is already in
  // memory; only the episode's costs a round trip, and it is fired once per episode.
  if (level >= 1 && focus.row == SEC_COMMENTS && isSeries()) {
    if (focus.column != commentEp) commentEp = focus.column;
    if (commentEp) {
      const CatItem *ci = cat_item(idx);
      int t = 0, ep = 0;
      if (ci && detail_ep_focus(&t, &ep) && t > 0 && ep > 0)
        extras_request_comments_ep(ci->imdb, t, ep);
    }
  }

  // AFTER syncColumns, not before: the stacking asks sectionN who has content, and
  // sectionN looks at data that arrives from the network. Recalculating with the
  // previous frame's count would leave the layout one frame behind — visible as a jolt
  // when the cast or the trailers arrive.
  recomputeLayout();
  // THE FLIGHT. Critically damped, and the ONLY easing on it: whoever reads `t`
  // reads a curve that is already eased, and must not ease it a second time —
  // that composition is what turned this movement into a two-frame cut (the
  // arithmetic is in layout.h, at NV_SPRING2_SCREEN).
  //
  // Closing is given its own frequency rather than its own curve: it is the same
  // spring chasing 0 instead of 1. A Back pressed mid-opening therefore has no
  // clock to restart and no keyframe to jump to — it turns round from wherever the
  // rectangle had got to. (anim_spring2 drops the velocity on a reversal, on
  // purpose: carrying the outward speed into the return is an overshoot.)
  t  = anim_spring2(&velT, t, exiting ? 0.0f : 1.0f, dt,
                    exiting ? NV_SPRING2_SCREEN_OUT : NV_SPRING2_SCREEN);
  // A stiffness of its own: the web app takes 0.8s to fade the backdrop out
  // (cubic-bezier .4,0,.2,1), and the NV_SPRING_SCREEN spring settles in ~330ms.
  pg = anim_spring(pg, level >= 1 ? 1.0f : 0.0f, dt, NV_SPRING_PAGE);
  // LET GO WHERE THE FADE ENDS, which is now a number the ramp itself names rather
  // than one guessed near zero. At NV_DETAIL_EXIT_CUT the backdrop's opacity is
  // exactly 0, so there is nothing on screen to pop — and the spring's tail below
  // that point was 270ms of an invisible rectangle being drawn over the home.
  if (exiting && t < NV_DETAIL_EXIT_CUT) {
    is_open = 0; exiting = 0; velT = 0.0f; t = 0.0f; return;
  }

  for (int r = 0; r < N_SECTIONS; r++)
    for (int c = 0; c < sectionN(r) && c < N_ITEMS; c++) {
      float target = (level >= 1 && focus_index(&focus, r, c)) ? 1.0f : 0.0f;
      animFocus[r][c] = anim_spring(animFocus[r][c], target, dt,
                                 target > animFocus[r][c] ? NV_SPRING_FOCUS : NV_SPRING_BLUR);
    }

  // How lit the episode rows are: dimmed around the focused one while the list has
  // the focus, all at rest otherwise. A spring, so entering the list from the picker
  // dims the neighbours instead of cutting them.
  epListA = anim_spring(epListA, (level >= 1 && focus.row == SEC_EPISODES)
                                   ? NV_DETEP_DIM : NV_DETEP_REST,
                        dt, NV_SPRING_BLUR);

  // The hero tooltip's fade. It is a RAMP and not a spring because the sheet gives it
  // a duration (140ms, linear-ish) and not a settle — the same reason the menu's veil
  // uses one. Only the hero shows tooltips, so everything below level 0 targets zero
  // and the labels are gone by the time the page scrolls.
  { int n = nButtons();
    for (int k = 0; k < N_BUTTONS; k++) {
      float target = (level == 0 && k > 0 && k < n && button == k) ? 1.0f : 0.0f;
      tipA[k] = anim_ramp(tipA[k], target, dt, NV_DETWEB_TIP_MS);
    } }

  // The three placeholders' crossfades. Each is independent, because the two threads
  // that fill them answer at different moments and there is no reason to make the
  // provider wait on the status. They only ever run forward: once a field is in, an
  // eviction or a reset elsewhere must not fade it back out under the viewer.
  { const CatItem *ciS = cat_item(idx);
    int haveProv = ciS && badges_provider(ciS->providerName) != 0;
    int haveSup  = ciS && ciS->directing[0] != 0;
    int haveStat = statusWord() != NULL;
    // Reduced motion SNAPS: the crossfade is the only thing moving here, so honouring
    // the setting means arriving rather than fading.
    int reduced = settings_animations_reduced();
    float ms = NV_DETW2_SKEL_FADE_MS;
    if (haveProv) skelProv = reduced ? 1.0f : anim_ramp(skelProv, 1.0f, dt, ms);
    if (haveSup)  skelSup  = reduced ? 1.0f : anim_ramp(skelSup,  1.0f, dt, ms);
    if (haveStat) skelStat = reduced ? 1.0f : anim_ramp(skelStat, 1.0f, dt, ms);
    // A slot whose source has FINISHED without producing anything closes too, or the
    // bar would sit there for the rest of the visit on a title that simply is not on
    // streaming here — which is an honest empty, not a slow one.
    if (!haveProv && !holdFor(0, pendingMeta()))                  skelProv = 1.0f;
    if (!haveSup  && !holdFor(0, pendingMeta()))                  skelSup  = 1.0f;
    if (!haveStat && !holdFor(0, isSeries() && pendingExtras()))  skelStat = 1.0f; }

  // --- the focused row's HORIZONTAL scroll ---------------------------------
  // Two rules, both from the web app's source (`getHorizontalTrackScrollLeft`): the
  // episode row BRINGS the focused card up against the left margin; the others scroll
  // only as far as needed, with 24px of slack at the edges.
  { int r = focus.row;
    if (r >= 0 && r < N_SECTIONS && sectionN(r) > 0) {
      float x = xItem(r, focus.column) - NV_DETP_X;
      float w = widthItem(r, focus.column);
      float view = NV_SCREEN_W - NV_DETP_X * 2;
      float target = scrollSec[r];
      // The episode list's scroll is VERTICAL, in the same slot: the focused row sits
      // second in the window, so the one you came from stays in sight above it.
      // The rows above the focused one are at rest height; the focused one is grown
      // to its whole synopsis, and that is what the bottom of the list has to allow.
      if (r == SEC_EPISODES) {
        float max = sectionN(r) * NV_DETEP_ROW_H - NV_DETEP_LIST_H
                  + episodeFullH(focus.column) - NV_DETEP_ROW_H;
        target = (focus.column - 1) * NV_DETEP_ROW_H;
        if (target > max) target = max;
      }
      else {
        // A focused pill: the row goes back to the start. The pills do not scroll along
        // with the cards (they sit on a line of their own, fixed), so leaving an old
        // card's scroll hanging would hide the first card as soon as the focus moved up
        // to the selector.
        if (r == SEC_COMMENTS && focus.column < nPillsCom()) target = 0.0f;
        else if (focus.column == 0) target = 0.0f;
        else if (x + w > target + view - 24.0f) target = x + w - view + 24.0f;
        else if (x < target + 24.0f)             target = x - 24.0f;
      }
      if (target < 0.0f) target = 0.0f;
      // THE EPISODE LIST MOVES LIKE DISCOVER'S GRID, on the owner's word: the
      // first-order spring at NV_SPRING_GRID, which leaves at speed and settles,
      // rather than the second-order one the horizontal rows use (see velSec), which
      // eases in. A vertical list read row by row wants the row you asked for to be
      // there at once, not to gather speed on the way.
      if (r == SEC_EPISODES) {
        scrollSec[r] = settings_animations_reduced()
                     ? target : anim_spring(scrollSec[r], target, dt, NV_SPRING_GRID);
        velSec[r] = 0.0f;
      } else
      scrollSec[r] = anim_spring2_reduced(&velSec[r], scrollSec[r], target, dt,
                                          NV_SPRING2_SCROLL,
                                          settings_animations_reduced());
    } }

  // --- VERTICAL scroll ------------------------------------------------------
  // TWO RULES, and which one applies is the section's own business (snapSec).
  //
  // A SERIES SNAPS TO A PAGE: the focused group's page goes to the top of the screen,
  // so the episode page shows the episode page and nothing else. That is the owner's
  // rule for this screen and the reason the fraction below is no longer used there —
  // 33% always keeps the end of the block above and the start of the one below in
  // frame, which from Play meant the hero's synopsis and the insight tabs at once.
  //
  // A FILM KEEPS THE FRACTION: the focused group's top goes to 33% of the usable
  // height (40% on the tabs), the web app's rule, checked on all four groups. A film's
  // sections are stacked from their content's height and have no pages to snap to. So
  // does a series' Trakt section, for the same reason.
  float targetY = 0.0f;
  if (level >= 1 && focus.row >= 0 && focus.row < N_SECTIONS) {
    // Aim at the top of the CONTENT (the track), not the group's: the section header
    // sits above and comes on screen with it, for free. It is what focusInList does in
    // the web app — `target.closest(".movie-cast-track, ...")`.
    float maxY = docEnd - NV_SCREEN_H;
    targetY = (snapSec[focus.row] >= 0.0f)
            ? snapSec[focus.row]
            : contentSec[focus.row] - NV_SCREEN_H * targetSec[focus.row];
    if (targetY > maxY) targetY = maxY;
    if (targetY < 0.0f) targetY = 0.0f;
  }
  scrollY = anim_spring2_reduced(&velY, scrollY, targetY, dt, NV_SPRING2_SCROLL,
                                settings_animations_reduced());
}

// ---------------------------------------------------------------------------
// HERO
// ---------------------------------------------------------------------------
// Nothing here is an overlay on a card: the screen is full-bleed, the column starts at
// x=72 and the stack is anchored to the BASE (`.detail-hero-section` is a flex column
// with `justify-content: flex-end`). Stacking from the top down makes the whole block
// rise and fall with the synopsis's length; in the web app it is pinned to the base and
// only the top moves.
// How much of the title has been watched, 0..100. 0 when it was never started.
static int progressOf(int i) {
  const CatItem *c = cat_item(i);
  return c ? c->progress : 0;
}

// The resting fill and the focused fill, the only two colours in this row. They are
// the SAME pair for the pill and for the circles — see the NV_DETWEB_* block.
static float btnFill(int focused) {
  return focused ? NV_DETWEB_FOCUS : NV_DETWEB_REST;
}
static float btnInk(int focused) {
  return focused ? NV_DETWEB_FOCUS_INK : NV_DETWEB_REST_INK;
}

// The `box-shadow 0 0 0 4px #fff` every focused control in this row carries. It is a
// white plate NV_DETW_RING larger drawn BEHIND the button, not a GFX_RING: the ring
// has to be flush with the border box and fully opaque, which is what a box-shadow at
// blur 0 is. The button's own fill then covers the middle, so nothing shows through.
//
// Against a focused #f5f5f5 fill the white reads as a 4px halo rather than an
// outline — and that is exactly what the web renders, checked on the element:
// `rgb(255,255,255) 0px 0px 0px 4px` over `rgb(245,245,245)`.
static void drawRing(GfxRect r, float a) {
  GfxRect ring = { r.x - NV_DETW_RING, r.y - NV_DETW_RING,
                   r.w + NV_DETW_RING * 2, r.h + NV_DETW_RING * 2 };
  gfx_color(ring, NV_RADIUS_PILL, 1, 1, 1, a);
}

static void drawButton(GfxRect r, const char *rot, int icon, int focused, float a) {
  // FOCUS IS A RING AND A COLOUR SWAP. THE BUTTON DOES NOT MOVE.
  //
  // This used to scale: the focused pill grew 1.114x1.147 and the circle 96 -> 110,
  // from captures of the native TCL app, with no ring anywhere. NuvioWeb does the
  // opposite and it was measured on the running app on 2026-09-15 — `transform: none`
  // on the focused element as well as the resting ones, and a 4px #fff box-shadow.
  //
  // The scale was there to solve a real defect: a white pill cannot show a white
  // ring. That defect is gone with the fill, not with the size — at rest the pill is
  // #222 like the circles, so the ring lands on dark in the only state that matters,
  // and the focused button separates itself by turning light.
  int circular = (rot == NULL);
  if (focused) drawRing(r, a);
  float fill = btnFill(focused), ink = btnInk(focused);
  gfx_color(r, NV_RADIUS_PILL, fill, fill, fill, a);

  if (circular) {
    float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
    // REAL ICONS, from art/icons (the web app's SVGs rasterised). Each glyph used to be
    // drawn by hand in the shader — a "+" of two rectangles, an eye of two discs, three
    // bars — and each was an approximation of the original. Now it is the file, and the
    // colour comes from here through GFX_BRAND.
    //
    // 44 inside a 96 circle, measured on `.series-btn-svg` in NuvioWeb (x=451.3 in a
    // circle at 425.3, so 26 of slack on each side). The device set had 0.333, which
    // is a glyph 12px smaller on the same button.
    float g = r.w * NV_DETWEB_CIRC_GLYPH;
    GfxRect ig = { cx - g * 0.5f, cy - g * 0.5f, g, g };
    if (icon == 1) {
      // THE LIST: NuvioWeb's pair, `renderLibraryGlyph`, from
      // ic_detail_library_add.svg and ic_detail_library_saved{,_filled}.svg.
      //
      // An EYE used to stand for "already on the watchlist", which meant the two
      // circles side by side were both eyes the moment a title was saved — the same
      // glyph for "it is in your library" and for "you have watched it", on adjacent
      // buttons. The web has no eye here at all: saved draws a FOLDER WITH A MINUS,
      // which is the action `tooltipOf` already names ("Remove from Watchlist").
      //
      // The "+" moves to the web's file too. It is a heavier plus than more.png (a
      // 12px stroke painted under the fill, so the strokes are thicker and the caps
      // square), and at 44px on a 96 circle the thin one read as a different set
      // from the folder next to it.
      //
      // Only the SAVED half has a focused twin, and that is the sheet's doing rather
      // than an omission here: `--series-icon-focused` is set on saved and not on
      // add, because a plus has no solid form to fill into.
      const CatItem *ci = cat_item(idx);
      int saved = (ci && ci->inList);
      gfx_icon(ig, saved ? (focused ? "detail_library_saved_filled"
                                    : "detail_library_saved")
                         : "detail_library_add",
               ink, ink, ink, a);
    } else if (icon == 2) {
      // WATCHED: NuvioWeb's own pair of glyphs, `renderWatchedGlyph` in
      // metaDetailsScreen.js, rasterised from ic_detail_watched{,_off}{,_filled}.svg.
      //
      // THE GLYPH IS THE ACTION, NOT THE STATE, and that is the part this port had
      // backwards. Here the open eye meant "seen" — so on a title already watched the
      // icon said EYE while the tooltip 16px above it said "Mark Unwatched", and the
      // two halves of the same button disagreed. `tooltipOf` has always read the web's
      // way; it is the icon that moves. Watched now offers the struck-through eye
      // ("unwatch this"), unwatched the open one ("mark this watched").
      //
      // AND THE PAIR FILLS ON FOCUS — `--series-icon-focused` in the sheet. The
      // outline is the resting glyph and the solid twin comes in with the focus, on
      // top of the ink inversion the button already does.
      //
      // With the list button moved over too, nothing calls more.png, watched.png or
      // unwatched.png any more — a different, thinner set that predates the web's.
      // The files stay in art/icons; they are 3KB each and removing a shipped asset
      // is not this change.
      int seen = progressOf(idx) >= 90;
      gfx_icon(ig, seen ? (focused ? "detail_watched_off_filled" : "detail_watched_off")
                        : (focused ? "detail_watched_filled"     : "detail_watched"),
               ink, ink, ink, a);
    } else {
      // SOURCES: a CLOUD, which is the shape this button has always had here — what
      // was wrong with the old one was the DRAWING, not the idea. sources.png comes
      // from neither of the sets the rest of the row uses; put beside the eye its
      // stroke is half again as heavy and the shape is a different cloud altogether.
      //
      // This is Phosphor's "cloud" and "cloud-fill", the same family as the eye and
      // the folder (the web's ic_detail_* are all Phosphor, 256 viewBox and all), so
      // the four circles finally read as one set. And the pack ships the filled twin,
      // so it fills on focus with the others.
      //
      // sources.png stays where it is: the PLAYER's source button still draws it, at
      // 44px over video, where it is not sitting next to these.
      gfx_icon(ig, focused ? "detail_source_filled" : "detail_source",
               ink, ink, ink, a);
    }
    return;
  }

  // THE PRIMARY PILL, laid out the way the flex row lays it out: padding 36, a 36px
  // icon box, the row's 24px gap, then the label, then padding 36 again. Measured on
  // "Play" — the svg sits at x=244 in a pill at x=208 (36), the pill is 193.3 wide and
  // the label's ink 61.3, and 36+36+24+61.3+36 = 193.3 to the pixel.
  //
  // It is ANCHORED LEFT, not centred. Centring was needed only while focus was a
  // scale, because the pill grew and the rasterised label did not; with the size
  // fixed, left is both simpler and what the flex does.
  //
  // A CLEAN PILL. The whole button used to double as a progress bar, with the unwatched
  // part under a 30% black veil. The intention was good but it read as a DISABLED
  // control, not as "16% watched" — "that resume looks awful, it doesn't even look like
  // the same app". The progress lives on the text line just above, in words.
  { int c = (int)(ink * 255.0f + 0.5f);
    float ix = r.x + NV_DETWEB_BTN_PADX;
    TxtLine l = txt_line(TXT_DETWEB_BTN, rot, c, c, c, 255);
    GfxRect tri = { ix, r.y + (r.h - NV_DETWEB_BTN_ICON) * 0.5f,
                    NV_DETWEB_BTN_ICON, NV_DETWEB_BTN_ICON };
    // THE GLYPH IS THE WEB'S FILE, NOT GFX_PLAY'S SHARP TRIANGLE. That is what was
    // still making this button read as another app's: `ic_detail_play.svg` is a
    // triangle with all THREE corners rounded (its path is three cubic curves), and
    // the shader primitive draws a hard-edged one. At 36px on a 96 pill the difference
    // is the whole character of the button.
    //
    // `detail_play.png` is that SVG rasterised at 128 with rsvg-convert, viewBox and
    // all, so the ink keeps the web's proportion inside the 36 box (the existing
    // play.png is the same shape but was rasterised tighter, and lands ~2px short).
    // gfx_icon draws it through GFX_BRAND, so the colour still comes from `ink` and
    // inverts with the label.
    gfx_icon(tri, "detail_play", ink, ink, ink, a);
    txt_draw_alpha(l, ix + NV_DETWEB_BTN_ICON + NV_DETWEB_BTN_GAPI,
                       r.y + (r.h - l.h) * 0.5f, a); }
}

// THE TOOLTIP over a focused circular button, `.series-circle-btn::after`.
//
// It exists because the circles carry no label: the sheet pulls `attr(aria-label)`
// into a pseudo-element and shows it on `.focused`, since a native `title` only ever
// fires for a mouse and this screen is driven by a d-pad. Without it the three
// circles are three glyphs the owner has to already know.
//
// `opacity` is the transition's own 0..1, kept per button by the caller so that
// moving the focus crossfades the way the CSS does; it also carries the 4px rise.
static void drawTooltip(GfxRect btn, const char *label, float opacity, float a) {
  if (!label || !label[0] || opacity <= 0.01f) return;
  float al = a * opacity * NV_DETWEB_TIP_ALPHA;
  TxtLine l = txt_line(TXT_DETWEB_TIP, label, 255, 255, 255, 255);
  // The box bottom sits at `calc(100% + 16px)` — 16 above the button's top edge — and
  // the line is centred in a box of PADY + 24 + PADY. What is positioned is the BOX,
  // so the ink is placed from the box's top plus the padding, and the line's own
  // height is recentred inside the 24px line-box.
  float boxTop = btn.y - NV_DETWEB_TIP_GAP - NV_DETWEB_TIP_H;
  float y = boxTop + NV_DETWEB_TIP_PADY + (NV_DETWEB_TIP_LH - l.h) * 0.5f;
  y += (1.0f - opacity) * NV_DETWEB_TIP_RISE;          // translateY(4px) -> 0
  float x = btn.x + (btn.w - l.w) * 0.5f;              // left:50% + translateX(-50%)
  // `text-shadow: 0 2px 8px rgba(0,0,0,.8)` APPROXIMATED, and it matters: the tooltip
  // has no background pill and sits straight on the backdrop, so on a bright frame
  // white-on-white would swallow it. There is no blur pass for glyphs here, so the
  // shadow is three offset black copies — the 2px drop plus one either side, which
  // spreads the ink enough to stand in for the 8px blur at this body size.
  txt_draw_alpha(txt_line(TXT_DETWEB_TIP, label, 0, 0, 0, 255), x, y + 2.0f, al * 0.80f);
  txt_draw_alpha(txt_line(TXT_DETWEB_TIP, label, 0, 0, 0, 255), x - 1.0f, y + 3.0f, al * 0.40f);
  txt_draw_alpha(txt_line(TXT_DETWEB_TIP, label, 0, 0, 0, 255), x + 1.0f, y + 3.0f, al * 0.40f);
  txt_draw_alpha(l, x, y, al);
}

// The primary button's width: padding 36 + icon 36 + gap 24 + text + padding 36.
// It is the flex row's own arithmetic and it checks out to the pixel against
// NuvioWeb: "Play" has an ink of 61.3 and the pill measures 193.3. A longer label
// grows the pill instead of spilling out from under the text.
static float widthPrimary(const char *rot) {
  TxtLine l = txt_line(TXT_DETWEB_BTN, rot, 0, 0, 0, 255);
  return NV_DETWEB_BTN_PADX * 2 + NV_DETWEB_BTN_ICON + NV_DETWEB_BTN_GAPI + l.w;
}

// The tooltip each circular button shows while focused. The strings are NuvioWeb's
// own aria-labels (metaDetailsScreen.js:2343-2363), including the rule that the
// library button says "Watchlist" when the library IS the Trakt watchlist — which,
// on this port, is the only thing it is: `inList` is filled from Trakt's real list.
//
// The PRIMARY has none, and that is the reference's behaviour too: it carries no
// aria-label because its label is already inside the pill.
static const char *tooltipOf(int action) {
  const CatItem *ci = cat_item(idx);
  switch (action) {
    case ACTION_LIST:
      if (trakt_active())
        return (ci && ci->inList) ? "Remove from Watchlist" : "Add to Watchlist";
      return (ci && ci->inList) ? "Remove from Library" : "Add to Library";
    case ACTION_WATCHED:
      return progressOf(idx) >= 90 ? "Mark Unwatched" : "Mark Watched";
    // NuvioWeb has no sources button on the hero — there the stream chooser opens
    // from the play button. This port keeps it, so the label is this port's, written
    // to the same shape as the others.
    case ACTION_SOURCES: return "Sources";
  }
  return NULL;
}

// The year taken out of the `meta` field ("2025 · 1 h 54 min" -> "2025" and "1 h 54 min").
static void fromMeta(const char *meta, char *year, size_t na, char *rest, size_t nr) {
  year[0] = 0; rest[0] = 0;
  if (!meta || !meta[0]) return;
  const char *sep = strstr(meta, "\xc2\xb7");        // U+00B7
  if (!sep) { snprintf(year, na, "%s", meta); return; }
  size_t n = (size_t)(sep - meta);
  while (n && (meta[n-1] == ' ')) n--;
  if (n >= na) n = na - 1;
  memcpy(year, meta, n); year[n] = 0;
  const char *r = sep + 2;
  while (*r == ' ') r++;
  snprintf(rest, nr, "%s", r);
}

// The IMDb badge: the real mark 60 wide with its height following the file, the score
// 8px later in rgb(179,179,179) — the SAME colour as the rest of the line, and not
// white. Measured on the two captures from the device (the badge at x=628..687 on the
// series and 714..773 on the film, always y=938..967).
//
// It is NOT the 109x60 this file carried over from the web app: there the logo is 60
// TALL and here the whole badge is 30. With the web app's value the badge was twice the
// height of the line it lives on.
//
// The mark used to be DRAWN here (a yellow rectangle with "IMDb" in it) because the app
// packages no SVG. art/icons/imdb_logo.png is the same mark as a PNG, so every screen
// that shows an IMDb score now rasterises it — this line, the episode cards below, and
// the home hero. See NV_IMDB_MARK_TEX_W for why they all ask for one width.
static float drawBadgeImdb(float x, float yCenter, int score, float a) {
  if (score <= 0) return 0.0f;
  char txt[8];
  snprintf(txt, sizeof txt, "%d.%d", score / 10, score % 10);
  TxtLine l = txt_line(TXT_DET_SIN, txt, 179, 179, 179, 255);
  // THE HEIGHT FOLLOWS THE FILE, not a constant: the mark is 575x289.83 and forcing
  // it into a fixed 60x30 would stretch it by whatever the rounding left over. The
  // aspect is 0 until the first decode lands, and NV_DETW2_IMDB_H stands in for that
  // one frame — the same two-step the hero logo does, for the same reason.
  const char *file = gfx_icon_path("imdb_logo");
  float ar = tex_aspect(file);
  float h = ar > 0 ? NV_DETW2_IMDB_W / ar : NV_DETW2_IMDB_H;
  GLuint mark = tex_get_exact(file, NV_DETW2_IMDB_W);
  GfxRect brand = { x, yCenter - h * 0.5f, NV_DETW2_IMDB_W, h };
  // GFX_TEXT keeps the texture's RGB *and* its alpha, which is the only mode that
  // can draw black letters on a yellow plate. gfx_icon would take the alpha alone.
  if (mark) gfx_rect(brand, mark, GFX_TEXT, 0, 0, 0, 0.0f, 1, 1, 1, a);
  txt_draw_alpha(l, x + NV_DETW2_IMDB_W + NV_DETW2_IMDB_GAP,
                     yCenter - l.h * 0.5f, a);
  return NV_DETW2_IMDB_W + NV_DETW2_IMDB_GAP + l.w;
}

// The OUTLINE badge of the second meta line. It carries TWO things inside the same box
// — the age rating and the production's status —, separated by a vertical bar: "TV-MA |
// RENEWED" on a series, "R | RELEASED" on a film. The rating comes out in
// rgb(179,179,179) and the status in WHITE, which makes the eye read the important part
// first.
//
// They used to be two loose things on the line, and the outline was faked with a filled
// rectangle 1px larger underneath — which only works over a flat background. Here it is
// GFX_RING, which draws a real outline and lets the art show through the middle, as on
// the device.
//
// `dir` may be NULL: with no status the badge carries only the rating and NO divider.
// There is no fallback value — CatItem has no status field, and stamping "RELEASED" on
// everything would be invented data, which has already cost dearly here.
static float drawBadgeMeta(float x, float y, const char *left, const char *dir,
                             float a) {
  TxtLine le = txt_line(TXT_DET_META2, left, 179, 179, 179, 255);
  TxtLine ld = { 0, 0, 0 };
  float w = NV_DETW2_BADGE_PADX * 2 + le.w;
  if (dir && dir[0]) {
    ld = txt_line(TXT_DET_META2, dir, 255, 255, 255, 255);
    w += NV_DETW2_DIV_PAD * 2 + NV_DETW2_DIV_W + ld.w;
  }
  GfxRect box = { x, y, w, NV_DETW2_BADGE_H };
  gfx_rect(box, 0, GFX_RING, 0, NV_DETW2_BADGE_BORDER / NV_DETW2_BADGE_H, 0,
           NV_DETW2_BADGE_R / NV_DETW2_BADGE_H, 0.42f, 0.42f, 0.42f, a);
  float cx = x + NV_DETW2_BADGE_PADX;
  float cy = y + NV_DETW2_BADGE_H * 0.5f;
  txt_draw_alpha(le, cx, cy - le.h * 0.5f, a);
  cx += le.w;
  if (dir && dir[0]) {
    GfxRect bar = { cx + NV_DETW2_DIV_PAD, cy - NV_DETW2_DIV_H * 0.5f,
                    NV_DETW2_DIV_W, NV_DETW2_DIV_H };
    gfx_color(bar, 0.0f, 0.42f, 0.42f, 0.42f, a);
    cx += NV_DETW2_DIV_PAD * 2 + NV_DETW2_DIV_W;
    txt_draw_alpha(ld, cx, cy - ld.h * 0.5f, a);
  }
  return w;
}

// The separator: THE WEB APP'S BAR, `.detail-meta-dot`. It was a 6px disc here, and
// the disc is what made this line read as a bulleted list rather than as one sentence
// with its parts fenced off — three discs of the same weight as the type, at three
// different spacings, with the IMDb plate and the tomato among them.
//
// A 2x18 rule has no weight of its own: it divides and disappears, which is the whole
// job. There are two uses with the SAME shape and different colours, and the colour is
// what groups the line: between genres rgb(179,179,179) (the text's own colour) and
// between GROUPS rgb(128,128,128), more muted.
//
// Squared off, not rounded: `gfx_color`'s first argument is the corner radius and a
// 2px bar with any radius at all turns back into a lozenge.
static void drawSep(float x, float yCenter, float luma, float a) {
  GfxRect bar = { x, yCenter - NV_DETW2_BAR_H * 0.5f,
                  NV_DETW2_BAR_W, NV_DETW2_BAR_H };
  gfx_color(bar, 0.0f, luma, luma, luma, a);
}

// BETWEEN GENRES, where the separator is punctuation and not a fence: a 6px disc in
// the text's own rgb(179,179,179), which is the " • " the web joins that run with.
//
// The bar was tried here too and it was wrong: it gave "Drama / Fantasy" the same
// weight of division as "genres / year", so a list of two genres read as two
// unrelated fields. The line has one change of subject per bar and the dots sit
// inside those groups.
static void drawDot(float x, float yCenter, float a) {
  GfxRect pt = { x, yCenter - NV_DETW2_DOT_D * 0.5f,
                 NV_DETW2_DOT_D, NV_DETW2_DOT_D };
  gfx_color(pt, 0.5f, 0.702f, 0.702f, 0.702f, a);
}

// The production status as the meta line prints it, or NULL when there is none to
// print. Factored out because the SKELETON has to ask the same question the drawing
// does — "will there be a badge here?" — and two copies of this mapping would drift.
static const char *statusWord(void) {
  const char *raw = extras_profile_status();
  if (!isSeries()) return NULL;          // a film never carries one
  if (!strcmp(raw, "canceled") || !strcmp(raw, "Canceled")) return "CANCELLED";
  if (!strcmp(raw, "ended")    || !strcmp(raw, "Ended"))    return "ENDED";
  if (!strcmp(raw, "returning series"))                     return "NOW SHOWING";
  if (!strcmp(raw, "renewed"))                              return "RENEWED";
  return NULL;
}

// IS A LATE FIELD STILL COMING? Two threads fill this block and they are asked
// separately: discover.c's /meta call carries the provider lockup, the "Director:"
// credit and the country (photosOfCast, which fetches the watch providers, runs on
// that same thread — see discover.c), and extras.c's TMDB sheet carries the status.
static int pendingMeta(void)   { return disc_episodes_loading(idx); }
static int pendingExtras(void) { return !extras_settled(); }

// THE CEILING ON WAITING. Past it the held slots close and the line settles once.
// Without it, a title that is simply not on streaming here — which is an honest
// empty, not a slow one — would hold an empty box for the whole visit.
static int skelDone(void) { return SDL_GetTicks() - skelSince > NV_DETW2_SKEL_MS; }

// Hold a place when the field is absent, its source is still working, and the clock
// has not run out. All three, or the slot closes.
static int holdFor(int have, int pending) {
  return !have && pending && !skelDone();
}

// THE PLACEHOLDER: a dim pill where the real thing will be, with the shine crossing
// it — the same greys as the skeletons below the fold (drawSkeletonEpisodes,
// drawSkeletonCast), luma around 0.2 at roughly 0.6 alpha.
//
// A PULSE was tried here before and dropped, and the reason it was dropped is the
// reason the shine works: a pulse is per block, so two bars breathing on the hero
// while the season picker below sat still read as two interfaces rather than one
// screen filling in. GFX_SKELETON's band travels across the SCREEN, so these bars
// and the blocks below the fold are lit by one light passing over the page — they
// cannot fall out of step, because there is only one of it. See NV_SKEL_SHINE_W.
static void drawSkel(float x, float yCenter, float w, float h, float a) {
  gfx_skeleton((GfxRect){ x, yCenter - h * 0.5f, w, h },
               NV_RADIUS_PILL, 0.20f, 0.21f, 0.23f, a * 0.60f);
}

static void heroWeb(float a, float offset) {
  // THE FLIGHT KEEPS THIS BLOCK ALIVE AT ALPHA 0, and only for the logo.
  //
  // Everything here comes in on phase2, which is still 0 for the first 45% of the
  // transition — but the title's logo is crossing the screen during exactly those
  // frames, and home.c has already stopped drawing its copy. Returning early would
  // leave no logo anywhere for the first half of the movement and then have it
  // appear mid-flight, which is worse than the crossfade this replaces.
  //
  // The test is narrow on purpose: a block laid out at alpha 0 costs geometry
  // (gfx_rect has no alpha early-out), so it is paid only while a logo is actually
  // in the air. A title with no logo, or a hero that never drew one, returns as
  // before. The layout is not wasted either — it is this call that asks text.c for
  // the lines, so they are rasterised and ready by the time phase2 lifts them.
  if (a <= 0.005f) {
    float hx, hy, hw, hh;
    if (!(sharedOrigin && t < 0.999f && logoOf(idx) &&
          home_hero_logo_rect(&hx, &hy, &hw, &hh))) return;
  }
  const CatItem *ci = cat_item(idx);

  char year[32], duration[64];
  fromMeta(profileOf(idx), year, sizeof year, duration, sizeof duration);

  // THE SECOND SOURCE FOR THE YEAR AND THE RUNTIME, and the reason the line used to
  // show different amounts of information on different titles.
  //
  // `meta` is built in discover.c from the addon's `releaseInfo` and `runtime`
  // fields, and NOT EVERY ADDON SENDS THEM — where Cinemeta answers "2015 · 2h 0min"
  // the TMDB catalogue can answer nothing at all, and then both halves of the line
  // vanish at once. Mad Max: Fury Road came out with no year and no runtime while
  // Practical Magic, two rows away in the same catalogue, had both.
  //
  // TMDB's fact sheet has carried them the whole time: extras.c already parses
  // release_date and runtime out of the /movie/<id> body it fetches for the
  // collection, and this screen already made that request. Nothing new goes over the
  // network — the values were being fetched and dropped.
  //
  // This is NOT the invented "14" age rating that discover.c warns about. A fallback
  // VALUE would be a constant pretending to be data; this is the same fact from the
  // other source that already has it, and when TMDB has not answered either the
  // fields stay empty and the groups stay off the line.
  if (!year[0]) {
    const char *rel = extras_profile_release();       // "2015-05-13"
    if (rel && strlen(rel) >= 4 && rel[0] >= '0' && rel[0] <= '9')
      snprintf(year, sizeof year, "%.4s", rel);
  }
  if (!duration[0] && !isSeries()) {
    int m = extras_profile_duration();
    // The addons' own spelling, so the fallback is indistinguishable from the field
    // it stands in for: "1h44min" over the hour, "44min" under it.
    if (m >= 60)     snprintf(duration, sizeof duration, "%dh%02dmin", m / 60, m % 60);
    else if (m > 0)  snprintf(duration, sizeof duration, "%dmin", m);
  }

  // On a series the web app writes "Writer:"/"Creator:"; on a film, "Director:".
  char sup[192] = "";
  if (ci && ci->directing[0])
    snprintf(sup, sizeof sup, "%s: %s", isSeries() ? "Writer" : "Director",
             ci->directing);

  const char *sin = synopsisOf(idx);

  // --- THE COLUMN'S ORDER, as in the owner's reference ------------------------
  //
  // From top to bottom: the logo, the meta line (year, seasons, rating), genres, who
  // directed it, the synopsis, the resume line and finally the BUTTONS.
  //
  // The buttons used to come just below the logo and the genres/rating fell into the
  // footer, which split the title's information into two blocks with the action in
  // between. In the reference everything that DESCRIBES the title comes together and the
  // action closes the block — that is what the owner asked for on comparing the two screens.
  //
  // It is still anchored to the BASE: the synopsis changes height with the text, and
  // anchoring at the top would make the button dance from title to title.
  // THE ACTION COMES JUST BELOW THE LOGO, and all the text that DESCRIBES the title
  // comes together, underneath it.
  //
  // It was the other way round: the actions closed the block, with ~500 px of text above
  // them — the screen's only interactive target was the furthest from the top. MEASURED
  // against the reference (TCL, 1920x1080): logo 311..473, ACTIONS 509..608, support
  // 640..685, synopsis 700..900, meta 938..970, rating/country 1003..1045.
  //
  // This does NOT reintroduce the defect that motivated the old order. That one was
  // information SPLIT IN TWO — part above the action, part in the footer. Here the
  // action rises and the text drops WHOLE, in a single block. This file's own constants
  // already described this order (NV_DETW_GAP_ACTIONS is literally "actions ->
  // Director:"); it was the code that had drifted from the token sheet.
  //
  // It still stacks FROM THE BOTTOM UP and is anchored to the base: the synopsis changes
  // height with the text, and anchoring at the top would make the whole block dance from
  // title to title. That was CONFIRMED on the device: between the series (5 lines of
  // synopsis) and the film (4) the two meta lines fall at exactly the same y (938 and
  // 999) and so does the LAST line of the synopsis — what grows upwards is the rest of
  // the stack.
  //
  // THE NEW GRID, measured on the TCL: actions 512..606, support 649..675, the synopsis
  // in steps of 40 ending with its ink at 890, meta 1 (genres/date/IMDb) 938..968 and
  // meta 2 (the outline badge + country) 999..1048.
  //
  // THE LOOSE GENRE LINE HAS GONE: on the device the genres open the first meta line,
  // and the year and score come after them, separated by a dot. Here there were two
  // lines — one with genres alone, another with the year and duration — and the IMDb
  // badge sat by itself against the screen's right edge, half a metre from the text
  // block it belongs to.
  float hSin = 0.0f;
  if (sin) hSin = txt_block(TXT_DET_SIN, sin, 255, 255, 255, -1.0f, 0.0f,
                            NV_DETW2_TEXT_W, NV_DETW2_LD_SIN, 0.0f,
                            NV_DETW2_SIN_LINES);
  float yMeta2 = NV_DETW2_BASE - NV_DETW2_BADGE_H;
  float yMeta1 = yMeta2 - NV_DETW2_META_GAP - NV_DETW2_M1_H;
  float ySin   = yMeta1 - NV_DETW2_GAP_SIN - hSin;
  // THE CREDIT'S ROW IS RESERVED WHILE IT COULD STILL ARRIVE, and this one `if` is
  // why: the stack is anchored at the base and grows upward, so it decides where the
  // ACTION ROW lands. "Director:" coming in on discover.c's thread used to open its
  // row underneath the buttons and shove the pill and the three circles 62px up, under
  // a hand already reaching for them. Held open, the credit fades into a space that
  // was always there and nothing moves.
  int supSlot  = sup[0] || holdFor(0, pendingMeta());
  float ySup   = supSlot ? ySin - NV_DETW2_GAP_SUP : ySin;
  float yActions = ySup - NV_DETW2_GAP_ACTIONS - NV_DETWEB_BTN_H;

  // It moves a few pixels as it comes in, instead of appearing ready in place.
  // WHICH WAY is NV_DETW_COPY_TOGETHER — up into place, against the home's copy
  // leaving downward, or down into place alongside it. `offset` is the document's
  // scroll and is not part of the choice.
  // Travelling WITH the home's copy only means anything when the home's copy is
  // the thing being replaced. Opened from anywhere else there is nothing leaving
  // downward to keep company with, and a block descending into place on its own
  // reads as the page dropping rather than settling — so those rise, as before.
  float travel = (sharedOrigin && NV_DETW_COPY_TOGETHER) ? -26.0f : 26.0f;
  float rises = (1.0f - a) * travel + offset;
  yMeta2 += rises; yMeta1 += rises; ySin += rises; ySup += rises;
  yActions += rises;

  // --- logo -----------------------------------------------------------------
  const char *fileLogo = logoOf(idx);
  // TEX_GET CAPS THE DECODE AT 640 AND THIS LOGO IS DRAWN UP TO 1000.
  //
  // The note that used to sit here said "the 960 ceiling would already be
  // enough" — and it would, except NV_TEX_WIDTH_MAX is 640 (tex_cache.c), so a
  // wide logo was decoded at 640 and stretched up to NV_DETW_LOGO_MAXW. At an
  // aspect of 5 that is 1.56x of upscale on the largest piece of type on the
  // screen, which is exactly where softness is easiest to see.
  //
  // The first request stays tex_get because the WIDTH depends on the aspect and
  // the aspect is only known once something has been decoded. With the aspect
  // in hand the art is asked for again at the size it is actually drawn;
  // tex_get_width promotes the item and re-decodes once, which is the same path
  // the hero already uses when it takes over a card's art.
  GLuint texLogo = fileLogo ? tex_get(fileLogo) : 0;
  if (texLogo) {
    float aspect = tex_aspect(fileLogo);
    if (aspect <= 0.0f) aspect = 2.5f;
    float h = NV_DETW_LOGO_H, w = h * aspect;
    if (w > NV_DETW_LOGO_MAXW) { w = NV_DETW_LOGO_MAXW; h = w / aspect; }
    // TEX_GET_EXACT ONCE THE PAGE OWNS THE SCREEN, tex_get_width while it does not.
    //
    // Exact is what this wants: tex_get_width leaves the texture 1.25x-1.6x the drawn
    // size and GL_LINEAR_MIPMAP_NEAREST snaps at 1.414, so the largest type on the
    // screen lands either undersampled or halved-and-magnified depending on which side
    // of that the title's aspect puts it. The collection hero had the same defect —
    // see the note in home.c, which measures it.
    //
    // THE GATE IS NOT TIMIDITY. `ci->logo` is asked for by FOUR places (this, the home
    // hero, the open card, the player) and an exact entry re-decodes when the width
    // moves in EITHER direction, while every other caller promotes it back up. While
    // the detail is opening, home_draw still runs underneath (app.c only skips it once
    // detail_covers_screen) and the home hero is usually showing THIS title — so the
    // two would take turns re-decoding the same PNG for the length of the animation,
    // which is the per-frame decode tex_get_exact's header warns about. Once the page
    // covers the screen it is the only caller left and the swap is safe; on the way out
    // the gate opens again before home returns. Two decodes per visit, not dozens.
    //
    // A hero that never covers (a banded one, or the detail opened from search) keeps
    // the tex_get_width path for its whole life, which is exactly what it draws today.
    { GLuint sharp = detail_covers_screen() ? tex_get_exact(fileLogo, w)
                                            : tex_get_width(fileLogo, w);
      if (sharp) texLogo = sharp; }
    // The logo settles above the actions row.
    float baseLogo = yActions - NV_DETW_LOGO_GAP;
    GfxRect r = { NV_DETW2_X, baseLogo - h, w, h };

    // THE LOGO IS ONE ELEMENT, NOT TWO OF THEM CROSSFADING. The home's hero and this
    // screen draw the SAME image — `ci->logo`, the title's own mark — so there is no
    // reason for one to fade out while the other fades in. It FLIES: from where the
    // hero had it to where this layout puts it, on the same curve as the backdrop, so
    // the mark the viewer was already reading is the one that ends up on the page.
    //
    // home.c stops drawing its copy from the first frame of the transition (see the
    // note at its logo), so only ever one is on screen.
    //
    // IT TARGETS THE RESTING RECT, with `rises` taken back off. Everything else in
    // this block comes in 26px low and rises as it fades; the logo must not do both,
    // or it would arrive travelling upward after having just travelled across.
    //
    // ALPHA GOES TO 1 while it flies. The fade is for copy that has nowhere to come
    // from; this has somewhere, and fading something that is moving only makes the
    // movement harder to follow.
    float aLogo = a;
    { GfxRect hl;
      if (sharedOrigin && t < 0.999f &&
          home_hero_logo_rect(&hl.x, &hl.y, &hl.w, &hl.h)) {
        // ON detail_progress, NOT ON `t`. The two are the same on the way in and
        // differ on the way out, where detail_progress is remapped to reach 0 at
        // NV_DETAIL_EXIT_CUT — the frame detail_update releases the screen.
        //
        // Driven by the raw curve this landed at 0.06 instead: on the last drawn
        // frame the mark was still 6% of the way towards the title screen's
        // layout, in POSITION and in SIZE, and then the home drew its own at rest.
        // On a wide wordmark 6% is tens of pixels and a visible change of scale,
        // arriving in one frame. That is the snap at the end of the close; the
        // rows had the same defect and the same cause.
        float p = detail_progress();
        GfxRect rest = { r.x, baseLogo - rises - h, r.w, r.h };
        r.x = hl.x + (rest.x - hl.x) * p;
        r.y = hl.y + (rest.y - hl.y) * p;
        r.w = hl.w + (rest.w - hl.w) * p;
        r.h = hl.h + (rest.h - hl.h) * p;
        aLogo = 1.0f;
      } }
    gfx_tex_aspect_current = 0.0f;   // the logo already comes at the right aspect ratio
    // A BLACK LOGO BECOMES WHITE. TMDB serves the same mark in a light and a dark
    // version and does NOT say which is which — there is no field for it, and the web
    // app's own ranking orders only by language and score. When the dark one comes up,
    // it appears black over a dark backdrop and the title disappears from the screen:
    // that is what happened with "The Invite".
    //
    // The decision is by MEASUREMENT, not by a fixed rule: tex_luminance returns the
    // average of the opaque pixels, computed once on the decode thread. Only genuinely
    // dark art is tinted; a light or COLOURFUL logo (the gold one, the red one) passes
    // through GFX_TEXT untouched, because flattening it to white would swap one defect
    // for another.
    //
    // -1 = still loading: treat it as light and do not tint. Erring on the side of not
    // touching the art is right while it is not known.
    { GfxMode m = tex_brand_dark(fileLogo) ? GFX_BRAND : GFX_TEXT;
      gfx_rect(r, texLogo, m, 0, 0, 0, 0.0f, 1, 1, 1, aLogo); }
  } else {
    // With no logo, the NAME. The box's height is still the logo's, so the button row
    // does not jump between a title with a logo and one without.
    const char *name = titleOf(idx);
    if (name) {
      TxtLine t2 = txt_line_trim(TXT_TITLE1, name, 255, 255, 255, 255,
                                    NV_DETW_LOGO_MAXW);
      // The same anchor as the logo: above the actions. The BOX's height is still
      // the logo's, so the actions row does not jump between a title with a logo and
      // one without.
      float baseLogo = yActions - NV_DETW_LOGO_GAP;
      txt_draw_alpha(t2, NV_DETW2_X,
                         baseLogo - NV_DETW_LOGO_H
                                  + (NV_DETW_LOGO_H - t2.h) * 0.5f, a);
    }
  }

  // --- buttons --------------------------------------------------------------
  // In FLOW, with 24px between neighbours — not the 63 that came from the web app.
  // MEASURED on the device: the pill 96..417, the circles centred at 488.5 and 608.5
  // (step 120, diameter 96), which gives 23.5 and 24 of gap. The circles are 96 and not
  // 84, and sit 1px taller than the pill (511..606 against 512..606), which in practice
  // is the same vertical centre — and that is how they are aligned here.
  //
  // Two label states, measured: "Resume S2E3" when there is progress, "Play" when there
  // is not. (The web app has a third, "Next S2E4", which comes from the next unwatched
  // episode — the native catalogue does not store which episodes have been watched, so
  // that state has nowhere to come from.)
  // THREE states, like the web app: "Resume SxEy" (in progress), "Next SxEy" (the first
  // unwatched) and "Play" (never opened). The third state was taken to be impossible
  // here; it has been possible ever since extras_ep_watched existed.
  char rot[48];
  { int t = 0, e = 0, de = 0;
    if (isSeries() && episodeTarget(&t, &e, &de) && t > 0 && e > 0 && de >= 2)
      snprintf(rot, sizeof rot, "%s S%dE%d",
               de == 2 ? "Resume" : "Next", t, e);
    else if (ci && ci->progress > 0) snprintf(rot, sizeof rot, "Resume");
    else snprintf(rot, sizeof rot, "Play"); }

  { float cyBtn = yActions + NV_DETWEB_BTN_H * 0.5f;
    int nb = 0, n = nButtons();
    // Changing title with the focus on the last circular button of a FILM and landing
    // on a series would leave `button` = 3 in a row of 3 buttons: none would appear
    // focused and OK would find no action. It is fixed here, in the drawing, which is
    // what every frame goes through.
    if (button >= n) button = n - 1;
    float bx = NV_DETW2_X;
    GfxRect rp = { bx, yActions, widthPrimary(rot), NV_DETWEB_BTN_H };
    drawButton(rp, rot, 0, level == 0 && button == nb, a);
    bx += rp.w + NV_DETWEB_BTN_GAP; nb++;
    // The circles are drawn first and the TOOLTIPS after, in a second pass. They
    // overhang the button above them by 54px, and a tooltip drawn inside the loop
    // would be painted over by the next circle's ring — which is what a z-index of 10
    // buys the pseudo-element in the sheet.
    GfxRect rc[N_BUTTONS];
    int nc = 0;
    for (; nb < n; nb++) {
      GfxRect r = { bx, cyBtn - NV_DETWEB_CIRC * 0.5f,
                    NV_DETWEB_CIRC, NV_DETWEB_CIRC };
      drawButton(r, NULL, actionIn(nb), level == 0 && button == nb, a);
      rc[nc++] = r;
      bx += NV_DETWEB_CIRC + NV_DETWEB_BTN_GAP;
    }
    for (int k = 0; k < nc; k++)
      drawTooltip(rc[k], tooltipOf(actionIn(k + 1)), tipA[k + 1], a);
    regionAdd("actions", (GfxRect){ NV_DETW2_X, yActions,
                                    bx - NV_DETWEB_BTN_GAP - NV_DETW2_X,
                                    NV_DETWEB_BTN_H });
  }

  // --- the resume line: NOT DRAWN --------------------------------------------
  // The web app writes "Resume available · 45% · Episode S2E3" in the band between the
  // actions row and the support line. Here that band is where the circular buttons'
  // TOOLTIPS come out — they sit 16px above the button's top edge — so the two pieces of
  // ink landed on top of each other whenever a focused action had a tooltip. The line is
  // dropped rather than moved: the primary button already reads "Resume S1E1", which is
  // the same fact in the place the eye is already on.

  // --- "Writer: ..." / "Director: ..." ---------------------------------------
  // The same BODY SIZE as the synopsis, and not a smaller one: in the reference the "W"
  // of "Writer" and the "C" of the synopsis measure the same 20 of cap height. It was
  // on TXT_DET_META (25) against TXT_DET_SIN (26) because of a web app measurement,
  // where the two lines really do differ.
  if (sup[0] || supSlot) {
    // Both are drawn while the crossfade runs, at complementary alphas, in the same
    // place — skelSup is 0 on the bar's side and 1 on the text's.
    if (skelSup < 0.999f)
      drawSkel(NV_DETW2_X, ySup + NV_DETW2_SKEL_H * 0.5f + 4.0f,
               NV_DETW2_SKEL_SUP, NV_DETW2_SKEL_H, a * (1.0f - skelSup));
    if (sup[0] && skelSup > 0.001f) {
      TxtLine l = txt_line_trim(TXT_DET_SIN, sup, 179, 179, 179, 255,
                                   NV_DETW2_TEXT_W);
      txt_draw_alpha(l, NV_DETW2_X, ySup, a * skelSup);
    }
  }

  // --- sinopse --------------------------------------------------------------
  if (sin) txt_block(TXT_DET_SIN, sin, 255, 255, 255, NV_DETW2_X, ySin,
                     NV_DETW2_TEXT_W, NV_DETW2_LD_SIN, a, NV_DETW2_SIN_LINES);

  // --- meta line 1: genres | genres  |  year  |  [IMDb] score ----------------
  //
  // A single line, in the device's order. The IMDb badge goes HERE, at the end of the
  // groups, and not up against the screen's right edge: it was orphaned, more than
  // 1000px from the text it belongs to, because the inherited value was NV_DETW_DIR.
  //
  // Two shapes of separator: a BAR between groups, a DOT inside one — see drawSep
  // and drawDot.
  {
    float x = NV_DETW2_X, yc = yMeta1 + NV_DETW2_M1_H * 0.5f;
    const CatItem *badgeItem=cat_item(idx);
    int something = 0;
    // THE PROVIDER IS A GROUP OF ITS OWN, so it gets the group bar like every other
    // boundary on this line. It used to run straight into the genres on the row's own
    // 14px of slack — which is the gap BETWEEN two marks, not between the lockup and
    // the next subject, so "HBOmax" and "Drama" read as one run.
    //
    // `something` is what puts the bar there: setting it makes the first genre take
    // the group separator instead of opening the line. NV_BADGE_GAP comes back off
    // first — badges_draw's width ends one gap past the last mark, and leaving it in
    // would centre the bar 14px to the right of where the other four sit.
    // THE LOCKUP OPENS THE LINE, so it is the one whose lateness moves everything —
    // genres, year, score and all. Its slot is held while discover's /meta call could
    // still name a provider, and the mark fades into it when it does.
    {
      uint64_t provMask = badgeItem ? badges_provider(badgeItem->providerName) : 0;
      int provHold = holdFor(provMask != 0, pendingMeta());
      float wProv = 0.0f;
      if (provMask && skelProv > 0.001f)
        wProv = badges_draw_sharp(provMask, x, yc - NV_DETW2_PROV_H * 0.5f,
                                  NV_DETW2_PROV_MAXW, NV_DETW2_PROV_H, a * skelProv);
      if (provHold || (provMask && skelProv < 0.999f))
        drawSkel(x, yc, NV_DETW2_SKEL_PROV, NV_DETW2_SKEL_H, a * (1.0f - skelProv));
      // The slot keeps the RESERVED width until the mark is fully in: advancing by the
      // real width halfway through the crossfade would step the line sideways, which is
      // the one thing the reservation exists to stop.
      if (wProv > 0.0f && skelProv >= 0.999f) { x += wProv - NV_BADGE_GAP; something = 1; }
      else if (provHold || wProv > 0.0f)      { x += NV_DETW2_SKEL_PROV;   something = 1; }
    }
    // GENRES without the first field. `genre` comes from the catalogue as
    // "TV Show · Action · Adventure" and the first piece is always the TYPE
    // (see catalog.c:539) — the reference does not show it on the meta line, only the
    // genres. They are one GROUP: a bar in front of the first, dots between the rest.
    // `nGenre` is what tells those two cases apart — `something` cannot, because by
    // the first genre it may already be set by the provider lockup.
    int nGenre = 0;
    const char *g = genreOf(idx);
    if (g) {
      const char *p = strstr(g, "\xc2\xb7");
      while (p) {
        char term[80]; size_t n;
        p += 2; while (*p == ' ') p++;
        const char *end = strstr(p, "\xc2\xb7");
        n = end ? (size_t)(end - p) : strlen(p);
        while (n && p[n-1] == ' ') n--;
        if (n && n < sizeof term) {
          memcpy(term, p, n); term[n] = 0;
          if (nGenre) {                    // inside the group: punctuation
            drawDot(x + NV_DETW2_BULLET_SEP, yc, a);
            x += NV_DETW2_BULLET_SEP * 2 + NV_DETW2_DOT_D;
          } else if (something) {          // provider | genres: a change of subject
            drawSep(x + NV_DETW2_SEP, yc, 0.502f, a);
            x += NV_DETW2_SEP * 2 + NV_DETW2_BAR_W;
          }
          TxtLine lt = txt_line(TXT_DET_SIN, term, 179, 179, 179, 255);
          txt_draw_alpha(lt, x, yc - lt.h * 0.5f, a);
          x += lt.w; something = 1; nGenre++;
        }
        p = end;
      }
    }
    // THE YEAR. On a series the reference writes "2023-" and on a film the full date;
    // the catalogue stores only the year in both cases (discover.c cuts the series' en
    // dash on purpose), so the year is what comes out. Empty when the metadata has not
    // arrived — and then the whole group disappears, with no fallback value.
    if (year[0]) {
      if (something) { drawSep(x + NV_DETW2_SEP, yc, 0.502f, a);    // 128
                  x += NV_DETW2_SEP * 2 + NV_DETW2_BAR_W; }
      TxtLine la = txt_line(TXT_DET_SIN, year, 179, 179, 179, 255);
      txt_draw_alpha(la, x, yc - la.h * 0.5f, a);
      x += la.w; something = 1;
    }
    if (ci && ci->score > 0) {
      if (something) { drawSep(x + NV_DETW2_SEP, yc, 0.502f, a);
                  x += NV_DETW2_SEP * 2 + NV_DETW2_BAR_W; }
      x += drawBadgeImdb(x, yc, ci->score, a);
      something = 1;
    }
    const int sources[] = { EX_TOMATOES, EX_TRAKT };
    for(int i=0;i<2;i++) {
      int n=extras_score(sources[i]);
      if(n<=0) continue;
      // Rotten Tomatoes: a FRESH tomato from 60% up, the green SPLAT below — it is the
      // site's own convention, and it is the icon that gives the verdict before the
      // number. Trakt: the WORDMARK (the name), not the icon.
      const char *brand;
      if(sources[i]==EX_TOMATOES) brand=extras_path_brand_name(n>=600?"tomatoes_fresh":"tomatoes_rotten");
      else brand=extras_path_brand_name("trakt_wordmark");
      GLuint logo=tex_get(brand);
      char value[20];snprintf(value,sizeof value,"%d%%",n/10);
      TxtLine lv=txt_line(TXT_DET_META2,value,220,220,225,255);
      float mh=sources[i]==EX_TRAKT?22.0f:32.0f,mw=mh;
      if(logo){float ap=tex_aspect(brand);if(ap>0)mw=mh*ap;if(mw>110)mw=110;}
      // FENCED LIKE EVERY OTHER GROUP ON THE LINE. These two used to join on a bare
      // 24px gap, so the line ran "IMDb 4.3   trakt 46%" — the one join with nothing
      // between it, and at 24 it read as a wide word space rather than as a new group.
      // The budget is checked BEFORE the bar is drawn, or a rating that does not fit
      // would leave its separator behind pointing at nothing.
      float adv = something ? NV_DETW2_SEP * 2 + NV_DETW2_BAR_W : 0.0f;
      if(x+adv+mw+10+lv.w>NV_DETW2_X+NV_DETW2_RATE_W)break;
      if(something) drawSep(x + NV_DETW2_SEP, yc, 0.502f, a);
      x+=adv; something=1;
      // GFX_TEXT and not GFX_SNAP: SNAP ignores the texture's alpha and the tomato came
      // out with a dark square around it. TEXT preserves the RGB and uses the alpha.
      // The Trakt wordmark is dark: it goes through GFX_BRAND, which tints the alpha.
      if(logo){GfxMode m=sources[i]==EX_TRAKT&&tex_brand_dark(brand)?GFX_BRAND:GFX_TEXT;
        gfx_rect((GfxRect){x,yc-mh*.5f,mw,mh},logo,m,0,0,0,0,.93f,.94f,.96f,a);}
      else {TxtLine label=txt_line(TXT_MINI,extras_source_brand(sources[i]),200,200,205,255);
        txt_draw_alpha(label,x,yc-label.h*.5f,a);mw=label.w;}
      x+=mw+10;txt_draw_alpha(lv,x,yc-lv.h*.5f,a);x+=lv.w;
    }
  }

  // --- meta line 2: [status]  |  duration  |  country ----------------------
  //
  // The outline badge used to carry the age rating, with the production's status beside
  // it behind a divider ("TV-MA | RENEWED"). The rating has gone from the app: a
  // certification plate in the hero is a warning label, and it is not what the line is
  // for. The badge is now the status alone, so on a FILM it does not appear at all.
  //
  // STATUS: it comes from TMDB and only on a series. Stamping "RELEASED" on everything
  // would repeat the mistake that already removed the fixed "14" rating and the demo
  // cast from here.
  //
  // DURATION on a FILM only. On a series `meta`'s second field is the season count
  // ("3 seasons"), and the reference does not show it in the hero — what counts the
  // seasons are the tabs just below the fold, which this screen already draws.
  // Repeating the information here would be adding back what the device took away.
  {
    float x = NV_DETW2_X, yc = yMeta2 + NV_DETW2_BADGE_H * 0.5f;
    int something = 0;
    // The status badge opens line 2 the way the lockup opens line 1 — the duration and
    // the country sit to its right — so it gets a held slot on the same terms. On a
    // FILM there is never a badge, and statusWord answers that without waiting.
    const char *status = statusWord();
    int statHold = holdFor(status != NULL, isSeries() && pendingExtras());
    int statBusy = statHold || (status && skelStat < 0.999f);
    if (status && skelStat > 0.001f) {
      float wStat = drawBadgeMeta(x, yMeta2, status, NULL, a * skelStat);
      if (skelStat >= 0.999f) { x += wStat; something = 1; }
    }
    if (statBusy) {
      drawSkel(x, yc, NV_DETW2_SKEL_STAT, NV_DETW2_BADGE_H * 0.62f,
               a * (1.0f - skelStat));
      x += NV_DETW2_SKEL_STAT; something = 1;
    }
    if (!isSeries() && duration[0]) {
      if (something) { drawSep(x + NV_DETW2_SEP, yc, 0.502f, a);
                  x += NV_DETW2_SEP * 2 + NV_DETW2_BAR_W; }
      TxtLine ld = txt_line(TXT_DET_META2, duration, 255, 255, 255, 255);
      txt_draw_alpha(ld, x, yc - ld.h * 0.5f, a);
      x += ld.w; something = 1;
    }
    if (ci && ci->country[0]) {
      if (something) { drawSep(x + NV_DETW2_SEP, yc, 0.502f, a);
                  x += NV_DETW2_SEP * 2 + NV_DETW2_BAR_W; }
      TxtLine lp = txt_line(TXT_DET_META2, ci->country, 255, 255, 255, 255);
      txt_draw_alpha(lp, x, yc - lp.h * 0.5f, a);
    }
  }
}

// ---------------------------------------------------------------------------
// THE PAGE: seasons, episodes, information tabs, cast
// ---------------------------------------------------------------------------

// The REAL season number at position `c`. A series that starts at 2 (which happens when
// Cinemeta does not have season 1) showed "Season 1" pointing at 2, and the list below
// did not match the label.
// HOW MANY SEASONS, which sectionN no longer answers: that returns 1, the picker.
static int nSeasonsOf(void) {
  const CatItem *ci = cat_item(idx);
  if (!isSeries()) return 0;
  if (ci && ci->nSeasons > 0)
    return ci->nSeasons < N_ITEMS ? ci->nSeasons : N_ITEMS;
  return 0;
}

static int seasonIn(int c) {
  const CatItem *ci = cat_item(idx);
  if (ci && ci->nSeasons > 0)
    return (c >= 0 && c < ci->nSeasons) ? ci->seasons[c] : ci->seasons[0];
  return c + 1;
}
// THE EPISODES OF ONE SEASON. The catalogue holds EVERY season's episodes in one flat
// list — Cinemeta's `videos` comes whole — and the row used to draw all of them: on
// Fallout that is 16 cards under a picker that said "Season 1", with season 2's episodes
// appended after season 1's. Switching season only moved the FOCUS to the first card of
// that season (goToSeason), which is the Apple TV app's single-track model and not the
// web's: `.series-episode-track` there holds the chosen season and nothing else.
//
// These two map a season-relative index onto that flat list, and every caller that used
// to index it directly now goes through them.
static int nEpsOfSeason(int s2) {
  int q = cat_n_episodes(idx), n = 0;
  for (int i = 0; i < q; i++) {
    const CatEp *e = cat_episode(idx, i);
    if (e && e->season == s2) n++;
  }
  return n;
}
static const CatEp *epOfSeason(int s2, int i) {
  int q = cat_n_episodes(idx), n = 0;
  for (int k = 0; k < q; k++) {
    const CatEp *e = cat_episode(idx, k);
    if (e && e->season == s2 && n++ == i) return e;
  }
  return NULL;
}
// The season on stage, as a REAL season number.
static int seasonNow(void) { return seasonIn(season); }

static void labelSeason(int c, char *dst, size_t n) {
  int s = seasonIn(c);
  if (s == 0) snprintf(dst, n, "Specials");
  else snprintf(dst, n, "Season %d", s);
}
// How many episodes the season at `c` has. The picker's label carries it (" · 8 Eps")
// and it is the only place on this screen that says how long a season is.
static int epsInSeason(int c) {
  int s2 = seasonIn(c);
  // THE CATALOGUE FIRST, because it is the list actually on screen. It used to fall
  // back to cat_n_episodes(idx), which is every season's episodes added together: on
  // Fallout the picker read "Season 1 · 16 Eps" for a season of 8, and season 2 got no
  // count at all because the fallback only applied to the chosen one.
  int n = nEpsOfSeason(s2);
  if (n > 0) return n;
  // Trakt's season list, for a season whose episodes have not been fetched yet.
  for (int t = 0; t < extras_n_seasons(); t++)
    if (extras_season_number(t) == s2) return extras_n_eps(t);
  return 0;
}

// "Season 3" and, separately, " · 8 Eps". They are two styles on one line — 600 white
// and 400 grey — so the caller draws them in two passes and this only builds the text.
static void labelSeasonEps(int c, char *tail, size_t n) {
  int q = epsInSeason(c);
  if (q > 0) snprintf(tail, n, "· %d Eps", q);
  else       tail[0] = 0;
}

// The picker's width: padding + the label + the gap + the chevron + padding. It is the
// WIDEST season's, not the chosen one's, so the control does not resize as you scroll
// the list — the web's is a flex item over a fixed set of options and does the same.
static float widthSeason(int c) {
  (void)c;
  float widest = 0.0f;
  int n = nSeasonsOf();
  for (int i = 0; i < n; i++) {
    char rot[32], tail[24];
    labelSeason(i, rot, sizeof rot);
    labelSeasonEps(i, tail, sizeof tail);
    TxtLine l  = txt_line(TXT_DETWEB_SEA, rot, 255, 255, 255, 255);
    TxtLine lt = tail[0] ? txt_line(TXT_DETWEB_SEA_EPS, tail, 179, 179, 179, 255)
                         : (TxtLine){0};
    float w = l.w + (lt.w > 0.0f ? NV_DETWEB_SEA_TAIL + lt.w : 0.0f);
    if (w > widest) widest = w;
  }
  return NV_DETWEB_SEA_PADX * 2 + widest + NV_DETWEB_SEA_GAP + NV_DETWEB_SEA_CHEV;
}
static float widthTabInfo(int i) {
  TxtLine l = txt_line(TXT_PLR_BODY, TAB_LABEL[tabIdOf(i)], 255, 255, 255, 255);
  return l.w;
}

// THE SEASON PICKER — a DROPDOWN, which is what NuvioWeb has.
//
// It was a row of one pill per season, from the Apple TV app. Two things were wrong
// with that beyond the look: the web's `.series-season-row` holds a single
// `.library-picker` with `aria-haspopup="listbox"`, and a series with eight seasons
// pushed the row off the right of the screen with no way to see the far end.
//
// The anchor at rest: 80 tall, a full pill, #222 with a 1px rgba(255,255,255,.1) hair
// line, the season 30/600 white and the " · N Eps" tail 30/400 grey, then a 32px
// chevron in the same grey.
//
// FOCUSED it does NOT ring outwards like the hero's buttons: the background lifts to
// rgb(48,48,48) and the ring is INSET — `box-shadow: inset 0 0 0 3px rgba(255,255,255,
// .96)`. Measured in both the closed and the open state, and it is the same in both.
static void drawSeason(GfxRect r, int c, float f, float a) {
  (void)c;
  char rot[32], tail[24];
  labelSeason(season, rot, sizeof rot);
  labelSeasonEps(season, tail, sizeof tail);

  { float luma = NV_DETWEB_REST + (NV_DETWEB_SEA_FOCUS_BG - NV_DETWEB_REST) * f;
    gfx_color(r, NV_RADIUS_PILL, luma, luma, luma, a); }
  // The hair line at rest, which the inset focus ring replaces rather than sits on.
  if (f < 0.99f)
    gfx_rect(r, 0, GFX_RING, 0, NV_DETWEB_SEA_BORDER / r.h, 0, NV_RADIUS_PILL,
             1, 1, 1, 0.10f * (1.0f - f) * a);
  if (f > 0.01f) {
    // INSET, and it needs a mode of its own. GFX_RING strokes ACROSS the quad's edge,
    // so on a pill — whose outline touches the quad at twelve and six o'clock — the
    // outer half of the stroke is clipped exactly there and the ring comes out with
    // flat bites off the top and bottom. Insetting the quad by half the stroke, which
    // is what this did, just carries the problem inward. GFX_RING_INSET keeps the whole
    // band inside the edge, where there is nothing to clip.
    gfx_rect(r, 0, GFX_RING_INSET, 0, NV_DETWEB_SEA_RING / r.h, 0,
             NV_RADIUS_PILL, 1, 1, 1, 0.96f * f * a);
  }

  { float x = r.x + NV_DETWEB_SEA_PADX;
    TxtLine l = txt_line(TXT_DETWEB_SEA, rot, 255, 255, 255, 255);
    txt_weight(l, x, r.y + (r.h - l.h) * 0.5f, a, 1.0f);
    x += l.w + NV_DETWEB_SEA_TAIL;
    if (tail[0]) {
      TxtLine lt = txt_line(TXT_DETWEB_SEA_EPS, tail, 179, 179, 179, 255);
      txt_draw_alpha(lt, x, r.y + (r.h - lt.h) * 0.5f, a);
    } }
  // The chevron is the web's own SVG rasterised; it points DOWN closed and the web does
  // not flip it when open, so neither does this.
  { GfxRect ch = { r.x + r.w - NV_DETWEB_SEA_PADX - NV_DETWEB_SEA_CHEV,
                   r.y + (r.h - NV_DETWEB_SEA_CHEV) * 0.5f,
                   NV_DETWEB_SEA_CHEV, NV_DETWEB_SEA_CHEV };
    gfx_icon(ch, "chevron_down", 0.702f, 0.702f, 0.702f, a); }
}

// The OPEN list. It is drawn LAST of everything on the page, over the episode row it
// covers — the web gives it `z-index` and a shadow for exactly that reason.
//
// It hangs 8 below the anchor, matches its width, and scrolls when the seasons run past
// six rows (the web caps the menu at 540px, which is 6.4 rows of 84).
static void drawSeasonMenu(GfxRect anchor, float a) {
  int n = nSeasonsOf();
  if (n <= 0) return;
  int vis = n < NV_DETWEB_SEA_OPT_VIS ? n : NV_DETWEB_SEA_OPT_VIS;
  float h = NV_DETWEB_SEA_MENU_PADY * 2 + vis * NV_DETWEB_SEA_OPT_H;
  GfxRect box = { anchor.x, anchor.y + anchor.h + NV_DETWEB_SEA_MENU_GAP,
                  anchor.w, h };
  // The radius is 64 CSS px on a box ~182 tall, and gfx normalises the radius to the
  // HEIGHT — so it is 64/h here and NOT NV_RADIUS_PILL. On the anchor and the options
  // the pill constant IS right, because CSS clamps a 64 radius to half of an 80- or
  // 84-tall box; on a box this tall it does not, and 0.5 would round the menu into a
  // lozenge.
  float radius = 64.0f / box.h;
  // The drop shadow first, then the plate: `0 8px 32px rgba(0,0,0,.6)` under a menu
  // that sits on a still, without which the #222 plate and a dark thumbnail merge.
  //
  // GFX_SHADOW multiplies by uFOCUS, not just by the colour's alpha (gfx.c:146). Passed
  // the 0 that every other mode here takes for `focus`, the blot comes out completely
  // invisible — profile.c is the only other caller and it passes its focus value.
  { GfxRect sh = { box.x, box.y + 8.0f, box.w, box.h };
    gfx_rect(sh, 0, GFX_SHADOW, 1.0f, 0, 0, radius, 0, 0, 0, 0.6f * a); }
  gfx_color(box, radius, NV_DETWEB_REST, NV_DETWEB_REST, NV_DETWEB_REST, a);
  gfx_rect(box, 0, GFX_RING, 0, 1.0f / box.h, 0, radius, 1, 1, 1, 0.08f * a);

  // Which six. The focused option is kept in view by scrolling the window, not by
  // moving the menu.
  int first = seasonMenuFocus - vis + 1;
  if (first < 0) first = 0;
  if (first > n - vis) first = n - vis;
  if (seasonMenuFocus < first) first = seasonMenuFocus;

  for (int i = 0; i < vis; i++) {
    int c = first + i;
    char rot[32], tail[24];
    labelSeason(c, rot, sizeof rot);
    labelSeasonEps(c, tail, sizeof tail);
    GfxRect op = { box.x + NV_DETWEB_SEA_MENU_PADX,
                   box.y + NV_DETWEB_SEA_MENU_PADY + i * NV_DETWEB_SEA_OPT_H,
                   box.w - NV_DETWEB_SEA_MENU_PADX * 2, NV_DETWEB_SEA_OPT_H };
    int on = (c == seasonMenuFocus);
    // The focused row inverts to #f5f5f5 with #111 ink; the rest are transparent with
    // white. The SELECTED season gets no mark of its own — the list opens with the
    // focus already on it, which is how the web shows which one you are on.
    if (on) gfx_color(op, NV_RADIUS_PILL, NV_DETWEB_FOCUS, NV_DETWEB_FOCUS,
                      NV_DETWEB_FOCUS, a);
    { int ink = on ? 17 : 255;
      int grey = on ? 90 : 179;
      float x = op.x + NV_DETWEB_SEA_OPT_PADX;
      TxtLine l = txt_line(TXT_DETWEB_OPT, rot, ink, ink, ink, 255);
      txt_draw_alpha(l, x, op.y + (op.h - l.h) * 0.5f, a);
      x += l.w + NV_DETWEB_SEA_TAIL;
      if (tail[0]) {
        TxtLine lt = txt_line(TXT_DETWEB_OPT, tail, grey, grey, grey, 255);
        txt_draw_alpha(lt, x, op.y + (op.h - lt.h) * 0.5f, a);
      } }
  }
}

// THE EPISODE'S SCORE in tenths, and whether it is IMDb's.
//
// Cinemeta answers `"rating": "0"` for a great many series — every Fallout, Game of
// Thrones and Stranger Things episode does, while Breaking Bad and Friends carry real
// numbers. Drawing 0.0 ports a defect and omitting the score leaves those titles with
// none, so a 0 falls back to the TRAKT score extras already fetched — and the caller
// labels it as Trakt's, because an IMDb label on a Trakt number misleads.
static int episodeScore(const CatEp *ep, int *fromImdb) {
  *fromImdb = 0;
  if (!ep) return 0;
  if (ep->imdb > 0) { *fromImdb = 1; return ep->imdb; }
  for (int st = 0; st < extras_n_seasons(); st++) {
    if (extras_season_number(st) != ep->season) continue;
    for (int ei = 0; ei < extras_n_eps(st); ei++)
      if (extras_ep_number(st, ei) == ep->episode) return extras_ep_score(st, ei);
    break;
  }
  return 0;
}

// How far into `ep` the viewer is, 0..100. Only ONE episode ever carries it: the one
// the title's progress record names.
static int episodeProgress(const CatEp *ep) {
  const CatItem *ci = cat_item(idx);
  if (ci && ep && ci->progress > 0 && ci->season == ep->season &&
      ci->episode == ep->episode) return ci->progress;
  return 0;
}
// Between 2% and 98%: under that the episode has not really started, over it it is as
// good as finished.
static int episodeStarted(const CatEp *ep) {
  int p = episodeProgress(ep);
  return p > 2 && p < 98;
}

// THE FOCUSED ROW'S RIGHT-HAND COLUMN, laid out once: the Resume / Play pill ending at
// NV_DETEP_RIGHT and, when there is some, the time left beside it. The row's HEIGHT
// wraps the synopsis to the width this leaves, so the height and the drawing have to
// read the same numbers.
typedef struct {
  TxtLine label, left;    // `left.w` is 0 when there is no time to show
  GfxRect pill;           // x and w only; the caller centres it on the row
  float xStart;           // the column's left edge
} EpActions;
static void episodeActions(const CatEp *ep, EpActions *o) {
  int started = episodeStarted(ep);
  int ink = (int)(NV_DETWEB_FOCUS_INK * 255.0f + 0.5f);
  o->label = txt_line(TXT_ROW_TITLE, started ? "Resume" : "Play", ink, ink, ink, 255);
  o->pill.w = NV_DETEP_BTN_PADX * 2 + NV_DETEP_BTN_ICON + NV_DETEP_BTN_GAPI
            + o->label.w;
  o->pill.x = NV_DETEP_RIGHT - o->pill.w;
  o->pill.y = 0.0f; o->pill.h = NV_DETEP_BTN_H;
  o->xStart = o->pill.x;
  o->left = (TxtLine){0};
  { int total = ep ? atoi(ep->duration) : 0;
    if (started && total > 0) {
      char minutes[24];
      int rest = total - total * episodeProgress(ep) / 100;
      snprintf(minutes, sizeof minutes, "%d min left", rest < 1 ? 1 : rest);
      o->left = txt_line(TXT_DETWEB_EP_META, minutes, 200, 200, 200, 255);
      o->xStart -= NV_DETEP_LEFT_GAP + o->left.w;
    } }
}

static float episodeCopyX(void) {
  return NV_DETP_X + NV_DETEP_THUMB_W + NV_DETEP_TEXT_GAP;
}
// Where the copy has to stop: short of the pill on the focused row, short of the
// watched mark on the others.
static float episodeCopyRight(const CatEp *ep, int focused) {
  if (!focused) return NV_DETEP_RIGHT - NV_DETEP_CHECK - NV_DETEP_ACT_GAP;
  EpActions ac; episodeActions(ep, &ac);
  return ac.xStart - NV_DETEP_ACT_GAP;
}
// The copy's INK height, from the title's cap top to the last baseline — which is what
// the eye centres, not the text boxes and their air.
static float episodeInkH(int lines) {
  float h = txt_baseline(TXT_DETWEB_EP_TITLE) - txt_cap_inset(TXT_DETWEB_EP_TITLE)
          + NV_DETEP_META_DY;
  if (lines > 0) h += NV_DETEP_DESC_DY + (lines - 1) * NV_DETEP_DESC_LD;
  return h;
}
// How many lines the WHOLE synopsis takes on the focused row.
static int episodeFullLines(const CatEp *ep) {
  if (!ep || !ep->synopsis[0]) return 0;
  return txt_block_lines(TXT_DETWEB_EPD, ep->synopsis,
                         episodeCopyRight(ep, 1) - episodeCopyX());
}
// The row's height focused: its whole synopsis plus the air, never under the resting
// height.
static float episodeFullH(int c) {
  float h = episodeInkH(episodeFullLines(epOfSeason(seasonNow(), c)))
          + NV_DETEP_PADY * 2.0f;
  return h > NV_DETEP_ROW_H ? h : NV_DETEP_ROW_H;
}
// The height it has THIS frame, on its focus spring, so the rows under a row that is
// opening slide down with it instead of jumping.
static float episodeRowH(int c) {
  float f = animFocus[SEC_EPISODES][c];
  if (f < 0.001f) return NV_DETEP_ROW_H;
  return NV_DETEP_ROW_H + (episodeFullH(c) - NV_DETEP_ROW_H) * f;
}

// The "·" between two items of the meta line. It is only drawn BETWEEN items, so the
// caller passes whether one is already on the line.
static float metaDot(float x, float y, int any, float a) {
  if (!any) return x;
  int dim = (int)(255.0f * NV_HERO_META_DOT + 0.5f);
  TxtLine ld = txt_line(TXT_DETWEB_EP_META, "\xc2\xb7", dim, dim, dim, 255);
  txt_draw_alpha(ld, x + NV_DETEP_DOT_SEP, y, a);
  return x + NV_DETEP_DOT_SEP * 2.0f + ld.w;
}

// The meta line: duration · date · score, from `x`, on the baseline `base`. Every item
// omits itself when it has nothing to say — there is no invented fallback.
//
// The score is the IMDb MARK on the focused row and plain words ("IMDb 8.3") on the
// others, as in the reference: a column of yellow badges down a list pulls the eye to
// the one thing on each row that matters least.
static void drawEpisodeMeta(const CatEp *ep, float x, float base, float xEnd,
                            int focused, float a) {
  int any = 0;
  float y = base - txt_baseline(TXT_DETWEB_EP_META);
  if (!ep) return;
  if (ep->duration[0]) {
    TxtLine l = txt_line(TXT_DETWEB_EP_META, ep->duration, 175, 175, 175, 255);
    txt_draw_alpha(l, x, y, a);
    x += l.w; any = 1;
  }
  // The date SPELLED OUT ("9 August 2025"), or the year alone when the setting says so.
  if (ep->date[0]) {
    const char *date = ep->date;
    size_t nDate = strlen(date);
    if (!settings_date_full() && nDate >= 4) date += nDate - 4;
    x = metaDot(x, y, any, a);
    TxtLine l = txt_line_trim(TXT_DETWEB_EP_META, date, 175, 175, 175, 255, xEnd - x);
    txt_draw_alpha(l, x, y, a);
    x += l.w; any = 1;
  }
  { int fromImdb, score = episodeScore(ep, &fromImdb);
    char value[8];
    if (score <= 0 || xEnd - x < 120.0f) return;
    snprintf(value, sizeof value, "%d.%d", score / 10, score % 10);
    x = metaDot(x, y, any, a);
    if (focused && fromImdb) {
      // The real mark, at NV_IMDB_MARK_TEX_W like every other caller: two widths for
      // one file is a decode on every frame. GFX_TEXT, because GFX_BRAND would flatten
      // its yellow and black into one tint. Centred on the line's capitals.
      float capMid = base - (txt_baseline(TXT_DETWEB_EP_META)
                             - txt_cap_inset(TXT_DETWEB_EP_META)) * 0.5f;
      const char *fileMark = gfx_icon_path("imdb_logo");
      float arMark = tex_aspect(fileMark);
      GLuint markImdb = tex_get_exact(fileMark, NV_IMDB_MARK_TEX_W);
      GfxRect brand = { x, capMid - NV_DETEP_IMDB_H * 0.5f, 0.0f, NV_DETEP_IMDB_H };
      brand.w = brand.h * (arMark > 0.0f ? arMark : NV_IMDB_MARK_AR);
      if (markImdb) gfx_rect(brand, markImdb, GFX_TEXT, 0, 0, 0, 0.0f, 1, 1, 1, a);
      x += brand.w + NV_DETEP_IMDB_GAP;
      // Beside the mark the score takes the mark's own yellow, rgb(245,197,24) — the
      // same pairing the web measures on `.series-imdb-badge` and its span.
      TxtLine l = txt_line(TXT_DETWEB_EP_META, value, 245, 197, 24, 255);
      txt_draw_alpha(l, x, y, a);
    } else {
      char words[24];
      snprintf(words, sizeof words, "%s %s", fromImdb ? "IMDb" : "Trakt", value);
      TxtLine l = txt_line(TXT_DETWEB_EP_META, words, 175, 175, 175, 255);
      txt_draw_alpha(l, x, y, a);
    } }
}

// ONE ROW of the episode list: `y` on screen, `h` its height this frame.
//
// `f` is the row's focus spring, and everything that CAN travel follows it: the band,
// the ring, the pill, the row's height and where the copy sits in it. The synopsis
// crossfades — the one clipped line out, the whole text in — because a block of text
// cannot be half-wrapped; its first line lands where the clipped one was, so only the
// lines below it appear.
static void drawEpisodeRow(float y, float h, int c, float f, float a) {
  const CatEp *ep = epOfSeason(seasonNow(), c);
  int focused = level >= 1 && focus.row == SEC_EPISODES && focus.column == c;
  float rowA = a * (epListA + (1.0f - epListA) * f);
  float mid = y + h * 0.5f;

  // The band: from the screen's left edge, dissolving to the right (GFX_ROW_FADE) so
  // there is no line where it stops.
  if (f > 0.01f) {
    GfxRect band = { 0.0f, y, NV_SCREEN_W, h };
    gfx_rect(band, 0, GFX_ROW_FADE, 0, NV_DETEP_BAND_FADE, 0, 0.0f,
             1, 1, 1, NV_DETEP_BAND_A * f * a);
  }

  GfxRect th = { NV_DETP_X, mid - NV_DETEP_THUMB_H * 0.5f,
                 NV_DETEP_THUMB_W, NV_DETEP_THUMB_H };
  float radiusTh = NV_DETEP_THUMB_R / th.h;
  if (f > 0.01f) {
    float ring = NV_DETEP_RING;
    GfxRect ro = { th.x - ring, th.y - ring, th.w + ring * 2, th.h + ring * 2 };
    gfx_color(ro, (NV_DETEP_THUMB_R + ring) / ro.h, 1, 1, 1, f * a);
  }
  { const CatItem *series = cat_item(idx);
    const char *art = (ep && ep->thumb[0]) ? ep->thumb
                       : (series && series->backdrop[0] ? series->backdrop : NULL);
    GLuint t2 = art ? tex_get_width(art, NV_DETEP_THUMB_W) : 0;
    if (t2) {
      gfx_tex_aspect_current = tex_aspect(art);
      gfx_rect(th, t2, GFX_CARD, 0, 0, 0, radiusTh, 0, 0, 0, rowA);
      gfx_tex_aspect_current = 0.0f;
    } else gfx_color(th, radiusTh, 0.133f, 0.133f, 0.133f, rowA); }

  // The Continue Watching card's bar, along the thumbnail's base and cut by its corner
  // (GFX_CW_BAR — see gfx.h).
  if (episodeStarted(ep)) {
    float fill = anim_clamp(episodeProgress(ep) / 100.0f, 0.0f, 1.0f);
    float min  = NV_CW_BAR_MINW / th.w;
    if (fill < min) fill = min;
    gfx_rect(th, 0, GFX_CW_BAR, 0, NV_CW_BAR_H / th.h, fill, radiusTh, 1, 1, 1, rowA);
  }

  // --- the right-hand column ---------------------------------------------------
  if (f > 0.01f) {
    EpActions ac; episodeActions(ep, &ac);
    float aAct = f * a;
    GfxRect pill = { ac.pill.x, mid - NV_DETEP_BTN_H * 0.5f, ac.pill.w, NV_DETEP_BTN_H };
    gfx_color(pill, NV_RADIUS_PILL, NV_DETWEB_FOCUS, NV_DETWEB_FOCUS,
              NV_DETWEB_FOCUS, aAct);
    { GfxRect ic = { pill.x + NV_DETEP_BTN_PADX, mid - NV_DETEP_BTN_ICON * 0.5f,
                     NV_DETEP_BTN_ICON, NV_DETEP_BTN_ICON };
      gfx_icon(ic, "detail_play", NV_DETWEB_FOCUS_INK, NV_DETWEB_FOCUS_INK,
               NV_DETWEB_FOCUS_INK, aAct); }
    txt_draw_alpha(ac.label, pill.x + NV_DETEP_BTN_PADX + NV_DETEP_BTN_ICON
                             + NV_DETEP_BTN_GAPI, mid - ac.label.h * 0.5f, aAct);
    if (ac.left.w > 0.0f)
      txt_draw_alpha(ac.left, ac.xStart, mid - ac.left.h * 0.5f, aAct);
  }
  // THE WATCHED MARK. `ep_watched` is a disc with the tick KNOCKED OUT, so a light
  // circle under a dark glyph shows the tick in the circle's colour: a dim disc with a
  // grey tick. It is not dimmed with the row — at the row's rest opacity it would all
  // but vanish, and it is already quiet by design.
  if (ep && extras_ep_watched(ep->season, ep->episode) && f < 0.99f) {
    float aChk = a * (1.0f - f);
    GfxRect st = { NV_DETEP_RIGHT - NV_DETEP_CHECK, mid - NV_DETEP_CHECK * 0.5f,
                   NV_DETEP_CHECK, NV_DETEP_CHECK };
    gfx_color(st, 0.5f, 0.55f, 0.55f, 0.55f, aChk);
    gfx_icon_at(st, "ep_watched", NV_DETEP_CHECK, 0.16f, 0.16f, 0.16f, aChk);
  }

  // --- the copy, on baselines ----------------------------------------------------
  float tx = episodeCopyX();
  float right = episodeCopyRight(ep, focused);
  const char *syn = (ep && ep->synopsis[0]) ? ep->synopsis : NULL;
  int full = focused ? episodeFullLines(ep) : 0;
  float cap = txt_baseline(TXT_DETWEB_EP_TITLE) - txt_cap_inset(TXT_DETWEB_EP_TITLE);
  // The title's baseline, with the copy's ink centred on the row: at rest for one
  // synopsis line, focused for all of them, and travelling between the two on `f`.
  float base1 = mid - episodeInkH(syn ? 1 : 0) * 0.5f + cap;
  float baseN = mid - episodeInkH(full) * 0.5f + cap;
  float base = focused ? base1 + (baseN - base1) * f : base1;

  // "EP3" and the title, on ONE BASELINE: the kicker is 21 and the title 34. No space
  // inside the kicker — the tracking already opens the letters up, and a word space
  // on top of it split "EP" from its number.
  { char kick[16], fallback[32];
    int epNum = ep ? ep->episode : c + 1;
    snprintf(kick, sizeof kick, "EP%d", epNum);
    const char *name = (ep && ep->name[0]) ? ep->name : NULL;
    // With no name, "Episode N" — a true label from the number, never another
    // series' text.
    if (!name) { snprintf(fallback, sizeof fallback, "Episode %d", epNum);
                 name = fallback; }
    float wKick = txt_tracking(TXT_DETWEB_EP_BADGE, kick, 170, 170, 170,
                               -1.0f, 0.0f, 0.0f, NV_DETEP_KICK_LS);
    float xTitle = tx + wKick + NV_DETEP_KICK_GAP;
    txt_tracking(TXT_DETWEB_EP_BADGE, kick, 170, 170, 170,
                 tx, base - txt_baseline(TXT_DETWEB_EP_BADGE), rowA, NV_DETEP_KICK_LS);
    TxtLine lt = txt_line_trim(TXT_DETWEB_EP_TITLE, name, 255, 255, 255, 255,
                               right - xTitle);
    txt_draw_alpha(lt, xTitle, base - txt_baseline(TXT_DETWEB_EP_TITLE), rowA); }

  drawEpisodeMeta(ep, tx, base + NV_DETEP_META_DY, right, focused, rowA);

  if (syn) {
    float yD = base + NV_DETEP_META_DY + NV_DETEP_DESC_DY
             - txt_baseline(TXT_DETWEB_EPD);
    float aFull = focused ? f : 0.0f;
    if (aFull < 0.99f) {
      float restRight = episodeCopyRight(ep, 0);
      TxtLine l = txt_line_trim(TXT_DETWEB_EPD, syn, 170, 170, 170, 255,
                                restRight - tx);
      txt_draw_alpha(l, tx, yD, rowA * (1.0f - aFull));
    }
    if (aFull > 0.01f)
      txt_block(TXT_DETWEB_EPD, syn, 225, 225, 225, tx, yD, right - tx,
                NV_DETEP_DESC_LD, rowA * aFull, 0);
  }
}

// The list: a window from NV_DETP_EP_Y to the page's end, the rows stacking down it
// from scrollSec[SEC_EPISODES]. CLIPPED at both ends: at the top to the picker's base,
// and at the bottom to the page's end, because page 3 must not show a row while the
// page scrolls.
//
// ROWS DISSOLVE AS THEY LEAVE, the way Discover's grid does (anim_edge): across the
// NV_DETEP_LIST_GAP of air between the picker and the first row, a row going up fades
// to nothing, so it is gone before the clip would guillotine it against the picker.
// The ramp reads the row's y against the LIST's own top, not the screen, so the page
// scrolling in or out moves both together and fades nothing.
static void drawEpisodeList(float y, float a) {
  int n = sectionN(SEC_EPISODES);
  float top = y - NV_DETEP_LIST_GAP;
  float c0 = top < 0.0f ? 0.0f : top;
  float c1 = y + NV_DETEP_LIST_H;
  if (c1 > NV_SCREEN_H) c1 = NV_SCREEN_H;
  if (n <= 0 || c1 <= c0) return;
  regionAdd("episodes", (GfxRect){ 0.0f, c0, NV_SCREEN_W, c1 - c0 });
  gfx_crop(0.0f, c0, NV_SCREEN_W, c1 - c0);
  float ry = y - scrollSec[SEC_EPISODES];
  for (int c = 0; c < n && c < N_ITEMS && ry < c1; c++) {
    float h = episodeRowH(c);
    float edge = anim_edge(ry, top, NV_DETEP_LIST_GAP);
    if (ry + h > c0 && edge > 0.004f) {
      gfx_opacity_group = edge;
      drawEpisodeRow(ry, h, c, animFocus[SEC_EPISODES][c], a);
      gfx_opacity_group = 1.0f;
    }
    ry += h;
  }
  gfx_no_crop();
}

// Information tabs: plain text, with no pill. The chosen one (or the focused one) in
// white, the others in #808080; the "|" divider is 32/700 #808080. Focus in the web app
// is `transform: scale(1.03)` — the only place on this screen that scales.
static void drawTabInfo(float x, float y, int i, float f, float a) {
  int sel = (i == tabInfo);
  int base = sel ? 255 : 128;
  int color = (int)(base + (255 - base) * f);
  TxtLine l = txt_line(TXT_PLR_BODY, TAB_LABEL[tabIdOf(i)], color, color, color, 255);
  txt_weight(l, x, y + (NV_DETP_TAB_H - l.h) * 0.5f, a, 0.5f + f * 0.6f);
}

// Cast: a round 140 avatar ALIGNED LEFT in the 220 card (not centred, which was the
// previous design), the name 26/500 rgb(179,179,179) and the role 21/400
// rgb(128,128,128) below it.
// --- the TRAILER card --------------------------------------------------------
//
// A 520x292 thumbnail with radius 24, a play badge in the centre, the name below and
// the type in grey. The thumbnail comes from img.youtube.com by a predictable URL, and
// tex_get downloads and caches it by itself — there is no network code here.
//
// IT IS NOT FOCUSABLE, and that is a decision, not an omission: this app has no YouTube
// player. The same rule already removed the trailer button from the hero (detail.c) and
// the YouTube glyph from the third circular button (gfx.c) — a control that promises
// what it cannot deliver is worse than its absence. The card takes part in the
// composition so the page does not lie about what the film has; open, it does not.
static void drawTrailer(float x, float y, int c, float a) {
  const char *mini = extras_trailer_thumb(c);
  GfxRect v = { x, y, NV_DETF_TR_W, NV_DETF_TR_VIDEO_H };
  float radius = NV_DETF_TR_RADIUS / NV_DETF_TR_VIDEO_H;   // a fraction of the SMALLER side
  GLuint tex = (mini && mini[0]) ? tex_get_width(mini, NV_DETF_TR_W) : 0;

  if (tex) {
    gfx_tex_aspect_current = tex_aspect(mini);
    gfx_rect(v, tex, GFX_CARD, 0, 0, 0, radius, 1, 1, 1, a);
    gfx_tex_aspect_current = 0.0f;
  } else {
    gfx_color(v, radius, 0.13f, 0.13f, 0.13f, a);
  }

  // The play badge: a dark disc with the triangle over it, centred on the thumbnail.
  { float d = NV_DETF_TR_PLAY_D;
    GfxRect disk = { x + (NV_DETF_TR_W - d) * 0.5f,
                      y + (NV_DETF_TR_VIDEO_H - d) * 0.5f, d, d };
    GfxRect tri   = { disk.x + d * 0.34f, disk.y + d * 0.28f,
                      d * 0.36f, d * 0.44f };
    gfx_color(disk, 0.5f, 0.0f, 0.0f, 0.0f, a * 0.48f);
    gfx_rect(tri, 0, GFX_PLAY, 0, 0, 0, 0.0f, 1, 1, 1, a); }

  { TxtLine ln = txt_line_trim(TXT_ROW_TITLE, extras_trailer_name(c),
                                  245, 248, 255, 255, NV_DETF_TR_W);
    txt_draw_alpha(ln, x, y + NV_DETF_TR_NAME_DY, a); }
  { TxtLine lt = txt_line(TXT_CAPTION2, "YouTube", 179, 179, 179, 255);
    txt_draw_alpha(lt, x, y + NV_DETF_TR_KIND_DY, a * 0.9f); }
}

// --- the "Movie Details" table ----------------------------------------------
//
// Two columns: the key in grey on the left, the value in white in a FIXED column.
// The value's column does not follow the key's width — if it did, every row would start
// at a different x and the table would jag. A 1px divider under every row but the last,
// as in the reference.
//
// It takes `f` only to know whether the section is focused: the table has no per-item
// focus, so the focus on it is the section itself, and the highlight is deliberately
// subtle —
static void drawDetails(float x, float y, float f, float a) {
  LineDet l[NV_DETF_DET_MAXL];
  int n = buildDetails(l, NV_DETF_DET_MAXL), i;
  for (i = 0; i < n; i++) {
    float ly = y + i * NV_DETF_DET_LINE;
    float yc = ly + NV_DETF_DET_LINE * 0.5f;
    TxtLine lk = txt_line(TXT_DET_META2, l[i].key, 150, 154, 163, 255);
    TxtLine lv = txt_line_trim(TXT_DET_META, l[i].value, 235, 238, 245, 255,
                                  NV_DETF_DET_W - NV_DETF_DET_KEY_W);
    txt_draw_alpha(lk, x, yc - lk.h * 0.5f, a * 0.9f);
    txt_draw_alpha(lv, x + NV_DETF_DET_KEY_W, yc - lv.h * 0.5f, a);
    if (i < n - 1) {
      GfxRect d = { x, ly + NV_DETF_DET_LINE - 1.0f, NV_DETF_DET_W, 1.0f };
      gfx_color(d, 0.0f, 1, 1, 1, a * (0.10f + 0.06f * f));
    }
  }
}

static void drawCast(float x, float y, int c, float f, float a) {
  const CatItem *ci = cat_item(idx);
  const char *name = NULL, *role = NULL, *photo = NULL;
  if (ci && c < ci->nCast) {
    name = ci->cast[c].name;
    role = ci->cast[c].role;
    if (ci->cast[c].photo[0]) photo = ci->cast[c].photo;
  }
  if (ci && ci->nCast > 0 && c >= ci->nCast) return;
  // WITH NO CAST, DO NOT INVENT A CAST. There used to be a hard-coded fallback here
  // (`CAST[c % N_CAST]`) that filled the row with the cast of "Shrinking" — and the
  // result was Spider-Man crediting Jason Segel and Harrison Ford, looking like real
  // data. The same defect as the "14" hard-coded in discover.c: a demo value shown as
  // information.
  //
  // The row does not even reach here with no data, because sectionN returns 0 (and
  // focus_move skips an empty row). This `return` is the second lock.
  if (!name || !name[0]) return;

  GfxRect av = { x, y, NV_DETP_EL_AVATAR, NV_DETP_EL_AVATAR };
  if (f > 0.01f) {
    GfxRect ring = { av.x - NV_DETP_RING, av.y - NV_DETP_RING,
                     av.w + NV_DETP_RING * 2, av.h + NV_DETP_RING * 2 };
    gfx_color(ring, 0.5f, 1, 1, 1, f * a);
  }
  GLuint t2 = photo ? tex_get_width(photo, NV_DETP_EL_AVATAR) : 0;
  if (t2) {
    gfx_tex_aspect_current = tex_aspect(photo);
    gfx_rect(av, t2, GFX_CARD, 0, 0, 0, 0.5f, 0, 0, 0, a);
    gfx_tex_aspect_current = 0.0f;
  } else {
    // With no photo, the initial over #222 (#303030 with focus) — which is what the
    // web app does with `.movie-cast-avatar-fallback`.
    float luma = 0.133f + 0.055f * f;
    gfx_color(av, 0.5f, luma, luma, luma, a);
    char start[5] = {0};
    for (int k = 0; k < 4 && name[k] && (unsigned char)name[k] >= 0x20; k++) {
      start[k] = name[k];
      if ((name[k] & 0xC0) != 0x80) { if (k) { start[k] = 0; break; } }
    }
    TxtLine li = txt_line(TXT_TITLE3, start, 210, 212, 220, 255);
    txt_draw_alpha(li, av.x + (av.w - li.w) * 0.5f,
                       av.y + (av.h - li.h) * 0.5f, a * 0.9f);
  }
  float yn = y + NV_DETP_EL_AVATAR + NV_DETP_EL_NAME_DY;
  TxtLine ln = txt_line_trim(TXT_CALLOUT, name, 179, 179, 179, 255, NV_DETP_EL_W);
  txt_draw_alpha(ln, x, yn, a);
  if (role && role[0]) {
    TxtLine lp = txt_line_trim(TXT_CAPTION2, role, 128, 128, 128, 255,
                                  NV_DETP_EL_W);
    txt_draw_alpha(lp, x, yn + NV_DETP_EL_ROLE_DY, a * 0.95f);
  }
}

// The "Ratings" tab. In the web app (metaDetailsScreen.js:3699) these are two cards
// side by side, IMDb and TMDB: a .movie-rating-card of 160x120, radius 14, background
// rgba(18,23,31,.9) with a 1px border at 16%, a 56x28 logo on top and the value in
// 34/800 below; when the data is missing the card shows "-".
//
// DIVERGENCE NOTED: on a SERIES the web app swaps this for a PER-EPISODE ratings panel
// (renderSeriesRatingsPanel), with a season selector. This port has no per-episode
// score from any source — Cinemeta does not return one — so a series shows the same
// two cards as a film. It is not the web app's screen; it is what the data allows, and
// showing two correct cards is better than an empty grid.
#define RATING_CARD_W  160.0f
#define RATING_CARD_H  120.0f
#define RATING_CARD_GAP 16.0f
// A 1px outline at 16%, in four bands: there is no border helper in gfx and this same
// drawing serves the score card and the comment card.
// `radius` in PIXELS; the conversion to the fraction of the smaller side that gfx
// expects is done here. Passing 14 directly (the CSS radius) made the SDF saturate and
// the card came out with square corners — gfx's value is a fraction, not a pixel.
// A POSTER RADIUS, as a fraction of the smaller side — the shader's SDF is normalised.
//
// It exists because five points in this file did `NV_RADIUS_CARD / width`, and
// NV_RADIUS_CARD IS ALREADY A FRACTION (0.055). Dividing again by the width gave
// ~0.0003, that is a square corner: it was why the "Recommendations" posters and the
// filmography photos came out square while the home's were rounded. The value comes
// from the home's own `posterCardCornerRadiusDp`, so the two screens have the same corner.
static float radiusPoster(float w, float h) {
  (void)w;
  if (h <= 0.0f) return NV_RADIUS_CARD;
  return settings_radius_poster_px() / h;
}

static void frame(GfxRect r, float radius, float a) {
  radius = r.h > 0.0f ? radius / r.h : 0.0f;
  // A NEUTRAL GREY, the same #2D2D2D as the season pills and the rest of the
  // components. The dark blue that was here (#12171F) was the only tone of its family
  // on this screen: the comment card read as a piece from another app.
  gfx_color(r, radius, 0.176f, 0.176f, 0.176f, 0.94f * a);
  // THE BORDER FOLLOWS THE CORNER. They used to be FOUR STRAIGHT 1 px RECTANGLES, one
  // per side — they crossed outside the rounding and drew a spike at all four corners,
  // which is what the owner saw as "the border isn't rounded". GFX_RING uses the same
  // SDF as the fill, so the stroke follows the radius.
  gfx_rect(r, 0, GFX_RING, 0, r.h > 0.0f ? 1.0f / r.h : 0.0f, 0, radius,
           1, 1, 1, 0.14f * a);
}

// The score card: the BRAND on top and the value below, like the web app's
// .movie-rating-card (a 56x28 logo, the value 34/800). The brands are the web app's own
// files converted to PNG in art/brands — drawing a coloured rectangle with the
// initials, which is what used to be here, looks like a sketch beside components that
// use real art.
static void cardScore(float x, float y, const char *brand, const char *value,
                       float a) {
  GfxRect card = { x, y, RATING_CARD_W, RATING_CARD_H };
  const char *cam = brand;
  GLuint t;
  frame(card, 14.0f, a);
  // The mark is 28 tall in a card that never resizes, and the files are ~96 tall:
  // tex_get's 640 ceiling left them at full size on the GPU to be drawn a third as
  // wide, which sends the mipmap filter two levels down and draws the badge from a
  // quarter-resolution copy. tex_get_exact asks for the width it is about to use —
  // known here without the aspect, because the width is capped at 96 either way.
  { float ap0 = tex_aspect(cam);
    float w0 = ap0 > 0.0f ? 28.0f * ap0 : 96.0f;
    t = tex_get_exact(cam, w0 > 96.0f ? 96.0f : w0); }
  { TxtLine lv = txt_line(TXT_TITLE3, value, 245, 248, 255, 255);
    float hLogo = 28.0f, hBlock = hLogo + 12.0f + lv.h;
    float yb = y + (RATING_CARD_H - hBlock) * 0.5f;
    if (t) {
      float ap = tex_aspect(cam);
      float w;
      if (ap <= 0.0f) ap = 2.0f;
      w = hLogo * ap;
      if (w > 96.0f) { w = 96.0f; hLogo = w / ap; }
      { GfxRect rl = { x + (RATING_CARD_W - w) * 0.5f, yb, w, hLogo };
        // GFX_CARD and not GFX_TEXT: the text mode paints the shape in the GIVEN
        // COLOUR and throws away the texture's RGB — the IMDb logo would come out as a
        // white silhouette. Here the mark has to keep its own colour.
        gfx_tex_aspect_current = 0.0f;
        gfx_rect(rl, t, GFX_CARD, 0, 0, 0, 0.0f, 0, 0, 0, a); }
    }
    txt_draw_alpha(lv, x + (RATING_CARD_W - lv.w) * 0.5f, yb + 28.0f + 12.0f, a);
  }
}

// THE ROW of scores, in the web app's order: trakt, imdb, tmdb, tomatoes, audience,
// metacritic, letterboxd. Only a source that HAS a score goes in — the web app does the
// same (`.filter(([,,value]) => value != null)`), and a row of "-" says nothing. IMDb
// comes from the catalogue when mdbList did not answer for it.
// AN EPISODE'S SCORE CHIP. The colours and the bands are the web app's
// (ratingToneClass, metaDetailsScreen.js:912, and the .series-episode-rating-chip.*
// rules): >=9 excellent, >=8 great, >=7.5 good, >=7 mixed, >=6 poor, >0 awful. The
// number is the Trakt score, not the IMDb one.
static void colorOfScore(int scoreDec, float *r, float *g, float *b, float *tx) {
  float rr, gg, bb, t;
  if      (scoreDec >= 90) { rr=0.078f; gg=0.643f; bb=0.302f; t=0.97f; }  /* #14a44d */
  else if (scoreDec >= 80) { rr=0.180f; gg=0.733f; bb=0.404f; t=0.97f; }  /* #2ebb67 */
  else if (scoreDec >= 75) { rr=0.243f; gg=0.722f; bb=0.400f; t=0.97f; }  /* #3eb866 */
  else if (scoreDec >= 70) { rr=0.906f; gg=0.706f; bb=0.196f; t=0.10f; }  /* #e7b432 */
  else if (scoreDec >= 60) { rr=0.906f; gg=0.298f; bb=0.235f; t=0.97f; }  /* #e74c3c */
  else if (scoreDec >  0)  { rr=0.388f; gg=0.224f; bb=0.455f; t=0.97f; }  /* #633974 */
  else                    { rr=0.925f; gg=0.816f; bb=0.239f; t=0.09f; }  /* #ecd03d */
  *r = rr; *g = gg; *b = bb; *tx = t;
}

#define RAT_PIL_W    86.0f
#define RAT_PIL_H    62.0f
#define RAT_PIL_GAP  10.0f
#define RAT_TEMP_H   38.0f
#define RAT_TEMP_GAP 10.0f

// A SERIES' panel: the season row and the grid of per-episode chips.
// It is the web app's renderSeriesRatingsPanel, which until now had no source here —
// the series tab fell back to the same cards as the film.
static void drawScoresEpisode(float x, float y, float a) {
  int nt = extras_n_seasons(), t, i, ne;
  if (ratTemp >= nt) ratTemp = 0;
  for (t = 0; t < nt; t++) {
    char rot[8];
    float bx = x + t * (58.0f + RAT_TEMP_GAP);
    GfxRect r = { bx, y, 58.0f, RAT_TEMP_H };
    int sel = (t == ratTemp);
    snprintf(rot, sizeof rot, "S%d", extras_season_number(t));
    gfx_color(r, 0.5f, 1, 1, 1, (sel ? 0.28f : 0.14f) * a);
    { TxtLine l = txt_line(TXT_DET_META2, rot, 241, 247, 254, 255);
      txt_draw_alpha(l, bx + (58.0f - l.w) * 0.5f,
                         y + (RAT_TEMP_H - l.h) * 0.5f, a); }
  }
  ne = extras_n_eps(ratTemp);
  { float gy = y + RAT_TEMP_H + 18.0f;
    for (i = 0; i < ne; i++) {
      float gx = x + i * (RAT_PIL_W + RAT_PIL_GAP);
      int nd = extras_ep_score(ratTemp, i);
      float cr, cg, cb, tx;
      char ep[8], nv[8];
      if (gx + RAT_PIL_W > NV_SCREEN_W - NV_DETP_X) break;
      colorOfScore(nd, &cr, &cg, &cb, &tx);
      gfx_color((GfxRect){ gx, gy, RAT_PIL_W, RAT_PIL_H }, 14.0f / RAT_PIL_H,
              cr, cg, cb, a);
      snprintf(ep, sizeof ep, "E%d", extras_ep_number(ratTemp, i));
      if (nd > 0) snprintf(nv, sizeof nv, "%.1f", nd / 10.0f);
      else        snprintf(nv, sizeof nv, "-");
      // 14/700 on the label and 28/800 on the value, from the web app
      // (.series-episode-rating-ep and .series-episode-rating-val). TXT_TITLE3 is 48
      // and overflowed the 62 chip — the "E1" was pushed out of it.
      { int c = (int)(tx * 255.0f);
        TxtLine le = txt_line(TXT_MINI, ep, c, c, c, 255);
        TxtLine lv = txt_line(TXT_ROW_TITLE, nv, c, c, c, 255);
        float h = le.h + 2.0f + lv.h;
        float yb = gy + (RAT_PIL_H - h) * 0.5f;
        txt_draw_alpha(le, gx + (RAT_PIL_W - le.w) * 0.5f, yb, a);
        txt_draw_alpha(lv, gx + (RAT_PIL_W - lv.w) * 0.5f, yb + le.h + 2.0f, a); }
    } }
}

static void drawRatings(float x, float y, float a) {
  int i, col = 0;
  for (i = 0; i < EX_NSOURCES; i++) {
    int v = extras_score(i);
    char txt[8];
    // Without mdbList the IMDb score still comes from the catalogue, which stores
    // 0..100; in the array the scale is "raw x 10", and for imdb the raw is 0..10.
    if (i == EX_IMDB && !v) v = scoreOf(idx);
    if (!v) continue;
    if (extras_source_percentual(i))
      snprintf(txt, sizeof txt, "%d%%", (v + 5) / 10);
    else
      snprintf(txt, sizeof txt, "%.1f", v / 10.0f);
    cardScore(x + col * (RATING_CARD_W + RATING_CARD_GAP), y,
               extras_path_brand(i), txt, a);
    col++;
  }
}

// The "More like this" tab: Trakt's /related. A column of titles with the year, and not
// the web app's posters — Trakt's related returns an identifier and a name, and
// fetching a poster for twelve titles just to paint this tab would cost twelve network
// requests on every opening. What the tab has to answer is "what else is like this",
// and the name answers it.
// "More like this" in POSTERS, and not as a text list: it is how the web app shows it
// (renderPreviewRail) and it is what the owner asked for on seeing the raw list. The
// poster comes from Trakt itself, with `extended=images` on /related — fetching art
// from another service would be one request per title just to paint this tab.
// "More like this" appears by TWO routes and they are not the same state:
//   SERIES -> it is a TAB, drawn in the SEC_CAST slot, with its own focus
//             (`relFocus`), because the cast row has a different count.
//   FILM   -> it is a SECTION of its own, SEC_RELATED, and what rules is `focus.column`.
//
// Only the first case was handled. On a film the row DID take focus (sectionN returns
// the right count) but nothing lit up and OK did not respond — it looked as though the
// whole section did not exist as far as the D-pad was concerned. This pair solves both
// at once.
static int relInList(void) {
  return focus.row == SEC_CAST || focus.row == SEC_RELATED;
}
static int relIndex(void) {
  return (focus.row == SEC_RELATED) ? focus.column : relFocus;
}

static void drawRelated(float x, float y, float a) {
  int n = extras_n_related(), i;
  int inList = relInList();
  int foc = relIndex();
  for (i = 0; i < n && i < 7; i++) {
    float cx = x + i * (REL_CARD_W + REL_CARD_GAP);
    GfxRect r = { cx, y, REL_CARD_W, REL_CARD_H };
    int lit = inList && i == foc;
    const char *po = extras_related_poster(i);
    GLuint t = po[0] ? tex_get_width(po, REL_CARD_W) : 0;
    float radius = radiusPoster(REL_CARD_W, REL_CARD_H);
    if (cx + REL_CARD_W > NV_SCREEN_W - NV_DETP_X) break;
    if (lit) {
      GfxRect ring = { r.x - 4, r.y - 4, r.w + 8, r.h + 8 };
      gfx_color(ring, radius, 1, 1, 1, a);
    }
    if (t) {
      gfx_tex_aspect_current = tex_aspect(po);
      gfx_rect(r, t, GFX_CARD, lit ? 1.0f : 0.0f, 0, 0, radius, 0, 0, 0, a);
      gfx_tex_aspect_current = 0.0f;
    } else {
      gfx_color(r, radius, 0.133f, 0.133f, 0.133f, a);
    }
    { int c = lit ? 255 : 225;
      TxtLine lt = txt_line_trim(TXT_DET_META2, extras_related_title(i),
                                    c, c, c, 255, REL_CARD_W);
      txt_draw_alpha(lt, cx, y + REL_CARD_H + 12.0f, a);
      { const char *year = extras_related_year(i);
        if (year[0]) {
          TxtLine la = txt_line(TXT_MINI, year, 140, 144, 153, 255);
          txt_draw_alpha(la, cx, y + REL_CARD_H + 12.0f + lt.h + 6.0f,
                             a * 0.9f);
        } } }
  }
}

// The COLLECTION tab: the franchise's parts, in the order TMDB returns them. The same
// vertical list as "More like this" — what changes is the source and the header with
// the collection's name.
static void drawCollection(float x, float y, float a) {
  int n = extras_n_collection(), i;
  float y0 = y;
  if (extras_collection_name()[0]) {
    TxtLine ln = txt_line_trim(TXT_DET_META2, extras_collection_name(),
                                  150, 154, 163, 255, 900.0f);
    txt_draw_alpha(ln, x, y0, a * 0.9f);
    y0 += ln.h + 16.0f;
  }
  for (i = 0; i < n && i < 7; i++) {
    float yl = y0 + i * 52.0f;
    int lit = (focus.row == SEC_CAST) && i == relFocus;
    int c = lit ? 255 : 225;
    if (lit) {
      GfxRect track = { x - 16.0f, yl - 8.0f, 940.0f, 48.0f };
      gfx_color(track, 10.0f / 48.0f, 1, 1, 1, 0.12f * a);
    }
    { TxtLine lt = txt_line_trim(TXT_DET_META, extras_collection_title(i),
                                    c, c, c, 255, 900.0f);
      txt_draw_alpha(lt, x, yl, a);
      { const char *year = extras_collection_year(i);
        if (year[0]) {
          TxtLine la = txt_line(TXT_DET_META2, year, 150, 154, 163, 255);
          txt_draw_alpha(la, x + lt.w + 18.0f, yl + 2.0f, a * 0.9f);
        } } }
  }
}

// The "Comments" tab: Trakt's /comments/likes, the most-liked first. One line with the
// user and the likes, and the text wrapped below.
// Comments in CARDS side by side, with the same frame as the score cards, so they do
// not sit as loose text in the middle of a screen made of components.
// The comment card IN THE REFERENCE'S FORMAT. MEASURED on the TCL: 722x466, a gap of
// 25, a corner of ~20. What was here was 560x240 with the name and the likes on the
// SAME line — the card fitted three lines of text and cut the rest, and the
// commenter's score did not appear.
//
// The reference splits it into three blocks, and the order matters: the NAME alone at
// the top, the TEXT in the middle taking what is left, and a footer "10/10  17 likes"
// against the base. Read the name, decide whether it interests you and only then read
// — in that order.
// The section's header, as in the reference: the trakt WORDMARK, "Comments" beside it,
// "Trakt ratings" below, and the "Series | Episode" selector.
//
// None of this existed — the cards appeared loose, saying neither where they came from
// nor that there were two sets. The selector is not decoration: an EPISODE's comments
// are a different Trakt query, and without it half the content was unreachable.
#define COM_PILL_H    64.0f
#define COM_PILL_PAD  34.0f
// The header's height, measured the same way headerComments walks it: 46 for the
// title + 44 for the subtitle + the pills (on a series only) + 28 of breathing room. It
// comes from a function and not from a constant precisely because the film's is smaller
// — hard-coding a single number would make one of the two wrong.
// THE ACTIVE TAB'S BASE on a series: the ABSOLUTE y where the SEC_CAST slot's content ends.
//
// It exists because that slot draws things of VERY different heights depending on the
// tab: the cast is ~230, but "More like this" has a 318 poster plus the label. The
// Trakt section was always stacked from the CAST's height, so choosing
// "Recommendations" sent the posters down on top of it. A single height will not do:
// using the largest would push Trakt away from the cast for no reason, and using the
// smallest is the defect the owner saw.
static float baseOfTabActive(void) {
  // The cast is drawn at NV_DETP_EL_Y itself; the other tabs at EL_Y + 40 (drawSection's
  // `yTab`). They are two different starting points.
  switch (tabIdOf(tabInfo)) {
    case TAB_RELATED:
      return NV_DETP_EL_Y + 40.0f + REL_CARD_H + 12.0f
           + NV_DETP_EL_LINE * 2.0f;          // title + year under the poster
    case TAB_RATINGS:
      if (isSeries() && extras_n_seasons() > 0)
        return NV_DETP_EL_Y + 40.0f + RAT_TEMP_H + 18.0f + RAT_PIL_H;
      return NV_DETP_EL_Y + 40.0f + RATING_CARD_H;
    case TAB_COLLECTION: {
      int n = extras_n_collection();
      if (n > 7) n = 7;
      return NV_DETP_EL_Y + 40.0f + 30.0f + (float)n * 52.0f;
    }
    default:
      return NV_DETP_EL_Y + NV_DETP_EL_AVATAR + NV_DETP_EL_NAME_DY
           + NV_DETP_EL_ROLE_DY + NV_DETP_EL_LINE * 2.0f;
  }
}

static float heightHeaderComments(void) {
  return 46.0f + 44.0f + (isSeries() ? COM_PILL_H : 0.0f) + 28.0f;
}

// The selector's labels, at file scope: the column count and the item width need them
// outside the drawing.
static const char *COM_LABEL[2] = { "Series", "Episode" };

// The comments row has TWO natures in sequence: the selector's pills (on a series only)
// and, after them, the CARDS.
//
// The cards had to become columns: they were drawn three and that was that, with no
// focus, so the other five Trakt sends (EX_COMMENT_MAX = 8) were unreachable — it was
// the "I couldn't navigate the comments".
static int nPillsCom(void) { return isSeries() ? 2 : 0; }
static int nCardsCom(void) {
  int n = (isSeries() && commentEp) ? extras_n_comments_ep()
                                 : extras_n_comments();
  return n > EX_COMMENT_MAX ? EX_COMMENT_MAX : n;
}

static float widthPillCom(const char *rot) {
  TxtLine l = txt_line(TXT_PLR_BODY, rot, 255, 255, 255, 255);
  return l.w + COM_PILL_PAD * 2;
}

// Draws the header and returns the Y where the CARDS start.
static float headerComments(float x, float y, float a) {
  float yy = y;
  // The wordmark. The mark is already in art/brands/trakt.png, the same one the scores
  // row uses — there is no "trakt" text drawn with a font, because the logotype has a
  // design of its own and writing the word would come out different from the reference.
  //
  // GFX_CARD and not GFX_BRAND/GFX_TEXT, for the same reason as the score card: the
  // shape modes paint with the given colour and discard the texture's RGB, and the
  // wordmark would become a silhouette. gfx_icon will not do either — it builds the
  // path from art/icons/, and the mark lives in art/brands/.
  //
  // art/brands/trakt_wordmark.png (282x106, with alpha) — the real wordmark, supplied
  // by the owner. Before, I drew the circular LOGOMARK here (trakt.png, 96x96)
  // stretched to a wordmark's width, and out came a deformed red badge that was neither
  // one thing nor the other.
  //
  // GFX_BRAND, and not GFX_CARD: the card mode IGNORES THE TEXTURE'S ALPHA and paints
  // the whole rectangle, so a BOX came out behind the letters — with the old file (a
  // screenshot, flat background) and with the vector one too, because there the
  // background is transparent and the RGB underneath is black.
  //
  // GFX_BRAND exists for exactly this: the shape comes from the ALPHA and the colour
  // from uColor. It works because the wordmark is a SINGLE COLOUR. It would not work for
  // the IMDb badge, which is yellow and black and needs the file's RGB — which is why
  // the score card stays on GFX_CARD.
  //
  // The height rules and the width comes from the file's REAL aspect ratio — hard-coding
  // the width would deform the drawing if the art were swapped.
  float widthBrand = 0.0f;
  { const char *cam = extras_path_brand_name("trakt_wordmark");
    // 320x122 drawn at 34 tall, i.e. 89 wide: the 160 ceiling was 1.8x that, deep
    // into the half-resolution mip. Asked for at the drawn width, as the brand art
    // in menu.c and profile_select.c is.
    float ap = tex_aspect(cam);
    GLuint t = tex_get_exact(cam, ap > 0.0f ? 34.0f * ap : 160.0f);
    if (t) {
      float h = 34.0f;
      if (ap <= 0.0f) ap = 282.0f / 106.0f;
      widthBrand = h * ap;
      { GfxRect m = { x, yy + 6.0f, widthBrand, h };
        gfx_tex_aspect_current = 0.0f;
        gfx_rect(m, t, GFX_BRAND, 0, 0, 0, 0.0f, 1, 1, 1, a); }
      widthBrand += 14.0f;
    } }
  // No "Comments" word beside the wordmark: the trakt logo already says whose they are,
  // and the subtitle just below already says what they are. It was three labels for one
  // thing.
  (void)widthBrand;
  yy += 46.0f;
  { TxtLine ls = txt_line(TXT_DET_META2, "Trakt ratings", 179, 179, 179, 255);
    txt_draw_alpha(ls, x, yy, a * 0.95f); }
  yy += 44.0f;

  // The two pills. On a FILM only the series' one exists — there is no episode — so the
  // whole row disappears instead of showing a dead control.
  if (isSeries()) {
    float px = x;
    int k;
    int row = SEC_COMMENTS;
    for (k = 0; k < 2; k++) {
      float w = widthPillCom(COM_LABEL[k]);
      GfxRect r = { px, yy, w, COM_PILL_H };
      // MEASURED against the reference: the CHOSEN pill is WHITE with dark text, and the
      // other is #2D2D2D with white text. It is the opposite of the season pills, where
      // the chosen one stays dark and only the text whitens — they are two components
      // with rules of their own, and I had applied the wrong rule here.
      //
      // That is why FOCUS cannot be the inversion: the inversion is already the "chosen"
      // state. It gets the white ring, which is the app's other focus language and
      // collides with nothing.
      float f = (level >= 1 && focus.row == row && focus.column == k)
                ? animFocus[row][k] : 0.0f;
      int sel = (commentEp == k);
      // FOCUS: a white ring on the DARK pill, GROWTH on the white one.
      //
      // A white ring around a white fill leaves a dark gap between the two, and that gap
      // is the "odd halo" — two whites separated by a black line, which reads neither as
      // focus nor as selection. On the already-inverted pill the focus is marked by
      // SIZE, which is the same language measured on the hero's circular buttons.
      { float grows = sel ? (1.0f + 0.07f * f) : 1.0f;
        float dw = r.w * (grows - 1.0f), dh = r.h * (grows - 1.0f);
        GfxRect rc = { r.x - dw * 0.5f, r.y - dh * 0.5f, r.w + dw, r.h + dh };
        float luma = sel ? 0.961f : 0.176f;      // #F5F5F5 / #2D2D2D
        gfx_color(rc, NV_RADIUS_PILL, luma, luma, luma, a);
        r = rc; }
      if (f > 0.01f && !sel) {
        GfxRect ring = { r.x - NV_RING_FOCUS, r.y - NV_RING_FOCUS,
                         r.w + NV_RING_FOCUS * 2, r.h + NV_RING_FOCUS * 2 };
        gfx_rect(ring, 0, GFX_RING, 0, NV_RING_FOCUS / ring.h, 0, NV_RADIUS_PILL,
                 1, 1, 1, f * a);
      }
      { int color = sel ? 17 : 255;
        TxtLine l = txt_line(TXT_PLR_BODY, COM_LABEL[k], color, color, color, 255);
        txt_weight(l, r.x + (r.w - l.w) * 0.5f, r.y + (r.h - l.h) * 0.5f, a, 0.5f); }
      px += w + COM_PILL_GAP;
    }
    yy += COM_PILL_H;
  }
  return yy + 28.0f;
}

static void drawComments(float x, float y, float a) {
  int ofSeries = !(isSeries() && commentEp);
  int n = ofSeries ? extras_n_comments() : extras_n_comments_ep();
  int i;
  y = headerComments(x, y, a);
  // Loading and "there are none" are the SAME empty list; without separating the two
  // the episode looked as though it never had a single comment.
  if (n == 0) {
    const char *msg = (!ofSeries && extras_comments_ep_loading())
                    ? "Loading comments…"
                    : "No comments yet.";
    TxtLine l = txt_line(TXT_DET_META2, msg, 150, 154, 163, 255);
    txt_draw_alpha(l, x, y, a * 0.9f);
    return;
  }
  // ALL the cards, not three: the others Trakt sends were unreachable. What limits
  // what appears is the horizontal clip below, and what brings in the off-screen ones
  // is the row's horizontal scroll.
  for (i = 0; i < n; i++) {
    float cx = x + i * (COM_CARD_W + COM_CARD_GAP) - scrollSec[SEC_COMMENTS];
    GfxRect card = { cx, y, COM_CARD_W, COM_CARD_H };
    int foc = (level >= 1 && focus.row == SEC_COMMENTS &&
               focus.column - nPillsCom() == i);
    float px, width;
    // Off screen on either side: do not even draw it. There are up to 8 cards of 722
    // px, and painting the ones nobody sees costs fill on a device where fill is the
    // scarce resource.
    if (cx > NV_SCREEN_W || cx + COM_CARD_W < 0.0f) continue;
    char footer[64];
    px = cx + COM_PAD; width = COM_CARD_W - COM_PAD * 2;
    frame(card, 20.0f, a);
    if (foc) {
      GfxRect ring = { card.x - NV_RING_FOCUS, card.y - NV_RING_FOCUS,
                       card.w + NV_RING_FOCUS * 2, card.h + NV_RING_FOCUS * 2 };
      // The OUTER radius = the card's radius + the ring's thickness, otherwise the
      // ring's corner is squarer than the card's and the two curves come apart.
      gfx_rect(ring, 0, GFX_RING, 0, NV_RING_FOCUS / ring.h, 0,
               (20.0f + NV_RING_FOCUS) / ring.h, 1, 1, 1, a);
    }

    { TxtLine lu = txt_line_trim(TXT_ROW_TITLE,
                                    ofSeries ? extras_comment_user(i)
                                            : extras_comment_ep_user(i),
                                    245, 248, 255, 255, width);
      txt_draw_alpha(lu, px, y + COM_PAD, a); }

    // The text stops BEFORE the footer: without the line ceiling it ran over the
    // likes. 5 lines is what fits between the name and the footer at a leading of 34.
    txt_block(TXT_DET_META2, ofSeries ? extras_comment_text(i)
                                     : extras_comment_ep_text(i),
              200, 205, 214,
              px, y + COM_PAD + 46.0f, width, 34.0f, a * 0.95f, 5);

    { int score = ofSeries ? extras_comment_score(i)
                         : extras_comment_ep_score(i);
      int cur  = ofSeries ? extras_comment_likes(i)
                         : extras_comment_ep_likes(i);
      if (score > 0)
        snprintf(footer, sizeof footer, "%d/10   %d likes", score, cur);
      else
        snprintf(footer, sizeof footer, "%d likes", cur);
      { TxtLine lr = txt_line(TXT_CAPTION2, footer, 150, 154, 163, 255);
        txt_draw_alpha(lr, px, y + COM_CARD_H - COM_PAD - lr.h, a * 0.9f); } }
  }
}

static void drawSection(int r, float a, Uint32 now) {
  (void)now;
  int n = sectionN(r);
  // An information tab other than "Creator and cast": the web app SWAPS the section's
  // content (per-episode ratings, a row of similar titles, a trailer). None of that
  // data exists in the native catalogue, and the web app shows exactly this line when
  // the data is missing (`.series-insight-empty`).
  //
  // Swap, do not overlay: in the first capture from the device the message came out ON
  // TOP of the cast's avatars, and both were illegible.
  { int tab = tabIdOf(tabInfo);
    float yTab = NV_DETP_EL_Y - scrollY + 40.0f;
    if (r == SEC_CAST && tab == TAB_RATINGS) {
      // A series with per-episode scores shows the web app's panel; everything else (a
      // film, or a series without that source) falls back to the score cards.
      if (isSeries() && extras_n_seasons() > 0)
        drawScoresEpisode(NV_DETP_X, yTab, a);
      else
        drawRatings(NV_DETP_X, yTab, a);
      return;
    }
    if (r == SEC_CAST && tab == TAB_RELATED) {
      drawRelated(NV_DETP_X, yTab, a); return;
    }
    if (r == SEC_CAST && tab == TAB_COLLECTION) {
      drawCollection(NV_DETP_X, yTab, a); return;
    }
    if (r == SEC_CAST && tab == TAB_COMMENTS) {
      drawComments(NV_DETP_X, yTab, a); return;
    } }
  if (n <= 0) return;
  // A FILM reads the stacked layout; a SERIES keeps the measured coordinates. Note that
  // on a series the GROUP's top and the DRAWING y are different numbers (the seasons
  // group starts at 1080 and the pill is drawn at 1160), which is why the two constants
  // coexist rather than one deriving from the other.
  float y;
  if (!isSeries()) y = contentSec[r];
  else switch (r) {
    case SEC_SEASONS: y = NV_DETP_TEMP_Y; break;
    case SEC_EPISODES:  y = NV_DETP_EP_Y;   break;
    case SEC_TABS_INFO:  y = NV_DETP_TAB_Y;  break;
    // THE TRAKT SECTION IS STACKED, not measured: it comes AFTER the cast and the
    // cast's height varies (a long name wraps onto two lines). The `default` below sent
    // it to NV_DETP_EL_Y, which is the CAST's own y — which is why it was drawn on top
    // of the avatars.
    //
    // Fixing recalcLayout was not enough: that governs focus and scrolling, and this
    // switch is what chooses where to DRAW. They were two numbers for the same place,
    // and only one of them had been corrected.
    case SEC_COMMENTS: y = contentSec[r]; break;
    default:             y = NV_DETP_EL_Y;   break;
  }
  y -= scrollY;

  // THE SECTION TITLE ("Seasons", "Cast"), as in the reference. The port had no header
  // at all and the rows appeared loose, without saying what they were. It sits ABOVE
  // the row and disappears with it on scrolling.
  // Only "Seasons". The cast row is already labelled by the TAB above it ("Creator and
  // cast"), and a "Cast" header just below it said the same thing twice — on the first
  // attempt the two even overlapped.
  // On a SERIES only "Seasons": the cast row is already labelled by the "Creator and
  // cast" tab just above, and a "Cast" header below it said the same thing twice. On a
  // FILM there are no tabs, so each section carries its own name — which is what makes
  // the page readable without the bar.
  //
  // On a SERIES there is NO header at all. There used to be "Seasons" above the row of
  // pills; the reference on the device does not have it: the "Season 1" pill already
  // says what the row is, and the label above it repeated the word twice on
  // consecutive lines. On a FILM each section still carries its own name, because there
  // is no tab bar there to say what is what.
  { const char *header = isSeries() ? NULL : headerOf(r);
    if (header) {
      TxtLine lc = txt_line(TXT_HEADLINE, header, 245, 248, 255, 255);
      txt_draw_alpha(lc, NV_DETP_X, y - lc.h - NV_DETF_HEADER_GAP, a);
    } }
  { float height = heightSection(r);
    if (y > NV_SCREEN_H || y + height < -40.0f) return; }
  // The episode list is a vertical window of its own, not a row of columns.
  if (r == SEC_EPISODES) { drawEpisodeList(y, a); return; }

  for (int c = 0; c < n && c < N_ITEMS; c++) {
    float f = animFocus[r][c];
    float x = xItem(r, c) - scrollSec[r];
    float w = widthItem(r, c);
    if (x > NV_SCREEN_W || x + w < -w) continue;
    switch (r) {
      case SEC_SEASONS: {
        GfxRect b = { x, y, w, NV_DETWEB_SEA_H };
        regionAdd("picker", b);
        drawSeason(b, c, f, a);
        // The expanded list is remembered, not drawn here: it hangs over the episode
        // row below and has to be painted after every section. See seasonMenuRect.
        if (seasonMenuOpen) seasonMenuAt = b;
        break;
      }
      case SEC_TABS_INFO: {
        if (c == 0) regionAdd("tabs", (GfxRect){ x, y, NV_SCREEN_W - x, NV_DETP_TAB_H });
        drawTabInfo(x, y, c, f, a);
        if (c + 1 < n) {
          TxtLine d = txt_line(TXT_PLR_BODY, "|", 128, 128, 128, 255);
          txt_weight(d, x + w + NV_DETP_TAB_SEP,
                   y + (NV_DETP_TAB_H - d.h) * 0.5f, a, 1.4f);
        }
        break;
      }
      case SEC_TRAILERS: drawTrailer(x, y, c, a); break;
      // They reuse the drawing that already served the series' TABS: it is the same
      // content, only now in a section of its own instead of behind a tab.
      case SEC_RELATED: if (c == 0) drawRelated(NV_DETP_X, y, a); break;
      case SEC_COMMENTS:  drawComments(NV_DETP_X, y, a); break;
      case SEC_DETAILS: drawDetails(x, y, f, a); break;
      default: drawCast(x, y, c, f, a); break;
    }
  }
}

// The page's structure appears while Cinemeta answers. It does not go into sectionN():
// a skeleton takes no focus and invents no items. It occupies exactly the final
// coordinates of the seasons/episodes, so the answer merely fills the blocks in and
// does not shift the page under the remote control.
static void drawSkeletonEpisodes(float a) {
  int c;
  float yt, ye;
  if (!isSeries() || cat_n_episodes(idx) > 0 ||
      !disc_episodes_loading(idx)) return;
  yt = NV_DETP_TEMP_Y - scrollY;
  ye = NV_DETP_EP_Y - scrollY;

  // ONE block for the picker, not three for a row of chips: the skeleton has to occupy
  // the FINAL coordinates, and the final shape here is a single dropdown. Its width is
  // widthSeason's own arithmetic on a placeholder label, so the answer arriving does
  // not resize the block under the remote.
  if (yt < NV_SCREEN_H && yt + NV_DETWEB_SEA_H > 0) {
    GfxRect p = { NV_DETP_X, yt, 360.0f, NV_DETWEB_SEA_H };
    gfx_skeleton(p, NV_RADIUS_PILL, 0.17f, 0.18f, 0.20f, a * 0.62f);
  }
  // Four rows, at the list's own coordinates: the thumbnail and the three lines of
  // copy — title, meta, synopsis — so the answer fills them in without a shift.
  for (c = 0; c < 4; c++) {
    float ry = ye + c * NV_DETEP_ROW_H;
    if (ry > NV_SCREEN_H || ry + NV_DETEP_ROW_H < 0) continue;
    GfxRect thumb = { NV_DETP_X, ry + (NV_DETEP_ROW_H - NV_DETEP_THUMB_H) * 0.5f,
                      NV_DETEP_THUMB_W, NV_DETEP_THUMB_H };
    float tx = NV_DETP_X + NV_DETEP_THUMB_W + NV_DETEP_TEXT_GAP;
    GfxRect title = { tx, ry + 58.0f, 360.0f, 26.0f };
    GfxRect meta  = { tx, ry + 106.0f, 240.0f, 18.0f };
    GfxRect desc  = { tx, ry + 148.0f, 620.0f, 18.0f };
    // All four through gfx_skeleton: one screen-space band lights the whole row as a
    // piece instead of each bar lighting on its own.
    gfx_skeleton(thumb, NV_DETEP_THUMB_R / NV_DETEP_THUMB_H,
                 0.105f, 0.11f, 0.12f, a * 0.82f);
    gfx_skeleton(title, 0.5f, 0.25f, 0.26f, 0.28f, a * 0.62f);
    gfx_skeleton(meta,  0.5f, 0.20f, 0.21f, 0.23f, a * 0.52f);
    gfx_skeleton(desc,  0.5f, 0.20f, 0.21f, 0.23f, a * 0.42f);
  }
}

// The same idea for a film's CAST: six avatars and the two lines of text at the final
// coordinates, with the "Cast" header in its place.
static void drawSkeletonCast(float a) {
  if (!castLoading()) return;
  float y = contentSec[SEC_CAST] - scrollY;
  if (y > NV_SCREEN_H || y + NV_DETF_EL_HEIGHT < -40.0f) return;
  { TxtLine lc = txt_line(TXT_HEADLINE, "Cast", 245, 248, 255, 255);
    txt_draw_alpha(lc, NV_DETP_X, y - lc.h - NV_DETF_HEADER_GAP, a); }
  for (int c = 0; c < 6; c++) {
    float x = NV_DETP_X + c * NV_DETP_EL_STEP;
    GfxRect av = { x, y, NV_DETP_EL_AVATAR, NV_DETP_EL_AVATAR };
    GfxRect name = { x, y + NV_DETP_EL_AVATAR + NV_DETP_EL_NAME_DY + 4.0f,
                     c % 2 ? 150.0f : 184.0f, 20.0f };
    GfxRect role = { x, name.y + NV_DETP_EL_ROLE_DY, 110.0f, 16.0f };
    gfx_skeleton(av, 0.5f, 0.17f, 0.18f, 0.20f, a * 0.62f);
    gfx_skeleton(name, 0.5f, 0.22f, 0.23f, 0.25f, a * 0.55f);
    gfx_skeleton(role, 0.5f, 0.20f, 0.21f, 0.23f, a * 0.45f);
  }
}

// THE PERSON'S CARD — the screen the web app calls castDetailScreen. It fills the whole
// screen over an opaque background, with the photo and the biography on the left and
// the filmography in poster cards on the right. There is no measured web layout to copy
// here (the web app's screen is a scrollable page of fluid width), so the measurements
// follow the ones this screen already uses: a gutter of 96, a 212x318 poster, and a
// card with the same radius as the others.

static void drawPerson(float a) {
  GfxRect screen = { 0, 0, NV_SCREEN_W, NV_SCREEN_H };
  gfx_color(screen, 0.0f, 0.051f, 0.051f, 0.051f, a);

  { GLuint t = person_photo()[0] ? tex_get(person_photo()) : 0;
    GfxRect r = { NV_DETP_X, 96.0f, PES_PHOTO_W, PES_PHOTO_H };
    if (t) {
      gfx_tex_aspect_current = tex_aspect(person_photo());
      gfx_rect(r, t, GFX_CARD, 0, 0, 0, radiusPoster(PES_PHOTO_W, PES_PHOTO_H), 0, 0, 0, a);
      gfx_tex_aspect_current = 0.0f;
    } else {
      gfx_color(r, radiusPoster(PES_PHOTO_W, PES_PHOTO_H), 0.13f, 0.13f, 0.13f, a);
    } }

  { float y = 96.0f + PES_PHOTO_H + 32.0f;
    TxtLine ln = txt_line_trim(TXT_TITLE3, person_name(), 245, 248, 255, 255,
                                  PES_PHOTO_W);
    txt_draw_alpha(ln, NV_DETP_X, y, a);
    y += ln.h + 10.0f;
    if (person_area()[0]) {
      TxtLine la = txt_line(TXT_DET_META2, person_area(), 150, 154, 163, 255);
      txt_draw_alpha(la, NV_DETP_X, y, a * 0.9f);
      y += la.h + 18.0f;
    }
    if (person_bio()[0])
      txt_block(TXT_DET_META2, person_bio(), 190, 195, 205, NV_DETP_X, y,
                PES_PHOTO_W, 32.0f, a * 0.9f, 6);
  }

  { int n = person_n_credits(), i;
    float x0 = PES_COL_X;
    TxtLine lt = txt_line(TXT_HEADLINE, "Filmography", 245, 248, 255, 255);
    txt_draw_alpha(lt, x0, 96.0f, a);
    for (i = 0; i < n; i++) {
      int col = i % PES_PER_LINE, lin = i / PES_PER_LINE;
      float x = x0 + col * (PES_CARD_W + PES_CARD_GAP);
      float y = 96.0f + lt.h + 28.0f + (lin - personLine) * (PES_CARD_H + 92.0f);
      if (lin < personLine) continue;
      GfxRect r = { x, y, PES_CARD_W, PES_CARD_H };
      const char *po = person_credit_poster(i);
      GLuint t = po[0] ? tex_get_width(po, PES_CARD_W) : 0;
      if (y + PES_CARD_H > NV_SCREEN_H - 24.0f) break;
      if (i == personFocus) {
        GfxRect ring = { r.x - 4, r.y - 4, r.w + 8, r.h + 8 };
        gfx_color(ring, radiusPoster(PES_CARD_W, PES_CARD_H), 1, 1, 1, a);
      }
      if (t) {
        gfx_tex_aspect_current = tex_aspect(po);
        gfx_rect(r, t, GFX_CARD, i == personFocus ? 1.0f : 0.0f, 0, 0,
                 radiusPoster(PES_CARD_W, PES_CARD_H), 0, 0, 0, a);
        gfx_tex_aspect_current = 0.0f;
      } else {
        gfx_color(r, radiusPoster(PES_CARD_W, PES_CARD_H), 0.13f, 0.13f, 0.13f, a);
      }
      { TxtLine lc = txt_line_trim(TXT_DET_META2, person_credit_title(i),
                                      230, 234, 242, 255, PES_CARD_W);
        txt_draw_alpha(lc, x, y + PES_CARD_H + 12.0f, a);
        { const char *year = person_credit_year(i);
          const char *pap = person_credit_role(i);
          char sub[96];
          snprintf(sub, sizeof sub, "%s%s%s", year,
                   (year[0] && pap[0]) ? "  \xc2\xb7  " : "", pap);
          if (sub[0]) {
            TxtLine ls = txt_line_trim(TXT_MINI, sub, 140, 144, 153, 255,
                                          PES_CARD_W);
            txt_draw_alpha(ls, x, y + PES_CARD_H + 12.0f + lc.h + 6.0f,
                               a * 0.9f);
          } } }
    } }
}

// THE DIRECTOR'S PORTRAIT IS NOT DRAWN HERE, and that is deliberate.
//
// A TMDB headshot used to come in over the backdrop on every film, an 840x930 box
// anchored to the right of the hero. The owner's call is that a person's face has no
// business as an overlay on a title screen: the art is the film's, and a portrait
// laid over it competes with the thing the screen is about. The credit is already in
// words, on the support line — "Director: Griffin Dunne" — which is the same fact
// without taking the picture away.
//
// director.c stays: the HOME still uses it for the director folders, where the
// portrait IS the subject (home.c). What is gone is only this screen's overlay.

// THE BACKDROP IS DRAWN BEFORE THE HOME, NOT OVER IT.
//
// It used to be part of detail_draw, which app.c calls after home_draw, so the
// title's art was composited ON TOP of the home's shelves and copy. Its left
// boundary then cut straight through them — a hard vertical line sweeping left
// across "Continue watching", through a poster card, through the middle of a
// sentence. No treatment of the art's own edge could fix that, because the edge
// was not the problem: a picture was in front of content it should have been
// behind. Filling the uncovered band with page ground, with a clamped edge texel
// and with a mirror fold all left the same line in the same place, because all
// three were changing what was on the RIGHT of it.
//
// app.c now calls this before the screen underneath is drawn, so the art is the
// background it is supposed to be and the home's own copy and shelves lie over it
// and fade on their own (home.c fades them with detail_progress).
void detail_draw_bg(Uint32 now) {
  if (!is_open) return;
  float s = t;
  // ONE call, at the top: the page is painted before the art and both read the
  // same arithmetic, so there is no way for the two to come off different frames.
  GfxRect target, cell; float aEntry, zoom;
  backdropRect(&target, &aEntry, &zoom, &cell);

  if (!detail_covers_screen()) {
    // IT STAYS ON THE WAY OUT, and the returning card depends on it. GFX_DETAIL's
    // vignette takes the art to #0d0d0d at the card's left edge; this paints the
    // rest of the screen the same #0d0d0d, so the shrinking rectangle sits on its
    // own colour instead of on the home's lit rows. Take it away and every edge of
    // the card is suddenly a boundary between a picture and a catalogue.
    // THE GROUND AND THE CARD NEVER OVERLAP, in either direction.
    //
    // This layer sits BETWEEN the home and the card, so with the card at alpha `a`
    // over a ground at `g` the home reaches the screen at (1-g)(1-a). Fade the two
    // together and g = a, so the home arrives at (1-a)^2 against the card's a: at
    // a = 0.5 that totals 0.75 and the screen dims 25% in the middle of the
    // dissolve. Drop the ground in one frame instead and the home snaps from black
    // to full brightness around a card still 98% the size of the screen. Both were
    // tried; the first reads as a flicker and the second as a snap.
    //
    // So they take turns. OPENING, the ground rises across the flight while the
    // card is already opaque (front-loaded, by t = 0.126) — the ground only ever
    // affects the area OUTSIDE the card, which is why that direction has always
    // looked clean. CLOSING is the same shape backwards: the ground fades out
    // first, finishing at NV_DETAIL_EXIT_GROUND while the card is still solid, and
    // only then does the card dissolve, over a home at full brightness with
    // nothing stacked in front of it.
    GfxRect screen = { 0, 0, NV_SCREEN_W, NV_SCREEN_H };
    gfx_color(screen, 0.0f, 0.051f, 0.051f, 0.051f,
              (exiting && sharedOrigin)
                ? anim_clamp((s - NV_DETAIL_EXIT_GROUND)
                             / (1.0f - NV_DETAIL_EXIT_GROUND), 0.0f, 1.0f)
                : anim_clamp((detail_progress() - NV_DETAIL_OPEN_GROUND_AT)
                             / NV_DETAIL_OPEN_GROUND_OVER, 0.0f, 1.0f));
  }
  gfx_no_crop();

  // --- the backdrop ---------------------------------------------------------
  // At rest it is a 1920x1080 image at (0,0) with the vignette over it: no card, no
  // frame, no neighbouring titles.
  //
  // IT IS BORN ON THE HOME'S HERO AND GROWS INTO THAT. The background does not
  // SWAP, it CONTINUES — the home's hero was already showing this same title's
  // art, so the rect starts exactly where that art was and flies from there, with
  // no flash and no crossfade. With a full-screen hero the two rects are the same
  // and the eye sees no movement at all, only the text rearranging; with a banded
  // one the flight is the transition, and what would otherwise expose it — four
  // travelling edges — is handled by the pair of gradients in backdropRect.
  //
  // The art must not come in from zero opacity over the identical art already on
  // screen: that dips the brightness in the middle, and it is why the opacity ramp
  // is front-loaded rather than spread over the flight.
  //
  // The home's rows go down underneath for as long as they are visible (home_draw
  // reads detail_progress), which is until the page's ground is opaque.
  const char *art = artOf(idx);
  int artPoster = artDetailIsPoster(idx);
  // A full-screen backdrop: it asks for the 1920 ceiling. With the common ceiling of
  // 960 the art was decoded at half the resolution and enlarged twofold on screen.
  GLuint tex = art ? tex_get_hero(art) : 0;
  // On scrolling, the web app does NOT blur the art: it FADES it. Measured in
  // `.series-detail-shell.detail-scrolled` — the backdrop goes to `opacity: 0.15` and
  // the vignette to 0, both over 0.8s cubic-bezier(.4,0,.2,1).
  //
  // THE VEIL IS FULL SCREEN and cost dearly on a GPU already drowning in fill
  // (measured: clr=38.3ms with the CPU idle). But it paints #0d0d0d — which is EXACTLY
  // the colour main.c clears the frame with (NV_COLOR_BACKGROUND_*). With the screen
  // already covered by the detail, underneath it there is no home and no other screen:
  // there is the glClear. Painting #0d0d0d over #0d0d0d changes not a pixel, and the
  // whole layer goes.
  //
  // It stays when the screen is NOT covered: then the home is underneath, and the veil
  // is what fades it out.
  if (pg > 0.01f && !detail_covers_screen()) {
    GfxRect screen = { 0, 0, NV_SCREEN_W, NV_SCREEN_H };
    gfx_color(screen, 0.0f, 0.051f, 0.051f, 0.051f, pg);
  }
  // The 4th parameter = the VIGNETTE's strength, not "focus". It goes to 0 along with
  // the scrolling, which is the pairing that was missing: the web app fades the art to
  // 15% AND removes the vignette at the same time. A fallback poster uses a contained
  // composition, with no cover crop.
  // WITH NO TEXTURE the call above fills the rectangle with #0d0d0d, and the alpha
  // it is handed used to be a literal 1.0 — an OPAQUE black rectangle, flying on
  // the transition's geometry while the home was still underneath it. At rest
  // aEntry is 1.0 anyway, so the page still has its opaque background when the art
  // never arrives; what goes is the black rectangle during the flight.
  // THE VIGNETTE STAYS ON when the page scrolls, which is a departure from the web
  // (it takes the vignette to 0 and the art to 15%). The owner's episode-page
  // reference is darker than that and dark on the LEFT, where the rows' copy sits,
  // with the picture still there on the right — which is the vignette's own shape.
  // So the art fades to 18% under a full vignette: the left goes to #0d0d0d and the
  // right keeps a dim picture.
  drawArtDetail(target, tex, art, artPoster,
                     tex ? aEntry * (1.0f - 0.82f * pg) : aEntry, 0.0f, zoom, cell);
}

void detail_draw(Uint32 now) {
  if (!is_open) return;
  float s = t, a2 = phase2();
  (void)s;


  // The hero SCROLLS with the document: it does not disappear and is not replaced by a
  // fixed header. That was what made the port's page look like another screen instead
  // of the same screen scrolled.
  // The content RISES into place as it appears, instead of merely turning up: it is the
  // counterpart of the home's text, which drops and fades. Together, it reads as one
  // block changing arrangement, which is what the owner asked for.
  // The same sign as the 26px above, and for the same reason: the block's two
  // movements are one movement and must not pull against each other.
  heroWeb(a2, -scrollY + (1.0f - a2) * NV_SCREEN_H *
                            ((sharedOrigin && NV_DETW_COPY_TOGETHER) ? -0.05f : 0.05f));


  if (pg <= 0.01f && scrollY < 1.0f) {
    if (personIs_open) drawPerson(s);
    return;
  }
  regionReset();
  drawSkeletonEpisodes(pg);
  drawSkeletonCast(pg);
  for (int r = 0; r < N_SECTIONS; r++) drawSection(r, pg, now);
  // The expanded season list, over every section: it hangs 8px below its anchor and
  // covers the episode row, which is drawn after it in the loop above.
  if (seasonMenuOpen) drawSeasonMenu(seasonMenuAt, pg);
  // ABOVE everything: the card is another screen, not a section of this one.
  if (personIs_open) drawPerson(s);
}

int detail_index(void) { return idx; }
int detail_requested_play(void) { int v = reqPlay; reqPlay = 0; return v; }
int detail_requested_open(void) { int v = reqOpen; reqOpen = -1; return v; }
int detail_requested_watched(void) { int v = reqWatched; reqWatched = 0; return v; }
int detail_requested_mark(void)     { int v = reqMark;     reqMark = 0;     return v; }
int detail_requested_sources(void)     { int v = reqSources;     reqSources = 0;     return v; }
// "Play from the start" still falls into the same path as the primary: the router only
// knows how to open the player at the saved point. Consuming the request here stops it
// being left hanging.
int detail_requested_do_start(void)  { int v = reqOfStart;   reqOfStart = 0;   return v; }
