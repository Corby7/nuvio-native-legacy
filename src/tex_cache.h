// A texture cache with the decode OUTSIDE the drawing thread.
//
// Why a thread: a decode measured on the device costs ~30ms per image. At 60fps
// the whole frame is 16.6ms — decoding inline means losing 2 frames for every
// card that comes on screen. The thread decodes into memory; the drawing thread
// only does the GL upload (which needs the context and is cheap).
//
// The policy: LRU with an item cap. Without a cap, walking the whole catalogue
// blows the app's memory — the TV has a tight budget and we have already seen
// the web app at 266MB.
#ifndef NV_TEX_CACHE_H
#define NV_TEX_CACHE_H
#include "gl_compat.h"

int  tex_start(int max_items);

// The folder where images fetched from a URL are stored on disk. Without it,
// tex_get with http(s) simply does not load — the app does not break, it just
// has no art.
void tex_cache_dir(const char *dir);
void tex_shutdown(void);

// Returns the texture if it is already ready; otherwise 0, and queues the
// decode. Never blocks the drawing thread.
GLuint tex_get(const char *path);
// The same thing, with a 1920 cap: for art that fills the whole screen (the
// home's hero, the detail's backdrop, the player's art). With the ordinary 960
// cap those three were decoded at half resolution and scaled up on screen.
GLuint tex_get_hero(const char *path);

// DOWNLOADS THE ART AND STOPS THERE — no slot, no decode, no texture.
//
// It exists for the art we know we are ABOUT to need but have not been asked to
// draw: the hero the focus is resting towards, and the neighbours in the
// direction of travel. tex_get_hero cannot serve that purpose, because it costs
// a 1920 texture (~8 MB) the moment it is called — and the note at
// NV_HERO_IDLE_MS records what a dozen of those across one row does to the
// budget: it evicts the posters that are on screen.
//
// So this asks for the only part that is BOTH expensive and free of memory: the
// file. It goes into the disk cache; when the real request follows, ensureLocal
// finds it there and only the decode is left. The 220 ms rest period before the
// hero swaps then holds the download instead of coming before it.
//
// It never blocks, never allocates a slot, and answers nothing: a failure here
// is not recorded and not retried, because the request that follows will do the
// download properly, with the backoff and the FAILED state.
void tex_prefetch(const char *path);

// The scale between a BUFFER pixel and a layout pixel (1 on the TV, 2 on a
// retina Mac). Set once at startup, alongside the text's.
void tex_scale(float e);

// Like tex_get, but saying AT WHAT WIDTH the art will be drawn, in layout
// pixels. The decode cap comes from that, instead of the single 640 default —
// which was sized by the largest card art and charged the same price for a
// 212-wide poster. See the note in tex_cache.c: it is the difference between
// ~40 textures fitting the budget and ~230.
//
// Prefer this over tex_get for any list art: that is where the cache blows.
GLuint tex_get_width(const char *path, float widthLayout);

// The aspect (w/h) of the already-loaded texture; 0 if it is not ready yet.
// Needed for the shader's "cover" — without it the art stretches.
float tex_aspect(const char *path);

// 1 when the cache has GIVEN UP on this art: the decode failed and the retries
// are spent. It answers 0 while a request is still in flight.
//
// It exists because from the outside a decode in flight and a dead URL look
// IDENTICAL — tex_get* answer 0 for both — and a caller that writes "Art
// unavailable" on a 0 is lying for the whole length of the download. The
// threshold is the same >= 3 failures that slotFree uses to reuse the slot:
// below it a retry is still scheduled, so the art is late, not missing.
int  tex_failed(const char *path);

// 1 when the art is a DARK AND ACHROMATIC mark — the black-logo case — and so
// should be drawn tinted (GFX_MARK) instead of with its own colours.
//
// It exists because of THE TITLE'S LOGO. TMDB serves the same mark in a light and
// a dark version and does NOT say which is which: there is no field for it, and
// the web app's own ranking sorts only by language and score. When the dark one
// comes up, it vanishes against the dark backdrop.
//
// THERE ARE TWO CONDITIONS, and the second matters as much as the first: dark
// enough (luminance) AND with no colour of its own (chroma). Luminance alone
// would also tint a dark-red brand logo white, which is a deliberate colour and
// not the wrong variant — it would swap one defect for another. Both are
// measured exactly once, on the decode thread, sampling 1/16 of the opaque
// pixels.
//
// It answers 0 while the texture has not loaded: not tinting is the safe
// default.
int  tex_brand_dark(const char *path);

// Call once per frame, on the drawing thread: uploads to the GPU whatever the
// decode thread has finished. Returns how many it uploaded.
int tex_pump(int max_per_frame);

// The cache's per-frame telemetry: how many lookups by path and what they cost.
// findIndex was LINEAR over 192 slots and every card in the list calls it 2-3
// times per frame; these numbers say whether that actually weighs or not.
extern int    tex_n_search;
extern double tex_ms_search;
void tex_new_frame(void);

void tex_stats(int *items, int *pending, long *bytes);

#endif
