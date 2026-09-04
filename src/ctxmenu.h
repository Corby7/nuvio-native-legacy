// MENU DE CONTEXTO do cartaz, aberto SEGURANDO OK sobre um card da home.
//
// E o `posterHoldMenu` do app web, e as opcoes sao as dele, medidas no bundle
// 1.0.4 (getPosterHoldMenuOptions): "Ver detalhes", "Adicionar/Remover da
// biblioteca" e, so em filme e serie, "Marcar como assistido/nao assistido".
// Os rotulos sao os do pt-BR do proprio app.
//
// Existe porque as duas acoes de biblioteca so tinham caminho DENTRO da tela de
// titulo: para marcar um filme como visto era preciso abrir o detalhe, esperar
// a rede e descer ate o botao. Segurando o OK sobre o cartaz sao dois toques.
#ifndef NV_CTXMENU_H
#define NV_CTXMENU_H
#include <SDL2/SDL.h>

// `indice` e a posicao no catalogo global.
// A integracao da pressao longa fica em home.c: ele mede NV_HOLD_MS no KEYUP e
// chama ctx_abrir somente quando o limiar foi atingido. Este modulo nao mede a
// tecla nem abre no KEYDOWN; assim o toque curto continua abrindo o detalhe e
// o modal recebe apenas o foco D-pad depois de estar visivel.
void ctx_abrir(int indice);
int  ctx_aberto(void);
void ctx_evento(const SDL_Event *e);
void ctx_atualizar(float dt, Uint32 agora);
void ctx_desenhar(Uint32 agora);
// Indice do titulo cujo detalhe o dono pediu, ou -1. Consumido uma vez.
int  ctx_pediu_detalhes(void);
#endif
