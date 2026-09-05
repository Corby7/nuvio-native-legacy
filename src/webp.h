// WebP decoding, through the DEVICE's libwebp, opened with dlopen.
//
// The same arrangement as net.c and for the same reason: the SDK has no libwebp
// to link against, and the TV does have /usr/lib/libwebp.so.7. Here there is a
// second reason on top of that one — the TV's libSDL2_image EXPORTS
// IMG_LoadWEBP_RW and IMG_isWEBP and neither of them works. MEASURED on the C3
// with a test binary run on the device:
//
//     IMG_Init -> 0x3  JPG:yes PNG:yes WEBP:NO
//     IMG_GetError: 'WEBP images are not supported'
//     IMG_isWEBP on a real WebP: 0
//
// So asking SDL_image is not an option and the format cannot be ignored either:
// art/badges ships 42 .webp files, and the metadata addon serves part of its
// posters and backdrops as WebP — some of them behind URLs ending in .jpg, so
// the extension does not save anyone. Every one of those was a card with no art
// and, since the caption exists, an "Art unavailable" that was telling the
// truth for a reason nobody could see.
#ifndef NV_WEBP_H
#define NV_WEBP_H

#include <SDL2/SDL.h>

// 1 when the bytes are a WebP ("RIFF" + 4 bytes of size + "WEBP"). Cheap, and
// it does not need the library to answer.
int webp_is(const void *data, long n);

// Decodes a WebP FILE into a fresh surface, or NULL. NULL covers every case
// where this module is not the answer: the file is not WebP, libwebp is not on
// the device, or the data is corrupt. Whoever calls carries on with IMG_Load.
//
// The surface comes as ABGR8888, which is what tex_cache converts everything to
// anyway — returning it ready avoids one full copy per image.
SDL_Surface *webp_load(const char *path);

#endif
