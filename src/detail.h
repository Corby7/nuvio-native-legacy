// The title's detail screen.
//
// The transition is the point: on the Apple TV the card does NOT disappear to make
// way for a new screen — it grows until it becomes the detail's hero, and the rest
// comes in afterwards. That is why detail_open takes the card's real source
// rectangle on screen, and not just the title: it is that rect that gives the
// movement continuity.
#ifndef NV_DETAIL_H
#define NV_DETAIL_H
#include <SDL2/SDL.h>
#include "home.h"

void detail_open(const HomeItem *item);
int  detail_is_open(void);
// 0..1 of how much the detail has taken over the screen; the home uses it to push
// the rows down.
float detail_progress(void);        // 1 while the screen exists, exiting included
// 1 when the card already covers the whole screen and drawing the home underneath
// is work thrown away. Measured: the home costs the full-screen hero plus ~20
// cards, and without this cut the detail page ran at 20fps.
int  detail_covers_screen(void);
// 1 when the card has settled in place and is not stretched: in that state the
// home behind it only shows through the frame.
int  detail_settled(void);

// Which catalogue title is on stage, and the requests the screen does not resolve
// on its own: play and mark on the list. The detail does not call the player or the
// library directly — what knows the other screens is the router.
int  detail_index(void);
// The season and episode in focus (1 = there is an episode; 0 = a title with none).
int  detail_ep_focus(int *season, int *episode);
int  detail_requested_play(void);   // consumes the request

// The index of the title the screen asked to OPEN in place of the current one, or
// -1. It consumes the request. It comes from two places: a credit in an actor's
// filmography and an item from the "More like this" tab — both carry an IMDb id,
// and what knows how to turn that into an index is the catalogue. What SWAPS the
// title is the router in app.c, not this screen: reopening itself in the middle of
// its own drawing is the kind of thing that breaks silently.
int  detail_requested_open(void);

// The eye button: mark the title as WATCHED. It is not the same as
// detail_requested_mark, which is "add to the list" — the eye fell into the same
// `else` as the sources button and never marked anything.
int  detail_requested_watched(void);
int  detail_requested_mark(void);       // the "+" button
int  detail_requested_sources(void);       // OK held, or the "..." button
// The secondary "Play from the start" button, which only exists when there is
// progress. Today it also sets `detail_requested_play`, because the router does not
// yet know how to open the player ignoring the saved point.
int  detail_requested_do_start(void);
// --- The title's page (below the fold) --------------------------------------
// EVERYTHING from here down was measured in the web app SIGNED IN, at 1920x1080,
// on the series "Silo" (getBoundingClientRect / getComputedStyle), on 2026-08-31.
// The numbers do not come from the CSS: the stylesheet declares
// `clamp(440px, 29vw, 540px)` for the episode card and what the screen draws is
// 640 — reading the stylesheet is 100px out.
//
// They live here and not in layout.h because layout.h is being edited by other
// agents in this same session.
//
// The model is the web app's: ONE scrollable document 2144px tall, of which the
// screen shows 1080. The hero occupies 0..1080 and SCROLLS with it — there is no
// fixed header, and no logo centred at the top (that was the Apple TV app's). The
// sections sit at ABSOLUTE document coordinates, and scrolling is just subtracting
// scrollY.
#define NV_DETP_X             96.0f   // the rows' gutter (--tv-safe-gutter-wide)
// The end of the scrollable document. It is NOT where the cast ends (2024): it is
// the scrollable height the web app has, because below the cast it still builds
// the comments and production-company sections, which this port does not have.
//
// The number comes from the measurement, not from arithmetic: with the cast
// focused the web app stops at scrollTop 1393, and for the group's top (1749) to
// land at 33% of the screen (356) the document has to be at least 1393 + 1080 =
// 2473. With the 2144 of the "cast + padding" arithmetic the scrolling hit its
// ceiling at 1064 and the cast row sat at y=693 instead of y=364 — half a screen
// out of place, and that is how it appeared in the first capture from the device.
#define NV_DETP_END         2473.0f
// The web app's scrolling rule, found in the source and checked against four
// measurements: the top of the focused GROUP goes to 33% of the usable height (40%
// on the tabs). The DETAIL_ROW_FOCUS_TARGET / DETAIL_TAB_FOCUS_TARGET constants of
// metaDetailsScreen.js. Checked: seasons -> scrollTop 724, episodes -> 838, tabs ->
// 1248, cast -> 1393. All four match to the pixel.
#define NV_DETP_TARGET_ROW  0.33f
#define NV_DETP_TARGET_TABS     0.40f
// The gap between items on a meta line. The flex declares 24 and what is measured
// is 38 (edge to edge) on all SIX occurrences: genre->dot, dot->year, year->IMDb,
// badge->duration, duration->country, country->language. You measure, you do not read.
#define NV_DETP_SEP           38.0f

// The top of each focusable GROUP, in document coordinates.
#define NV_DETP_G_TEMP      1080.0f
#define NV_DETP_G_EP        1194.0f
#define NV_DETP_G_TABS      1680.0f
#define NV_DETP_G_CAST    1749.0f

// Season tabs: 269x80 at x=96, step 321 (gap 52), radius 40, font 32/500.
// The width comes from the text + padding, and is not constant: "Specials" measures 219.
#define NV_DETP_TEMP_Y      1160.0f
#define NV_DETP_TEMP_H        83.0f   // MEASURED against the reference (it was 80)
#define NV_DETP_TEMP_PADX     40.0f
#define NV_DETP_TEMP_GAP      52.0f

// Episode: a 640x422 card at x=96, step 726, radius 32. The structural difference
// from the previous port (which was the Apple TV app's) is that the TEXT SITS
// INSIDE the thumbnail, over a vertical gradient, and not below it.
#define NV_DETP_EP_Y        1286.0f
// THE CARD'S MEASUREMENTS WERE REDONE ON THE DEVICE (TCL, 1920x1080, the series
// "Furious", 2026-09-01), because the web app's were wrong on almost all of them:
// the step was 726 against the 671 measured (the card ended up with an 86px gap
// instead of 31) and the text block came 30px above where it should be, up against
// the middle of the thumbnail.
//
// Reference read: a focused card with a 4px ring at x=94..737 and y=245..662, that
// is a box of 96..735 x 247..660 — 640x414. The next card starts at x=767.
#define NV_DETP_EP_W         640.0f
#define NV_DETP_EP_H         414.0f   // the box IS the thumbnail: the text sits inside
#define NV_DETP_EP_STEP     672.0f   // 767 - 96 = 671, rounded to 640+32
#define NV_DETP_EP_THUMB_H   414.0f
#define NV_DETP_EP_RADIUS       32.0f
#define NV_DETP_EP_PAD        32.0f   // the text's margin inside the thumbnail
#define NV_DETP_EP_TEXT_W   576.0f
// The "EPISODE n" badge: a 152x43 box at (32,155) inside the card, with the text's
// ink at 147..266 — 19 of slack on the left. Measured on card 1 of "Furious".
#define NV_DETP_EP_BADGE_Y    124.0f
#define NV_DETP_EP_BADGE_H     38.0f
#define NV_DETP_EP_BADGE_PADX  18.0f
// The three offsets below are the TOP OF THE LINE'S BOX, not the top of the ink:
// the measured ink (the title's cap height at +221, the synopsis at +274, the
// footer at +349) sits a few pixels below the top of the box SDL_ttf returns, and
// the difference is that between Inter's ascender and cap height at each line's
// body size.
#define NV_DETP_EP_TITLE_Y     174.0f   // the title, separated from the badge
#define NV_DETP_EP_SIN_Y     224.0f   // three lines before the footer
#define NV_DETP_EP_LD_SIN     32.0f
#define NV_DETP_EP_META_Y    344.0f   // clock + duration + date, ink at +349
#define NV_DETP_EP_ICON      28.0f
#define NV_DETP_EP_BAR_Y   390.0f   // barra 576x8, raio 999
#define NV_DETP_EP_BAR_H     8.0f
#define NV_DETP_EP_STATUS     48.0f   // the dashed "unwatched" circle
// The focus ring. The web app uses 2px on the episode thumbnail and 4px on the
// hero's buttons; here it is 4 on both, because the stylesheet declares CSS pixels
// and this page's screen runs with a scale factor of ~1.6 over the `clamp`s — that
// is, the episode's 2px ring comes close to 3 real px, and 4 is the whole number
// closest to it without disappearing on the TV.
#define NV_DETP_RING           4.0f

// The tabs "Creator and cast | Ratings | More like this | Trailer": font 32/500,
// the selected one white, the others #808080; the "|" divider is 32/700 #808080,
// with 20 of slack on each side.
#define NV_DETP_TAB_Y       1758.0f
#define NV_DETP_TAB_H         51.0f
#define NV_DETP_TAB_SEP       20.0f

// Cast: a card 220 wide, step 270; a 140x140 avatar ALIGNED LEFT in the card (not
// centred); the name 26/500 rgb(179,179,179) and the role 21/400 rgb(128,128,128)
// below.
#define NV_DETP_EL_Y        1817.0f
#define NV_DETP_EL_W         220.0f
#define NV_DETP_EL_STEP     270.0f
#define NV_DETP_EL_AVATAR    140.0f
#define NV_DETP_EL_NAME_DY    10.0f   // the avatar's base -> the name's top
#define NV_DETP_EL_ROLE_DY   43.0f   // the name's top -> the role's top
// The height of a line of text on the cast card (name or role), and the gap
// MEASURED between the cast's base and the top of the Trakt wordmark in the
// reference capture (~105 px at 1920). They exist so that stacking the comments
// section on a SERIES need not guess where the cast ends — using NV_DETF_EL_HEIGHT,
// which is the FILM's cast height, put the section ON TOP of the avatars.
#define NV_DETP_EL_LINE      34.0f
#define NV_DETP_EL_GAP_TRAKT 105.0f

// The badges of the HERO's meta stack, measured in the same session.
#define NV_DETW_IMDB_W       109.0f   // a 60x60 logo + slack + the score 20.7/400
#define NV_DETW_IMDB_H        60.0f
#define NV_DETW_BADGE_H        45.0f   // .detail-meta-badge, radius 8, 1px border
#define NV_DETW_BADGE_PADX     10.0f
// The secondary "Play from the start" button: 345x96, radius 64, background #222,
// white text; focused it becomes #f5f5f5 with #111 text and the 4px ring.
#define NV_DETW_BTN2_PADX     34.0f

// --- THE HERO BLOCK, MEASURED ON THE DEVICE --------------------------------
//
// Everything prefixed NV_DETW2_ was measured PIXEL BY PIXEL on captures of the
// reference app running on the TCL at 1920x1080 (adb exec-out screencap), on a
// SERIES ("Lioness") and a FILM ("Ma"), on 2026-09-01. Both captures were read by
// a PNG decoder of our own, not by eye.
//
// WHY A SECOND SET, rather than correcting NV_DETW_*: those came from the WEB app
// (getBoundingClientRect over .series-detail-shell). The web and the TV are two
// different applications and diverge on almost everything here — the column starts
// at 96 and not 72, the primary button is 94 tall and not 96, the focus is a SCALE
// and not a ring, and the IMDb badge is 60x30 and not 109x60. Where the two
// disagree the device wins, because that is what you see. The NV_DETW_* are still
// alive because other parts of the screen still use them.
//
// They live in detail.h and not in layout.h because layout.h is being edited by
// other agents in this same session.
#define NV_DETW2_X            96.0f   // the content column (it was 72, from the web)
// The stack's base: the bottom edge of the age-rating badge falls at 1047/1048 in
// BOTH captures, with synopses of different lengths. It is the same value
// NV_DETW_BASE already had, and it is what anchors the whole stack.
#define NV_DETW2_BASE       1048.0f

// THE ACTIONS ROW. The pill at rest is 321x94 (x=96..417, y=512..606 on the
// series), circles of 96 with a 24 gap between neighbours (centres at 488.5 and
// 608.5, step 120). The pill's width comes from the label: 54 + 28 + 21 + text + 54
// = 319 for "Watch S1:E1", against the 321 measured.
#define NV_DETW2_BTN_H        94.0f
#define NV_DETW2_BTN_PADX     54.0f
#define NV_DETW2_BTN_ICON_W  28.0f   // a 28x30 triangle, vertically centred
#define NV_DETW2_BTN_ICON_H  30.0f
#define NV_DETW2_BTN_GAPI     21.0f   // fim do triangulo -> tinta do rotulo
#define NV_DETW2_CIRC         96.0f
#define NV_DETW2_BTN_GAP      24.0f
// The glyph inside the circle: 32 in a 96 circle at rest and 36 in a focused 110 —
// 0.333 of the diameter in both. It was 0.45, which came from a loose capture of
// the owner's and fattened the "+" until it nearly touched the edge.
#define NV_DETW2_CIRC_GLYPH    0.333f
// FOCUS: the device does NOT draw a ring. The focused item GROWS, with its centre
// still, and the dark circle also changes colour (#222 -> #f5f5f5, white glyph ->
// #111). The white pill stays white in both states: it is the size that says focus.
//
// The two factors are measured and are NOT equal, which is surprising but
// repeatable: the pill goes from 321x94 to 357.6x107.8 (1.114 in x, 1.147 in y) and
// the circle from 96 to 110 (1.146 on both axes). Writing a single factor would
// make the pill grow 10px more than the reference; both stay, as measured.
#define NV_DETW2_FOCUS_SX       1.114f
#define NV_DETW2_FOCUS_SY       1.147f

// THE TEXT STACK. Both captures give the same absolute coordinates for what is
// BELOW the synopsis (the IMDb badge at y=938, the age-rating badge at y=999) and
// the synopsis grows UPWARDS — 5 lines on the series, 4 on the film, and the LAST
// line falls at the same y in both. That is why the stack is built from the bottom
// up, and not from the logo down.
#define NV_DETW2_LD_SIN       40.0f   // the step between synopsis lines (measured)
#define NV_DETW2_SIN_LINES       5   // the most seen in the reference
#define NV_DETW2_TEXT_W    1040.0f   // the synopsis and the support line (96..1136)
// Where the ratings row (Rotten Tomatoes, Trakt) stops adding sources: 96 + 640.
// It used to reach for the HOME hero's NV_HERO_SIN_W to get that 640 — a constant
// from another screen, so widening the hero's synopsis silently widened this row.
// Same number it has always drawn at, under a name of its own.
#define NV_DETW2_RATE_W     640.0f
// The distance between the BOX TOPS of neighbouring lines, with the font's metrics
// already discounted: between the ink the reference gives 62 from the top of the
// "W" of "Writer" to the top of the "C" of the synopsis, and both lines use the
// same body size.
#define NV_DETW2_GAP_SUP      62.0f
// The end of the synopsis's box -> the top of the IMDb badge. The last line's ink
// ends at 890 and the badge starts at 938; the rest is the font's descender.
#define NV_DETW2_GAP_SIN      33.0f
// The pill's base at rest (606) -> the top of the support line's box. The "W"'s ink
// starts at 649, and the box starts ~6 above it at a body size of 26.
#define NV_DETW2_GAP_ACTIONS    37.0f

// META LINE 1: genres, date and the IMDb badge, all in rgb(179,179,179).
// The line's height is the badge's (30), which is the tallest item on it.
#define NV_DETW2_M1_H         30.0f
#define NV_DETW2_META_GAP     31.0f   // 999 - 968
// The IMDb badge: a 60x30 yellow #f6c700 rectangle, radius ~4, with a black "IMDb"
// inside; the score comes 8px later, in the same grey as the rest of the line. It
// is NOT the web app's 109x60 — this one is smaller and the mark fills the whole badge.
#define NV_DETW2_IMDB_W       60.0f
#define NV_DETW2_IMDB_H       30.0f
#define NV_DETW2_IMDB_R        4.0f
#define NV_DETW2_IMDB_GAP      8.0f
// The separator dot. There are TWO different dots and the difference is only the
// colour: between genres it is rgb(179,179,179) with 11 of slack on each side, and
// between GROUPS (genres | date | score) it is rgb(128,128,128) with 30. Both
// measure 6x7.
#define NV_DETW2_DOT_D       6.0f
#define NV_DETW2_SEP          30.0f
#define NV_DETW2_BULLET_SEP   11.0f

// META LINE 2: an OUTLINE badge with the age rating and the status together, then
// the duration (film only) and the country. A box 49 tall at y=999, radius 8, with
// a 2px rgb(107,107,107) border — a real outline, with no painted middle.
#define NV_DETW2_BADGE_H       49.0f
#define NV_DETW2_BADGE_R        8.0f
#define NV_DETW2_BADGE_PADX    16.0f
#define NV_DETW2_BADGE_BORDER    2.0f
// The internal divider: a 2x24 bar in the same colour as the border, with 18 of
// slack on each side. It is what separates "TV-MA" from "RENEWED" inside the same
// badge.
#define NV_DETW2_DIV_W         2.0f
#define NV_DETW2_DIV_H        24.0f
#define NV_DETW2_DIV_PAD      18.0f

// --- The title page for a FILM, below the fold ------------------------------
//
// A film does NOT reuse the series' absolute coordinates. Those were measured on a
// series page (seasons + episodes) and, on a film, left a 600px hole. Here the
// sections STACK: each one knows its own height and the next starts where the
// previous one ended.
//
// THE ORIGIN OF THESE MEASUREMENTS, and it is worth writing down because there are
// TWO sources:
//
// SIZES (card, avatar, typography) — from the WEB app, measured with
// getBoundingClientRect at 1920x1080. It is the TV reference, made to be seen from
// a distance, and that is why it wins here.
//
// STRUCTURE (stacked sections, each with a header of its own) — from the NATIVE
// MAC app, /Applications/Nuvio.app, com.nuvio.media.desktop 1.1.22, built in
// Compose Multiplatform. That is where the reference captures came from, and the
// classes confirm the design: DetailCastSectionKt, DetailTrailersSectionKt,
// DetailAdditionalInfoSectionKt, DetailProductionSectionKt. The WEB app does not
// have this — there it is a row of tabs that swaps the content in place.
//
// That app's sizes were NOT copied: it is a desktop app (breakpoints
// 600/840/1200dp) and at its largest the cast avatar measures 100dp against the
// TV's 140. Only the internal proportion of the details table came from it, for
// want of another source — see the note at NV_DETF_DET_*.
//
// THE FOUR GAPS BELOW are still a proportion taken from the captures, not a
// measurement: the Mac parameterises the spacing per call
// (DetailSectionContainer(horizontalPadding, contentMaxWidth, bottomPadding)), so
// there is no single constant to read in the bytecode.
#define NV_DETF_HERO_END     1080.0f   // the hero occupies 0..1080, as on the series
#define NV_DETF_HEADER_H          46.0f   // the header line (TXT_HEADLINE, 38)
#define NV_DETF_HEADER_GAP        20.0f   // header -> content
#define NV_DETF_SEC_GAP        64.0f   // end of one section -> the next section's header
#define NV_DETF_PAD_END       130.0f   // the scroller's padding-bottom (clamp(116,12vh,168))

// CAST. A 220x193 card, step 270, a 140 avatar aligned LEFT in the card.
// Measured in the web app (.movie-cast-card / .movie-cast-track).
#define NV_DETF_EL_HEIGHT        193.0f
#define NV_DETF_EL_MAX           18    // the web app's .slice(0, 18)

// TRAILERS. A card 520 wide, a 520x292 thumbnail with radius 24, step 582.
// The play badge is a 96 circle at rgba(0,0,0,.48) with a 44 triangle.
#define NV_DETF_TR_W          520.0f
#define NV_DETF_TR_STEP      582.0f
#define NV_DETF_TR_VIDEO_H    292.0f
#define NV_DETF_TR_RADIUS        24.0f
#define NV_DETF_TR_NAME_DY    302.0f   // topo do card -> nome (28/500 branco)
#define NV_DETF_TR_KIND_DY    344.6f   // topo do card -> subrotulo (24/400 cinza)
#define NV_DETF_TR_HEIGHT        377.0f
#define NV_DETF_TR_PLAY_D      96.0f

// FILM DETAILS. A two-column table with a divider per row.
//
// THE STRUCTURE IS REAL, and it was checked: the native Mac app
// (/Applications/Nuvio.app, com.nuvio.media.desktop 1.1.22, Compose Multiplatform)
// has the classes DetailCastSectionKt, DetailTrailersSectionKt and
// DetailAdditionalInfoSectionKt — that is, STACKED sections with headers of their
// own, and not NuvioWeb-0.3.38-beta's row of tabs. The reference captures came from
// that app.
//
// THE SIZES ARE NOT COPIED FROM IT, and that is a decision. That is a desktop app,
// with breakpoints at 600/840/1200dp; at its largest the cast avatar measures
// 100dp, against the 140 measured in the TV web app. Copying the Mac's dp would
// shrink the screen for someone watching from a distance. What is taken is the
// internal PROPORTION, which had no source at all before:
//
//   Mac DetailInfoRow: a 176dp label column in 720dp of content = 24.4%
//
// The width comes from the TV measurement (the hero's text block, 1040), and the
// value column starts at the same 24.4% of it. The row step of 68 matches the Mac's
// arithmetic rescaled to this screen's body size of 25 (25*1.4 + 2*15.6 = 66).
//
// The key aligns left at NV_DETP_X; the value starts in a FIXED column, and not
// after the key's text — otherwise the second column jags from line to line.
#define NV_DETF_DET_LINE      68.0f   // the vertical step of one row
#define NV_DETF_DET_W        1040.0f   // the width of the table and the divider
#define NV_DETF_DET_KEY_W   254.0f   // 24.4% of NV_DETF_DET_W (the Mac's proportion)
#define NV_DETF_DET_MAXL          6    // Status, Release, Duration, Rating, Country

void detail_event(const SDL_Event *e);
void detail_update(float dt, Uint32 now);
void detail_draw(Uint32 now);   // desenhe DEPOIS da home: ele cobre

#endif
