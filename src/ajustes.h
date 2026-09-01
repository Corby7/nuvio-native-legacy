// Tela de Ajustes: lista vertical de opcoes em secoes, rotulo a esquerda e
// valor a direita.
//
// Os valores ficam em MEMORIA, sem persistencia. E deliberado: gravar em disco
// e uma decisao de plataforma (onde grava no webOS, o que acontece na primeira
// execucao) que nao pertence a tela — e uma tela que le seus proprios valores
// de getters continua funcionando igual no dia em que a persistencia entrar.
#ifndef NV_AJUSTES_H
#define NV_AJUSTES_H
#include <SDL2/SDL.h>

int  ajustes_iniciar(void);

// Pasta onde os ajustes sao lidos e gravados. Chamar uma vez, no inicio.
void ajustes_dir(const char *dir);
void ajustes_evento(const SDL_Event *e);
void ajustes_atualizar(float dt, Uint32 agora);
void ajustes_desenhar(Uint32 agora);
int  ajustes_quer_sair(void);   // 1 quando o Back deve fechar a tela
void ajustes_encerrar(void);

// Leitura pelo resto do app. "Animacoes reduzidas" e a que mais importa: com
// ela ligada, quem anima deve ir direto ao alvo em vez de chamar anim_mola —
// e um ajuste de acessibilidade, nao um gosto, e uma tela que o ignora nao
// serve para quem o ligou.
int ajustes_animacoes_reduzidas(void);
int ajustes_dolby_vision(void);
int ajustes_dolby_atmos(void);
int ajustes_idioma_ingles(void);
// "Automática", "4K", "1080p" ou "720p" — o rotulo exibido, para quem seleciona
// a fonte de video mostrar exatamente o que o usuario escolheu.
const char *ajustes_qualidade(void);

#endif
