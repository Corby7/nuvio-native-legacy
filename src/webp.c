#include "webp.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

// The two entry points that matter. Written by hand rather than pulled from
// decode.h, for the same reason net.c writes the libcurl constants by hand:
// there is no header in the SDK, and the ABI of these two has not moved since
// libwebp 0.2.
//
// WebPDecodeRGBA gives back R,G,B,A in BYTE order. SDL_PIXELFORMAT_ABGR8888 is
// the mask-based name for exactly that byte order on a little-endian machine,
// which is what tex_cache already converts everything to.
static int   (*webp_info)(const unsigned char *, size_t, int *, int *);
static unsigned char *(*webp_decode)(const unsigned char *, size_t, int *, int *);
static void  (*webp_free)(void *);
static int   ready;

static pthread_mutex_t openLock = PTHREAD_MUTEX_INITIALIZER;

// Same shape as net.c's openHandle, and for the same reason: TWO decode threads
// plus four network threads can arrive here at once on the first frame, and
// dlopen racing with itself is not worth finding out about later.
static int openLib(void) {
  void *h;
  int r;
  if (ready) return ready > 0;
  pthread_mutex_lock(&openLock);
  if (ready) { r = ready > 0; pthread_mutex_unlock(&openLock); return r; }
  ready = -1;
  h = dlopen("libwebp.so.7", RTLD_NOW);
  if (!h) h = dlopen("libwebp.so.6", RTLD_NOW);
  if (!h) h = dlopen("libwebp.so", RTLD_NOW);
  // Mac, for the tools/mac.sh loop. The bare name does not resolve: homebrew
  // does not install into the dyld cache and /opt/homebrew/lib is not on the
  // default search path, so the full path is what actually opens.
  if (!h) h = dlopen("/opt/homebrew/lib/libwebp.7.dylib", RTLD_NOW);
  if (!h) h = dlopen("/opt/homebrew/lib/libwebp.dylib", RTLD_NOW);
  if (!h) h = dlopen("/usr/local/lib/libwebp.7.dylib", RTLD_NOW);   // Intel Mac
  if (!h) h = dlopen("libwebp.7.dylib", RTLD_NOW);
  if (!h) h = dlopen("libwebp.dylib", RTLD_NOW);
  if (!h) {
    printf("[webp] no libwebp: %s — WebP art will not be drawn\n", dlerror());
    fflush(stdout);
    pthread_mutex_unlock(&openLock);
    return 0;
  }
  *(void **)(&webp_info)   = dlsym(h, "WebPGetInfo");
  *(void **)(&webp_decode) = dlsym(h, "WebPDecodeRGBA");
  // WebPFree only exists from 0.5 on. Before it, the buffer was released with
  // plain free() and that is still correct on those versions — the allocation
  // comes from the same malloc. Missing symbol is not a reason to give up the
  // decoder.
  *(void **)(&webp_free)   = dlsym(h, "WebPFree");
  if (!webp_info || !webp_decode) {
    printf("[webp] libwebp is missing the expected symbols\n");
    fflush(stdout);
    pthread_mutex_unlock(&openLock);
    return 0;
  }
  ready = 1;
  printf("[webp] libwebp ready%s\n", webp_free ? "" : " (no WebPFree; using free)");
  fflush(stdout);
  pthread_mutex_unlock(&openLock);
  return 1;
}

int webp_is(const void *data, long n) {
  const unsigned char *b = (const unsigned char *)data;
  if (!b || n < 12) return 0;
  return b[0] == 'R' && b[1] == 'I' && b[2] == 'F' && b[3] == 'F' &&
         b[8] == 'W' && b[9] == 'E' && b[10] == 'B' && b[11] == 'P';
}

SDL_Surface *webp_load(const char *path) {
  FILE *f;
  long n = 0;
  unsigned char *body = NULL;
  unsigned char *px = NULL;
  SDL_Surface *s = NULL;
  int w = 0, h = 0, y;

  if (!path || !*path) return NULL;
  f = fopen(path, "rb");
  if (!f) return NULL;
  if (fseek(f, 0, SEEK_END) == 0) n = ftell(f);
  // A WebP with no header cannot be one, and the ceiling keeps a file that is
  // not what it claims from being read whole into memory on a device with
  // 832 MB free. A 4K backdrop in WebP does not reach 24 MB.
  if (n < 12 || n > 24L * 1024 * 1024) { fclose(f); return NULL; }
  rewind(f);
  body = (unsigned char *)malloc((size_t)n);
  if (!body) { fclose(f); return NULL; }
  if (fread(body, 1, (size_t)n, f) != (size_t)n) { free(body); fclose(f); return NULL; }
  fclose(f);

  // The signature is checked BEFORE the library is opened: this is called for
  // every image that fails to decode, and most of those are not WebP.
  if (!webp_is(body, n) || !openLib()) { free(body); return NULL; }
  if (!webp_info(body, (size_t)n, &w, &h) || w < 1 || h < 1) {
    free(body);
    return NULL;
  }
  px = webp_decode(body, (size_t)n, &w, &h);
  free(body);
  if (!px) return NULL;

  s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ABGR8888);
  if (s) {
    // Row by row, because the surface's pitch is not necessarily w*4: SDL pads
    // lines, and a single memcpy of the whole thing would skew the image by a
    // few pixels per row on any width that is not aligned.
    for (y = 0; y < h; y++)
      memcpy((unsigned char *)s->pixels + (size_t)y * s->pitch,
             px + (size_t)y * (size_t)w * 4, (size_t)w * 4);
  }
  if (webp_free) webp_free(px); else free(px);
  return s;
}
