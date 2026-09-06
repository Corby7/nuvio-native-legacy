// The native port's visual tokens. The reference is Nuvio 1.0.1 legacy (webOS):
// the modern hero, a fixed rail and rows of posters. The Apple TV prototype lives
// in another build and does not define these values.
//
// Typography scale: a FACT from the tvOS HIG. At 1080p, 1pt = 1px, so the values
// are literal. Title1 76 / Title2 57 / Title3 48 / Headline 38 / Body 29 /
// Caption 25. Body never below 29.
#ifndef NV_LAYOUT_H
#define NV_LAYOUT_H

#define NV_SCREEN_W        1920.0f
#define NV_SCREEN_H        1080.0f

// The legacy shell uses a 72dp rail (144px on the 1080p canvas) and starts the
// content 104px after it, as in the CSS .home-main + --home-content-start.
#define NV_LEGACY_RAIL_W        144.0f
#define NV_LEGACY_CONTENT_X     248.0f
#define NV_LEGACY_CONTENT_RIGHT 104.0f
// The real rule, measured in both states: the content ALWAYS has a 104 inset, and
// the rail adds its own 144 when it is open. They are not two layouts — it is
// `collapseSidebar` in layoutPreferences.js, which the owner's profile has set to
// true. With it collapsed the home starts at 104.
#define NV_CONTENT_PAD          104.0f

// A FULL-SCREEN hero (`modernHeroFullScreenBackdropEnabled`, also true in the
// owner's profile). MEASURED: .home-modern-hero-media 1920x1062 at (0,0), the image
// in `object-fit: cover` with `object-position: 100% 0`. The text block is still
// 640 wide, but at x = NV_CONTENT_PAD and starting at y=40.
// 1080 and not 1062. The 1062 came from measuring `.home-modern-hero-media` in the
// WEB app, and there 18px were left over because the browser window had a bar. On
// this TV the screen is exactly 1080, and the 18px the art did not cover appeared
// as a BAND at the bottom — it was the "hole/margin at the end of the background"
// the owner saw.
//
// MEASURED on the device's capture: y=1060 gave (13,13,13), y=1064 jumped to
// (37,38,41) and stayed that way to the end of the screen, in any column.
//
// INVESTIGATED to the end, and the conclusion matters to whoever touches this next:
// the last 18px ARE NOT OURS. The evidence, in order:
//   - with the hero at 1062 there was a band; raising it to 1080 left the band
//     UNCHANGED, at the same y — so it was not the hero's size;
//   - changing the glClear colour to magenta did not paint that region;
//   - drawing an opaque rectangle there did not paint it either;
//   - SDL_GL_GetDrawableSize answers 1920x1080 (it is in /tmp/nuvio-fps.txt).
// That is: SDL reports 1080 and the real GL surface is 1062. The band is the webOS
// compositor's background showing through, and no drawing by the app reaches it.
//
// It stays at 1080 anyway: it is the CORRECT value for the screen, the excess is
// discarded at no cost, and if a firmware ever returns the whole surface the art
// will cover it by itself.
#define NV_HERO_FULL_H        1080.0f
#define NV_HERO_FULL_COPY_Y     40.0f
// With the hero FULL-SCREEN the text block moves up: the web app puts the logo at
// y=65 and the meta line at 257, against 135 and 327 for the banded hero. MEASURED
// in the signed-in session, which is the one with
// `modernHeroFullScreenBackdropEnabled`. The port used the banded numbers in both
// modes, and that is why all the text sat 70px too low — it is what the owner
// described as the wrong margin.
// The synopsis stays at 411 in BOTH modes; only what is above it moves.
#define NV_HERO_FULL_LOGO_Y     65.0f
#define NV_HERO_FULL_META_Y    257.0f
// Full-screen, the logo can be MUCH larger: 640 wide against the banded hero's 440.
// MEASURED in the signed-in session (.home-hero-logo = 640x160 at 104,65). The port
// limited it to 440 in both modes, and that is what left the title's art small —
// one of the points the owner raised looking at the reference.
#define NV_LOGO_HERO_FULL_MAX_W 640.0f
// THE SECONDARY LINE, which only exists full-screen: "2H REMAINING • 6.3 • EN".
// y=341, height 38, font 18 weight 600, rgba(255,255,255,0.88). It sits BETWEEN the
// meta line (257) and the synopsis (411); without it a gap was left in the middle
// of the block, which is part of what read as wrong spacing.
#define NV_HERO_FULL_SEC_Y     341.0f
// Breathing room between the base of the hero's text block and the first row's
// title, and between the lines of the block itself. It comes from the difference
// measured in the captures: the synopsis ends ~48px above the row's title, and the
// block's lines are ~52 apart.
// The hero's text block, READ from the web app's CSS and not estimated from the
// captures. `.home-modern-hero-copy` (components.css:6723) is a flex column with
// justify-content:flex-end and gap:16 — that is, ANCHORED TO THE BASE, with the
// lines 16 apart, not the 52/84 I had deduced from an image. The base:
//   bottom: var(--modern-rows-viewport-height) + var(--modern-hero-copy-bottom-gap)
//         = 52% of 1080 + 40 = 601.6  ->  the block's base at y = 478.4
// and the top of the rows falls at the same 518.4 NV_SHELF_TOP already uses.
#define NV_HERO_COPY_GAP        40.0f   // --modern-hero-copy-bottom-gap
#define NV_HERO_COPY_LINE      16.0f   // the flex column's gap

// The BACK scancode in LG's SDL (SDL_SCANCODE_WEBOS_BACK). It is not in the
// standard SDL_scancode.h, so it comes as a number.
#define NV_SCANCODE_BACK 482
#define NV_SCANCODE_BLUE 489 // SDL_webOS.h: SDL_WEBOS_SCANCODE_BLUE
// How long OK has to be held to count as a long press. 500ms is the classic
// threshold: shorter fires by accident, longer feels as though the button did not respond.
#define NV_HOLD_MS       500
// the slice of the neighbouring row that stays visible above/below the focused one
#define NV_PEEK_NEIGHBOUR 0.10f

// Safe area: the HIG asks for >=60pt from the edges; classic overscan uses 80-90 at
// the sides. Measured on the reference photos: it matches 90.
// The OFFICIAL tvOS safe area: 80px at the sides, 60px top and bottom (HIG
// Layout). The 90 that used to be here was a guess of mine.
#define NV_MARGIN_X      80.0f
#define NV_MARGIN_Y      60.0f

// In the modern legacy layout the art occupies the top band (72% of the width and
// ~650px tall); the rows' viewport stays fixed in the bottom 52%.
#define NV_HERO_H       650.0f
// Measured on the device photo: the hero's button row ends at ~65% of the height,
// and the first row starts just below it, appearing cut off. With the base at 150
// the buttons came down to where the row's header begins, and the two overlapped.
// Measured on the device photo: the hero's button row ends at ~69% of the height
// and the row's header comes ~100px later. With the base at 380 there was too much
// margin left between the block and the cards.
#define NV_HERO_BASE     570.0f
// How much scrolling before the hero's block starts to disappear, and over how many
// px it disappears completely.
#define NV_HERO_FADE_START 420.0f
#define NV_HERO_FADE_EXT 260.0f
#define NV_BACKGROUND_DARK   0.40f   // how much the home's background darkens the art
// How much scrolling before the hero's ART starts to disappear, and over how many
// px. After that what you see is the blurred background.
#define NV_HERO_ART_START 300.0f
#define NV_HERO_ART_EXT 520.0f
#define NV_HERO_BUTTON_H   68.0f
#define NV_HERO_NBUTTONS      3
// MEASURED in the web app: .home-modern-hero-media at x=555, y=0, 1421x670, with
// the art in object-fit:cover. The gradients that dissolve the left edge and the
// base are in the GFX_HERO shader, with the stops recorded there.
#define NV_HERO_ART_X   555.0f
#define NV_HERO_ART_W  1421.0f
#define NV_HERO_ART_H   670.0f
// CSS (components.css:6755): .home-hero-logo in modern has height AND max-height
// --modern-hero-logo-max-height (200px), width min(100%, 440px), object-fit contain
// with object-position LEFT TOP. That is, the box always measures 440x200 and the
// art sits against its TOP — it does not grow from the base, which is what the port
// did. 160 was the measurement of one particular piece of art, not of the box.
#define NV_LOGO_HERO_H   200.0f

// When a logo is TMDB's DARK VARIANT and needs to be tinted white.
//
// TWO conditions, and the second matters as much as the first. The numbers come
// from measuring the 40 logos in art/logo (the average of the opaque pixels, chroma
// = max(RGB)-min(RGB)):
//
//   file     lum  chroma  decision
//   08, 19     0      0   TINT   — pure black, the wrong variant
//   07        17     19   TINT   — almost black
//   27        74     21   TINT   — dark grey, illegible over the backdrop
//   01        63    124   pass   — DARK RED: a brand colour, deliberate
//   24        76    255   pass   — pure red
//   38        84     43   pass
//   20       129      0   pass   — achromatic, but light
//
// Luminance alone will not do: it would fail 01 along with the blacks, and tinting
// a dark-red logo white swaps one defect for another. Chroma alone will not either:
// it would fail 20, which is LIGHT grey and reads well. It is the conjunction that
// isolates exactly the wrong variant — dark AND with no colour of its own.
//
// The luminance threshold is 80 and not 70 because of 27, which measures 74: at 70
// it escaped by four points and went on disappearing on screen.
#define NV_LOGO_LUMA_MIN     80
#define NV_LOGO_CHROMA_MAX   45
#define NV_LOGO_HERO_MAX_W 440.0f
// ABSOLUTE positions of the hero's text block, measured in the web app with the
// home at the top. This used to be anchored to the BASE (base = 1080 -
// NV_HERO_BASE) and stacked upwards, which is how the Apple app does it — the side
// effect was the text coming down as the synopsis grew and touching the first row's
// title. In the web app each line has a fixed place:
//   logo      y=135  (h 160)
//   meta      y=327  (font 21, weight 500, rgb(179,179,179))
//   synopsis  y=411  (width 640, font 22, h 89 over 2 lines)
// and row 0 starts at 518, just below the 500 where the synopsis ends.
#define NV_HERO_LOGO_Y   135.0f
#define NV_HERO_META_Y   327.0f
#define NV_HERO_SIN_Y    411.0f
// 560 is the default theme's value; the TV falls into the `.legacy-webos` rule
// (components.css:19171), which returns 640 for the meta, the secondary line AND
// the synopsis — at 560 an episode's meta line broke over two or three lines.
#define NV_HERO_SIN_W    640.0f

// THE EDITORIAL HERO FOR COLLECTIONS. A collection's art is already a finished 16:9
// composition (gradient, light and breathing room); the text must not compete with
// a second large cover on the right. The positions follow the Awards screen's
// reference: the group at the top, the list's real logo centre-left and the action
// coming to rest just before the row's header.
#define NV_COLLECTION_HERO_GROUP_Y      182.0f
#define NV_COLLECTION_HERO_LOGO_Y       258.0f
#define NV_COLLECTION_HERO_LOGO_MAX_W   520.0f
#define NV_COLLECTION_HERO_LOGO_MAX_H   150.0f
#define NV_COLLECTION_HERO_CAPTION_Y    448.0f
// line-height do CSS: sinopse 22*1.35, meta 21*1.25, secundaria 18*1.35.
#define NV_LD_HERO_SIN   30
#define NV_LD_HERO_META  26
#define NV_LD_HERO_SEC   24
// MEASURED in the web app: the first row's title sits at y=518 and the cards at
// y=564. The "2/3 of the screen" rule that used to be here is from tvOS's
// productTemplate, and is not ours: in the web app the rows rise over the bottom
// part of the hero's art (which runs to 670), instead of starting after it.
#define NV_SHELF_TOP     518.0f   // the top of the first row's header
#define NV_LEGACY_ROW_HEAD_H 46.0f // title + margin down to the cards (564 - 518)

// The four visual sections the Apple TV home uses, each with its own proportion —
// OBSERVED in the reference photos:
//  1. HERO      full-bleed 16:9 art that CHANGES by itself (carousel + dots)
//  2. CARD      an ordinary 16:9 landscape, the default row
//  3. POSTER    2:3 portrait, used in the Top 10 beside the numeral
//  4. HIGHLIGHT a large card with a metadata block below ("Watch next")
// The OFFICIAL widths from tvOS's grid table (they add up to 1760 = the usable area):
//   4 columns -> 410   |   5 -> 320   |   6 -> 260   |   8 -> 184
// MEASURED in the web app running at 1920x1080 (getBoundingClientRect, not reading
// the CSS): .home-content-card = 212 x 322, the first card at x=248, the second at
// x=520. The previous values came from tvOS's grid and were wrong on both: a height
// of 318 (the "neat" 212 x 1.5 the web app does not use) and a gap of 24.
#define NV_CARD_W        212.0f
#define NV_CARD_H        322.0f

// This gutter belongs to the POSTER card only. The continue-watching card is
// built differently in the web: `.home-continue-media` has `border: 0` and the
// artwork does run edge to edge (components.css:5893).
#define NV_CARD_BORDER   2.0f   // .home-content-card border-width
#define NV_FRAME_BORDER  2.0f   // .home-poster-frame border-width
#define NV_CARD_PAD      (NV_CARD_BORDER + NV_FRAME_BORDER)

// 24, and the 60 that used to be here was a MEASUREMENT ERROR. The arithmetic
// "520 - 248 = 272 of step, minus the card's 212 = 60" measured a state with the
// card EXPANDED by focus; the step at rest is different.
//
// MEASURED in the reference app (com.nuvio.tv on the TCL, 1920x1080, gutter
// detection column by column): card 210, gutter 24.0 px on every consecutive
// occurrence, step 234. The web app's CSS agrees: `--home-track-gap: 24px` in
// `.home-layout-modern`. Two independent sources against one piece of arithmetic
// done on the wrong state.
//
// Effect: from ~6.6 to ~7.8 posters per screen, and the row stops looking sparse —
// which was the opposite of the defect the old comment claimed to fix.
#define NV_CARD_GAP      24.0f
// The step between rows MEASURED: row 0's title at y=518, row 1's at y=934 -> 416.
// Of those, 46 are from the header down to the cards (518 -> 564) and 322 the
// card's, leaving 48 of breathing room between one row and the next.
#define NV_ROW_GAP  48.0f

// A LANDSCAPE POSTER (`modernLandscapePostersEnabled`). MEASURED in the web app
// with the preference on: `.home-poster-card.is-landscape` = 318 wide, a
// 314x178.875 (16:9) frame with a 2px border around it -> a box of 318x182.9.
//
// Where the 318 comes from: the modern layout's stylesheet defines
// `--home-landscape-poster-width: calc(var(--home-poster-width) * 1.5)` and
// `--home-landscape-poster-height: calc(... * 0.5625)` over the modern layout's own
// `--home-poster-width: 212px` (components.css:6462). It does NOT come from
// `posterCardWidthDp`: the inline variable `buildModernHomeSizingStyle` writes
// (399x225 for 120dp) is overridden, and this was CHECKED in the running app —
// changing the variable to 300px did not move the card by a pixel.
//
// The landscape row also tightens the vertical step: `--home-row-gap` drops from 32
// to 24 in `.home-modern-landscape-posters` (components.css:6473).
#define NV_CARD_LAND_W   318.0f
#define NV_CARD_LAND_H   182.9f   // a 178.875 frame + 2px of border top and bottom
#define NV_CARD_LAND_ART 178.875f
#define NV_ROW_GAP_LAND 24.0f
// The landscape card's caption sits INSIDE the frame: left/right 14, bottom 12, a
// maximum width of 76% of the card, over a gradient covering 54% of the height.
#define NV_LAND_COPY_PAD  14.0f
#define NV_LAND_COPY_BASE 12.0f
#define NV_LAND_COPY_MAXW 0.76f
#define NV_LAND_VEIL       0.54f

// The label below the poster (`posterLabelsEnabled`). `.home-poster-copy`: padding
// 8px 2px 0, fixed height `--home-poster-copy-height: 74px`, title 16/500 and
// subtitle 13/400 rgba(255,255,255,0.7).
//
// CAREFUL: in the MODERN layout the stylesheet hides this block —
// `.home-screen-shell.home-layout-modern .home-poster-copy { display: none }`
// (components.css:7334) — and that is why the web app's Settings screen does not
// even show the option when the layout is modern (`!isModernLayout` in
// settingsScreen.js:4050). The port draws the block when the preference is on; see
// the note in home.c.
#define NV_POSTER_COPY_H   74.0f
#define NV_POSTER_COPY_PADT 8.0f
#define NV_POSTER_COPY_PADX 2.0f

#define NV_POSTER_W      212.0f
#define NV_POSTER_H      322.0f
// The Top 10 reserves space below the poster for the genre label.
#define NV_TOP10_LABEL   38.0f
#define NV_POSTER_NUM_W  118.0f   // the band for the Top 10's giant numeral

// CORRECTED after a reference photo: on the Apple TV the large card's metadata sits
// INSIDE the art, overlaid at the base over a dark veil — not below it. The title's
// logo appears embedded in the key art itself.
// Measured by proportion in the photos: the large card takes ~40% of the screen's
// width (~760px at 1920). And it is NOT 16:9 — comparing the height in the Apple TV
// photo, the proportion is close to 3:2. Since our source art is a 16:9 backdrop,
// the shader crops (cover) instead of stretching; without that the image deforms,
// which was exactly the defect that showed up on the first attempt.
#define NV_HIGHLIGHT_W    419.0f
#define NV_CW_PAD          18.0f
#define NV_CW_BAR_H         4.0f
#define NV_CW_BAR_BOTTOM   10.0f
#define NV_CW_BADGE_PAD_X  14.0f
#define NV_CW_BADGE_PAD_Y   8.0f
#define NV_CW_BADGE_RADIUS  7.0f
#define NV_HIGHLIGHT_H    236.0f   // continue watching: 419 x 236
                                  //  compared side by side on the TV)
// The hero carousel: the time on each piece of art and the crossfade's duration.
#define NV_HERO_INTERVAL_MS  7000
// ERASE the old art, do NOT dissolve one into the other.
//
// MEASURED against the reference (a screenrecord from the device, frames with
// timestamps, three hero changes on the home, the RIGHT key on "Continue
// watching"):
//   - the art stays intact until ~90 ms after the keypress (input latency);
//   - from there it FADES OUT, reaching alpha 0 at ~450 ms after the keypress,
//     that is ~330 ms of fading;
//   - the screen is REALLY EMPTY (just the background) for a time that depends on
//     the loading: I measured 120 ms with the art cached and 780 ms without;
//   - the new art comes in as a HARD CUT, in a SINGLE FRAME. On one of the changes
//     the luminance of the art's region jumped from 20.5 to 93.1 between two
//     consecutive frames (+72.5), and over the following 1.8 s the recorder did not
//     emit a single frame — nothing moved. There is no fade in.
// There is no crossfade at any point: the old art and the new one never appear
// together. It used to be 900 ms of blending, almost triple the time and the wrong
// shape.
#define NV_HERO_FADE_MS       220.0f
// A REST PERIOD BEFORE CHANGING THE HERO. The background only follows the focus
// once it has STOPPED for this long.
//
// Without it, crossing a row of 12 cards changed the hero 12 times: the art
// flickered at every step (the owner: "in the other app, if I move quickly through
// the posters, it keeps the art it had until I stop on a film") and, worse, every
// change ASKED FOR A 1920 TEXTURE — ~8 MB each. Twelve of those in two seconds blow
// the cache's budget and evict precisely the posters that are on screen, which is
// the other complaint ("it still isn't showing all the posters"). The two were the
// same defect.
//
// 220 ms: above the repeat interval of a held D-pad (~130 ms on this TV), so
// crossing the row fires no change at all; and short enough that stopping on a card
// and seeing the background respond feels immediate.
#define NV_HERO_IDLE_MS    220
#define NV_HERO_DOT           9.0f
#define NV_HERO_DOT_GAP      14.0f

// Typography (px at 1080p)
// The tvOS type scale at 1080p (1pt = 1px). The weights come from the file: the TV
// only has Light and Regular of the LG font, so the bold is synthetic.
#define NV_FT_TITLE1    76
// 56 and not the tvOS scale's 57: today this style serves only
// `.library-page-title` and the empty-state titles of search and library, and all
// three measure 56 in the web app. The Apple app's title-page header, which owned
// the 57, no longer exists in the port.
#define NV_FT_TITLE2    56
#define NV_FT_TITLE3    48
#define NV_FT_HEADLINE   38
// The small body sizes sit BELOW the tvOS table on purpose. The official scale
// assumes SF Pro, and Inter — which is the possible substitute here — is visibly
// wider: at the same size, the same sentence took 336px against 225px in the
// device's capture. Keeping the official numbers would make each card's block of
// text half again as large as the original's. The titles stay at the official
// values, where the measurement matched (cap 40 against 41).
#define NV_FT_BODY       25
#define NV_FT_CALLOUT    28
#define NV_FT_CAPTION    22
#define NV_FT_CAPTION2   21
#define NV_FT_MINI       15   // the age-rating badge (an icon, not text)
// The PLAYER's sizes. They do not come from the tvOS scale: they come from the web
// app, which is this variant's reference. The values are resolved for 1920x1080,
// which is where the app runs — in the CSS they are min(2.92vw,56px) and
// min(1.67vw,32px), and the TV always hits the ceiling. 56 does not become TITLE2
// (57) and 32 does not become HEADLINE (38) because the difference shows: the
// subtitle at 38 pushes the progress bar out of the place the web app reserves for it.
// The web app's .home-row-title: 26px, weight 600. The native one used HEADLINE
// (38), and that is what made the row's title invade the card just below it.
// 33 and not 26. MEASURED by comparing the SAME string ("Top 100 Today - Film")
// present in both captures: the ink's width 258 px in ours against 329 in the
// reference, the height 25.2 against 32.0 — a ratio of 1.27 on both axes. 26 x 1.27
// = 33. The 26 came from the web app's `.home-row-title`; the web and the TCL
// diverge here and the TCL wins, being the device.
#define NV_FT_ROW_TITLE 33
// .home-modern-hero-secondary: 18/600 in the signed-in session.
#define NV_FT_HERO_SEC   18   // --modern-hero-secondary-size (212*0.085)
#define NV_FT_HERO_META  21   // --modern-hero-meta-size (212*0.1), weight 500
#define NV_FT_HERO_SIN   22   // --modern-hero-description-size, weight 400
// The DETAIL screen, measured in the running web app (getBoundingClientRect and
// getComputedStyle over .series-detail-shell), not read from the stylesheet.
#define NV_FT_DET_BUTTON  25   // .series-primary-btn (weight 600)
#define NV_FT_DET_META   25   // .series-detail-support and .detail-meta-row
#define NV_FT_DET_SIN    26   // .series-detail-description
#define NV_FT_DET_META2  23   // .detail-meta-row.secondary
#define NV_FT_PLR_TITLE 56   // .player-title
#define NV_FT_PLR_BODY  32   // .player-subtitle and .player-time-label
// The player's top corner. The clock and the "Ends at" come from the ATV block
// (components.css:15282), already converted to the 1920 canvas; the parental guide
// is not redone there and keeps the base rule's 22.
#define NV_FT_PG_CLOCK 26   // .player-clock
#define NV_FT_PG_END     20   // .player-ends-at
#define NV_FT_PG_LABEL  22   // .player-parental-label
#define NV_FT_PG_SEV    22   // .player-parental-severity
// The OFFICIAL leading of each style. Using the height SDL_ttf returns is not the
// same thing: it varies with the line's accents, so a paragraph ends up with
// irregular spacing line by line.
#define NV_LD_TITLE1    96
#define NV_LD_TITLE3    56
#define NV_LD_HEADLINE   46
#define NV_LD_BODY       32
#define NV_LD_CAPTION    29
#define NV_LD_CAPTION2   30

// THE FOCUS RING, in pixels, for the WHOLE app. MEASURED against the reference: 4
// solid px of pure white, with no ramp, outside the element's box. It holds for the
// home card, the episode card, the detail button and a keyboard key — one number.
// NV_DETW_RING was already 4 and was only applied on the detail screen.
#define NV_RING_FOCUS      4.0f

// POSTER CARD FOCUS RING. The poster is the one card that does NOT take the
// 4px outer ring above, and the reason is structural rather than a taste call:
// its ring is the 2px border of `.home-poster-frame`, transparent at rest and
// lit to rgba(255,255,255,0.8) on focus (components.css:5619). Being a border
// on a border-box element it grows INWARD, so it sits in the 4px gutter that
// NV_CARD_PAD opens — outer edge 2px in from the card box, inner edge flush
// against the art, and the card keeps its footprint.
//
// The 4px outer ring stays right where the web puts it: on the
// continue-watching card, whose `::before` carries
// `0 0 0 4px var(--focus-color)` — a real outset box-shadow (components.css:
// 5874), with --focus-color #ffffff (base.css:24).
//
// THICKER THAN THE WEB, and deliberately: the sheet says 2px, the owner asked
// for more after seeing 2px on the C3. At 4 it fills the whole NV_CARD_PAD
// gutter, so the lit border runs flush with the card outline and the artwork
// keeps its 4px inset — the resting card does not move when it takes focus.
// The alpha is still the web's 0.8, because only the weight was the complaint.
#define NV_FRAME_RING      4.0f
#define NV_FRAME_RING_A    0.8f

// Area util explicita da home: a rail pode variar, mas o texto e o foco nunca
// encostam na safe area direita.
#define NV_HOME_SAFE_RIGHT    NV_LEGACY_CONTENT_RIGHT
#define NV_HOME_TEXT_GUTTER   24.0f
#define NV_HOLD_FEEDBACK_MS   110.0f

// FOCUS ON A SURFACE (pill, menu item, chip): a DARK background with white text —
// not the other way round.
//
// We used #E4E4E9 (light) with dark text, which besides being inverted relative to
// the reference is no system colour at all: neither #FFFFFF nor the #F5F5F5 of
// --secondary-color. It was an invented bluish off-white. The reference has ONE
// token: --focus-bg #303030, confirmed in the web app's CSS and MEASURED exactly on
// the TCL.
//
// A legitimate exception: the detail screen's primary button ("Play") is white with
// black text in BOTH apps. That one stays as it is.
#define NV_COLOR_FOCUS_R     0.188f
#define NV_COLOR_FOCUS_G     0.188f
#define NV_COLOR_FOCUS_B     0.188f

// Radii, as a fraction of the smaller side (the shader uses a normalised SDF)
#define NV_RADIUS_CARD     0.055f
#define NV_RADIUS_PILL     0.5f
#define NV_RADIUS_BADGE    0.18f

// Background: #0D0D0D. This used to be #252629, with the justification that "the
// near-black made the cards float in the void" — but the reference IS near-black:
// MEASURED #0D0D0D on the TCL's home and #020202 on the scrolled home, and
// `--bg-color: #0D0D0D` in the web app's CSS. The two sources agree.
//
// It is not cosmetic. With #252629 the placeholder for a card with no art (#242429)
// sat at a distance of (1,2,0) from the background — a contrast of 1.0:1, that is,
// INVISIBLE. The posters "that were not appearing" were appearing: as rectangles in
// exactly the background's colour. See NV_COLOR_SKELETON just below.
#define NV_COLOR_BACKGROUND_R   0.051f
#define NV_COLOR_BACKGROUND_G   0.051f
#define NV_COLOR_BACKGROUND_B   0.051f

// The surface of a CARD WITH NO ART (#2C2C2C). MEASURED against the reference,
// which draws it solid in the card's exact box while the image has not arrived —
// a luminance ~22x the background's, impossible to miss. It is what makes "loading"
// read as loading instead of as broken.
#define NV_COLOR_SKELETON_R 0.173f
#define NV_COLOR_SKELETON_G 0.173f
#define NV_COLOR_SKELETON_B 0.173f

// POSTER SURFACE WITH NO ART. On the home the web does not use the flat
// #2C2C2C above: `.content-poster` — the element that IS the image, and the
// only one visible while the file is in flight — carries
// `background: linear-gradient(180deg, #1c1c1c, #111)` (components.css:5641).
// The frame behind it never shows through, so this gradient is the whole
// loading surface.
//
// It is darker than #2C2C2C, and the note above is still the reason that value
// exists: a card must not vanish into the #0D0D0D page. It does not — #1c1c1c
// is 28 against 13, and it stays legible all the way down to the #111 (17) at
// the bottom edge. NV_COLOR_SKELETON keeps its job on the screens that have no
// such layer (search, library).
#define NV_POSTER_BG_R      0.110f   // #1c1c1c, top of the gradient
#define NV_POSTER_BG_G      0.110f
#define NV_POSTER_BG_B      0.110f
// Black laid over that top colour to land on #111 at the bottom: 1 - 17/28.
#define NV_POSTER_BG_FADE   0.393f

// Focus: a scale of 1.05-1.10x in the HIG. We use 1.09 on the card.
// The focus scale DERIVED from the official Top Shelf tables, which publish focused
// and unfocused sizes: a 2:3 poster and a square grow ~14%, a 16:9 card grows ~9%.
// I used 9% for everything, which left the poster undersized.
#define NV_FOCUS_SCALE   0.09f    // 16:9 cards
#define NV_FOCUS_SCALE_P 0.14f    // 2:3 posters and circles

// THE HOME'S OWN FOCUS SCALES, which are NOT the two above. Those come from tvOS's
// Top Shelf tables; these are read off the web app, and they are much smaller — the
// home card barely grows, and does the work of showing focus with a border and with
// the dimming of everything else.
//
// The two card families really do differ, and this is not an oversight in the sheet:
//   .home-layout-modern .home-content-card.focused  -> scale(1.02)  (components.css:7747)
//   .home-continue-card.focused                     -> scale(1.05)  (components.css:5933,
//                                                                    with !important)
// Specificity decides the poster: the 1.05 at components.css:5587 is real but dead in
// the modern layout, where 7747 (0,4,0) outranks it. In classic/grid the 1.05 is what
// runs, which is presumably where it came from.
//
// Both grow from `transform-origin: top` — that is `50% 0%`, so horizontally centred
// and vertically PINNED. The card only ever grows downward. This matters: the note that
// used to sit on scaleOf() records a focused card rising 22px into the row title, and
// that was 9% on a CENTRED origin plus an 8px lift. Anchored at the top it cannot
// happen again.
//
// The see-all card does not scale at all (components.css:5804, transform: none
// !important), and neither do loading skeletons.
//
// NOT APPLICABLE HERE, but worth recording so nobody "fixes" this against the wrong
// device: components.css:19313 crushes the scale to 1.005 under .legacy-webos /
// .legacy-tizen. That needs webOS <= 6 (js/app.js:186) and the C3 is webOS 23, so the
// full 1.02 is what the reference actually shows on the owner's TV.
#define NV_FOCUS_SCALE_POSTER 0.02f
#define NV_FOCUS_SCALE_CW     0.05f

// EVERY CARD THAT IS NOT FOCUSED IS DIMMED. This is the strongest focus cue the web
// has — stronger than the border or the scale — and it is global, not row-scoped:
// every poster and continue card on screen that is not the focused one is darkened,
// including the ones in other rows. With focus parked outside the rows, they are all
// dim.
//
//   .home-screen-shell .home-poster-card.focusable:not(.focused)   { filter: brightness(0.8) }
//   .home-screen-shell .home-continue-card.focusable:not(.focused) { filter: brightness(0.8) }
//                                                            (components.css:21945, 21951)
//
// The `.focusable` in those selectors is what keeps loading skeletons at full
// brightness (homeScreen.js:2610 omits the class while a card is loading), and the
// see-all card is neither of those two types, so it is never dimmed either.
//
// This value used to live in the GFX_CARD shader as `color *= 0.80` and was taken out
// as a port invention. It was not — 0.8 is exactly right. What was wrong was the reach:
// the shader dimmed EVERY card in the app drawn with focus 0, which is detail
// thumbnails, player key art, episode stills, the search and library grids, screens
// with no focus at all. Scoped to the home's cards, where the web puts it, it is
// correct. Do not move it back into the shader.
//
// No .performance-constrained or .legacy-* rule disables it: it ships on every device.
#define NV_DIM_UNFOCUSED      0.8f
// The focused item also RISES, it does not only grow: on tvOS it lifts towards the
// viewer and the shadow falls beneath it. Without the offset, the scale and the
// shadow together read as "the image swelled", not as "this item came forward".
#define NV_FOCUS_LIFT      8.0f
// The focused item's shadow. The numbers come from third-party reimplementations of
// the tvOS effect (Apple does not publish theirs): a 25px radius, offset 16px
// downwards, black at 30%. The vertical offset matters more than it seems — a
// centred shadow reads as a halo, a fallen shadow reads as a lifted object.
#define NV_FOCUS_SHADOW   25.0f
#define NV_SHADOW_DY     16.0f
#define NV_SHADOW_ALFA   0.30f

// Springs: the stiffness used in anim_spring(). ~250-350ms to settle, with no
// overshoot. Gaining focus is faster than losing it: an asymmetry Apple states in
// the HIG ("focusing animations should be prominent, unfocusing subtler"). With the
// same stiffness in both directions the navigation has a uniform weight that does
// not exist on the device.
// THE FOCUS RING — MEASURED against the reference: it does not fade, it JUMPS.
//
// I traced the focused card's white edge frame by frame. On the first frame drawn
// after the keypress (16-49 ms) the ring is already on the new card at full white,
// and the web app's stylesheet declares 120 ms `ease` for focus/border. The 13.0 /
// 8.5 that used to be here gave 230 ms and 350 ms to 95% — double and triple.
// 25.0 closes 95% in 120 ms, which is the measurement. The focus/blur asymmetry
// that was here came from the tvOS HIG, not from this interface: in the reference
// both sides take the same time, and with different times there is an instant with
// TWO rings on screen, which the reference never shows.
#define NV_SPRING_FOCUS     25.0f    // coming into focus  (95% in 120ms)
#define NV_SPRING_BLUR  25.0f    // leaving it         (the same time: see above)
#define NV_SPRING_SCROLL    8.0f
// The frequency (rad/s) of the second-order spring that scrolls the home's rows. It
// takes the k of the TAIL measured on the reference's glide (~12.5 /s); 11.5 is the
// value that makes the whole curve match, because in this spring the tail is only
// half the fit: p(t)=1-(1+wt)e^-wt crosses the halfway point at 1.678/w = 146 ms
// with w=11.5, and the measurement was ~145 ms. See anim_spring2() in anim.h for why
// the spring was swapped.
#define NV_SPRING2_SCROLL  11.5f
#define NV_SPRING_SCREEN      9.0f
// The opening of the detail's sections PAGE. MEASURED in the web app's stylesheet:
// `.series-detail-shell.detail-scrolled .series-detail-backdrop` goes to
// `opacity: 0.15` over 0.8s cubic-bezier(0.4, 0, 0.2, 1). exp(-3.8*0.8) = 0.05,
// that is 95% of the way in 800ms. With NV_SPRING_SCREEN (9.0) the spring settles in
// ~330ms and the art fades out in a blink, which is less than half the web app's time.
#define NV_SPRING_PAGE    3.8f

// The detail screen: a CARD of the art covering almost everything, with the home
// showing through the frame. The card's flight uses NV_SPRING_SCREEN, deliberately
// slower than the focus's: changing screen is a larger movement, and at the focus's
// stiffness it would become a cut.
// Measured in the video of the Apple app: the central card takes ~88% of the width
// and ~94% of the height. The SIDE margin is large on purpose — it is through it
// that the neighbouring cards show, ~95px on each side. With a small margin the card
// becomes full screen and the glide stops looking like a change of card: it looks
// like a change of frame in a film, which is exactly what the owner saw in the first
// version.
// MEASURED by homographic rectification of a frame from the device (the TV's screen
// mapped to exactly 1920x1080; validation: the card's centre landed at 957 of the
// expected 960). A 1674x?? card with a side margin of 120 and a top one of 38 — and
// it has NO bottom margin: it is cut off by the bottom of the screen.
// ---------------------------------------------------------------------------
// The DETAIL screen — the web app's FULL-BLEED layout.
//
// Everything MEASURED in the web app running at 1920x1080, with the title "The
// Whisper Man" open (getBoundingClientRect). What used to be here — a card with a
// 120px frame, a carousel of neighbours, three zoom levels — is the Apple TV app's
// pattern, not this variant's. The web app has no card: it has the backdrop
// covering 1920x1080 at (0,0), the horizontal vignette over it, and ONE content
// column anchored to the base.
//
//   .detail-hero-section   padding 0 96 32 72, justify-content: flex-end
//   .series-detail-logo    261x104 at (72, 445)   [fixed height 104, max-w 710]
//   .series-detail-actions 1752x108 at (72, 589), padding 6, gap 24
//     .series-primary-btn  298x96  at (78, 595)  radius 64, font 25/600
//                          side padding 48, icon-text gap 16, icon 36
//     .series-circle-btn   84x84   at (439|586|734, 601)  radius 999, bg #222
//   .series-detail-support 1040x36  at (72, 727)  font 25/400 rgb(179,179,179)
//   .series-detail-descr.  1040x117 at (72, 787)  font 26/400 white, lh 39
//   .detail-meta-stack     1752x120 at (72, 928)  gap 16
//     .detail-meta-row     y=928 h=49, font 25/400 rgb(179,179,179);
//                          genres on the left, the YEAR pushed right (1824)
//     .detail-meta-row.sec y=1003 h=45, font 23/400 WHITE; duration and country
//
// The gaps between blocks (30, 24, 24) are CSS margins and not layout slack: with a
// shorter synopsis the web app shrinks from the base, because the column is
// flex-end. So here too it stacks FROM THE BOTTOM UP.
#define NV_DETW_X          72.0f   // the content column
#define NV_DETW_DIR      1824.0f   // the usable right edge (1920 - 96)
#define NV_DETW_BASE     1048.0f   // the block's base (1080 - 32 of padding)
// 104x710 was what was MEASURED in the web app (.series-detail-logo 261x104 at
// 72,445), but that measurement came from a narrow window. On the 1920 screen the
// title's art took up a quarter of the width and the owner pointed it out side by
// side with their reference, where it takes over two thirds. 200x1000 doubles the
// size without letting the logo dominate the text column below it.
//
// A DELIBERATE DIVERGENCE from the web app's measurement, and not an oversight.
#define NV_DETW_LOGO_H    200.0f
#define NV_DETW_LOGO_MAXW 1000.0f
#define NV_DETW_LOGO_GAP   40.0f   // the logo's base to the top of the actions row
#define NV_DETW_ACTIONS_H   108.0f   // includes the focus ring's 6px of padding
#define NV_DETW_BTN_H      96.0f
#define NV_DETW_BTN_PADX   48.0f
// 34 and not the 16 the flex `gap` declares. MEASURED in both states: the icon
// starts at 126 and the label at 196, and the icon is 36 wide — 34 left over. The
// stylesheet lies here, as it lied about the player title's body size.
#define NV_DETW_BTN_GAPI   34.0f   // icon -> label
#define NV_DETW_BTN_ICON  36.0f
#define NV_DETW_CIRC       84.0f
// The buttons sit in FLOW, with 63px between one and the next. The x=439/586/734
// positions that used to be here are not constants: they are what the arithmetic
// gives when the label is "Play" and there is no secondary button. Measured on two
// different screens (Whisper Man signed out, Silo signed in): on both the gap
// between neighbouring buttons is 63, and the primary changes width with its label
// — "Resume S2E3" gives 334 instead of 298, and everything to the right moves with it.
#define NV_DETW_BTN_GAP    63.0f
#define NV_DETW_RING        4.0f   // the focused item's box-shadow 0 0 0 4px #fff
#define NV_DETW_GAP_ACTIONS  30.0f   // actions -> "Director:"
#define NV_DETW_GAP_SUP    24.0f   // "Director:" -> synopsis
#define NV_DETW_GAP_SIN    24.0f   // synopsis -> the meta stack
#define NV_DETW_TEXT_W  1040.0f   // the width of the synopsis and the support line
#define NV_DETW_LD_SUP     36.0f   // the support line's line-height
#define NV_DETW_LD_SIN     39.0f   // the synopsis's line-height
#define NV_DETW_SIN_LINES    3    // 117 / 39
#define NV_DETW_META_GAP   26.0f   // gap 16 + the second line's margin-top 10
// The resume line, only when the title has progress. MEASURED in the signed-in
// session (Silo, 45%): 1720x37 at (72,633), font 22.66/400 rgba(255,255,255,0.82),
// between the actions row and the support line.
#define NV_DETW_RESUME_H    37.0f
#define NV_DETW_GAP_RESUME  22.0f   // actions -> resume (633 - 611)
#define NV_DETW_LD_META    35.0f
#define NV_DETW_LD_META2   31.0f
#define NV_DETW_META_SEP   24.0f   // gap do flex, dos dois lados do ponto

// ---------------------------------------------------------------------------
// The SEARCH screen — MEASURED in the running web app (the owner's profile, 1920x1080).
//
// The port had a 6x7 GRID KEYBOARD on the left and a grid of results on the right.
// The web app has no keyboard at all: it has a wide text field at the top (the TV's
// system opens its keyboard) and the results come in horizontal ROWS, one per addon
// catalogue, with a title and the origin below it.
//
//   .search-header        y=22  h=110, padding 0 104
//     .search-discover-btn 110x110 at (104,22)  bg #222, 1px #333 border, radius 22
//     .search-voice-btn    110x110 at (262,22)  -> step 158 (gap 48)
//     .search-input-field  1396x110 at (420,22) bg #222, radius 22, 34/500,
//                          padding 0 32; focused: border #f5f5f5 and
//                          box-shadow 0 0 0 2px rgba(245,245,245,.22)
//   .search-empty-state   y=148 h=400, centred: a 136 icon at y=220.5,
//                          the title 56/600 at y=378.5, support 24/400 at y=446.7
//   .search-results-row   title 48/600 lh 51.84; subtitle 20/400 rgb(179)
//                          with margin-top 4; the track 88.3 below the title
//     .search-result-card  248 wide, poster 248x372 radius 22 border 2px
//                          name 28/500 lh 33.6 (margin-top 8)
//                          date 20/400 rgb(179) (margin-top 4)
//                          horizontal step 280 (248 + 32)
//   step between rows 562.4
#define NV_SEARCH_HEAD_Y     22.0f
#define NV_SEARCH_HEAD_H    110.0f
#define NV_SEARCH_BTN       110.0f
#define NV_SEARCH_BTN_GAP    48.0f
#define NV_SEARCH_BTN_ICON  54.0f
#define NV_SEARCH_RADIUS       22.0f
#define NV_SEARCH_FIELD_PADX 32.0f
#define NV_SEARCH_EMPTY_Y   148.0f
#define NV_SEARCH_EMPTY_ICO 136.0f
#define NV_SEARCH_EMPTY_TITLE 378.5f
#define NV_SEARCH_EMPTY_SUB 446.7f
#define NV_SEARCH_ROW_SUB    55.8f   // topo do titulo -> topo do subtitulo
#define NV_SEARCH_ROW_RAIL 92.3f   // topo do titulo -> topo dos cards
#define NV_SEARCH_ROW_STEP 562.4f
#define NV_SEARCH_CARD_W    248.0f
#define NV_SEARCH_CARD_STEP 280.0f
#define NV_SEARCH_POSTER_H  372.0f
#define NV_SEARCH_NAME_GAP    8.0f
#define NV_SEARCH_DATE_GAP    4.0f

// ---------------------------------------------------------------------------
// The LIBRARY screen — MEASURED in the running web app.
//
// The port had three centred tabs ("My List", "Purchased", "Genres") and a 6-column
// grid of 212. The web app has: the title on the left with a source badge on the
// right, TWO mode pills ("Saved" / "Cloud") and TWO wide pickers ("Type" and
// "Sort"), and only then the grid.
//
//   .library-main       padding 48 96 64 -> content at x=96, y=48, width 1728
//   .library-page-title 56/600, letter-spacing 1px, at (96,48)
//   .library-page-source 28/500 rgb(128,128,128) ls 4px, right-aligned (1824)
//   .library-view-mode-row y=136 h=56, gap 16: 150x56 pills radius 999,
//                        14/24 of padding, 21/400; the chosen one bg #303030 with a
//                        2px #fff border; the others bg #222 border 2px #333
//   .library-picker-row  y=212 h=110: two 840x110 pickers at x=96 and x=984,
//                        radius 36, padding 18/28; focused bg #303030 border 1px
//                        #fff, the others bg #222 border 1px rgba(255,255,255,.1)
//     .library-picker-title 19/500 rgb(128,128,128) ls 0.45 lh 24
//     .library-picker-value 30/500 white ls 0.3 lh 40, margin-top 4
//   .library-empty-state y=354, padding-top 38, gap 18: title 46/500 lh 49.68,
//                        support 28/400 rgb(179,179,179) lh 35
//   .library-grid       6 columns of 268 (auto-fill over a minimum of 252 in 1728,
//                        with a 24 gutter), 2:3 poster = 268x402 radius 24 with a
//                        4px border ON THE INSIDE, title 32/500 lh 1.18 at 16 from
//                        the poster; row step 487.8 (455.8 + 32)
#define NV_LIB_X            96.0f
#define NV_LIB_Y            48.0f
#define NV_LIB_W          1728.0f
#define NV_LIB_DIR        1824.0f
#define NV_LIB_MODE_Y      136.0f
#define NV_LIB_MODE_W      150.0f
#define NV_LIB_MODE_H       56.0f
#define NV_LIB_MODE_STEP  182.0f
#define NV_LIB_PICK_Y      212.0f
#define NV_LIB_PICK_W      840.0f
#define NV_LIB_PICK_H      110.0f
#define NV_LIB_PICK_STEP  888.0f
#define NV_LIB_PICK_RADIUS    36.0f
#define NV_LIB_PICK_PADX    28.0f
#define NV_LIB_PICK_PADY    18.0f
#define NV_LIB_EMPTY_Y     354.0f
#define NV_LIB_GRID_Y     354.0f
#define NV_LIB_COLUMNS         6
#define NV_LIB_CARD_W      268.0f
#define NV_LIB_CARD_GAP     24.0f
#define NV_LIB_POSTER_H    402.0f
#define NV_LIB_POSTER_BORDER  4.0f
#define NV_LIB_TITLE_GAP      16.0f
#define NV_LIB_LINE_STEP 487.8f
// `.library-grid-card.focused { transform: scale(1.02) }` with the origin at the top
// — it is the ONLY focus scale left on any screen of this app, and it is the web
// app's: the others came from tvOS's Top Shelf tables and were removed.
#define NV_LIB_FOCUS_SCALE  0.02f

#define NV_DET_MARGIN_X  120.0f
#define NV_DET_MARGIN_Y   38.0f
#define NV_DET_PAD        44.0f   // measured: the text 44px from the card's edge
#define NV_DET_BUTTON_H    70.0f   // measured: a 254x70 pill, radius = h/2
#define NV_DET_BASE      163.0f   // measured: the button's end to the bottom of the screen
// The height of the title's logo. It matches the ink height measured against the
// reference (a cap of 83px), with slack for the descenders.
#define NV_LOGO_H        104.0f
#define NV_LOGO_MAX_W    620.0f
// In the page's header the logo appears smaller than on the card — there it is the
// screen's label, not the protagonist.
#define NV_LOGO_HEADER_H     62.0f
#define NV_LOGO_HEADER_MAX_W 420.0f
// The logo inside the home's highlight card, at the unfocused card's size. It grows
// along with the card, otherwise the title "peels away" from the art on focus.
#define NV_LOGO_CARD_H    54.0f
// How much the art LAGS inside the frame during the glide, as a fraction of the
// texture. 0 = the art stuck to the frame (it looks like a single panorama going
// past); 0.12 = the window runs over it and the art almost stays put — the effect
// of a fairground board where you put your face in and the picture changes.
#define NV_DET_PARALLAX  0.12f
// How much the art grows on becoming the stretched page's background, and how much
// it darkens. It is the two together that turn it from a photo into a field of colour.
#define NV_DET_ZOOM_BACKGROUND  1.35f
#define NV_DET_DARK_BACKGROUND 0.62f
// The mipmap level sampled for the page's background: the higher, the blurrier.
// Measured: on the page's background NO structure smaller than ~250px survives — it
// is practically a gradient of blotches. A bias of 5.5 preserved too much detail.
#define NV_BLUR_STEP       2.4f   // the gaussian's step, in texels of the target
// The textures' memory ceiling. The TV has a quota, and real art is large: a
// 1920x1080 backdrop takes 8 MB once decoded.
// 72 MB was the ceiling set after a "double free" — but that overflow came from the
// cache having NO ceiling at all (it passed 104 MB and kept growing), not from 96
// being too much. With the dynamic catalogue there are ~48 titles x 2 images, and at
// 72 the cache lived pressed against the limit (measured: 70.7 MB with 46 textures),
// evicting and re-fetching without pause — 12 to 15 janks per second while navigating.
#define NV_TEX_BUDGET_MB 96
// The sections of the title's page.
#define NV_TAB_W          236.0f   // measured
#define NV_TAB_H           63.0f   // measured; a capsule (radius = h/2)
#define NV_TAB_PITCH      277.0f   // measured: text to text
// Measured: a 410x228 thumbnail, the text BELOW it, 18px from the thumbnail's base
// to the "EPISODE n" label, and 143px from the end of the text to the next header.
#define NV_EP_H           512.0f   // thumbnail + label + title + 5 lines + date
#define NV_EP_THUMB_GAP    18.0f
#define NV_AVATAR         168.0f
// Tracking: on tvOS it is SLIGHTLY POSITIVE on small body sizes (+0.4px) and
// practically zero on large titles — the opposite of the reflex to tighten titles
// that comes from web design. The page's header is the exception: it is uppercase
// and tracked out on purpose, and you can see that in the device's photo.
#define NV_TRACKING_HEADER     9.0f   // measured against the device's capture
// Vertical spacings MEASURED on the expanded page.
// 64 and not 84: the measured value (84) is where the capitals' INK starts, and the
// text is drawn from the top of the line's box, some 20px above that.
#define NV_PG_TOP         64.0f
#define NV_PG_TITLE_TABS     82.0f   // the title's base to the top of the tabs
#define NV_PG_SEC_CARDS    22.0f   // the section header to the top of the cards
#define NV_PG_BETWEEN_SEC   143.0f   // the end of one section to the next one's header
#define NV_WHERE_W         420.0f
#define NV_WHERE_H         106.0f
#define NV_ABOUT_H        150.0f
#define NV_DET_GAP        35.0f   // measured: the gutter between the card and its neighbour
// How far the card overshoots its final size before settling. Without that
// overshoot the opening looks like it "appeared larger"; with it, it looks like it
// came forward.
#define NV_DET_OVERFLOW   0.035f

#endif
