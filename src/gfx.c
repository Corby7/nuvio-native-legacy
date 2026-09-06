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
  GLint rect, screen, tex, focus, par, radius, color, aspect, texAspect;
} Program;
static Program progs[GFX_NMODES];
static int progressCurrent = -1;
// The current texture's aspect ratio, for the "cover". It is global because the
// drawing is immediate: the caller sets it before each textured rect.
float gfx_tex_aspect_current = 0.0f;
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
  NV_GLSL_PREFIX
  "attribute vec2 aPos;\n"
  "uniform vec4 uRect;\n"
  "uniform vec2 uScreen;\n"
  "varying vec2 vUv;\n"
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
  NV_GLSL_PREFIX
  "varying vec2 vUv;\n"
  "uniform sampler2D uTex;\n"
  "uniform float uFocus;\n"
  "uniform vec2  uPar;\n"
  "uniform float uRadius;\n"
  "uniform vec4  uColor;\n"
  "uniform float uAspect;\n"
  "uniform float uTexAsp;   // w/h of the TEXTURE; 0 = do not adjust\n";

// A rounded-rectangle SDF, corrected for the aspect ratio — without the
// correction a landscape card's corner comes out oval.
static const char *FS_SDF =
  "float sdf(vec2 uv, float r, float asp){\n"
  "  vec2 p = (uv - 0.5) * vec2(asp, 1.0);\n"
  "  vec2 b = vec2(0.5*asp, 0.5) - r;\n"
  "  vec2 q = abs(p) - b;\n"
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
  "  float m = smoothstep(0.006,-0.006,d);\n"
  "  if (m <= 0.001) discard;\n"
  "  vec2 uv = clamp(cover(vUv) + uPar, 0.0, 1.0);\n"
  "  gl_FragColor = vec4(texture2D(uTex, uv).rgb, m * uColor.a);\n"
  "}\n",

  // GFX_SHADOW — a soft blot behind the focused item
  "void main(){\n"
  "  float d = sdf(vUv, uRadius, uAspect);\n"
  "  gl_FragColor = vec4(0.0,0.0,0.0, smoothstep(0.22,-0.03,d)*uFocus*uColor.a);\n"
  "}\n",

  // GFX_COLOR — a solid-colour rectangle/pill
  "void main(){\n"
  "  float m = smoothstep(0.006,-0.006, sdf(vUv, uRadius, uAspect));\n"
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
  "  float m = smoothstep(0.006,-0.006, sdf(vUv, uRadius, uAspect));\n"
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
  "void main(){\n"
  "  vec3 c = texture2D(uTex, clamp(cover(vUv), 0.0, 1.0)).rgb;\n"
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
  "  float m = smoothstep(0.006,-0.006, sdf(vUv, uRadius, uAspect));\n"
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
  "  vec3 c=texture2D(uTex,clamp(cover(vUv),0.0,1.0)).rgb;\n"
  "  gl_FragColor=vec4(c,m*uColor.a);\n"
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
  {0,0}    /* GFX_DISK */
};

static GLuint compiles(GLenum kind, const char *src) {
  GLuint s = glCreateShader(kind);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) { char log[700]; glGetShaderInfoLog(s, 700, NULL, log); printf("gfx shader: %s\n", log); }
  return s;
}

int gfx_start(void) {
  GLuint vs = compiles(GL_VERTEX_SHADER, VS);
  char source[6000];
  for (int m = 0; m < GFX_NMODES; m++) {
    snprintf(source, sizeof source, "%s%s%s%s", FS_HEAD,
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
    progs[m].color  = glGetUniformLocation(p, "uColor");
    progs[m].aspect  = glGetUniformLocation(p, "uAspect");
    progs[m].texAspect = glGetUniformLocation(p, "uTexAsp");
    glUseProgram(p);
    glUniform2f(progs[m].screen, NV_SCREEN_W, NV_SCREEN_H);
    glUniform1i(progs[m].tex, 0);
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
void gfx_new_frame(void) {
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
  if (P->aspect >= 0)    glUniform1f(P->aspect, r.h > 0 ? r.w / r.h : 1.0f);
  if (P->texAspect >= 0) glUniform1f(P->texAspect, gfx_tex_aspect_current);
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

void gfx_icon(GfxRect r, const char *name, float cr, float cg, float cb, float ca) {
  char cam[600];
  GLuint t;
  if (!name || !name[0] || !dirIcons[0]) return;
  // An ABSOLUTE path: the app's working directory is not the art folder, and with
  // a relative path IMG_Load fails silently and the icon disappears with no error.
  // The same trap already documented in extras_path_brand.
  snprintf(cam, sizeof cam, "%s/%s.png", dirIcons, name);
  // Ask by drawing width: a 38px icon does not need the file's 128, and the
  // per-use ceiling is what keeps the cache out of the red.
  t = tex_get_width(cam, r.w);
  if (!t) return;
  gfx_tex_aspect_current = 0.0f;   // the file is already square
  gfx_rect(r, t, GFX_BRAND, 0, 0, 0, 0.0f, cr, cg, cb, ca);
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

static int createsTarget(int i, int w, int h) {
  glGenTextures(1, &borderTex[i]);
  glBindTexture(GL_TEXTURE_2D, borderTex[i]);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  gfx_tex_forget(0);  // the bind above went around gfx_rect
  glGenFramebuffers(1, &borderFbo[i]);
  glBindFramebuffer(GL_FRAMEBUFFER, borderFbo[i]);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, borderTex[i], 0);
  GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  return st == GL_FRAMEBUFFER_COMPLETE;
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
