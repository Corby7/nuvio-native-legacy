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

// `shared` says the SCREEN BEHIND is the home, with its hero showing this title:
// only then does the opening fly out of the hero's rect and carry the logo across.
// From the search, the Discover page, a "See all" grid or the context menu there is
// no hero on screen to continue from — the flight would start at a rectangle the
// viewer has never seen — so those pass 0 and get a plain full-bleed fade.
void detail_open(const HomeItem *item, int shared);
// 1 while the screen that is open was given a shared origin. The home reads it to
// hand its hero logo over instead of drawing a second copy of it.
// The title screen's BACKDROP, drawn before whatever is underneath it. It is a
// background and has to be behind the home's copy and shelves — composited over
// them its boundary cuts through text and cards. detail_draw() draws the rest.
void detail_draw_bg(Uint32 now);
int  detail_shared_origin(void);
// 1 while the screen is on its way out, for the home's return travel.
int  detail_is_exiting(void);
int  detail_is_open(void);
// 0..1 of how much the detail has taken over the screen; the home uses it to push
// the rows down.
float detail_progress(void);        // 1 while the screen exists, exiting included
// 1 when the card already covers the whole screen and drawing the home underneath
// is work thrown away. Measured: the home costs the full-screen hero plus ~20
// cards, and without this cut the detail page ran at 20fps.
int  detail_covers_screen(void);
// Where the title's logo was last drawn, for the player's handoff to fly it from.
// 0 when the page drew the name instead (or nothing).
int  detail_logo_rect(GfxRect *out);
// While the player's copy of the logo is in the air the page must not draw its own,
// or the flight leaves a ghost behind at the start.
void detail_hide_logo(int hide);
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
// The IMDb trailer (trailers.h index) OK asked to play, -1 for none; consumes it.
int  detail_requested_trailer(void);

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
// THE DOCUMENT'S END IS NOW ARITHMETIC, and NV_DETP_END (2473) has gone with it.
//
// 2473 was the WEB APP's scrollable height, measured: with the cast focused the web
// app stops at scrollTop 1393, and 1393 + 1080 = 2473. Every word of that is still
// true of the web app and none of it is true here any more — the below-fold document
// is now THREE PAGES of this screen's own (NV_DETP_P2 / NV_DETP_P3), so its end is
// NV_DETP_P3 + NV_SCREEN_H and recomputeLayout computes it.
//
// The note that stood here warned against deducing the end from a SUM OF HEIGHTS,
// because the "cast + padding" arithmetic (2144) clipped the scroll at 1064 and left
// the cast row half a screen out of place. That warning still holds for a sum of
// CONTENT heights. A page model is not one: where the last page ends does not depend
// on how tall the cast happens to be.
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

// --- THE BELOW-FOLD DOCUMENT IS THREE PAGES ---------------------------------
//
// THE OWNER'S RULE, and it is the reason the numbers below are no longer NuvioWeb's:
// DOWN from Play shows the season dropdown, the episode rail and the episode
// description AND NOTHING ELSE; DOWN from there shows the rest of the information.
//
// The fraction rule cannot do that. Taking the focused group's top to 33% of the
// screen (NV_DETP_TARGET_ROW) always leaves the tail of the block above and the head
// of the block below in frame: from Play it parked at scrollY 724, so the top third
// was still the hero's synopsis and the insight tabs were already sitting at y=759
// under the episode copy. It is not a number that was wrong, it is the rule.
//
// So each page is ONE SCREEN and a group scrolls its page's top to the top of the
// screen — see snapSec in detail.c. The pages are:
//
//   PAGE 1  hero          0 .. 1080
//   PAGE 2  episodes   1080 .. 2160   picker, rail, description
//   PAGE 3  information 2160 .. 3240  tab strip, cast, and Trakt below it
//
// NuvioWeb has itself moved halfway here and the port had not followed: focusInList
// now scrolls .series-season-row to focus target 0 and .series-episode-track to
// seasonMountHeight / clientHeight (metaDetailsScreen.js:6072-6079) — the picker
// pinned at the top of the viewport with the rail directly under it. The 0.33 this
// screen was carrying for those two groups was measured against a build that no
// longer behaves that way.
#define NV_DETP_P2          1080.0f   // the episode page's top (== the hero's end)
#define NV_DETP_P3          2160.0f   // the information page's top
// A page's TOP INSET. It is NV_DETP_X, the gutter, used vertically: the page's inset
// is then one number in both axes instead of two that have to be kept in step.
#define NV_DETP_PAD_TOP       96.0f

// The top of each focusable GROUP, in document coordinates. They are what snapSec and
// the culling read; what the drawing reads is the NV_DETP_*_Y set below, and on a
// series the two are DELIBERATELY separate numbers (see drawSection).
//
// These were re-measured in NuvioWeb 0.3.8 on "The Gentlemen" as long as they were
// scroll targets for the fraction rule (season row 1080 h176, episode track 1240 h467,
// insight tabs 1839 h136.6, tab content 1991.6). They are page anchors now, so the
// measurement no longer sets them — the page does. The episode group loses the 16px it
// sat above its own drawing y, which existed only to feed the fraction, and the tabs
// and the cast share page 3's top so that moving the focus between them does not
// retarget the page under a hand that is already on the d-pad.
#define NV_DETP_G_TEMP      NV_DETP_P2
#define NV_DETP_G_EP        NV_DETP_EP_Y
#define NV_DETP_G_TABS      NV_DETP_P3
#define NV_DETP_G_CAST      NV_DETP_P3

// THE EPISODE PAGE'S HEAD is the season picker alone, at the page's top-left. The
// owner's reference has the title's logo there with the picker beside it; the logo
// was tried under the picker and dropped — the hero one page up already names the
// title, and without it the list gets four rows instead of three and a half.
// FOUR ROWS, NOT THREE AND A HALF. The list is whatever the page has left under the
// picker, and at 80 + 56 around it (with 236 rows) that was 864px: 3.66 rows, so
// the fourth was always cut and, once the focused row grew, only three showed. At
// 56 + 24 and 228 rows it is 920px, four at rest with room to spare.
#define NV_DETEP_TOP          56.0f   // the page's top -> the picker
#define NV_DETP_TEMP_Y      (NV_DETP_P2 + NV_DETEP_TOP)
#define NV_DETEP_LIST_GAP     24.0f   // the picker's base -> the first row
// WHERE THE EPISODE LIST STARTS, and it runs to the page's end (NV_DETP_P3). The rows
// scroll inside that window and are clipped to it, so page 3 never shows a row.
#define NV_DETP_EP_Y        (NV_DETP_TEMP_Y + NV_DETWEB_SEA_H + NV_DETEP_LIST_GAP)
#define NV_DETEP_LIST_H     (NV_DETP_P3 - NV_DETP_EP_Y)
// The focus ring, 4px, and it is now a MEASUREMENT on both places rather than a
// rounding: `.series-episode-card.focused .series-episode-thumb` reads
// `box-shadow: rgb(255,255,255) 0 0 0 4px` at 1920, same as the hero's buttons. The
// note that used to sit here said the episode used 2 and that 4 was the nearest whole
// number after a ~1.6 scale factor — read off the stylesheet's base block, which the
// 1920 rules override.
#define NV_DETP_RING           4.0f

// The tabs "Creator and cast | Ratings | More like this | Trailer": font 32/500,
// the selected one white, the others #808080; the "|" divider is 32/700 #808080,
// with 20 of slack on each side.
#define NV_DETP_TAB_Y       (NV_DETP_P3 + NV_DETP_PAD_TOP)   /* 2256 */
#define NV_DETP_TAB_H         51.0f
#define NV_DETP_TAB_SEP       20.0f

// Cast: a card 220 wide, step 270; a 140x140 avatar ALIGNED LEFT in the card (not
// centred); the name 26/500 rgb(179,179,179) and the role 21/400 rgb(128,128,128)
// below.
// The tab strip's base plus the 54 that separated the two when both were measured
// (1992 - 1887 - 51). Every tab body and the Trakt section below stack off this one
// number (baseOfTabActive), so page 3 restacks from here.
#define NV_DETP_EL_Y        (NV_DETP_TAB_Y + NV_DETP_TAB_H + 54.0f)   /* 2361 */
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

// --- THE ACTIONS ROW, MEASURED IN NuvioWeb --------------------------------
//
// NV_DETWEB_* replaces the NV_DETW2_BTN_*/CIRC/FOCUS_* set, and the change of
// source is the point. Those came from the native TCL app (adb screencap,
// 2026-09-01) and gave a row that reads as a different application from the web
// one: a 94-tall pill that GROWS on focus, a white pill in both states, a glyph at
// a third of the circle. The owner's instruction is that the title screen looks
// like NuvioWeb, so the web is the reference here and the device set is gone
// rather than kept alongside — two live sets for one row is how the row drifted in
// the first place.
//
// Measured 2026-09-15 with getBoundingClientRect / getComputedStyle on
// NuvioWeb 0.3.8 at 1920x1080 (`npm run serve`), film "The Whisper Man",
// `.series-detail-actions` and its buttons, at rest AND focused.
//
//   row        x=208, y=562.61, h=96, flex, gap 24
//   primary    193.3x96 for "Play", radius 64, padding 0 36, font 32/600
//   icon       36x36 at x=244 — 36 from the pill's edge, then 24 to the label
//   circles    96x96, radius 999, step 120 (96 + the row's gap of 24)
//   glyph      44x44 centred in the circle (26 of slack on each side)
//
#define NV_DETWEB_BTN_H       96.0f
#define NV_DETWEB_BTN_PADX    36.0f   // .series-primary-btn padding: 0 36px
#define NV_DETWEB_BTN_ICON    36.0f   // the play triangle, square and centred
#define NV_DETWEB_BTN_GAPI    24.0f   // the flex `gap` — icon box -> label ink
#define NV_DETWEB_CIRC        96.0f
#define NV_DETWEB_BTN_GAP     24.0f
#define NV_DETWEB_CIRC_GLYPH   0.4583f  // 44 / 96, and NOT the device's 0.333
// FOCUS IS A RING AND A COLOUR SWAP, AND THE SIZE DOES NOT MOVE.
//
// `transform: none` in both states, checked on the focused element as well as the
// resting ones — there is no scale anywhere in this row. What changes is
// background, ink and a `box-shadow 0 0 0 4px #fff` (NV_DETW_RING).
//
// AND THE PRIMARY IS NOT A WHITE PILL AT REST. That is the correction that most
// changes the screen: `.series-primary-btn` measures rgb(34,34,34) with white ink
// unfocused and rgb(245,245,245) with rgb(17,17,17) focused — the SAME pair the
// circles use. The port had it white in both states, from the device, so the row
// read as one lit button next to three dark ones with nothing to say which had the
// focus.
#define NV_DETWEB_REST        0.133f  // #222222, the resting fill of every button
#define NV_DETWEB_REST_INK    1.000f  // white ink and glyphs at rest
#define NV_DETWEB_FOCUS       0.961f  // #f5f5f5, the focused fill
#define NV_DETWEB_FOCUS_INK   0.067f  // #111111, the focused ink

// THE TOOLTIP over the focused circular button. It is `.series-circle-btn::after`
// with `content: attr(aria-label)` (components.css:17732) and it exists because the
// circles carry no label of their own: the native `title` never fires for a d-pad,
// so the sheet shows the aria-label on `.focused`.
//
// Measured on the focused button, same session: font 24/700, rgba(255,255,255,.92),
// `text-shadow: 0 2px 8px rgba(0,0,0,.8)`, padding 7px 16px, line-height 24 and NO
// background pill — bold text with a shadow, over the backdrop. It is centred on
// the button (`left:50%` + `translateX(-50%)`) and its BOX BOTTOM sits at
// `calc(100% + 16px)`, that is 16 above the button's top edge.
//
// The box is 7 + 24 + 7 = 38 tall, so it occupies the band from 54 to 16 above the
// button — which is why NV_DETW_LOGO_GAP had to grow; see layout.h.
#define NV_DETWEB_TIP_GAP     16.0f   // button top -> the tooltip box's base
#define NV_DETWEB_TIP_PADY     7.0f
#define NV_DETWEB_TIP_LH      24.0f
#define NV_DETWEB_TIP_H       38.0f   // PADY + LH + PADY
#define NV_DETWEB_TIP_ALPHA    0.92f
// It fades in over 140ms while rising 4px, `transition: opacity 140ms, transform
// 140ms`. The rise is what stops it appearing already in place next to a focus that
// moved instantly.
#define NV_DETWEB_TIP_MS     140.0f
#define NV_DETWEB_TIP_RISE     4.0f

// --- THE SEASON PICKER, MEASURED IN NuvioWeb -------------------------------
//
// IT IS A DROPDOWN, NOT A ROW OF PILLS. `.series-season-row` holds ONE
// `.library-picker` — the same control the library screen uses — with
// `aria-haspopup="listbox"`; the row of one chip per season this port drew is the
// Apple TV app's and does not exist on the web. With eight seasons that row also ran
// off the right of the screen, which the dropdown cannot do.
//
// Measured 2026-09-15 at 1920x1080 on the series "The Gentlemen", closed, focused
// and open.
//
//   anchor   x=208, 359.8x80, radius 64, #222, 1px rgba(255,255,255,.1)
//            padding 0 36, flex gap 24 between the label and the chevron
//   label    "Season 1" 30/600 white, then " · 8 Eps" 30/400 rgb(179,179,179)
//   chevron  a 32x32 glyph in rgb(179,179,179), inside a 39.93 box
//   focused  background rgb(48,48,48) AND an INSET ring, not an outer one:
//            `box-shadow: inset 0 0 0 3px rgba(255,255,255,.96)`
//   menu     8 below the anchor, same width, #222, radius 64,
//            1px rgba(255,255,255,.08), shadow 0 8px 32px rgba(0,0,0,.6)
//   option   84 tall, radius 64, font 28/500, padding 20 32; the focused/selected
//            one #f5f5f5 with #111 ink, the rest transparent with white
#define NV_DETWEB_SEA_H       80.0f
#define NV_DETWEB_SEA_PADX    36.0f
#define NV_DETWEB_SEA_GAP     24.0f   // label -> chevron
#define NV_DETWEB_SEA_CHEV    32.0f
// "Season 1" and " · 16 Eps" are ONE string in the web, split here because they are two
// styles (600 white, 400 grey). The spaces around the dot have to be drawn as GAPS:
// given to the tail as a leading space it disappears, because SDL_ttf trims the line it
// rasterises and the label came out as "Season 1· 16 Eps". Each side of the dot, the
// same 16 as NV_DETEP_DOT_SEP on the episode rows underneath — a plain space after it
// (~8 at 30px) read as cramped next to every other dot in the app.
#define NV_DETWEB_SEA_DOT     16.0f
#define NV_DETWEB_SEA_BORDER   1.0f
// The focused ring is INSET, so it eats into the pill instead of growing it. Drawn
// as a ring on the same rect, not as a plate behind it.
#define NV_DETWEB_SEA_RING     3.0f
#define NV_DETWEB_SEA_FOCUS_BG 0.188f  // rgb(48,48,48)
// The open list.
#define NV_DETWEB_SEA_MENU_GAP   8.0f   // anchor base -> the menu's top
#define NV_DETWEB_SEA_MENU_PADY  4.0f
#define NV_DETWEB_SEA_MENU_PADX 10.0f   // 9.984 measured
#define NV_DETWEB_SEA_OPT_H     84.0f
#define NV_DETWEB_SEA_OPT_PADX  32.0f
// How many options fit before the list scrolls. The web caps the menu at
// `max-height: 540px` and scrolls; 540 / 84 = 6.4, so six whole rows.
#define NV_DETWEB_SEA_OPT_VIS      6

// --- THE EPISODE LIST ------------------------------------------------------
//
// A VERTICAL LIST, one row per episode, from the owner's reference (2026-09-22),
// measured off it at 1920 and then opened up a quarter — type and air both — because
// at the reference's own sizes the rows were hard to read on the TV. It replaces
// NuvioWeb's horizontal rail of 600x395 cards,
// which showed two and a half episodes and one synopsis at a time.
//
//   row      236 tall at rest, rows touching. The FOCUSED row grows to hold its whole
//            synopsis, and the rows under it move down on the focus spring.
//   band     the focused row is lit by a white wash from the screen's left edge that
//            DISSOLVES to the right (GFX_ROW_FADE) — no line where it stops
//   thumb    320x180 (16:9), radius 12, at the gutter; focused: grown by
//            NV_DETEP_THUMB_GROW from the gutter, a 4px white ring, and the Continue
//            Watching bar along its base when there is progress
//   copy     56 right of the thumbnail (the focused one pushes its copy along) and
//            centred on the row, on BASELINES: title -> meta 42, meta -> synopsis 44, synopsis leading 35. At rest the
//            synopsis is one line; focused, all of it.
//   watched  a white disc with a bold dark tick in the thumbnail's top-right corner
//   right    focused, "22 min left" and a Resume / Play pill that ends at
//            NV_DETEP_RIGHT
//   rest     every row but the focused one at NV_DETEP_DIM while the list has focus
#define NV_DETEP_ROW_H       228.0f  // 24px of air above and below the 180 thumbnail
#define NV_DETEP_PADY         40.0f   // a grown row's air above and below its copy
#define NV_DETEP_THUMB_W     320.0f
#define NV_DETEP_THUMB_H     180.0f
#define NV_DETEP_THUMB_GROW    1.10f  // the focused thumbnail's scale: 352x198
#define NV_DETEP_THUMB_R      12.0f
#define NV_DETEP_RING          4.0f
#define NV_DETEP_TEXT_GAP     56.0f   // thumbnail -> copy
#define NV_DETEP_BAND_A        0.08f  // the band is white at this alpha
#define NV_DETEP_BAND_FADE     0.50f  // where across the screen the band starts to go
#define NV_DETEP_RIGHT      (NV_SCREEN_W - NV_DETP_X)  // 1824: where the pill ends
#define NV_DETEP_ACT_GAP      40.0f   // the copy's right edge -> the right-hand column
// How lit a row is. DIM is every unfocused row while the list has the focus; REST is
// every row while it does not (the picker is focused, or the page is scrolling in),
// so the list does not look disabled before you have entered it.
#define NV_DETEP_DIM           0.55f
#define NV_DETEP_REST          0.72f
#define NV_DETEP_KICK_GAP     30.0f   // "EP3" -> the title
#define NV_DETEP_KICK_LS       2.5f   // "EP 3"'s letter-spacing
#define NV_DETEP_META_DY      42.0f   // title baseline -> meta baseline
#define NV_DETEP_DESC_DY      44.0f   // meta baseline -> the synopsis' first baseline
#define NV_DETEP_DESC_LD      35.0f
#define NV_DETEP_DOT_SEP      16.0f   // each side of the "·" on the meta line
// The mark's height; the width follows the art. 23, a little over the score's digits,
// as in the owner's reference: at 26 the badge outweighed the number it labels, and at
// 20 it read smaller than the digits beside it.
#define NV_DETEP_IMDB_H       23.0f
#define NV_DETEP_IMDB_GAP     12.0f   // mark -> score
// The focused row's pill: #f5f5f5 with #111 ink, the hero's focused pair.
#define NV_DETEP_BTN_H        72.0f
#define NV_DETEP_BTN_PADX     32.0f
#define NV_DETEP_BTN_ICON     24.0f
#define NV_DETEP_BTN_GAPI     14.0f   // icon -> label
#define NV_DETEP_LEFT_GAP     24.0f   // "22 min left" -> the pill
#define NV_DETEP_CHECK        34.0f   // the watched mark's diameter, on the thumbnail
#define NV_DETEP_CHECK_INSET  11.0f   // from the thumbnail's top and right edges
#define NV_DETEP_CHECK_TICK    0.66f  // the tick's box, as a fraction of the disc

// THE TEXT STACK. Both captures give the same absolute coordinates for what is
// BELOW the synopsis (the IMDb badge at y=938, the age-rating badge at y=999) and
// the synopsis grows UPWARDS — 5 lines on the series, 4 on the film, and the LAST
// line falls at the same y in both. That is why the stack is built from the bottom
// up, and not from the logo down.
#define NV_DETW2_LD_SIN       40.0f   // the step between synopsis lines (measured)
#define NV_DETW2_SIN_LINES       5   // the most seen in the reference
#define NV_DETW2_TEXT_W    1040.0f   // the synopsis and the support line (96..1136)
// Where the ratings row (Rotten Tomatoes, Trakt) stops adding sources.
//
// THE SAME RIGHT EDGE AS THE SYNOPSIS, because it is the same column. It used to be
// 640, which was never measured for this line: it came from the HOME hero's
// NV_HERO_SIN_W — a constant from another screen — and was then frozen under a name
// of its own, which made it look deliberate.
//
// 640 was narrow enough to CLIP, and that is what "sometimes it shows different
// information" was. The budget is tested against the running x, so whether Trakt
// appeared depended on how long the GENRE NAMES were: "Romance" left room and
// "Adventure | Sci-Fi" did not, on the same build, seconds apart. The score was
// there the whole time — the line simply ran out of allowance before reaching it.
#define NV_DETW2_RATE_W    NV_DETW2_TEXT_W
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
// THE PROVIDER LOCKUP that opens the line (Netflix, HBO Max, Apple TV+ ...). The
// box it is fitted into, height-first: the width follows the file's aspect.
//
// 32 and not the 28 it was drawn at. Every badge file carries a uniform 14%
// transparent margin top and bottom (measured: the ink fills 72% of all ten), so a
// 28 box was only ever putting 20px of ink on a line whose type is 26 — the logo
// came out smaller than the words next to it. The box is allowed to overhang
// NV_DETW2_M1_H: there is 33 above the line and 31 below it, and the lockup is the
// one thing here that is a picture rather than a word.
#define NV_DETW2_PROV_H       32.0f
#define NV_DETW2_PROV_MAXW   150.0f

// --- THE SKELETON, and where it is and is not worth having -------------------
//
// Three of this block's fields come off a network thread and land after the screen
// is up: the provider lockup and the "Director:" credit (discover.c's /meta call)
// and the production status (extras.c's TMDB sheet). Drawn as they arrive, each one
// APPEARS AND PUSHES what is already there — and the credit is the worst of the
// three, because the stack is anchored at the base, so its row landing shoves the
// whole action row 62px up under a hand that is already reaching for it.
//
// A placeholder holds the space so nothing moves, then crossfades in place.
//
// IT IS ONLY WORTH IT WHERE SOMETHING SITS TO THE RIGHT, and that is the whole
// design rule here. The provider opens meta line 1 and the status opens meta line 2,
// so a late arrival there moves everything after it; the credit owns a whole row.
// The Trakt and Tomatoes scores CLOSE line 1 and the country closes line 2 — nothing
// follows them, so they can simply fade in where they land and no skeleton is needed.
// Reserving a slot for them would only add a box that has to be guessed and then
// taken away again.
//
// The reserved width for the lockup: the marks run 64 (Apple TV+) to 132 (HBO Max)
// at NV_DETW2_PROV_H, and 108 is the middle of that — near enough that the settle
// when the real mark lands is a few pixels, and the eye does not catch it.
#define NV_DETW2_SKEL_PROV   108.0f
// The credit line's bar. "Director: " plus a name, at the synopsis's size.
#define NV_DETW2_SKEL_SUP    420.0f
// The status badge holds its own outline shape, so the bar matches the box it will
// become: NV_DETW2_BADGE_H tall and the width of a typical word plus the padding.
#define NV_DETW2_SKEL_STAT   190.0f
#define NV_DETW2_SKEL_H       22.0f   // the bar's height on the two meta lines
// HOW LONG A PLACE IS HELD. After this the slot closes and the line settles — ONE
// reflow, at a moment that has passed, instead of one per field as each answers.
// Without a ceiling a title that genuinely has no provider (not on streaming here,
// which is the honest empty case) would hold an empty box for the whole visit.
#define NV_DETW2_SKEL_MS      1400
// The crossfade from the bar to the real thing, in place. Short: nothing moves, so
// this only has to cover the swap.
#define NV_DETW2_SKEL_FADE_MS  180
#define NV_DETW2_META_GAP     31.0f   // 999 - 968
// The IMDb badge: THE REAL MARK, imdb_logo_2016.svg — the file NuvioWeb serves
// from `renderImdbBadge`, drawn 60 wide with its own height following the file's
// aspect (575:289.83, so ~30). The sheet gives it a 60x60 box with
// `object-fit: contain`, which is the same 60x30 once the aspect is honoured.
//
// It used to be a HAND-BUILT PLATE: a 60x30 yellow rectangle with the word "IMDb"
// set in TXT_MINI and centred in it. TXT_MINI is the age-rating size, so the type
// came out far smaller than the box and the badge read as mostly yellow — the
// padding the owner saw. The real mark has the letters running edge to edge, which
// is the whole design of it, and no amount of nudging a font size reproduces that.
//
// It is drawn with GFX_TEXT and not gfx_icon: the mark is yellow with black
// letters, and gfx_icon's GFX_BRAND takes only the alpha and would flatten both
// into one tint. Same reasoning as the brand lockup — see gfx.h.
//
// SIZED BY HEIGHT NOW, like the episode rows (NV_DETEP_IMDB_H). At 60x30 the plate stood
// taller than every glyph on its line and was the loudest thing in the hero; 24 sits
// just over the digits of the 26px line, the same proportion the episode row settled on,
// so the two marks on this one screen read as the same badge. The decode stays at
// NV_IMDB_MARK_TEX_W — see there for why every caller asks for that one width.
#define NV_DETW2_IMDB_H       24.0f
#define NV_DETW2_IMDB_GAP     12.0f   // mark -> score, NV_DETEP_IMDB_GAP's value
// TWO SEPARATORS, and they are deliberately different shapes — which is exactly
// what NuvioWeb does, and what one shape for both got wrong.
//
// BETWEEN GROUPS (genres | date | score) it is a VERTICAL BAR: `.detail-meta-dot`,
// `width: 1px; height: 14px`. The sheet sizes that against a 20.7px meta row
// (--tv-secondary-text at 1920); this line runs at 26 and meta line 2 at 23, so the
// same proportion lands on 2x18 for both.
//
// BETWEEN GENRES it is a DISC, because there the separator is part of a sentence
// rather than a fence: the web builds that line as ONE text run joined with " • ",
// so what falls between two genres is a bullet glyph. Drawing a bar there made the
// genres read as separate fields — the same weight of division between "Drama" and
// "Fantasy" as between the genres and the year, when one is a list and the other is
// a change of subject.
#define NV_DETW2_BAR_W        2.0f
#define NV_DETW2_BAR_H       18.0f
#define NV_DETW2_DOT_D        6.0f
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

// --- THE REGION RECORDER, for the dev channel -------------------------------
//
// Prints the ON-SCREEN rect of each named part of this screen, as drawn THIS
// frame — after the scroll, after the focus scale, after everything.
//
// It exists because cropping a capture by guesswork is expensive twice over: the
// crop misses, and then the full frame gets read anyway to find out where the
// thing actually was. Eight crops missed that way while porting this screen. The
// numbers are recorded BY the drawing rather than recomputed beside it, so they
// cannot drift from what is on the glass.
void detail_regions(void);

void detail_event(const SDL_Event *e);
void detail_update(float dt, Uint32 now);
void detail_draw(Uint32 now);   // desenhe DEPOIS da home: ele cobre

#endif
