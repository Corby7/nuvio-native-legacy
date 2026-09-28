// LIVE TV: the viewer's IPTV source, loaded and kept.
//
// Two ways in, the two every IPTV service offers:
//   - an M3U PLAYLIST URL, with an optional XMLTV guide URL (when the playlist
//     does not name its own with url-tvg);
//   - an XTREAM CODES login (server, username, password), from which the
//     playlist and the guide addresses are built, get.php and xmltv.php, the
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
  char epg[1024];       // either: the viewer's own guides, space-separated, read before the source's
  char server[512];     // Xtream: "http://host:port"
  char user[128];
  char pass[128];
} IptvSource;

typedef enum { IPTV_IDLE, IPTV_LOADING, IPTV_READY, IPTV_FAILED } IptvState;

// Why a playlist did not load, for a screen that names the fix.
typedef enum {
  IPTV_FAIL_NONE,
  IPTV_FAIL_UNREACHABLE,   // no answer: a typo in the host, or the TV offline
  IPTV_FAIL_NOT_FOUND,     // 404: the path is wrong
  IPTV_FAIL_REFUSED,       // 401/403 on a playlist address
  IPTV_FAIL_NOT_PLAYLIST,  // 200, but a web page (a login page) or nothing usable
  IPTV_FAIL_GUIDE,         // 200, but an XMLTV guide
  IPTV_FAIL_LOGIN,         // Xtream: the panel turned the login down
  IPTV_FAIL_EXPIRED,       // Xtream: the subscription has run out
  IPTV_FAIL_ACCOUNT,       // Xtream: the account is disabled, banned…
  IPTV_FAIL_TIMEOUT,       // nothing within the ceiling
  IPTV_FAIL_PROVIDER,      // a panel's own code, 884 and the like
  IPTV_FAIL_SERVER,        // 5xx
  IPTV_FAIL_OTHER,
} IptvFailure;

// Reads the saved source, favourites and history. Cheap; loads nothing.
void iptv_start(void);
// Called when the Live TV screen opens: starts the first load, or a refresh
// when the guide is older than a few hours. Idempotent while one is running.
void iptv_touch(void);
// Installs what the loader thread finished. Returns 1 on the frame a new list
// arrived, the screen rebuilds its filtered view then.
int  iptv_step(void);
void iptv_shutdown(void);

const IptvSource *iptv_source(void);
int  iptv_configured(void);
// Saves `s` and reloads everything from it.
void iptv_set_source(const IptvSource *s);

// TRY A SOURCE: load `s` while the current source and its channels stay, and
// make it the source only once its playlist has arrived. iptv_try_state goes
// LOADING -> READY (it is now the source; the guide follows as usual) or
// FAILED, with the current source untouched, also when nothing has arrived
// within `ceilingS` seconds. iptv_try_failure says why (an IptvFailure), with
// the HTTP status and the loader's own words.
void iptv_try_source(const IptvSource *s, int ceilingS);
int  iptv_try_state(void);
int  iptv_try_failure(int *status, char *why, size_t n);
// Gives a try up while it loads: the current source carries on as it was.
void iptv_try_cancel(void);
// Back to IDLE once the screen has shown the outcome (not while loading).
void iptv_try_forget(void);
// When the current playlist last came from the network (unix seconds), 0 when
// only the saved copy has been shown.
long long iptv_refreshed_at(void);
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
// The viewer's own name for the source, "" when it goes by its host; setting it
// ("" to go back to the host) saves it with the source in iptv.txt.
const char *iptv_source_name(void);
void iptv_set_source_name(const char *name);

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
// The same stream in the provider's other container: an Xtream address serves
// /live/user/pass/<id>.m3u8 and .ts alike, and a TV or a panel that chokes on
// one often plays the other. NULL when the address is not of that shape.
const char *iptv_play_url_alt(int ch, char *dst, unsigned size);

// CATCH-UP. Whether channel `ch`'s archive reaches back to unix time `t` (and
// `t` is in the past), and the URL that plays it from `start` to `stop`, the
// programme's bounds, or any instant and an hour past it. NULL when the channel
// has no archive or the URL does not fit.
int iptv_has_archive(int ch, long long t);
const char *iptv_archive_url(int ch, long long start, long long stop, char *dst, unsigned size);

#endif
