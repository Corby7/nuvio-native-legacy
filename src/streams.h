// Escolha da fonte de reproducao.
//
// Duas coisas moram aqui: a REGRA de qual stream tocar quando ninguem escolhe,
// e a FOLHA que lista as fontes quando o usuario quer escolher na mao.
//
// A regra vem do dono, e a ordem importa: MP4 em 4K com Dolby Vision primeiro;
// nao havendo, o primeiro da lista. "Automatico" nunca deve travar por falta do
// preferido — um app que abre a lista de fontes toda vez que o melhor formato
// falta transfere ao usuario um trabalho que e da maquina.
//
// LIMITE DE HOJE, escrito para nao enganar: nao ha addon ligado nem pipeline de
// video no app nativo. As fontes abaixo sao um CONJUNTO DE EXEMPLO, marcado
// como tal em streams.c, que existe para exercitar a ordenacao e a folha. Quem
// ligar os addons de verdade so precisa preencher `stream_definir_lista`.
#ifndef NV_STREAMS_H
#define NV_STREAMS_H
#include <SDL2/SDL.h>

#define STREAM_MAX 12

typedef struct {
  char rotulo[96];      // "Nuvio · 4K HDR"
  char provedor[40];
  // 1024 e nao 512. MEDIDO: os links de reproducao do AIOStreams tem 525 a 547
  // caracteres (dois segmentos assinados), e com 512 TODOS eram cortados em
  // silencio. O servidor entao respondia com um MP4 de aviso de 120s que TOCA
  // NORMALMENTE — o app parecia funcionar e mostrava o cartao de erro. Nao ha
  // erro para detectar nesse caminho, so o tamanho do campo.
  char url[1024];
  int  altura;          // 2160, 1080, 720...
  int  dolbyVision;
  int  dolbyAtmos;
  int  mp4;             // 1 = MP4 progressivo; 0 = HLS ou outro
  long tamanhoMB;       // 0 quando desconhecido
} Stream;

// Substitui a lista do titulo corrente. Chamar quando os addons responderem.
void stream_definir_lista(const Stream *lista, int n);
int  stream_n(void);
const Stream *stream_item(int i);

// Indice do stream que o modo automatico escolhe, ou -1 se a lista esta vazia.
int  stream_automatico(void);

// Ha quantos ms a lista chegou. Os links de reproducao dos servicos de debrid
// sao ASSINADOS E EXPIRAM: usar um link de minutos atras faz o servidor
// redirecionar para um video de aviso ("This playback link couldn't be
// verified", 120s, 720p) que TOCA NORMALMENTE — ou seja, falha parecendo
// sucesso. Renovar antes de reproduzir e o que evita isso.
Uint32 stream_idade_ms(void);

// Percorre as fontes na ordem da regra e devolve a primeira cujo link resolve
// para conteudo DE VERDADE, testando ate `tentativas`. -1 se nenhuma serve.
// BLOQUEIA — chamar de fio proprio.
int  stream_primeira_boa(int tentativas);

// --- folha de fontes (a lista que sobe por cima do player/detalhe) ---
void stream_folha_abrir(void);
int  stream_folha_aberta(void);
void stream_folha_evento(const SDL_Event *e);
void stream_folha_atualizar(float dt, Uint32 agora);
void stream_folha_desenhar(Uint32 agora);
// Devolve 1 uma vez quando o usuario escolheu, com o indice em *escolhido.
int  stream_folha_escolheu(int *escolhido);

#endif
