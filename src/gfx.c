#include "gfx.h"
#include "tex_cache.h"
#include <SDL2/SDL.h>
#include "layout.h"
#include <stdio.h>

// One program per mode, and each one's uniforms: the locations do NOT match
// across programs, so keeping a single set would return rubbish in the second
// shader that used the same variable.
typedef struct {
  GLuint progress;
  GLint rect, screen, tex, focus, par, radius, color, aspect, texAspect, cell, aa;
  // The last uCell uploaded to THIS program. Uniforms are per-program state, so
  // the cache has to be too — and it is what keeps the sprite sheet off the hot
  // path: exactly one card on the screen is ever animating, so every other
  // GFX_CARD draw in the frame finds the default already loaded and skips the
  // call. See the note above the uniform block in gfx_rect.
  float cellLast[4];
} Program;
static Program progs[GFX_NMODES];
static int progressCurrent = -1;
// The current texture's aspect ratio, for the "cover". It is global because the
// drawing is immediate: the caller sets it before each textured rect.
float gfx_tex_aspect_current = 0.0f;
// The sub-rectangle of the texture to sample, in 0..1 texture space. The whole
// texture by default; a single cell of a sprite sheet when the collection tile
// is animating (home.c). Global for the same reason the aspect above is: the
// drawing is immediate and the caller sets it before the rect.
GfxRect gfx_tex_cell_current = {0.0f, 0.0f, 1.0f, 1.0f};
float gfx_opacity_group = 1.0f;
// The real size of the screen target (on retina, larger than 1920x1080). Kept
// here because every return from an FBO has to restore the viewport with it.
static int screenX = 0, screenY = 0;
static int screenW = (int)NV_SCREEN_W, screenH = (int)NV_SCREEN_H;
void gfx_size_target(int x, int y, int w, int h) {
  screenX = x; screenY = y; screenW = w; screenH = h;
}
// Every return to the default framebuffer goes through here. Writing the
// viewport by hand was fine while it was always the whole drawable; with the
// letterbox the origin moves too, and a single missed call puts the frame in
// the corner of the screen.
static void viewportTarget(void) {
  glViewport(screenX, screenY, screenW, screenH);
}

static GLuint snapFbo = 0, snapTex = 0;
static int snapW = 0, snapH = 0;
// Two targets: the gaussian blur is separable, so one pass writes into the second
// and the other comes back to the first.
static GLuint borderFbo[4] = {0,0,0,0}, borderTex[4] = {0,0,0,0};
static int borderW = 0, borderH = 0;

static const char *VS =
  "attribute vec2 aPos;\n"
  "uniform vec4 uRect;\n"
  "uniform vec2 uScreen;\n"
  "varying NV_UV_P vec2 vUv;\n"
  "void main(){\n"
  "  vUv = aPos;\n"
  "  vec2 p = uRect.xy + aPos * uRect.zw;\n"
  "  gl_Position = vec4(p.x/uScreen.x*2.0-1.0, 1.0-p.y/uScreen.y*2.0, 0.0, 1.0);\n"
  "}\n";

// The SDF corrects for the rect's aspect (uAspect), otherwise a landscape card's
// corner comes out oval.
// ONE PROGRAM PER MODE. This used to be a single shader with a `uniform int
// uModo` and eight branches. Even with the if being coherent across a whole
// draw, the GPU reserves registers for the shader's WORST branch, and fewer free
// registers means fewer fragments in flight at once — this TV's Mali-G71
// delivered ~40fps with only two full-screen layers. With one lean program per
// mode, each draw uses only what it needs.
static const char *FS_HEAD =
  "varying NV_UV_P vec2 vUv;\n"
  "uniform sampler2D uTex;\n"
  "uniform float uFocus;\n"
  "uniform vec2  uPar;\n"
  "uniform float uRadius;\n"
  "uniform vec4  uColor;\n"
  "uniform float uAspect;\n"
  "uniform float uAA;      // half-ramp of the edge, in the SDF's units\n"
  "uniform float uTexAsp;   // w/h of the TEXTURE; 0 = do not adjust\n"
  "uniform vec4  uCell;     // xy offset + zw scale into the texture; 0,0,1,1 = all\n";

// A rounded-rectangle SDF, corrected for the aspect ratio — without the
// correction a landscape card's corner comes out oval.
//
// vUv AND THIS FUNCTION ARE highp, AND ON A WIDE QUAD THAT IS THE DIFFERENCE
// BETWEEN A CLEAN CURVE AND A STAIRCASE.
//
// The shader's default is `precision mediump float`, which guarantees only ten
// bits of mantissa: a varying in [0,1] then carries about 1024 distinct values,
// and what that is worth in PIXELS depends entirely on how wide the quad is.
//
//   the search field   1792 wide -> ~1.75 px per representable step
//   a Discover picker   589 wide -> ~0.58 px
//   a poster            279 wide -> ~0.27 px
//
// edgeAA asks for 1.25 px of ramp in total. On the poster and the picker the
// varying resolves far finer than that and the edge ramps properly; on the
// search field the quantisation is WIDER THAN THE WHOLE RAMP, so the SDF jumps
// from one side of the edge to the other with nothing in between and the cap
// comes out visibly stepped. It was reported exactly that way: the same pill
// shape looked sharp as a dropdown and jagged as the search bar, and the only
// thing that differed between them was width.
//
// The note on edgeAA's mediump floor was circling this — "the edge starts to
// dither instead of ramping" — but tied it to uAA being small, when the real
// variable is the quad's aspect.
//
// It is the VARYING that has to change, not the arithmetic: reformulating to
// keep the intermediates small does not help, because the error is already
// baked into vUv before this function sees it. Both stages declare it the same
// way or the program does not link.
static const char *FS_SDF =
  "float sdf(NV_UV_P vec2 uv, float r, float asp){\n"
  "  NV_UV_P vec2 p = (uv - 0.5) * vec2(asp, 1.0);\n"
  "  NV_UV_P vec2 b = vec2(0.5*asp, 0.5) - r;\n"
  "  NV_UV_P vec2 q = abs(p) - b;\n"
  "  return min(max(q.x,q.y),0.0) + length(max(q,0.0)) - r;\n"
  "}\n";

// "cover": it crops the excess instead of deforming the art.
static const char *FS_COVER =
  "vec2 cover(vec2 uv){\n"
  "  if (uTexAsp <= 0.0) return uv;\n"
  "  float ra = uAspect / uTexAsp;\n"
  "  if (ra > 1.0) uv.y = (uv.y - 0.5) / ra + 0.5;\n"
  "  else          uv.x = (uv.x - 0.5) * ra + 0.5;\n"
  "  return uv;\n"
  "}\n";

static const char *FS_BODY[GFX_NMODES] = {
  // GFX_CARD — artwork with rounded corners. `object-fit: cover`, and nothing
  // else on top of it.
  //
  // THREE EFFECTS CAME OFF THIS SHADER, and they were the reason a legacy card
  // did not look like the same card in the web app:
  //
  //  1. A 3% over-scan, `(cover(vUv)-0.5)*(0.94-0.05*uFocus)+0.5`. It was the
  //     margin the parallax needed so a wobbling card never showed empty edge —
  //     but the parallax is gone (see the note in home.c) and every one of the
  //     ~25 call sites in this project passes uPar = 0. What was left was a
  //     6.4% crop of every card in the app, going to 12.4% on focus: the
  //     "legacy looks more zoomed in" the owner reported, and the reason the
  //     framing of a poster did not match the web's.
  //  2. `color *= 0.80` unfocused, back to 1.0 on focus. Every resting card in
  //     the app was 20% darker than the same image in the browser, where the
  //     modern poster carries no filter at all — checked through the whole
  //     sheet, focused and not.
  //  3. The focus decoration: a diagonal specular sweep and a white glow along
  //     the card edge. The web draws neither, and the glow in particular sat
  //     right inside the 2px focus border, which read as a smeared double edge.
  //
  // None of the three cost a focus cue anywhere: EVERY caller that passes a
  // non-zero focus already draws its own ring (home, library, search, seeall,
  // the two card rows in detail).
  "void main(){\n"
  "  float d = sdf(vUv, uRadius, uAspect);\n"
  "  float m = smoothstep(uAA,-uAA,d);\n"
  "  if (m <= 0.001) discard;\n"
  "  vec2 uv = clamp(cover(vUv) + uPar, 0.0, 1.0);\n"
  // AFTER the clamp, never before: the clamp is what stops the parallax reaching
  // past the edge, and mapping first would let a neighbouring cell of the sprite
  // sheet bleed in instead of the edge pixel repeating.
  "  uv = uCell.xy + uv * uCell.zw;\n"
  // AND THE TEXTURE'S OWN ALPHA IS KEPT, which it was not.
  //
  // This read `vec4(texture2D(...).rgb, m * uColor.a)`: it threw t.a away and
  // painted whatever RGB sat under a transparent pixel at FULL opacity. The decode
  // writes ZERO there — tex_cache.c's box filter divides the colour back out by the
  // alpha sum and deliberately "keeps the colour channels at zero" for a block that
  // is entirely transparent — so every transparent region of a cover arrived on
  // screen as OPAQUE BLACK, laid straight over the #1c1c1c->#111 surface that
  // drawArtSkeleton had just drawn underneath it.
  //
  // That is the whole of "the collection card is black in legacy and charcoal in the
  // web". Both draw the SAME coverImageUrl — the web's <img> simply composites it
  // over .content-poster's identical gradient instead of flattening it onto black.
  //
  // STRAIGHT alpha, not premultiplied: the box filter un-premultiplies (q[0] = r/a),
  // and the blend is GL_SRC_ALPHA / GL_ONE_MINUS_SRC_ALPHA. For opaque art t.a is 1,
  // so every poster, backdrop, still and logo in the app renders bit-identically to
  // before — only art that actually carries transparency changes, which is the point.
  "  vec4 t = texture2D(uTex, uv);\n"
  "  gl_FragColor = vec4(t.rgb, t.a * m * uColor.a);\n"
  "}\n",

  // GFX_SHADOW — a soft blot behind the focused item
  "void main(){\n"
  "  float d = sdf(vUv, uRadius, uAspect);\n"
  "  gl_FragColor = vec4(0.0,0.0,0.0, smoothstep(0.22,-0.03,d)*uFocus*uColor.a);\n"
  "}\n",

  // GFX_COLOR — a solid-colour rectangle/pill
  "void main(){\n"
  "  float m = smoothstep(uAA,-uAA, sdf(vUv, uRadius, uAspect));\n"
  "  if (m <= 0.001) discard;\n"
  "  gl_FragColor = vec4(uColor.rgb, uColor.a*m);\n"
  "}\n",

  // GFX_HERO — the top band's art, dissolving into the background.
  //
  // The two ramps are the web app's, MEASURED on the pseudo-elements of
  // .home-modern-hero-media (getComputedStyle, not reading the stylesheet):
  //
  //   ::before  horizontal, covering the LEFT 639px of 1421 (= 45% of the UV):
  //             #0d0d0d -> 0.86 at 22% -> 0.56 at 46% -> 0.16 at 76% -> 0
  //   ::after   vertical, full height:
  //             0 up to 82% -> 0.25 at 89.2% -> 0.65 at 95.5% -> solid at the end
  //
  // They are PIECEWISE LINEAR ramps, so the arithmetic uses clamp and not
  // smoothstep: a single smoothstep does not pass through the intermediate points
  // (at 89.2% it gave 0.35 instead of 0.25) and it is precisely the middle of the
  // ramp that you see.
  //
  // What used to be here came from the Apple app: the vertical fade started at
  // 45% of the height, almost twice as early, and the horizontal one MULTIPLIED
  // the colour (c*0.35) instead of blending into the background — which left the
  // hard edge visible instead of dissolving it.
  "void main(){\n"
  "  vec3 c = texture2D(uTex, clamp(cover(vUv), 0.0, 1.0)).rgb;\n"
  "  vec3 bg = vec3(0.051,0.051,0.051);\n"   // #0d0d0d
  "  float y = vUv.y;\n"
  "  float av = clamp((y-0.820)/0.072,0.0,1.0)*0.25\n"
  "           + clamp((y-0.892)/0.063,0.0,1.0)*0.40\n"
  "           + clamp((y-0.955)/0.045,0.0,1.0)*0.35;\n"
  "  float t = vUv.x/0.45;\n"
  "  float ah = 1.0 - clamp(t/0.22,0.0,1.0)*0.14\n"
  "                 - clamp((t-0.22)/0.24,0.0,1.0)*0.30\n"
  "                 - clamp((t-0.46)/0.30,0.0,1.0)*0.40\n"
  "                 - clamp((t-0.76)/0.24,0.0,1.0)*0.16;\n"
  "  ah *= step(vUv.x, 0.45);\n"
  "  c = mix(c, bg, clamp(ah + av - ah*av, 0.0, 1.0));\n"
  "  gl_FragColor = vec4(c, uColor.a);\n"
  "}\n",

  // GFX_VEIL — darkens the base AND the left, where the overlaid text sits
  "void main(){\n"
  "  float m = smoothstep(uAA,-uAA, sdf(vUv, uRadius, uAspect));\n"
  "  if (m <= 0.001) discard;\n"
  "  float gb = smoothstep(0.34, 1.0, vUv.y);\n"
  "  float ge = smoothstep(0.62, 0.0, vUv.x) * 0.78;\n"
  "  gl_FragColor = vec4(0.0,0.0,0.0, clamp(gb+ge-gb*ge,0.0,1.0)*uColor.a*m);\n"
  "}\n",

  // GFX_TEXT — the letter's shape comes from the texture's ALPHA, never the RGB
  "void main(){\n"
  "  vec4 g = texture2D(uTex, vUv);\n"
  "  gl_FragColor = vec4(g.rgb, g.a * uColor.a);\n"
  "}\n",

  // GFX_BACKGROUND — art blurred by mipmap (uFocus carries the bias), with the
  // gradient measured on the device: light at the top, almost black at the base,
  // plus a vignette
  "void main(){\n"
  "  // The texture already arrives blurred by the two-pass gaussian.\n"
  "  vec3 cb = texture2D(uTex, vec2(vUv.x, 1.0 - vUv.y)).rgb;\n"
  // The curve was checked against a capture of the Apple app on the same TV: its
  // base is much darker than mine was (L~39 against L~84 at 5/6 of the height)
  // and the side vignette is much deeper (L~55 at the edge against L~139 in the
  // centre, at the top). Without darkening the base, the white text of the lower
  // sections loses contrast; without the vignette the page has no centre.
  // The fall starts late: in the original the background stays light until near
  // the middle and only then darkens. Chasing the reference's ABSOLUTE brightness
  // would be a mistake — it depends on the title's art, which is different — so
  // what is copied here is the shape of the curve.
  // It starts falling earlier and from lower down: with the peak at 1.12 the
  // middle band was too bright and the grey text of unfocused cards vanished into
  // it. The background exists to give the page colour, not to compete with the text.
  "  float ky = mix(0.92, 0.05, smoothstep(0.16, 0.98, vUv.y));\n"
  "  float vg = 1.0 - 0.66 * smoothstep(0.46, 0.0, min(vUv.x, 1.0 - vUv.x));\n"
  "  gl_FragColor = vec4(cb * ky * vg, uColor.a);\n"
  "}\n",

  // GFX_VEIL_TOP — a top-to-bottom gradient, under the fixed header
  "void main(){\n"
  "  gl_FragColor = vec4(0.0,0.0,0.0, smoothstep(1.0,0.15,vUv.y)*uColor.a);\n"
  "}\n",

  // GFX_SNAP — a ready-made image: no SDF, no effect, just the quad.
  // uPar.y > 0.5 says the source is an FBO: since the render target has its origin
  // in the BOTTOM corner and the rest of the app works with y growing downwards,
  // the image comes out upside down if read directly.
  "void main(){\n"
  "  vec2 uv = (uPar.y > 0.5) ? vec2(vUv.x, 1.0 - vUv.y) : vUv;\n"
  "  gl_FragColor = vec4(texture2D(uTex, uv).rgb, uColor.a);\n"
  "}\n",

  // GFX_PLAY — a triangle pointing right. It exists as a primitive because
  // depending on the font's U+25B6 glyph is a lottery: if the embedded family does
  // not have the character, the symbol simply does not appear, and drawing a
  // rectangle in its place (which is what I had done) is worse than having nothing.
  "void main(){\n"
  "  float dy = abs(vUv.y - 0.5) * 2.0;\n"
  "  float m = smoothstep(0.02, -0.02, vUv.x - (1.0 - dy));\n"
  "  if (m <= 0.001) discard;\n"
  "  gl_FragColor = vec4(uColor.rgb, uColor.a * m);\n"
  "}\n",

  // GFX_BLUR — one pass of a 9-sample gaussian blur. uPar gives the direction and
  // the step (horizontal on one pass, vertical on the other). Splitting into two
  // passes costs 18 reads instead of the 81 of a 9x9 kernel.
  "void main(){\n"
  "  vec3 c = texture2D(uTex, vUv).rgb * 0.1633;\n"
  "  c += (texture2D(uTex, vUv + uPar).rgb        + texture2D(uTex, vUv - uPar).rgb)        * 0.1531;\n"
  "  c += (texture2D(uTex, vUv + uPar*2.0).rgb    + texture2D(uTex, vUv - uPar*2.0).rgb)    * 0.1224;\n"
  "  c += (texture2D(uTex, vUv + uPar*3.0).rgb    + texture2D(uTex, vUv - uPar*3.0).rgb)    * 0.0836;\n"
  "  c += (texture2D(uTex, vUv + uPar*4.0).rgb    + texture2D(uTex, vUv - uPar*4.0).rgb)    * 0.0477;\n"
  "  gl_FragColor = vec4(c, 1.0);\n"
  "}\n",

  // GFX_DETAIL — the title screen's backdrop, with the vignette already in.
  //
  // MEASURED in the web app (getComputedStyle on .series-detail-vignette, not
  // reading the stylesheet): a linear-gradient(90deg) from #0d0d0d to
  // transparent, with NINE stops — 0%:1.00  7.8%:0.95  17.16%:0.84  28.08%:0.70
  // 40.56%:0.52  51.48%:0.34  60.84%:0.18  70.2%:0.07  78%:0. After 78% the art
  // shows clean. As in the home's hero, these are PIECEWISE LINEAR ramps: a single
  // smoothstep misses the middle, which is exactly where the white text rests.
  //
  // The art comes in "cover" with CENTRAL anchoring, which is what the web app
  // does in practice: the rule is `background-position:100% 0`, but the backdrop
  // is 16:9 in a 16:9 frame and there is nothing left over to shift.
  //
  // uPar.x IS THE CONTENT'S ZOOM, and only the CLOSING uses it. Opening, the rect
  // itself flies out of the home hero's rect and the cover crop does the work, so
  // the zoom is 1.0 and this costs nothing. Closing, the rect stays full-bleed — a
  // rectangle shrinking back across the home puts four travelling edges on screen,
  // and hiding those was tried three ways and rejected — so the movement has to
  // happen inside a frame that never appears. Dividing the offset from the centre
  // by z > 1 samples less of the texture and magnifies what is left, so the
  // picture pulls back with no edge anywhere to pull back from.
  //
  // cover() never returns outside [0,1] and z is never below 1, so the window
  // stays inside the texture and no edge is ever smeared. 0 means "no zoom":
  // every other caller of this mode passes uPar = 0 and is untouched.
  // uCell MAPS THE QUAD ONTO THE FLIGHT'S RECTANGLE, and it is what lets the
  // opening have no edges at all.
  //
  // The backdrop used to be DRAWN at the flying rect. With the hero set to Top
  // band that rect is 80% of the screen anchored top-right, so an L of bare screen
  // — 384px down the left, 216 along the bottom — sat outside it for the whole
  // flight, and its boundary was a hard line travelling over the home.
  //
  // Now the quad is the whole viewport and the FRAMING moves instead: the standard
  // `uCell.xy + uv*uCell.zw` inverts to (uv - origin)/size, so the art lands
  // exactly where the rect would have put it while the texcoords run outside [0,1]
  // in the L — where the clamp below repeats the edge row and column into it.
  // There is no rectangle on screen to have an edge. At rest the caller passes the
  // identity cell, so the settled screen samples [0,1] and is unchanged.
  //
  // cover() has to keep using the RECT's aspect, not the quad's, or the framing
  // would not match; uCell.zw carries the inverse size, so uAspect*uCell.w/uCell.z
  // recovers it. The vignette below stays on vUv, which is now viewport space:
  // on the opening that holds the scrim still under the copy instead of sweeping
  // it 384px leftward. The CLOSING path passes the identity cell and draws at its
  // own rect, so its falloff stays anchored there — which is what hides the
  // returning card's left edge against the page ground.
  // MIRRORED, NOT CLAMPED. Edge-clamp hides a seam a few pixels wide; across the
  // 384px the cell mapping puts outside [0,1] it repeats one column of texels into
  // a streak, and detail stopping dead along a line IS an edge — the same line, in
  // the same place, moving the same way, just made of stretched image instead of
  // page ground. A fold reflects the outer band back into the picture instead, so
  // texture and gradient carry across the boundary.
  //
  // Folded HERE and not by the sampler: the coordinate is back inside [0,1] before
  // the fetch, so GL_CLAMP_TO_EDGE on the texture cannot undo it and no wrap state
  // has to be set per draw. mir() is the identity on [0,1], so the settled screen
  // samples exactly what it always did.
  "vec2 mir(vec2 u){\n"
  "  u = mod(abs(u), 2.0);\n"
  "  return mix(u, 2.0 - u, step(1.0, u));\n"
  "}\n"
  "void main(){\n"
  "  vec2 q = uCell.xy + vUv * uCell.zw;\n"
  "  float rectAsp = uAspect * (uCell.w / uCell.z);\n"
  "  vec2 uvz = q;\n"
  "  if (uTexAsp > 0.0) {\n"
  "    float ra = rectAsp / uTexAsp;\n"
  "    if (ra > 1.0) uvz.y = (uvz.y - 0.5) / ra + 0.5;\n"
  "    else          uvz.x = (uvz.x - 0.5) * ra + 0.5;\n"
  "  }\n"
  "  float z = max(uPar.x, 1.0);\n"
  "  uvz = (uvz - 0.5) / z + 0.5;\n"
  "  vec3 c = texture2D(uTex, mir(uvz)).rgb;\n"
  "  vec3 bg = vec3(0.051,0.051,0.051);\n"   // #0d0d0d
  "  float x = vUv.x;\n"
  "  float a = 1.0 - clamp(x/0.0780,0.0,1.0)*0.05\n"
  "                - clamp((x-0.0780)/0.0936,0.0,1.0)*0.11\n"
  "                - clamp((x-0.1716)/0.1092,0.0,1.0)*0.14\n"
  "                - clamp((x-0.2808)/0.1248,0.0,1.0)*0.18\n"
  "                - clamp((x-0.4056)/0.1092,0.0,1.0)*0.18\n"
  "                - clamp((x-0.5148)/0.0936,0.0,1.0)*0.16\n"
  "                - clamp((x-0.6084)/0.0936,0.0,1.0)*0.11\n"
  "                - clamp((x-0.7020)/0.0780,0.0,1.0)*0.07;\n"
  // uFocus = the vignette's STRENGTH: 1 at the top, 0 with the page scrolled. In
  // the web app the vignette is a SIBLING LAYER of the backdrop and has its own
  // opacity — on scrolling, `.detail-scrolled` takes the art to 0.15 AND the
  // vignette to 0 (components.css:17348). Here the two are merged into one mode,
  // for fill rate (see gfx.h:21-25), so the vignette's opacity has to come in as a
  // uniform. Without this it stayed at FULL strength over art already at 15%, and
  // the left-hand 78% — which is exactly where the text rests — became solid black.
  "  c = mix(c, bg, clamp(a,0.0,1.0) * uFocus);\n"
  "  gl_FragColor = vec4(c, uColor.a);\n"
  "}\n",

  // GFX_HERO_FULL — a hero filling the whole screen.
  //
  // MEASURED on the pseudo-elements of .home-modern-hero-media with
  // `modernHeroFullScreenBackdropEnabled` on (1920x1062 at 0,0):
  //
  //   ::before  horizontal, covering the LEFT 1248px of 1920 (= 65%):
  //             #0d0d0d -> 0.90 at 22% -> 0.80 at 46% -> 0.42 at 76% -> 0
  //   ::after   vertical, full height:
  //             0 up to 64% -> 0.35 at 74.8% -> 0.75 at 85.6% -> solid at the end
  //
  // The percentage stops are the SAME as the banded hero's; what changes is the
  // coverage (65% of the width instead of 45%) and the depth. That makes sense:
  // with the art filling the whole screen, the text needs more dark ground under it.
  "void main(){\n"
  "  vec3 c = texture2D(uTex, clamp(cover(vUv), 0.0, 1.0)).rgb;\n"
  "  vec3 bg = vec3(0.051,0.051,0.051);\n"
  "  float y = vUv.y;\n"
  "  float av = clamp((y-0.640)/0.108,0.0,1.0)*0.35\n"
  "           + clamp((y-0.748)/0.108,0.0,1.0)*0.40\n"
  "           + clamp((y-0.856)/0.144,0.0,1.0)*0.25;\n"
  "  float t = vUv.x/0.65;\n"
  "  float ah = 1.0 - clamp(t/0.22,0.0,1.0)*0.10\n"
  "                 - clamp((t-0.22)/0.24,0.0,1.0)*0.10\n"
  "                 - clamp((t-0.46)/0.30,0.0,1.0)*0.38\n"
  "                 - clamp((t-0.76)/0.24,0.0,1.0)*0.42;\n"
  "  ah *= step(vUv.x, 0.65);\n"
  "  c = mix(c, bg, clamp(ah + av - ah*av, 0.0, 1.0));\n"
  "  gl_FragColor = vec4(c, uColor.a);\n"
  "}\n",

  // GFX_RING — an outline, solid or dashed, with no middle.
  //
  // It exists because the title page's "unwatched episode" badge is a RING, and
  // with GFX_COLOR it came out as a grey disc. Painting the middle in the
  // background colour does not solve it: there the veil is at 0.06 and the
  // background shows through, so the "plug" would be visible as a lighter smudge.
  //
  // The existing SDF gives the signed distance to the edge; a ring is simply
  // `abs(d) < thickness`. That is why this mode costs the same as GFX_COLOR and
  // serves a rounded rectangle as well as a circle (radius 0.5 on the smaller
  // side = circle).
  //
  // Parameters, reusing uPar so as not to create a new uniform:
  //   uPar.x = the stroke's thickness, on the same normalised scale as uRadius
  //   uPar.y = the number of dashes; 0 (or <0.5) = a continuous ring
  "void main(){\n"
  "  float d = sdf(vUv, uRadius, uAspect);\n"
  "  float esp = max(uPar.x, 0.0015);\n"
  // The outer and inner edges get the same feathering, otherwise the ring comes
  // out jagged on the inside and smooth on the outside.
  "  float m = smoothstep(esp, esp*0.55, abs(d));\n"
  "  if (m <= 0.002) discard;\n"
  "  if (uPar.y > 0.5) {\n"
  "    vec2 p = (vUv - 0.5) * vec2(uAspect, 1.0);\n"
  "    float t = fract((atan(p.y, p.x) / 6.2831853 + 0.5) * uPar.y);\n"
  // A 50% cycle: half stroke, half gap, with the ends softened so the dotting
  // does not shimmer when the circle is small.
  "    m *= smoothstep(0.56, 0.44, t);\n"
  "    if (m <= 0.002) discard;\n"
  "  }\n"
  "  gl_FragColor = vec4(uColor.rgb, uColor.a * m);\n"
  "}\n",

  // The lens is the INTERSECTION of two large discs offset up and down; it is the
  // classic construction of the almond shape, and it comes out cheaper (two
  // distances) than trying two Bezier arcs. The outline is `abs(d) < esp`, as in
  // GFX_RING, and the iris is a filled disc in the centre.
  //
  // uPar.x > 0.5 adds the diagonal stroke (the "unwatched" state): a band around
  // the line y = x, with the edge faded on both sides so the stroke does not jag.
  "void main(){\n"
  "  vec2 p = (vUv - 0.5) * vec2(uAspect, 1.0);\n"
  // Centres at +-0.62 and radius 0.78: the resulting almond is about 1.0 wide by
  // 0.32 tall, which is the proportion of the web app's glyph.
  "  float d = max(length(p - vec2(0.0, 0.62)) - 0.78,\n"
  "                length(p + vec2(0.0, 0.62)) - 0.78);\n"
  // A stroke of 0.055 and not 0.038: next to a 5px "+" the thin outline made the
  // eye look like it came from another icon family. The iris grew too.
  "  float esp = 0.055;\n"
  "  float m = smoothstep(esp, esp*0.45, abs(d));\n"
  "  m = max(m, smoothstep(0.185, 0.160, length(p)));\n"
  // The stroke: it carves a groove in the eye and draws the bar inside it, so the
  // stroke reads over the lens as it does in the SVG (which uses two paths).
  // The stroke crosses the whole eye, with a groove behind it so it stands out
  // over the lens — which is what the SVG does with two paths.
  "  if (uPar.x > 0.5) {\n"
  "    float r = (p.x - p.y) * 0.7071;\n"
  "    m *= smoothstep(0.045, 0.075, abs(r));\n"
  "    float lim = step(max(abs(p.x), abs(p.y) * 1.6), 0.60);\n"
  "    m = max(m, smoothstep(0.045, 0.026, abs(r)) * lim);\n"
  "  }\n"
  "  if (m <= 0.002) discard;\n"
  "  gl_FragColor = vec4(uColor.rgb, uColor.a * m);\n"
  "}\n",

  // GFX_SOURCES — three stacked bars, the bottom one shorter: it is the symbol
  // for "source list". It replaced the YouTube glyph on the third round button:
  // the app does not play YouTube trailers, and a button that promises what it
  // does not do is worse than a button with another function.
  "void main(){\n"
  "  vec2 p = (vUv - 0.5) * vec2(uAspect, 1.0);\n"
  // Three bars 0.12 tall, centred at -0.28, 0 and +0.28. The bottom one is half
  // the width, which is what makes the symbol read as a list and not as a grid.
  "  float m = 0.0;\n"
  "  for (int i = 0; i < 3; i++) {\n"
  "    float cy = (float(i) - 1.0) * 0.28;\n"
  "    float wide = (i == 2) ? 0.24 : 0.46;\n"
  "    vec2 q = abs(p - vec2(0.0, cy)) - vec2(wide, 0.06) + 0.06;\n"
  "    float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - 0.06;\n"
  "    m = max(m, smoothstep(0.012, -0.012, d));\n"
  "  }\n"
  "  if (m <= 0.002) discard;\n"
  "  gl_FragColor = vec4(uColor.rgb, uColor.a * m);\n"
  "}\n",

  // GFX_BRAND — the shape comes from the ALPHA, the colour from uColor. See the note in gfx.h.
  "void main(){\n"
  "  float m = texture2D(uTex, vUv).a;\n"
  "  gl_FragColor = vec4(uColor.rgb, uColor.a * m);\n"
  "}\n",

  // GFX_VEIL_BOTTOM — purely vertical, transparent at the top. See the note in gfx.h.
  //
  // The rounded mask is what lets this mode shade a CARD and not just a
  // full-bleed strip: the poster placeholder is a gradient inside a 22px
  "void main(){\n"
  "  float m = smoothstep(uAA,-uAA, sdf(vUv, uRadius, uAspect));\n"
  "  if (m <= 0.001) discard;\n"
  "  float t = clamp(vUv.y, 0.0, 1.0);\n"
  "  float g = t * t * (3.0 - 2.0 * t);\n"
  "  g = g * g;\n"
  "  gl_FragColor = vec4(0.0, 0.0, 0.0, g * uColor.a * m);\n"
  "}\n",
  // GFX_SOCIAL: broad off-centre light, quiet left side for copy.
  "void main(){\n"
  "  vec2 p = vUv;\n"
  "  float glow = 1.0-smoothstep(0.0,0.95,length((p-vec2(0.88,0.18))*vec2(1.0,1.25)));\n"
  "  float ribbon = 1.0-smoothstep(0.04,0.40,abs(p.y-0.12-p.x*0.44));\n"
  "  vec3 c = mix(vec3(0.105,0.065,0.095),vec3(0.40,0.14,0.18),glow);\n"
  "  c += vec3(0.065,0.028,0.020)*ribbon*glow;\n"
  "  c = mix(c,vec3(0.047,0.045,0.055),smoothstep(0.44,1.0,p.y));\n"
  "  gl_FragColor = vec4(c,uColor.a);\n"
  "}\n",

  // GFX_AVATAR: an exact radial mask. GFX_CARD uses the rounded-rectangle SDF and
  // parallax over-scan; on a small circle that left the edge irregular and shifted
  // the photograph inside the disc.
  "void main(){\n"
  "  vec2 p=(vUv-0.5)*vec2(uAspect,1.0);\n"
  "  float d=length(p);\n"
  "  float m=smoothstep(0.500,0.486,d);\n"
  "  if(m<=0.001) discard;\n"
  "  vec4 t=texture2D(uTex,clamp(cover(vUv),0.0,1.0));\n"
  // THE ART'S OWN ALPHA, which this mode used to throw away — it took the .rgb
  // and lit every texel opaque. On a photograph that is invisible: tex_cache
  // converts everything to ABGR8888, so a JPEG arrives with alpha 255 and this
  // multiply changes nothing. On a PNG WITH A TRANSPARENT GROUND it is the whole
  // picture: the transparent texels carry rgb (0,0,0) and were painted as a
  // BLACK DISC. Seen on the profile picker, where the web app puts the profile's
  // colour behind the avatar precisely so it shows through the cut-out, and the
  // native circle came out black instead of red.
  "  gl_FragColor=vec4(t.rgb,m*t.a*uColor.a);\n"
  "}\n",

  // GFX_PORTRAIT: it preserves the profile still's vertical framing and anchors it
  // to the right. Outside the photograph the shader stays transparent, letting the
  // base hero show through without the seam of a second panel.
  "void main(){\n"
  // The pipeline may deliver JPEG/RGB or PNG with real alpha. We do not try to
  // guess the background by luminance: dark hair and clothing are valid pixels too
  // and a heuristic chroma-key would erase them. With no matte, the fallback is
  // the whole photo with a safe edge dissolve; with alpha, the silhouette supplied
  // by the source stays intact.
  // An editorial zoom: the reference does not show the whole portrait; it shows the
  // head filling the hero and running off the right edge. The vertical crop
  // enlarges the face without stretching the texture.
  "  float cropY=0.05;\n"
  "  float cropH=0.78;\n"
  "  float dispW=clamp((uTexAsp/uAspect)/cropH,0.46,0.90);\n"
  "  float x0=1.0-dispW;\n"
  "  float localX=clamp((vUv.x-x0)/dispW,0.0,1.0);\n"
  "  vec2 uv=vec2(localX,cropY+vUv.y*cropH);\n"
  "  float inside=step(x0,vUv.x)*step(vUv.x,1.0);\n"
  "  vec4 pix=texture2D(uTex,clamp(uv,0.0,1.0));\n"
  "  vec3 c=pix.rgb;\n"
  // A wide dissolve on all four edges: the portrait blends into the banner instead
  // of giving away a grey rectangle. The centre stays whole so the face keeps its
  // detail and contrast.
  "  float left=smoothstep(0.0,0.28,localX);\n"
  "  float right=1.0-smoothstep(0.82,1.0,localX);\n"
  "  float top=smoothstep(0.0,0.12,vUv.y);\n"
  "  float bottom=1.0-smoothstep(0.68,0.99,vUv.y);\n"
  "  float mask=inside*left*right*top*bottom*pix.a;\n"
  "  if(mask<=0.001) discard;\n"
  "  gl_FragColor=vec4(c,uColor.a*mask);\n"
  "}\n",

  // GFX_DISK: a circular fill with antialiasing. Sitting behind the avatar it
  // produces a perfect rim without hiding pixels of the image or creating burrs.
  "void main(){\n"
  "  vec2 p=(vUv-0.5)*vec2(uAspect,1.0);\n"
  "  float m=smoothstep(0.500,0.486,length(p));\n"
  "  if(m<=0.001) discard;\n"
  "  gl_FragColor=vec4(uColor.rgb,uColor.a*m);\n"
  "}\n",

  // GFX_BACKDROP — the side menu's frosted glass. The strip arrives already
  // blurred (gfx_backdrop ran the separable gaussian over it); what is left here
  // is the rest of the web app's chain, in the order the browser applies it.
  //
  // MEASURED from .home-sidebar::before: backdrop-filter is
  // `blur(52px) saturate(160%) brightness(0.85)` and the background over it is
  // `linear-gradient(180deg, rgba(255,255,255,0.06) 0%, transparent 18%)` on the
  // top 100px, over a flat rgba(8,12,24,0.62).
  //
  // The source is an FBO, hence the vertical flip, and uCell narrows the strip to
  // the width the panel currently has.
  "void main(){\n"
  "  vec2 uv = uCell.xy + vec2(vUv.x, 1.0 - vUv.y) * uCell.zw;\n"
  "  vec3 c = texture2D(uTex, uv).rgb;\n"
  "  float l = dot(c, vec3(0.2126, 0.7152, 0.0722));\n"
  "  c = clamp(mix(vec3(l), c, 1.60), 0.0, 1.0) * 0.85;\n"
  "  c = mix(c, vec3(0.0314, 0.0471, 0.0941), 0.62);\n"
  // The sheen is ADDED, not mixed: it is a white glaze at 6%, and mixing towards
  // white at that strength washes the colour the saturation has just restored.
  "  c += 0.06 * (1.0 - smoothstep(0.0, max(uPar.x, 0.0001), vUv.y));\n"
  "  gl_FragColor = vec4(c, uColor.a);\n"
  "}\n",

  // GFX_PROFILE_BG — the profile picker's page. See the note in gfx.h: two CSS
  // gradients, both derived from the accent in uColor.rgb.
  //
  // The stops are the web app's, in the order the browser composites them: the
  // vertical one IS the page, and the horizontal accent is painted over it. That
  // order is what puts the strongest wash on the LEFT, where the first card sits.
  //
  // #0D0D0D and #1A1A1A are --bg-color and --bg-elevated. They are written in as
  // literals rather than passed in because getBackgroundThemeColors() reads them
  // from :root once and this app has one theme.
  //
  // Branchless on purpose: `mix` with a `step` costs the same everywhere, and a
  // divergent branch across a full-screen quad does not.
  "void main(){\n"
  "  vec3 a = uColor.rgb;\n"
  "  vec3 bg = vec3(0.05098);\n"
  "  vec3 el = vec3(0.10196);\n"
  "  vec3 hi = mix(el, a, 0.30);\n"
  "  vec3 md = mix(bg, a, 0.14);\n"
  "  float y = clamp(vUv.y, 0.0, 1.0);\n"
  "  vec3 c = mix(mix(hi, md, clamp(y / 0.42, 0.0, 1.0)),\n"
  "               mix(md, bg, clamp((y - 0.42) / 0.58, 0.0, 1.0)),\n"
  "               step(0.42, y));\n"
  "  float x = clamp(vUv.x, 0.0, 1.0);\n"
  "  float w = mix(mix(0.26, 0.08, clamp(x / 0.45, 0.0, 1.0)),\n"
  "                mix(0.08, 0.00, clamp((x - 0.45) / 0.27, 0.0, 1.0)),\n"
  "                step(0.45, x));\n"
  "  gl_FragColor = vec4(mix(c, a, w), uColor.a);\n"
  "}\n",

  // GFX_RING_CSS — a border at a given radius, ramped on BOTH edges. See gfx.h
  // for why GFX_RING could not do this.
  //
  // Its own `length(p)`, not the shared sdf(): that one measures from the rect's
  // rounded edge and its zero is pinned to the inscribed circle, which is exactly
  // the constraint being escaped here.
  "void main(){\n"
  "  vec2 p = (vUv - 0.5) * vec2(uAspect, 1.0);\n"
  "  float d = length(p);\n"
  "  float ro = uPar.x;\n"
  "  float ri = max(uPar.x - uPar.y, 0.0);\n"
  // Two ramps multiplied: the outer falls off going out, the inner going in. On a
  // stroke thinner than two ramps they overlap and the ring simply comes out
  // fainter, which is the right answer for a hairline.
  "  float m = smoothstep(ro + uAA, ro - uAA, d) * smoothstep(ri - uAA, ri + uAA, d);\n"
  "  if (m <= 0.002) discard;\n"
  "  gl_FragColor = vec4(uColor.rgb, uColor.a * m);\n"
  "}\n",

  // GFX_CW_SCRIM — the Continue Watching card's copy scrim. See gfx.h for the
  // seven stops and for why they are not a smoothstep.
  //
  // `u` runs from 0 at the BASE to 1 at the top, which is the direction the CSS
  // `to top` gradient is written in, so the stops below can be read straight off
  // the stylesheet. Each clamp() is one segment's contribution, subtracted from
  // the 0.96 the base starts at; they sum to exactly 0.96, so the top of the card
  // is untouched and the whole ramp is one expression with no branches.
  "void main(){\n"
  "  float m = smoothstep(uAA,-uAA, sdf(vUv, uRadius, uAspect));\n"
  "  if (m <= 0.001) discard;\n"
  "  float u = clamp(1.0 - vUv.y, 0.0, 1.0);\n"
  "  float a = 0.96\n"
  "          - clamp( u        / 0.12, 0.0, 1.0) * 0.06\n"
  "          - clamp((u - 0.12)/ 0.16, 0.0, 1.0) * 0.16\n"
  "          - clamp((u - 0.28)/ 0.18, 0.0, 1.0) * 0.26\n"
  "          - clamp((u - 0.46)/ 0.18, 0.0, 1.0) * 0.26\n"
  "          - clamp((u - 0.64)/ 0.18, 0.0, 1.0) * 0.16\n"
  "          - clamp((u - 0.82)/ 0.18, 0.0, 1.0) * 0.06;\n"
  "  gl_FragColor = vec4(0.0314, 0.0314, 0.0392, a * uColor.a * m);\n"
  "}\n",

  // GFX_CW_BAR — the same card's progress bar, cut by the card's own corner.
  //
  // The quad IS the card, and everything above the band is discarded: that is
  // what buys the rounded ends, and it costs nothing, because a discarded
  // fragment never reaches the blender.
  //
  // The track/fill boundary is a `step` and not a smoothstep on purpose — CSS
  // puts a hard edge at the end of the span, and a ramp there reads as the bar
  // being out of focus.
  "void main(){\n"
  "  float m = smoothstep(uAA,-uAA, sdf(vUv, uRadius, uAspect));\n"
  "  if (m <= 0.001) discard;\n"
  "  if (vUv.y < 1.0 - uPar.x) discard;\n"
  "  float fill = step(vUv.x, uPar.y);\n"
  "  vec3  c = mix(vec3(1.0), vec3(0.9608), fill);\n"
  "  float a = mix(0.16, 1.0, fill);\n"
  // TINTED BY uColor.rgb, which is a multiply and not a replacement: (1,1,1) is
  // the Continue Watching card's own bar, unchanged, and every existing caller
  // passes exactly that. A dark ink instead gives the same bar inverted, for a
  // host that has itself gone light — see the skip button's countdown.
  "  gl_FragColor = vec4(c * uColor.rgb, a * uColor.a * m);\n"
  "}\n",

  // GFX_EP_SCRIM — the episode card's copy gradient. See gfx.h for why it is a
  // shader and not the stack of bands detail.c used to build.
  //
  // `v` runs from 0 at the TOP, the direction the CSS is written in, so the stops
  // are the measured ones unchanged: nothing until 52%, 0.77 by 72%, 0.95 at the
  // base. The two clamps are one segment each and sum to 0.95.
  "void main(){\n"
  "  float m = smoothstep(uAA,-uAA, sdf(vUv, uRadius, uAspect));\n"
  "  if (m <= 0.001) discard;\n"
  "  float v = clamp(vUv.y, 0.0, 1.0);\n"
  "  float a = clamp((v - 0.52) / 0.20, 0.0, 1.0) * 0.77\n"
  "          + clamp((v - 0.72) / 0.28, 0.0, 1.0) * 0.18;\n"
  "  if (a <= 0.002) discard;\n"
  "  gl_FragColor = vec4(0.0, 0.0, 0.0, a * uColor.a * m);\n"
  "}\n",

  // GFX_RING_INSET — a border strictly inside the quad's edge, at any radius.
  //
  // The band is between the edge (d = 0) and `thickness` inside it, so unlike
  // GFX_RING no part of the stroke lies outside the quad and there is nothing for
  // the quad to clip. Both edges are ramped: the outer against the shape itself,
  // the inner against the offset contour.
  "void main(){\n"
  "  float d = sdf(vUv, uRadius, uAspect);\n"
  "  float t = uPar.x;\n"
  "  float m = smoothstep(uAA,-uAA, d) * smoothstep(-t - uAA, -t + uAA, d);\n"
  "  if (m <= 0.002) discard;\n"
  "  gl_FragColor = vec4(uColor.rgb, uColor.a * m);\n"
  "}\n",

  // GFX_CORNER_SCRIM — a soft dark corner. See gfx.h for what it is for.
  //
  // `q` is the distance from the TOP-RIGHT corner with each axis divided by how
  // far the scrim is meant to reach on that axis, so one length() gives an ellipse
  // rather than a circle and the two extents can differ.
  //
  // The first THIRD of the dome is FLAT — the ramp starts at 0.35, not at 0. The
  // type sits in exactly that region, and a falloff that starts falling at the
  // corner itself is well down its depth by the time it reaches the label's far
  // end: MEASURED on the capture, the ground under "13m left" went from L50 at the
  // right of the line to L89 at its left, and the first word was visibly thinner on
  // the ground than the last. Widening the whole dome would fix that too, at the
  // price of shading more of the frame; moving the plateau out costs nothing.
  "void main(){\n"
  "  float m = smoothstep(uAA,-uAA, sdf(vUv, uRadius, uAspect));\n"
  "  if (m <= 0.001) discard;\n"
  "  vec2 q = vec2((1.0 - vUv.x) / max(uPar.x, 0.001),\n"
  "                vUv.y        / max(uPar.y, 0.001));\n"
  "  float a = uColor.a * (1.0 - smoothstep(0.35, 1.0, length(q)));\n"
  "  if (a <= 0.002) discard;\n"
  "  gl_FragColor = vec4(uColor.rgb, a * m);\n"
  "}\n",

  // GFX_HERO_FIT — the whole backdrop in a band at the top-right, dissolved into
  // the background on the two edges that are not a screen edge.
  //
  // THE TWO RAMPS ARE PARAMETERS, and that is the difference from GFX_HERO and
  // GFX_HERO_FULL. Those two draw a rectangle the layout fixes, so their stops are
  // written in as fractions of it; this band's size is a PREFERENCE, and a stop
  // fixed at a fraction of the band would slide across the copy as the size moved.
  // The caller works out where the ramps have to be on the SCREEN and hands them
  // over in the band's own 0..1 (heroFitPar in home.c):
  //
  //   uPar.x  the x the horizontal ramp clears at — the copy's right edge plus air
  //   uPar.y  the y the vertical one starts at    — the first row's title
  //
  // The horizontal SHAPE is GFX_HERO_FULL's, stop for stop (0.12/0.12/0.38/0.38
  // over 0.22/0.24/0.30/0.24), because it is making the same thing: ground for the
  // same copy, in the same typeface, at the same size. Only its width moves.
  //
  // The vertical one is written as fractions of what is LEFT below uPar.y, so the
  // ramp always lands exactly on the band's base whatever the start.
  //
  // Both are piecewise linear, for the reason GFX_HERO records: a smoothstep does
  // not pass through the intermediate points, and the middle of the ramp is
  // exactly what the eye reads.
  //
  // The two are combined with `a + b - a*b` and not by adding: at the bottom-left
  // CORNER both are near 1 and a sum overshoots, which shows up as a hard step
  // where the two ramps meet.
  "void main(){\n"
  "  vec3 c = texture2D(uTex, clamp(cover(vUv), 0.0, 1.0)).rgb;\n"
  "  vec3 bg = vec3(0.051,0.051,0.051);\n"   // #0d0d0d
  "  float s = clamp(uPar.y, 0.05, 0.98);\n"
  "  float d = 1.0 - s;\n"
  "  float y = vUv.y;\n"
  "  float av = clamp((y-s)/(d*0.34),0.0,1.0)*0.30\n"
  "           + clamp((y-(s+d*0.34))/(d*0.30),0.0,1.0)*0.40\n"
  "           + clamp((y-(s+d*0.64))/(d*0.36),0.0,1.0)*0.30;\n"
  "  float cx = max(uPar.x, 0.02);\n"
  "  float t = vUv.x/cx;\n"
  "  float ah = 1.0 - clamp(t/0.22,0.0,1.0)*0.12\n"
  "                 - clamp((t-0.22)/0.24,0.0,1.0)*0.12\n"
  "                 - clamp((t-0.46)/0.30,0.0,1.0)*0.38\n"
  "                 - clamp((t-0.76)/0.24,0.0,1.0)*0.38;\n"
  "  ah *= step(vUv.x, cx);\n"
  "  c = mix(c, bg, clamp(ah + av - ah*av, 0.0, 1.0));\n"
  "  gl_FragColor = vec4(c, uColor.a);\n"
  "}\n",

  // GFX_SKELETON — the loading block, with the light passing over it.
  //
  // THE NUMBERS LIVE HERE and not in layout.h, because nothing outside this
  // shader uses them: 0.075 of added light and a tilt of 0.36 (tan 20 degrees).
  // What layout.h keeps is the band's WIDTH and the clock, which the caller needs
  // to place the band at all (NV_SKEL_SHINE_W).
  //
  // `u` is the fragment's position along the band's axis. The tilt term converts
  // an offset in the quad's y into the quad's x AT A CONSTANT SCREEN ANGLE —
  // dy_px = (vUv.y-0.5)*h, dx_px = dy_px*tan, and dx in the quad's own x is
  // dx_px/w = (vUv.y-0.5)*tan/uAspect, since uAspect is w/h. Without that
  // division the same tilt lies almost flat across a 420x22 bar and steeply
  // across a 600x395 card.
  //
  // The falloff is a SQUARED smoothstep, for the reason GFX_VEIL_BOTTOM records:
  // a plain smoothstep still leaves a perceptible line where the ramp begins,
  // because the eye reads the second derivative. Squared, the band has no edge at
  // all — it is a light, and a light with a rim is a shape.
  //
  // The shine is ADDED to uColor.rgb rather than mixed towards white: every
  // skeleton in the app keeps its own grey and catches the same light. It is not
  // clamped, because no caller passes a grey anywhere near 1 — these are all
  // placeholders around a luma of 0.2.
  "void main(){\n"
  "  float m = smoothstep(uAA,-uAA, sdf(vUv, uRadius, uAspect));\n"
  "  if (m <= 0.001) discard;\n"
  "  float u = vUv.x + (vUv.y - 0.5) * 0.36 / max(uAspect, 0.001);\n"
  "  float t = abs(u - uPar.x) / max(uPar.y, 0.001);\n"
  "  float s = 1.0 - smoothstep(0.0, 1.0, t);\n"
  "  gl_FragColor = vec4(uColor.rgb + s * s * 0.075, uColor.a * m);\n"
  "}\n",

  // GFX_VEIL_PLAYER — the transport scrim, piecewise-linear through the web app's
  // five stops. See the note in gfx.h for why GFX_VEIL_BOTTOM could not be reused.
  //
  // uColor.a scales the whole ramp, so the caller still fades it with the controls.
  "void main(){\n"
  "  float t = clamp(vUv.y, 0.0, 1.0);\n"
  "  float g;\n"
  "  if (t < 0.30)      g = mix(0.00, 0.18, t / 0.30);\n"
  "  else if (t < 0.58) g = mix(0.18, 0.48, (t - 0.30) / 0.28);\n"
  "  else if (t < 0.78) g = mix(0.48, 0.74, (t - 0.58) / 0.20);\n"
  "  else               g = mix(0.74, 0.88, (t - 0.78) / 0.22);\n"
  "  gl_FragColor = vec4(0.0, 0.0, 0.0, g * uColor.a);\n"
  "}\n",

  // GFX_VEIL_POOL — an elliptical pool hung from the quad's TOP-RIGHT corner.
  //
  // The ellipse's radii ARE the quad's two sides, so `d` is already the normalised
  // distance the CSS measures its stops along: 0 at the corner, 1 where the
  // gradient reaches zero. Anything past 1 is outside the pool and discarded, which
  // is what keeps the rest of the top of the frame completely untouched.
  "void main(){\n"
  "  vec2 p = vec2(1.0 - vUv.x, vUv.y);\n"
  "  float d = length(p);\n"
  "  if (d >= 1.0) discard;\n"
  "  float g;\n"
  "  if (d < 0.30)      g = mix(0.62, 0.50, d / 0.30);\n"
  "  else if (d < 0.56) g = mix(0.50, 0.28, (d - 0.30) / 0.26);\n"
  "  else if (d < 0.78) g = mix(0.28, 0.10, (d - 0.56) / 0.22);\n"
  "  else               g = mix(0.10, 0.00, (d - 0.78) / 0.22);\n"
  "  gl_FragColor = vec4(0.0, 0.0, 0.0, g * uColor.a);\n"
  "}\n",

  // GFX_MENU_FEATHER — the right-hand menu's surface. See gfx.h for why the stops
  // ease rather than run straight, and why this is a mode and not a stack of bands.
  //
  // `t` is the distance across the feather, 0 at the panel's left edge and 1 where
  // the ink reaches full strength; everything to the right of that is solid. The
  // seven stops are the stylesheet's, at their own fractions of the feather.
  "void main(){\n"
  "  float f = max(uPar.x, 0.001);\n"
  "  float u = uPar.y >= 0.5 ? 1.0 - vUv.x : vUv.x;\n"
  "  float t = clamp(u / f, 0.0, 1.0);\n"
  "  float g;\n"
  "  if (t < 0.15)      g = mix(0.00, 0.03, t / 0.15);\n"
  "  else if (t < 0.30) g = mix(0.03, 0.13, (t - 0.15) / 0.15);\n"
  "  else if (t < 0.45) g = mix(0.13, 0.33, (t - 0.30) / 0.15);\n"
  "  else if (t < 0.60) g = mix(0.33, 0.58, (t - 0.45) / 0.15);\n"
  "  else if (t < 0.75) g = mix(0.58, 0.79, (t - 0.60) / 0.15);\n"
  "  else if (t < 0.88) g = mix(0.79, 0.93, (t - 0.75) / 0.13);\n"
  "  else               g = mix(0.93, 1.00, (t - 0.88) / 0.12);\n"
  "  gl_FragColor = vec4(uColor.rgb, g * uColor.a);\n"
  "}\n",

  // GFX_MENU_SCRIM — the backdrop behind them, ramping the SAME WAY the feather
  // does. Two linear segments is all the CSS has, and at 0.26 over a whole screen
  // there is no steep part for a knee to show in.
  "void main(){\n"
  "  float t = clamp(vUv.x, 0.0, 1.0);\n"
  "  float g = t < 0.42 ? mix(0.00, 0.10, t / 0.42)\n"
  "                     : mix(0.10, 0.26, (t - 0.42) / 0.58);\n"
  "  gl_FragColor = vec4(0.0, 0.0, 0.0, g * uColor.a);\n"
  "}\n",

  // GFX_ERAIL_SCRIM — the episode rail's ground, piecewise-linear through the web
  // app's four stops. The ink is rgba(8,10,13), carried in uColor so the caller
  // fades the whole sheet with it on open and close.
  "void main(){\n"
  "  float t = clamp(vUv.y, 0.0, 1.0);\n"
  "  float g;\n"
  "  if (t < 0.22)      g = mix(0.00, 0.72, t / 0.22);\n"
  "  else if (t < 0.48) g = mix(0.72, 0.94, (t - 0.22) / 0.26);\n"
  "  else               g = mix(0.94, 0.98, (t - 0.48) / 0.52);\n"
  "  gl_FragColor = vec4(uColor.rgb, g * uColor.a);\n"
  "}\n",
};

// Each body declares what it uses; assembling only what is needed keeps the
// shader lean.
static const struct { int sdf, cover; } NEEDS[GFX_NMODES] = {
  {1,1}, {1,0}, {1,0}, {0,1}, {1,0}, {0,0}, {0,0}, {0,0}, {0,0}, {0,0}, {0,0},
  {0,1}, {0,1},
  {1,0},   /* GFX_RING */
  {0,0},   /* GFX_EYE     — its own SDF, not the rectangle's */
  {0,0},   /* GFX_SOURCES — likewise */
  {0,0},   /* GFX_BRAND   — only the texture's alpha: no SDF, no cover */
  {1,0},   /* GFX_VEIL_BOTTOM — a vertical gradient, clipped by the radius */
  {0,0},   /* GFX_SOCIAL */
  {0,1},   /* GFX_AVATAR */
  {0,0},   /* GFX_PORTRAIT */
  {0,0},   /* GFX_DISK */
  {0,0},   /* GFX_BACKDROP — the strip is a flat quad: no SDF, no cover */
  {0,0},   /* GFX_PROFILE_BG — a full-screen wash: no SDF, no texture */
  {0,0},   /* GFX_RING_CSS — its own radial distance, not the rect SDF */
  {1,0},   /* GFX_CW_SCRIM — a vertical ramp, clipped by the card's corner */
  {1,0},   /* GFX_CW_BAR   — the same corner, cutting the bar's ends */
  {1,0},   /* GFX_EP_SCRIM — a vertical ramp, clipped by the thumbnail's corner */
  {1,0},   /* GFX_RING_INSET — the rect's own SDF, offset inward */
  {1,0},   /* GFX_CORNER_SCRIM — a dome, clipped by the host's own corner */
  {0,1},   /* GFX_HERO_FIT — the art in cover; the band is the quad, no SDF */
  {1,0},   /* GFX_SKELETON — the block's own SDF; the shine is untextured */
  {0,0},   /* GFX_VEIL_PLAYER — a full-bleed vertical ramp: no SDF, no texture */
  {0,0},   /* GFX_VEIL_POOL   — its own radial distance, not the rect SDF */
  {0,0},   /* GFX_MENU_FEATHER — a horizontal ramp over a square-cornered panel */
  {0,0},   /* GFX_MENU_SCRIM   — likewise, full-bleed behind it */
  {0,0}    /* GFX_ERAIL_SCRIM  — a full-bleed vertical ramp: no SDF, no texture */
};

static GLuint compiles(GLenum kind, const char *src) {
  GLuint s = glCreateShader(kind);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) { char log[700]; glGetShaderInfoLog(s, 700, NULL, log); printf("gfx shader: %s\n", log); }
  return s;
}

// WHICH PRECISION THE SDF'S COORDINATE GETS, decided once and given to BOTH
// stages.
//
// It cannot be decided inside the shader. GL_FRAGMENT_PRECISION_HIGH is defined
// by the compiler only in the FRAGMENT stage, so a `#ifdef` would resolve one
// way in the vertex shader and the other in the fragment shader — and a varying
// whose precision disagrees between the two is exactly the kind of thing that
// links on one driver and fails on the next.
//
// glGetShaderPrecisionFormat answers the same question from the C side, where
// the answer can be handed to both. `precision` comes back as the number of
// mantissa bits and is 0 when the format is not supported at all.
static void uvPrecision(char *dst, size_t n) {
#ifdef __APPLE__
  // Desktop GLSL 1.20 has no precision qualifiers and every float is single
  // precision anyway, so the marker expands to nothing.
  snprintf(dst, n, "#define NV_UV_P\n");
#else
  GLint range[2] = { 0, 0 }, bits = 0;
  glGetShaderPrecisionFormat(GL_FRAGMENT_SHADER, GL_HIGH_FLOAT, range, &bits);
  snprintf(dst, n, "#define NV_UV_P %s\n", bits > 0 ? "highp" : "mediump");
  printf("[gfx] SDF coordinate precision: %s (highp mantissa bits %d)\n",
         bits > 0 ? "highp" : "mediump", bits);
#endif
}

int gfx_start(void) {
  char uvp[64];
  char vsrc[1200];
  uvPrecision(uvp, sizeof uvp);
  snprintf(vsrc, sizeof vsrc, "%s%s%s", NV_GLSL_PREFIX, uvp, VS);
  GLuint vs = compiles(GL_VERTEX_SHADER, vsrc);
  char source[6000];
  for (int m = 0; m < GFX_NMODES; m++) {
    snprintf(source, sizeof source, "%s%s%s%s%s%s", NV_GLSL_PREFIX, uvp, FS_HEAD,
             NEEDS[m].sdf ? FS_SDF : "", NEEDS[m].cover ? FS_COVER : "",
             FS_BODY[m]);
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, compiles(GL_FRAGMENT_SHADER, source));
    glBindAttribLocation(p, 0, "aPos");
    glLinkProgram(p);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[700]; glGetProgramInfoLog(p, 700, NULL, log);
               printf("gfx link mode %d: %s\n", m, log); return 0; }
    progs[m].progress = p;
    progs[m].rect = glGetUniformLocation(p, "uRect");
    progs[m].screen = glGetUniformLocation(p, "uScreen");
    progs[m].tex  = glGetUniformLocation(p, "uTex");
    progs[m].focus = glGetUniformLocation(p, "uFocus");
    progs[m].par  = glGetUniformLocation(p, "uPar");
    progs[m].radius = glGetUniformLocation(p, "uRadius");
    progs[m].aa     = glGetUniformLocation(p, "uAA");
    progs[m].color  = glGetUniformLocation(p, "uColor");
    progs[m].aspect  = glGetUniformLocation(p, "uAspect");
    progs[m].texAspect = glGetUniformLocation(p, "uTexAsp");
    progs[m].cell      = glGetUniformLocation(p, "uCell");
    progs[m].cellLast[0] = 0.0f; progs[m].cellLast[1] = 0.0f;
    progs[m].cellLast[2] = 1.0f; progs[m].cellLast[3] = 1.0f;
    glUseProgram(p);
    glUniform2f(progs[m].screen, NV_SCREEN_W, NV_SCREEN_H);
    glUniform1i(progs[m].tex, 0);
    // THE DEFAULT HAS TO BE UPLOADED, not merely assumed. A GLSL uniform starts
    // at ZERO, so leaving uCell alone makes the shader compute uv*0 and sample
    // texel (0,0) for the whole quad — every card comes out one flat colour, in
    // silence. The cache above only skips a write it believes has already
    // happened, so seeding the cache without seeding the uniform is what makes
    // the two disagree. Caught by tests/focus_sheet.c.
    if (progs[m].cell >= 0) glUniform4f(progs[m].cell, 0.0f, 0.0f, 1.0f, 1.0f);
  }
  glUseProgram(progs[GFX_CARD].progress);
  progressCurrent = GFX_CARD;

  static const GLfloat quad[] = { 0,0, 1,0, 0,1, 1,1 };
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
  glEnable(GL_BLEND);
  // SEPARATE blending for colour and alpha, and the alpha's GL_ONE is no detail.
  //
  // With GL_SRC_ALPHA on both channels, every translucent draw computes
  // dst.a = a*a + dst.a*(1-a) — that is, it PUNCTURES its own surface. A veil at
  // 40% drops the destination's alpha from 1.0 to 0.76. On the TV the compositor
  // mixes the window with what is behind it using that alpha, so the hole appears
  // as a dark smudge; in a glReadPixels capture it is invisible, because the
  // capture reads the colour and not the composition. This holds for ALL the app's
  // veils and fades, not just the video screen.
  glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  return 1;
}

void gfx_shutdown(void) {
  for (int m = 0; m < GFX_NMODES; m++)
    if (progs[m].progress) { glDeleteProgram(progs[m].progress); progs[m].progress = 0; }
  progressCurrent = -1;
}

// The last texture seen at bind time. The driver does ignore a rebind of the same
// name, but only after paying the entry cost of the call — and in a frame full of
// text the SAME glyph texture is drawn several times in a row.
static GLuint texCurrent = 0;

// Call it when a texture is destroyed (the name may be reused by glGenTextures)
// or when somebody has called glBindTexture outside gfx_rect (an art upload, a
// glyph raster) — in both cases the cache would be lying.
// tex = 0 means "forget everything": that is what the uploads use.
void gfx_tex_forget(GLuint tex) { if (tex == 0 || texCurrent == tex) texCurrent = 0; }

int    gfx_n_rect = 0, gfx_n_progress = 0, gfx_n_bind = 0, gfx_n_others = 0;
double gfx_ms_rect = 0.0, gfx_ms_others = 0.0;
// FILL SUBMITTED this frame, in full screens (1920x1080 = 1.0).
// On this Mali the cost is per fragment, not per call: the note at the top of this
// file says TWO full-screen layers dropped the frame rate to ~40fps. Without
// counting the area, "how many full layers does this screen have" is a guess —
// with the counter it is a measurement per frame.
double gfx_fill = 0.0;
int    gfx_n_full = 0;   // draws covering >= 50% of the screen
static double gfxFreqMs = 0.0;
// WHERE THE SKELETONS' LIGHT IS THIS FRAME, in screen pixels: the centre of the
// band. Advanced once per frame by gfx_new_frame, read by every gfx_skeleton.
static float skelSweep = 0.0f;

void gfx_new_frame(unsigned now) {
  // The band's centre crosses from just off the left edge to just off the right over
  // NV_SKEL_SHINE_SWEEP_MS, then PARKS there for the rest of the period. The travel
  // is exactly the screen plus one band, so the highlight is at precisely zero as it
  // enters and as it leaves — no light appears or vanishes at a screen edge.
  float p = (float)(now % NV_SKEL_SHINE_MS) / (float)NV_SKEL_SHINE_SWEEP_MS;
  if (p > 1.0f) p = 1.0f;
  skelSweep = -NV_SKEL_SHINE_W * 0.5f + p * (NV_SCREEN_W + NV_SKEL_SHINE_W);
  gfx_n_rect = gfx_n_progress = gfx_n_bind = gfx_n_others = 0;
  gfx_ms_rect = gfx_ms_others = 0.0;
  gfx_fill = 0.0; gfx_n_full = 0;
}
// A clock for the GL points that are NOT gfx_rect: the clip, the snapshot's FBO
// and the blur's three passes. On a tiled GPU, changing render target mid-frame
// forces a tile flush — and that is the natural suspect for the CPU cost left
// inside app_draw once gfx_rect and text have been discounted.
#define GFX_OUTRO_START() \
  if (gfxFreqMs == 0.0) gfxFreqMs = 1000.0 / (double)SDL_GetPerformanceFrequency(); \
  Uint64 tO_ = SDL_GetPerformanceCounter()
#define GFX_OUTRO_END() do { \
  gfx_ms_others += (double)(SDL_GetPerformanceCounter() - tO_) * gfxFreqMs; \
  gfx_n_others++; } while (0)

// THE EDGE'S ANTIALIASING, IN PIXELS AND NOT IN A FRACTION OF THE ELEMENT.
//
// The SDF is normalised by the rect's HEIGHT, so the `smoothstep(0.006,-0.006,d)`
// that used to be written into every mask meant a ramp of 0.012 x height: a
// quarter of a pixel on a 20px chip and SIX pixels on a 500px card. The card is
// where it showed — the poster's 4px focus border is drawn as the gap between two
// of these masks, so with 6px of ramp on each of them the border had no hard edge
// anywhere and read as a grey smear rather than a white line. That is the "the
// focus border looks fuzzy", and it was never about the border's own drawing.
//
// 1.25 device pixels of TOTAL ramp, so an axis-aligned edge lights about one
// partial pixel and a curve still resolves smoothly. `screenH` is the letterboxed
// viewport in device pixels, which is what turns a layout height into the real
// one (2 on a 4K panel, 1 at 1080p, 1.4167 on this Mac's retina window).
static float edgeAA(float h) {
  if (h <= 0.0f) return 0.006f;
  float scale = screenH > 0 ? (float)screenH / NV_SCREEN_H : 1.0f;
  float dev = h * scale;
  if (dev < 1.0f) dev = 1.0f;
  float aa = 0.625f / dev;
  // A floor for mediump: below ~0.0008 the ramp gets close to the precision the
  // varying itself carries, and the edge starts to dither instead of ramping.
  if (aa < 0.0008f) aa = 0.0008f;
  return aa;
}

void gfx_rect(GfxRect r, GLuint tex, GfxMode mode, float focus,
              float parx, float pary, float radius,
              float cr, float cg, float cb, float ca) {
  if ((int)mode < 0 || (int)mode >= GFX_NMODES) return;
  if (gfxFreqMs == 0.0) gfxFreqMs = 1000.0 / (double)SDL_GetPerformanceFrequency();
  (void)gfxFreqMs;
#ifdef NV_PERF_FINE
  Uint64 t0 = SDL_GetPerformanceCounter();
#endif
  gfx_n_rect++;
  { float area = (r.w * r.h) / (NV_SCREEN_W * NV_SCREEN_H);
    gfx_fill += area;
    if (area >= 0.5f) gfx_n_full++; }
  const Program *P = &progs[mode];
  if (progressCurrent != (int)mode) { glUseProgram(P->progress); progressCurrent = (int)mode; gfx_n_progress++; }
  // A uniform the mode's shader does not declare comes back as -1 from the link;
  // passing -1 to glUniform is a valid no-op but still pays the GL call's
  // traversal. In a typical home frame there are hundreds of gfx_rect calls, most
  // in modes that use no focus/parallax/texAsp, so the cheap test here saves the
  // expensive call.
  glUniform4f(P->rect, r.x, r.y, r.w, r.h);
  if (P->focus >= 0)   glUniform1f(P->focus, focus);
  if (P->par >= 0)    glUniform2f(P->par, parx, pary);
  if (P->radius >= 0)   glUniform1f(P->radius, radius);
  if (P->aa >= 0)       glUniform1f(P->aa, edgeAA(r.h));
  if (P->aspect >= 0)    glUniform1f(P->aspect, r.h > 0 ? r.w / r.h : 1.0f);
  if (P->texAspect >= 0) glUniform1f(P->texAspect, gfx_tex_aspect_current);
  if (P->cell >= 0) {
    Program *W = &progs[mode];
    if (W->cellLast[0] != gfx_tex_cell_current.x || W->cellLast[1] != gfx_tex_cell_current.y ||
        W->cellLast[2] != gfx_tex_cell_current.w || W->cellLast[3] != gfx_tex_cell_current.h) {
      glUniform4f(P->cell, gfx_tex_cell_current.x, gfx_tex_cell_current.y,
                           gfx_tex_cell_current.w, gfx_tex_cell_current.h);
      W->cellLast[0] = gfx_tex_cell_current.x; W->cellLast[1] = gfx_tex_cell_current.y;
      W->cellLast[2] = gfx_tex_cell_current.w; W->cellLast[3] = gfx_tex_cell_current.h;
    }
  }
  if (P->color >= 0)    glUniform4f(P->color, cr, cg, cb, ca * gfx_opacity_group);
  if (tex && tex != texCurrent) {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    texCurrent = tex;
    gfx_n_bind++;
  }
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
#ifdef NV_PERF_FINE
  gfx_ms_rect += (double)(SDL_GetPerformanceCounter() - t0) * gfxFreqMs;
#endif
}

// A LOADING PLACEHOLDER WITH THE SHINE. See gfx.h, and NV_SKEL_SHINE_W for why the
// sweep is in screen space rather than per block.
//
// All this does is put the band — whose screen position gfx_new_frame already
// worked out — into THIS quad's own coordinates. That conversion is the whole trick:
// one light in screen space, expressed in the local x of every block it crosses, so
// a 360px picker and a 600px card catch the same pass at the same moment.
void gfx_skeleton(GfxRect r, float radius,
                  float cr, float cg, float cb, float ca) {
  float half = NV_SKEL_SHINE_W * 0.5f;
  if (r.w <= 0.0f || r.h <= 0.0f) return;
  gfx_rect(r, 0, GFX_SKELETON, 0.0f,
           (skelSweep - r.x) / r.w,   /* the band's centre, in the quad's x */
           half / r.w,                /* its half-width, likewise */
           radius, cr, cg, cb, ca);
}

void gfx_color(GfxRect r, float radius, float cr, float cg, float cb, float ca) {
  gfx_rect(r, 0, GFX_COLOR, 0, 0, 0, radius, cr, cg, cb, ca);
}
// A transparent hole for the device's video plane to show through.
//
// It has to be done with blending OFF. With blending on, writing alpha 0 merely
// mixes with what is already in the destination and the final alpha stays 1 — the
// surface remains opaque and the video stays invisible, with no error at all. And
// the alpha here is the window's composition channel, so this only has any effect
// with SDL_GL_ALPHA_SIZE 8 requested before the window is created.
void gfx_hole(GfxRect r) {
  glDisable(GL_BLEND);
  gfx_rect(r, 0, GFX_COLOR, 0, 0, 0, 0.0f, 0, 0, 0, 0);
  glEnable(GL_BLEND);
}

void gfx_texture(GfxRect r, GLuint tex) {
  gfx_rect(r, tex, GFX_CARD, 0, 0, 0, 0.0f, 0, 0, 0, 1);
}


int gfx_snap_start(int w, int h) {
  snapW = w; snapH = h;
  glGenTextures(1, &snapTex);
  glBindTexture(GL_TEXTURE_2D, snapTex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  gfx_tex_forget(0);  // the bind above went around gfx_rect
  glGenFramebuffers(1, &snapFbo);
  glBindFramebuffer(GL_FRAMEBUFFER, snapFbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, snapTex, 0);
  GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  if (st != GL_FRAMEBUFFER_COMPLETE) {
    printf("snapshot unavailable (fbo 0x%x) — carrying on without it\n", st);
    glDeleteFramebuffers(1, &snapFbo); glDeleteTextures(1, &snapTex);
    snapFbo = snapTex = 0;
    return 0;
  }
  return 1;
}

void gfx_snap_begin(void) {
  if (!snapFbo) return;
  GFX_OUTRO_START();
  glBindFramebuffer(GL_FRAMEBUFFER, snapFbo);
  // uScreen stays in full-screen coordinates: the smaller viewport does the
  // reduction by itself, and no layout code needs to know an FBO exists.
  glViewport(0, 0, snapW, snapH);
  GFX_OUTRO_END();
}

void gfx_snap_finish(void) {
  if (!snapFbo) return;
  GFX_OUTRO_START();
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  viewportTarget();
  GFX_OUTRO_END();
}

void gfx_snap_draw(void) {
  if (!snapTex) return;
  GfxRect r = { 0, 0, NV_SCREEN_W, NV_SCREEN_H };
  gfx_tex_aspect_current = 0.0f;
  gfx_rect(r, snapTex, GFX_SNAP, 0, 0.0f, 1.0f, 0.0f, 0, 0, 0, 1.0f);
}

void gfx_snap_shutdown(void) {
  if (snapFbo) glDeleteFramebuffers(1, &snapFbo);
  if (snapTex) glDeleteTextures(1, &snapTex);
  snapFbo = snapTex = 0;
}

// --- icones ------------------------------------------------------------------
static char dirIcons[512];

void gfx_icons_dir(const char *dirArt) {
  snprintf(dirIcons, sizeof dirIcons, "%s/icons", dirArt ? dirArt : ".");
}

// An ABSOLUTE path: the app's working directory is not the art folder, and with
// a relative path IMG_Load fails silently and the icon disappears with no error.
// The same trap already documented in extras_path_brand.
const char *gfx_icon_path(const char *name) {
  static char cam[600];
  cam[0] = 0;
  if (name && name[0] && dirIcons[0])
    snprintf(cam, sizeof cam, "%s/%s.png", dirIcons, name);
  return cam;
}

void gfx_icon_at(GfxRect r, const char *name, float wRequest,
                 float cr, float cg, float cb, float ca) {
  char cam[600];
  GLuint t;
  if (!name || !name[0] || !dirIcons[0]) return;
  snprintf(cam, sizeof cam, "%s/%s.png", dirIcons, name);
  // Ask by drawing width EXACTLY, not tex_get_width's width-plus-headroom: an icon
  // is drawn at one fixed size and never scales, so any headroom is a texture the
  // GPU has to minify — and the mipmap filter snaps to the half-resolution level
  // once that minification passes 1.414x. The rail's wordmark sat at 1.4147 and was
  // being drawn from an 80px copy of a 221px file. See tex_get_exact.
  //
  // `wRequest` is the size to DECODE at, which is r.w for everything that does not
  // move and the RESTING size for anything that does. See the header.
  t = tex_get_exact(cam, wRequest > 0.0f ? wRequest : r.w);
  if (!t) return;
  gfx_tex_aspect_current = 0.0f;   // the file is already square
  gfx_rect(r, t, GFX_BRAND, 0, 0, 0, 0.0f, cr, cg, cb, ca);
}

void gfx_icon(GfxRect r, const char *name, float cr, float cg, float cb, float ca) {
  gfx_icon_at(r, name, r.w, cr, cg, cb, ca);
}

void gfx_crop(float x, float y, float w, float h) {
  GFX_OUTRO_START();
  if (w <= 0.0f || h <= 0.0f) { glEnable(GL_SCISSOR_TEST); glScissor(0, 0, 0, 0);
                                GFX_OUTRO_END(); return; }
  // Two conversions happen here, and nowhere else in the app:
  //
  // 1. glScissor counts from the BOTTOM-left corner; everything else works with y
  //    growing downwards.
  // 2. glScissor speaks in BUFFER PIXELS, not layout coordinates. On a retina
  //    screen the buffer is twice the size, and without the scaling the clip
  //    covered a quarter of the requested area — the side menu lost its first two
  //    items and the labels came out cut mid-word.
  float ex = (float)screenW / NV_SCREEN_W, ey = (float)screenH / NV_SCREEN_H;
  // 3. the letterbox moves the origin: the bars are outside the box and the
  //    crop has to be measured from its corner, not the window's.
  int yy = (int)((NV_SCREEN_H - (y + h)) * ey) + screenY;
  glEnable(GL_SCISSOR_TEST);
  glScissor((int)(x * ex) + screenX, yy, (int)(w * ex), (int)(h * ey));
  GFX_OUTRO_END();
}
void gfx_no_crop(void) { glDisable(GL_SCISSOR_TEST); }

// The colour target and the FBO that writes into it, together: every render
// target in this file is the same pair, and the blur's four and the backdrop's
// two used to be two copies of it.
static int createsTargetIn(GLuint *tex, GLuint *fbo, int w, int h) {
  glGenTextures(1, tex);
  glBindTexture(GL_TEXTURE_2D, *tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  gfx_tex_forget(0);  // the bind above went around gfx_rect
  glGenFramebuffers(1, fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
  GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  return st == GL_FRAMEBUFFER_COMPLETE;
}

static int createsTarget(int i, int w, int h) {
  return createsTargetIn(&borderTex[i], &borderFbo[i], w, h);
}

int gfx_blur_start(int w, int h) {
  borderW = w; borderH = h;
  // 0/1 = the detail's pair (the gaussian's ping-pong), 2/3 = the home's pair
  if (!createsTarget(0, w, h) || !createsTarget(1, w, h) ||
      !createsTarget(2, w, h) || !createsTarget(3, w, h)) {
    gfx_blur_shutdown();
    printf("blur unavailable: carrying on without it\n");
    return 0;
  }
  return 1;
}

// Draws the art into the target and runs the gaussian over it twice. It only runs
// when the art changes — the result stays in the texture.
void gfx_blur_generate(int via, unsigned int tex, float texAspect) {
  int a0 = via ? 2 : 0, a1 = via ? 3 : 1;
  if (!borderFbo[a0] || !tex) return;
  GfxRect full = { 0, 0, NV_SCREEN_W, NV_SCREEN_H };
  GFX_OUTRO_START();
  glDisable(GL_BLEND);
  glViewport(0, 0, borderW, borderH);

  glBindFramebuffer(GL_FRAMEBUFFER, borderFbo[a0]);
  gfx_tex_aspect_current = texAspect;
  gfx_rect(full, tex, GFX_SNAP, 0, 0, 0, 0.0f, 0, 0, 0, 1.0f);
  gfx_tex_aspect_current = 0.0f;

  // The step is larger than one texel: with a one-texel step the blur barely
  // covers 4px of the target, which stretched 4x still leaves the image's
  // structure visible.
  float px = NV_BLUR_STEP / (float)borderW, py = NV_BLUR_STEP / (float)borderH;
  glBindFramebuffer(GL_FRAMEBUFFER, borderFbo[a1]);
  gfx_rect(full, borderTex[a0], GFX_BLUR, 0, px, 0.0f, 0.0f, 0, 0, 0, 1.0f);
  glBindFramebuffer(GL_FRAMEBUFFER, borderFbo[a0]);
  gfx_rect(full, borderTex[a1], GFX_BLUR, 0, 0.0f, py, 0.0f, 0, 0, 0, 1.0f);

  glEnable(GL_BLEND);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  viewportTarget();
  GFX_OUTRO_END();
}

void gfx_blur_draw(int via, GfxRect r, float alpha) {
  int a0 = via ? 2 : 0;
  if (!borderTex[a0]) return;
  gfx_tex_aspect_current = 0.0f;
  gfx_rect(r, borderTex[a0], GFX_BACKGROUND, 0, 0, 0, 0.0f, 0, 0, 0, alpha);
}

void gfx_blur_shutdown(void) {
  for (int i = 0; i < 4; i++) {
    if (borderFbo[i]) { glDeleteFramebuffers(1, &borderFbo[i]); borderFbo[i] = 0; }
    if (borderTex[i]) { glDeleteTextures(1, &borderTex[i]); borderTex[i] = 0; }
  }
}

// --- backdrop ----------------------------------------------------------------
//
// The glass under the side menu. The pipeline is the blur's, with ONE difference
// that changes everything: the source is not a texture the caller owns, it is the
// FRAME BUFFER — whatever app_draw has already put on the screen this frame.
//
// THE DOWNSCALE IS MOST OF THE BLUR, and it has to be done in HALVING STEPS.
//
// GL_LINEAR reads exactly four texels. Asked to minify 6x in one draw it does not
// average the 36 source pixels it covers — it picks four of them and interpolates,
// which is point-sampling on a grid. That is an alias, not a blur, and it showed
// as a crosshatch of thin bright streaks over the whole panel, crawling sideways
// whenever the row behind the bar scrolled. The one-shot 64x256 target this
// started as had exactly that defect.
//
// So the strip walks down a chain, each level half the last, where four taps
// really do cover the footprint. The last two levels are the same size: they are
// the separable gaussian's ping-pong pair. Levels 0..BD_FINAL are the descent,
// BD_FINAL is where the blur lands and where gfx_backdrop reads from.
// The top level is 3x the last, not 4x, and that is sized off THE DEVICE: the TV
// grabs 392x1080, so 192x528 is a clean 2.04x first step — the most four taps can
// average — while costing 44% less fill than the 256x704 it started at. The Mac
// preview grabs at retina and takes a 2.9x first step instead, which is the one
// place the two differ and the softer of the two errors to make.
static const struct { int w, h; } BD_LEVEL[] = {
  { NV_BACKDROP_W * 3, NV_BACKDROP_H * 3 },
  { NV_BACKDROP_W + NV_BACKDROP_W / 2, NV_BACKDROP_H + NV_BACKDROP_H / 2 },
  { NV_BACKDROP_W,     NV_BACKDROP_H     },
  { NV_BACKDROP_W,     NV_BACKDROP_H     }
};
#define BD_LEVELS  ((int)(sizeof BD_LEVEL / sizeof BD_LEVEL[0]))
#define BD_FINAL   (BD_LEVELS - 2)
static GLuint bdGrab = 0;
static GLuint bdFbo[BD_LEVELS] = {0}, bdTex[BD_LEVELS] = {0};
static int   bdW = 0, bdH = 0;     // the strip, in layout units
static int   bdGw = 0, bdGh = 0;   // the grab, in buffer pixels
static unsigned bdWhen = 0;
static int   bdHas = 0;

int gfx_backdrop_start(float w, float h) {
  int i;
  bdW = (int)w; bdH = (int)h;
  bdGw = bdGh = 0; bdHas = 0; bdWhen = 0;
  for (i = 0; i < BD_LEVELS; i++) {
    if (createsTargetIn(&bdTex[i], &bdFbo[i], BD_LEVEL[i].w, BD_LEVEL[i].h)) continue;
    gfx_backdrop_shutdown();
    printf("backdrop unavailable: carrying on without the glass\n");
    return 0;
  }
  glGenTextures(1, &bdGrab);
  return 1;
}

// Grabs the strip and runs the gaussian over it. It is the whole cost of the
// glass, and the reason gfx_backdrop rations it by the clock.
static void backdropGenerates(void) {
  float ex = (float)screenW / NV_SCREEN_W, ey = (float)screenH / NV_SCREEN_H;
  int gw = (int)(bdW * ex), gh = (int)(bdH * ey);
  // glCopyTexImage2D reads from the BOTTOM-left corner, and the letterbox moves
  // the origin — the same two conversions gfx_crop documents.
  int gx = screenX, gy = (int)((NV_SCREEN_H - bdH) * ey) + screenY;
  GfxRect full = { 0, 0, NV_SCREEN_W, NV_SCREEN_H };
  float px, py;
  int i;
  if (gw < 1 || gh < 1) return;

  GFX_OUTRO_START();
  glBindTexture(GL_TEXTURE_2D, bdGrab);
  if (gw != bdGw || gh != bdGh) {
    // The first grab, and any window resize: glCopyTexImage2D ALLOCATES as well
    // as copying, so the filter has to be set again with it.
    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, gx, gy, gw, gh, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    bdGw = gw; bdGh = gh;
  } else {
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, gx, gy, gw, gh);
  }
  gfx_tex_forget(0);  // the bind above went around gfx_rect

  glDisable(GL_BLEND);
  gfx_tex_aspect_current = 0.0f;

  // THE DESCENT. Only the FIRST step flips (pary = 1): the grab comes off the
  // frame buffer bottom-up, like an FBO, and GFX_SNAP's flip is what puts it the
  // right way up. Every step after it reads an FBO and writes an FBO, so the two
  // already agree and flipping again would stand the picture back on its head.
  // The number of steps therefore does not reach the final draw at all.
  glViewport(0, 0, BD_LEVEL[0].w, BD_LEVEL[0].h);
  glBindFramebuffer(GL_FRAMEBUFFER, bdFbo[0]);
  gfx_rect(full, bdGrab, GFX_SNAP, 0, 0.0f, 1.0f, 0.0f, 0, 0, 0, 1.0f);
  for (i = 1; i <= BD_FINAL; i++) {
    glViewport(0, 0, BD_LEVEL[i].w, BD_LEVEL[i].h);
    glBindFramebuffer(GL_FRAMEBUFFER, bdFbo[i]);
    gfx_rect(full, bdTex[i - 1], GFX_SNAP, 0, 0.0f, 0.0f, 0.0f, 0, 0, 0, 1.0f);
  }

  // THE GAUSSIAN, over a picture the descent has already box-filtered. Both halves
  // live at the same size, so the viewport is already right from the last step
  // down. Each iteration is one horizontal pass out to the partner and one
  // vertical pass back, which leaves the result in BD_FINAL whatever the count.
  // See NV_BACKDROP_PASSES for why the count is three and not one wider pass.
  px = NV_BLUR_STEP / (float)BD_LEVEL[BD_FINAL].w;
  py = NV_BLUR_STEP / (float)BD_LEVEL[BD_FINAL].h;
  for (i = 0; i < NV_BACKDROP_PASSES; i++) {
    glBindFramebuffer(GL_FRAMEBUFFER, bdFbo[BD_FINAL + 1]);
    gfx_rect(full, bdTex[BD_FINAL], GFX_BLUR, 0, px, 0.0f, 0.0f, 0, 0, 0, 1.0f);
    glBindFramebuffer(GL_FRAMEBUFFER, bdFbo[BD_FINAL]);
    gfx_rect(full, bdTex[BD_FINAL + 1], GFX_BLUR, 0, 0.0f, py, 0.0f, 0, 0, 0, 1.0f);
  }

  glEnable(GL_BLEND);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  viewportTarget();
  GFX_OUTRO_END();
}

// THE CLOCK, not the frame: the rail is on screen every frame of the app's life,
// and a grab plus three passes at 60Hz is a bill it does not need to pay. A 52px
// blur simply has no detail left that 20 updates a second cannot carry.
void gfx_backdrop_grab(unsigned now) {
  if (!bdFbo[0] || bdW <= 0) return;
  if (bdHas && now - bdWhen < (unsigned)NV_BACKDROP_MS) return;
  backdropGenerates();
  bdWhen = now; bdHas = 1;
}

void gfx_backdrop(GfxRect r, float alpha, float sheen) {
  float cw;
  if (!bdFbo[0] || !bdHas || bdW <= 0 ||
      r.w <= 1.0f || r.h <= 1.0f || alpha <= 0.004f) return;
  // The strip was grabbed at its widest; the panel takes the left slice of it.
  // Sampling the whole strip into a narrower rect would SQUEEZE the picture, and
  // the glass would stop lining up with the content beside it.
  cw = r.w / (float)bdW; if (cw > 1.0f) cw = 1.0f;
  gfx_tex_cell_current = (GfxRect){ 0.0f, 0.0f, cw, 1.0f };
  gfx_tex_aspect_current = 0.0f;
  gfx_rect(r, bdTex[BD_FINAL], GFX_BACKDROP, 0, sheen, 0.0f, 0.0f, 0, 0, 0, alpha);
  gfx_tex_cell_current = (GfxRect){ 0.0f, 0.0f, 1.0f, 1.0f };
}

void gfx_backdrop_shutdown(void) {
  for (int i = 0; i < BD_LEVELS; i++) {
    if (bdFbo[i]) { glDeleteFramebuffers(1, &bdFbo[i]); bdFbo[i] = 0; }
    if (bdTex[i]) { glDeleteTextures(1, &bdTex[i]); bdTex[i] = 0; }
  }
  if (bdGrab) { glDeleteTextures(1, &bdGrab); bdGrab = 0; }
  bdGw = bdGh = 0; bdHas = 0;
}
