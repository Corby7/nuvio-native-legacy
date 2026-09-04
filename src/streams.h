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
// Lista real dos addons. Resposta vazia permanece vazia, sem fontes de exemplo.
#ifndef NV_STREAMS_H
#define NV_STREAMS_H
#include <SDL2/SDL.h>
#include <stdint.h>

// A lista cresce conforme a resposta dos addons; a UI virtualiza as linhas.

typedef struct {
  char label[192];     // Nome curto da fonte
  char provider[96];
  // 1024 e nao 512. MEDIDO: os links de reproducao do AIOStreams tem 525 a 547
  // caracteres (dois segmentos assinados), e com 512 TODOS eram cortados em
  // silencio. O servidor entao respondia com um MP4 de aviso de 120s que TOCA
  // NORMALMENTE — o app parecia funcionar e mostrava o cartao de erro. Nao ha
  // erro para detectar nesse caminho, so o tamanho do campo.
  char url[4096];
  int  height;          // 2160, 1080, 720...
  int  dolbyVision;
  int  dolbyAtmos;
  uint64_t badges;     // classificados uma vez, nunca regex no desenho
  int  mp4;             // 1 = MP4 progressivo; 0 = HLS ou outro
  long sizeMB;       // 0 quando desconhecido
  char description[2048];
  char file[512];
} Stream;

// Parser sem rede: o chamador libera *saida. Retorna -1 se a alocacao falhar.
int stream_parse(const char *json, const char *provider, Stream **output);
void stream_set_current(int index_);
int stream_current(void);
void stream_sheet_context(const char *text);
int stream_sheet_reload(void);

// Substitui a lista do titulo corrente. Chamar quando os addons responderem.
void stream_set_list(const Stream *list, int n);
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
int  stream_first_boa(int attempts);

// --- folha de fontes (a lista que sobe por cima do player/detalhe) ---
void stream_sheet_open(void);
int  stream_sheet_is_open(void);
void stream_sheet_event(const SDL_Event *e);
void stream_sheet_update(float dt, Uint32 now);
void stream_sheet_draw(Uint32 now);
// Devolve 1 uma vez quando o usuario escolheu, com o indice em *escolhido.
int  stream_sheet_chose(int *chosen);

#endif
