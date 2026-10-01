#ifndef NV_RESUME_H
#define NV_RESUME_H
#include "catalog.h"
#include "gfx.h"

// Content over the cover art; the home stays responsible for the image and focus.
// `baked`: the art was drawn as GFX_CW_CARD, which already carries the scrim
// and the progress bar (resume_bar), so neither is drawn again here.
void resume_draw(const CatItem *item, GfxRect card, int baked);

// The progress bar for GFX_CW_CARD: its height as a fraction of the card's and
// its fill, or 0 with *band = 0 when the title has no bar.
int resume_bar(const CatItem *item, GfxRect card, float *band, float *fill);

// A progress bar's DRAWN fill for one title/episode: `target` (0..1) the first
// time it is asked, then chasing each new target on a spring. See the note in
// resume.c; `id` plus season/episode say which bar this is.
float resume_fill(const char *id, int season, int episode, float target);
#endif
