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

// FOR ART THAT MUST COME OUT SHARP AT ONE FIXED SIZE: the brand lockup, the rail's
// icons — UI furniture, drawn at a width the layout already knows and never
// animates.
//
// tex_get_width deliberately asks for MORE than the drawing width (NV_TEX_SLACK,
// then rounded up to a multiple of 32) so that a card taking focus does not force a
// second decode. The cost of that headroom is that the texture is always between
// 1.25x and 1.6x the size it is drawn at — and the minification filter is
// GL_LINEAR_MIPMAP_NEAREST, which SNAPS at a scale factor of 1.414: below it the
// GPU samples level 0 and undersamples (aliasing, jagged edges); above it the GPU
// takes level 1, HALF the resolution, and magnifies it back up (blur). The wordmark
// in the side rail landed at 1.4147 and came out visibly soft.
//
// This asks for the drawing width EXACTLY — no slack, no rounding — and the texture
// is uploaded WITHOUT a mipmap chain and with GL_LINEAR, so what reaches the screen
// is a 1:1 blit and the filter never has a choice to get wrong.
//
// Only for art whose drawn size is fixed: an entry re-decodes when the requested
// width moves (in EITHER direction, unlike the promotion tex_get_width relies on),
// so asking with a size that animates would decode on every frame.
GLuint tex_get_exact(const char *path, float widthLayout);

// The aspect (w/h) of the already-loaded texture; 0 if it is not ready yet.
// Needed for the shader's "cover" — without it the art stretches.
//
// It is the SOURCE file's aspect, not the decoded texture's. Those differ: the
// decode scales to a width ceiling and the height follows as an integer division,
// so a 221x41 wordmark capped at 160 becomes 160x29 — an aspect of 5.517 where the
// file's is 5.390. Callers size their box from this (`w = h * tex_aspect(...)`), so
// the truncation was reaching the screen as a 2% horizontal stretch on the very art
// that is most sensitive to it, the wordmarks. It also has to be STABLE for
// tex_get_exact: a caller that derives its width from the aspect and then asks for
// a decode at that width would otherwise chase its own tail, each decode moving the
// aspect that chooses the next one.
float tex_aspect(const char *path);

// The source file's width in pixels, 0 until a texture exists. For art that
// comes in small (IPTV logos are often 96px) and must not be blown up past what
// it holds: the caller caps its drawing scale on it.
int  tex_source_width(const char *path);

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

// Where the VISIBLE pixels sit, as fractions of the image: box = {x0, y0, x1, y1}.
// Logos come with arbitrary transparent margins, so two marks drawn in the same
// box can end up very different sizes on screen; this is what lets a caller size
// the mark and not the file. Answers 0 (box untouched) until the texture exists,
// or when the art has nothing solid in it.
int  tex_content_box(const char *path, float box[4]);

// Call once per frame, on the drawing thread: uploads to the GPU whatever the
// decode thread has finished. Returns how many it uploaded.
int tex_pump(int max_per_frame);

// The entrance of freshly uploaded art, 0..1: 0 the frame it reached the GPU,
// 1 once NV_TEX_APPEAR_MS have passed — and 1 for anything already resident,
// for a promotion to a bigger decode, and with reduced motion. A draw site that
// has a skeleton lays it down first and multiplies the art's alpha by this.
float tex_appear(GLuint t);

// The cache's per-frame telemetry: how many lookups by path and what they cost.
// findIndex was LINEAR over 192 slots and every card in the list calls it 2-3
// times per frame; these numbers say whether that actually weighs or not.
extern int    tex_n_search;
extern double tex_ms_search;
void tex_new_frame(void);

void tex_stats(int *items, int *pending, long *bytes);

#endif
