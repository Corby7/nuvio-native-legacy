// Bootstrap: window, GL context, loop and telemetry. All the UI lives in the modules.
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include "gl_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "gfx.h"
#include "text.h"
#include "mark.h"
#include "net.h"
#include "tex_cache.h"
#include "home.h"
#include "text.h"
#include "detail.h"
#include "data.h"
#include "cloud.h"
#include "session.h"
#include "profiles.h"
#include "sync.h"
#include "traktauth.h"
#include "simklauth.h"
#include "app.h"
#include "video.h"
#include "plane.h"
#include "addons.h"
#include "settings.h"
#include "discover.h"
#include "trakt.h"
#include "player.h"
#ifndef __APPLE__
#include <dlfcn.h>
#include <SDL2/SDL_syswm.h>
#endif
#include "layout.h"

// Screenshots on demand. The TV's framebuffer cannot be read even as root
// ("Operation not permitted") and LG's capture service answers with an error, so
// the only way to see what the app draws is for the app to photograph itself.
// Without this, every visual tweak depends on somebody pointing a phone at the TV.
//
// Protocol: someone creates /tmp/nuvio-shot-req; on the next frame the app writes
// /tmp/nuvio-shot.png and deletes the request.
// Keys injected from a file, so the UI can be checked without somebody on the
// sofa with the remote: write "down", "ok", "back"... into /tmp/nuvio-key and the
// app handles it as though it came from the D-pad. One key per line; the file is
// consumed.
static SDL_Keycode codeOfKey(const char *name) {
  if (!strcmp(name, "up"))    return SDLK_UP;
  if (!strcmp(name, "down"))  return SDLK_DOWN;
  if (!strcmp(name, "left"))  return SDLK_LEFT;
  if (!strcmp(name, "right")) return SDLK_RIGHT;
  if (!strcmp(name, "ok"))    return SDLK_RETURN;
  if (!strcmp(name, "back"))  return SDLK_AC_BACK;
  return 0;
}

// The file is CONSUMED by truncating, never by deleting: /tmp has the sticky bit
// and the files are created by root, so the app (uid 5410) cannot remove them.
// Until that was noticed, every request was reprocessed on every frame — a single
// "down" key became hundreds and the focus ran to the end of the page.
static void consume(const char *path) {
  FILE *f = fopen(path, "w");
  if (f) fclose(f);
}

// Is the request NEW? A guard against the file that cannot be consumed.
//
// `consume` empties it by opening with "w" instead of deleting, precisely because
// of /tmp's sticky bit. But that also fails when the file belongs to ANOTHER
// user: a request created over ssh as root ends up 644, and the app (uid 5152)
// can neither delete nor truncate it. The request then holds forever.
//
// MEASURED on the owner's TV, and I caused it: a /tmp/nuvio-shot-req left behind
// as root made the app capture the whole screen (a 1920x1080 glReadPixels plus
// 8 MB written) ON EVERY FRAME for hours — `aux` went from 0.0 to 100.7 ms and
// the app dropped from 60 to 9 fps. What came back was "the interface is sluggish".
//
// Comparing the modification time solves it without depending on writing: a
// request that has not changed since it was last served is not a new request.
// The time is only consulted when the consumption FAILS, and not always: it has
// one-second resolution, and two bursts of keys in the same second would be
// treated as one. On the normal path (a file the app owns) the consumption works
// and none of this comes into play.
static int requestNew(const char *path, time_t *blocked) {
  struct stat st;
  if (stat(path, &st) != 0 || st.st_size <= 0) return 0;
  // A request that has already been served and cannot be emptied: ignore it until
  // it changes. Without this it holds forever and the work is redone per frame.
  if (*blocked && st.st_mtime == *blocked) return 0;
  *blocked = 0;
  return 1;
}

// Empties it and checks. Returns 0 when it did NOT manage — a different owner,
// the sticky bit — and in that case marks the request to be ignored until its
// time changes.
static int consumeOrBlocks(const char *path, time_t *blocked) {
  struct stat st;
  consume(path);
  if (stat(path, &st) == 0 && st.st_size > 0) {
    *blocked = st.st_mtime;
    printf("[main] %s cannot be consumed (different owner); ignoring\n",
           path);
    fflush(stdout);
    return 0;
  }
  return 1;
}
// stat() and not fopen+fseek: this probe runs for THREE files on EVERY frame, and
// each fopen pays a FILE allocation and two extra syscalls just to find the size.
// stat answers the same question with one syscall.
static long sizeOf(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0) return -1;
  return (long)st.st_size;
}

// A deferred KEYUP for a held key.
static Uint32 releaseIn = 0;
static SDL_Keycode releaseKey = 0;

static void keysInjected(void (*deliver)(const SDL_Event *)) {
  if (releaseIn && SDL_GetTicks() >= releaseIn) {
    SDL_Event up; SDL_zero(up);
    up.type = SDL_KEYUP; up.key.keysym.sym = releaseKey;
    deliver(&up);
    releaseIn = 0;
  }
  static time_t blockedKey;
  if (!requestNew("/tmp/nuvio-key", &blockedKey)) return;
  FILE *f = fopen("/tmp/nuvio-key", "r");
  if (!f) return;
  char line[32];
  while (fgets(line, sizeof line, f)) {
    char *end = line + strlen(line);
    while (end > line && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ')) *--end = 0;
    // "ok:hold" simulates the long press: its KEYUP is scheduled for after the
    // threshold, instead of coming along with it. Without this there is no way to
    // exercise anything here that depends on holding the button.
    int hold = 0;
    char *dp = strchr(line, ':');
    if (dp && !strcmp(dp + 1, "hold")) { *dp = 0; hold = 1; }

    SDL_Keycode k = codeOfKey(line);
    if (!k) continue;
    SDL_Event e; SDL_zero(e);
    e.type = SDL_KEYDOWN; e.key.keysym.sym = k;
    deliver(&e);

    // The KEYUP pair exists because part of the interface only decides when the
    // key GOES UP — the short press against the long press of OK, for example.
    // Sending only the KEYDOWN left those actions mute.
    if (hold) { releaseIn = SDL_GetTicks() + NV_HOLD_MS + 120; releaseKey = k; }
    else { e.type = SDL_KEYUP; deliver(&e); }
  }
  fclose(f);
  consumeOrBlocks("/tmp/nuvio-key", &blockedKey);
}

// --- GOTO: reach a screen without driving the arrows -------------------------
//
// Write an IMDb id into /tmp/nuvio-goto and the app opens that title's detail.
// Same protocol as the key and capture files, and it exists for the same reason
// they do: without it, reaching one screen is a blind sequence of injected arrows
// followed by a screenshot to find out where it landed, and the app restores a
// different focus on every launch so the sequence is not repeatable.
//
// IT RETRIES RATHER THAN CONSUMING ON FAILURE. On a cold start the catalogue is
// still arriving and the id resolves to nothing; consuming the request there
// would silently do nothing at all, which is the failure mode this whole channel
// exists to avoid. It holds for ~8s, then gives up and says so.
static void gotoIfRequested(void) {
  static time_t blocked;
  static Uint32 since;
  char id[32];
  FILE *f;
  if (!requestNew("/tmp/nuvio-goto", &blocked)) { since = 0; return; }
  f = fopen("/tmp/nuvio-goto", "r");
  if (!f) return;
  if (!fgets(id, sizeof id, f)) { id[0] = 0; }
  fclose(f);
  { char *e = id + strlen(id);
    while (e > id && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0; }
  if (!id[0]) { consumeOrBlocks("/tmp/nuvio-goto", &blocked); return; }
  if (!since) since = SDL_GetTicks();
  if (app_goto_detail(id)) {
    consumeOrBlocks("/tmp/nuvio-goto", &blocked);
    since = 0;
    return;
  }
  if (SDL_GetTicks() - since > 8000) {
    printf("[goto] %s is not in the catalogue\n", id);
    fflush(stdout);
    consumeOrBlocks("/tmp/nuvio-goto", &blocked);
    since = 0;
  }
}

// WHERE THE INTERFACE IS, printed only when it CHANGES. A screenshot costs a
// megapixel to answer a question a line of text answers; this is what makes
// "which screen am I on" a grep.
static void whereIfChanged(void) {
  static char last[64];
  char now[64];
  app_where(now, sizeof now);
  if (!strcmp(now, last)) return;
  snprintf(last, sizeof last, "%s", now);
  printf("[nav] %s\n", now);
  fflush(stdout);
}

// The size of the buffer the capture reads from. Set at startup, alongside the
// viewport.
static int capX = 0, capY = 0;
static int capW = (int)NV_SCREEN_W, capH = (int)NV_SCREEN_H;

// The same protocol as the other tools: write a URL into /tmp/nuvio-video and the
// app plays it. It is the only way to test playback without somebody on the sofa
// — and the video cannot be checked by screenshot, because it lives in another plane.
static void videoIfRequested(void) {
  static time_t blocked;
  char url[1024];
  FILE *f;
  if (!requestNew("/tmp/nuvio-video", &blocked)) return;
  f = fopen("/tmp/nuvio-video", "r");
  if (!f) return;
  if (fgets(url, sizeof url, f)) {
    char *end = url + strlen(url);
    while (end > url && (end[-1] == '\n' || end[-1] == '\r')) *--end = 0;
    printf("[video] request: %s\n", url);
    fflush(stdout);
    if (url[0] == '-') video_stop();
    else { video_play(url); video_window(0, 0, 1920, 1080); }
  }
  fclose(f);
  consumeOrBlocks("/tmp/nuvio-video", &blocked);
}

// The same protocol as /tmp/nuvio-video, for the CROP: write eight numbers
//
//   sx sy sw sh dx dy dw dh
//
// and the app sends that source/destination pair to the plane. It exists because
// the crop is the one part of video that CANNOT be checked from the pipeline's
// log: the uMS does not know a crop happened — the compositor does — and the
// plane cannot be photographed. Without this, the only way to test the aspect
// modes is to ask someone to look at the TV and describe what they see.
static void rectIfRequested(void) {
  static time_t blocked;
  char line[256];
  FILE *f;
  if (!requestNew("/tmp/nuvio-rect", &blocked)) return;
  f = fopen("/tmp/nuvio-rect", "r");
  if (!f) return;
  if (fgets(line, sizeof line, f)) {
    int sx, sy, sw, sh, dx, dy, dw, dh;
    if (sscanf(line, "%d %d %d %d %d %d %d %d",
               &sx, &sy, &sw, &sh, &dx, &dy, &dw, &dh) == 8) {
      printf("[video] rect request: source %d,%d %dx%d -> destination %d,%d %dx%d\n",
             sx, sy, sw, sh, dx, dy, dw, dh);
      fflush(stdout);
      video_window_source(sx, sy, sw, sh, dx, dy, dw, dh);
    } else {
      printf("[video] rect request: expected 8 numbers\n");
      fflush(stdout);
    }
  }
  fclose(f);
  consumeOrBlocks("/tmp/nuvio-rect", &blocked);
}

// THE LETTERBOX. The layout is authored at 1920x1080 and the shader maps it
// onto whatever the viewport is (uScreen stays 1920x1080, gfx.c), so any surface
// size already works — but only a 16:9 one works WITHOUT DISTORTION. The TV is
// 16:9 and this returns the whole drawable there. A Mac window is not: 16:10
// built in, and anything at all once it can be dragged.
//
// Bars instead of a stretch because this window's job is to stand in for the
// TV. A 3% vertical stretch is invisible until you are holding a screenshot
// next to the reference and every measurement is off by 3%.
static void surfaceBox(SDL_Window *win, int *bx, int *by, int *bw, int *bh) {
  int dw = 0, dh = 0;
  SDL_GL_GetDrawableSize(win, &dw, &dh);
  if (dw < 1) dw = 1;
  if (dh < 1) dh = 1;
  int w = dw, h = (int)(dw * (NV_SCREEN_H / NV_SCREEN_W) + 0.5f);
  if (h > dh) { h = dh; w = (int)(dh * (NV_SCREEN_W / NV_SCREEN_H) + 0.5f); }
  *bw = w; *bh = h;
  *bx = (dw - w) / 2;
  *by = (dh - h) / 2;
}

// Applied at startup and on every SDL_WINDOWEVENT_SIZE_CHANGED. Without the
// second call the app keeps drawing into the box it was born with: go
// fullscreen and the frame stays in a corner at its old size.
static void applySurface(SDL_Window *win) {
  int bx, by, bw, bh;
  surfaceBox(win, &bx, &by, &bw, &bh);
  glViewport(bx, by, bw, bh);
  gfx_size_target(bx, by, bw, bh);
  capX = bx; capY = by; capW = bw; capH = bh;
}

static void captureIfRequested(void) {
  static time_t blocked;
  if (!requestNew("/tmp/nuvio-shot-req", &blocked)) return;
  consumeOrBlocks("/tmp/nuvio-shot-req", &blocked);

  // Reads the WHOLE drawable, not a fixed 1920x1080: on a retina screen the
  // buffer is larger than the window, and reading the window's size captures only
  // a quarter of the image.
  int w = capW, h = capH;
  size_t n = (size_t)w * h * 4;
  unsigned char *px = malloc(n);
  if (!px) return;
  // From the box's corner, not the buffer's: on a window that is not 16:9 the
  // bars would otherwise be baked into every screenshot.
  glReadPixels(capX, capY, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);

  // A BMP written by hand, in ONE fwrite. SDL_SaveBMP converts pixel by pixel
  // when the masks do not match the native format, and on this CPU that takes
  // seconds: the file was incomplete by the time I went to read it. Here the only
  // conversion is the R<->B swap, done in the buffer itself.
  for (size_t i = 0; i < n; i += 4) { unsigned char t2 = px[i]; px[i] = px[i+2]; px[i+2] = t2; }

  unsigned int size = 54 + (unsigned int)n;
  unsigned char header[54] = {0};
  header[0] = 'B'; header[1] = 'M';
  header[2] = size & 255; header[3] = (size >> 8) & 255; header[4] = (size >> 16) & 255; header[5] = (size >> 24) & 255;
  header[10] = 54; header[14] = 40;
  header[18] = w & 255; header[19] = (w >> 8) & 255;
  // a POSITIVE height = rows bottom-to-top, which is exactly the order
  // glReadPixels returns. So there is no flip to do.
  header[22] = h & 255; header[23] = (h >> 8) & 255;
  header[26] = 1; header[28] = 32;
  header[34] = n & 255; header[35] = (n >> 8) & 255; header[36] = (n >> 16) & 255; header[37] = (n >> 24) & 255;

  // write to a temporary and only then rename: a reader never picks up a half file
  FILE *f = fopen("/tmp/.nuvio-shot.tmp", "wb");
  if (f) {
    fwrite(header, 1, 54, f);
    fwrite(px, 1, n, f);
    fclose(f);
    rename("/tmp/.nuvio-shot.tmp", "/tmp/nuvio-shot.bmp");
    printf("capture: /tmp/nuvio-shot.bmp (%u bytes)\n", size);
  }
  free(px);
}

int main(int argc, char **argv) {
  // Without the app's identity, webOS's SDL registers the surface as "(null)" and
  // the compositor does NOT show the window — the app runs at 60fps drawing for
  // nobody. Measured: "Invalid appId specified OR Unsupported Application Type".
#ifndef __APPLE__
  setenv("APPID", "space.nuvio.native.legacy", 0);
  setenv("LS2_APPID", "space.nuvio.native.legacy", 0);
  setenv("SDL_VIDEODRIVER", "wayland", 0);
#endif
  // Launched by SAM, stdout and stderr go to /dev/null — all the telemetry (FPS,
  // textures, keys) was being discarded silently. A log file is the only way to
  // read anything out of a native app in normal operation.
#ifndef __APPLE__
  freopen("/tmp/nuvio.log", "w", stdout);
  freopen("/tmp/nuvio.log", "a", stderr);
#endif
  setvbuf(stdout, NULL, _IOLBF, 0);
  // WHICH BUILD IS RUNNING. The file's md5 proves what is on DISK; this line
  // proves what was LAUNCHED, which is a different question — and the one that
  // already cost 2h30 reading the log of an old process while believing the
  // deploy had landed. Guarded by #ifdef because mac.sh defines none of this.
#ifdef NV_BUILD
  printf("[main] build %s\n", NV_BUILD);
#endif
  if (!getenv("XDG_RUNTIME_DIR")) setenv("XDG_RUNTIME_DIR", "/tmp/xdg", 1);

  // SAM launches the app passing the launch JSON as argv[1], so we only treat
  // argv[1] as a path when it is NOT JSON.
  char dirBuf[512];
  const char *dirArt = NULL;
  if (argc > 1 && argv[1][0] != '{') dirArt = argv[1];
  if (!dirArt) {
    char *base = SDL_GetBasePath();
    if (base) { snprintf(dirBuf, sizeof dirBuf, "%sart", base); SDL_free(base); dirArt = dirBuf; }
    else dirArt = "/tmp/art";
  }

  // The webOS compositor swallows BACK and opens the app bar — unless the surface
  // declares that the app wants the key. What makes that declaration is LG's SDL
  // Wayland backend, through this hint, and it is only read when the window is
  // CREATED: setting it afterwards is no use.
  //
  // With the hint on, Back arrives as a webOS-specific scancode (482), and not as
  // SDLK_AC_BACK nor as the 461 of web apps. That is why logging every SDL event
  // showed nothing: the key was never delivered, and the code it uses was not the
  // one I was looking for either.
  SDL_SetHint("SDL_WEBOS_ACCESS_POLICY_KEYS_BACK", "true");

  if (SDL_Init(SDL_INIT_VIDEO) != 0) { printf("SDL_Init: %s\n", SDL_GetError()); return 1; }
  // THE FLAG IS ASKED FOR AND THE C3 REFUSES IT. Kept deliberately, with the
  // measurement, so nobody spends the afternoon discovering this twice.
  //
  // MEASURED on the C3 (webOS 23) with a test binary run on the device:
  //     IMG_Init -> 0x3  JPG:yes PNG:yes WEBP:NO
  //     IMG_GetError: 'WEBP images are not supported'
  //     IMG_isWEBP on a real WebP: 0
  // The TV's libSDL2_image EXPORTS IMG_LoadWEBP_RW and IMG_isWEBP, and
  // /usr/lib/libwebp.so.7 is installed — but the library has no reference to
  // libwebp at all and those symbols are stubs. Asking for the format is free
  // and would start working on a firmware that ships a real decoder; believing
  // the exported symbol is what costs time.
  //
  // The consequence is not academic: art/badges holds 42 .webp files and they
  // account for most of the "[tex] decode failed (Unsupported image format)"
  // lines, against ZERO network errors. WebP that arrives from the network
  // cannot be drawn either. Fixing it means not handing this TV a WebP.
  IMG_Init(IMG_INIT_JPG | IMG_INIT_PNG | IMG_INIT_WEBP);

#ifdef __APPLE__
  // The compatibility profile: it is the only one on macOS that still accepts
  // GLSL 1.20 and the fixed functions GLES2 has as core.
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
  Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI |
                 SDL_WINDOW_RESIZABLE;
#else
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
  // An alpha channel in the framebuffer. Without it the surface has no way to be
  // transparent, and the device's video plane — which sits BEHIND the window and
  // only shows through the alpha — could never be revealed.
  SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
  Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN;
#endif
  // 4K IS NOT POSSIBLE ON THIS DEVICE — MEASURED, not assumed.
  //
  // The TV is 4K, and the idea (the owner's) was to render at 3840x2160 and draw
  // everything at double size: the text would stop being rasterised at 1080p and
  // enlarged by the panel, which is the blur you see next to the web app.
  //
  // Both routes were tried, on the TV, with the app's own frame counter writing to
  // /tmp/nuvio-fps.txt:
  //   1. SDL_CreateWindow with 3840x2160  -> drawable=1920x1080
  //   2. appinfo.json "resolution": "3840x2160" -> drawable=1920x1080
  // The webOS 4.10 compositor pins a native app's surface at 1080p and ignores
  // both requests, silently. There is nothing to optimise here: the outcome would
  // be the panel receiving 1080p and scaling it, which is what already happens.
  //
  // A baseline for future comparison, measured on this screen (home, not scrolling):
  //   drawable=1920x1080 FPS=50.0 worst=21ms janks=0
  //
  // txt_start still receives the drawable's scale: on the device it is 1 and
  // changes nothing, on the Mac (retina) it is 2 and the preview stops lying.
  int winW = (int)NV_SCREEN_W, winH = (int)NV_SCREEN_H;
#ifdef __APPLE__
  // 1920x1080 is bigger than the screen it has to fit inside. SDL takes those
  // numbers as POINTS, and a 14" MacBook Pro reports 1512x982 of them — the
  // window was 27% too wide before the desk even came into it, so macOS clamped
  // it and the edges of the layout went off-screen.
  //
  // 90% of the usable bounds (which already exclude the menu bar and the Dock),
  // in 16:9, keeps the whole frame on screen with room to grab the title bar.
  // F makes it fullscreen when the point is to look at it rather than work.
  {
    SDL_Rect usable;
    if (SDL_GetDisplayUsableBounds(0, &usable) == 0 &&
        usable.w > 0 && usable.h > 0) {
      float room = 0.9f;
      winW = (int)(usable.w * room);
      winH = (int)(winW * (NV_SCREEN_H / NV_SCREEN_W));
      if (winH > usable.h * room) {
        winH = (int)(usable.h * room);
        winW = (int)(winH * (NV_SCREEN_W / NV_SCREEN_H));
      }
    }
  }
#endif
  SDL_Window *win = SDL_CreateWindow("Nuvio", SDL_WINDOWPOS_CENTERED,
                                     SDL_WINDOWPOS_CENTERED,
                                     winW, winH, flags);
  if (!win) { printf("window: %s\n", SDL_GetError()); return 1; }
  // A TV app has no pointer: the cursor over the interface pollutes the reading
  // and disappears on its own on the device, but not on the Mac.
  SDL_ShowCursor(SDL_DISABLE);
#ifndef __APPLE__
  // Declares the surface NON-opaque. By default the compositor treats the window
  // as opaque and discards the whole alpha channel — the hole from gfx_hole would
  // exist in the framebuffer and still nothing would appear behind it.
  //
  // Two traps measured on this device, both silent:
  // 1. the SDL here writes an SDL_SysWMinfo LARGER than the header declares, so
  //    the struct goes into a generous buffer and not into a variable of the
  //    "right" size;
  // 2. `version` has to come from SDL_GetVersion(); filled in by hand, SDL refuses
  //    silently and the only clue is a black screen.
  {
    static char infoBuf[512];
    SDL_SysWMinfo *info = (SDL_SysWMinfo *)infoBuf;
    SDL_GetVersion(&info->version);
    if (SDL_GetWindowWMInfo(win, info)) {
      // The SDK's SDL_config.h ships with SDL_VIDEO_DRIVER_WAYLAND off, so the
      // info.wl field does not even exist in the header — but the device's SDL IS
      // wayland. Reading by offset avoids depending on a header that describes a
      // different build: version takes 3 bytes (aligned to 4), subsystem comes at
      // 4, and the union starts at 8. For wayland it is {display, surface, ...}.
      int sub = *(int *)(infoBuf + 4);
      void **fields = (void **)(infoBuf + 8);
      void *sup = fields[1];
      void *wl = dlopen("libwayland-client.so.0", RTLD_NOW);
      void (*marshal)(void *, unsigned, ...) =
          wl ? (void (*)(void *, unsigned, ...))dlsym(wl, "wl_proxy_marshal") : NULL;
      printf("syswm sub=%d display=%p surface=%p\n", sub, fields[0], sup);
      // wl_surface's opcode 4 is set_opaque_region; NULL = "nothing is opaque".
      // Deliberately without a commit: the commit comes from the next SwapWindow.
      if (marshal && sup) { marshal(sup, 4, NULL); printf("non-opaque surface\n"); }
      else printf("no wayland: video will not appear\n");
      // Export the surface as a video object NOW, not on the first play: the
      // windowId arrives as an event, and com.webos.media's load cannot go out
      // without it. Discovering that inside the play path would mean either
      // blocking the drawing thread on the compositor or sending a load that
      // fails silently. Here we are still single-threaded, and a roundtrip costs
      // nothing.
      plane_start(fields[0], sup);
    }
  }
#endif
  SDL_GLContext ctx = SDL_GL_CreateContext(win);
#ifdef __APPLE__
  // No vsync on the Mac. Homebrew's SDL2 has become a layer over SDL3
  // (sdl2-compat), and in it SwapWindow blocks waiting for a vsync signal that
  // never arrives when the window is not in the foreground — the app freezes on
  // the first frame. On the device SDL2 is the real one and vsync stays on, which
  // is what keeps 60fps steady there.
  SDL_GL_SetSwapInterval(0);
#else
  SDL_GL_SetSwapInterval(1);
#endif
  // The REAL buffer size matters more than the requested one: this TV is 4K, and
  // if the compositor hands over a 3840x2160 surface, every full-screen layer
  // costs four times what the 1080p arithmetic says.
  int dw = 0, dh = 0, jw = 0, jh = 0;
  SDL_GL_GetDrawableSize(win, &dw, &dh);
  SDL_GetWindowSize(win, &jw, &jh);
  printf("GPU: %s | %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
  printf("window=%dx%d drawable=%dx%d\n", jw, jh, dw, dh);
  // Asking for SDL_GL_ALPHA_SIZE does not guarantee getting it: EGL picks the
  // closest config and may hand over 0 bits of alpha silently. With 0 here, the
  // hole in the surface is impossible and the video plane will NEVER appear,
  // however right the ACB side may be.
  { int a = -1, r = -1, g = -1, b = -1;
    SDL_GL_GetAttribute(SDL_GL_ALPHA_SIZE, &a);
    SDL_GL_GetAttribute(SDL_GL_RED_SIZE, &r);
    SDL_GL_GetAttribute(SDL_GL_GREEN_SIZE, &g);
    SDL_GL_GetAttribute(SDL_GL_BLUE_SIZE, &b);
    printf("framebuffer R%d G%d B%d A%d%s\n", r, g, b, a,
           a > 0 ? "" : "  <<< NO ALPHA: video has no way to show through"); }

  // On a retina screen the drawable is larger than the window; without adjusting
  // the viewport, the drawing occupies a quarter of the screen.
  applySurface(win);

  // The milestone clock starts HERE and not at the top of main: what comes before
  // is argument parsing and SDL_Init, which depend on nothing of ours.
  mark_start();
  // BEFORE tex_start and app_start, which are what create the network threads.
  net_prepare();
  mark("gfx_start");
  if (!gfx_start()) return 1;
  // fonts/ sits next to art/: drop the last component of the art path
  char dirRec[512];
  snprintf(dirRec, sizeof dirRec, "%s", dirArt);
  char *bar = strrchr(dirRec, '/');
  if (bar) *bar = 0;
  int sbx, sby, sbw, sbh;
  surfaceBox(win, &sbx, &sby, &sbw, &sbh);
  txt_start(dirRec, (float)sbw / NV_SCREEN_W);
  // The SAME scale goes to the texture cache: it is what decides each piece of
  // art's decode ceiling from the width the card draws it at. Without this every
  // card decoded with the single ceiling of 640 and the cache hit its budget at
  // ~40 textures.
  tex_scale((float)sbw / NV_SCREEN_W);
  mark("fonts+tex ready");
  // 192 slots, not 96. A slot ceiling only makes sense alongside the size of each
  // texture: with the single ceiling of 640 each one cost 2.4 MB and 96 slots
  // already blew the 96 MB budget (measured: `textures=40 pending=32 92.3MB` with
  // the home scrolling — the cache was evicting things still on screen). With the
  // per-use ceiling the same art costs ~500 KB on the TV, and 192 slots fit with
  // room to spare.
  //
  // That also doubles the ceiling of items IN FLIGHT, which is nMax/3 in freeSlot:
  // the row coming on screen asks for everything at once instead of a bit at a time.
  tex_start(192);
  // The account comes BEFORE the UI: app_start decides between opening on the home
  // and opening on login, and to decide it needs to know whether there is a stored
  // session.
  data_start(dirArt);
  cloud_configure(dirArt);
  session_start();
  profiles_load_active();
  // Links made ON THIS TV. It comes before trakt_load (which reads the package's
  // file) so the user's link beats the file of whoever built it — and in a
  // distributable package that file does not even exist.
  traktauth_load();
  simklauth_load();
  if (!app_start(dirArt)) return 1;
  // Progress is the USER's data: it leaves the package folder, which is the same
  // for everyone using the device, and moves to the installation folder.
  if (data_dir()[0]) cat_dir_writing(data_dir());
  // The addon configuration lives next to the art. Absent, the app carries on with
  // the sample list — it is never left with nothing to show.
  addons_load(dirArt);
  // Settings are the USER's too, not the package's.
  settings_dir(data_dir()[0] ? data_dir() : dirArt);
  { // The images that came from a URL sit next to the package's art. Once
    // downloaded they hold forever: a film's art does not change.
    char c[600];
    snprintf(c, sizeof c, "%s/cache", dirArt);
    tex_cache_dir(c); }
  // Os icones da interface saem de art/icones (SVG do app web rasterizados).
  gfx_icons_dir(dirArt);
  // The network catalogue. The package's is already loaded and stays on screen
  // until the answer arrives — opening empty while searching would be worse than
  // showing yesterday's for two seconds.
  trakt_load(dirArt);
  disc_tmdb(dirArt);
  disc_start();
  // Half resolution: the snapshot only appears darkened and at the edges.
  int hasSnap = gfx_snap_start((int)NV_SCREEN_W / 2, (int)NV_SCREEN_H / 2);
  int snapValid = 0;
  // A deliberately tiny target: stretched, it is what becomes the background blur.
  // 480x270: with the two-pass gaussian, what matters is not the target being tiny
  // (that is what produced blocks when stretching) but the blur being real.
  // Stretched 4x, no texel edge shows.
  gfx_blur_start(480, 270);
  // The side menu's glass: the strip is grabbed at the menu's WIDEST, so the
  // texture never has to be reallocated while the bar opens.
  gfx_backdrop_start(NV_MENU_W_IS_OPEN, NV_SCREEN_H);

  Uint32 lastReport = SDL_GetTicks();
  double txtMsFrame = 0, worstTxtMs = 0;
  int    txtNFrame = 0, worstTxtN = 0;
  int frames = 0, janks = 0; double worst = 0;

  // PER-PHASE TELEMETRY. The worst frame cost 22ms against a 20ms target and there
  // was no way to know WHERE. The clocks are CPU ones (SDL_GetPerformanceCounter)
  // and there is NO glFinish anywhere: glFinish hides the jank, because it spreads
  // the GPU cost evenly across every frame instead of letting the delay show up
  // where it is born. Here, `draw` is the cost of SUBMITTING the drawing (CPU) and
  // `swap` absorbs the vsync wait PLUS whatever the GPU still owed — a
  // GPU-heavy frame shows up as a large swap, a CPU-heavy frame shows up in the
  // phase that caused it.
  double perFreq = (double)SDL_GetPerformanceFrequency();
  Uint64 lastFrame = SDL_GetPerformanceCounter();
  // What the Mac frame limiter slept at the END of the previous frame. It has to
  // come back out of the next frame's dt: dt is wall clock, so the sleep is in it,
  // and without subtracting it `worst` would be the target frametime on every
  // frame and the phase breakdown would describe a frame taken at random. Stays 0
  // off the Mac, so the arithmetic below is unchanged on the device.
  double lastSleepMs = 0;
#ifdef __APPLE__
  // The limiter's rolling deadline. Zero means "not started"; it is seeded from
  // the first frame rather than from here, so the window setup above does not
  // count as time the loop already owes.
  Uint64 frameTicks = (Uint64)(perFreq / 60.0);
  Uint64 nextFrame = 0;
#endif
  double fEv=0, fPump=0, fUpd=0, fDraw=0, fSwap=0, fAux=0, fColor=0;
  double pEv=0, pPump=0, pUpd=0, pDraw=0, pSwap=0, pAux=0, pColor=0;
  // Inside `draw`: how much is GL traversal and how much is cache lookup.
  double fFill=0, pFill=0; int fNFull=0, pNFull=0;
  double fGfxMs=0, fTexMs=0, fOutMs=0; int fNRect=0, fNProgress=0, fNBind=0, fNSearch=0, fNOut=0;
  double pGfxMs=0, pTexMs=0, pOutMs=0; int pNRect=0, pNProgress=0, pNBind=0, pNSearch=0, pNOut=0;
#define NV_T0() (SDL_GetPerformanceCounter())
#define NV_DT(a) ((SDL_GetPerformanceCounter() - (a)) * 1000.0 / perFreq)

  while (!app_wants_exit()) {
    SDL_Event e;
    Uint64 tEv = NV_T0();
    // While the detail screen exists it keeps the whole keyboard: the home is
    // still drawn underneath, but must not react to the D-pad.
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_WINDOWEVENT) {
        // The only one that matters: the surface changed shape, so the
        // letterbox has to be measured again. Everything else (focus, expose,
        // moves) is still noise to a TV app.
        if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) applySurface(win);
        continue;
      }
#ifdef __APPLE__
      // F toggles fullscreen, Mac only — the TV is already fullscreen and has
      // no keyboard. Safe as a bare letter because the app never reads typed
      // text: there is no SDL_TEXTINPUT handler anywhere in it.
      //
      // DESKTOP fullscreen, not the real thing: it keeps the display mode and
      // just fills the screen, so switching costs nothing and the bars do the
      // aspect work they already do in the window.
      if (e.type == SDL_KEYDOWN &&
          (e.key.keysym.sym == SDLK_f || e.key.keysym.sym == SDLK_F11)) {
        Uint32 now = SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP;
        SDL_SetWindowFullscreen(win, now ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
        continue;
      }
#endif
      // webOS's BACK arrives with its own scancode (482), not as AC_BACK, and with
      // KEYDOWN and KEYUP almost together — only the KEYDOWN counts. This had
      // already been solved once and broke again when I cleaned out the old
      // patches: the handling went with them.
      if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == NV_SCANCODE_BACK) {
        SDL_Event back; SDL_zero(back);
        back.type = SDL_KEYDOWN;
        back.key.keysym.sym = SDLK_AC_BACK;
        app_event(&back);
        continue;
      }
      app_event(&e);
    }
    keysInjected(app_event);
    fEv = NV_DT(tEv);

    Uint32 now = SDL_GetTicks();
    // dt COMES FROM THE HIGH-RESOLUTION CLOCK, not from SDL_GetTicks.
    //
    // SDL_GetTicks counts in WHOLE MILLISECONDS. On the Mac the app runs without
    // vsync at ~1300 fps, so almost every frame lasts less than 1 ms and the
    // subtraction gave ZERO — and the floor `if (dt <= 0) dt = 1/60` handed over a
    // SYNTHETIC 16.7 ms for a frame of 0.8 ms of real clock. Every animation ran
    // ~20x faster than the clock: measured, a 330 ms fade finished in 92 ms.
    //
    // On the TV vsync hid the defect (a real dt, always >= 20 ms), but the
    // practical effect was worse than a Mac bug: ANY animation calibration done in
    // the preview chased a number the TV would never reproduce, and the worst-frame
    // measurement on the Mac came out distorted too.
    //
    // The clamp stays, but only as a CEILING: coming back from suspend hands over
    // a dt of several seconds and an animation would jump. There is no floor any
    // more — a short frame has to be a short dt.
    Uint64 cFrame = SDL_GetPerformanceCounter();
    double dtms = (double)(cFrame - lastFrame) * 1000.0 / perFreq;
    lastFrame = cFrame;
    if (dtms > 100.0) dtms = 100.0;
    if (dtms < 0.0) dtms = 0.0;
    float dt = (float)(dtms / 1000.0);
    // WORK time, not wall time: dtms minus whatever the limiter slept. On the
    // device the two are the same number.
    double workMs = dtms - lastSleepMs;
    if (workMs < 0.0) workMs = 0.0;
    if (frames > 20) {
      if (workMs > worst) { worst = workMs; worstTxtMs = txtMsFrame; worstTxtN = txtNFrame;
                         pEv=fEv; pPump=fPump; pUpd=fUpd; pDraw=fDraw; pSwap=fSwap; pAux=fAux; pColor=fColor;
                         pGfxMs=fGfxMs; pTexMs=fTexMs; pNRect=fNRect; pNProgress=fNProgress;
                         pNBind=fNBind; pNSearch=fNSearch; pOutMs=fOutMs; pNOut=fNOut; pFill=fFill; pNFull=fNFull; }
      if (workMs > 33.0) janks++;
    }
    // zeroes the counters of the frame that starts now; what was measured above
    // belongs to the previous frame, which is what has just cost dtms
    txtMsFrame = txt_ms; txtNFrame = txt_rasterized;
    txt_ms = 0.0; txt_rasterized = 0;

    // THREE per frame. The limit of 1 came from when ALL art was decoded with the
    // single ceiling of 640: each glTexImage2D cost ~2 MB and two in the same frame
    // went over 20 ms, showing up as a jolt on entering a row.
    //
    // That argument fell along with the single ceiling: each piece of art is now
    // decoded at the width it is drawn at (tex_get_width), and on the TV a poster
    // comes out at ~500 KB instead of 2.4 MB. Three small uploads add up to less
    // than the ONE large upload of before, and the row coming on screen stops
    // appearing in pieces.
    Uint64 t0 = NV_T0();
    tex_pump(3);
    fPump = NV_DT(t0);
    t0 = NV_T0();
    app_update(dt, now);
    fUpd = NV_DT(t0);

    // THE CLIP IS TURNED OFF BEFORE THE CLEAR. glClear respects the scissor test:
    // if any screen ends the frame with a clip active, the next clear wipes ONLY
    // that rectangle and the rest of the screen keeps the previous frame.
    // Today every caller balances crop/no-crop, but that is an invariant nobody
    // checks — and the symptom would be precisely a band with stale content, hard
    // to trace to its cause. One call per frame.
    t0 = NV_T0();
    gfx_new_frame();
    tex_new_frame();
    gfx_no_crop();
    glClearColor(NV_COLOR_BACKGROUND_R, NV_COLOR_BACKGROUND_G, NV_COLOR_BACKGROUND_B, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    fColor = NV_DT(t0);
    t0 = NV_T0();
    txt_new_frame();
    app_draw(now);
    fDraw = NV_DT(t0);
    fGfxMs = gfx_ms_rect; fTexMs = tex_ms_search;
    fNRect = gfx_n_rect; fNProgress = gfx_n_progress; fNBind = gfx_n_bind; fNSearch = tex_n_search;
    fOutMs = gfx_ms_others; fNOut = gfx_n_others;
    fFill = gfx_fill; fNFull = gfx_n_full;
    t0 = NV_T0();
    plane_pump();
    videoIfRequested();
    gotoIfRequested();
    whereIfChanged();
    rectIfRequested();
    captureIfRequested();
    fAux = NV_DT(t0);
    t0 = NV_T0();
    SDL_GL_SwapWindow(win);
    fSwap = NV_DT(t0);
    // FIRST PIXEL. It is the number that answers "how long until the TV shows
    // anything", which no per-frame metric gave.
    //
    // A flag of its OWN and not `if (!frames)`: `frames` resets on every 3 s
    // report, so that would stamp "first frame" three times a minute.
    { static int alreadyStamped;
      if (!alreadyStamped) { alreadyStamped = 1; mark("first frame on screen"); } }
    frames++;

    if (now - lastReport >= 3000) {
      int items, pending; long bytes;
      tex_stats(&items, &pending, &bytes);
      printf("FPS=%.1f worst=%.1fms janks=%d | worst frame: text %.1fms in %d lines"
             " | textures=%d pending=%d %.1fMB | evictions=%d\n",
             frames * 1000.0 / (double)(now - lastReport), worst, janks,
             worstTxtMs, worstTxtN, items, pending, bytes / 1048576.0, txt_evictions);
      fflush(stdout);
      // The SAME line goes to a file. On the device the standard output of an app
      // launched by applicationManager does not reach anywhere readable, and
      // running the binary by hand does not work (without the app's identity the
      // compositor refuses the surface and it dies silently). Without this there
      // is no way to MEASURE a frame on the device — only to look and guess.
      { FILE *fp = fopen("/tmp/nuvio-fps.txt", "w");
        if (fp) {
          fprintf(fp, "drawable=%dx%d FPS=%.1f worst=%.1fms janks=%d"
                  " text=%.1fms/%d textures=%d %.1fMB"
                  " | worst: ev=%.1f pump=%.1f upd=%.1f clr=%.1f draw=%.1f aux=%.1f swap=%.1f"
                  " | des: gfx=%.1f/%d(p%d,b%d) tex=%.2f/%d out=%.1f/%d fill=%.2fx(full=%d)"
                  " | evictions=%d\n",
                  dw, dh,
                  frames * 1000.0 / (double)(now - lastReport), worst, janks,
                  worstTxtMs, worstTxtN, items, bytes / 1048576.0,
                  pEv, pPump, pUpd, pColor, pDraw, pAux, pSwap,
                  pGfxMs, pNRect, pNProgress, pNBind, pTexMs, pNSearch, pOutMs, pNOut, pFill, pNFull,
                  txt_evictions);
          fclose(fp);
        } }
      frames = 0; lastReport = now; worst = 0; janks = 0; worstTxtMs = 0; worstTxtN = 0;
      txt_evictions = 0;
      pEv=pPump=pUpd=pDraw=pSwap=pAux=pColor=0;
      pGfxMs=pTexMs=pOutMs=0; pNRect=pNProgress=pNBind=pNSearch=pNOut=0; pFill=0; pNFull=0;
    }

#ifdef __APPLE__
    // FRAME LIMITER, Mac only. With SetSwapInterval(0) above there is nothing
    // pacing this loop and it free-ran at 1383 fps — 23x the device — which is
    // pure heat for a preview that can only ever show 60. The swap interval
    // cannot simply go back to 1: under sdl2-compat that is the hang described
    // where it is set.
    //
    // A ROLLING DEADLINE, not "sleep out the rest of this frame". SDL_Delay only
    // promises to sleep AT LEAST what it is asked for, and the truncation to
    // whole milliseconds loses the rest: measured, sleeping per-frame from the
    // frame's own start settled at 58.0 fps, never 60. Advancing a deadline by a
    // fixed 1/60 lets a long sleep shorten the next one, so the AVERAGE is the
    // target even though no single sleep is exact.
    //
    // The catch-up is capped at one frame: after a stall the deadline resets to
    // now instead of firing off the frames it "owes" back to back.
    //
    // AFTER the telemetry, never inside it. Folding this into `swap` would make
    // swap a constant ~16 ms on the Mac and destroy the one signal it carries —
    // that a GPU-heavy frame shows up as a large swap.
    {
      if (!nextFrame) nextFrame = cFrame;
      nextFrame += frameTicks;
      Uint64 nowTicks = SDL_GetPerformanceCounter();
      lastSleepMs = 0;
      // Signed: these are counters, and `nextFrame - nowTicks` on a frame that
      // overran wraps instead of going negative.
      if ((Sint64)(nextFrame - nowTicks) <= 0) {
        nextFrame = nowTicks;
      } else {
        double restMs = (double)(nextFrame - nowTicks) * 1000.0 / perFreq;
        // Under a millisecond is not worth asking for: SDL_Delay rounds up to the
        // scheduler's granularity and would overshoot the budget it is protecting.
        // The deadline absorbs the skipped fraction on the next frame.
        if (restMs > 1.0) {
          Uint64 tSleep = SDL_GetPerformanceCounter();
          SDL_Delay((Uint32)restMs);
          // What it ACTUALLY slept, not what was asked for — the subtraction from
          // the next dt has to use the real number or it lands short.
          lastSleepMs = (double)(SDL_GetPerformanceCounter() - tSleep) * 1000.0 / perFreq;
        }
      }
    }
#endif
  }

  gfx_backdrop_shutdown();
  gfx_blur_shutdown();
  gfx_snap_shutdown();
  app_shutdown();
  tex_shutdown();
  txt_shutdown();
  gfx_shutdown();
  SDL_GL_DeleteContext(ctx);
  SDL_DestroyWindow(win);
  IMG_Quit();
  SDL_Quit();
  printf("fim\n");
  return 0;
}
