// The drawing layer: a single shader with a rounded-rectangle SDF, able to draw
// a textured card, a soft shadow, a solid-colour rectangle and the hero with its
// gradient. One GL program avoids a state change per primitive, which is what
// costs most on this GPU.
#ifndef NV_GFX_H
#define NV_GFX_H
#include "gl_compat.h"

typedef enum {
  GFX_CARD   = 0,  // texture with corners, parallax and specular
  GFX_SHADOW = 1,  // soft shadow behind the focused card
  GFX_COLOR    = 2,  // solid-colour rectangle/pill
  GFX_HERO   = 3,  // art with a gradient into the background (takes alpha for crossfade)
  GFX_VEIL    = 4,  // dark veil at the card's base, under overlaid text
  GFX_TEXT  = 5,  // a glyph: the shape comes from the texture's alpha, not its RGB
  GFX_BACKGROUND  = 6,  // art blurred by mipmap; `focus` carries the bias
  GFX_VEIL_TOP = 7,// dark gradient from the top downwards (header bar)
  GFX_SNAP   = 8,  // a ready-made image: no SDF, no effect, just the quad
  GFX_PLAY   = 9,  // the "play" triangle, pointing right
  GFX_BLUR   = 10, // one gaussian blur pass; uPar gives the direction
  // The DETAIL screen's background: the art in "cover" already blended into the
  // web app's horizontal vignette. One mode, and not art + veil on top, because
  // those are two FULL-screen layers — on this GPU the cost is fill, and the
  // second pass alone dropped the frame rate.
  GFX_DETAIL = 11,
  // A full-screen hero. The same idea as GFX_HERO, with the ramps of the OTHER
  // state of the preference: they cover more of the screen and are deeper. Two
  // modes rather than one parameterised because the stops are recorded alongside
  // the measurements, and that is how this shader has been maintained.
  GFX_HERO_FULL = 12,
  // An outline with no fill, solid or dashed. It uses the same SDF as the other
  // modes — a ring is `abs(d) < thickness` — so it serves a rounded rectangle as
  // well as a circle (radius 0.5 = circle).
  //
  // Pass the thickness in `parx`, on the same normalised scale as `radius`, and
  // the number of dashes in `pary` (0 = a continuous ring). For example, the
  // dashed circle of "unwatched episode":
  //
  //   gfx_rect(r, 0, GFX_RING, 0, 0.06f, 12.0f, 0.5f, 1,1,1, 0.55f);
  //
  // Do not paint the middle in the background colour to fake a ring: where the
  // veil is at 0.06 the background shows through it and the plug reads as a
  // light smudge.
  GFX_RING = 13,
  // Icons for the title screen's round buttons. They exist as SDFs for the same
  // reason as GFX_PLAY: the glyphs are SVG in the web app and the embedded family
  // guarantees no symbol at all. The three buttons used to be a "+" and two
  // "...", which say nothing about what they do.
  //
  // GFX_EYE — "mark as watched". A lens (two arcs), a filled iris and, with
  // `parx > 0.5`, the diagonal stroke of the "unwatched" state.
  GFX_EYE = 14,
  // GFX_SOURCES — three stacked bars, the symbol for "source list". It took the
  // place of the YouTube glyph: this app does not play YouTube trailers (there is
  // no stream extractor), and a button that promises what it does not do is worse
  // than a button with another function. Sources are something the app CAN do.
  GFX_SOURCES = 15,
  // GFX_BRAND — a ONE-COLOUR logo: the shape comes from the texture's ALPHA and
  // the colour from uColor. It is the opposite of GFX_TEXT, which preserves the
  // texture's RGB.
  //
  // It exists for the title's logo. TMDB serves light and dark logos without
  // marking which is which, and a black logo on a dark backdrop disappears.
  // Tinting through GFX_TEXT does not work: there the texture's RGB passes
  // straight through, and it HAS to — otherwise the IMDb logo becomes a white
  // silhouette and every piece of coloured text on screen loses its colour, which
  // is already baked in by SDL_ttf.
  //
  // Use it ONLY with single-colour art. A colourful logo (the gold one, the red
  // one) goes through GFX_CARD, otherwise it becomes a flat smudge.
  GFX_BRAND = 16,
  // GFX_VEIL_BOTTOM — a PURELY VERTICAL gradient, transparent at the top and dark
  // at the base. It is the counterpart of GFX_VEIL_TOP.
  //
  // The player used GFX_VEIL here, which darkens the base AND THE LEFT. That veil
  // was made for the home's hero, where the text sits in the bottom-left corner;
  // in the player the sideways component left the rectangle's top-left corner
  // dark while the right was transparent, and the boundary between the two read
  // as a plate — the "straight rectangle" the owner pointed out.
  //
  // The curve is the smoothstep SQUARED: a plain smoothstep still leaves a
  // perceptible band where the ramp starts, because the eye sees the second
  // derivative. Squared, the start is almost flat and the transition disappears.
  GFX_VEIL_BOTTOM = 17,
  GFX_SOCIAL = 18, // static wine/coral ambient background, no texture or blur
  // A circular photo without the card SDF. The disc has its own radial mask, so
  // the edge stays even and free of burrs at any size.
  GFX_AVATAR = 19,
  // An editorial portrait: a clean photo anchored to the right, desaturated and
  // dissolved into the background. Made for people, not for 16:9 backdrops with
  // text baked in.
  GFX_PORTRAIT = 20,
  // A solid geometric disc. Used as the base of the avatar and the focus so that
  // the outline is always concentric, instead of being painted over the photo.
  GFX_DISK = 21,
  // GFX_BACKDROP — the frosted glass of the side menu: a strip of the screen
  // already blurred by gfx_backdrop, put back with the web app's filter chain on
  // it — `backdrop-filter: blur(52px) saturate(160%) brightness(0.85)` and the
  // rgba(8,12,24,0.62) plate over the result.
  //
  // The saturation is not decoration. Blurring alone pulls every pixel towards
  // the average and the strip comes out grey; the web app puts the colour back
  // with the same 160%, which is what makes the glass pick up the poster art
  // behind it instead of reading as one more dark band.
  //
  // The source texture comes from an FBO, so it is sampled UPSIDE DOWN, and uCell
  // picks the part of the strip the panel actually covers (see gfx_backdrop).
  // uPar.x carries the height of the white sheen at the top, in 0..1 of the rect.
  GFX_BACKDROP = 22,
  // GFX_PROFILE_BG — the profile picker's page, washed with the colour of the
  // profile the cursor is on. It is a MODE and not two stacked quads because the
  // web app's background is two overlapping CSS gradients and BOTH are derived
  // from the same accent colour (buildBackgroundGradient in
  // profileSelectionScreen.js), so one colour uniform describes the whole thing:
  //
  //   linear-gradient(90deg,  rgba(a,.26) 0, rgba(a,.08) 45%, transparent 72%)
  //   linear-gradient(180deg, mix(#1A1A1A,a,.30) 0, mix(#0D0D0D,a,.14) 42%, #0D0D0D)
  //
  // Two full-screen quads would also cost two full screens of fill, and gfx.c
  // records that as the thing this Mali cannot afford.
  //
  // Pass the accent in uColor.rgb. There are no parameters.
  GFX_PROFILE_BG = 23,
  // GFX_RING_CSS — a ring at an ARBITRARY radius inside the quad, antialiased on
  // both edges. It exists because GFX_RING cannot draw a CSS border.
  //
  // GFX_RING strokes `abs(d) < thickness` around the SDF's zero, and that zero is
  // the quad's INSCRIBED CIRCLE — so half the stroke always falls outside the
  // circle and the quad clips it. On a small dashed badge nobody notices. On the
  // profile picker's 256px ring it is the whole shape: MEASURED on the TV, the
  // white stopped dead at radius 128.1 with no antialiasing at 12 o'clock, and
  // ran on to 134 at 45 degrees, where the quad's CORNER leaves room. A circle
  // cut flat at four points and swollen between them.
  //
  // Here the radius is a parameter, so the caller pads the quad and the whole
  // stroke — ramp included — lands inside it:
  //   uPar.x = the ring's OUTER radius / the quad's width  (0.5 = inscribed)
  //   uPar.y = the stroke's thickness  / the quad's width
  GFX_RING_CSS = 24,
  // GFX_CW_SCRIM — the copy scrim of the Continue Watching card, clipped by the
  // card's own rounded corner. `.home-screen-shell .home-continue-media::after`,
  // a SEVEN-STOP linear-gradient(to top) in rgba(8,8,10,…):
  //
  //   0.96 at 0% · 0.90 at 12% · 0.74 at 28% · 0.48 at 46%
  //   0.22 at 64% · 0.06 at 82% · 0 at 100%
  //
  // Piecewise linear, for the reason GFX_HERO already records: a smoothstep does
  // not pass through the intermediate points, and on a ramp this long it is the
  // MIDDLE that is actually looked at — at 46% the curve gives 0.31 where the
  // sheet asks for 0.48, and the card comes out with its artwork washed out
  // exactly where the title sits.
  //
  // It replaces a flat GFX_VEIL at 0.85 over the WHOLE card, which is what made
  // the port's cards read as muddy next to the web's: the web darkens the base
  // and lets the top of the frame through untouched.
  //
  // The ink is rgba(8,8,10) and not black. At 0.96 over bright artwork the
  // difference is a hair; over the dark frames that most episode stills are, the
  // slightly blue-lifted black is what keeps the base from reading as a hole.
  GFX_CW_SCRIM = 25,
  // GFX_CW_BAR — the same card's progress bar, `.home-continue-progress`:
  // full-bleed against the bottom edge, 6px tall, a rgba(255,255,255,0.16) track
  // with a #F5F5F5 fill over it.
  //
  // It is a MODE and not two gfx_color rectangles because the bar is full-bleed
  // and the card's corner is 24px: a square rectangle across the base pokes its
  // corners 24px out past the card's outline, which the web never shows because
  // `.home-continue-media` clips it with `overflow: hidden`. Passing the CARD's
  // rect and radius here borrows the same SDF the artwork is cut with, so both
  // ends of the bar round exactly as the corner does.
  //
  //   uPar.x = the bar's height / the card's height  (the band, from the base)
  //   uPar.y = the filled fraction of the width      (0 draws the track alone)
  //   uColor.rgb = a TINT, multiplied into both halves. (1,1,1) is the bar as the
  //            Continue Watching card wears it and is what every caller passed
  //            when the colours were hardcoded. It has to be a multiply because
  //            the two halves are not one colour: the track is white at 0.16 and
  //            the fill #F5F5F5 at 1, and a replacement would flatten them into
  //            each other. A dark ink gives the same bar inverted, which is what a
  //            host that has itself gone light needs — the skip button's countdown
  //            runs while that button is focused and therefore WHITE, and a white
  //            bar on a white pill is a countdown nobody can see.
  //
  // resume.c never passes 0: a Continue Watching card that has not been started
  // draws no bar at all rather than an empty rail. The mode still supports the
  // track-only state — it is the caller's policy, not the shader's.
  GFX_CW_BAR = 26,
  // GFX_EP_SCRIM — the episode card's copy gradient, and it exists for the reason
  // GFX_CW_SCRIM does: a ramp has to be evaluated PER PIXEL.
  //
  // It was built in detail.c as a stack of rounded bands, each running from a
  // height to the card's base, because compositing N layers of alpha d gives
  // 1-(1-d)^n. That works when the ramp is gentle — the old one climbed 0.06 to
  // 0.95 across the whole 395px thumbnail and its bands were 28px apart. The
  // MEASURED gradient is flat for the first 52% and does all its work in the last
  // 190px, and there the same trick draws visible horizontal stripes across the
  // still. Widening the band count only makes the stripes narrower and the draw
  // call count higher; the answer is not more bands, it is no bands.
  //
  // Measured on `.series-episode-overlay`:
  //   linear-gradient(rgba(0,0,0,0) 52%, rgba(0,0,0,.77) 72%, rgba(0,0,0,.95))
  //
  // `v` runs from 0 at the TOP, which is how the CSS is written, so the two
  // clamps below read straight off the stylesheet and sum to 0.95 at the base.
  // One expression, no branches, clipped by the thumbnail's own corner.
  GFX_EP_SCRIM = 27,
  // GFX_RING_INSET — a border drawn ENTIRELY INSIDE the quad, at any corner
  // radius. It is `box-shadow: inset 0 0 0 Npx`, which neither ring mode could do.
  //
  // GFX_RING strokes abs(d) < thickness around the SDF's zero, and that zero IS
  // the quad's edge — so half the stroke always falls outside the quad and is
  // clipped. On a pill (radius 0.5) the outline touches the quad at twelve and six
  // o'clock, so the clipping lands exactly there and the ring comes out with flat
  // bites taken off the top and bottom. Insetting the quad by half the stroke does
  // not help: it moves the whole problem inward with it.
  //
  // GFX_RING_CSS escapes it by using its own `length(p)` — but that is a CIRCLE,
  // and this control is a pill.
  //
  // Here the band is `-thickness < d < 0`: strictly inside the edge, so there is
  // nothing to clip. Pass the thickness in `parx`, normalised to the height like
  // the radius.
  GFX_RING_INSET = 28,
  // GFX_CORNER_SCRIM — a soft dark corner, clipped by the host's own rounded
  // corner. It is the counterpart of GFX_CW_SCRIM: that one darkens the BASE of
  // the Continue Watching card so the copy can be read, this one darkens the
  // TOP-RIGHT so the time can be, and the two leave the middle of the frame alone.
  //
  // It exists to delete an object rather than add one. Every other treatment this
  // badge has had — the flat chip, the web's glass pill, a frosted plate — puts a
  // CONTAINER on the artwork, and a container reads as a control: something that
  // could be pressed. The time left is a label. With the ground under it shaded
  // instead, the type floats on the frame and the card carries one less shape.
  //
  // The falloff is RADIAL from the corner and scaled per axis, so it is a smooth
  // dome rather than a band: a linear ramp on one axis leaves a visible line where
  // it ends, which is exactly the defect the owner's reference render showed.
  //
  //   uPar.x = how far the scrim reaches along the width, in 0..1 of the host
  //   uPar.y = the same along the height
  //   uColor = the ink and, in its alpha, the depth at the corner itself
  GFX_CORNER_SCRIM = 29,
  // GFX_HERO_FIT — the hero when the backdrop is NOT stretched over the screen:
  // the whole image, at its own aspect, hung from the TOP-RIGHT corner of a band
  // that ends where the first row's cards begin (NV_HERO_FIT_H). Below and to the
  // left of it the screen is the background colour, which is the point — the
  // bottom of a full-screen backdrop is covered by the rows anyway, and trading
  // it for a smaller picture that is entirely visible shows MORE of the art, not
  // less.
  //
  // Its ramps dissolve only the two edges that do not land on a screen edge — the
  // left and the base — which is what stops the band reading as a photograph
  // pasted on a black wall. Both are PARAMETERS and not stops written into the
  // shader, because the band's size is a preference and the ramps have to stay
  // where the COPY is, not where a fraction of the band happens to fall:
  //
  //   parx  the x the horizontal ramp clears at, in 0..1 of the band
  //   pary  the y the vertical ramp starts at, in 0..1 of the band
  //
  // home.c's heroFitPar works both out from where the band lands on the screen;
  // nothing else should be calling this mode.
  GFX_HERO_FIT = 30,
  // GFX_SKELETON — a loading placeholder with the travelling highlight on it.
  //
  // The FILL is the caller's colour, and the shine is ADDED to it: every skeleton
  // in the app keeps the grey it already had (#2C2C2C card art, the hero's bars,
  // the episode card's darker plate) and gains the same light passing over it.
  // Mixing towards white instead would flatten all of them onto one tone.
  //
  // THE BAND IS POSITIONED IN SCREEN SPACE, handed over in the quad's own x:
  //
  //   uPar.x = the band's CENTRE,     in 0..1 of this quad's width (may be <0 or >1)
  //   uPar.y = the band's HALF-WIDTH, likewise
  //
  // so a block narrower than the band gets uPar.y > 0.5 and simply brightens as a
  // whole, while a 600px card gets a band that crosses it. gfx_skeleton does that
  // conversion; see the note at NV_SKEL_SHINE_W for why the sweep cannot be per
  // block.
  //
  // The tilt is corrected by uAspect, so the angle is constant on screen whatever
  // the quad's proportions — without it the band lies almost flat across a wide
  // bar and steeply across a tall card, and a row containing both reads as two
  // different lights.
  GFX_SKELETON = 31,
  // GFX_VEIL_PLAYER — the player transport's bottom scrim, with the web app's OWN
  // stops rather than a curve chosen to look about right:
  //
  //   0%  0.00   30%  0.18   58%  0.48   78%  0.74   100%  0.88
  //
  // It exists because GFX_VEIL_BOTTOM could not express that shape. That one is a
  // SQUARED smoothstep, deliberately so — the note on it explains that squaring
  // removes the visible line where a plain ramp begins. The cost is that it is
  // heavily back-loaded: at the height the scrubber sits it has only reached ~0.22
  // of its strength, against ~0.60 for the ramp above. That is the whole reason the
  // transport read as if it had no scrim behind it at all — the bar looked
  // transparent and the buttons looked like they were sitting on raw picture,
  // because very nearly they were.
  //
  // Piecewise-linear between the five stops, like the CSS. It does not need the
  // squaring trick: the first stop IS zero and the segments are shallow, so there
  // is no edge for the eye to find.
  GFX_VEIL_PLAYER = 32,
  // GFX_VEIL_POOL — a radial scrim anchored at the TOP-RIGHT corner, the web app's
  // --player-top-scrim:
  //
  //   radial-gradient(ellipse 600px 360px at 100% 0,
  //                   0.62 0%, 0.5 30%, 0.28 56%, 0.1 78%, 0 100%)
  //
  // WHY A POOL AND NOT A BAND. As a full-width band the top scrim shades the whole
  // top of the frame, and almost none of that strip has anything drawn on it — so
  // it was dimming picture for free. The corner cluster (clock, "Ends at") is all
  // it exists for, so it covers that and stops. The web app's own comment says
  // exactly this.
  //
  // The ellipse's radii are the QUAD's width and height, so the caller sizes the
  // quad to 600x360 and anchors it at the corner; the falloff is measured in that
  // quad's own normalised space, which makes the ellipse fall out of the geometry
  // instead of needing two more uniforms.
  GFX_VEIL_POOL = 33,
  // GFX_MENU_FEATHER — the right-hand menu's own surface, the web app's
  // --player-menu-surface: opaque under the text and fading to nothing across the
  // leftmost NV_TRK_FEATHER, so the panel DISSOLVES into the video instead of
  // sitting on it as a drawer with an edge.
  //
  // It is a mode for the reason GFX_EP_SCRIM is one: a ramp has to be evaluated
  // per pixel. Stacking bands is the obvious alternative and it is wrong twice
  // over — N layers of alpha d composite to 1-(1-d)^n rather than to the ramp, and
  // on this OLED the linear segments band visibly against dark video. More bands
  // only makes the stripes narrower and the draw calls costlier.
  //
  // The stops trace a smoothstep rather than a straight line. The web app's
  // comment records why, and it was measured there: a two-stop ramp puts 73% of
  // the alpha range in the first 55% of the distance, which reads as an airbrushed
  // stripe with a visible knee where the steep part ends. Eased in and out there is
  // no knee, and no step between stops exceeds ~0.2 alpha.
  //
  // It resolves to a TRUE 1, not 0.97: the last 3% let bright motion ghost faintly
  // under the text column, which is the one place the panel cannot afford it.
  //
  //   uPar.x = the feather's width as a fraction of the quad's
  //   uPar.y = 0 fades in from the LEFT (a right-hand panel); >=0.5 mirrors it, so
  //            the same mode closes the left edge of a left-to-right rail
  //   uColor = the ink, its alpha scaling the whole panel for the open/close fade
  GFX_MENU_FEATHER = 34,
  // GFX_MENU_SCRIM — the backdrop behind those menus, .player-modal-backdrop
  // .is-side-panel: rgba(0,0,0,0) -> 0.10 at 42% -> 0.26 at the right edge.
  //
  // CRITICALLY IT RAMPS THE SAME WAY THE FEATHER DOES. The web app had it running
  // the other way once — 0.34 at the left edge down to 0 at the right — while the
  // panel ramps up from ~38% across. Composited, the frame went 0.34 -> 0.19 ->
  // 0.97 left to right: it LIGHTENED for the first 720px and only then slammed
  // dark, so the brightest band in the picture was a vertical strip sitting
  // immediately left of the panel, which reads as a glow behind the panel's edge
  // rather than as a scrim. Both ramps running the same way makes the composite
  // monotonic, which is the only property the eye needs here.
  //
  // The panel's feather does the separating, so this only has to take the edge off
  // the exposed video — hence 0.26 and not the 0.88 the old full-screen sheet used.
  GFX_MENU_SCRIM = 35,
  // GFX_ERAIL_SCRIM — the episode rail's own ground, .player-episode-panel:
  //
  //   linear-gradient(180deg, rgba(8,10,13,0) 0%, 0.72 22%, 0.94 48%, 0.98 100%)
  //
  // It is the rail's whole separation from the frame — it supplies its own
  // gradient so the full-screen scrim can step back and the video stays watchable
  // above it. A flat plate here would cut the picture off on a hard line across
  // the middle of the screen, which is the one thing a bottom sheet over video
  // must not do.
  GFX_ERAIL_SCRIM = 36,
  GFX_NMODES = 37
} GfxMode;

typedef struct {
  float x, y, w, h;
} GfxRect;

// The aspect ratio (w/h) of the texture to draw. 0 = maps directly (text, veil).
// Set it BEFORE gfx_rect so the art is cropped, never stretched.
extern float gfx_tex_aspect_current;

// The sub-rectangle of the texture GFX_CARD samples, in 0..1 texture space:
// {x, y} is the offset and {w, h} the scale. The default {0,0,1,1} is the whole
// texture and is what every caller wants.
//
// It exists for ONE case: the collection tile's focus animation. The web app
// plays an mp4 there in a <video> element (`focusGifUrl`); this app cannot —
// video.c is a hardware plane BEHIND the GL surface, one instance, a bare
// rectangle, so it can neither be rounded nor clipped to a scrolling row. The
// animation is a sprite sheet instead, and this is how one cell of it is drawn.
//
// Set it BEFORE gfx_rect and PUT IT BACK afterwards, exactly like the aspect
// above — leaving it set would crop every card drawn after it to one cell.
extern GfxRect gfx_tex_cell_current;
// Group opacity: it must go back to 1 when the group ends.
extern float gfx_opacity_group;

// Snapshot: renders a whole screen to a texture, so it can be redrawn as a
// single quad. It exists because the home stays visible through the detail
// screen's frame, and redrawing the hero + ~20 cards every frame just to fill a
// 120px border dropped the app to 30fps. The home does not change while the
// detail screen is open, so keeping its image is enough.
//
// The half resolution is deliberate: the snapshot appears darkened and only at
// the edges, and nobody can tell — but the fill cost drops to a quarter.
// Scissor clip: limits the drawing to a rectangle of the screen. It serves to
// paint only the part that shows — in the detail screen the card covers the
// centre and the home behind it is seen only through the frame, so painting the
// whole screen underneath it is work thrown away.
void gfx_crop(float x, float y, float w, float h);

// --- ICONS -------------------------------------------------------------------
//
// The interface glyphs used to be DRAWN IN THE SHADER (GFX_EYE, GFX_SOURCES,
// the "+" built from two rectangles). Each was a hand-made approximation of the
// original SVG, and the owner summed the result up as: those icons look awful,
// stop inventing them, use the real SVGs.
//
// Now they are THE REAL FILES. The web app's .svg were rasterised at 128px into
// deploy/app/art/icons/ (script /tmp/svg2png.py: injects width/height, swaps
// currentColor for white and calls sips). The PNG keeps the shape in the ALPHA,
// so gfx_icon draws with GFX_BRAND and the COLOUR comes from the caller — the
// same file serves black on a white pill and white on a dark circle.
//
// `name` is the basename without the extension. It must match a file in
// deploy/app/art/icons/ EXACTLY: gfx_icon builds "<dir>/<name>.png" with no
// mapping, and a name with no file loads nothing and draws nothing, with no
// error. The set is: the PLAYER's "play", "pause", "sources", "subtitles",
// "audio", "aspect", "forward", "episodes"; the title page's "chevron_down",
// "ep_watched" and "detail_play"; the side menu's "menu_home" / "menu_library" /
// "menu_profile" / "menu_search" / "menu_settings", each with a "_fill" twin for
// the row the user is on; "brand_wordmark"; and the title screen's three circular
// buttons — "detail_library_add" / "detail_library_saved", "detail_watched" /
// "detail_watched_off" and "detail_source" — each with a "_filled" twin for the
// FOCUSED state, which is NuvioWeb's `--series-icon-focused`. The plus is the one
// exception and it is the sheet's: it has no solid form to fill into.
//
// THE FOLDER HOLDS NOTHING THAT IS NOT LOADED. "more", "watched", "unwatched" and
// "trailer" were dropped once the title screen moved to the web's files and the
// trailer button was removed; they went on being packaged and shipped to the TV
// long after the last call site for them was gone. If a name here stops being
// drawn, delete the file with the call — an unreferenced PNG in this folder is
// invisible, because gfx_icon fails silently and nothing ever reports it missing.
//
// "brand_lockup" and "brand_mark" also live here and do NOT go through gfx_icon
// (see below); "imdb_logo" is fetched by path too, for the same reason.
//
// "brand_mark" also lives there but does NOT go through gfx_icon: it is a colour
// gradient, and this function's GFX_BRAND would flatten it to one tint. Fetch its
// path with gfx_icon_path and draw it with GFX_TEXT, the only mode that keeps a
// texture's RGB and its alpha at the same time.
void gfx_icons_dir(const char *dirArt);
void gfx_icon(GfxRect r, const char *name, float cr, float cg, float cb, float ca);

// FOR AN ICON WHOSE RECT MOVES: decode at `wRequest` and draw at `r`.
//
// gfx_icon asks tex_get_exact for r.w, and tex_cache.h is explicit that an exact
// request whose width ANIMATES re-decodes as it moves. Worse than the cost is what
// the caller gets back meanwhile: a re-decode to a LARGER width leaves the entry
// PENDING with `limit > serves`, and tex_get_limit_mode answers 0 — so the icon is
// not drawn at all until the decode lands.
//
// That is what ailed the episode card's watched tick. The card scales by
// NV_DETWEB_EP_FOCUS (1.05) as it takes focus, so the 50px marker asked for 50, then
// 52.5 — past the 2px deadband — and VANISHED for the length of the focus spring,
// leaving the bare white disc it is drawn over. It read as a broken icon because it
// was a missing one.
//
// Pass the RESTING size here and let the quad do the scaling: 5% of magnification on
// a 50px glyph is invisible, and one decode serves every frame of the animation.
void gfx_icon_at(GfxRect r, const char *name, float wRequest,
                 float cr, float cg, float cb, float ca);
// The file gfx_icon would load, for the callers that need something gfx_icon does
// not do: the icon's own aspect ratio (tex_aspect), or a mode other than
// GFX_BRAND — the brand mark is a colour gradient and the tinting mode would
// flatten it. The buffer is static, so use it before calling again.
const char *gfx_icon_path(const char *name);
void gfx_no_crop(void);

int  gfx_snap_start(int w, int h);

// Blur by DOWNSCALING, in place of the mipmap. The mipmap looked like the cheap
// way out, but the art textures have no power-of-two side, and in GLES2 the
// pyramid of an NPOT texture is ill-defined — the result was a pattern of
// vertical stripes on the page's background, clearly visible against the
// reference. Here the art is drawn into a tiny target and then stretched with a
// linear filter: the blur comes out smooth and costs one read per pixel.
// `via` picks the target: 0 = the detail page, 1 = the home's background. Two
// targets because the two screens coexist — the detail covers the home, but the
// home is still drawn behind it, and a single target would make the two fight
// over the same texture every frame.
int  gfx_blur_start(int w, int h);
void gfx_blur_generate(int via, unsigned int tex, float texAspect);
void gfx_blur_draw(int via, GfxRect r, float alpha);
void gfx_blur_shutdown(void);

// --- BACKDROP ----------------------------------------------------------------
//
// A LIVE blur of what has ALREADY been drawn this frame, which is what the web
// app's `backdrop-filter` is and what gfx_blur_* is not: gfx_blur_generate blurs
// A TEXTURE the caller hands it (a piece of art), and the side menu needs the
// blur of the home BEHIND it — cards, hero and all — which exists nowhere but in
// the frame buffer.
//
// `w`/`h` fix the STRIP that is grabbed, in layout units, always anchored at the
// screen's top-left corner: the menu grows from 144 to 392 and grabbing the wider
// figure once means the texture never has to be reallocated mid-animation, with
// gfx_backdrop's own rect deciding how much of it is drawn.
//
// Returns 0 if the target could not be created; then gfx_backdrop draws nothing
// and the caller's own plate is all that shows — which is exactly the app as it
// was before the glass.
int  gfx_backdrop_start(float w, float h);
// Takes the picture. CALL IT BEFORE DRAWING ANY OF THE PANEL, veil included:
// the source is the frame buffer, so a grab taken after the panel is painted
// photographs the panel — and since the result is fed straight back into the same
// place, each grab compounds the last. It shows as ghosts of the focus pill
// hanging in the glass, and the strip drifting towards a flat wash.
//
// `now` is the app's clock: the strip is re-grabbed at most every NV_BACKDROP_MS,
// because a blur this wide cannot be told apart at 20 updates a second and the
// grab forces a tile flush on this GPU.
void gfx_backdrop_grab(unsigned now);
// Draws the last grab over `r`, tinted and filtered like the web app's plate.
void gfx_backdrop(GfxRect r, float alpha, float sheen);
void gfx_backdrop_shutdown(void);
void gfx_snap_begin(void);   // redirects the drawing into the snapshot
void gfx_snap_finish(void);  // back to the screen
void gfx_snap_draw(void);  // paints the snapshot over the whole screen
void gfx_snap_shutdown(void);

// The box the 1920x1080 layout is drawn into, in BUFFER pixels: where to put
// the viewport back after an FBO, and the scale the scissor works in.
//
// x/y exist for the letterbox. The TV surface is 16:9 and the box is the whole
// drawable, so there both are 0; a Mac window is 16:10 (and can be any shape
// the user drags), and without bars the layout would stretch — every poster
// subtly the wrong shape on the one screen used to compare against the
// reference.
void gfx_size_target(int x, int y, int w, int h);
int  gfx_start(void);
void gfx_shutdown(void);

// gfx_rect remembers the last texture it bound itself and skips repeated
// rebinds. Anything that binds or destroys a texture OUTSIDE it has to say so:
// pass the destroyed name, or 0 for "forget everything" (after an upload).
void gfx_tex_forget(GLuint tex);

// Draws a rectangle. `focus` 0..1 controls the specular/shadow; `parx/pary`
// shift the art inside the card (parallax); `radius` as a fraction of the
// smaller side.
// FRAME TELEMETRY. Zeroed by gfx_new_frame, once per frame.
//
// WHAT THESE NUMBERS HAVE ALREADY ANSWERED (measured on the TV, home scrolling,
// 1920x1080): the worst frame spent ~20ms inside app_draw and the suspicion was
// GL traversal. It was not: with 123 draws per frame, gfx_rect added up to 1.9ms
// — less than 10% of the frame. The cost was in layout/text CPU, outside this file.
//
// `gfx_fill` is what stayed useful day to day: the note at the top of gfx.c says
// TWO full-screen layers dropped this Mali to ~40fps, and without measuring the
// area, "how many full layers does this screen have" is a guess. Measured: the
// home 1.4-1.9 screens of fill; the DETAIL screen 2.2-3.5 screens, with 2 draws
// covering more than half a screen each. The detail screen is the one that runs
// near the limit.
//
// The fine-grained clocks (gfx_ms_rect, tex_ms_search) sit behind NV_PERF_FINO
// because they cost TWO clock reads per draw — around 250 calls per frame just to
// measure. Turn them on with -DNV_PERF_FINO when the question comes back.
extern int    gfx_n_rect;   // gfx_rect calls
extern int    gfx_n_progress;   // GL program changes (glUseProgram)
extern int    gfx_n_bind;   // texture changes (glBindTexture)
extern double gfx_ms_rect;  // ms of CPU inside gfx_rect
extern int    gfx_n_others;  // clip/FBO/blur calls
extern double gfx_ms_others; // ms of CPU at those GL points
extern double gfx_fill;      // area submitted this frame, in full screens
extern int    gfx_n_full;   // draws covering >= 50% of the screen
// Opens the frame: zeroes the telemetry above AND advances the skeletons' shine.
//
// `now` is the frame's clock, read once in main.c's loop. The sweep is worked out
// here, once, rather than in each gfx_skeleton call: there is ONE light crossing the
// screen, so there is one place to compute where it is. It also means a screen
// drawing a placeholder needs no clock of its own — which is what keeps the sweep
// identical on the hero, on the page below it and on another screen entirely.
void gfx_new_frame(unsigned now);

void gfx_rect(GfxRect r, GLuint tex, GfxMode mode, float focus,
              float parx, float pary, float radius,
              float cr, float cg, float cb, float ca);


// Atalhos legiveis para os casos comuns.
void gfx_color(GfxRect r, float radius, float cr, float cg, float cb, float ca);
// A LOADING PLACEHOLDER, with the shine on it. A DROP-IN for the gfx_color call
// that drew the flat block: same rect, same radius, same colour, and no clock —
// the sweep's position for this frame was set by gfx_new_frame.
//
// That is deliberate: a screen should not have to hold a clock to draw a
// placeholder. The alternative, passing `now` in, had every screen keeping its own
// copy of the frame time, which is five chances for one of them to fall a frame
// behind the others and break the single-light illusion this is built on.
void gfx_skeleton(GfxRect r, float radius,
                  float cr, float cg, float cb, float ca);
// Zeroes the rectangle's colour AND alpha, with blending off, opening the
// surface to the video plane behind it. See video.h.
void gfx_hole(GfxRect r);
void gfx_texture(GfxRect r, GLuint tex);

#endif
