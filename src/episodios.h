#ifndef NV_EPISODIOS_H
#define NV_EPISODIOS_H
#include <SDL2/SDL.h>
void episodios_abrir(int titulo, int temporada, int episodio);
int episodios_aberto(void);
void episodios_evento(const SDL_Event *e);
void episodios_atualizar(float dt);
void episodios_desenhar(void);
int episodios_escolheu(int *temporada, int *episodio);
void episodios_fechar(void);
#endif
