#ifndef NV_EPISODES_H
#define NV_EPISODES_H
#include <SDL2/SDL.h>
void episodes_open(int title, int season, int episode);
int episodes_is_open(void);
void episodes_event(const SDL_Event *e);
void episodes_update(float dt);
void episodes_draw(void);
int episodes_chose(int *season, int *episode);
void episodes_close(void);
#endif
