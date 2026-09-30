#ifndef NV_RESUME_H
#define NV_RESUME_H
#include "catalog.h"
#include "gfx.h"

// Content over the cover art; the home stays responsible for the image and focus.
void resume_draw(const CatItem *item, GfxRect card);

// A progress bar's DRAWN fill for one title/episode: `target` (0..1) the first
// time it is asked, then chasing each new target on a spring. See the note in
// resume.c; `id` plus season/episode say which bar this is.
float resume_fill(const char *id, int season, int episode, float target);
#endif
