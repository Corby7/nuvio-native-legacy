#ifndef NV_HOME_H
#define NV_HOME_H
#include <SDL2/SDL.h>
#include "gfx.h"
#include "catalog.h"

// Tipos de fileira presentes na home moderna do Nuvio 1.0.1 legacy.
typedef enum {
  ROW_CONTINUE,   // card 16:9 com barra de progresso
  ROW_NORMAL,      // poster retrato 2:3
  ROW_HIGHLIGHT,    // seleção editorial: arte landscape maior
  ROW_TOP10,       // ranking usa poster retrato no legacy
  ROW_COLLECTION,     // premiações / coleções: landscape intermediário
  ROW_SERVICE,     // catálogo por serviço: landscape compacto
  ROW_SOCIAL,      // atividade dos amigos: editorial largo com autoria
  ROW_RETURN,     // sessão recém-interrompida: faixa compacta de retomar
  ROW_CATALOGS    // atalhos para catálogos existentes, não títulos
} KindRow;

// The item under the focus, with the rectangle it occupies on screen THIS
// frame. The transition to the detail starts from there: the card does not "open
// a new screen", it flies until it becomes the hero — and without the real
// source rect the animation would have to guess where it came from, which is
// exactly what makes a transition look cheap.
typedef struct {
  // The focused card's index IN THE CATALOGUE. It was missing, which is why the
  // detail always opened item 0: the destination screen had no way to know what
  // had been opened.
  int index_;
  GfxRect rect;
  const char *art;
  const char *title;
  const char *genre;
  const char *meta;
} HomeItem;

int  home_start(const char *dirArt);
int  home_item_focused(HomeItem *out);      // 0 se o foco ainda nao foi desenhado
int  home_n_arts(void);                   // acervo de backdrops, usado pelo detalhe
const char *home_art(int i);
const char *home_backdrop(int i);   // arte do titulo i do catalogo
void home_event(const SDL_Event *e);
void home_update(float dt, Uint32 now);
void home_draw(Uint32 now);

// Where the hero's ART was drawn on the last frame. The detail screen uses this
// to be born with the backdrop in the same place — the detail's background IS
// the focused title's art, and it should not reappear, it should continue.
void home_hero_rect(float *x, float *y, float *w, float *h);
void home_shutdown(void);
// Records the interrupted title for the contextual "Resume now" band. The band
// only exists while the progress makes sense (neither the start nor the end).
void home_registrar_return(int index_, double posSeg, double durationSeg);
int  home_wants_exit(void);
int  home_requested_open(void);   // OK pressionado: consome o pedido
int  home_requested_menu(void);    // ESQUERDA na primeira coluna: chama o menu
int home_requested_social(void);
int home_requested_person_social(CatItem *output);

#endif
