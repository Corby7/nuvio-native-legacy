#include "splash.h"
#include "intro_sound.h"
#include "gfx.h"
#include "layout.h"
#include "settings.h"
#include <SDL2/SDL_image.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// WHERE THE LOCKUP SITS: the 1149x378 brand_lockup.png (tools/build-brand.sh),
// centred, at the scale that keeps the wordmark the size it has always had.
#define LOCK_FILE_W 1149.0f
#define LOCK_FILE_H 378.0f
#define LOCK_K (721.0f / 1004.0f)
#define LOCK_W (LOCK_FILE_W * LOCK_K)
#define LOCK_H (LOCK_FILE_H * LOCK_K)
#define LOCK_X ((NV_SCREEN_W - LOCK_W) * 0.5f)
#define LOCK_Y ((NV_SCREEN_H - LOCK_H) * 0.5f)
// The Facet Bands mark is the file's columns 23..302 (rows 31..346). Its clip ends
// at 365, in the gap before the wordmark, where there is no art for a hard scissor
// edge to cut through. The three bands are concentric about the point where the
// facets meet, (122, 189) in the file: they grow and breathe about it.
#define MARK_END 365.0f
#define MARK_PX  122.03f
#define MARK_PY  188.79f
// intro_bands.png: the core, the middle band and the outer band apart, each in a
// 312x362 cell that is the lockup's region from (8, 8), so a cell drawn there sits
// exactly on its band in the lockup.
#define BANDS 3
#define BANDS_FILE_W 936
#define BANDS_FILE_H 362
#define BAND_FILE_X 8.0f
#define BAND_FILE_Y 8.0f
#define BAND_W 312.0f
#define BAND_H 362.0f
// The letters of the wordmark, as columns of the file, each clip meeting the next
// in the gap between two letters: n 428..563, u 591..726, v 739..898, i 911..951,
// o 973..1132. Their middle row is 177 (the letters span 113..241).
static const float letterCut[6] = { 365.0f, 577.0f, 732.5f, 904.5f, 962.0f, 1149.0f };
#define WORD_MID_Y 177.0f

// THE GROUND: the banner's navy, #0d101e, with a purple glow rising from the bottom
// (intro_ground.png, the purple at its own alpha, laid over the navy). The navy is
// a flat fill so the portal can cut its hole in it. deploy/app/splash.png is the
// same ground at full size, so the hand-over from webOS's still is invisible.
#define GROUND_R (13.0f / 255.0f)
#define GROUND_G (16.0f / 255.0f)
#define GROUND_B (30.0f / 255.0f)

// The timeline, in ms from the first frame.
#define T_DROP     110   // the core starts its fall toward the screen
#define T_HIT      360   // it lands: the startup sound's hit is here (intro_sound.h)
#define T_HOLD     33    // hit-stop: the bands hold on the contact this long
#define T_BAND     40    // the middle band springs out of the core this long after the hit
#define T_BAND_GAP 60    // and the outer band this much after the middle one
#define T_BAND_POP 320   // each band's spring
#define T_WAVE     (T_HIT + 130)  // the letters' wave starts, after the hit has had its beat
#define T_WAVE_POP 230   // each letter's pop
#define T_WAVE_GAP 34    // the next letter pops this much later
#define T_REST     (T_WAVE + 700)  // the lockup is whole: the earliest the exit can start
#define NV_SPLASH_MAX_MS 6000
#define T_EXIT       480   // the dive through the "o"
#define T_EXIT_QUIET 360   // reduced motion: a fade, no dive
// The mark's centre on screen, in its place in the lockup.
#define MARK_X (LOCK_X + MARK_PX * LOCK_K)
#define MARK_Y (LOCK_Y + MARK_PY * LOCK_K)

static GLuint tex, bands, ground;
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

// One fix to a file before it becomes a texture: fully transparent pixels carry
// black RGB, and GL_LINEAR blends that black into the edge texels: a dark rim on
// anything lighter than black. Each one takes the colour of an opaque neighbour, a
// few pixels deep. (The bands are translucent on purpose, so nothing is flattened
// onto the ground: the glow shows through them and through the gaps between them.)
static void prepare(unsigned char *px, int w, int h) {
  unsigned char *has, *copy;
  int x, y, pass;
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
  // The full 1149px file with no mipmap: drawn at 0.72 it is a mild minification
  // GL_LINEAR handles, and the exit magnifies it, where a mipmap would not help.
  tex = upload(rgba);
  SDL_FreeSurface(rgba);

  // The three bands apart, for their entrance. Without them the opening starts on
  // the finished lockup and the rest (the breathing, the exit) plays as usual.
  rgba = loadArt(dirArt, "intro_bands.png", path, sizeof path);
  if (rgba && rgba->w == BANDS_FILE_W && rgba->h == BANDS_FILE_H) {
    prepare(rgba->pixels, rgba->w, rgba->h);
    bands = upload(rgba);
  } else printf("[splash] no intro bands at %s (or not %dx%d): starting on the lockup\n",
                path, BANDS_FILE_W, BANDS_FILE_H);
  SDL_FreeSurface(rgba);
  // The ground's purple glow. Without it the ground is plain navy.
  rgba = loadArt(dirArt, "intro_ground.png", path, sizeof path);
  if (rgba) ground = upload(rgba);
  SDL_FreeSurface(rgba);
  // The startup sound, ready before the first frame (see intro_sound.h).
  if (bands && settings_intro_sound()) intro_sound_load(dirArt);
}

int splash_active(void) { return tex && !done; }

static void release(void) {
  if (tex) { glDeleteTextures(1, &tex); gfx_tex_forget(tex); }
  if (bands) { glDeleteTextures(1, &bands); gfx_tex_forget(bands); }
  if (ground) { glDeleteTextures(1, &ground); gfx_tex_forget(ground); }
  tex = bands = ground = 0;
  done = 1;
}

void splash_finish(void) {
  if (!splash_active()) return;
  intro_sound_stop();
  release();
  printf("[splash] ended by the dev channel\n");
  fflush(stdout);
}

int splash_event(const SDL_Event *e) {
  int i;
  if (e->type == SDL_KEYUP) {
    for (i = 0; i < 8; i++)
      if (held[i] && held[i] == e->key.keysym.scancode) { held[i] = 0; return 1; }
  }
  if (!splash_active()) return 0;
  // THE OPENING CANNOT BE SKIPPED: it is short, and a press only swallowed. A key
  // pressed at an overlay you cannot see through must not move a focus you cannot
  // see either.
  switch (e->type) {
    case SDL_KEYDOWN:
      for (i = 0; i < 8; i++)
        if (!held[i] || held[i] == e->key.keysym.scancode) {
          held[i] = e->key.keysym.scancode; break; }
      return 1;
    case SDL_KEYUP:
    case SDL_MOUSEBUTTONDOWN:
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
// stadium inside the letter: x 1015..1091, y 144..210), pulled in a pixel so the
// letter's own antialiased edge covers the seam.
#define O_HOLE_X0 (LOCK_X + 1016.0f * LOCK_K)
#define O_HOLE_Y0 (LOCK_Y + 145.0f * LOCK_K)
#define O_HOLE_X1 (LOCK_X + 1090.0f * LOCK_K)
#define O_HOLE_Y1 (LOCK_Y + 209.0f * LOCK_K)
#define PORTAL_ZOOM 80.0f   // enough for the hole to hold the whole screen

// The lockup, whole, clipped to the mark's columns, scaled by `s` about the mark's
// centre: the mark when there are no bands to bring in.
static void drawMark(float s, float a) {
  GfxRect r = { MARK_X + (LOCK_X - MARK_X) * s, MARK_Y + (LOCK_Y - MARK_Y) * s,
                LOCK_W * s, LOCK_H * s };
  const float pad = 4.0f * s;
  GfxRect c = cam((GfxRect){ r.x - pad, r.y - pad, MARK_END * LOCK_K * s + pad, r.h + pad * 2.0f });
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

// Band `i` of intro_bands.png (0 the core, 1 the middle band, 2 the outer band),
// scaled by `s` about the mark's centre.
static void drawBand(int i, float s, float a) {
  const float x = LOCK_X + BAND_FILE_X * LOCK_K, y = LOCK_Y + BAND_FILE_Y * LOCK_K;
  if (s <= 0.02f || a <= 0.002f) return;
  // The cell has to go back to the whole texture afterwards, or it crops
  // everything drawn next (see gfx_tex_cell_current).
  gfx_tex_aspect_current = 0.0f;
  gfx_tex_cell_current = (GfxRect){ (float)i / BANDS, 0.0f, 1.0f / BANDS, 1.0f };
  gfx_opacity_group = a;
  gfx_texture(cam((GfxRect){ MARK_X + (x - MARK_X) * s, MARK_Y + (y - MARK_Y) * s,
                             BAND_W * LOCK_K * s, BAND_H * LOCK_K * s }), bands);
  gfx_opacity_group = 1.0f;
  gfx_tex_cell_current = (GfxRect){ 0.0f, 0.0f, 1.0f, 1.0f };
}

void splash_draw(Uint32 now, int ready) {
  float t, tr, tb, s = 1.0f, markA = 1.0f, bgA = 1.0f, glowA = 1.0f;
  float bs[BANDS], ba[BANDS];
  const float cx = MARK_X, cy = MARK_Y;
  int i;
  if (!splash_active()) return;
  if (!t0) {
    t0 = now;
    // Reduced motion: the finished lockup, still, and the app fades up through it.
    quiet = settings_animations_reduced();
    printf("[splash] start%s\n", quiet ? " (reduced motion)" : "");
    fflush(stdout);
    // The sound starts with the first frame, so its hit lands on the slam. Reduced
    // motion has no slam to land on: no sound either.
    if (quiet) intro_sound_drop();
    else intro_sound_play();
  }
  t = (float)(now - t0);
  if (!exitAt && ((ready && t >= (quiet ? 0 : T_REST)) || t >= NV_SPLASH_MAX_MS)) {
    exitAt = now;
    exitLen = quiet ? T_EXIT_QUIET : T_EXIT;
    if (!ready) printf("[splash] still not ready after %d ms: going in anyway\n",
                       NV_SPLASH_MAX_MS);
  }
  // The exit freezes the choreography where it was and plays the exit over it,
  // rather than jumping to the end of a part that had not happened yet (only the
  // NV_SPLASH_MAX_MS limit can start it before the lockup is whole).
  tr = exitAt ? (float)(exitAt - t0) : t;
  // Nothing to bring in (reduced motion, or no bands): straight to the finished lockup.
  if (quiet || !bands) tr = (float)T_REST;
  // The hit-stop: the bands and the letters hold on the contact for a beat while
  // the world (the camera) reacts on the real clock, `tr`.
  tb = tr < T_HIT ? tr : (tr < T_HIT + T_HOLD ? (float)T_HIT : tr - T_HOLD);

  // THE BANDS ARRIVE. The core falls in from close to the camera, gathering speed,
  // and lands in its place at T_HIT; then the middle band and the outer one spring
  // out of it like a ripple from the landing, overshooting a little and settling.
  { const float p = phase(tb, T_DROP, T_HIT);
    bs[0] = 1.0f + 1.6f * (1.0f - p * p);
    ba[0] = clamp01(p * 3.0f); }
  for (i = 1; i < BANDS; i++) {
    const float at = (float)(T_HIT + T_BAND + (i - 1) * T_BAND_GAP);
    const float q = phase(tb, at, at + T_BAND_POP);
    bs[i] = 0.55f + 0.45f * easeOutBack(q, 1.7f);
    ba[i] = clamp01(q * 3.0f);
  }
  // Waiting for the app: the bands breathe, each a little after the one inside it,
  // so a slow catalogue does not look like a frozen frame.
  if (!quiet && tr >= T_REST) {
    const float w = clamp01((tr - T_REST) / 400.0f);
    for (i = 0; i < BANDS; i++)
      bs[i] *= 1.0f + 0.012f * w * sinf((tr - T_REST) * 0.0045f - 0.6f * (float)i);
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
  } else if (!quiet && bands && tr >= T_HIT) {
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
  if (!bands) { drawMark(s, markA); return; }
  // Back to front: the outer band, the middle one, the core, so a band springing
  // out of the core starts behind it.
  for (i = BANDS - 1; i >= 0; i--) drawBand(i, bs[i], ba[i] * markA);
}
