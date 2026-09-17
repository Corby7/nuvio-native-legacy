#ifndef NV_BADGES_H
#define NV_BADGES_H
#include <stdint.h>

// The slack the row leaves AFTER each badge, the last one included — so the width
// badges_draw returns ends one gap past the final mark. A caller that puts its own
// separator after the row has to subtract it, or the separator sits off-centre by
// exactly this much (detail.c does).
#define NV_BADGE_GAP 14.0f
void badges_load(const char *dir);
uint64_t badges_detect(const char *metadata);
uint64_t badges_provider(const char *name);
float badges_draw(uint64_t mask,float x,float y,float maxW,float height,float alpha);

// THE SAME ROW, DECODED TO THE SIZE IT IS DRAWN AT. For the one place a badge is
// not a small tag in a list but a LOCKUP the eye lands on: the provider mark that
// opens the title screen's meta line.
//
// badges_draw goes through tex_get_width, and that has a 128px FLOOR (tex_cache.c)
// — so a mark drawn 64 wide is decoded at 128 and minified 2x, which is where
// GL_LINEAR_MIPMAP_NEAREST snaps to the half-resolution level and magnifies it back.
// That is the softness: it is not the source art, which is 390-798px wide.
//
// This asks through tex_get_exact instead. Keep it to ONE caller per file: the
// exact flag travels with the request, so a second screen asking for the same mark
// at another height in the same frame re-decodes it every frame (see the note on
// `exact` in tex_cache.c). The stream rows and the see-all panel stay on
// badges_draw for that reason — there the badges are 26px tags, where the floor
// costs nothing visible.
float badges_draw_sharp(uint64_t mask,float x,float y,float maxW,float height,float alpha);
#endif
