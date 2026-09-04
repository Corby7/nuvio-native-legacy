// Library screen: tabs along the top and a grid of posters below.
//
// It is the only screen in the app where the content is the user's own SET, not
// an editorial shelf. That is why the tabs are not decoration: "My List" and
// "Purchased" exist on the device as account state and change during use, and
// the screen has to stay readable when that set is empty.
#ifndef NV_LIBRARY_H
#define NV_LIBRARY_H
#include <SDL2/SDL.h>

int  library_start(void);
void library_event(const SDL_Event *e);
void library_update(float dt, Uint32 now);
void library_draw(Uint32 now);
int  library_wants_exit(void);   // 1 quando o Back deve fechar a tela
void library_shutdown(void);

// OK pressed on a poster: consumes the request and returns 1, writing into
// *catalogIndex the item's index IN THE CATALOG (not its position in the grid —
// the grid is filtered, and whoever opens the detail needs the real item).
int  library_requested_open(int *indexCatalog);

// Account state, in memory. Exposed because the thing that marks a title is the
// detail screen (the "+" button), not the library: without this the "My List"
// tab could only be fed from inside this module, and the detail's "+" would
// have nowhere to write.
int  library_na_list(int indexCatalog);
void library_toggle_list(int indexCatalog);
int  library_bought(int indexCatalog);

#endif
