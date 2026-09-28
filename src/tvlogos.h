// Better channel logos for Live TV, from the community tv-logo/tv-logos
// repository: 512px marks on a transparent ground, where providers ship 96px
// ones, often a grey variant.
//
// The index (art/tv-logos.txt) is built by tools/build-tv-logos.mjs and pins
// one commit of the repo, so a URL it gives never moves. A playlist name is
// matched by its words, quality tags dropped ("UK: SKY SPORTS F1 ᴿᴬᵂ HD" is
// sky-sports-f1), and by its country prefix: a known country takes only that
// country's logo, or an international one; no prefix takes a name that exists
// in one country only. Anything less certain is no match, and the provider's
// logo stays: a wrong logo is worse than a small one.
#ifndef NV_TVLOGOS_H
#define NV_TVLOGOS_H
#include <stddef.h>

// The folder that holds tv-logos.txt. Read on the first lookup, not here.
void tvlogos_dir(const char *dirArt);

// The tv-logos URL for the playlist channel called `name`, into `out`.
// 1 when there is one; 0 (and `out` empty) when there is none, or no index.
int tvlogos_find(const char *name, char *out, size_t n);

#endif
