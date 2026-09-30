// Settings: a list of sections, each a button that opens that section's options —
// a vertical list, the label on the left and the value on the right.
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
#include "scrollbar.h"
#include "anim.h"
#include "layout.h"
#include "session.h"
#include "sync.h"
#include "profiles.h"
#include "qr.h"
#include "lang.h"
#include "traktauth.h"
#include "simklauth.h"
#include "js.h"
#include "homerows.h"
#include "pointer.h"
#include "app.h"
#include "appid.h"
#include "iptv.h"
#include <stdio.h>
#include <string.h>

#define SETTING_VERSION       NV_APP_VERSION

// The screen is a quiet list on the left and a preview panel on the right. Rows
// have no plate at rest — only a hairline between them — so the one focused row,
// lifted onto a plate with a white ring, is the only solid thing in the column.
#define SETTING_LINE_H       68.0f
// A group header ("PLAYBACK", "SIDEBAR"): a tracked kicker and a hairline, in the
// room above the group's first row. It is part of the list and scrolls with it.
#define SETTING_GROUP_H      56.0f
// The glyph before a section group's kicker on the list of sections.
#define SETTING_GROUP_ICON   20.0f
// Not a constant: it follows the rail, like all the rest of the content. With the
// bar collapsed the list also starts at 104 — leaving 248 hard-coded here made
// the Settings screen the only one misaligned with the others.
#define SETTING_LIST_X      settings_content_x()
#define SETTING_LIST_W      880.0f
#define SETTING_PAD           24.0f    // row edge to text
// The title is the same as Library's and Search's: TITLE3 with 1px tracking, at
// the same height. Inside a section the title becomes the path, "‹ Settings /
// Playback", on the same line; the list starts at the same height on both levels.
#define SETTING_TOP        (NV_DSC_Y + NV_DSC_TITLE_H + 52.0f)
// Where scrolling keeps the focused row above. The list itself is NOT cut here:
// it runs on to the bottom of the screen, like Library's grid.
#define SETTING_BASE        (NV_SCREEN_H - NV_MARGIN_Y)
// The row's radius as a fraction of the smaller side (the shader's SDF is
// normalised): 12px over a height of 68.
#define SETTING_RADIUS           0.18f
// The preview panel: its gap from the list and its widest.
#define SETTING_PANEL_GAP    80.0f
#define SETTING_PANEL_MAX_W 780.0f
// The brand violet, for what the preview points at.
#define SETTING_ACCENT_R  (0x83 / 255.0f)
#define SETTING_ACCENT_G  (0x67 / 255.0f)
#define SETTING_ACCENT_B  (0xF5 / 255.0f)

// The enum's order is the order the rows appear in: each section is a run of it
// (SECTIONS below), and the group headers inside a section fall where
// groupOfOption says. The tables are indexed by NAME ([SETTING_X] = ...) and the
// file is keyed (see settings_dir), so moving a row is moving its name here.
typedef enum {
  // Playback
  SETTING_QUALITY, SETTING_DV, SETTING_ATMOS,
  SETTING_AUDIO_LANG, SETTING_AUDIO_ANIME, SETTING_SUBS, SETTING_SUBS_FORCED,
  SETTING_NEXT_AUTOPLAY, SETTING_NEXT_COUNTDOWN, SETTING_NEXT_MODE, SETTING_NEXT_SECONDS,
  SETTING_NEXT_PERCENT,
  SETTING_SEEK_COLOR, SETTING_PAUSE_DELAY,
  // IPTV
  SETTING_LIVE_ON, SETTING_LIVE_SOURCE, SETTING_LIVE_BUFFER, SETTING_LIVE_PREVIEW,
  // Hero
  SETTING_HERO, SETTING_HERO_CATALOGS, SETTING_HERO_TRAILER, SETTING_HERO_TRAILER_DELAY,
  SETTING_HERO_TRAILER_CW,
  SETTING_HERO_FULL, SETTING_HERO_AREA, SETTING_HERO_BAND, SETTING_HERO_TRAILER_SIZE,
  // Catalogue rows
  SETTING_SUFFIX_KIND, SETTING_NAME_ADDON, SETTING_HIDE_UNRELEASED, SETTING_SCORES_HOME,
  // Continue watching. SETTING_CW_ON leads and SETTING_CW_NOT_SHOWN closes: the
  // checks for "a row that needs Continue watching on" read that range.
  SETTING_CW_ON, SETTING_CW_ORDER, SETTING_CW_PLAY,
  SETTING_CW_STYLE, SETTING_CW_LOGO, SETTING_CW_THUMB, SETTING_CW_BLUR_NEXT,
  SETTING_CW_FURTHEST, SETTING_CW_NOT_SHOWN,
  // Sidebar and navigation
  SETTING_RAIL, SETTING_RAIL_MODERN, SETTING_RAIL_BLUR, SETTING_DISCOVER,
  SETTING_NAV_FAST, SETTING_ROW_SCROLL,
  SETTING_ANIM,
  // Posters
  SETTING_LANDSCAPE, SETTING_LABELS,
  SETTING_EXPAND, SETTING_EXPAND_DELAY, SETTING_GRADIENT_CLASSIC,
  SETTING_WIDTH_DP, SETTING_RADIUS_DP,
  // Depth
  SETTING_DEPTH, SETTING_DEPTH_BORDER, SETTING_DEPTH_BRIGHTNESS, SETTING_DEPTH_COVERAGE,
  SETTING_DEPTH_POSTERS, SETTING_DEPTH_CW, SETTING_DEPTH_EPS, SETTING_DEPTH_CAST, SETTING_DEPTH_TRAILERS,
  // Detail page
  SETTING_DET_TRAILER, SETTING_DET_META_EXT, SETTING_DET_BLUR_NOT_WATCHED, SETTING_DET_DATE_FULL,
  // Account
  SETTING_PROFILE_ACTIVE, SETTING_PRIMARY_ADDONS, SETTING_SYNC, SETTING_TRAKT, SETTING_SIMKL, SETTING_EXIT,
  // About
  SETTING_VERSION_I, SETTING_SPACE,
  SETTING_N
} OptionId;

#define TRAILER_SAME 45   /* Trailer size's "Same as backdrop" stop */
static const char *V_QUALITY[] = { "Automatic", "4K", "1080p", "720p" };
// Which subtitle the player turns on BY ITSELF when a title starts: the file's
// own track in that language first, an addon's download after. "Automatic" is
// English — it was Portuguese-then-English, the app's first owner's order — and
// the named values pin one language with no fallback: whoever asks for English
// and is given Portuguese has been answered a question they did not ask.
// From "Portuguese" on, value k is lang.h's language k - 2 — the names below are
// that table's, in its order, and the count is checked against it at startup.
// Portuguese and English keep the values 2 and 3 they were stored as before.
static const char *V_SUBS[] = { "Off", "Automatic",
  "Portuguese", "English", "Spanish", "French", "German", "Italian", "Dutch",
  "Polish", "Swedish", "Danish", "Norwegian", "Finnish", "Russian", "Ukrainian",
  "Czech", "Hungarian", "Romanian", "Greek", "Turkish", "Arabic", "Hebrew",
  "Hindi", "Japanese", "Korean", "Chinese", "Thai", "Vietnamese", "Indonesian" };
#define N_SUBS (int)(sizeof V_SUBS / sizeof *V_SUBS)
// The AUDIO track the player switches to by itself when the file has one in
// this language. "File default" leaves the pipeline's own choice alone, which is
// what the app always did. Value k is lang.h's language k - 1.
static const char *V_AUDIO[] = { "File default",
  "Portuguese", "English", "Spanish", "French", "German", "Italian", "Dutch",
  "Polish", "Swedish", "Danish", "Norwegian", "Finnish", "Russian", "Ukrainian",
  "Czech", "Hungarian", "Romanian", "Greek", "Turkish", "Arabic", "Hebrew",
  "Hindi", "Japanese", "Korean", "Chinese", "Thai", "Vietnamese", "Indonesian" };
#define N_AUDIO (int)(sizeof V_AUDIO / sizeof *V_AUDIO)
// The same for ANIME, which a household often watches in Japanese while every
// other show plays dubbed. "Same as Audio language" defers to the row above;
// from "File default" on it is that row's list shifted by one, so value k is
// lang.h's language k - 2.
static const char *V_AUDIO_ANIME[] = { "Same as Audio language", "File default",
  "Portuguese", "English", "Spanish", "French", "German", "Italian", "Dutch",
  "Polish", "Swedish", "Danish", "Norwegian", "Finnish", "Russian", "Ukrainian",
  "Czech", "Hungarian", "Romanian", "Greek", "Turkish", "Arabic", "Hebrew",
  "Hindi", "Japanese", "Korean", "Chinese", "Thai", "Vietnamese", "Indonesian" };
#define N_AUDIO_ANIME (int)(sizeof V_AUDIO_ANIME / sizeof *V_AUDIO_ANIME)
typedef char subtitle_names_match_lang[(N_SUBS == LANG_COUNT + 2) ? 1 : -1];
typedef char audio_names_match_lang[(N_AUDIO == LANG_COUNT + 1) ? 1 : -1];
typedef char anime_audio_names_match_lang[(N_AUDIO_ANIME == LANG_COUNT + 2) ? 1 : -1];
// When the Up next card appears: a fixed lead before the end, or a share of the
// episode — the web app's nextEpisodeThresholdMode, same two modes, same default.
// The credits marker brings the card up earlier in either mode.
static const char *V_NEXT_MODE[] = { "Before the end", "Percentage watched" };
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
// Live TV's pause buffer, in minutes (settings_live_buffer_minutes).
static const char *V_LIVE_BUFFER[] = { "Off", "15 min", "30 min", "60 min" };
static const char *V_ANIM[]      = { "Full", "Reduced" };
// How a row follows the focus sideways. Local to this port: "Minimal" scrolls only
// as far as the focused card needs to fit; "First slot" puts it in the row's first
// slot on every step and slides the rest along.
static const char *V_ROW_SCROLL[] = { "Minimal", "First slot" };
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
  [SETTING_QUALITY] = ESC("Maximum quality",           V_QUALITY, 4),
  [SETTING_DV] = ESC("Dolby Vision",               V_ON, 2),
  [SETTING_ATMOS] = ESC("Dolby Atmos",                V_ON, 2),
  [SETTING_AUDIO_LANG] = ESC("Audio language",             V_AUDIO, N_AUDIO),
  [SETTING_AUDIO_ANIME] = ESC("Anime audio language",       V_AUDIO_ANIME, N_AUDIO_ANIME),
  [SETTING_SUBS] = ESC("Subtitles",                  V_SUBS, N_SUBS),
  [SETTING_SUBS_FORCED] = ESC("Forced subtitles",           V_ON, 2),
  [SETTING_SEEK_COLOR] = ESC("Seek bar colour",            V_SEEK, 6),
  [SETTING_NEXT_AUTOPLAY] = ESC("Autoplay next episode",      V_ON, 2),
  [SETTING_NEXT_COUNTDOWN] = NUM("Next episode countdown",     5, 30, 5, " s"),
  [SETTING_NEXT_MODE] = ESC("Up next appears",            V_NEXT_MODE, 2),
  [SETTING_NEXT_SECONDS] = NUM("Up next before the end",     0, 210, 30, " s"),
  [SETTING_NEXT_PERCENT] = NUM("Up next at",                 90, 100, 1, "%"),
  // 0 is Off: textValue draws it as a word, not as "0 s".
  [SETTING_PAUSE_DELAY] = NUM("Pause overlay after",        0, 60, 5, " s"),
  [SETTING_LIVE_ON] = ESC("Live TV",                       V_ON, 2),
  [SETTING_LIVE_SOURCE] = ACTION("IPTV source"),
  [SETTING_LIVE_BUFFER] = ESC("Pause buffer",               V_LIVE_BUFFER, 4),
  [SETTING_LIVE_PREVIEW] = ESC("Preview while browsing",    V_ON, 2),

  [SETTING_LANDSCAPE] = ESC("Landscape posters",       V_ON, 2),   // modernLandscapePostersEnabled
  [SETTING_HERO_FULL] = ESC("Full-screen backdrop",        V_ON, 2),   // modernHeroFullScreenBackdropEnabled
  [SETTING_HERO_AREA] = ESC("Backdrop area",               V_HERO_AREA, 2), // heroBackdropArea (local)
  // heroBackdropScale (local). A percentage of the SCREEN'S WIDTH, and it is a row
  // rather than a constant because it is judged by eye from the sofa: every value
  // tried in the source costs an ARM build and a deploy. Steps of 5 — 1% of 1920
  // is 19px and nobody is choosing between 1536 and 1555.
  [SETTING_HERO_BAND] = NUM("Backdrop size",               50, 100, 5, "%"),  // heroBackdropScale
  // Local. The trailer's own band in Top band, larger or smaller than the art's:
  // the video is cover-cropped, and the size that suits a still can show too much
  // or too little of a moving picture. The lowest stop, TRAILER_SAME, is "Same as
  // backdrop" — the default, so the trailer follows the Backdrop size row until
  // it is set on its own.
  [SETTING_HERO_TRAILER_SIZE] = NUM("Trailer size",  TRAILER_SAME, 100, 5, "%"),  // heroTrailerScale

  [SETTING_RAIL] = ESC("Sidebar",              V_RAIL, 2),   // collapseSidebar
  [SETTING_RAIL_MODERN] = ESC("Modern sidebar",      V_ON, 2),   // modernSidebar
  [SETTING_RAIL_BLUR] = ESC("Modern sidebar blur",  V_ON, 2),   // modernSidebarBlur
  [SETTING_HERO] = ESC("Show hero",           V_ON, 2),   // heroSectionEnabled
  [SETTING_HERO_CATALOGS] = READ("Hero catalogues"),                   // heroCatalogKeys (a count)
  [SETTING_HERO_TRAILER] = ESC("Trailer previews",    V_ON, 2),   // local: the hero plays the trailer
  // Local. How long the hero rests on a title before its trailer starts, in TENTHS
  // of a second (shown as "2.2 s"): whole seconds were too coarse around the
  // default, which was tuned by eye at 2.2.
  [SETTING_HERO_TRAILER_DELAY] = NUM("Trailer delay",  2, 100, 2, " s"),  // heroTrailerDelay
  [SETTING_HERO_TRAILER_CW] = ESC("Trailers on Continue watching", V_ON, 2), // local
  [SETTING_DISCOVER] = ESC("Discover location",         V_DISCOVER, 3), // discoverLocation
  [SETTING_LABELS] = ESC("Poster labels",       V_ON, 2),   // posterLabelsEnabled
  [SETTING_NAME_ADDON] = ESC("Addon name in the catalogue",  V_ON, 2),   // catalogAddonNameEnabled
  [SETTING_SUFFIX_KIND] = ESC("Content type",           V_ON, 2),   // catalogTypeSuffixEnabled
  [SETTING_HIDE_UNRELEASED] = ESC("Hide unreleased",       V_ON, 2),   // hideUnreleasedContent
  [SETTING_SCORES_HOME] = ESC("Overall ratings",          V_SCORES, 2),  // homeImdbRatingsVisibility
  [SETTING_GRADIENT_CLASSIC] = ESC("Classic focus gradient", V_ON, 2),   // classicFocusGradientEnabled

  [SETTING_CW_ON] = ESC("Show \"Continue watching\"", V_ON, 2), // continueWatchingEnabled
  [SETTING_CW_STYLE] = ESC("\"Continue watching\" style", V_CW, 3), // continueWatchingCardStyle
  [SETTING_CW_LOGO] = ESC("Show logo",              V_ON, 2),   // local: the title's logo for its name
  [SETTING_CW_PLAY] = ESC("Play on select",         V_ON, 2),   // local: OK plays, skipping the detail
  [SETTING_CW_THUMB] = ESC("Episode thumbnail",      V_ON, 2),   // useEpisodeThumbnailsInCw
  [SETTING_CW_BLUR_NEXT] = ESC("Blur next episode",  V_ON, 2),   // blurContinueWatchingNextUp
  [SETTING_CW_FURTHEST] = ESC("Next from the furthest episode", V_ON, 2),// nextUpFromFurthestEpisode
  [SETTING_CW_NOT_SHOWN] = ESC("Show unaired episodes", V_ON, 2),// showUnairedNextUp
  [SETTING_CW_ORDER] = ESC("Sort order",                  V_CW_ORDER, 3), // continueWatchingSortMode

  [SETTING_DET_BLUR_NOT_WATCHED] = ESC("Blur unwatched",    V_ON, 2),   // blurUnwatchedEpisodes
  [SETTING_DET_TRAILER] = ESC("Trailer button",           V_ON, 2),   // detailPageTrailerButtonEnabled
  [SETTING_DET_META_EXT] = ESC("Prefer external metadata", V_ON, 2), // preferExternalMetaAddonDetail
  [SETTING_DET_DATE_FULL] = ESC("Full release date", V_ON, 2),  // showFullReleaseDate

  [SETTING_EXPAND] = ESC("Expand poster on focus",   V_ON, 2),   // focusedPosterBackdropExpandEnabled
  [SETTING_EXPAND_DELAY] = NUM("Expansion delay",         0, 10, 1, " s"), // ...ExpandDelaySeconds
  [SETTING_NAV_FAST] = ESC("Fast horizontal navigation", V_ON, 2),  // fastHorizontalNavigationEnabled
  [SETTING_ROW_SCROLL] = ESC("Row scrolling",             V_ROW_SCROLL, 2), // local: rowScrollAnchor

  [SETTING_DEPTH] = ESC("Depth effect",     V_ON, 2),   // cardDepthEnabled
  [SETTING_DEPTH_BORDER] = NUM("Edge brightness",            0, 100, 2, "%"), // cardDepthEdgeStrength
  [SETTING_DEPTH_BRIGHTNESS] = NUM("Sheen",                      0, 100, 2, "%"), // cardDepthSheenStrength
  [SETTING_DEPTH_COVERAGE] = NUM("Edge coverage",         0, 100, 2, "%"), // cardDepthEdgeCoverage
  [SETTING_DEPTH_POSTERS] = ESC("Depth on posters",  V_ON, 2),
  [SETTING_DEPTH_CW] = ESC("Depth on \"Continue\"", V_ON, 2),
  [SETTING_DEPTH_EPS] = ESC("Depth on episodes", V_ON, 2),
  [SETTING_DEPTH_CAST] = ESC("Depth on cast",     V_ON, 2),
  [SETTING_DEPTH_TRAILERS] = ESC("Depth on trailers",  V_ON, 2),

  [SETTING_WIDTH_DP] = NUM("Item width",            72, 200, 2, " dp"), // posterCardWidthDp
  [SETTING_RADIUS_DP] = NUM("Corner radius",              0, 40, 1, " dp"),   // posterCardCornerRadiusDp

  [SETTING_ANIM] = ESC("Animations",                  V_ANIM, 2),

  [SETTING_PROFILE_ACTIVE] = READ("Profile"),
  [SETTING_PRIMARY_ADDONS] = ESC("Use primary profile's addons", V_ON, 2),   // local, per profile
  [SETTING_SYNC] = READ("Sync"),
  [SETTING_TRAKT] = ACTION("Trakt"),
  [SETTING_SIMKL] = ACTION("Simkl"),
  [SETTING_EXIT] = ACTION("Sign out"),
  [SETTING_VERSION_I] = READ("Version"),
  [SETTING_SPACE] = READ("Memory used by images"),
};

// Each option's name in the file. The format used to be POSITIONAL — one line per
// option, in the enum's order — and that is why adding an option in the middle
// made the file of anyone who already had the app apply the wrong values,
// silently. With one key per line, a new option is born at its default and the
// old ones stay where they were. The names follow the web app's where there is a
// counterpart.
static const char *KEY[] = {
  [SETTING_QUALITY] = "quality", [SETTING_DV] = "dolbyVision", [SETTING_ATMOS] = "dolbyAtmos",
  // Local to this port, like the subtitle row below: the account's player
  // settings store the language as a code, and this row stores an index.
  [SETTING_AUDIO_LANG] = "audioPreferredLanguageIndex",
  // Local to this port too: the web app has no anime row.
  [SETTING_AUDIO_ANIME] = "animeAudioPreferredLanguageIndex",
  // NOT "subtitleLanguage": that name exists in the web app's blob with values of
  // its own ("off", "eng", "system"), and sharing the name would have the account
  // feed a string this row cannot read on every sync. A name of this port's own
  // never matches in the blob, which is what keeps the row local — and, unlike a
  // "-" key, it is still written to settings.txt.
  [SETTING_SUBS] = "subtitlePreferredGroup",
  // The web's useForcedSubtitles lives inside its subtitleStyle object, which
  // the blob never flattens into this file; a name of the port's own.
  [SETTING_SUBS_FORCED] = "subtitleForcedFallback",
  // Local to this port: the web app has no such key, so the blob never touches it.
  [SETTING_SEEK_COLOR] = "seekBarColor",
  // The web's autoplayNextEpisode, under a name of the port's own for the same
  // reason as the subtitle row: the account's copy is not this file's format.
  [SETTING_NEXT_AUTOPLAY] = "nextEpisodeAutoplay",
  // Local to this port as well: how long the Up next card waits before it plays.
  [SETTING_NEXT_COUNTDOWN] = "nextEpisodeCountdownSeconds",
  // nextEpisodeThresholdMode / ...MinutesBeforeEnd / ...Percent, in this port's
  // units: an index, seconds and whole percent.
  [SETTING_NEXT_MODE] = "nextEpisodeTriggerMode", [SETTING_NEXT_SECONDS] = "nextEpisodeSecondsBeforeEnd", [SETTING_NEXT_PERCENT] = "nextEpisodePercentWatched",
  // Local to this port: NuvioTV has an on/off switch at a fixed 5 s; this is one
  // row, seconds, with 0 for off.
  [SETTING_PAUSE_DELAY] = "pauseOverlayDelaySeconds",
  // Local to this port: Live TV has no web counterpart.
  [SETTING_LIVE_ON] = "liveTvEnabled", [SETTING_LIVE_SOURCE] = "-livesource", [SETTING_LIVE_BUFFER] = "liveTvPauseBufferIndex", [SETTING_LIVE_PREVIEW] = "liveTvPreviewWhileBrowsing",
  [SETTING_LANDSCAPE] = "modernLandscapePostersEnabled", [SETTING_HERO_FULL] = "modernHeroFullScreenBackdropEnabled",
  [SETTING_HERO_AREA] = "heroBackdropArea", [SETTING_HERO_BAND] = "heroBackdropScale",
  [SETTING_HERO_TRAILER_SIZE] = "heroTrailerScale",
  [SETTING_RAIL] = "collapseSidebar", [SETTING_RAIL_MODERN] = "modernSidebar", [SETTING_RAIL_BLUR] = "modernSidebarBlur",
  [SETTING_HERO] = "heroSectionEnabled", [SETTING_HERO_CATALOGS] = "-heroCatalogKeys",
  // Local to this port: the web app has no such key, so the blob never touches it.
  [SETTING_HERO_TRAILER] = "heroTrailerPreviews", [SETTING_HERO_TRAILER_DELAY] = "heroTrailerDelay",
  [SETTING_HERO_TRAILER_CW] = "heroTrailerContinueWatching",
  [SETTING_DISCOVER] = "discoverLocation", [SETTING_LABELS] = "posterLabelsEnabled", [SETTING_NAME_ADDON] = "catalogAddonNameEnabled",
  [SETTING_SUFFIX_KIND] = "catalogTypeSuffixEnabled", [SETTING_HIDE_UNRELEASED] = "hideUnreleasedContent",
  [SETTING_SCORES_HOME] = "homeImdbRatingsVisibility", [SETTING_GRADIENT_CLASSIC] = "classicFocusGradientEnabled",
  [SETTING_CW_ON] = "continueWatchingEnabled", [SETTING_CW_STYLE] = "continueWatchingCardStyle",
  // Local to this port: the web app has no such key, so the blob never touches it.
  [SETTING_CW_LOGO] = "continueWatchingTitleLogo", [SETTING_CW_PLAY] = "continueWatchingPlayOnSelect",
  [SETTING_CW_THUMB] = "useEpisodeThumbnailsInCw", [SETTING_CW_BLUR_NEXT] = "blurContinueWatchingNextUp",
  [SETTING_CW_FURTHEST] = "nextUpFromFurthestEpisode", [SETTING_CW_NOT_SHOWN] = "showUnairedNextUp", [SETTING_CW_ORDER] = "continueWatchingSortMode",
  [SETTING_DET_BLUR_NOT_WATCHED] = "blurUnwatchedEpisodes", [SETTING_DET_TRAILER] = "detailPageTrailerButtonEnabled",
  [SETTING_DET_META_EXT] = "preferExternalMetaAddonDetail", [SETTING_DET_DATE_FULL] = "showFullReleaseDate",
  [SETTING_EXPAND] = "focusedPosterBackdropExpandEnabled", [SETTING_EXPAND_DELAY] = "focusedPosterBackdropExpandDelaySeconds",
  [SETTING_NAV_FAST] = "fastHorizontalNavigationEnabled",
  // Local to this port: the web app has no such key, so the blob never touches it.
  [SETTING_ROW_SCROLL] = "rowScrollAnchor",
  [SETTING_DEPTH] = "cardDepthEnabled", [SETTING_DEPTH_BORDER] = "cardDepthEdgeStrength", [SETTING_DEPTH_BRIGHTNESS] = "cardDepthSheenStrength",
  [SETTING_DEPTH_COVERAGE] = "cardDepthEdgeCoverage", [SETTING_DEPTH_POSTERS] = "cardDepthPostersEnabled",
  [SETTING_DEPTH_CW] = "cardDepthContinueWatchingEnabled", [SETTING_DEPTH_EPS] = "cardDepthEpisodeCardsEnabled",
  [SETTING_DEPTH_CAST] = "cardDepthCastEnabled", [SETTING_DEPTH_TRAILERS] = "cardDepthTrailersEnabled",
  [SETTING_WIDTH_DP] = "posterCardWidthDp", [SETTING_RADIUS_DP] = "posterCardCornerRadiusDp",
  [SETTING_ANIM] = "reducedAnimations",
  // Account: these are local rows; they neither come from nor go to the cloud profile.
  [SETTING_PROFILE_ACTIVE] = "-profile",
  // THE TV'S OWN, not the account's `uses_primary_addons`: it is kept in this
  // profile's settings file and never read from the blob, whose keys this does
  // not match. On by default, which is how a second profile gets the addons the
  // household has set up rather than the few it was created with.
  [SETTING_PRIMARY_ADDONS] = "tvUsePrimaryAddons",
  [SETTING_SYNC] = "-sync", [SETTING_TRAKT] = "-trakt", [SETTING_SIMKL] = "-simkl", [SETTING_EXIT] = "-exit",
  [SETTING_VERSION_I] = "-version", [SETTING_SPACE] = "-space",
};

// The compiler CHECKS that there is one key per option. Without this, adding an
// option to the enum and forgetting the key leaves the last entries NULL and
// MISALIGNS every key after the insertion point — and the defect does not show up
// at once: only when settings.txt starts to exist does the strcmp(NULL,...) bring
// the app down on the next start. That is exactly what happened, and the only
// symptom on the TV was the app opening and closing.
typedef char checked_one_key_per_option[
  (sizeof KEY / sizeof *KEY == SETTING_N) ? 1 : -1];

// Where each section starts and how many options it has. A section is a
// navigation level: the screen opens on a list of the sections, and OK on one
// opens its options. The titles are the web app's; the blurb is what the side
// panel says about a section before it is opened. `group` starts a new group
// header on the list of sections; NULL continues the one above.
static const struct { const char *group, *title; int start, n; const char *blurb; } SECTIONS[] = {
  { "Playback", "Playback",          SETTING_QUALITY,              14,
    "Quality, Dolby formats, languages, subtitles, what happens at the end of an episode and the player's controls." },
  { "IPTV", "IPTV",                  SETTING_LIVE_ON,              4,
    "Live TV on or off, the IPTV source and its extra TV guides, and Live TV's pause buffer and preview while browsing." },
  { "Home", "Hero",                  SETTING_HERO,                 9,
    "The featured title at the top of Home: whether it shows, whether it plays the trailer and how its backdrop is drawn." },
  // No options of its own: `start` is SETTING_N, the marker openSection reads to
  // open the Home rows list (level 2) instead of a list of options.
  { NULL, "Home rows",               SETTING_N,                    0,
    "The order of the Home rows and which of them show. Saved to your account, so the web app follows it." },
  { NULL, "Catalogue rows",          SETTING_SUFFIX_KIND,          4,
    "What the catalogue rows say about themselves and which titles they leave out." },
  { NULL, "Continue watching",       SETTING_CW_ON,                9,
    "Whether the resume row appears, how it is sorted, how its cards look and what counts as next up." },
  { "Interface", "Sidebar and navigation", SETTING_RAIL,           7,
    "The sidebar, where Discover lives, how rows scroll and motion across the app." },
  { NULL, "Posters",                 SETTING_LANDSCAPE,            7,
    "Poster shape, labels, what a focused poster does and its size." },
  { NULL, "Depth effect",            SETTING_DEPTH,                9,
    "The lit edge and sheen on cards, and which cards get it." },
  { NULL, "Detail page",             SETTING_DET_TRAILER,          4,
    "The trailer button, metadata, spoilers and release dates on a title's page." },
  { "Account", "Account",            SETTING_PROFILE_ACTIVE,       6,
    "Profile, addons, sync, Trakt, Simkl and signing out." },
  { NULL, "About",                   SETTING_VERSION_I,            2,
    "Version and image memory." },
};
#define SETTING_N_SECTIONS (int)(sizeof SECTIONS / sizeof *SECTIONS)

// Each option's value. For OP_CHOICE it is the index; for OP_NUMBER it is the
// number itself. The defaults are those of layoutPreferences.js, with ONE
// exception noted line by line: the four the owner's profile diverges from the
// factory settings on are born as they left them, because that is what they see
// today. All of them are changeable here, which was the point.
static int value[SETTING_N] = {
  [SETTING_PRIMARY_ADDONS] = 0,         /* use the primary profile's addons: on */
  [SETTING_QUALITY] = 0, [SETTING_DV] = 0, [SETTING_ATMOS] = 0,          /* quality, DV, Atmos */
  [SETTING_AUDIO_LANG] = 0,                /* audio language: the file's default */
  [SETTING_AUDIO_ANIME] = 0,                /* anime audio language: same as the row above */
  // AUTOMATIC and not "Off". Off is what the app did before this row existed —
  // nothing ever selected a subtitle, on any title — and it is the behaviour the
  // owner reported as "subtitles are not really a thing here". A default of Off
  // would ship the same complaint with a switch next to it.
  [SETTING_SUBS] = 1,                /* subtitles: automatic (English) */
  [SETTING_SUBS_FORCED] = 1,                /* forced subtitles: off (the web's default) */
  [SETTING_SEEK_COLOR] = 0,                /* seek bar colour: violet */
  [SETTING_NEXT_AUTOPLAY] = 0,                /* autoplay next episode: on */
  [SETTING_NEXT_COUNTDOWN] = 15,               /* next episode countdown: 15 s */
  [SETTING_NEXT_MODE] = 0,                /* up next appears: before the end (the web's default) */
  [SETTING_NEXT_SECONDS] = 120,              /* up next lead: 2 min, the web's default and the old fixed value */
  [SETTING_NEXT_PERCENT] = 99,               /* up next share: 99% (the web's default) */
  [SETTING_PAUSE_DELAY] = 10,               /* pause overlay: after 10 s paused */
  // Both off: the buffer writes the channel to the TV's flash while it plays, and
  // a preview opens a connection on every rest.
  // OFF: Live TV is for whoever brings a playlist. Until it is turned on the
  // side bar has no Live TV row.
  [SETTING_LIVE_ON] = 1,                    /* Live TV: off */
  [SETTING_LIVE_BUFFER] = 0,                /* Live TV pause buffer: off */
  [SETTING_LIVE_PREVIEW] = 1,               /* Live TV preview while browsing: off */

  [SETTING_LANDSCAPE] = 0,                /* landscape posters: ON (the owner's profile; factory: off) */
  [SETTING_HERO_FULL] = 0,                /* full-screen backdrop: ON (profile; factory: off) */
  [SETTING_HERO_AREA] = 0,                /* backdrop area: the whole screen, which is what it did before */
  [SETTING_HERO_BAND] = NV_HERO_FIT_PCT_DEFAULT, /* backdrop size, % of the screen's width */
  [SETTING_HERO_TRAILER_SIZE] = TRAILER_SAME, /* trailer size: same as the backdrop */

  [SETTING_RAIL] = 0,                /* sidebar: collapsed (profile; factory: fixed) */
  [SETTING_RAIL_MODERN] = 1,                /* modern sidebar: off */
  [SETTING_RAIL_BLUR] = 0,                /* modern bar blur: on (profile) */
  [SETTING_HERO] = 0,                /* show hero: on */
  [SETTING_HERO_CATALOGS] = 0,                /* hero catalogues: read-only */
  // OFF by default: a video with sound starting on the home is not something to
  // spring on anyone. Turned on in Settings, it stays on.
  [SETTING_HERO_TRAILER] = 1,                /* trailer previews: off */
  [SETTING_HERO_TRAILER_DELAY] = 22,         /* trailer delay: 2.2 s */
  [SETTING_HERO_TRAILER_CW] = 0,             /* trailers on continue watching: on */
  [SETTING_DISCOVER] = 0,                /* discover location: in search */
  // OFF by default: the poster already carries the title printed on the art, and
  // repeating the name just below is the same information twice taking up row
  // height. It is still a setting — anyone who wants the label turns it on in Settings.
  [SETTING_LABELS] = 1,                /* poster labels: off */
  [SETTING_NAME_ADDON] = 0,                /* addon name: on */
  [SETTING_SUFFIX_KIND] = 0,                /* content type: on */
  [SETTING_HIDE_UNRELEASED] = 1,                /* hide unreleased: off */
  [SETTING_SCORES_HOME] = 0,                /* overall ratings: show (SHOW_ALL) */
  [SETTING_GRADIENT_CLASSIC] = 1,                /* classic focus gradient: off */

  [SETTING_CW_ON] = 0,                /* continue watching: on */
  [SETTING_CW_STYLE] = 0,                /* style: card */
  [SETTING_CW_LOGO] = 1,                /* show logo: off */
  [SETTING_CW_PLAY] = 1,                /* play on select: off */
  [SETTING_CW_THUMB] = 0,                /* episode thumbnail: on */
  [SETTING_CW_BLUR_NEXT] = 1,                /* blur next up: off */
  [SETTING_CW_FURTHEST] = 0,                /* next from the furthest episode: on */
  [SETTING_CW_NOT_SHOWN] = 0,                /* show unaired: on */
  [SETTING_CW_ORDER] = 0,                /* sort order: default */

  [SETTING_DET_BLUR_NOT_WATCHED] = 1,                /* blur unwatched: off */
  [SETTING_DET_TRAILER] = 0,                /* trailer button: on */
  [SETTING_DET_META_EXT] = 0,                /* external metadata: on */
  [SETTING_DET_DATE_FULL] = 0,                /* full date: on */

  [SETTING_EXPAND] = 0,                /* expand poster on focus: on (the web's DEFAULT) */
  [SETTING_EXPAND_DELAY] = 3,                /* delay: 3s */
  [SETTING_NAV_FAST] = 1,                /* fast horizontal navigation: off (factory) */
  [SETTING_ROW_SCROLL] = 0,                /* row scrolling: minimal, what it always did */

  [SETTING_DEPTH] = 1,                /* depth effect: off (factory) */
  [SETTING_DEPTH_BORDER] = 28,               /* edge brightness */
  [SETTING_DEPTH_BRIGHTNESS] = 10,               /* sheen */
  [SETTING_DEPTH_COVERAGE] = 0,                /* edge coverage */
  /* depth on posters, cw, episodes, cast, trailers: all on */
  [SETTING_DEPTH_POSTERS] = 0, [SETTING_DEPTH_CW] = 0, [SETTING_DEPTH_EPS] = 0,
  [SETTING_DEPTH_CAST] = 0, [SETTING_DEPTH_TRAILERS] = 0,

  [SETTING_WIDTH_DP] = 126,              /* item width, dp (factory; the owner's profile uses 120) */
  [SETTING_RADIUS_DP] = 12,               /* corner rounding, dp */

  [SETTING_ANIM] = 0,                /* animations: full */
  // The account and about rows hold no value; they are left at zero.
};

// Two levels: 0 is the list of sections, 1 is the options of section `focusSec`.
// focusOp only means anything on level 1, and it always lies inside that section.
// Level 2 is the Home rows list, which has rows of its own instead of options.
static int level = 0;
static int rowFocus, rowHolding;   // level 2: the focused row; 1 while it is picked up
static float rowScroll;
static float rowAnim[512];
static void leaveRows(void);
static int focusSec = 0;
static int focusOp = 0;
// A ONE-column list does not need focus.h: the column memory it exists to solve
// has nothing to remember here.
static float animFocus[SETTING_N];
static float animSec[SETTING_N_SECTIONS];
// One scroll per level, so coming back from a section finds the list where it was.
static float scrollSec = 0.0f;
static float scrollY = 0.0f;
// Whether the list's scroll follows the focus. A row the Magic Remote's pointer
// focused leaves it where it is (goalScroll): bringing a half-shown row into view
// would slide the next under a pointer that had not moved. Any arrow, the
// wheel's included, hands it back.
static int follow = 1;
static float goalScroll;
static ScrollBar bar;
// Where the focused row's "‹" was drawn, so a click on it steps back rather than
// forward. Written by drawStepper each frame.
static float stepLtX0, stepLtX1;
static int wantsExit = 0;
static int requestMenu = 0;   // LEFT on a row LEFT cannot change
static int requestLiveSource; // OK on IPTV source: the app opens Live TV's source screen

// How many catalogues the hero uses. 0 = all, which is what the web app writes as
// "All" when heroCatalogKeys is empty — and it is the owner's profile's case.
static int heroCatalogs = 0;

static int on(int op)  { return value[op] == 0; }

int settings_animations_reduced(void) { return value[SETTING_ANIM] == 1; }
int settings_dolby_vision(void)        { return on(SETTING_DV); }
int settings_dolby_atmos(void)         { return on(SETTING_ATMOS); }
int settings_subtitle_pref(void)       { return value[SETTING_SUBS]; }
int settings_subtitle_language(void) {
  int p = value[SETTING_SUBS];
  return p == 0 ? -1 : p == 1 ? LANG_ENGLISH : p - 2;
}
int settings_subtitle_forced(void)     { return on(SETTING_SUBS_FORCED); }
int settings_audio_language(void)      { return value[SETTING_AUDIO_LANG] - 1; }
int settings_anime_audio_language(void) {
  return value[SETTING_AUDIO_ANIME] ? value[SETTING_AUDIO_ANIME] - 2 : settings_audio_language();
}
int settings_next_autoplay(void)       { return on(SETTING_NEXT_AUTOPLAY); }
double settings_next_lead(double durationSeg) {
  if (value[SETTING_NEXT_MODE] == 1)
    return durationSeg * (100 - value[SETTING_NEXT_PERCENT]) / 100.0;
  return (double)value[SETTING_NEXT_SECONDS];
}
int settings_next_countdown(void)    { return value[SETTING_NEXT_COUNTDOWN]; }
int settings_pause_overlay_ms(void)  { return value[SETTING_PAUSE_DELAY] * 1000; }
static void save(void);
int settings_live_buffer_minutes(void) {
  static const int MIN[4] = { 0, 15, 30, 60 };
  int i = value[SETTING_LIVE_BUFFER];
  return i >= 0 && i < 4 ? MIN[i] : 0;
}
int settings_live_preview(void)      { return on(SETTING_LIVE_PREVIEW); }
int settings_live_enabled(void)      { return on(SETTING_LIVE_ON); }
void settings_set_live(int bufferMinutes, int preview) {
  int i = bufferMinutes >= 60 ? 3 : bufferMinutes >= 30 ? 2 : bufferMinutes > 0 ? 1 : 0;
  value[SETTING_LIVE_BUFFER] = i;
  value[SETTING_LIVE_PREVIEW] = preview ? 0 : 1;
  save();
}
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
int settings_hero_trailer(void)    { return on(SETTING_HERO) && on(SETTING_HERO_TRAILER); }
int settings_hero_trailer_delay_ms(void) { return value[SETTING_HERO_TRAILER_DELAY] * 100; }
int settings_hero_trailer_cw(void) { return on(SETTING_HERO_TRAILER_CW); }
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
float settings_hero_trailer_scale(void) {
  if (value[SETTING_HERO_TRAILER_SIZE] <= TRAILER_SAME) return 0.0f;
  return (float)value[SETTING_HERO_TRAILER_SIZE] / 100.0f;
}
int settings_posters_landscape(void)   { return on(SETTING_LANDSCAPE); }
int settings_gradient_focus_classic(void) { return on(SETTING_GRADIENT_CLASSIC); }

int settings_labels_poster(void)      { return on(SETTING_LABELS); }
int settings_name_addon(void)          { return on(SETTING_NAME_ADDON); }
int settings_suffix_kind(void)         { return on(SETTING_SUFFIX_KIND); }
int settings_use_primary_addons(void)  { return on(SETTING_PRIMARY_ADDONS); }
int settings_hide_unreleased(void){ return on(SETTING_HIDE_UNRELEASED); }
int settings_date_full(void)       { return on(SETTING_DET_DATE_FULL); }
int settings_scores_home(void)          { return value[SETTING_SCORES_HOME] == 0; }
int settings_local_discover(void)     { return value[SETTING_DISCOVER]; }
int settings_discover_na_search(void)  { return value[SETTING_DISCOVER] == 0; }

int settings_cw_on(void)           { return on(SETTING_CW_ON); }
int settings_cw_style(void)           { return value[SETTING_CW_STYLE]; }
int settings_cw_logo(void)            { return on(SETTING_CW_LOGO); }
int settings_cw_play(void)            { return on(SETTING_CW_PLAY); }
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
int   settings_row_first_slot(void) { return value[SETTING_ROW_SCROLL] == 1; }

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

// ONE FILE PER PROFILE: settings.txt for profile 1, settings-p<N>.txt for the
// others. There used to be one for the whole TV, so a profile took over whatever
// the last person had set, and an option changed here was undone by the
// profile's account blob on the way back. The Android TV app keeps them per
// profile too.
static int settingsProfile = 1;
// The built-in values, taken before the first file is read: a switch starts
// from these, not from the profile being left.
static int defaults[SETTING_N];
static int defaultsTaken;
// 1 when the last profile loaded had no file of its own yet.
static int profileFresh;

static void settingsPath(char *dst, size_t size, int profile, const char *ext) {
  if (profile <= 1) snprintf(dst, size, "%s/settings.%s", dirSettings, ext);
  else snprintf(dst, size, "%s/settings-p%d.%s", dirSettings, profile, ext);
}


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


// Reads one settings file over the values in force; 0 when there is none.
static int readFile(const char *path) {
  FILE *f;
  char line[96];
  f = fopen(path, "r");
  if (!f) return 0;
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
  return 1;
}

static void loadProfile(void) {
  char path[600];
  memcpy(value, defaults, sizeof value);
  settingsPath(path, sizeof path, settingsProfile, "txt");
  profileFresh = !readFile(path);
  if (!profileFresh || settingsProfile <= 1) return;
  // A PROFILE'S FIRST TIME ON THIS TV starts from profile 1's settings — the
  // owner's choice: the TV already set up the way the household uses it, not
  // the built-in defaults. The profile's account blob still goes on top on the
  // cycle the switch starts, and from then on the file is its own.
  settingsPath(path, sizeof path, 1, "txt");
  readFile(path);
  save();
}

void settings_dir(const char *dir) {
  if (!dir || !*dir) return;
  snprintf(dirSettings, sizeof dirSettings, "%s", dir);
  if (!defaultsTaken) { memcpy(defaults, value, sizeof defaults); defaultsTaken = 1; }
  loadProfile();
}

void settings_set_profile(int profile) {
  if (profile <= 0) profile = 1;
  if (profile == settingsProfile) return;
  settingsProfile = profile;
  // Before settings_dir (profiles_load_active runs first at startup) this only
  // picks the file settings_dir will read.
  if (dirSettings[0]) loadProfile();
}

int settings_profile_fresh(void) { return profileFresh; }

static void save(void) {
  char path[600], tmp[600];
  FILE *f;
  int i;
  if (!dirSettings[0]) return;
  settingsPath(path, sizeof path, settingsProfile, "txt");
  settingsPath(tmp, sizeof tmp, settingsProfile, "tmp");
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

int settings_start(void) {
  leaveRows();
  level = 0; focusSec = 0; focusOp = 0;
  scrollSec = 0.0f; scrollY = 0.0f; wantsExit = 0;
  return 1;
}
void settings_resume(void) { wantsExit = 0; requestMenu = 0; }
void settings_shutdown(void) { leaveRows(); }
int settings_wants_exit(void) { return wantsExit; }
int settings_requested_menu(void) { int v = requestMenu; requestMenu = 0; return v; }
int settings_requested_live_source(void) { int v = requestLiveSource; requestLiveSource = 0; return v; }

// The value of the read-only rows. The disk space is NOT an invented number: it
// comes from the texture cache, which is exactly what "images" consumes on the
// device — a fixed number here would be a lie and would never change.
static const char *textRead(int op) {
  if (op == SETTING_LIVE_SOURCE)
    return !on(SETTING_LIVE_ON) ? "Live TV is off" : iptv_configured() ? iptv_source_label() : "not set up";
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
      case TRA_ON:     return "connected";
      case TRA_REQUESTING:    return "preparing…";
      case TRA_WAITING: return "waiting";
      case TRA_ERROR:       return "failed";
      default:             return "connect";
    }
  }
  if (op == SETTING_SIMKL) {
    switch (simklauth_state()) {
      case SMK_ON:     return "connected";
      case SMK_REQUESTING:    return "preparing…";
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
    case SETTING_HERO_CATALOGS: case SETTING_HERO_TRAILER: return !settings_hero_on();
    case SETTING_HERO_TRAILER_SIZE: return !settings_hero_trailer() || !settings_hero_top_band();
    case SETTING_HERO_TRAILER_DELAY: case SETTING_HERO_TRAILER_CW: return !settings_hero_trailer();
    case SETTING_CW_STYLE: case SETTING_CW_PLAY: case SETTING_CW_THUMB: case SETTING_CW_FURTHEST:
    case SETTING_CW_NOT_SHOWN: case SETTING_CW_ORDER:
      return !settings_cw_on();
    case SETTING_CW_BLUR_NEXT: return !settings_cw_on() || !settings_cw_thumb_episode();
    // The Poster style draws plain poster cards, which have no title to replace.
    case SETTING_CW_LOGO: return !settings_cw_on() || settings_cw_style() == 2;
    case SETTING_EXPAND_DELAY: return !settings_expand_poster();
    case SETTING_DEPTH_BORDER: case SETTING_DEPTH_BRIGHTNESS: case SETTING_DEPTH_COVERAGE:
    case SETTING_DEPTH_POSTERS: case SETTING_DEPTH_CW: case SETTING_DEPTH_EPS:
    case SETTING_DEPTH_CAST: case SETTING_DEPTH_TRAILERS:
      return !settings_depth();
    case SETTING_NEXT_COUNTDOWN: return !settings_next_autoplay();
    case SETTING_NEXT_SECONDS:   return value[SETTING_NEXT_MODE] != 0;
    case SETTING_NEXT_PERCENT:   return value[SETTING_NEXT_MODE] != 1;
    default: return 0;
  }
}

// An action is NOT read-only (it has the active row's highlight), but it is NOT
// mutable either (left/right do nothing on it). The two answers are deliberately
// different, and that is why they are two functions.
static int readOnly(int op) { return OPTIONS[op].kind == OP_READ; }
static int mutable(int op)   { return OPTIONS[op].kind != OP_READ &&
                                      OPTIONS[op].kind != OP_ACTION && !inactive(op); }

static int isRowsSection(int s) { return SECTIONS[s].start == SETTING_N; }

// Leaving the Home rows list is when its edit applies: one rebuild and one push
// for the whole session in the list, not one per key press.
static void leaveRows(void) {
  if (level != 2) return;
  rowHolding = 0;
  homerows_commit();
  level = 0;
}

static void openSection(int s) {
  leaveRows();
  if (isRowsSection(s)) {
    level = 2;
    focusSec = s;
    rowFocus = 0; rowHolding = 0; rowScroll = 0.0f;
    homerows_open();
    return;
  }
  level = 1;
  focusSec = s;
  focusOp = SECTIONS[s].start;
  scrollY = 0.0f;
}

// NULL when the row has nothing to add beyond its label and value.
static const char *helpOption(int op) {
  if (inactive(op)) {
    if (op == SETTING_RAIL) return "Turn off the modern sidebar to choose between collapsed and fixed.";
    if (op == SETTING_RAIL_BLUR) return "Turn on the modern sidebar to use the blur.";
    if (op == SETTING_HERO_CATALOGS) return "Turn on Show hero to display catalogues at the top of Home.";
    if (op == SETTING_HERO_TRAILER)
      return "Turn on Show hero to play trailers at the top of Home.";
    if (op == SETTING_HERO_TRAILER_DELAY)
      return "Turn on Trailer previews to choose how long it waits.";
    if (op == SETTING_HERO_TRAILER_CW)
      return "Turn on Trailer previews to choose where they play.";
    if (op == SETTING_HERO_TRAILER_SIZE)
      return settings_hero_trailer() ? "Set Backdrop area to Top band to size the trailer on its own."
                                     : "Turn on Trailer previews to size the trailer.";
    if (op > SETTING_CW_ON && op <= SETTING_CW_NOT_SHOWN)
      return op == SETTING_CW_BLUR_NEXT && settings_cw_on()
        ? "Turn on Episode thumbnail to blur the next episode image."
        : op == SETTING_CW_LOGO && settings_cw_on()
        ? "Set the style to Card or Wide to show logos."
        : "Turn on Continue watching to adjust the resume cards.";
    if (op == SETTING_EXPAND_DELAY) return "Turn on Expand poster on focus to adjust the delay.";
    if (op == SETTING_NEXT_COUNTDOWN) return "Turn on Autoplay next episode to set the countdown.";
    if (op == SETTING_NEXT_SECONDS) return "Set Up next appears to Before the end to use this.";
    if (op == SETTING_NEXT_PERCENT) return "Set Up next appears to Percentage watched to use this.";
    return "Turn on Depth effect to customise this detail.";
  }
  switch (op) {
    case SETTING_RAIL: return "Collapsed hides the bar until you press left into it, and the content starts 104 px from the edge. Fixed keeps the 144 px icon bar on every screen.";
    case SETTING_RAIL_MODERN: return "On this TV the modern sidebar has no look of its own: turning it on keeps the bar fixed.";
    case SETTING_HERO: return "The large featured title at the top of Home. Off, the top of Home stays empty and the rows keep their place.";
    case SETTING_LABELS: return "The title under each poster.";
    case SETTING_SUFFIX_KIND: return "Adds the content type to a row's name, such as Popular - Movie.";
    case SETTING_LANDSCAPE: return "Wide 16:9 cards in place of upright posters, on every catalogue row.";
    case SETTING_HERO_FULL: return "The hero's art fills the whole screen behind the rows. Off, it sits in a band at the top right.";
    case SETTING_HERO_TRAILER: return "Resting on a title for a moment plays its trailer behind the hero, with sound, until you move on. Off, the hero keeps the still backdrop.";
    case SETTING_HERO_AREA: return "Top band draws the whole image at its own shape in the top right, so nothing is cropped.";
    case SETTING_HERO_BAND: return "The top band's width, as a share of the screen.";
    case SETTING_HERO_TRAILER_DELAY: return "How long the hero rests on a title before its trailer starts.";
    case SETTING_HERO_TRAILER_CW: return "Off, titles in Continue watching keep the still backdrop; every other row still plays trailers.";
    case SETTING_HERO_TRAILER_SIZE: return "The trailer's width in the top band, as a share of the screen. Same as backdrop follows Backdrop size.";
    case SETTING_CW_ON: return "The row of titles you have started, at the top of Home.";
    case SETTING_CW_STYLE: return "Card and Wide both draw 16:9 resume cards on this TV; Poster uses upright posters.";
    case SETTING_EXPAND: return "A focused poster opens out into its 16:9 art after the delay.";
    case SETTING_DEPTH: return "A lit edge and sheen on the focused card.";
    case SETTING_ROW_SCROLL: return "First slot moves every row so the focused card sits at its start, and the rest slide sideways. Minimal scrolls only when the card would leave the screen.";
    case SETTING_SEEK_COLOR: return "The colour of the player's progress bar and its playhead.";
    case SETTING_NEXT_COUNTDOWN: return "How long the Up next card waits before it plays the next episode.";
    case SETTING_AUDIO_LANG: return "The audio track to switch to when the file has one in this language. File default keeps the file's own choice.";
    case SETTING_AUDIO_ANIME: return "The same, for anime only: Japanese animation, or a title from an anime catalogue. Same as Audio language follows the row above.";
    case SETTING_SUBS: return "The subtitle turned on when a title starts: the file's own track first, then an addon's. Addon subtitles are only listed in English and this language, so pick yours here.";
    case SETTING_SUBS_FORCED: return "When no subtitle is turned on, shows a forced track in the audio's language: signs and foreign dialogue only.";
    case SETTING_NEXT_AUTOPLAY: return "Plays the next episode when the Up next countdown ends. Off, the card waits for you.";
    case SETTING_NEXT_MODE: return "When the Up next card appears. The credits, when they are known, bring it up earlier.";
    case SETTING_NEXT_SECONDS: return "How long before the end of an episode the Up next card appears.";
    case SETTING_NEXT_PERCENT: return "How much of an episode has to be watched before the Up next card appears.";
    case SETTING_PAUSE_DELAY: return "How long playback sits paused, with no key pressed, before the title's details come up over a blurred picture. Off never shows them.";
    case SETTING_LIVE_ON: return "Live TV in the side bar, for an IPTV playlist of your own. Off, it is gone from the side bar and nothing of it runs.";
    case SETTING_LIVE_SOURCE: return "Your M3U playlist or Xtream Codes login, and extra TV guides for the channels it leaves without one. Opens Live TV's source screen, where a phone can fill it in too.";
    case SETTING_LIVE_BUFFER: return "Keeps the last minutes of a live channel on the TV so you can pause and rewind it. Works on MPEG-TS channels, and writes to the TV's storage while you watch.";
    case SETTING_LIVE_PREVIEW: return "Rest on a channel in the Live TV list or guide and it plays in the preview. It counts as watched only when you press OK.";
    case SETTING_QUALITY: return "Sets the resolution preference. Availability depends on the addon sources.";
    case SETTING_DV: case SETTING_ATMOS: return "Preference for compatible sources. The available format also depends on the file and the TV.";
    case SETTING_HERO_CATALOGS: return "How many catalogues the hero includes. This row is informational only.";
    case SETTING_PRIMARY_ADDONS: return "This profile uses the primary profile's addons instead of its own list. It has no effect on the primary profile.";
    case SETTING_CW_PLAY: return "Pressing OK on a resume card plays it straight away, skipping the title's page. Hold OK for the other options.";
    case SETTING_CW_LOGO: return "Shows the title's logo in place of its name on the resume cards. A title with no logo keeps its name.";
    case SETTING_CW_FURTHEST: return "Picks the next episode from the furthest one marked as watched.";
    case SETTING_CW_BLUR_NEXT: case SETTING_DET_BLUR_NOT_WATCHED: return "Hides thumbnail detail to avoid spoilers for episodes you have not watched.";
    case SETTING_ANIM: return "Use Reduced for subtler motion when moving through the interface.";
    case SETTING_SPACE: return "Current memory used by the image cache, not space taken on the TV storage.";
    case SETTING_VERSION_I: return "Application version. This information cannot be changed.";
    case SETTING_WIDTH_DP: return "Sets the poster width on rows that use the customisable size.";
    case SETTING_RADIUS_DP: return "Controls how rounded the poster corners are.";
    default: return NULL;
  }
}

// The group header over an option, or NULL when the option continues the group
// above it. Every section opens with one, so the list always starts on a kicker.
static const char *groupOfOption(int op) {
  switch (op) {
    case SETTING_QUALITY:        return "Picture and sound";
    case SETTING_AUDIO_LANG:     return "Audio and subtitles";
    case SETTING_NEXT_AUTOPLAY:  return "Up next";
    case SETTING_SEEK_COLOR:     return "Player";
    case SETTING_LIVE_ON:        return "Live TV";
    case SETTING_LIVE_SOURCE:    return "Source";
    case SETTING_LIVE_BUFFER:    return "Watching";
    case SETTING_HERO:           return "Hero";
    case SETTING_HERO_FULL:      return "Backdrop";
    case SETTING_SUFFIX_KIND:    return "Row titles";
    case SETTING_HIDE_UNRELEASED: return "Content";
    case SETTING_CW_ON:          return "Row";
    case SETTING_CW_STYLE:       return "Cards";
    case SETTING_CW_FURTHEST:    return "Next up";
    case SETTING_RAIL:           return "Sidebar";
    case SETTING_DISCOVER:       return "Discover";
    case SETTING_NAV_FAST:       return "Scrolling";
    case SETTING_ANIM:           return "Motion";
    case SETTING_LANDSCAPE:      return "Posters";
    case SETTING_EXPAND:         return "Focus";
    case SETTING_WIDTH_DP:       return "Size";
    case SETTING_DEPTH:          return "Effect";
    case SETTING_DEPTH_POSTERS:  return "Where it applies";
    case SETTING_DET_TRAILER:    return "Page";
    case SETTING_DET_BLUR_NOT_WATCHED: return "Episodes";
    case SETTING_PROFILE_ACTIVE: return "Profile";
    case SETTING_TRAKT:          return "Services";
    case SETTING_EXIT:           return "Session";
    case SETTING_VERSION_I:      return "App";
    default:                     return NULL;
  }
}

// The group header over row `i` of the list on screen.
static const char *groupOfRow(int i) {
  if (level == 2) return i == 0 ? "Order and visibility" : NULL;
  if (level == 1) return groupOfOption(SECTIONS[focusSec].start + i);
  return SECTIONS[i].group;
}

// Where row i sits from the list's top: the rows above it plus every group
// header down to and including its own. The Home rows list has one header only,
// and can run to hundreds of rows, so it skips the walk.
static float yOfRow(int i) {
  float y = 0.0f;
  int j;
  if (level == 2) return SETTING_GROUP_H + (float)i * SETTING_LINE_H;
  for (j = 0; j <= i; j++) {
    if (groupOfRow(j)) y += SETTING_GROUP_H;
    if (j < i) y += SETTING_LINE_H;
  }
  return y;
}

// Scrolls the minimum for row `i` to fit the window — its group header included,
// so coming back up to a group's first row shows what the group is.
static float scrollFor(float current, int i) {
  float top = yOfRow(i) - (groupOfRow(i) ? SETTING_GROUP_H : 0.0f);
  float base = yOfRow(i) + SETTING_LINE_H;
  float target = current;
  if (base - target > SETTING_BASE - SETTING_TOP) target = base - (SETTING_BASE - SETTING_TOP);
  if (top - target < 0.0f) target = top;
  if (target < 0.0f) target = 0.0f;
  return target;
}

// --- THE MAGIC REMOTE'S POINTER -------------------------------------------
// Pointing at a row focuses it. A click on a section opens it (OK); on an option
// it does what the remote's arrows do — the "‹" steps back, anywhere else steps
// forward — or runs it if it is an action; on a home row it picks the row up
// (OK), or shows / hides it from the arrows beside the value.
static void sendKey(SDL_Keycode k) {
  SDL_Event ev;
  SDL_zero(ev);
  ev.type = SDL_KEYDOWN;
  ev.key.keysym.sym = k;
  settings_event(&ev);
  follow = 0;
}
static int onStepBack(void) {
  float x = pointer_x();
  return stepLtX1 > stepLtX0 && x >= stepLtX0 - 24.0f && x <= stepLtX1 + 24.0f;
}
static void pointSection(int s, int unused) {
  (void)unused;
  if (level == 0 && s >= 0 && s < SETTING_N_SECTIONS) { focusSec = s; follow = 0; }
}
static void pointOption(int op, int unused) {
  (void)unused;
  if (level == 1) { focusOp = op; follow = 0; }
}
static void clickOption(int op, int unused) {
  (void)unused;
  if (level != 1) return;
  focusOp = op;
  if (OPTIONS[op].kind == OP_ACTION) sendKey(SDLK_RETURN);
  else if (mutable(op)) sendKey(onStepBack() ? SDLK_LEFT : SDLK_RIGHT);
}
static void pointHomeRow(int i, int unused) {
  (void)unused;
  if (level == 2 && !rowHolding) { rowFocus = i; follow = 0; }
}
static void clickHomeRow(int i, int unused) {
  (void)unused;
  if (level != 2) return;
  if (!rowHolding) rowFocus = i;
  if (!rowHolding && onStepBack()) sendKey(SDLK_RIGHT);
  else if (!rowHolding && pointer_x() > SETTING_LIST_X + SETTING_LIST_W * 0.6f)
    sendKey(SDLK_RIGHT);
  else sendKey(SDLK_RETURN);
}

void settings_event(const SDL_Event *e) {
  if (e->type != SDL_KEYDOWN) return;
  SDL_Keycode k = e->key.keysym.sym;
  if (k == SDLK_UP || k == SDLK_DOWN || k == SDLK_LEFT || k == SDLK_RIGHT) follow = 1;

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
  int back = k == SDLK_ESCAPE || k == SDLK_AC_BACK || k == SDLK_BACKSPACE ||
             k == SDLK_DELETE;

  // Level 0: the list of sections. OK or RIGHT opens one; LEFT is the way out to
  // the side menu, as on every other list with no value to step.
  if (level == 0) {
    if (back) { wantsExit = 1; return; }
    if (k == SDLK_DOWN)      { if (focusSec < SETTING_N_SECTIONS - 1) focusSec++; }
    else if (k == SDLK_UP)   { if (focusSec > 0) focusSec--; }
    else if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_RIGHT) openSection(focusSec);
    else if (k == SDLK_LEFT) requestMenu = 1;
    return;
  }

  // Level 2: the Home rows. OK picks a row up and puts it down; up and down move
  // the focus, or the row while it is held; left and right show or hide it.
  if (level == 2) {
    int n = homerows_n();
    if (back) {
      // Back while holding only puts the row down: leaving the list mid-move
      // would apply an order the person did not finish choosing.
      if (rowHolding) rowHolding = 0; else leaveRows();
      return;
    }
    if (k == SDLK_RETURN || k == SDLK_KP_ENTER) { if (n) rowHolding = !rowHolding; }
    else if (k == SDLK_UP || k == SDLK_DOWN) {
      int dir = k == SDLK_DOWN ? 1 : -1;
      if (rowHolding) rowFocus = homerows_move(rowFocus, dir);
      else if (rowFocus + dir >= 0 && rowFocus + dir < n) rowFocus += dir;
    }
    else if ((k == SDLK_LEFT || k == SDLK_RIGHT) && !rowHolding && n) homerows_toggle(rowFocus);
    else if ((k == SDLK_PAGEUP || k == SDLK_PAGEDOWN) && !rowHolding) {
      int s = focusSec + (k == SDLK_PAGEDOWN ? 1 : -1);
      if (s >= 0 && s < SETTING_N_SECTIONS) openSection(s);
    }
    return;
  }

  // Level 1: one section's options. Back returns to the list of sections, with
  // focus still on the section that was open.
  if (back) { level = 0; return; }
  int first = SECTIONS[focusSec].start;
  int last  = first + SECTIONS[focusSec].n - 1;

  if (k == SDLK_DOWN)      { if (focusOp < last)  focusOp++; }
  else if (k == SDLK_UP)   { if (focusOp > first) focusOp--; }
  else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
    if (OPTIONS[focusOp].kind != OP_ACTION) return;
    if (focusOp == SETTING_TRAKT) { traktauth_begin(); return; }
    // Off, there is no Live TV to open the source screen in.
    if (focusOp == SETTING_LIVE_SOURCE) { if (on(SETTING_LIVE_ON)) requestLiveSource = 1; return; }
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
    int s = focusSec + (k == SDLK_PAGEDOWN ? 1 : -1);
    if (s >= 0 && s < SETTING_N_SECTIONS) openSection(s);
  }
  else if (k == SDLK_LEFT || k == SDLK_RIGHT) {
    // A read-only item, or one switched off by its dependency, changes with nothing.
    // LEFT there has no value to step, so it goes back up to the list of sections —
    // on a value row it keeps stepping the value, which is what the row is for.
    if (!mutable(focusOp)) { if (k == SDLK_LEFT) level = 0; return; }
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
    // A different addon list: the next cycle reads it, and its arrival rebuilds
    // the home.
    if (focusOp == SETTING_PRIMARY_ADDONS) sync_resync();
  }
}

void settings_update(float dt, Uint32 now) {
  (void)now;
  int reduced = settings_animations_reduced();
  // A NEW LEVEL starts from its own scroll, whatever the pointer did on the last:
  // the goal is shared, and a click that opened a section left it holding the
  // section list's.
  { static int seenLevel = -1;
    if (level != seenLevel) {
      seenLevel = level;
      follow = 1;
      goalScroll = level == 2 ? rowScroll : level ? scrollY : scrollSec;
    } }
  // THE POINTER RESTING ON THE LIST'S EDGE scrolls it; the focus stays put.
  { float d = app_screen_in_front() && traktauth_state() != TRA_WAITING
            ? pointer_edge_scroll(SETTING_LIST_X, SETTING_LIST_X + SETTING_LIST_W,
                                  SETTING_TOP, SETTING_BASE, dt) : 0.0f;
    int rows = level == 2 ? homerows_n() : level ? SECTIONS[focusSec].n : SETTING_N_SECTIONS;
    if (d != 0.0f && rows > 0) {
      float max = yOfRow(rows - 1) + SETTING_LINE_H - (SETTING_BASE - SETTING_TOP);
      if (max < 0.0f) max = 0.0f;
      goalScroll = anim_clamp(goalScroll + d, 0.0f, max);
      follow = 0;
    } }
  for (int i = 0; i < SETTING_N; i++) {
    float target = (level == 1 && i == focusOp) ? 1.0f : 0.0f;
    animFocus[i] = reduced ? target : anim_spring(animFocus[i], target, dt,
                            target > animFocus[i] ? NV_SPRING_FOCUS : NV_SPRING_BLUR);
  }
  for (int s = 0; s < SETTING_N_SECTIONS; s++) {
    float target = (level == 0 && s == focusSec) ? 1.0f : 0.0f;
    animSec[s] = reduced ? target : anim_spring(animSec[s], target, dt,
                            target > animSec[s] ? NV_SPRING_FOCUS : NV_SPRING_BLUR);
  }
  { int n = homerows_n() < 512 ? homerows_n() : 512;
    for (int i = 0; i < n; i++) {
      float target = (level == 2 && i == rowFocus) ? 1.0f : 0.0f;
      rowAnim[i] = reduced ? target : anim_spring(rowAnim[i], target, dt,
                              target > rowAnim[i] ? NV_SPRING_FOCUS : NV_SPRING_BLUR);
    } }
  if (level == 0) {
    float target = follow ? scrollFor(scrollSec, focusSec) : goalScroll;
    goalScroll = target;
    scrollSec = reduced ? target : anim_spring(scrollSec, target, dt, NV_SPRING_SCROLL);
  } else if (level == 2) {
    float target = follow ? scrollFor(rowScroll, rowFocus) : goalScroll;
    goalScroll = target;
    rowScroll = reduced ? target : anim_spring(rowScroll, target, dt, NV_SPRING_SCROLL);
  } else {
    float target = follow ? scrollFor(scrollY, focusOp - SECTIONS[focusSec].start)
                          : goalScroll;
    goalScroll = target;
    scrollY = reduced ? target : anim_spring(scrollY, target, dt, NV_SPRING_SCROLL);
  }
}

// The text of a row's value. A static buffer because only one row is drawn at a
// time inside drawLine.
static const char *textValue(int op) {
  static char buf[48];
  const Option *o = &OPTIONS[op];
  if (o->kind == OP_READ || o->kind == OP_ACTION) return textRead(op);
  if (o->kind == OP_NUMBER) {
    if (op == SETTING_PAUSE_DELAY && value[op] == 0) return "Off";
    if (op == SETTING_HERO_TRAILER_SIZE && value[op] <= TRAILER_SAME) return "Same as backdrop";
    if (op == SETTING_HERO_TRAILER_DELAY) {
      snprintf(buf, sizeof buf, "%d.%d s", value[op] / 10, value[op] % 10);
      return buf;
    }
    snprintf(buf, sizeof buf, "%d%s", value[op], o->suffix ? o->suffix : "");
    return buf;
  }
  return o->values[value[op]];
}

// What an inactive row is waiting for, in the few words its value slot has room
// for. The side panel carries the whole sentence (helpOption).
static const char *needsOf(int op) {
  switch (op) {
    case SETTING_RAIL:           return "Modern sidebar off";
    case SETTING_RAIL_BLUR:      return "Modern sidebar";
    case SETTING_HERO_CATALOGS:  case SETTING_HERO_TRAILER: return "Show hero";
    case SETTING_HERO_TRAILER_SIZE: return settings_hero_trailer() ? "Top band" : "Trailer previews";
    case SETTING_HERO_TRAILER_DELAY: case SETTING_HERO_TRAILER_CW: return "Trailer previews";
    case SETTING_CW_BLUR_NEXT:   return settings_cw_on() ? "Episode thumbnail" : "Continue watching";
    case SETTING_CW_LOGO:        return settings_cw_on() ? "Card or Wide style" : "Continue watching";
    case SETTING_EXPAND_DELAY:   return "Expand poster";
    case SETTING_NEXT_COUNTDOWN: return "Autoplay";
    case SETTING_NEXT_SECONDS:   return "Before the end";
    case SETTING_NEXT_PERCENT:   return "Percentage watched";
    default:
      if (op > SETTING_CW_ON && op <= SETTING_CW_NOT_SHOWN) return "Continue watching";
      return "Depth effect";
  }
}

// Capitals for a kicker. The strings live in sentence case so the same names
// could serve elsewhere; the tracked capitals are the kicker's look only.
static const char *caps(const char *s) {
  static char b[64];
  size_t i;
  for (i = 0; s[i] && i < sizeof b - 1; i++)
    b[i] = (s[i] >= 'a' && s[i] <= 'z') ? (char)(s[i] - 'a' + 'A') : s[i];
  b[i] = 0;
  return b;
}

// A tracked-capitals label, and with `ruleTo` > 0 a hairline running from its end
// to that x on its baseline. Returns the label's height.
static float drawKicker(const char *s, float x, float y, float ruleTo, float a) {
  float w = txt_tracking(TXT_CWC_KICKER, caps(s), 128, 130, 136, x, y, a, 2.5f);
  if (ruleTo > x + w + 16.0f) {
    float by = y + txt_baseline(TXT_CWC_KICKER) - 2.0f;
    gfx_color((GfxRect){ x + w + 16.0f, by, ruleTo - (x + w + 16.0f), 1.0f }, 0.0f,
              1.0f, 1.0f, 1.0f, 0.08f * a);
  }
  return (float)txt_line(TXT_CWC_KICKER, caps(s), 128, 130, 136, 255).h;
}

// A row's opacity at `y`: it fades in under the title, and the rows past the
// bottom of the window fade out so the list reads as going on rather than cut.
// The focused row never fades — it is the one being read.
static float rowAlpha(float y, float f) {
  float a, low;
  if (y + SETTING_LINE_H < SETTING_TOP - 40.0f || y > NV_SCREEN_H) return 0.0f;
  a = anim_clamp((y - (SETTING_TOP - 70.0f)) / 60.0f, 0.0f, 1.0f);
  low = anim_clamp((NV_SCREEN_H - 20.0f - (y + SETTING_LINE_H * 0.5f)) / 110.0f, 0.0f, 1.0f);
  if (low < f) low = f;
  return a * low;
}

// Nothing at rest; a lifted plate and a white ring on focus. `hold` keeps the
// plate lit whatever the focus spring says (a picked-up Home row).
static void drawPlate(float y, float f, int focused, float hold, float a) {
  GfxRect line = { SETTING_LIST_X, y, SETTING_LIST_W, SETTING_LINE_H };
  float fill = hold > f ? hold : f;
  if (fill > 0.01f)
    gfx_color(line, SETTING_RADIUS, NV_COLOR_FOCUS_R, NV_COLOR_FOCUS_G, NV_COLOR_FOCUS_B,
              fill * a);
  if (focused)
    gfx_rect(line, 0, GFX_RING_INSET, 0, 2.5f / SETTING_LINE_H, 0,
             SETTING_RADIUS, 0.96f, 0.96f, 0.97f, a);
}

// The hairline under row i, when there is a row after it in the same group and
// neither of the two is lit — a line against a plate reads as a scratch on it.
static void drawRule(int i, int rows, int focusedRow, float y, float a) {
  if (i + 1 >= rows || groupOfRow(i + 1) || i == focusedRow || i + 1 == focusedRow) return;
  gfx_color((GfxRect){ SETTING_LIST_X + SETTING_PAD, y + SETTING_LINE_H - 0.5f,
                       SETTING_LIST_W - SETTING_PAD * 2.0f, 1.0f }, 0.0f,
            1.0f, 1.0f, 1.0f, 0.07f * a);
}

// The glyph beside a section group's header on level 0 (art/icons/set_*.png,
// tools/build-source-icons.sh). Keyed by the section that opens the group.
static const char *iconOfSection(int s) {
  switch (SECTIONS[s].start) {
    case SETTING_QUALITY:              return "set_playback";
    // Not the rail's menu_live: icons decode at their drawing width, and one
    // file asked for at 44 there and 20 here was re-decoded every frame.
    case SETTING_LIVE_ON:              return "set_live";
    case SETTING_HERO:                 return "set_home";
    case SETTING_RAIL:                 return "set_appearance";
    case SETTING_PROFILE_ACTIVE:       return "set_account";
    default:                           return NULL;
  }
}

// The header over row i, in the room yOfRow left for it.
static void drawGroup(int i, float y) {
  const char *g = groupOfRow(i);
  float a;
  if (!g) return;
  // The header fades as it reaches the list's top edge rather than being sliced
  // in half by the crop there.
  a = rowAlpha(y, 0.0f) * anim_clamp((y - SETTING_GROUP_H - SETTING_TOP + 16.0f) / 24.0f, 0.0f, 1.0f);
  if (a <= 0.005f) return;
  float x = SETTING_LIST_X + SETTING_PAD, ky = y - SETTING_GROUP_H + 22.0f;
  const char *icon = level == 0 ? iconOfSection(i) : NULL;
  if (icon) {
    // Centred on the kicker's line, in the kicker's grey.
    float h = (float)txt_line(TXT_CWC_KICKER, caps(g), 128, 130, 136, 255).h;
    gfx_icon((GfxRect){ x, ky + (h - SETTING_GROUP_ICON) * 0.5f, SETTING_GROUP_ICON, SETTING_GROUP_ICON },
             icon, 128 / 255.0f, 130 / 255.0f, 136 / 255.0f, a);
    x += SETTING_GROUP_ICON + 12.0f;
  }
  drawKicker(g, x, ky, SETTING_LIST_X + SETTING_LIST_W - SETTING_PAD, a);
}

// The row's label, left.
static float drawLabel(const char *s, float y, int c, float a) {
  TxtLine l = txt_line_trim(TXT_BODY, s, c, c, c + 2, 255, SETTING_LIST_W * 0.55f);
  txt_draw_alpha(l, SETTING_LIST_X + SETTING_PAD, y + (SETTING_LINE_H - l.h) * 0.5f, a);
  return (float)l.w;
}

// A value with the ‹ › that say left and right step it, ending at `xr`.
static void drawStepper(const char *v, float xr, float y, int c, float a, float f,
                        const char *lt, const char *gt) {
  TxtLine val = txt_line_trim(TXT_BODY, v, c, c, c + 2, 255, 360.0f);
  TxtLine l = txt_line(TXT_CALLOUT, lt, 210, 210, 214, 255);
  TxtLine g = txt_line(TXT_CALLOUT, gt, 210, 210, 214, 255);
  float gx = xr - g.w;
  float vx = gx - 26.0f - val.w;
  // The focused row's only: a row fading out still draws its arrows for a frame
  // or two, and must not move the target under the one that has the focus.
  if (f > 0.5f) { stepLtX0 = vx - 26.0f - l.w; stepLtX1 = vx - 26.0f; }
  txt_draw_alpha(g, gx, y + (SETTING_LINE_H - g.h) * 0.5f, a * f);
  txt_draw_alpha(l, vx - 26.0f - l.w, y + (SETTING_LINE_H - l.h) * 0.5f, a * f);
  txt_draw_alpha(val, vx, y + (SETTING_LINE_H - val.h) * 0.5f, a);
}

static void drawLine(int op, int i, int rows, float y, float f) {
  float a = rowAlpha(y, f);
  int focused = op == focusOp;
  if (a <= 0.005f) return;
  drawPlate(y, f, focused, 0.0f, a);
  drawRule(i, rows, focusOp - SECTIONS[focusSec].start, y, a);

  float xr = SETTING_LIST_X + SETTING_LIST_W - SETTING_PAD;
  const Option *o = &OPTIONS[op];

  // Inactive: the label greys out and the value slot says what it is waiting
  // for. The row stays in place — see inactive().
  if (inactive(op)) {
    char need[80];
    drawLabel(o->label, y, 236, a * 0.42f);
    TxtLine dash = txt_line(TXT_CAPTION, "\xe2\x80\x94", 120, 122, 128, 255);
    snprintf(need, sizeof need, "needs %s", needsOf(op));
    TxtLine n = txt_line_trim(TXT_CAPTION2, need, 128, 130, 136, 255, 360.0f);
    txt_draw_alpha(dash, xr - dash.w, y + (SETTING_LINE_H - dash.h) * 0.5f, a);
    txt_draw_alpha(n, xr - dash.w - 18.0f - n.w, y + (SETTING_LINE_H - n.h) * 0.5f, a);
    return;
  }

  drawLabel(o->label, y, readOnly(op) ? 190 : 236, a);
  const char *v = textValue(op);

  if (o->kind == OP_READ) {
    // Information: muted, and nothing on the row suggests it can be moved.
    TxtLine val = txt_line_trim(TXT_BODY, v, 150, 152, 158, 255, 420.0f);
    txt_draw_alpha(val, xr - val.w, y + (SETTING_LINE_H - val.h) * 0.5f, a);
    return;
  }
  if (o->kind == OP_ACTION) {
    // An action opens something: the chevron of a section row, not the ‹ ›.
    TxtLine chev = txt_line(TXT_CALLOUT, "\xe2\x80\xba", 150, 152, 158, 255);
    TxtLine val = txt_line_trim(TXT_BODY, v, 170, 172, 178, 255, 360.0f);
    txt_draw_alpha(chev, xr - chev.w, y + (SETTING_LINE_H - chev.h) * 0.5f, a * (0.6f + 0.4f * f));
    txt_draw_alpha(val, xr - chev.w - 24.0f - val.w, y + (SETTING_LINE_H - val.h) * 0.5f, a);
    return;
  }

  // A switch reads at a glance: On is bright, Off recedes. Any other value is
  // muted at rest and brightens with focus.
  int c = o->values == V_ON ? (value[op] == 0 ? 240 : 140) : 172;
  c = (int)(c + (244 - c) * f);

  if (o->kind == OP_NUMBER) {
    // The numeric row's fill bar. Without it, "28%" says nothing about where 28
    // sits in the range — and the web app shows a slider for exactly that reason.
    float t = (o->max > o->min) ? (float)(value[op] - o->min) / (float)(o->max - o->min) : 0.0f;
    float bw = 140.0f;
    float bx = xr - (f > 0.02f ? 44.0f : 0.0f) - bw;
    float by = y + SETTING_LINE_H - 13.0f;
    gfx_color((GfxRect){ bx, by, bw, 3.0f }, 0.5f, 1.0f, 1.0f, 1.0f, 0.16f * a);
    if (t > 0.004f)
      gfx_color((GfxRect){ bx, by, bw * anim_clamp(t, 0.0f, 1.0f), 3.0f }, 0.5f,
                0.94f, 0.94f, 0.96f, 0.85f * a);
    y -= 7.0f;
  }
  if (f > 0.02f) {
    drawStepper(v, xr, y, c, a, f, "\xe2\x80\xb9", "\xe2\x80\xba");
  } else {
    TxtLine val = txt_line_trim(TXT_BODY, v, c, c, c + 2, 255, 420.0f);
    txt_draw_alpha(val, xr - val.w, y + (SETTING_LINE_H - val.h) * 0.5f, a);
  }
}

// A Home row on level 2: the option row's look, the row's name on the left and
// whether it shows on the right. A held row trades the ‹ › for ▲ ▼ — the gesture
// that works on it has changed, and the arrows are the instruction.
static void drawHomeRow(int i, int rows, float y, float f) {
  float a = rowAlpha(y, f);
  if (a <= 0.005f) return;
  int shown = homerows_enabled(i);
  int over = homerows_over_cap(i);
  int held = rowHolding && i == rowFocus;
  drawPlate(y, f, i == rowFocus, held ? 1.0f : 0.0f, a);
  drawRule(i, rows, rowFocus, y, a);

  // A hidden row, or one past the cap, reads as muted: it is in the order but
  // not on the Home.
  drawLabel(homerows_title(i), y, 236, a * (shown && !over ? 1.0f : 0.5f));
  const char *v = held ? "Moving" : !shown ? "Hidden" : over ? "Won't fit" : "Shown";
  int c = held || (shown && !over) ? 240 : 140;
  float xr = SETTING_LIST_X + SETTING_LIST_W - SETTING_PAD;
  if (f > 0.02f) {
    drawStepper(v, xr, y, c, a, f, held ? "\xe2\x96\xb2" : "\xe2\x80\xb9",
                held ? "\xe2\x96\xbc" : "\xe2\x80\xba");
  } else {
    TxtLine val = txt_line(TXT_BODY, v, c, c, c + 2, 255);
    txt_draw_alpha(val, xr - val.w, y + (SETTING_LINE_H - val.h) * 0.5f, a);
  }
}

// A section's row on level 0: the title on the left and a chevron that says the
// row opens something. It carried a summary of the section's values too ("Auto
// quality · auto subtitles · …"); the owner found it confusing and it went.
static void drawSection(int s, float y, float f) {
  float a = rowAlpha(y, f);
  if (a <= 0.005f) return;
  drawPlate(y, f, s == focusSec, 0.0f, a);
  drawRule(s, SETTING_N_SECTIONS, focusSec, y, a);

  drawLabel(SECTIONS[s].title, y, 238, a);
  float xr = SETTING_LIST_X + SETTING_LIST_W - SETTING_PAD;
  int cc = (int)(120 + 110 * f);
  TxtLine chev = txt_line(TXT_CALLOUT, "\xe2\x80\xba", cc, cc, cc + 2, 255);
  txt_draw_alpha(chev, xr - chev.w, y + (SETTING_LINE_H - chev.h) * 0.5f, a);
}

// --- THE PREVIEW -------------------------------------------------------------
//
// A schematic of the screen the focused section or option changes, drawn from
// the live values: flip "Sidebar" and the rail in the picture widens, turn off
// "Show hero" and the rows slide up. Flat plates only — no textures, no text in
// the picture — so it costs a few dozen quads and nothing to load. What the
// focused option touches is outlined in the brand violet.

enum { PV_NONE, PV_HOME, PV_PLAYER, PV_DETAIL, PV_EPISODES };
enum { HL_NONE, HL_RAIL, HL_HERO, HL_BACKDROP, HL_TRAILER, HL_ROWS, HL_CW, HL_CARD,
       HL_BADGES, HL_SUBS, HL_SEEK, HL_NEXT, HL_BUTTONS, HL_EPISODES, HL_META };

static int sceneOf(int s) {
  switch (SECTIONS[s].start) {
    case SETTING_QUALITY:              return PV_PLAYER;
    case SETTING_LIVE_ON:              return PV_NONE;
    case SETTING_DET_TRAILER:          return PV_DETAIL;
    case SETTING_PROFILE_ACTIVE: case SETTING_VERSION_I: return PV_NONE;
    default:                           return PV_HOME;
  }
}

// Rows the port stores and syncs but does not act on yet: the web app reads
// them, this app does not. Their preview marks nothing, and the panel says so —
// a picture that changed for them would be promising an effect that is not there.
static int notWired(int op) {
  switch (op) {
    case SETTING_QUALITY: case SETTING_DV: case SETTING_ATMOS:
    case SETTING_RAIL_BLUR: case SETTING_DISCOVER: case SETTING_NAME_ADDON:
    case SETTING_SCORES_HOME: case SETTING_GRADIENT_CLASSIC:
    case SETTING_CW_THUMB: case SETTING_CW_BLUR_NEXT: case SETTING_CW_FURTHEST:
    case SETTING_CW_NOT_SHOWN: case SETTING_CW_ORDER:
    case SETTING_DET_BLUR_NOT_WATCHED: case SETTING_DET_META_EXT:
    case SETTING_WIDTH_DP: case SETTING_DEPTH_COVERAGE: case SETTING_DEPTH_EPS:
    case SETTING_DEPTH_CAST: case SETTING_DEPTH_TRAILERS:
      return 1;
    default: return 0;
  }
}

static int highlightOf(void) {
  if (level == 2) return HL_ROWS;
  if (level == 0 || notWired(focusOp)) return HL_NONE;
  switch (focusOp) {
    case SETTING_SUBS: case SETTING_SUBS_FORCED: return HL_SUBS;
    case SETTING_SEEK_COLOR: return HL_SEEK;
    case SETTING_NEXT_AUTOPLAY: case SETTING_NEXT_COUNTDOWN: case SETTING_NEXT_MODE:
    case SETTING_NEXT_SECONDS: case SETTING_NEXT_PERCENT: return HL_NEXT;
    case SETTING_HERO_FULL: case SETTING_HERO_AREA: case SETTING_HERO_BAND: return HL_BACKDROP;
    case SETTING_HERO_TRAILER_SIZE: return HL_TRAILER;
    case SETTING_RAIL: case SETTING_RAIL_MODERN: return HL_RAIL;
    case SETTING_HERO: case SETTING_HERO_CATALOGS: return HL_HERO;
    case SETTING_HERO_TRAILER: case SETTING_HERO_TRAILER_DELAY:
    case SETTING_HERO_TRAILER_CW: return HL_TRAILER;
    case SETTING_DET_TRAILER: return HL_BUTTONS;
    case SETTING_DET_DATE_FULL: return HL_META;
    case SETTING_LANDSCAPE: case SETTING_LABELS: case SETTING_SUFFIX_KIND:
    case SETTING_HIDE_UNRELEASED: return HL_ROWS;
    case SETTING_AUDIO_LANG: case SETTING_AUDIO_ANIME: case SETTING_ANIM:
    case SETTING_PAUSE_DELAY: case SETTING_LIVE_ON: case SETTING_LIVE_SOURCE: case SETTING_LIVE_BUFFER:
    case SETTING_LIVE_PREVIEW: return HL_NONE;
    default:
      if (focusOp >= SETTING_CW_ON && focusOp <= SETTING_CW_NOT_SHOWN) return HL_CW;
      return HL_CARD;
  }
}

// The scenes are laid out in the SCREEN'S OWN 1920x1080 coordinates — the same
// numbers home.c, menu.c and player.c draw with — and scaled into the box by `k`.
// A preview drawn from invented proportions is a picture of a different app.
typedef struct { GfxRect box; float k; } Pv;
static GfxRect pvR(Pv v, float x, float y, float w, float h) {
  return (GfxRect){ v.box.x + x * v.k, v.box.y + y * v.k, w * v.k, h * v.k };
}

// A plate in the picture, `lum` grey; the radius in screen px.
static void pvPlate(Pv v, float x, float y, float w, float h, float radiusPx, float lum, float a) {
  GfxRect r = pvR(v, x, y, w, h);
  float s = r.w < r.h ? r.w : r.h;
  if (r.w <= 0.5f || r.h <= 0.5f) return;
  gfx_color(r, anim_clamp(radiusPx * v.k / s, 0.0f, 0.5f), lum, lum, lum + 0.01f, a);
}

// The box's own ground colour, laid over the art in steps: how the hero's art
// dissolves into the page. `dir` 0 fades towards the left edge, 1 towards the bottom.
#define PV_BG 0.078f
static void pvFade(Pv v, GfxRect art, int dir, float span) {
  int n = 32, i;
  for (i = 0; i < n; i++) {
    float t = (float)i / n, a = (1.0f - t) * (1.0f - t);
    if (dir == 0)
      pvPlate(v, art.x + span * t, art.y, span / n + 1.0f, art.h, 0.0f, PV_BG, a);
    else
      pvPlate(v, art.x, art.y + art.h - span * t - span / n, art.w, span / n + 1.0f, 0.0f, PV_BG, a);
  }
}

// What the focused option touches: a violet ring and a faint violet wash.
static void pvMark(Pv v, GfxRect s) {
  GfxRect r = pvR(v, s.x, s.y, s.w, s.h);
  GfxRect o = { r.x - 3.0f, r.y - 3.0f, r.w + 6.0f, r.h + 6.0f };
  float m = o.w < o.h ? o.w : o.h;
  float rad = anim_clamp(6.0f / m, 0.0f, 0.5f);
  if (m <= 1.0f) return;
  gfx_color(o, rad, SETTING_ACCENT_R, SETTING_ACCENT_G, SETTING_ACCENT_B, 0.12f);
  gfx_rect(o, 0, GFX_RING_INSET, 0, 2.0f / m, 0, rad,
           SETTING_ACCENT_R, SETTING_ACCENT_G, SETTING_ACCENT_B, 0.95f);
}

// A soft light where the artwork's subject would be.
static void pvGlow(Pv v, float x, float y, float blur, float a) {
  GfxRect r = pvR(v, x, y, 0, 0);
  gfx_glow((GfxRect){ r.x - 2.0f, r.y - 2.0f, 4.0f, 4.0f }, 2.0f, blur * v.k, 1.0f, 1.0f, 1.0f, a);
}

// Placeholder copy in the picture: the screen's own style at its own size times
// `k`, so the lines keep the proportions they have to each other on the screen.
static TxtLine pvLine(Pv v, TxtStyle st, const char *s, float px, int lum) {
  return txt_line_px(st, s, px * v.k, lum, lum, lum, 255);
}

// A line of placeholder copy with its corner at a point in SCREEN coordinates.
static void pvWrite(Pv v, TxtStyle st, const char *s, float px, int lum,
                    float x, float y, float a) {
  GfxRect r = pvR(v, x, y, 0, 0);
  txt_draw_alpha(pvLine(v, st, s, px, lum), r.x, r.y, a);
}

// The Home as home.c lays it out: the hero's art (banded at 555,0 1421x670, the
// whole screen, or the top-right band), its copy from y 187, the rows' viewport
// from NV_SHELF_TOP whether the hero is on or not, and the fixed rail (menu.c)
// only when the sidebar is not collapsed.
static void drawHomeScene(Pv v, int hl) {
  int hero = settings_hero_on(), full = settings_hero_full(), band = settings_hero_top_band();
  float cx0 = settings_content_x();
  GfxRect railR = { 8.0f, 8.0f, 72.0f, 1064.0f }, heroR = { 0 }, artR = { 0 };
  GfxRect rowsR, cwR = { 0 }, cardR = { 0 };

  if (hero) {
    if (!full) artR = (GfxRect){ NV_HERO_ART_X, 0, NV_HERO_ART_W, NV_HERO_ART_H };
    else if (!band) artR = (GfxRect){ 0, 0, NV_SCREEN_W, NV_SCREEN_H };
    else {
      float w = NV_SCREEN_W * settings_hero_band_scale(), h = w / NV_HERO_FIT_ASP;
      if (h > NV_SCREEN_H) { h = NV_SCREEN_H; w = h * NV_HERO_FIT_ASP; }
      artR = (GfxRect){ NV_SCREEN_W - w, 0, w, h };
      // TRAILER SIZE shows the trailer's band in the art's place, as home.c's
      // heroTrailerRect builds it: 16:9 from the same top-right corner, or the
      // art's own band while it is "Same as backdrop".
      if (hl == HL_TRAILER && settings_hero_trailer_scale() > 0.0f) {
        w = NV_SCREEN_W * settings_hero_trailer_scale(); h = w * 9.0f / 16.0f;
        if (h > NV_SCREEN_H) { h = NV_SCREEN_H; w = h * 16.0f / 9.0f; }
        artR = (GfxRect){ NV_SCREEN_W - w, 0, w, h };
      }
    }
    pvPlate(v, artR.x, artR.y, artR.w, artR.h, 0.0f, 0.19f, 1.0f);
    pvGlow(v, artR.x + artR.w * 0.62f, artR.y + artR.h * 0.36f, artR.h * 0.55f, 0.16f);
    if (artR.x > 0.0f) pvFade(v, artR, 0, artR.w * 0.35f);
    pvFade(v, artR, 1, artR.h * (full && !band ? 0.55f : 0.4f));

    // The copy, stacked bottom-up as home.c stacks it, from 48 above the first
    // row's title — the same place whatever the backdrop does: four lines of
    // synopsis (24/400 on 35), the meta line (21/500 at rgb 179) 16 above, and
    // the logo 24 above that — here the name set as a wordmark in its place.
    { static const char *SIN[] = {
        "A placeholder synopsis for the featured title. It runs",
        "to a few lines, the way a real description does, so",
        "the copy here takes the room it takes on the Home",
        "itself." };
      float base = NV_SHELF_TOP + NV_SHELF_PAD_TOP - NV_HERO_COPY_GAP;
      float ySin = base - 4 * 35.0f, yMeta = ySin - 16.0f - 26.0f, logoFoot = yMeta - 24.0f;
      int l;
      TxtLine logo = pvLine(v, TXT_TITLE1, "Title Logo", 92.0f, 255);
      GfxRect at = pvR(v, cx0, logoFoot, 0, 0);
      txt_draw(logo, at.x, at.y - logo.h);
      pvWrite(v, TXT_HERO_META, "2024  \xc2\xb7  Drama  \xc2\xb7  2 Seasons", 21.0f, 179,
              cx0, yMeta, 1.0f);
      for (l = 0; l < 4; l++)
        pvWrite(v, TXT_HERO_SIN, SIN[l], 24.0f, 255, cx0, ySin + l * 35.0f, 0.85f);
      heroR = (GfxRect){ cx0, logoFoot - NV_LOGO_HERO_H, NV_HERO_SIN_W, base - logoFoot + NV_LOGO_HERO_H }; }
  }

  // The rows: the viewport starts at NV_SHELF_TOP + its padding; each row is its
  // title then the cards. Continue watching leads when it is on, in 419x236
  // cards unless its style is Poster. The focus sits on its first card — or, for
  // an option about poster cards, on the first poster row's.
  { float y = NV_SHELF_TOP + NV_SHELF_PAD_TOP;
    int land = settings_posters_landscape(), labels = settings_labels_poster();
    int cwRow0 = settings_cw_on() && settings_cw_style() != 2;
    int focusRow = (hl == HL_CARD && cwRow0) ? 1 : 0, r, catalog = 0;
    float rad = settings_radius_poster_px();
    rowsR = (GfxRect){ cx0, y, NV_SCREEN_W - cx0 - 16.0f, NV_SCREEN_H - y - 16.0f };
    for (r = 0; y < NV_SCREEN_H && r < 5; r++) {
      int cw = r == 0 && cwRow0;
      float w = cw ? NV_HIGHLIGHT_W : land ? NV_CARD_LAND_W : NV_CARD_W;
      float h = cw ? NV_HIGHLIGHT_H : land ? NV_CARD_LAND_H : NV_CARD_H;
      float x = cx0, cy = y + NV_LEGACY_ROW_HEAD_H;
      int c;
      // The row's heading, 28/600: " - Movie" only while the type suffix is on.
      { char t[48];
        if (!cw) catalog++;
        if (cw) snprintf(t, sizeof t, "Continue Watching");
        else snprintf(t, sizeof t, "Catalog Row %d%s", catalog,
                      settings_suffix_kind() ? (catalog % 2 ? " - Movie" : " - Series") : "");
        pvWrite(v, TXT_ROW_TITLE, t, 28.0f, 246, cx0, y, 1.0f); }
      for (c = 0; x < NV_SCREEN_W && c < 12; c++) {
        int focus = r == focusRow && c == 0;
        float ww = w;
        if (focus && !cw && !land && settings_expand_poster()) ww = h * 16.0f / 9.0f;
        pvPlate(v, x, cy, ww, h, rad, focus ? 0.32f : 0.18f, 1.0f);
        if (cw) {
          // The resume card's copy, bottom-up from 22 above its base as resume.c
          // builds it: the episode's name (21, dimmed), the title's logo or its
          // name (30/600), the "S1 E3" kicker (17) — then the progress bar.
          float base = cy + h - 22.0f;
          pvWrite(v, TXT_CWC_SUB, "Episode Name", 21.0f, 255, x + 24.0f, base - 27.0f, 0.62f);
          base -= 30.0f;
          if (settings_cw_logo()) {
            pvWrite(v, TXT_TITLE1, "Title Logo", 44.0f, 255, x + 24.0f, base - 58.0f, 1.0f);
            base -= 58.0f;
          } else {
            pvWrite(v, TXT_CWC_TITLE, "Series Title", 30.0f, 255, x + 24.0f, base - 37.0f, 1.0f);
            base -= 37.0f;
          }
          pvWrite(v, TXT_CWC_KICKER, "S1 E3", 17.0f, 255, x + 24.0f, base - 26.0f, 0.62f);
          pvPlate(v, x, cy + h - 6.0f, ww * 0.45f, 6.0f, 0.0f, 0.92f, 0.9f);
        }
        // Labels are the landscape card's only: its name (22) and genre (15)
        // inside the frame, 14 in and 12 up. home.c draws none under a poster.
        if (labels && land && !cw) {
          pvWrite(v, TXT_CAPTION, "Title", 22.0f, 245, x + 14.0f, cy + h - 12.0f - 20.0f - 4.0f - 28.0f, 0.98f);
          pvWrite(v, TXT_MINI, "Genre", 15.0f, 200, x + 14.0f, cy + h - 12.0f - 20.0f, 0.85f);
        }
        if (focus) {
          GfxRect fr = pvR(v, x, cy, ww, h);
          float m = fr.w < fr.h ? fr.w : fr.h;
          float rf = anim_clamp(rad * v.k / m, 0.0f, 0.5f);
          if (settings_depth() && (cw ? settings_depth_cw() : settings_depth_posters()))
            gfx_card_depth(fr, rf, settings_depth_border(), settings_depth_brightness(), 18.0f * v.k);
          gfx_rect(fr, 0, GFX_RING_INSET, 0, 1.5f / m, 0, rf, 0.96f, 0.96f, 0.97f, 1.0f);
          cardR = (GfxRect){ x, cy, ww, h };
        }
        x += ww + 24.0f;
      }
      if (cw) cwR = (GfxRect){ cx0, y, NV_SCREEN_W - cx0 - 16.0f, NV_LEGACY_ROW_HEAD_H + h };
      // A poster option touches the catalogue rows only, not the resume row.
      else if (level == 1 && rowsR.y < y) rowsR = (GfxRect){ cx0, y, rowsR.w, NV_SCREEN_H - y - 16.0f };
      y = cy + h + (cw || land ? NV_ROW_GAP_LAND : NV_ROW_GAP);
    } }

  // The fixed rail: frosted glass 144 wide, menu.c's four glyphs centred down
  // it on 116px lines — Home filled and lit, the rest at half — the profile at the foot.
  if (!settings_rail_collapsed()) {
    static const char *ICON[] = { "menu_home_fill", "menu_search", "menu_library", "menu_settings" };
    int i;
    float y0 = (NV_SCREEN_H - 4 * 116.0f) * 0.5f;
    railR = (GfxRect){ 0, 0, NV_LEGACY_RAIL_W, NV_SCREEN_H };
    pvPlate(v, 0, 0, NV_LEGACY_RAIL_W, NV_SCREEN_H, 0.0f, 0.16f, 0.92f);
    for (i = 0; i < 4; i++)
      gfx_icon(pvR(v, 72.0f - 22.0f, y0 + i * 116.0f + 36.0f, 44.0f, 44.0f), ICON[i],
               1.0f, 1.0f, 1.0f, i == 0 ? 1.0f : 0.5f);
    pvPlate(v, 72.0f - 24.0f, NV_SCREEN_H - 96.0f, 48.0f, 48.0f, 24.0f, 0.3f, 1.0f);
  }

  switch (hl) {
    case HL_RAIL:
      if (!settings_rail_collapsed()) { pvMark(v, railR); break; }
      // Collapsed there is no bar to point at: the line is where the content
      // starts instead, with the inset written beside it.
      { GfxRect l = pvR(v, cx0 - 20.0f, 16.0f, 0, NV_SCREEN_H - 32.0f);
        char t[24];
        gfx_color((GfxRect){ l.x - 1.0f, l.y, 2.0f, l.h }, 0.0f,
                  SETTING_ACCENT_R, SETTING_ACCENT_G, SETTING_ACCENT_B, 0.95f);
        snprintf(t, sizeof t, "%d PX", (int)cx0);
        txt_tracking(TXT_MINI, t, 160, 140, 255, l.x + 6.0f, l.y + 2.0f, 1.0f, 1.5f); }
      break;
    case HL_HERO:     pvMark(v, hero ? heroR : (GfxRect){ cx0, 150.0f, 720.0f, 300.0f }); break;
    case HL_BACKDROP: case HL_TRAILER:
      if (hero) pvMark(v, (GfxRect){ artR.x + 8, artR.y + 8, artR.w - 16, artR.h - 16 }); break;
    case HL_ROWS:     pvMark(v, rowsR); break;
    case HL_CW:       pvMark(v, settings_cw_on() && cwR.w > 0 ? cwR : rowsR); break;
    case HL_CARD:     if (cardR.w > 0) pvMark(v, cardR); break;
  }
}

// The player with its controls up, as player.c lays it out: the title block
// over the bar, the bar 96px in from each side at y 882, the button row under
// it. With an Up next option focused, the card player.c raises above the
// controls, bottom-right.
//
// The copy is placeholder words at the player's sizes (layout.h: title 56,
// episode line 30, subtitles at the default 120% = 48, the card's 17/28/23).
// The stream's facts, the clock and the time readout are left out: they would
// be words no option here changes.
static void drawPlayerScene(Pv v, int hl) {
  // rowButtons() for an episode with a next one: every button the row can hold,
  // in its order, with the glyph player.c draws — pause, since it is playing.
  static const char *ICONS[] = { "pause", "skip_next", "subtitles", "audio", "episodes",
                                 "stack", "aspect", "stats", "details" };
  float r, g, bl;
  int i;
  const float barX = 96.0f, barW = NV_SCREEN_W - 192.0f, barY = 882.0f;
  GfxRect subR = { 560.0f, 604.0f, 800.0f, 104.0f }, nextR = { 0 };
  settings_seek_color(&r, &g, &bl);

  pvGlow(v, 960.0f, 380.0f, 520.0f, 0.12f);
  pvFade(v, (GfxRect){ 0, 560.0f, NV_SCREEN_W, 520.0f }, 1, 520.0f);

  // Subtitles, when the player would turn one on by itself: a line pair, or a
  // lone short line for a forced track — centred, their foot at the 700 the
  // player lifts them to while the controls are up.
  // Not under the Up next card: the player hides the subtitles' band behind it.
  if (hl != HL_NEXT && (value[SETTING_SUBS] || settings_subtitle_forced())) {
    const char *l1 = value[SETTING_SUBS] ? "We should have left an hour ago." : NULL;
    const char *l2 = value[SETTING_SUBS] ? "Nobody ever listens to me." : "Over here!";
    GfxRect foot = pvR(v, 960.0f, 700.0f, 0, 0);
    TxtLine lb = pvLine(v, TXT_SUB_120, l2, 48.0f, 255);
    float y = foot.y - lb.h;
    txt_draw_shadow(lb, foot.x - lb.w * 0.5f, y + 1.0f, 0.8f);
    txt_draw(lb, foot.x - lb.w * 0.5f, y);
    if (l1) {
      TxtLine lt = pvLine(v, TXT_SUB_120, l1, 48.0f, 255);
      txt_draw_shadow(lt, foot.x - lt.w * 0.5f, y - lt.h + 1.0f, 0.8f);
      txt_draw(lt, foot.x - lt.w * 0.5f, y - lt.h);
    }
  }

  // The name, then the episode line in its two weights, on the bar's 40px gap.
  { GfxRect base = pvR(v, barX, barY - 40.0f, 0, 0);
    TxtLine lc = pvLine(v, TXT_PLR_EPCODE, "S1 E3", 30.0f, 255);
    TxtLine ln = pvLine(v, TXT_PLR_EPNAME, "Episode Name", 30.0f, 255);
    TxtLine lt = pvLine(v, TXT_PLR_TITLE, "Series Title", 56.0f, 255);
    float ye = base.y - (lc.h > ln.h ? lc.h : ln.h);
    txt_draw_alpha(lc, base.x, ye, 0.55f);
    txt_draw_alpha(ln, base.x + lc.w + 16.0f * v.k, ye, 0.92f);
    txt_draw(lt, base.x, ye - 8.0f * v.k - lt.h); }

  // The bar: neutral track, the played part and (focused) the knob in the colour.
  pvPlate(v, barX, barY, barW, 8.0f, 4.0f, 1.0f, 0.26f);
  { GfxRect f = pvR(v, barX, barY, barW * 0.42f, 8.0f);
    GfxRect kn = pvR(v, barX + barW * 0.42f - 15.0f, barY + 4.0f - 15.0f, 30.0f, 30.0f);
    gfx_color(f, 0.5f, r, g, bl, 1.0f);
    if (hl == HL_SEEK) gfx_color(kn, 0.5f, r, g, bl, 1.0f); }

  // The button row, from the bar's left edge, 90px circles 14 apart: the glyphs
  // bare, the focused one on its white puck — player.c draws no circle under the rest.
  for (i = 0; i < (int)(sizeof ICONS / sizeof *ICONS); i++) {
    float cx = barX + 45.0f + i * 104.0f, cy = 971.0f;
    float lum = i == 0 ? 0.13f : 0.94f;
    if (i == 0) pvPlate(v, cx - 45.0f, cy - 45.0f, 90.0f, 90.0f, 45.0f, 1.0f, 1.0f);
    gfx_icon(pvR(v, cx - 24.0f, cy - 24.0f, 48.0f, 48.0f), ICONS[i], lum, lum, lum, 0.94f);
  }

  // The card, as drawNextCard builds it: the still, "UP NEXT · Playing in Ns"
  // (the count only with autoplay on), the episode's title, the two pills — and
  // as wide as its copy, anchored to the raised prompt corner.
  if (hl == HL_NEXT) {
    char count[32];
    TxtLine kick, dot = { 0 }, cnt = { 0 }, code, sep, name, play, notNow;
    float k = v.k, titleW, pillPlay, pillNot, rowW, copyW, w, h, x, y, cx, py, ph;
    snprintf(count, sizeof count, "Playing in %ds", settings_next_countdown());
    kick = pvLine(v, TXT_NEXT_KICK, "UP NEXT", 17.0f, 150);
    if (settings_next_autoplay()) {
      dot = pvLine(v, TXT_NEXT_COUNT, "\xc2\xb7", 17.0f, 82);
      cnt = pvLine(v, TXT_NEXT_COUNT, count, 17.0f, 255);
    }
    code = pvLine(v, TXT_NEXT_TITLE, "S1 E4", 28.0f, 255);
    sep  = pvLine(v, TXT_NEXT_TITLE, "\xc2\xb7", 28.0f, 150);
    name = pvLine(v, TXT_NEXT_TITLE, "Next Episode", 28.0f, 255);
    play = pvLine(v, TXT_NEXT_PILL, "Play now", 23.0f, 226);
    notNow = pvLine(v, TXT_NEXT_PILL, "Not now", 23.0f, 226);
    titleW = code.w + sep.w + name.w + 24.0f * k;
    pillPlay = (28.0f * 2 + 18.0f * 0.6875f + 12.0f) * k + play.w;
    pillNot = 28.0f * 2 * k + notNow.w;
    rowW = pillPlay + 12.0f * k + pillNot;
    copyW = titleW > rowW ? titleW : rowW;
    w = (24.0f * 2 + 148.0f * 16.0f / 9.0f + 24.0f) * k + copyW;
    h = (24.0f * 2 + 148.0f) * k;
    x = v.box.x + (NV_SCREEN_W - 64.0f) * k - w;
    y = v.box.y + (NV_SCREEN_H - 236.0f) * k - h;
    nextR = (GfxRect){ (x - v.box.x) / k, (y - v.box.y) / k, w / k, h / k };
    pvPlate(v, nextR.x, nextR.y, nextR.w, nextR.h, 20.0f, 0.043f, 0.97f);
    pvPlate(v, nextR.x + 24.0f, nextR.y + 24.0f, 148.0f * 16.0f / 9.0f, 148.0f, 12.0f, 0.28f, 1.0f);
    cx = x + (24.0f + 148.0f * 16.0f / 9.0f + 24.0f) * k;
    y += 24.0f * k;
    txt_draw(kick, cx, y);
    if (cnt.tex) {
      txt_draw(dot, cx + kick.w + 6.0f * k, y);
      txt_draw(cnt, cx + kick.w + (16.0f) * k + dot.w, y);
    }
    txt_draw(code, cx, y + 28.0f * k);
    txt_draw(sep, cx + code.w + 12.0f * k, y + 28.0f * k);
    txt_draw(name, cx + code.w + sep.w + 24.0f * k, y + 28.0f * k);
    ph = 56.0f * k;
    py = y + (148.0f - 56.0f) * k;
    { GfxRect p1 = { cx, py, pillPlay, ph }, p2 = { cx + pillPlay + 12.0f * k, py, pillNot, ph };
      float ib = 18.0f * k;
      gfx_color(p1, 0.5f, 0.110f, 0.114f, 0.129f, 1.0f);
      gfx_color(p2, 0.5f, 0.110f, 0.114f, 0.129f, 1.0f);
      gfx_rect(p1, 0, GFX_RING_INSET, 0, 1.5f / ph, 0, 0.5f, 0.227f, 0.235f, 0.259f, 1.0f);
      gfx_rect(p2, 0, GFX_RING_INSET, 0, 1.5f / ph, 0, 0.5f, 0.227f, 0.235f, 0.259f, 1.0f);
      gfx_icon((GfxRect){ p1.x + 28.0f * k - ib * 0.25f, py + (ph - ib) * 0.5f, ib, ib },
               "play", 0.886f, 0.886f, 0.886f, 1.0f);
      txt_draw(play, p1.x + (28.0f + 18.0f * 0.6875f + 12.0f) * k, py + (ph - play.h) * 0.5f);
      txt_draw(notNow, p2.x + 28.0f * k, py + (ph - notNow.h) * 0.5f); }
  }

  switch (hl) {
    case HL_SUBS: pvMark(v, subR); break;
    case HL_SEEK: pvMark(v, (GfxRect){ barX, barY - 11.0f, barW, 30.0f }); break;
    case HL_NEXT: pvMark(v, nextR); break;
  }
}

// A white focus ring `wPx` wide round a rect in screen coordinates, outside it,
// with `radius` a fraction of the ringed rect's height.
static void pvRing(Pv v, GfxRect s, float wPx, float radius) {
  GfxRect r = pvR(v, s.x - wPx, s.y - wPx, s.w + wPx * 2.0f, s.h + wPx * 2.0f);
  if (r.h <= 1.0f) return;
  gfx_rect(r, 0, GFX_RING_INSET, 0, wPx * v.k / r.h, 0, radius, 1.0f, 1.0f, 1.0f, 1.0f);
}

// The separators of detail.c's meta lines: a 2x18 bar (128) with 30 either side
// between groups, a 6px dot with 11 either side inside one. Both return the new x.
static float pvBar(Pv v, float x, float yc) {
  pvPlate(v, x + 30.0f, yc - 9.0f, 2.0f, 18.0f, 0.0f, 0.5f, 1.0f);
  return x + 62.0f;
}
static float pvDot(Pv v, float x, float yc) {
  pvPlate(v, x + 11.0f, yc - 3.0f, 6.0f, 6.0f, 3.0f, 0.5f, 1.0f);
  return x + 28.0f;
}
// A line of copy on the meta line's centre `yc`, returning where it ends — in
// screen coordinates, so the separators can follow it.
static float pvMeta(Pv v, TxtStyle st, const char *s, float px, int lum, float x, float yc) {
  TxtLine l = pvLine(v, st, s, px, lum);
  GfxRect r = pvR(v, x, yc, 0, 0);
  txt_draw(l, r.x, r.y - l.h * 0.5f);
  return x + l.w / v.k;
}

// A series' page as detail.c stacks it, bottom-up from its base at 1048 over the
// full-screen backdrop: the logo 98 above the actions, the row of buttons (Play
// focused, then List, Sources and — when it is on — Trailer), "Writer:", three
// lines of synopsis (26 on 40), then the two meta lines.
static void drawDetailScene(Pv v, int hl) {
  static const char *SIN[] = {
    "A placeholder synopsis for the title. It runs to a few lines,",
    "the way a real description does, so the column takes the room",
    "it takes on the page itself." };
  const float x0 = 96.0f;
  float yMeta2 = 1048.0f - 49.0f, yMeta1 = yMeta2 - 31.0f - 30.0f;
  float ySin = yMeta1 - 33.0f - 3 * 40.0f, ySup = ySin - 62.0f;
  float yAct = ySup - 37.0f - 96.0f, x, k = v.k;
  GfxRect art = { 0, 0, NV_SCREEN_W, NV_SCREEN_H }, btnR;
  int l;

  pvPlate(v, art.x, art.y, art.w, art.h, 0.0f, 0.19f, 1.0f);
  pvGlow(v, art.w * 0.68f, 330.0f, 480.0f, 0.14f);
  pvFade(v, (GfxRect){ 0, 0, 1300.0f, NV_SCREEN_H }, 0, 1300.0f);
  pvFade(v, art, 1, 560.0f);

  // The logo — the name set as a wordmark — on its base 98 above the actions.
  { TxtLine lg = pvLine(v, TXT_TITLE1, "Title Logo", 120.0f, 255);
    GfxRect at = pvR(v, x0, yAct - 98.0f, 0, 0);
    txt_draw(lg, at.x, at.y - lg.h); }

  // The actions: the primary pill (padding 36, the 36px glyph, 24, the label 32/600,
  // 36) focused in #f5f5f5 with #111 ink and a 4px white ring, then 96px circles
  // in #222 with 44px white glyphs, 24 apart.
  { TxtLine lp = pvLine(v, TXT_DETWEB_BTN, "Play", 32.0f, 17);
    float pw = 36.0f + 36.0f + 24.0f + lp.w / k + 36.0f;
    static const char *ICON[] = { "detail_library_add", "detail_stack", "detail_trailer" };
    int n = settings_button_trailer() ? 3 : 2, i;
    GfxRect p = { x0, yAct, pw, 96.0f }, t = pvR(v, x0 + 36.0f + 36.0f + 24.0f, yAct, 0, 96.0f);
    pvPlate(v, p.x, p.y, p.w, p.h, 48.0f, 0.961f, 1.0f);
    pvRing(v, p, 4.0f, 0.5f);
    gfx_icon(pvR(v, x0 + 36.0f, yAct + 30.0f, 36.0f, 36.0f), "detail_play", 0.067f, 0.067f, 0.067f, 1.0f);
    txt_draw(lp, t.x, t.y + (t.h - lp.h) * 0.5f);
    x = x0 + pw + 24.0f;
    for (i = 0; i < n; i++, x += 96.0f + 24.0f) {
      pvPlate(v, x, yAct, 96.0f, 96.0f, 48.0f, 0.133f, 1.0f);
      gfx_icon(pvR(v, x + 26.0f, yAct + 26.0f, 44.0f, 44.0f), ICON[i], 1.0f, 1.0f, 1.0f, 1.0f);
    }
    btnR = (GfxRect){ x0, yAct, x - 24.0f - x0, 96.0f }; }

  // "Writer:" at 128 and the name at 179, both 26/400 like the synopsis.
  { TxtLine lr = pvLine(v, TXT_DET_SIN, "Writer: ", 26.0f, 128);
    TxtLine ln = pvLine(v, TXT_DET_SIN, "Writer Name", 26.0f, 179);
    GfxRect at = pvR(v, x0, ySup, 0, 0);
    txt_draw(lr, at.x, at.y);
    txt_draw(ln, at.x + lr.w, at.y); }
  for (l = 0; l < 3; l++) pvWrite(v, TXT_DET_SIN, SIN[l], 26.0f, 255, x0, ySin + l * 40.0f, 1.0f);

  // Meta line 1: genres (a dot between them) | year | IMDb score.
  { float yc = yMeta1 + 15.0f;
    x = pvMeta(v, TXT_DET_SIN, "Drama", 26.0f, 179, x0, yc);
    x = pvDot(v, x, yc);
    x = pvMeta(v, TXT_DET_SIN, "Thriller", 26.0f, 179, x, yc);
    x = pvBar(v, x, yc);
    x = pvMeta(v, TXT_DET_SIN, "2024", 26.0f, 179, x, yc);
    x = pvBar(v, x, yc);
    pvMeta(v, TXT_DET_SIN, "IMDb 8.1", 26.0f, 179, x, yc); }
  // Meta line 2: the status in its outline badge (49 tall, radius 8, 2px), then
  // duration and country.
  { float yc = yMeta2 + 24.5f;
    TxtLine lb = pvLine(v, TXT_DET_SIN, "Returning Series", 26.0f, 255);
    GfxRect b = { x0, yMeta2, 16.0f * 2 + lb.w / k, 49.0f }, br = pvR(v, b.x, b.y, b.w, b.h);
    gfx_rect(br, 0, GFX_RING_INSET, 0, 2.0f * k / br.h, 0, 8.0f / 49.0f, 1.0f, 1.0f, 1.0f, 0.5f);
    txt_draw(lb, br.x + 16.0f * k, br.y + (br.h - lb.h) * 0.5f);
    x = pvBar(v, b.x + b.w, yc);
    x = pvMeta(v, TXT_DET_SIN, "52min", 26.0f, 179, x, yc);
    x = pvBar(v, x, yc);
    pvMeta(v, TXT_DET_SIN, "United States", 26.0f, 179, x, yc); }

  if (hl == HL_BUTTONS) pvMark(v, btnR);
}

// The page under the hero: the season picker (an 80px pill, "Season 1 · 8 Eps"
// and its chevron) 56 from the top, then the episode rows, 228 apart — the 320x180
// still, and beside it at 472 the "EP 1" kicker, the name (34/600), the meta line
// (23/400: duration · the date, spelled out or the year alone) and the synopsis.
static void drawEpisodesScene(Pv v, int hl) {
  static const char *DATE[] = { "9 August 2025", "16 August 2025", "23 August 2025", "30 August 2025" };
  const float x0 = 96.0f, tx = 96.0f + 320.0f + 56.0f;
  float y = 56.0f + 80.0f + 24.0f, k = v.k;
  GfxRect metaR = { 0 };
  int i;

  pvGlow(v, 1500.0f, 200.0f, 500.0f, 0.06f);

  { TxtLine ls = pvLine(v, TXT_DETWEB_SEA, "Season 1", 30.0f, 255);
    TxtLine le = pvLine(v, TXT_DETWEB_SEA_EPS, " \xc2\xb7 8 Eps", 30.0f, 179);
    float w = 36.0f + (ls.w + le.w) / k + 24.0f + 32.0f + 36.0f;
    GfxRect p = pvR(v, x0, 56.0f, w, 80.0f);
    gfx_color(p, 0.5f, 0.133f, 0.133f, 0.14f, 1.0f);
    gfx_rect(p, 0, GFX_RING_INSET, 0, 1.0f / p.h, 0, 0.5f, 1.0f, 1.0f, 1.0f, 0.12f);
    txt_draw(ls, p.x + 36.0f * k, p.y + (p.h - ls.h) * 0.5f);
    txt_draw(le, p.x + 36.0f * k + ls.w, p.y + (p.h - le.h) * 0.5f);
    gfx_icon(pvR(v, x0 + w - 36.0f - 32.0f, 56.0f + 24.0f, 32.0f, 32.0f), "chevron_down",
             0.702f, 0.702f, 0.702f, 1.0f); }

  for (i = 0; i < 4 && y < NV_SCREEN_H; i++, y += 228.0f) {
    char code[8], meta[48];
    float xm;
    snprintf(code, sizeof code, "EP %d", i + 1);
    snprintf(meta, sizeof meta, "52min");
    pvPlate(v, x0, y + 24.0f, 320.0f, 180.0f, 12.0f, 0.18f, 1.0f);
    pvWrite(v, TXT_DETWEB_EP_BADGE, code, 21.0f, 255, tx, y + 34.0f, 0.55f);
    pvWrite(v, TXT_DETWEB_EP_TITLE, "Episode Name", 34.0f, 255, tx, y + 62.0f, 1.0f);
    xm = pvMeta(v, TXT_DETWEB_EP_META, meta, 23.0f, 175, tx, y + 124.0f);
    xm = pvDot(v, xm, y + 124.0f);
    { float x1 = xm;
      xm = pvMeta(v, TXT_DETWEB_EP_META, settings_date_full() ? DATE[i] : "2025", 23.0f, 175, xm, y + 124.0f);
      if (i == 0) metaR = (GfxRect){ x1, y + 110.0f, xm - x1, 28.0f }; }
    pvWrite(v, TXT_DETWEB_EPD, "A placeholder line about what happens in this episode.",
            25.0f, 255, tx, y + 150.0f, 0.55f);
  }

  if (hl == HL_META) pvMark(v, metaR);
}

// The picture box: the screen at 16:9, a hairline edge, the scene clipped inside.
static float drawScene(int scene, float x, float y, float w) {
  Pv v = { { x, y, w, w * 0.5625f }, w / NV_SCREEN_W };
  float rad = 14.0f / v.box.h;
  gfx_color(v.box, rad, PV_BG, PV_BG, PV_BG + 0.008f, 1.0f);
  gfx_crop(v.box.x, v.box.y, v.box.w, v.box.h);
  if (scene == PV_HOME) drawHomeScene(v, highlightOf());
  else if (scene == PV_PLAYER) drawPlayerScene(v, highlightOf());
  else if (scene == PV_EPISODES) drawEpisodesScene(v, highlightOf());
  else drawDetailScene(v, highlightOf());
  gfx_no_crop();
  gfx_rect(v.box, 0, GFX_RING_INSET, 0, 1.5f / v.box.h, 0, rad, 1.0f, 1.0f, 1.0f, 0.08f);
  return v.box.h;
}

// A key and its value, in two columns.
static float drawPair(const char *k, const char *v, float x, float y, float w) {
  TxtLine kl = txt_line_trim(TXT_CAPTION, k, 150, 152, 158, 255, w * 0.42f);
  TxtLine vl = txt_line_trim(TXT_BODY, v, 238, 238, 242, 255, w * 0.55f);
  float h = vl.h > kl.h ? vl.h : kl.h;
  txt_draw(kl, x, y + (h - kl.h) * 0.5f);
  txt_draw(vl, x + w * 0.45f, y + (h - vl.h) * 0.5f);
  return 38.0f;
}

// Every value of a choice as a chip, the current one filled. A long list (the
// thirty languages) shows the five around the current one.
// EVERY value, always: a window of five that slid with the focus had values
// arriving from nowhere as it moved. A long list (thirty subtitle languages)
// steps down a size until it fits; `draw` 0 only measures.
static const struct { TxtStyle st; float h, gap, pad; } CHIP[] = {
  { TXT_BODY, 50.0f, 12.0f, 24.0f },
  { TXT_SRC_META, 38.0f, 8.0f, 16.0f },
  { TXT_SRC_TEXT, 32.0f, 6.0f, 12.0f },
};
#define CHIP_N ((int)(sizeof CHIP / sizeof *CHIP))

static float drawChips(int op, float x, float y, float w, int size, int draw) {
  const Option *o = &OPTIONS[op];
  float cx = x, cy = y, h = CHIP[size].h, gap = CHIP[size].gap, pad = CHIP[size].pad;
  int k;
  for (k = 0; k < o->n; k++) {
    int sel = k == value[op];
    int c = sel ? 22 : 226;
    float cw = txt_width(CHIP[size].st, o->values[k]) + pad * 2.0f;
    if (cx + cw > x + w && cx > x) { cx = x; cy += h + gap; }
    if (draw) {
      TxtLine t = txt_line(CHIP[size].st, o->values[k], c, c, c + 2, 255);
      GfxRect r = { cx, cy, cw, h };
      if (sel) gfx_color(r, 0.22f, 0.93f, 0.93f, 0.94f, 1.0f);
      else gfx_rect(r, 0, GFX_RING_INSET, 0, 1.5f / h, 0, 0.22f, 1.0f, 1.0f, 1.0f, 0.18f);
      txt_draw(t, cx + pad, cy + (h - t.h) * 0.5f);
    }
    cx += cw + gap;
  }
  return cy + h - y;
}

// A number's range: the bar at full width, the ends under it.
static void drawRange(int op, float x, float y, float w) {
  const Option *o = &OPTIONS[op];
  float t = (o->max > o->min) ? (float)(value[op] - o->min) / (float)(o->max - o->min) : 0.0f;
  char lo[24], hi[24];
  float d = 18.0f;
  t = anim_clamp(t, 0.0f, 1.0f);
  gfx_color((GfxRect){ x, y + 6.0f, w, 6.0f }, 0.5f, 1.0f, 1.0f, 1.0f, 0.16f);
  if (t > 0.004f) gfx_color((GfxRect){ x, y + 6.0f, w * t, 6.0f }, 0.5f, 0.93f, 0.93f, 0.94f, 1.0f);
  gfx_color((GfxRect){ x + w * t - d * 0.5f, y + 9.0f - d * 0.5f, d, d }, 0.5f, 0.97f, 0.97f, 0.98f, 1.0f);
  snprintf(lo, sizeof lo, "%d%s", o->min, o->suffix ? o->suffix : "");
  if (op == SETTING_PAUSE_DELAY) snprintf(lo, sizeof lo, "Off");
  if (op == SETTING_HERO_TRAILER_SIZE) snprintf(lo, sizeof lo, "Backdrop");
  snprintf(hi, sizeof hi, "%d%s", o->max, o->suffix ? o->suffix : "");
  if (op == SETTING_HERO_TRAILER_DELAY) {
    snprintf(lo, sizeof lo, "%d.%d s", o->min / 10, o->min % 10);
    snprintf(hi, sizeof hi, "%d s", o->max / 10);
  }
  TxtLine l = txt_line(TXT_CAPTION2, lo, 140, 142, 148, 255);
  TxtLine h = txt_line(TXT_CAPTION2, hi, 140, 142, 148, 255);
  txt_draw(l, x, y + 26.0f);
  txt_draw(h, x + w - h.w, y + 26.0f);
}

// The right-hand panel: what the focus is on, a picture of what it changes, and
// its current values or its choices.
static void drawPanel(float hx, float hw) {
  const char *head, *help;
  int scene, chipSize;
  float y = SETTING_TOP + 22.0f;

  if (level == 0) {
    head = SECTIONS[focusSec].title;
    help = SECTIONS[focusSec].blurb;
    scene = sceneOf(focusSec);
  } else if (level == 2) {
    head = homerows_n() ? homerows_title(rowFocus) : "Nothing to list yet";
    help = !homerows_n()
         ? "The rows appear here once the Home has loaded your addons' catalogues."
         : rowHolding
         ? "Up and down move the row. OK puts it down."
         : homerows_over_cap(rowFocus)
         ? "Shown, but past the Home's row limit, so it does not appear. Hide a row above it, or move this one up."
         : "OK picks the row up to move it. Left or right shows or hides it. Changes apply when you leave this list.";
    scene = PV_HOME;
  } else {
    head = OPTIONS[focusOp].label;
    help = helpOption(focusOp);
    scene = sceneOf(focusSec);
    // A picture with nothing marked is the same for every row — quality, the audio
    // languages, animations — and says nothing about any of them. Nor does one for
    // a row the app does not act on: nothing in it changes.
    if (notWired(focusOp) || highlightOf() == HL_NONE) scene = PV_NONE;
    // The full date is the episode list's: the page's own hero shows the year alone.
    else if (focusOp == SETTING_DET_DATE_FULL) scene = PV_EPISODES;
  }

  y += drawKicker(scene != PV_NONE ? "Preview" : level ? "Details" : "Section", hx, y, 0.0f, 1.0f) + 14.0f;
  y += txt_block(TXT_PANEL_TITLE, head, 246, 246, 248, hx, y, hw, 48, 1, 2) + 10.0f;
  if (help) y += txt_block(TXT_CAPTION, help, 170, 172, 178, hx, y, hw, 32, 1, 4);
  if (level == 1 && notWired(focusOp)) {
    if (help) y += 10.0f;
    y += txt_block(TXT_CAPTION, "Saved to your account for the web app. The TV app does not use it yet.",
                   SETTING_ACCENT_R * 255 + 40, SETTING_ACCENT_G * 255 + 60, 255, hx, y, hw, 32, 1, 2);
  }
  y += 26.0f;
  // The choices must all fit under the picture: the chips step down a size, and
  // when even the smallest does not fit, the picture gives up the height.
  { int chips = level == 1 && !inactive(focusOp) && OPTIONS[focusOp].kind == OP_CHOICE;
    float bottom = NV_SCREEN_H - NV_MARGIN_Y, sw = hw;
    float kick = (float)txt_line(TXT_CWC_KICKER, "OPTIONS", 128, 130, 136, 255).h + 16.0f;
    chipSize = 0;
    if (chips) {
      float room = bottom - y - kick - (scene != PV_NONE ? 32.0f : 0.0f), need = 0.0f;
      for (chipSize = 0; chipSize < CHIP_N; chipSize++) {
        need = drawChips(focusOp, hx, 0.0f, hw, chipSize, 0);
        if (need <= room - (scene != PV_NONE ? hw * 0.5625f : 0.0f)) break;
      }
      if (chipSize == CHIP_N) chipSize = CHIP_N - 1;
      if (scene != PV_NONE && (room - need) / 0.5625f < sw) sw = (room - need) / 0.5625f;
    }
    if (scene != PV_NONE && sw > 120.0f) y += drawScene(scene, hx, y, sw) + 32.0f; }

  if (level == 0) {
    int s = focusSec, k;
    if (isRowsSection(s)) {
      int n = homerows_n(), shown = 0;
      char a[16], h[16];
      if (!n) return;
      for (k = 0; k < n; k++) if (homerows_enabled(k)) shown++;
      snprintf(a, sizeof a, "%d", shown);
      snprintf(h, sizeof h, "%d", n - shown);
      y += drawKicker("Currently", hx, y, 0.0f, 1.0f) + 14.0f;
      y += drawPair("Shown", a, hx, y, hw);
      drawPair("Hidden", h, hx, y, hw);
      return;
    }
    y += drawKicker("Currently", hx, y, 0.0f, 1.0f) + 14.0f;
    for (k = 0; k < SECTIONS[s].n && k < 3; k++) {
      int op = SECTIONS[s].start + k;
      y += drawPair(OPTIONS[op].label, inactive(op) ? "\xe2\x80\x94" : textValue(op), hx, y, hw);
    }
    return;
  }
  if (level == 2) {
    const char *source;
    if (!homerows_n()) return;
    source = homerows_source(rowFocus);
    y += drawKicker("Row", hx, y, 0.0f, 1.0f) + 14.0f;
    y += drawPair("Type", homerows_kind(rowFocus), hx, y, hw);
    if (source[0]) drawPair("Source", source, hx, y, hw);
    return;
  }
  if (inactive(focusOp)) return;
  if (OPTIONS[focusOp].kind == OP_CHOICE) {
    y += drawKicker("Options", hx, y, 0.0f, 1.0f) + 16.0f;
    drawChips(focusOp, hx, y, hw, chipSize, 1);
  } else if (OPTIONS[focusOp].kind == OP_NUMBER) {
    y += drawKicker("Range", hx, y, 0.0f, 1.0f) + 18.0f;
    drawRange(focusOp, hx, y, hw);
  }
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

// The title line. On the list of sections it is the page title; inside a
// section it is the path back — "‹ Settings / Playback" — with the section's
// size at the far right, over the panel's edge.
static void drawHeader(float right) {
  if (!level) {
    txt_tracking(TXT_TITLE3, "Settings", 255, 255, 255,
                 SETTING_LIST_X, NV_DSC_Y, 1.0f, NV_DSC_TITLE_LS);
    return;
  }
  float base = NV_DSC_Y + txt_baseline(TXT_TITLE3);
  float ys = base - txt_baseline(TXT_CALLOUT);
  float x = SETTING_LIST_X;
  TxtLine back = txt_line(TXT_CALLOUT, "\xe2\x80\xb9", 150, 152, 158, 255);
  TxtLine root = txt_line(TXT_CALLOUT, "Settings", 150, 152, 158, 255);
  TxtLine slash = txt_line(TXT_CALLOUT, "/", 96, 98, 104, 255);
  txt_draw(back, x, ys);
  x += back.w + 18.0f;
  txt_draw(root, x, ys);
  x += root.w + 20.0f;
  txt_draw(slash, x, ys);
  x += slash.w + 20.0f;
  txt_tracking(TXT_TITLE3, SECTIONS[focusSec].title, 255, 255, 255, x, NV_DSC_Y, 1.0f,
               NV_DSC_TITLE_LS);

  char count[32];
  int n = level == 2 ? homerows_n() : SECTIONS[focusSec].n;
  snprintf(count, sizeof count, level == 2 ? (n == 1 ? "%d row" : "%d rows")
                                           : (n == 1 ? "%d setting" : "%d settings"), n);
  TxtLine c = txt_line(TXT_CAPTION, count, 150, 152, 158, 255);
  txt_draw(c, right - c.w, base - txt_baseline(TXT_CAPTION));
}

void settings_draw(Uint32 now) {
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

  float hx = SETTING_LIST_X + SETTING_LIST_W + SETTING_PANEL_GAP;
  float hw = NV_SCREEN_W - NV_MARGIN_X - hx;
  if (hw > SETTING_PANEL_MAX_W) hw = SETTING_PANEL_MAX_W;
  drawHeader(hw > 240.0f ? hx + hw : NV_SCREEN_W - NV_MARGIN_X);

  int rows = level == 2 ? homerows_n() : level ? SECTIONS[focusSec].n : SETTING_N_SECTIONS;
  float scroll = level == 2 ? rowScroll : level ? scrollY : scrollSec;
  gfx_crop(SETTING_LIST_X - NV_RING_FOCUS, SETTING_TOP,
               SETTING_LIST_W + NV_RING_FOCUS * 2, NV_SCREEN_H - SETTING_TOP);
  pointer_clip(SETTING_LIST_X - NV_RING_FOCUS, SETTING_TOP,
               SETTING_LIST_W + NV_RING_FOCUS * 2, NV_SCREEN_H - SETTING_TOP);
  stepLtX0 = stepLtX1 = 0.0f;
  for (int i = 0; i < rows; i++) {
    float y = SETTING_TOP - scroll + yOfRow(i);
    drawGroup(i, y);
    if (level == 2)
      pointer_zone_act(SETTING_LIST_X, y, SETTING_LIST_W, SETTING_LINE_H,
                       pointHomeRow, clickHomeRow, i, 0);
    else if (level)
      pointer_zone_act(SETTING_LIST_X, y, SETTING_LIST_W, SETTING_LINE_H,
                       pointOption, clickOption, SECTIONS[focusSec].start + i, 0);
    else
      pointer_zone(SETTING_LIST_X, y, SETTING_LIST_W, SETTING_LINE_H, pointSection, i, 0);
    if (level == 2) {
      drawHomeRow(i, rows, y, i < 512 ? rowAnim[i] : 0.0f);
    } else if (level) {
      int op = SECTIONS[focusSec].start + i;
      drawLine(op, i, rows, y, animFocus[op]);
    } else {
      drawSection(i, y, animSec[i]);
    }
  }
  gfx_no_crop();
  pointer_no_clip();

  if (rows > 0) {
    float total = yOfRow(rows - 1) + SETTING_LINE_H;
    float window = SETTING_BASE - SETTING_TOP;
    // The continuous scrollbar (scrollbar.h), at the scroll's GOAL so it moves
    // on the press and not along the spring. No count chip: it would sit over
    // the rows' values.
    if (total > window)
      scrollbar_draw(&bar, SETTING_LIST_X + SETTING_LIST_W + 28.0f, SETTING_TOP,
                     SETTING_TOP + window, goalScroll / (total - window),
                     window / total, 0, 0, 1.0f, now);
    else
      bar.seen = 0;
  }

  if (hw > 240.0f) drawPanel(hx, hw);

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
