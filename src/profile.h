// The Trakt profile and statistics screen.
//
// The module is deliberately "stupid" about the network: it receives a finished
// data snapshot, keeps a copy and draws. That lets the app fetch Trakt on a
// worker without ever blocking the TV's SDL/GLES frame.
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
  // Zero is the safe default for older producers. A monthly snapshot proves
  // neither annual coverage nor a streak that crosses the turn of the month.
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
  PROFILE_STATE_NO_ACTIVITY,
  PROFILE_STATE_PRIVATE,
  PROFILE_STATE_DISCONNECTED,
  PROFILE_STATE_UNAVAILABLE
} ProfileState;

// An opaque identifier for a selected title. The router resolves the item in the
// current catalogue before opening the detail, without inventing metadata.
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

// While loading, the screen preserves the structure with skeletons. A NULL
// snapshot, or one with no activity, produces the empty state — never invented
// numbers.
void profile_set_loading(int loading);
void profile_set_data(const ProfileData *data);
// Preserves the last snapshot when an update fails. With no snapshot, OK asks
// for another attempt. The caller does the network work and consumes the flag
// below.
void profile_set_error(const char *message);
void profile_set_state(ProfileState state, const char *message);
ProfileState profile_state(void);
int  profile_requested_update(void);

void profile_event(const SDL_Event *e);
void profile_update(float dt, Uint32 now);
void profile_draw(Uint32 now);

// Returns 1 once after OK on a highlight. `out`, if not NULL, receives a stable
// copy of the item so the app can open the detail without depending on internal
// storage.
int profile_item_selected(ProfileHighlight *output);

#endif
