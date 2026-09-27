// The sprite-sheet cell GFX_CARD samples through gfx_tex_cell_current.
//
// Written for the collection tile's old focus sheet, which is gone (the tile now
// plays the ident on the video plane); the uniform it pins down is still what
// GFX_SCRIM_HOLE reads, so the test stays.
//
// This is the one genuinely new piece of RENDERING in that feature: GFX_CARD
// gained a uCell uniform, and everything downstream assumes cell N of an 8x8
// sheet is the Nth frame in READING ORDER. Getting the offset or the scale wrong
// neither crashes nor looks obviously broken — it plays the wrong frames, or one
// frame with its neighbours smeared into it, which is the sort of defect that
// reaches the TV.
//
// THE SHEET IS BUILT HERE rather than taken from the asset repo. A real ident
// was tried first and made a poor test twice over: its frames live in another
// repository, and consecutive frames of a slow, dark clip are identical to
// within the noise of two different resamplings, so "which frame is this"
// stopped being answerable for a dozen of the 64. A synthetic sheet gives every
// cell a colour that no other cell has and a bright corner that no rotation or
// flip preserves, which is what turns the check into an equality instead of a
// nearest-match tournament.
//
// It already caught one: the uCell default was cached but never uploaded, so the
// shader sampled texel (0,0) and every card drew as one flat colour.
#include "gfx.h"
#include "tex_cache.h"
#include "layout.h"
#include <SDL2/SDL.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COLS 8
#define ROWS 8
#define CELL_W 240
#define CELL_H 136
// The rect drawn into. Its aspect equals the sheet's, and the grid is square, so
// a cell has that same aspect too — the shader's cover crop is the identity for
// both draws and one rect serves them.
#define RX 100.0f
#define RY 100.0f
#define RW 1440.0f
#define RH 816.0f

static unsigned char *frame;   // 1920x1080 RGBA, bottom-up as GL returns it

// Cell i's base colour. Four levels per channel gives exactly 64 combinations,
// every pair at least 60 apart in some channel — far outside anything filtering
// can move them.
//
// The top level is 200 and not 230 so that adding MARK stays inside a byte. At
// 230 the marked quarter overflowed and WRAPPED to 19, which read as the wrong
// cell entirely and cost a debugging pass on code that was correct.
static void colourOf(int i, int *r, int *g, int *b) {
  *r = (i % 4) * 60 + 20;
  *g = ((i / 4) % 4) * 60 + 20;
  *b = ((i / 16) % 4) * 60 + 20;
}
#define MARK 45   // how much brighter the cell's top-left quarter is

static void buildSheet(const char *path) {
  SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, COLS*CELL_W, ROWS*CELL_H, 32,
                                                  SDL_PIXELFORMAT_RGBA32);
  int i, x, y;
  assert(s);
  for (i = 0; i < COLS*ROWS; i++) {
    int r, g, b, ox = (i % COLS) * CELL_W, oy = (i / COLS) * CELL_H;
    colourOf(i, &r, &g, &b);
    for (y = 0; y < CELL_H; y++) for (x = 0; x < CELL_W; x++) {
      // The bright quarter is at the cell's TOP LEFT. A mapping that flipped y,
      // swapped the axes or transposed the grid would still produce the right
      // colour and would be caught only by this.
      int lit = (x < CELL_W/2 && y < CELL_H/2) ? MARK : 0;
      unsigned char *p = (unsigned char *)s->pixels + (size_t)(oy+y)*s->pitch + (size_t)(ox+x)*4;
      p[0] = (unsigned char)(r + lit); p[1] = (unsigned char)(g + lit);
      p[2] = (unsigned char)(b + lit); p[3] = 255;
    }
  }
  assert(SDL_SaveBMP(s, path) == 0);
  SDL_FreeSurface(s);
}

// Mean colour of a rectangle, in the top-left coordinates gfx draws in.
static void mean(float x, float y, float w, float h, double *out) {
  int px, py, n = 0;
  double s[3] = {0, 0, 0};
  // Inset so the sample never straddles a boundary, where filtering legitimately
  // mixes neighbours.
  x += w * 0.15f; y += h * 0.15f; w *= 0.7f; h *= 0.7f;
  for (py = (int)y; py < (int)(y + h); py++) {
    int gy = 1079 - py;                    // glReadPixels returns bottom-up
    if (gy < 0 || gy >= 1080) continue;
    for (px = (int)x; px < (int)(x + w); px++) {
      const unsigned char *p;
      if (px < 0 || px >= 1920) continue;
      p = frame + ((size_t)gy * 1920 + px) * 4;
      s[0] += p[0]; s[1] += p[1]; s[2] += p[2]; n++;
    }
  }
  assert(n > 0);
  out[0] = s[0]/n; out[1] = s[1]/n; out[2] = s[2]/n;
}

static void draw(GLuint tex, float texAspect, GfxRect cell) {
  GfxRect r = {RX, RY, RW, RH};
  glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
  gfx_tex_aspect_current = texAspect;
  gfx_tex_cell_current = cell;
  // radius 0: a rounded corner discards pixels, and the corner samples would
  // then be taken partly over the background.
  gfx_rect(r, tex, GFX_CARD, 0, 0, 0, 0.0f, 0, 0, 0, 1);
  gfx_tex_cell_current = (GfxRect){0, 0, 1, 1};
  gfx_tex_aspect_current = 0;
  glReadPixels(0, 0, 1920, 1080, GL_RGBA, GL_UNSIGNED_BYTE, frame);
}

int main(void) {
  const char *path = "/tmp/nuvio-focus-sheet-test.bmp";
  SDL_Window *win; SDL_GLContext gl;
  GLuint tex = 0;
  int i, n = COLS * ROWS;
  float sheetAspect;

  assert(SDL_Init(SDL_INIT_VIDEO) == 0);
  SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
  win = SDL_CreateWindow("focus sheet", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                         1920, 1080, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
  assert(win);
  gl = SDL_GL_CreateContext(win); assert(gl);
  glViewport(0, 0, 1920, 1080);
  gfx_size_target(0, 0, 1920, 1080);
  assert(gfx_start());
  tex_start(16);
  frame = malloc((size_t)1920*1080*4); assert(frame);
  buildSheet(path);

  // 1920 is what home.c asks for, and it is what keeps the cells on their grid:
  // tex_get_width caps the DECODE width, so a sheet fetched at the tile's own 360
  // would have cells four pixels across.
  for (i = 0; i < 600 && !tex; i++) { tex_pump(4); tex = tex_get_width(path, 1920.0f); SDL_Delay(10); }
  assert(tex && "sheet did not load");
  sheetAspect = tex_aspect(path);

  for (i = 0; i < n; i++) {
    GfxRect cell;
    double all[3], tl[3], br[3];
    int r, g, b, k;
    colourOf(i, &r, &g, &b);
    cell.x = (float)(i % COLS) / COLS;
    cell.y = (float)(i / COLS) / ROWS;
    cell.w = 1.0f / COLS;
    cell.h = 1.0f / ROWS;
    draw(tex, sheetAspect * (float)ROWS / (float)COLS, cell);

    // THE RIGHT CELL: the colour belongs to frame i and to no other frame.
    mean(RX, RY, RW, RH, all);
    for (k = 0; k < 3; k++) {
      double want = (k == 0 ? r : k == 1 ? g : b) + MARK / 4.0;
      if (all[k] < want - 6.0 || all[k] > want + 6.0) {
        printf("cell %d channel %d: %.1f, expected %.1f\n", i, k, all[k], want);
        assert(0);
      }
    }
    // THE RIGHT WAY UP: the bright quarter has to land top left. Colour alone
    // would pass a mapping that flipped the cell inside itself.
    mean(RX, RY, RW/2, RH/2, tl);
    mean(RX + RW/2, RY + RH/2, RW/2, RH/2, br);
    assert(tl[0] > br[0] + MARK*0.7 && tl[1] > br[1] + MARK*0.7 && tl[2] > br[2] + MARK*0.7);
  }

  // THE RESET. gfx_tex_cell_current is a global set before a draw, so the real
  // hazard is not the arithmetic but forgetting to put it back: every card drawn
  // after the animating tile would be cropped to one frame of its animation.
  // After 64 cell draws the default has to give the whole sheet again — whose
  // mean is the mean of all 64 cells.
  { double all[3]; double want[3] = {0, 0, 0};
    draw(tex, sheetAspect, (GfxRect){0, 0, 1, 1});
    mean(RX, RY, RW, RH, all);
    for (i = 0; i < n; i++) {
      int r, g, b; colourOf(i, &r, &g, &b);
      want[0] += r; want[1] += g; want[2] += b;
    }
    for (i = 0; i < 3; i++) {
      double w = want[i]/n + MARK/4.0;
      if (all[i] < w - 6.0 || all[i] > w + 6.0) {
        printf("after reset, channel %d: %.1f, expected %.1f\n", i, all[i], w);
        assert(0);
      }
    } }

  printf("focus sheet: PASS (%dx%d, all %d cells by colour and orientation, "
         "cell reset clean)\n", COLS, ROWS, n);
  tex_shutdown();
  SDL_GL_DeleteContext(gl); SDL_DestroyWindow(win); SDL_Quit();
  return 0;
}
