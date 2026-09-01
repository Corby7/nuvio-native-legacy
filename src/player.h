// Tela de REPRODUCAO, no formato do app da Apple TV.
//
// LEIA ISTO ANTES DE USAR: este modulo NAO reproduz video. Ele e a camada de
// INTERFACE do player, e nada mais. O que se ve no lugar do quadro de video e a
// arte-chave do titulo, parada, e o tempo que corre na barra vem de um relogio
// SIMULADO que so soma o dt de cada quadro. Nao ha libmedia, nao ha
// umediaserver, nao ha pipeline nenhum por tras — de proposito.
//
// Por que assim: a navegacao precisa estar redonda ANTES do video entrar. Um
// stub que fingisse tocar (que abrisse um handle, que reportasse estado de
// midia) seria pior que nada, porque esconderia a ausencia do pipeline atras de
// uma API que parece funcionar — e o primeiro bug de verdade viria disfarcado.
// Aqui a mentira e visivel e esta escrita.
//
// ONDE O VIDEO REAL ENTRA: em `player_atualizar`, no trecho marcado
// "RELOGIO SIMULADO". Hoje ele faz `posSeg += dt`; quando houver pipeline,
// `posSeg` e `duracaoSeg` passam a ser LIDOS do player de midia e o dt sai de
// cena, e `player_desenhar` troca o desenho da arte pelo quadro do decodificador
// (ou por um buffer de video atras da tela GL). Nenhuma outra parte do arquivo
// precisa mudar: todo o resto ja consome so essas duas variaveis.
#ifndef NV_PLAYER_H
#define NV_PLAYER_H
#include <SDL2/SDL.h>

// Abre a reproducao do titulo `indiceCatalogo` (indice circular, igual ao do
// catalogo). Titulo, logo, sinopse e arte saem dali.
//
// `url` e a fonte a tocar. Com URL, posicao e duracao vem do pipeline de video;
// com NULL, o relogio volta a ser simulado e a duracao sai do campo `meta`
// ("1 h 54 min") — util no Mac, onde nao ha pipeline nenhum.
void player_abrir(int indiceCatalogo, const char *url);

// 1 quando ha video de verdade por tras desta sessao. O desenho usa isto para
// nao pintar a arte-chave por cima do plano de video.
int  player_com_video(void);
int  player_pediu_faixas(void);   // CIMA no player abre audio/legendas

// 1 enquanto a fonte abre. A tela mostra a arte-chave e um indicador; sem isso
// o usuario aperta Reproduzir e encara uma tela parada sem saber se funcionou.
int  player_carregando(void);

// Liga a fonte numa sessao ja aberta. Existe porque o link so pode ser pedido
// no ultimo instante (ver stream_idade_ms), entao a tela abre antes de haver
// URL e o video entra quando chega.
void player_definir_fonte(const char *url);

int  player_aberto(void);   // 1 enquanto a tela existe, inclusive durante o fade de saida
void player_evento(const SDL_Event *e);
void player_atualizar(float dt, Uint32 agora);
void player_desenhar(Uint32 agora);
int  player_quer_sair(void);  // 1 assim que o Back foi apertado
void player_encerrar(void);

#endif
