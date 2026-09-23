// Settings: a vertical list in sections, the label on the left and the value on
// the right.
//
// The rule that organises the whole screen: there are THREE natures of row and
// they HAVE to look different. A choice row takes focus and gets arrows around
// the value; a NUMERIC row adds a fill bar under the value, because "28%" with no
// bar does not say where it sits in the range; a read-only row gets a muted
// highlight, with no arrows. With the same drawing on all three, the user presses
// left and right on the app's version expecting something to happen — which is
// why the distinction became a requirement.
//
// The keys and the groupings follow the web app's Layout screen
// (js/ui/screens/settings/settingsScreen.js), including the labels read off the
// running screen.
#include "settings.h"
#include "gfx.h"
#include "text.h"
#include "tex_cache.h"
#include "anim.h"
#include "layout.h"
#include "session.h"
#include "sync.h"
#include "profiles.h"
#include "qr.h"
#include "traktauth.h"
#include "simklauth.h"
#include "js.h"
#include <stdio.h>
#include <string.h>

// The app's version: the same string as the packaged appinfo.json. It lives here
// because the screen has no way to read the manifest at runtime on the device.
#define SETTING_VERSION       "1.0.1"

#define SETTING_LINE_H       88.0f
#define SETTING_LINE_GAP      8.0f
#define SETTING_SEC_GAP       46.0f    // end of one section to the next section's header
#define SETTING_SEC_HEADER     44.0f    // height reserved for the section header
// Not a constant: it follows the rail, like all the rest of the content. With the
// bar collapsed the list also starts at 104 — leaving 248 hard-coded here made
// the Settings screen the only one misaligned with the others.
#define SETTING_LIST_X      settings_content_x()
#define SETTING_LIST_W     1120.0f
#define SETTING_PAD           34.0f    // row edge to text
#define SETTING_TOP        (NV_MARGIN_Y + 118.0f)   // below the screen's title
#define SETTING_BASE        (NV_SCREEN_H - NV_MARGIN_Y - 48.0f)
// The row's radius as a fraction of the smaller side (the shader's SDF is
// normalised): 12px over a height of 88.
#define SETTING_RADIUS           0.14f

// The enum's order = the order in the key file and in the tables. Adding in the
// MIDDLE is safe: the file is keyed, not positional (see settings_dir).
typedef enum {
  // Playback
  SETTING_QUALITY, SETTING_DV, SETTING_ATMOS, SETTING_SUBS, SETTING_SEEK_COLOR,
  // Layout da Home
  SETTING_LANDSCAPE, SETTING_HERO_FULL, SETTING_HERO_AREA, SETTING_HERO_BAND,
  // Conteudo da Home
  SETTING_RAIL, SETTING_RAIL_MODERN, SETTING_RAIL_BLUR, SETTING_HERO, SETTING_HERO_CATALOGS,
  SETTING_DISCOVER, SETTING_LABELS, SETTING_NAME_ADDON, SETTING_SUFFIX_KIND,
  SETTING_HIDE_UNRELEASED, SETTING_SCORES_HOME, SETTING_GRADIENT_CLASSIC,
  SETTING_SOCIAL,
  // Continue watching
  SETTING_CW_ON, SETTING_CW_STYLE, SETTING_CW_THUMB, SETTING_CW_BLUR_NEXT,
  SETTING_CW_FURTHEST, SETTING_CW_NOT_SHOWN, SETTING_CW_ORDER,
  // Detail page
  SETTING_DET_BLUR_NOT_WATCHED, SETTING_DET_TRAILER, SETTING_DET_META_EXT, SETTING_DET_DATE_FULL,
  // Poster focus
  SETTING_EXPAND, SETTING_EXPAND_DELAY, SETTING_NAV_FAST,
  // Depth
  SETTING_DEPTH, SETTING_DEPTH_BORDER, SETTING_DEPTH_BRIGHTNESS, SETTING_DEPTH_COVERAGE,
  SETTING_DEPTH_POSTERS, SETTING_DEPTH_CW, SETTING_DEPTH_EPS, SETTING_DEPTH_CAST, SETTING_DEPTH_TRAILERS,
  // Item size
  SETTING_WIDTH_DP, SETTING_RADIUS_DP,
  // Interface
  SETTING_ANIM,
  // Account
  SETTING_PROFILE_ACTIVE, SETTING_SYNC, SETTING_TRAKT, SETTING_SIMKL, SETTING_EXIT,
  // About
  SETTING_VERSION_I, SETTING_SPACE,
  SETTING_N
} OptionId;

static const char *V_QUALITY[] = { "Automatic", "4K", "1080p", "720p" };
// Which subtitle the player turns on BY ITSELF when a title starts: the file's
// own track in that language first, an addon's download after. "Automatic" is
// English — it was Portuguese-then-English, the app's first owner's order — and
// the named values pin one language with no fallback: whoever asks for English
// and is given Portuguese has been answered a question they did not ask. The
// languages are the ones addons.c searches; there is no third in the search.
static const char *V_SUBS[] = { "Off", "Automatic", "Portuguese", "English" };
// The player's seek bar: the fill and the playhead. Violet is the brand mark's
// own colour and the default; White is what the bar was before. The RGB lives in
// SEEK_RGB below, in the same order.
static const char *V_SEEK[] = { "Violet", "White", "Blue", "Green", "Red", "Orange" };
static const float SEEK_RGB[][3] = {
  { 0x83 / 255.0f, 0x67 / 255.0f, 0xF5 / 255.0f },   /* #8367F5 */
  { 0xF5 / 255.0f, 0xF5 / 255.0f, 0xF5 / 255.0f },   /* #F5F5F5, the old fill */
  { 0x3B / 255.0f, 0x82 / 255.0f, 0xF6 / 255.0f },   /* #3B82F6 */
  { 0x22 / 255.0f, 0xC5 / 255.0f, 0x5E / 255.0f },   /* #22C55E */
  { 0xE5 / 255.0f, 0x48 / 255.0f, 0x4D / 255.0f },   /* #E5484D */
  { 0xF5 / 255.0f, 0x9E / 255.0f, 0x0B / 255.0f },   /* #F59E0B */
};
static const char *V_ON[]      = { "On", "Off" };
static const char *V_ANIM[]      = { "Full", "Reduced" };
// `collapseSidebar`: collapsed = the rail disappears and the content starts at 104.
static const char *V_RAIL[]      = { "Collapsed", "Fixed" };
// `continueWatchingCardStyle`, validated in layoutPreferences.js against exactly
// these three values.
static const char *V_CW[]        = { "Card", "Wide", "Poster" };
// `continueWatchingSortMode`, normalizado em normalizeContinueWatchingSortMode.
static const char *V_CW_ORDER[]  = { "Default", "Streaming style", "Separate upcoming" };
// `discoverLocation`, validado contra estes tres.
static const char *V_DISCOVER[] = { "Show in Search", "In the sidebar", "Off" };
// `homeImdbRatingsVisibility` — normalizeHomeImdbRatingsVisibility so aceita
// SHOW_ALL e HIDE_ALL.
static const char *V_SCORES[]     = { "Show", "Hide" };
// Where a full-screen backdrop is DRAWN. Local to this port: the web app has the
// backdrop either banded or full-screen and nothing in between, so there is no
// key of its own to match and the account blob never touches it.
// "Top band" is the third state of ONE picture — it only means anything with
// "Full-screen backdrop" on, which is why it sits directly under it.
static const char *V_HERO_AREA[] = { "Whole screen", "Top band" };

// The row's nature.
// OP_ACTION responds to OK, not to left/right. It is NOT read-only: a row that
// does something has to have the same highlight as one that changes a value,
// otherwise the user presses OK expecting nothing to happen.
typedef enum { OP_CHOICE, OP_NUMBER, OP_READ, OP_ACTION } OptionKind;

typedef struct {
  const char  *label;
  OptionKind    kind;
  const char **values;   // OP_CHOICE
  int          n;         // OP_CHOICE: how many values
  int          min, max, step;   // OP_NUMBER
  const char  *suffix;            // OP_NUMBER: "%", "s", "dp"
} Option;

#define ESC(rot, vals, count) { rot, OP_CHOICE, vals, count, 0, 0, 0, NULL }
#define NUM(rot, lo, hi, st, suffix) { rot, OP_NUMBER, NULL, 0, lo, hi, st, suffix }
#define READ(rot)            { rot, OP_READ, NULL, 0, 0, 0, 0, NULL }
#define ACTION(rot)           { rot, OP_ACTION,    NULL, 0, 0, 0, 0, NULL }

static const Option OPTIONS[SETTING_N] = {
  ESC("Maximum quality",           V_QUALITY, 4),
  ESC("Dolby Vision",               V_ON, 2),
  ESC("Dolby Atmos",                V_ON, 2),
  ESC("Subtitles",                  V_SUBS, 4),
  ESC("Seek bar colour",            V_SEEK, 6),

  ESC("Landscape posters",       V_ON, 2),   // modernLandscapePostersEnabled
  ESC("Full-screen backdrop",        V_ON, 2),   // modernHeroFullScreenBackdropEnabled
  ESC("Backdrop area",               V_HERO_AREA, 2), // heroBackdropArea (local)
  // heroBackdropScale (local). A percentage of the SCREEN'S WIDTH, and it is a row
  // rather than a constant because it is judged by eye from the sofa: every value
  // tried in the source costs an ARM build and a deploy. Steps of 5 — 1% of 1920
  // is 19px and nobody is choosing between 1536 and 1555.
  NUM("Backdrop size",               50, 100, 5, "%"),  // heroBackdropScale

  ESC("Sidebar",              V_RAIL, 2),   // collapseSidebar
  ESC("Modern sidebar",      V_ON, 2),   // modernSidebar
  ESC("Modern sidebar blur",  V_ON, 2),   // modernSidebarBlur
  ESC("Show hero",           V_ON, 2),   // heroSectionEnabled
  READ("Hero catalogues"),                   // heroCatalogKeys (a count)
  ESC("Discover location",         V_DISCOVER, 3), // discoverLocation
  ESC("Poster labels",       V_ON, 2),   // posterLabelsEnabled
  ESC("Addon name in the catalogue",  V_ON, 2),   // catalogAddonNameEnabled
  ESC("Content type",           V_ON, 2),   // catalogTypeSuffixEnabled
  ESC("Hide unreleased",       V_ON, 2),   // hideUnreleasedContent
  ESC("Overall ratings",          V_SCORES, 2),  // homeImdbRatingsVisibility
  ESC("Classic focus gradient", V_ON, 2),   // classicFocusGradientEnabled
  ESC("Show \"Among friends\"",  V_ON, 2),   // socialRowEnabled

  ESC("Show \"Continue watching\"", V_ON, 2), // continueWatchingEnabled
  ESC("\"Continue watching\" style", V_CW, 3), // continueWatchingCardStyle
  ESC("Episode thumbnail",      V_ON, 2),   // useEpisodeThumbnailsInCw
  ESC("Blur next episode",  V_ON, 2),   // blurContinueWatchingNextUp
  ESC("Next from the furthest episode", V_ON, 2),// nextUpFromFurthestEpisode
  ESC("Show unaired episodes", V_ON, 2),// showUnairedNextUp
  ESC("Sort order",                  V_CW_ORDER, 3), // continueWatchingSortMode

  ESC("Blur unwatched",    V_ON, 2),   // blurUnwatchedEpisodes
  ESC("Trailer button",           V_ON, 2),   // detailPageTrailerButtonEnabled
  ESC("Prefer external metadata", V_ON, 2), // preferExternalMetaAddonDetail
  ESC("Full release date", V_ON, 2),  // showFullReleaseDate

  ESC("Expand poster on focus",   V_ON, 2),   // focusedPosterBackdropExpandEnabled
  NUM("Expansion delay",         0, 10, 1, " s"), // ...ExpandDelaySeconds
  ESC("Fast horizontal navigation", V_ON, 2),  // fastHorizontalNavigationEnabled

  ESC("Depth effect",     V_ON, 2),   // cardDepthEnabled
  NUM("Edge brightness",            0, 100, 2, "%"), // cardDepthEdgeStrength
  NUM("Sheen",                      0, 100, 2, "%"), // cardDepthSheenStrength
  NUM("Edge coverage",         0, 100, 2, "%"), // cardDepthEdgeCoverage
  ESC("Depth on posters",  V_ON, 2),
  ESC("Depth on \"Continue\"", V_ON, 2),
  ESC("Depth on episodes", V_ON, 2),
  ESC("Depth on cast",     V_ON, 2),
  ESC("Depth on trailers",  V_ON, 2),

  NUM("Item width",            72, 200, 2, " dp"), // posterCardWidthDp
  NUM("Corner radius",              0, 40, 1, " dp"),   // posterCardCornerRadiusDp

  ESC("Animations",                  V_ANIM, 2),

  READ("Profile"),
  READ("Sync"),
  ACTION("Trakt"),
  ACTION("Simkl"),
  ACTION("Sign out"),
  READ("Version"),
  READ("Memory used by images"),
};

// Each option's name in the file. The format used to be POSITIONAL — one line per
// option, in the enum's order — and that is why adding an option in the middle
// made the file of anyone who already had the app apply the wrong values,
// silently. With one key per line, a new option is born at its default and the
// old ones stay where they were. The names follow the web app's where there is a
// counterpart.
static const char *KEY[] = {
  "quality", "dolbyVision", "dolbyAtmos",
  // NOT "subtitleLanguage": that name exists in the web app's blob with values of
  // its own ("off", "eng", "system"), and sharing the name would have the account
  // feed a string this row cannot read on every sync. A name of this port's own
  // never matches in the blob, which is what keeps the row local — and, unlike a
  // "-" key, it is still written to settings.txt.
  "subtitlePreferredGroup",
  // Local to this port: the web app has no such key, so the blob never touches it.
  "seekBarColor",
  "modernLandscapePostersEnabled", "modernHeroFullScreenBackdropEnabled",
  "heroBackdropArea", "heroBackdropScale",
  "collapseSidebar", "modernSidebar", "modernSidebarBlur",
  "heroSectionEnabled", "-heroCatalogKeys",
  "discoverLocation", "posterLabelsEnabled", "catalogAddonNameEnabled",
  "catalogTypeSuffixEnabled", "hideUnreleasedContent",
  "homeImdbRatingsVisibility", "classicFocusGradientEnabled",
  "socialRowEnabled",
  "continueWatchingEnabled", "continueWatchingCardStyle",
  "useEpisodeThumbnailsInCw", "blurContinueWatchingNextUp",
  "nextUpFromFurthestEpisode", "showUnairedNextUp", "continueWatchingSortMode",
  "blurUnwatchedEpisodes", "detailPageTrailerButtonEnabled",
  "preferExternalMetaAddonDetail", "showFullReleaseDate",
  "focusedPosterBackdropExpandEnabled", "focusedPosterBackdropExpandDelaySeconds",
  "fastHorizontalNavigationEnabled",
  "cardDepthEnabled", "cardDepthEdgeStrength", "cardDepthSheenStrength",
  "cardDepthEdgeCoverage", "cardDepthPostersEnabled",
  "cardDepthContinueWatchingEnabled", "cardDepthEpisodeCardsEnabled",
  "cardDepthCastEnabled", "cardDepthTrailersEnabled",
  "posterCardWidthDp", "posterCardCornerRadiusDp",
  "reducedAnimations",
  // Account: these are local rows; they neither come from nor go to the cloud profile.
  "-profile", "-sync", "-trakt", "-simkl", "-exit",
  "-version", "-space",
};

// The compiler CHECKS that there is one key per option. Without this, adding an
// option to the enum and forgetting the key leaves the last entries NULL and
// MISALIGNS every key after the insertion point — and the defect does not show up
// at once: only when settings.txt starts to exist does the strcmp(NULL,...) bring
// the app down on the next start. That is exactly what happened, and the only
// symptom on the TV was the app opening and closing.
typedef char checked_one_key_per_option[
  (sizeof KEY / sizeof *KEY == SETTING_N) ? 1 : -1];

// Where each section starts and how many options it has. A section is a visual
// grouping, not a navigation level: up/down crosses the headers without stopping
// on them, as on the device. The titles are the web app's.
static const struct { const char *title; int start, n; } SECTIONS[] = {
  { "Playback",                     SETTING_QUALITY,           5 },
  { "Home layout",                    SETTING_LANDSCAPE,           4 },
  { "Home content",               SETTING_RAIL,               13 },
  { "Continue watching",           SETTING_CW_ON,           7 },
  { "Detail page",             SETTING_DET_BLUR_NOT_WATCHED, 4 },
  { "Poster focus",                 SETTING_EXPAND,            3 },
  { "Depth effect",         SETTING_DEPTH,                9 },
  { "Item size",              SETTING_WIDTH_DP,          2 },
  { "Interface",                      SETTING_ANIM,                  1 },
  { "Account",                          SETTING_PROFILE_ACTIVE,        5 },
  { "About",                          SETTING_VERSION_I,            2 },
};
#define SETTING_N_SECTIONS (int)(sizeof SECTIONS / sizeof *SECTIONS)

// Each option's value. For OP_CHOICE it is the index; for OP_NUMBER it is the
// number itself. The defaults are those of layoutPreferences.js, with ONE
// exception noted line by line: the four the owner's profile diverges from the
// factory settings on are born as they left them, because that is what they see
// today. All of them are changeable here, which was the point.
static int value[SETTING_N] = {
  0, 0, 0,          /* quality, DV, Atmos */
  // AUTOMATIC and not "Off". Off is what the app did before this row existed —
  // nothing ever selected a subtitle, on any title — and it is the behaviour the
  // owner reported as "subtitles are not really a thing here". A default of Off
  // would ship the same complaint with a switch next to it.
  1,                /* subtitles: automatic (English) */
  0,                /* seek bar colour: violet */

  0,                /* landscape posters: ON (the owner's profile; factory: off) */
  0,                /* full-screen backdrop: ON (profile; factory: off) */
  0,                /* backdrop area: the whole screen, which is what it did before */
  NV_HERO_FIT_PCT_DEFAULT, /* backdrop size, % of the screen's width */

  0,                /* sidebar: collapsed (profile; factory: fixed) */
  1,                /* modern sidebar: off */
  0,                /* modern bar blur: on (profile) */
  0,                /* show hero: on */
  0,                /* hero catalogues: read-only */
  0,                /* discover location: in search */
  // OFF by default: the poster already carries the title printed on the art, and
  // repeating the name just below is the same information twice taking up row
  // height. It is still a setting — anyone who wants the label turns it on in Settings.
  1,                /* poster labels: off */
  0,                /* addon name: on */
  0,                /* content type: on */
  1,                /* hide unreleased: off */
  0,                /* overall ratings: show (SHOW_ALL) */
  1,                /* classic focus gradient: off */
  0,                /* "Among friends" row: on */

  0,                /* continue watching: on */
  0,                /* style: card */
  0,                /* episode thumbnail: on */
  1,                /* blur next up: off */
  0,                /* next from the furthest episode: on */
  0,                /* show unaired: on */
  0,                /* sort order: default */

  1,                /* blur unwatched: off */
  0,                /* trailer button: on */
  0,                /* external metadata: on */
  0,                /* full date: on */

  0,                /* expand poster on focus: on (the web's DEFAULT) */
  3,                /* delay: 3s */
  1,                /* fast horizontal navigation: off (factory) */

  1,                /* depth effect: off (factory) */
  28,               /* edge brightness */
  10,               /* sheen */
  0,                /* edge coverage */
  0, 0, 0, 0, 0,    /* depth on posters, cw, episodes, cast, trailers */

  126,              /* item width, dp (factory; the owner's profile uses 120) */
  12,               /* corner rounding, dp */

  0, 0,             /* language, animations */
  0, 0,             /* version, space */
};

static int focusOp = 0;
// A ONE-column list does not need focus.h: the column memory it exists to solve
// has nothing to remember here, and the raw index lets "skip the section header"
// be an addition instead of a row map.
static float animFocus[SETTING_N];
static float scrollY = 0.0f;
static int wantsExit = 0;

// How many catalogues the hero uses. 0 = all, which is what the web app writes as
// "All" when heroCatalogKeys is empty — and it is the owner's profile's case.
static int heroCatalogs = 0;

static int on(int op)  { return value[op] == 0; }

int settings_animations_reduced(void) { return value[SETTING_ANIM] == 1; }
int settings_dolby_vision(void)        { return on(SETTING_DV); }
int settings_dolby_atmos(void)         { return on(SETTING_ATMOS); }
int settings_subtitle_pref(void)       { return value[SETTING_SUBS]; }
void settings_seek_color(float *r, float *g, float *b) {
  int i = value[SETTING_SEEK_COLOR];
  if (i < 0 || i >= (int)(sizeof SEEK_RGB / sizeof *SEEK_RGB)) i = 0;
  *r = SEEK_RGB[i][0]; *g = SEEK_RGB[i][1]; *b = SEEK_RGB[i][2];
}

// `collapseSidebar: modernSidebar ? false : Boolean(collapseSidebar)` — the modern
// bar TURNS OFF the collapsing, and not the other way round. Copied from
// normalizeLayoutPreferences so as not to invent a precedence.
int settings_rail_modern(void)        { return on(SETTING_RAIL_MODERN); }
int settings_rail_collapsed(void)      { return settings_rail_modern() ? 0 : on(SETTING_RAIL); }
int settings_rail_modern_blur(void)   { return on(SETTING_RAIL_BLUR); }
int settings_hero_on(void)         { return on(SETTING_HERO); }
int settings_hero_full(void)          { return on(SETTING_HERO_FULL); }
// Only ever true WITH the full-screen backdrop on: it is that backdrop's shape,
// not a third layout. With the banded hero the row has nothing to say, and
// answering 1 there would shrink a band that is already a band.
int settings_hero_top_band(void) {
  return settings_hero_full() && value[SETTING_HERO_AREA] == 1;
}
// The band's width as a fraction of the screen's. The height follows the ART'S
// aspect, so this one number is the whole size.
float settings_hero_band_scale(void) {
  return (float)value[SETTING_HERO_BAND] / 100.0f;
}
int settings_posters_landscape(void)   { return on(SETTING_LANDSCAPE); }
int settings_social_row(void)          { return on(SETTING_SOCIAL); }
int settings_gradient_focus_classic(void) { return on(SETTING_GRADIENT_CLASSIC); }

int settings_labels_poster(void)      { return on(SETTING_LABELS); }
int settings_name_addon(void)          { return on(SETTING_NAME_ADDON); }
int settings_suffix_kind(void)         { return on(SETTING_SUFFIX_KIND); }
int settings_hide_unreleased(void){ return on(SETTING_HIDE_UNRELEASED); }
int settings_date_full(void)       { return on(SETTING_DET_DATE_FULL); }
int settings_scores_home(void)          { return value[SETTING_SCORES_HOME] == 0; }
int settings_local_discover(void)     { return value[SETTING_DISCOVER]; }
int settings_discover_na_search(void)  { return value[SETTING_DISCOVER] == 0; }

int settings_cw_on(void)           { return on(SETTING_CW_ON); }
int settings_cw_style(void)           { return value[SETTING_CW_STYLE]; }
int settings_cw_thumb_episode(void)   { return on(SETTING_CW_THUMB); }
int settings_cw_blur_next(void) { return on(SETTING_CW_BLUR_NEXT); }
int settings_cw_do_episode_more_alto(void) { return on(SETTING_CW_FURTHEST); }
int settings_cw_show_unaired(void)  { return on(SETTING_CW_NOT_SHOWN); }
int settings_cw_order(void)            { return value[SETTING_CW_ORDER]; }

int settings_blur_unwatched(void) { return on(SETTING_DET_BLUR_NOT_WATCHED); }
int settings_button_trailer(void)       { return on(SETTING_DET_TRAILER); }
int settings_meta_external(void)        { return on(SETTING_DET_META_EXT); }

int   settings_expand_poster(void)   { return on(SETTING_EXPAND); }
float settings_expand_poster_delay(void) { return (float)value[SETTING_EXPAND_DELAY]; }
int   settings_navigation_horizontal_fast(void) { return on(SETTING_NAV_FAST); }

int   settings_depth(void)      { return on(SETTING_DEPTH); }
float settings_depth_border(void)     { return value[SETTING_DEPTH_BORDER] / 100.0f; }
float settings_depth_brightness(void)    { return value[SETTING_DEPTH_BRIGHTNESS] / 100.0f; }
float settings_depth_coverage(void) { return value[SETTING_DEPTH_COVERAGE] / 100.0f; }
int   settings_depth_posters(void)   { return on(SETTING_DEPTH_POSTERS); }
int   settings_depth_cw(void)        { return on(SETTING_DEPTH_CW); }
int   settings_depth_episodes(void) { return on(SETTING_DEPTH_EPS); }
int   settings_depth_cast(void)    { return on(SETTING_DEPTH_CAST); }
int   settings_depth_trailers(void)  { return on(SETTING_DEPTH_TRAILERS); }

int   settings_width_poster_dp(void) { return value[SETTING_WIDTH_DP]; }
int   settings_radius_poster_dp(void)    { return value[SETTING_RADIUS_DP]; }
// dpToPx = 2 em buildModernHomeSizingStyle. 12dp -> 24px, que e o raio medido.
float settings_radius_poster_px(void)    { return (float)value[SETTING_RADIUS_DP] * 2.0f; }

// The web app's rule, and not two layouts: the content always has a 104 inset and
// the rail adds its own 144 when it is fixed.
float settings_content_x(void) {
  return settings_rail_collapsed() ? NV_CONTENT_PAD
                                  : NV_LEGACY_RAIL_W + NV_CONTENT_PAD;
}
const char *settings_quality(void)   { return V_QUALITY[value[SETTING_QUALITY]]; }

// Where the settings live. Until the previous version nothing was written:
// changing an option held only while the app was open, and coming back afterwards
// showed everything at its default — which makes the whole screen look decorative.
static char dirSettings[512];


// The LITERAL values the web app writes for the options that are not boolean. The
// order matches, one for one, that of the corresponding label array — and that
// correspondence is the contract: touching one array without touching the other
// changes the person's setting silently. All checked against the web app's code.
static const char *W_DISCOVER[] = { "in_search", "in_sidebar", "off", NULL };
static const char *W_SCORES[]     = { "SHOW_ALL", "HIDE_ALL", NULL };
static const char *W_CW[]        = { "card", "wide", "poster", NULL };
static const char *W_CW_ORDER[]  = { "default", "streaming_style", "split_upcoming", NULL };

// `heroSectionEnabled` -> `hero_section_enabled`. A run of capitals counts as a
// single word (`homeImdbRatingsVisibility` -> `home_imdb_ratings_visibility`, and
// not `home_i_m_d_b_...`).
static void camelToSnake(const char *src, char *dst, size_t size) {
  size_t w = 0;
  int i;
  for (i = 0; src[i] && w + 2 < size; i++) {
    int alto = src[i] >= 'A' && src[i] <= 'Z';
    if (alto && w > 0) {
      int previousBottom = src[i - 1] >= 'a' && src[i - 1] <= 'z';
      int previousDigit = src[i - 1] >= '0' && src[i - 1] <= '9';
      int nextBottom = src[i + 1] >= 'a' && src[i + 1] <= 'z';
      if (previousBottom || previousDigit || nextBottom) dst[w++] = '_';
    }
    dst[w++] = alto ? (char)(src[i] - 'A' + 'a') : src[i];
  }
  dst[w] = 0;
}

static int equalWithoutBox(const char *a, const char *b) {
  for (; *a && *b; a++, b++) {
    char x = (*a >= 'A' && *a <= 'Z') ? (char)(*a - 'A' + 'a') : *a;
    char y = (*b >= 'A' && *b <= 'Z') ? (char)(*b - 'A' + 'a') : *b;
    if (x != y) return 1;
  }
  return *a || *b;   // 0 when equal, like strcmp
}

static const char *const *literalsOf(int op) {
  switch (op) {
    case SETTING_DISCOVER:  return W_DISCOVER;
    case SETTING_SCORES_HOME: return W_SCORES;
    case SETTING_CW_STYLE:  return W_CW;
    case SETTING_CW_ORDER:   return W_CW_ORDER;
    default:            return NULL;
  }
}

static int limits(int op, int v) {
  const Option *o = &OPTIONS[op];
  if (o->kind == OP_CHOICE) return (v >= 0 && v < o->n) ? v : value[op];
  if (o->kind == OP_NUMBER)  return v < o->min ? o->min : (v > o->max ? o->max : v);
  return value[op];
}

void settings_dir(const char *dir) {
  FILE *f;
  char path[600], line[96];
  if (!dir || !*dir) return;
  snprintf(dirSettings, sizeof dirSettings, "%s", dir);
  snprintf(path, sizeof path, "%s/settings.txt", dirSettings);
  f = fopen(path, "r");
  if (!f) return;
  while (fgets(line, sizeof line, f)) {
    char key[64]; int v, i;
    if (sscanf(line, "%63s %d", key, &v) != 2) continue;
    for (i = 0; i < SETTING_N; i++) {
      if (!KEY[i] || strcmp(KEY[i], key)) continue;
      if (OPTIONS[i].kind == OP_READ || OPTIONS[i].kind == OP_ACTION) continue;
      // A value outside the range (a file from another version, or hand-edited)
      // falls back to the default instead of indexing outside the array.
      value[i] = limits(i, v);
      break;
    }
  }
  fclose(f);
}

static void save(void) {
  char path[600], tmp[600];
  FILE *f;
  int i;
  if (!dirSettings[0]) return;
  snprintf(path, sizeof path, "%s/settings.txt", dirSettings);
  snprintf(tmp, sizeof tmp, "%s/settings.tmp", dirSettings);
  f = fopen(tmp, "w");
  if (!f) return;
  for (i = 0; i < SETTING_N; i++) {
    // "-" marks a local row (version, space, account): it has no value to store.
    // An action does not either. And a missing key NEVER goes to the file — it was
    // a "(null) 0" written that way that brought the app down on the next read.
    if (!KEY[i] || KEY[i][0] == '-') continue;
    if (OPTIONS[i].kind == OP_READ || OPTIONS[i].kind == OP_ACTION) continue;
    fprintf(f, "%s %d\n", KEY[i], value[i]);
  }
  fclose(f);
  rename(tmp, path);
}


int settings_apply_blob(const char *json) {
  const char *end;
  int i, changed = 0, recognized = 0;
  if (!json || !*json) return 0;
  end = json + strlen(json);

  for (i = 0; i < SETTING_N; i++) {
    char snake[80], wrapper[400], raw[160];
    int new;
    // A read-only/action row has no value; a key with "-" is a local marker
    // (heroCatalogKeys, version, space) and does not come from the blob.
    if (OPTIONS[i].kind == OP_READ || OPTIONS[i].kind == OP_ACTION) continue;
    if (!KEY[i] || KEY[i][0] == '-') continue;
    // MEASURED on the TV, with a real account: the blob is NOT a flat camelCase
    // map. It is
    //   {"version":1,"features":{"layout_settings":{
    //      "hero_section_enabled":{"type":"boolean","value":true}, ...}}}
    // — the key in snake_case, nested by "feature", and the value WRAPPED in an
    // object with a type. Searching for `heroSectionEnabled` the app found zero
    // keys in 12 KB of settings and applied nothing, with no error at all.
    //
    // The search by name ignores the nesting on purpose: js_raw sweeps the whole
    // text, and these keys' names are unique in the document.
    camelToSnake(KEY[i], snake, sizeof snake);
    if (!js_raw(json, end, snake, wrapper, sizeof wrapper) &&
        !js_raw(json, end, KEY[i], wrapper, sizeof wrapper)) continue;
    if (wrapper[0] == '{') {
      // Unwraps {"type":...,"value":X}. The `type` comes before the `value` in the
      // web app's encoder, so the first "value" key is the right one.
      if (!js_raw(wrapper, wrapper + strlen(wrapper), "value",
                    raw, sizeof raw)) continue;
    } else {
      snprintf(raw, sizeof raw, "%s", wrapper);
    }

    if (!strcmp(raw, "true") || !strcmp(raw, "false")) {
      // V_ON's first label is "On" and V_RAIL's is "Collapsed" — in both, index 0
      // is the web's `true`. A useful coincidence, but a coincidence: if a new
      // array starts with the off state, it needs literals of its own in literalsOf().
      new = !strcmp(raw, "true") ? 0 : 1;
    } else if (raw[0] == '"') {
      const char *const *lit = literalsOf(i);
      char text[128];
      size_t n = strlen(raw);
      if (n < 2) continue;
      if (n - 2 >= sizeof text) continue;
      memcpy(text, raw + 1, n - 2);
      text[n - 2] = 0;
      new = -1;
      // A CASE-INSENSITIVE comparison. MEASURED on the TV: the server stores these
      // enums in UPPER CASE ("IN_SEARCH", "CARD", "DEFAULT") while the web app's JS
      // writes them in lower case. Reading only the web app's code led to rejecting
      // the real value — and the rejection was CORRECT (better to keep than to
      // invent), but the effect was the setting never arriving.
      if (lit) { int k; for (k = 0; lit[k]; k++) if (!equalWithoutBox(lit[k], text)) { new = k; break; } }
      if (new < 0) {
        // A value this app does not know (a newer version of the web app, a new
        // option). Keeping what is there is the right answer: choosing a default
        // here would invent a preference the person never set.
        printf("[settings] %s=\"%s\" not recognised; kept\n", KEY[i], text);
        continue;
      }
    } else if ((raw[0] >= '0' && raw[0] <= '9') || raw[0] == '-' || raw[0] == '.') {
      new = (int)(atof(raw) + 0.5);
    } else {
      continue;   // null, object, array: there is nothing to apply
    }

    recognized++;
    new = limits(i, new);
    if (new != value[i]) { value[i] = new; changed++; }
  }

  if (changed) save();   // what came from the account has to survive a restart
  // ALWAYS log it, zero included. "No line in the log" has two opposite readings —
  // the blob was not applied, or it was applied and everything was already the
  // same — and without the number there is no way to tell which. That was exactly
  // the doubt left over from the first check on the TV.
  printf("[settings] account blob: %d key(s) recognised, %d changed\n",
         recognized, changed);
  return changed;
}

int settings_start(void) { focusOp = 0; scrollY = 0.0f; wantsExit = 0; return 1; }
void settings_shutdown(void) { }
int settings_wants_exit(void) { return wantsExit; }

// The value of the read-only rows. The disk space is NOT an invented number: it
// comes from the texture cache, which is exactly what "images" consumes on the
// device — a fixed number here would be a lie and would never change.
static const char *textRead(int op) {
  static char buf[64];
  if (op == SETTING_VERSION_I) return SETTING_VERSION;
  if (op == SETTING_PROFILE_ACTIVE) {
    static char bufp[80];
    int i;
    for (i = 0; i < profiles_n(); i++)
      if (profiles_item(i)->index_ == profiles_active()) return profiles_item(i)->name;
    // With no profile list, saying "Profile 1" is more honest than leaving it
    // empty: it is literally what the app is using in p_profile_id.
    snprintf(bufp, sizeof bufp, "Profile %d", profiles_active());
    return bufp;
  }
  if (op == SETTING_SYNC) {
    switch (sync_state()) {
      case SYNC_RUNNING: return "syncing…";
      case SYNC_FAILED:  return "failed";
      case SYNC_READY:  return sync_summary();
      default:           return session_loggedin() ? "waiting" : "no account";
    }
  }
  if (op == SETTING_TRAKT) {
    switch (traktauth_state()) {
      case TRA_ON:     return "conectado";
      case TRA_REQUESTING:    return "preparando…";
      case TRA_WAITING: return "waiting";
      case TRA_ERROR:       return "failed";
      default:             return "connect";
    }
  }
  if (op == SETTING_SIMKL) {
    switch (simklauth_state()) {
      case SMK_ON:     return "conectado";
      case SMK_REQUESTING:    return "preparando…";
      case SMK_WAITING: return "waiting";
      case SMK_ERROR:       return "failed";
      default:             return "connect";
    }
  }
  if (op == SETTING_EXIT) return "OK";
  if (op == SETTING_HERO_CATALOGS) {
    // "All" with an empty list is what the web app writes (common_all), and it is
    // the owner's profile's state. A "0" there would read as "none", the opposite
    // of what it is.
    if (heroCatalogs <= 0) return "All";
    snprintf(buf, sizeof buf, "%d", heroCatalogs);
    return buf;
  }
  int items = 0, pending = 0; long bytes = 0;
  tex_stats(&items, &pending, &bytes);
  snprintf(buf, sizeof buf, "%.1f MB across %d images", bytes / 1048576.0, items);
  return buf;
}

// An option may be INACTIVE because of another — the web app hides the row
// (`model.layout.modernSidebar ? "" : renderToggleRow(...)`), but hiding on a
// D-pad changes the number of rows under the user's finger with every press. Here
// it stays in place, muted and without arrows: the dependency stays visible
// instead of the row vanishing.
static int inactive(int op) {
  switch (op) {
    case SETTING_RAIL:         return settings_rail_modern();
    case SETTING_RAIL_BLUR:    return !settings_rail_modern();
    case SETTING_HERO_CATALOGS: return !settings_hero_on();
    case SETTING_CW_STYLE: case SETTING_CW_THUMB: case SETTING_CW_FURTHEST:
    case SETTING_CW_NOT_SHOWN: case SETTING_CW_ORDER:
      return !settings_cw_on();
    case SETTING_CW_BLUR_NEXT: return !settings_cw_on() || !settings_cw_thumb_episode();
    case SETTING_EXPAND_DELAY: return !settings_expand_poster();
    case SETTING_DEPTH_BORDER: case SETTING_DEPTH_BRIGHTNESS: case SETTING_DEPTH_COVERAGE:
    case SETTING_DEPTH_POSTERS: case SETTING_DEPTH_CW: case SETTING_DEPTH_EPS:
    case SETTING_DEPTH_CAST: case SETTING_DEPTH_TRAILERS:
      return !settings_depth();
    default: return 0;
  }
}

// An action is NOT read-only (it has the active row's highlight), but it is NOT
// mutable either (left/right do nothing on it). The two answers are deliberately
// different, and that is why they are two functions.
static int readOnly(int op) { return OPTIONS[op].kind == OP_READ; }
static int mutable(int op)   { return OPTIONS[op].kind != OP_READ &&
                                      OPTIONS[op].kind != OP_ACTION && !inactive(op); }

static int sectionCurrent(void) {
  for (int s = 0; s < SETTING_N_SECTIONS; s++)
    if (focusOp < SECTIONS[s].start + SECTIONS[s].n) return s;
  return SETTING_N_SECTIONS - 1;
}

static const char *helpOption(int op) {
  if (inactive(op)) {
    if (op == SETTING_RAIL) return "Turn off the modern sidebar to choose between collapsed and fixed.";
    if (op == SETTING_RAIL_BLUR) return "Turn on the modern sidebar to use the blur.";
    if (op == SETTING_HERO_CATALOGS) return "Turn on Show hero to display catalogues at the top of Home.";
    if (op >= SETTING_CW_STYLE && op <= SETTING_CW_ORDER)
      return op == SETTING_CW_BLUR_NEXT && settings_cw_on()
        ? "Turn on Episode thumbnail to blur the next episode image."
        : "Turn on Continue watching to adjust the resume cards.";
    if (op == SETTING_EXPAND_DELAY) return "Turn on Expand poster on focus to adjust the delay.";
    return "Turn on Depth effect to customise this detail.";
  }
  switch (op) {
    case SETTING_SEEK_COLOR: return "The colour of the player's progress bar and its playhead.";
    case SETTING_QUALITY: return "Sets the resolution preference. Availability depends on the addon sources.";
    case SETTING_DV: case SETTING_ATMOS: return "Preference for compatible sources. The available format also depends on the file and the TV.";
    case SETTING_HERO_CATALOGS: return "How many catalogues the hero includes. This row is informational only.";
    case SETTING_CW_FURTHEST: return "Picks the next episode from the furthest one marked as watched.";
    case SETTING_CW_BLUR_NEXT: case SETTING_DET_BLUR_NOT_WATCHED: return "Hides thumbnail detail to avoid spoilers for episodes you have not watched.";
    case SETTING_ANIM: return "Use Reduced for subtler motion when moving through the interface.";
    case SETTING_SPACE: return "Current memory used by the image cache, not space taken on the TV storage.";
    case SETTING_VERSION_I: return "Application version. This information cannot be changed.";
    case SETTING_WIDTH_DP: return "Sets the poster width on rows that use the customisable size.";
    case SETTING_RADIUS_DP: return "Controls how rounded the poster corners are.";
    default: return "Use the left and right arrows to choose. The preference applies as the value changes.";
  }
}

// The vertical offset from the top of the list to row `op`, counting the headers
// of the sections that came before.
static float yOfOption(int op) {
  float y = 0.0f;
  for (int s = 0; s < SETTING_N_SECTIONS; s++) {
    y += (s ? SETTING_SEC_GAP : 0.0f) + SETTING_SEC_HEADER;
    for (int k = 0; k < SECTIONS[s].n; k++) {
      int o = SECTIONS[s].start + k;
      if (o == op) return y;
      y += SETTING_LINE_H + SETTING_LINE_GAP;
    }
  }
  return y;
}

void settings_event(const SDL_Event *e) {
  if (e->type != SDL_KEYDOWN) return;
  SDL_Keycode k = e->key.keysym.sym;

  // A link in progress is a question: while it is standing, nothing else on the
  // screen answers the remote.
  { TraState ta = traktauth_state();
    SmkState sa = simklauth_state();
    int traActive = (ta == TRA_REQUESTING || ta == TRA_WAITING || ta == TRA_ERROR);
    int smkActive = (sa == SMK_REQUESTING || sa == SMK_WAITING || sa == SMK_ERROR);
    if (traActive || smkActive) {
      if (k == SDLK_ESCAPE || k == SDLK_AC_BACK || k == SDLK_BACKSPACE) {
        if (traActive) traktauth_cancel(); else simklauth_cancel();
      } else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
        // OK only repeats the request when it errored; with the code on screen it
        // deliberately does nothing, so as not to swap out the code the person has
        // just typed on their phone.
        if (traActive && ta == TRA_ERROR) traktauth_begin();
        else if (smkActive && sa == SMK_ERROR) simklauth_begin();
      }
      return;
    } }
  if (k == SDLK_ESCAPE || k == SDLK_AC_BACK || k == SDLK_BACKSPACE ||
      k == SDLK_DELETE) { wantsExit = 1; return; }

  if (k == SDLK_DOWN)      { if (focusOp < SETTING_N - 1) focusOp++; }
  else if (k == SDLK_UP)   { if (focusOp > 0)        focusOp--; }
  else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
    if (OPTIONS[focusOp].kind != OP_ACTION) return;
    if (focusOp == SETTING_TRAKT) { traktauth_begin(); return; }
    if (focusOp == SETTING_SIMKL) { simklauth_begin(); return; }
    if (focusOp == SETTING_EXIT) {
      // Signing out erases the session from disk. Deliberately without
      // confirmation: the cost of signing out by accident is one QR login, and a
      // confirmation box in this list would need a modal the screen does not have.
      session_exit();
      traktauth_forget();
      simklauth_forget();
      // The session alone is not enough: addons, Trakt, profile and progress would
      // be left for the next person. See the header of sync_forget_user.
      sync_forget_user();
      wantsExit = 1;   // back to the home, which falls into login on the next frame
    }
  }
  else if (k == SDLK_PAGEUP || k == SDLK_PAGEDOWN) {
    int s = sectionCurrent() + (k == SDLK_PAGEDOWN ? 1 : -1);
    if (s >= 0 && s < SETTING_N_SECTIONS) focusOp = SECTIONS[s].start;
  }
  else if (k == SDLK_LEFT || k == SDLK_RIGHT) {
    // A read-only item, or one switched off by its dependency, changes with nothing.
    if (!mutable(focusOp)) return;
    const Option *o = &OPTIONS[focusOp];
    int dir = (k == SDLK_RIGHT) ? 1 : -1;
    if (o->kind == OP_NUMBER) {
      // A number does NOT wrap: going from 100% to 0% with one extra press is a
      // jump nobody asks for, and on the TV remote the arrow repeats by itself.
      int v = value[focusOp] + dir * o->step;
      value[focusOp] = limits(focusOp, v);
    } else {
      // A choice does wrap: the list is short and going from the end back to the
      // start saves presses on the remote. Without wrapping, the last value is a
      // dead end.
      value[focusOp] = (value[focusOp] + (dir > 0 ? 1 : o->n - 1)) % o->n;
    }
    save();   // saves on every change: there is no "save" button on this screen
  }
}

void settings_update(float dt, Uint32 now) {
  (void)now;
  for (int i = 0; i < SETTING_N; i++) {
    float target = (i == focusOp) ? 1.0f : 0.0f;
    animFocus[i] = settings_animations_reduced() ? target : anim_spring(animFocus[i], target, dt,
                            target > animFocus[i] ? NV_SPRING_FOCUS : NV_SPRING_BLUR);
  }
  // Scrolls the minimum for the focused row to fit, and brings the section header
  // along when the row is its first — without that, entering a section shows the
  // option without saying which group it belongs to.
  float top = yOfOption(focusOp);
  for (int s = 0; s < SETTING_N_SECTIONS; s++)
    if (SECTIONS[s].start == focusOp) { top -= SETTING_SEC_HEADER; break; }
  float base = yOfOption(focusOp) + SETTING_LINE_H;
  float target = scrollY;
  if (base - target > SETTING_BASE - SETTING_TOP) target = base - (SETTING_BASE - SETTING_TOP);
  if (top - target < 0.0f)              target = top;
  if (target < 0.0f) target = 0.0f;
  scrollY = settings_animations_reduced() ? target : anim_spring(scrollY, target, dt, NV_SPRING_SCROLL);
}

// The text of a row's value. A static buffer because only one row is drawn at a
// time inside drawLine.
static const char *textValue(int op) {
  static char buf[48];
  const Option *o = &OPTIONS[op];
  if (o->kind == OP_READ || o->kind == OP_ACTION) return textRead(op);
  if (o->kind == OP_NUMBER) {
    snprintf(buf, sizeof buf, "%d%s", value[op], o->suffix ? o->suffix : "");
    return buf;
  }
  return o->values[value[op]];
}

static void drawLine(int op, float y, float f) {
  if (y + SETTING_LINE_H < SETTING_TOP - 40.0f || y > SETTING_BASE + 40.0f) return;
  // It disappears before crossing the screen's title, like the detail page's
  // sections: text passing under text reads as a blur.
  float a = anim_clamp((y - (SETTING_TOP - 70.0f)) / 60.0f, 0.0f, 1.0f);
  if (a <= 0.005f) return;

  int off = inactive(op);
  int canChange = mutable(op);
  GfxRect line = { SETTING_LIST_X, y, SETTING_LIST_W, SETTING_LINE_H };
  // The same vocabulary as the menu: a dark surface, light text and explicit focus.
  gfx_color(line, SETTING_RADIUS, NV_COLOR_FOCUS_R, NV_COLOR_FOCUS_G, NV_COLOR_FOCUS_B,
          (0.34f + 0.66f * f) * a);
  if (op == focusOp)
    gfx_rect(line, 0, GFX_RING, 0, NV_RING_FOCUS / SETTING_LINE_H, 0,
             SETTING_RADIUS, 0.96f, 0.96f, 0.97f, a);

  // An inactive row is visibly more muted THAN a read-only one: read-only is
  // information, inactive is "this exists but depends on something else".
  float aText = a * (off ? 0.65f : 1.0f);
  int cr = canChange ? 240 : 192;
  TxtLine rot = txt_line_trim(TXT_CALLOUT, OPTIONS[op].label,
                                cr, cr, cr, 255, SETTING_LIST_W - 420.0f);
  txt_draw_alpha(rot, SETTING_LIST_X + SETTING_PAD,
                     y + (SETTING_LINE_H - rot.h) * 0.5f, aText);

  const char *v = textValue(op);
  int cv = canChange ? 220 : 176;
  TxtLine val = txt_line_trim(TXT_CALLOUT, v, cv, cv, cv, 255, 310.0f);
  float xDir = SETTING_LIST_X + SETTING_LIST_W - SETTING_PAD;
  float valueDir = xDir - 36.0f;
  float vy = y + (SETTING_LINE_H - val.h) * 0.5f;

  // The numeric row's fill bar. Without it, "28%" says nothing about where 28 sits
  // in the range — and the web app shows a slider for exactly that reason.
  if (OPTIONS[op].kind == OP_NUMBER) {
    const Option *o = &OPTIONS[op];
    float t = (o->max > o->min)
            ? (float)(value[op] - o->min) / (float)(o->max - o->min) : 0.0f;
    float bw = 220.0f, bh = 4.0f;
    float bx = valueDir - bw;
    float by = y + SETTING_LINE_H - 17.0f;
    vy -= 8.0f;
    GfxRect rail = { bx, by, bw, bh };
    GfxRect full  = { bx, by, bw * anim_clamp(t, 0.0f, 1.0f), bh };
    gfx_color(rail, 0.5f, 0.94f, 0.94f, 0.96f, 0.22f * aText);
    if (full.w > 0.5f)
      gfx_color(full, 0.5f, 0.94f, 0.94f, 0.96f, 0.92f * aText);
  }

  // The arrows only appear on the focused row that CHANGES. They are the
  // instruction: without them, nothing on screen says left/right is the right gesture.
  if (canChange && f > 0.02f) {
    TxtLine dir = txt_line(TXT_CAPTION2, "\xe2\x96\xb6", cv, cv, cv, 255);
    TxtLine left = txt_line(TXT_CAPTION2, "\xe2\x97\x80", cv, cv, cv, 255);
    txt_draw_alpha(dir, xDir - dir.w, y + (SETTING_LINE_H - dir.h) * 0.5f, aText * f);
    txt_draw_alpha(left, valueDir - val.w - 16.0f - left.w,
                       y + (SETTING_LINE_H - left.h) * 0.5f, aText * f);
  }
  txt_draw_alpha(val, valueDir - val.w, vy, aText);
}

// Where the QR should point: the address, with the code IN THE PATH when the
// service accepts it there. That is the difference between a symbol that saves
// typing the address and one that finishes the job — scan it and the activation
// page comes up with the code already in it.
//
// Trakt accepts it. VERIFIED against the live site: GET /activate/ABCD1234
// answers 302 to /signin?callbackURL=%2Factivate%3Fuser_code%3DABCD1234, so the
// code survives even the sign-in detour. The web app builds the same URL
// (NuvioWeb settingsScreen.js:959).
//
// Simkl does NOT get the same treatment. Its /pin answers 403 to anything that is
// not a browser, so whether it takes a code in the path could not be checked, and
// a symbol that leads to the wrong page is worse than one that only carries the
// address.
static void qrTargetOf(char *output, size_t n, const char *address,
                       const char *code, int codeInPath) {
  size_t len;
  const char *p;
  snprintf(output, n, "%s", address && address[0] ? address : "");
  if (!codeInPath || !code || !code[0] || !output[0]) return;
  len = strlen(output);
  while (len && output[len - 1] == '/') output[--len] = 0;
  // Trakt's user_code is A-Z0-9. Anything else is not something to paste into a
  // URL unescaped, so the symbol falls back to the address alone.
  for (p = code; *p; p++)
    if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
          (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')) return;
  snprintf(output + len, n - len, "/%s", code);
}

// The link overlay (Trakt or Simkl).
//
// IT DOES GET A QR. The argument against one used to be that these codes are
// SHORT — 8 characters on Trakt — unlike the 32 hexadecimal digits of the
// account login, so they can be read off the TV and typed. That was the wrong
// half of the problem. With the code in the path (see qrTargetOf) scanning is the
// WHOLE flow: no address to type, no code to transcribe. The code stays on screen
// in large type for whoever would rather type it. Same treatment as the login
// screen, one texture, cached by qr.c.
static void drawLink(const char *service, const char *code,
                           const char *address, const char *qrTarget,
                           const char *failure, int waiting) {
  GfxRect screen = { 0, 0, NV_SCREEN_W, NV_SCREEN_H };
  GfxRect card = { (NV_SCREEN_W - 1000.0f) * 0.5f, 250.0f, 1000.0f, 560.0f };
  TxtLine l;
  char t[80];
  float y = 300.0f;
  // An ALMOST opaque veil plus a solid card behind the block. With a 0.80 veil and
  // no card, the Settings rows showed through the text — "waiting" landed on top
  // of "Trakt" and "connect" on top of "and enter the code". A code the person has
  // to transcribe cannot compete with background text.
  gfx_color(screen, 0.0f, 0.0f, 0.0f, 0.0f, 0.92f);
  gfx_color(card, 0.045f, NV_COLOR_BACKGROUND_R, NV_COLOR_BACKGROUND_G, NV_COLOR_BACKGROUND_B, 1.0f);

  snprintf(t, sizeof t, "Connect %s", service);
  l = txt_line(TXT_TITLE2, t, 255, 255, 255, 255);
  txt_draw(l, (NV_SCREEN_W - l.w) * 0.5f, y);
  y += 92.0f;

  if (failure && failure[0]) {
    l = txt_line(TXT_HEADLINE, failure, 236, 108, 108, 255);
    txt_draw(l, (NV_SCREEN_W - l.w) * 0.5f, y);
    y += 70.0f;
    l = txt_line(TXT_CAPTION, "OK to try again · Back to close",
                  150, 152, 160, 255);
    txt_draw(l, (NV_SCREEN_W - l.w) * 0.5f, y);
    return;
  }
  if (!code || !code[0]) {
    l = txt_line(TXT_HEADLINE, "Preparing the code…", 210, 212, 220, 255);
    txt_draw(l, (NV_SCREEN_W - l.w) * 0.5f, y);
    return;
  }

  // Two columns: the symbol on the left, the steps on the right. Stacking them
  // would not fit — the card is 560 tall and the QR alone wants 300 of it.
  { GLuint tex = qr_texture(qrTarget && qrTarget[0] ? qrTarget : "");
    float qrSide = 300.0f;
    float qrX = card.x + 72.0f;
    float qrY = y + 24.0f;
    float textX = qrX + qrSide + 72.0f;
    float ty = y;

    if (tex) {
      // A light frame slightly larger than the symbol: against the card's dark
      // background the texture's own quiet zone would already be enough, but the
      // rounded frame makes the block read as a card.
      GfxRect frame = { qrX - 16.0f, qrY - 16.0f, qrSide + 32.0f, qrSide + 32.0f };
      GfxRect r = { qrX, qrY, qrSide, qrSide };
      gfx_color(frame, 0.06f, 1.0f, 1.0f, 1.0f, 1.0f);
      gfx_tex_aspect_current = 0.0f;   // 1:1, sem recorte
      gfx_rect(r, tex, GFX_SNAP, 0, 0.0f, 0.0f, 0.0f, 0, 0, 0, 1.0f);
    } else {
      // No symbol is not an error worth a red line — the address below is still
      // the whole instruction, and it is short.
      textX = card.x + 72.0f;
    }

    l = txt_line(TXT_BODY, tex ? "Scan with your phone, or open:"
                                : "On your phone, open:", 176, 178, 186, 255);
    txt_draw(l, textX, ty);
    ty += 52.0f;
    l = txt_line(TXT_TITLE3, address && address[0] ? address : "-", 255, 255, 255, 255);
    txt_draw(l, textX, ty);
    ty += 92.0f;
    l = txt_line(TXT_BODY, "and enter the code:", 176, 178, 186, 255);
    txt_draw(l, textX, ty);
    ty += 66.0f;

    // Letter spacing: a short code without tracking reads as a word, and the
    // person transcribes it wrongly.
    txt_tracking(TXT_TITLE1, code, 255, 255, 255, textX, ty, 1.0f, 16.0f);
    ty += 130.0f;

    if (waiting) {
      l = txt_line(TXT_CAPTION, "Waiting for authorisation…", 150, 152, 160, 255);
      txt_draw(l, textX, ty);
    }
  }
}

void settings_draw(Uint32 now) {
  (void)now;
  // An opaque background of its own: the screen covers everything and cannot
  // depend on whoever drew before it — without this the home shows between the
  // rows of the list.
  GfxRect screen = { 0, 0, NV_SCREEN_W, NV_SCREEN_H };
  // The screen has already been cleared with THIS VERY COLOUR by
  // glClearColor/glClear in main.c before app_draw. Painting over it was one
  // full-screen layer thrown away per frame — and the dominant cost on this GPU is
  // fill rate (gfx.c records that TWO full-screen layers dropped the Mali-G71 to
  // ~40fps). Do not put it back without first changing the clear colour.
  (void)screen;

  TxtLine title = txt_line(TXT_TITLE1, "Settings", 255, 255, 255, 255);
  txt_draw(title, SETTING_LIST_X, NV_MARGIN_Y);

  int sec = sectionCurrent();
  char pos[80];
  snprintf(pos, sizeof pos, "%s  ·  %d of %d", SECTIONS[sec].title,
           focusOp - SECTIONS[sec].start + 1, SECTIONS[sec].n);
  TxtLine context = txt_line(TXT_CAPTION, pos, 178, 180, 186, 255);
  txt_draw(context, SETTING_LIST_X, NV_MARGIN_Y + title.h + 10.0f);

  float hx = SETTING_LIST_X + SETTING_LIST_W + 52.0f;
  float hw = NV_SCREEN_W - NV_MARGIN_X - hx;
  if (hw > 240.0f) {
    TxtLine kind = txt_line(TXT_CAPTION, inactive(focusOp) ? "Option unavailable"
                        : readOnly(focusOp) ? "Information" : "Customise", 168, 171, 180, 255);
    txt_draw(kind, hx, SETTING_TOP + SETTING_SEC_HEADER);
    float hy = SETTING_TOP + SETTING_SEC_HEADER + kind.h + 22.0f;
    hy += txt_block(TXT_HEADLINE, OPTIONS[focusOp].label, 237, 238, 242,
                   hx, hy, hw, 40, 1, 3);
    hy += 22.0f;
    hy += txt_block(TXT_CAPTION, helpOption(focusOp), 183, 186, 194,
                   hx, hy, hw, 32, 1, 7);
    hy += 42.0f;
    txt_block(TXT_CAPTION, "↑ ↓  Navigate\n← →  Change value\nBack  Leave settings",
              155, 159, 169, hx, hy, hw, 34, 1, 4);
  }

  gfx_crop(SETTING_LIST_X - NV_RING_FOCUS, SETTING_TOP,
               SETTING_LIST_W + NV_RING_FOCUS * 2, SETTING_BASE - SETTING_TOP);
  float y = SETTING_TOP - scrollY;
  for (int s = 0; s < SETTING_N_SECTIONS; s++) {
    if (s) y += SETTING_SEC_GAP;
    // The section header in a small grey face: it labels the group, it does not
    // compete with the options' labels.
    float aC = anim_clamp((y - (SETTING_TOP - 70.0f)) / 60.0f, 0.0f, 1.0f);
    TxtLine ts = txt_line(TXT_CAPTION, SECTIONS[s].title, 150, 152, 160, 255);
    if (aC > 0.005f && y < SETTING_BASE)
      txt_draw_alpha(ts, SETTING_LIST_X + SETTING_PAD, y + SETTING_SEC_HEADER - ts.h - 10.0f, aC);
    y += SETTING_SEC_HEADER;
    for (int k = 0; k < SECTIONS[s].n; k++) {
      int op = SECTIONS[s].start + k;
      drawLine(op, y, animFocus[op]);
      y += SETTING_LINE_H + SETTING_LINE_GAP;
    }
  }
  gfx_no_crop();

  float total = yOfOption(SETTING_N - 1) + SETTING_LINE_H;
  float window = SETTING_BASE - SETTING_TOP;
  if (total > window) {
    float height = window * window / total;
    float sy = SETTING_TOP + (window - height) * anim_clamp(scrollY / (total - window), 0, 1);
    gfx_color((GfxRect){ SETTING_LIST_X + SETTING_LIST_W + 18, SETTING_TOP, 3, window },
            0.5f, 0.60f, 0.62f, 0.66f, 0.14f);
    gfx_color((GfxRect){ SETTING_LIST_X + SETTING_LIST_W + 18, sy, 3, height },
            0.5f, 0.80f, 0.82f, 0.86f, 0.8f);
  }
  TxtLine footer = txt_line(TXT_CAPTION, "PgUp / PgDn  Change section", 156, 159, 168, 255);
  txt_draw(footer, SETTING_LIST_X, SETTING_BASE + 20);

  // Above everything: while a link is in progress, it is the screen's question.
  { TraState ta = traktauth_state();
    SmkState sa = simklauth_state();
    char target[220];
    if (ta == TRA_REQUESTING || ta == TRA_WAITING || ta == TRA_ERROR) {
      qrTargetOf(target, sizeof target, traktauth_url(), traktauth_code(), 1);
      drawLink("Trakt", traktauth_code(), traktauth_url(), target,
                     traktauth_error(), ta == TRA_WAITING);
    } else if (sa == SMK_REQUESTING || sa == SMK_WAITING || sa == SMK_ERROR) {
      qrTargetOf(target, sizeof target, simklauth_url(), simklauth_code(), 0);
      drawLink("Simkl", simklauth_code(), simklauth_url(), target,
                     simklauth_error(), sa == SMK_WAITING);
    } }
}
