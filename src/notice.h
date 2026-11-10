// THE NOTICE: the one shape every short message on screen takes — the film
// player's toasts, hints and failures, Live TV's, and the home's "update
// available". A dark pill at the top centre led by a round badge drawn as the
// hold ring, with a glyph inside. It began in player.c; Live TV kept a plain
// text pill of its own until both were given this.
#ifndef NV_NOTICE_H
#define NV_NOTICE_H
#include <SDL2/SDL.h>

// What the ring says. GOOD and MODE: the logo's colours. BUSY: an arc running
// round a dim track. WARN and ERROR: solid amber / red, ring and glyph.
// MODE is the same shape a size up and lower down (the aspect key).
enum { NOTE_GOOD, NOTE_WARN, NOTE_ERROR, NOTE_BUSY, NOTE_MODE };

// `text` splits at its first " · ": what happened in white, the detail in grey.
// `icon` is a name under art/icons. `a` is the caller's fade, `from` when the
// notice came up (for the drop-in), `now` the frame's tick.
void notice_draw(const char *text, const char *icon, int kind, float a,
                 Uint32 from, Uint32 now);
#endif
