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
// The bar's OPEN width. It lives here and not in menu.c because main.c sizes the
// backdrop's grab from it: the strip has to be as wide as the menu ever gets.
// It stays at 392 and not at .home-sidebar's 340: the web app has no "Profile and
// Stats" row, and that label does not fit 340 without being cut.
#define NV_MENU_W_IS_OPEN       392.0f
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

// THE SAME HERO, NOT STRETCHED OVER THE SCREEN (`heroBackdropArea` = "Top band").
//
// A full-screen backdrop is 1920x1080 and the rows start at NV_SHELF_TOP with
// their first card at 615, so the bottom of the picture is paid for and never
// seen — and worse, GFX_HERO_FULL's horizontal ramp darkens the left 1248px of
// it to make ground for the copy. Between the two, most of the art is gone.
//
// The band draws the SAME image smaller, at its own aspect, hung from the
// TOP-RIGHT corner, with the background showing below and to the left: the whole
// picture lands in the part of the screen the rows leave alone, and the ramp that
// makes the copy's ground is proportionally narrower, so much more of the art
// survives it.
//
// THE SIZE IS A PREFERENCE, not a constant: `heroBackdropScale`, a percentage of
// the SCREEN'S WIDTH, with the height following the art's own aspect. It is a
// setting and not a number here because it is tuned by eye from the sofa, and
// every value tried otherwise costs a full ARM build and a deploy.
//
// 80 is the factory value: 1536x864 for a 16:9 backdrop, which reads as very
// nearly full width — the 384px left of it are where the copy sits, and the ramp
// would have darkened them anyway.
#define NV_HERO_FIT_PCT_DEFAULT  80

// WHERE THE RAMPS ARE, and they are given in SCREEN coordinates, converted to the
// band's own 0..1 at the draw (heroFitPar in home.c) and handed to the shader in
// uPar. The band's width is a preference, and a stop written as a fraction of it
// would slide across the copy as the size moved: at 50% it would clear to the left
// of the text, at 100% far to the right of it. What has to hold still is the screen
// x the ramp clears at and the screen y the art starts going at.
//
// BOTH NUMBERS ARE GFX_HERO_FULL'S OWN, read off the stops recorded in gfx.c:
// its horizontal ramp covers the left 65% of 1920 (= 1248) and its vertical one
// opens at 64% of 1080 (= 691). They are measurements of the web app making ground
// for THIS copy — the same typeface at the same size in the same place — so there
// is nothing better to choose, and it means the band at 100% is the full-screen
// hero exactly, which is what makes the preference read as one continuous size.
#define NV_HERO_FIT_CLEAR_X  1248.0f
#define NV_HERO_FIT_FADE_Y    691.0f
// The bounds those two are held to, in 0..1 of the band. The minimum edge exists
// for the SMALL sizes, where the band starts to the right of where the ramp would
// clear: it is then not making ground for anything, but the left edge still has to
// dissolve instead of being cut.
#define NV_HERO_FIT_EDGE_MIN   0.18f
#define NV_HERO_FIT_EDGE_MAX   0.70f
#define NV_HERO_FIT_FADE_MIN   0.40f
#define NV_HERO_FIT_FADE_MAX   0.92f

// The aspect the band is built at before the art has decoded, and the range a
// decoded one is held to. tex_aspect answers 0 until the image lands, and a band
// that changed shape when it did would read as the hero resizing itself; the
// clamp keeps a stray square or panoramic file from making a band that is taller
// than the screen or thinner than the copy beside it.
#define NV_HERO_FIT_ASP      (16.0f / 9.0f)
#define NV_HERO_FIT_ASP_MIN  1.20f
#define NV_HERO_FIT_ASP_MAX  2.40f

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
// The hero's text block. `.home-modern-hero-copy` is a flex column with
// justify-content:flex-end — ANCHORED TO ITS BASE — and everything below was
// MEASURED in the running app at 1920x1080 (2026-09-07), which corrected three
// numbers this block had been carrying from the stylesheet alone.
//
// THE BASE IS 470.4, NOT 478.4. It does not come from
// `--modern-hero-copy-bottom-gap` (40) at all: the copy lives inside
// `.home-hero-card`, a box of 518.4 (48% of the height) with `padding: 48px 64px`,
// so the block's base is 518.4 - 48. The 40 in the variable is not what lands on
// screen — the 48 of padding is. Measured: copy box y=48, h=422.4, bottom 470.4.
//
// THE GAP BETWEEN LINES IS 12, NOT 16. `gap: 12px` on the column, confirmed by the
// children's rects (brand ends 271.5, meta starts 283.5).
//
// AND THE SYNOPSIS HAS 4 MORE. `.home-hero-description` is a <p> with
// `margin-top: 4px` (components.css:7255) on top of the column's gap, which is why
// the one space in the block that measures 16 is the one above the synopsis.
// AND IT IS MEASURED FROM THE FIRST ROW'S TITLE, NOT FROM THE VIEWPORT — a
// DELIBERATE DIVERGENCE, asked for twice by the owner with the two apps side by side.
//
// In the web these are two independent anchors: the copy hangs 48 under the hero
// card's own edge (base 470.4) and the rows rest 46 under the viewport's (title
// 564.4), which leaves 94 of nothing between the synopsis and the title. Ported
// literally it reads as a block floating away from the row beneath it — "it still
// looks way too high, should be connecting more to the element under it". Hanging the
// copy off the title keeps the 48 the port has always drawn and moves the whole
// composition down together, which is what "move everything down" meant.
//
// The number itself is still the web's: 48 is the hero card's bottom padding. What
// changed is what it is subtracted FROM (home.c, `base`).
#define NV_HERO_COPY_GAP        48.0f   // .home-hero-card's bottom padding
#define NV_HERO_COPY_LINE      12.0f   // the flex column's gap
#define NV_HERO_SIN_MARGIN      4.0f   // .home-hero-description's margin-top

// THE GAP UNDER THE TITLE'S LOGO, which is the one space in the block that is NOT
// the flex column's 12. A DELIBERATE DIVERGENCE, asked for by the owner looking at
// the homescreen: "visually looks a bit too close sometimes".
//
// It only became a divergence worth making once the logo stopped reserving a fixed
// box (home.c). Before that the 12 was reached by a mark square enough to fill
// NV_LOGO_HERO_H and by nothing else — every wider wordmark inherited the box's
// leftover on top of it, up to ~139. Fixing that hands EVERY logo the 12 that used
// to be the tight end of the range, so the constant has to come up with it or the
// fix reads as the whole hero tightening.
//
// 24 is not measured, unlike the rest of this block: the web's own number is 12, and
// the web does not need more because its logos are the art's own box with no
// padding of their own to fall inside. Tune against the panel, not against the CSS.
#define NV_HERO_LOGO_GAP       24.0f   // logo -> meta line; web parity would be 12

// THE HEIGHT OF THE COPY BOX, which is what decides how many lines of synopsis are
// drawn. 518.4 of card minus its 48 of padding top and bottom = 422.4, and the box
// is fixed: the flex column does not grow with the text, the text is fitted to it.
// See the line count computed in home.c, which is the port of
// applyModernHeroDescriptionBounds (homeScreen.js:6531).
#define NV_HERO_COPY_H        422.4f

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
// 760, MEASURED on the element (`.home-hero-description` comes back 760 wide) and
// declared as `max-width: min(100%, 760px)` in the modern rule (components.css:7255).
//
// The 640 that was here came from the `.legacy-webos` block, which narrows this to
// 560 and was read as if it gave 640. It applies to neither of the two things it was
// chosen for: that block needs webOS <= 6 (js/app.js:186) and the C3 is webOS 23, so
// what the owner's TV renders is the 760 above. The narrower column is also what made
// the synopsis need FOUR lines where the web takes three — at 760 the same text fits
// in three without ending mid-sentence, which is the whole reason the clamp had been
// raised here.
#define NV_HERO_SIN_W    760.0f

// THE EDITORIAL HERO FOR COLLECTIONS. A collection's art is already a finished 16:9
// composition (gradient, light and breathing room); the text must not compete with
// a second large cover on the right. The positions follow the Awards screen's
// reference: the group at the top, the list's real logo centre-left and the action
// coming to rest just before the row's header.
// ONE LINE FOR EVERY COLLECTION HERO — the top of the title's INK, whether that ink
// is a wordmark's art or the capital of a name. Streaming sat 100px above Discover
// and Genres before it: a mark was hung from this line while a name still hung from
// the baseline under it, so walking DOWN the home moved the whole block up and back
// again. Now only the air UNDER the title changes.
//
// 326 is where the two ends meet, and it is pinned from BELOW: MAX_H is 172, so the
// tallest mark reaches 498 and the rows' viewport opens at NV_SHELF_TOP (518). The
// two numbers move together — every time the marks are asked to grow, this line comes
// up by the same amount. It should not rise much further: a name pinned near the
// box's original 282 left a quarter of the screen empty before the first row.
#define NV_COLLECTION_HERO_LOGO_Y       326.0f
// THE GROUP LABEL SITS A FIXED GAP ABOVE THE TOP OF THE LOGO BOX (LOGO_Y), so it is
// on the same line for every folder in the row.
//
// It has been in all three places: at an absolute 182, where it came apart from a
// short wordmark by up to 180px and stopped reading as one block; then measured from
// the top of the MARK, which held the pair together but gave the label a different
// height for every folder — a wide wordmark `contain`-ed to 119 tall against a mark
// filling the box — so it jumped as the focus travelled the row, and again on the
// frame each mark decoded. Anchoring it to the BOX keeps it still, and the mark is
// now hung from that same top edge (see NV_COLLECTION_HERO_LOGO_BASE), so the gap
// below it is the same on every card as well as on every frame.
//
// 16 is what the EYE gets: the label's BASELINE to the top of the title's ink — the
// capital's own top, or the wordmark's. It is not a gap between line boxes. Measured
// that way the first attempt at this came out at 38, because a TxtLine is the height
// of the FACE and hides some 22px of air above a 76px capital plus 5 below a 21px
// baseline (see txt_cap_inset in text.h) — which is why the pair still read as
// detached after the boxes had been put 14 apart.
#define NV_COLLECTION_HERO_GROUP_GAP     32.0f
// THE BOX IS THE WEB APP'S: `.home-layout-modern .home-hero-logo` is 440x200 with
// `object-fit: contain` and `object-position: bottom center` (components.css:7122, the
// --modern-hero-logo-max-* pair). That base rule is the one THIS TV runs under. The two
// overrides of it do not apply here and both were read as though they did: 440x120 at
// :19436 is `.legacy-webos`, which app.js:187 sets only for webOS 6 AND BELOW — a C3 is
// webOS 8 — and the 440x200 at :19862 sits inside `@supports not (font-size: clamp())`,
// a fallback for browsers this one is not.
//
// THE HEIGHT IS NOT A FLAT CEILING, and that is the third try at this. Under a plain
// `contain` the binding dimension decides, and the two obvious boxes are the two ends
// of the same pendulum — measured on the nine services actually installed here:
//
//   440x200 (aspect 2.2)   HBO 276x200, Netflix 440x119 — equal INK (55k vs 52k), but
//                          the drawn heights run 71..200, a 2.8x spread the eye reads
//                          as "some logos are twice the size of others".
//   520x150 (aspect 3.47)  equal HEIGHTS, but HBO falls to 207x150 against Netflix's
//                          520x140 — 31k against 73k of ink, which is the version that
//                          was complained about before.
//
// So neither dimension is held constant: the target height FALLS WITH THE ASPECT, but
// only by its fourth root, which lands halfway between "equal height" (no fall) and
// "equal area" (a square-root fall). H_REF is that curve's height at aspect 1 —
// h = H_REF / aspect^0.25 — capped by MAX_H for a near-square mark and by MAX_W for a
// long one. Measured over the nine installed here: 172, 165, 160, 157, 141, 139, 139,
// 134 and (the one outlier, at aspect 6.2) 84. Heights within 1.3x where the flat box
// put them within 2.8x.
//
// MAX_H is also what keeps the band clear of the rows: LOGO_Y + MAX_H is 498, and the
// rows' viewport opens at 518. The two numbers move together.
//
// MAX_W is 520 and no longer the web's 440. The 440 was kept on the grounds that it
// held the wordmark clear of the hero art, which starts at x=555 with the banded
// backdrop — but the hero's own synopsis is NV_HERO_SIN_W (760) wide from the same
// x=104 in that very mode, so it runs to 864 and crosses the art already. What 440
// actually did was cap the five WIDEST marks below the curve's height while the
// compact ones grew freely, which is the spread that kept coming back: Netflix was
// held at 118 tall while HBO reached 157. At 520 only Crunchyroll (aspect 6.2) is
// still bound by width.
//
#define NV_COLLECTION_HERO_LOGO_MAX_W   520.0f
#define NV_COLLECTION_HERO_LOGO_H_REF   186.0f
#define NV_COLLECTION_HERO_LOGO_MAX_H   172.0f
// The band's BOTTOM — the floor the tallest mark reaches, and nothing is drawn from
// it any more. It WAS the anchor: the web bottom-aligns inside the logo box, and the
// name standing in for a missing wordmark hung from it too. The cost only became
// visible once the group label was tightened onto the title. The label has to sit a
// fixed distance above the title's ink; a mark's height varies with its aspect and a
// name's with the face; so anchoring the bottom moved the label on every card of the
// row AND put a name 80px below a wordmark. Anchoring the TOP (LOGO_Y) fixes every
// line at once, and what varies instead is the air under the title — where there is
// nothing but the rows.
#define NV_COLLECTION_HERO_LOGO_BASE \
  (NV_COLLECTION_HERO_LOGO_Y + NV_COLLECTION_HERO_LOGO_MAX_H)
// There is no caption token: the hero's "N lists · OK to explore" line was removed.

// THE COLLECTION TILE'S FOCUS ANIMATION. Two shapes feed the same loop in
// home.c's drawShortcuts:
//
//  - PACKAGED collections carry a folder of numbered JPEGs (frameDir/%03d.jpg),
//    written into the .ipk by tools/import-collections.mjs;
//  - ACCOUNT collections carry one sprite sheet over the network
//    (ColFolder.focusSheet), written by nuvio-assets/scripts/make-focus-sheet.sh.
//
// The sheet's grid is FIXED and needs no metadata from the account, because the
// script loops short clips and truncates long ones to fill every cell — the
// idents run from 2.5 s to 16 s, so a fixed frame rate over each clip's own
// length would have made the count vary per service.
//
// 8x8 at 1920 wide, because tex_get_width clamps every decode to
// NV_TEX_HERO_WIDTH_MAX: a wider sheet would come back downscaled, and since the
// whole sheet shares one decode budget, every cell would pay for it in
// sharpness. The cell coordinates are normalised and would still be correct.
#define NV_FOCUS_SHEET_COLS  8
#define NV_FOCUS_SHEET_ROWS  8
// 15 fps, the rate the packaged path already chose for this same animation.
#define NV_FOCUS_FRAME_MS    67
// How long the tile has to KEEP the focus before it starts moving. It stops the
// animation firing on every tile you merely pass through on the way to another.
#define NV_FOCUS_DELAY_MS    350
// CSS line-heights: description 24*1.45, meta 21*1.25, secondary 18*1.35.
#define NV_LD_HERO_SIN   35
#define NV_LD_HERO_META  26
#define NV_LD_HERO_SEC   24
// The IMDb chip at the end of the hero's meta line. NV_HERO_IMDB_W is the width the
// mark is DRAWN at — the web app's 40px badge — with its height following the file's
// own aspect. NV_HERO_IMDB_GAP is the space between the "•" and that mark: the line's
// own separator is three spaces of Inter (~16), which left a box-shaped token floating
// away from the text, so it is tighter here on purpose.
#define NV_HERO_IMDB_W   40.0f
#define NV_HERO_IMDB_GAP 12.0f
// THE ONE WIDTH THE IMDb MARK IS EVER DECODED AT, for every screen that draws it.
//
// tex_get_exact re-decodes whenever the requested width MOVES — in either direction,
// unlike the promotion tex_get_width relies on — so two screens asking for the same
// file at two sizes re-decode it on every frame they are both alive. That is the trap
// detail.c's hero logo already documents, and it is a real one here: the home hero and
// the title screen both draw for the length of a transition, and the title screen draws
// the mark TWICE itself (the meta line and every episode card). So all three call sites
// ask for this one width and scale the draw to their own box.
//
// 60 is the largest of the three (the title screen's meta badge), so no call site is
// ever magnified past 1:1.
#define NV_IMDB_MARK_TEX_W 60.0f
// The file's own aspect (art/icons/imdb_logo.png, 256x130). tex_aspect answers 0 until
// the decode lands, and this stands in for that one frame.
#define NV_IMDB_MARK_AR    1.969f
// --- THE HERO'S META LINE, RE-MEASURED 2026-09-15 in NuvioWeb -----------------
//
// The line read as clutter and the cause was not the number of words: it was that the
// SEPARATORS were as bright as them. Measured on `.home-modern-hero-meta-line`, the
// web draws the tokens at rgba(255,255,255,.62) and every "•" at
// rgba(255,255,255,.34) — a little over half. The eye then groups the words and the
// dots recede to punctuation. The port baked the bullets into ONE string drawn at a
// flat rgb(179,179,179), so every dot competed with the text around it.
//
// It also carries ONE genre, not a list: the measured line is "Movie • Action • 1h 51m
// • September 4, 2026 • IMDb 6.9" — five tokens where the port had six or seven.
#define NV_HERO_META_INK   0.62f   // the tokens
#define NV_HERO_META_DOT   0.34f   // the separators, and this is the whole trick
#define NV_HERO_META_SEP  12.0f    // the space each side of a dot (282 -> 305.2)
// The line is TWO groups with 14 between them: what the title IS (provider, type,
// genre) and then its NUMBERS (year, runtime, score). The gap is a second, quieter
// readability device — it splits the line without adding a mark.
#define NV_HERO_META_GROUP 14.0f
#define NV_HERO_META_MAXTOK  10
// MEASURED in the web app (getBoundingClientRect at 1920x1080, modern layout,
// 2026-09-07): `.home-modern-rows-viewport` is a block pinned to the bottom of the
// screen, 52% of its height — y=518.4, h=561.6 — and the rows scroll INSIDE it.
// The "2/3 of the screen" rule that used to be here is from tvOS's productTemplate,
// and is not ours: in the web app the rows rise over the bottom part of the hero's
// art (which runs to 670), instead of starting after it.
#define NV_SHELF_TOP     518.0f   // the top of the ROWS VIEWPORT

// AND THE FIRST ROW DOES NOT BEGIN THERE. `.home-modern-rows-scroll` — the flex
// column that lives inside that viewport — opens with
// `padding-top: var(--modern-rows-top-feather)` = 46px (components.css:7346), so the
// first row's header rests at 564.4 and its cards at 614.
//
// The note that used to sit here read "the first row's title sits at y=518 and the
// cards at y=564": it had taken the VIEWPORT's top for the title's, and the 564 it
// quoted for the cards is in fact where the TITLE sits. The whole shelf was drawn
// 46px too high because of it, and what gave it away was the BOTTOM of the screen —
// 65px of the second row's cards showing where the web shows 20.
//
// The padding is not decoration in the web either: the viewport carries a mask that
// fades its own top 46px (components.css:7295), and this is what keeps the first
// row's title clear of it — "opaque by 46px, which is where rows come to rest", in
// the sheet's own comment.
//
// MEASURED as a CONSTANT offset and not only at rest: with the focus moved two rows
// down, so that the scroll is 782px in, the focused row's header still lands at
// 564.6. It is where a row comes to rest, not where the first one happens to start,
// which is why it is added to the shelf's origin rather than to the first row alone.
#define NV_SHELF_PAD_TOP  46.0f
// The header block down to the first card: the title's LINE BOX plus the 16px the
// web puts between the two. Those 16 are `.home-track`'s `padding: 16px 52px 16px
// 104px` (components.css:7415) — the row's cards start one padding below the
// `.home-row-head` that carries the title, and nothing else sits between them.
//
// 51 and not the web's 49.6, and the 1.4 is not slop. SDL_ttf's line box is
// ascent + descent = ceil(1984*28/2048) + floor(494*28/2048) = 35, where the CSS
// box is line-height 1.2 = 33.6 with a NEGATIVE half-leading of -0.7. The glyphs
// are the same in both, so ours land ~1.6px lower inside a box that starts at the
// same y; adding that back is what makes the WHITE SPACE the eye actually judges —
// from the descender down to the card — come out at the web's 16.
#define NV_ROW_TITLE_GAP     16.0f
#define NV_LEGACY_ROW_HEAD_H 51.0f // 35 of line box + NV_ROW_TITLE_GAP

// The four visual sections the Apple TV home uses, each with its own proportion —
// OBSERVED in the reference photos:
//  1. HERO      full-bleed 16:9 art that CHANGES by itself (carousel + dots)
//  2. CARD      an ordinary 16:9 landscape, the default row
//  3. POSTER    2:3 portrait, used in the Top 10 beside the numeral
//  4. HIGHLIGHT a large card with a metadata block below ("Watch next")
// MEASURED LIVE in the web app (getBoundingClientRect on `.home-poster-card`, the
// modern layout signed in, 2026-09-07): the card's border box is 229 x 347 and the
// `.home-poster-frame` inside it 229 x 343. The art that survives both borders is
// therefore 221 x 339, which is exactly NV_CARD_W/H minus NV_CARD_PAD on each side.
//
// 212 x 322 — what this was — came from `--home-poster-width: 212px` in the modern
// block (components.css:6662) and from the note in settings.h saying the inline
// variable loses to it. THE NOTE IS WRONG, and the element proves it: the shell
// carries `--home-poster-width:229px;--home-poster-height:343px` in its style
// attribute, written by `buildModernHomeSizingStyle` out of `posterCardWidthDp`
// (126 dp: round(126 * 0.84 * 1.08 * 2) = 229, and x1.5 for the height), and an
// inline custom property beats a stylesheet's. The 347 is the 343 frame plus the
// card's own 2px border top and bottom.
#define NV_CARD_W        229.0f
#define NV_CARD_H        347.0f

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
// From the bottom of a row's cards to the top of the next row's title. It is TWO
// numbers in the web and not one: `.home-track` closes with `padding-bottom: 16px`
// (components.css:7415) and `.home-modern-rows-scroll` — the flex column the rows
// sit in — adds `gap: 32px` (7346). 16 + 32 = 48, which is also what the step
// measured end to end: rows at y=740.2 and y=1184.8, a card 347 tall under a
// 33.6 title with 16 above it.
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
// 419 x 237 of frame, from the SAME inline block as the portrait above
// (`--home-landscape-poster-width/height`, round(126 * 1.24 * 1.34 * 2) and that
// over 1.77), plus the card's 2px border top and bottom. The 318 x 182.9 here
// before was the stylesheet's `calc(212px * 1.5)`, which the inline overrides.
#define NV_CARD_LAND_W   419.0f
#define NV_CARD_LAND_H   241.0f   // a 237 frame + 2px of border top and bottom
#define NV_CARD_LAND_ART 237.0f
// 24 of flex gap, like NV_ROW_GAP, plus the same 16 of track padding.
#define NV_ROW_GAP_LAND 40.0f
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
// THE REMAINING TIME, WITH NO CONTAINER. There is no pill, no chip and no plate:
// the corner of the artwork is shaded and the type sits on it. The card had a
// container here through three attempts — the port's flat rectangle, the web's
// `.home-continue-badge` glass, a frosted plate — and the objection each time was
// the same one, that a box in the corner reads as a control. The time left is a
// label, so it is set like one.
//
// The scrim reaches this far across the card from the TOP-RIGHT corner, as a
// fraction of each side, and goes on at this depth in the corner itself. Both are
// bigger than they look written down: the ramp is a dome (GFX_CORNER_SCRIM), so
// the depth quoted here is reached only at the very corner and half of it is gone
// by the middle of the label.
#define NV_CW_TIME_W        0.58f
#define NV_CW_TIME_H        0.58f
#define NV_CW_TIME_INK      0.78f
// The label's own inset from the top and the right edge. It is the badge's, not
// the copy's — see NV_CW_COPY_X below.
#define NV_CW_PAD          18.0f
// The type sits high in its rasterised box (SDL_ttf gives every line the font's
// full height, ascenders and descenders included), so hanging the box off the
// inset puts the LETTERS further down than the number says. This is that
// difference, measured on the capture rather than derived from the metrics.
#define NV_CW_TIME_RISE     5.0f

// THE COPY AND THE BAR, read off `.home-screen-shell.home-layout-modern
// .home-continue-*` (components.css:7709-7754) with the base block at 5892-6103.
// Everything here is the MODERN layout's number: the base block is the classic
// one and is half the size — 11/18/13 against 17/30/21 — and reading it instead
// is what left the port's card with type a third too small.
//
// The port used to put the copy at 18 from the side and 30 from the base, with
// one 23px style doing for both the episode code and the episode name and no
// tracking on either, so the three lines read as one grey block. The web gives
// them three different jobs: a tracked-out uppercase LABEL, a title with real
// weight, and a supporting line, the outer two dimmed to 62%.
#define NV_CW_COPY_X       24.0f   // .home-continue-copy left / right
#define NV_CW_COPY_BOTTOM  22.0f   // .home-continue-copy bottom
// The LINE BOXES, not the glyph heights: the block is bottom-anchored and CSS
// stacks it by line box, so the port lays out the boxes and centres each
// rasterised line inside its own. Doing it by glyph height instead makes the
// spacing depend on the font's ascent, which is not what moves in the browser.
#define NV_CW_KICKER_LH    22.1f   // 17 * 1.3
#define NV_CW_TITLE_LH     37.2f   // 30 * 1.24
#define NV_CW_SUB_LH       27.3f   // 21 * 1.3
#define NV_CW_TITLE_GAP     4.0f   // .home-continue-title    margin-top
#define NV_CW_SUB_GAP       3.0f   // .home-continue-subtitle margin-top
#define NV_CW_KICKER_TRACK  2.38f  // letter-spacing 0.14em at 17px
// The kicker and the subtitle are rgba(255,255,255,0.62); the title is #FFF.
#define NV_CW_DIM           0.62f
// `-webkit-line-clamp: 2`. One line ate real titles on a card this narrow, and
// the block grows UPWARD into the scrim, so the second line costs nothing below.
#define NV_CW_TITLE_LINES      2
// Full-bleed against the base, 6px tall, with a visible track behind the fill.
// It was a 4px line inset by the copy's margin and floating 10px up, with no
// track at all.
//
// The bar is drawn ONLY on a title that has been started — see resume.c, which
// diverges from the web here on purpose. NV_CW_BAR_MINW is therefore the floor
// for a REAL position, so a title two minutes in still shows something; it is
// not the web's "always paint a 12px stub" state.
#define NV_CW_BAR_H         6.0f
#define NV_CW_BAR_MINW     12.0f   // .home-continue-progress span { min-width }
// Below this the title counts as NOT STARTED and gets no bar at all. The same 2
// the detail screen's episode card has always used, where under it the dashed
// "not started" ring is drawn instead. See the long note in resume.c.
#define NV_CW_BAR_MIN_PCT     2
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
// HOW LONG A CARD HAS TO KEEP THE FOCUS BEFORE ITS DETAIL PAGE IS FETCHED AHEAD.
//
// The detail screen's meta lines are filled by two requests (/meta and the extras
// sheet) that used to start on OK, so the page opened and then rearranged itself as
// they answered. Asked for here, they have usually answered by the time OK is pressed.
// Longer than the hero's rest: both fetchers run one request at a time, and a walk
// along a row should not queue a request per poster it passes.
#define NV_HOME_PREFETCH_MS 400
// HOW LONG THE COPY WAITS FOR THE TITLE'S LOGO before coming in without it.
//
// The logo is a CDN file; the description is already in the catalogue and costs
// nothing. Holding one hostage to the other left the hero's text blank for the
// whole download, which is worse than the logo arriving a moment later on a fade
// of its own. Below this the two come in together, which is the common case with
// the art cached.
#define NV_HERO_LOGO_WAIT_MS  400
// HOW LONG THE HERO HOLDS THE PICTURE OF THE ROW KIND JUST LEFT before crossing to
// the new one without waiting for its art.
//
// Going from a poster row to a collection row (or to the social row) exchanges one
// whole hero drawing for another, and the arriving one's art may be a CDN cover that
// lands seconds later. Holding the standing picture until then is right — it is the
// same gate the swap along a row already applies — but only up to a point: past it
// the viewer has moved and the hero is still showing the row they left, which reads
// as the screen having stopped responding. Same trade-off as the logo's wait above,
// and the same number: below it the change is a single clean dissolve, and art that
// never lands costs 400 ms, not the session.
#define NV_HERO_FAMILY_WAIT_MS  400
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
// `.home-row-title` under the MODERN layout: 28px / 1.2, weight 600
// (components.css:7406). Note it is the modern rule that wins and not the 24px of
// `.home-screen-shell .home-row-title` at 5519, nor the 30px of the bare
// `.home-row-title` at 5086 — two classes plus the element beat both.
//
// It was 33 for a while, scaled up by the 1.27 the SAME string measured wider on
// the TCL ("Top 100 Today - Film", ink 329 px there against 258 here). The owner
// has since asked for the WEB's size, seen side by side with it: the TCL is no
// longer the reference for this line, nuvioweb is.
#define NV_FT_ROW_TITLE 28
// .home-modern-hero-secondary: 18/600 in the signed-in session.
#define NV_FT_HERO_SEC   18   // --modern-hero-secondary-size (212*0.085)
#define NV_FT_HERO_META  21   // --modern-hero-meta-size (212*0.1), weight 500
// .home-hero-description at the MODERN rule's 24/1.45, and NOT the 22/30 that
// `.legacy-webos` hands the C3 (components.css:19453). A deliberate divergence: the
// stylesheet only drops to 22 on webOS <= 6, so every other client the project ships —
// the Mac build included — shows the synopsis at 24, and beside one of those the 22
// read as a smaller font rather than as the same design.
#define NV_FT_HERO_SIN   24   // --modern-hero-description-size, weight 400
// The DETAIL screen, measured in the running web app (getBoundingClientRect and
// getComputedStyle over .series-detail-shell), not read from the stylesheet.
#define NV_FT_DET_BUTTON  25   // .series-primary-btn (weight 600)
#define NV_FT_DET_META   25   // .series-detail-support and .detail-meta-row
#define NV_FT_DET_SIN    26   // .series-detail-description
#define NV_FT_DET_META2  23   // .detail-meta-row.secondary
// The ACTIONS ROW, re-measured in NuvioWeb 0.3.8 on 2026-09-15 (see the
// NV_DETWEB_* block in detail.h). The label is 32, not the 25 NV_FT_DET_BUTTON
// carries: that 25 came from an older measurement of the same button and is still
// right for profile.c, which is the only other caller — so the detail screen gets
// a size of its own rather than dragging an unrelated screen along with it.
#define NV_FT_DETWEB_BTN 32   // .series-primary-btn (weight 600)
// The tooltip over a focused circular button: 24 at weight 700 exactly, which is
// the one place on this screen where Bold is a MEASUREMENT and not the optical
// choice text.c makes for 600.
#define NV_FT_DETWEB_TIP 24   // .series-circle-btn::after (weight 700)
// The SEASON PICKER, measured in NuvioWeb on 2026-09-15, and the EPISODE LIST (see
// the NV_DETEP_* block in detail.h). The list's sizes started from the owner's
// reference and were raised a quarter on the owner's word that at the reference's
// 17-20px the rows were hard to read from the sofa.
#define NV_FT_DETWEB_SEA  30   // .library-picker-value (600) and its " · N Eps" (400)
#define NV_FT_DETWEB_OPT  28   // .library-picker-option (500)
#define NV_FT_DETWEB_EPB  21   // the row's "EP 3" kicker (600, tracked out)
#define NV_FT_DETWEB_EPM  23   // the row's meta line: duration, date, score (400)
#define NV_FT_DETWEB_EPT  34   // the row's title (600)
#define NV_FT_DETWEB_EPD  25   // the row's synopsis (400), leading NV_DETEP_DESC_LD
#define NV_FT_PLR_TITLE 56   // .player-title
#define NV_FT_PLR_BODY  32   // .player-subtitle and .player-time-label
// The player's top corner, RE-MEASURED 2026-09-17 against the transport block at
// the end of components.css, which supersedes the ATV block these came from. The
// corner was sized 26/20 there and is 36/24 now — the sheet's own note says the
// clock "keeps the larger size it gained ... the size difference is what separates
// the two lines now that the hairline divider is gone". At 26/20 the two lines were
// close enough in weight to read as one smudge from the sofa.
//
// The parental guide is not redone in that block and keeps the base rule's 22.
// 32, not the sheet's 36. The web app's value is sized for a browser window you
// sit close to; on a 65" panel at sofa distance the owner read 36 as shouting
// ("the clock font is a bit too big"). 32 keeps the size STEP over the 24 below it,
// which is what the sheet says the pairing depends on now that the divider is gone,
// without the corner competing with the title.
#define NV_FT_PG_CLOCK 32   // .player-clock       36 / 600 in the sheet
#define NV_FT_PLR_ENDS  24   // .player-ends-at     24 / 500, white 55%
// .player-controls-overlay .player-meta-tertiary — the third line of the title
// block, which is where the stream's own facts belong.
#define NV_FT_PLR_META3 22
// The episode line under the show's name: .player-subtitle at the legacy-webos
// block's 30, set in TWO weights — the "S1 E3" code Bold and dimmed, the name
// Medium and near white — so the eye lands on the name and the code reads as its
// label, the same split the time readout uses.
#define NV_FT_PLR_EP    30
// .player-controls-row .player-time-label — both halves are the same body; only
// the weight separates them.
#define NV_FT_PLR_TIME  30
// .player-controls-row .player-seek-delta — the signed jump, in its own pill.
#define NV_FT_PLR_DELTA 24
// The quick-seek readout at the screen's side ("+ 10 >") with the controls down.
#define NV_FT_PLR_QUICK 44
// The stats panel. The label is the quieter half of the pair, and the quality
// badge is 0.75em of the value in the sheet (.player-stats-quality).
#define NV_FT_PLR_STAT   22
#define NV_FT_PLR_STATL  20
#define NV_FT_PLR_BADGE  16
#define NV_FT_PG_END     20   // shared: episode lists, sources, tracks
#define NV_FT_PG_LABEL  22   // shared: episode list, tracks panel
// The parental guide, brought up onto the same scale as the rest of the transport.
// The sheet puts all three of its parts at 22, and at 22 the guide was the smallest
// type in a player whose every other line had grown — the clock to 32, "Ends at"
// and the seek pill to 24, the stream facts to 22. It read as a leftover from
// another screen rather than the opening warning it is.
//
// The CATEGORY cannot borrow NV_FT_PG_LABEL to get there: that one is shared with
// the episode list and the tracks panel, and resizing it for this corner would
// resize two other screens. Same trap as NV_FT_PG_END, one line up.
#define NV_FT_PLR_PG_CAT 24  // .player-parental-label     24 / 600
#define NV_FT_PG_SEV    24   // .player-parental-severity  24 / 400, player-only
// --- THE TRACK MENUS (audio and subtitles) -----------------------------------
// The web app's right-hand player menus. Every size below is that stylesheet's
// own `min(X vw, Y px)` RESOLVED AT 1920, which is the width this app draws at
// and the point where both halves of the expression meet — so these are the
// sheet's numbers, not an approximation of them.
//
// None of them can borrow the styles the sources sheet and the episode list use
// (NV_FT_PG_END, NV_FT_PG_LABEL): those are shared three ways, and resizing one
// for this panel resizes two other screens. That trap is recorded twice already,
// in the two blocks immediately above; this is the third time it applies.
#define NV_FT_TRK_TITLE  46  // .player-dialog-title        min(2.4vw,46)  / 800
#define NV_FT_TRK_LABEL  24  // .player-select-label        min(1.25vw,24) / 600
#define NV_FT_TRK_VALUE  26  // .player-select-value        min(1.35vw,26) / 700
#define NV_FT_TRK_OPT    24  // .player-select-option-main  min(1.25vw,24) / 600
#define NV_FT_TRK_OPTSUB 20  // .player-select-option-sub   min(1.04vw,20) / 500
#define NV_FT_TRK_STEP   28  // .player-dialog-step, the +/- glyph  min(1.46vw,28)/700
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
// 4 IS THE WEB'S OWN BAND, not a thickening of it, and the arithmetic only closed
// once the card was measured properly (see NV_CARD_W): the focused frame lights its
// 2px border AND a `0 0 0 2px` shadow around it, so what shows is the whole 4px
// gutter between the card's box (229 x 347) and the art (221 x 339) — the same
// gutter NV_CARD_PAD opens here. The card keeps its footprint either way: the band
// grows inwards from the card outline, never past it.
//
// THE COLOUR IS #F5F5F5 SOLID, not white at 0.8. The 0.8 came from
// `.home-poster-card.focused .home-poster-frame` at components.css:5619, and the
// MODERN layout overrides that rule at 7765: `border-color: var(--secondary-color)`
// plus a `0 0 0 2px var(--secondary-color)` shadow, and --secondary-color is
// #f5f5f5 (base.css:11). The difference is not subtle on a dark card — white at
// 0.8 over the #0D0D0D background composites to 207, against 245 — and it is
// exactly the "nuvioweb's is whiter" the owner saw with the two side by side.
// The pure #FFFFFF of NV_RING_FOCUS is a different token (--focus-color) and stays
// where it is: the continue-watching card really does use white in the web.
#define NV_FRAME_RING      4.0f
#define NV_FRAME_RING_C    0.9608f  // 245/255
#define NV_FRAME_RING_A    1.0f

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

// --- THE SKELETON'S SHINE ----------------------------------------------------
//
// The travelling highlight that says a still grey block is WAITING and not
// broken. GFX_SKELETON draws it; these are its numbers.
//
// IT SWEEPS ACROSS THE SCREEN, NOT ACROSS EACH BLOCK, and that is the whole
// design of it. Give every block its own 0..1 phase and a row of three episode
// cards pulses in unison, three lights blinking together — which reads as three
// separate controls, not as one page filling in. In screen space there is ONE
// light passing over the page: the picker catches it, then the first card, then
// the second. The caller converts the sweep's screen x into the block's own
// coordinates (gfx_skeleton), so a block does not need to know where it is.
//
// The same reasoning is what killed the PULSE this replaces (see drawSkel in
// detail.c): two bars breathing on the hero while the blocks below sat still read
// as two interfaces. One sweep, one clock, every skeleton in the app on it.
#define NV_SKEL_SHINE_W     440.0f   // the band's width on screen
// HOW BRIGHT THE BAND IS AND HOW FAR IT LEANS are in the shader (GFX_SKELETON in
// gfx.c) and deliberately not here: nothing in C computes with them, and a second
// copy in this file could only ever drift from the one the GPU reads. What does
// live here is what the CALLER needs — the band's width, to convert the sweep into
// a block's own coordinates, and the clock.
// ONE PASS EVERY 1800ms, of which the band is crossing for 1200 and parked off
// the right-hand edge for the remaining 600.
//
// The dwell is not idle time to be tightened up. Without it the passes run
// back-to-back and the page strobes: the eye reads a repeating light as a
// progress indicator and starts timing it, and 1.5s of continuous sweeping on a
// screen that is merely waiting for one HTTP answer is busier than the thing it
// is standing in for. With the gap it is a slow glance across the page.
#define NV_SKEL_SHINE_MS       1800
#define NV_SKEL_SHINE_SWEEP_MS 1200

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
// The two families land on the SAME number, and the poster's is 1.05 — not the 1.02
// that used to be here. Four rules compete for a focused poster card, and the sheet
// is read in the wrong order if you stop at the first three:
//   .home-screen-shell .home-content-card.focused                    -> 1.01  (0,3,0)
//   .home-screen-shell .home-poster-card.focused                     -> 1.05  (0,3,0)
//   .home-screen-shell.home-layout-modern .home-content-card.focused -> 1.02  (0,4,0)
//   [class*="-card"].focusable:not([class*="player-"]).focused       -> 1.05  (0,4,0)
// The last one (components.css:21168) is what wins: an attribute selector weighs the
// same as a class, `:not()` takes its argument's weight, so it TIES with the modern
// rule at (0,4,0) and comes ~13000 lines later in the same sheet. The tie-break is
// source order, and the previous note stopped one rule short of it.
//
// CONFIRMED, not deduced: with the focus on a poster in the running app,
// getComputedStyle().transform is `matrix(1.05, 0, 0, 1.05, 0, 0)` and the card's
// border box measures 240.4 x 364.3 against the 229 x 347 of the one beside it.
// The continue-watching card reaches 1.05 by its own route
// (`.home-continue-card.focused`, with !important) and so is unchanged.
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
// device: components.css:19314 crushes the scale to 1.005 under .legacy-webos /
// .legacy-tizen. That needs webOS <= 6 (js/app.js:186) and the C3 is webOS 23, so the
// full 1.05 is what the reference actually shows on the owner's TV.
#define NV_FOCUS_SCALE_POSTER 0.05f
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
// THE POSTER GRIDS' PAGE SCROLL — the Library, Discover and the collection grid.
//
// FIRST-ORDER AND NOT anim_spring2, which is the opposite of what the rest of
// this app was moving towards, so it is worth saying why. The second-order
// spring's whole virtue is a SOFT START: it leaves at zero velocity, which is
// what makes the home's row glide match the reference. On these grids the soft
// start is a liability, because the thing the viewer is waiting for is the row
// arriving from off-screen, and it only becomes visible in the last tenth of the
// travel — the part the soft start pushes furthest away. MEASURED in simulation
// against the shipped builds: the collection grid on spring2 at 11.5 left the
// incoming row invisible for 367 ms and 90% opaque at 517 ms, where the Library
// on the first-order spring was 250/450. It was reported, twice, as the cards
// coming back into view being slow — and as being slower than the Library, which
// it measurably was.
//
// 12 and not the 8 the Library used: 8 matched the Library's own feel, and the
// Library was itself called slow. At 12 the three grids land at 167-217 ms to
// first sight and settle by ~520 ms.
#define NV_SPRING_GRID     12.0f
// The frequency (rad/s) of the second-order spring that scrolls the home's rows. It
// takes the k of the TAIL measured on the reference's glide (~12.5 /s); 11.5 is the
// value that makes the whole curve match, because in this spring the tail is only
// half the fit: p(t)=1-(1+wt)e^-wt crosses the halfway point at 1.678/w = 146 ms
// with w=11.5, and the measurement was ~145 ms. See anim_spring2() in anim.h for why
// the spring was swapped.
#define NV_SPRING2_SCROLL  11.5f
#define NV_SPRING_SCREEN      9.0f
// THE DETAIL'S OPENING AND CLOSING — the frequency (rad/s) of the critically
// damped spring that drives it. `NV_SPRING_SCREEN` still names the FIRST-ORDER
// stiffness the menu, the context menu and the episode sheet use; the detail's
// flight left it for the reason anim.h records at anim_spring2().
//
// WHAT WAS WRONG. The flight ran `anim_spring(t, 1, dt, NV_SPRING_SCREEN)` and
// then drew `smooth(t) = 1-(1-t)^3`. The two compose into a single exponential:
// with 1-t = e^-9s, (1-t)^3 = e^-27s — an EFFECTIVE stiffness of 27, 95% of the
// way in 111ms, leaving at maximum speed on the first frame. At the device's
// 60Hz that is 36% of the zoom inside the FIRST frame and 59% at 30Hz: the
// opening was not an animation, it was a cut with two frames of smear. The
// closing, which composes the other way round, took ~460ms — so the two ends of
// the same movement were four times apart.
//
// p(t) = 1-(1+wt)e^-wt starts at ZERO velocity, accelerates, and ends on an
// e^-wt tail with no overshoot. 10.0 puts the halfway point at 1.678/w = 168ms
// and 95% at 474ms; the closing is a little brisker, as a movement the viewer
// has already decided on should be.
#define NV_SPRING2_SCREEN      10.0f
// THE COLLECTION GRID'S WINDOW, which opens out of the card that was pressed. It
// is quicker than the detail's flight on purpose: that one carries a picture the
// viewer is meant to watch arrive, this one is a frame getting out of the way of a
// list. 15 puts the halfway point at 1.678/w = 112ms and 95% at ~316ms.
#define NV_SPRING2_GRID        15.0f
// CLOSING IS THE OPENING REVERSED, AND FASTER. 22 settles it in ~265ms against
// the opening's ~316ms, which is the difference between a window closing and a
// window being closed: going in is a place being entered and can take its time,
// coming out is an instruction already given.
//
// What reverses is the WORDMARK AND ITS LABEL, travelling back down to the hero;
// the window itself does not close again (see viewRect for why a shrinking clip
// over a full list is too busy to read). The content fades under them, and this is
// how long that takes.
#define NV_SPRING2_GRID_OUT    22.0f
// HOW FAR THE GRID'S COPY AND CARDS RISE into place as they come in. The folder's
// wordmark is already travelling UP, from the home hero's 326 to this screen's 83,
// so everything else rising with it means nothing on screen moves against anything
// else — the same rule NV_DETW_COPY_TOGETHER applies to the title screen.
//
// It is an ADDITION to the window opening out of the card, not a replacement for
// it: the window is what says "this card became the view", and taking movement
// away to tidy a transition is the mistake the detail screen's notes record twice.
#define NV_SEEALL_RISE          56.0f
#define NV_SPRING2_SCREEN_OUT  14.0f
// HOW THE BACKDROP GIVES THE SCREEN BACK on the way out: opaque down to
// CUT+FADE, gone at CUT, and the screen let go of there.
//
// MEASURED IN TIME, at NV_SPRING2_SCREEN_OUT: opaque to s = 0.24 (~196ms), clear
// at s = 0.06 (~325ms) — a crossfade of about 130ms with a hard stop after it.
//
// It was one multiplier (1.8) before, which put the art semi-transparent from
// 111ms all the way to the 0.006 cut at 593ms. That is 480ms of the title's
// picture blended over the home's, and because the closing ZOOMS the two copies
// are at different scales for the whole of it — a soft double image, which is the
// unease the owner reported on the backdrop. The art is the same photograph twice;
// the longer the two are mixed the worse it looks, so the fix is to spend less
// time there, not to ease it differently.
//
// The two directions are not symmetrical, and that is the shape that was settled
// on after trying every symmetrical one. OPENING, the rect flies out of the home
// hero's rect and the ramp only swaps one gradient for another over art that is
// already there, so it is front-loaded and over in a blink — spreading it dips
// the brightness in the middle. CLOSING, the rect does NOT fly: it stays
// full-bleed and dissolves, because a rectangle shrinking back across the home
// puts four travelling edges on screen and nothing hid them acceptably.
// THE SHELVES LEAVE BY SLIDING OUT OF THE VIEWPORT, so this is the whole depth of
// that viewport and not a fraction of the screen: NV_SHELF_TOP-96 is where the
// rows are clipped, and travelling the rest of the way past the bottom takes every
// row out of the clip. 658px at 1080.
//
// They used to go 8% and then be OCCLUDED by the title screen's backdrop painting
// over them. That backdrop is drawn behind them now (see detail_draw_bg), so it
// cannot cover anything and a nudge left them sitting on top of it. Fading them
// instead was tried and is not the same gesture: the cards should go DOWN and off,
// at full strength, the way they always appeared to.
#define NV_HOME_SHELF_EXIT  (NV_SCREEN_H - (NV_SHELF_TOP - 96.0f))
// AND HOW MUCH OF THE FLIGHT THEY TAKE TO DO IT. The shelves are not the subject
// of this transition, they are what is getting out of the way of it — spread over
// the WHOLE flight they hang around in the middle of the frame long after the
// title screen has arrived behind them, and at 40% they are gone before the eye
// has followed them anywhere. Two thirds is the middle ground: clearly quicker
// than the flight it sits inside, slow enough to read as cards leaving.
#define NV_HOME_SHELF_OUT   0.65f
// WHERE THE PAGE'S GROUND HAS FINISHED LEAVING on the way out, in the flight's
// own units. It fades from 1 at the start of the close to 0 here, and here is
// ABOVE NV_DETAIL_EXIT_CUT + NV_DETAIL_EXIT_FADE (0.24), where the card begins to
// dissolve — so the ground is always gone before the card turns translucent.
//
// That gap is the whole design. Both at once and the home arrives at (1-g)(1-a),
// which dips the screen 25% in the middle of a dissolve; ground vanishing in one
// frame instead, as it did, and the home snaps from black to full brightness
// around a card still 98% the size of the screen. Sequenced, neither happens, and
// the opening is the same shape read backwards: there the ground rises across the
// flight while the card is already opaque.
// WHERE THE PAGE'S GROUND ARRIVES on the way IN, in the flight's own units: 0
// until the backdrop is opaque, 1 shortly after.
//
// MEASURED PROBLEM. With the hero set to Top band the home's art is 80% of the
// screen, anchored top-right (heroRectFor), so 20% of the height carries no art
// at all. The backdrop flies out of that rect, so for most of the opening the
// bottom of the screen is whatever is behind it — and the ground used to rise on
// the flight's own curve, which left the home's ROWS lit under the art's bottom
// edge for the first third of the transition. A bright horizontal edge travelling
// over lit content is the most conspicuous thing in the frame.
//
// It cannot start any earlier than the backdrop turning opaque (t = 0.126) or the
// two are translucent together and the home arrives at (1-g)(1-a) — the 25% dip
// this file's exit notes describe. So it waits, then goes quickly: the art's edge
// is against the page's own colour from ~120ms instead of ~400ms.
#define NV_DETAIL_OPEN_GROUND_AT   0.13f
#define NV_DETAIL_OPEN_GROUND_OVER 0.17f
#define NV_DETAIL_EXIT_GROUND 0.30f
// WHERE THE HOME'S HERO COPY HAS FINISHED LEAVING, in detail_progress units.
// phase2 starts the title screen's copy at 0.45, so ending here means the two
// blocks are never on screen together. They are the same sentence at two sizes in
// two places, and overlapped they read as the text ghosted twice — which is the
// defect home.c's own note predicts for two copies crossfading.
#define NV_HOME_COPY_OUT     0.45f
#define NV_DETAIL_EXIT_CUT   0.06f
#define NV_DETAIL_EXIT_FADE  0.18f

// WHICH WAY THE COPY TRAVELS while the screens change. One line, two readings of
// the same moment, and the only difference between them is a sign.
//
//   0 — the two blocks move APART: the home's drops 6% of the screen and fades,
//       the detail's comes up 5% + 26px into place. Opposed directions read as a
//       SWAP — one thing leaving, a different thing arriving.
//   1 — both travel the SAME way, downward, with the rows the detail pushes down.
//       Nothing changes direction anywhere on screen, so it reads as ONE block
//       re-laying out rather than two blocks trading places.
//
// The logo is unaffected either way: it flies between the two rects and is the one
// element that genuinely IS shared (see the note at the detail's logo).
#define NV_DETW_COPY_TOGETHER  1
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
// THE LOGO'S BASE TO THE TOP OF THE ACTIONS ROW, and the 40 it used to be was the
// CSS margin rather than the rendered gap. Measured in NuvioWeb on 2026-09-15 at
// 1920x1080: the logo ends at 465.02 and the row starts at 562.61 — 97.59. The
// difference is `.detail-trailer-hint`, a paragraph that is in the flow at all times
// and only carries text while a trailer plays, so it opens 57.6px of empty band that
// the sheet's margin does not mention.
//
// It is NOT slack to be trimmed: that band is where the focused circular button's
// tooltip lives (see NV_DETWEB_TIP_* in detail.h). The label's box reaches 54px above
// the button, so at the old 40 it would have been drawn over the logo.
#define NV_DETW_LOGO_GAP   98.0f
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
// The resume line the web app draws between the actions row and the support line
// (MEASURED, signed in: 1720x37 at (72,633)) has NO token here: the native screen does
// not draw it, because that band is where the action tooltips come out. See detail.c.
#define NV_DETW_LD_META    35.0f
#define NV_DETW_LD_META2   31.0f
#define NV_DETW_META_SEP   24.0f   // gap do flex, dos dois lados do ponto

// ---------------------------------------------------------------------------
// The SEARCH screen — READ OFF NuvioWeb's css/components.css (2026-09-19).
//
// THE NUMBERS THAT WERE HERE BEFORE ARE GONE, and it is worth saying why rather
// than leaving a diff to explain it. They were measured live on 2026-09-01 and
// were right then. Since then the sheet was rewritten, and `.search-content`
// now appears TWICE in components.css: a short block at ~2659 and the real one
// at ~2760. The last matching rule wins, so the first block is dead — porting
// from it reproduces the SUPERSEDED screen, which is exactly the trap this
// checkout has fallen into before.
//
// WHAT MOVED, old block -> live block:
//   content inset       104                -> 64   (padding: 48px 0 48px 64px)
//   header              y=22, h=110        -> under a "Search" page title, h=100
//   field and buttons   radius 22          -> radius 64, i.e. a full pill
//   field text          34/500, padding 32 -> 28/500, padding 88 (icon inside it)
//   focus ring          outer 2px halo     -> INSET 3px white
//   row title           48/600             -> 28/600
//   card name           28/500, gap 8      -> 24/500, gap 16
//   card focus          none               -> scale 1.05, transform-origin top
//   horizontal step     280                -> 272 (248 + a 24 gap)
//   row step            562.4              -> 586.8
//   empty state         icon + two lines   -> "Recent searches" chips, or "No Results"
//   end of a row        nothing            -> a 100px round "See All" button
//
// DERIVED, NOT MEASURED, and the difference matters to whoever checks this
// next: every value below comes from the stylesheet, which is exact for all of
// them but one — `line-height: normal` on the two 20px lines. That resolves
// through the FONT's metrics, and for Inter (unitsPerEm 2048, hhea ascender
// 1984, descender -494, lineGap 0) it is 2478/2048 = 1.21. So a 20px line
// occupies 24.2 and a browser measurement should agree to within a pixel. If a
// future measurement disagrees by more than that, the font changed, not this.
#define NV_SEARCH_X           64.0f   // .search-content padding-left
#define NV_SEARCH_TOP         48.0f   // .search-content padding-top
// The right edge. .search-content has padding-right 0 and it is .search-header
// that insets itself by 64; the RESULT TRACK really does run to 1920 and is
// clipped by the screen, which is how the web app makes a row read as
// continuing past the edge. The field stops at 1856.
#define NV_SEARCH_RIGHT       (NV_SCREEN_W - 64.0f)     // 1856
#define NV_SEARCH_TITLE_H     48.0f   // .library-page-title 48/600, line-height 1
#define NV_SEARCH_TITLE_LS     1.0f   // its letter-spacing
#define NV_SEARCH_TITLE_GAP   32.0f   // .library-page-header margin-bottom
#define NV_SEARCH_HEAD_Y      (NV_SEARCH_TOP + NV_SEARCH_TITLE_H + NV_SEARCH_TITLE_GAP)  // 128
#define NV_SEARCH_HEAD_H     100.0f   // .search-input-field, and the two buttons
#define NV_SEARCH_HEAD_GAP    64.0f   // .search-header margin-bottom
#define NV_SEARCH_BODY_Y      (NV_SEARCH_HEAD_Y + NV_SEARCH_HEAD_H + NV_SEARCH_HEAD_GAP) // 292
// A PILL, not a rounded rectangle. The sheet says `border-radius: 64px` on a box
// 100 tall, and a radius over half the height clamps to half — so the correct
// value for this gfx (where the radius is a fraction OF THE HEIGHT) is 0.5.
// Writing 64/100 here would draw a shape the browser never draws.
#define NV_SEARCH_PILL         0.5f
#define NV_SEARCH_FIELD_PADX  88.0f   // .search-input-field padding, both sides
#define NV_SEARCH_ICON        36.0f   // .search-input-icon svg
#define NV_SEARCH_ICON_X      36.0f   // .search-input-icon left
#define NV_SEARCH_CLEAR       32.0f   // .search-clear-btn svg
#define NV_SEARCH_CLEAR_X     32.0f   // .search-clear-btn right
// box-shadow: inset 0 0 0 3px rgb(255 255 255 / .96) on the focused field.
// INSET is the whole point: the old outer halo washed the field out (see the
// note in search.c), and an inset band cannot be clipped by a neighbour either.
#define NV_SEARCH_RING         3.0f
#define NV_SEARCH_BTN        100.0f   // .search-discover-btn / .search-voice-btn
#define NV_SEARCH_BTN_GAP     24.0f   // .search-header gap
#define NV_SEARCH_BTN_ICON    48.0f   // their svg
// One row: the title, the origin under it, then the track.
//   title      28/600, line-height 1.2                       -> 33.6
//   subtitle   margin-top 4, 20/400, line-height normal      -> 4 + 24.2
//   track      padding 16px 64px 32px 16px, gap 24
#define NV_SEARCH_ROW_SUB     37.6f   // row top -> the origin line (33.6 + 4)
#define NV_SEARCH_ROW_RAIL    77.8f   // row top -> the cards (33.6 + 4 + 24.2 + 16)
#define NV_SEARCH_TRACK_X     16.0f   // .search-results-track padding-left
#define NV_SEARCH_CARD_W     248.0f
#define NV_SEARCH_CARD_STEP  272.0f   // 248 + the track's 24 gap
#define NV_SEARCH_POSTER_H   372.0f
#define NV_SEARCH_POSTER_R    22.0f   // calc(var(--home-poster-radius,24px) - 2px)
#define NV_SEARCH_NAME_GAP    16.0f   // .search-result-name margin-top
#define NV_SEARCH_DATE_GAP     4.0f   // .search-result-date margin-top
// 372 + 16 + 28.8 (24 x 1.2) + 4 + 24.2 (20 x normal)
#define NV_SEARCH_CARD_H     445.0f
// The card scales on FOCUS now — it did not in the old sheet, and search.c still
// carried a comment saying so. `transform-origin: top`, so the poster's top edge
// stays put and the growth goes downwards.
#define NV_SEARCH_CARD_FOCUS  1.05f
// 77.8 + 445.0 + 32 (track padding-bottom) + 32 (.search-results-row margin-bottom)
#define NV_SEARCH_ROW_STEP   586.8f
// The "See All" button at the end of a track: 100x100, round, `align-self:
// center` with `margin: 0 0 80px 32px`. Centring acts on the MARGIN box, so the
// 80 below pushes the circle up: (445.0 - 180) / 2 = 132.5 from the top of the
// track's content, which puts its centre within 4px of the posters'.
#define NV_SEARCH_SEEALL     100.0f
#define NV_SEARCH_SEEALL_GAP  32.0f
#define NV_SEARCH_SEEALL_Y   132.5f
#define NV_SEARCH_SEEALL_ICO  48.0f
// The idle state: "Recent searches" and the chips under it.
#define NV_SEARCH_HIST_GAP    32.0f   // .search-history-label margin-bottom
#define NV_SEARCH_HIST_LS      1.0f   // its letter-spacing, and it is uppercase
#define NV_SEARCH_CHIP_H      60.0f   // 14 + 28 (line-height 1) + 14 + 2x2 border
#define NV_SEARCH_CHIP_PADX   28.0f
#define NV_SEARCH_CHIP_GAP    24.0f   // .search-history-chips gap, both axes
#define NV_SEARCH_CHIP_ICON   28.0f
#define NV_SEARCH_CHIP_ICOGAP 16.0f
#define NV_SEARCH_CHIP_BORDER  2.0f
#define NV_SEARCH_CHIP_FOCUS  1.06f
#define NV_SEARCH_HIST_MAX       6    // SEARCH_HISTORY_MAX in searchScreen.js
// The "No Results" state. .search-empty-state-results reserves 420 and the block
// is centred HORIZONTALLY only (justify-content: center on a flex container),
// with its own two lines left-aligned inside it.
#define NV_SEARCH_EMPTY_H    420.0f
#define NV_SEARCH_EMPTY_TOP   22.0f   // .search-empty-state h2 margin-top
#define NV_SEARCH_EMPTY_GAP   10.0f   // its margin-bottom

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
// THESE ARE THE APP'S GRID. Every screen that lays posters out in a grid uses
// this card, this gap and this row step — the Library, the Discover screen at
// its own inset, and the collection grid (seeall.c). Moving between them should
// be the same wall with different titles in it, and a grid re-measured per
// screen is how that stops being true: the collection grid used to be 248-wide
// posters 16 apart, which read as another app's screen.
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

// ---------------------------------------------------------------------------
// THE DROPDOWN — the app's one picker control, drawn by dropdown.c.
//
// It was the Discover screen's, measured off NuvioWeb's `.library-picker-anchor`
// and its menu, and the numbers lived in the NV_DSC_ block below. The collection
// grid then needed the same control, and the choice was to copy sixty lines of
// drawing or to move them. This is the move: the geometry is here, the drawing is
// in dropdown.c, and both screens ask for the same furniture.
//
// What stays with each screen is WHERE its pickers sit and how wide they are —
// that is page layout, not the control.
#define NV_DD_PICK_H      100.0f   // min(5.21vw, 100px)
#define NV_DD_PICK_PADX    36.0f   // .library-picker-anchor padding
// A PILL, like the search field: border-radius 64 on a box 100 tall clamps to
// half the height. The radius here is a fraction OF THE HEIGHT, so 0.5.
#define NV_DD_PILL          0.5f
#define NV_DD_RING          3.0f   // box-shadow: inset 0 0 0 3px, focused
#define NV_DD_COPY_GAP      4.0f   // .library-picker-copy gap
#define NV_DD_CHEV         32.0f   // the chevron, right-aligned in the anchor
// The list a picker opens. It is the detail screen's season picker, and
// deliberately so — that control already taught this app how a list behaves on
// a D-pad, and a second idiom for the same job would be a worse answer twice.
// The numbers are its numbers (NV_DETWEB_SEA_MENU_* in detail.h), restated here
// so a screen does not include the detail screen's header for five constants and
// inherit its whole vocabulary.
#define NV_DD_MENU_GAP      8.0f   // anchor base -> the menu's top
#define NV_DD_MENU_PADY     4.0f
#define NV_DD_MENU_PADX    10.0f
#define NV_DD_OPT_H        84.0f
#define NV_DD_OPT_PADX     32.0f
// Six rows, then it scrolls. The web caps its menu at 540px, which is 6.4 of
// these — and six is also what the season picker settled on.
#define NV_DD_OPT_VIS         6

// ---------------------------------------------------------------------------
// The DISCOVER screen — READ OFF NuvioWeb's css/components.css (2026-09-19).
//
// It is the Library's furniture with a different data source: the same picker
// row and the same poster grid, over a catalogue chosen by hand instead of the
// owner's saved list. So almost nothing here is new geometry — it is the
// Library's, re-measured at Discover's own inset.
//
// THE `.discover-card-*` BLOCK IN THE SHEET IS DEAD, and it is worth saying so
// because it is the obvious thing to port from: it describes a 372-tall poster
// with a 12px radius. discoverScreen.js renders `library-grid-card`, so the
// LIBRARY's rules are the ones that apply. This is the same superseded-block
// trap already recorded against the search screen.
#define NV_DSC_X            64.0f   // .discover-main padding
#define NV_DSC_Y            48.0f
#define NV_DSC_W           (NV_SCREEN_W - 2 * NV_DSC_X)    // 1792
// The page header: the title, and the context label right-aligned against it.
#define NV_DSC_TITLE_H      48.0f   // .library-page-title 48/600, line-height 1
#define NV_DSC_TITLE_LS      1.0f
#define NV_DSC_HEAD_GAP     36.0f   // .library-page-header margin-bottom 32 + 4
// Three pickers sharing the width, `flex: 1 1 0` with a 12 gap.
#define NV_DSC_PICK_Y       (NV_DSC_Y + NV_DSC_TITLE_H + NV_DSC_HEAD_GAP)   // 132
#define NV_DSC_PICK_GAP     12.0f   // .discover-picker-row gap
#define NV_DSC_PICK_N          3
#define NV_DSC_PICK_W       ((NV_DSC_W - (NV_DSC_PICK_N - 1) * NV_DSC_PICK_GAP) / NV_DSC_PICK_N)
#define NV_DSC_PICK_STEP    (NV_DSC_PICK_W + NV_DSC_PICK_GAP)
// The control itself is NV_DD_* above; only the row's own geometry is here.
#define NV_DSC_PICK_H       NV_DD_PICK_H
// The grid. The web says `repeat(auto-fill, minmax(252px, 252px))` with
// `gap: 48px 24px` — FIXED 252 columns, which at this width fits six and leaves
// 160px of the row empty on the right.
//
// THE COLUMNS ARE NOT FIXED HERE, and that is a deliberate divergence. A browser
// window is any width and auto-fill has to cope; a television is exactly 1920,
// and 160px of dead margin on the right of every row is not a layout, it is the
// remainder. Six columns SHARE the width instead: 278.67 each, which is 10%
// more poster for nothing. Everything else — the gaps, the radius, the 4px
// inside border, the 1.05 focus — is the sheet's.
// The picker row's 64 of clearance (.library-picker-groups margin-bottom).
#define NV_DSC_GRID_Y       (NV_DSC_PICK_Y + NV_DSC_PICK_H + 64.0f)   // 296
#define NV_DSC_COLUMNS         6
#define NV_DSC_CARD_GAP     24.0f
#define NV_DSC_CARD_W      ((NV_DSC_W - (NV_DSC_COLUMNS - 1) * NV_DSC_CARD_GAP) / NV_DSC_COLUMNS)
#define NV_DSC_CARD_STEP    (NV_DSC_CARD_W + NV_DSC_CARD_GAP)
#define NV_DSC_POSTER_H     (NV_DSC_CARD_W * 1.5f)     // 2:3, like every poster here
#define NV_DSC_POSTER_R     24.0f   // --library-poster-radius
#define NV_DSC_BORDER        4.0f   // the focus border, on the INSIDE
#define NV_DSC_TITLE_GAP    16.0f   // .library-grid-card gap
#define NV_DSC_ROW_GAP      48.0f   // the grid's row gap
// .library-grid-title is 24/500 with line-height 1.18 -> 28.3.
#define NV_DSC_LD_TITLE     28.3f
#define NV_DSC_LINE_STEP    (NV_DSC_POSTER_H + NV_DSC_TITLE_GAP + NV_DSC_LD_TITLE + NV_DSC_ROW_GAP)
#define NV_DSC_FOCUS_SCALE  0.05f   // .library-grid-card.focused, origin top
// WHERE THE GRID MAY DRAW. To the bottom of the SCREEN, not to NV_MARGIN_Y above
// it: the web app's scroller clips at its border box and the 48px of padding is
// inside the scrollport, so a card really does run to the last pixel. Stopping
// 60 short was cutting cards off with screen still left under them.
#define NV_DSC_GRID_BOTTOM  NV_SCREEN_H
// The line the grid is clipped at, AND the top of the band a row dissolves
// across on its way out — the two are the same line, because a row that has
// already reached zero cannot be cut by the clip.
//
// IT IS THE PICKER ROW'S BASE NOW, where it used to sit 24 above the posters and
// deliberately not up here. That note is worth keeping, because the reason it
// sat lower no longer applies:
//
//   MEASURED: with the clip at 248 (pickers + 16) a THREE-PIXEL sliver of the
//   previous row's titles survived at y 250-252. The arithmetic says it should
//   not: a snapped scroll puts row N's posters at NV_DSC_GRID_Y, which leaves
//   row N-1's title ending exactly NV_DSC_ROW_GAP higher, at 248. But
//   NV_DSC_LD_TITLE is the CSS line box (24 x 1.18 = 28.3) and SDL_ttf's box for
//   the same face is nearer 30, so the descenders ran a couple of pixels past
//   where the sheet says the line ends.
//
// The dissolve settles that sliver at the source: opacity is keyed off the ROW's
// top, and at rest row N-1's top is a whole line step above the fold, so the row
// and its descenders are at zero and nothing is drawn to be cut. The clip is now
// only a backstop.
#define NV_DSC_CLIP_TOP     (NV_DSC_PICK_Y + NV_DSC_PICK_H)
// The 64 of clearance under the pickers, read as what it now does: the distance
// a row has to dissolve across. Same ramp as the home's and the collection
// grid's — anim_edge.
#define NV_DSC_FADE         (NV_DSC_GRID_Y - NV_DSC_CLIP_TOP)

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
// THE SIDE MENU'S GLASS: the size the strip ends up blurred at, and how often it
// is re-grabbed.
//
// 64x176 for a 392x1080 strip — 6.1 screen pixels per texel in BOTH directions,
// and the isotropy is not incidental. This was 64x256, which is 6.1 across and 4.2
// down, and the gaussian steps in TEXELS: the same kernel then reached 45% further
// sideways than downwards and the glass came out smeared horizontally.
//
// The strip does not arrive here in one jump. gfx_backdrop walks it down in
// HALVING steps (see BD_LEVEL) because GL_LINEAR reads FOUR texels, no matter how
// far apart the source pixels are: minifying 6x in a single draw samples a grid
// out of the picture instead of averaging it, and what came back was a crosshatch
// of thin streaks that crawled whenever the content behind scrolled. Halving keeps
// each step inside what four taps can actually average.
//
// 80ms = 12.5 updates a second, and it is a MEASURED figure, not a guess. With the
// regeneration pinned off, the worst frame with the bar open sat at 6.7-12.4ms; at
// 20 updates a second it went to 12.8-28.2ms. So one regeneration costs the better
// part of a frame, and it lands on whole frames rather than spreading. The blur is
// 6 screen pixels per texel: at that softness nothing in it can be seen to lag 80ms
// behind, and dropping from 20 to 12.5 takes a third of the cost off for nothing.
//
// TURN IT UP, not down, if the device shows janks with the bar open. This is the
// knob for it, and the numbers above were taken on the Mac preview at retina — the
// TV grabs roughly half as many pixels, and has not been measured.
//
// THREE ITERATIONS OF THE GAUSSIAN, and the number is arithmetic rather than
// taste. GFX_BLUR's nine taps have a sigma of 2.078 x its step, so at
// NV_BLUR_STEP 2.4 on a target where one texel is 392/64 = 6.1 screen pixels, one
// pass blurs with sigma 30.5px. The web app asks for `blur(52px)`, and CSS means
// the STANDARD DEVIATION by that number — so a single pass was at 59% strength,
// which is why card edges behind the bar still came through as soft vertical
// bands after the aliasing was fixed. Convolving a gaussian with itself adds
// variance, so n passes give sigma*sqrt(n): 30.5 -> 43.2 -> 52.9. Three lands on
// 52.9px, the figure in the stylesheet.
//
// Three iterations and NOT one wide one. The same sigma is reachable by opening
// NV_BLUR_STEP to 4.16, and it costs four fewer render-target switches — but the
// nine taps would then be 4.2 texels apart on a picture that still carries detail
// down to the texel, which point-samples it and puts the streaks straight back.
#define NV_BACKDROP_W        64
#define NV_BACKDROP_H       176
#define NV_BACKDROP_PASSES    3
#define NV_BACKDROP_MS       80
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

// ---------------------------------------------------------------------------
// THE ACCOUNT'S PROFILE PICKER ("Who's watching?")
//
// MEASURED in the web app at 1920x1080, from `.profile-screen` and the block of
// rules under it (components.css:420-660) plus the markup in
// profileSelectionScreen.js `render()`. Every number below is a rendered
// bounding box, not a value read off the stylesheet: the layout is a column of
// flex items whose top is decided by `.profile-title { margin: auto 0 0 0 }` —
// the auto margin pushes title, subtitle, grid and hint to the BASE of the
// padded content box, so their y depends on how tall the grid is. The tokens are
// therefore the pieces, and profile_select.c stacks them from NV_PSEL_BOTTOM up.
//
// The one number that is not a measurement is NV_PSEL_COLS. The web app wraps
// with `flex-wrap: wrap` inside 1696px of content width, and 5 cards
// (5*304 + 4*56 = 1744) do not fit while 4 (1384) do. Four is that wrap point
// written down, not a choice.

// The wordmark: `.profile-logo`, 380x88 at the top of the content box, with a
// 56px margin under it. It is dropped when the cards need two rows — there is no
// room for it and the web app, whose grid is a fixed 400px tall, simply overflows.
#define NV_PSEL_LOGO_W        380.0f
#define NV_PSEL_LOGO_H         88.0f
#define NV_PSEL_LOGO_Y         96.0f
#define NV_PSEL_LOGO_GAP       56.0f
// `.profile-main-layer` is padded 96px top and bottom: 1080 - 96 = 984.
#define NV_PSEL_BOTTOM        984.0f
#define NV_PSEL_TOP            96.0f

// The header block, in CSS LINE BOXES (font-size x line-height), because that is
// what decides the spacing — the rasterised ink is centred inside each one.
#define NV_PSEL_TITLE_H        50.4f   // .profile-title      48 / 1.05
#define NV_PSEL_TITLE_SUB      24.0f
#define NV_PSEL_SUB_H          46.8f   // .profile-subtitle   36 / 1.3
#define NV_PSEL_SUB_GRID       32.0f
#define NV_PSEL_GRID_HINT      48.0f
#define NV_PSEL_HINT_H         37.8f   // .profile-hint       28 / 1.35

// One card. `.profile-card` is 304 wide with 16/20 padding; the ring's own
// `margin: 12px auto` collapses with the name's 24px margin-top, which is why the
// gap under the ring is 24 and not 36 — measured, and the reason the card is
// 384.8 tall and not 396.8.
#define NV_PSEL_CARD_W        304.0f
#define NV_PSEL_GAP            56.0f
#define NV_PSEL_PAD_TOP        16.0f
#define NV_PSEL_RING_MARGIN    12.0f
#define NV_PSEL_NAME_GAP       24.0f
#define NV_PSEL_NAME_H         40.8f   // .profile-name       34 / 1.2
#define NV_PSEL_BADGE_GAP      16.0f
#define NV_PSEL_BADGE_LINE     24.2f   // .profile-badge      22 / 1.1
#define NV_PSEL_BADGE_H        32.0f   // min-height, so both states align
#define NV_PSEL_PAD_BOTTOM     16.0f
// `.profile-grid` is a fixed 400px against a 384.8 card: 15.2 of slack under the
// row, which is where the focused card's 5% growth goes.
#define NV_PSEL_GRID_SLACK     15.2f
#define NV_PSEL_COLS              4

// Focus. The ring and the avatar change SIZE (a layout change in the web app),
// and the whole card is then scaled — `[class*="-card"].focusable.focused`
// (components.css:21169) wins over `.profile-card.focused`'s 1.04 with a
// transform of scale(1.05), and the origin computes to `center top`, not the
// `center center` .profile-card declares. Growing downwards only is why the row
// of names does not shift when the cursor moves.
#define NV_PSEL_RING          228.0f
#define NV_PSEL_RING_F        244.0f
#define NV_PSEL_BORDER          2.0f
#define NV_PSEL_BORDER_F        6.0f
#define NV_PSEL_AVATAR        192.0f
#define NV_PSEL_AVATAR_F      204.0f
#define NV_PSEL_SCALE_F        0.05f
// The initial inside the disc is 77 and goes to 82 on focus — a font-size change
// ON TOP of the transform, so the ink really grows by 82/77 * 1.05.
#define NV_PSEL_INITIAL_F     (82.0f / 77.0f)

// The star that marks the primary profile. `.profile-primary-dot` is 52px, and
// its right/bottom offsets are measured against the ring's PADDING box — inside
// the border — which is why the offset changes with the focus state.
#define NV_PSEL_DOT            52.0f
#define NV_PSEL_DOT_RIGHT      16.0f
#define NV_PSEL_DOT_BOTTOM     14.0f
#define NV_PSEL_DOT_BORDER      4.0f
// .profile-badge letter-spacing. At 22px over seven capitals it is 11px of extra
// width — 12% of the word, and visible.
#define NV_PSEL_BADGE_TRACK     1.6f

// Colours. rgba(51,51,51,0.75) for the resting ring, white for the focused one;
// the name goes --text-secondary -> --text-color; #FFB300 is the primary marker.
#define NV_PSEL_RING_RGB       0.200f
#define NV_PSEL_RING_A         0.750f
#define NV_PSEL_NAME_RGB       0.702f   // #B3B3B3
#define NV_PSEL_HINT_RGB       0.502f   // rgba(128,128,128,0.9)
#define NV_PSEL_HINT_A         0.900f
#define NV_PSEL_GOLD_R         1.000f
#define NV_PSEL_GOLD_G         0.702f
#define NV_PSEL_GOLD_B         0.000f
// The default when a profile carries no colour: --secondary-color #F5F5F5,
// which is `DEFAULT_PROFILE_COLOR` in profileSelectionScreen.js.
#define NV_PSEL_DEFAULT_RGB    0.961f

// The background tween. updateBackground() runs a 520ms RAF loop over the accent
// colour with a fast-out-slow-in curve; the gradient itself is baked into the
// GFX_PROFILE_BG shader, because both of its layers derive from that one colour.
#define NV_PSEL_BG_MS         520.0f

// --- THE PLAYER SHEETS' TABS (tabs.c) ------------------------------------------
// A word, and a 3px rule under the chosen one, the rule sitting NV_TAB_LINE_GAP
// under the capitals. The strip lines up with the heading's left edge — there is
// no pill padding to indent it any more.
#define NV_TAB_H            50.0f
#define NV_TAB_GAP          40.0f
#define NV_TAB_LINE          3.0f
#define NV_TAB_LINE_GAP     12.0f

// --- THE PLAYER'S TRACK MENUS -------------------------------------------------
// The audio and subtitle panels, ported from the web app's "Track menus" block.
// Same resolution rule as the font sizes up at NV_FT_TRK_*: each `min(X vw, Y px)`
// is evaluated at 1920.
//
// THE SIDE PANEL WEARS THE SOURCES SHEET'S SHELL: the same veil, the same 712px
// column against the right edge, the heading at the same height and the tabs on
// the same line. They are the player's two sheets of choices, and opening one
// after the other should look like the same furniture with different contents.
// THE VEIL IS THE SOURCES SHEET'S ramp and ink, over a narrower run: the curve
// is the same, it simply reaches full strength sooner, as the column it carries
// is narrower — a subtitle row is a short name and a line of detail, not a source
// card with a row of badges.
#define NV_TRK_VEIL_W      820.0f
// Where the veil starts to clear downwards, as a fraction of the screen's height.
// The selects and their menus sit in the top half; below that the veil only has
// to take the edge off, not hold type up.
#define NV_TRK_VEIL_CLEAR   0.50f
#define NV_TRK_CONTENT_W   560.0f
#define NV_TRK_PAD          48.0f   // the content's right margin
#define NV_TRK_FOOT         48.0f   // clear space under an expanded list
#define NV_TRK_TITLE_Y      72.0f
#define NV_TRK_TABS_Y      160.0f
#define NV_TRK_TOP         244.0f   // the first row's top edge
// A short slide rather than the sources sheet's full-width travel: this sheet is
// reopened constantly mid-film, and a drawer hauled across the picture every time
// is a lot of motion for one track change.
#define NV_TRK_SLIDE        0.06f


// rgba(11,13,16): a lifted black, not a true one — at full strength over dark
// video a true black is a hole. The player's hot buttons ink with it.
#define NV_TRK_INK_R       0.0431f
#define NV_TRK_INK_G       0.0510f
#define NV_TRK_INK_B       0.0627f

// FOCUS IS AN INVERSION, not an outline: the row fills with white and its type
// goes dark. From three metres a thin ring on a dark row is the easiest thing on
// the screen to lose, and focus is the one thing the sheet must not lose.
#define NV_TRK_FOCUS_FILL   1.000f
#define NV_TRK_FOCUS_INK      11    // #0b0d10, as 0..255
// The sub-label on a focused row: rgba(11,13,16,0.62) over white, flattened.
#define NV_TRK_FOCUS_INK_SUB 104
#define NV_TRK_ROW_FILL     0.06f
// A select whose list is open: lifted, not inverted — the cursor is in the list.
#define NV_TRK_ROW_OWNER    0.16f

// THE TRACKS TAB IS TWO SELECTS, stacked: Language, then Subtitle. They are
// drawn as the SOURCES SHEET'S CARDS — the same corner, the same faint fill that
// lifts under the cursor with a thin inset ring — because the two sheets sit in
// the same place in the same player. The detail screen's pill picker, tried
// first, belonged to a different screen and looked it.
#define NV_TRK_SEL_H        72.0f
#define NV_TRK_SEL_GAP      12.0f
#define NV_TRK_SEL_PADX     24.0f
#define NV_TRK_CHEV         24.0f
#define NV_TRK_MENU_GAP      8.0f   // a select's base -> its open menu's top
#define NV_TRK_MENU_PAD      8.0f
#define NV_TRK_LANG_H       64.0f
#define NV_TRK_OPT_H        84.0f   // a subtitle option: its name and a detail line
#define NV_TRK_OPT_R        12.0f
// The open menu's plate is opaque: it stands in front of the select below it.
#define NV_TRK_MENU_BG      0.105f
// The playing row's marker: a DOT and the word ON. At 12px a tick glyph is a smudge.
#define NV_TRK_DOT          12.0f

// --- THE STYLE BAR -------------------------------------------------------------
// The Style tab is NOT the side panel. A style is judged by where the subtitle
// actually lands, at its real width, and a panel over the right half would squeeze
// the preview into the left half and misstate both. So the tab hands the screen
// back to the picture and keeps its controls along the bottom: a row of tiles, one
// per setting, and under it the choices for the focused tile.
// Measured off the design at 1920: a 48px margin each side — the header's own
// right margin. The chips sit 28px off the bottom edge, lower than the design's
// 56: the TV cannot raise its own embedded subtitle any further, so the bar
// goes down to clear it instead.
#define NV_TRK_BAR_X        48.0f
#define NV_TRK_BAR_BOTTOM   28.0f
#define NV_TRK_ROWS_GAP     20.0f   // tiles -> chips
// The transport scrim's run. It starts well above the tiles because its first
// stretch is nearly clear — it only reaches 0.18 a third of the way down — and a
// shorter run would make the ramp steep enough to see its steps.
#define NV_TRK_SCRIM_Y     540.0f
// The top-right pool behind the header once the panel's veil has gone.
#define NV_TRK_POOL_W      900.0f
#define NV_TRK_POOL_H      420.0f
#define NV_TRK_BOX_R        12.0f   // tiles and chips share one corner
#define NV_TRK_LIGHT       0.94f    // the focused tile's, and the chosen chip's, face
#define NV_TRK_TILE_H       82.0f
#define NV_TRK_TILE_GAP     16.0f
#define NV_TRK_TILE_PADX    20.0f
// A resting tile or chip is the SOURCES CARD'S fill, with a faint edge: the two
// sheets' resting surfaces read as the same material.
#define NV_TRK_TILE_FILL    NV_SRC_CARD_FILL
#define NV_TRK_TILE_LINE    0.08f
#define NV_TRK_TILE_OWNER   0.10f   // the tile whose choices the cursor is in
#define NV_TRK_TILE_OWNER_LINE 0.16f
#define NV_TRK_TILE_RING     2.0f   // the grey edge round the focused tile's face
#define NV_TRK_CHIP_H       58.0f
#define NV_TRK_CHIP_PAD     26.0f
#define NV_TRK_CHIP_GAP     20.0f
#define NV_TRK_CHIP_FILL    NV_SRC_CARD_FILL
#define NV_TRK_CHIP_LINE    0.10f
#define NV_TRK_RESET_ICON   20.0f
// The preview's lowest line may not sink below this while the bar is up: the
// Height setting can put the subtitle 144px lower than the default, which is
// straight through the tiles.
// It sits a clear 130px over the tiles, so the style is judged
// against the picture and not against the bar's own shading.
#define NV_TRK_PREVIEW_FLOOR 720.0f
// The raise for the EMBEDDED subtitle, in the uMS's own position steps (-3..4),
// since the pipeline and not the overlay draws it. The player applies it both
// under the Style bar and while its own controls are up; 4 from the default is
// as high as the uMS goes.
#define NV_TRK_EMBED_LIFT      4

// --- THE PLAYER'S TWO "JUMP AHEAD" PROMPTS -----------------------------------
// Skip intro and the next-episode card. They SHARE an anchor, and that is the
// point rather than a coincidence: both are optional "jump past this" offers over
// the frame, so landing them in the same corner at the same height makes them read
// as one affordance that returns to the same place, instead of two that arrive
// from different corners. It also keeps the bottom LEFT clear, which is where the
// title block and the transport's leading controls live.
//
// Legacy had them in opposite corners — skip intro bottom-left, the next card
// centred across the middle — which is exactly the arrangement the web app moved
// away from.
//
// Both sit low while the transport is down (nothing beneath them to clear) and
// rise to 236 when it comes up, the first height that clears the button row.
#define NV_PJ_RIGHT        64.0f   // right  min(3.33vw,64)
#define NV_PJ_BOTTOM       60.0f   // bottom min(3.125vw,60)
#define NV_PJ_BOTTOM_UP   236.0f   // .is-raised

// Skip intro. The resting fill is rgba(30,30,30,0.85); focused it takes
// --secondary-color, the same inversion the track menus use.
#define NV_SKIP_R          24.0f   // min(1.25vw,24)
#define NV_SKIP_PADX       36.0f   // min(1.875vw,36)
#define NV_SKIP_PADY       24.0f   // min(1.25vw,24)
#define NV_SKIP_GAP        16.0f   // min(0.83vw,16)
// THE ICON BOX IS NOT THE ICON. forward.png carries its arrow in the middle HALF
// of a 128px square — measured: the ink is 64x64 at (32,32), so the outer quarter
// on every side is transparent padding. The web app's ic_player_skip_next.svg has
// a tight viewBox, so its 24px box draws 24px of arrow; the same 24 here drew
// TWELVE, which is what "the skip icon is too small" was. It was an asset
// difference, not a sizing taste.
//
// So the BOX is twice the ink and the layout advances by the INK — the padding is
// transparent and must not open a gap the eye reads as spacing.
//
// 24 against the 28px label — the web app's own ~0.85 icon-to-label ratio, and the
// ratio the next-episode pill keeps too (18 against 22). It was briefly 28, which
// is 1:1, and at 1:1 the arrow stops reading as the label's mark and starts
// competing with it: the pair looked out of proportion with each other. The size
// to fix was never this one, it was the 12px the padded asset was really drawing.
#define NV_SKIP_ICON_INK   24.0f
#define NV_SKIP_ICON      (NV_SKIP_ICON_INK * 2.0f)   /* forward.png is 50% ink */
#define NV_SKIP_BG         0.1176f // rgb(30,30,30)
#define NV_SKIP_BG_A       0.85f
// The countdown band, the same 6px the Continue Watching card's bar uses.
#define NV_SKIP_BAR         6.0f
#define NV_FT_SKIP         28      // .player-skip-intro-label 28 / 500

// The next-episode card. Flat panel fill at the panel radius, no blur: every
// other surface in the player dropped backdrop-filter, which is a per-frame
// compositor cost paid on top of video decode, and this card was the last holdout.
#define NV_NEXT_R          20.0f
#define NV_NEXT_PAD        24.0f   // the card's inset above and below its content
// Left and right match, so the copy column ends as far from the card's edge as
// the thumbnail starts from it.
#define NV_NEXT_PADX       24.0f
#define NV_NEXT_GAP        24.0f   // thumbnail to copy column
// The thumbnail sits INSIDE the card's padding at 16:9 with its own radius — a
// picture on the card, not the card's left end. Its height is the copy stack's:
// caption, title, pills.
#define NV_NEXT_THUMB_H   132.0f
#define NV_NEXT_THUMB_W   (NV_NEXT_THUMB_H * 16.0f / 9.0f)
#define NV_NEXT_THUMB_R    12.0f
// The copy column is as wide as the wider of the title and the pill row, capped
// so a long episode name trims instead of stretching the card across the frame.
#define NV_NEXT_COPY_MAX  440.0f
#define NV_NEXT_TITLE_Y    28.0f   // caption top to title top
#define NV_NEXT_TITLE_DOT_GAP 12.0f // either side of the dot in "S1 E12 · Name"
#define NV_NEXT_PILL_H     44.0f
#define NV_NEXT_PILL_PADX  20.0f
#define NV_NEXT_PILL_GAP   10.0f
// play.png's triangle is not centred in half its box the way the skip glyph is:
// its ink runs from 25% to 94% of the width (x 32..120 of 128). Layout therefore
// counts the INK width and hangs the box so the ink starts on the text column —
// assuming the 2x rule put the triangle's tip against the "P".
#define NV_NEXT_PLAY_BOX   14.0f
#define NV_NEXT_PLAY_INK_X 0.25f
#define NV_NEXT_PLAY_INK_W (NV_NEXT_PLAY_BOX * 0.6875f)
#define NV_NEXT_PILL_ICON_GAP 10.0f
#define NV_NEXT_PILL_RING   1.5f   // the Not now outline, in px
// The countdown along the card's base, the same 6px band the skip button carries.
#define NV_NEXT_BAR         6.0f
#define NV_FT_NEXT_KICK    17      // "UP NEXT"          17 / 700, ls .2em
#define NV_FT_NEXT_COUNT   17      // "Playing in 8s"    17 / 700
#define NV_FT_NEXT_TITLE   28      // "S1 E11 · Name"    28 / 700
#define NV_FT_NEXT_PILL    19      // the action pills   19 / 700
#define NV_NEXT_KICK_TRACK  3.4f   // 0.2em at 17px

// THE PLAYER'S DROPDOWNS — the season pill here, the subtitle sheet's selects —
// are the title page's, but see-through: over the picture and the veil a solid
// #222 read as a slab pasted on. The anchor's ground and the open menu's plate
// both take this alpha; the focused option stays solid, it is what is read.
#define NV_PLR_DD_A        0.78f

// --- THE PLAYER'S EPISODE LIST -----------------------------------------------
// A right-hand list over the track menus' veil. It was a bottom-sheet rail of
// 400px cards ported from the web app, and that sheet covered the lower 62% of
// the picture — subtitles included — to show four episodes at a time. In the
// player the job is "find episode N" while the film keeps playing, which is a
// list's job.
//
// The web app left its drawer for two reasons, and the rows answer both: every
// row keeps its thumbnail, and only the FOCUSED row opens to show its synopsis,
// so the text is read once instead of being clamped into every row.
//
// Drawn to the design board under the track menus' heading: the season pill on
// the line their tabs use, rows that dim unless they are focused or playing, and
// focus as a band across the whole row with the ring on the thumbnail alone.
#define NV_EPL_W          780.0f   // the content column, set against the right margin
#define NV_EPL_PAD         48.0f   // the column's right margin
#define NV_EPL_VEIL_W    1100.0f
#define NV_EPL_ROW_H      144.0f   // a closed row
#define NV_EPL_ROW_GAP      4.0f
#define NV_EPL_PADX        16.0f   // the band's left edge to the thumb
#define NV_EPL_THUMB_W    208.0f
#define NV_EPL_THUMB_H    117.0f
#define NV_EPL_THUMB_R     12.0f
// The focused thumb grows about its left edge, as the title page's episode
// list does (NV_DETEP_THUMB_GROW), and the text beside it makes way.
#define NV_EPL_THUMB_GROW   1.10f
#define NV_EPL_TEXT_GAP    24.0f   // thumb -> the name
#define NV_EPL_TEXT_GROW   12.0f   // and the extra the focused row adds to it
// THE TEXT IS ONE BLOCK, centred in the row as the thumb is: the name, the
// detail line under it, and on an open row the synopsis under that. Offsets are
// from the block's top, measured on the lines' boxes.
#define NV_EPL_SUB_DY      40.0f   // the detail line
#define NV_EPL_SYN_DY      78.0f   // the synopsis
#define NV_EPL_BLOCK_H     66.0f   // a closed row's block: the name and the detail line
#define NV_EPL_PADY        20.0f   // an open row's air above and below its block
#define NV_EPL_SYN_LINES    3
#define NV_EPL_SYN_LD      28.0f   // 20px type x 1.4
#define NV_EPL_BAND         0.09f  // the focused row's band, white at this alpha
#define NV_EPL_BAND_LEAD   80.0f   // how far left of the column the band starts
#define NV_EPL_BAND_FEATHER 260.0f // the run over which it fades in from nothing
#define NV_EPL_DIM        128      // a resting row's name, as 0..255
#define NV_EPL_DIM_SUB    104      // and its detail line
#define NV_EPL_DIM_THUMB    0.55f  // and its thumbnail
#define NV_EPL_CHECK       30.0f   // the watched disc on the thumb's corner
#define NV_EPL_CHECK_TICK   0.55f  // the tick inside it, as a share of the disc
#define NV_EPL_PROG_H       5.0f   // the playing episode's bar along the thumb's base
#define NV_EPL_NEXT_H      36.0f   // the UP NEXT pill
#define NV_EPL_NEXT_PADX   14.0f
// The season pill, and the menu it opens.
// The subtitle select's type and chevron at a compact height, and one width for
// the pill and its menu, as the title page's picker and menu share theirs.
#define NV_EPL_PILL_H      64.0f
#define NV_EPL_PILL_W     300.0f
#define NV_EPL_PILL_PADX   28.0f
#define NV_EPL_PILL_CHEV   NV_TRK_CHEV
#define NV_EPL_SMENU_ROW   64.0f   // an option; the plate and its padding are detail.h's
#define NV_EPL_SMENU_VIS      8     // options in view before the menu scrolls
// The list's window: from under the pill to the sources sheet's foot. Rows are
// drawn past the bottom to the screen's edge; this is only the scroll's measure.
#define NV_EPL_VIEW_TOP    (NV_TRK_TABS_Y + NV_EPL_PILL_H + 20.0f)
#define NV_EPL_VIEW_BOTTOM (NV_SCREEN_H - NV_SRC_FOOT)
#define NV_FT_ERAIL_META   22      // .player-episode-detail-meta     22 / 700, ls .06em
#define NV_FT_ERAIL_TITLE  44      // .player-episode-detail-title    44 / 800
#define NV_FT_ERAIL_OVER   22      // .player-episode-detail-overview 22 / 500
#define NV_FT_ERAIL_CODE   24      // .player-episode-code            24 / 800
#define NV_FT_ERAIL_PILL   20      // .player-episode-current         20 / 800
#define NV_FT_ERAIL_CTITLE 24      // .player-episode-card-title      24 / 600
#define NV_LD_ERAIL_OVER   31.0f   // 22 x line-height 1.4
#define NV_ERAIL_META_TRACK 1.32f  // 0.06em at 22px

// The SEARCH screen's own sizes. Three, and none of them borrows a neighbour:
//   .search-result-name      24 / 500  — and .search-history-label at the same
//                                        24 / 500, uppercase and tracked
//   .search-results-subtitle 20 / 400  — the same as .search-result-date
//   .search-empty-state p    24 / 400
// The 20/400 could have taken NV_FT_PG_END and the 24/400 NV_FT_HERO_SIN, and
// both were refused for the reason already written beside those two: they are
// shared, and resizing one there would resize a screen nobody was looking at.
#define NV_FT_SRCH_NAME   24      // .search-result-name / .search-history-label
#define NV_FT_SRCH_META   20      // .search-results-subtitle / .search-result-date
#define NV_FT_SRCH_EMPTY  24      // .search-empty-state p
// 24 x line-height 1.2, the card's name box.
#define NV_LD_SRCH_NAME   28.8f
// 20 x Inter's `normal` (1.21), the origin line and the year.
#define NV_LD_SRCH_META   24.2f

// --- THE SOURCES SHEET -------------------------------------------------------
//
// The panel that rises over the title screen and the player to list the sources.
//
// It used to be four blocks of prose per row: the addon's name, its provider,
// two wrapped lines of its own description, a meta line, and a strip of logos.
// The three facts the eye actually wants — is it 4K, is it Dolby Vision, how big
// is it — were each stated two or three times, in two or three visual languages,
// and none of them was readable from three metres.
//
// The rebuild is the badge sheet's rule: THREE CHIPS AT MOST — resolution,
// dynamic range, source — all one height and one radius, and everything else
// drops to quiet text. Filled means 4K and nothing below it, and every chip is
// white: Dolby Vision's gold outline went when the tier word and the Instant
// mark took the row's colour budget. A row that keeps every token to one shape is
// scannable in a single fixation, which is what a list of twelve near-identical
// files needs and what wrapped prose can never give.
//
// The numbers are this sheet's own, resolved at the 1920x1080 canvas the app
// draws on. They borrow nothing from the tracks panel or the episode list: those
// are prose lists and this one is a table, and the trap of sharing a size across
// three screens is recorded three times over in the font block above.
// IT IS NOT A PANEL, AND THE WIDTH BELOW IS THE VEIL, NOT A SURFACE.
//
// What stops a panel reading as a panel is removing its EDGE. The first attempt
// at that reused the track menus' feather (GFX_MENU_FEATHER), which softens the
// leading edge of a surface that is otherwise solid — so the ramp finished at
// the content column, 56% across the screen, and the right 44% was flat black.
// That completion point IS an edge. The eye finds it exactly as fast as it finds
// a hard one, and the complaint it was meant to answer came back unchanged.
//
// So there is no surface here at all, only a ramp: 1240px of it, anchored right,
// climbing through four segments and topping out at 0.97 (GFX_SRC_VEIL carries
// the stops). Nowhere across those 1240px is it flat, which is the property that
// matters — there is no pixel you can point at and say the image ends here.
//
// THE VEIL IS WIDER THAN THE CONTENT, and it has to be: the content column is
// 804px and the ramp is 1240, so 436px of ramp reach out to the left of the
// first chip with nothing drawn on them. That stretch is the effect. Writing the
// two as one number is what produced the slab.
//
// IT ALSO HAS TO CLEAR THE TITLE SCREEN'S SYNOPSIS, and that is what sets the
// numbers rather than any proportion of the screen. The description block behind
// is NV_DETW2_X..+NV_DETW2_TEXT_W, which is 96..1136; the rows used to start at
// 1068, so the sheet's first chip began 68px BEFORE the prose ended and sat on
// top of it. The column moved right until it clears, with a breath:
//
//   veil x    = 1920 - 1060 = 860
//   content x = 1920 - 48 - 712 = 1160     content w = 712   (synopsis ends 1136)
//
// The ramp's lead-in still crosses the last stretch of that prose, and it has to:
// a ramp soft enough to have no edge needs some 600px of run, and there are only
// 784 between the end of the synopsis and the screen edge, of which the rows want
// 712. So the choice is a faint wash over the synopsis's final words or a short
// ramp with a visible edge, and the wash is the cheaper of the two — at 860 it
// reaches the prose's last character at about 0.48 instead of the 0.66 it was.
#define NV_SRC_VEIL_W     1060.0f  // the ramp, anchored to the right edge
#define NV_SRC_CONTENT_W   712.0f  // the column the rows actually occupy
#define NV_SRC_PAD          48.0f  // the content's right margin
// THE FOCUS BAND SHARES THE SHEET'S LEFT EDGE AND ITS RULE, not its curve.
//
// It began by borrowing GFX_SRC_VEIL outright, which put a pale wash out over the
// picture wherever the veil was still faint — a highlight that bleeds is worse
// than no highlight, because the eye reads the bleed as the thing rather than the
// row. Pulled back to a short 72px lead it stopped bleeding and started stopping:
// the band ended in mid-air, well inside the sheet, which is its own kind of edge.
//
// THE BAND IS THE ROW'S OWN BOX. It ends immediately left of the first chip and
// runs to the screen edge; it does not reach out into the picture at any width.
//
// The soft left end is a SHORT lead, not a dissolve into the backdrop. Making it
// follow the veil's curve, so that it faded out where the sheet did, put a pale
// wash across the open picture for hundreds of pixels — correct by one reading of
// "dissolves into the screen" and wrong by the one that matters, which is that a
// row's highlight belongs to the row.
//
// WHERE IT ENDS AND HOW LONG IT TAKES ARE TWO NUMBERS, and collapsing them into
// one is why this took so many passes. LEAD is the only thing that decides how
// far left the band reaches — 20px, just clear of the first chip. FADE is how far
// it then travels before it is at full strength, and it runs to the RIGHT, in
// over the row's own content. Lengthening the ramp therefore costs nothing at the
// left end: the band still stops exactly where it stops.
//
// 150 rather than 20 because at 20 the wash arrived all at once. It is only a
// 0.12 wash either way, so nothing here is a hard edge; the difference is whether
// the eye reads an end or a gradient, and 150 reads as a gradient.
#define NV_SRC_BAND_LEAD    20.0f   // how far left of the first chip it reaches
#define NV_SRC_BAND_FADE   150.0f   // how far it takes to reach full, rightwards
#define NV_SRC_BAND_W      (NV_SRC_BAND_LEAD + NV_SRC_CONTENT_W + NV_SRC_PAD)
// CARDS, ON TRIAL. 1 draws every row as its own rounded card and marks focus with
// a white ring, the language the home screen's cards already speak; 0 is the
// band above. The cards reuse the row's box (NV_SRC_ROW_H on NV_SRC_ROW's pitch,
// so the 16px between them is the gap) and cost no rows in view.
#define NV_SRC_CARDS         1
#define NV_SRC_CARD_PADX    32.0f   // card edge to the first chip, and to the size
#define NV_SRC_CARD_H      130.0f   // the row's 112px box plus 9px above and below
#define NV_SRC_CARD_R       18.0f
#define NV_SRC_CARD_RING     2.5f
// Unfocused cards are a faint fill and NO border: with a ring on every card the
// list became boxes of boxes, the chips' outlines inside the cards' outlines.
// EVERY CARD STANDS ON A DARK GROUND OF ITS OWN, the sheet's ink, under the white
// film. Over the veil alone the film was all a card had, and at the first chip
// the veil is only ~0.70: a lit backdrop came through, the 5% white turned it
// milky, and the unfocused rows' grey type sat on grey. The ground makes a card
// read as a dark surface wherever it lies, and the white film on top still does
// the ranking (idle < focused < playing).
#define NV_SRC_CARD_BASE    0.55f
#define NV_SRC_CARD_FILL    0.05f
#define NV_SRC_CARD_FOCUS   0.18f
// The card that is playing is the most solid of all, focused or not: it is the
// one the list is measured against, and it stays pinned under the tabs.
#define NV_SRC_CARD_PLAYING 0.23f
// The ink is THE PAGE'S OWN #0d0d0d — the colour the title screen's vignette ramps
// to on the left, behind the episode list — at the ramp's full depth. It was TRUE
// BLACK, which read as a black slab over the title rather than the same darkness the
// rest of the page is shaded with; the owner's word was "too black". What changes is
// the colour, not the depth: holding the ramp to 84% as well took the veil so far
// back that it read as gone. A navy rgb(6,7,10) and a grey rgb(6,6,6) were both tried
// before and rejected, so the ink stays neutral.
#define NV_SRC_INK_R      0.051f
#define NV_SRC_INK_G      0.051f
#define NV_SRC_INK_B      0.051f
#define NV_SRC_VEIL_A     1.0f
// The header sits 32px lower than it first did: at 40 the heading crowded the top
// edge, and the design gives the sheet a real top margin.
#define NV_SRC_TITLE_Y      72.0f
#define NV_SRC_TABS_Y      160.0f  // the tab pills' top edge
// The header's Reload/Close pill: two round buttons inside one rounded pill.
#define NV_SRC_HEAD_H       64.0f   // the pill
#define NV_SRC_HEAD_BTN     52.0f   // each button, and the white disc under focus
#define NV_SRC_HEAD_INSET    6.0f   // pill edge to a button
#define NV_SRC_HEAD_ICON    24.0f   // the glyph
// The header tooltip, under the focused icon: the detail screen's circle-button
// tooltip (bold, shadowed, no pill, 140ms fade with a 4px travel), set smaller.
#define NV_SRC_TIP_GAP      12.0f  // the header pill's bottom -> the label's top
#define NV_SRC_TIP_MS      140.0f
#define NV_SRC_TIP_RISE      4.0f
// The tabs are tabs.c's words-and-underline strip, on NV_SRC_TABS_Y.
#define NV_SRC_TOP         244.0f  // the first row's top edge
#define NV_SRC_FOOT         32.0f  // clear space below the last row
#if NV_SRC_CARDS
#define NV_SRC_ROW         148.0f  // pitch: a 130px card and an 18px gap
#else
#define NV_SRC_ROW         128.0f  // pitch: 6 rows and a half are in view
#endif
#define NV_SRC_ROW_H       112.0f  // the block the focused row fills
#define NV_SRC_CHIP_H       38.0f  // the badge sheet's one chip height
#define NV_SRC_CHIP_R       10.0f  // ... and its one radius
#define NV_SRC_CHIP_PADX    13.0f  // ink to edge, each side
// ONE GAP, EVERYWHERE ALONG THE ROW. It was briefly two — a tight one between the
// chips and a wider one before the bare text, to group the chips — and the uneven
// rhythm was more conspicuous than the grouping was useful. The hierarchy is
// already carried by shape and colour: the chips have borders and the text does
// not. It does not also need the spacing.
#define NV_SRC_CHIP_GAP     14.0f
// THE ROW CARRIES ITS OWN LEGIBILITY. The veil was asked to do this and could
// not: opaque enough to guarantee contrast meant a ramp short enough to show its
// own end. A shadow costs one draw call per line and makes the type readable on
// whatever it happens to lie over, which frees the gradient to be chosen for the
// picture alone. The chips are exempt — they have a ground of their own, and a
// shadow under the filled 4K one only muddies it.
#define NV_SRC_SHADOW        2.0f   // `text-shadow: 0 2px`, as the player's tips use
#define NV_SRC_SHADOW_A      0.75f
#define NV_SRC_CHIP_RING     2.0f  // the stroke of an OUTLINED chip
#define NV_SRC_CHIP_Y       17.0f  // the chip row, from the row's top
#define NV_SRC_META_Y       72.0f  // the availability line, from the row's top
#define NV_SRC_SIZE_COL    150.0f  // kept clear on the chip line for the size
#define NV_SRC_SIZE_Y       22.0f  // the file size, from the row's top: sat low,
                                   // so it and the quality bar read as one block
#define NV_SRC_BOLT         21.0f  // the "Instant" mark, square
// THE ROW THAT IS PLAYING gets a moving equaliser where the others carry a bolt,
// and it is the one animated thing on the sheet. It earns that: "this is the one
// you are watching" is a state, not a fact, and every static mark on the row is
// already spoken for. Three bars on three different periods, so the group never
// falls into step and never reads as a progress indicator.
#define NV_SRC_EQ_W          4.0f   // one bar
#define NV_SRC_EQ_GAP        3.0f
#define NV_SRC_EQ_H         17.0f   // the tallest a bar goes, ~ the cap height beside it
#define NV_SRC_EQ_MIN       0.30f   // and the shortest, as a fraction of it
#define NV_SRC_EQ_R          1.5f   // the bars' corner, in pixels at any height
#define NV_SRC_SEG_W        15.0f  // one segment of the quality bar
#define NV_SRC_SEG_H         4.0f
#define NV_SRC_SEG_GAP       5.0f
#define NV_SRC_SEG_N            4  // four segments, one per tier
// AN UNFOCUSED ROW IS DIMMED, and that is the whole of the focus treatment —
// there is no ring and no border. The old sheet drew a white frame around the
// focused row, which on a list where every row is a box meant the eye had to
// find a box inside boxes. Dimming the other eleven leaves exactly one row at
// full strength, and a 4K chip that is white on one row and grey on the rest
// says "this one" before any shape is read.
//
// 0.72 and not 0.50: the unfocused rows' type is ALREADY a dimmer grey (drawRow's
// `sel ?` colours), and halving it again on top left ~73/255 on a dark card —
// hard to read, not merely quieter. The focused card still has the ring and the
// brighter film, which is what actually says "this one".
#define NV_SRC_DIM         0.72f
// What reaches the screen, directly: the band carries the fill at full strength
// across the whole content column. 0.055 was the original and it was a hint
// rather than a state — the focused row was legible mostly because its type was
// brighter, which is not something that carries across a room.
#define NV_SRC_FOCUS_FILL   0.12f

#define NV_FT_SRC_TITLE   40  // "Sources"
#define NV_FT_SRC_COUNT   22  // "12 found", and the episode the sheet was opened on
#define NV_FT_SRC_TAB     22  // the provider tabs
// THE CHIPS AND THE AUDIO/CODEC RUN ARE TRACKED CAPS, as the design sets them.
// Untracked at 20px Inter packed "5.1" into something that read as "51", and the
// chips looked like buttons rather than labels. Two pixels smaller buys back the
// width the tracking costs.
#define NV_FT_SRC_CHIP    18  // 4K / DV / REMUX
#define NV_SRC_CHIP_TRACK  1.4f  // 0.08em at 18px
#define NV_FT_SRC_TEXT    18  // audio and codec, bare text after the chips
#define NV_SRC_TEXT_TRACK  0.9f  // 0.05em at 18px
#define NV_FT_SRC_META    20  // the availability line
#define NV_FT_SRC_SIZE    30  // the file size
#define NV_FT_SRC_TIER    17  // BEST / GOOD / FAIR / POOR
#define NV_SRC_TIER_TRACK  1.4f  // 0.08em at 17px

#endif
