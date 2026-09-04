// "SEE ALL" SCREEN: a whole catalogue in a grid, opened from the card at the
// end of a home row.
//
// It exists because the row shows 12 items and the catalogue has hundreds — on
// the web this is `catalogSeeAllScreen`, opened by the `openCatalogSeeAll`
// card. Without it the rest of each catalogue was unreachable: there was NO
// path in the app to see item 13 of a row.
//
// The grid is 5 columns at 1920, measured from the web (.seeall-card is 248px
// wide, and the body reserves 368px for the side panel). The detail panel the
// web has on the right is NOT in this version — it repeats what the title
// screen already shows, and the grid alone solves what was missing.
#ifndef NV_SEEALL_H
#define NV_SEEALL_H
#include <SDL2/SDL.h>
#include "collections.h"
void seeall_collection(const ColFolder *folder);

// Opens with the catalogue behind a home row.
void seeall_open(const char *base, const char *kind, const char *catId,
                   const char *title);
int  seeall_is_open(void);
void seeall_event(const SDL_Event *e);
void seeall_update(float dt, Uint32 now);
void seeall_draw(Uint32 now);
// Indice no catalogo global do titulo que o dono abriu, ou -1. Consumido uma
// vez: o roteador chama, abre o detalhe e a tela se fecha.
int  seeall_requested_open(void);
#endif
