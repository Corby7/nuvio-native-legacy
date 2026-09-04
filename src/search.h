// Tela de Busca, no formato do app da Apple TV: teclado na tela a esquerda,
// grade de posteres a direita filtrando enquanto se digita.
//
// A tela existe porque num aparelho de TV nao ha teclado fisico: TODA a entrada
// de texto passa pelo D-pad, e por isso a escolha da forma do teclado (grade
// contra a linha unica do tvOS) muda mais a experiencia do que qualquer detalhe
// visual. A justificativa da escolha esta no topo de busca.c.
#ifndef NV_SEARCH_H
#define NV_SEARCH_H
#include <SDL2/SDL.h>
#include "home.h"

int  search_start(void);
void search_event(const SDL_Event *e);
void search_update(float dt, Uint32 now);
void search_draw(Uint32 now);
// 1 quando o Back deve fechar a tela. O Back so chega aqui depois de esgotar o
// que ele tem para desfazer DENTRO da busca (sair dos resultados de volta para
// o teclado); fechar direto do meio dos resultados perde o texto digitado sem
// que o usuario tenha pedido isso.
int  search_wants_exit(void);
void search_shutdown(void);

// OK pressionado sobre um poster: devolve 1 UMA vez e escreve em
// `indiceCatalogo` o indice em cat_item(). Consome o pedido, como
// home_pediu_abrir.
int  search_requested_open(int *indexCatalog);

// Poster em foco com o retangulo que ele ocupa na tela NESTE quadro, no mesmo
// formato que a home entrega — e o que detail_abrir precisa para o card voar
// dali em vez de aparecer do nada. 0 se ainda nao houve um quadro desenhado com
// foco nos resultados.
int  search_item_focused(HomeItem *out);

#endif
