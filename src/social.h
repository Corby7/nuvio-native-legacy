#ifndef NV_SOCIAL_H
#define NV_SOCIAL_H
#include <SDL2/SDL.h>
#include "catalogo.h"
typedef enum { SOCIAL_ESTADO_CARREGANDO = 0, SOCIAL_ESTADO_ATUALIZANDO,
  SOCIAL_ESTADO_PRONTO, SOCIAL_ESTADO_STALE, SOCIAL_ESTADO_SEM_ATIVIDADE,
  SOCIAL_ESTADO_PRIVADO, SOCIAL_ESTADO_DESCONECTADO,
  SOCIAL_ESTADO_INDISPONIVEL } SocialEstado;
void social_abrir(const CatItem *pessoa);
void social_evento(const SDL_Event *e);
void social_atualizar(float dt, Uint32 agora);
void social_desenhar(Uint32 agora);
int social_quer_sair(void);
SocialEstado social_estado(void);
typedef struct {
  char imdb[16];
  char titulo[160];
} SocialItemSelecionado;
int social_item_selecionado(SocialItemSelecionado *saida);
#endif
