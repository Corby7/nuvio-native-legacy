// LIVE TV: the viewer's IPTV source, loaded and kept.
//
// Two ways in, the two every IPTV service offers:
//   - an M3U PLAYLIST URL, with an optional XMLTV guide URL (when the playlist
//     does not name its own with url-tvg);
//   - an XTREAM CODES login (server, username, password), from which the
//     playlist and the guide addresses are built — get.php and xmltv.php, the
//     two endpoints every Xtream panel serves.
//
// THE LOAD IS ON A THREAD OF ITS OWN and in two stages, because the two halves
// arrive at very different speeds: a playlist is a few hundred kilobytes and
// lands in a second, a guide is tens of megabytes (often gzipped) and can take
// half a minute. The channels are installed the moment they parse, so the
// screen is usable while the guide is still coming; the guide replaces that list
// with the same channels plus their programmes.
//
// The playlist is cached in the data folder, so opening Live TV shows the
// channels at once and refreshes behind them. The guide is not cached: stale
// programme data is worse than a "loading guide" line.
//
// THREADING CONTRACT: everything here is called from the main thread. The list
// returned by iptv_list() is valid until the next iptv_step(); the screen keeps
// channel INDICES across frames, never pointers, and indices survive the swap
// from channels-only to channels-with-guide because both come from the same
// playlist text.
#ifndef NV_IPTV_H
#define NV_IPTV_H
#include "iptv_parse.h"

typedef enum { IPTV_SRC_NONE, IPTV_SRC_M3U, IPTV_SRC_XTREAM } IptvSourceKind;

typedef struct {
  int  kind;            // IptvSourceKind
  char url[1024];       // M3U: the playlist
  char epg[1024];       // either: the guide, overriding what the source names
  char server[512];     // Xtream: "http://host:port"
  char user[128];
  char pass[128];
} IptvSource;

typedef enum { IPTV_IDLE, IPTV_LOADING, IPTV_READY, IPTV_FAILED } IptvState;

// Reads the saved source, favourites and history. Cheap; loads nothing.
void iptv_start(void);
// Called when the Live TV screen opens: starts the first load, or a refresh
// when the guide is older than a few hours. Idempotent while one is running.
void iptv_touch(void);
// Installs what the loader thread finished. Returns 1 on the frame a new list
// arrived — the screen rebuilds its filtered view then.
int  iptv_step(void);
void iptv_shutdown(void);

const IptvSource *iptv_source(void);
int  iptv_configured(void);
// Saves `s` and reloads everything from it.
void iptv_set_source(const IptvSource *s);
// Throws the list away and loads it again (the "Reload" button).
void iptv_reload(void);

const IptvList *iptv_list(void);   // NULL until the first playlist parsed
int  iptv_state(void);             // the playlist's IptvState
int  iptv_guide_state(void);       // the guide's
// One line for the header: "Loading channels…", "Guide unavailable", …
const char *iptv_status(void);
// A short name for the source, for the header: the playlist's host, or the
// Xtream server's.
const char *iptv_source_label(void);

// Favourites and history are keyed by the channel's NAME: a provider rotates
// stream URLs (they carry the credentials) far more often than it renames.
int  iptv_is_favourite(int ch);
void iptv_toggle_favourite(int ch);
// Most recent first. `i` from 0; -1 past the end or when the channel is gone.
int  iptv_recent(int i);
void iptv_note_watched(int ch);

// The URL to hand the video pipeline for channel `ch`: the stream itself, or a
// loopback relay when the playlist asked for request headers. `dst` holds the
// relay address when one is used.
const char *iptv_play_url(int ch, char *dst, unsigned size);

// PLAYBACK PREFERENCES, saved with the source and applied at once (no reload):
// the pause buffer's length in minutes (0 = off; see timeshift.h), and whether
// resting on a channel while browsing previews it.
int  iptv_pref_buffer(void);
int  iptv_pref_preview(void);
void iptv_set_prefs(int bufferMinutes, int preview);

// CATCH-UP. Whether channel `ch`'s archive reaches back to unix time `t` (and
// `t` is in the past), and the URL that plays it from `start` to `stop` — the
// programme's bounds, or any instant and an hour past it. NULL when the channel
// has no archive or the URL does not fit.
int iptv_has_archive(int ch, long long t);
const char *iptv_archive_url(int ch, long long start, long long stop, char *dst, unsigned size);

#endif
