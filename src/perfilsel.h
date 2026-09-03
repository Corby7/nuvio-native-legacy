// Escolha de perfil — a tela entre o login e a home.
//
// Ela aparece UMA vez por instalacao (a escolha e gravada) e so quando a conta
// tem mais de um perfil. Numa conta de uma pessoa so, mostrar isto seria uma
// pergunta com uma resposta possivel.
//
// O PIN e verificado NO SERVIDOR (verify_profile_pin). Guardar o PIN aqui para
// comparar localmente seria guardar o segredo no aparelho — e um perfil
// travado existe justamente para o aparelho nao poder abri-lo sozinho.
#ifndef NV_PERFILSEL_H
#define NV_PERFILSEL_H
#include <SDL2/SDL.h>

void perfilsel_iniciar(void);
void perfilsel_evento(const SDL_Event *e);
void perfilsel_atualizar(float dt, Uint32 agora);
void perfilsel_desenhar(Uint32 agora);
int  perfilsel_concluido(void);

#endif
