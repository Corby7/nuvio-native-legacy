#ifndef NV_RESUME_H
#define NV_RESUME_H
#include "catalog.h"
#include "gfx.h"

// Content over the cover art; the home stays responsible for the image and focus.
void resume_draw(const CatItem *item, GfxRect card);
#endif
