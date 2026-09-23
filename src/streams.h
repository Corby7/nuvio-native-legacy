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

// The list grows as the addons answer; the UI virtualises the rows.

typedef struct {
  char label[192];     // Short name of the source
  char provider[96];   // the ADDON that answered: "AIOStreams", "Torrentio"
  // THE SERVICE THE ADDON GOT IT FROM, when the addon says so.
  //
  // An aggregator is not a source. AIOStreams answers for a dozen upstreams and
  // puts the real one in the stream's name — "Cached  Comet  ****" — so a row
  // that printed `provider` said "AIOStreams" twelve times and told the user
  // nothing they could not read off the tab above the list.
  //
  // Empty when the addon is the source, and the row falls back to `provider`.
  char service[48];
  // 1024 and not 512. MEASURED: AIOStreams playback links are 525 to 547
  // characters long (two signed segments), and at 512 they were ALL truncated
  // silently. The server then answered with a 120s notice MP4 that PLAYS
  // PERFECTLY WELL — the app looked like it worked and showed the error card.
  // There is no error to detect on that path, only the field size.
  char url[4096];
  int  height;          // 2160, 1080, 720...
  int  dolbyVision;
  int  dolbyAtmos;
  int  mp4;             // 1 = progressive MP4; 0 = HLS or something else
  long sizeMB;       // 0 when unknown
  char description[2048];
  char file[512];

  // THE ROW'S TOKENS, decided ONCE by the parser and never re-derived while
  // drawing. The old sheet read the same blob of text at 60fps to work out what
  // it was looking at, and having no single place where a source is classified
  // is how the row came to state "2160p", "4K" and "Dolby Vision" three times in
  // three sizes. Empty (or 0) means THE ADDON DID NOT SAY — draw nothing, never
  // a fallback value.
  char res[8];       // "4K", "1080p", "720p", "SD"
  char range[8];     // "DV", "HDR10+", "HDR10", "HLG", "HDR"; empty for SDR
  char source[8];    // "REMUX", "BLURAY", "WEB-DL", "WEBRIP", "HDTV", "HDRIP", "DVD", "CAM"
  char audio[16];    // "ATMOS 7.1", "DTS-HD 5.1", "EAC3 5.1"
  char codec[8];     // "HEVC", "AV1", "H.264"
  int  seeders;      // torrent swarm size
  int  cached;       // 1 = ready to stream now, which the sheet calls "Instant"
  // 1 = a torrent that has to be fetched. IT IS THE ONLY ROW WHERE THE SEED
  // COUNT MEANS ANYTHING: on a cached debrid file the number is a leftover from
  // whatever the aggregator scraped, and putting it on every row taught the eye
  // to skip the one place it decides whether the thing will play at all.
  int  p2p;
  float mbps;        // the video bitrate; see stream_sheet_runtime
  int  tier;         // 0 POOR, 1 FAIR, 2 GOOD, 3 BEST — the segmented bar

  // behaviorHints.bingeGroup: the label an addon puts on every stream it
  // considers THE SAME SOURCE from one episode to the next. What sourcepref.c
  // matches a remembered choice by first. Empty is common.
  char bingeGroup[128];
  // A torrent with NO url, only its hash (Torrentio/Comet with no debrid key in
  // the addon URL). Kept only while debrid_active(); `url` is filled at
  // verification, by debrid_resolve.
  char infoHash[48];
  int  fileIdx;      // -1 when the addon did not say
} Stream;

// THE SEGMENTED BAR'S VALUE, and the bitrate it is worked out from.
//
// Called by the parser with no runtime, and again by the sheet once one is known
// (stream_sheet_runtime). It fills `mbps` when it is still 0 and the runtime lets
// it be derived, then sets `tier`. Safe to call repeatedly: a bitrate the addon
// stated itself is never overwritten.
void stream_rank(Stream *s, int runtimeSeconds);

// Network-free parser: the caller frees *output. Returns -1 if the allocation fails.
int stream_parse(const char *json, const char *provider, Stream **output);
void stream_set_current(int index_);
int stream_current(void);
// THE RUNTIME THE SOURCES BELONG TO, in seconds; 0 when it is not known.
//
// It is what turns a file size into a BITRATE, and the bitrate is the one number
// that separates two 54 GB remuxes. Most addons never state it: Torrentio sends
// a size and a seed count and nothing else, so a sheet that only printed the
// bitrate when the addon spelled it out left it blank on the majority of rows.
//
// The player passes video_duration(); the title screen passes TMDB's runtime,
// which it only has for films. Unknown stays unknown — a row with no runtime and
// no stated bitrate simply does not show one.
void stream_sheet_runtime(int seconds);
int stream_sheet_reload(void);

// Replaces the current title's list. Call when the addons answer.
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
// resolves to REAL content, testing up to `attempts` of them (plus the preferred
// one, which goes first). -1 if none will do. A torrent row is resolved through
// the debrid here, and `season`/`episode` (0,0 for a film) pick its file.
// BLOCKS — call from a thread of your own.
int  stream_first_good(int attempts, int season, int episode);
// The same check for ONE row: the one the person picked in the sheet. Returns
// `index` when it resolves, -1 when not. BLOCKS.
int  stream_verify_one(int index, int season, int episode);

// THE SOURCE TO TRY FIRST: the one remembered for this title (sourcepref.c).
// An index in the CURRENT list, or -1 for none. A new list resets it — an index
// into the previous list would point at some other source of the next episode.
void stream_prefer(int index);

// --- source sheet (the list that rises over the player/detail screen) ---
void stream_sheet_open(void);
int  stream_sheet_is_open(void);
void stream_sheet_event(const SDL_Event *e);
void stream_sheet_update(float dt, Uint32 now);
void stream_sheet_draw(Uint32 now);
// Returns 1 once when the user has chosen, with the index in *chosen.
int  stream_sheet_chose(int *chosen);

#endif
