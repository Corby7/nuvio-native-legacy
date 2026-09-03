#ifndef NV_SOCIAL_H
#define NV_SOCIAL_H
#include <SDL2/SDL.h>
#include "catalogo.h"
void social_abrir(const CatItem *pessoa);
void social_evento(const SDL_Event *e);
void social_atualizar(float dt, Uint32 agora);
void social_desenhar(Uint32 agora);
int social_quer_sair(void);
#endif
