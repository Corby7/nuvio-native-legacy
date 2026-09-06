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
  GFX_NMODES = 22
} GfxMode;

typedef struct {
  float x, y, w, h;
} GfxRect;

// The aspect ratio (w/h) of the texture to draw. 0 = maps directly (text, veil).
// Set it BEFORE gfx_rect so the art is cropped, never stretched.
extern float gfx_tex_aspect_current;
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
// error. The set is: "more", "watched", "unwatched", "sources", "play",
// "pause", "subtitles", "audio", "aspect", "forward", "episodes", "trailer",
// and "menu_home" / "menu_library" / "menu_profile" / "menu_search" /
// "menu_settings".
void gfx_icons_dir(const char *dirArt);
void gfx_icon(GfxRect r, const char *name, float cr, float cg, float cb, float ca);
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
void gfx_new_frame(void);

void gfx_rect(GfxRect r, GLuint tex, GfxMode mode, float focus,
              float parx, float pary, float radius,
              float cr, float cg, float cb, float ca);

// Atalhos legiveis para os casos comuns.
void gfx_color(GfxRect r, float radius, float cr, float cg, float cb, float ca);
// Zeroes the rectangle's colour AND alpha, with blending off, opening the
// surface to the video plane behind it. See video.h.
void gfx_hole(GfxRect r);
void gfx_texture(GfxRect r, GLuint tex);

#endif
