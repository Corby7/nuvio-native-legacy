#include "debrid.h"
#include "net.h"
#include "js.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>

// The three bases. The version is IN the base on purpose: TorBox wants /v1/
// before /api/ (api.torbox.app/v1/api/...), and leaving that to each route is
// the kind of detail that is got wrong once and paid for in a silent 404.
#define RD "https://api.real-debrid.com/rest/1.0"
#define TB "https://api.torbox.app/v1/api"
#define PM "https://www.premiumize.me/api"

enum { SRD, STB, SPM, SN };
static const char *serviceName[SN] = { "Real-Debrid", "TorBox", "Premiumize" };

// ONE KEY PER SERVICE, not one key. The account can carry more than one
// "debrid:*" credential (sync.c calls debrid_set_key once per provider), and
// keeping only the last would make the result depend on the ORDER the server
// returns the rows in — the worst kind of defect, because it changes by itself.
static char key[SN][200];

static int serviceId(const char *s) {
  if (!strcasecmp(s, "realdebrid") || !strcasecmp(s, "real-debrid")) return SRD;
  if (!strcasecmp(s, "torbox")     || !strcasecmp(s, "tor-box"))     return STB;
  if (!strcasecmp(s, "premiumize") || !strcasecmp(s, "premiumize-me")
      || !strcasecmp(s, "premiumizeme")) return SPM;
  return -1;
}

void debrid_set_key(const char *service, const char *k) {
  int q;
  if (!service || !k || !*k) return;
  q = serviceId(service);
  if (q < 0) {
    printf("[debrid] %s: no resolver for this service, ignored\n", service);
    return;
  }
  snprintf(key[q], sizeof key[q], "%s", k);
  printf("[debrid] %s key from the account\n", serviceName[q]);
}
int debrid_active(void) {
  int q;
  for (q = 0; q < SN; q++) if (key[q][0]) return 1;
  return 0;
}
void debrid_forget(void) { memset(key, 0, sizeof key); }

// ---------------------------------------------------------------- http

static void urlenc(char *dst, unsigned n, const char *s) {
  unsigned k = 0;
  for (; *s && k + 4 < n; s++) {
    unsigned char c = (unsigned char)*s;
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') dst[k++] = (char)c;
    else k += (unsigned)snprintf(dst + k, n - k, "%%%02X", c);
  }
  dst[k] = 0;
}

// All three take the key as "Authorization: Bearer" — what each one's docs
// recommend, Premiumize included: it still accepts ?apikey= for compatibility,
// and says the header exists precisely so the key stays out of server logs and
// Referers. The one exception is TorBox's requestdl, further down.
static char *getAuth(const char *base, const char *route, int which, int *st) {
  char url[900], auth[260];
  const char *hdr[2];
  snprintf(url, sizeof url, "%s/%s", base, route);
  snprintf(auth, sizeof auth, "Authorization: Bearer %s", key[which]);
  hdr[0] = auth; hdr[1] = NULL;
  return net_download_st(url, 15, hdr, st);
}
static char *postForm(const char *base, const char *route, int which,
                      const char *body, int *st) {
  char url[900], auth[260];
  const char *hdr[3];
  snprintf(url, sizeof url, "%s/%s", base, route);
  snprintf(auth, sizeof auth, "Authorization: Bearer %s", key[which]);
  hdr[0] = auth; hdr[1] = "Content-Type: application/x-www-form-urlencoded"; hdr[2] = NULL;
  return net_post_st(url, 15, hdr, body, st);
}
static int ok2xx(const char *r, int st) { return r && st >= 200 && st < 300; }

static void lower(char *s) { for (; *s; s++) *s = (char)tolower((unsigned char)*s); }

// ---------------------------------------------------------------- the file

static int isVideo(const char *name) {
  static const char *ext[] = { ".mp4", ".mkv", ".webm", ".avi", ".mov", ".m4v", ".ts", ".m2ts", ".wmv", NULL };
  size_t L = strlen(name); int i;
  for (i = 0; ext[i]; i++) {
    size_t e = strlen(ext[i]);
    if (L > e && !strcasecmp(name + L - e, ext[i])) return 1;
  }
  return 0;
}

// The web app's selectDebridFile order: SxxEyy pattern > fileIdx > largest
// video. Returns the chosen ELEMENT (a pointer to its '{', inside `files`) or
// NULL.
//
// WHY THE ELEMENT AND NOT AN id: the three services name their fields their own
// way — RD sends "path"/"bytes" and the file has an "id"; TorBox sends
// "name"/"size" with an "id"; Premiumize sends "path"/"size" and NO id at all,
// the direct link is inside the element. Returning the element lets each
// resolver take what its service uses, and the ORDER OF CHOICE — the one thing
// that must not differ between the three — is written once. The field names
// come in as parameters for the same reason.
static const char *pickFile(const char *files, int fileIdx, int season, int episode,
                            const char *kName, const char *kSize) {
  char pat1[16] = "", pat2[16] = "";
  const char *p, *best = NULL; int idx = 0; double bestSize = -1;
  if (season > 0 && episode > 0) {
    snprintf(pat1, sizeof pat1, "s%02de%02d", season, episode);
    snprintf(pat2, sizeof pat2, "%dx%02d", season, episode);
  }
  for (p = files; p && *p == '{'; p = js_next(js_end(p)), idx++) {
    const char *f = js_end(p);
    char name[600]; double size;
    if (!js_text(p, f, kName, name, sizeof name)) continue;
    size = js_num(p, f, kSize, 0);
    lower(name);
    if (!isVideo(name)) continue;
    if (pat1[0] && (strstr(name, pat1) || strstr(name, pat2))) return p;
    if (idx == fileIdx && fileIdx >= 0) { best = p; bestSize = 1e18; continue; }
    if (size > bestSize) { best = p; bestSize = size; }
  }
  return best;
}

// ---------------------------------------------------------------- Real-Debrid
//
// Routes (api.real-debrid.com/rest/1.0, the official "REST API" docs):
//   POST torrents/addMagnet, GET torrents/info/<id>,
//   POST torrents/selectFiles/<id>, POST unrestrict/link.

static int resolveRD(const char *infoHash, int fileIdx, int season, int episode,
                     char *url, unsigned n) {
  char body[700], enc[600], route[120], tid[64], status[32], link[600];
  char *r; int st = 0, id, attempt;
  const char *files, *links, *el;

  snprintf(body, sizeof body, "magnet:?xt=urn:btih:%s", infoHash);
  urlenc(enc, sizeof enc, body);
  snprintf(body, sizeof body, "magnet=%s", enc);
  r = postForm(RD, "torrents/addMagnet", SRD, body, &st);
  if (!ok2xx(r, st) || !js_text(r, NULL, "id", tid, sizeof tid)) {
    printf("[debrid] Real-Debrid addMagnet: HTTP %d\n", st); free(r); return 0;
  }
  free(r);

  snprintf(route, sizeof route, "torrents/info/%s", tid);
  r = getAuth(RD, route, SRD, &st);
  if (!ok2xx(r, st) || !(files = js_array(r, NULL, "files"))) {
    printf("[debrid] Real-Debrid info: HTTP %d\n", st); free(r); return 0;
  }
  el = pickFile(files, fileIdx, season, episode, "path", "bytes");
  id = el ? (int)js_num(el, js_end(el), "id", -1) : -1;
  free(r);
  if (id < 0) { printf("[debrid] Real-Debrid: no usable video in the torrent\n"); return 0; }

  snprintf(route, sizeof route, "torrents/selectFiles/%s", tid);
  snprintf(body, sizeof body, "files=%d", id);
  r = postForm(RD, route, SRD, body, &st);
  free(r);
  if (st < 200 || st >= 300) {
    printf("[debrid] Real-Debrid selectFiles: HTTP %d\n", st); return 0;
  }

  // Cached, RD reports "downloaded" almost at once; not cached, it would start
  // DOWNLOADING — and that is not "play now". Three looks and it gives up.
  link[0] = 0;
  for (attempt = 0; attempt < 3 && !link[0]; attempt++) {
    snprintf(route, sizeof route, "torrents/info/%s", tid);
    r = getAuth(RD, route, SRD, &st);
    if (ok2xx(r, st) && js_text(r, NULL, "status", status, sizeof status)
        && !strcmp(status, "downloaded") && (links = js_array(r, NULL, "links"))
        && *links == '"') {
      const char *end = strchr(links + 1, '"');
      if (end && (size_t)(end - links - 1) < sizeof link) {
        memcpy(link, links + 1, (size_t)(end - links - 1)); link[end - links - 1] = 0;
      }
    }
    free(r);
    if (!link[0]) sleep(1);
  }
  if (!link[0]) {
    // The torrent stays in the RD list. Deleting it would change what the owner
    // sees in their own account, which is their call, not a resolver's.
    printf("[debrid] %s is not cached on Real-Debrid\n", infoHash);
    return 0;
  }

  urlenc(enc, sizeof enc, link);
  snprintf(body, sizeof body, "link=%s", enc);
  r = postForm(RD, "unrestrict/link", SRD, body, &st);
  if (!ok2xx(r, st) || !js_text(r, NULL, "download", url, n)) {
    printf("[debrid] Real-Debrid unrestrict: HTTP %d\n", st); free(r); return 0;
  }
  free(r);
  return 1;
}

// ---------------------------------------------------------------- TorBox
//
// Routes (api.torbox.app/v1/api, the official docs and torbox-sdk-py):
//   GET  torrents/checkcached?hash=&format=list
//   POST torrents/createtorrent
//   GET  torrents/mylist?id=&bypass_cache=true
//   GET  torrents/requestdl?token=&torrent_id=&file_id=
//
// createtorrent goes as multipart/form-data, not urlencoded: that is what the
// official SDK sends, and the docs do not promise the other works. A guessed
// format would fail only in the homes of people who have TorBox.
#define BND "----nuvio-debrid"

static char *tbCreate(const char *magnet, int *st) {
  char auth[260], body[1200];
  const char *hdr[3];
  snprintf(auth, sizeof auth, "Authorization: Bearer %s", key[STB]);
  hdr[0] = auth;
  hdr[1] = "Content-Type: multipart/form-data; boundary=" BND;
  hdr[2] = NULL;
  // add_only_if_cached=true is what keeps the rule: the service REFUSES what is
  // not cached instead of starting a download. The cache check already
  // filtered, but the item can leave the cache between the two calls.
  snprintf(body, sizeof body,
           "--" BND "\r\nContent-Disposition: form-data; name=\"magnet\"\r\n\r\n%s\r\n"
           "--" BND "\r\nContent-Disposition: form-data; name=\"add_only_if_cached\"\r\n\r\ntrue\r\n"
           "--" BND "--\r\n", magnet);
  return net_post_st(TB "/torrents/createtorrent", 15, hdr, body, st);
}

static int resolveTB(const char *infoHash, int fileIdx, int season, int episode,
                     char *url, unsigned n) {
  char h[80], magnet[300], route[600];
  char *r; int st = 0, fid = -1, attempt, tid;
  const char *files, *el;

  // TorBox stores the hash in lower case and the cache check compares TEXT: a
  // hash the addon sent in capitals comes back "not cached" even when it is.
  snprintf(h, sizeof h, "%s", infoHash);
  lower(h);

  // 1) Only what is ALREADY CACHED plays now. `data` comes back as a list of
  //    {name,size,hash}; missing or empty means not cached, and then nothing
  //    is asked of the service.
  snprintf(route, sizeof route, "torrents/checkcached?hash=%s&format=list", h);
  r = getAuth(TB, route, STB, &st);
  if (!ok2xx(r, st) || !js_array(r, NULL, "data")) {
    printf("[debrid] TorBox: %s not cached (HTTP %d)\n", h, st);
    free(r); return 0;
  }
  free(r);

  snprintf(magnet, sizeof magnet, "magnet:?xt=urn:btih:%s", h);
  r = tbCreate(magnet, &st);
  tid = r ? (int)js_num(r, NULL, "torrent_id", -1) : -1;
  free(r);
  if (tid < 0) { printf("[debrid] TorBox createtorrent: HTTP %d\n", st); return 0; }

  // 3) The fields here are "name"/"size", not RD's "path"/"bytes". Three
  //    one-second looks, as with RD: cached, the list is ready at once.
  for (attempt = 0; attempt < 3 && fid < 0; attempt++) {
    snprintf(route, sizeof route, "torrents/mylist?id=%d&bypass_cache=true", tid);
    r = getAuth(TB, route, STB, &st);
    if (ok2xx(r, st) && (files = js_array(r, NULL, "files"))
        && (el = pickFile(files, fileIdx, season, episode, "name", "size")) != NULL)
      fid = (int)js_num(el, js_end(el), "id", -1);
    free(r);
    if (fid < 0) sleep(1);
  }
  if (fid < 0) { printf("[debrid] TorBox: no usable video in %s\n", h); return 0; }

  // 4) THE ONLY ROUTE THAT CARRIES THE KEY IN THE QUERY, and not by choice:
  //    request_download_link takes `token` as a query parameter. So this URL
  //    must never reach the log, whole or in part.
  { char u[1200]; const char *hdr[1];
    hdr[0] = NULL;
    snprintf(u, sizeof u, TB "/torrents/requestdl?token=%s&torrent_id=%d&file_id=%d&redirect=false",
             key[STB], tid, fid);
    r = net_download_st(u, 15, hdr, &st); }
  if (!ok2xx(r, st) || !js_text(r, NULL, "data", url, n)) {
    printf("[debrid] TorBox requestdl: HTTP %d\n", st); free(r); return 0;
  }
  free(r);
  return 1;
}

// ---------------------------------------------------------------- Premiumize
//
// Routes (www.premiumize.me/api, the official docs):
//   POST cache/check          items[]=<link>  -> {"status","response":[bool],...}
//   POST transfer/directdl    src=<link>      -> {"status","content":[{path,size,link}]}
//
// There is no "select the file" step as on RD: directdl already returns EVERY
// file of the torrent with its own direct link, and the choice is local.

static int resolvePM(const char *infoHash, int fileIdx, int season, int episode,
                     char *url, unsigned n) {
  char magnet[300], enc[500], body[600], raw[64];
  char *r; int st = 0, cached;
  const char *content, *el;

  snprintf(magnet, sizeof magnet, "magnet:?xt=urn:btih:%s", infoHash);
  urlenc(enc, sizeof enc, magnet);

  // The whole magnet as the item, not the bare hash: the docs call the
  // parameter "links to check", and directdl below takes the same text in
  // `src` — the same form in both keeps "is it cached" and "give me the link"
  // talking about the same thing.
  snprintf(body, sizeof body, "items%%5B%%5D=%s", enc);
  r = postForm(PM, "cache/check", SPM, body, &st);
  // "response" is an array of BOOLEANS, which js_array cannot open (it opens
  // arrays of objects or strings) — hence the raw read.
  cached = ok2xx(r, st) && js_raw(r, NULL, "response", raw, sizeof raw);
  if (cached) {
    const char *q = raw;
    if (*q == '[') q++;
    while (*q && (unsigned char)*q <= ' ') q++;
    cached = !strncmp(q, "true", 4);
  }
  free(r);
  if (!cached) {
    printf("[debrid] Premiumize: %s not cached (HTTP %d)\n", infoHash, st);
    return 0;
  }

  snprintf(body, sizeof body, "src=%s", enc);
  r = postForm(PM, "transfer/directdl", SPM, body, &st);
  if (!ok2xx(r, st) || !(content = js_array(r, NULL, "content"))) {
    printf("[debrid] Premiumize directdl: HTTP %d\n", st); free(r); return 0;
  }
  el = pickFile(content, fileIdx, season, episode, "path", "size");
  // "link" and not "stream_link": the second is a transcode, which does not
  // always exist and is not always the file that was asked for.
  if (!el || !js_text(el, js_end(el), "link", url, n)) {
    printf("[debrid] Premiumize: no usable video in %s\n", infoHash);
    free(r); return 0;
  }
  free(r);
  return 1;
}

// ---------------------------------------------------------------- resolve

int debrid_resolve(const char *infoHash, int fileIdx, int season, int episode,
                   char *url, unsigned n) {
  int q;
  if (!infoHash || !*infoHash || !url || n == 0) return 0;

  // FIXED ORDER: Real-Debrid, TorBox, Premiumize; the FIRST THAT RESOLVES wins,
  // not the first with a key. Someone with two accounts usually has a main one
  // and there is no way to know which from here — so the order is always the
  // same (predictable in the log), and each costs one cache check when it does
  // not have the content.
  for (q = 0; q < SN; q++) {
    int got;
    if (!key[q][0]) continue;
    url[0] = 0;
    got = (q == SRD) ? resolveRD(infoHash, fileIdx, season, episode, url, n)
        : (q == STB) ? resolveTB(infoHash, fileIdx, season, episode, url, n)
                     : resolvePM(infoHash, fileIdx, season, episode, url, n);
    if (got && url[0]) {
      // THE PATH OF THIS URL IS THE CREDENTIAL: whoever has it downloads on the
      // account of whoever asked. Only the origin goes to the log.
      char seg[120];
      printf("[debrid] %s: %s -> %s\n", serviceName[q], infoHash,
             net_url_public(url, seg, sizeof seg));
      return 1;
    }
    url[0] = 0;
  }
  return 0;
}
