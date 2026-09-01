#ifndef NV_HOME_H
#define NV_HOME_H
#include <SDL2/SDL.h>
#include "gfx.h"

// Tipos de fileira presentes na home moderna do Nuvio 1.0.1 legacy.
typedef enum {
  FILEIRA_CONTINUE,   // card 16:9 com barra de progresso
  FILEIRA_NORMAL,      // poster retrato 2:3
  FILEIRA_DESTAQUE,    // reservado para colecoes landscape/continue
  FILEIRA_TOP10        // ranking usa poster retrato no legacy
} TipoFileira;

// O item sob o foco, com o retangulo que ele ocupa na tela NESTE quadro. A
// transicao para o detalhe parte dai: o card nao "abre uma tela nova", ele voa
// ate virar o hero — e sem o rect de origem real a animacao teria que chutar
// de onde veio, que e exatamente o que faz uma transicao parecer barata.
typedef struct {
  // Indice NO CATALOGO do card focado. Faltava, e por isso o detalhe abria
  // sempre o item 0: a tela de destino nao tinha como saber o que fora aberto.
  int indice;
  GfxRect rect;
  const char *arte;
  const char *titulo;
  const char *genero;
  const char *meta;
} HomeItem;

int  home_iniciar(const char *dirArte);
int  home_item_focado(HomeItem *out);      // 0 se o foco ainda nao foi desenhado
int  home_n_artes(void);                   // acervo de backdrops, usado pelo detalhe
const char *home_arte(int i);
const char *home_backdrop(int i);   // arte do titulo i do catalogo
void home_evento(const SDL_Event *e);
void home_atualizar(float dt, Uint32 agora);
void home_desenhar(Uint32 agora);
void home_encerrar(void);
int  home_quer_sair(void);
int  home_pediu_abrir(void);   // OK pressionado: consome o pedido
int  home_pediu_menu(void);    // ESQUERDA na primeira coluna: chama o menu

#endif
