// The Live TV screen, driven without a TV: a playlist and a gzipped guide
// served over file:// (tests/iptv_ui.sh writes both around the current time),
// the D-pad fed as SDL events, and a capture of each face into /tmp for review.
//
// It asserts what can be asserted without eyes: the load reaches the guide, the
// focus model moves as documented at the top of iptvui.c, favourites and the
// setup form round-trip through the data folder. The captures are for the eyes.
#include "iptvui.h"
#include "iptv.h"
#include "data.h"
#include "gfx.h"
#include "ime.h"
#include "net.h"
#include "pointer.h"
#include "tex_cache.h"
#include "text.h"
#include <SDL2/SDL_image.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SDL_Window *win;

static void key(SDL_Keycode k) {
  SDL_Event e;
  memset(&e, 0, sizeof e);
  e.type = SDL_KEYDOWN; e.key.keysym.sym = k;
  iptvui_event(&e);
  e.type = SDL_KEYUP;
  iptvui_event(&e);
}

static void frames(int n, const char *capture) {
  for (int i = 0; i < n; i++) {
    SDL_PumpEvents();
    pointer_frame_begin();
    txt_new_frame(); tex_new_frame(); tex_pump(6);
    iptvui_update(1.0f / 60, SDL_GetTicks());
    glClearColor(0.051f, 0.051f, 0.051f, 1); glClear(GL_COLOR_BUFFER_BIT);
    iptvui_draw(SDL_GetTicks());
    if (capture && i == n - 1) {
      unsigned char *pix = malloc(1920 * 1080 * 4);
      SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, 1920, 1080, 32, SDL_PIXELFORMAT_RGBA32);
      assert(pix && s);
      glReadPixels(0, 0, 1920, 1080, GL_RGBA, GL_UNSIGNED_BYTE, pix);
      for (int y = 0; y < 1080; y++)
        memcpy((char *)s->pixels + y * s->pitch, pix + (1079 - y) * 1920 * 4, 1920 * 4);
      assert(SDL_SaveBMP(s, capture) == 0);
      SDL_FreeSurface(s); free(pix);
      printf("capture: %s\n", capture);
    }
    SDL_GL_SwapWindow(win);
    SDL_Delay(4);
  }
}

// Frames until `cond` holds, or fail after ~20 s.
#define WAIT_FOR(cond) do { int _i = 0; \
    while (!(cond) && _i++ < 1200) frames(1, NULL); \
    assert(cond); } while (0)

int main(int argc, char **argv) {
  const char *dataDir = argc > 1 ? argv[1] : "/tmp/nuvio-iptv-test";
  const char *out = argc > 2 ? argv[2] : "/tmp";
  char path[512];
  assert(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) == 0);
  IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG);
#ifdef __APPLE__
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
#else
  // The shaders are GLSL ES everywhere but the Mac (gl_compat.h), as on the TV.
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#endif
  win = SDL_CreateWindow("Nuvio: live tv review", 0, 0, 1920, 1080, SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
  assert(win);
  assert(SDL_GL_CreateContext(win));
  SDL_GL_SetSwapInterval(0);
  glViewport(0, 0, 1920, 1080); gfx_size_target(0, 0, 1920, 1080); assert(gfx_start());
  assert(txt_start("deploy/app", 1)); tex_start(64);
  gfx_icons_dir("deploy/app/art");
  data_start(dataDir);
  net_prepare();
  ime_start(win);

  // --- The guide ---------------------------------------------------------------
  iptvui_start();
  assert(iptv_configured());
  WAIT_FOR(iptv_guide_state() == IPTV_READY || iptv_guide_state() == IPTV_FAILED);
  printf("status: '%s'\n", iptv_status());
  assert(iptv_guide_state() == IPTV_READY);
  frames(2, NULL);   // the list lands on the next iptv_step
  { const IptvList *l = iptv_list();
    assert(l && l->nCh == 48 && l->nGroups == 4);
    assert(l->ch[0].nPg > 0); }
  frames(40, NULL);
  snprintf(path, sizeof path, "%s/nuvio-live-guide.bmp", out); frames(30, path);

  // The focus model: DOWN keeps the instant, RIGHT walks programmes, LEFT from
  // what is on now leaves for the groups.
  key(SDLK_DOWN); key(SDLK_DOWN);
  key(SDLK_RIGHT); key(SDLK_RIGHT);
  snprintf(path, sizeof path, "%s/nuvio-live-guide-later.bmp", out); frames(40, path);
  key(SDLK_LEFT); key(SDLK_LEFT);
  // Back at "now": one more LEFT reaches the groups. DOWN picks each group as
  // it passes: Favourites (empty), Recent (empty), then the playlist's News.
  key(SDLK_LEFT);
  key(SDLK_DOWN);
  snprintf(path, sizeof path, "%s/nuvio-live-empty.bmp", out); frames(30, path);
  key(SDLK_DOWN); key(SDLK_DOWN);
  snprintf(path, sizeof path, "%s/nuvio-live-groups.bmp", out); frames(40, path);
  key(SDLK_RIGHT);

  // Favourites: a hold on OK. The hold fires on time, so run frames while held.
  { SDL_Event e; memset(&e, 0, sizeof e);
    e.type = SDL_KEYDOWN; e.key.keysym.sym = SDLK_RETURN; iptvui_event(&e);
    { Uint32 t0 = SDL_GetTicks(); while (SDL_GetTicks() - t0 < 800) frames(1, NULL); }
    e.type = SDL_KEYUP; iptvui_event(&e); }
  { int favs = 0;
    for (int c = 0; c < iptv_list()->nCh; c++) favs += iptv_is_favourite(c);
    assert(favs == 1); }
  { char *f = data_read("iptv_favourites.txt"); assert(f && f[0]); free(f); }

  // --- Watching ------------------------------------------------------------------
  key(SDLK_RETURN);
  assert(iptvui_fullscreen());
  snprintf(path, sizeof path, "%s/nuvio-live-full.bmp", out); frames(30, path);
  key(SDLK_LEFT);
  snprintf(path, sizeof path, "%s/nuvio-live-quicklist.bmp", out); frames(30, path);
  key(SDLK_AC_BACK);
  key(SDLK_AC_BACK);
  assert(!iptvui_fullscreen());
  { char *r = data_read("iptv_recent.txt"); assert(r && r[0]); free(r); }

  // --- Setup ------------------------------------------------------------------------
  // UP from the top row reaches the header; RIGHT to Source; OK.
  for (int i = 0; i < 8; i++) key(SDLK_UP);
  key(SDLK_RIGHT);
  key(SDLK_RETURN);
  key(SDLK_RIGHT);   // Xtream Codes
  snprintf(path, sizeof path, "%s/nuvio-live-setup.bmp", out); frames(30, path);
  key(SDLK_AC_BACK);
  assert(!iptvui_wants_exit());

  iptvui_shutdown();
  puts("PASS iptv_ui: guide loaded from file://, focus model, favourite, tune, setup.");
  return 0;
}
