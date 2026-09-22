// Text: SDL_ttf rasterises to a texture, cached by (font, size, string). Without
// the cache, every frame would rasterise the same row titles again — text
// rasterisation is expensive and the content here changes little.
#ifndef NV_TEXT_H
#define NV_TEXT_H
#include "gl_compat.h"

// The tvOS scale. Each style carries a size AND a weight: on the device the
// difference between a title and a subtitle comes as much from the weight as
// from the size, and using a single weight flattens the whole hierarchy — that
// is what made the screen look like "everything the same size, some bigger".
typedef enum {
  TXT_TITLE1, TXT_TITLE2, TXT_TITLE3, TXT_HEADLINE,
  TXT_BODY, TXT_CALLOUT, TXT_CAPTION, TXT_CAPTION2, TXT_MINI,
  // The player's two come from the web app, not from the tvOS scale. They sit at
  // the END of the enum on purpose: the STYLES table in text.c is indexed by this
  // order, and inserting in the middle shifts every following style silently.
  TXT_PLR_TITLE, TXT_PLR_BODY, TXT_ROW_TITLE, TXT_HERO_SEC,
  // The DETAIL screen, measured in the web app. They reuse no tvOS style because
  // none matches: the synopsis there is 26/400 and TXT_CAPTION here is 22/400 —
  // four pixels that change how many lines fit in the block.
  TXT_DET_BUTTON,   // .series-primary-btn      25 / 600
  TXT_DET_META,    // .series-detail-support   25 / 400
  TXT_DET_SIN,     // .series-detail-description 26 / 400
  TXT_DET_META2,   // .detail-meta-row.secondary 23 / 400
  // The hero's actions row, re-measured in NuvioWeb on 2026-09-15. TXT_DET_BUTTON
  // stays where it is because profile.c draws its "OK · Try again" hint with it.
  TXT_DETWEB_BTN,  // .series-primary-btn       32 / 600
  TXT_DETWEB_TIP,  // .series-circle-btn::after 24 / 700
  // The season picker and the episode card, re-measured in NuvioWeb 2026-09-15.
  TXT_DETWEB_SEA,      // .library-picker-value       30 / 600
  TXT_DETWEB_SEA_EPS,  // its " · N Eps" tail          30 / 400
  TXT_DETWEB_OPT,      // .library-picker-option      28 / 500
  TXT_DETWEB_EP_BADGE, // .series-episode-badge       20 / 600
  TXT_DETWEB_EP_META,  // .series-episode-meta        20 / 400
  TXT_DETWEB_EP_TITLE, // .series-episode-title       32 / 800
  TXT_DETWEB_EPD,      // .series-episode-desc-row    32 / 400
  // The HERO's meta line: 21 / 500, rgb(179,179,179). It is neither TXT_CAPTION
  // (22/400) nor TXT_CALLOUT (28/500) — one gets the weight wrong, the other the
  // size, and the line came out either too faint or too heavy against the art.
  TXT_HERO_META,
  // The hero's synopsis: .home-hero-description, 24/400 full white — the modern
  // rule's size, not the 22 `.legacy-webos` drops to (see NV_FT_HERO_SIN). The
  // colour comes from whoever draws, not from the style.
  TXT_HERO_SIN,
  // Top corner of the PLAYER, from the web app's #playerUiRoot block:
  //   .player-clock          26 / 600
  //   .player-ends-at        20 / 400
  //   .player-parental-label 22 / 600
  //   .player-parental-severity and .player-parental-separator 22 / 400
  TXT_PG_CLOCK, TXT_PG_END, TXT_PG_LABEL, TXT_PG_SEV,
  TXT_PANEL_TITLE, TXT_PANEL_ITEM,
  TXT_CW_TITLE, TXT_CW_META, TXT_CW_BADGE,
  TXT_RANK,
  // External subtitle: 50%..200%, in steps of 10. The C9 firmware offers only
  // five steps; these fonts belong to the app's own overlay.
  TXT_SUB_50, TXT_SUB_60, TXT_SUB_70, TXT_SUB_80,
  TXT_SUB_90, TXT_SUB_100, TXT_SUB_110, TXT_SUB_120,
  TXT_SUB_130, TXT_SUB_140, TXT_SUB_150, TXT_SUB_160,
  TXT_SUB_170, TXT_SUB_180, TXT_SUB_190, TXT_SUB_200,
  // The account's profile picker, measured in the web app. None of the tvOS
  // styles fits: the title there is 48/500 and TXT_TITLE3 is 48/BOLD, which is
  // the same size shouting, and the name at 34 has no neighbour at all.
  //
  // They are APPENDED, like the subtitle sizes above and for the same reason:
  // STYLES in text.c is indexed by this order.
  TXT_PSEL_TITLE,    // .profile-title     48 / 500
  TXT_PSEL_SUB,      // .profile-subtitle  36 / 500
  TXT_PSEL_NAME,     // .profile-name      34 / 500
  TXT_PSEL_NAME_F,   // the same, focused: 34 / 600
  TXT_PSEL_BADGE,    // .profile-badge     22 / 600, tracked out 1.6px
  TXT_PSEL_HINT,     // .profile-hint      28 / 500
  TXT_PSEL_INITIAL,  // .profile-avatar    77 / 700 (82 focused; drawn scaled)
  TXT_PSEL_STAR,     // .profile-primary-dot 28 / 700 — U+2605, which Inter has
  // The Continue Watching card, measured in the web app's MODERN layout. The
  // TXT_CW_* three above are the CLASSIC block's sizes and stay where they are:
  // the "Among friends" card still draws with them.
  //
  // APPENDED, for the reason given twice above: STYLES in text.c is indexed by
  // this order and an insertion in the middle shifts every following style.
  TXT_CWC_TITLE,     // .home-continue-title    30 / 600
  TXT_CWC_KICKER,    // .home-continue-kicker   17 / 600, uppercase, ls 0.14em
  TXT_CWC_SUB,       // .home-continue-subtitle 21 / 400
  // The player's TRANSPORT ROW, re-measured in NuvioWeb on 2026-09-17 against the
  // "Transport - clock, title block, scrubber" block, which is the last one in the
  // sheet and supersedes the ATV port these styles were first taken from.
  //
  // APPENDED, for the reason given three times above: STYLES in text.c is indexed
  // by this order and an insertion in the middle shifts every following style.
  TXT_PLR_TIP,       // .player-control-btn::after     20 / bold
  TXT_PLR_TIME,      // .player-controls-row .player-time-label 30 / 600
  // The "Ends at" under the clock. It cannot reuse TXT_PG_END any more: that one
  // is shared with the episode list, the sources sheet and the tracks panel, so
  // resizing it for this corner would resize four other screens.
  TXT_PLR_ENDS,      // .player-ends-at                24 / 500
  TXT_PLR_META3,     // .player-meta-tertiary          22 / 500
  // The " / total" half of the time readout. The sheet sets the whole label at
  // 600, but on the device the elapsed time is the number you are actually
  // reading and the duration is the thing it is measured against — so the two
  // are split here and the tail carries less weight. See the note at the draw.
  TXT_PLR_TIME_T,    // .player-time-label, tail       30 / 500
  TXT_PLR_DELTA,     // .player-seek-delta             24 / 700
  TXT_PLR_STAT,      // .player-stats-value            22 / 500
  TXT_PLR_STATL,     // .player-stats-label            20 / 500
  TXT_PLR_BADGE,     // .player-stats-quality          16 / 700
  TXT_PLR_PG_CAT,    // .player-parental-label         24 / 600
  // The track menus. APPENDED, for the reason given four times above: STYLES in
  // text.c is indexed by this order, so an insertion in the middle shifts every
  // style that follows it.
  TXT_TRK_TITLE,     // .player-dialog-title           46 / 800
  TXT_TRK_LABEL,     // .player-select-label           24 / 600
  TXT_TRK_VALUE,     // .player-select-value           26 / 700
  TXT_TRK_OPT,       // .player-select-option-main     24 / 600
  TXT_TRK_OPTSUB,    // .player-select-option-sub      20 / 500
  TXT_TRK_STEP,      // .player-dialog-step, "+" / "-" 34 / 700
  // The two jump-ahead prompts and the episode rail. Appended, same reason.
  TXT_SKIP,          // .player-skip-intro-label            28 / 500
  TXT_NEXT_KICK,     // .player-next-episode-kicker         20 / 700, uppercase
  TXT_NEXT_TITLE,    // .player-next-episode-title          32 / 700
  TXT_NEXT_PILL,     // .player-next-episode-play/-dismiss  22 / 500
  TXT_ERAIL_META,    // .player-episode-detail-meta         22 / 700, uppercase
  TXT_ERAIL_TITLE,   // .player-episode-detail-title        44 / 800
  TXT_ERAIL_OVER,    // .player-episode-detail-overview     22 / 500
  TXT_ERAIL_CODE,    // .player-episode-code                24 / 800
  TXT_ERAIL_PILL,    // .player-episode-current ("Playing") 20 / 800, uppercase
  TXT_ERAIL_CTITLE,  // .player-episode-card-title          24 / 600
  // THE SEARCH SCREEN, read off the live sheet on 2026-09-19. APPENDED, like
  // everything above it and for the same reason: STYLES in text.c is indexed by
  // this order, so a style inserted in the middle silently shifts every one
  // after it.
  //
  // Only three. The rest of the screen reuses what already exists and matches
  // exactly — TXT_TITLE3 for the 48/600 page title, TXT_TITLE2 for the 56/600
  // empty-state heading, TXT_ROW_TITLE for the 28/600 row title, and
  // TXT_CALLOUT for the 28/500 shared by the input field and the history chip.
  TXT_SRCH_NAME,     // .search-result-name        24 / 500
                     // and .search-history-label  24 / 500, uppercase, tracked 1
  TXT_SRCH_META,     // .search-results-subtitle and .search-result-date  20 / 400
  TXT_SRCH_EMPTY,    // .search-empty-state p      24 / 400
  // The SOURCES sheet, rebuilt as a table — see "THE SOURCES SHEET" in layout.h.
  // None of these borrows TXT_PG_END, the 20/400 the old rows drew with: that one
  // is shared with the episode list and the tracks panel, and this screen needs
  // its meta line at 500 to hold up beside a chip.
  //
  // APPENDED, for the reason given five times above: STYLES in text.c is indexed
  // by this order, so an insertion in the middle shifts every style after it.
  TXT_SRC_COUNT,     // "12 found" beside the heading      22 / 400
  TXT_SRC_TAB,       // the provider tabs                  24 / 600
  TXT_SRC_CHIP,      // 4K / DV / REMUX, tracked           18 / 700
  TXT_SRC_TEXT,      // audio and codec, tracked caps      18 / 500
  TXT_SRC_META,      // the availability line              20 / 500
  TXT_SRC_SIZE,      // the file size                      30 / 700
  TXT_SRC_TIER,      // BEST / GOOD / FAIR / POOR          17 / 700
  TXT_SRC_STATE,     // "Playing", on the availability line 20 / 700
  TXT_NFONTS
} TxtStyle;

typedef struct { GLuint tex; int w, h; } TxtLine;

// An alternative family used ONLY by the external subtitle renderer. The
// interface stays on Inter; mixing the subtitle family into menus would make a
// playback preference redraw the whole app.
typedef enum {
  TXT_FAMILY_INTER = 0,
  TXT_FAMILY_LG,
  TXT_FAMILY_DROID,
  TXT_FAMILY_N
} TxtFamily;

extern const char *const TXT_FAMILIES_LABEL[TXT_FAMILY_N];

// Instrumentation: how many lines were RASTERISED (rather than coming from the
// cache) in the frame, and what that cost. Rasterising text is the most
// expensive operation inside a frame, and without a counter there is no telling
// whether a jank came from there or from a texture upload.
extern int    txt_rasterized;
// Evictions from the line cache. Anything other than zero with the screen idle
// means the table does not fit what the screen draws, and the text flickers.
extern int    txt_evictions;
extern double txt_ms;
// How many lines the frame did NOT find in the cache — the ones it rasterised
// AND the ones TXT_PER_FRAME refused. It answers the question a caller cannot
// answer for itself: "is this block of text settled, or is it still arriving?".
// The hero reads it to decide whether its copy can be shown whole; without it,
// the description faded in while it was still being rasterised two lines at a
// time, which is precisely the worst case.
extern int    txt_misses;

// `dirAssets` is the folder containing fonts/. On the device it is the app's
// folder; on the Mac, the package's — without this parameter the font was only
// looked for next to the executable, and running locally fell straight to the
// fallback.
// `scale` is the ratio between the buffer and the layout canvas (2 on a 4K TV, 1
// at 1080p). The fonts are opened at that size and the returned line still
// measures in layout units — see text.c.
int  txt_start(const char *dirAssets, float scale);
void txt_shutdown(void);

// Returns a cached line. Colour in 0..255. Never returns NULL; on failure,
// w/h = 0.
// Resets the frame's rasterisation budget. Call once per frame, before drawing;
// without it the budget runs out and the text disappears.
void txt_new_frame(void);

TxtLine txt_line(TxtStyle style, const char *s, int r, int g, int b, int a);

// Like txt_line, but picks one of the families that are safe for subtitles. If
// the system font does not exist (in the Mac preview, for instance), it falls
// back to the embedded Inter and logs the fallback once.
TxtLine txt_line_family(TxtStyle style, const char *s, int r, int g,
                           int b, int a, TxtFamily family);

// A line that NEVER exceeds `maxW`: it trims by word (or by character, if a
// single word already overflows) and closes with "…". Content that comes from
// outside (an addon's name, a TMDB genre) has no guaranteed length, and without
// trimming it invades the neighbouring column — which is what showed up in the
// Top 10 and on the tracks sheet.
TxtLine txt_line_trim(TxtStyle style, const char *s, int r, int g, int b,
                         int a, float maxW);
TxtLine txt_line_trim_family(TxtStyle style, const char *s, int r, int g,
                                 int b, int a, float maxW, TxtFamily family);

// WHERE THE INK IS INSIDE A LINE. A TxtLine is a box the height of the FONT
// (ascent + descent), so its top edge is not where the letters start: a 76px
// Inter line has some 22px of air above a capital and 18 below a baseline. Two
// lines of different sizes stacked box-to-box therefore look far further apart
// than the numbers say — which is what made the collection hero's group label
// read as detached from the title it belongs to.
//
// txt_cap_inset is the distance from the line's TOP to the top of a CAPITAL,
// txt_baseline the distance from the top to the BASELINE. Measured off the face
// itself (the ascent against 'H''s own extent), in LAYOUT units like TxtLine.w/h,
// so they stay right if the interface is ever scaled.
float txt_cap_inset(TxtStyle style);
float txt_baseline(TxtStyle style);

// Draws at the top-left corner (x,y).
void txt_draw(TxtLine l, float x, float y);
void txt_draw_alpha(TxtLine l, float x, float y, float alpha);

// A SHADOW FROM THE LINE ALREADY IN HAND — the same texture, tinted black.
//
// The player fakes its text-shadow by rasterising a SECOND TxtLine in black and
// drawing it 2px down. That is right for one label and wrong for a list: the
// cache is keyed on (font, size, string, COLOUR), so every shadowed string takes
// a second slot, and the sources sheet puts some seventy strings on screen at
// once. Doubling that is how a cache starts evicting, and an evicting line cache
// does not degrade quietly — the text flickers (see txt_evictions).
//
// SDL_ttf bakes the colour into RGB and leaves coverage in ALPHA, so the glyphs'
// shape is in the alpha channel whatever colour they were rendered. GFX_BRAND
// takes its shape from exactly there and its colour from the caller, which makes
// a black copy of any cached line for one extra draw call and no extra entry.
void txt_draw_shadow(TxtLine l, float x, float y, float alpha);

// Draws with letter SPACING (tracking) and returns the total width. SDL_ttf has
// no tracking, and the tvOS page title depends on it: without the wide spacing
// the same text in capitals reads as shouting, not as a heading. Pass x = -1 to
// measure only, without drawing.
float txt_tracking(TxtStyle style, const char *s, int r, int g, int b,
                   float x, float y, float alpha, float tracking);

// Draws text WRAPPED into lines that fit `width`, returning the height used.
// Without this, any variable-length text (an episode synopsis, a title's name)
// spills into the neighbouring column — there is no "writing short enough" when
// the content comes from outside.
float txt_block(TxtStyle style, const char *s, int r, int g, int b,
                float x, float y, float width, float leading, float alpha, int maxLines);

// The same block, but RIGHT-ALIGNED: every line ends at `xRight`. The credits in
// the bottom-right corner need this — left-aligned, their edge comes out ragged
// against the card's margin.
float txt_block_dir(TxtStyle style, const char *s, int r, int g, int b,
                    float xDir, float y, float width, float leading,
                    float alpha, int maxLines);

#endif
