#include "splash.h"
#include "splash_intro.h"
#include "gfx.h"
#include "layout.h"
#include "settings.h"
#include <SDL2/SDL_image.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// WHERE THE LOCKUP SITS: the 1004x344 brand_lockup.png at 0.718 and centred, art
// spanning x 600..1320 and y 416..663. The pyramid lands straight into its place
// in it.
#define LOCK_FILE_W 1004.0f
#define LOCK_X 599.5f
#define LOCK_Y 416.0f
#define LOCK_W 721.0f
#define LOCK_H 247.0f
// The lockup's columns, as fractions of its width: the play mark is 9..247 of
// 1004 (rows 26..294, centred on the letters) and the wordmark starts at 299. The
// mark's clip ends in that gap, where there is no art for a hard scissor edge to
// cut through.
#define MARK_END  (273.0f / LOCK_FILE_W)
#define MARK_W    (248.0f / LOCK_FILE_W * LOCK_W)
#define MARK_MID_Y 160.0f
// The letters of the wordmark, as columns of the file, each clip meeting the next
// in the gap between two letters: n 299..434, u 462..597, v 610..769, i 782..822,
// o 844..1003. Their middle row is 160 (the letters span 96..224).
static const float letterCut[6] = { 273.0f, 448.0f, 603.5f, 775.5f, 833.0f, 1004.0f };
#define WORD_MID_Y 160.0f

// THE GROUND: the banner's navy, #0d101e, with a purple glow rising from the bottom
// (intro_ground.png, the purple at its own alpha, laid over the navy). The navy is
// a flat fill so the portal can cut its hole in it. deploy/app/splash.png is the
// same ground at full size, so the hand-over from webOS's still is invisible.
#define GROUND_R (13.0f / 255.0f)
#define GROUND_G (16.0f / 255.0f)
#define GROUND_B (30.0f / 255.0f)

// The timeline, in ms from the first frame.
#define T_FRAMES   (INTRO_FRAMES * 1000 / INTRO_FPS)  // the pyramid, as rendered
#define T_HIT      INTRO_HIT_MS  // it hits the ground: the frames after it are the rebound
#define T_HOLD     33    // hit-stop: the pyramid holds on the contact frame this long
#define T_WAVE     (T_HIT + 130)  // the letters' wave starts, after the hit has had its beat
#define T_WAVE_POP 230   // each letter's pop
#define T_WAVE_GAP 34    // the next letter pops this much later
#define T_REST     (T_WAVE + 700)  // the lockup is whole: the earliest the exit can start
#define NV_SPLASH_MAX_MS 6000
#define T_EXIT       480   // the dive through the "o"
#define T_EXIT_SKIP  360
// The finished mark's top-left on screen, in its place in the lockup.
#define MARK_X (LOCK_X + INTRO_MARK_FILE_X * LOCK_W / LOCK_FILE_W)
#define MARK_Y (LOCK_Y + INTRO_MARK_FILE_Y * LOCK_W / LOCK_FILE_W)

static GLuint tex, atlas, ground;
static Uint32 t0, exitAt, exitLen;
static int done, quiet;
// Scancodes whose KEYDOWN was swallowed: their KEYUP must not reach a screen that
// never saw the press (OK acts on its release, to tell a press from a hold).
static SDL_Scancode held[8];

static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
static float phase(float t, float a, float b) { return clamp01((t - a) / (b - a)); }
static float easeInOut(float p) {
  return p < 0.5f ? 4.0f * p * p * p : 1.0f - powf(-2.0f * p + 2.0f, 3.0f) * 0.5f;
}
// Past 1 and back, by `c`: the letters' pop.
static float easeOutBack(float p, float c) {
  const float q = p - 1.0f;
  return p <= 0.0f ? 0.0f : 1.0f + (c + 1.0f) * q * q * q + c * q * q;
}

// Two fixes to the file before it becomes a texture:
//
// 1. The play symbol inside the mark is a HOLE in the art, with a soft
//    semi-transparent bevel, not dark paint. Every pixel between a row's first and
//    last opaque pixel of the mark is flattened onto the ground colour; only the
//    mark's outer edge keeps its antialiasing. (The mark is drawn from the lockup
//    only when the intro atlas is missing.)
// 2. Fully transparent pixels carry black RGB, and GL_LINEAR blends that black
//    into the edge texels: a dark rim on anything lighter than black. Each one
//    takes the colour of an opaque neighbour, a few pixels deep.
static void prepare(unsigned char *px, int w, int h) {
  const int bg[3] = { 13, 16, 30 }, markEnd = (int)(MARK_END * w);
  unsigned char *has, *copy;
  int x, y, pass, c;
  for (y = 0; y < h; y++) {
    unsigned char *row = px + (size_t)y * w * 4;
    int first = -1, last = -1;
    for (x = 0; x < markEnd; x++)
      if (row[x * 4 + 3] >= 250) { if (first < 0) first = x; last = x; }
    for (x = first + 1; first >= 0 && x < last; x++) {
      unsigned char *p = row + x * 4;
      int a = p[3];
      for (c = 0; c < 3; c++) p[c] = (unsigned char)((p[c] * a + bg[c] * (255 - a)) / 255);
      p[3] = 255;
    }
  }
  // `has` marks a pixel whose RGB is real: opaque in the file, or filled by an
  // earlier pass. Each pass grows the filled band by one pixel.
  has = malloc((size_t)w * h);
  copy = malloc((size_t)w * h);
  if (!has || !copy) { free(has); free(copy); return; }
  for (x = 0; x < w * h; x++) has[x] = px[(size_t)x * 4 + 3] > 0;
  for (pass = 0; pass < 3; pass++) {
    memcpy(copy, has, (size_t)w * h);
    for (y = 0; y < h; y++)
      for (x = 0; x < w; x++) {
        unsigned char *p = px + ((size_t)y * w + x) * 4;
        int dx, dy;
        if (copy[(size_t)y * w + x]) continue;
        for (dy = -1; dy <= 1 && !has[(size_t)y * w + x]; dy++)
          for (dx = -1; dx <= 1; dx++) {
            size_t n;
            if (x + dx < 0 || x + dx >= w || y + dy < 0 || y + dy >= h) continue;
            n = (size_t)(y + dy) * w + x + dx;
            if (!copy[n]) continue;
            memcpy(p, px + n * 4, 3);
            has[(size_t)y * w + x] = 1;
            break;
          }
      }
  }
  free(has);
  free(copy);
}

// An RGBA surface as a plain GL_LINEAR texture: no mipmap, clamped edges.
static GLuint upload(SDL_Surface *rgba) {
  GLuint id = 0;
  glGenTextures(1, &id);
  glBindTexture(GL_TEXTURE_2D, id);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rgba->w, rgba->h, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, rgba->pixels);
  gfx_tex_forget(0);
  return id;
}

// An art file as RGBA, or NULL.
static SDL_Surface *loadArt(const char *dirArt, const char *name, char *path, size_t n) {
  SDL_Surface *raw, *rgba;
  snprintf(path, n, "%s/icons/%s", dirArt ? dirArt : ".", name);
  raw = IMG_Load(path);
  rgba = raw ? SDL_ConvertSurfaceFormat(raw, SDL_PIXELFORMAT_ABGR8888, 0) : NULL;
  SDL_FreeSurface(raw);
  return rgba;
}

void splash_start(const char *dirArt) {
  const char *off = getenv("NUVIO_INTRO");
  char path[600];
  SDL_Surface *rgba;
  if (off && !strcmp(off, "0")) { done = 1; return; }
  rgba = loadArt(dirArt, "brand_lockup.png", path, sizeof path);
  if (!rgba) {
    printf("[splash] no lockup at %s: starting without the animation\n", path);
    done = 1;
    return;
  }
  prepare(rgba->pixels, rgba->w, rgba->h);
  // The full 1004px file with no mipmap: drawn at 0.72 it is a mild minification
  // GL_LINEAR handles, and the exit magnifies it, where a mipmap would not help.
  tex = upload(rgba);
  SDL_FreeSurface(rgba);

  // The pyramid's frames, its light and halo. Without them the opening starts on
  // the finished lockup and the rest (the breathing, the exit) plays as usual.
  rgba = loadArt(dirArt, "intro_atlas.png", path, sizeof path);
  if (rgba && rgba->w == INTRO_ATLAS_W && rgba->h == INTRO_ATLAS_H) atlas = upload(rgba);
  else printf("[splash] no intro atlas at %s (or not %dx%d): starting on the lockup\n",
              path, INTRO_ATLAS_W, INTRO_ATLAS_H);
  SDL_FreeSurface(rgba);
  // The ground's purple glow. Without it the ground is plain navy.
  rgba = loadArt(dirArt, "intro_ground.png", path, sizeof path);
  if (rgba) ground = upload(rgba);
  SDL_FreeSurface(rgba);
}

int splash_active(void) { return tex && !done; }

static void release(void) {
  if (tex) { glDeleteTextures(1, &tex); gfx_tex_forget(tex); }
  if (atlas) { glDeleteTextures(1, &atlas); gfx_tex_forget(atlas); }
  if (ground) { glDeleteTextures(1, &ground); gfx_tex_forget(ground); }
  tex = atlas = ground = 0;
  done = 1;
}

void splash_finish(void) {
  if (!splash_active()) return;
  release();
  printf("[splash] ended by the dev channel\n");
  fflush(stdout);
}

static void skip(void) {
  if (exitAt) return;
  exitAt = SDL_GetTicks();
  exitLen = T_EXIT_SKIP;
  if (!t0) t0 = exitAt;
}

int splash_event(const SDL_Event *e) {
  int i;
  if (e->type == SDL_KEYUP) {
    for (i = 0; i < 8; i++)
      if (held[i] && held[i] == e->key.keysym.scancode) { held[i] = 0; return 1; }
  }
  if (!splash_active()) return 0;
  switch (e->type) {
    case SDL_KEYDOWN:
      for (i = 0; i < 8; i++)
        if (!held[i] || held[i] == e->key.keysym.scancode) {
          held[i] = e->key.keysym.scancode; break; }
      skip();
      return 1;
    case SDL_KEYUP:
      return 1;
    case SDL_MOUSEBUTTONDOWN:
      skip();
      return 1;
    case SDL_MOUSEBUTTONUP:
    case SDL_MOUSEWHEEL:
      return 1;
    default:
      return 0;
  }
}

// THE CAMERA. Every rectangle the splash draws is zoomed by camZ about (camPX,
// camPY) and panned by camD. On landing it kicks toward the viewer about the mark
// and jolts down; on the way out it dives into the counter of the "o", which
// drifts to the middle of the screen while the hole opens over the app. At rest
// camZ is 1 and camD 0, and nothing moves.
static float camZ = 1.0f, camPX, camPY, camDX, camDY;
static GfxRect cam(GfxRect r) {
  return (GfxRect){ camPX + (r.x - camPX) * camZ + camDX, camPY + (r.y - camPY) * camZ + camDY,
                    r.w * camZ, r.h * camZ };
}
// The counter of the "o" on screen, measured off brand_lockup.png (the clear
// stadium inside the letter: x 886..962, y 127..193), pulled in a pixel so the
// letter's own antialiased edge covers the seam.
#define O_HOLE_X0 (LOCK_X + 887.0f * LOCK_W / LOCK_FILE_W)
#define O_HOLE_Y0 (LOCK_Y + 128.0f * LOCK_W / LOCK_FILE_W)
#define O_HOLE_X1 (LOCK_X + 961.0f * LOCK_W / LOCK_FILE_W)
#define O_HOLE_Y1 (LOCK_Y + 192.0f * LOCK_W / LOCK_FILE_W)
#define PORTAL_ZOOM 80.0f   // enough for the hole to hold the whole screen

// The lockup, whole, clipped to the mark's columns, scaled by `s` about the mark's
// centre: the mark when there are no frames to play.
static void drawMark(float s, float a) {
  const float mcx = LOCK_X + MARK_W * 0.5f, mcy = LOCK_Y + MARK_MID_Y * LOCK_W / LOCK_FILE_W;
  GfxRect r = { mcx + (LOCK_X - mcx) * s, mcy + (LOCK_Y - mcy) * s, LOCK_W * s, LOCK_H * s };
  const float pad = 4.0f * s;
  GfxRect c = cam((GfxRect){ r.x - pad, r.y - pad, LOCK_W * MARK_END * s + pad, r.h + pad * 2.0f });
  gfx_crop(c.x, c.y, c.w, c.h);
  gfx_logo(cam(r), tex, 0, 1, 1, 1, a);
  gfx_no_crop();
}

// One letter of the wordmark, `p` (0..1) of the way through its pop: it grows from
// nothing about its own centre, past full size and back. Its clip scales with it,
// so it never shows a neighbour's edge.
static void drawLetter(int i, float p, float a) {
  const float k = LOCK_W / LOCK_FILE_W;
  const float lx = LOCK_X + (letterCut[i] + letterCut[i + 1]) * 0.5f * k;
  const float ly = LOCK_Y + WORD_MID_Y * k;
  const float s = easeOutBack(p, 1.9f), la = a * clamp01(p * 4.0f);
  GfxRect r = { lx + (LOCK_X - lx) * s, ly + (LOCK_Y - ly) * s, LOCK_W * s, LOCK_H * s };
  GfxRect c;
  if (s <= 0.02f || la <= 0.002f) return;
  c = cam((GfxRect){ r.x + letterCut[i] * k * s, r.y,
                     (letterCut[i + 1] - letterCut[i]) * k * s + 2.0f, r.h });
  gfx_crop(c.x, c.y, c.w, c.h);
  gfx_logo(cam(r), tex, 0, 1, 1, 1, la);
  gfx_no_crop();
}

// One cell of the atlas, drawn into `r` (before the camera).
static void drawCell(const IntroCell *c, GfxRect r, float a) {
  if (c->w <= 0 || a <= 0.002f) return;
  // The cell has to go back to the whole texture afterwards, or it crops
  // everything drawn next (see gfx_tex_cell_current).
  gfx_tex_aspect_current = 0.0f;
  gfx_tex_cell_current = (GfxRect){ (float)c->x / INTRO_ATLAS_W, (float)c->y / INTRO_ATLAS_H,
                                    (float)c->w / INTRO_ATLAS_W, (float)c->h / INTRO_ATLAS_H };
  gfx_opacity_group = a;
  gfx_texture(cam(r), atlas);
  gfx_opacity_group = 1.0f;
  gfx_tex_cell_current = (GfxRect){ 0.0f, 0.0f, 1.0f, 1.0f };
}

// Where cell `c` goes on frame `f`: the light and the halo are made for the
// finished mark, and on the rebound's frames they follow the mark's size, placed
// as they sit on the last frame. Then scaled by `s` about the mark's centre.
static GfxRect follow(const IntroCell *c, int f, float s) {
  const IntroCell *fr = &introFrame[f], *last = &introFrame[INTRO_FRAMES - 1];
  const float k = fr->sh / last->sh;
  const float cx = MARK_X + INTRO_MARK_W * 0.5f, cy = MARK_Y + INTRO_MARK_H * 0.5f;
  const float x = MARK_X + fr->dx + (c->dx - last->dx) * k;
  const float y = MARK_Y + fr->dy + (c->dy - last->dy) * k;
  return (GfxRect){ cx + (x - cx) * s, cy + (y - cy) * s, c->sw * k * s, c->sh * k * s };
}

void splash_draw(Uint32 now, int ready) {
  float t, tr, tb, s = 1.0f, light = 0.0f, halo = 0.0f, grow = 1.22f;
  float markA = 1.0f, bgA = 1.0f, glowA = 1.0f;
  const float cx = MARK_X + INTRO_MARK_W * 0.5f, cy = MARK_Y + INTRO_MARK_H * 0.5f;
  int i, f;
  if (!splash_active()) return;
  if (!t0) {
    t0 = now;
    // Reduced motion: the finished lockup, still, and the app fades up through it.
    quiet = settings_animations_reduced();
    printf("[splash] start%s\n", quiet ? " (reduced motion)" : "");
    fflush(stdout);
  }
  t = (float)(now - t0);
  if (!exitAt && ((ready && t >= (quiet ? 0 : T_REST)) || t >= NV_SPLASH_MAX_MS)) {
    exitAt = now;
    exitLen = quiet ? T_EXIT_SKIP : T_EXIT;
    if (!ready) printf("[splash] still not ready after %d ms: going in anyway\n",
                       NV_SPLASH_MAX_MS);
  }
  // A skip or the exit freezes the choreography where it was and plays the exit
  // over it, rather than jumping to the end of a part that had not happened yet.
  tr = exitAt ? (float)(exitAt - t0) : t;
  // No frames to play (reduced motion, or no atlas): straight to the finished lockup.
  if (quiet || !atlas) tr = (float)T_REST;
  // The hit-stop: the pyramid and the letters hold on the contact for a beat while
  // the world (the camera, the light) reacts on the real clock, `tr`.
  tb = tr < T_HIT ? tr : (tr < T_HIT + T_HOLD ? (float)T_HIT : tr - T_HOLD);
  f = (int)(tb * INTRO_FPS / 1000.0f);
  if (f >= INTRO_FRAMES) f = INTRO_FRAMES - 1;

  // The landing lights the core: up within 30 ms of the hit, fading over about
  // 0.7 s, with a soft halo behind the mark that slowly spreads.
  if (!quiet && atlas && tr >= T_HIT) {
    const float u = tr - T_HIT, env = clamp01(u / 30.0f) * expf(-u / 300.0f);
    light = env;
    halo = 0.5f * env;
    grow = 1.22f + 0.18f * clamp01(u / 400.0f);
  }
  // Waiting for the app: the light breathes, so a slow catalogue does not look
  // like a frozen frame.
  if (!quiet && tr >= T_REST) {
    const float w = clamp01((tr - T_REST) / 400.0f);
    const float b = 0.5f + 0.5f * sinf((tr - T_REST) * 0.0045f - 1.5707963f);
    if (w * (0.06f + 0.14f * b) > light) light = w * (0.06f + 0.14f * b);
    if (w * (0.10f + 0.15f * b) > halo) halo = w * (0.10f + 0.15f * b);
    s *= 1.0f + 0.012f * w * sinf((tr - T_REST) * 0.0045f);
  }

  camZ = 1.0f; camDX = camDY = 0.0f;
  camPX = (O_HOLE_X0 + O_HOLE_X1) * 0.5f;
  camPY = (O_HOLE_Y0 + O_HOLE_Y1) * 0.5f;
  if (exitAt) {
    float e = clamp01((float)(now - exitAt) / (float)exitLen);
    if (e >= 1.0f) {
      release();
      camZ = 1.0f; camDX = camDY = 0.0f;
      printf("[splash] done after %u ms\n", (unsigned)(now - t0));
      fflush(stdout);
      return;
    }
    if (quiet) {
      // Reduced motion: no dive, everything fades and the app comes up through it.
      markA = 1.0f - easeInOut(clamp01((e - 0.1f) / 0.55f));
      bgA = 1.0f - easeInOut(clamp01((e - 0.25f) / 0.75f));
      glowA = bgA;
    } else {
      // Into the "o": the zoom accelerates (exponentially, so each moment doubles
      // the size by the same amount of time) while the counter drifts to the
      // middle. The letters and the mark fade as they swell past the screen, so
      // the "o" reads as a rim going by rather than a white flash.
      // It winds up first: a quick pull back before the plunge.
      const float pull = e < 0.14f ? 0.06f * sinf(3.14159265f * e / 0.14f) : 0.0f;
      const float d = clamp01((e - 0.1f) / 0.9f);
      camZ = (1.0f - pull) * expf(logf(PORTAL_ZOOM) * d * d);
      camDX = (NV_SCREEN_W * 0.5f - camPX) * easeInOut(d);
      camDY = (NV_SCREEN_H * 0.5f - camPY) * easeInOut(d);
      markA = 1.0f - easeInOut(clamp01((e - 0.25f) / 0.45f));
      // The purple lies over the hole too: it goes before the app shows through.
      glowA = 1.0f - easeInOut(clamp01(e / 0.35f));
    }
    light *= quiet ? markA : 1.0f - clamp01(e * 4.0f);
    halo *= quiet ? markA : 1.0f - clamp01(e * 4.0f);
  } else if (!quiet && atlas && tr >= T_HIT) {
    // The hit: the camera kicks toward the viewer about the mark and jolts down,
    // both settling within a few frames.
    const float u = tr - T_HIT;
    camPX = cx;
    camPY = cy;
    camZ = 1.0f + 0.035f * expf(-u / 60.0f);
    camDY = 7.0f * expf(-u / 45.0f) * cosf(u * 0.09f);
  }

  gfx_tex_aspect_current = 0.0f;
  if (exitAt && !quiet) {
    // The ground with the counter cut out: the app shows through the "o". A plug
    // over the counter fades in the first moments, so the portal opens softly.
    const GfxRect hole = cam((GfxRect){ O_HOLE_X0, O_HOLE_Y0, O_HOLE_X1 - O_HOLE_X0,
                                        O_HOLE_Y1 - O_HOLE_Y0 });
    const float plug = 1.0f - easeInOut(clamp01((float)(now - exitAt) / (exitLen * 0.3f)));
    const GfxRect cell = gfx_tex_cell_current;
    gfx_tex_cell_current = hole;
    gfx_rect((GfxRect){ 0, 0, NV_SCREEN_W, NV_SCREEN_H }, 0, GFX_SCRIM_HOLE, 0.0f,
             NV_SCREEN_W, NV_SCREEN_H, hole.h * 0.5f, GROUND_R, GROUND_G, GROUND_B, 1.0f);
    gfx_tex_cell_current = cell;
    if (plug > 0.002f) gfx_color(hole, 0.5f, GROUND_R, GROUND_G, GROUND_B, plug);
  } else {
    gfx_color((GfxRect){ 0, 0, NV_SCREEN_W, NV_SCREEN_H }, 0.0f, GROUND_R, GROUND_G, GROUND_B, bgA);
  }
  // The purple glow, a little past the screen's edges so the camera's jolt never
  // shows one.
  if (ground && glowA > 0.002f) {
    gfx_opacity_group = glowA;
    gfx_texture(cam((GfxRect){ -24.0f, -24.0f, NV_SCREEN_W + 48.0f, NV_SCREEN_H + 48.0f }), ground);
    gfx_opacity_group = 1.0f;
  }
  if (exitAt && !quiet) {
    // The portal's edge lit in the mark's colours: cyan on the rim, violet just
    // inside it, burning off as the dive gathers speed.
    const GfxRect hole = cam((GfxRect){ O_HOLE_X0, O_HOLE_Y0, O_HOLE_X1 - O_HOLE_X0,
                                        O_HOLE_Y1 - O_HOLE_Y0 });
    const float e = clamp01((float)(now - exitAt) / (float)exitLen);
    const float plug = 1.0f - easeInOut(clamp01((float)(now - exitAt) / (exitLen * 0.3f)));
    const float rim = (1.0f - plug * 0.6f) * (1.0f - easeInOut(clamp01((e - 0.2f) / 0.6f)));
    const GfxRect in = { hole.x + hole.h * 0.06f, hole.y + hole.h * 0.06f,
                         hole.w - hole.h * 0.12f, hole.h * 0.88f };
    if (rim > 0.002f) {
      gfx_rect(hole, 0, GFX_RING, 0, 0.05f, 0.0f, 0.5f, 0.40f, 0.85f, 1.0f, 0.95f * rim);
      gfx_rect(in, 0, GFX_RING, 0, 0.05f, 0.0f, 0.5f, 0.70f, 0.32f, 0.95f, 0.75f * rim);
    }
  }
  // The wordmark: a wave from the hit runs through it, popping each letter, the one
  // nearest the mark first.
  for (i = 0; i < 5; i++) {
    const float at = (float)(T_WAVE + i * T_WAVE_GAP);
    drawLetter(i, phase(tb, at, at + T_WAVE_POP), markA);
  }
  if (!atlas) { drawMark(s, markA); return; }
  // The halo behind the mark, the pyramid, then the light over it.
  if (halo > 0.002f) drawCell(&introHalo, follow(&introHalo, f, s * grow), halo * markA);
  { const IntroCell *c = &introFrame[f];
    drawCell(c, (GfxRect){ cx + (MARK_X + c->dx - cx) * s, cy + (MARK_Y + c->dy - cy) * s,
                           c->sw * s, c->sh * s }, markA); }
  if (light > 0.002f) drawCell(&introLight, follow(&introLight, f, s), light * markA);
}
