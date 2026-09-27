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
// A guide older than this is fetched again when the screen is opened.
#define IPTV_GUIDE_STALE_MS   (4u * 3600u * 1000u)
#define IPTV_MAX_FAVOURITES   500
#define IPTV_MAX_RECENT        30

static IptvSource source;
static char sourceLabel[96];

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

static void guideUrl(const IptvSource *s, const IptvList *l, char *dst, size_t n) {
  if (s->epg[0]) { snprintf(dst, n, "%s", s->epg); return; }
  if (s->kind == IPTV_SRC_XTREAM) {
    char base[600], u[400], p[400];
    serverBase(s->server, base, sizeof base);
    urlEncode(s->user, u, sizeof u);
    urlEncode(s->pass, p, sizeof p);
    snprintf(dst, n, "%s/xmltv.php?username=%s&password=%s", base, u, p);
    return;
  }
  snprintf(dst, n, "%s", l && l->epgUrl[0] ? l->epgUrl : "");
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
static int xtreamFromUrl(const IptvSource *in, IptvSource *out) {
  const char *g;
  IptvSource s;
  if (in->kind != IPTV_SRC_M3U || !(g = strstr(in->url, "/get.php?"))) return 0;
  memset(&s, 0, sizeof s);
  s.kind = IPTV_SRC_XTREAM;
  if (!queryParam(in->url, "username", s.user, sizeof s.user) ||
      !queryParam(in->url, "password", s.pass, sizeof s.pass)) return 0;
  if ((size_t)(g - in->url) >= sizeof s.server) return 0;
  memcpy(s.server, in->url, (size_t)(g - in->url));
  s.server[g - in->url] = 0;
  snprintf(s.epg, sizeof s.epg, "%s", in->epg);
  *out = s;
  return 1;
}

// THE XTREAM API: the account, then the live categories and streams, written
// out as the playlist get.php would have served (iptv_xtream_m3u). NULL when
// the API is not there or refused; `why` then says so when the answer was
// specific (a refused login, an expired account), and stays empty otherwise so
// get.php can be tried.
static char *xtreamApi(const IptvSource *src, char *why, size_t n) {
  char base[600], u[400], p[400], api[1400], url[1500], state[40] = "", ext[8] = "m3u8";
  char *acct, *cats, *streams, *m3u;
  int st = 0;
  why[0] = 0;
  serverBase(src->server, base, sizeof base);
  urlEncode(src->user, u, sizeof u);
  urlEncode(src->pass, p, sizeof p);
  snprintf(api, sizeof api, "%s/player_api.php?username=%s&password=%s", base, u, p);
  acct = net_download_st(api, IPTV_PLAYLIST_TIMEOUT_S, NULL, &st);
  if (!acct || st >= 400 || !strstr(acct, "user_info")) {
    printf("[iptv] xtream api: HTTP %d, %s\n", st, acct ? "not an account answer" : "no answer");
    free(acct);
    return NULL;
  }
  { int auth = (int)js_num(acct, NULL, "auth", -1);
    const char *f = strstr(acct, "\"allowed_output_formats\"");
    const char *fe = f ? strchr(f, ']') : NULL;
    js_text(acct, NULL, "status", state, sizeof state);
    // HLS when the account allows it, for the reason playlistUrl gives; raw TS
    // when that is all it may use.
    if (f && fe) {
      char list[128];
      size_t k = (size_t)(fe - f) < sizeof list - 1 ? (size_t)(fe - f) : sizeof list - 1;
      memcpy(list, f, k); list[k] = 0;
      if (!strstr(list, "\"m3u8\"") && strstr(list, "\"ts\"")) snprintf(ext, sizeof ext, "ts");
    }
    printf("[iptv] xtream api: auth %d, status '%s', streams as .%s\n", auth, state, ext);
    free(acct);
    if (auth == 0) { snprintf(why, n, "The server refused this login \xE2\x80\x94 check the username and password"); return NULL; }
    if (!strcasecmp(state, "Expired")) { snprintf(why, n, "This IPTV subscription has expired"); return NULL; }
    if (state[0] && strcasecmp(state, "Active")) {
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
  snprintf(sourceLabel, sizeof sourceLabel, "%s", host[0] ? host : "IPTV");
}

// --- The loader ----------------------------------------------------------------------
typedef struct { IptvSource src; unsigned gen; int hasList; } Job;

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

static IptvList *parsePlaylist(const char *text) {
  IptvList *l = malloc(sizeof *l);
  if (!l) return NULL;
  iptv_list_init(l);
  if (iptv_parse_m3u(l, text) <= 0) { iptv_list_free(l); free(l); return NULL; }
  return l;
}

static char *fetchGuide(const char *url) {
  long n = 0, m = 0;
  char *raw = net_download_bin(url, IPTV_GUIDE_TIMEOUT_S, &n), *xml;
  if (!raw) return NULL;
  xml = iptv_gunzip(raw, n, &m);
  if (xml) { free(raw); return xml; }
  return raw;   // plain XML; net_download_bin terminates it
}

// What went wrong with a playlist download, in words that point at the fix.
static void describeFailure(const IptvSource *src, const char *body, int status, int timedOut,
                            char *dst, size_t n) {
  if (timedOut) { snprintf(dst, n, "The playlist took too long to answer"); return; }
  if (!body && !status) {
    snprintf(dst, n, "Couldn't reach the server \xE2\x80\x94 check the address and the TV's connection");
    return;
  }
  if (status == 404) { snprintf(dst, n, "No playlist at that address (404) \xE2\x80\x94 check it in a browser"); return; }
  if (status == 401 || status == 403) {
    snprintf(dst, n, src->kind == IPTV_SRC_XTREAM ? "The server refused this login (%d)"
                                                  : "The server refused access (%d)", status);
    return;
  }
  // Xtream-style panels answer 884, 458 and the like when they turn an app
  // away: a blocked playlist download, a blocked device, too many connections.
  if (status >= 600) {
    snprintf(dst, n, "The provider refused the playlist download (%d)", status);
    return;
  }
  if (status >= 500) { snprintf(dst, n, "The server had an error (%d) \xE2\x80\x94 try again later", status); return; }
  if (status >= 400) { snprintf(dst, n, "The server answered %d", status); return; }
  // It answered, with something that is not a playlist. A guide is the usual
  // mix-up: both come from the same provider page, one line apart.
  if (body && (((unsigned char)body[0] == 0x1f && (unsigned char)body[1] == 0x8b) ||
               strstr(body, "<tv") || !strncmp(body, "<?xml", 5))) {
    snprintf(dst, n, "That's a TV guide, not a playlist \xE2\x80\x94 put it in the guide field");
    return;
  }
  snprintf(dst, n, src->kind == IPTV_SRC_XTREAM ? "The server refused this login"
                                                : "That address didn't return a playlist");
}

static void *loader(void *arg) {
  Job job = *(Job *)arg;
  char url[1400], gurl[1400];
  char *text = NULL, *cached = NULL;
  IptvList *l;
  int fromCache = 0;
  free(arg);
  if (xtreamFromUrl(&job.src, &job.src))
    printf("[iptv] the playlist address is an Xtream login; loading it as one\n");

  // Stage 0: the cached playlist, so the screen has channels at once.
  if (!job.hasList && (cached = data_read(IPTV_CACHE_FILE))) {
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
  setStatus("Loading channels\xE2\x80\xA6");
  { int status = 0;
    char why[160] = "";
    text = NULL;
    // Xtream: the API first, get.php only when there is no API answer at all.
    if (job.src.kind == IPTV_SRC_XTREAM) text = xtreamApi(&job.src, why, sizeof why);
    if (!text && !why[0]) {
      playlistUrl(&job.src, url, sizeof url);
      text = net_download_st(url, IPTV_PLAYLIST_TIMEOUT_S, NULL, &status);
      if (text && status >= 400) { free(text); text = NULL; }
    }
    if (job.gen != atomic_load(&generation)) goto out;
    l = text ? parsePlaylist(text) : NULL;
    if (!l) {
      if (!why[0])
        describeFailure(&job.src, text, status, !text && net_timed_out(), why, sizeof why);
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
      post(l, job.gen);
      atomic_store(&state, IPTV_READY);
    } }

  // Stage 2: the guide, onto a fresh parse of the same playlist text — the
  // channel indices come out identical, which is what lets the screen keep its
  // focus across the swap.
  { IptvList *probe = parsePlaylist(text);
    guideUrl(&job.src, probe, gurl, sizeof gurl);
    if (!gurl[0]) {
      setStatus("");
      atomic_store(&guideState, IPTV_FAILED);
      printf("[iptv] no guide: the playlist names none\n");
      if (probe) { iptv_list_free(probe); free(probe); }
      goto out;
    }
    atomic_store(&guideState, IPTV_LOADING);
    setStatus("Loading the TV guide\xE2\x80\xA6");
    { char *xml = fetchGuide(gurl);
      long long now = (long long)time(NULL);
      int n;
      if (job.gen != atomic_load(&generation)) {
        free(xml);
        if (probe) { iptv_list_free(probe); free(probe); }
        goto out;
      }
      n = (xml && probe) ? iptv_parse_xmltv(probe, xml, now - IPTV_GUIDE_BEHIND_S,
                                            now + IPTV_GUIDE_AHEAD_S) : 0;
      free(xml);
      if (n > 0) {
        int with = 0;
        for (int i = 0; i < probe->nCh; i++) with += probe->ch[i].nPg > 0;
        printf("[iptv] guide: %d programmes on %d of %d channels\n", n, with, probe->nCh);
        setStatus("");
        post(probe, job.gen);
        atomic_store(&guideState, IPTV_READY);
      } else {
        printf("[iptv] guide unavailable (%s)\n", xml ? "nothing matched" : "download failed");
        setStatus("TV guide unavailable");
        atomic_store(&guideState, IPTV_FAILED);
        if (probe) { iptv_list_free(probe); free(probe); }
      }
    }
  }
out:
  free(text); free(cached);
  fflush(stdout);
  atomic_store(&busy, 0);
  return NULL;
}

static void startLoad(void) {
  pthread_t t;
  Job *job;
  if (!iptv_configured()) return;
  if (atomic_load(&busy)) { restartPending = 1; return; }
  job = malloc(sizeof *job);
  if (!job) return;
  job->src = source;
  job->gen = atomic_fetch_add(&generation, 1) + 1;
  job->hasList = live != NULL;
  restartPending = 0;
  loadedAt = SDL_GetTicks();
  if (!loadedAt) loadedAt = 1;
  if (!live) atomic_store(&state, IPTV_LOADING);
  atomic_store(&guideState, IPTV_LOADING);
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
  }
  free(s);
  updateLabel();
}

static void writeConfig(void) {
  char buf[4096];
  snprintf(buf, sizeof buf, "kind=%s\nurl=%s\nepg=%s\nserver=%s\nuser=%s\npass=%s\n",
           source.kind == IPTV_SRC_XTREAM ? "xtream" : source.kind == IPTV_SRC_M3U ? "m3u" : "none",
           source.url, source.epg, source.server, source.user, source.pass);
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
// changed — then its channels are somebody else's and go at once.
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

void iptv_set_source(const IptvSource *s) {
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

const char *iptv_play_url(int ch, char *dst, unsigned size) {
  const IptvChannel *c;
  if (!live || ch < 0 || ch >= live->nCh) return NULL;
  c = &live->ch[ch];
  // The relay serves ONE resource. An HLS playlist's segments are resolved
  // against the playlist's own address, which would be the relay's — so HLS
  // goes direct, and loses the headers, rather than losing every segment.
  if (c->headers[0] && !strstr(c->url, ".m3u8")) return proxy_wrap(c->url, c->headers, dst, size);
  return c->url;
}
