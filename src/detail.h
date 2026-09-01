// Tela de detalhe do titulo.
//
// A transicao e o ponto: no Apple TV o card NAO some para dar lugar a uma tela
// nova — ele cresce ate virar o hero do detalhe, e o resto entra depois. Por
// isso detail_abrir recebe o retangulo de origem real do card na tela, e nao
// apenas o titulo: e esse rect que da continuidade ao movimento.
#ifndef NV_DETAIL_H
#define NV_DETAIL_H
#include <SDL2/SDL.h>
#include "home.h"

void detail_abrir(const HomeItem *item);
int  detail_aberto(void);        // 1 enquanto a tela existe, inclusive saindo
// 1 quando o cartao ja cobre a tela inteira e desenhar a home por baixo e
// trabalho jogado fora. Medido: a home custa o hero em tela cheia mais ~20
// cards, e sem este corte a pagina de detalhe rodava a 20fps.
int  detail_cobre_tela(void);
// 1 quando o cartao ja parou no lugar e nao esta esticado: nesse estado a home
// atras so aparece pela moldura.
int  detail_assentado(void);

// Qual titulo do catalogo esta em cena, e os pedidos que a tela nao resolve
// sozinha: reproduzir e marcar na lista. O detalhe nao chama o player nem a
// biblioteca direto — quem conhece as outras telas e o roteador.
int  detail_indice(void);
int  detail_pediu_reproduzir(void);   // consome o pedido
int  detail_pediu_marcar(void);       // botao "+"
int  detail_pediu_fontes(void);       // OK segurado, ou o botao "..."
void detail_evento(const SDL_Event *e);
void detail_atualizar(float dt, Uint32 agora);
void detail_desenhar(Uint32 agora);   // desenhe DEPOIS da home: ele cobre

#endif
