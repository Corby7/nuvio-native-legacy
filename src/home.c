// The native home, matching the modern interface of Nuvio 1.0.1 legacy: a hero at
// the top, a fixed rail on the left and horizontal rows of posters. The native
// infrastructure handles the asynchronous cache, focus and transitions.
#include "home.h"
#include "resume.h"
#include "seeall.h"
#include "ctxmenu.h"
#include "mark.h"
#include "gfx.h"
#include "text.h"
#include "tex_cache.h"
#include "focus.h"
#include "anim.h"
#include "layout.h"
#include "settings.h"
#include "catalog.h"
#include "collections.h"
#include "discover.h"
#include "badges.h"
#include "extras.h"
#include "director.h"
#include <strings.h>
// Declared by hand rather than including detail.h: that header includes THIS one
// (because of HomeItem), and the cycle only fails to explode thanks to the guards.
// A one-line function is not worth tying the two files together.
float detail_progress(void);
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <stdlib.h>
#include <math.h>
#include <ctype.h>

#define MAX_ART   64
// TWICE the catalogue cap (CAT_FILTER_MAX, currently 24), because the home's rows
// are not only catalogues: the owner's collections, the packaged groups and the
// synthetic ones (Continue watching, Among friends) all land here too. This array
// is the headroom, not the limit — what actually trims the list is CAT_FILTER_MAX.
#define MAX_FILTER    48
// 13 and not 12: there are 12 POSTERS plus the "See all" card's column, which takes
// the position after the last piece of art. With 12 here, animFocus[r][12] wrote
// outside the array — the card never lit up on focus and the neighbour's memory was
// silently corrupted.
#define MAX_CARDS 33

typedef struct {
  char title[96];
  KindRow kind;
  int n;
  // The first item of THIS row in the catalogue. The drawing used to do `r * 8 + c`,
  // that is, each row was a fixed window of 8 in the flat array — which only worked
  // because the rows were four and hard-coded. With the rows coming from the addons'
  // catalogues, each has a size of its own.
  int start;
  // The "See all" card takes column `n` (the one after the last piece of art). Kept
  // per row because only the ones that came from an addon catalogue have it.
  int seeAll;
  char base[600], catId[96];
  // The CATALOGUE's "movie" | "series". The `kind` above is the card's shape
  // (portrait/landscape), which is a different thing — you cannot deduce one from
  // the other.
  char catKind[8];
  // This row's catalogue key. It exists so the focus survives a republication:
  // discovery now publishes on every row that arrives from the network, and finding
  // it again by INDEX will not do — a new row may come in the middle, because the
  // order comes from art/rows.txt.
  char key[192];
  int folders[MAX_CARDS];
  int stackN;
} Row;

static char bd[MAX_ART][512];    int nBd = 0;    // backdrops 16:9
static char pst[MAX_ART][512];   int nPst = 0;   // posters 2:3

// A FALLBACK, and that is all: it is what the home shows while the network has not
// answered, or when none has answered. The real rows come from cat_row(), assembled
// in discover.c from the catalogues the addons declare.
static Row rows[MAX_FILTER] = {
  { "Continue watching", ROW_CONTINUE, 8, 0  },
  { "Popular - Film",      ROW_NORMAL,   8, 8  },
  { "Popular - Series",  ROW_NORMAL, 8, 16 },
  { "Trending",         ROW_NORMAL,   8, 24 },
};
static int nRows = 4;
static int resumeIndex = -1;
static char resumeId[64];
static unsigned resumeRev, resumeApplied;
static int requestSocial;
static int requestPersonSocial;
static CatItem personSocial;
int home_requested_person_social(CatItem *output) {
  if (!requestPersonSocial) return 0;
  requestPersonSocial=0; if(output)*output=personSocial; return 1;
}
int home_requested_social(void) { int v=requestSocial;requestSocial=0;return v; }

// It classifies only the catalogue's public names. It never inspects the URL (which
// may contain tokens) and never invents awards or availability.
static int containsName(const char *name, const char *term) {
  size_t n = strlen(term);
  for (; name && *name; name++) {
    size_t i;
    for (i = 0; i < n && name[i] &&
         tolower((unsigned char)name[i]) == (unsigned char)term[i]; i++) {}
    if (i == n) return 1;
  }
  return 0;
}
static KindRow profileCatalog(const char *name) {
  static const char *awards[] = {"oscar", "academy", "award", "premia", "cannes", "golden globe"};
  static const char *services[] = {"netflix", "disney", "prime video", "amazon", "apple tv", "hbo", "max -", "paramount", "globoplay", "mubi", "crunchyroll"};
  for (size_t i = 0; i < sizeof awards / sizeof awards[0]; i++)
    if (containsName(name, awards[i])) return ROW_COLLECTION;
  for (size_t i = 0; i < sizeof services / sizeof services[0]; i++)
    if (containsName(name, services[i])) return ROW_SERVICE;
  return ROW_NORMAL;
}
static int editorial(KindRow t) {
  return t == ROW_HIGHLIGHT || t == ROW_COLLECTION || t == ROW_SERVICE
      || t == ROW_SOCIAL;
}


static Focus focus;
static HomeItem itemFocus;      // filled in while drawing, read by the transition
static int  hasItemFocus = 0;
static float animFocus[MAX_FILTER][MAX_CARDS];
static float scrollX[MAX_FILTER];
static float scrollY = 0.0f;
// The speeds of the glide's second-order springs. They sit next to the position
// because anim_spring2() needs both. See anim.h.
static float velX[MAX_FILTER];
static float velY = 0.0f;
static int wantsExit = 0, requestOpen = 0, requestMenu = 0;
static Uint32 okSince = 0;
static int okPressing = 0;
static int okLongFired = 0;
static int okConsumeRelease = 0;
static float okHold = 0.0f;

// --- hero-carrossel ---
static int heroCurrent = 0, heroPrevious = 0;
// The hero candidate and how long it has been the candidate. See NV_HERO_IDLE_MS.
static int    heroPending = 0;
static Uint32 heroPendingIn = 0;
// ART ALREADY CHOSEN BUT NOT YET ON SCREEN.
//
// NV_HERO_IDLE_MS's rest period decides WHEN to swap; this decides WHETHER it can
// swap yet. Before, at the moment the rest period expired the old art started fading
// out and the new one only appeared once the texture was ready — in between it was
// EMPTY, and moving quickly along the row the owner saw the background flicker
// between one piece of art and the next. Now the old one stays IN PLACE until the
// new one is decoded; only then does the swap begin. Moving fast stops touching the
// background.
static int    heroWanted = -1;

// --- EXPANDING THE FOCUSED POSTER AT REST ------------------------------------
//
// `focusedPosterBackdropExpandEnabled` and `...DelaySeconds` already existed in
// art/settings.txt and in settings.c, but NOTHING in the drawing read them — the
// setting sat on the Settings screen with no effect at all. It is the behaviour the
// owner calls "the card growing when it's still": the focus lands on a poster, and
// after a few seconds it opens out into the LANDSCAPE art, pushing its neighbours aside.
//
// Stored per row/column and not just the target, because moving the focus has to
// CLOSE what was open in the same frame that starts the new one's clock.
static int    expRow = -1, expColumn = -1;
static Uint32 expSince = 0;
static float  expOpen = 0.0f;   // 0 closed, 1 open

// MEASURED on the TCL (1920x1080, com.nuvio.tv), capturing 0.8 s and 7.8 s after the
// keypress: the focused card goes from 214x320 to 565x320. THE HEIGHT DOES NOT
// CHANGE and the LEFT edge stays put at x=102 — it grows only to the right and
// pushes its neighbours. 565/320 = 1.77, that is 16:9 at the same height.
#define NV_EXP_ASPECT (16.0f / 9.0f)

// A row that can expand: only the UPRIGHT poster one. Continue watching and the
// landscape poster row already show the wide art — there is nothing to open out into.
static int canExpand(int r) {
  if (r < 0 || r >= nRows) return 0;
  if (rows[r].kind != ROW_NORMAL) return 0;
  return !settings_posters_landscape();
}
static Uint32 heroSwapIn = 0;
// FADE OUT -> EMPTY -> HARD CUT. See the measurement at NV_HERO_FADE_MS.
// `heroExits`    the alpha of the art that is LEAVING: 1 at the moment of the swap,
//              0 at the end.
// `heroEnters` 0 while the new art is hidden; it becomes 1 all at once, on the frame
//              where the texture is ready AND the fade has finished.
static float heroExits   = 0.0f;
static float heroEnters = 1.0f;

// THE COPY IS ONE BLOCK — the logo, the meta line, the highlight and the
// synopsis — and it arrives whole or not at all.
//
// It used to arrive in pieces, and that is what the owner saw as the text
// "streaming in" beside a backdrop that fades properly. Two causes, both fixed:
// txt_block rasterised a texture for every cumulative word-prefix while looking
// for its wrap point (see widthOf in text.c), so at TXT_PER_FRAME 2 the block
// needed a dozen frames to settle and typed itself in; and the logo simply
// appeared on the frame its download decoded.
//
// `heroCopy` is that block's alpha. It leaves AT THE COMMIT, on the frame the new
// art is adopted — zeroed there, beside heroLogo and heroCopyIn — and it comes
// back once drawHeroCopy has confirmed every line of it is rasterised AND the
// logo is in (or has been waited for long enough; see NV_HERO_LOGO_WAIT_MS).
//
// THIS NOTE USED TO SAY the copy leaves when the swap is DECIDED (heroWanted) and
// not when it lands, "which is what gives the copy the same fade-out -> gap ->
// fade-in shape the art has". No code ever did that: heroCopy is written in one
// place and that place is the commit. Corrected rather than implemented, because
// the shape it describes is not obviously the better one — leaving at the
// decision blanks the text while the OLD art is still standing, which is the one
// moment nothing has changed on screen yet. Whoever wants to try it should move
// the three zeroed lines, not trust this paragraph.
static float  heroCopy = 1.0f;      // 0 hidden, 1 fully in
static int    heroCopyReady = 1;    // written by drawHeroCopy, read by home_update
static Uint32 heroCopyIn = 0;       // when the copy on screen became the current one
// The logo has a ramp of ITS OWN for one case only: it lands AFTER the block is
// already up. While the block is still coming in the logo just rides the block's
// alpha, so the two are never seen out of step.
static float  heroLogo = 1.0f;
static int    heroLogoReady = 1;
// The candidate whose copy has already been laid out by the warm pass, so it runs
// only for the frames it actually has work in. -1 = nothing warmed. See the call
// at the end of drawHero.
static int    heroWarmedFor = -1;

// THE COLLECTION HERO CROSS-FADE, which is the same idea one row type over.
//
// The poster rows keep heroCurrent/heroPrevious and only swap once the new art has
// DECODED, so a fast walk leaves the picture standing. The collection rows had none of
// it: drawHero read the focused folder afresh every frame and drew it immediately. Two
// visible consequences, and the owner reported both as "it looks different" —
//
//  - every step along Discover or Streaming was a HARD CUT, never a fade;
//  - and because these covers are CDN URLs that land seconds later (see the note in
//    collections.h), stepping onto a folder whose art had not arrived drew NOTHING and
//    the hero went blank until the download finished.
//
// `colHeroFade` is the outgoing art's alpha: 1 at the instant of the swap, 0 once the
// new art stands alone. It decays in home_update, beside heroExits and at the same
// NV_HERO_FADE_MS rate, because that is where dt lives.
static int   colHeroCurrent = -1, colHeroPrevious = -1;
static float colHeroFade = 0.0f;
// The folder whose NEIGHBOURS have already been warmed. It is not colHeroCurrent:
// that one lags behind until the art decodes, and the warming has to happen while
// it is still lagging — which is the whole point of warming.
static int   colWarmFor = -1;
// The hero art of a collection, with the card's cover as the fallback the drawing
// already used.
static const char *colHeroArt(const ColFolder *f) {
  if (!f) return NULL;
  if (f->hero[0])  return f->hero;
  if (f->cover[0]) return f->cover;
  return NULL;
}

// THE HERO'S FAMILY, and the fade BETWEEN families.
//
// drawHero is not one drawing, it is three: the social row's, the collection rows' and
// the poster rows'. Each already crossfades ALONG ITS OWN ROW — heroExits for the
// posters, colHeroFade for the collections — but WHICH of the three runs was decided
// fresh every frame from rows[focus.row].kind, so going DOWN from a poster row onto a
// collection row exchanged one whole drawing for another between two frames. The owner
// reported exactly that: horizontal has the fade with the logos and the backdrops,
// vertical does not. Nothing was missing from either fade; neither had any hold over
// the boundary BETWEEN the two drawings.
//
// `famFade` is the alpha of the picture being LEFT, on the same clock and at the same
// NV_HERO_FADE_MS rate as the other two, and the arriving family's art AND copy come up
// on 1-famFade. So every move on the home now dissolves: along a row, across rows, and
// across the three kinds of row.
typedef enum { FAM_POSTER, FAM_COLLECTION, FAM_SOCIAL } HeroFamily;
// What the focused family had on screen LAST FRAME — just the art, so the family it
// hands over to can go on drawing it while it fades. The COPY is deliberately not here:
// the poster path already documents why two hero texts must not overlap (they are
// anchored to a base and stack upward, so the outgoing and incoming blocks sit at
// different heights and read as doubled text, not as a dissolve). Across families they
// are not even the same shape of text. So the copy keeps the shape it has within a row
// — gone at the handover, faded in by the family arriving — and only the art crossfades.
typedef struct {
  int     valid;
  int     social;         // the GFX_SOCIAL ambience belongs under it
  char    art[512];
  GfxRect rect;
  GfxMode mode;
  float   aspect;         // 0 = leave gfx_tex_aspect_current alone
} HeroShot;
static HeroShot   heroShot, heroLeaving;
static HeroFamily heroFamily = FAM_POSTER;
static int        heroFamilyKnown = 0;
static float      famFade = 0.0f;
// The fade WAITS for the arriving family to have something to show, which is the same
// gate heroWanted and colHeroCurrent already apply within a row. Without it, stepping
// onto a collection whose cover is still on the CDN (see the note in collections.h)
// would fade the standing picture out into an empty rectangle for the length of the
// download — the blank hero that lag was introduced to get rid of, back one layer.
static int        famHold = 0;
static Uint32     famHoldIn = 0;

// The three drawings, by the kind of row the focus is on. Every kind that is not the
// social row or a collection row is a title on a poster row, whatever the card's shape,
// and they all share one hero.
static HeroFamily familyOfRow(int r) {
  if (r < 0 || r >= nRows) return FAM_POSTER;
  if (rows[r].kind == ROW_SOCIAL)   return FAM_SOCIAL;
  if (rows[r].kind == ROW_CATALOGS) return FAM_COLLECTION;
  return FAM_POSTER;
}

static void heroShotArt(const char *art, GfxRect rect, GfxMode mode,
                        float aspect, int social) {
  heroShot.social = social;
  heroShot.valid  = social || (art && art[0]);
  heroShot.rect   = rect;
  heroShot.mode   = mode;
  heroShot.aspect = aspect;
  if (art && art[0]) snprintf(heroShot.art, sizeof heroShot.art, "%s", art);
  else               heroShot.art[0] = '\0';
}

// THE BAND'S TWO GRADIENTS, in the band's own 0..1, worked out from where the band
// LANDS ON THE SCREEN. They are passed in uPar rather than written into the shader
// because the band's size is a preference: a ramp fixed at a fraction of the band
// would slide across the copy as the size moved, clearing to the left of the text
// at one setting and to the right of it at another. What has to hold still is the
// SCREEN x the ramp clears at, and the SCREEN y the art is gone by.
//
// See the note on NV_HERO_FIT_CLEAR_X in layout.h for the two anchors: they are
// GFX_HERO_FULL's own measured stops, so the band at 100% IS the full-screen hero.
static void heroFitPar(GfxRect r, float *px, float *py) {
  float cover = r.w > 1.0f ? (NV_HERO_FIT_CLEAR_X - r.x) / r.w
                           : NV_HERO_FIT_EDGE_MIN;
  float fade  = r.h > 1.0f ? NV_HERO_FIT_FADE_Y / r.h : NV_HERO_FIT_FADE_MAX;
  if (cover < NV_HERO_FIT_EDGE_MIN) cover = NV_HERO_FIT_EDGE_MIN;
  if (cover > NV_HERO_FIT_EDGE_MAX) cover = NV_HERO_FIT_EDGE_MAX;
  if (fade  < NV_HERO_FIT_FADE_MIN) fade  = NV_HERO_FIT_FADE_MIN;
  if (fade  > NV_HERO_FIT_FADE_MAX) fade  = NV_HERO_FIT_FADE_MAX;
  *px = cover; *py = fade;
}

// One draw of the hero's art. It exists so that the band's uPar is filled in at
// EVERY call site: the mode is chosen once in drawHero and reaches five different
// gfx_rect calls, and a band drawn with uPar at zero comes out with its ramps
// collapsed against the left edge.
static void heroArtDraw(GfxRect r, GLuint tex, GfxMode mode, float alpha) {
  float px = 0.0f, py = 0.0f;
  if (mode == GFX_HERO_FIT) heroFitPar(r, &px, &py);
  gfx_rect(r, tex, mode, 0, px, py, 0, 0, 0, 0, alpha);
}

// The picture the family we have just left had up, still standing while it goes.
// Requested only WHILE the blend runs, for the reason the poster path documents: asking
// on every frame would drag an evicted 1920 texture back through the decoder purely so
// as not to draw it, and push the visible posters out of the cache's budget.
static void drawHeroLeaving(float alpha) {
  GLuint t;
  if (!heroLeaving.valid || alpha <= 0.004f) return;
  if (heroLeaving.social)
    gfx_rect((GfxRect){0,0,NV_SCREEN_W,NV_SCREEN_H}, 0, GFX_SOCIAL,
             0, 0, 0, 0, 1, 1, 1, alpha);
  if (!heroLeaving.art[0]) return;
  t = tex_get_hero(heroLeaving.art);
  if (!t) return;
  if (heroLeaving.aspect > 0.0f) gfx_tex_aspect_current = heroLeaving.aspect;
  heroArtDraw(heroLeaving.rect, t, heroLeaving.mode, alpha);
  gfx_tex_aspect_current = 0.0f;
}

static void loadsDir(const char *dir, char destination[][512], int *n, const char *sub) {
  char path[512];
  if (sub) snprintf(path, sizeof path, "%s/%s", dir, sub);
  else snprintf(path, sizeof path, "%s", dir);
  DIR *d = opendir(path);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d)) && *n < MAX_ART) {
    if (!strstr(e->d_name, ".jpg") && !strstr(e->d_name, ".png")) continue;
    snprintf(destination[*n], 512, "%s/%s", path, e->d_name);
    (*n)++;
  }
  closedir(d);
}

// The card's shape comes from TWO preferences, and not from the row's kind:
//
//   `modernLandscapePostersEnabled` swaps the 2:3 poster (212x322) for the 16:9
//   landscape card (318x182.9). MEASURED in the web app with the preference on.
//
//   `continueWatchingCardStyle` decides the "Continue watching" row: "card" and
//   "wide" draw landscape, "poster" uses the same 2:3 as the others.
static float widthOf(KindRow t) {
  switch (t) {
    case ROW_CONTINUE: return NV_HIGHLIGHT_W;
    case ROW_HIGHLIGHT: return 568.0f;
    case ROW_COLLECTION: return 480.0f;
    case ROW_SERVICE: return 360.0f;
    case ROW_SOCIAL: return 540.0f;
    // The ranking's card is an ordinary portrait poster with a numeral beside it;
    // the web has no top-10 row to measure, so it follows the poster.
    case ROW_TOP10: return NV_CARD_W;
    case ROW_RETURN: return 680.0f;
    // MEASURED in the web app's modern layout, where this card is a
    // `.home-collection-card.is-collection-landscape`: its width is
    // `--home-landscape-poster-width`, which is `--home-poster-width * 1.5`
    // (components.css:6674) and that base is 212 in the modern block — so 318, not
    // the 360 that stood here. The card is a COMPOSED BITMAP (the catalogue's name
    // and its item count are lettering baked into the cover the account serves), so
    // 13% of extra width was 13% of extra enlargement applied to type.
    case ROW_CATALOGS: return 318.0f;
    default:               return settings_posters_landscape() ? NV_CARD_LAND_W
                                                              : NV_CARD_W;
  }
}
// How many titles the hero cycles through. It comes from the catalogue when there is one.
static int nArchiveHero(void) { int n = cat_n(); if (n) return n; return nBd ? nBd : 1; }

// A title's art can never be filled in from an equivalent position in another array.
// The catalogue arrives in batches, and the order of the package's backdrops has no
// stable relation to the order of the network's items. Returning only art that
// belongs to the item itself leaves the no-art state explicit, instead of silently
// swapping identity.
static const char *artOfItem(const CatItem *item, int *isPoster) {
  if (isPoster) *isPoster = 0;
  if (!item) return NULL;
  if (item->backdrop[0]) return item->backdrop;
  if (item->poster[0]) {
    if (isPoster) *isPoster = 1;
    return item->poster;
  }
  return NULL;
}

// A poster is a good editorial fallback, but it must not be cover-stretched into a
// 16:9 hero, because that crops precisely the face and the title. It stays contained
// on the right-hand side, with the hero's own vignette, and the rest of the
// composition remains available for the title's copy.
static int drawArtHero(GfxRect r, GfxMode mode, const CatItem *item,
                           const char *path, float alpha) {
  int isPoster = 0;
  const char *art = item ? artOfItem(item, &isPoster) : path;
  GLuint tex;
  if (!art || !art[0]) return 0;
  tex = tex_get_hero(art);
  if (!tex) return 0;
  gfx_tex_aspect_current = tex_aspect(art);
  // RECORDED HERE and not at the call, because the contained-poster case below picks a
  // rectangle and a mode of its own: the family that inherits this picture has to go on
  // drawing it exactly where it was, not where the caller asked for it. The poster path
  // calls this twice a frame, the outgoing art first and the incoming one second, so the
  // last write is what is actually on screen — including the frames where the incoming
  // art has not decoded and the outgoing one is still the whole picture.
  //
  // Only when it is visible: during a hold the arriving family draws at alpha 0 purely
  // to queue the decode, and a shot of that would hand the NEXT family a picture to fade
  // out that was never faded in.
  if (!isPoster) {
    heroArtDraw(r, tex, mode, alpha);
    if (alpha > 0.004f) heroShotArt(art, r, mode, gfx_tex_aspect_current, 0);
  } else {
    float ap = gfx_tex_aspect_current > 0.05f ? gfx_tex_aspect_current : (2.0f / 3.0f);
    float h = r.h, w = h * ap, limit = r.w * 0.42f;
    if (w > limit) { w = limit; h = w / ap; }
    GfxRect poster = { r.x + r.w - w, r.y + (r.h - h) * 0.5f, w, h };
    gfx_rect(poster, tex, GFX_HERO, 0, 0, 0, 0, 0, 0, 0, alpha);
    if (alpha > 0.004f) heroShotArt(art, poster, GFX_HERO, gfx_tex_aspect_current, 0);
  }
  gfx_tex_aspect_current = 0.0f;
  return 1;
}

static void drawPlaceholderHero(GfxRect r, const CatItem *item, float alpha) {
  GfxRect block = { r.x + r.w * 0.58f, r.y + 32.0f,
                    r.w * 0.34f, r.h - 64.0f };
  gfx_color(block, 0.035f, 0.075f, 0.082f, 0.098f, alpha * 0.92f);
  { TxtLine t = txt_line(TXT_HERO_META, "Art unavailable",
                            185, 191, 204, 255);
    txt_draw_alpha(t, block.x + 28.0f,
                       block.y + block.h * 0.5f - t.h * 0.5f,
                       alpha); }
  if (item && item->title[0]) {
    TxtLine t = txt_line_trim(TXT_CAPTION, item->title,
                                 211, 216, 226, 255, block.w - 56.0f);
    txt_draw_alpha(t, block.x + 28.0f,
                       block.y + block.h * 0.5f + 20.0f, alpha * 0.76f);
  }
}

// The solid card surface, with nothing written on it. This is the LOADING
// state: the art has a URL and the decode thread is on it. Same skeleton the
// library grid and see-all already draw while they wait — the home was the only
// screen captioning that wait as a failure.
// The surface an image sits on before it lands. It is not flat: the web puts
// `linear-gradient(180deg, #1c1c1c, #111)` on `.content-poster` itself, so the
// gradient IS what the eye reads for the whole download. Two draws — the top
// colour, then black rolled down to the bottom stop — because the shader has no
// two-colour ramp and adding one for an 11-level range is not worth a mode.
// See NV_POSTER_BG_*.
//
// THE TOP COLOUR CARRIES THE SHINE (gfx_skeleton), so a row of cards still
// downloading is crossed by the same light as every other placeholder in the app —
// one sweep, set by gfx_new_frame, with no clock needed here. The darkening pass
// stays ON TOP of it, which is the right order: the gradient is what the card looks
// like and the shine is a light moving over it, so the base of the card dims the
// light exactly as it dims the colour.
// THE CARD'S SURFACE AT REST: the gradient, with nothing moving on it.
//
// Split out of drawArtSkeleton because the two say different things. This one is
// what a card IS; the skeleton is this plus the travelling shine, and the shine
// means WAITING. Anything that has already arrived must be drawn on this one.
static void drawArtSurface(GfxRect r, float radius, float alpha) {
  gfx_color(r, radius, NV_POSTER_BG_R, NV_POSTER_BG_G, NV_POSTER_BG_B, alpha);
  gfx_rect(r, 0, GFX_VEIL_BOTTOM, 0, 0, 0, radius,
           0, 0, 0, NV_POSTER_BG_FADE * alpha);
}

static void drawArtSkeleton(GfxRect r, float radius, float alpha) {
  gfx_skeleton(r, radius, NV_POSTER_BG_R, NV_POSTER_BG_G, NV_POSTER_BG_B, alpha);
  gfx_rect(r, 0, GFX_VEIL_BOTTOM, 0, 0, 0, radius,
           0, 0, 0, NV_POSTER_BG_FADE * alpha);
}

static void drawArtMissing(GfxRect r, float radius, const CatItem *item,
                               float alpha) {
  drawArtSkeleton(r, radius, alpha);
  TxtLine state = txt_line_trim(TXT_CAPTION, "Art unavailable",
                                    184, 188, 198, 255, r.w - 32.0f);
  float center = r.y + r.h * 0.5f;
  txt_draw_alpha(state, r.x + (r.w - state.w) * 0.5f,
                     center - state.h * 0.5f - (item && item->title[0] ? 8.0f : 0.0f),
                     alpha * 0.9f);
  if (item && item->title[0]) {
    TxtLine name = txt_line_trim(TXT_MINI, item->title,
                                    160, 165, 178, 255, r.w - 32.0f);
    txt_draw_alpha(name, r.x + (r.w - name.w) * 0.5f,
                       center + 12.0f, alpha * 0.78f);
  }
}

// A card with no texture is in ONE OF TWO states and they do not look alike to
// the viewer: the art is on its way, or there is no art. tex_get* answer 0 for
// both, so asking the texture alone gets it wrong for the whole download — and
// that is the "Art unavailable" that clears the moment the card opens: nothing
// was missing, the caption just went up before the image landed.
static void drawArtAbsent(GfxRect r, float radius, const char *art,
                                   const CatItem *item, float alpha) {
  if (!art || tex_failed(art)) drawArtMissing(r, radius, item, alpha);
  else                         drawArtSkeleton(r, radius, alpha);
}

// The format choice for cards. The artOfItem helper above says whether it had to use
// a poster as a fallback; here the card's visual order stays explicit.
static const char *art_by_format(const CatItem *item, int landscape) {
  if (!item) return NULL;
  if (landscape) return item->backdrop[0] ? item->backdrop
                                      : (item->poster[0] ? item->poster : NULL);
  return item->poster[0] ? item->poster
                         : (item->backdrop[0] ? item->backdrop : NULL);
}

// `cat_item()` wraps around for screens that walk circular lists. The home cannot use
// that contract to resolve art: a stale index becomes an absence, never another title.
static const CatItem *cat_item_exact(int i) {
  int n = cat_n();
  if (i < 0 || i >= n) return NULL;
  return cat_item(i);
}

// The art/ folder is a fallback collection only in the no-catalogue mode. Once the
// network has published items, no generic file may take another title's place.
static const char *art_by_identity(int index_, int landscape) {
  const CatItem *item = cat_item_exact(index_);
  const char *art = art_by_format(item, landscape);
  if (art) return art;
  if (cat_n() == 0 && index_ >= 0) {
    if (!landscape && index_ < nPst) return pst[index_];
    if (index_ < nBd) return bd[index_];
  }
  return NULL;
}

// The art a CARD shows, which is not always the art the HERO shows.
//
// On a resume row the item's `backdrop` is the SERIES' background — the same
// file the hero is drawing at that moment — so the card repeated the top of the
// screen and gave no sign of which episode it resumes. When Cinemeta has the
// episode's still, that is what the card is for. See thumbEp in catalog.h.
//
// The hero deliberately does NOT go through here: a still is a 1280-wide frame
// at best and would be stretched to 1920 across the whole screen, and the
// backdrop is the image chosen to carry text over it.
static const char *art_of_card(KindRow kind, int index_, int landscape) {
  if (kind == ROW_CONTINUE || kind == ROW_RETURN) {
    const CatItem *item = cat_item_exact(index_);
    if (item && item->thumbEp[0]) return item->thumbEp;
  }
  return art_by_identity(index_, landscape);
}

static int focus_can_press_long(void) {
  if (focus.row < 0 || focus.row >= nRows) return 0;
  const Row *s = &rows[focus.row];
  if (s->kind == ROW_CATALOGS || s->kind == ROW_SOCIAL ||
      s->kind == ROW_TOP10) return 0;
  if (s->seeAll && focus.column == s->n) return 0;
  return focus.column >= 0 && focus.column < s->n;
}


static float heightOf(KindRow t) {
  switch (t) {
    case ROW_CONTINUE: return NV_HIGHLIGHT_H;
    case ROW_HIGHLIGHT: return 320.0f;
    case ROW_COLLECTION: return 270.0f;
    case ROW_SERVICE: return 203.0f;
    case ROW_SOCIAL: return 240.0f;
    case ROW_RETURN: return 178.0f;
    case ROW_TOP10: return NV_CARD_H;
    // 318 * 0.5625, the 16:9 the web derives it at
    // (`--home-landscape-poster-height`, components.css:6675). Was 203.
    case ROW_CATALOGS: return 178.875f;
    default:               return settings_posters_landscape() ? NV_CARD_LAND_H
                                                              : NV_CARD_H;
  }
}
// The TOTAL height the row occupies: the art plus the label block, when there is one.
// Without adding the label here, the next row rises over the text — it was the same
// defect the row title already had over the cards.
static int hasLabel(KindRow t) {
  // NOTHING UNDER THE POSTER. The upright card used to carry the title and the genre
  // in a block below the art (.home-poster-copy, 74 tall), which is what the web app
  // does; here the art already says which title it is, and the row read as a list of
  // captions rather than as artwork.
  //
  // This is the ONLY gate: it feeds heightTotalOf as well, so the 74 goes back to the
  // row instead of being left as a blank strip under every card.
  //
  // The LANDSCAPE card is untouched. Its caption is a different element and sits INSIDE
  // the frame over the gradient (.home-poster-landscape-copy), where it is not a label
  // under a poster; it is drawn from settings_labels_poster() directly.
  (void)t;
  return 0;
}
static float heightTotalOf(KindRow t) {
  return heightOf(t) + (hasLabel(t) ? NV_POSTER_COPY_H : 0.0f);
}
// tvOS's gap is fixed at 40px and was already sized to accommodate the focus's
// growth: a 410 card growing 9% invades 18px on each side. The highlight card is
// larger than anything Apple uses, so for it the gap stays proportional — otherwise
// the invasion (31px) eats almost all the breathing room.
static float gapOf(KindRow t) {
  (void)t;
  return NV_CARD_GAP;
}
// The vertical step between rows. `.home-modern-landscape-posters` tightens
// `--home-row-gap` from 32 to 24 (components.css:6473) — the landscape row is shorter
// and the upright poster's breathing room would be too much for it.
static float rowGap(void) {
  return settings_posters_landscape() ? NV_ROW_GAP_LAND : NV_ROW_GAP;
}
// The card's radius, as a fraction of the smaller side (the shader's SDF is
// normalised). This is the ONLY number of the modern card that really does come from
// `posterCardCornerRadiusDp`: 12dp x 2 = 24px, checked in the running app. The width
// does NOT come from there (see the note in settings.h).
static float radiusOf(float w, float h) {
  (void)w;
  if (h <= 0.0f) return NV_RADIUS_CARD;
  return settings_radius_poster_px() / h;
}
// THE SHADER'S RADIUS IS A FRACTION OF THE HEIGHT, not of the smaller side.
// Every comment in this project said "smaller side" and the SDF disagrees:
//
//   vec2 p = (uv - 0.5) * vec2(asp, 1.0);   // asp = w/h
//
// p.y spans the full height, so one unit of the SDF is h pixels on BOTH axes —
// p.x is (w/h) units wide, measured in that same height unit. Dividing by the
// smaller side therefore only happened to be right when the smaller side WAS
// the height, which is every landscape card in the app. On the 212x322 portrait
// poster it asked for 24/212 = 0.1132, and the shader drew 0.1132 * 322 = 36.5px
// — half again the 24 the web uses, which is the "too much" the owner saw. The
// landscape poster measured correct in the same screenshot, which is exactly why
// this survived so long.
//
// Corner radius of a box drawn `pad` px inside the card outline. Concentric
// corners share a centre, so the radius loses exactly the inset.
static float radiusInset(float w, float h, float pad) {
  (void)w;
  if (h <= 0.0f) return NV_RADIUS_CARD;
  float r = settings_radius_poster_px() - pad;
  if (r < 0.0f) r = 0.0f;
  return r / h;
}
// Radius of everything INSIDE the frame — the art, the scrim, the label veil.
// 24 - 2 = 22, which is the `.home-poster-frame`'s own inner corner: the frame
// carries `border-radius: var(--home-poster-radius)` on a border box and a 2px
// border, so what it clips its image to is 22 (components.css:7757).
//
// The centre of that corner is 4 + 22 = 26 px in from the CARD's corner on both
// axes, and the focus band's outer corner has to share it — see radiusFocus.
static float radiusArt(float w, float h) {
  return radiusInset(w, h, NV_FRAME_BORDER);
}
// The outer corner of the focus band, `in` px inside the card's box.
//
// IT IS THE CARD'S RADIUS PLUS 2, not the card's radius, and that is the whole of
// the "border isn't even on all sides". Concentric corners share a centre: the art
// is 4px in with a radius of 22, so its centre sits 26px in from the corner, and
// only a 26 outer radius puts the band's outer arc on that same centre. Drawn at
// 24 the two arcs came apart — 4px of band along the straight edges, about 2 at
// each corner, which is exactly where the eye checks a rounded frame.
//
// 26 is also the web's own number: the focused frame lights its 2px border AND a
// `0 0 0 2px` shadow around it (components.css:7765), and a spread shadow's corner
// is the element's radius plus the spread — 24 + 2.
static float radiusFocus(float h, float in) {
  if (h <= 0.0f) return NV_RADIUS_CARD;
  float r = settings_radius_poster_px() + NV_FRAME_BORDER - in;
  if (r < 0.0f) r = 0.0f;
  return r / h;
}

// Which rows are `.home-poster-card` in the web, and so open the NV_CARD_PAD
// gutter and wear the ring inside it. Continue watching is the exception and
// the only one: `.home-continue-media` has `border: 0`, its art runs to the
// card edge and its focus ring is the 4px outset shadow on the card itself.
// ROW_RETURN is drawn by the same resume_draw, so it follows that card.
static int posterFrame(KindRow t) {
  return t != ROW_CONTINUE && t != ROW_RETURN;
}
// The frame's padding box: where the artwork actually lives. `pad` is the
// inset, already carrying the focus scale — in the web one transform scales
// the card and its borders together, so the gutter scales with it.
static GfxRect frameOf(GfxRect card, float pad) {
  GfxRect f = { card.x + pad, card.y + pad,
                card.w - pad * 2.0f, card.h - pad * 2.0f };
  if (f.w < 1.0f) { f.x = card.x; f.w = card.w; }
  if (f.h < 1.0f) { f.y = card.y; f.h = card.h; }
  return f;
}

// --- Card depth (`cardDepth*`) -----------------------------------------------
// The web app does this with two pseudo-elements over the art: a glow on the TOP edge
// with opacity `--card-depth-edge` and a discreet light band —
// `--card-depth-sheen` — crossing the upper part of the card. `--card-depth-coverage`
// thickens the edge band: `12 + round(18 * coverage)` px
// (layoutPreferences.js:181). They are the same three numbers as on the Settings screen.
static void drawDepth(GfxRect card, float radius, int onHere) {
  if (!settings_depth() || !onHere) return;
  float border = settings_depth_border();
  float brightness = settings_depth_brightness();
  float coverage = settings_depth_coverage();
  if (border > 0.001f) {
    float h = 12.0f + 18.0f * coverage;
    GfxRect track = { card.x, card.y, card.w, h };
    // A proportional radius: the band is much shorter than the card, so repeating the
    // card's fraction would round it too much and the edge would peel away from the corner.
    gfx_color(track, radius * (card.h / (h > 0.0f ? h : 1.0f)) * 0.5f,
            1.0f, 1.0f, 1.0f, border * 0.55f);
  }
  if (brightness > 0.001f) {
    GfxRect refl = { card.x, card.y + card.h * 0.06f, card.w, card.h * 0.28f };
    gfx_color(refl, radius, 1.0f, 1.0f, 1.0f, brightness * 0.18f);
  }
}
// The focused card DOES grow, by a little, and by a different little per card family:
// see NV_FOCUS_SCALE_POSTER and NV_FOCUS_SCALE_CW for the two values and the CSS they
// are read from.
//
// This returned ZERO behind a note saying the web had been MEASURED with
// `transform: none` and the same getBoundingClientRect as the card beside it. The
// stylesheet says otherwise, so that measurement caught a state which suppresses the
// transform — almost certainly a `.performance-constrained` session, which every webOS
// and Tizen device runs (js/app.js:184) and which sets `transition: none` on these
// cards.
//
// The failure the old note describes is real and worth remembering: a focused card
// rising 22px into the row title, which sits 15px above the cards (title 518..549,
// cards at 564). But that was 9% on a CENTRED origin plus an 8px lift. These grow from
// `transform-origin: top` and do not move upward at all.
//
// A DELIBERATE DIVERGENCE lives here too: the web SNAPS this scale, and the dimming
// with it. Not by design — `.performance-constrained` strips the transition off poster
// cards (components.css:19362) and leaves `filter` out of its transition allowlist
// (19190), both to dodge style-recalc cost in a browser. This renderer has no such
// problem: it redraws unconditionally every frame (main.c, no dirty-rect gating), so
// the ramp is free and rides animFocus, the spring that already exists per card and
// already honours reduce-motion.
static float scaleOf(KindRow t) {
  if (t == ROW_CONTINUE || t == ROW_RETURN) return NV_FOCUS_SCALE_CW;
  return NV_FOCUS_SCALE_POSTER;
}
static float stepOf(KindRow t) {
  return widthOf(t) + gapOf(t);
}

int home_start(const char *dirArt) {
  extras_load(dirArt);
  col_load(dirArt);
  badges_load(dirArt);
  cat_load(dirArt);
  // The LAST session's cache goes in over the package's catalogue, before any
  // network. If it does not exist (a first run) or is from another build, it carries
  // on with the package's, as it always did.
  if (cat_read_cache(dirArt)) mark("cached catalog on screen");
  loadsDir(dirArt, bd, &nBd, NULL);
  loadsDir(dirArt, pst, &nPst, "poster");
  if (!nBd) { printf("home: no backdrop in %s\n", dirArt); return 0; }
  if (!nPst) { printf("home: no portrait posters, Top 10 will use the backdrop\n"); }

  // In the modern legacy layout the hero is informational; the navigation starts on
  // the first content row (as in buildModernNavigationRows()).
  int cols[MAX_FILTER];
  for (int i = 0; i < nRows; i++)
    cols[i] = rows[i].n + (rows[i].seeAll ? 1 : 0);
  focus_start(&focus, nRows, cols);
  heroSwapIn = SDL_GetTicks() + NV_HERO_INTERVAL_MS;
  printf("home: %d backdrops, %d posters, %d rows\n", nBd, nPst, nRows);
  return 1;
}

void home_event(const SDL_Event *e) {
  if (e->type == SDL_QUIT) { wantsExit = 1; return; }

  // HOLDING OK OPENS THE POSTER'S MENU.
  //
  // The duration is only known when the key GOES UP, so the KEYUP has to be seen —
  // and it was discarded just below, along with every event that was not a KEYDOWN.
  // The title screen already uses this same measure (NV_HOLD_MS) to separate "Play"
  // from "choose source".
  { SDL_Keycode kk = e->key.keysym.sym;
    int isOk = (kk == SDLK_RETURN || kk == SDLK_KP_ENTER || kk == SDLK_SPACE);
    if (e->type == SDL_KEYDOWN && isOk) {
      if (!okPressing) {
        okPressing = 1;
        okLongFired = 0;
        okConsumeRelease = 0;
        okSince = SDL_GetTicks();
      }
      return;
    } else if (e->type == SDL_KEYUP && isOk) {
      // A RELEASE WITHOUT A PRESS SEEN HERE IS NOT A CLICK — the same guard
      // detail.c and ctxmenu.c already carry, and the one this screen lacked.
      //
      // The sidebar decides on the KEYDOWN (menu.c, choose()) and closes itself
      // right there. The KEYUP of the SAME press arrives when menu_open() is
      // already 0, so app.c's router hands it to the home — which opened the
      // card in focus. Picking "Switch profile" made that the reported defect:
      // the app went to the profile screen and, on returning to the home, the
      // pending open fired and started whatever was focused, which is normally
      // the first card of "Continue watching". It holds for every sidebar item,
      // and equally for the OK that dismisses any sheet drawn above the home.
      if (!okPressing) { okSince = 0; okHold = 0.0f; return; }
      if (okConsumeRelease) {
        okConsumeRelease = 0;
        okSince = 0;
        okPressing = 0;
        okHold = 0.0f;
        return;
      }
      Uint32 duration = okSince ? SDL_GetTicks() - okSince : 0;
      int onSeeAll = (focus.row >= 0 && focus.row < nRows &&
                       rows[focus.row].seeAll &&
                       focus.column == rows[focus.row].n);
      okSince = 0;
      okPressing = 0;
      okLongFired = 0;
      okHold = 0.0f;
      if (focus.row < 0 || focus.row >= nRows) return;
      if(rows[focus.row].kind==ROW_TOP10 && rows[focus.row].stackN) {
        Row *s=&rows[focus.row];
        s->n=s->stackN<10?s->stackN:10;
        s->stackN=0;s->seeAll=1;
        focus.column=0;focus.columnRemembered[focus.row]=0;
        focus.nColumns[focus.row]=s->n+1;
        return;
      }
      if(rows[focus.row].kind==ROW_SOCIAL && rows[focus.row].start<0) {
        requestSocial=1;return;
      }
      if(rows[focus.row].kind==ROW_SOCIAL) {
        const CatItem *ci=cat_item_exact(rows[focus.row].start+focus.column);
        if(ci){personSocial=*ci;requestPersonSocial=1;}return;
      }
      if (rows[focus.row].kind == ROW_CATALOGS) {
        if (focus.column >= 0 && focus.column < rows[focus.row].n) {
          seeall_collection(col_folder(rows[focus.row].folders[focus.column]));
        }
      } else if (onSeeAll) {
        seeall_open(rows[focus.row].base, rows[focus.row].catKind,
                      rows[focus.row].catId, rows[focus.row].title);
      } else if (duration >= NV_HOLD_MS) {
        ctx_open(rows[focus.row].start + focus.column);
      } else {
        requestOpen = 1;
      }
      return;
    } }

  if (e->type != SDL_KEYDOWN) return;
  SDL_Keycode k = e->key.keysym.sym;
#ifdef __APPLE__
  // Running on the Mac, Back on the home does NOT close: closing the window in the
  // middle of a test costs a recompile and a reopen. On the device it exits the app,
  // as it should.
  if (k == SDLK_ESCAPE || k == SDLK_AC_BACK || k == SDLK_BACKSPACE) return;
  if (k == SDLK_q) { wantsExit = 1; return; }
#else
  if (k == SDLK_ESCAPE || k == SDLK_AC_BACK) { wantsExit = 1; return; }
#endif
  // OK NO LONGER ACTS ON THE KEYDOWN. Opening the title there made "holding"
  // impossible: by the time the key went up, the detail had been open for half a
  // second. The whole decision — open, "See all" or the poster's menu — lives in the
  // KEYUP above, which is the only point that knows the DURATION.
  if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) return;
  // ANY OTHER KEY ENDS THE HOLD. An arrow pressed while OK is down already voids the
  // gesture inside ctxmenu (holdCancelled), but the home did not hear about it: the
  // rail went on filling, and on a card that was no longer the one being pressed.
  okPressing = 0; okSince = 0; okHold = 0.0f; okLongFired = 0;
  if (k == SDLK_RIGHT) {
    // The `&&` here was a short-circuit with a side effect: written as
    // `if (row == 0 && !focus_move(...))`, focus_move was only called ON THE HERO —
    // on any other row the right arrow moved nothing. Move first, decide afterwards.
    (void)focus_move(&focus, 1, 0);
  } else if (k == SDLK_LEFT) {
  // Left in the first column calls up the side menu, on ANY row — the hero included.
    // Before, the hero was an exception and used left to go back a title in the
    // carousel: anyone arriving there (coming back from another screen, say) had no
    // way to open the menu without first going down. The carousel is still reachable
    // by the right arrow and by the automatic change.
    if (focus.column == 0) { requestMenu = 1; return; }
    focus_move(&focus, -1, 0);
  }
  else if (k == SDLK_DOWN)  focus_move(&focus, 0, 1);
  else if (k == SDLK_UP)    focus_move(&focus, 0, -1);
}

// Rebuilds the list from the catalogue. Called every frame because discovery runs on
// another thread and may swap the catalogue at any moment; it returns early when
// nothing has changed, so it costs one integer comparison.
static int filtersApplied = -1;
// A signature of the preferences that CHANGE the row list. Without this, turning
// "Continue watching" off in Settings only took effect once the network swapped the
// catalogue — the `nCat` comparison returned early and the row stayed on screen.
static int prefsApplied = -1;
static int subscriptionPrefs(void) {
  return (settings_cw_on() ? 1 : 0)
       | (settings_cw_style() << 1)
       | (settings_posters_landscape() ? 8 : 0)
       | (settings_labels_poster() ? 16 : 0)
       | (settings_social_row() ? 32 : 0);
}
// THE KNOWN CURATION. It no longer decides the ORDER — the account's list does —
// and is left with two jobs: supplying the display name and the special kind of
// certain rows, and acting as a fallback order when the account has no order at
// all (a fresh account, or a server without the RPC).
//
// The "@Name" entries are packaged collection GROUPS, matched by title. The
// account's own collections do not come through here: they arrive by id, in the
// position the person gave them.
static const char *const CURATED_ID[]={"continue_watching","social_activity","now_playing_movies","@Streaming",
  "trending_movies","trending_series","@Themes","ai_movies_for_you",
  "ai_series_for_you","snoak_top100_movies","snoak_top100_series",
  "@Awards","@Directors","@Genres"};
static const char *const CURATED_NAME[]={"Continue watching","Among friends","Recent Release","Streaming",
  "Trending Movies","Trending Series","Themes","Picked for You · Movies",
  "Picked for You · Series","Top 100 · Movies","Top 100 · Series",
  "Awards","Directors","Genres"};
#define CURATED_N (sizeof CURATED_ID / sizeof CURATED_ID[0])

// Applies the display name and special kind to a catalogue row, if the curation
// recognises it. It does not touch the position: that was already decided by the
// caller.
static void decorateRow(Row *row) {
  size_t s;
  if(!row)return;
  for(s=0;s<CURATED_N;s++) {
    if(CURATED_ID[s][0]=='@')continue;
    if(strcmp(row->catId,CURATED_ID[s])&&strcmp(row->key,CURATED_ID[s]))continue;
    snprintf(row->title,sizeof row->title,"%s",CURATED_NAME[s]);
    if(s==1)row->kind=ROW_SOCIAL;
    else if(s==2)row->kind=ROW_HIGHLIGHT;
    else if(s==9||s==10)row->kind=ROW_TOP10;
    else if(s!=0)row->kind=ROW_NORMAL;
    return;
  }
}

static void syncRows(void) {
  int nCat = cat_n_rows(), r, destination = 0;
  int sub = subscriptionPrefs();
  static unsigned ultimaRevision;
  unsigned revision = 2166136261u;
  for (r = 0; r < nCat; r++) {
    const CatRow *cf = cat_row(r);
    if (!cf) break;
    for (const unsigned char *s = (const unsigned char *)cf->key; *s; s++)
      revision = (revision ^ *s) * 16777619u;
    for (const unsigned char *s = (const unsigned char *)cf->title; *s; s++)
      revision = (revision ^ *s) * 16777619u;
    revision = (revision ^ (unsigned)cf->start) * 16777619u;
    revision = (revision ^ (unsigned)cf->n) * 16777619u;
  }
  // The COLLECTIONS join the sum. Without this, a collection arriving from the
  // account after the catalogue does not change the revision, the early return
  // just below skips the rebuild, and its row never comes to exist.
  revision = (revision ^ col_revision()) * 16777619u;
  // Stored BEFORE the loop below, which overwrites rows[]: after it there is no
  // longer any way to know which row the focus was on.
  char keyFocus[192];
  int colFocus = focus.column;
  keyFocus[0] = 0;
  if (focus.row >= 0 && focus.row < nRows)
    snprintf(keyFocus, sizeof keyFocus, "%s", rows[focus.row].key);
  // `nCat < 1` alone hid the COLLECTIONS: with no catalogue rows the function
  // returned right here, so a home that had only collections came out empty.
  if ((nCat < 1 && !col_n()) || (nCat == filtersApplied && sub == prefsApplied
      && revision == ultimaRevision && resumeApplied == resumeRev)) return;
  // Store the state by key: inserting the hub must not transfer one row's horizontal
  // scroll to another.
  static Row old[MAX_FILTER];
  float oldX[MAX_FILTER];
  int nOld = nRows;
  memcpy(old, rows, sizeof old);
  memcpy(oldX, scrollX, sizeof oldX);
  int hasHighlight = 0;
  for (r = 0; r < nCat && destination < MAX_FILTER - 1; r++) {
    const CatRow *cf = cat_row(r);
    if (!cf) break;
    if (cf->n < 1) continue;
    // `continueWatchingEnabled: false` takes the row out of the home entirely — it
    // does not empty it, it removes it. It is what renderModernHomeLayout does when
    // computeContinueWatchingRenderState returns the row switched off.
    if (!strcmp(cf->key, "continue_watching") && !settings_cw_on()) continue;
    // Same rule for the friends' feed: turning it off REMOVES the row. The
    // catalogue can still carry it — it was built while the option was on, or
    if (!strcmp(cf->key, "social_activity") && !settings_social_row()) continue;
    snprintf(rows[destination].title, sizeof rows[destination].title, "%s", cf->title);
    // "Continue watching" is the only landscape one: it is the profile's
    // `continueWatchingCardStyle: "card"`. Everything else is a 2:3 poster.
    // `continueWatchingCardStyle`: "card" and "wide" draw landscape, "poster" uses
    // the same 2:3 as the other rows. It is the preference, not the row's kind, that
    // decides the shape.
    rows[destination].kind = (!strcmp(cf->key, "continue_watching")
                              && settings_cw_style() != 2)
                           ? ROW_CONTINUE : profileCatalog(cf->title);
    if (!strcmp(cf->key, "continue_watching")) {
      if (settings_cw_style() == 2) rows[destination].kind = ROW_NORMAL;
    } else if (!hasHighlight && cf->base[0] && cf->catId[0]) {
      rows[destination].kind = ROW_HIGHLIGHT;
      hasHighlight = 1;
    }
    // MAX_CARDS - 1: the last column belongs to the "See all" card. Without
    // reserving it, a full row would push the card outside the animation array.
    rows[destination].n   = cf->n > 12 ? 12 : cf->n;
    // ONE EXTRA COLUMN: the "See all" card at the end. Only on a row that came from
    // an addon CATALOGUE — "Continue watching" and the Trakt lists have no
    // continuation to ask for (their base is empty).
    rows[destination].seeAll = (cf->base[0] && cf->catId[0]) ? 1 : 0;
    snprintf(rows[destination].base,  sizeof rows[destination].base,  "%s", cf->base);
    snprintf(rows[destination].catId, sizeof rows[destination].catId, "%s", cf->catId);
    snprintf(rows[destination].catKind, sizeof rows[destination].catKind, "%s", cf->kind);
    rows[destination].start = cf->start;
    if(!strcmp(cf->key,"social_activity"))rows[destination].kind=ROW_SOCIAL;
    snprintf(rows[destination].key, sizeof rows[destination].key,
             "%s", cf->key);
    destination++;
  }
  if (col_n()) {
    static Row orig[MAX_FILTER];int total=destination;memcpy(orig,rows,sizeof orig);destination=0;
    // CONTINUE WATCHING IS NOT PART OF THIS ORDER, and has to be placed before
    // anything that is. It is SYNTHETIC — discover.c builds it as row 0 and the
    // web has no ordering key for it at all, because there it is a separate node
    // rendered before `.home-modern-catalogs` and so is structurally always the
    // first row.
    //
    // Re-placing every row from `orig` below has nothing to place it BY: the
    // account's order never names it, so it fell through to the curated pass —
    // behind the pinned collections, and behind every catalogue the account HAD
    // ordered. On an account with an order list that is the bottom of the home,
    // which is the row going missing.
    for(int k=0;k<total;k++) {
      if(strcmp(orig[k].key,"continue_watching"))continue;
      rows[destination++]=orig[k];
      break;
    }
    // pinToTop: the collections the owner pinned go FIRST and are never cut. It
    // is step 5 of the web's algorithm, described in catalog.h and missing here —
    // and without it a collection fell to the end, behind 16 catalogue rows, i.e.
    // off the screen, which is the same as not existing.
    for(int i=0;i<col_n() && destination<MAX_FILTER;i++) {
      const ColFolder *folder=col_folder(i);
      char key[192];
      int already=0;
      if(!folder||!folder->group[0]||!col_group_pinned(folder->group))continue;
      snprintf(key,sizeof key,"collection_%s",folder->group);
      for(int j=0;j<destination;j++) if(!strcmp(rows[j].key,key)){already=1;break;}
      if(already)continue;
      { Row v={0};
        v.n=col_group(folder->group,v.folders,MAX_CARDS);
        if(!v.n)continue;
        v.kind=ROW_CATALOGS;
        snprintf(v.key,sizeof v.key,"%s",key);
        snprintf(v.title,sizeof v.title,"%s",folder->group);
        rows[destination++]=v; }
    }
    // THE ACCOUNT'S ORDER DECIDES. It is a single list interleaving catalogues
    // (`<addonId>_<type>_<catalogId>`) and collections (`collection_<id>`) — the
    // position the person chose in the web app. Collections could previously only
    // appear at the end, behind every catalogue, because nothing here could read
    // that list.
    //
    // The curation table just below no longer decides the ORDER and only
    // DECORATES: it supplies the name and special kind of the rows it recognises.
    // When there is no account order (a fresh account, or a server without the
    // RPC), this loop places nothing and the table becomes the only criterion
    // again, exactly as before.
    for(int i=0;i<disc_prefs_n() && destination<MAX_FILTER;i++) {
      const char *key=disc_prefs_key(i);
      const char *custom;
      int already=0;
      if(!key[0]||disc_prefs_hidden(key))continue;
      if(!strncmp(key,"collection_",11)) {
        // The account identifies a collection by ID; the row is grouped by TITLE.
        // col_group_by_id is the bridge between the two.
        const char *group=col_group_by_id(key+11);
        Row v={0};
        char rowKey[192];
        if(!group||!group[0]) {
          // The order names a collection that is not loaded: deleted on the web,
          // or still on its way. Saying which one avoids hunting for a fault in
          // the wrong place when an expected row fails to appear.
          static int said;
          if(said<4){printf("[home] order names %s, which matches no collection\n",key);said++;}
          continue;
        }
        snprintf(rowKey,sizeof rowKey,"collection_%s",group);
        for(int j=0;j<destination;j++) if(!strcmp(rows[j].key,rowKey)){already=1;break;}
        if(already)continue;             // already placed by the pinToTop pass
        v.n=col_group(group,v.folders,MAX_CARDS);
        if(!v.n)continue;
        v.kind=ROW_CATALOGS;
        snprintf(v.key,sizeof v.key,"%s",rowKey);
        snprintf(v.title,sizeof v.title,"%s",group);
        custom=disc_prefs_title(key);
        if(custom&&*custom)snprintf(v.title,sizeof v.title,"%s",custom);
        rows[destination++]=v;
        continue;
      }
      for(int j=0;j<destination;j++) if(!strcmp(rows[j].key,key)){already=1;break;}
      if(already)continue;
      for(int k=0;k<total;k++) {
        if(strcmp(orig[k].key,key))continue;
        rows[destination]=orig[k];
        decorateRow(&rows[destination]);
        destination++;
        break;
      }
    }
    const char *const *ids=CURATED_ID, *const *names=CURATED_NAME;
    // The known curation takes priority over whatever the account did NOT order,
    // but it is not a cut-off list. The web keeps new keys at the end and the
    // native home has to do the same: catalogues and groups that were not in this
    // table stay reachable.
    for(size_t s=0;s<CURATED_N && destination<MAX_FILTER;s++) {
      if(ids[s][0]=='@') {
        Row v={0};int already=0;
        v.n=col_group(ids[s]+1,v.folders,MAX_CARDS);
        if(!v.n)continue;
        v.kind=ROW_CATALOGS;
        snprintf(v.key,sizeof v.key,"collection_%s",ids[s]+1);
        // May already have been added by the pinToTop pass above.
        for(int j=0;j<destination;j++) if(!strcmp(rows[j].key,v.key)){already=1;break;}
        if(already)continue;
        snprintf(v.title,sizeof v.title,"%s",names[s]);rows[destination++]=v;
      } else for(int k=0;k<total;k++) {
        int dup=0;
        if(strcmp(orig[k].catId,ids[s])&&strcmp(orig[k].key,ids[s]))continue;
        // May already have been placed by the account's order, just above.
        for(int j=0;j<destination;j++) if(!strcmp(rows[j].key,orig[k].key)){dup=1;break;}
        if(dup)break;
        rows[destination]=orig[k];
        snprintf(rows[destination].title,sizeof rows[destination].title,"%s",names[s]);
        if(s==1)rows[destination].kind=ROW_SOCIAL;
        else if(s==2)rows[destination].kind=ROW_HIGHLIGHT;
        else if(s==9||s==10)rows[destination].kind=ROW_TOP10;
        else if(s!=0)rows[destination].kind=ROW_NORMAL;
        destination++;break;
      }
    }
    // Everything that did not match the curation above stays in the order the
    // addon/preference declared. Comparing by key avoids duplicating a special row
    // that has already been promoted.
    for(int k=0;k<total && destination<MAX_FILTER;k++) {
      int watched=0;
      for(int j=0;j<destination;j++) if(!strcmp(rows[j].key,orig[k].key)){watched=1;break;}
      if(!watched) rows[destination++]=orig[k];
    }
    // Extra groups are the user's configuration too. They do not depend on names we
    // knew when the table was written, and each group appears once with all its folders.
    for(int i=0;i<col_n() && destination<MAX_FILTER;i++) {
      const ColFolder *folder=col_folder(i);
      int groupWatched=0;
      if(!folder||!folder->group[0])continue;
      for(int j=0;j<destination;j++) {
        char key[192];snprintf(key,sizeof key,"collection_%s",folder->group);
        if(!strcmp(rows[j].key,key)){groupWatched=1;break;}
      }
      if(groupWatched)continue;
      { Row v={0};
        v.n=col_group(folder->group,v.folders,MAX_CARDS);
        if(!v.n)continue;
        v.kind=ROW_CATALOGS;
        snprintf(v.key,sizeof v.key,"collection_%s",folder->group);
        snprintf(v.title,sizeof v.title,"%s",folder->group);
        rows[destination++]=v;
      }
    }
  }
  for(int i=0;i<destination;i++) {
    Row *s=&rows[i];s->stackN=0;
    if(s->kind==ROW_TOP10 && s->base[0] && s->catId[0]) {
      s->stackN=s->n;s->n=1;s->seeAll=0;
    }
  }
  int socialExists=0;
  for(int i=0;i<destination;i++)if(rows[i].kind==ROW_SOCIAL)socialExists=1;
  if(!socialExists && settings_social_row() && destination<MAX_FILTER) {
    int pos=destination>0?1:0;
    memmove(rows+pos+1,rows+pos,(destination-pos)*sizeof *rows);
    Row *s=&rows[pos];memset(s,0,sizeof *s);
    s->kind=ROW_SOCIAL;s->start=-1;s->n=1;
    snprintf(s->title,sizeof s->title,"Among friends");
    snprintf(s->key,sizeof s->key,"social_activity");destination++;
  }
  // A return from the player is context, not catalogue: it goes above the rows and
  // disappears when there is no incomplete session. It duplicates no data and makes
  // no network calls; it points at the item the player has just updated in memory.
  if (resumeId[0]) resumeIndex = cat_index_by_imdb(resumeId);
  if (resumeIndex >= 0 && destination < MAX_FILTER) {
    memmove(rows + 1, rows, sizeof(Row) * (size_t)destination);
    memset(&rows[0], 0, sizeof rows[0]);
    snprintf(rows[0].title, sizeof rows[0].title, "Resume now");
    snprintf(rows[0].key, sizeof rows[0].key, "last_session");
    rows[0].kind = ROW_RETURN;
    rows[0].start = resumeIndex; rows[0].n = 1;
    destination++;
  }
  nRows = destination;
  // THE HOME'S FINAL ORDER, once per rebuild.
  //
  // The "[disc] row" lines only tell half the story: those are the catalogues, and
  // the interleaving with collections happens here. Without this list there is no
  // way to check the order without someone watching the TV — and the order is
  // precisely what the person configured and expects to recognise.
  //
  // Only when it CHANGES. The catalogue is published row by row, so this function
  // runs ~22 times per launch; printing the whole list each time would drown the
  // log in exactly the section it exists to make readable.
  { static unsigned printed;
    unsigned now = 2166136261u;
    int i;
    for (i = 0; i < nRows; i++)
      for (const unsigned char *s = (const unsigned char *)rows[i].key; *s; s++)
        now = (now ^ *s) * 16777619u;
    if (now != printed) {
      printed = now;
      printf("[home] %d row(s):\n", nRows);
      for (i = 0; i < nRows; i++)
        printf("[home]  %2d %-10s %s\n", i,
               rows[i].kind == ROW_CATALOGS ? "COLLECTION" : "catalogue",
               rows[i].title);
      fflush(stdout);
    } }
  resumeApplied = resumeRev;
  ultimaRevision = revision;
  filtersApplied = nCat;
  prefsApplied = sub;
  memset(animFocus, 0, sizeof animFocus);
  memset(velX, 0, sizeof velX);
  memset(scrollX, 0, sizeof scrollX);
  for (r = 0; r < nRows; r++)
    for (int a = 0; a < nOld; a++)
      if (!strcmp(rows[r].key, old[a].key)) {
        if(rows[r].kind==ROW_TOP10 && old[a].kind==ROW_TOP10 &&
           !old[a].stackN && old[a].seeAll && rows[r].stackN) {
          rows[r].n=rows[r].stackN<10?rows[r].stackN:10;
          rows[r].stackN=0;rows[r].seeAll=1;
        }
        scrollX[r] = oldX[a]; break;
      }
  expRow = expColumn = -1; expOpen = 0.0f;
  if (nRows < 1) return;
  {
    int cols[MAX_FILTER], k;
    // PRESERVE THE FOCUS. focus_start does a memset and zeroes the row and column,
    // and discovery now publishes the catalogue ON EVERY ROW that arrives from the
    // network — that is ~16 publications in the first few seconds. With the reset,
    // the owner's focus would jump to the first card some sixteen times while they
    // try to navigate. Before, this did not show because there was a single publication.
    //
    // The row is found again by the catalogue's KEY, not by index: a new row may come
    // in the middle (the order comes from art/rows.txt), and the old index would then
    // point at something else.
    int found = -1;
    for (k = 0; k < nRows; k++)
      cols[k] = rows[k].n + (rows[k].seeAll ? 1 : 0);
    focus_start(&focus, nRows, cols);
    if (keyFocus[0])
      for (k = 0; k < nRows; k++)
        if (!strcmp(rows[k].key, keyFocus)) { found = k; break; }
    if (found >= 0) {
      focus.row = found;
      // The row may have shrunk between one publication and the next.
      focus.column = colFocus < focus.nColumns[found] ? colFocus
                  : (focus.nColumns[found] > 0 ? focus.nColumns[found] - 1 : 0);
    }
  }
  printf("[home] %d rows from the catalog\n", nRows);
}

// WARMING THE ART THE FOCUS IS ABOUT TO NEED.
//
// The hero's file is downloaded HERE, the moment the candidate changes, and not
// at the swap. Before, the first request for the new art was the tex_get_hero in
// drawHero — which only runs once heroWanted is set, that is, once the 220 ms of
// NV_HERO_IDLE_MS have ALREADY passed. Download, decode and upload were stacked
// AFTER the rest period, one behind the other, and the empty gap the owner sees
// is their sum.
//
// This makes the rest period hold the download instead of preceding it: by the
// time it expires the file is on disk (or on its way) and the swap only has the
// decode left. It cannot go through tex_get_hero — that costs a 1920 texture at
// the moment of the call, which is precisely what NV_HERO_IDLE_MS exists to
// avoid — so it goes down the download-only lane; see tex_prefetch.
//
// The NEIGHBOURS go too, in the direction of travel. Sweeping a row, the card
// after the one under the focus is the likeliest place to stop, and two ahead
// covers the held key. A wrong guess costs a file in the disk cache and nothing
// else: no texture, no slot, no budget.
static void warmHero(int target, int previous) {
  const CatItem *ci;
  const char *art;
  if (target < 0) return;
  // THE LANE SERVES THE LAST THING PUSHED (see tex_prefetch), so this goes from
  // the least urgent to the most: the far neighbour, then the near one, then the
  // title's logo, and the backdrop under the focus LAST — which is the first the
  // lane will fetch.
  if (focus.row >= 0 && focus.row < nRows) {
    const Row *s = &rows[focus.row];
    int column = target - s->start;
    int columnPrev = previous - s->start;
    int steps[2], k;
    // The direction comes from the PREVIOUS candidate, and only when it was on
    // this same row. Arriving from another row there is no direction to speak of
    // — the neighbour is one to each side, with the right-hand one pushed last
    // because that is the way a row is read.
    int direction = 0;
    if (columnPrev >= 0 && columnPrev < s->n)
      direction = (column > columnPrev) - (column < columnPrev);
    if (direction > 0)      { steps[0] =  2; steps[1] =  1; }
    else if (direction < 0) { steps[0] = -2; steps[1] = -1; }
    else                    { steps[0] = -1; steps[1] =  1; }
    for (k = 0; k < 2; k++) {
      int c = column + steps[k];
      const char *a;
      if (c < 0 || c >= s->n) continue;
      a = art_of_card(s->kind, s->start + c, 1);
      if (a) tex_prefetch(a);
    }
  }
  // The logo is the hero's OTHER download, and the copy waits for it — see
  // NV_HERO_LOGO_WAIT_MS. Warming only the backdrop would bring the art forward
  // and leave the text lagging behind it, which is the same defect one layer up.
  ci = cat_item_exact(target);
  if (ci && ci->logo[0]) tex_prefetch(ci->logo);
  art = art_by_identity(target, 1);
  if (art) tex_prefetch(art);
}

void home_update(float dt, Uint32 now) {
  syncRows();

  const int motionReduced = settings_animations_reduced();
  // NV_HOLD_FEEDBACK_MS IS THE SILENCE BEFORE THE BAR, NOT THE WHOLE GESTURE.
  // Dividing by it filled the rail — and fired ctx_open right below — after 110ms,
  // which is INSIDE an ordinary tap on a remote: a deliberate press of OK lasts
  // 100-200ms, so most taps opened the modal instead of the title, and the ones that
  // did not were simply the fastest. The threshold is NV_HOLD_MS, the same 500ms
  // detail.c and episodes.c measure on their own KEYUP; the bar appears once the
  // press is clearly not a tap and fills over what is left of the 500.
  if (okPressing && focus_can_press_long()) {
    Uint32 held = now - okSince;
    okHold = held <= NV_HOLD_FEEDBACK_MS
               ? 0.0f
               : anim_clamp((held - NV_HOLD_FEEDBACK_MS) /
                              (NV_HOLD_MS - NV_HOLD_FEEDBACK_MS), 0.0f, 1.0f);
  } else if (!okPressing)
    okHold = 0.0f;
  if (okPressing && okHold >= 1.0f && !okLongFired) {
    okLongFired = 1;
    okConsumeRelease = 1;
    okPressing = 0;
    okSince = 0;
    // The context menu is still the owner of the actions and the UI. The home only
    // fires once at the threshold and consumes the following KEYUP.
    ctx_open(rows[focus.row].start + focus.column);
  }

  // The catalogue may shrink between two responses. Normalising the carousel's
  // indices stops a stale state falling into cat_item()'s wrap-around.
  { int total = nArchiveHero();
    if (heroCurrent < 0 || heroCurrent >= total) heroCurrent = 0;
    if (heroPrevious < 0 || heroPrevious >= total) heroPrevious = heroCurrent;
    if (heroPending < 0 || heroPending >= total) heroPending = heroCurrent;
  }

  // THE CHANGE OF ROW KIND, detected HERE and not in the drawing, because this is the
  // one pass that runs once a frame and runs BEFORE anything is drawn: heroShot still
  // holds what the family being left had on screen, and the poster candidate below can
  // be brought forward in the same pass.
  int famChanged = 0;
  { HeroFamily fam = familyOfRow(focus.row);
    if (!heroFamilyKnown) { heroFamily = fam; heroFamilyKnown = 1; }
    else if (fam != heroFamily) {
      famChanged = 1;
      heroFamily  = fam;
      heroLeaving = heroShot;
      famFade = motionReduced ? 0.0f : 1.0f;
      // Nothing to hold on to — the family being left was drawing no art — so there is
      // nothing to wait for either: the arriving one simply comes up over the
      // background, which still beats appearing between two frames.
      famHold   = (!motionReduced && heroLeaving.valid) ? 1 : 0;
      famHoldIn = now;
      // THE COLLECTION HERO'S LAG IS MEASURED AGAINST THE FOLDER IT LAST SHOWED, and
      // arriving from another family that folder is one the viewer left behind — very
      // likely on a different row. Kept, it would fade the family in on a stale cover
      // and then run colHeroFade a second time to reach the focused one: two dissolves
      // for one keypress. Dropped, the only fade is the family's, which is the one the
      // movement actually was.
      if (fam == FAM_COLLECTION) {
        colHeroCurrent = colHeroPrevious = -1;
        colHeroFade = 0.0f;
        colWarmFor = -1;
      }
    }
  }

  // THE HERO FOLLOWS THE FOCUS. The owner's request, and a DELIBERATE DIVERGENCE
  // from the web app: I measured twice with the focus really moving (the card changed
  // from "54 minutes left" to "1h 12m left") and the `.home-hero-backdrop`'s art
  // stayed the same — in the web app it does not follow the selection. It is recorded
  // so nobody "corrects" this back thinking it is a deviation: it is a chosen
  // improvement, not a mistake.
  //
  // While there is focus on a card, the automatic carousel does not run: two sources
  // touching the same art would give changes on top of the user's choice.
  {
    int target = -1;
    // `driven` answers "is a card focused at all", which is NOT the same question as
    // "does the focused card name a title". Three cases give a target of -1 while the
    // focus is very much on something: a collection row (its hero is drawn on its own
    // path, further down), the see-all card at the end of a row, and the social row's
    // empty state. Reading -1 as "nobody is here" handed all three back to the
    // automatic carousel — so parking on a See all rotated the hero every 7 s, and
    // walking a collection row left the carousel turning UNSEEN behind its hero, then
    // flashed whatever it had landed on when the focus returned to a poster row.
    int driven = 0;
    if (focus.row >= 0 && focus.row < nRows) {
      driven = 1;
      int i = rows[focus.row].start + focus.column;
      if (rows[focus.row].kind == ROW_CATALOGS)
        i = -1;
      else if (focus.column >= rows[focus.row].n) i = -1;
      if (i >= 0 && i < cat_n()) target = i;
    }
    // IT ONLY SWAPS WITH THE FOCUS AT REST. `heroPending` is the candidate; while the
    // owner moves along the row it changes at every step and the clock restarts, so
    // no swap ever happens. When the focus stops for NV_HERO_IDLE_MS, the candidate
    // becomes the hero.
    //
    // This is not only aesthetic: each swap asks for a 1920 texture (~8 MB), and
    // crossing a row asked for a dozen of them in two seconds — the cache overflowed
    // and evicted the visible posters. See the note in layout.h.
    if (target >= 0 && target != heroPending) {
      int previous = heroPending;
      heroPending = target;
      heroPendingIn = now;
      // Back to the art that is already on screen: it cancels the swap that has not
      // happened yet, otherwise it would fire later with nobody having asked for it.
      if (heroPending == heroCurrent) heroWanted = -1;
      // THE DOWNLOAD STARTS HERE, not at the swap — see warmHero. This is the
      // one place that knows the candidate has changed, and it runs once per
      // keypress, not once per frame.
      warmHero(target, previous);
    }
    // ARRIVING FROM ANOTHER KIND OF ROW, THE REST PERIOD DOES NOT APPLY.
    //
    // NV_HERO_IDLE_MS exists so that CROSSING a row does not change the hero at every
    // step. Coming down onto a poster row is not that: it is one move, and the picture
    // being replaced belongs to a different drawing altogether. Waiting it out would
    // dissolve to whatever backdrop the poster hero happened to be left on and then,
    // 220 ms later, dissolve AGAIN to the focused title — two fades where the viewer
    // made one movement. Back-dating the clock lets the family's own fade carry the
    // right art in, once. A vertical walk still cannot thrash: every step changes the
    // row, so the candidate changes and this runs once per step, not once per frame.
    if (famChanged && target >= 0) heroPendingIn = now - NV_HERO_IDLE_MS;
    if (target >= 0 && heroPending != heroCurrent &&
        now - heroPendingIn >= NV_HERO_IDLE_MS) {
      // It only ANNOUNCES the wish. What carries out the swap is the drawing, once
      // the new art's texture is ready — see heroWanted.
      heroWanted = heroPending;
    } else if (target < 0 && !driven && now >= heroSwapIn) {
      // With no card in focus — genuinely none, which on a populated home means the
      // rows have not been built yet — it schedules the next item and lets the drawing
      // carry out the swap only once the texture or the placeholder is ready.
      int total = nArchiveHero();
      int next = total > 0 ? (heroCurrent + 1) % total : 0;
      heroPending = next;
      heroPendingIn = now - NV_HERO_IDLE_MS;
      heroWanted = next;
      heroSwapIn = now + NV_HERO_INTERVAL_MS;
    }
  }
  // The expansion's clock. It resets on every movement; it counts only with the focus
  // still.
  if (settings_expand_poster()) {
    if (focus.row != expRow || focus.column != expColumn) {
      expRow = focus.row; expColumn = focus.column;
      expSince = now;
      expOpen = 0.0f;            // it closes at once; it is opening that is gradual
    }
    { float delay = settings_expand_poster_delay();
      int ready = expSince && (now - expSince) >= (Uint32)(delay * 1000.0f);
      // A LANDSCAPE row (and the continue-watching one) already shows the wide art:
      // there is nothing to expand into.
      if (ready && canExpand(focus.row))
        expOpen = motionReduced ? 1.0f
                   : anim_spring(expOpen, 1.0f, dt, NV_SPRING_SCREEN); }
  } else {
    expOpen = 0.0f; expRow = expColumn = -1;
  }

  if (heroExits > 0.0f) {
    heroExits -= dt * (1000.0f / NV_HERO_FADE_MS);
    if (heroExits < 0.0f) heroExits = 0.0f;
    heroEnters = motionReduced ? 1.0f : 1.0f - heroExits;
  } else {
    heroEnters = 1.0f;
  }
  if (colHeroFade > 0.0f) {
    colHeroFade -= dt * (1000.0f / NV_HERO_FADE_MS);
    if (colHeroFade < 0.0f) colHeroFade = 0.0f;
  }
  // The hold is released by the DRAWING, which is the only place that knows whether the
  // arriving family has its art — and bounded here, because art that never lands must
  // not leave the hero of a row the viewer has already walked away from standing for
  // the rest of the session. See NV_HERO_FAMILY_WAIT_MS.
  if (famHold && now - famHoldIn >= NV_HERO_FAMILY_WAIT_MS) famHold = 0;
  if (famFade > 0.0f && !famHold) {
    famFade -= dt * (1000.0f / NV_HERO_FADE_MS);
    if (famFade < 0.0f) famFade = 0.0f;
  }

  // THE COPY'S FADE, on the art's clock and at the art's duration.
  //
  // It follows the ART, which is not the same as following the FOCUS: the swap is
  // held back until the new backdrop has decoded (see heroWanted), so the old
  // copy stays up for the whole download and both change at the commit. heroCopy
  // is zeroed there — the old copy is simply GONE from that frame, and only the
  // new one fades. See the note at the call in drawHero for why this one does not
  // crossfade the way the art does.
  //
  // The one thing it adds over the art: it does not come up until every line of
  // the block is rasterised. That is what stops the description arriving two
  // lines at a time.
  //
  // ONCE FULLY IN IT STAYS IN. `heroCopyReady` is recomputed every frame from
  // text.c's cache, and a single eviction there would otherwise tear the whole
  // block down and fade it back for no reason the viewer can see.
  { float copyTarget = (heroCopyReady || heroCopy >= 0.999f) ? 1.0f : 0.0f;
    heroCopy = motionReduced ? copyTarget
                             : anim_ramp(heroCopy, copyTarget, dt, NV_HERO_FADE_MS);
    // While the block itself is coming in, the logo takes the block's alpha
    // directly: giving it a ramp of its own there would fade it a second time,
    // over the fade it is already inside. The ramp is only for the logo that
    // lands late, with the text already standing.
    float logoTarget = heroLogoReady ? 1.0f : 0.0f;
    if (heroCopy < 0.999f || motionReduced) heroLogo = logoTarget;
    else heroLogo = anim_ramp(heroLogo, logoTarget, dt, NV_HERO_FADE_MS); }

  // The automatic focus walk was only there to see the prototype moving with nobody
  // on the remote. With the app navigable it gets in the way: it steals the focus in
  // the middle of any test.

  for (int r = 0; r < nRows; r++) {
    int nAnim = rows[r].n + (rows[r].seeAll ? 1 : 0);
    if (nAnim > MAX_CARDS) nAnim = MAX_CARDS;
    for (int c = 0; c < nAnim; c++) {
      float target = focus_index(&focus, r, c) ? 1.0f : 0.0f;
      animFocus[r][c] = motionReduced
                     ? target
                     : anim_spring(animFocus[r][c], target, dt,
                                 target > animFocus[r][c] ? NV_SPRING_FOCUS : NV_SPRING_BLUR);
    }
    if (r == focus.row) {
      // It scrolls only as far as needed for the focused item to fit in the usable
      // area. Shifting in proportion to the column, as it used to, threw the first
      // card off screen as soon as the focus moved to the second — content disappears
      // on the left without the user having gone over there.
      float lw = rows[r].stackN ? 680.0f : widthOf(rows[r].kind);
      float step = lw + gapOf(rows[r].kind);
      float left = (float)focus.column * step;
      float dir = left + lw;
      float util = NV_SCREEN_W - settings_content_x() - NV_HOME_SAFE_RIGHT;
      float target = scrollX[r];
      // the slack covers the focus's growth: the card grows on both sides, and
      // without reserving that half it touches the edge when it takes focus
      float slack = lw * scaleOf(rows[r].kind) * 0.5f;
      if (dir + slack - target > util)  target = dir + slack - util;
      if (left - target < 0.0f)  target = left;
      if (target < 0) target = 0;
      scrollX[r] = anim_spring2_reduced(&velX[r], scrollX[r], target, dt,
                                       NV_SPRING2_SCROLL, motionReduced);
    }
  }

  // THE FOCUSED ROW ALWAYS STAYS AT THE SAME Y, and the ones above DISAPPEAR.
  //
  // Observed in the owner's two reference captures: with the focus on "Continue
  // watching" that title appears at the same height at which, on going down one row,
  // "For You - Film" appears. The previous row does not rise — it stops being drawn.
  // In their words: "when you go down a line the things disappear, they don't rise".
  //
  // What was here was a CAMERA: it kept the focused row inside a viewport and scrolled
  // the minimum needed. That slides everything upwards, and it was what made the
  // hero's text pass over the row's title.
  //
  // The offset is the sum of the rows BEFORE the focused one, so the focused one's
  // top falls exactly on the shelf's resting line (NV_SHELF_TOP + NV_SHELF_PAD_TOP,
  // measured at 564.4 in the web whatever the scroll position). It still has a spring:
  // a hard jump between rows of different heights reads as a cut, not as navigation.
  float targetY = 0.0f;
  { int r = focus.row;
    for (int i = 0; i < r && i < nRows; i++)
      targetY += NV_LEGACY_ROW_HEAD_H + heightTotalOf(rows[i].kind) + rowGap();
  }
  scrollY = anim_spring2_reduced(&velY, scrollY, targetY, dt,
                                NV_SPRING2_SCROLL, motionReduced);
}

// The media occupies the right of the top 650px; the text sits in the left block and
// the rows scroll in an independent viewport below. The hero takes no focus: spatial
// navigation starts on the first card, as in the legacy DOM.
// The rect of the hero's ART on the last frame. The detail screen reads this to start
// its backdrop EXACTLY where the art already was, instead of appearing from nowhere:
// the background is the title's own, so it should neither flash nor grow.
static GfxRect heroArtRect = { 0, 0, NV_SCREEN_W, NV_SCREEN_H };
void home_hero_rect(float *x, float *y, float *w, float *h) {
  *x = heroArtRect.x; *y = heroArtRect.y;
  *w = heroArtRect.w; *h = heroArtRect.h;
}

// ONE separator dot on the hero's meta line, and it is the BULLET.
//
// The line was drawn with two different characters. The catalogue writes its own
// separators as U+00B7 MIDDLE DOT ("  \xc2\xb7  ", catalog.c:174) and they arrive baked
// into `genre` and `meta`; the group join here was U+2022 BULLET. Side by side in one
// sentence the middle dot does not read as a different KIND of separator, it reads as
// the same dot rendered smaller and fainter — a size change nobody chose.
//
// Converted HERE, on the way to the screen, and not in the catalogue: detail.c (1330,
// 1644) and catalog.c (777) split those same strings by SEARCHING for the U+00B7, and
// changing what is stored would break every one of those parsers at once.
//
// The spaces around the dot are normalised with it, so a "  \xc2\xb7  " from the file and
// the "   \xe2\x80\xa2   " written here end up identical.
// `bulletize` lived here: it walked a joined string and swapped each "·" for a
// spaced "•". It went with the joined string itself — the hero's meta line is now a
// list of TOKENS, because the separators have to be drawn dimmer than the words and one
// TxtLine can only carry one colour. See the NV_HERO_META_* block in layout.h.

// `output` = 0..1 of how much the detail has already taken over the screen. Only the
// hero's TEXT leaves (it drops and fades); the art stays put, because it is the same
// art the detail will use. That was what was missing for the opening to read as a
// rearrangement of the layout and not as a change of screen.
// THE HERO'S COPY, laid out and drawn as ONE thing.
//
// It lives in a function of its own because the layout and the drawing cannot be
// separated here: the block stacks upward from a fixed base, so its position is
// only known once every line of it has been measured. That is also why it is
// called during the gap, at alpha 0.
//
// Returns how many lines text.c had to go and rasterise (or refused to, on the
// TXT_PER_FRAME budget). ZERO means the block is settled and can be shown whole.
// It is deliberately called with alpha 0 while the block is still coming in: it
// is the LAYOUT that asks text.c for the lines, so a block nobody lays out never
// becomes ready.
// `warm` LAYS THE BLOCK OUT WITHOUT SHOWING IT, for a title that is not the hero
// yet. It is the copy's half of the warming warmHero does for the art: the whole
// function runs, so every txt_* call puts its line in text.c's cache and the
// logo's decode is started, but nothing is drawn and — the part that matters —
// heroCopyReady and heroLogoReady are NOT written, because those two describe
// the block that is ON SCREEN and the caller is asking about a different title.
//
// It costs the rasteriser's budget and nothing else, which is why the caller runs
// it AFTER the visible copy: TXT_PER_FRAME is 2 for the whole frame, and the
// block that is coming in has to have first claim on it.
static int drawHeroCopy(const CatItem *ci, float alpha, float slideDownCopy,
                        int full, int warm) {
  int missBefore = txt_misses;
  // Belt and braces with the caller: warming draws nothing. txt_draw_alpha and
  // txt_block already discard a line at alpha 0 AFTER rasterising it (see the
  // note in text.c) — which is exactly the asymmetry this pass is built on.
  if (warm) alpha = 0.0f;
  // THE HERO'S TEXT BLOCK — transcribed from the web app's CSS, not deduced from a
  // capture. `.home-modern-hero-copy` is a flex column with justify-content flex-end
  // and gap 16, anchored to a fixed base; the children, in order:
  //   .home-hero-brand         the logo's box, 440x200, art at the top left
  //   .home-modern-hero-meta-line   21/500 #b3b3b3, tokens separated by •
  //   .home-modern-hero-secondary   18/600 white 88%, with the badges and the IMDb
  //   .home-hero-description        24/400 white, leading 35 (the modern rule;
  //                                 `.legacy-webos` would drop it to 22/30/560)
  // Each empty block disappears (`.is-empty { display: none }`), and that is why the
  // set's height changes from title to title — not by absolute position.
  //
  // Each line's content comes from buildModernHeroPresentation
  // (homeScreen.js:2497), which separates the "continue watching" case from the rest.
  int contHero = (ci && ci->progress > 0 && ci->remainingMin > 0);

  // The meta line. In the web app these are tokens joined by "•"; ci->genre already
  // arrives as "Film · Horror", which is the web app's (type, first genre) pair.
  // THE META LINE IS A LIST OF TOKENS, not one pre-bulleted string, because the
  // separators have to be drawn DIMMER than the words — see NV_HERO_META_DOT. Joining
  // them into a single TxtLine, which is what this did, forces one colour on both and
  // is what made the line read as clutter.
  char metaBuf[288];
  const char *tok[NV_HERO_META_MAXTOK];
  int nTok = 0, nLead = 0;
  metaBuf[0] = 0;
  { size_t o = 0;
    // Copies into metaBuf and returns a pointer to it: the tokens have to outlive the
    // locals they came from, and ci->genre / ci->meta are split in place.
    #define META_PUSH(S) do { \
        const char *s_ = (S); \
        if (s_ && s_[0] && nTok < NV_HERO_META_MAXTOK && o + strlen(s_) + 1 < sizeof metaBuf) { \
          tok[nTok++] = metaBuf + o; \
          o += (size_t)snprintf(metaBuf + o, sizeof metaBuf - o, "%s", s_) + 1; \
        } } while (0)

    // THE STREAMING SERVICE, AS TEXT. It used to be a LOGO drawn ahead of the line by
    // badges_draw, and the owner's call is that the name reads better than the mark:
    // "dont need logos to indicate what streaming service it is on, just text is
    // enough". It leads the group that says what the title IS.
    //
    // NuvioWeb shows no provider on the hero at all, so this is a deliberate
    // divergence and not a measurement — it is information the owner asked for, in the
    // place the line already had for it.
    if (ci && ci->providerName[0]) META_PUSH(ci->providerName);
    if (contHero && ci && ci->season > 0) {
      char header[64];
      snprintf(header, sizeof header, "S%d E%d", ci->season, ci->episode);
      META_PUSH(header);
    }
    // ci->genre arrives "·"-joined as (type, genre, genre...). ONE genre, like the
    // measured line: "Movie • Action", not "TV Show • Reality • Romance". The third
    // token onwards is where the line stopped being scannable.
    if (ci && ci->genre[0]) {
      char g[128]; snprintf(g, sizeof g, "%s", ci->genre);
      char *cur = g; int taken = 0;
      while (cur && *cur && taken < 2) {
        char *dot = strstr(cur, "\xc2\xb7");
        if (dot) *dot = 0;
        { char *e = cur + strlen(cur); while (e > cur && e[-1] == ' ') *--e = 0; }
        while (*cur == ' ') cur++;
        META_PUSH(cur);
        taken++;
        cur = dot ? dot + 2 : NULL;
      }
    }
    // Everything above says what the title IS; everything below is its numbers. The
    // boundary is drawn as a wider gap, not as another mark.
    nLead = nTok;
    if (ci && ci->meta[0]) {
      char m[128]; snprintf(m, sizeof m, "%s", ci->meta);
      char *cur = m;
      while (cur && *cur) {
        char *dot = strstr(cur, "\xc2\xb7");
        if (dot) *dot = 0;
        { char *e = cur + strlen(cur); while (e > cur && e[-1] == ' ') *--e = 0; }
        while (*cur == ' ') cur++;
        META_PUSH(cur);
        cur = dot ? dot + 2 : NULL;
      }
    }
    #undef META_PUSH
  }
  // The layout below only ever asked "is there a line at all".
  char metaLine[2]; metaLine[0] = nTok ? 'x' : 0; metaLine[1] = 0;

  // The secondary line: the progress highlight, and nothing else. The age badge has left
  // the hero, and the IMDb score went up to the end of the meta line with it — the web
  // app's showImdbSecondary kept the score down here to give this line company, which is
  // not a reason to separate it from the year and the runtime it belongs with.
  char highlight[64];
  highlight[0] = 0;
  if (contHero) snprintf(highlight, sizeof highlight, "%d MINUTES LEFT",
                         ci->remainingMin);
  char score[8];
  score[0] = 0;
  if (ci && ci->score > 0) snprintf(score, sizeof score, "%.1f", ci->score / 10.0f);
  // The empty line COLLAPSES, it is not drawn blank: `.is-empty { display: none }` on
  // the flex column, which is what lifts the copy back up on a title with no highlight.
  int hasSec = (highlight[0] != 0);

  const char *synopsis = (ci && ci->synopsis[0]) ? ci->synopsis : "";

  // --- stacking from the bottom up, like the CSS's flex-end ---
  // 48 above the FIRST ROW'S TITLE, not above the rows viewport: see the note on
  // NV_HERO_COPY_GAP for why this one number leaves the web's own anchor behind.
  float base = NV_SHELF_TOP + NV_SHELF_PAD_TOP - NV_HERO_COPY_GAP + slideDownCopy;

  // HOW MANY LINES OF SYNOPSIS. Not a constant in the web either: the CSS clamp is 4,
  // and applyModernHeroDescriptionBounds (homeScreen.js:6531) then lowers it to
  // however many WHOLE lines are left in the copy box once the logo, the meta line
  // and the secondary have taken their share — `floor(available / lineHeight)`,
  // capped at 4. That is the rule ported here, and not a fixed number, because it is
  // what makes the block breathe: with the "N MINUTES LEFT" line present three lines
  // fit and with it absent four do, in both apps.
  //
  // A fixed 4 was what this used to pass, and it is what pushed the whole block up:
  // a fourth line of 35 that the web was not drawing lifted the logo by exactly that
  // much, on top of the gap and base errors recorded on the constants.
  //
  // MEASURED AND DRAWN WITH THE SAME COUNT: the two txt_block calls have to agree or
  // the copy stacks against a height it is not drawn at.
  float reserved = NV_LOGO_HERO_H + NV_HERO_COPY_LINE + NV_HERO_SIN_MARGIN
                 + (metaLine[0] ? NV_HERO_COPY_LINE + NV_LD_HERO_META : 0.0f)
                 + (hasSec      ? NV_HERO_COPY_LINE + NV_LD_HERO_SEC  : 0.0f);
  int nSin = (int)floorf((NV_HERO_COPY_H - reserved) / NV_LD_HERO_SIN);
  if (nSin > 4) nSin = 4;
  if (nSin < 1) nSin = 1;

  float hSin = synopsis[0] ? txt_block(TXT_HERO_SIN, synopsis, 255, 255, 255, -1, 0,
                                      NV_HERO_SIN_W, NV_LD_HERO_SIN, 0.0f, nSin)
                          : 0.0f;
  float ySin  = base - hSin;
  // The synopsis carries its own 4px of margin on top of the column's gap; the rest
  // of the block is spaced by the gap alone.
  float ySec  = hasSec ? (ySin - (synopsis[0] ? NV_HERO_COPY_LINE + NV_HERO_SIN_MARGIN
                                              : 0.0f)
                          - NV_LD_HERO_SEC) : ySin;
  float yMeta = ySec - (hasSec ? NV_HERO_COPY_LINE
                              : (synopsis[0] ? NV_HERO_COPY_LINE + NV_HERO_SIN_MARGIN
                                             : 0.0f))
                - (metaLine[0] ? NV_LD_HERO_META : 0.0f);
  float x = settings_content_x();

  // The title's logo, or the name in text when there is no logo
  // (.home-hero-title-text, 56/600 in modern — not TXT_TITLE1's 76).
  // ASKED FOR AT THE WIDTH IT IS DRAWN, not at tex_get's blanket 640. At
  // NV_LOGO_HERO_FULL_MAX_W the two numbers happen to be the same 640, and the
  // art was landing decoded at exactly its drawn size with none of the slack
  // NV_TEX_SLACK exists to give — the shader samples with filtering, so 1:1 is
  // the point where a logo starts to look soft rather than the point where it
  // stops.
  GLuint tlogo = (ci && ci->logo[0])
      ? tex_get_width(ci->logo, full ? NV_LOGO_HERO_FULL_MAX_W
                                     : NV_LOGO_HERO_MAX_W) : 0;

  // THE ART'S OWN HEIGHT IS WHAT THE STACK MEASURES FROM, not the box's.
  // NV_LOGO_HERO_H is a CEILING the art is fitted into, and the art is fitted by
  // WIDTH: any wordmark wider than maxW/NV_LOGO_HERO_H comes out SHORTER than the
  // box, and with the art hung from the box's top (object-position: left top) the
  // leftover was left standing as dead space between the logo and the meta line.
  //
  // That leftover was the whole of the inconsistency. From the one constant the gap
  // measured 12 under a mark square enough to fill the box and ~139 under a wide
  // one — the shape of the art, not the layout, deciding the spacing.
  //
  // The web does not have the problem because .home-hero-logo carries no fixed
  // height: it is the art's own box inside the flex column (MEASURED 640x160, see
  // layout.h), so the column's gap is all that ever sits beneath it. The fixed box
  // is the port's, and so was the bug.
  //
  // Sized HERE rather than at the draw because logoY needs the height. Note that
  // nSin above deliberately goes on reserving the full NV_LOGO_HERO_H: the art can
  // only come out shorter than the box, never taller, so the block cannot overflow,
  // and keeping the synopsis's line count clear of which logo happened to land is
  // what stops it reflowing when a late one arrives.
  float wTitle = 0.0f, hTitle = 0.0f;
  if (tlogo) {
    float ap = tex_aspect(ci->logo);
    if (ap <= 0.0f) ap = 4.0f;
    float maxW = full ? NV_LOGO_HERO_FULL_MAX_W : NV_LOGO_HERO_MAX_W;
    hTitle = NV_LOGO_HERO_H; wTitle = hTitle * ap;
    if (wTitle > maxW) { wTitle = maxW; hTitle = wTitle / ap; }
  }
  // With no logo the name is drawn bottom-aligned inside the full box, so there the
  // box IS the row. Either way it is the row's BASE the gap hangs from.
  float hLogo = tlogo ? hTitle : NV_LOGO_HERO_H;
  float logoY = yMeta - NV_HERO_LOGO_GAP - hLogo;

  if (tlogo) {
    // object-position: left top — the art sits at the TOP of its box, which is now
    // the art's own height, so that edge is also its base.
    GfxRect rl = { x, logoY, wTitle, hTitle };
    gfx_tex_aspect_current = 0.0f;
    // A dark logo becomes white. The same rule as the detail screen's: TMDB does not
    // mark light/dark, so the decision comes from the MEASURED luminance
    // (tex_luminance). A light or colourful logo passes through untouched; -1 (still
    // loading) does not tint.
    { GfxMode m = tex_brand_dark(ci->logo) ? GFX_BRAND : GFX_TEXT;
      // THE TITLE'S LOGO GOES WITH THE REST OF THE COPY. It used to be gated on
      // heroEnters alone, which meant it disappeared at the swap and came back on
      // the frame the new art did — and if its own download was not finished by
      // then it simply popped in later, on its own, which is half of what the
      // owner saw as the hero "streaming in".
      //
      // heroCopy already waits for it (NV_HERO_LOGO_WAIT_MS). anim_smooth(heroLogo)
      // is only for the logo that misses that window: it is 1 in the ordinary case
      // and rides heroCopy, and fades in by itself when it lands late.
      //
      // FULLY TRANSPARENT IS NOT SUBMITTED, the same rule txt_draw_alpha applies
      // to every line (text.c) and for the same reason: gfx_rect has no alpha
      // early-out of its own, so an invisible logo still cost geometry. Two cases
      // reach it — the warm pass, which runs the whole layout at 0, and the first
      // frames of the ordinary fade, where heroCopy starts there.
      float aLogo = alpha * anim_smooth(heroLogo);
      if (aLogo > 0.004f)
        gfx_rect(rl, tlogo, m, 0, 0, 0, 0.0f, 1, 1, 1, aLogo); }
  } else {
    // .legacy-webos .home-hero-title-text: 76px (components.css:19164), not the
    // default theme's 56.
    // With no title, do NOT invent a title. There used to be a demo list here
    // ("Severance", "Silo", "Shrinking"...) that filled the hero with another
    // series' name when the item did not have one yet — indistinguishable from real
    // data to whoever is looking at the screen. The same family as the cast and the
    // age rating that have already left the detail screen. With no name, the hero
    // keeps just the art, which is enough, and the text appears when the data arrives.
    if (ci && ci->title[0]) {
      TxtLine title = txt_line(TXT_TITLE1, ci->title, 255, 255, 255, 255);
      txt_draw_alpha(title, x, logoY + hLogo - (float)title.h,
                         alpha);
    }
  }

  if (metaLine[0]) {
    // TOKEN BY TOKEN, with the separators DIMMER than the words. That is the whole
    // difference between this line and the one that read as clutter: the web draws the
    // tokens at rgba(255,255,255,.62) and every dot at .34, so the eye groups the words
    // and the dots fall back to punctuation. Drawn as one string — which is what this
    // did — both are forced to the same flat grey and every dot competes.
    //
    // THE PROVIDER LOGO HAS GONE WITH IT. badges_draw used to paint a service mark
    // ahead of the line; the name is now the line's first token.
    //
    // WIDTH: to the safe right edge, NOT to the synopsis's 640. In the web app only
    // .home-hero-description carries a width; the meta line has none. Sharing the
    // description's cap left "Film • Comedy • Drama • 2026 • 107 min" ellipsised
    // mid-line.
    //
    // The score is subtracted from the budget because it is drawn AFTER the tokens end:
    // without it there, a long enough genre would run under the chip.
    TxtLine ln = { 0, 0, 0 };
    float imdbW = 0.0f;
    if (score[0]) {
      ln = txt_line(TXT_HERO_META, score, 179, 179, 179, 255);
      imdbW = NV_HERO_IMDB_GAP + NV_HERO_IMDB_W + 10.0f + (float)ln.w;
    }
    float limit = NV_SCREEN_W - NV_HOME_SAFE_RIGHT - imdbW;
    int ink  = (int)(255.0f * NV_HERO_META_INK  + 0.5f);
    int dim  = (int)(255.0f * NV_HERO_META_DOT  + 0.5f);
    float cx = x, hLine = 0.0f;
    int drawn = 0;
    for (int i = 0; i < nTok; i++) {
      TxtLine lt = txt_line(TXT_HERO_META, tok[i], ink, ink, ink, 255);
      float sep = 0.0f, lead = 0.0f;
      TxtLine ld = { 0, 0, 0 };
      if (drawn) {
        // The group boundary KEEPS ITS DOT and widens the space BEFORE it. Measured:
        // the lead group ends at 370.7, the trailing group opens at 384.7 (a gap of 14)
        // and its first dot sits there, with the usual 12 after it. Dropping the dot
        // entirely — which is what the first attempt did — reads as a missing separator,
        // not as a group.
        lead = (i == nLead) ? NV_HERO_META_GROUP : NV_HERO_META_SEP;
        ld = txt_line(TXT_HERO_META, "\xe2\x80\xa2", dim, dim, dim, 255);
        sep = lead + (float)ld.w + NV_HERO_META_SEP;
      }
      // A token that does not fit is dropped WHOLE, with its separator. Trimming it
      // mid-word, which txt_line_trim did to the joined string, left an ellipsis in the
      // middle of a genre.
      if (cx + sep + (float)lt.w > limit) break;
      if (drawn) {
        txt_draw_alpha(ld, cx + lead,
                       yMeta + ((float)lt.h - (float)ld.h) * 0.5f, alpha);
        cx += sep;
      }
      txt_draw_alpha(lt, cx, yMeta, alpha);
      cx += (float)lt.w;
      if ((float)lt.h > hLine) hLine = (float)lt.h;
      drawn++;
    }
    if (score[0] && drawn) {
      // .home-hero-imdb: the 40px mark and the score just after it, with 10 of breathing
      // room. THE REAL MARK, art/icons/imdb_logo.png — the same file the title screen's
      // meta line draws, and the one NuvioWeb serves from `renderImdbBadge`.
      //
      // It used to be a HAND-BUILT PLATE: a 40px yellow rectangle with "IMDb" set in
      // TXT_MINI and centred in it, on the grounds that the SVG is not packaged. The
      // PNG is, and the letters of the real mark run edge to edge — which is the whole
      // design of it, and no amount of nudging a font size reproduces that.
      //
      // The score stays at 179 grey — measured BRIGHTER than the tokens around it,
      // because it is the one number on the line anyone looks for.
      float bx = cx + NV_HERO_META_SEP * 2.0f;
      { TxtLine ld = txt_line(TXT_HERO_META, "\xe2\x80\xa2", dim, dim, dim, 255);
        txt_draw_alpha(ld, cx + NV_HERO_META_SEP,
                       yMeta + (hLine - (float)ld.h) * 0.5f, alpha);
        bx = cx + NV_HERO_META_SEP * 2.0f + (float)ld.w + NV_HERO_IMDB_GAP; }
      // THE HEIGHT FOLLOWS THE FILE, not a constant: forcing the mark into a fixed box
      // would stretch it by whatever the rounding left over. NV_IMDB_MARK_AR stands in
      // for the one frame before the decode lands — the same two-step the hero logo does.
      const char *fileMark = gfx_icon_path("imdb_logo");
      float arMark = tex_aspect(fileMark);
      float sh = NV_HERO_IMDB_W / (arMark > 0.0f ? arMark : NV_IMDB_MARK_AR);
      float sy = yMeta + (hLine - sh) * 0.5f;
      // Invisible is not submitted — see the note on the logo above.
      // GFX_TEXT keeps the texture's RGB *and* its alpha, the only mode that can draw
      // black letters on a yellow plate; gfx_icon's GFX_BRAND would take the alpha alone
      // and flatten both into one tint.
      GLuint markImdb = tex_get_exact(fileMark, NV_IMDB_MARK_TEX_W);
      if (markImdb && alpha > 0.004f)
        gfx_rect((GfxRect){ bx, sy, NV_HERO_IMDB_W, sh }, markImdb, GFX_TEXT,
                 0, 0, 0, 0.0f, 1, 1, 1, alpha);
      txt_draw_alpha(ln, bx + NV_HERO_IMDB_W + 10.0f, yMeta, alpha);
    }
  }

  if (hasSec) {
    // .home-modern-hero-highlight: full white, weight 600, tracking 0.04em.
    txt_tracking(TXT_HERO_SEC, highlight, 255, 255, 255, x, ySec, alpha,
                 NV_FT_HERO_SEC * 0.04f);
  }

  if (synopsis[0])
    txt_block(TXT_HERO_SIN, synopsis, 255, 255, 255, x, ySin, NV_HERO_SIN_W,
              NV_LD_HERO_SIN, alpha, nSin);
  // IS THE BLOCK SETTLED? Anything above zero means it would arrive in pieces —
  // which is exactly the "streaming in" this whole path exists to stop — so
  // heroCopy holds it back for another frame.
  int missed = txt_misses - missBefore;
  // THE WARM PASS ANSWERS FOR NOBODY. heroCopyReady and heroLogoReady describe the
  // block ON SCREEN; this call was about a title that is not the hero yet, and
  // writing them here would tell home_update the visible copy had gone unready and
  // fade it out under a viewer who had not asked for anything.
  //
  // `missed` is still returned: it is the count for THIS block, and it is the one
  // number that says whether the warming has finished its work.
  if (warm) return missed;
  heroLogoReady = !(ci && ci->logo[0]) || tlogo != 0;
  // A logo is a CDN download that can land seconds after the backdrop. Waiting
  // for it unconditionally leaves the hero's text blank for the whole download;
  // not waiting at all means it always arrives separately. So: wait a little,
  // then come in without it and let heroLogo bring it in if it is late.
  { int logoOk = heroLogoReady || tex_failed(ci->logo)
               || SDL_GetTicks() - heroCopyIn > NV_HERO_LOGO_WAIT_MS;
    heroCopyReady = (missed == 0) && logoOk; }
  return missed;
}

// THE RECTANGLE THE HERO'S ART IS DRAWN IN, for the art the hero is showing.
//
// Three states, and the third is the only one that needs the art itself: a band
// at the top-right holds the WHOLE image, so its width is the height times the
// image's own aspect and not a constant. `art` may be NULL or still decoding —
// tex_aspect answers 0 until it lands — and the fallback is 16:9, which every
// backdrop the app receives already is; the clamp is for the odd file that is not.
//
// The height gives way if a wide image would run past the left of the screen, so
// the band is never cropped by the viewport it exists to fit inside.
static GfxRect heroRectFor(const char *art) {
  if (!settings_hero_full())
    return (GfxRect){ NV_HERO_ART_X, 0, NV_HERO_ART_W, NV_HERO_ART_H };
  if (!settings_hero_top_band())
    return (GfxRect){ 0, 0, NV_SCREEN_W, NV_HERO_FULL_H };
  { float ap = art ? tex_aspect(art) : 0.0f;
    float w = NV_SCREEN_W * settings_hero_band_scale(), h;
    if (ap <= 0.0f) ap = NV_HERO_FIT_ASP;
    if (ap < NV_HERO_FIT_ASP_MIN) ap = NV_HERO_FIT_ASP_MIN;
    if (ap > NV_HERO_FIT_ASP_MAX) ap = NV_HERO_FIT_ASP_MAX;
    h = w / ap;
    // A tall aspect at a large size would run past the bottom of the screen. The
    // WIDTH gives way there, not the height: a band that overflowed downward would
    // put art under the rows again, which is the thing this mode exists to stop.
    if (h > NV_SCREEN_H) { h = NV_SCREEN_H; w = h * ap; }
    return (GfxRect){ NV_SCREEN_W - w, 0, w, h }; }
}

static void drawHero(Uint32 now, float output) {
  (void)now;
  const int motionReduced = settings_animations_reduced();
  float aArt = 1.0f;
  // MEASURED in the web app: .home-modern-hero-media sits at x=555, y=0, 1421x670.
  // The arithmetic that used to be here (0.28*W - 56 = 481.6 of a width of 1438) came
  // from an estimated proportion and put the art 73px to the left of where it belongs.
  // A band or full screen, according to `modernHeroFullScreenBackdropEnabled`. They
  // are the two states of the SAME screen, not two layouts — and each has its own
  // gradient ramp, measured separately (see GFX_HERO and GFX_HERO_FULL in gfx.c).
  //
  // And the full-screen state has a SHAPE of its own (`heroBackdropArea`): the
  // whole screen, or the whole image at its own aspect in a band at the top-right
  // with the background showing below it. The copy does not move between the two —
  // it is the same hero, laid out full-screen, with the art occupying less of it.
  int full = settings_hero_full();
  // Full screen, the block moves up 70px (see layout.h).
  GfxMode modeHero = settings_hero_top_band() ? GFX_HERO_FIT
                    : full ? GFX_HERO_FULL : GFX_HERO;
  // Rebuilt below for each branch once the art it will draw is known: only the
  // band depends on it, but the rect has to be the one the art is drawn in.
  GfxRect r = heroRectFor(NULL);

  // THE FADE ACROSS THE THREE DRAWINGS. See the note at famFade.
  //
  // `famIn` multiplies EVERYTHING the arriving family draws — art and copy alike — so
  // each branch below needs no knowledge of the handover: it draws what it always drew,
  // and the whole of it comes up together. The picture being left goes out first, under
  // all of it, so the two overlap the way two backdrops crossfading do.
  float famIn = anim_smooth(1.0f - famFade);
  aArt *= famIn;
  drawHeroLeaving(anim_smooth(famFade));
  // Cleared every frame and filled in by whichever branch draws: a family that puts no
  // art on screen this frame leaves it empty, and the next handover then correctly has
  // nothing to hold.
  heroShot.valid = 0; heroShot.social = 0; heroShot.art[0] = '\0';

  if(focus.row>=0 && focus.row<nRows && rows[focus.row].kind==ROW_SOCIAL) {
    float x=settings_content_x(),a=(1-output)*famIn;
    // The ambience is part of this hero, not a backdrop to all of them: it comes up
    // with the rest of the family and the shot below carries it out again.
    gfx_rect((GfxRect){0,0,NV_SCREEN_W,NV_SCREEN_H},0,GFX_SOCIAL,0,0,0,0,1,1,1,famIn);
    const Row *s=&rows[focus.row];
    const CatItem *p=(s->start>=0&&focus.column<s->n)
                    ?cat_item_exact(s->start+focus.column):NULL;
    if(p) {
      const char *art=art_by_format(p, 1);
      GLuint ta=art?tex_get_hero(art):0;
      r=heroRectFor(art);
      // The activity carries on with a discreet ambience, but once Trakt has brought
      // real art it becomes the hero's subject. The person stays only on the social
      // card, where the avatar has context and does not compete with the title.
      if(ta){gfx_tex_aspect_current=tex_aspect(art);
        heroArtDraw(r,ta,modeHero,aArt);gfx_tex_aspect_current=0;
        heroShotArt(art,r,modeHero,tex_aspect(art),1);famHold=0;}
      else if (!art) { drawArtMissing(r, 0.0f, p, aArt);
        // No art is an arrival too: the ambience and the attribution are the hero here,
        // and there is nothing further to wait for.
        heroShotArt(NULL,r,modeHero,0.0f,1);famHold=0; }

      const char *name=p->socialName[0]&&strcmp(p->socialName,"Friend")?p->socialName:NULL;
      char authorship[240];
      if(name&&p->socialAction[0])snprintf(authorship,sizeof authorship,"%s  ·  %s",name,p->socialAction);
      else if(name)snprintf(authorship,sizeof authorship,"%s",name);
      else snprintf(authorship,sizeof authorship,"%s",p->socialAction);
      txt_draw_alpha(txt_line_trim(TXT_HERO_META,authorship,210,210,221,255,680),x,146,a);

      GLuint tl=p->logo[0]?tex_get_width(p->logo,520):0;
      if(tl&&tex_aspect(p->logo)>0){
        float ap=tex_aspect(p->logo),w=520,h=w/ap;
        if(h>104){h=104;w=h*ap;}
        gfx_rect((GfxRect){x,208,w,h},tl,tex_brand_dark(p->logo)?GFX_BRAND:GFX_TEXT,
                 0,0,0,0,1,1,1,a);
      } else if(p->title[0]) {
        txt_draw_alpha(txt_line_trim(TXT_TITLE1,p->title,244,243,247,255,680),x,208,a);
      }
      if(p->directing[0])
        txt_draw_alpha(txt_line_trim(TXT_CALLOUT,p->directing,230,231,238,255,680),x,326,a);
      if(p->meta[0])
        txt_draw_alpha(txt_line_trim(TXT_HERO_META,p->meta,190,194,205,255,680),x,364,a);
      if(p->synopsis[0])
        txt_block(TXT_HERO_SIN,p->synopsis,229,231,237,x,402,700,31,a,2);
    } else {
      // The empty state is the ambience and the invitation, with no art behind it: it
      // is ready the moment the focus lands, so the family's fade has nothing to wait
      // for and the ambience is what the next family will carry out.
      heroShotArt(NULL,r,modeHero,0.0f,1);famHold=0;
      const char *brand=extras_path_brand_name("trakt_wordmark");
      GLuint logo=tex_get(brand);
      float brandAspect=logo?tex_aspect(brand):2.66f;
      if(brandAspect<=0)brandAspect=2.66f;
      if(logo)gfx_rect((GfxRect){x,144,44*brandAspect,44},logo,GFX_BRAND,0,0,0,0,.96f,.94f,.95f,a);
      txt_draw_alpha(txt_line(TXT_HERO_META,"YOUR COMMUNITY",210,191,199,255),x+44*brandAspect+24,154,a);
      txt_draw_alpha(txt_line(TXT_TITLE1,"Good stories connect us.",244,243,247,255),x,226,a);
      txt_block(TXT_HERO_SIN,"Discover what your friends are watching.\nA new recommendation can start here.",187,190,202,x,330,740,36,a,2);
    }
    heroArtRect=r;
    return;
  }

  if(focus.row>=0&&focus.row<nRows&&rows[focus.row].kind==ROW_CATALOGS) {
    // WHICH folder the hero shows is not simply the focused one: it lags behind until
    // that folder's art has decoded, exactly as the poster hero does, so walking the
    // row leaves the picture standing instead of blanking it. See colHeroFade.
    int want = (focus.column >= 0 && focus.column < rows[focus.row].n)
             ? rows[focus.row].folders[focus.column] : -1;
    // The same warming the poster rows get, one row type over — and it matters
    // MORE here, because these covers are CDN urls that land seconds later (the
    // note in collections.h) and this hero has no rest period to hide it in: it
    // asks the instant the focus arrives.
    //
    // Guarded by colWarmFor because this is drawing code and runs every frame,
    // while `want != colHeroCurrent` stays true for the whole download.
    if (want >= 0 && want != colWarmFor) {
      const Row *s = &rows[focus.row];
      int k;
      colWarmFor = want;
      // THE WANTED FOLDER'S LOGO, asked for HERE and not at the draw. The hero only
      // adopts a folder once its art has decoded, and the drawing is what asks for the
      // logo — so the logo's download did not even start until the art's had finished,
      // and the title band stood empty for the length of it. Asked for at the moment
      // the focus lands, the two run together and the logo is usually resident by the
      // time the art gate opens. A wordmark is a few KB against a 1920 backdrop.
      { const ColFolder *fw = col_folder(want);
        if (fw && fw->logo[0]) tex_prefetch(fw->logo); }
      // Both sides, the right-hand one last so the lane serves it first. A
      // collection row is walked in one direction far more often than a poster
      // row is, but there is no cheap way to tell which — and a folder cover is
      // a fraction of a backdrop.
      for (k = 0; k < 2; k++) {
        int c = focus.column + (k ? 1 : -1);
        const ColFolder *fn;
        const char *a;
        if (c < 0 || c >= s->n) continue;
        fn = col_folder(s->folders[c]);
        a = colHeroArt(fn);
        if (a) tex_prefetch(a);
        if (fn && fn->logo[0]) tex_prefetch(fn->logo);
      }
    }
    if (want >= 0 && want != colHeroCurrent) {
      const char *artW = colHeroArt(col_folder(want));
      // No art is a ready state too — the folder's title block can come in without
      // first erasing the hero that is up.
      if (!artW || tex_get_hero(artW)) {
        colHeroPrevious = colHeroCurrent;
        colHeroCurrent  = want;
        // The first collection row of a session has nothing to fade FROM, and ramping
        // there would raise the art out of an empty rectangle instead of replacing
        // something. Adopt it whole.
        colHeroFade = (motionReduced || colHeroPrevious < 0 || !artW) ? 0.0f : 1.0f;
      }
    }
    const ColFolder *folder = col_folder(colHeroCurrent >= 0 ? colHeroCurrent : want);
    const ColFolder *leaving = colHeroFade > 0.0f ? col_folder(colHeroPrevious) : NULL;
    float fadeIn = anim_smooth(1.0f - colHeroFade);
    float fadeOut = anim_smooth(colHeroFade);
    if(folder) {
      if(folder->editorial) {
        /* Art is authored for this rectangle, not cropped as a movie backdrop.
           The neutral canvas continues below it; no art behind the shelves. */
        float x=settings_content_x(),a=(1-output)*famIn;
        GLuint art=tex_get_hero(folder->hero);
        GfxRect header={0,0,1920,500};
        if(leaving&&leaving->hero[0]) {
          GLuint out=tex_get_hero(leaving->hero);
          if(out)gfx_rect(header,out,GFX_TEXT,0,0,0,0,1,1,1,fadeOut*a);
        }
        if(art)gfx_rect(header,art,GFX_TEXT,0,0,0,0,1,1,1,fadeIn*a);
        // The authored banner draws with no aspect override, so the shot carries none:
        // whatever family inherits it puts it back in this same rectangle.
        if(art){heroShotArt(folder->hero,header,GFX_TEXT,0.0f,0);famHold=0;}
        else if(!folder->hero[0])famHold=0;
        heroArtRect=header;
        int director=!strcasecmp(folder->group,"Directors");
        txt_draw_alpha(txt_line(TXT_HERO_META,director?"DIRECTORS":"COLLECTIONS",
                                190,193,200,255),
                       x,183+txt_cap_inset(TXT_TITLE1)-NV_COLLECTION_HERO_GROUP_GAP
                          -txt_baseline(TXT_HERO_META),a);
        txt_block(TXT_TITLE1,folder->title,244,243,247,x,183,860,72,a,2);
        // NO LIST COUNT AND NO "OK to explore": how many lists the folder happens to
        // hold is bookkeeping, and the hint states what the row already teaches on the
        // first press. What is left is what the folder IS.
        txt_draw_alpha(txt_line_trim(TXT_HERO_META,
                                     director?"Filmography":"A selection of film and series",
                                     190,193,200,255,860),x,358,a);
        return;
      }
      int isDirector=!strcasecmp(folder->group,"Directors");
      // The collection already carries the right banner: it is a neutral background,
      // with no lettering, made to receive the content on top. The director's portrait
      // comes in as a second layer dissolved on the right-hand side — never as a card
      // and never as a known film's backdrop.
      const char *art=folder->hero[0]?folder->hero:folder->cover;
      GLuint t=0;
      r=heroRectFor(art);
      if (isDirector) director_request(folder->title);
      if (!t && art[0]) t=tex_get_hero(art);
      // The art being LEFT stays up underneath while it fades, so the swap never goes
      // through an empty frame. Only requested while the blend is running: asking every
      // frame would drag an evicted 1920 texture back through the decoder purely so as
      // not to draw it, which is the trap the poster path documents.
      { const char *outArt = colHeroArt(leaving);
        GLuint outT = outArt ? tex_get_hero(outArt) : 0;
        if(outT){gfx_tex_aspect_current=tex_aspect(outArt);
          heroArtDraw(r,outT,modeHero,fadeOut*aArt);
          gfx_tex_aspect_current=0;} }
      if(t){gfx_tex_aspect_current=tex_aspect(art);heroArtDraw(r,t,modeHero,fadeIn*aArt);gfx_tex_aspect_current=0;}
      // THE COVER IS WHAT THE FAMILY'S FADE WAITS FOR. These are CDN urls that land
      // seconds after the focus arrives, and until one does the picture of the row just
      // left is the only thing there is to show. A folder that names no art at all is
      // ready by definition — the group, the title and the wordmark are its hero.
      if(t){heroShotArt(art,r,modeHero,tex_aspect(art),0);famHold=0;}
      else if(!art[0])famHold=0;
      heroArtRect=r;
      float x=settings_content_x(),a=(1-output)*famIn;
      // The label is DRAWN by each branch below, once that branch knows where the top
      // of its title is: see NV_COLLECTION_HERO_GROUP_GAP.
      TxtLine group=txt_line(TXT_HERO_META,folder->group,201,206,218,255);
      if (isDirector) {
        const char *photo=director_photo(folder->title);
        // THE PORTRAIT IS ART, so it follows the BACKDROP'S extent and not the copy's
        // layout: with the band on, a portrait falling to 1120 would stand over the
        // rows while the backdrop beside it stops at 615.
        int tallArt=full&&!settings_hero_top_band();
        GLuint portrait=photo[0]
          ?tex_get_width(photo,tallArt?1280.0f:1100.0f):0;
        if (portrait) {
          // The shader preserves the vertical proportion and dissolves all four edges.
          // The width is deliberately generous so the head has the same visual
          // presence as the approved example, without looking like a squashed photo.
          GfxRect pr=tallArt ? (GfxRect){840.0f,-20.0f,1080.0f,1120.0f}
                           : (GfxRect){980.0f,-15.0f,940.0f,700.0f};
          gfx_tex_aspect_current=tex_aspect(photo);
          gfx_rect(pr,portrait,GFX_PORTRAIT,0,0,0,0,0,0,0,aArt);
          gfx_tex_aspect_current=0.0f;
        }
        TxtLine name=txt_line_trim(TXT_TITLE1,folder->title,244,243,247,255,780);
        // The CAPITAL sits on the shared line, so the name lines up with a wordmark
        // one row over; the two lines under it keep their distance from the name's
        // own box, not from the line.
        float topName=NV_COLLECTION_HERO_LOGO_Y-txt_cap_inset(TXT_TITLE1);
        txt_draw_alpha(group,x,NV_COLLECTION_HERO_LOGO_Y
                              -NV_COLLECTION_HERO_GROUP_GAP
                              -txt_baseline(TXT_HERO_META),a);
        txt_draw_alpha(name,x,topName,a);
        const char *meta=director_meta(folder->title);
        if (meta[0])
          txt_draw_alpha(txt_line_trim(TXT_HERO_META,meta,201,206,218,255,780),
                             x,topName+92.0f,a);
        const char *con=director_known(folder->title);
        if (con[0]) {
          char line[300];
          snprintf(line,sizeof line,"Known for  %s",con);
          txt_draw_alpha(txt_line_trim(TXT_HERO_META,line,220,224,233,255,780),
                             x,topName+136.0f,a);
        }
        return;
      }
      // TEX_GET_EXACT, NOT TEX_GET_WIDTH — this is UI furniture at a size the layout
      // already knows, which is the case that function exists for.
      //
      // tex_get_width asks for NV_TEX_SLACK (1.25) more than the drawing width and
      // rounds up to a multiple of 32, so the texture always lands between 1.25x and
      // 1.6x the size it is drawn at. GL_LINEAR_MIPMAP_NEAREST SNAPS at 1.414: just
      // under it the GPU samples level 0 and undersamples (jagged), just over it takes
      // level 1 — HALF the resolution — and magnifies it back (chunky). Both sides of
      // that snap were on screen at once, which is why two services looked worse than
      // the rest while HBO and Paramount looked fine:
      //
      //   Apple TV   797 wide, decoded 608, drawn 398  ->  1.528   level 1, magnified
      //   Prime      880 wide, decoded 608, drawn 440  ->  1.382   level 0, aliased
      //   HBO max    553 wide, decoded 553, drawn 276  ->  2.004   level 1, 1:1
      //
      // The web app has no such step: the browser reduces the source once, straight to
      // the drawn size. tex_get_exact does the same thing here — the decode thread's
      // box filter (premultiplied, tex_cache.c) reduces the source to the drawn width
      // in one pass, the upload carries no mipmap chain, and what reaches the panel is
      // a 1:1 blit with nothing left for a filter to get wrong.
      //
      // So the ASPECT comes first and the request second, which is the order
      // tex_get_exact documents: before anything has decoded tex_aspect answers 0 and
      // the box is asked for at its full width; the frame the aspect lands, the width
      // becomes the real one and the entry re-decodes ONCE. tex_aspect is the source
      // file's aspect, not the decoded texture's, so it does not move under us.
      int hasLogo=!isDirector&&folder->logo[0];
      float ap=hasLogo?tex_aspect(folder->logo):0.0f;
      // THE HEIGHT COMES FROM THE ASPECT, the width from the height, and the width cap
      // has the last word — NV_COLLECTION_HERO_LOGO_H_REF says why the fall is a fourth
      // root and not the flat ceiling this had. Still `contain`: nothing is cropped or
      // stretched, it is the box the mark is contained IN that changes shape with it.
      float w=NV_COLLECTION_HERO_LOGO_MAX_W,h=0.0f;
      if(ap>0){
        h=NV_COLLECTION_HERO_LOGO_H_REF/sqrtf(sqrtf(ap));
        if(h>NV_COLLECTION_HERO_LOGO_MAX_H)h=NV_COLLECTION_HERO_LOGO_MAX_H;
        w=h*ap;
        if(w>NV_COLLECTION_HERO_LOGO_MAX_W){w=NV_COLLECTION_HERO_LOGO_MAX_W;h=w/ap;}
      }
      GLuint logo=hasLogo?tex_get_exact(folder->logo,w):0;
      float endTitle=NV_COLLECTION_HERO_LOGO_BASE;
      // The name stands in only for a logo that will never arrive — the branch below
      // says why.
      int showName=!hasLogo||tex_failed(folder->logo);
      TxtLine name=showName?txt_line_trim(TXT_TITLE1,folder->title,241,243,247,255,700)
                           :(TxtLine){0,0,0};
      // ONE LINE, AND IT NEVER MOVES: the label sits a fixed gap above LOGO_Y and the
      // title's ink starts ON it, whether that ink is a wordmark's art or a capital.
      // Nothing here depends on the mark's height, so the pair does not shift between
      // the folders of a row, on the frame a mark decodes, or between a row of
      // wordmarks and a row of names — which is what put Streaming 100px above
      // Discover and Genres.
      //
      // The gap is measured on the INK at both ends (label BASELINE to title CAP), so
      // 24 is 24 wherever it is read. A TxtLine's own box would hide some 22px of air
      // above a 76px capital and 5 below a 21px baseline, and that is what made an
      // earlier "14" arrive on screen as 38.
      txt_draw_alpha(group,x,NV_COLLECTION_HERO_LOGO_Y-NV_COLLECTION_HERO_GROUP_GAP
                             -txt_baseline(TXT_HERO_META),a);
      if(logo&&ap>0){
        gfx_rect((GfxRect){x,NV_COLLECTION_HERO_LOGO_Y,w,h},logo,
                 tex_brand_dark(folder->logo)?GFX_BRAND:GFX_TEXT,
                 0,0,0,0,.96f,.97f,.98f,a);
      } else if(showName){
        // THE NAME ONLY WHEN THERE IS NO LOGO TO COME. A logo is a CDN download that
        // lands a few frames after the folder is adopted, and drawing the name in the
        // meantime meant every streaming service came up as type and then flicked over
        // to its wordmark. The web has never done that: `.home-hero-title-text` carries
        // `is-hidden` whenever there is a titleLogoUrl and is only unhidden by the
        // img's own onerror (homeScreen.js's getLogoErrorHandler). tex_failed is that
        // onerror — it answers 0 while a retry is still scheduled, so the name appears
        // for a DEAD url and not for a slow one.
        txt_draw_alpha(name,x,NV_COLLECTION_HERO_LOGO_Y-txt_cap_inset(TXT_TITLE1),a);
      }
      // Nothing is drawn while the logo is still on its way: the band keeps its height
      // either way, so whatever sits below it does not move when the logo lands.
      if(isDirector) {
        // The TMDB record below the name: who they are, when and where they were born,
        // three lines of biography and the titles they are known for. It arrives in the
        // background; until it does, the caption stays where it always was.
        director_request(folder->title);
        if(director_ready(folder->title)) {
          // The block starts just below the name and ENDS before the row's header
          // (NV_SHELF_TOP): the number of biography lines is what gives way.
          // Width 780: it stops short of the cover card (which starts at 1096).
          float yy=endTitle+30,cap=NV_SHELF_TOP-30,width=780;
          const char *meta=director_meta(folder->title),*bio=director_bio(folder->title),*con=director_known(folder->title);
        float fixed=(meta[0]?38:0)+(con[0]?38:0);   // meta + known for
          int lines=(int)((cap-yy-fixed-12)/31);if(lines>3)lines=3;
          if(meta[0]){txt_draw_alpha(txt_line_trim(TXT_HERO_META,meta,201,206,218,255,width),x,yy,a);yy+=38;}
          if(bio[0]&&lines>0){yy+=txt_block(TXT_HERO_SIN,bio,222,225,232,x,yy,width,31,a,lines)+12;}
          if(con[0]){char l[300];snprintf(l,sizeof l,"Known for  %s",con);
            txt_draw_alpha(txt_line_trim(TXT_HERO_META,l,236,232,244,255,width),x,yy,a);yy+=38;}
        }
      }
      // The "N lists · OK to explore" caption that used to close this block is gone:
      // the count is bookkeeping and the hint repeats what OK on the row already does.
      return;
    }
  }

  // IT ONLY SWAPS WITH THE NEW ART ALREADY DECODED.
  //
  // The request is made here, in the drawing, because this is where we know which file
  // the art is (the path comes from the catalogue, with the folder as a fallback).
  // While the cache does not return a texture, heroCurrent does not change and the
  // screen carries on with the art it already had — which is exactly what the owner
  // asked for when moving quickly.
  if (heroWanted >= 0 && heroWanted != heroCurrent) {
    const char *artD = art_by_identity(heroWanted, 1);
    // An absence of art is a ready state too: the placeholder belongs to the item and
    // can come in without first erasing the previous hero.
    int artReady = !artD || tex_get_hero(artD);
    if (artReady) {
      // ARRIVING FROM ANOTHER KIND OF ROW, THE CROSSFADE THAT MATTERS IS THE FAMILY'S.
      // The picture being replaced is not the previous poster's — it is the collection's
      // or the social row's, and it is already going out on famFade. Running heroExits
      // as well would dissolve a backdrop nobody can see (it is behind the family's own
      // fade, at alpha 0) against one that is itself still coming up, which reads as the
      // new art arriving at half strength.
      int cross = !famHold;
      heroPrevious = heroCurrent;
      heroCurrent = heroWanted;
      heroWanted = -1;
      heroExits = (motionReduced || !artD || !cross) ? 0.0f : 1.0f;
      heroEnters = (motionReduced || !artD || !cross) ? 1.0f : 0.0f;
      heroSwapIn = SDL_GetTicks() + NV_HERO_INTERVAL_MS;
      // The copy changes HERE, with the art: the old one stops being drawn on this
      // very frame and the new one starts at zero and waits for its lines. It is
      // zeroed even under reduced motion — there the ramp snaps, so the only thing
      // the zero buys is the two frames the text needs, which nobody sees.
      //
      // heroLogo goes back to zero too: a logo that has to be fetched for the new
      // title must not inherit the previous one's ramp and arrive already faded
      // in. heroCopyIn is what NV_HERO_LOGO_WAIT_MS counts against.
      heroCopy = 0.0f;
      heroLogo = 0.0f;
      heroCopyIn = SDL_GetTicks();
    }
  }

  const CatItem *ci = cat_item_exact(heroCurrent);
  const char *artA = art_by_identity(heroCurrent, 1);
  const CatItem *cAnt = cat_item_exact(heroPrevious);
  const char *artB = art_by_identity(heroPrevious, 1);
  // THE INCOMING ART DECIDES THE BAND, including for the one on its way out. The
  // two are crossfading in the same rectangle — that is what makes them read as one
  // image dissolving — and a band that changed shape halfway through the fade would
  // turn the dissolve into a resize. The outgoing image is cover-cropped to the new
  // shape for those frames, which is invisible between two 16:9 backdrops.
  r = heroRectFor(artA);

  // A ceiling of 1920: the hero fills the screen and at 960 it came out stretched to
  // double.
  // The PREVIOUS one is only requested WHILE the blend is happening. It was being
  // requested on EVERY frame, even with the swap already finished, when it is not
  // drawn: if the cache had already evicted it, the request brought it back — a 1920
  // texture (~8 MB) re-decoded so as NOT to be drawn, pushing the visible posters out
  // of the budget.
  GLuint tAnt = (heroExits > 0.0f && artB) ? tex_get_hero(artB) : 0;
  // Ask for the new one NOW, during the fade: it is this request that queues the
  // decode, and it is why the emptiness lasts the loading time and no longer.
  GLuint tCurrent = artA ? tex_get_hero(artA) : 0;
  if (tAnt) {
    // A fade with acceleration and deceleration: the measurement leaves ~25% of the
    // travel almost still at the start, so a straight ramp reads as a cut on the way out.
    (void)drawArtHero(r, modeHero, cAnt, artB,
                          anim_smooth(heroExits) * aArt);
  } else if (heroExits > 0.0f && (!artB || tex_failed(artB))) {
    drawPlaceholderHero(r, cAnt, anim_smooth(heroExits) * aArt);
  }
  if (tCurrent && heroEnters > 0.0f) {
    (void)drawArtHero(r, modeHero, ci, artA,
                          anim_smooth(heroEnters) * aArt);
  } else if (!artA || tex_failed(artA)) {
    drawPlaceholderHero(r, ci,
                           aArt * (heroEnters > 0.0f ? 1.0f : heroEnters));
  }
  // THE FAMILY'S FADE WAITS FOR THE ART OF THE TITLE THAT IS ACTUALLY FOCUSED, not for
  // whatever this hero happened to be showing when it was last on screen. heroPending is
  // that title (home_update back-dates its clock on the change of row kind, so there is
  // no rest period to sit through), and while it has not been adopted the swap above is
  // still waiting on a decode. Releasing the hold here would cross to the old backdrop
  // and then cross again to the right one.
  //
  // A dead url would hold for ever; NV_HERO_FAMILY_WAIT_MS in home_update is the bound.
  if (famHold && heroWanted < 0 && heroPending == heroCurrent &&
      (tCurrent || !artA || tex_failed(artA))) famHold = 0;
  gfx_tex_aspect_current = 0.0f;
  heroArtRect = r;

  // NOT scaled by famIn here: the early return below would then skip drawHeroCopy for
  // the whole of a hold, and that call is what ASKS text.c for the block's lines (see
  // the note on it). The family's alpha goes on at the call instead, so the block still
  // rasterises during the wait and is ready on the frame the fade starts.
  float aText = 1.0f - output;
  float slideDownCopy = output * NV_SCREEN_H * 0.06f;
  if (aText <= 0.004f) return;

  // THE COPY FADES IN ONLY. The outgoing one is NOT drawn: it goes on the frame
  // of the swap.
  //
  // It was on a second layer at first, fading on heroExits like the art it sits
  // on — but the art and the copy are not the same kind of thing. Two backdrops
  // crossfading occupy the same rectangle and read as one image dissolving; two
  // COPIES do not. The block is anchored to its base and stacks upward, so the
  // outgoing and incoming texts sit at different heights (different line counts,
  // different logo) and the overlap reads as doubled text, not as a dissolve.
  //
  // Drawn even at alpha 0: this is the call that ASKS text.c for the lines, and
  // without it the block could never become ready.
  (void)drawHeroCopy(ci, anim_smooth(heroCopy) * aText * famIn, slideDownCopy, full, 0);

  // THE NEXT TITLE'S COPY IS LAID OUT NOW, while this one is still up.
  //
  // The art had the same defect and warmHero fixed it: nothing was ASKED FOR
  // until the swap, so the whole cost fell after it. For the copy the cost is the
  // rasteriser — the block is 3 to 7 lines and TXT_PER_FRAME is 2, so it needed
  // two to four frames AFTER the commit before `missed` reached 0 and the fade
  // was even allowed to start. Running the layout here spends those frames during
  // the wait instead, so the block is already in text.c's cache when the art
  // lands and the fade begins on the first frame.
  //
  // It also calls tex_get_width on the next logo, which starts the DECODE — the
  // half tex_prefetch cannot do, since it only puts the file on disk.
  //
  // AFTER the visible copy, never before: they share TXT_PER_FRAME, and the block
  // coming in has to have first claim on it. Whatever is left over goes to the
  // next one, which by definition can wait.
  //
  // The condition is the candidate, not heroWanted: that way the warming has the
  // whole of NV_HERO_IDLE_MS as well, and not only the art's loading time.
  // IT STOPS WHEN IT IS DONE. `missed == 0` means every line of the next block
  // came out of text.c's cache, so there is nothing left to warm and the layout
  // arithmetic would just repeat itself on every frame until the swap. It matters
  // more than it looks: the swap can WAIT INDEFINITELY — the commit needs the art
  // decoded, and art that never decodes never commits — so without this the pass
  // would spin for as long as the hero is stuck.
  if (heroPending != heroCurrent && heroWarmedFor != heroPending) {
    const CatItem *cNext = cat_item_exact(heroPending);
    if (!cNext) heroWarmedFor = heroPending;
    else if (drawHeroCopy(cNext, 0.0f, slideDownCopy, full, 1) == 0)
      heroWarmedFor = heroPending;
  }
}

// A GREY background, and nothing else. I had put the highlighted title's art here,
// blurred, thinking that was the "Apple TV grey" — but the effect was the opposite of
// what was asked for: the hero's art rose and left normally, and its blurred copy
// stayed in the background, giving the impression the image had never risen. A
// neutral background competes with nothing.
static void drawBackground(void) {
  GfxRect screen = { 0, 0, NV_SCREEN_W, NV_SCREEN_H };
  // The screen has already been cleared with THIS VERY COLOUR by
  // glClearColor/glClear in main.c before app_draw. Painting over it was one
  // full-screen layer thrown away per frame — and the dominant cost on this GPU is
  // fill rate (gfx.c records that TWO full-screen layers dropped the Mali-G71 to
  // ~40fps). Do not put it back without first changing the clear colour.
  (void)screen;
}

// COLLECTION ROWS — Discover, Streaming, and whatever else the account pinned. They
// are ROW_CATALOGS and are drawn here rather than in the card loop, which is why they
// used to miss the focus scale and the sibling dimming the poster rows have. In the
// web they are not a special case at all: a collection card is
// `home-content-card home-poster-card home-collection-card` (homeScreen.js:2547), so
// it takes the poster card's 1.02 and the same brightness(0.8) as everything else.
//
// Still a divergence, and a deliberate one for now: the frame. A collection card in
// the web carries a `.home-poster-frame`, so it should have the NV_CARD_PAD gutter and
// the border inside it, where this still paints the 4px ring OUTSIDE the box.
static void drawShortcuts(int r, float y) {
  float lw = widthOf(ROW_CATALOGS), lh = heightOf(ROW_CATALOGS);
  static int last=-1;static Uint32 since;
  for (int c = 0; c < rows[r].n; c++) {
    float f = animFocus[r][c];
    // Same growth as a poster, and from the same corner: centred across, pinned along
    // the top, so the card only ever gets taller downward.
    float scale = 1.0f + scaleOf(ROW_CATALOGS) * f;
    float w = lw * scale, h = lh * scale;
    float x = settings_content_x() + c * stepOf(ROW_CATALOGS) - scrollX[r]
            - (w - lw) * 0.5f;
    // A CARD OF MARGIN EACH SIDE, which the poster rows already had and this did not.
    // The cull is not only about pixels: tex_get_width is what QUEUES the decode, so a
    // card the loop skips has not even been asked for. Testing the exact bounds meant a
    // collection cover was requested at the moment a pixel of it appeared — and these
    // are CDN URLs that land seconds later (collections.h), so the card scrolled in
    // grey and filled itself in afterwards. The poster rows never showed that because
    // their test (further down, on the card's CENTRE) already reaches a width past the
    // edge.
    if (x + w < -lw || x > NV_SCREEN_W + lw) continue;
    float radius = radiusOf(w, h);
    GfxRect card = {x, y, w, h};
    // The whole texture unless the focus animation below picks a cell out of a
    // sprite sheet. Re-set per card, never carried over: leaving a cell set would
    // crop the NEXT tile to one frame of the previous one's animation.
    GfxRect cell = {0.0f, 0.0f, 1.0f, 1.0f};
    if (f > .01f) {
      float smaller = w < h ? w : h;
      gfx_color((GfxRect){x - NV_RING_FOCUS, y - NV_RING_FOCUS,
        w + 2*NV_RING_FOCUS, h + 2*NV_RING_FOCUS},
        (radius * smaller + NV_RING_FOCUS) / (smaller + 2*NV_RING_FOCUS), .96f, .97f, .98f, f);
    }
    // The SAME surface the poster rows sit on, and for the same reason: in the web
    // a collection card is `home-content-card home-poster-card home-collection-card`
    // (homeScreen.js:2547), so `.home-poster-card .content-poster` applies to it and
    // it gets `linear-gradient(180deg, #1c1c1c, #111)` like every other card. This
    // drew the flat #2C2C2C of NV_COLOR_SKELETON instead — lighter than its
    // neighbours and with no ramp, which is what made the Discover row read as a
    // strip of grey slabs next to the posters.
    // AT REST, NOT THE SKELETON. This drew the shine as well, and the shine was
    // invisible only by accident: the cover was blitted over it opaquely, so nothing
    // behind it reached the screen. Now that GFX_CARD keeps the cover's alpha, the
    // surface under a transparent region is genuinely on screen — and the sweep came
    // with it, a light crossing the Discover and Genres cards every 1800ms for ever
    // on a shelf that had finished loading. The shine is put back below, for the one
    // case that means it: a cover still in flight.
    drawArtSurface(card, radius, 1.0f);
    const ColFolder *folder=col_folder(rows[r].folders[c]);
    if (folder) {
      const char *art = folder->cover;
      // THE WIDTH THE COVER IS REALLY DRAWN AT, which is not the card's width.
      //
      // GFX_CARD is a COVER fit: art that is WIDER in proportion than the 360x203
      // card is fitted by HEIGHT and cropped across, so it reaches the screen at
      // lh * aspect and not at lw. These covers are composed CARDS — the
      // catalogue's name, its item count and a ranked poster baked into one bitmap
      // by whoever publishes the collection — and they are wide: the packaged
      // editorial art is 3840x1000, or 3.84:1 against the card's 1.77:1. Asking by
      // `lw` decoded that at 480 and the GPU then ENLARGED it to fill a box over
      // 700 wide. A magnified copy of an image that had already been thrown away
      // once is why the lettering inside these cards was the softest thing on the
      // shelf, while the row titles beside them — real text — stayed crisp.
      //
      // STILL NOT `w`: the original note here is right that the focus scale must
      // not move the ceiling, or the cover re-decodes the moment the card is landed
      // on and blinks back to its skeleton. `1 + scaleOf` is the scale at FULL
      // focus, a constant, so the ceiling covers the growth without following it.
      //
      // AND tex_get_width, NOT tex_get_exact. The width comes from tex_aspect,
      // which answers 0 until the first decode lands, so this request moves once —
      // and tex_get_width only ever promotes UPWARD, settling after that one
      // re-decode (the same pattern detail.c uses for its logo). An exact entry
      // moves in both directions, and would also fight tex_get_hero: colHeroArt
      // falls back to this very cover at 1920 when the collection has no hero.
      float coverW = lw;
      { float asp = art && art[0] ? tex_aspect(art) : 0.0f;
        if (asp > 0.0f && asp * lh > coverW) coverW = asp * lh; }
      coverW *= 1.0f + scaleOf(ROW_CATALOGS);
      // AND EXACT, whenever the collection has a hero of its own.
      //
      // MEASURED on this row and this is the whole of it: a 360-wide card asked by
      // tex_get_width decodes at 480 (1.25 slack, rounded to a multiple of 32) and
      // draws at 360 — a ratio of 1.333, which sits JUST UNDER
      // GL_LINEAR_MIPMAP_NEAREST's 1.414 snap. The GPU therefore minifies off level
      // 0 with a four-texel tap and throws a third of the source away. These covers
      // are composed CARDS — the catalogue's name and its item count are lettering
      // baked into the bitmap — and aliasing lands hardest on lettering, which is
      // why the text inside the card read as soft while the row title beside it,
      // drawn as real text, stayed sharp. tex_get_exact box-filters to the drawn
      // width on the decode thread and uploads no mipmap chain: one clean
      // reduction, then a 1:1 blit.
      //
      // THE GUARD IS colHeroArt. It falls back to this very cover when a collection
      // has no hero, and the hero asks at 1920 — an exact entry moves in both
      // directions while tex_get_hero only promotes upward, so a cover wanted by
      // both would re-decode every frame, for ever. With a hero present the cover
      // is provably never asked for at 1920, which is the same condition the poster
      // cards below use.
      GLuint tex = art && art[0]
                 ? (folder->hero[0] ? tex_get_exact(art, coverW)
                                    : tex_get_width(art, coverW))
                 : 0;
      // The aspect the art is CROPPED to. It follows `tex` — when a frame of the
      // focus animation replaces the cover below, the aspect has to become the
      // frame's or the shader would crop the animation to the cover's shape.
      float texAspect = art && art[0] ? tex_aspect(art) : 0.0f;
      int animating = focus.row==r && focus.column==c &&
                      !settings_animations_reduced();
      if(animating && (folder->frames>0 || folder->focusSheet[0])) {
        int id=rows[r].folders[c];Uint32 now=SDL_GetTicks();
        if(last!=id){last=id;since=now;}
        if(folder->focusSheet[0]) {
          // ONE SPRITE SHEET, and the request is made from the moment the tile takes
          // the focus rather than after the delay below. It is a CDN file, not a
          // local one: asking only once the animation was due to start meant the
          // first pass through the loop had nothing to draw and the tile sat still
          // for as long as the download took. Asked for here, it is usually
          // resident by the time the 350 ms are up.
          //
          // 1920 and not `w`: the cap tex_get_width applies is the DECODE width, and
          // it is the WHOLE sheet being decoded, not one frame. Asking for the
          // tile's 360 would give cells of 45 pixels to draw at 360. The cell
          // arithmetic itself is unaffected — the coordinates below are normalised,
          // so they survive any uniform scale — this is only about resolution.
          // NV_TEX_HERO_WIDTH_MAX is 1920, the sheet's own width, so nothing is lost.
          GLuint sheet = tex_get_width(folder->focusSheet, 1920.0f);
          if(sheet && now-since>NV_FOCUS_DELAY_MS) {
            int n = NV_FOCUS_SHEET_COLS * NV_FOCUS_SHEET_ROWS;
            int index = (int)((now-since-NV_FOCUS_DELAY_MS)/NV_FOCUS_FRAME_MS) % n;
            cell.x = (float)(index % NV_FOCUS_SHEET_COLS) / NV_FOCUS_SHEET_COLS;
            cell.y = (float)(index / NV_FOCUS_SHEET_COLS) / NV_FOCUS_SHEET_ROWS;
            cell.w = 1.0f / NV_FOCUS_SHEET_COLS;
            cell.h = 1.0f / NV_FOCUS_SHEET_ROWS;
            tex = sheet;
            // The CELL's aspect, not the sheet's. They happen to be equal here (a
            // grid of 16:9 cells is itself 16:9), but writing the sheet's would be
            // right by accident and would break the day the grid stops being square.
            texAspect = tex_aspect(folder->focusSheet)
                      * (float)NV_FOCUS_SHEET_ROWS / (float)NV_FOCUS_SHEET_COLS;
          }
        } else if(now-since>NV_FOCUS_DELAY_MS) {
          char frame[700];
          int index=(int)((now-since-NV_FOCUS_DELAY_MS)/NV_FOCUS_FRAME_MS)%folder->frames+1;
          snprintf(frame,sizeof frame,"%s/%03d.jpg",folder->frameDir,index);
          GLuint motion=tex_get_width(frame,480);
          if(motion){tex=motion;texAspect=tex_aspect(frame);}
          snprintf(frame,sizeof frame,"%s/%03d.jpg",folder->frameDir,index%folder->frames+1);
          tex_get_width(frame,480);
          snprintf(frame,sizeof frame,"%s/%03d.jpg",folder->frameDir,(index+1)%folder->frames+1);
          tex_get_width(frame,480);
        }
      }
      if (tex) {
        // FILL, NOT COVER — and the aspect of 0 is how this shader is told so.
        //
        // `.home-collection-card .content-poster` and its focus overlay both carry
        // `object-fit: fill` (components.css:7586), i.e. the cover is STRETCHED to
        // the card's box. GFX_CARD defaults to a cover-fit CROP, which on art whose
        // aspect does not match the card magnified it further and cut content off
        // the edge — visible as the ranked poster sitting harder against the card's
        // right edge here than it does in the browser.
        //
        // Passing 0 is the documented way to ask for the stretch (tex_cache.h: an
        // unknown aspect and "the shader would stretch the art"). It applies to the
        // focus sheet too, deliberately: the web gives `.home-poster-focus-gif` the
        // same `object-fit: fill` in that very rule.
        (void)texAspect;
        gfx_tex_aspect_current = 0;
        gfx_tex_cell_current = cell;
        gfx_rect(card, tex, GFX_CARD, 0, 0, 0, radius, 0, 0, 0, 1);
        gfx_tex_cell_current = (GfxRect){0.0f, 0.0f, 1.0f, 1.0f};
        gfx_tex_aspect_current = 0;
      } else if (folder->title[0]) {
        // AND HERE THE SWEEP IS RIGHT: this branch is reached only while `tex` is 0,
        // which for a folder that HAS a cover means the file is still on its way.
        // Gated on `art` so a collection with no cover at all — nothing to wait for —
        // keeps the still surface and just shows its name.
        if (art && art[0]) drawArtSkeleton(card, radius, 1.0f);
        // NO ART YET, so the name stands in for it. The packaged collections' covers
        // are local files and appear on the first frame; the ACCOUNT's are CDN URLs
        // and arrive seconds later. Until now the card was a mute grey rectangle —
        // indistinguishable from a broken one, and saying nothing about what it was.
        //
        // The poster rows already solve this with drawArtMissing; here the name is
        // enough, because an "Art unavailable" caption would be a lie: the art is on
        // its way, not missing.
        TxtLine name = txt_line_trim(TXT_CAPTION, folder->title,
                                     184, 188, 198, 255, card.w - 32.0f);
        txt_draw_alpha(name, card.x + (card.w - name.w) * 0.5f,
                       card.y + card.h * 0.5f - name.h * 0.5f, 0.9f);
      }
      // The cover itself is the catalogue's identity. The name/logo was being drawn
      // again over it and created exactly the duplication the user pointed out on
      // Netflix, Prime Video, Disney+ and the IMDb lists. The row's title stays in the
      // header; inside the card there is only the art, with no veil, badge or auxiliary
      // logo.
    }
    // Dimmed like every other unfocused card. Outside the `if (folder)` so that a
    // collection whose cover has not arrived dims with its neighbours instead of
    // sitting there at full brightness — that card is still focusable, and in the web
    // it is `.focusable:not(.focused)` like any other.
    if (f < 0.999f) {
      gfx_color(card, radius, 0, 0, 0, (1.0f - NV_DIM_UNFOCUSED) * (1.0f - f));
    }
  }
}

void home_draw(Uint32 now) {
  drawBackground();
  float pd = detail_progress();
  if (settings_hero_on()) drawHero(now, pd);

  // THE DETAIL OPENING: the rows GO DOWN and fade; the background art stays.
  //
  // It is the movement the owner described — "only the posters go down and the
  // background stays". The detail no longer flies out of the card: its art comes in
  // full screen gaining opacity, so what the eye follows is the rows leaving. Going
  // down 8% of the screen's height is enough to read as an exit without the last row
  // disappearing too early.
  //
  // `pd` comes from the SAME spring the detail uses to draw (detail_progress), and not
  // from a clock of its own: two clocks would drift and the home would leave early or
  // late relative to the art coming in.
  float slideDown = pd * NV_SCREEN_H * 0.08f;
  if (pd >= 0.996f) return;   // the detail has settled: nothing of the home shows

  // THE ROWS' VIEWPORT. `.home-modern-rows-viewport` (components.css:6929) is an
  // absolute block with bottom:0, height 52% and overflow-y:auto — that is, the rows
  // scroll INSIDE the bottom 52% and whatever rises beyond that is CLIPPED. The port
  // drew the rows loose over the whole screen, and that is why the row leaving at the
  // top appeared across the hero's block instead of disappearing. The hero does not
  // scroll: only its contents change with the focus.
  gfx_crop(0, NV_SHELF_TOP-96, NV_SCREEN_W, NV_SCREEN_H - NV_SHELF_TOP+96);
  // NV_SHELF_PAD_TOP is the web's `padding-top` on the scroll column, not a nudge:
  // the rows come to rest 46px below the viewport's top edge, clear of the mask
  // that fades it. See the note on the constant.
  float y = NV_SHELF_TOP + NV_SHELF_PAD_TOP - scrollY + slideDown;
  for (int r = 0; r < nRows; r++) {
    KindRow kind = rows[r].kind;
    float fade=anim_clamp((y-(NV_SHELF_TOP-80))/80,0,1);
    gfx_opacity_group=fade*fade*(3-2*fade);
    float lw = rows[r].stackN ? 680.0f : widthOf(kind);
    float lh = heightOf(kind), step = lw + gapOf(kind);
    float artH = lh;
    // `y` is the top of the row's header; the cards start after the title.
    // Separating the two stops the next row's title being drawn over the previous
    // one's art when the row has tall cards.
    float cardY = y + NV_LEGACY_ROW_HEAD_H;

    int landscape = editorial(kind) || ((kind != ROW_CONTINUE) && settings_posters_landscape());
    if (y < NV_SCREEN_H + 200 && y + NV_LEGACY_ROW_HEAD_H + lh > -200) {
      // `catalogTypeSuffixEnabled`. formatCatalogRowTitle (homeUtils.js:62) does
      // `if (!showTypeSuffix) return base;` — it returns the capitalised name and
      // that is that. Here the suffix is removed while DRAWING and not in discovery,
      // otherwise the preference would only take effect once the network brought the
      // catalogues again — that is, only on the next start.
      const char *rotFilter = rows[r].title;
      char withoutSuffix[96];
      if (!settings_suffix_kind() && rotFilter) {
        const char *cut = strstr(rotFilter, " - ");
        const char *last = NULL;
        while (cut) { last = cut; cut = strstr(cut + 3, " - "); }
        if (last && (!strcmp(last + 3, "Film")
                       || !strcmp(last + 3, "Series"))) {
          size_t n = (size_t)(last - rotFilter);
          if (n >= sizeof withoutSuffix) n = sizeof withoutSuffix - 1;
          memcpy(withoutSuffix, rotFilter, n);
          withoutSuffix[n] = 0;
          rotFilter = withoutSuffix;
        }
      }
      TxtLine tl = txt_line_trim(TXT_ROW_TITLE, rotFilter, 245, 246, 249, 255,
                                    NV_SCREEN_W - settings_content_x() - 180);
      txt_draw(tl, settings_content_x(), y);
      if(kind==ROW_SOCIAL) {
        const char *brand=extras_path_brand_name("trakt_wordmark");
        GLuint logo=tex_get(brand);float ap=logo?tex_aspect(brand):2.66f;
        if(ap<=0)ap=2.66f;
        if(logo)gfx_rect((GfxRect){settings_content_x()+tl.w+18,y+(tl.h-30)*.5f,30*ap,30},
                         logo,GFX_BRAND,0,0,0,0,.95f,.93f,.94f,1);
      }
      if(!strncmp(rows[r].catId,"ai_",3)) {
        TxtLine ai=txt_line(TXT_HERO_META,"AI-powered",183,192,219,255);
        txt_draw(ai,settings_content_x()+tl.w+22,y+(tl.h-ai.h)*.5f);
      }
      if (focus.row == r) {
        char pos[32];
        if (focus.column < rows[r].n)
          snprintf(pos, sizeof pos, "%d / %d", focus.column + 1, rows[r].n);
        else snprintf(pos, sizeof pos, "See all");
        TxtLine lp = txt_line(TXT_HERO_META, pos, 186, 191, 202, 255);
        txt_draw(lp, NV_SCREEN_W - NV_HOME_SAFE_RIGHT - lp.w, y + (tl.h - lp.h)*.5f);
      }
      if (kind == ROW_CATALOGS) {
        drawShortcuts(r, cardY);
        y += NV_LEGACY_ROW_HEAD_H + heightTotalOf(kind) + rowGap();
        continue;
      }

      // THE "SEE ALL" CARD at the end of the row. Drawn before the posters' loop so
      // as not to inherit its variables; it is not a title and uses no art.
      //
      // MEASURED in the web app (.home-seeall-card-inner): a 2 px frame in
      // rgba(255,255,255,0.12) over rgba(255,255,255,0.06), with the arrow and label
      // stacked and centred. Focused, the frame lights up.
      if (rows[r].seeAll) {
        int c = rows[r].n;
        float f = animFocus[r][c];
        // The see-all card does NOT take the row's focus scale — components.css:5804
        // is `transform: none !important` on it. It is not a poster card either, so the
        // sibling dimming further down never touches it.
        float scale = 1.0f;
        float w = lw * scale, h = artH * scale;
        float cx = settings_content_x() + c * step - scrollX[r] + lw * 0.5f;
        float cy = cardY + artH * 0.5f;
        if (cx > -lw * 1.5f && cx < NV_SCREEN_W + lw) {
          float px = cx - w * 0.5f, py = cy - h * 0.5f;
          // `.home-seeall-card-inner` fills the card's CONTENT box, so it is
          // inset by the card's own 2px border and no further — the poster's
          // second border (NV_CARD_PAD) has no counterpart here. Its visible
          // outline is its own 2px rgba(255,255,255,0.12) border, which lights
          // up to #f5f5f5 on focus over a rgba(255,255,255,0.06) fill.
          float in = NV_CARD_BORDER * scale;
          GfxRect r0 = frameOf((GfxRect){ px, py, w, h }, in);
          float radius = radiusInset(r0.w, r0.h, in);
          float luma = 0.06f + 0.10f * f;
          // Fill, then the border stroked on top of it. NOT the fill-under-fill
          // the poster's focus border uses: there the artwork is opaque and
          // hides the sheet under it, but here both layers are white washes, so
          // painting one over the other would ADD — the 0.06 middle would come
          // out at 0.22 and the card would read as a grey slab.
          gfx_color(r0, radius, 1, 1, 1, luma);
          gfx_rect(r0, 0, GFX_RING, 0, NV_FRAME_RING * scale / r0.h, 0, radius,
                   1, 1, 1, 0.12f + 0.84f * f);
          { TxtLine ls = txt_line(TXT_TITLE2, "\xe2\x86\x92",
                                    236, 237, 242, 255);
            TxtLine lr = txt_line(TXT_ROW_TITLE, "See all",
                                    f > 0.5f ? 255 : 190, f > 0.5f ? 255 : 194,
                                    f > 0.5f ? 255 : 203, 255);
            float block = ls.h + 14.0f + lr.h;
            float by = r0.y + (r0.h - block) * 0.5f;
            txt_draw_alpha(ls, r0.x + (r0.w - ls.w) * 0.5f, by, 0.95f);
            txt_draw_alpha(lr, r0.x + (r0.w - lr.w) * 0.5f,
                               by + ls.h + 14.0f, 1.0f); }
        }
      }

      for (int passe = 1; passe < 2; passe++) {
        for (int c = 0; c < rows[r].n; c++) {
          float f = animFocus[r][c];
          if (passe == 0 && f < 0.01f) continue;
          float scale = 1.0f + scaleOf(kind) * f;
          float w = lw * scale, h = artH * scale;
          // EXPANSION AT REST. `openAmt` is only non-zero on this row's focused card;
          // the ones AFTER it are pushed along by the same amount.
          //
          // The height does not come into it: in the reference it does not change, and
          // the card grows only to the right from a stationary left edge.
          float openAmt = (r == expRow && c == expColumn) ? expOpen : 0.0f;
          float widthIs_open = artH * scale * NV_EXP_ASPECT;
          float pushes = 0.0f;
          if (r == expRow && expOpen > 0.0f && c > expColumn)
            pushes = (artH * NV_EXP_ASPECT - lw) * expOpen;
          if (openAmt > 0.0f) w = lw * scale + (widthIs_open - lw * scale) * openAmt;
          float cx = settings_content_x() + c * step - scrollX[r] + lw * 0.5f
                   + pushes + (w - lw * scale) * 0.5f;
          if (cx < -lw * 1.5f || cx > NV_SCREEN_W + lw) continue;
          // `transform-origin: top` is `50% 0%`: centred across, PINNED along the top.
          // x grows both ways from the centre, y does not move at all — the card only
          // ever gets taller downward. Centring y as well, which is what this did while
          // the scale was zero, would lift it by artH*(scale-1)/2 and walk it back
          // towards the row title.
          float px = cx - w * 0.5f, py = cardY;

          if (passe == 0) {
            // No shadow. It existed to separate the card from the background, but over
            // colourful art it becomes a dark halo around the focused item — and the
            // device does not have that: there the focus is marked by scale and glow.
            (void)f;
            continue;
          }

          const int idxCat = rows[r].start + c;
          if(kind==ROW_TOP10 && rows[r].stackN) {
            // THE CARD'S SURFACE. What stood here was "no backing plate: the stacked
            // posters already form the card". That is true of the posters' OWN
            // footprint and false of the box around them: the plate is 680 wide and
            // six posters at a step of 78 cover about 410 of it, so the remaining
            // third showed the page's own #0D0D0D and the card read as a hole cut in
            // the shelf rather than as a card. In the web this one is a
            // `.home-content-card` like every other, so it carries the same
            // `linear-gradient(180deg, #1c1c1c, #111)` — NV_POSTER_BG_* here.
            //
            // NOT drawArtSkeleton, which is that gradient PLUS the travelling shine:
            // the shine says WAITING, and this card never waits for anything as a
            // whole. Its posters arrive one by one and each already draws its own
            // skeleton below. The plate is what the card looks like at rest.
            drawArtSurface((GfxRect){px, py, w, h}, radiusOf(w, h), 1.0f);
            int count=rows[r].stackN<6?rows[r].stackN:6;
            // THE WIDTH THE POSTER IS REALLY DRAWN AT, which is not the 178 of its
            // box. GFX_CARD with gfx_tex_aspect_current set is a COVER fit, and the
            // box (178 x 275 at rest) is TALLER in proportion than a 2:3 poster, so
            // the texture is fitted by HEIGHT and its width is cropped: 275 * 2/3 =
            // 183 of texture across a 178 window. Asking for 178 would leave the
            // decode UNDER the drawn size and hand the magnify filter the difference.
            //
            // At FULL FOCUS, for the same reason wAsk is: only the height carries the
            // scale here (the 178 is literal), so the drawn width grows to ~195 when
            // the card is landed on. A ceiling that moved with it would re-decode
            // every frame of the animation, which is the one thing tex_get_exact
            // cannot take. At rest the texture is then minified by 1.06 — inside the
            // margin where a four-texel tap is still clean, and nowhere near the
            // 1.414 snap that started all this.
            float stackW = (artH * (1.0f + scaleOf(kind)) - 72.0f) * (2.0f / 3.0f);
            if (stackW < 178.0f) stackW = 178.0f;
            for(int k=0;k<count;k++) {
              const CatItem *it=cat_item_exact(idxCat+k);if(!it)continue;
              GfxRect pr={px+20+k*78,py+18,178,h-72};
              const char *pa=art_by_format(it,0);
              // EXACT, on the same terms as wAsk below: a poster whose item has a
              // backdrop is provably never asked for at 1920 by either hero, so the
              // two cannot fight over the entry's size.
              int sharpK = it->backdrop[0] && pa == it->poster;
              GLuint tx=pa?(sharpK?tex_get_exact(pa,stackW)
                                  :tex_get_width(pa,stackW)):0;
              if(tx){gfx_tex_aspect_current=tex_aspect(pa);gfx_rect(pr,tx,GFX_CARD,0,0,0,.055f,1,1,1,1);gfx_tex_aspect_current=0;}
              else drawArtAbsent(pr,.055f,pa,it,1);
            }
            txt_draw(txt_line(TXT_CAPTION,"TOP 100   ·   Explore the first 10",242,235,248,255),px+24,py+h-42);
            if(focus.row==r)hasItemFocus=0;
            continue;
          }
          if(kind==ROW_SOCIAL && rows[r].start<0) {
            GfxRect b={px,py,w,h};
            gfx_color(b,.055f,.115f,.09f,.15f,1);
            if(f>.01f)gfx_rect(b,0,GFX_RING,0,.008f,0,.055f,.95f,.93f,.99f,f);
            txt_draw(txt_line_trim(TXT_CALLOUT,"Among friends",240,234,248,255,w-48),px+24,py+24);
            txt_draw(txt_line_trim(TXT_CAPTION,"No activity available right now.",195,183,211,255,w-48),px+24,py+91);
            txt_draw(txt_line_trim(TXT_CAPTION,"Follow people on Trakt to discover more.",195,183,211,255,w-48),px+24,py+126);
            txt_draw(txt_line_trim(TXT_CAPTION,"OK · Check connection",240,231,250,255,w-48),px+24,py+h-50);
            if(focus.row==r)hasItemFocus=0;
            continue;
          }
          const CatItem *cItem = cat_item_exact(idxCat);
          if(kind==ROW_SOCIAL && cItem) {
            if(focus.row==r)hasItemFocus=0;
            // Social activity needs context, not a second hero. No panel and no
            // outline: the home's neutral stage does the background work. The image
            // has a lesser editorial role, a thumbnail of the work, while the
            // authorship and the action breathe directly on the screen.
            const float contentTop=py+24.0f;
            const float contentBase=py+h-24.0f;
            const float artW=134.0f;
            const float artX=px+w-24.0f-artW;
            const char *thumbPath=cItem->poster[0]?cItem->poster:
                                  (cItem->backdrop[0]?cItem->backdrop:NULL);
            if(thumbPath){GLuint thumb=tex_get_width(thumbPath,artW);
              if(thumb){GfxRect tr={artX,contentTop,artW,contentBase-contentTop};
                gfx_tex_aspect_current=tex_aspect(thumbPath);
                gfx_rect(tr,thumb,GFX_CARD,0,0,0,.055f,0,0,0,1);gfx_tex_aspect_current=0;
              }
            }
            // A larger avatar, centred on the same vertical band as the art.
            // The shared axis gives the composition the look of an editorial card,
            // rather than an avatar floating at the top with a separate thumbnail below.
            float d=120.0f, ax=px+24.0f, ay=py+(h-d)*.5f;
            GfxRect avatar={ax,ay,d,d};
            GLuint photo=cItem->socialAvatar[0]?tex_get_width(cItem->socialAvatar,220):0;
            // The focus is a disc behind the image, never a stroke over it.
            // That way the two circles share the same centre and the rim stays even,
            // including at the row's upper limit.
            float pad=5.0f*f;
            if(f>.01f)gfx_rect(avatar,0,GFX_DISK,0,0,0,0,.96f,.96f,.98f,f);
            GfxRect core={ax+pad,ay+pad,d-pad*2,d-pad*2};
            gfx_rect(core,0,GFX_DISK,0,0,0,0,.15f,.16f,.18f,1);
            if(photo){gfx_tex_aspect_current=tex_aspect(cItem->socialAvatar);
              gfx_rect(core,photo,GFX_AVATAR,0,0,0,0,1,1,1,1);gfx_tex_aspect_current=0;}
            else {char initial[8]="?";const char *name=cItem->socialName[0]?cItem->socialName:cItem->country;
              if(name[0]){size_t z=1;while(z<4 && (name[z]&0xc0)==0x80)z++;memcpy(initial,name,z);initial[z]=0;}
              TxtLine l=txt_line(TXT_TITLE2,initial,235,236,240,255);txt_draw(l,ax+(d-l.w)*.5f,ay+(d-l.h)*.5f);}
            float tx=ax+d+24.0f,tw=thumbPath?artX-tx-24.0f:w-192.0f;
            TxtLine name=txt_line_trim(TXT_CW_TITLE,cItem->socialName[0]?cItem->socialName:cItem->country,245,245,247,255,tw);
            txt_draw(name,tx,contentTop);
            TxtLine action=txt_line_trim(TXT_MINI,cItem->socialAction[0]?cItem->socialAction:cItem->providerName,181,185,196,255,tw);
            txt_draw(action,tx,contentTop+38.0f);
            TxtLine title=txt_line_trim(TXT_CW_META,cItem->title,228,231,239,255,tw);
            txt_draw(title,tx,contentTop+92.0f);
            TxtLine ep=txt_line_trim(TXT_MINI,cItem->season?cItem->directing:"Film",181,185,196,255,tw);
            txt_draw(ep,tx,contentTop+130.0f);
            TxtLine source=txt_line_trim(TXT_MINI,cItem->providerName[0]?cItem->providerName:"Trakt",155,161,174,255,tw);
            txt_draw(source,tx,contentBase-14.0f);
            if(f>.1f){TxtLine see=txt_line(TXT_MINI,"See profile",235,237,244,255);txt_draw_alpha(see,tx,contentBase-40.0f,f);}
            continue;
          }
          const char *path = NULL;
          // A LANDSCAPE card calls for landscape art. In the web app the landscape
          // card's poster comes from `landscapePoster` -> `background` -> `backdrop`
          // -> `poster` (homeScreen.js:3155), not from the 2:3 poster — using the
          // portrait here would make the shader crop everyone's head off to fit 16:9.
          // Open, the card shows the LANDSCAPE art: that is what it opens for.
          // The swap happens halfway, once the frame is 16:9 wide and the portrait
          // would start being cropped badly.
          // A resume card shows the EPISODE (art_of_card), and it keeps showing
          // it while the card opens: the art is landscape in both states here,
          // so there is nothing to swap halfway, and swapping would cost the
          // re-decode the note on wAsk below is about.
          path = art_of_card(kind, idxCat, openAmt > 0.5f ||
                                        kind == ROW_CONTINUE ||
                                        kind == ROW_RETURN || landscape);

          if (focus_index(&focus, r, c)) {
            GfxRect here = { px, py, w, h };
            itemFocus.index_ = idxCat;
            itemFocus.rect   = here;
            itemFocus.art   = path;
            itemFocus.title = cItem ? cItem->title : NULL;
            itemFocus.genre = cItem ? cItem->genre : NULL;
            itemFocus.meta   = cItem ? cItem->meta : NULL;
            hasItemFocus = 1;
          }
          // Ask by the card's REAL width: it is this row that multiplies.
          // With the blanket ceiling of 640 each poster cost 2.4 MB and the cache
          // overflowed at ~40 textures, evicting what was still on screen.
          //
          // BUT AT THE RESTING WIDTH, NEVER THE ANIMATED ONE. `w` carries the
          // focus scale (5%) and the expansion, so it grows frame by frame while
          // the card takes focus — and tex_get_width rounds the ceiling up to a
          // multiple of 32, so that growth crosses a step and PROMOTES the entry:
          // the poster that was already decoded is thrown away and decoded again
          // for 32 more pixels. MEASURED, landing on a card: `PROMOTE
          // ready->pending w=416 limit=448` and, on the very next line, that same
          // card drawing its skeleton. That is the blink of a frame or two on
          // every poster you move onto — grey while the re-decode runs, and "Art
          // unavailable" when it fails, before the poster comes back.
          //
          // NV_TEX_SLACK is 1.25 precisely so the focused card needs no second
          // decode; it can only do that job if the ceiling STANDS STILL while the
          // scale moves. Open, the art is the landscape one — a different entry —
          // and its width is asked for whole, for the same reason.
          //
          // AND THE WIDTH IS THE ARTWORK'S, NOT THE CARD'S. The art is drawn into
          // frameOf(card, NV_CARD_PAD * scale), so a framed card's picture is 8px
          // narrower than its box; asking by the box overstated every poster by that
          // much on top of everything else.
          float scaleMax = 1.0f + scaleOf(kind);
          float boxMax = openAmt > 0.5f ? artH * scaleMax * NV_EXP_ASPECT
                                        : lw * scaleMax;
          float wAsk = posterFrame(kind) ? boxMax - 2.0f * NV_CARD_PAD * scaleMax
                                         : boxMax;
          // EXACT FOR THE PORTRAIT POSTER, tex_get_width for everything else.
          //
          // tex_get_width lands the texture between 1.25x and 1.6x the drawn size
          // (NV_TEX_SLACK, then rounded up to a multiple of 32) and card textures
          // carry GL_LINEAR_MIPMAP_NEAREST, which SNAPS at 1.414. A 229 card decoded
          // at 288 draws its art at 221: a ratio of 1.303, just under the snap, so
          // the GPU minifies off level 0 with a four-texel tap and throws away a
          // third of the source. That is the "pixelated" look — it is aliasing, not
          // a small texture. The same arithmetic is what home.c's collection hero
          // records for the wordmarks, and tex_get_exact is the fix it already uses:
          // the decode thread box-filters to the drawn width in one pass and the
          // upload carries no mipmap chain, so what reaches the panel is a 1:1 blit.
          // Asked for at scaleMax the ceiling STANDS STILL while the focus scale
          // moves, which is the one thing tex_get_exact cannot tolerate.
          //
          // ONLY WHEN THE ITEM HAS A BACKDROP, and that condition is load-bearing.
          // An exact entry re-decodes when the request moves in EITHER direction,
          // while tex_get_hero's 1920 only ever promotes upward — so a path wanted by
          // both would decode every frame, for ever. Both heroes that could want a
          // poster take it only as a fallback (`artOfItem` here, `artOf` in detail.c,
          // each gated on an empty backdrop), so a poster whose item HAS a backdrop
          // is provably never asked for at 1920. Smaller non-exact callers (the
          // social panel's 96) cannot start the fight: they promote upward or not at
          // all. The landscape/open art IS the backdrop, so it keeps tex_get_width.
          int sharp = cItem && path == cItem->poster && cItem->backdrop[0];
          GLuint t = path ? (sharp ? tex_get_exact(path, wAsk)
                                   : tex_get_width(path, wAsk)) : 0;
          float radius = radiusOf(w, h);
          GfxRect card = { px, py, w, h };
          // Continue watching keeps the old geometry, and that is the reference
          // too: its `.home-continue-media` has `border: 0`, so the art does run
          // to the edge, and its ring is a real 4px outset shadow on the card.
          int framed = posterFrame(kind);
          float pad = framed ? NV_CARD_PAD * scale : 0.0f;
          GfxRect art = frameOf(card, pad);
          float radiusA = framed ? radiusArt(art.w, art.h) : radius;
          if (f > 0.01f) {
            if (framed) {
              // The frame's own border, lit to #F5F5F5 (NV_FRAME_RING_C), and
              // PAINTED THE WAY CSS PAINTS A BORDER: fill the frame's border
              // box, then let the artwork land on top of it. What is left
              // showing is exactly the 2px band.
              //
              // The obvious alternative — GFX_RING on the border's centre line —
              // is what this did first, and it looked wrong on the TV. That ring
              // is `smoothstep(esp, esp*0.55, abs(d))`, so a 2px stroke carries
              // about a pixel of ramp on EACH side: two thirds of the border is
              // then partial white, and instead of a crisp edge you get a grey
              // smear. Filling shares the artwork's own antialiased outline, so
              // the border is as clean as the corner it follows.
              // The border eats the gutter from the INSIDE OUT: its inner edge
              // is always flush with the artwork, so widening it walks the outer
              // edge towards the card outline instead of growing a halo past it.
              // At NV_FRAME_RING == NV_CARD_PAD the two coincide.
              float in = NV_CARD_PAD - NV_FRAME_RING;
              if (in < 0.0f) in = 0.0f;
              in *= scale;
              GfxRect frame = frameOf(card, in);
              gfx_color(frame, radiusFocus(frame.h, in),
                        NV_FRAME_RING_C, NV_FRAME_RING_C, NV_FRAME_RING_C,
                        NV_FRAME_RING_A * f);
            } else {
              // 4 px of #FFFFFF, OUTSIDE the art. MEASURED on the reference device
              // (TCL, same card, same row): 4 solid px, x 102->105 with no ramp, and
              // the same number appears on the episode card and the detail button.
              GfxRect border = { px - NV_RING_FOCUS, py - NV_RING_FOCUS,
                                w + NV_RING_FOCUS * 2, h + NV_RING_FOCUS * 2 };
              // THE RADIUS HAS TO BE RE-NORMALISED FOR THE BIGGER BOX. The
              // shader reads it as a fraction of the HEIGHT, and this rect is
              // 2*NV_RING_FOCUS taller than the card, so passing the card's
              // `radius` straight through asked for radius*(h+8) px instead of
              // the radius*h + 4 that a concentric outer corner needs. On the
              // continue watching card (236 tall, 24px corners) that is 24.8px
              // against 28: the white band survived along the straight edges
              // and pinched to under a pixel at each corner, which is the
              // "border doesn't apply" — it was there, just not on the corners.
              float rPx = radius * h + NV_RING_FOCUS;
              gfx_color(border, rPx / (h + NV_RING_FOCUS * 2.0f),
                        1.0f, 1.0f, 1.0f, f);
            }
          }
          // A CARD WITH NO ART: a solid surface, not emptiness. Without this the card
          // took the background's colour — MEASURED: #242429 over #252629, a
          // difference of (1,2,0), a contrast of 1.0:1. It was literally invisible, and
          // it was the origin of the complaint "not all the posters show up": they did
          // show up, in exactly the background's tone. The reference draws #2C2C2C in
          // the exact box.
          if (t) {
            // NO OSCILLATING PARALLAX on the focused card.
            //
            // There used to be a sin/cos of the clock here shifting the focused card's
            // art forever — a tvOS-style "breathing". The reference does NOT have it,
            // and the proof is direct: the device's screenrecord only writes a frame
            // when something changes on screen, and after the navigation settled it
            // went 1.8 s and 2.4 s WITHOUT EMITTING A SINGLE FRAME, in two different
            // recordings. A genuinely still screen, not "almost".
            //
            // Besides not existing there, it was the worst kind of animation for this
            // GPU: it forced the whole row to be redrawn on every frame forever, and
            // the dominant cost here is fill rate.
            gfx_tex_aspect_current = tex_aspect(path);
            gfx_rect(art, t, GFX_CARD, f, 0.0f, 0.0f,
                     radiusA, 0, 0, 0, 1);
            gfx_tex_aspect_current = 0.0f;
          } else {
            // A CARD WITH NO ART: a SOLID, visible surface, not emptiness.
            //
            // This used to be #242429 (0.14,0.14,0.16) — MEASURED against the
            // background of the time, #252629: a difference of (1,2,0), a contrast of
            // 1.0:1. The card existed and was literally invisible, and that was the
            // origin of the complaint "not all the posters show up". They did show up,
            // in exactly the background's tone, and the only sign was the focus ring
            // around an empty rectangle — which reads as BROKEN, not as loading.
            //
            // The reference uses #2C2C2C over #0D0D0D: a luminance ~22x the
            // background's, impossible to miss.
            drawArtAbsent(art, radiusA, path, cItem, 1.0f);
          }
          // THE WATCHED BADGE: a white disc with a dark tick, in the poster's top
          // right corner. The reference has it and we had no indicator at all on the
          // home — without it there is no way to scan a row and see what has already
          // been watched, which is the screen's main use.
          //
          // >= 90% is "watched", not 100%: almost nobody watches the credits, and the
          // player itself already rounds to the end when less than a minute is left
          // (player_close). Marking only at 100% would leave out precisely what has
          // just been watched.
          if (cItem && cItem->progress >= 90 && kind != ROW_CONTINUE) {
            float d = art.w * 0.16f;             // proportional to the card
            float mx = art.x + art.w - d - 10.0f, my = art.y + 10.0f;
            GfxRect disk = { mx, my, d, d };
            gfx_color(disk, 0.5f, 1, 1, 1, 0.94f);
            // The tick drawn with two strokes: the font's glyph will not do here
            // because it would need a whole line of text just for this, and the line
            // cache has 256 entries already fought over by the titles.
            { float cx = mx + d * 0.5f, cy = my + d * 0.5f;
              float e = d * 0.085f;              // thickness
              GfxRect a1 = { cx - d * 0.20f, cy - e * 0.5f, d * 0.20f, e };
              GfxRect a2 = { cx - d * 0.02f, cy - e * 0.5f, d * 0.34f, e };
              gfx_rect(a1, 0, GFX_COLOR, 0, 0, 0, 0.5f, 0.07f, 0.07f, 0.07f, 0.94f);
              gfx_rect(a2, 0, GFX_COLOR, 0, 0, 0, 0.5f, 0.07f, 0.07f, 0.07f, 0.94f); }
          }

          // `cardDepthEnabled` plus the per-section switch: `cardDepthPosters` on the
          // catalogue rows, `cardDepthContinueWatching` on the first one.
          drawDepth(art, radiusA,
                              kind == ROW_CONTINUE ? settings_depth_cw()
                                                       : settings_depth_posters());

          // --- posterLabelsEnabled ---------------------------------------
          // A LANDSCAPE card only: the caption goes INSIDE the frame, over a gradient
          // covering 54% of the height, with 14 of side inset and 12 from the base
          // (.home-poster-landscape-copy). The UPRIGHT card's block below the poster
          // (.home-poster-copy) is gone — see hasLabel.
          if (kind != ROW_CONTINUE && kind != ROW_RETURN && !editorial(kind) && settings_labels_poster() && cItem) {
            const char *name = cItem->title[0] ? cItem->title : NULL;
            const char *sub  = cItem->genre[0] ? cItem->genre : NULL;
            if (landscape && name) {
              // `.home-poster-landscape-copy` is a child of the frame, so its
              // 14/12 insets are measured from the ART edge, not the card one.
              float bottom = art.y + art.h;
              GfxRect veil = { art.x, bottom - art.h * NV_LAND_VEIL,
                               art.w, art.h * NV_LAND_VEIL };
              gfx_rect(veil, 0, GFX_VEIL, 0, 0, 0, radiusA, 0, 0, 0, 0.80f);
              float maxW = art.w * NV_LAND_COPY_MAXW;
              float bx = art.x + NV_LAND_COPY_PAD;
              TxtLine tn = txt_line_trim(TXT_CAPTION, name, 245, 246, 250, 255, maxW);
              if (sub) {
                TxtLine ts = txt_line_trim(TXT_MINI, sub, 200, 202, 210, 255, maxW);
                txt_draw_alpha(ts, bx, bottom - NV_LAND_COPY_BASE - ts.h, 0.85f);
                txt_draw_alpha(tn, bx,
                                   bottom - NV_LAND_COPY_BASE - ts.h - 4.0f - tn.h, 0.98f);
              } else {
                txt_draw_alpha(tn, bx, bottom - NV_LAND_COPY_BASE - tn.h, 0.98f);
              }
            }
          }

          if(kind==ROW_TOP10) {
            char rank[8];snprintf(rank,sizeof rank,"%d",c+1);
            TxtLine number=txt_line(TXT_RANK,rank,240,241,245,255);
            TxtLine ink=txt_line(TXT_RANK,rank,16,17,20,255);
            float nx=art.x-12,ny=art.y+art.h-number.h-8;
            for(int dx=-2;dx<=2;dx+=2)for(int dy=-2;dy<=2;dy+=2)
              txt_draw(number,nx+dx,ny+dy);
            txt_draw(ink,nx,ny);
          }

          if (kind == ROW_CONTINUE)
            resume_draw(cItem, (GfxRect){px, py, w, h});
          if (kind == ROW_RETURN)
            resume_draw(cItem, (GfxRect){px, py, w, h});

          // 4. HIGHLIGHT: title and metadata INSIDE the art, over a dark veil at the
          // base — as the Apple TV does. The title plays the part of the logo embedded
          // in the key art, which we do not have (TMDB does not always have a logo;
          // when it does, it goes here in the text's place).
          // AN OPEN CARD: a veil at the base and the title's LOGO, as on the TCL.
          //
          // The logo and not the name set in the interface font: every production has
          // typography of its own, and writing "The Pitt" in Inter erases precisely
          // what makes a title recognisable from a distance. With no logo in the
          // catalogue the card keeps just the art — better than a generic name over it.
          if (openAmt > 0.01f && cItem && cItem->logo[0]) {
            GLuint tl = tex_get(cItem->logo);
            if (tl) {
              float pad = 34.0f * scale;
              float ap = tex_aspect(cItem->logo);
              float hL, wL, maxW;
              GfxRect veil = art;
              gfx_rect(veil, 0, GFX_VEIL, 0, 0, 0, radiusA, 0, 0, 0, 0.72f * openAmt);
              // NO GUESSING THE ASPECT RATIO. The fallback of 4.0 that used to be here
              // drew a rectangle wider than the image, and the card mode CROPS what is
              // left over — "REACHER" came out with both ends cut off. With no
              // measurement from the file, it does not draw.
              if (ap <= 0.0f) { tl = 0; }
              // The WIDTH rules and the height follows from it: that way the rectangle
              // always has the image's aspect ratio and the crop never happens.
              //
              // MEASURED on the TCL with the card open: a 163 px logo in a 565 card
              // (29% of the width) and 66 tall in a card of 320 (21%). The height
              // ceiling exists so a square logo does not become a block.
              maxW = art.w * 0.30f;
              wL = maxW; hL = wL / ap;
              if (hL > art.h * 0.22f) { hL = art.h * 0.22f; wL = hL * ap; }
              // GFX_BRAND/GFX_TEXT, NOT GFX_CARD. The card mode is for ART: it does
              // cover with 3% of over-scan on purpose (the parallax margin) and
              // discards the texture's alpha. On a logo that cuts both ends off —
              // "REACHER" came out as "EACHE" — and it also paints black where it
              // should be transparent.
              //
              // The right pair already existed in the project, in the highlight row:
              // tex_brand_dark decides whether the shape comes from the alpha (a light
              // logo) or from the drawing (a dark logo). Reused here instead of
              // reinvented.
              if (tl) { GfxRect rl = { art.x + pad, art.y + art.h - pad - hL, wL, hL };
                GfxMode m = tex_brand_dark(cItem->logo) ? GFX_BRAND : GFX_TEXT;
                gfx_tex_aspect_current = 0.0f;
                gfx_rect(rl, tl, m, 0, 0, 0, 0.0f, 1, 1, 1, openAmt); }
            }
          }

          if (editorial(kind)) {
            // The editorial block is painted over the artwork, so it measures
            // from the frame: `ex`/`ey`/`ew`/`eh` are the art box, which is the
            // card pulled in by NV_CARD_PAD.
            float ex = art.x, ey = art.y, ew = art.w, eh = art.h;
            gfx_rect(art, 0, GFX_VEIL, 0, 0, 0, radiusA, 0, 0, 0, 0.88f);


            // The title's logo, as on the device: every production has typography of
            // its own, and writing the name in the interface font erases that.
            const CatItem *ci = cItem;
            GLuint tlogo = (ci && ci->logo[0]) ? tex_get_width(ci->logo, ew * .65f) : 0;
            // No data, no text — not the demo list that used to be here, stamping
            // another title's name and genre onto the card.
            const char *name   = (ci && ci->title[0]) ? ci->title : NULL;
            const char *genre = (ci && ci->genre[0]) ? ci->genre
                                : ci ? (!strcmp(ci->kind, "series") ? "Series" : "Film") : NULL;
            TxtLine tg = genre
                        ? txt_line_trim(TXT_HERO_META, genre, 226, 228, 233, 255, ew - 64)
                        : (TxtLine){ 0, 0, 0 };

            float pad = kind == ROW_HIGHLIGHT ? 28.0f : 22.0f;
            float base = ey + eh - pad;
            float yMeta = base - tg.h;
            float hTitle;
            if (tlogo) {
              float ap = tex_aspect(ci->logo);
              if (ap <= 0.0f) ap = 4.0f;
              hTitle = eh * .22f;
              float wTitle = hTitle * ap, maxW = ew * .65f;
              if (wTitle > maxW) { wTitle = maxW; hTitle = wTitle / ap; }
              GfxRect rl = { ex + pad, yMeta - hTitle - 10.0f, wTitle, hTitle };
              gfx_tex_aspect_current = 0.0f;
              { GfxMode m = tex_brand_dark(ci->logo) ? GFX_BRAND : GFX_TEXT;
              gfx_rect(rl, tlogo, m, 0, 0, 0, 0.0f, 1, 1, 1, 1.0f); }
            } else if (name) {
              TxtLine tn = txt_line_trim(TXT_CW_TITLE, name, 245, 246, 249, 255, ew - pad*2);
              hTitle = (float)tn.h;
              txt_draw(tn, ex + pad, yMeta - hTitle - 10.0f);
            } else {
              hTitle = 0.0f;
            }
            if (genre) txt_draw(tg, ex + pad, yMeta);

            // No age badge here. A red plate beside the genre reads as an official
            // warning, and the certification is not what this card is for — the same
            // reason the rating left the hero and the detail screen's meta line.
          }

          // EVERY CARD BUT THE FOCUSED ONE IS DIMMED — see NV_DIM_UNFOCUSED. It is the
          // web's strongest focus cue, and it is global: cards in the other rows go
          // down too, and with focus parked outside the rows they all do.
          //
          // Multiplying by 0.8 and compositing black at 0.2 over the same pixels are
          // the same arithmetic (`c*0.8 + 0*0.2`), so this needs no shader work and no
          // new GfxMode — it is one rounded rect over the art, LAST, so that it takes
          // the label veil, the resume overlay and the editorial block down with it,
          // exactly as a CSS filter on the article would.
          //
          // Over the ART and not the card box: the card's own background is transparent
          // in the web, so the 4px gutter has nothing in it to darken, and covering it
          // would only spill onto the page behind.
          if (f < 0.999f) {
            gfx_color(art, radiusA, 0, 0, 0,
                      (1.0f - NV_DIM_UNFOCUSED) * (1.0f - f));
          }

          // Progressive feedback for the gesture, without duplicating the context menu.
          // The bar only appears while the same item is under pressure; once the
          // threshold is reached, ctxmenu has already opened and the release is consumed.
          if (okPressing && okHold > 0.0f &&
              focus_can_press_long() && focus_index(&focus, r, c)) {
            float bx = art.x + NV_HOME_TEXT_GUTTER;
            float bw = art.w - NV_HOME_TEXT_GUTTER * 2.0f;
            GfxRect rail = { bx, art.y + art.h - 12.0f, bw, 4.0f };
            gfx_color(rail, 0.5f, 0.18f, 0.19f, 0.22f, 0.92f);
            gfx_color((GfxRect){ bx, rail.y, bw * okHold, rail.h },
                    0.5f, 0.92f, 0.93f, 0.96f, 1.0f);
            TxtLine hint = txt_line(TXT_MINI,
                                      okHold >= 1.0f ? "Release to open options"
                                                     : "Hold for options",
                                      225, 228, 235, 255);
            txt_draw_alpha(hint, bx, art.y + art.h - 38.0f, 0.92f);
          }
        }
      }
    }
    y += NV_LEGACY_ROW_HEAD_H + heightTotalOf(kind) + rowGap();
  }
  gfx_opacity_group=1;
  gfx_no_crop();
}

void home_shutdown(void) {}
void home_record_return(int index_, double posSeg, double durationSeg) {
  int new = -1;
  if (index_ >= 0 && durationSeg > 1.0) {
    double p = posSeg / durationSeg;
    if (p >= 0.01 && p < 0.90) new = index_;
  }
  if (new != resumeIndex) { resumeIndex = new; resumeRev++; }
  else if (new >= 0) resumeRev++; // updates the same session's bar/time
  const CatItem *c = new >= 0 ? cat_item_exact(new) : NULL;
  snprintf(resumeId, sizeof resumeId, "%s", c ? c->imdb : "");
}
int home_wants_exit(void) { return wantsExit; }

int home_item_focused(HomeItem *out) {
  if (!hasItemFocus) return 0;
  *out = itemFocus;
  return 1;
}

int home_n_arts(void) { return nBd; }
// When there is a catalogue, the art comes from it (in the right order, matched to
// the title); with no catalogue, it falls back to sweeping the folder.
const char *home_backdrop(int i) {
  const CatItem *c = cat_item_exact(i);
  return c && c->backdrop[0] ? c->backdrop : NULL;
}
const char *home_art(int i) { return (nBd && i >= 0 && i < nBd) ? bd[i] : NULL; }

// Consumes the request to open: whoever reads it, clears it. That way OK counts once
// only, even if the frame takes a while.
int home_requested_open(void) { int v = requestOpen; requestOpen = 0; return v; }

// Consumes the request to open the side menu: whoever reads it, clears it.
int home_requested_menu(void) { int v = requestMenu; requestMenu = 0; return v; }
