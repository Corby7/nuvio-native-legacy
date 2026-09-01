// Roteador de telas.
//
// Ate agora o main.c decidia sozinho entre home e detalhe, com um if. Com
// menu, busca, biblioteca, ajustes e player, esse if viraria um emaranhado onde
// cada tela precisa saber das outras. Aqui a regra fica num lugar so: existe
// uma tela CORRENTE, uma PILHA de retorno, e cada tela apenas diz "quero sair"
// ou "abre este titulo".
#ifndef NV_APP_H
#define NV_APP_H
#include <SDL2/SDL.h>

typedef enum {
  TELA_HOME, TELA_BUSCA, TELA_BIBLIOTECA, TELA_AJUSTES, TELA_PLAYER
} Tela;

int  app_iniciar(const char *dirArte);
void app_evento(const SDL_Event *e);
void app_atualizar(float dt, Uint32 agora);
void app_desenhar(Uint32 agora);
int  app_quer_sair(void);
void app_encerrar(void);

#endif
