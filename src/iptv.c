// Live TV's source, loader and small persistent state. See iptv.h.
#include "iptv.h"
#include "data.h"
#include "net.h"
#include "proxy.h"
#include <SDL2/SDL.h>
#include <ctype.h>
#include <pthread.h>
#include <stdatomic.h>
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
// Playback preferences, kept in iptv.txt beside the source. Off by default: the
// pause buffer writes the channel to the TV's flash for as long as it plays.
static int prefBuffer;     // minutes, 0 = off
static int prefPreview;    // preview the focused channel while browsing

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
  Archives archives;
  free(arg);
  memset(&archives, 0, sizeof archives);

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
  playlistUrl(&job.src, url, sizeof url);
  { int status = 0;
    text = net_download_st(url, IPTV_PLAYLIST_TIMEOUT_S, NULL, &status);
    if (text && status >= 400) { free(text); text = NULL; }
    if (job.gen != atomic_load(&generation)) goto out;
    l = text ? parsePlaylist(text) : NULL;
    if (!l) {
      char why[160];
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
      fetchArchives(&job.src, &archives);
      if (job.gen != atomic_load(&generation)) { iptv_list_free(l); free(l); goto out; }
      applyArchives(l, &archives);
      post(l, job.gen);
      atomic_store(&state, IPTV_READY);
    } }

  // Stage 2: the guide, onto a fresh parse of the same playlist text — the
  // channel indices come out identical, which is what lets the screen keep its
  // focus across the swap.
  { IptvList *probe = parsePlaylist(text);
    long long behind = IPTV_GUIDE_BEHIND_S;
    applyArchives(probe, &archives);
    // With an archive, the past is watchable: keep as much of it as the guide
    // has, up to a day (two would double the guide's memory for little use).
    if (probe) for (int i = 0; i < probe->nCh; i++)
      if (probe->ch[i].catchup) { behind = IPTV_GUIDE_ARCHIVE_S; break; }
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
      n = (xml && probe) ? iptv_parse_xmltv(probe, xml, now - behind,
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
  free(text); free(cached); free(archives.a);
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
  prefBuffer = 0; prefPreview = 0;
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
    else if (!strcmp(line, "buffer"))  prefBuffer = atoi(v);
    else if (!strcmp(line, "preview")) prefPreview = atoi(v) != 0;
  }
  free(s);
  updateLabel();
}

static void writeConfig(void) {
  char buf[4096];
  snprintf(buf, sizeof buf, "kind=%s\nurl=%s\nepg=%s\nserver=%s\nuser=%s\npass=%s\n"
           "buffer=%d\npreview=%d\n",
           source.kind == IPTV_SRC_XTREAM ? "xtream" : source.kind == IPTV_SRC_M3U ? "m3u" : "none",
           source.url, source.epg, source.server, source.user, source.pass, prefBuffer, prefPreview);
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
int  iptv_pref_buffer(void) { return prefBuffer; }
int  iptv_pref_preview(void) { return prefPreview; }
void iptv_set_prefs(int bufferMinutes, int preview) {
  prefBuffer = bufferMinutes < 0 ? 0 : bufferMinutes > 240 ? 240 : bufferMinutes;
  prefPreview = preview != 0;
  writeConfig();
}

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
  // against the playlist's own address, which would be the relay's — so HLS
  // goes direct, and loses the headers, rather than losing every segment.
  if (c->headers[0] && !strstr(c->url, ".m3u8")) return proxy_wrap(c->url, c->headers, dst, size);
  return c->url;
}
