// Choosing the playback source.
//
// Two things live here: the RULE for which stream to play when nobody chooses,
// and the SHEET that lists the sources when the user wants to choose by hand.
//
// The rule comes from the owner, and the order matters: MP4 in 4K with Dolby
// Vision first; failing that, the first in the list. "Automatic" must never
// stall for want of the preferred one — an app that opens the source list every
// time the best format is missing hands the user work that belongs to the
// machine.
//
// The addons' real list. An empty response stays empty, with no sample
// sources.
#ifndef NV_STREAMS_H
#define NV_STREAMS_H
#include <SDL2/SDL.h>
#include <stdint.h>

// A lista cresce conforme a resposta dos addons; a UI virtualiza as linhas.

typedef struct {
  char label[192];     // Nome curto da fonte
  char provider[96];
  // 1024 and not 512. MEASURED: AIOStreams playback links are 525 to 547
  // characters long (two signed segments), and at 512 they were ALL truncated
  // silently. The server then answered with a 120s notice MP4 that PLAYS
  // PERFECTLY WELL — the app looked like it worked and showed the error card.
  // There is no error to detect on that path, only the field size.
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

// Index of the stream automatic mode picks, or -1 if the list is empty.
int  stream_automatic(void);

// How many ms ago the list arrived. Debrid services' playback links are SIGNED
// AND THEY EXPIRE: using a link from minutes ago makes the server redirect to a
// notice video ("This playback link couldn't be verified", 120s, 720p) that
// PLAYS PERFECTLY WELL — that is, failure that looks like success. Refreshing
// before playing is what avoids it.
Uint32 stream_age_ms(void);

// Walks the sources in the rule's order and returns the first whose link
// resolves to REAL content, testing up to `attempts` of them. -1 if none will
// do. BLOCKS — call from a thread of your own.
int  stream_first_good(int attempts);

// --- folha de fontes (a lista que sobe por cima do player/detalhe) ---
void stream_sheet_open(void);
int  stream_sheet_is_open(void);
void stream_sheet_event(const SDL_Event *e);
void stream_sheet_update(float dt, Uint32 now);
void stream_sheet_draw(Uint32 now);
// Returns 1 once when the user has chosen, with the index in *chosen.
int  stream_sheet_chose(int *chosen);

#endif
