#include "plane.h"

#include <stdio.h>
#include <string.h>

#ifdef __APPLE__
// No compositor and no video plane on the Mac. The stubs let the rest of the
// app compile and run the same, just without a moving image. Every function in
// plane.h has to appear in BOTH branches — the mirror of the trap already
// documented in video.c: the Mac does not compile the device half, so it is the
// link step that finds a missing stub, and only after everything else built.
int  plane_start(void *display, void *surface) { (void)display; (void)surface; return 0; }
int  plane_ready(void) { return 0; }
const char *plane_window_id(void) { return ""; }
void plane_window(int sx, int sy, int sw, int sh, int dx, int dy, int dw, int dh) {
  (void)sx; (void)sy; (void)sw; (void)sh; (void)dx; (void)dy; (void)dw; (void)dh;
}
void plane_pump(void) {}
void plane_forget(void) {}
void plane_stop(void) {}
#else

#include <dlfcn.h>
#include <stdint.h>

// Only the head of wl_interface is needed, and only to read `name` and
// `version`. Declaring the whole thing would mean copying wl_message too, for
// no gain: the compositor is matched by the NAME the library itself carries, so
// a rename upstream cannot desync this file.
struct WlInterface {
  const char *name;
  int         version;
  int         methodCount;
  const void *methods;
  int         eventCount;
  const void *events;
};

// Wire opcodes. Taken from the request order in the protocol XML, which is what
// defines them; the generated headers only wrap these same numbers.
//   wayland.xml            wl_display.get_registry      = 1
//                          wl_registry.bind             = 0
//                          wl_compositor.create_region  = 1
//                          wl_region.destroy            = 0
//                          wl_region.add                = 1
//   webos-foreign.xml      wl_webos_foreign.destroy     = 0
//                          wl_webos_foreign.export_element = 1
//                          wl_webos_exported.destroy    = 0
//                          wl_webos_exported.set_exported_window = 1
#define OP_GET_REGISTRY   1
#define OP_BIND           0
#define OP_CREATE_REGION  1
#define OP_REGION_DESTROY 0
#define OP_REGION_ADD     1
#define OP_EXPORT_ELEMENT 1
#define OP_EXPORTED_WINDOW 1
#define OP_DESTROY        0

// webos_exported_type. The video plane is 0; the other three (subtitle,
// transparent, opaque) are not used here.
#define EXPORTED_VIDEO 0

// The variadic prototypes have to be declared variadic, not "close enough":
// on ARM EABI the calling convention for a variadic function differs from the
// fixed one, and a mismatch here would corrupt the argument list rather than
// fail to compile.
static void *(*proxyConstruct)(void *, uint32_t, const struct WlInterface *, ...);
static void *(*proxyConstructVer)(void *, uint32_t, const struct WlInterface *, uint32_t, ...);
static void  (*proxyMarshal)(void *, uint32_t, ...);
static int   (*proxyListen)(void *, void (**)(void), void *);
static void  (*proxyDestroy)(void *);
static int   (*displayRoundtrip)(void *);
static int   (*displayFlush)(void *);
static int   (*displayDispatchPending)(void *);

static const struct WlInterface *ifaceRegistry;
static const struct WlInterface *ifaceRegion;
static const struct WlInterface *ifaceCompositor;
static const struct WlInterface *ifaceForeign;
static const struct WlInterface *ifaceExported;

static void *display;
static void *surface;
static void *registry;
static void *compositor;
static void *foreign;
static void *exported;

// The id the compositor assigns. 64 bytes is generous: what this TV returns is
// of the shape "_Window_Id_1".
static char windowId[64];
static int  assigned;

// Last rectangles applied, so the same pair is not re-sent every frame. -1 in
// srcW means "nothing applied yet".
static int lastSx, lastSy, lastSw = -1, lastSh;
static int lastDx, lastDy, lastDw, lastDh;

// ---------------------------------------------------------------- listeners

// wl_registry.global. The interface is matched against the name the LIBRARY
// itself carries, never against a literal typed here. The two cannot desync
// that way, and the failure this avoids is a quiet one: a name that does not
// match means the app exports nothing, the load then goes out with an empty
// window id, and the symptom looks like a broken pipeline rather than a typo.
static void onGlobal(void *data, void *reg, uint32_t name,
                     const char *iface, uint32_t version) {
  (void)data;
  if (!iface) return;
  if (ifaceForeign && !strcmp(iface, ifaceForeign->name) && !foreign) {
    uint32_t v = version;
    if (ifaceForeign->version > 0 && v > (uint32_t)ifaceForeign->version)
      v = (uint32_t)ifaceForeign->version;
    foreign = proxyConstructVer(reg, OP_BIND, ifaceForeign, v,
                                name, ifaceForeign->name, v, NULL);
    printf("[plane] foreign '%s' v%u -> %p\n", iface, v, foreign);
  } else if (ifaceCompositor && !strcmp(iface, ifaceCompositor->name) && !compositor) {
    uint32_t v = version > 1 ? 1 : version;
    compositor = proxyConstructVer(reg, OP_BIND, ifaceCompositor, v,
                                   name, ifaceCompositor->name, v, NULL);
    printf("[plane] compositor v%u -> %p\n", v, compositor);
  }
}

static void onGlobalGone(void *data, void *reg, uint32_t name) {
  (void)data; (void)reg; (void)name;
}

// wl_webos_exported.window_id_assigned. This is the whole point of the export:
// the string that goes into the com.webos.media load payload.
static void onWindowId(void *data, void *exp, const char *id, uint32_t type) {
  (void)data; (void)exp;
  if (!id || !*id) {
    printf("[plane] window_id_assigned with an EMPTY id — the load would be silently ignored\n");
    fflush(stdout);
    return;
  }
  snprintf(windowId, sizeof windowId, "%s", id);
  assigned = 1;
  printf("[plane] window id '%s' (type %u)\n", windowId, type);
  fflush(stdout);
}

// Listener tables must OUTLIVE the call: wl_proxy_add_listener stores the
// pointer, it does not copy the table. A table on the stack works for exactly
// as long as the function that registered it, and then dispatches into
// whatever replaced the frame.
static void (*listenRegistry[2])(void) = {
  (void (*)(void))onGlobal, (void (*)(void))onGlobalGone
};
static void (*listenExported[1])(void) = {
  (void (*)(void))onWindowId
};

// ---------------------------------------------------------------- internals

#define BIND(h, v, n) do {                                      \
    *(void **)(&v) = dlsym(h, n);                               \
    if (!v) { printf("[plane] missing %s\n", n); return 0; }    \
  } while (0)

static void *makeRegion(int x, int y, int w, int h) {
  void *r = proxyConstruct(compositor, OP_CREATE_REGION, ifaceRegion, NULL);
  if (!r) return NULL;
  proxyMarshal(r, OP_REGION_ADD, x, y, w, h);
  return r;
}

static void dropRegion(void *r) {
  if (!r) return;
  proxyMarshal(r, OP_REGION_DESTROY);
  proxyDestroy(r);
}

// ------------------------------------------------------------------ public

int plane_start(void *dpy, void *surf) {
  void *W, *X;
  if (exported) return 1;
  if (!dpy || !surf) { printf("[plane] no wayland display/surface\n"); return 0; }
  display = dpy;
  surface = surf;

  W = dlopen("libwayland-client.so.0", RTLD_NOW);
  if (!W) W = dlopen("libwayland-client.so", RTLD_NOW);
  // The webOS extension library. Without it there is no foreign interface and
  // no export; the app still runs, and playback reports a source error.
  X = dlopen("libwayland-webos-client.so.1", RTLD_NOW);
  if (!X) X = dlopen("libwayland-webos-client.so", RTLD_NOW);
  if (!W || !X) { printf("[plane] libs: %s\n", dlerror()); return 0; }

  BIND(W, proxyConstruct,    "wl_proxy_marshal_constructor");
  BIND(W, proxyConstructVer, "wl_proxy_marshal_constructor_versioned");
  BIND(W, proxyMarshal,      "wl_proxy_marshal");
  BIND(W, proxyListen,       "wl_proxy_add_listener");
  BIND(W, proxyDestroy,      "wl_proxy_destroy");
  BIND(W, displayRoundtrip,  "wl_display_roundtrip");
  BIND(W, displayFlush,      "wl_display_flush");
  BIND(W, displayDispatchPending, "wl_display_dispatch_pending");

  // Interfaces are DATA symbols, not functions. dlsym returns them the same way.
  BIND(W, ifaceRegistry,   "wl_registry_interface");
  BIND(W, ifaceRegion,     "wl_region_interface");
  BIND(W, ifaceCompositor, "wl_compositor_interface");
  BIND(X, ifaceForeign,    "wl_webos_foreign_interface");
  BIND(X, ifaceExported,   "wl_webos_exported_interface");

  registry = proxyConstruct(display, OP_GET_REGISTRY, ifaceRegistry, NULL);
  if (!registry) { printf("[plane] no registry\n"); return 0; }
  proxyListen(registry, listenRegistry, NULL);

  // One roundtrip to receive the globals, a second because the bind requests
  // issued from inside the first one's dispatch have not been answered yet.
  displayRoundtrip(display);
  displayRoundtrip(display);

  if (!foreign) {
    printf("[plane] compositor does not advertise '%s' — no video plane\n",
           ifaceForeign->name);
    fflush(stdout);
    return 0;
  }
  if (!compositor) {
    printf("[plane] no wl_compositor — cannot build regions\n");
    fflush(stdout);
    return 0;
  }

  exported = proxyConstruct(foreign, OP_EXPORT_ELEMENT, ifaceExported,
                            NULL, surface, (uint32_t)EXPORTED_VIDEO);
  if (!exported) { printf("[plane] export_element failed\n"); return 0; }
  proxyListen(exported, listenExported, NULL);

  // The id arrives as an event, so it needs a dispatch to land. It is fetched
  // HERE, at startup, and not at the first play: the load cannot go out without
  // it, and discovering that inside the play path would mean either blocking
  // the render thread or sending a load that fails silently.
  displayFlush(display);
  displayRoundtrip(display);

  printf("[plane] exported=%p assigned=%d id='%s'\n", exported, assigned, windowId);
  fflush(stdout);
  return 1;
}

// Counted in FRAMES rather than milliseconds on purpose: this file does not
// know about SDL, and the draw loop is already the clock. ~30 frames is half a
// second; 12 tries cover about 6 s, far longer than the compositor took to
// answer at startup.
#define PLANE_TRY_EVERY 30
#define PLANE_TRY_MAX   12

void plane_pump(void) {
  static int frame, tries;
  if (!exported || assigned) return;
  // Cheap and non-blocking: if the event is already queued (SDL reads the same
  // default queue every frame) this delivers it without talking to the
  // compositor at all.
  if (displayDispatchPending) displayDispatchPending(display);
  if (assigned || tries >= PLANE_TRY_MAX) return;
  if (++frame < PLANE_TRY_EVERY) return;
  frame = 0;
  tries++;
  // A roundtrip BLOCKS the frame. That is acceptable exactly here: without an
  // id there is no video to draw anyway, and the connection is idle.
  displayFlush(display);
  displayRoundtrip(display);
  if (!assigned && tries >= PLANE_TRY_MAX) {
    printf("[plane] NO window id after %d roundtrips — playback will refuse to "
           "load, because a load without it reports success and fetches nothing\n",
           tries);
    fflush(stdout);
  }
}

int plane_ready(void) { return assigned && windowId[0]; }

const char *plane_window_id(void) { return windowId; }

void plane_window(int sx, int sy, int sw, int sh,
                  int dx, int dy, int dw, int dh) {
  void *src, *dst;
  if (!exported || !compositor) return;
  // A degenerate region is not a smaller picture, it is an undefined one. The
  // ACB path had the same guard for the same reason.
  if (sw < 2 || sh < 2 || dw < 1 || dh < 1) return;
  if (sx == lastSx && sy == lastSy && sw == lastSw && sh == lastSh &&
      dx == lastDx && dy == lastDy && dw == lastDw && dh == lastDh) return;

  src = makeRegion(sx, sy, sw, sh);
  dst = makeRegion(dx, dy, dw, dh);
  if (!src || !dst) {
    dropRegion(src); dropRegion(dst);
    printf("[plane] could not build the regions\n"); fflush(stdout);
    return;
  }
  proxyMarshal(exported, OP_EXPORTED_WINDOW, src, dst);
  // The compositor reads the regions when it processes the request, and
  // requests are processed in order, so releasing them right after is safe and
  // is what keeps a resize from leaking one region pair per call.
  dropRegion(src);
  dropRegion(dst);
  displayFlush(display);

  lastSx = sx; lastSy = sy; lastSw = sw; lastSh = sh;
  lastDx = dx; lastDy = dy; lastDw = dw; lastDh = dh;
  printf("[plane] source %d,%d %dx%d -> destination %d,%d %dx%d\n",
         sx, sy, sw, sh, dx, dy, dw, dh);
  fflush(stdout);
}

void plane_forget(void) { lastSw = -1; }

void plane_stop(void) {
  if (exported) {
    proxyMarshal(exported, OP_DESTROY);
    proxyDestroy(exported);
    exported = NULL;
  }
  assigned = 0;
  windowId[0] = 0;
  lastSw = -1;
}
#endif
