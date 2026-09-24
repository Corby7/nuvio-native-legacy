// debrid.c and sourcepref.c in isolation: the network, the data folder, the
// profile and the stream list are stubbed, so the three services' flows and the
// remembered-source match run on canned answers, with no account and no TV.
//
//   bash tests/debrid_sources.sh
#include "debrid.h"
#include "sourcepref.h"
#include "streams.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- the network: the first route whose fragment is in the URL answers --------
typedef struct { const char *frag, *body; int status; } Route;
static const Route *routes;
static char lastPost[2048], lastType[128];
static int calls;

static char *answer(const char *url, int *st) {
  const Route *r;
  calls++;
  for (r = routes; r && r->frag; r++)
    if (strstr(url, r->frag)) { *st = r->status; return strdup(r->body); }
  *st = 404;
  return strdup("{}");
}
char *net_download_st(const char *url, int s, const char *const *h, int *st) {
  (void)s; (void)h; return answer(url, st);
}
char *net_post_st(const char *url, int s, const char *const *h, const char *body, int *st) {
  int k;
  (void)s;
  snprintf(lastPost, sizeof lastPost, "%s", body ? body : "");
  lastType[0] = 0;
  for (k = 0; h && h[k]; k++)
    if (!strncmp(h[k], "Content-Type:", 13)) snprintf(lastType, sizeof lastType, "%s", h[k]);
  return answer(url, st);
}
const char *net_url_public(const char *url, char *dst, unsigned n) {
  snprintf(dst, n, "%.*s", (int)(strstr(url, "://") ? strcspn(strstr(url, "://") + 3, "/") + (strstr(url, "://") + 3 - url) : strlen(url)), url);
  return dst;
}

// --- the data folder, in memory ------------------------------------------------
static char files[4][64], contents[4][16384];
int data_write(const char *name, const char *c) {
  int i;
  for (i = 0; i < 4; i++) if (!files[i][0] || !strcmp(files[i], name)) {
    snprintf(files[i], sizeof files[i], "%s", name);
    snprintf(contents[i], sizeof contents[i], "%s", c);
    return 1;
  }
  return 0;
}
char *data_read(const char *name) {
  int i;
  for (i = 0; i < 4; i++) if (!strcmp(files[i], name)) return strdup(contents[i]);
  return NULL;
}
int data_erase(const char *name) {
  int i;
  for (i = 0; i < 4; i++) if (!strcmp(files[i], name)) files[i][0] = 0;
  return 1;
}
static int profile = 1;
int profiles_active(void) { return profile; }

// --- the stream list -------------------------------------------------------------
static Stream list[8];
static int nList;
int stream_n(void) { return nList; }
const Stream *stream_item(int i) { return i >= 0 && i < nList ? &list[i] : NULL; }

static Stream row(const char *provider, const char *label, const char *desc,
                  const char *binge, int height) {
  Stream s;
  memset(&s, 0, sizeof s);
  snprintf(s.provider, sizeof s.provider, "%s", provider);
  snprintf(s.label, sizeof s.label, "%s", label);
  snprintf(s.description, sizeof s.description, "%s", desc);
  snprintf(s.bingeGroup, sizeof s.bingeGroup, "%s", binge);
  s.height = height;
  return s;
}

int main(void) {
  char url[4096];

  // ---- no key: nothing is asked of anyone ----
  calls = 0;
  assert(!debrid_active());
  assert(!debrid_resolve("ABCDEF", -1, 0, 0, url, sizeof url) && calls == 0);

  // ---- Real-Debrid: the season pack's S02E05 is picked over the bigger file ----
  { static const Route rd[] = {
      { "torrents/addMagnet", "{\"id\":\"T1\",\"uri\":\"x\"}", 201 },
      { "torrents/info/T1", "{\"status\":\"downloaded\",\"files\":["
          "{\"id\":1,\"path\":\"/Show.S02E04.mkv\",\"bytes\":900},"
          "{\"id\":2,\"path\":\"/Show.S02E05.mkv\",\"bytes\":500},"
          "{\"id\":3,\"path\":\"/sample.txt\",\"bytes\":9999}],"
          "\"links\":[\"https:\\/\\/real-debrid.com\\/d\\/SECRET\"]}", 200 },
      { "torrents/selectFiles/T1", "", 204 },
      { "unrestrict/link", "{\"download\":\"https:\\/\\/dl.real-debrid.com\\/d\\/SECRET\\/Show.S02E05.mkv\"}", 200 },
      { NULL, NULL, 0 } };
    routes = rd;
    debrid_set_key("realdebrid", "rdkey");
    assert(debrid_active());
    assert(debrid_resolve("ABCDEF", -1, 2, 5, url, sizeof url));
    assert(!strcmp(url, "https://dl.real-debrid.com/d/SECRET/Show.S02E05.mkv"));
    assert(strstr(lastType, "x-www-form-urlencoded"));
    printf("ok  Real-Debrid resolves the episode's file\n");
  }

  // ---- TorBox: lower-cased hash, multipart create, fileIdx wins with no SxxEyy ----
  debrid_forget();
  { static const Route tb[] = {
      { "checkcached?hash=abcdef", "{\"success\":true,\"data\":[{\"name\":\"x\",\"size\":1,\"hash\":\"abcdef\"}]}", 200 },
      { "createtorrent", "{\"success\":true,\"data\":{\"torrent_id\":77}}", 200 },
      { "mylist?id=77", "{\"data\":{\"files\":["
          "{\"id\":0,\"name\":\"Film.2160p.mkv\",\"size\":9000},"
          "{\"id\":1,\"name\":\"Film.1080p.mp4\",\"size\":3000}]}}", 200 },
      { "requestdl?token=tbkey&torrent_id=77&file_id=1", "{\"success\":true,\"data\":\"https:\\/\\/store.torbox.app\\/dl\\/1\"}", 200 },
      { NULL, NULL, 0 } };
    routes = tb;
    debrid_set_key("torbox", "tbkey");
    assert(debrid_resolve("ABCDEF", 1, 0, 0, url, sizeof url));
    assert(!strcmp(url, "https://store.torbox.app/dl/1"));
    assert(strstr(lastType, "multipart/form-data") && strstr(lastPost, "add_only_if_cached"));
    printf("ok  TorBox resolves through the cache check and fileIdx\n");
  }

  // ---- TorBox, not cached: stops after the check ----
  { static const Route tbMiss[] = {
      { "checkcached", "{\"success\":true,\"data\":[]}", 200 },
      { NULL, NULL, 0 } };
    routes = tbMiss; calls = 0;
    assert(!debrid_resolve("ABCDEF", -1, 0, 0, url, sizeof url) && calls == 1 && !url[0]);
    printf("ok  TorBox: not cached asks nothing more\n");
  }

  // ---- Premiumize: boolean cache answer, largest video from directdl ----
  debrid_forget();
  { static const Route pm[] = {
      { "cache/check", "{\"status\":\"success\",\"response\":[true],\"transcoded\":[false]}", 200 },
      { "transfer/directdl", "{\"status\":\"success\",\"content\":["
          "{\"path\":\"Film/extras.mkv\",\"size\":100,\"link\":\"https:\\/\\/pm\\/extras\"},"
          "{\"path\":\"Film/Film.mkv\",\"size\":8000,\"link\":\"https:\\/\\/pm\\/film\",\"stream_link\":\"https:\\/\\/pm\\/transcode\"}]}", 200 },
      { NULL, NULL, 0 } };
    routes = pm;
    debrid_set_key("premiumize", "pmkey");
    assert(debrid_resolve("ABCDEF", -1, 0, 0, url, sizeof url));
    assert(!strcmp(url, "https://pm/film"));
    printf("ok  Premiumize resolves the largest video\n");
  }
  { static const Route pmMiss[] = {
      { "cache/check", "{\"status\":\"success\",\"response\":[false]}", 200 },
      { NULL, NULL, 0 } };
    routes = pmMiss; calls = 0;
    assert(!debrid_resolve("ABCDEF", -1, 0, 0, url, sizeof url) && calls == 1);
    printf("ok  Premiumize: not cached asks nothing more\n");
  }

  // ---- order: Real-Debrid misses, TorBox then gets its turn ----
  debrid_forget();
  { static const Route both[] = {
      { "torrents/addMagnet", "{\"error\":\"x\"}", 503 },
      { "checkcached", "{\"data\":[{\"hash\":\"abcdef\"}]}", 200 },
      { "createtorrent", "{\"data\":{\"torrent_id\":5}}", 200 },
      { "mylist?id=5", "{\"data\":{\"files\":[{\"id\":9,\"name\":\"a.mkv\",\"size\":5}]}}", 200 },
      { "requestdl", "{\"data\":\"https:\\/\\/tb\\/a\"}", 200 },
      { NULL, NULL, 0 } };
    routes = both;
    debrid_set_key("real-debrid", "a");
    debrid_set_key("torbox", "b");
    debrid_set_key("alldebrid", "c");   // no resolver: ignored, not an error
    assert(debrid_resolve("ABCDEF", -1, 0, 0, url, sizeof url) && !strcmp(url, "https://tb/a"));
    printf("ok  the next service is tried when the first cannot resolve\n");
  }

  // ---- the audio signature ----
  { char a[64];
    Stream s = row("Torrentio", "Torrentio\n1080p", "Show.S01E01 \xF0\x9F\x87\xB3\xF0\x9F\x87\xB1 / \xF0\x9F\x87\xAC\xF0\x9F\x87\xA7 Dual Audio", "", 1080);
    sourcepref_audio(&s, a, sizeof a);
    assert(!strcmp(a, "DUAL+GB+NL"));
    s = row("Torrentio", "Dubai.Nights", "", "", 1080);   // "dub" inside a word
    sourcepref_audio(&s, a, sizeof a);
    assert(!a[0]);
    printf("ok  audio signature: flags and terms, sorted, whole words\n");
  }

  // ---- remembering a pick ----
  nList = 0;
  assert(sourcepref_pick("tt1") == -1);              // nothing stored: automatic
  list[0] = row("Torrentio", "Torrentio 2160p", "", "", 2160);
  list[1] = row("Torrentio", "Torrentio 1080p", "Show.S01E01 Dual Audio NL", "torrentio|1080p|dual", 1080);
  nList = 2;
  assert(sourcepref_store("tt1:1:1", &list[1]) == 1);
  assert(sourcepref_store("tt1:1:1", &list[1]) == 0);  // same pick: no rewrite

  // The next episode: the list comes in another order; bingeGroup finds it.
  list[0] = row("Torrentio", "Torrentio 1080p", "Show.S01E02 Dual Audio NL", "torrentio|1080p|dual", 1080);
  list[1] = row("Torrentio", "Torrentio 2160p", "", "", 2160);
  assert(sourcepref_pick("tt1:1:2") == 0);

  // No bingeGroup today: provider + audio, and a mark more is still a match.
  list[0] = row("Torrentio", "Torrentio 2160p", "", "", 2160);
  list[1] = row("Torrentio", "Torrentio 1080p", "Show.S01E03 Dual Audio NL Subbed", "", 1080);
  assert(sourcepref_pick("tt1") == 1);

  // A remembered mark is never dropped: only the original audio today -> automatic.
  list[1] = row("Torrentio", "Torrentio 1080p", "Show.S01E04", "", 1080);
  assert(sourcepref_pick("tt1") == -1);

  // Another provider with the same audio does not count.
  list[1] = row("Comet", "Comet 1080p", "Dual Audio NL", "", 1080);
  assert(sourcepref_pick("tt1") == -1);

  // Per profile: profile 2 has no preference for tt1.
  list[1] = row("Torrentio", "Torrentio 1080p", "Dual Audio NL", "", 1080);
  profile = 2;
  assert(sourcepref_pick("tt1") == -1);
  profile = 1;
  assert(sourcepref_pick("tt1") == 1);                 // read back from the file
  // HAS answers for the title, not for today's list: the router holds the early
  // start on it even when the remembered row has not landed yet.
  assert(sourcepref_has("tt1") && sourcepref_has("tt1:2:5") && !sourcepref_has("tt9"));

  sourcepref_forget();
  assert(sourcepref_pick("tt1") == -1);
  assert(!sourcepref_has("tt1"));
  printf("ok  source memory: bingeGroup, audio, profiles, sign-out\n");

  printf("all debrid/source tests passed\n");
  return 0;
}
