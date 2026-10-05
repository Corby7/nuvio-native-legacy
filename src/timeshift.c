#include "timeshift.h"
#include "data.h"
#include "net.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define TS_FILE        "timeshift.ts"
#define TS_PACKET      188
#define TS_INDEX       16384     // arrival samples; at two a second, over two hours
#define TS_SAMPLE_S    0.5
#define TS_CHUNK       (64 * 1024)
#define TS_RETRIES     5         // reconnects in a row that brought nothing
#define TS_ASSUMED_BPS (1250000LL)   // 10 Mbit/s: an HD channel, generously
#define TS_MIN_BUDGET  (48LL * 1024 * 1024)   // below this, a budget is not worth it
#define TS_MIN_RING    (4LL * 1024 * 1024)    // what timeshift_begin accepts (tests wrap it)
#define TS_PCR_WRAP    (1LL << 33)
#define TS_REJOIN_BACK (120LL * 90000)        // a burst goes back less than this
#define TS_REJOIN_BLIND (4LL * 1024 * 1024)   // no PCR on its PID this long: give up looking

typedef struct { double t; long long b; } Sample;

// Everything below is under `mu`.
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t grew = PTHREAD_COND_INITIALIZER;
static unsigned session;          // bumped by every begin and end
static int state = TS_OFF;
static int fd = -1;
static long long cap, written, syncAt = -1;
static Sample idx[TS_INDEX];
static int idxHead, idxN;         // a ring: idx[(idxHead + i) % TS_INDEX], oldest first

// REJOINING AFTER A RECONNECT. An Xtream server opens every connection with a
// burst of what it already sent, 20-45 s of it measured, so a reconnect that
// simply appended wrote that stretch twice and the picture played it again.
// The recorder keeps the newest PCR it wrote; after a reconnect it drops whole
// packets until the stream's clock is past it. A clock that went far back (a
// real restart of the channel) is taken as it is, and so is a stream whose PCR
// cannot be found again.
static int pcrPid = -1;
static long long lastPcr = -1;    // 90 kHz, 33 bits
static int rejoin, rejoinSynced;
static long long rejoinBlind;     // bytes skipped since the PCR PID was last seen
static unsigned char carry[TS_PACKET];
static int carryN;

static int port;                  // 0 = not started, -1 = failed
static int listener = -1;

static double wallNow(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec + tv.tv_usec / 1e6;
}

// --- The index (under mu) ------------------------------------------------------------
static const Sample *sampleAt(int i) { return &idx[(idxHead + i) % TS_INDEX]; }

static void samplePush(double t, long long b) {
  if (idxN && t - sampleAt(idxN - 1)->t < TS_SAMPLE_S) {
    idx[(idxHead + idxN - 1) % TS_INDEX].b = b;   // the latest sample moves along
    return;
  }
  if (idxN == TS_INDEX) { idxHead = (idxHead + 1) % TS_INDEX; idxN--; }
  idx[(idxHead + idxN) % TS_INDEX] = (Sample){ t, b };
  idxN++;
}

static long long oldestByte(void) {
  // A margin of 1/32 of the ring behind the writer is never served: the writer
  // could overwrite it while a reader is copying it out.
  long long o = written - cap + cap / 32;
  return o > (syncAt > 0 ? syncAt : 0) ? o : (syncAt > 0 ? syncAt : 0);
}

static long long alignPacket(long long b) {
  if (syncAt < 0 || b <= syncAt) return syncAt < 0 ? 0 : syncAt;
  return b - (b - syncAt) % TS_PACKET;
}

// The byte that arrived at wall time `t`.
static long long byteAt(double t) {
  long long b = oldestByte();
  for (int i = idxN - 1; i >= 0; i--)
    if (sampleAt(i)->t <= t) { b = sampleAt(i)->b; break; }
  if (b < oldestByte()) b = oldestByte();
  if (b > written) b = written;
  return alignPacket(b);
}

// When byte `b` arrived.
static double timeAt(long long b) {
  for (int i = 0; i < idxN; i++)
    if (sampleAt(i)->b >= b) return sampleAt(i)->t;
  return idxN ? sampleAt(idxN - 1)->t : 0;
}

// --- The recorder ----------------------------------------------------------------------
typedef struct {
  unsigned session;
  char url[2048];
  char headers[1024];
  int code, isHls, got;
  char type[96];
} Rec;

static size_t onHeader(char *p, size_t size, size_t count, void *u) {
  Rec *r = u;
  size_t n = size * count;
  if (n >= 5 && !strncmp(p, "HTTP/", 5)) {
    const char *sp = memchr(p, ' ', n);
    r->code = sp ? atoi(sp + 1) : 0;
    r->type[0] = 0;
  } else if (n > 13 && !strncasecmp(p, "content-type:", 13)) {
    size_t k = n - 13;
    if (k >= sizeof r->type) k = sizeof r->type - 1;
    memcpy(r->type, p + 13, k); r->type[k] = 0;
  }
  return n;
}

// Case-blind: "application/vnd.apple.mpegurl", "audio/x-mpegURL"… (strcasestr is
// a GNU extension the device's glibc hides without _GNU_SOURCE.)
static int typeIsHls(const char *t) {
  for (; *t; t++) if (!strncasecmp(t, "mpegurl", 7)) return 1;
  return 0;
}

static int looksHls(const Rec *r, const char *p, size_t n) {
  if (typeIsHls(r->type)) return 1;
  while (n && (*p == ' ' || *p == '\r' || *p == '\n' || (unsigned char)*p == 0xEF ||
               (unsigned char)*p == 0xBB || (unsigned char)*p == 0xBF)) { p++; n--; }
  return n >= 7 && !strncmp(p, "#EXTM3U", 7);
}

// --- PCR (under mu) ---------------------------------------------------------------------
// The packet's PCR when it carries one on the PCR PID (the first PID seen with
// one becomes it); -1 otherwise. Needs the packet's first 12 bytes.
static long long packetPcr(const unsigned char *p) {
  int pid = ((p[1] & 0x1f) << 8) | p[2];
  if (p[0] != 0x47 || !((p[3] >> 4) & 2) || p[4] < 7 || !(p[5] & 0x10)) return -1;
  if (pcrPid < 0) pcrPid = pid;
  if (pid != pcrPid) return -1;
  return (long long)p[6] << 25 | (long long)p[7] << 17 | (long long)p[8] << 9 |
         (long long)p[9] << 1 | p[10] >> 7;
}

// Notes the PCRs of `n` bytes just written at stream byte `at`.
static void notePcrs(const unsigned char *p, size_t n, long long at) {
  long long k;
  if (syncAt < 0) return;
  k = at < syncAt ? syncAt - at : (TS_PACKET - (at - syncAt) % TS_PACKET) % TS_PACKET;
  for (; k + 12 <= (long long)n; k += TS_PACKET) {
    long long v = packetPcr(p + k);
    if (v >= 0) lastPcr = v;
  }
}

// Writes `n` bytes at the ring's head. 0 when the disk refused.
static int ringWrite(const unsigned char *p, size_t n) {
  size_t done = 0;
  while (done < n) {
    long long at = written % cap;
    size_t k = n - done;
    ssize_t w;
    if ((long long)k > cap - at) k = (size_t)(cap - at);
    w = pwrite(fd, p + done, k, (off_t)at);
    if (w <= 0) return 0;
    done += (size_t)w;
    written += w;
  }
  return 1;
}

// After a reconnect: is this packet past what the ring already has?
static int rejoinsAt(const unsigned char *pk) {
  long long v = packetPcr(pk), d;
  if (lastPcr < 0) return 1;
  if (v < 0) {
    rejoinBlind += TS_PACKET;
    return rejoinBlind > TS_REJOIN_BLIND;
  }
  rejoinBlind = 0;
  d = ((v - lastPcr) % TS_PCR_WRAP + TS_PCR_WRAP) % TS_PCR_WRAP;
  if (d >= TS_PCR_WRAP / 2) d -= TS_PCR_WRAP;
  return d > 0 || d <= -TS_REJOIN_BACK;
}

// Drops the replayed burst of a new connection, a packet at a time; returns the
// offset in `p` where writing goes on (n when all of it is dropped). A packet
// split across two calls waits in `carry`; when that packet is the one that
// rejoins, it is written here.
static size_t rejoinSkip(const unsigned char *p, size_t n, int *fail) {
  size_t k = 0;
  if (!rejoinSynced) {
    for (; k < n; k++)
      if (p[k] == 0x47 && (k + TS_PACKET >= n || p[k + TS_PACKET] == 0x47)) break;
    if (k == n) return n;
    rejoinSynced = 1;
  }
  if (carryN) {
    size_t need = TS_PACKET - (size_t)carryN;
    if (n - k < need) { memcpy(carry + carryN, p + k, n - k); carryN += (int)(n - k); return n; }
    memcpy(carry + carryN, p + k, need);
    k += need; carryN = 0;
    if (rejoinsAt(carry)) {
      rejoin = 0;
      if (!ringWrite(carry, TS_PACKET)) { *fail = 1; return n; }
      notePcrs(carry, TS_PACKET, written - TS_PACKET);
      return k;
    }
  }
  for (; k + TS_PACKET <= n; k += TS_PACKET)
    if (rejoinsAt(p + k)) { rejoin = 0; return k; }
  memcpy(carry, p + k, n - k);
  carryN = (int)(n - k);
  return n;
}

static size_t onBody(char *p, size_t size, size_t count, void *u) {
  Rec *r = u;
  size_t n = size * count, from = 0;
  pthread_mutex_lock(&mu);
  if (r->session != session) { pthread_mutex_unlock(&mu); return 0; }
  if (r->code >= 400) { pthread_mutex_unlock(&mu); return 0; }
  if (syncAt < 0) {
    // The first bytes decide: an HLS playlist is refused, and MPEG-TS starts
    // at its first sync byte whose next packet also starts with one.
    if (!written && looksHls(r, p, n)) {
      r->isHls = 1; state = TS_REFUSED;
      pthread_mutex_unlock(&mu);
      return 0;
    }
    for (size_t k = 0; k < n; k++)
      if (p[k] == 0x47 && (k + TS_PACKET >= n || p[k + TS_PACKET] == 0x47)) {
        syncAt = written + (long long)k; break;
      }
    if (syncAt < 0 && written + (long long)n > 64 * TS_PACKET) {
      // Nothing like MPEG-TS in the first twelve kilobytes.
      state = TS_REFUSED;
      pthread_mutex_unlock(&mu);
      return 0;
    }
  }
  r->got = 1;
  if (rejoin) {
    int fail = 0;
    from = rejoinSkip((const unsigned char *)p, n, &fail);
    if (fail) { state = TS_FAILED; pthread_mutex_unlock(&mu); return 0; }
    if (!rejoin) {
      printf("[timeshift] rejoined past the replayed burst\n");
      fflush(stdout);
    }
  }
  if (from < n) {
    long long at = written;
    if (!ringWrite((const unsigned char *)p + from, n - from)) {
      state = TS_FAILED; pthread_mutex_unlock(&mu); return 0;
    }
    notePcrs((const unsigned char *)p + from, n - from, at);
  }
  if (syncAt >= 0) state = TS_RECORDING;
  samplePush(wallNow(), written);
  pthread_cond_broadcast(&grew);
  pthread_mutex_unlock(&mu);
  return n;
}

static void *record(void *u) {
  Rec *r = u;
  const char *lines[25];
  char hdrs[1024];
  int nLines = 0, fails = 0;
  snprintf(hdrs, sizeof hdrs, "%s", r->headers);
  { char *save = NULL, *line = strtok_r(hdrs, "\n", &save);
    for (; line && nLines < 24; line = strtok_r(NULL, "\n", &save)) lines[nLines++] = line; }
  lines[nLines] = NULL;
  for (;;) {
    long status = 0;
    int live;
    r->got = 0; r->code = 0;
    net_stream(r->url, lines, 0, onHeader, onBody, r, &status);
    pthread_mutex_lock(&mu);
    live = r->session == session && state != TS_REFUSED && state != TS_FAILED;
    if (live) {
      fails = r->got ? 0 : fails + 1;
      if (fails >= TS_RETRIES) {
        state = TS_FAILED;
        pthread_cond_broadcast(&grew);
        live = 0;
      } else if (syncAt >= 0) {
        // The connection may have ended inside a packet: padded to the packet's
        // end, the ring stays in whole packets, and the next connection's are
        // written where a reader expects one to start.
        long long part = (written - syncAt) % TS_PACKET;
        if (part) {
          unsigned char pad[TS_PACKET];
          memset(pad, 0xFF, sizeof pad);
          if (!ringWrite(pad, (size_t)(TS_PACKET - part))) { state = TS_FAILED; live = 0; }
        }
        rejoin = 1; rejoinSynced = 0; rejoinBlind = 0; carryN = 0;
      }
    }
    pthread_mutex_unlock(&mu);
    if (!live) break;
    // A live TS connection that ends is a provider hiccup: join it again. The
    // ring takes the gap in its stride; the demuxer resyncs on the next packet.
    printf("[timeshift] upstream ended (HTTP %ld), reconnecting\n", status);
    fflush(stdout);
    { struct timespec pause = { fails ? 1 : 0, fails ? 0 : 200000000L };
      nanosleep(&pause, NULL); }
  }
  printf("[timeshift] recorder %u done%s\n", r->session, r->isHls ? " (HLS: not recorded)" : "");
  fflush(stdout);
  free(r);
  return NULL;
}

// --- The loopback server ---------------------------------------------------------------
static int sendAll(int s, const void *p, size_t n) {
  const char *c = p;
  while (n > 0) {
    ssize_t w = send(s, c, n, MSG_NOSIGNAL);
    if (w <= 0) return 0;
    c += w; n -= (size_t)w;
  }
  return 1;
}

static const char NOT_FOUND[] =
  "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";

static void *serve(void *u) {
  int s = (int)(intptr_t)u;
  char req[4096], method[8], target[256];
  size_t n = 0;
  unsigned want;
  long long pos;
  char *buf = NULL;
  struct timeval tv = { 15, 0 };
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  while (n < sizeof req - 1) {
    ssize_t got = recv(s, req + n, sizeof req - 1 - n, 0);
    if (got <= 0) break;
    n += (size_t)got; req[n] = 0;
    if (strstr(req, "\r\n\r\n")) break;
  }
  req[n] = 0;
  if (sscanf(req, "%7s %255s", method, target) != 2 ||
      sscanf(target, "/ts/%u/%lld", &want, &pos) != 2) {
    sendAll(s, NOT_FOUND, sizeof NOT_FOUND - 1);
    goto out;
  }
  pthread_mutex_lock(&mu);
  if (want != session || state == TS_OFF) {
    pthread_mutex_unlock(&mu);
    sendAll(s, NOT_FOUND, sizeof NOT_FOUND - 1);
    goto out;
  }
  pthread_mutex_unlock(&mu);
  // No length and no ranges: a live stream that starts where it was asked to.
  { static const char HEAD[] = "HTTP/1.1 200 OK\r\nContent-Type: video/mp2t\r\n"
                               "Cache-Control: no-cache\r\nConnection: close\r\n\r\n";
    if (!sendAll(s, HEAD, sizeof HEAD - 1) || !strcmp(method, "HEAD")) goto out; }
  if (!(buf = malloc(TS_CHUNK))) goto out;
  for (;;) {
    long long at, avail, lo;
    ssize_t got;
    size_t k;
    pthread_mutex_lock(&mu);
    for (;;) {
      if (want != session || state == TS_OFF) { pthread_mutex_unlock(&mu); goto out; }
      lo = oldestByte();
      // Paused past what the ring keeps: the oldest there is, is where it goes on.
      if (pos < lo) pos = alignPacket(lo + cap / 32);
      if (pos < written) break;
      if (state == TS_FAILED || state == TS_REFUSED) { pthread_mutex_unlock(&mu); goto out; }
      { struct timespec ts; struct timeval now;
        gettimeofday(&now, NULL);
        ts.tv_sec = now.tv_sec + 1; ts.tv_nsec = now.tv_usec * 1000L;
        pthread_cond_timedwait(&grew, &mu, &ts); }
    }
    at = pos % cap;
    avail = written - pos;
    k = (size_t)(avail < TS_CHUNK ? avail : TS_CHUNK);
    if ((long long)k > cap - at) k = (size_t)(cap - at);
    got = pread(fd, buf, k, (off_t)at);
    // Still ours after the copy? The writer may have lapped it meanwhile.
    if (got <= 0 || pos < oldestByte()) { pthread_mutex_unlock(&mu); continue; }
    pthread_mutex_unlock(&mu);
    if (!sendAll(s, buf, (size_t)got)) break;   // the pipeline closed: a seek, or the end
    pos += got;
  }
out:
  free(buf);
  close(s);
  return NULL;
}

static void *acceptLoop(void *u) {
  (void)u;
  for (;;) {
    pthread_t t;
    int s = accept(listener, NULL, NULL);
    if (s < 0) continue;
#ifdef SO_NOSIGPIPE
    { int one = 1; setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); }
#endif
    if (pthread_create(&t, NULL, serve, (void *)(intptr_t)s) == 0) pthread_detach(t);
    else close(s);
  }
  return NULL;
}

// Under mu.
static int listen_(void) {
  struct sockaddr_in a;
  socklen_t len = sizeof a;
  pthread_t t;
  int one = 1;
  if (port) return port > 0;
  port = -1;
  if ((listener = socket(AF_INET, SOCK_STREAM, 0)) < 0) return 0;
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(listener, (struct sockaddr *)&a, sizeof a) < 0 || listen(listener, 8) < 0 ||
      getsockname(listener, (struct sockaddr *)&a, &len) < 0 ||
      pthread_create(&t, NULL, acceptLoop, NULL) != 0) {
    close(listener); listener = -1;
    printf("[timeshift] no loopback listener\n");
    return 0;
  }
  pthread_detach(t);
  port = ntohs(a.sin_port);
  return 1;
}

// --- The API ----------------------------------------------------------------------------
long long timeshift_budget(int minutes) {
  struct statvfs v;
  long long want = (long long)minutes * 60 * TS_ASSUMED_BPS;
  if (minutes <= 0) return 0;
  if (data_dir() && statvfs(data_dir(), &v) == 0) {
    long long half = (long long)v.f_bavail * (long long)v.f_frsize / 2;
    if (want > half) want = half;
  }
  want -= want % TS_PACKET;
  return want >= TS_MIN_BUDGET ? want : 0;
}

int timeshift_begin(const char *url, const char *headers, long long capBytes) {
  char path[1024];
  pthread_t t;
  Rec *r;
  if (!url || capBytes < TS_MIN_RING) return 0;
  capBytes -= capBytes % TS_PACKET;
  // An HLS address is not tried at all; the recorder would only find out later.
  { const char *q = url + strcspn(url, "?#");
    if (q - url >= 5 && !strncasecmp(q - 5, ".m3u8", 5)) return 0;
    if (q - url >= 4 && !strncasecmp(q - 4, ".m3u", 4)) return 0; }
  if (!data_path(path, sizeof path, TS_FILE)) return 0;
  net_prepare();
  if (!(r = calloc(1, sizeof *r))) return 0;
  snprintf(r->url, sizeof r->url, "%s", url);
  snprintf(r->headers, sizeof r->headers, "%s", headers ? headers : "");
  pthread_mutex_lock(&mu);
  if (!listen_()) { pthread_mutex_unlock(&mu); free(r); return 0; }
  session++;
  if (fd >= 0) close(fd);
  // A fresh file each time: nothing of the last channel is ever served.
  unlink(path);
  fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) { state = TS_OFF; pthread_mutex_unlock(&mu); free(r); return 0; }
  cap = capBytes; written = 0; syncAt = -1;
  idxHead = idxN = 0;
  pcrPid = -1; lastPcr = -1; rejoin = 0; carryN = 0;
  state = TS_STARTING;
  r->session = session;
  pthread_cond_broadcast(&grew);
  pthread_mutex_unlock(&mu);
  if (pthread_create(&t, NULL, record, r) != 0) {
    pthread_mutex_lock(&mu); state = TS_FAILED; pthread_mutex_unlock(&mu);
    free(r);
    return 0;
  }
  pthread_detach(t);
  printf("[timeshift] recording, ring of %lld MB\n", capBytes >> 20);
  fflush(stdout);
  return 1;
}

void timeshift_end(void) {
  char path[1024];
  pthread_mutex_lock(&mu);
  if (state == TS_OFF) { pthread_mutex_unlock(&mu); return; }
  session++;
  state = TS_OFF;
  if (fd >= 0) close(fd);
  fd = -1;
  written = 0; syncAt = -1; idxN = 0;
  pthread_cond_broadcast(&grew);
  pthread_mutex_unlock(&mu);
  // The ring is gigabytes of someone's channel: it does not outlive the viewing.
  if (data_path(path, sizeof path, TS_FILE)) unlink(path);
}

int timeshift_state(void) {
  int s;
  pthread_mutex_lock(&mu); s = state; pthread_mutex_unlock(&mu);
  return s;
}

int timeshift_range(double *oldest, double *newest) {
  int ok;
  pthread_mutex_lock(&mu);
  ok = state == TS_RECORDING && idxN > 0;
  if (ok) {
    if (oldest) *oldest = timeAt(oldestByte());
    // The recording is live: its newest byte is now, give or take a chunk.
    if (newest) *newest = sampleAt(idxN - 1)->t;
  }
  pthread_mutex_unlock(&mu);
  if (!ok) { if (oldest) *oldest = 0; if (newest) *newest = 0; }
  return ok;
}

const char *timeshift_url(double t, char *dst, unsigned size) {
  long long b;
  unsigned s;
  int p;
  pthread_mutex_lock(&mu);
  if (state != TS_RECORDING || !idxN) { pthread_mutex_unlock(&mu); return NULL; }
  b = byteAt(t);
  s = session; p = port;
  pthread_mutex_unlock(&mu);
  if (snprintf(dst, size, "http://127.0.0.1:%d/ts/%u/%lld", p, s, b) >= (int)size) return NULL;
  return dst;
}
