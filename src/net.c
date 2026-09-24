#include "net.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dlfcn.h>
#include <time.h>
#include "data.h"

// libcurl constants written out by hand: there is no curl.h in the device's
// SDK, and pulling in the whole header for half a dozen numbers does not pay
// for itself. The values have been stable forever (CURLOPTTYPE_OBJECTPOINT = 10000 etc).
#define OPT_URL             10002
#define OPT_WRITEFUNCTION   20011
#define OPT_WRITEDATA       10001
#define OPT_TIMEOUT            13
#define OPT_FOLLOWLOCATION     52
#define OPT_SSL_VERIFYPEER     64
#define OPT_SSL_VERIFYHOST     81
#define OPT_USERAGENT       10018
#define OPT_ACCEPT_ENCODING 10102
#define OPT_NOSIGNAL          99
#define OPT_HTTPHEADER      10023
#define OPT_NOBODY             44
#define OPT_RANGE           10007
#define INFO_URL_FINAL    1048577
#define OPT_POSTFIELDS      10015
#define OPT_POST               47
#define OPT_CUSTOMREQUEST   10036
// CURLOPT_CONNECTTIMEOUT. Separate from OPT_TIMEOUT because the two measure
// different things: a DEAD host spent the whole transfer budget on a connection
// that was never going to complete, and that was the same number giving a slow
// but LIVE server room to answer. With the connect ceiling on its own, an addon
// that is down fails in CONNECT_SECONDS and the slow one keeps its full 8 s.
#define OPT_CONNECTTIMEOUT     78
// For net_stream: the response headers go to a callback of their own, and a
// transfer that stops moving is cut by rate rather than by a total ceiling —
// a film is two hours long, and a total timeout would end it.
#define OPT_HEADERFUNCTION  20079
#define OPT_HEADERDATA      10029
#define OPT_LOW_SPEED_LIMIT    19
#define OPT_LOW_SPEED_TIME     20
// CURLINFO_RESPONSE_CODE = CURLINFO_LONG (0x200000) + 2.
#define INFO_RESPONSE_CODE   2097154
// For the timing log: CURLINFO_LONG + 26, and CURLINFO_DOUBLE (0x300000) + 3,
// 4, 5 and 33 — total, name lookup, connect and TLS done, each in seconds from
// the start of the transfer.
#define INFO_NUM_CONNECTS    2097178
#define INFO_TOTAL_TIME      3145731
#define INFO_NAMELOOKUP_TIME 3145732
#define INFO_CONNECT_TIME    3145733
#define INFO_APPCONNECT_TIME 3145761

// Seconds to ESTABLISH the connection (DNS + TCP + TLS), not the ceiling for
// the transfer. MEASURED on the Mac: a good connection completes the whole
// handshake in under 300 ms; 3 s covers the TV, which is slower, without coming
// near the 8 s a catalogue has to answer in full.
#define CONNECT_SECONDS 3L

static void *(*curl_init)(void);
static int   (*curl_setopt)(void *, int, ...);
static int   (*curl_perform)(void *);
static void  (*curl_cleanup)(void *);
static void  (*curl_reset)(void *);
static int   (*curl_global)(long);
static void *(*slist_append)(void *, const char *);
static void  (*slist_free)(void *);
static int   (*curl_getinfo)(void *, int, ...);
static void *(*share_init)(void);
static int   (*share_setopt)(void *, int, ...);
static int    ready;

// `cap`: optional byte ceiling for THIS transfer; 0 = no ceiling. It exists
// because a server that IGNORES the Range header answers 200 with the whole
// file, and in that case the header it was asked for limits nothing.
//
// It lives in the bucket and not in a global because `receive` is the receiver
// for EVERY transfer in the app, on any thread. While it was global, the 320 KB
// ceiling mkv.c uses to read a film's header also applied to whatever the four
// artwork threads were downloading at that instant — and a truncated image
// passes tex_cache's signature check (the magic bytes are intact) and goes to
// the disk cache, which has neither expiry nor eviction.
typedef struct { char *p; size_t n; long cap; } Bucket;

static char *net_download_internal(const char *url, int seconds, long *size,
                                 const char *const *header, long cap);
static char *net_download_internal2(const char *url, int seconds, long *size,
                                  const char *const *header, int *status,
                                  long cap);

static size_t receive(void *data, size_t size, size_t count, void *u) {
  Bucket *b = (Bucket *)u;
  size_t bytes = size * count;
  char *new;
  if (b->cap > 0 && b->n >= (size_t)b->cap) return 0;   // cuts the connection
  if (b->cap > 0 && b->n + bytes > (size_t)b->cap)
    bytes = (size_t)b->cap - b->n;
  new = realloc(b->p, b->n + bytes + 1);
  if (!new) return 0;              // returning 0 aborts the transfer
  b->p = new;
  memcpy(b->p + b->n, data, bytes);
  b->n += bytes;
  b->p[b->n] = 0;
  return bytes;
}

// --- A POOL OF HANDLES, LENT TO WHICHEVER THREAD ASKS -----------------------
//
// Every request used to open and close its own handle, and everything libcurl
// keeps INSIDE the handle went with it: the open connection, the DNS cache and
// the TLS session. Startup makes ~75 requests, nearly all against the same few
// hosts (v3-cinemeta.strem.io, api.trakt.tv, api.themoviedb.org, the addons),
// and each one paid for DNS, TCP and a handshake again.
//
// WHY A POOL AND NOT ONE HANDLE PER THREAD, which is what this was before.
// Almost every screen starts a thread of its own for its requests — detail
// extras, person, parental guide, director, the stream addons, subtitles,
// search — and a per-thread handle is born cold and dies with that thread.
// MEASURED on the TV (2026-09-24): one cold start opened 27 connections, 17 of
// them to cinemeta alone, and every title opened 8 more; a new connection costs
// 60-150 ms there against ~40 ms for a reused one. The pool outlives the threads,
// so the next thread borrows a handle whose connection is still open.
//
// WHY NOT CURL_LOCK_DATA_CONNECT, the share that looks made for this: libcurl
// documents that sharing CONNECTIONS between concurrent threads is unsupported.
// A handle, by contrast, may move between threads freely as long as only one
// uses it at a time — and a handle only ever sits with the thread that took it.
//
// The pool prefers a handle that last talked to the same host, because that is
// the one holding the open connection. Each handle keeps a few connections of
// its own (libcurl's default is 5), so a handle that served cinemeta and then
// trakt stays warm for both.
#define POOL_MAX   16
#define POOL_HOSTS  4
// What the pool knows about one handle, carried BY the handle (CURLOPT_PRIVATE)
// so it travels with it while a thread has it borrowed.
typedef struct { void *h; char host[POOL_HOSTS][80]; } Pooled;
static Pooled *idle[POOL_MAX];
static int nIdle;
static int pooled;
static pthread_mutex_t poolLock = PTHREAD_MUTEX_INITIALIZER;
#define OPT_PRIVATE  10103
#define INFO_PRIVATE 1048597

// THE PART THAT IS SAFE TO SHARE ACROSS THREADS: the DNS cache and the TLS
// sessions. A handle the pool cannot warm (none idle for that host) still skips
// the lookup and RESUMES the TLS session instead of a full handshake. The locks
// are per data kind, because libcurl takes the share's own lock while holding
// the DNS one.
#define OPT_SHARE             10100
#define OPT_DNS_CACHE_TIMEOUT    92
static void *share;
static pthread_mutex_t shareLocks[8];
static void shareLock(void *h, int data, int access, void *u) {
  (void)h; (void)access; (void)u;
  pthread_mutex_lock(&shareLocks[data & 7]);
}
static void shareUnlock(void *h, int data, void *u) {
  (void)h; (void)u;
  pthread_mutex_unlock(&shareLocks[data & 7]);
}

// "https://host:port/path" -> "host:port". Empty when there is no scheme.
static void hostOf(const char *url, char *dst, size_t size) {
  const char *p = url ? strstr(url, "://") : NULL;
  size_t n = 0;
  dst[0] = 0;
  if (!p) return;
  p += 3;
  while (p[n] && p[n] != '/' && p[n] != '?' && p[n] != '#') n++;
  if (n >= size) n = size - 1;
  memcpy(dst, p, n);
  dst[n] = 0;
}

// A handle ready to take the options, borrowed for `url`. `own` comes back 1
// when the handle is disposable and the caller has to close it (no pool); 0 when
// it goes back to the pool in handleGive.
static void *handleTake(const char *url, int *own) {
  char host[80];
  Pooled *p = NULL;
  int i, k, pick = -1;
  *own = 1;
  if (!pooled) return curl_init();
  hostOf(url, host, sizeof host);
  pthread_mutex_lock(&poolLock);
  // Newest first: the handle returned last is the likeliest to still hold a
  // live connection, the server not having timed it out yet.
  for (i = nIdle - 1; i >= 0 && pick < 0; i--)
    for (k = 0; k < POOL_HOSTS; k++)
      if (host[0] && !strcmp(idle[i]->host[k], host)) { pick = i; break; }
  // No handle knows this host: any idle one will do, since its DNS cache and TLS
  // sessions are shared anyway and it adds this host to its connections.
  if (pick < 0 && nIdle > 0) pick = nIdle - 1;
  if (pick >= 0) {
    p = idle[pick];
    memmove(&idle[pick], &idle[pick + 1], (nIdle - pick - 1) * sizeof idle[0]);
    nIdle--;
  }
  pthread_mutex_unlock(&poolLock);
  if (p) {
    // Clears the options and KEEPS the connections — which is exactly what we
    // are here to keep. Without this reset the handle would reach the next
    // request still carrying net_url_final's OPT_RANGE, or the headers of a
    // Trakt call.
    curl_reset(p->h);
  } else {
    p = calloc(1, sizeof *p);
    if (!p) return curl_init();
    p->h = curl_init();
    if (!p->h) { free(p); return NULL; }
  }
  *own = 0;
  // The reset cleared this along with everything else, so it goes back every time.
  curl_setopt(p->h, OPT_PRIVATE, p);
  if (share) curl_setopt(p->h, OPT_SHARE, share);
  // libcurl forgets a lookup after 60 s. The hosts here do not move in minutes,
  // and a connection to an address that did move fails and is opened again.
  curl_setopt(p->h, OPT_DNS_CACHE_TIMEOUT, (long)300);
  return p->h;
}

// Closes the request. `list` is the header list, which libcurl does NOT own: it
// only holds a pointer to it, so it has to be unhooked from the handle BEFORE
// being freed — the handle outlives the request, and the next curl_easy_reset
// would be the only thing standing between a dangling pointer and a request that
// reuses it.
static void handleGive(void *h, int own, void *list, const char *url) {
  Pooled *p = NULL, *evict = NULL;
  char host[80];
  int k;
  if (h && list && curl_setopt) curl_setopt(h, OPT_HTTPHEADER, (void *)0);
  if (list && slist_free) slist_free(list);
  if (!h) return;
  if (!own && curl_getinfo) curl_getinfo(h, INFO_PRIVATE, (char **)&p);
  if (own || !p) { if (curl_cleanup) curl_cleanup(h); return; }
  // This host first, the ones it already knew after: the handle keeps several
  // connections, and the oldest name is the one libcurl drops first.
  hostOf(url, host, sizeof host);
  for (k = 0; k < POOL_HOSTS && strcmp(p->host[k], host); k++) {}
  if (k == POOL_HOSTS) k = POOL_HOSTS - 1;
  memmove(p->host[1], p->host[0], k * sizeof p->host[0]);
  snprintf(p->host[0], sizeof p->host[0], "%s", host);
  pthread_mutex_lock(&poolLock);
  // Full: the oldest idle handle goes, its connections the likeliest dead.
  if (nIdle == POOL_MAX) {
    evict = idle[0];
    memmove(&idle[0], &idle[1], (POOL_MAX - 1) * sizeof idle[0]);
    nIdle--;
  }
  idle[nIdle++] = p;
  pthread_mutex_unlock(&poolLock);
  if (evict) { curl_cleanup(evict->h); free(evict); }
}

// LOADING LIBCURL, ONCE ONLY AND UNDER A LOCK.
//
// `curl_global_init` is NOT thread-safe — libcurl documents that itself. This
// used to be a plain flag, and while only discovery and two decode threads
// called it, the race almost never happened. Adding FOUR network threads for
// the artwork, all starting at boot, made it happen: two threads enter with
// `ready == 0`, both dlopen and both call curl_global_init at the same time.
// The global state is corrupted and EVERY download starts failing — catalogues,
// addons and artwork at once, which is exactly what the owner saw after the
// last deploy.
//
// The lock is static and without dynamic initialisation on purpose: it has to
// exist BEFORE the first thread, and a PTHREAD_MUTEX_INITIALIZER guarantees
// that without depending on anybody calling anything first.
static pthread_mutex_t openLock = PTHREAD_MUTEX_INITIALIZER;

// THE SINGLE 401 LISTENER — see net_notify_401 in net.h.
//
// A pointer and not a list: exactly one module cares (traktauth, through
// trakt.c), and a 401 can arrive on ANY call — continue watching, history,
// extras — so the hook belongs at the one place that sees every response code
// rather than at each caller.
static void (*notify401)(const char *url);
void net_notify_401(void (*f)(const char *url)) { notify401 = f; }

static int openHandle(void) {
  void *h;
  int r;
  // A quick lock-free read for the common case (already loaded). An int write
  // is atomic on the architectures this app runs on; what needs the lock is the
  // SEQUENCE dlopen+global_init, not the flag.
  if (ready) return ready > 0;
  pthread_mutex_lock(&openLock);
  if (ready) { r = ready > 0; pthread_mutex_unlock(&openLock); return r; }
  ready = -1;
  h = dlopen("libcurl.so.5", RTLD_NOW);
  if (!h) h = dlopen("libcurl.so.4", RTLD_NOW);
  if (!h) h = dlopen("libcurl.4.dylib", RTLD_NOW);   // Mac
  if (!h) h = dlopen("libcurl.dylib", RTLD_NOW);
  if (!h) { printf("[net] no libcurl: %s\n", dlerror());
            pthread_mutex_unlock(&openLock); return 0; }
  *(void **)(&curl_init)    = dlsym(h, "curl_easy_init");
  *(void **)(&curl_setopt)  = dlsym(h, "curl_easy_setopt");
  *(void **)(&curl_perform) = dlsym(h, "curl_easy_perform");
  *(void **)(&curl_cleanup) = dlsym(h, "curl_easy_cleanup");
  *(void **)(&curl_reset)   = dlsym(h, "curl_easy_reset");
  *(void **)(&curl_global)  = dlsym(h, "curl_global_init");
  *(void **)(&slist_append) = dlsym(h, "curl_slist_append");
  *(void **)(&slist_free)   = dlsym(h, "curl_slist_free_all");
  *(void **)(&curl_getinfo) = dlsym(h, "curl_easy_getinfo");
  *(void **)(&share_init)   = dlsym(h, "curl_share_init");
  *(void **)(&share_setopt) = dlsym(h, "curl_share_setopt");
  if (!curl_init || !curl_setopt || !curl_perform) {
    printf("[net] libcurl is missing the expected symbols\n");
    pthread_mutex_unlock(&openLock);
    return 0;
  }
  if (curl_global) curl_global(3 /* CURL_GLOBAL_DEFAULT */);
  // The pool depends on curl_easy_reset. Without it there is no way to return
  // a handle to a clean state between one request and the next, and reusing it
  // would carry the previous request's options along — a Trakt header on an
  // artwork request, one request's Range into the following one. In that case
  // the module goes back to a handle per request. Slower, and correct.
  if (curl_reset && curl_cleanup && curl_getinfo) pooled = 1;
  else printf("[net] no curl_easy_reset: one handle per request\n");
  // 1 = CURLSHOPT_SHARE, 3/4 = LOCKFUNC/UNLOCKFUNC; data 3 = DNS, 4 = SSL_SESSION.
  // Without the share every handle simply keeps its own caches.
  if (share_init && share_setopt && (share = share_init())) {
    int k;
    for (k = 0; k < 8; k++) pthread_mutex_init(&shareLocks[k], NULL);
    share_setopt(share, 3, shareLock);
    share_setopt(share, 4, shareUnlock);
    share_setopt(share, 1, 3);
    share_setopt(share, 1, 4);
  }
  ready = 1;
  pthread_mutex_unlock(&openLock);
  return 1;
}

void net_prepare(void) { openHandle(); }

// --- THE TIMING LOG ------------------------------------------------------------
//
// One line per request in data_dir()/net-timing.log, ONLY while a file named
// `net-timing` exists in that folder. It exists because "is the connection being
// reused" cannot be seen from outside: the app log sits in the TV's private /tmp,
// and the cost this module fights — DNS, TCP and TLS paid again — never shows up
// as an error, only as a slower screen. The data folder is readable over ssh.
//
// Line: start ms (monotonic, since the first request) | total ms | new
// connections opened (0 = reused one) | dns | tcp | tls ms | host.
//
// The flag is looked at on the first request after data_start has chosen a
// folder, never again: touching the file needs a relaunch.
static pthread_mutex_t timingLock = PTHREAD_MUTEX_INITIALIZER;
static FILE *timingFile;
static int timingChecked;
static double timingZero;

static double nowMs(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

// The start stamp of a request; 0 when the log is off, so a disabled log costs
// one unlocked read.
static double timingStart(void) {
  if (timingChecked && !timingFile) return 0;
  return nowMs();
}

static void timingEnd(void *c, double start, const char *url) {
  long fresh = 0;
  double total = 0, dns = 0, tcp = 0, tls = 0;
  char host[120];
  if (!start || !curl_getinfo) return;
  pthread_mutex_lock(&timingLock);
  if (!timingChecked && data_dir()[0]) {
    char flag[600], path[600];
    FILE *probe;
    timingChecked = 1;
    if (data_path(flag, sizeof flag, "net-timing") && (probe = fopen(flag, "r"))) {
      fclose(probe);
      if (data_path(path, sizeof path, "net-timing.log"))
        timingFile = fopen(path, "w");
      timingZero = start;
    }
  }
  if (timingFile) {
    curl_getinfo(c, INFO_NUM_CONNECTS, &fresh);
    curl_getinfo(c, INFO_TOTAL_TIME, &total);
    curl_getinfo(c, INFO_NAMELOOKUP_TIME, &dns);
    curl_getinfo(c, INFO_CONNECT_TIME, &tcp);
    curl_getinfo(c, INFO_APPCONNECT_TIME, &tls);
    fprintf(timingFile, "%.0f %.0f %ld %.0f %.0f %.0f %s\n", start - timingZero,
            total * 1000, fresh, dns * 1000, tcp * 1000, tls * 1000,
            net_url_public(url, host, sizeof host));
    fflush(timingFile);
  }
  pthread_mutex_unlock(&timingLock);
}

char *net_download_bin(const char *url, int seconds, long *size) {
  return net_download_internal(url, seconds, size, NULL, 0);
}

char *net_download(const char *url, int seconds) {
  return net_download_internal(url, seconds, NULL, NULL, 0);
}

char *net_download_chunk(const char *url, int seconds, long start, long end,
                         long *size) {
  char track[80];
  const char *header[2];
  // Range is an ordinary header, so the existing with-headers path serves.
  // There is no separate "binary with headers" mode because
  // net_download_internal already returns the size when `size` is passed — it
  // is whoever asked for text that ignores that field.
  snprintf(track, sizeof track, "Range: bytes=%ld-%ld", start, end);
  header[0] = track; header[1] = NULL;
  // A REAL CEILING, and not just the header. MEASURED: a server that ignores
  // Range answers 200 with the WHOLE file — in the test 31 MB came back for a
  // 2 MB request. Without the ceiling, reading the header of a 20 GB film would
  // download the film. The cut is in the receiver, so the connection dies at
  // the limit instead of waiting for the end.
  return net_download_internal(url, seconds, size, header, end - start + 1);
}

char *net_download_headers(const char *url, int seconds, const char *const *header) {
  return net_download_internal(url, seconds, NULL, header, 0);
}

char *net_download_st(const char *url, int seconds, const char *const *header,
                     int *status) {
  return net_download_internal2(url, seconds, NULL, header, status, 0);
}

static char *net_download_internal(const char *url, int seconds, long *size,
                                 const char *const *header, long cap) {
  return net_download_internal2(url, seconds, size, header, NULL, cap);
}

// Whether this thread's last download ran out of time — see net_timed_out.
static __thread int timedOut;
int net_timed_out(void) { return timedOut; }

static char *net_download_internal2(const char *url, int seconds, long *size,
                                  const char *const *header, int *status,
                                  long cap) {
  Bucket b = { NULL, 0, 0 };
  void *c, *list = NULL;
  int r, own;
  double t0;
  if (status) *status = 0;
  timedOut = 0;
  if (!url || !*url || !openHandle()) return NULL;
  b.cap = cap;
  c = handleTake(url, &own);
  if (!c) return NULL;
  curl_setopt(c, OPT_URL, url);
  curl_setopt(c, OPT_WRITEFUNCTION, receive);
  curl_setopt(c, OPT_WRITEDATA, &b);
  curl_setopt(c, OPT_FOLLOWLOCATION, (long)1);
  curl_setopt(c, OPT_TIMEOUT, (long)(seconds > 0 ? seconds : 30));
  curl_setopt(c, OPT_CONNECTTIMEOUT, CONNECT_SECONDS);
  // The app runs with threads; without NOSIGNAL libcurl uses alarms for the DNS
  // timeout and can bring the whole process down from a secondary thread.
  curl_setopt(c, OPT_NOSIGNAL, (long)1);
  // The addons are served by hosts with chains this 2019 device does not know;
  // its CA bundle is factory-fitted and does not update. Verifying would refuse
  // the owner's legitimate sources. The content is public media and the choice
  // is written down here deliberately.
  curl_setopt(c, OPT_SSL_VERIFYPEER, (long)0);
  curl_setopt(c, OPT_SSL_VERIFYHOST, (long)0);
  curl_setopt(c, OPT_USERAGENT, "Nuvio/1.0 (webOS)");
  curl_setopt(c, OPT_ACCEPT_ENCODING, "");   // "" = all the ones the lib supports
  if (header && slist_append) {
    int k;
    for (k = 0; header[k]; k++) list = slist_append(list, header[k]);
    if (list) curl_setopt(c, OPT_HTTPHEADER, list);
  }
  t0 = timingStart();
  r = curl_perform(c);
  timingEnd(c, t0, url);
  timedOut = r == 28;   // CURLE_OPERATION_TIMEDOUT
  // THE HTTP STATUS, and not just libcurl's error code. MEASURED: on one pass
  // through the home the log had 93 "decode failed" and ZERO "[net] failure" —
  // that is, curl_easy_perform returned 0 (TRANSPORT success) for responses
  // that were not the image. A 404, a 403 or a 429 is a successful transfer as
  // far as libcurl is concerned; it is the caller who has to look at the status.
  //
  // Without this the error reached tex_cache nameless, which only saw "short
  // body" and returned 0 silently — and the only symptom was a card with no
  // art. The image signature that already lives there catches the 404 with a
  // LARGE error page; this check catches the rest, and says WHICH code it was.
  //
  // THE EXCEPTION is anyone who asked for `status`: for Supabase, a 4xx is not
  // a failure, it is the answer. The 404's body says WHICH function or table
  // does not exist (PGRST202 / PGRST205), and it is that string that separates
  // "old server" from "wrong parameter". Throwing the body away here would erase the only clue.
  { long http = 0;
    if (curl_getinfo) curl_getinfo(c, INFO_RESPONSE_CODE, &http);
    if (status) *status = (int)http;
    // Told BEFORE the 4xx-to-NULL decision below, and regardless of whether the
    // caller asked for the status: a credential that has been refused is refused
    // whoever happened to make the call.
    if (http == 401 && notify401) notify401(url);
    if (!r && http >= 400 && !status) {
      handleGive(c, own, list, url);
      free(b.p);
      // REDACTED, and not the 60 characters that used to be here: those were
      // enough to include the path segment that carries a debrid key or a JWT.
      // See net_url_public in net.h.
      { char safe[120];
        printf("[net] HTTP %ld on %s\n", http, net_url_public(url, safe, sizeof safe)); }
      fflush(stdout);
      return NULL;
    } }
  handleGive(c, own, list, url);
  // 23 = CURLE_WRITE_ERROR. When there is a ceiling, it is the EXPECTED result:
  // the receiver returns fewer bytes on purpose to cut the connection as soon
  // as it fills. In that case what has arrived is exactly what was wanted —
  // treating it as a failure would throw away the whole header we just downloaded.
  if (r == 23 && b.cap > 0 && b.n > 0) r = 0;
  if (r != 0) { char safe[120]; free(b.p);
    printf("[net] failure %d on %s\n", r, net_url_public(url, safe, sizeof safe));
    return NULL; }
  if (size) *size = (long)b.n;
  return b.p;
}

int net_url_final(const char *url, int seconds, char *dst, unsigned size) {
  Bucket b = { NULL, 0, 0 };
  void *c;
  char *end = NULL;
  int r, own;
  double t0;
  if (!url || !*url || !openHandle() || !curl_getinfo) return 0;
  c = handleTake(url, &own);
  if (!c) return 0;
  curl_setopt(c, OPT_URL, url);
  curl_setopt(c, OPT_WRITEFUNCTION, receive);
  curl_setopt(c, OPT_WRITEDATA, &b);
  curl_setopt(c, OPT_FOLLOWLOCATION, (long)1);
  curl_setopt(c, OPT_TIMEOUT, (long)(seconds > 0 ? seconds : 20));
  curl_setopt(c, OPT_CONNECTTIMEOUT, CONNECT_SECONDS);
  curl_setopt(c, OPT_NOSIGNAL, (long)1);
  curl_setopt(c, OPT_SSL_VERIFYPEER, (long)0);
  curl_setopt(c, OPT_SSL_VERIFYHOST, (long)0);
  curl_setopt(c, OPT_USERAGENT, "Nuvio/1.0 (webOS)");
  // A tiny piece instead of a HEAD: several debrid servers answer HEAD with 405
  // or lie in the redirect, but do honour Range.
  curl_setopt(c, OPT_RANGE, "0-64");
  t0 = timingStart();
  r = curl_perform(c);
  timingEnd(c, t0, url);
  if (!r) curl_getinfo(c, INFO_URL_FINAL, &end);
  // The address comes out of the handle, so it has to be COPIED before the
  // handle is given back: on a reused handle the next curl_easy_reset frees it.
  if (!r && end) snprintf(dst, size, "%s", end);
  handleGive(c, own, NULL, url);
  free(b.p);
  return (!r && end) ? 1 : 0;
}

int net_stream(const char *url, const char *const *header, int headOnly,
               size_t (*onHeader)(char *, size_t, size_t, void *),
               size_t (*onBody)(char *, size_t, size_t, void *), void *u,
               long *status) {
  void *c, *list = NULL;
  int r, own;
  if (status) *status = 0;
  if (!url || !*url || !openHandle()) return -1;
  c = handleTake(url, &own);
  if (!c) return -1;
  curl_setopt(c, OPT_URL, url);
  curl_setopt(c, OPT_HEADERFUNCTION, onHeader);
  curl_setopt(c, OPT_HEADERDATA, u);
  curl_setopt(c, OPT_WRITEFUNCTION, onBody);
  curl_setopt(c, OPT_WRITEDATA, u);
  curl_setopt(c, OPT_FOLLOWLOCATION, (long)1);
  curl_setopt(c, OPT_CONNECTTIMEOUT, (long)10);
  // No OPT_TIMEOUT: see OPT_LOW_SPEED_TIME above. A minute under 1 byte/s is a
  // dead upstream — or a player paused long enough that its socket stopped
  // draining, and then the pipeline reconnects with a Range on resume anyway.
  curl_setopt(c, OPT_LOW_SPEED_LIMIT, (long)1);
  curl_setopt(c, OPT_LOW_SPEED_TIME, (long)60);
  curl_setopt(c, OPT_NOSIGNAL, (long)1);
  curl_setopt(c, OPT_SSL_VERIFYPEER, (long)0);
  curl_setopt(c, OPT_SSL_VERIFYHOST, (long)0);
  curl_setopt(c, OPT_USERAGENT, "Nuvio/1.0 (webOS)");
  // NO OPT_ACCEPT_ENCODING, unlike every other call here: the bytes are relayed
  // as they are, next to the upstream's Content-Length and Content-Range, and a
  // body libcurl had decompressed would no longer match either.
  if (headOnly) curl_setopt(c, OPT_NOBODY, (long)1);
  if (header && slist_append) {
    int k;
    for (k = 0; header[k]; k++) list = slist_append(list, header[k]);
    if (list) curl_setopt(c, OPT_HTTPHEADER, list);
  }
  r = curl_perform(c);
  if (status && curl_getinfo) curl_getinfo(c, INFO_RESPONSE_CODE, status);
  handleGive(c, own, list, url);
  return r;
}

char *net_post(const char *url, int seconds, const char *const *header,
                  const char *body) {
  return net_post_st(url, seconds, header, body, NULL);
}

// A POST, or — with `method` — the same request under another verb (DELETE). The
// handle is reset before it is lent again (handleTake), so the verb set here
// cannot carry over into the next request that borrows it.
static char *sendBody(const char *method, const char *url, int seconds,
                      const char *const *header, const char *body, int *status);

char *net_post_st(const char *url, int seconds, const char *const *header,
                     const char *body, int *status) {
  return sendBody(NULL, url, seconds, header, body, status);
}

char *net_delete_st(const char *url, int seconds, const char *const *header,
                    int *status) {
  return sendBody("DELETE", url, seconds, header, NULL, status);
}

static char *sendBody(const char *method, const char *url, int seconds,
                      const char *const *header, const char *body, int *status) {
  Bucket b = { NULL, 0, 0 };
  void *c, *list = NULL;
  int r, own;
  double t0;
  if (status) *status = 0;
  if (!url || !*url || !openHandle()) return NULL;
  c = handleTake(url, &own);
  if (!c) return NULL;
  curl_setopt(c, OPT_URL, url);
  curl_setopt(c, OPT_WRITEFUNCTION, receive);
  curl_setopt(c, OPT_WRITEDATA, &b);
  curl_setopt(c, OPT_TIMEOUT, (long)(seconds > 0 ? seconds : 20));
  curl_setopt(c, OPT_CONNECTTIMEOUT, CONNECT_SECONDS);
  curl_setopt(c, OPT_NOSIGNAL, (long)1);
  curl_setopt(c, OPT_SSL_VERIFYPEER, (long)0);
  curl_setopt(c, OPT_SSL_VERIFYHOST, (long)0);
  curl_setopt(c, OPT_USERAGENT, "Nuvio/1.0 (webOS)");
  if (method) {
    curl_setopt(c, OPT_CUSTOMREQUEST, method);
  } else {
    curl_setopt(c, OPT_POST, (long)1);
    curl_setopt(c, OPT_POSTFIELDS, body ? body : "");
  }
  if (slist_append) {
    int k, typed = 0;
    // JSON unless the caller says otherwise: the debrid APIs take form and
    // multipart bodies, and a second Content-Type would go out next to theirs.
    for (k = 0; header && header[k]; k++)
      if (!strncasecmp(header[k], "Content-Type:", 13)) typed = 1;
    if (!typed) list = slist_append(list, "Content-Type: application/json");
    for (k = 0; header && header[k]; k++) list = slist_append(list, header[k]);
    if (list) curl_setopt(c, OPT_HTTPHEADER, list);
  }
  t0 = timingStart();
  r = curl_perform(c);
  timingEnd(c, t0, url);
  // The code comes out BEFORE the cleanup: after it the handle no longer exists.
  // It is read whether or not the caller wanted it, because the 401 listener has
  // to be told either way — see net_notify_401.
  if (!r && curl_getinfo) {
    long code = 0;
    curl_getinfo(c, INFO_RESPONSE_CODE, &code);
    if (status) *status = (int)code;
    if (code == 401 && notify401) notify401(url);
  }
  handleGive(c, own, list, url);
  // A TRANSPORT failure (r != 0) is still NULL — there was no response at all.
  // The body of a 4xx, by contrast, IS returned: it is where PostgREST explains
  // what was missing.
  if (r != 0) { free(b.p); return NULL; }
  return b.p ? b.p : strdup("");
}
