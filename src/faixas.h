// Folha de AUDIO E LEGENDA, aberta pelos icones do player.
//
// Duas colunas num painel so, como no aparelho: a esquerda o audio, a direita
// a legenda. Separar em duas telas obrigaria a sair e voltar para conferir o
// par escolhido, que e justamente o que se quer comparar.
//
// A lista de legendas junta as EMBUTIDAS no arquivo (o pipeline as enxerga) com
// as do OpenSubtitles (baixadas pelo addon). Sao coisas diferentes na origem e
// a mesma coisa para quem assiste, entao aparecem juntas, marcadas.
#ifndef NV_FAIXAS_H
#define NV_FAIXAS_H
#include <SDL2/SDL.h>

void faixas_abrir(void);
int  faixas_aberta(void);
void faixas_evento(const SDL_Event *e);
void faixas_atualizar(float dt, Uint32 agora);
void faixas_desenhar(Uint32 agora);

#endif
