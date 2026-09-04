// Tela de perfil e estatisticas do Trakt.
//
// O modulo e deliberadamente "burro" sobre rede: ele recebe um snapshot de
// dados pronto, guarda uma copia e desenha. Isso permite ao app buscar Trakt em
// uma worker sem jamais bloquear o quadro de SDL/GLES da TV.
#ifndef NV_PROFILE_H
#define NV_PROFILE_H

#include <SDL2/SDL.h>
#include <stdint.h>

#define PROFILE_MAX_GENRES    8
#define PROFILE_MAX_HIGHLIGHTS  6
#define PROFILE_MAX_DAYS       42

typedef struct {
  char name[40];
  int  count;
  uint32_t color;              // 0xRRGGBB; zero usa a paleta da tela
} ProfileGenre;

typedef struct {
  char id[64];               // imdb/trakt/slug, devolvido sem interpretacao
  char title[128];
  char detail[96];          // ex.: "T2E3 · Solo" ou "Filme"
  char poster[768];
  char backdrop[768];
  int  plays;
  int  minutes;
} ProfileHighlight;

typedef struct {
  char name[96];
  char user[64];
  char avatar[768];
  char period[48];          // ex.: "SETEMBRO 2026"

  int minutes;
  int plays;
  int movies;
  int episodes;

  int streakCurrent;
  int streakPrevious;
  int daysActiveMonth;
  int daysActiveYear;
  int firstDayWeek;     // 0=domingo..6=sabado
  int nDays;
  unsigned short activity[PROFILE_MAX_DAYS];

  int nGenres;
  ProfileGenre genres[PROFILE_MAX_GENRES];
  int nHighlights;
  ProfileHighlight highlights[PROFILE_MAX_HIGHLIGHTS];
  // Zero e o padrao seguro para produtores antigos. O snapshot mensal nao
  // comprova cobertura anual nem uma sequencia que cruza a virada do mes.
  int partial;
  int yearComplete;
  int streakComplete;
  char warning[160];
} ProfileData;

typedef enum {
  PROFILE_STATE_LOADING = 0,
  PROFILE_STATE_UPDATING,
  PROFILE_STATE_READY,
  PROFILE_STATE_STALE,
  PROFILE_STATE_ERROR,
  PROFILE_STATE_SEM_ACTIVITY,
  PROFILE_STATE_PRIVATE,
  PROFILE_STATE_DISCONNECTED,
  PROFILE_STATE_UNAVAILABLE
} ProfileState;

// Identificador opaco de uma obra selecionada. O roteador resolve o item no
// catalogo atual antes de abrir o detalhe, sem inventar metadados.
typedef struct {
  char id[64];
  char title[128];
} ProfileItemSelected;

int  profile_start(void);
void profile_shutdown(void);

void profile_open(void);
void profile_open_side(void);
int profile_side(void);
int profile_requested_complete(void);
void profile_close(void);
int  profile_is_open(void);
int  profile_wants_exit(void);       // consome o pedido de voltar

// Enquanto carrega, a tela preserva a estrutura com esqueletos. Um snapshot
// NULL/sem atividade produz o estado vazio, nunca numeros inventados.
void profile_set_loading(int loading);
void profile_set_data(const ProfileData *data);
// Preserva o ultimo snapshot ao falhar uma atualizacao. Sem snapshot, OK
// pede nova tentativa. O chamador faz a rede e consome a flag abaixo.
void profile_set_error(const char *message);
void profile_set_state(ProfileState state, const char *message);
ProfileState profile_state(void);
int  profile_requested_update(void);

void profile_event(const SDL_Event *e);
void profile_update(float dt, Uint32 now);
void profile_draw(Uint32 now);

// Retorna 1 uma vez apos OK num destaque. `saida`, se nao NULL, recebe copia
// estavel do item para o app abrir detalhes sem depender do storage interno.
int profile_item_selected(ProfileHighlight *output);

#endif
