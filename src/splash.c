#include "splash.h"
#include "gfx.h"
#include "layout.h"
#include "settings.h"
#include <SDL2/SDL_image.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// WHERE THE SPLASH HAS THE LOGO, measured off deploy/app/splash.png: the lockup's
// art spans x 570..1348 and y 416..662 on a #0d0d0d field, which is the 1085x344
// brand_lockup.png at 0.718 and centred. The field is NV_COLOR_BACKGROUND, so the
// screen under the logo is already the right colour.
#define LOCK_X 570.0f
#define LOCK_Y 416.0f
#define LOCK_W 779.0f
#define LOCK_H 247.0f
// The lockup's columns, as fractions of its width: the play mark is 0..312 of
// 1085 and the wordmark starts at 380. The mark's clip ends in that gap, where
// there is no art for a hard scissor edge to cut through.
#define MARK_END  (330.0f / 1085.0f)
#define MARK_W    (313.0f / 1085.0f * LOCK_W)
// The letters of the wordmark, as columns of the file, each clip meeting the next
// in the gap between two letters: n 380..515, u 543..678, v 691..850, i 863..903,
// o 925..1084. Their middle row is 160 (the letters span 96..224).
static const float letterCut[6] = { 346.0f, 529.0f, 684.5f, 856.5f, 914.0f, 1085.0f };
#define WORD_MID_Y 160.0f

// The timeline, in ms from the first frame.
#define T_HOLD       180   // the splash, unchanged: the hand-over has to land first
#define T_LETTER     280   // each letter's exit
#define T_STAGGER    45    // the next letter leaves this much later
#define T_GLIDE_FROM 320
#define T_GLIDE_END  820   // the mark is centred
#define T_PRESS_END  940   // the mark is down
#define T_REST       1300  // it has sprung back: the earliest the exit can start
#define NV_SPLASH_MAX_MS 6000
#define T_EXIT       620
#define T_EXIT_SKIP  320

static GLuint tex;
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
static float easeOut(float p) { return 1.0f - (1.0f - p) * (1.0f - p); }

// Two fixes to the file before it becomes a texture, both because the glow puts
// colour BEHIND the mark, where the splash only ever had flat background:
//
// 1. The play symbol inside the mark is a HOLE in the art, with a soft
//    semi-transparent bevel, not dark paint. With the glow behind it the hole lit
//    up violet. Every pixel between a row's first and last opaque pixel of the
//    mark is flattened onto the background colour, which is exactly how the
//    splash shows it; only the mark's outer edge keeps its antialiasing.
// 2. Fully transparent pixels carry black RGB, and GL_LINEAR blends that black
//    into the edge texels: invisible on the dark field, a dark rim on the glow.
//    Each one takes the colour of an opaque neighbour, a few pixels deep.
static void prepare(unsigned char *px, int w, int h) {
  const int bg = 13, markEnd = (int)(MARK_END * 1085.0f) * w / 1085;
  unsigned char *has, *copy;
  int x, y, pass;
  for (y = 0; y < h; y++) {
    unsigned char *row = px + (size_t)y * w * 4;
    int first = -1, last = -1;
    for (x = 0; x < markEnd; x++)
      if (row[x * 4 + 3] >= 250) { if (first < 0) first = x; last = x; }
    for (x = first + 1; first >= 0 && x < last; x++) {
      unsigned char *p = row + x * 4;
      int a = p[3];
      p[0] = (unsigned char)((p[0] * a + bg * (255 - a)) / 255);
      p[1] = (unsigned char)((p[1] * a + bg * (255 - a)) / 255);
      p[2] = (unsigned char)((p[2] * a + bg * (255 - a)) / 255);
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

void splash_start(const char *dirArt) {
  const char *off = getenv("NUVIO_INTRO");
  char path[600];
  SDL_Surface *raw, *rgba;
  if (off && !strcmp(off, "0")) { done = 1; return; }
  snprintf(path, sizeof path, "%s/icons/brand_lockup.png", dirArt ? dirArt : ".");
  raw = IMG_Load(path);
  rgba = raw ? SDL_ConvertSurfaceFormat(raw, SDL_PIXELFORMAT_ABGR8888, 0) : NULL;
  SDL_FreeSurface(raw);
  if (!rgba) {
    printf("[splash] no lockup at %s: starting without the animation\n", path);
    done = 1;
    return;
  }
  prepare(rgba->pixels, rgba->w, rgba->h);
  // The full 1085px file with no mipmap: drawn at 0.72 it is a mild minification
  // GL_LINEAR handles, and the exit magnifies it, where a mipmap would not help.
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rgba->w, rgba->h, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, rgba->pixels);
  gfx_tex_forget(0);
  SDL_FreeSurface(rgba);
}

int splash_active(void) { return tex && !done; }

static void release(void) {
  if (tex) { glDeleteTextures(1, &tex); gfx_tex_forget(tex); }
  tex = 0;
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

// The lockup, whole, so that its play mark lands centred on (cx, cy) at scale s,
// clipped to the mark's columns. The scissor keeps the wordmark of this copy out.
static void drawMark(float cx, float cy, float s, float a) {
  const float mcx = LOCK_X + MARK_W * 0.5f, mcy = LOCK_Y + LOCK_H * 0.5f;
  GfxRect r = { cx + (LOCK_X - mcx) * s, cy + (LOCK_Y - mcy) * s, LOCK_W * s, LOCK_H * s };
  const float pad = 4.0f * s;
  gfx_crop(r.x - pad, r.y - pad, LOCK_W * MARK_END * s + pad, r.h + pad * 2.0f);
  gfx_logo(r, tex, 0, 1, 1, 1, a);
  gfx_no_crop();
}

// One letter of the wordmark, `p` (0..1) of the way through its exit: it slides
// back into the mark, shrinking a little about its own centre, and fades. Its clip
// moves with it, so it never shows a neighbour's edge.
static void drawLetter(int i, float p, float a) {
  const float k = LOCK_W / 1085.0f;
  const float lx = LOCK_X + (letterCut[i] + letterCut[i + 1]) * 0.5f * k;
  const float ly = LOCK_Y + WORD_MID_Y * k;
  const float e = easeInOut(p), s = 1.0f - 0.12f * e, dx = -46.0f * e;
  const float la = a * (1.0f - easeInOut(clamp01(p * 1.15f)));
  GfxRect r = { lx + dx + (LOCK_X - lx) * s, ly + (LOCK_Y - ly) * s, LOCK_W * s, LOCK_H * s };
  if (la <= 0.002f) return;
  gfx_crop(r.x + letterCut[i] * k * s, r.y,
           (letterCut[i + 1] - letterCut[i]) * k * s + 2.0f, r.h);
  gfx_logo(r, tex, 0, 1, 1, 1, la);
  gfx_no_crop();
}

// The mark's own two colours, as light behind it: the cyan of its top edge up
// and to the left, the violet of its body down and to the right. The plates sit
// on the triangle's centroid, a third of the way in from its flat side.
static void drawGlow(float cx, float cy, float s, float a, float bloom) {
  const float d = MARK_W * 0.55f * s, blur = (120.0f + 170.0f * bloom) * s;
  const float gx = cx - MARK_W * 0.08f * s;
  if (a <= 0.002f) return;
  gfx_glow((GfxRect){ gx - d * 0.62f, cy - d * 0.62f, d, d }, d * 0.5f, blur,
           0.25f, 0.85f, 0.95f, a * 0.65f);
  gfx_glow((GfxRect){ gx - d * 0.38f, cy - d * 0.38f, d, d }, d * 0.5f, blur,
           0.68f, 0.30f, 0.95f, a * 0.85f);
}

void splash_draw(Uint32 now, int ready) {
  float t, tb, s = 1.0f, dx, glow = 0.0f, bloom = 0.0f, markA = 1.0f, bgA = 1.0f;
  float cx, cy;
  int i;
  if (!splash_active()) return;
  if (!t0) {
    t0 = now;
    // Reduced motion: the logo stays put and the app fades up through it.
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
  tb = exitAt ? (float)(exitAt - t0) : t;
  if (quiet) tb = 0.0f;

  dx = (NV_SCREEN_W * 0.5f - (LOCK_X + MARK_W * 0.5f)) *
       easeInOut(phase(tb, T_GLIDE_FROM, T_GLIDE_END));
  // The press: down to 0.9, then back on a damped spring that overshoots once.
  if (tb >= T_GLIDE_END && tb < T_PRESS_END) {
    s = 1.0f - 0.1f * easeOut(phase(tb, T_GLIDE_END, T_PRESS_END));
  } else if (tb >= T_PRESS_END) {
    float p = phase(tb, T_PRESS_END, T_REST);
    s = p >= 1.0f ? 1.0f : 1.0f - 0.1f * expf(-4.0f * p) * cosf(3.0f * 3.14159265f * p);
    // The burst rides the release: up in 80 ms, out over the next half second.
    { float k = (tb - T_PRESS_END) / 300.0f;
      glow = clamp01((tb - T_PRESS_END + 40.0f) / 80.0f) * expf(-k);
      bloom = clamp01(k * 0.6f); }
  }
  // Waiting for the app: the light breathes, so a slow catalogue does not look
  // like a frozen frame.
  if (!quiet && tb >= T_REST) {
    float w = clamp01((tb - T_REST) / 400.0f);
    float b = 0.30f + 0.12f * sinf((tb - T_REST) * 0.0045f - 1.5707963f);
    if (b * w > glow) glow = b * w;
    s *= 1.0f + 0.012f * w * sinf((tb - T_REST) * 0.0045f);
  }

  if (exitAt) {
    float e = clamp01((float)(now - exitAt) / (float)exitLen);
    // Through the mark: it rushes at the camera and is mostly gone by the time
    // the app comes up from behind it, so it never sits over the home as a big
    // play button.
    if (!quiet) s *= 1.0f + 3.4f * e * e;
    markA = 1.0f - easeInOut(clamp01((e - 0.1f) / 0.55f));
    glow *= markA;
    bgA = 1.0f - easeInOut(clamp01((e - 0.25f) / 0.75f));
    if (e >= 1.0f) {
      release();
      printf("[splash] done after %u ms\n", (unsigned)(now - t0));
      fflush(stdout);
      return;
    }
  }

  cx = LOCK_X + MARK_W * 0.5f + dx;
  cy = LOCK_Y + LOCK_H * 0.5f;
  gfx_tex_aspect_current = 0.0f;
  gfx_color((GfxRect){ 0, 0, NV_SCREEN_W, NV_SCREEN_H }, 0.0f, NV_COLOR_BACKGROUND_R,
            NV_COLOR_BACKGROUND_G, NV_COLOR_BACKGROUND_B, bgA);
  drawGlow(cx, cy, s, glow, bloom);
  // The wordmark leaves letter by letter, the one nearest the mark first, as if
  // the mark were drawing it in.
  for (i = 0; i < 5; i++)
    drawLetter(i, phase(tb, T_HOLD + i * T_STAGGER, T_HOLD + i * T_STAGGER + T_LETTER), markA);
  drawMark(cx, cy, s, markA);
}
