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
// 0..1, how far the panel is in: the player fades its own controls on it, as it
// does for the sources and track sheets.
float episodes_shown(void);
#endif
