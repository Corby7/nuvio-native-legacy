// "Add lists": the list finder opened from the Home's footer, over the Home.
//
// Settings already had a finder (Settings > Trakt lists), but it is a column of
// names: a list's title says little about what its row would look like. This one
// shows each result AS THE ROW IT WOULD BE — the list's name as a row heading and
// its first posters at the Home's own card size — so the owner can see a list
// before putting it on the Home. Trakt and MDBList are both searchable here; the
// added lists are traktlists.h's, the same set Settings edits.
//
// Up and down walk the results, left and right the preview's posters, OK adds the
// focused list to the Home or takes it off. The Home is rebuilt once, on close.
#ifndef NV_LISTADD_H
#define NV_LISTADD_H
#include <SDL2/SDL.h>

void listadd_open(void);
int  listadd_is_open(void);
// 1 once the fade-in is complete: the screen underneath need not be drawn.
int  listadd_covers_screen(void);
void listadd_event(const SDL_Event *e);
void listadd_update(float dt, Uint32 now);
void listadd_draw(Uint32 now);

#endif
