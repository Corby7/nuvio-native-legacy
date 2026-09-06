#include "tex_cache.h"
#include "net.h"
#include "webp.h"
#include "gfx.h"
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include "layout.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define MAX_ITEMS_ABS 512
#define MAX_QUEUE 128
#define NV_TEX_STALE_FRAMES 8
#define NV_TEX_STALE_MS 200
#define NV_TEX_UPLOAD_BUDGET_MS 4.0

// FAILED is a real state, not the absence of one. Without it, a path that does
// not decode goes back to EMPTY, the drawing asks again on the next frame and the
// cycle never ends: the item takes an in-flight slot and a turn in the queue ahead
// of the good images, only to fail again. With backoff, the second attempt only
// happens 2 s later, the third at 10 s, and after that it gives up for the session.
typedef enum { EMPTY=0, PENDING, DECODED, READY, FAILED } State;

typedef struct {
  char path[512];
  unsigned long hash;  // FNV-1a of the path, to skip the strcmp in the lookup
  State state;
  SDL_Surface *sup;   // filled by the thread; consumed in the pump
  GLuint tex;
  int w, h;
  // The width ceiling REQUESTED for this item. The full-screen hero needs 1920; a
  // 212 poster does not. A single ceiling for all served both badly: at 960 the
  // hero was decoded at half the resolution and stretched to 1920 on screen, which
  // is the blur the owner saw.
  int limit;
  unsigned long usage;  // LRU counter
  // The average luminance of the OPAQUE pixels, 0..255; -1 while it is not known.
  // Measured once, on the decode thread. It serves the title's logo: TMDB marks
  // light/dark nowhere (the web app's own ranking is only language +
  // vote_average), so the only way to know whether a logo is black is to LOOK at
  // the pixels.
  int luma;
  // When to try again (ticks) and how many times it has already failed. See the State enum.
  Uint32 tryIn;
  int    failures;
  unsigned long lastFrame;
  Uint32 lastRequest;
  // The average chroma: max(R,G,B) - min(R,G,B) of the same opaque pixels. It
  // separates a BLACK logo (achromatic, the wrong TMDB variant) from a dark but
  // colourful BRAND logo (red, wine), which must pass through untouched.
  int chroma;
} Item;

#define NV_TEX_THREADS 2
#define NV_TEX_THREADS_NET 4
static SDL_Thread *thrs[NV_TEX_THREADS];
static SDL_Thread *thrsNet[NV_TEX_THREADS_NET];
static Item items[MAX_ITEMS_ABS];
static int nMax = 64;
static unsigned long lruClock = 1;

static SDL_mutex *mtx;
static SDL_cond  *cond;
static SDL_Thread *thr;
static int running = 0;

// How much memory the READY textures take. Without this arithmetic the cache only
// evicted when a SLOT WAS MISSING — and with 96 slots of 1920x1080 art that comes
// to 800 MB. With real art the app reached 104 MB and died with "double free or
// corruption" inside SDL: it was running out of memory, not a pointer bug.
static long bytesUsed = 0;
static long budget = 0;
static unsigned long frameCurrent = 1;

// The driver also allocates the mipmap pyramid. Counting only the base level let
// the cache exceed the real ceiling by around 33% on card art.
static long bytesTexture(int w, int h) {
  long total = (long)w * h * 4;
  int mw = w, mh = h;
  if (w >= 1024) return total;
  while (mw > 1 || mh > 1) {
    mw = (mw + 1) / 2;
    mh = (mh + 1) / 2;
    total += (long)mw * mh * 4;
  }
  return total;
}

// `queue` was the only one, and the download happened INSIDE the decode thread
// (ensureLocal, called from threadDecode). With two decode threads at low
// priority, each one sat BLOCKED ON THE NETWORK for up to 8 s instead of decoding
// — with a cold cache the art came in a drop at a time even with plenty of network
// to spare, which was exactly the report: "the internet is fast and the artwork
// takes ages".
//
// Now `queue` is the NETWORK queue (downloading into the disk cache) and `queueDec`
// the DECODE one. Network is I/O waiting, not CPU work: it can have several
// threads without stealing a frame from the drawing. Decoding stays at two, at low
// priority, for the reason already measured.
static int queue[MAX_QUEUE];
static int queueStart = 0, queueEnd = 0;
static int queueDec[MAX_QUEUE];
static int decStart = 0, decEnd = 0;
static SDL_cond *condDec;

// Signalled by the decode threads when they FREE a place in the queue.
static SDL_cond *condFree;

// Enqueues for DECODING. Called with the mutex held, and it WAITS when the queue
// is full.
//
// Discarding silently, which is what used to be here, left the item PENDING
// forever: nobody decoded it and nothing re-enqueued it. MEASURED with a cold
// cache: the textures rose to 24 and STOPPED — four network threads fill the queue
// faster than two decode threads empty it, so discarding became the rule and not
// the exception. Waiting here is what makes the network move at the decode's pace
// instead of running it over.
static void forDecode(int idx) {
  for (;;) {
    int next = (decEnd + 1) % MAX_QUEUE;
    if (next != decStart) {
      queueDec[decEnd] = idx; decEnd = next;
      SDL_CondSignal(condDec);
      return;
    }
    if (!running) return;
    SDL_CondWait(condFree, mtx);
  }
}

// FNV-1a of the path. The lookup below runs for every visible card on every frame,
// against up to 96 slots; comparing an integer first reduces the strcmp to only
// the candidates with the same hash (in practice, the item itself).
static unsigned long hashPath(const char *s) {
  unsigned long h = 2166136261UL;
  for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619UL; }
  return h;
}

int    tex_n_search = 0;
double tex_ms_search = 0.0;
static double texFreqMs = 0.0;
static int requestStale(const Item *it) {
  Uint32 now;
  if (!it->lastRequest || !it->lastFrame) return 0;
  now = SDL_GetTicks();
  return frameCurrent > it->lastFrame + NV_TEX_STALE_FRAMES &&
         now - it->lastRequest >= NV_TEX_STALE_MS;
}

void tex_new_frame(void) {
  tex_n_search = 0;
  tex_ms_search = 0.0;
  (void)texFreqMs;
  if (!mtx) return;
  SDL_LockMutex(mtx);
  frameCurrent++;
  // A decode that finished but was never drawn again must take neither memory nor
  // block art that has come on screen. PENDING requests are cancelled by the
  // queue's consumer, so the slot index is not reused before the queue removes it.
  for (int i = 0; i < nMax; i++) {
    if (items[i].state == DECODED && requestStale(&items[i])) {
      SDL_FreeSurface(items[i].sup);
      items[i].sup = NULL;
      items[i].state = EMPTY;
      items[i].path[0] = 0;
    }
  }
  SDL_UnlockMutex(mtx);
}

// Wraps LOCK + lookup with a CPU clock. Only the callers on the DRAWING THREAD
// come through here; the decode thread uses the raw findIndex, otherwise the two
// threads would add up in the same counter and the number would stop describing
// the frame.
//
// THE LOCK COUNTS, and not only the lookup. The decode threads run at LOW priority
// and take this same mutex: a decode thread preempted while HOLDING the mutex
// makes the drawing thread wait for it — classic priority inversion. Measuring
// only findIndex would hide exactly that cost, which is the only path by which the
// decode can steal the frame.
//
// THE RESULT, so as not to redo the arithmetic: with 192 slots and ~70 lookups per
// frame on the home, lock + linear search added up to 0.04 to 0.12 ms PER FRAME —
// less than 1% of a 20ms frame. The linear search and the priority inversion were
// on the suspect list for the 22ms frame and both were RULED OUT BY MEASUREMENT;
// it is not worth swapping this for a hash table. The clock sits behind
// NV_PERF_FINO because it cost two reads per lookup.
#ifdef NV_PERF_FINE
#define SEARCH_MEASURE(idx, cam, h) do { \
  if (texFreqMs == 0.0) texFreqMs = 1000.0 / (double)SDL_GetPerformanceFrequency(); \
  Uint64 t0_ = SDL_GetPerformanceCounter(); \
  SDL_LockMutex(mtx); \
  (idx) = findIndex((cam), (h)); \
  tex_ms_search += (double)(SDL_GetPerformanceCounter() - t0_) * texFreqMs; \
  tex_n_search++; \
} while (0)
#else
#define SEARCH_MEASURE(idx, cam, h) do { \
  SDL_LockMutex(mtx); \
  (idx) = findIndex((cam), (h)); \
  tex_n_search++; \
} while (0)
#endif

static int findIndex(const char *path, unsigned long h) {
  for (int i = 0; i < nMax; i++)
    if (items[i].state != EMPTY && items[i].hash == h &&
        strcmp(items[i].path, path) == 0) return i;
  return -1;
}

// Picks an LRU victim among the READY ones. It never discards what is in flight,
// otherwise the thread would write over a reused slot.
// How many requests may be IN FLIGHT at once.
//
// With no ceiling, a screen full of new art asks for ~90 images at once, the slots
// fill with PENDING and freeSlot starts discarding READY textures to make room for
// more pending ones — the screen GOES BLANK and does not come back, because what
// finishes is thrown away before it appears.
//
// It only shows up with a COLD disk cache (the first run, or right after
// reinstalling the app, which deletes art/cache along with it). That is how it
// turned up here: I had just deleted the cache by hand and the owner saw the home
// with no posters at all.
//
// A third of the slots keeps the decode thread busy and still leaves two thirds
// for what is already on screen.
static int inFlight(void) {
  int n = 0;
  for (int i = 0; i < nMax; i++)
    if (items[i].state == PENDING || items[i].state == DECODED) n++;
  return n;
}

static int slotFree(void) {
  if (inFlight() >= nMax / 3) return -1;   // ask again on the next frame
  for (int i = 0; i < nMax; i++) if (items[i].state == EMPTY) return i;
  // A slot that has already given up is worth more as a vacancy than a READY one
  // in use: reuse it before evicting art that is on screen.
  for (int i = 0; i < nMax; i++)
    if (items[i].state == FAILED && items[i].failures >= 3) {
      memset(&items[i], 0, sizeof(Item));
      items[i].luma = -1;
      return i;
    }
  int best = -1; unsigned long smaller = ~0UL;
  for (int i = 0; i < nMax; i++) {
    if (items[i].state != READY) continue;
    if (items[i].usage < smaller) { smaller = items[i].usage; best = i; }
  }
  if (best >= 0) {
    if (items[best].tex) { gfx_tex_forget(items[best].tex); glDeleteTextures(1, &items[best].tex); }
    bytesUsed -= bytesTexture(items[best].w, items[best].h);
    if (bytesUsed < 0) bytesUsed = 0;
    memset(&items[best], 0, sizeof(Item));
    items[best].luma = -1;   // 0 seria "preto"; o desconhecido e -1
  }
  return best;
}

// Evicts the least used until it fits the budget. Called with the mutex locked.
static void prune(void) {
  while (bytesUsed > budget) {
    int best = -1; unsigned long smaller = ~0UL;
    for (int i = 0; i < nMax; i++) {
      if (items[i].state != READY) continue;
      if (items[i].usage < smaller) { smaller = items[i].usage; best = i; }
    }
    if (best < 0) break;          // only what is in flight is left
    if (items[best].tex) { gfx_tex_forget(items[best].tex); glDeleteTextures(1, &items[best].tex); }
    bytesUsed -= bytesTexture(items[best].w, items[best].h);
    if (bytesUsed < 0) bytesUsed = 0;
    memset(&items[best], 0, sizeof(Item));
    items[best].luma = -1;   // 0 seria "preto"; o desconhecido e -1
  }
}

// The directory the downloaded images live in. Once downloaded, an image holds
// forever: a film's art does not change. Without this, every return to the home
// would redo dozens of downloads.
// The texture's width ceiling.
//
// 960 and not 1280. The arithmetic: the art card measures 410px and the detail's
// large card 684; only the hero and full-screen art go beyond that, and in both
// the image already appears blurred or covered in text. At 1280 the cache lived
// PRESSED against the 72 MB ceiling (measured: 71.4 MB with 29 textures), evicting
// and re-fetching without pause — which shows up as 30fps with jank on every frame
// after a few minutes of use. At 960 the same scene fits with room to spare.
// The decode ceiling for CARD art.
//
// It was 960 for everything, and the largest card art the screen draws is the
// episode thumbnail, at 640 (NV_DETP_EP_W). A poster 212 wide was decoded at
// 960x1440 and cost 5 MB of texture — twenty of them already pass the whole 96 MB
// budget.
//
// That is what the owner saw: moving through the rows, and above all when OPENING
// A FILM (which asks for a 1920 backdrop plus thumbnails, related posters and cast
// photos all at once), the total blew and prune() evicted everything on screen —
// it went grey and did not come back.
//
// 640 covers the largest card art with nothing to spare and divides the cost by
// 2.25: the same poster now costs 2.2 MB. The hero keeps its own ceiling of 1920,
// through promotion.
#define NV_TEX_WIDTH_MAX 640
#define NV_TEX_HERO_WIDTH_MAX 1920

// A PER-USE CEILING — the 640 above is the default, and it is FAR TOO LARGE for
// most art. It was sized by the LARGEST card art (the episode thumbnail, 640), but
// it applies to all of them: a poster drawn 212 wide was decoded at 640x960 and
// cost 2.4 MB. With a 96 MB budget that gives ~40 textures — MEASURED on the
// device: with the home scrolling the log showed `textures=40 pending=32 92.3MB`,
// that is, the queue clogged and the cache already evicting what was still on
// screen to fit what was coming in.
//
// That is the "it loads things as you go past": it is not the network nor a slow
// decode, it is the cache hitting its ceiling and re-decoding what it has just evicted.
//
// Here each caller asks by the WIDTH IT DRAWS AT, and the arithmetic becomes
// width * buffer scale * slack. On the TV the scale is 1 (drawable=1920x1080,
// measured) and a poster now costs ~420 KB instead of 2.4 MB — almost six times as
// much art in the same budget. On a retina Mac the scale is 2 and the preview stays
// sharp.
//
// The SLACK of 1.25 covers the card growing when it takes focus (a scale of ~1.08)
// and avoids resampling at the exact limit, which aliases.
#define NV_TEX_SLACK 1.25f
static float scaleBuf = 1.0f;

void tex_scale(float e) {
  if (e > 0.1f && e < 8.0f) scaleBuf = e;
}

static char dirCache[512];

void tex_cache_dir(const char *dir) {
  if (!dir || !*dir) return;
  snprintf(dirCache, sizeof dirCache, "%s", dir);
  mkdir(dirCache, 0777);
  // THE FOLDER MAY EXIST AND NOT BE WRITABLE, and that has already happened:
  // tools/arm.sh sends art/ in a tar made on the Mac, and the tar extracted as root
  // on the TV stamps the owner with the Mac's uid (13888160) and mode 755. The app
  // runs as uid 5152, so after EVERY deploy the folder was read-only.
  //
  // The effect was invisible: ensureLocal downloaded the image, the temporary
  // file's fopen failed, it returned 0 without saying anything, and the only
  // symptom was "a card with no art". Measured: 91 "decode failed" in one pass,
  // with ZERO network errors — the images arrived and were thrown away. Each one
  // is asked for again until the third backoff, so the two decode threads sit busy
  // downloading what can never be stored.
  //
  // The app cannot fix it (a chmod by a non-owner fails), but it HAS to say so.
  // Without this line the defect cannot be traced to its cause.
  if (access(dirCache, W_OK) != 0)
    printf("[tex] CACHE FOLDER NOT WRITABLE: %s — every downloaded image will be"
           " discarded (check the owner; the deploy stamps the Mac uid)\n",
           dirCache);
  fflush(stdout);
}

// A stable file name from the URL. A simple hash (FNV-1a) and not the URL's name
// because URLs carry slashes, queries and characters that do not fit in a file
// name — and because two different URLs need different files.
static void nameOfCache(const char *url, char *dst, size_t size) {
  unsigned long h = 2166136261UL;
  const char *p = url, *dot = strrchr(url, '.');
  char ext[8] = ".jpg";
  for (; *p; p++) { h ^= (unsigned char)*p; h *= 16777619UL; }
  if (dot && strlen(dot) <= 5 && !strchr(dot, '/'))
    snprintf(ext, sizeof ext, "%s", dot);
  snprintf(dst, size, "%s/%08lx%s", dirCache, h, ext);
}

// Downloads the URL into the cache, if it is not already there. Returns 1 if there
// is a usable file at the end. It runs on the decode thread, so blocking here costs
// no frames.
static int ensureLocal(const char *url, char *dst, size_t size) {
  FILE *f;
  char *body;
  long n = 0;
  if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) {
    snprintf(dst, size, "%s", url);
    return 1;
  }
  if (!dirCache[0]) return 0;
  nameOfCache(url, dst, size);
  f = fopen(dst, "rb");
  if (f) { fseek(f, 0, SEEK_END); n = ftell(f); fclose(f); if (n > 512) return 1; }
  // 8 s and not 25: this is an IMAGE. At 25 s, two dead URLs held both decode
  // threads for almost a minute and the whole screen stopped receiving art —
  // repeatedly, because nothing stores the failure.
  body = net_download_bin(url, 8, &n);
  // THIS BRANCH WAS MUTE. Measured on one pass through the home: 93 "decode
  // failed" with ZERO "[net] failure" in the log — every failure came through here,
  // with curl reporting success and a body too short to be an image. Without the
  // line there was no way to tell "the server refused" from "the cache has no write
  // permission" from "an empty response". It does not repeat the HTTP >= 400 case,
  // which net.c now names by itself.
  if (!body || n <= 512) {
    if (body) { printf("[tex] body too short (%ld B): %.70s\n", n, url); fflush(stdout); }
    free(body);
    return 0;
  }
  // AN IMAGE SIGNATURE. net.c does not check the HTTP status, so a 404 with an
  // error page over 512 bytes was written as an "image" and stayed in the disk
  // cache FOREVER — the item would never have art again, not even after the server
  // came back. It accepts JPEG (FF D8), PNG (89 50 4E 47), GIF and RIFF/WEBP.
  { const unsigned char *b0 = (const unsigned char *)body;
    int ok = (n > 4) && (
       (b0[0] == 0xFF && b0[1] == 0xD8) ||
       (b0[0] == 0x89 && b0[1] == 0x50 && b0[2] == 0x4E && b0[3] == 0x47) ||
       (b0[0] == 'G'  && b0[1] == 'I'  && b0[2] == 'F') ||
       (b0[0] == 'R'  && b0[1] == 'I'  && b0[2] == 'F'  && b0[3] == 'F'));
    if (!ok) {
      printf("[tex] response is not an image (%ld B): %.70s\n", n, url);
      fflush(stdout);
      free(body);
      return 0;
    } }
  { char tmp[600];
    // Writes to a temporary and renames: another thread may be reading the same
    // file, and a half file decodes as a broken image and stays cached that way
    // forever.
    snprintf(tmp, sizeof tmp, "%s.partial", dst);
    f = fopen(tmp, "wb");
    // It failed SILENTLY. See the note in tex_cache_dir: a folder without write
    // permission throws away every downloaded image and the only symptom was a
    // grey card.
    if (!f) { printf("[tex] could not write %.80s\n", tmp); fflush(stdout);
              free(body); return 0; }
    fwrite(body, 1, (size_t)n, f);
    fclose(f);
    rename(tmp, dst);
  }
  free(body);
  return 1;
}

// NETWORK THREAD: it takes from the queue, makes sure the file is in the disk cache
// and passes it on to the decoding. It touches no pixels, so it can run at normal
// priority and in numbers — what it does is WAIT.
static int threadNet(void *arg) {
  (void)arg;
  for (;;) {
    int idx;
    char path[512], local[600];
    SDL_LockMutex(mtx);
    while (running && queueStart == queueEnd) SDL_CondWait(cond, mtx);
    if (!running) { SDL_UnlockMutex(mtx); return 0; }
    idx = queue[queueStart]; queueStart = (queueStart + 1) % MAX_QUEUE;
    if (items[idx].state != PENDING || requestStale(&items[idx])) {
      if (items[idx].state == PENDING) {
        items[idx].state = EMPTY;
        items[idx].path[0] = 0;
      }
      SDL_UnlockMutex(mtx);
      continue;
    }
    strncpy(path, items[idx].path, sizeof path - 1);
    path[sizeof path - 1] = 0;
    SDL_UnlockMutex(mtx);

    // A local path answers at once; only a URL goes out to the network.
    SDL_LockMutex(mtx);
    if (items[idx].state != PENDING || requestStale(&items[idx])) {
      if (items[idx].state == PENDING) {
        items[idx].state = EMPTY;
        items[idx].path[0] = 0;
      }
      SDL_UnlockMutex(mtx);
      continue;
    }
    SDL_UnlockMutex(mtx);

    if (!ensureLocal(path, local, sizeof local)) {
      // The download failed. It marks the failure HERE so the backoff counts —
      // before, it was the decode that marked it, and until then the item stayed
      // PENDING taking up a slot.
      SDL_LockMutex(mtx);
      if (items[idx].state != PENDING || requestStale(&items[idx])) {
        if (items[idx].state == PENDING) {
          items[idx].state = EMPTY;
          items[idx].path[0] = 0;
        }
        SDL_UnlockMutex(mtx);
        continue;
      }
      items[idx].state = FAILED;
      items[idx].failures++;
      items[idx].tryIn = SDL_GetTicks() +
          (items[idx].failures == 1 ? 2000 : (items[idx].failures == 2 ? 10000 : 60000));
      SDL_UnlockMutex(mtx);
      continue;
    }
    SDL_LockMutex(mtx);
    if (items[idx].state != PENDING || requestStale(&items[idx])) {
      if (items[idx].state == PENDING) {
        items[idx].state = EMPTY;
        items[idx].path[0] = 0;
      }
      SDL_UnlockMutex(mtx);
      continue;
    }
    forDecode(idx);
    SDL_UnlockMutex(mtx);
  }
}

static int threadDecode(void *arg) {
  (void)arg;
  // LOW PRIORITY, and this is no detail.
  //
  // Measured: while navigating, the worst frame sat at 42 ms and the janks matched
  // EXACTLY with `pending>0` — that is, with this thread working. It decodes JPEG
  // and shrinks the image with SDL_BlitScaled, all on the CPU, and this TV has four
  // weak cores: at equal priority it steals the frame from the drawing. Art that
  // appears a moment later nobody notices; the jolt, they do.
  SDL_SetThreadPriority(SDL_THREAD_PRIORITY_LOW);
  for (;;) {
    SDL_LockMutex(mtx);
    while (running && decStart == decEnd) SDL_CondWait(condDec, mtx);
    if (!running) { SDL_UnlockMutex(mtx); return 0; }
    int idx = queueDec[decStart]; decStart = (decStart + 1) % MAX_QUEUE;
    SDL_CondSignal(condFree);   // a place opened: release a waiting network thread
    if (items[idx].state != PENDING || requestStale(&items[idx])) {
      if (items[idx].state == PENDING) {
        items[idx].state = EMPTY;
        items[idx].path[0] = 0;
      }
      SDL_UnlockMutex(mtx);
      continue;
    }
    char path[512];
    int limit;
    strncpy(path, items[idx].path, sizeof path - 1);
    path[sizeof path - 1] = 0;
    // Copied UNDER THE MUTEX: the item may be promoted to hero while this thread
    // decodes, and reading the field afterwards would be a lock-free read.
    limit = items[idx].limit > 0 ? items[idx].limit : NV_TEX_WIDTH_MAX;
    SDL_UnlockMutex(mtx);

    // THE ORIGIN, KEPT. `path` is about to be overwritten with the cache path,
    // and after that the only name a failure can report is an FNV hash — which
    // says nothing about WHERE the art came from or WHOSE it is. Two images
    // failing for an unknown reason could not be chased any further than
    // "d7098286.jpg", a file that no longer exists by the time anyone looks.
    char origin[512];
    strncpy(origin, path, sizeof origin - 1);
    origin[sizeof origin - 1] = 0;

    // The download has ALREADY HAPPENED on the network thread; here ensureLocal
    // only translates the URL into the cache's path, without touching the network.
    { char local[600];
      if (ensureLocal(path, local, sizeof local))
        snprintf(path, sizeof path, "%s", local);
    }
    SDL_Surface *bruta = IMG_Load(path);
    SDL_Surface *conv = NULL;
    if (bruta) {
      conv = SDL_ConvertSurfaceFormat(bruta, SDL_PIXELFORMAT_ABGR8888, 0);
      SDL_FreeSurface(bruta);
    } else {
      // WEBP, WHICH THIS TV'S SDL_image CANNOT READ. Measured on the C3:
      // IMG_Init answers 0x3 (JPG and PNG only) and says "WEBP images are not
      // supported", even though the library exports IMG_LoadWEBP_RW and
      // /usr/lib/libwebp.so.7 is installed. So the file is handed to libwebp
      // directly — see webp.c.
      //
      // AFTER IMG_Load and not before: JPEG and PNG are the overwhelming
      // majority and must not pay for a file read that would tell them nothing.
      // webp_load already returns NULL for anything that is not WebP, so this
      // costs one open on a path that had failed anyway.
      //
      // It comes back as ABGR8888, which is the format the conversion above
      // produces, so there is nothing left to convert.
      conv = webp_load(path);
    }
    // A WIDTH CEILING. The art used to come from the package already reduced; now
    // it comes from the network at whatever size the server has, and a 1920
    // backdrop costs 8 MB DECODED — half a dozen of them blow the budget and the
    // cache starts evicting and re-fetching in a circle, which shows up as an fps
    // drop and 100 ms frames. Reducing here holds for any source, present or future.
    if (conv && conv->w > limit) {
      // BOX FILTER, NOT SDL_BlitScaled.
      //
      // The note that used to be here said BlitScaled "averages the
      // neighbours". It does not: on this path it goes through SDL_SoftStretch,
      // which is NEAREST-NEIGHBOUR. Reducing 1998 -> 640 it keeps roughly one
      // source pixel in three and discards the rest, and on thin lettering that
      // is aliasing — the art looked coarser than the same file does in the web
      // app, which is what the owner reported.
      //
      // MEASURED against an exact box average of the same file, RMS over RGB
      // (0 = identical to the ideal):
      //     logo 1998x518 -> 640 : BlitScaled 13.82   box 0 (this code)
      //     logo 1390x419 -> 640 : BlitScaled  7.64   box 0
      // Halving repeatedly first was tried and measured WORSE (19.93), because
      // with a nearest-neighbour scaler every extra pass throws away more
      // pixels rather than averaging them.
      //
      // Alpha is PREMULTIPLIED while averaging. A logo is transparent around
      // the letters and those transparent pixels usually carry black RGB;
      // averaging colour without weighting by alpha pulls that black into every
      // edge and leaves a dark fringe around the type.
      //
      // Cost is one pass over the source, on the decode thread, which already
      // runs at SDL_THREAD_PRIORITY_LOW and has just spent far more than this
      // decoding the file.
      int lw = limit;
      int lh = conv->h * lw / conv->w;
      SDL_Surface *smaller = SDL_CreateRGBSurfaceWithFormat(
          0, lw, lh > 0 ? lh : 1, 32, SDL_PIXELFORMAT_ABGR8888);
      if (smaller) {
        int ox, oy;
        for (oy = 0; oy < smaller->h; oy++) {
          int y0 = oy * conv->h / smaller->h;
          int y1 = (oy + 1) * conv->h / smaller->h;
          unsigned char *out = (unsigned char *)smaller->pixels
                             + (size_t)oy * smaller->pitch;
          if (y1 <= y0) y1 = y0 + 1;
          for (ox = 0; ox < smaller->w; ox++) {
            int x0 = ox * conv->w / smaller->w;
            int x1 = (ox + 1) * conv->w / smaller->w;
            unsigned long r = 0, g = 0, b = 0, a = 0;
            int n = 0, xx, yy;
            if (x1 <= x0) x1 = x0 + 1;
            for (yy = y0; yy < y1; yy++) {
              const unsigned char *ln = (const unsigned char *)conv->pixels
                                      + (size_t)yy * conv->pitch;
              for (xx = x0; xx < x1; xx++) {
                const unsigned char *p = ln + (size_t)xx * 4;
                unsigned al = p[3];
                r += (unsigned long)p[0] * al;
                g += (unsigned long)p[1] * al;
                b += (unsigned long)p[2] * al;
                a += al;
                n++;
              }
            }
            { unsigned char *q = out + (size_t)ox * 4;
              // Fully transparent block: keep the colour channels at zero
              // rather than dividing by an alpha sum of zero.
              q[0] = a ? (unsigned char)(r / a) : 0;
              q[1] = a ? (unsigned char)(g / a) : 0;
              q[2] = a ? (unsigned char)(b / a) : 0;
              q[3] = (unsigned char)(a / (unsigned)n); }
          }
        }
        SDL_FreeSurface(conv);
        conv = smaller;
      }
    }

    // A LUMINANCE MEASUREMENT, here and not while drawing: this thread already has
    // the pixels in hand and runs at low priority. It samples every 4th pixel on
    // both axes — 1/16 of the pixels is enough to say whether a piece of art is
    // dark, and doing the whole sum on a 700x271 logo would be work with no return.
    int lumaMedia = -1, chromaMedia = 0;
    if (conv && conv->format->BytesPerPixel == 4) {
      const unsigned char *px = (const unsigned char *)conv->pixels;
      long sum = 0, sumC = 0, n = 0;
      int yy, xx;
      for (yy = 0; yy < conv->h; yy += 4) {
        const unsigned char *ln = px + (size_t)yy * conv->pitch;
        for (xx = 0; xx < conv->w; xx += 4) {
          const unsigned char *q = ln + (size_t)xx * 4;   // ABGR8888: R,G,B,A
          if (q[3] < 200) continue;                       // only what is opaque
          sum += (q[0] * 299 + q[1] * 587 + q[2] * 114) / 1000;
          { int mx = q[0] > q[1] ? q[0] : q[1]; if (q[2] > mx) mx = q[2];
            int mn = q[0] < q[1] ? q[0] : q[1]; if (q[2] < mn) mn = q[2];
            sumC += mx - mn; }
          n++;
        }
      }
      if (n > 0) { lumaMedia = (int)(sum / n); chromaMedia = (int)(sumC / n); }
    }

    // THE FAILURE HAS TO SHOW. Without a log, an image that never decodes becomes a
    // silent loop: the drawing asks every frame, the thread tries every frame, and
    // the only symptom is "that card has no art". That is how metahub's WEBP went
    // unnoticed.
    int failed = 0;
    SDL_LockMutex(mtx);
    if (items[idx].state == PENDING && requestStale(&items[idx])) {
      // The image finished after the card left the screen. Do not publish it and do
      // not turn it into a failure: another card may reuse the cold slot.
      items[idx].state = EMPTY;
      items[idx].path[0] = 0;
      if (conv) { SDL_FreeSurface(conv); conv = NULL; }
    } else if (items[idx].state == PENDING) {
      items[idx].luma = lumaMedia;
      items[idx].chroma = chromaMedia;
      items[idx].sup = conv;
      if (conv) {
        items[idx].state = DECODED;
        items[idx].failures = 0;
      } else {
        // It KEEPS the path: it is what identifies the slot on the next lookup and
        // lets it answer "not yet, try later" instead of re-enqueuing. Zeroing the
        // path, as it used to, erased the memory of the failure along with it.
        static const Uint32 INSET[3] = { 2000, 10000, 60000 };
        int k = items[idx].failures;
        items[idx].state = FAILED;
        items[idx].failures = k + 1;
        items[idx].tryIn = SDL_GetTicks() + INSET[k < 3 ? k : 2];
        failed = 1;
      }
    } else if (conv) {
      SDL_FreeSurface(conv);  // the slot was reused along the way
    }
    SDL_UnlockMutex(mtx);

    if (failed) {
      // THE END OF THE PATH, NOT THE START. With %.70s this printed exactly the
      // 70 characters of the application directory and stopped — every line
      // identical, and no way to tell a packaged badge from a cached download.
      // What identifies the file is the last component.
      const char *leaf = strrchr(path, '/');
      // WHAT THE FILE ACTUALLY IS, read here because the next line deletes it.
      // "Unsupported image format" is SDL_image saying no loader recognised the
      // bytes; it does not say WHICH format it declined, and without that the
      // only way forward is guessing. The first twelve bytes name the container
      // outright: 'ftypavif' for AVIF, "RIFF..WEBP", the two bytes of a JPEG,
      // or an HTML error page that arrived with a 200.
      char head[13];
      int got = 0;
      { FILE *hf = fopen(path, "rb");
        if (hf) { got = (int)fread(head, 1, 12, hf); fclose(hf); } }
      printf("[tex] decode failed (%s): %s\n", IMG_GetError(),
             leaf ? leaf + 1 : path);
      if (got > 0) {
        int q;
        printf("[tex]   bytes:");
        for (q = 0; q < got; q++) printf(" %02x", (unsigned char)head[q]);
        printf("  '");
        for (q = 0; q < got; q++)
          putchar((head[q] >= 32 && head[q] < 127) ? head[q] : '.');
        printf("'\n");
      }
      printf("[tex]   from: %.180s\n", origin);
      fflush(stdout);
      // A LOCAL file that does not decode is poisoned: ensureLocal accepts it
      // forever because it is over 512 bytes, so without deleting it here the item
      // would never have art again. It only deletes what is in OUR cache.
      if (dirCache[0] && !strncmp(path, dirCache, strlen(dirCache)))
        remove(path);
    }
  }
}

int tex_start(int max_items) {
  nMax = max_items > 0 && max_items <= MAX_ITEMS_ABS ? max_items : 64;
  // THE BUDGET SCALES WITH THE BUFFER. The 96 MB is the TV's arithmetic, where the
  // scale is 1. On a retina Mac the scale is 2, and the SAME scene needs 4x the
  // pixels — with a fixed ceiling the preview lived pressed against the limit,
  // evicting visible art and showing a defect the device does not have. A preview
  // that lies is worse than no preview.
  //
  // On the TV the factor is 1 and nothing changes; that is exactly why the
  // arithmetic can be this and not a larger hard-coded number.
  { float e = scaleBuf > 0.1f ? scaleBuf : 1.0f;
    budget = (long)(NV_TEX_BUDGET_MB * e * e) * 1024L * 1024L; }
  bytesUsed = 0;
  memset(items, 0, sizeof items);
  mtx = SDL_CreateMutex(); cond = SDL_CreateCond();
  condDec = SDL_CreateCond(); condFree = SDL_CreateCond();
  running = 1;
  // TWO decode threads, not one. The queue is taken under the mutex and each thread
  // carries an index of its own, so more consumers is safe with no other change.
  //
  // One thread was the real limit of "it loads as you go past": with 32 items in
  // flight (freeSlot's ceiling) and ~30 ms per image on this TV, the queue took a
  // second to drain. Two threads halve that.
  //
  // No more than two: there are four weak cores, and the two run at LOW priority
  // precisely so as not to steal the frame from the drawing — the note above, in
  // threadDecode, records that at equal priority the jolt matched exactly with
  // `pending>0`. Four threads would compete with the drawing even at low priority.
  { int k;
    for (k = 0; k < NV_TEX_THREADS; k++)
      thrs[k] = SDL_CreateThread(threadDecode, "nv-decode", NULL);
    // FOUR NETWORK threads, and they are NOT like the decode ones: they touch no
    // pixels, they only wait on I/O. They can run at normal priority and in greater
    // numbers without competing with the drawing — the cost of a thread parked on a
    // socket is zero CPU. Four covers the four posters that come on screen at once.
    for (k = 0; k < NV_TEX_THREADS_NET; k++)
      thrsNet[k] = SDL_CreateThread(threadNet, "nv-net", NULL);
    thr = thrs[0]; }
  return thr != NULL;
}

void tex_shutdown(void) {
  SDL_LockMutex(mtx); running = 0;
  SDL_CondBroadcast(cond); SDL_CondBroadcast(condDec);
  SDL_CondBroadcast(condFree);
  SDL_UnlockMutex(mtx);
  // Wait for BOTH threads. Waiting only for the first left the other decoding into
  // items[] while the loop below was already freeing the surfaces.
  { int k;
    for (k = 0; k < NV_TEX_THREADS; k++)
      if (thrs[k]) { SDL_WaitThread(thrs[k], NULL); thrs[k] = NULL; }
    for (k = 0; k < NV_TEX_THREADS_NET; k++)
      if (thrsNet[k]) { SDL_WaitThread(thrsNet[k], NULL); thrsNet[k] = NULL; }
    thr = NULL; }
  for (int i = 0; i < nMax; i++) {
    if (items[i].tex) glDeleteTextures(1, &items[i].tex);
    if (items[i].sup) SDL_FreeSurface(items[i].sup);
  }
  SDL_DestroyCond(cond); SDL_DestroyMutex(mtx);
}

static GLuint tex_get_limit(const char *path, int limit) {
  if (!path || !*path) return 0;
  GLuint output = 0;
  unsigned long h = hashPath(path);
  int i; SEARCH_MEASURE(i, path, h);
  if (i >= 0 && items[i].state == FAILED) {
    items[i].lastFrame = frameCurrent;
    items[i].lastRequest = SDL_GetTicks();
    // It has already failed: it only goes back into the queue when the backoff
    // expires, and never after the third attempt. Without this the request came
    // back on every frame.
    if (items[i].failures < 3 && SDL_GetTicks() >= items[i].tryIn) {
      int next = (queueEnd + 1) % MAX_QUEUE;
      if (next != queueStart) {
        items[i].state = PENDING;
        items[i].usage = ++lruClock;
        queue[queueEnd] = i; queueEnd = next; SDL_CondSignal(cond);
      }
    }
    SDL_UnlockMutex(mtx);
    return 0;
  }
  if (i >= 0) {
    items[i].lastFrame = frameCurrent;
    items[i].lastRequest = SDL_GetTicks();
    items[i].usage = ++lruClock;
    // PROMOTION: the same art may be asked for as a poster (960) and later as a
    // hero (1920). If the new ceiling is larger and the ready texture came out
    // smaller than it, redo it — otherwise the hero inherits forever the small
    // version the card asked for first, and the blur comes back with no apparent
    // explanation.
    if (limit > items[i].limit) {
      int smallerFont = (items[i].state == READY && items[i].w < items[i].limit);
      items[i].limit = limit;
      // `w < limit` is NOT enough: a Cinemeta poster has a 250px source, and asking
      // for it at 320 redoes the decode to return the same 250 — pure work, plus
      // the grey while it redoes it. If the ready texture is already SMALLER than
      // the ceiling it itself had, the source has run out; there is nothing to gain.
      if (items[i].state == READY && items[i].w < limit && !smallerFont) {
        int next = (queueEnd + 1) % MAX_QUEUE;
        if (next != queueStart) {
          items[i].state = PENDING;
          queue[queueEnd] = i; queueEnd = next; SDL_CondSignal(cond);
        }
      }
    }
    output = items[i].state == READY ? items[i].tex : 0;
  } else {
    int new = slotFree();
    if (new >= 0) {
      strncpy(items[new].path, path, sizeof items[new].path - 1);
      items[new].hash = h;
      items[new].limit = limit;
      items[new].state = PENDING;
      items[new].usage = ++lruClock;
      items[new].lastFrame = frameCurrent;
      items[new].lastRequest = SDL_GetTicks();
      int next = (queueEnd + 1) % MAX_QUEUE;
      if (next != queueStart) { queue[queueEnd] = new; queueEnd = next; SDL_CondSignal(cond); }
      else { items[new].state = EMPTY; items[new].path[0] = 0; } // queue full
    }
  }
  SDL_UnlockMutex(mtx);
  return output;
}

GLuint tex_get(const char *path) {
  return tex_get_limit(path, NV_TEX_WIDTH_MAX);
}

// Art that fills the whole screen: the home's hero, the detail's backdrop and the
// player's art. 1920 is the panel's width — asking for more would only spend
// memory, asking for less means enlarging afterwards.
GLuint tex_get_width(const char *path, float widthLayout) {
  int cap;
  if (widthLayout <= 1.0f) return tex_get(path);
  cap = (int)(widthLayout * scaleBuf * NV_TEX_SLACK + 0.5f);
  // Rounds up to a multiple of 32: without this every drawing width becomes a
  // ceiling of its own, and the same art asked for from two places a few pixels
  // apart was promoted and RE-DECODED with no visible gain.
  cap = ((cap + 31) / 32) * 32;
  if (cap < 128) cap = 128;
  if (cap > NV_TEX_HERO_WIDTH_MAX) cap = NV_TEX_HERO_WIDTH_MAX;
  return tex_get_limit(path, cap);
}

GLuint tex_get_hero(const char *path) {
  return tex_get_limit(path, NV_TEX_HERO_WIDTH_MAX);
}

float tex_aspect(const char *path) {
  if (!path || !*path) return 0.0f;
  float a = 0.0f;
  unsigned long h = hashPath(path);
  int i; SEARCH_MEASURE(i, path, h);
  if (i >= 0 && items[i].state == READY && items[i].h > 0)
    a = (float)items[i].w / (float)items[i].h;
  SDL_UnlockMutex(mtx);
  return a;
}

int tex_failed(const char *path) {
  int r = 0;
  unsigned long h;
  int i;
  if (!path || !*path) return 0;
  h = hashPath(path);
  SEARCH_MEASURE(i, path, h);
  // A path the cache has never seen is not a failure: it is about to be asked
  // for. Only a spent FAILED entry answers yes.
  if (i >= 0 && items[i].state == FAILED && items[i].failures >= 3) r = 1;
  SDL_UnlockMutex(mtx);
  return r;
}

int tex_brand_dark(const char *path) {
  int r = 0;
  unsigned long h;
  int i;
  if (!path || !*path) return 0;
  h = hashPath(path);
  SEARCH_MEASURE(i, path, h);
  // luma < 0 means "not measured yet": answer NO, so as not to tint art that is
  // still to arrive. Err on the side of not touching it.
  if (i >= 0 && items[i].state == READY && items[i].luma >= 0)
    r = (items[i].luma < NV_LOGO_LUMA_MIN && items[i].chroma < NV_LOGO_CHROMA_MAX);
  SDL_UnlockMutex(mtx);
  return r;
}

int tex_pump(int max_per_frame) {
  int rose = 0;
  Uint64 start = SDL_GetPerformanceCounter();
  double freq = (double)SDL_GetPerformanceFrequency();
  for (int step = 0; step < max_per_frame; step++) {
    if (rose > 0 &&
        (double)(SDL_GetPerformanceCounter() - start) * 1000.0 / freq >=
            NV_TEX_UPLOAD_BUDGET_MS)
      break;
    SDL_Surface *sup = NULL; int target = -1;
    SDL_LockMutex(mtx);
    for (int i = 0; i < nMax; i++) {
      if (items[i].state == DECODED && items[i].sup) {
        sup = items[i].sup; items[i].sup = NULL; target = i; break;
      }
    }
    SDL_UnlockMutex(mtx);
    if (!sup) break;

    GLuint t; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, sup->w, sup->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, sup->pixels);
    // Mipmaps serve two things: reducing aliasing when the art appears smaller than
  // the original, and — the reason they came in now — enabling the detail page's
  // blurred background. In GLES2 there is no cheap blur; sampling a small level of
  // the pyramid with a bias is the blur.
  // A MIPMAP ONLY ON WHAT SHRINKS ON SCREEN. The pyramid costs +33% of GPU memory
  // over the texture, and `bytesUsed` does NOT count it — with the budget at 96 MB,
  // the real usage came close to 128 MB and it was the driver that decided what to
  // evict.
  //
  // Full-screen art (hero, backdrop) is drawn 1:1 or ENLARGED: a smaller level of
  // the pyramid is never sampled, so there the pyramid is pure cost. The cards, on
  // the other hand, do appear smaller than the decoded size and need it.
  //
  // Without a mipmap the filter MUST be GL_LINEAR: with MIPMAP_NEAREST on a texture
  // with no pyramid the sampling is undefined and the texture comes out BLACK.
  int comMip = (sup->w < 1024);
  if (comMip) glGenerateMipmap(GL_TEXTURE_2D);
  // MIPMAP_NEAREST and not _LINEAR: trilinear reads TWO levels of the pyramid per
  // sample, and on this GPU that is double the texture cost on every pixel of every
  // card. The visual difference is a step at the transition between levels, which
  // would only show in a continuous zoom animation — which the app does not do.
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                  comMip ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gfx_tex_forget(0);  // the upload's bind went around gfx_rect

    SDL_LockMutex(mtx);
    // PROMOTION LEAKED. When the same art is asked for with a larger ceiling (a
    // poster at 288 and later a hero at 1920), the item goes back to PENDING and
    // comes through here again — and this line overwrote `tex` without deleting the
    // old texture and ADDED the new bytes without subtracting the old ones.
    //
    // Two consequences, both silent: an orphan texture stayed on the GPU with every
    // promotion, and `bytesUsed` only grew, with phantom bytes. With the budget
    // inflated, prune() started evicting earlier and earlier — until it evicted art
    // that was on screen, one frame after it came up. It was one more source of
    // "the poster that disappears".
    if (items[target].tex) {
      gfx_tex_forget(items[target].tex);
      glDeleteTextures(1, &items[target].tex);
    bytesUsed -= bytesTexture(items[target].w, items[target].h);
      if (bytesUsed < 0) bytesUsed = 0;
    }
    items[target].tex = t; items[target].w = sup->w; items[target].h = sup->h;
    items[target].state = READY;
    bytesUsed += bytesTexture(sup->w, sup->h);
    prune();
    SDL_UnlockMutex(mtx);
    SDL_FreeSurface(sup);
    rose++;
  }
  return rose;
}

void tex_stats(int *nItems, int *nPending, long *bytes) {
  int a=0, p=0; long b=0;
  SDL_LockMutex(mtx);
  for (int i = 0; i < nMax; i++) {
    if (items[i].state == READY) { a++; b += bytesTexture(items[i].w, items[i].h); }
    else if (items[i].state != EMPTY) p++;
  }
  SDL_UnlockMutex(mtx);
  if (nItems) *nItems = a;
  if (nPending) *nPending = p;
  if (bytes) *bytes = b;
}
