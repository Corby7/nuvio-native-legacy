#ifndef NV_SOCIAL_H
#define NV_SOCIAL_H
#include <SDL2/SDL.h>
#include "catalog.h"
typedef enum { SOCIAL_STATE_LOADING = 0, SOCIAL_STATE_UPDATING,
  SOCIAL_STATE_READY, SOCIAL_STATE_STALE, SOCIAL_STATE_NO_ACTIVITY,
  SOCIAL_STATE_PRIVATE, SOCIAL_STATE_DISCONNECTED,
  SOCIAL_STATE_UNAVAILABLE } SocialState;
void social_open(const CatItem *person);
void social_event(const SDL_Event *e);
void social_update(float dt, Uint32 now);
void social_draw(Uint32 now);
int social_wants_exit(void);
SocialState social_state(void);
typedef struct {
  char imdb[16];
  char title[160];
} SocialItemSelected;
int social_item_selected(SocialItemSelected *output);
#endif
