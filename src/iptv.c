#define _GNU_SOURCE   // strcasestr
// Live TV's source, loader and small persistent state. See iptv.h.
#include "iptv.h"
#include "data.h"
#include "js.h"
#include "net.h"
#include "proxy.h"
#include <SDL2/SDL.h>
#include <ctype.h>
#include <pthread.h>
#include <stdatomic.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define IPTV_CONFIG_FILE  "iptv.txt"
#define IPTV_CACHE_FILE   "iptv_playlist.m3u"
#define IPTV_FAV_FILE     "iptv_favourites.txt"
#define IPTV_RECENT_FILE  "iptv_recent.txt"

// A playlist is small; a guide is not, and some panels build it on request.
#define IPTV_PLAYLIST_TIMEOUT_S  60
#define IPTV_GUIDE_TIMEOUT_S    180
// What of the guide is kept: the last three hours (what just aired is still
// worth reading about) to a day and a bit ahead.
#define IPTV_GUIDE_BEHIND_S   (3 * 3600)
#define IPTV_GUIDE_AHEAD_S    (30 * 3600)
#define IPTV_GUIDE_ARCHIVE_S  (24 * 3600)   // how far back, when there is catch-up
// A guide older than this is fetched again when the screen is opened.
#define IPTV_GUIDE_STALE_MS   (4u * 3600u * 1000u)
#define IPTV_MAX_FAVOURITES   500
#define IPTV_MAX_RECENT        30

static IptvSource source;
static char sourceLabel[96];
// THE VIEWER'S NAME FOR THE SOURCE ("name=" in iptv.txt): the label everywhere
// the host would be. Empty for the host. It belongs to the address: a source
// on another address starts without one.
static char sourceName[96];

// The list the screen reads (main thread only) and the one the loader has just
// finished (handed over under `mu`).
static IptvList *live;
static IptvList *incoming;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static char statusLine[160];
static _Atomic int state = IPTV_IDLE, guideState = IPTV_IDLE;
static _Atomic int busy;
// Bumped by every new load: a loader that finds it changed throws its work away.
static _Atomic unsigned generation;
static int restartPending;
static Uint32 loadedAt;

static char *favourites[IPTV_MAX_FAVOURITES];
static int nFavourites;
static char *recents[IPTV_MAX_RECENT];
static int nRecents;

// --- Small helpers --------------------------------------------------------------
static void setStatus(const char *s) {
  pthread_mutex_lock(&mu);
  snprintf(statusLine, sizeof statusLine, "%s", s ? s : "");
  pthread_mutex_unlock(&mu);
}

static void hostOf(const char *url, char *dst, size_t n) {
  const char *p = strstr(url, "://");
  size_t k = 0;
  p = p ? p + 3 : url;
  // user:pass@ is never shown.
  { const char *at = strchr(p, '@'), *slash = strchr(p, '/');
    if (at && (!slash || at < slash)) p = at + 1; }
  while (*p && *p != '/' && *p != ':' && *p != '?' && k + 1 < n) dst[k++] = *p++;
  dst[k] = 0;
}

static void urlEncode(const char *s, char *dst, size_t n) {
  static const char HEX[] = "0123456789ABCDEF";
  size_t k = 0;
  for (; *s && k + 4 < n; s++) {
    unsigned char c = (unsigned char)*s;
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') dst[k++] = (char)c;
    else { dst[k++] = '%'; dst[k++] = HEX[c >> 4]; dst[k++] = HEX[c & 15]; }
  }
  dst[k] = 0;
}

// "host:8080", "http://host:8080/", "host:8080/c/" -> "http://host:8080"
static void serverBase(const char *in, char *dst, size_t n) {
  size_t k;
  while (isspace((unsigned char)*in)) in++;
  if (strstr(in, "://")) snprintf(dst, n, "%s", in);
  else snprintf(dst, n, "http://%s", in);
  k = strlen(dst);
  while (k && (dst[k - 1] == '/' || isspace((unsigned char)dst[k - 1]))) dst[--k] = 0;
  // A portal address pasted from a browser ends in /c or /player_api.php.
  { char *c = strstr(dst + 8, "/c");
    if (c && (!c[2] || c[2] == '/')) *c = 0;
    if ((c = strstr(dst, "/player_api.php"))) *c = 0; }
}

static void playlistUrl(const IptvSource *s, char *dst, size_t n) {
  if (s->kind == IPTV_SRC_XTREAM) {
    char base[600], u[400], p[400];
    serverBase(s->server, base, sizeof base);
    urlEncode(s->user, u, sizeof u);
    urlEncode(s->pass, p, sizeof p);
    // HLS rather than raw TS: the TV's pipeline recovers from a dropped
    // segment on HLS, where a TS connection that stalls is simply a frozen
    // picture. m3u_plus is what carries tvg-id, tvg-logo and group-title.
    snprintf(dst, n, "%s/get.php?username=%s&password=%s&type=m3u_plus&output=m3u8",
             base, u, p);
  } else {
    snprintf(dst, n, "%s", s->url);
  }
}

// THE GUIDES TO READ, in order of priority. The viewer's own come first, typed
// because the provider's is wrong or thin, so theirs wins where both know a
// channel, then the provider's: its xmltv.php, or every address in the
// playlist's url-tvg. Each later guide only fills the channels the earlier ones
// left empty (iptv_parse_xmltv_more). Separators are anything that is not part
// of an address: spaces, new lines, commas.
#define IPTV_GUIDES_MAX 8
typedef struct { char url[IPTV_GUIDES_MAX][1400]; int n; } GuideUrls;

static void guideAdd(GuideUrls *g, const char *s, size_t k) {
  if (g->n >= IPTV_GUIDES_MAX || !k || k >= sizeof g->url[0]) return;
  for (int i = 0; i < g->n; i++)
    if (strlen(g->url[i]) == k && !strncmp(g->url[i], s, k)) return;
  memcpy(g->url[g->n], s, k); g->url[g->n][k] = 0;
  g->n++;
}

static void guideSplit(GuideUrls *g, const char *list) {
  const char *p = list;
  while (*p) {
    const char *e;
    while (*p && (isspace((unsigned char)*p) || *p == ',' || *p == ';')) p++;
    for (e = p; *e && !isspace((unsigned char)*e) && *e != ',' && *e != ';'; e++) {}
    guideAdd(g, p, (size_t)(e - p));
    p = e;
  }
}

static void guideUrls(const IptvSource *s, const IptvList *l, GuideUrls *g) {
  g->n = 0;
  guideSplit(g, s->epg);
  if (s->kind == IPTV_SRC_XTREAM) {
    char base[600], u[400], p[400], x[1400];
    serverBase(s->server, base, sizeof base);
    urlEncode(s->user, u, sizeof u);
    urlEncode(s->pass, p, sizeof p);
    snprintf(x, sizeof x, "%s/xmltv.php?username=%s&password=%s", base, u, p);
    guideAdd(g, x, strlen(x));
  } else if (l) {
    guideSplit(g, l->epgUrl);
  }
}

// A query parameter of `url`, percent-decoded. 0 when absent or empty.
static int queryParam(const char *url, const char *key, char *dst, size_t n) {
  const char *q = strchr(url, '?');
  size_t kl = strlen(key), k = 0;
  if (!q || !n) return 0;
  for (q++; *q; ) {
    const char *end = q + strcspn(q, "&#");
    if ((size_t)(end - q) > kl && !strncmp(q, key, kl) && q[kl] == '=') {
      for (q += kl + 1; q < end && k + 1 < n; q++) {
        unsigned v;
        if (*q == '%' && q + 2 < end && sscanf(q + 1, "%2x", &v) == 1) { dst[k++] = (char)v; q += 2; }
        else dst[k++] = *q == '+' ? ' ' : *q;
      }
      dst[k] = 0;
      return k > 0;
    }
    if (*end != '&') break;
    q = end + 1;
  }
  return 0;
}

// AN XTREAM LOGIN PASTED AS A PLAYLIST. Providers hand out
// "http://host/get.php?username=…&password=…&type=m3u_plus&output=ts" as "your
// M3U link", and people paste it as one. It is an Xtream login, and loading it
// as one reaches the API that panels which refuse get.php still answer.
static int xtreamFromUrl(const IptvSource *in, IptvSource *out, char *prefer, size_t np) {
  const char *g;
  IptvSource s;
  char o[16];
  if (in->kind != IPTV_SRC_M3U || !(g = strstr(in->url, "/get.php?"))) return 0;
  memset(&s, 0, sizeof s);
  s.kind = IPTV_SRC_XTREAM;
  if (!queryParam(in->url, "username", s.user, sizeof s.user) ||
      !queryParam(in->url, "password", s.pass, sizeof s.pass)) return 0;
  if ((size_t)(g - in->url) >= sizeof s.server) return 0;
  memcpy(s.server, in->url, (size_t)(g - in->url));
  s.server[g - in->url] = 0;
  snprintf(s.epg, sizeof s.epg, "%s", in->epg);
  // The container the link asked for ("output=ts"): the provider's own choice
  // for this account, and the one to try first.
  if (queryParam(in->url, "output", o, sizeof o) && (!strcmp(o, "ts") || !strcmp(o, "m3u8")))
    snprintf(prefer, np, "%s", o);
  *out = s;
  return 1;
}

// THE XTREAM API: the account, then the live categories and streams, written
// out as the playlist get.php would have served (iptv_xtream_m3u). NULL when
// the API is not there or refused; `why` then says so when the answer was
// specific (a refused login, an expired account), and stays empty otherwise so
// get.php can be tried. `apiStatus` gets the account call's HTTP status when
// that call failed, and 0 when it answered; `*kind` the IptvFailure of a
// specific refusal.
static char *xtreamApi(const IptvSource *src, const char *prefer, char *why, size_t n,
                       int *apiStatus, int *kind) {
  char base[600], u[400], p[400], api[1400], url[1500], state[40] = "", ext[8] = "m3u8";
  char *acct, *cats, *streams, *m3u;
  int st = 0;
  why[0] = 0;
  *apiStatus = 0;
  serverBase(src->server, base, sizeof base);
  urlEncode(src->user, u, sizeof u);
  urlEncode(src->pass, p, sizeof p);
  snprintf(api, sizeof api, "%s/player_api.php?username=%s&password=%s", base, u, p);
  acct = net_download_st(api, IPTV_PLAYLIST_TIMEOUT_S, NULL, &st);
  // Panels throw the odd one-off server error (a 513 seen in the wild, gone on
  // the next request): one more try before giving up on the API.
  if (!acct && st >= 500) {
    printf("[iptv] xtream api: HTTP %d, trying once more\n", st);
    SDL_Delay(1500);
    acct = net_download_st(api, IPTV_PLAYLIST_TIMEOUT_S, NULL, &st);
  }
  if (!acct || st >= 400 || !strstr(acct, "user_info")) {
    printf("[iptv] xtream api: HTTP %d, %s\n", st, acct ? "not an account answer" : "no answer");
    *apiStatus = st;
    free(acct);
    return NULL;
  }
  { int auth = (int)js_num(acct, NULL, "auth", -1);
    const char *f = strstr(acct, "\"allowed_output_formats\"");
    const char *fe = f ? strchr(f, ']') : NULL;
    js_text(acct, NULL, "status", state, sizeof state);
    // HLS when the account allows it, for the reason playlistUrl gives; raw TS
    // when that is all it may use.
    { char list[128] = "", want[12];
      if (f && fe) {
        size_t k = (size_t)(fe - f) < sizeof list - 1 ? (size_t)(fe - f) : sizeof list - 1;
        memcpy(list, f, k); list[k] = 0;
        if (!strstr(list, "\"m3u8\"") && strstr(list, "\"ts\"")) snprintf(ext, sizeof ext, "ts");
      }
      // The pasted link's own choice wins when the account allows it.
      snprintf(want, sizeof want, "\"%s\"", prefer);
      if (prefer[0] && (!list[0] || strstr(list, want))) snprintf(ext, sizeof ext, "%s", prefer);
    }
    printf("[iptv] xtream api: auth %d, status '%s', streams as .%s\n", auth, state, ext);
    free(acct);
    if (auth == 0) {
      *kind = IPTV_FAIL_LOGIN;
      snprintf(why, n, "The server refused this login. Check the username and password");
      return NULL;
    }
    if (!strcasecmp(state, "Expired")) { *kind = IPTV_FAIL_EXPIRED; snprintf(why, n, "This IPTV subscription has expired"); return NULL; }
    if (state[0] && strcasecmp(state, "Active")) {
      *kind = IPTV_FAIL_ACCOUNT;
      snprintf(why, n, "The provider says this account is %s", state);
      return NULL;
    } }
  snprintf(url, sizeof url, "%s&action=get_live_categories", api);
  cats = net_download_st(url, IPTV_PLAYLIST_TIMEOUT_S, NULL, &st);
  if (cats && st >= 400) { free(cats); cats = NULL; }
  snprintf(url, sizeof url, "%s&action=get_live_streams", api);
  streams = net_download_st(url, IPTV_PLAYLIST_TIMEOUT_S, NULL, &st);
  if (streams && st >= 400) { free(streams); streams = NULL; }
  // The stream addresses carry the login as the panel knows it, unencoded, the
  // way get.php writes them.
  m3u = streams ? iptv_xtream_m3u(streams, cats, base, src->user, src->pass, ext) : NULL;
  printf("[iptv] xtream api: %s\n", m3u ? "streams read" : streams ? "streams unreadable" : "no streams");
  free(cats); free(streams);
  return m3u;
}

static void updateLabel(void) {
  char host[80] = "";
  if (source.kind == IPTV_SRC_XTREAM) hostOf(source.server, host, sizeof host);
  else if (source.kind == IPTV_SRC_M3U) hostOf(source.url, host, sizeof host);
  snprintf(sourceLabel, sizeof sourceLabel, "%s", sourceName[0] ? sourceName : host[0] ? host : "IPTV");
}

// The address a source loads from: a new one is a different provider.
static const char *addressOf(const IptvSource *s) {
  return s->kind == IPTV_SRC_XTREAM ? s->server : s->url;
}
static void keepNameFor(const IptvSource *next) {
  if (next->kind != source.kind || strcmp(addressOf(next), addressOf(&source))) sourceName[0] = 0;
}

// --- The loader ----------------------------------------------------------------------
typedef struct { IptvSource src; unsigned gen; int hasList, trial; } Job;

// A TRIED SOURCE (iptv_try_source): loaded as a job of its own while the
// current source and its list stay as they are. It replaces them only once its
// playlist has come in, the loader raises trialCommit, the main thread swaps
// the source in iptv_step, and a failure leaves the current one untouched,
// with the reason kept for the screen that asked.
static IptvSource trialSrc;
static int trialWanted, trialBusyBefore;
static atomic_int trialState, trialCommit, trialKind, trialStatus;
static Uint32 trialDeadline;
static char trialWhy[160];
static _Atomic long long refreshedAt;

// Hands `l` to the main thread, unless a newer load has started meanwhile.
static void post(IptvList *l, unsigned gen) {
  pthread_mutex_lock(&mu);
  if (gen == atomic_load(&generation)) {
    if (incoming) { iptv_list_free(incoming); free(incoming); }
    incoming = l;
    l = NULL;
  }
  pthread_mutex_unlock(&mu);
  if (l) { iptv_list_free(l); free(l); }
}

// --- Xtream's archive ---------------------------------------------------------------
// Xtream's m3u_plus says nothing about catch-up; player_api.php does. Two calls:
// the account's server_info, whose local clock is what timeshift URLs are
// written in, and get_live_streams, which marks each stream's tv_archive and how
// many days of it. Both are best-effort: a panel without them simply has no
// catch-up.
typedef struct { long id; int days; } Archive;
typedef struct { Archive *a; int n; int offset; int known; } Archives;

// The number after "key": in [p, e), quoted or not. -1 when absent.
static long jsonNumber(const char *p, const char *e, const char *key) {
  size_t k = strlen(key);
  for (const char *q = p; q && q + k < e; q++) {
    q = memchr(q, '"', (size_t)(e - q));
    if (!q || q + k + 2 >= e) return -1;
    if (!strncmp(q + 1, key, k) && q[k + 1] == '"') {
      const char *v = q + k + 2;
      while (v < e && (*v == ':' || isspace((unsigned char)*v) || *v == '"')) v++;
      return (v < e && isdigit((unsigned char)*v)) ? strtol(v, NULL, 10) : -1;
    }
  }
  return -1;
}

static int archiveCmp(const void *a, const void *b) {
  long x = ((const Archive *)a)->id, y = ((const Archive *)b)->id;
  return x < y ? -1 : x > y;
}

static void fetchArchives(const IptvSource *src, Archives *out) {
  char base[600], u[400], p[400], url[1600];
  char *text;
  memset(out, 0, sizeof *out);
  if (src->kind != IPTV_SRC_XTREAM) return;
  serverBase(src->server, base, sizeof base);
  urlEncode(src->user, u, sizeof u);
  urlEncode(src->pass, p, sizeof p);
  snprintf(url, sizeof url, "%s/player_api.php?username=%s&password=%s", base, u, p);
  if ((text = net_download(url, 20))) {
    const char *t = strstr(text, "\"time_now\"");
    long stamp = jsonNumber(text, text + strlen(text), "timestamp_now");
    if (t && stamp > 0) {
      int y, mo, d, h, mi, se;
      t = strchr(t + 10, '"');
      if (t && sscanf(t + 1, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &se) == 6) {
        char xml[32];
        long long local;
        snprintf(xml, sizeof xml, "%04d%02d%02d%02d%02d%02d +0000", y, mo, d, h, mi, se);
        local = iptv_xmltv_time(xml);
        // Rounded to the quarter hour: the two readings are a network trip apart.
        if (local > 0) {
          long long diff = local - stamp;
          out->offset = (int)((diff >= 0 ? diff + 450 : diff - 450) / 900 * 900);
        }
      }
    }
    free(text);
  }
  snprintf(url, sizeof url, "%s/player_api.php?username=%s&password=%s&action=get_live_streams",
           base, u, p);
  if (!(text = net_download(url, 60))) return;
  out->known = 1;
  { const char *q = text, *end = text + strlen(text);
    int cap = 0;
    while ((q = strstr(q, "\"stream_id\""))) {
      const char *next = strstr(q + 11, "\"stream_id\"");
      const char *e = next ? next : end;
      long id = jsonNumber(q, e, "stream_id"), on = jsonNumber(q, e, "tv_archive");
      long days = jsonNumber(q, e, "tv_archive_duration");
      if (id >= 0 && on > 0) {
        if (out->n == cap) {
          Archive *g = realloc(out->a, (size_t)(cap ? cap * 2 : 256) * sizeof *g);
          if (!g) break;
          out->a = g; cap = cap ? cap * 2 : 256;
        }
        out->a[out->n].id = id;
        out->a[out->n].days = days > 0 ? (int)days : 1;
        out->n++;
      }
      q = e;
    }
  }
  free(text);
  if (out->n) qsort(out->a, (size_t)out->n, sizeof *out->a, archiveCmp);
  printf("[iptv] xtream: %d streams with an archive, server clock %+ds\n", out->n, out->offset);
}

static void applyArchives(IptvList *l, const Archives *ar) {
  if (!l || !ar->known) return;
  l->serverOffset = ar->offset;
  for (int i = 0; i < l->nCh; i++) {
    IptvChannel *c = &l->ch[i];
    Archive key, *hit;
    if (c->catchup || !iptv_xtream_parts(c->url, NULL, 0, NULL, 0, NULL, 0, &key.id, NULL, 0))
      continue;
    hit = ar->n ? bsearch(&key, ar->a, (size_t)ar->n, sizeof *ar->a, archiveCmp) : NULL;
    if (hit) { c->catchup = IPTV_CATCHUP_XC; c->catchupDays = hit->days; }
  }
}

static IptvList *parsePlaylist(const char *text) {
  IptvList *l = malloc(sizeof *l);
  if (!l) return NULL;
  iptv_list_init(l);
  if (iptv_parse_m3u(l, text) <= 0) { iptv_list_free(l); free(l); return NULL; }
  return l;
}

// A guide, inflated and cut to [from, to) as it inflates (iptv_guide_window):
// a week of a provider's or a country's guide never sits in memory whole.
static char *fetchGuide(const char *url, long long from, long long to) {
  long n = 0, m = 0;
  char *raw = net_download_bin(url, IPTV_GUIDE_TIMEOUT_S, &n), *xml;
  if (!raw) return NULL;
  xml = iptv_guide_window(raw, n, from, to, &m);
  free(raw);
  return xml;
}

// --- guides found for the channels no other guide covers --------------------------
//
// Providers tag their channels with the country they come from, "UK: BBC One",
// "|NL| NPO 1", "[DE] ZDF", or a group called "UK | SPORTS", and a country's
// public XMLTV guide (epgshare01's per-country files) covers most of what a
// provider's own guide leaves out. So when channels are still without a guide
// after the viewer's and the provider's, the countries of THOSE channels are
// counted and the guides of the most common ones (IPTV_AUTO_MAX) are read, as
// further guides: they only fill gaps. Each is kept on disk, already cut to the
// window, for IPTV_AUTO_KEEP_S, so the next start does not download it again.
#define IPTV_AUTO_MAX     3
#define IPTV_AUTO_MIN     3              // uncovered channels a country needs
#define IPTV_AUTO_KEEP_S  (8 * 3600)
#define IPTV_AUTO_BASE    "https://epgshare01.online/epgshare01/epg_ripper_"

// The provider's country tags and the guide file for each. "AR" is left out on
// purpose: on IPTV lists it means Arabic, not Argentina.
static const struct { const char *tag, *file; } COUNTRIES[] = {
  { "UK", "UK1" }, { "GB", "UK1" }, { "US", "US1" }, { "USA", "US1" }, { "CA", "CA1" },
  { "NL", "NL1" }, { "BE", "BE2" }, { "DE", "DE1" }, { "AT", "AT1" }, { "CH", "CH1" },
  { "FR", "FR1" }, { "IT", "IT1" }, { "ES", "ES1" }, { "PT", "PT1" }, { "IE", "IE1" },
  { "SE", "SE1" }, { "NO", "NO1" }, { "DK", "DK1" }, { "FI", "FI1" }, { "PL", "PL1" },
  { "TR", "TR1" }, { "GR", "GR1" }, { "AU", "AU1" }, { "NZ", "NZ1" }, { "IN", "IN1" },
  { "BR", "BR1" }, { "MX", "MX1" },
};
#define NCOUNTRIES ((int)(sizeof COUNTRIES / sizeof *COUNTRIES))

// The country tag leading `s`: two or three capitals, alone or in | or [ ],
// followed by ':', '|', ']' or '-'. -1 when none.
static int countryTag(const char *s) {
  char tag[4];
  size_t k = 0;
  if (!s) return -1;
  while (*s == ' ') s++;
  if (*s == '|' || *s == '[') s++;
  while (k < 3 && s[k] >= 'A' && s[k] <= 'Z') { tag[k] = s[k]; k++; }
  if (k < 2 || (s[k] >= 'A' && s[k] <= 'Z') || (s[k] >= 'a' && s[k] <= 'z')) return -1;
  { const char *q = s + k;
    while (*q == ' ') q++;
    if (!(*q == ':' || *q == '|' || *q == ']' || *q == '-')) return -1; }
  tag[k] = 0;
  for (int i = 0; i < NCOUNTRIES; i++) if (!strcmp(COUNTRIES[i].tag, tag)) return i;
  return -1;
}

typedef struct { char file[IPTV_AUTO_MAX][8]; int n; } AutoGuides;

static void autoGuides(const IptvList *l, AutoGuides *out) {
  int count[NCOUNTRIES];
  memset(count, 0, sizeof count);
  out->n = 0;
  for (int i = 0; i < l->nCh; i++) {
    int c;
    if (l->ch[i].nPg) continue;
    c = countryTag(l->ch[i].name);
    if (c < 0) c = countryTag(l->ch[i].group);
    if (c >= 0) count[c]++;
  }
  // Tags naming the same file ("UK", "GB") pool their counts.
  for (int i = 0; i < NCOUNTRIES; i++)
    for (int j = 0; j < i; j++)
      if (count[i] && !strcmp(COUNTRIES[i].file, COUNTRIES[j].file)) { count[j] += count[i]; count[i] = 0; }
  while (out->n < IPTV_AUTO_MAX) {
    int best = -1;
    for (int i = 0; i < NCOUNTRIES; i++)
      if (count[i] >= IPTV_AUTO_MIN && (best < 0 || count[i] > count[best])) best = i;
    if (best < 0) break;
    snprintf(out->file[out->n++], sizeof out->file[0], "%s", COUNTRIES[best].file);
    count[best] = 0;
  }
}

// A country's guide, cut to the window: from disk while fresh, else downloaded
// and kept. The first line of the kept file is the time it was fetched.
static char *autoGuide(const char *file, long long from, long long to) {
  char name[64], url[512], *xml, *kept;
  const char *base = getenv("NUVIO_AUTO_EPG_BASE");
  long long now = (long long)time(NULL);
  snprintf(name, sizeof name, "iptv-guide-%s.xml", file);
  if ((kept = data_read(name))) {
    long long at = strtoll(kept, NULL, 10);
    char *nl = strchr(kept, '\n');
    if (nl && at > 0 && now - at >= 0 && now - at < IPTV_AUTO_KEEP_S) {
      memmove(kept, nl + 1, strlen(nl + 1) + 1);
      return kept;
    }
    free(kept);
  }
  snprintf(url, sizeof url, "%s%s.xml.gz", base && *base ? base : IPTV_AUTO_BASE, file);
  // Kept for the whole of IPTV_AUTO_KEEP_S: the window reaches that much further.
  if (!(xml = fetchGuide(url, from, to + IPTV_AUTO_KEEP_S))) return NULL;
  { size_t n = strlen(xml);
    char *withStamp = malloc(n + 32);
    if (withStamp) {
      int k = snprintf(withStamp, 32, "%lld\n", now);
      memcpy(withStamp + k, xml, n + 1);
      data_write(name, withStamp);
      free(withStamp);
    } }
  return xml;
}

// What went wrong with a playlist download, in words that point at the fix.
// `*kind` is the IptvFailure, for a screen that words it its own way.
static void describeFailure(const IptvSource *src, const char *body, int status, int timedOut,
                            char *dst, size_t n, int *kind) {
  int x = src->kind == IPTV_SRC_XTREAM;
  if (timedOut) { *kind = IPTV_FAIL_TIMEOUT; snprintf(dst, n, "The playlist took too long to answer"); return; }
  if (!body && !status) {
    *kind = IPTV_FAIL_UNREACHABLE;
    snprintf(dst, n, "Couldn't reach the server. Check the address and the TV's connection");
    return;
  }
  if (status == 404) {
    *kind = IPTV_FAIL_NOT_FOUND;
    snprintf(dst, n, "No playlist at that address (404). Check it in a browser");
    return;
  }
  if (status == 401 || status == 403) {
    *kind = x ? IPTV_FAIL_LOGIN : IPTV_FAIL_REFUSED;
    snprintf(dst, n, x ? "The server refused this login (%d)" : "The server refused access (%d)", status);
    return;
  }
  // Xtream-style panels answer 884, 458 and the like when they turn an app
  // away: a blocked playlist download, a blocked device, too many connections.
  if (status >= 600) {
    *kind = IPTV_FAIL_PROVIDER;
    snprintf(dst, n, "The provider refused the playlist download (%d)", status);
    return;
  }
  if (status >= 500) {
    *kind = IPTV_FAIL_SERVER;
    snprintf(dst, n, "The server had an error (%d). Try again later", status);
    return;
  }
  if (status >= 400) { *kind = IPTV_FAIL_OTHER; snprintf(dst, n, "The server answered %d", status); return; }
  // It answered, with something that is not a playlist. A guide is the usual
  // mix-up: both come from the same provider page, one line apart.
  if (body && (((unsigned char)body[0] == 0x1f && (unsigned char)body[1] == 0x8b) ||
               strstr(body, "<tv") || !strncmp(body, "<?xml", 5))) {
    *kind = IPTV_FAIL_GUIDE;
    snprintf(dst, n, "That's a TV guide, not a playlist. Put it in the guide field");
    return;
  }
  // A web page where a playlist should be: almost always the provider's login.
  if (body && (strcasestr(body, "<html") || strcasestr(body, "<!doctype"))) {
    *kind = IPTV_FAIL_NOT_PLAYLIST;
    snprintf(dst, n, "That address returned a web page, not a playlist");
    return;
  }
  *kind = x ? IPTV_FAIL_LOGIN : IPTV_FAIL_NOT_PLAYLIST;
  snprintf(dst, n, x ? "The server refused this login" : "That address didn't return a playlist");
}

static void *loader(void *arg) {
  Job job = *(Job *)arg;
  char url[1400];
  GuideUrls guides;
  char *text = NULL, *cached = NULL;
  IptvList *l;
  int fromCache = 0;
  char prefer[8] = "";
  Archives archives;
  free(arg);
  memset(&archives, 0, sizeof archives);
  if (xtreamFromUrl(&job.src, &job.src, prefer, sizeof prefer))
    printf("[iptv] the playlist address is an Xtream login; loading it as one\n");

  // Stage 0: the cached playlist, so the screen has channels at once. Not for
  // a tried source: the cache is the current one's.
  if (!job.trial && !job.hasList && (cached = data_read(IPTV_CACHE_FILE))) {
    if ((l = parsePlaylist(cached))) {
      printf("[iptv] %d channels from the cache\n", l->nCh);
      post(l, job.gen);
      atomic_store(&state, IPTV_READY);
      fromCache = 1;
    }
  }

  // Stage 1: the playlist. With its HTTP status, so a failure can say WHICH
  // failure it was: "couldn't reach" for a 404 sends people checking their
  // network when the address is what is wrong.
  if (!job.trial) setStatus("Loading channels\xE2\x80\xA6");
  { int status = 0, apiStatus = 0, kind = IPTV_FAIL_OTHER;
    char why[160] = "";
    text = NULL;
    // Xtream: the API first, get.php only when there is no API answer at all.
    if (job.src.kind == IPTV_SRC_XTREAM)
      text = xtreamApi(&job.src, prefer, why, sizeof why, &apiStatus, &kind);
    if (!text && !why[0]) {
      playlistUrl(&job.src, url, sizeof url);
      text = net_download_st(url, IPTV_PLAYLIST_TIMEOUT_S, NULL, &status);
      // The body is kept for describeFailure: a login page says what it is.
      if (text && status >= 400) { free(text); text = NULL; }
    }
    if (job.gen != atomic_load(&generation)) goto out;
    l = text ? parsePlaylist(text) : NULL;
    if (!l) {
      // The API broke and get.php failed after it: the API's error is the one
      // that matters. Plenty of panels refuse get.php for good (884), and
      // blaming the playlist download then hides the real answer. Panels also
      // use 5xx for a login they don't know (World 8K: 513 for a mistyped
      // username, 200 for the right one), so the login comes first in the hint.
      if (!why[0] && apiStatus >= 500) {
        snprintf(why, sizeof why,
                 "The provider refused the login (%d). Check the username and password",
                 apiStatus);
        kind = IPTV_FAIL_LOGIN;
        status = apiStatus;
      }
    }
    if (!l && job.trial) {
      // Nothing of the current source is touched; the screen gets the reason.
      if (!why[0])
        describeFailure(&job.src, text, status, !text && net_timed_out(), why, sizeof why, &kind);
      printf("[iptv] tried source failed: HTTP %d, %s\n", status, why);
      pthread_mutex_lock(&mu);
      snprintf(trialWhy, sizeof trialWhy, "%s", why);
      pthread_mutex_unlock(&mu);
      atomic_store(&trialKind, kind);
      atomic_store(&trialStatus, status);
      atomic_store(&trialState, IPTV_FAILED);
      goto out;
    }
    if (l && job.trial) {
      // It works: it becomes the source (the main thread swaps it in).
      atomic_store(&trialCommit, 1);
      atomic_store(&guideState, IPTV_LOADING);
    }
    if (!l) {
      if (!why[0])
        describeFailure(&job.src, text, status, !text && net_timed_out(), why, sizeof why, &kind);
      printf("[iptv] playlist failed: HTTP %d, %s\n", status, why);
      if (fromCache) {
        setStatus("Offline \xC2\xB7 showing the saved channel list");
        atomic_store(&state, IPTV_READY);
      } else {
        setStatus(why);
        atomic_store(&state, IPTV_FAILED);
      }
      atomic_store(&guideState, IPTV_FAILED);
      free(text); text = cached; cached = NULL;
      if (!fromCache) goto out;
    } else {
      printf("[iptv] %d channels, %d groups\n", l->nCh, l->nGroups);
      data_write(IPTV_CACHE_FILE, text);
      atomic_store(&refreshedAt, (long long)time(NULL));
      fetchArchives(&job.src, &archives);
      if (job.gen != atomic_load(&generation)) { iptv_list_free(l); free(l); goto out; }
      applyArchives(l, &archives);
      post(l, job.gen);
      atomic_store(&state, IPTV_READY);
    } }

  // Stage 2: the guide, onto a fresh parse of the same playlist text, the
  // channel indices come out identical, which is what lets the screen keep its
  // focus across the swap.
  { IptvList *probe = parsePlaylist(text);
    long long behind = IPTV_GUIDE_BEHIND_S;
    applyArchives(probe, &archives);
    // With an archive, the past is watchable: keep as much of it as the guide
    // has, up to a day (two would double the guide's memory for little use).
    if (probe) for (int i = 0; i < probe->nCh; i++)
      if (probe->ch[i].catchup) { behind = IPTV_GUIDE_ARCHIVE_S; break; }
    guideUrls(&job.src, probe, &guides);
    if (!probe) {
      setStatus("");
      atomic_store(&guideState, IPTV_FAILED);
      goto out;
    }
    atomic_store(&guideState, IPTV_LOADING);
    { long long now = (long long)time(NULL), from = now - behind, to = now + IPTV_GUIDE_AHEAD_S;
      int total = 0, with = 0, nKept = 0;
      AutoGuides extra;
      // The guides read, kept cut to the window: the list with the countries'
      // guides added is built again from them, after the first one is shown.
      char *kept[IPTV_GUIDES_MAX];
      for (int gi = 0; gi < guides.n; gi++) {
        char *xml;
        int n, got;
        if (guides.n > 1) {
          char st[96];
          snprintf(st, sizeof st, "Loading the TV guide (%d of %d)\xE2\x80\xA6", gi + 1, guides.n);
          setStatus(st);
        } else {
          setStatus("Loading the TV guide\xE2\x80\xA6");
        }
        xml = fetchGuide(guides.url[gi], from, to);
        got = xml != NULL;
        if (job.gen != atomic_load(&generation)) {
          free(xml);
          for (int k = 0; k < nKept; k++) free(kept[k]);
          iptv_list_free(probe); free(probe);
          goto out;
        }
        n = !xml ? 0 : total ? iptv_parse_xmltv_more(probe, xml, from, to)
                             : iptv_parse_xmltv(probe, xml, from, to);
        if (n > 0) kept[nKept++] = xml; else free(xml);
        // Only the host in the log: xmltv.php carries the login in its query.
        { char host[256]; const char *h = strstr(guides.url[gi], "://");
          size_t k = 0;
          h = h ? h + 3 : guides.url[gi];
          while (h[k] && h[k] != '/' && h[k] != '?' && k + 1 < sizeof host) { host[k] = h[k]; k++; }
          host[k] = 0;
          printf("[iptv] guide %d/%d (%s): %s, %d programmes added\n", gi + 1, guides.n, host,
                 got ? "read" : "download failed", n); }
        total += n;
        with = 0;
        for (int i = 0; i < probe->nCh; i++) with += probe->ch[i].nPg > 0;
        if (with == probe->nCh) break;           // nothing left to fill
      }
      autoGuides(probe, &extra);
      if (total > 0) {
        printf("[iptv] guide: %d programmes on %d of %d channels\n", probe->nPg, with, probe->nCh);
        setStatus("");
        post(probe, job.gen);
        atomic_store(&guideState, IPTV_READY);
        probe = NULL;
      }
      if (extra.n) {
        // Again from the playlist, with the guides above, then the countries'.
        int added = 0;
        char st[96];
        if (!probe) {
          probe = parsePlaylist(text);
          applyArchives(probe, &archives);
          for (int k = 0; probe && k < nKept; k++) {
            if (k) iptv_parse_xmltv_more(probe, kept[k], from, to);
            else iptv_parse_xmltv(probe, kept[k], from, to);
          }
        }
        for (int k = 0; k < nKept; k++) free(kept[k]);
        nKept = 0;
        snprintf(st, sizeof st, "Finding guides for %d more channels\xE2\x80\xA6", probe ? probe->nCh - with : 0);
        setStatus(st);
        for (int k = 0; probe && k < extra.n; k++) {
          char *xml = autoGuide(extra.file[k], from, to);
          int n;
          if (job.gen != atomic_load(&generation)) {
            free(xml);
            iptv_list_free(probe); free(probe);
            goto out;
          }
          n = !xml ? 0 : (total + added) ? iptv_parse_xmltv_more(probe, xml, from, to)
                                         : iptv_parse_xmltv(probe, xml, from, to);
          printf("[iptv] country guide %s: %s, %d programmes added\n", extra.file[k],
                 xml ? "read" : "download failed", n);
          free(xml);
          added += n;
        }
        if (probe && added > 0) {
          with = 0;
          for (int i = 0; i < probe->nCh; i++) with += probe->ch[i].nPg > 0;
          printf("[iptv] guide with countries': %d programmes on %d of %d channels\n", probe->nPg, with, probe->nCh);
          setStatus("");
          post(probe, job.gen);
          atomic_store(&guideState, IPTV_READY);
          probe = NULL;
          total += added;
        } else if (total > 0) {
          setStatus("");
        }
      }
      for (int k = 0; k < nKept; k++) free(kept[k]);
      if (probe) { iptv_list_free(probe); free(probe); }
      if (total <= 0) {
        printf("[iptv] guide unavailable\n");
        setStatus(guides.n || extra.n ? "TV guide unavailable" : "");
        atomic_store(&guideState, IPTV_FAILED);
      }
    }
  }
out:
  free(text); free(cached); free(archives.a);
  fflush(stdout);
  atomic_store(&busy, 0);
  return NULL;
}

static void startLoad(void) {
  pthread_t t;
  Job *job;
  if (!iptv_configured() && !trialWanted) return;
  if (atomic_load(&busy)) { restartPending = 1; return; }
  job = malloc(sizeof *job);
  if (!job) return;
  job->trial = trialWanted;
  job->src = job->trial ? trialSrc : source;
  job->gen = atomic_fetch_add(&generation, 1) + 1;
  job->hasList = live != NULL;
  trialWanted = 0;
  restartPending = 0;
  loadedAt = SDL_GetTicks();
  if (!loadedAt) loadedAt = 1;
  if (!job->trial) {
    if (!live) atomic_store(&state, IPTV_LOADING);
    atomic_store(&guideState, IPTV_LOADING);
  }
  atomic_store(&busy, 1);
  if (pthread_create(&t, NULL, loader, job) != 0) {
    atomic_store(&busy, 0);
    free(job);
    return;
  }
  pthread_detach(t);
}

// --- Persistence ---------------------------------------------------------------------
static void readConfig(void) {
  char *s = data_read(IPTV_CONFIG_FILE), *line, *save = NULL;
  memset(&source, 0, sizeof source);
  sourceName[0] = 0;
  if (!s) { updateLabel(); return; }
  for (line = strtok_r(s, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
    char *eq = strchr(line, '='), *v;
    size_t k;
    if (!eq) continue;
    *eq = 0; v = eq + 1;
    k = strlen(v);
    while (k && (v[k - 1] == '\r' || v[k - 1] == ' ')) v[--k] = 0;
    if (!strcmp(line, "kind"))
      source.kind = !strcmp(v, "xtream") ? IPTV_SRC_XTREAM : !strcmp(v, "m3u") ? IPTV_SRC_M3U
                                                                               : IPTV_SRC_NONE;
    else if (!strcmp(line, "url"))    snprintf(source.url, sizeof source.url, "%s", v);
    else if (!strcmp(line, "epg"))    snprintf(source.epg, sizeof source.epg, "%s", v);
    else if (!strcmp(line, "server")) snprintf(source.server, sizeof source.server, "%s", v);
    else if (!strcmp(line, "user"))   snprintf(source.user, sizeof source.user, "%s", v);
    else if (!strcmp(line, "pass"))   snprintf(source.pass, sizeof source.pass, "%s", v);
    else if (!strcmp(line, "name"))   snprintf(sourceName, sizeof sourceName, "%s", v);
  }
  free(s);
  updateLabel();
}

static void writeConfig(void) {
  char buf[4096];
  snprintf(buf, sizeof buf, "kind=%s\nurl=%s\nepg=%s\nserver=%s\nuser=%s\npass=%s\nname=%s\n",
           source.kind == IPTV_SRC_XTREAM ? "xtream" : source.kind == IPTV_SRC_M3U ? "m3u" : "none",
           source.url, source.epg, source.server, source.user, source.pass, sourceName);
  data_write(IPTV_CONFIG_FILE, buf);
}

static int readNames(const char *file, char **dst, int max) {
  char *s = data_read(file), *line, *save = NULL;
  int n = 0;
  if (!s) return 0;
  for (line = strtok_r(s, "\n", &save); line && n < max; line = strtok_r(NULL, "\n", &save))
    if (line[0] && (dst[n] = strdup(line))) n++;
  free(s);
  return n;
}

static void writeNames(const char *file, char **names, int n) {
  size_t size = 1;
  char *buf, *w;
  for (int i = 0; i < n; i++) size += strlen(names[i]) + 1;
  if (!(buf = malloc(size))) return;
  w = buf;
  for (int i = 0; i < n; i++) { size_t k = strlen(names[i]); memcpy(w, names[i], k); w += k; *w++ = '\n'; }
  *w = 0;
  data_write(file, buf);
  free(buf);
}

// --- The API -------------------------------------------------------------------------
void iptv_start(void) {
  readConfig();
  nFavourites = readNames(IPTV_FAV_FILE, favourites, IPTV_MAX_FAVOURITES);
  nRecents = readNames(IPTV_RECENT_FILE, recents, IPTV_MAX_RECENT);
}

void iptv_touch(void) {
  if (!iptv_configured() || atomic_load(&busy)) return;
  // No list yet (a first visit, or a load that failed last time) is always
  // worth another try; a list is refreshed once its guide has gone stale.
  if (!loadedAt || !live || SDL_GetTicks() - loadedAt > IPTV_GUIDE_STALE_MS)
    startLoad();
}

int iptv_step(void) {
  IptvList *l;
  if (atomic_exchange(&trialCommit, 0)) {
    keepNameFor(&trialSrc);
    source = trialSrc;
    writeConfig();
    updateLabel();
    atomic_store(&state, IPTV_READY);
    atomic_store(&trialState, IPTV_READY);
  }
  // Past the ceiling: the try is given up, with the current source as it was.
  if (atomic_load(&trialState) == IPTV_LOADING && trialDeadline &&
      (Sint32)(SDL_GetTicks() - trialDeadline) >= 0) {
    atomic_fetch_add(&generation, 1);
    trialWanted = 0;
    atomic_store(&trialKind, IPTV_FAIL_TIMEOUT);
    atomic_store(&trialStatus, 0);
    atomic_store(&trialState, IPTV_FAILED);
    printf("[iptv] tried source: no playlist within the ceiling\n");
  }
  // A failed try interrupted the current source's own load: finish that one.
  if (atomic_load(&trialState) == IPTV_FAILED && trialBusyBefore) {
    trialBusyBefore = 0;
    if (iptv_configured()) { restartPending = 1; }
  }
  pthread_mutex_lock(&mu);
  l = incoming; incoming = NULL;
  pthread_mutex_unlock(&mu);
  if (restartPending && !atomic_load(&busy)) startLoad();
  if (!l) return 0;
  if (live) { iptv_list_free(live); free(live); }
  live = l;
  return 1;
}

void iptv_shutdown(void) {
  // A loader still running owns nothing of ours but `incoming`, which it hands
  // over under the lock; bumping the generation makes it throw its work away.
  atomic_fetch_add(&generation, 1);
  pthread_mutex_lock(&mu);
  if (incoming) { iptv_list_free(incoming); free(incoming); incoming = NULL; }
  pthread_mutex_unlock(&mu);
  if (live) { iptv_list_free(live); free(live); live = NULL; }
  for (int i = 0; i < nFavourites; i++) free(favourites[i]);
  for (int i = 0; i < nRecents; i++) free(recents[i]);
  nFavourites = nRecents = 0;
}

const IptvSource *iptv_source(void) { return &source; }

int iptv_configured(void) {
  if (source.kind == IPTV_SRC_M3U) return source.url[0] != 0;
  if (source.kind == IPTV_SRC_XTREAM) return source.server[0] && source.user[0];
  return 0;
}

// The list on screen stays until the new one lands, unless the source itself
// changed, then its channels are somebody else's and go at once.
static void reload(int keepList) {
  atomic_fetch_add(&generation, 1);
  pthread_mutex_lock(&mu);
  if (incoming) { iptv_list_free(incoming); free(incoming); incoming = NULL; }
  pthread_mutex_unlock(&mu);
  if (live && !keepList) { iptv_list_free(live); free(live); live = NULL; }
  atomic_store(&state, IPTV_LOADING);
  setStatus("Loading channels\xE2\x80\xA6");
  loadedAt = 0;
  startLoad();
}

void iptv_reload(void) { reload(1); }

void iptv_try_source(const IptvSource *s, int ceilingS) {
  IptvList *drop;
  trialSrc = *s;
  trialWanted = 1;
  trialBusyBefore = atomic_load(&busy) || atomic_load(&guideState) == IPTV_LOADING || !live;
  trialDeadline = SDL_GetTicks() + (Uint32)(ceilingS > 0 ? ceilingS : 15) * 1000u;
  if (!trialDeadline) trialDeadline = 1;
  atomic_store(&trialCommit, 0);
  atomic_store(&trialKind, IPTV_FAIL_NONE);
  atomic_store(&trialStatus, 0);
  atomic_store(&trialState, IPTV_LOADING);
  // Whatever the current source's loader is doing is set aside.
  atomic_fetch_add(&generation, 1);
  pthread_mutex_lock(&mu);
  drop = incoming; incoming = NULL;
  trialWhy[0] = 0;
  pthread_mutex_unlock(&mu);
  if (drop) { iptv_list_free(drop); free(drop); }
  startLoad();
}

int iptv_try_state(void) { return atomic_load(&trialState); }

int iptv_try_failure(int *status, char *why, size_t n) {
  if (status) *status = atomic_load(&trialStatus);
  if (why && n) {
    pthread_mutex_lock(&mu);
    snprintf(why, n, "%s", trialWhy);
    pthread_mutex_unlock(&mu);
  }
  return atomic_load(&trialKind);
}

void iptv_try_cancel(void) {
  if (atomic_load(&trialState) != IPTV_LOADING) return;
  atomic_fetch_add(&generation, 1);
  trialWanted = 0;
  atomic_store(&trialState, IPTV_IDLE);
  if (trialBusyBefore && iptv_configured()) restartPending = 1;
  trialBusyBefore = 0;
}

void iptv_try_forget(void) {
  if (atomic_load(&trialState) != IPTV_LOADING) atomic_store(&trialState, IPTV_IDLE);
}

long long iptv_refreshed_at(void) { return atomic_load(&refreshedAt); }

void iptv_set_source(const IptvSource *s) {
  keepNameFor(s);
  source = *s;
  writeConfig();
  updateLabel();
  data_erase(IPTV_CACHE_FILE);
  reload(0);
}

const IptvList *iptv_list(void) { return live; }
int iptv_state(void) { return atomic_load(&state); }
int iptv_guide_state(void) { return atomic_load(&guideState); }
const char *iptv_source_label(void) { return sourceLabel; }
const char *iptv_source_name(void) { return sourceName; }

void iptv_set_source_name(const char *name) {
  size_t a = 0, b;
  // Trimmed, and one line: iptv.txt is key=value per line.
  while (name[a] == ' ') a++;
  snprintf(sourceName, sizeof sourceName, "%s", name + a);
  for (char *c = sourceName; *c; c++) if (*c == '\n' || *c == '\r') *c = ' ';
  b = strlen(sourceName);
  while (b && sourceName[b - 1] == ' ') sourceName[--b] = 0;
  writeConfig();
  updateLabel();
}

const char *iptv_status(void) {
  static char copy[160];
  pthread_mutex_lock(&mu);
  memcpy(copy, statusLine, sizeof copy);
  pthread_mutex_unlock(&mu);
  return copy;
}

static int nameIndex(char **names, int n, const char *name) {
  for (int i = 0; i < n; i++) if (!strcmp(names[i], name)) return i;
  return -1;
}

int iptv_is_favourite(int ch) {
  if (!live || ch < 0 || ch >= live->nCh) return 0;
  return nameIndex(favourites, nFavourites, live->ch[ch].name) >= 0;
}

void iptv_toggle_favourite(int ch) {
  int i;
  if (!live || ch < 0 || ch >= live->nCh) return;
  i = nameIndex(favourites, nFavourites, live->ch[ch].name);
  if (i >= 0) {
    free(favourites[i]);
    memmove(favourites + i, favourites + i + 1, (size_t)(nFavourites - i - 1) * sizeof *favourites);
    nFavourites--;
  } else if (nFavourites < IPTV_MAX_FAVOURITES) {
    char *d = strdup(live->ch[ch].name);
    if (!d) return;
    favourites[nFavourites++] = d;
  }
  writeNames(IPTV_FAV_FILE, favourites, nFavourites);
}

int iptv_recent(int i) {
  if (!live || i < 0 || i >= nRecents) return -1;
  for (int c = 0; c < live->nCh; c++) if (!strcmp(live->ch[c].name, recents[i])) return c;
  return -1;
}

void iptv_note_watched(int ch) {
  int i;
  char *name;
  if (!live || ch < 0 || ch >= live->nCh) return;
  i = nameIndex(recents, nRecents, live->ch[ch].name);
  if (i == 0) return;
  if (i > 0) { name = recents[i]; memmove(recents + 1, recents, (size_t)i * sizeof *recents); }
  else {
    if (!(name = strdup(live->ch[ch].name))) return;
    if (nRecents == IPTV_MAX_RECENT) free(recents[--nRecents]);
    memmove(recents + 1, recents, (size_t)nRecents * sizeof *recents);
    nRecents++;
  }
  recents[0] = name;
  writeNames(IPTV_RECENT_FILE, recents, nRecents);
}

int iptv_has_archive(int ch, long long t) {
  const IptvChannel *c;
  long long now = (long long)time(NULL);
  if (!live || ch < 0 || ch >= live->nCh) return 0;
  c = &live->ch[ch];
  if (!c->catchup) return 0;
  // A minute of margin at the far end: the oldest minute is the next to go.
  return t < now && t >= now - (long long)c->catchupDays * 86400 + 60;
}

const char *iptv_archive_url(int ch, long long start, long long stop, char *dst, unsigned size) {
  const IptvChannel *c;
  char raw[2048];
  long long now = (long long)time(NULL);
  if (!live || ch < 0 || ch >= live->nCh) return NULL;
  c = &live->ch[ch];
  if (stop <= start) stop = start + 3600;
  if (!iptv_catchup_url(c, start, stop, now, live->serverOffset, raw, sizeof raw)) return NULL;
  if (c->headers[0] && !strstr(raw, ".m3u8")) return proxy_wrap(raw, c->headers, dst, size);
  if (snprintf(dst, size, "%s", raw) >= (int)size) return NULL;
  return dst;
}

const char *iptv_play_url(int ch, char *dst, unsigned size) {
  const IptvChannel *c;
  if (!live || ch < 0 || ch >= live->nCh) return NULL;
  c = &live->ch[ch];
  // The relay serves ONE resource. An HLS playlist's segments are resolved
  // against the playlist's own address, which would be the relay's, so HLS
  // goes direct, and loses the headers, rather than losing every segment.
  if (c->headers[0] && !strstr(c->url, ".m3u8")) return proxy_wrap(c->url, c->headers, dst, size);
  return c->url;
}

const char *iptv_play_url_alt(int ch, char *dst, unsigned size) {
  static char alt[2048];
  const IptvChannel *c;
  size_t n;
  if (!live || ch < 0 || ch >= live->nCh) return NULL;
  c = &live->ch[ch];
  n = strcspn(c->url, "?#");
  if (!strstr(c->url, "/live/") || n + 2 >= sizeof alt) return NULL;
  if (n > 5 && !strncmp(c->url + n - 5, ".m3u8", 5))
    snprintf(alt, sizeof alt, "%.*s.ts%s", (int)(n - 5), c->url, c->url + n);
  else if (n > 3 && !strncmp(c->url + n - 3, ".ts", 3))
    snprintf(alt, sizeof alt, "%.*s.m3u8%s", (int)(n - 3), c->url, c->url + n);
  else
    return NULL;
  // The relay rule of iptv_play_url, for the address actually played.
  if (c->headers[0] && !strstr(alt, ".m3u8")) return proxy_wrap(alt, c->headers, dst, size);
  return alt;
}
