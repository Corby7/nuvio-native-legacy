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
#include "director.h"
#include "gfx.h"
#include "text.h"
#include "tex_cache.h"
#include "focus.h"
#include "anim.h"
#include "layout.h"
#include "catalog.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

// A ceiling of items per section. 24 and not 8: a season of "Silo" has 10 episodes
// and the array of 8 hid the last two — the list looked smaller than the series is.
#define N_ITEMS    24
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
// Two states, not three: the hero (level 0) and the scrolled page (level 1). The
// intermediate "the card becomes full screen" only made sense while there was a
// card; in the web app the screen is born full.
static int  level = 0;
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
static int season = 0;            // the CHOSEN season (not the focused one)
// The focus's rest period over the season row, so the season changes when it STOPS
// on a pill rather than on every pill it passes through.
static int    tempPending = 0;
static Uint32 tempSince = 0;
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
static float baseOfTabActive(void);
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
static float docEnd = NV_DETP_END;

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
    case SEC_DETAILS:     return "Film Details";
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
  int target = seasonIn(c), n = cat_n_episodes(idx), i;
  if (target <= 0 || n < 1) return;
  for (i = 0; i < n; i++) {
    const CatEp *e = cat_episode(idx, i);
    if (e && e->season == target) {
      // Move the FOCUS and not just the scroll: the episode row already has the rule
      // that brings the focused card up against the left margin, and reusing it keeps
      // the tab and the D-pad agreeing about where the owner is.
      if (i < focus.nColumns[SEC_EPISODES]) {
        focus.row = SEC_EPISODES;
        focus.column = i;
      }
      return;
    }
  }
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
    }
    docEnd = NV_DETP_END;
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
                               int poster, float alpha, float pg) {
  if (!tex) {
    gfx_color(target, 0.0f, 0.051f, 0.051f, 0.051f, alpha);
    return;
  }
  gfx_tex_aspect_current = tex_aspect(art);
  if (!poster) {
    gfx_rect(target, tex, GFX_DETAIL, 1.0f - pg, 0, 0, 0.0f, 0, 0, 0,
             alpha);
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

void detail_open(const HomeItem *it) {
  mark("detail_open");
  item = *it;
  is_open = 1; exiting = 0; level = 0; button = 0;
  t = 0.0f; pg = 0.0f; scrollY = 0.0f; tabInfo = 0; personIs_open = 0;
  relFocus = 0; reqOpen = -1; ratTemp = 0;
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
  { const CatEp *e0 = cat_episode(idx, 0);
    const CatItem *ci0 = cat_item(idx);
    if (e0 && ci0) {
      int k;
      for (k = 0; k < ci0->nSeasons; k++)
        if (ci0->seasons[k] == e0->season) { season = k; break; }
    } }
  tempPending = season; tempSince = 0;
  int cols[N_SECTIONS]; for (int i = 0; i < N_SECTIONS; i++) cols[i] = sectionColumns(i);
  focus_start(&focus, N_SECTIONS, cols);
  memset(animFocus, 0, sizeof animFocus);
  memset(scrollSec, 0, sizeof scrollSec);
}

int detail_is_open(void) { return is_open; }

// 0..1 of how much the detail has already taken over the screen. The home reads this
// to push the rows DOWN as it comes in: it is the movement the owner describes as
// "only the posters go down". It lives here and not in a shared variable because the
// spring that produces it is the drawing's own — two different clocks would drift.
float detail_progress(void) { return is_open ? smooth(t) : 0.0f; }

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

  if (focus.row == SEC_EPISODES) ep = cat_episode(idx, focus.column);
  if (ep) {
    if (temp) *temp = ep->season;
    if (eps) *eps = ep->episode;
    if (origin) *origin = 1;
    return 1;
  }
  // In progress: the "Continue watching" item carries the season and episode.
  if (ci && ci->progress > 0 && ci->progress < 90 && ci->season > 0 && ci->episode > 0 &&
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
  ep = cat_episode(idx, 0);
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
static void backdropRect(GfxRect *r, float *opacity) {
  float s = smooth(t);
  GfxRect de;
  home_hero_rect(&de.x, &de.y, &de.w, &de.h);
  r->x = de.x + (0.0f - de.x) * s;
  r->y = de.y + (0.0f - de.y) * s;
  r->w = de.w + (NV_SCREEN_W - de.w) * s;
  r->h = de.h + (NV_SCREEN_H - de.h) * s;
  // It comes up FAST (s*3, not s): the art underneath is the same, so the ramp only
  // swaps the hero's vignette for the detail's.
  *opacity = anim_clamp(s * 3.0f, 0.0f, 1.0f);
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
  // all four edges AND is it already opaque? With the hero full screen it is born
  // practically screen-sized, so the answer arrives at t ~ 0.13 — the home leaves six
  // times sooner. When the origin does NOT cover (a banded hero, or the detail opened
  // from the search), the arithmetic answers `no` and the home is still drawn: which
  // is why this is a coverage measurement and not a new threshold on `t`.
  if (!is_open) return 0;
  { GfxRect r; float opacity;
    backdropRect(&r, &opacity);
    if (opacity < 0.999f) return 0;
    return r.x <= 0.5f && r.y <= 0.5f &&
           r.x + r.w >= NV_SCREEN_W - 0.5f && r.y + r.h >= NV_SCREEN_H - 0.5f;
  }
}

// --- the "Film Details" table -----------------------------------------------
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
    case SEC_SEASONS: return NV_DETP_TEMP_H;
    case SEC_EPISODES:  return NV_DETP_EP_H;
    case SEC_TABS_INFO:  return NV_DETP_TAB_H;
    case SEC_CAST:     return NV_DETF_EL_HEIGHT;
    case SEC_TRAILERS:     return NV_DETF_TR_HEIGHT;
    case SEC_RELATED: return 318.0f + 46.0f;   // poster + title/year
    // + the header: without it the next section ("Film Details") was stacked using
    // only the cards' height and came out ON TOP of them.
    case SEC_COMMENTS:  return heightHeaderComments() + COM_CARD_H;
    case SEC_DETAILS:     return nLinesDetail() * NV_DETF_DET_LINE;
  }
  return 0.0f;
}

static int sectionN(int r) {
  const CatItem *ci = cat_item(idx);
  switch (r) {
    case SEC_SEASONS:
      // A film has no seasons: the row DISAPPEARS instead of showing tabs that lead
      // nowhere. It is what the web app does — `.series-season-row` only exists in
      // the series layout.
      if (!isSeries()) return 0;
      if (ci && ci->nSeasons > 0)
        return ci->nSeasons < N_ITEMS ? ci->nSeasons : N_ITEMS;
      return 0;
    case SEC_EPISODES: {
      int q = cat_n_episodes(idx);
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
      // Changing tab FETCHES the season. Before it only moved the highlight and the
      // list stayed the same, which made the tab look broken.
      season = focus.column;
      tempPending = season; tempSince = 0;
      goToSeason(season);
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
      // In the web app it is `openEpisodeStreams`. Here the sources sheet is still the
      // title's: `stream_sheet_open()` takes no episode. Better to open the sheet that
      // exists than not to respond to OK.
      reqSources = 1;
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
    case SEC_EPISODES:   return NV_DETP_EP_W;
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
    if (r == SEC_EPISODES) { x += NV_DETP_EP_STEP; continue; }
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
    if (r == SEC_SEASONS) x += widthSeason(k) + NV_DETP_TEMP_GAP;
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

  // THE SEASON CHANGES ON THE FOCUS MOVING, not on OK.
  //
  // The season row is a SELECTOR in the reference: moving with the D-pad already swaps
  // the episode list. Here the swap only happened inside OK, and the owner, passing
  // through the pills, saw the list NOT change — which they described as "it takes ages
  // to update when you change season". It was not taking ages: it was not happening.
  //
  // With a REST PERIOD, for the same reason as the hero's (NV_HERO_IDLE_MS): sweeping
  // four seasons end to end would fire four queries of which only the last matters. It
  // waits for the focus to stop and only then swaps.
  if (level >= 1 && focus.row == SEC_SEASONS) {
    if (focus.column != tempPending) { tempPending = focus.column; tempSince = now; }
    else if (tempPending != season && tempSince &&
             now - tempSince >= NV_HERO_IDLE_MS) {
      season = tempPending;
      goToSeason(season);
      tempSince = 0;
    }
  } else {
    tempPending = season;
    tempSince = 0;
  }

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
  t  = anim_spring(t,  exiting ? 0.0f : 1.0f, dt, NV_SPRING_SCREEN);
  // A stiffness of its own: the web app takes 0.8s to fade the backdrop out
  // (cubic-bezier .4,0,.2,1), and the NV_SPRING_SCREEN spring settles in ~330ms.
  pg = anim_spring(pg, level >= 1 ? 1.0f : 0.0f, dt, NV_SPRING_PAGE);
  if (exiting && t < 0.02f) { is_open = 0; exiting = 0; t = 0.0f; return; }

  for (int r = 0; r < N_SECTIONS; r++)
    for (int c = 0; c < sectionN(r) && c < N_ITEMS; c++) {
      float target = (level >= 1 && focus_index(&focus, r, c)) ? 1.0f : 0.0f;
      animFocus[r][c] = anim_spring(animFocus[r][c], target, dt,
                                 target > animFocus[r][c] ? NV_SPRING_FOCUS : NV_SPRING_BLUR);
    }

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
      if (r == SEC_EPISODES) target = x;
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
      scrollSec[r] = anim_spring(scrollSec[r], target, dt, NV_SPRING_SCROLL);
    } }

  // --- VERTICAL scroll ------------------------------------------------------
  // The focused group's top goes to 33% of the usable height (40% on the tabs). It is
  // the web app's rule, and not a "scroll as much as needed": checked on all four groups.
  float targetY = 0.0f;
  if (level >= 1 && focus.row >= 0 && focus.row < N_SECTIONS) {
    // Aim at the top of the CONTENT (the track), not the group's: the section header
    // sits above and comes on screen with it, for free. It is what focusInList does in
    // the web app — `target.closest(".movie-cast-track, ...")`.
    float maxY = docEnd - NV_SCREEN_H;
    targetY = contentSec[focus.row] - NV_SCREEN_H * targetSec[focus.row];
    if (targetY > maxY) targetY = maxY;
    if (targetY < 0.0f) targetY = 0.0f;
  }
  scrollY = anim_spring(scrollY, targetY, dt, NV_SPRING_SCROLL);
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

static void drawButton(GfxRect r, const char *rot, int icon, int focused, float a) {
  // FOCUS IS SIZE, NOT A RING.
  //
  // There used to be two focus marks stacked here and both failed on the primary
  // button: first a gfx_color 8px larger — which FILLS, it does not outline — and right
  // after it the GFX_RING. On a button that is already white the filled pill merges
  // with it and the result is a white button 8px larger, which does not read as
  // "selected" but as "the button changed shape". That was the reported defect.
  //
  // The device solves it by scale: the focused item grows with its centre still, the
  // factors are in NV_DETW2_FOCUS_*, and there is no ring in any capture.
  // On the circular one the scale comes with the colour inversion, which already existed.
  int circular = (rot == NULL);
  if (focused) {
    float sx = circular ? NV_DETW2_FOCUS_SY : NV_DETW2_FOCUS_SX;
    float sy = NV_DETW2_FOCUS_SY;
    float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
    r.w *= sx; r.h *= sy;
    r.x = cx - r.w * 0.5f; r.y = cy - r.h * 0.5f;
  }
  if (circular) {
    float luma = focused ? 0.961f : 0.133f;   // #f5f5f5 / #222
    gfx_color(r, NV_RADIUS_PILL, luma, luma, luma, a);
    // The web app's three glyphs: library (+), watched (eye) and trailer (the YouTube
    // plate). They are SVG there and do not exist in the embedded family, so they come
    // from the shader — see GFX_EYE and GFX_SOURCES. They used to be a "+" and two
    // "...", which said nothing about what the buttons did.
    float ic = focused ? 0.067f : 1.0f;      // #111 with focus, white without
    float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
    // REAL ICONS, from art/icons (the web app's SVGs rasterised). Each glyph used to be
    // drawn by hand in the shader — a "+" of two rectangles, an eye of two discs, three
    // bars — and each was an approximation of the original. Now it is the file, and the
    // colour comes from here through GFX_BRAND.
    //
    // The glyph/circle proportion MEASURED on the device: the "+" is 32 inside the 96
    // circle at rest and 36 inside the focused 110 — 0.333 in both. It was 0.45, from a
    // loose capture, and the glyph nearly touched the edge.
    // It comes from `r` (already scaled) so the icon grows along with the button.
    float g = r.w * NV_DETW2_CIRC_GLYPH;
    GfxRect ig = { cx - g * 0.5f, cy - g * 0.5f, g, g };
    if (icon == 1) {
      // The button SHOWS THE STATE: with the title already on the watchlist the "+"
      // disappears and the open eye comes in — there is no point inviting you to add
      // what is already there. The state comes from ci->inList, which discovery fills
      // from Trakt's real list.
      const CatItem *ci = cat_item(idx);
      gfx_icon(ig, (ci && ci->inList) ? "watched" : "more", ic, ic, ic, a);
    } else if (icon == 2) {
      // WATCHED: an open eye once seen, a struck-through eye when not. The icon used to
      // be always the same and said no state at all — it was just decoration the owner
      // could not read ("tell me what's been watched").
      gfx_icon(ig, progressOf(idx) >= 90 ? "watched" : "unwatched", ic, ic, ic, a);
    } else {
      gfx_icon(ig, "sources", ic, ic, ic, a);
    }
    return;
  }
  // Primary: white with black text in BOTH states. Checked against the two captures
  // from the device — the middle measures (255,255,255) focused and at rest, and what
  // changes between them is only the size (321x94 -> 357.6x107.8), already applied to
  // `r` above.
  // A CLEAN WHITE PILL. Here the WHOLE button was the progress bar: the part still to
  // be watched got a 30% black veil, clipped at the progress point. The intention was
  // good and the clip was right, but the result read as a DISABLED BUTTON — a white pill
  // with two thirds dimmed looks like an inactive control, not "16% watched". It is what
  // the owner saw: "that resume looks awful, it doesn't even look like the same app".
  //
  // The reference does not do this: the button is clean white and the progress lives on
  // the TEXT LINE just above, which this file already draws ("Resume available  16%
  // Episode S2E1"). The veil was redundant as well as ugly — it said with ink what the
  // line already says in words.
  gfx_color(r, NV_RADIUS_PILL, 1, 1, 1, a);

  // A 28x30 triangle and a gap of 21 to the label's ink, measured on the device
  // (x=150..177 with the label at 200, inside the pill 96..417).
  //
  // The icon+label group is CENTRED in the pill rather than anchored to the left
  // padding: focused, the pill grows and the label does not — SDL_ttf rasterises at a
  // fixed size and there is no 28pt style in text.c's table, which is another agent's
  // file. Anchored left, the text would be visibly off centre in the focused state;
  // centred, the slack is the same on both sides.
  { float s = r.h / NV_DETW2_BTN_H;
    float iw = NV_DETW2_BTN_ICON_W * s, ih = NV_DETW2_BTN_ICON_H * s;
    TxtLine l = txt_line(TXT_DET_BUTTON, rot, 0, 0, 0, 255);
    float group = iw + NV_DETW2_BTN_GAPI * s + l.w;
    float x = r.x + (r.w - group) * 0.5f;
    GfxRect tri = { x, r.y + (r.h - ih) * 0.5f, iw, ih };
    gfx_rect(tri, 0, GFX_PLAY, 0, 0, 0, 0.0f, 0, 0, 0, a);
    txt_draw_alpha(l, x + iw + NV_DETW2_BTN_GAPI * s,
                       r.y + (r.h - l.h) * 0.5f, a); }
}

// The secondary button: 345x96, radius 64, background #222 and white text; focused,
// background #f5f5f5 and text #111, with the same 4px ring. It has no icon — in the web
// app it is only the label, which is why its width comes from `text + 2 x 34` and not
// from the primary's arithmetic.
static void drawSecondary(GfxRect r, const char *rot, int focused, float a) {
  if (focused) {
    GfxRect ring = { r.x - NV_DETW_RING, r.y - NV_DETW_RING,
                     r.w + NV_DETW_RING * 2, r.h + NV_DETW_RING * 2 };
    gfx_color(ring, NV_RADIUS_PILL, 1, 1, 1, a);
  }
  float luma = focused ? 0.961f : 0.133f;
  gfx_color(r, NV_RADIUS_PILL, luma, luma, luma, a);
  int color = focused ? 17 : 255;
  TxtLine l = txt_line(TXT_DET_BUTTON, rot, color, color, color, 255);
  txt_draw_alpha(l, r.x + (r.w - l.w) * 0.5f, r.y + (r.h - l.h) * 0.5f, a);
}

// The primary button's width: padding 54 + icon 28 + gap 21 + text. The arithmetic is
// the device's and matches the measurement: with "Watch S1:E1" (ink 162) it gives 319
// against the 321 read from the capture. A longer label grows the pill instead of
// spilling out from under the text.
static float widthPrimary(const char *rot) {
  TxtLine l = txt_line(TXT_DET_BUTTON, rot, 0, 0, 0, 255);
  return NV_DETW2_BTN_PADX * 2 + NV_DETW2_BTN_ICON_W + NV_DETW2_BTN_GAPI + l.w;
}
static float widthSecondary(const char *rot) {
  TxtLine l = txt_line(TXT_DET_BUTTON, rot, 255, 255, 255, 255);
  return NV_DETW_BTN2_PADX * 2 + l.w;
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

// The IMDb badge: 60x30, radius 4, yellow #f6c700 with a black "IMDb" inside; the score
// comes 8px later, in rgb(179,179,179) — the SAME colour as the rest of the line, and
// not white. Measured on the two captures from the device (the badge at x=628..687 on
// the series and 714..773 on the film, always y=938..967).
//
// It is NOT the 109x60 this file carried over from the web app: there the logo is 60
// TALL and here the whole badge is 30. With the web app's value the badge was twice the
// height of the line it lives on.
//
// The mark is still DRAWN (a rectangle + text) and not rasterised from the SVG: the app
// does not package SVG and the file cannot go in without reinstalling the ipk. It is
// the only part of the badge that is not 1:1.
static float drawBadgeImdb(float x, float yCenter, int score, float a) {
  if (score <= 0) return 0.0f;
  char txt[8];
  snprintf(txt, sizeof txt, "%d.%d", score / 10, score % 10);
  TxtLine l = txt_line(TXT_DET_SIN, txt, 179, 179, 179, 255);
  GfxRect brand = { x, yCenter - NV_DETW2_IMDB_H * 0.5f,
                    NV_DETW2_IMDB_W, NV_DETW2_IMDB_H };
  gfx_color(brand, NV_DETW2_IMDB_R / NV_DETW2_IMDB_H,
          0.965f, 0.780f, 0.0f, a);                       // #f6c700
  TxtLine lm = txt_line(TXT_MINI, "IMDb", 10, 10, 10, 255);
  txt_weight(lm, brand.x + (brand.w - lm.w) * 0.5f,
           brand.y + (brand.h - lm.h) * 0.5f, a, 0.8f);
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

// The separator dot: a 6 disc, and not the web app's 1x14 bar. There are two uses with
// the SAME shape and different colours, and the difference in colour is what groups the
// line: between genres it is rgb(179,179,179) (the text's own colour, because there it
// is a "•" of the sentence) and between GROUPS it is rgb(128,128,128), more muted.
static void drawDot(float x, float yCenter, float luma, float a) {
  GfxRect pt = { x, yCenter - NV_DETW2_DOT_D * 0.5f,
                 NV_DETW2_DOT_D, NV_DETW2_DOT_D };
  gfx_color(pt, 0.5f, luma, luma, luma, a);
}

static void heroWeb(float a, float offset) {
  if (a <= 0.005f) return;
  const CatItem *ci = cat_item(idx);

  char year[32], duration[64];
  fromMeta(profileOf(idx), year, sizeof year, duration, sizeof duration);

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
  float hasResume = (ci && ci->progress > 0) ? 1.0f : 0.0f;
  float hSin = 0.0f;
  if (sin) hSin = txt_block(TXT_DET_SIN, sin, 255, 255, 255, -1.0f, 0.0f,
                            NV_DETW2_TEXT_W, NV_DETW2_LD_SIN, 0.0f,
                            NV_DETW2_SIN_LINES);
  float yMeta2 = NV_DETW2_BASE - NV_DETW2_BADGE_H;
  float yMeta1 = yMeta2 - NV_DETW2_META_GAP - NV_DETW2_M1_H;
  float ySin   = yMeta1 - NV_DETW2_GAP_SIN - hSin;
  float ySup   = sup[0] ? ySin - NV_DETW2_GAP_SUP : ySin;
  float yActions = ySup - NV_DETW2_GAP_ACTIONS - NV_DETW2_BTN_H;
  // The resume line explains the BUTTON, so it sits right against it — just above.
  float yResume = yActions - NV_DETW_GAP_RESUME - NV_DETW_RESUME_H;

  // It rises a few pixels as it comes in: it continues the art's movement instead of
  // appearing ready in place. `offset` is the document's scroll.
  float rises = (1.0f - a) * 26.0f + offset;
  yMeta2 += rises; yMeta1 += rises; ySin += rises; ySup += rises;
  yResume += rises; yActions += rises;

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
    { GLuint sharp = tex_get_width(fileLogo, w);
      if (sharp) texLogo = sharp; }
      // The logo settles above whichever comes first: the resume line when there is
    // progress, otherwise the actions row itself.
    float baseLogo = (hasResume ? yResume : yActions) - NV_DETW_LOGO_GAP;
    GfxRect r = { NV_DETW2_X, baseLogo - h, w, h };
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
      gfx_rect(r, texLogo, m, 0, 0, 0, 0.0f, 1, 1, 1, a); }
  } else {
    // With no logo, the NAME. The box's height is still the logo's, so the button row
    // does not jump between a title with a logo and one without.
    const char *name = titleOf(idx);
    if (name) {
      TxtLine t2 = txt_line_trim(TXT_TITLE1, name, 255, 255, 255, 255,
                                    NV_DETW_LOGO_MAXW);
      // The same anchor as the logo: above the resume line when there is one, otherwise
      // above the actions. The BOX's height is still the logo's, so the actions row
      // does not jump between a title with a logo and one without.
      float baseLogo = (hasResume ? yResume : yActions) - NV_DETW_LOGO_GAP;
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

  { float cyBtn = yActions + NV_DETW2_BTN_H * 0.5f;
    int nb = 0, n = nButtons();
    // Changing title with the focus on the last circular button of a FILM and landing
    // on a series would leave `button` = 3 in a row of 3 buttons: none would appear
    // focused and OK would find no action. It is fixed here, in the drawing, which is
    // what every frame goes through.
    if (button >= n) button = n - 1;
    float bx = NV_DETW2_X;
    GfxRect rp = { bx, yActions, widthPrimary(rot), NV_DETW2_BTN_H };
    drawButton(rp, rot, 0, level == 0 && button == nb, a);
    bx += rp.w + NV_DETW2_BTN_GAP; nb++;
    for (; nb < n; nb++) {
      GfxRect rc = { bx, cyBtn - NV_DETW2_CIRC * 0.5f,
                     NV_DETW2_CIRC, NV_DETW2_CIRC };
      drawButton(rc, NULL, actionIn(nb), level == 0 && button == nb, a);
      bx += NV_DETW2_CIRC + NV_DETW2_BTN_GAP;
    } }

  // --- the resume line ------------------------------------------------------
  if (ci && ci->progress > 0) {
    char ln[160];
    if (ci->season > 0)
      snprintf(ln, sizeof ln, "Resume available   %d%%   Episode S%dE%d",
               ci->progress, ci->season, ci->episode);
    else
      snprintf(ln, sizeof ln, "Resume available   %d%%", ci->progress);
    TxtLine l = txt_line(TXT_CAPTION, ln, 255, 255, 255, 255);
    txt_draw_alpha(l, NV_DETW2_X, yResume + (NV_DETW_RESUME_H - l.h) * 0.5f,
                       a * 0.82f);
  }

  // --- "Writer: ..." / "Director: ..." ---------------------------------------
  // The same BODY SIZE as the synopsis, and not a smaller one: in the reference the "W"
  // of "Writer" and the "C" of the synopsis measure the same 20 of cap height. It was
  // on TXT_DET_META (25) against TXT_DET_SIN (26) because of a web app measurement,
  // where the two lines really do differ.
  if (sup[0]) {
    TxtLine l = txt_line_trim(TXT_DET_SIN, sup, 179, 179, 179, 255,
                                 NV_DETW2_TEXT_W);
    txt_draw_alpha(l, NV_DETW2_X, ySup, a);
  }

  // --- sinopse --------------------------------------------------------------
  if (sin) txt_block(TXT_DET_SIN, sin, 255, 255, 255, NV_DETW2_X, ySin,
                     NV_DETW2_TEXT_W, NV_DETW2_LD_SIN, a, NV_DETW2_SIN_LINES);

  // --- meta line 1: genres • genres  ·  year  ·  [IMDb] score ---------------
  //
  // A single line, in the device's order. The IMDb badge goes HERE, at the end of the
  // groups, and not up against the screen's right edge: it was orphaned, more than
  // 1000px from the text it belongs to, because the inherited value was NV_DETW_DIR.
  //
  // Two different separator dots, and the difference in colour is what groups the line
  // — see drawDot.
  {
    float x = NV_DETW2_X, yc = yMeta1 + NV_DETW2_M1_H * 0.5f;
    const CatItem *badgeItem=cat_item(idx);
    if(badgeItem)x+=badges_draw(badges_provider(badgeItem->providerName),x,yc-14,150,28,a);
    int something = 0;
    // GENRES without the first field. `genre` comes from the catalogue as
    // "TV Show · Action · Adventure" and the first piece is always the TYPE
    // (see catalog.c:539) — the reference does not show it on the meta line, only the
    // genres. Each one becomes a piece of its own with a "•" between them.
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
          if (something) {
            drawDot(x + NV_DETW2_BULLET_SEP, yc, 0.702f, a);   // 179
            x += NV_DETW2_BULLET_SEP * 2 + NV_DETW2_DOT_D;
          }
          TxtLine lt = txt_line(TXT_DET_SIN, term, 179, 179, 179, 255);
          txt_draw_alpha(lt, x, yc - lt.h * 0.5f, a);
          x += lt.w; something = 1;
        }
        p = end;
      }
    }
    // THE YEAR. On a series the reference writes "2023-" and on a film the full date;
    // the catalogue stores only the year in both cases (discover.c cuts the series' en
    // dash on purpose), so the year is what comes out. Empty when the metadata has not
    // arrived — and then the whole group disappears, with no fallback value.
    if (year[0]) {
      if (something) { drawDot(x + NV_DETW2_SEP, yc, 0.502f, a);    // 128
                  x += NV_DETW2_SEP * 2 + NV_DETW2_DOT_D; }
      TxtLine la = txt_line(TXT_DET_SIN, year, 179, 179, 179, 255);
      txt_draw_alpha(la, x, yc - la.h * 0.5f, a);
      x += la.w; something = 1;
    }
    if (ci && ci->score > 0) {
      if (something) { drawDot(x + NV_DETW2_SEP, yc, 0.502f, a);
                  x += NV_DETW2_SEP * 2 + NV_DETW2_DOT_D; }
      x += drawBadgeImdb(x, yc, ci->score, a);
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
      if(x+24+mw+10+lv.w>NV_DETW2_X+NV_DETW2_RATE_W)break;
      x+=24;
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

  // --- meta line 2: [status]  ·  duration  ·  country ---------------------
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
    const char *status=NULL,*raw=extras_profile_status();
    if(isSeries()) {
      if(!strcmp(raw,"canceled")||!strcmp(raw,"Canceled"))status="CANCELLED";
      else if(!strcmp(raw,"ended")||!strcmp(raw,"Ended"))status="ENDED";
      else if(!strcmp(raw,"returning series"))status="NOW SHOWING";
      else if(!strcmp(raw,"renewed"))status="RENEWED";
    }
    if (status) {
      x += drawBadgeMeta(x, yMeta2, status, NULL, a);
      something = 1;
    }
    if (!isSeries() && duration[0]) {
      if (something) { drawDot(x + NV_DETW2_SEP, yc, 0.502f, a);
                  x += NV_DETW2_SEP * 2 + NV_DETW2_DOT_D; }
      TxtLine ld = txt_line(TXT_DET_META2, duration, 255, 255, 255, 255);
      txt_draw_alpha(ld, x, yc - ld.h * 0.5f, a);
      x += ld.w; something = 1;
    }
    if (ci && ci->country[0]) {
      if (something) { drawDot(x + NV_DETW2_SEP, yc, 0.502f, a);
                  x += NV_DETW2_SEP * 2 + NV_DETW2_DOT_D; }
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
static int seasonIn(int c) {
  const CatItem *ci = cat_item(idx);
  if (ci && ci->nSeasons > 0)
    return (c >= 0 && c < ci->nSeasons) ? ci->seasons[c] : ci->seasons[0];
  return c + 1;
}
static void labelSeason(int c, char *dst, size_t n) {
  int s = seasonIn(c);
  if (s == 0) snprintf(dst, n, "Specials");
  else snprintf(dst, n, "Season %d", s);
}
static float widthSeason(int c) {
  char rot[32]; labelSeason(c, rot, sizeof rot);
  TxtLine l = txt_line(TXT_PLR_BODY, rot, 255, 255, 255, 255);
  return l.w + NV_DETP_TEMP_PADX * 2;
}
static float widthTabInfo(int i) {
  TxtLine l = txt_line(TXT_PLR_BODY, TAB_LABEL[tabIdOf(i)], 255, 255, 255, 255);
  return l.w;
}

// A season tab: 80 tall, radius 40 (a pill), a 1px rgba(255,255,255,0.16) border.
// Three states MEASURED, and not two:
//   normal    #222     text rgb(179,179,179)
//   chosen    #2d2d2d  white text
//   focused   #f5f5f5  text #111, no border
// Without the middle state, the user loses sight of which season they are on as soon as
// the focus moves down to the list.
static void drawSeason(GfxRect r, int c, float f, float a) {
  char rot[32]; labelSeason(c, rot, sizeof rot);
  int sel = (c == season);
  float radius = NV_RADIUS_PILL;
  // EVERY season is a CHIP, chosen or not. Before, only the chosen one had a container
  // and the others were loose text on the background — they did not read as a group of
  // buttons, and there was no way to guess they were clickable.
  //
  // MEASURED against the reference: #2D2D2D chips of 285x83; the CHOSEN one is
  // distinguished by its white TEXT (the background stays #2D2D2D), and the FOCUSED one
  // INVERTS — a near-white background, dark text, NO ring.
  //
  // What was here put a white ring around it and left the middle at #2D2D2D with grey
  // 170 text: the focused item became the MOST MUTED of the row, read on a TV as
  // "disabled". The inversion is the same focus language as the hero's circular
  // buttons, measured against the same reference — focus and "chosen" stop colliding
  // without needing two shades of grey that nobody can tell apart at 3 metres.
  { float base = sel ? 0.21f : 0.133f;
    float luma  = base + (0.961f - base) * f;   // -> #F5F5F5 on focus
    gfx_color(r, radius, luma, luma, luma, a); }
  if (sel && f < 0.99f)
    gfx_rect(r, 0, GFX_RING, 0, 1.5f / r.h, 0, radius,
             0.76f, 0.77f, 0.79f, 0.5f * (1 - f) * a);
  // Text: grey when it merely exists, white when chosen, DARK when focused.
  // Interpolated by `f` to follow the spring instead of snapping.
  { float light = sel ? 255.0f : 179.0f;
    float v = light + (17.0f - light) * f;     // -> #111 on focus
    int color = (int)(v + 0.5f);
    TxtLine l = txt_line(TXT_PLR_BODY, rot, color, color, color, 255);
    // A weight of 500 on Inter Regular: a second pass half a pixel to the right.
    txt_weight(l, r.x + (r.w - l.w) * 0.5f, r.y + (r.h - l.h) * 0.5f, a, 0.5f); }
}

// The `.series-episode-overlay` gradient: a vertical linear from rgba(0,0,0,0.06) to
// 0.95, with stops at 22% (0.18), 52% (0.62) and 82% (0.86). The shader has no mode for
// it — gfx.c is another agent's file — and the GFX_VEIL that exists darkens the LEFT
// too, which here would wipe out half the card.
//
// It comes out as bands anchored to the BASE: each band is a rounded rectangle running
// from a given height to the bottom of the thumbnail, with the same absolute radius.
// That way the bottom corners follow the thumbnail (a band with square corners would
// put two dark teeth outside the rounding) and the stacking reproduces the ramp,
// because compositing N layers of alpha `d` gives 1-(1-d)^n.
static void veilEpisode(GfxRect th, float a) {
  static const float STOPPED[5] = { 0.00f, 0.22f, 0.52f, 0.82f, 1.00f };
  static const float ALFA[5]   = { 0.06f, 0.18f, 0.62f, 0.86f, 0.95f };
  const int STEPS = 14;
  float accum = 0.0f;
  for (int i = 0; i <= STEPS; i++) {
    float u = (float)i / STEPS;
    // The target interpolated piecewise linearly, like the `linear-gradient`.
    float target = ALFA[4];
    for (int k = 0; k < 4; k++)
      if (u <= STOPPED[k + 1]) {
        float d = STOPPED[k + 1] - STOPPED[k];
        target = ALFA[k] + (ALFA[k + 1] - ALFA[k]) * (d > 0 ? (u - STOPPED[k]) / d : 0);
        break;
      }
    // How much THIS band has to add for the accumulated value to hit the target.
    float d = (target - accum) / (1.0f - accum);
    if (d <= 0.001f) continue;
    accum = target;
    float top = th.y + th.h * u;
    GfxRect track = { th.x, top, th.w, th.y + th.h - top };
    if (track.h < 2.0f) continue;
    float radius = NV_DETP_EP_RADIUS / track.h;
    if (radius > 0.5f) radius = 0.5f;
    gfx_color(track, radius, 0, 0, 0, d * a);
  }
}

// The episode card: 640x422, with the 640x414 thumbnail and ALL the text inside it,
// over the gradient. It is the structural difference from what was here before (the
// thumbnail on top, the text below, which is the Apple TV app's).
static void drawEpisode(GfxRect r, int c, float f, float a, Uint32 now) {
  (void)now;
  const CatEp *ep = cat_episode(idx, c);
  GfxRect th = { r.x, r.y, r.w, NV_DETP_EP_THUMB_H };
  float radiusTh = NV_DETP_EP_RADIUS / NV_DETP_EP_THUMB_H;

  // The focus ring: in the web app it is a box-shadow on the THUMBNAIL, not on the
  // card, and there is no scaling at all (`transform: none`).
  if (f > 0.01f) {
    GfxRect ring = { th.x - NV_DETP_RING, th.y - NV_DETP_RING,
                     th.w + NV_DETP_RING * 2, th.h + NV_DETP_RING * 2 };
    gfx_color(ring, radiusTh, 1, 1, 1, f * a);
  }

  const CatItem *series = cat_item(idx);
  const char *art = (ep && ep->thumb[0]) ? ep->thumb
                     : (series && series->backdrop[0] ? series->backdrop : NULL);
  GLuint t2 = art ? tex_get_width(art, th.w) : 0;
  if (t2) {
    gfx_tex_aspect_current = tex_aspect(art);
    gfx_rect(th, t2, GFX_CARD, 0, 0, 0, radiusTh, 0, 0, 0, a);
    gfx_tex_aspect_current = 0.0f;
  } else gfx_color(th, radiusTh, 0.133f, 0.133f, 0.133f, a);
  veilEpisode(th, a);

  // AN EPISODE ALREADY WATCHED, according to Trakt: a dark mask over the thumbnail and
  // a tick in the corner. The owner's request, and it answers a question the list did
  // not — where they stopped.
  //
  // The mask comes AFTER the text veil on purpose: it has to cover the whole thumbnail,
  // including the part already darkened, otherwise the watched and unwatched cards look
  // alike precisely over the text.
  if (ep && extras_ep_watched(ep->season, ep->episode)) {
    float d = 36.0f;
    GfxRect badge = { th.x + th.w - d - 16.0f, th.y + 16.0f, d, d };
    gfx_color(th, radiusTh, 0.0f, 0.0f, 0.0f, 0.22f * a);
    gfx_color(badge, 0.5f, 1, 1, 1, 0.92f * a);
    // The tick is made of two strokes; with no rotation in gfx, two thin rectangles in
    // steps read the same at a badge's size.
    { float cx2 = badge.x + d * 0.5f, cy2 = badge.y + d * 0.5f;
      int k;
      for (k = 0; k < 4; k++)
        gfx_color((GfxRect){ cx2 - 9.0f + k * 2.0f, cy2 - 1.0f + k * 2.0f, 3, 3 },
                0.4f, 0.05f, 0.05f, 0.05f, a);
      for (k = 0; k < 6; k++)
        gfx_color((GfxRect){ cx2 - 1.0f + k * 2.0f, cy2 + 5.0f - k * 2.0f, 3, 3 },
                0.4f, 0.05f, 0.05f, 0.05f, a); }
  }

  // NO INVENTED FALLBACK. Here the four lines fell back to a demo table (the name,
  // duration, date and synopsis of "Shrinking"), so an episode with no data did not
  // appear empty: it appeared with ANOTHER SERIES' TEXT, indistinguishable from real
  // information. A missing field now stays missing, and the drawing below already omits
  // each piece that comes back empty.
  const char *epName = (ep && ep->name[0])    ? ep->name    : NULL;
  const char *epDuration  = (ep && ep->duration[0]) ? ep->duration : NULL;
  const char *epDate = (ep && ep->date[0])    ? ep->date    : NULL;
  const char *epSin  = (ep && ep->synopsis[0]) ? ep->synopsis : NULL;
  int epNum = ep ? ep->episode : c + 1;

  float tx = r.x + NV_DETP_EP_PAD;

  // The absence of a tick does not assert that the history has arrived. It avoids an
  // invented "unwatched" badge while Trakt is still being queried.

  // The "EPISODE n" badge: a box 43 tall, radius 12, translucent dark background,
  // text in capitals.
  //
  // It went back to "EPISODE n" and not "S2E4". The short form came in from an old
  // capture of the owner's, but the reference on the device writes "EPISODE 1" in full —
  // and the argument that the short form "says which season it is" does not hold: the
  // card only appears inside the chosen season's tab, which is drawn just above it.
  { char header[24];
    snprintf(header, sizeof header, "EPISODE %d", epNum);
    TxtLine l = txt_line(TXT_CAPTION2, header, 255, 255, 255, 255);
    float w = l.w + NV_DETP_EP_BADGE_PADX * 2;
    GfxRect s = { tx, r.y + NV_DETP_EP_BADGE_Y, w, NV_DETP_EP_BADGE_H };
    gfx_color(s, 12.0f / NV_DETP_EP_BADGE_H, 0.05f, 0.05f, 0.06f, 0.78f * a);
    txt_weight(l, s.x + NV_DETP_EP_BADGE_PADX,
             s.y + (NV_DETP_EP_BADGE_H - l.h) * 0.5f, a, 1.0f); }

  // The title: 32/800. The 800 does not exist in the embedded family and the 32 only
  // exists in Regular in the style table, so it comes from three passes.
  // With no episode name, "Episode N" — which is a TRUE label, deduced from the number,
  // and not another series' title.
  { char fallback[32];
    const char *name = epName;
    if (!name) { snprintf(fallback, sizeof fallback, "Episode %d", epNum);
                 name = fallback; }
    TxtLine l = txt_line_trim(TXT_PLR_BODY, name, 255, 255, 255, 255,
                                 NV_DETP_EP_TEXT_W);
    txt_weight(l, tx, r.y + NV_DETP_EP_TITLE_Y, a, 1.4f); }

  // The synopsis: three lines, like the reference, with the block truncated.
  // With no synopsis the space stays empty: better a card with less text than a card
  // with the wrong text.
  if (epSin)
    txt_block(TXT_DET_SIN, epSin, 255, 255, 255, tx, r.y + NV_DETP_EP_SIN_Y,
              NV_DETP_EP_TEXT_W, NV_DETP_EP_LD_SIN, a * 0.9f, 3);

  // Meta: a clock + duration + date, 20/400 rgb(179,179,179), with 38 of slack between
  // the two blocks.
  { float x = tx, y = r.y + NV_DETP_EP_META_Y;
    // A 28x28 clock. The web app's glyph is a FILLED disc in rgb(179,179,179) with the
    // hands KNOCKED OUT — the SVG's `path` cuts the hand's L out of the disc. Here the
    // knockout comes out by painting the hands black over it: at that point of the
    // thumbnail the veil is already at 0.95, so what would be behind the cut-out is
    // practically black. Drawing the hands in the SAME colour as the disc, as it used
    // to, makes them disappear and leaves only a grey blob.
    // The clock only comes in WITH the duration beside it. On its own it is not an
    // icon, it is a label with no value: a loose grey disc in the card's corner, which
    // reads as a drawing fault.
    if (epDuration) {
      float cx = x + NV_DETP_EP_ICON * 0.5f, cy = y + NV_DETP_EP_ICON * 0.5f;
      GfxRect aro = { x, y, NV_DETP_EP_ICON, NV_DETP_EP_ICON };
      GfxRect pv = { cx - 1.5f, cy - 8, 3, 9.5f };
      GfxRect ph = { cx - 1.5f, cy - 1.5f, 8, 3 };
      gfx_rect(aro, 0, GFX_RING, 0, 2.0f / NV_DETP_EP_ICON, 0, 0.5f,
               0.76f, 0.77f, 0.79f, a);
      gfx_color(pv, 0.0f, 0.76f, 0.77f, 0.79f, a);
      gfx_color(ph, 0.0f, 0.76f, 0.77f, 0.79f, a);
      x += NV_DETP_EP_ICON + 8.0f;
      { TxtLine ld = txt_line(TXT_CAPTION2, epDuration, 179, 179, 179, 255);
        txt_draw_alpha(ld, x, y, a); x += ld.w + 16; }
    }
    // Extras supplies a Trakt rating per episode, not IMDb. Never use the series' score
    // or another provider's badge in this footer.
    int score = 0;
    if (ep) for (int st = 0; st < extras_n_seasons(); st++) {
      if (extras_season_number(st) != ep->season) continue;
      for (int ei = 0; ei < extras_n_eps(st); ei++)
        if (extras_ep_number(st, ei) == ep->episode) {
          score = extras_ep_score(st, ei); break;
        }
      break;
    }
    if (score > 0) {
      char value[32]; snprintf(value, sizeof value, "Trakt %d.%d", score / 10, score % 10);
      TxtLine ln = txt_line(TXT_CAPTION2, value, 229, 231, 236, 255);
      GfxRect badge = { x, y - 3, ln.w + 16, NV_DETP_EP_ICON + 6 };
      gfx_color(badge, 0.18f, 0.15f, 0.15f, 0.17f, 0.94f * a);
      txt_draw_alpha(ln, x + 8, y, a);
      x += badge.w + 16;
    }
    // THE DATE goes to the right of the card, as in the reference: the left keeps only
    // the duration, and the two stop competing for the same running line.
    if (epDate) {
      const char *date = epDate;
      size_t nDate = strlen(epDate);
      if (!settings_date_full() && nDate >= 4) date = epDate + nDate - 4;
      float available = r.x + r.w - NV_DETP_EP_PAD - x;
      if (available > 48) {
        TxtLine lf = txt_line_trim(TXT_CAPTION2, date, 179, 179, 179, 255, available);
        txt_draw_alpha(lf, r.x + r.w - NV_DETP_EP_PAD - lf.w, y, a);
      }
    } }

  // The progress bar: 576x8 at 16px from the thumbnail's base, track rgba(0,0,0,0.45)
  // and fill rgb(158,158,158). It only appears between 2% and 98% — the same range as
  // the web app's, and it is what stops a just-started episode getting a bar of zero
  // width.
  { int progress = 0;
    const CatItem *ci = cat_item(idx);
    if (ci && ci->progress > 0 && ep && ci->season == ep->season &&
        ci->episode == ep->episode) progress = ci->progress;
    if (progress > 2 && progress < 98) {
      GfxRect tr = { tx, r.y + NV_DETP_EP_BAR_Y, NV_DETP_EP_TEXT_W,
                     NV_DETP_EP_BAR_H };
      GfxRect at = { tr.x, tr.y, tr.w * (progress / 100.0f), tr.h };
      gfx_color(tr, 0.5f, 0, 0, 0, 0.45f * a);
      gfx_color(at, 0.5f, 0.62f, 0.62f, 0.62f, a);
    } }
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

// --- the "Film Details" table -----------------------------------------------
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
  t = tex_get(cam);
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
    GLuint t = tex_get_width(cam, 160.0f);
    if (t) {
      float ap = tex_aspect(cam);
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

  for (int c = 0; c < n && c < N_ITEMS; c++) {
    float f = animFocus[r][c];
    float x = xItem(r, c) - scrollSec[r];
    float w = widthItem(r, c);
    if (x > NV_SCREEN_W || x + w < -w) continue;
    switch (r) {
      case SEC_SEASONS: {
        GfxRect b = { x, y, w, NV_DETP_TEMP_H };
        drawSeason(b, c, f, a); break;
      }
      case SEC_EPISODES: {
        GfxRect b = { x, y, NV_DETP_EP_W, NV_DETP_EP_H };
        drawEpisode(b, c, f, a, now); break;
      }
      case SEC_TABS_INFO: {
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

  if (yt < NV_SCREEN_H && yt + NV_DETP_TEMP_H > 0) {
    for (c = 0; c < 3; c++) {
      float w = c == 0 ? 238.0f : 214.0f;
      GfxRect p = { NV_DETP_X + c * 276.0f, yt, w, NV_DETP_TEMP_H };
      gfx_color(p, 0.5f, 0.17f, 0.18f, 0.20f, a * 0.62f);
    }
  }
  if (ye < NV_SCREEN_H && ye + NV_DETP_EP_H > 0) {
    for (c = 0; c < 3; c++) {
      float x = NV_DETP_X + c * NV_DETP_EP_STEP;
      GfxRect card = { x, ye, NV_DETP_EP_W, NV_DETP_EP_H };
      GfxRect badge = { x + NV_DETP_EP_PAD, ye + NV_DETP_EP_BADGE_Y,
                       108.0f, NV_DETP_EP_BADGE_H };
      GfxRect title = { x + NV_DETP_EP_PAD, ye + NV_DETP_EP_TITLE_Y,
                         292.0f, 25.0f };
      GfxRect sin1 = { x + NV_DETP_EP_PAD, ye + NV_DETP_EP_SIN_Y,
                       NV_DETP_EP_TEXT_W, 18.0f };
      GfxRect sin2 = { sin1.x, sin1.y + NV_DETP_EP_LD_SIN, 420.0f, 18.0f };
      gfx_color(card, NV_DETP_EP_RADIUS / NV_DETP_EP_H,
              0.105f, 0.11f, 0.12f, a * 0.82f);
      gfx_color(badge, 0.48f, 0.19f, 0.20f, 0.22f, a * 0.70f);
      gfx_color(title, 0.5f, 0.25f, 0.26f, 0.28f, a * 0.62f);
      gfx_color(sin1, 0.5f, 0.20f, 0.21f, 0.23f, a * 0.52f);
      gfx_color(sin2, 0.5f, 0.20f, 0.21f, 0.23f, a * 0.52f);
    }
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
    gfx_color(av, 0.5f, 0.17f, 0.18f, 0.20f, a * 0.62f);
    gfx_color(name, 0.5f, 0.22f, 0.23f, 0.25f, a * 0.55f);
    gfx_color(role, 0.5f, 0.20f, 0.21f, 0.23f, a * 0.45f);
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

// The director's portrait belongs to the detail screen, not to the home's hero. The
// TMDB photo comes in over the backdrop with the same editorial treatment as the
// renderer and recedes when the document scrolls; the horizontal art is still the base.
static void drawDirectorDetail(const CatItem *ci, float a, float pg) {
  const char *photo;
  GLuint tex;
  GfxRect r;
  if (!ci || isSeries() || !ci->directing[0] || a <= 0.005f) return;
  director_request(ci->directing);
  photo = director_photo(ci->directing);
  if (!photo[0]) return;
  // The portrait takes up less than a full-bleed hero, but it is shown large on the TV.
  // Asking by the column's size avoids enlarging a blurred w500 without reserving the
  // ~2K of a horizontal cover.
  tex = tex_get_width(photo, 960.0f);
  if (!tex) return;
  // A real vertical box: the photo's aspect ratio is the GFX_PORTRAIT shader's
  // responsibility, and it anchors the image to the right and dissolves the edges.
  // The taller box lets the face breathe and avoids the look of a portrait squashed
  // inside a wide banner.
  r = (GfxRect){ 1080.0f, 0.0f, 840.0f, 930.0f };
  gfx_tex_aspect_current = tex_aspect(photo);
  gfx_rect(r, tex, GFX_PORTRAIT, 0, 0, 0, 0, 0, 0, 0,
           a * (1.0f - 0.82f * pg));
  gfx_tex_aspect_current = 0.0f;
}

void detail_draw(Uint32 now) {
  if (!is_open) return;
  float s = smooth(t), a2 = phase2();

  if (!detail_covers_screen()) {
    GfxRect screen = { 0, 0, NV_SCREEN_W, NV_SCREEN_H };
    gfx_color(screen, 0.0f, 0.051f, 0.051f, 0.051f, s);   // #0d0d0d, the web app's background
  }
  gfx_no_crop();

  // --- full-bleed backdrop --------------------------------------------------
  // The detail screen is a 1920x1080 image at (0,0) with the vignette over it; there is
  // no card, no frame and no neighbouring titles.
  //
  // THE BACKDROP DOES NOT GROW OUT OF THE CARD. It was the last remnant of the Apple
  // app's flight: the rectangle started from item.rect and opened out to the screen.
  // The owner described the right behaviour — "only the posters go down and the
  // background stays, and the background is the selected film's art" — and flying the
  // rectangle is the opposite of that: the art comes in small and grows, instead of
  // already being there.
  //
  // Now the art fills the screen from the first frame and only gains opacity. What
  // moves are the home's rows, which go down (see home_draw, which reads
  // detail_progress).
  // THE BACKGROUND DOES NOT SWAP: it CONTINUES. The home's hero was already showing
  // this same title's art, so the detail's backdrop is born at the exact rect it was in
  // and grows from there to full screen, with no flash and no crossfade — with the hero
  // full screen the two rects are practically the same and the eye sees no movement at
  // all, only the text rearranging. Before, the art came in from zero gaining opacity
  // over the identical art that was already there, which gave a flare in the middle of
  // the transition.
  GfxRect target; float aEntry;
  backdropRect(&target, &aEntry);
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
  drawArtDetail(target, tex, art, artPoster,
                     tex ? aEntry * (1.0f - 0.85f * pg) : 1.0f, pg);

  drawDirectorDetail(cat_item(idx), aEntry, pg);

  // The hero SCROLLS with the document: it does not disappear and is not replaced by a
  // fixed header. That was what made the port's page look like another screen instead
  // of the same screen scrolled.
  // The content RISES into place as it appears, instead of merely turning up: it is the
  // counterpart of the home's text, which drops and fades. Together, it reads as one
  // block changing arrangement, which is what the owner asked for.
  heroWeb(a2, -scrollY + (1.0f - a2) * NV_SCREEN_H * 0.05f);


  if (pg <= 0.01f && scrollY < 1.0f) {
    if (personIs_open) drawPerson(s);
    return;
  }
  drawSkeletonEpisodes(pg);
  drawSkeletonCast(pg);
  for (int r = 0; r < N_SECTIONS; r++) drawSection(r, pg, now);
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
