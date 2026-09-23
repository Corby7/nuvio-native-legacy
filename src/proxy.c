#include "proxy.h"
#include "net.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// Linux has a per-call flag; macOS only a per-socket option (set on accept).
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define PX_ROUTES   8      // sources wrapped at once; the oldest is recycled
#define PX_TOKEN   16      // hex characters
#define PX_REQUEST 8192    // the pipeline's request line and headers
#define PX_HEADERS 24      // header lines sent upstream, Range included

// One wrapped source. EIGHT and recycled in turn: a route is needed for as long
// as its source plays — the pipeline reconnects on every seek — and a new one is
// only made when a source is chosen, so eight choices would have to happen
// during one playback before the live one is overwritten.
typedef struct {
  char token[PX_TOKEN + 1];
  char origin[512];        // "https://host:port", no path
  char headers[1024];
} Route;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static Route routes[PX_ROUTES];
static int nextRoute;
static int port;           // 0 = not started; -1 = failed, and stays failed

// --- the table -----------------------------------------------------------------

static void newToken(char *dst) {
  unsigned char raw[PX_TOKEN / 2];
  FILE *f = fopen("/dev/urandom", "rb");
  size_t got = f ? fread(raw, 1, sizeof raw, f) : 0;
  int k;
  if (f) fclose(f);
  if (got != sizeof raw) {
    // Not a secret worth failing over: the socket is loopback-only. The token
    // just keeps one route from being reached by guessing the next number.
    unsigned v = (unsigned)time(NULL) ^ (unsigned)(size_t)dst ^ (unsigned)rand();
    for (k = 0; k < (int)sizeof raw; k++) { v = v * 1103515245u + 12345u; raw[k] = (unsigned char)(v >> 16); }
  }
  for (k = 0; k < (int)sizeof raw; k++) snprintf(dst + k * 2, 3, "%02x", raw[k]);
}

// Copies the route for `token` out from under the lock. 1 if it exists.
static int routeFind(const char *token, Route *out) {
  int k, ok = 0;
  pthread_mutex_lock(&lock);
  for (k = 0; k < PX_ROUTES; k++)
    if (routes[k].token[0] && !strncmp(routes[k].token, token, PX_TOKEN)) {
      *out = routes[k]; ok = 1; break;
    }
  pthread_mutex_unlock(&lock);
  return ok;
}

// --- one connection --------------------------------------------------------------

typedef struct {
  int fd, code, sent, failed;
  char head[2048];         // the response headers passed on, "Name: v\r\n" each
  size_t nHead;
} Relay;

static int sendAll(int fd, const void *p, size_t n) {
  const char *c = p;
  while (n > 0) {
    ssize_t w = send(fd, c, n, MSG_NOSIGNAL);
    if (w <= 0) return 0;
    c += w; n -= (size_t)w;
  }
  return 1;
}

static const char *reason(int code) {
  switch (code) {
    case 200: return "OK";
    case 206: return "Partial Content";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 416: return "Range Not Satisfiable";
    case 502: return "Bad Gateway";
    default:  return code >= 500 ? "Server Error" : code >= 400 ? "Error" : "OK";
  }
}

static int flushHead(Relay *r) {
  char status[64];
  if (r->sent) return !r->failed;
  r->sent = 1;
  snprintf(status, sizeof status, "HTTP/1.1 %d %s\r\n", r->code, reason(r->code));
  if (!sendAll(r->fd, status, strlen(status)) ||
      !sendAll(r->fd, r->head, r->nHead) ||
      !sendAll(r->fd, "Connection: close\r\n\r\n", 21)) r->failed = 1;
  return !r->failed;
}

// Only what describes the BODY is passed back. Everything else — cookies, the
// upstream's own connection handling, its transfer encoding, which libcurl has
// already undone — belongs to the other side of the relay.
static const char *const PASSED[] = {
  "content-type:", "content-length:", "content-range:", "accept-ranges:",
  "last-modified:", "etag:", NULL
};

static size_t onHeader(char *p, size_t size, size_t count, void *u) {
  Relay *r = u;
  size_t n = size * count, k;
  if (n >= 5 && !strncmp(p, "HTTP/", 5)) {
    // A new response — the first, or the next hop of a redirect. What the
    // previous one said about its body does not apply to this one.
    const char *sp = memchr(p, ' ', n);
    r->code = sp ? atoi(sp + 1) : 0;
    r->nHead = 0;
    return n;
  }
  for (k = 0; PASSED[k]; k++) {
    size_t kl = strlen(PASSED[k]);
    if (n > kl && !strncasecmp(p, PASSED[k], kl) && r->nHead + n + 2 < sizeof r->head) {
      size_t body = n;
      while (body > 0 && (p[body - 1] == '\r' || p[body - 1] == '\n')) body--;
      memcpy(r->head + r->nHead, p, body);
      r->nHead += body;
      memcpy(r->head + r->nHead, "\r\n", 2);
      r->nHead += 2;
      break;
    }
  }
  return n;
}

static size_t onBody(char *p, size_t size, size_t count, void *u) {
  Relay *r = u;
  size_t n = size * count;
  // The pipeline closing its socket is the normal end of a seek; returning 0
  // here is what stops the upstream download along with it.
  if (!flushHead(r) || !sendAll(r->fd, p, n)) { r->failed = 1; return 0; }
  return n;
}

static void answer(int fd, int code) {
  char text[128];
  snprintf(text, sizeof text,
           "HTTP/1.1 %d %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
           code, reason(code));
  sendAll(fd, text, strlen(text));
}

// The value of request header `name` (with its colon), copied as a whole line.
static int requestHeader(const char *req, const char *name, char *dst, size_t size) {
  const char *p = strstr(req, "\r\n");
  size_t nl = strlen(name);
  while (p && p[2] && !(p[2] == '\r' && p[3] == '\n')) {
    const char *line = p + 2, *end = strstr(line, "\r\n");
    if (!end) break;
    if (!strncasecmp(line, name, nl) && (size_t)(end - line) < size) {
      memcpy(dst, line, (size_t)(end - line));
      dst[end - line] = 0;
      return 1;
    }
    p = end;
  }
  return 0;
}

static void *serve(void *u) {
  int fd = (int)(intptr_t)u, headOnly, nLines = 0;
  char req[PX_REQUEST], method[8], target[4200], range[128], upstream[4800];
  char safe[120], names[256] = "";
  const char *lines[PX_HEADERS + 1];
  size_t n = 0;
  Route route;
  Relay relay;
  long status = 0;
  int result;
  struct timeval tv = { 15, 0 };

  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  // Reads up to the blank line that ends the request. The pipeline sends GET or
  // HEAD with no body, so that is the whole of it.
  while (n < sizeof req - 1) {
    ssize_t got = recv(fd, req + n, sizeof req - 1 - n, 0);
    if (got <= 0) break;
    n += (size_t)got;
    req[n] = 0;
    if (strstr(req, "\r\n\r\n")) break;
  }
  req[n] = 0;
  if (!strstr(req, "\r\n\r\n") ||
      sscanf(req, "%7s %4199s", method, target) != 2) { answer(fd, 400); goto out; }
  headOnly = !strcmp(method, "HEAD");
  if (!headOnly && strcmp(method, "GET")) { answer(fd, 405); goto out; }
  // /p/<token><path> — the path has to be there, and it starts with '/'.
  if (strncmp(target, "/p/", 3) || strlen(target) < 3 + PX_TOKEN + 1 ||
      target[3 + PX_TOKEN] != '/' || !routeFind(target + 3, &route)) {
    answer(fd, 404); goto out;
  }
  snprintf(upstream, sizeof upstream, "%s%s", route.origin, target + 3 + PX_TOKEN);

  // The stream's headers, one line each, then the pipeline's Range — which is
  // what makes seeking work through the relay at all.
  { char *save = NULL, *line = strtok_r(route.headers, "\n", &save);
    for (; line && nLines < PX_HEADERS - 1; line = strtok_r(NULL, "\n", &save)) {
      const char *colon = strchr(line, ':');
      lines[nLines++] = line;
      if (colon && strlen(names) + (size_t)(colon - line) + 2 < sizeof names)
        snprintf(names + strlen(names), sizeof names - strlen(names), "%s%.*s",
                 names[0] ? "," : "", (int)(colon - line), line);
    } }
  range[0] = 0;
  if (requestHeader(req, "range:", range, sizeof range)) lines[nLines++] = range;
  lines[nLines] = NULL;

  memset(&relay, 0, sizeof relay);
  relay.fd = fd;
  result = net_stream(upstream, lines, headOnly, onHeader, onBody, &relay, &status);
  if (!relay.sent) {
    // No body came (a HEAD, an empty answer, or no answer at all). A status we
    // never got from upstream is ours to invent: 502, the gateway's answer.
    if (!relay.code) { relay.code = 502; answer(fd, 502); }
    else flushHead(&relay);
  }
  printf("[proxy] %s %s [%s] %s -> %d%s\n", method,
         net_url_public(upstream, safe, sizeof safe), names,
         range[0] ? range + 6 : "", relay.code,
         relay.failed ? " (client closed)" : result ? " (upstream failed)" : "");
  fflush(stdout);
out:
  close(fd);
  return NULL;
}

// --- the listener ----------------------------------------------------------------

static int listener = -1;

static void *acceptLoop(void *u) {
  (void)u;
  for (;;) {
    pthread_t t;
    int fd = accept(listener, NULL, NULL);
    if (fd < 0) continue;
#ifdef SO_NOSIGPIPE
    { int one = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); }
#endif
    // A thread per connection: the pipeline opens a new one for each seek and
    // closes the old, so there are rarely more than two alive.
    if (pthread_create(&t, NULL, serve, (void *)(intptr_t)fd) == 0) pthread_detach(t);
    else close(fd);
  }
  return NULL;
}

// Under `lock`.
static int start(void) {
  struct sockaddr_in a;
  socklen_t len = sizeof a;
  pthread_t t;
  int one = 1;
  if (port) return port > 0;
  port = -1;
  listener = socket(AF_INET, SOCK_STREAM, 0);
  if (listener < 0) { printf("[proxy] socket failed\n"); return 0; }
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // loopback ONLY
  a.sin_port = 0;                               // any free port
  if (bind(listener, (struct sockaddr *)&a, sizeof a) < 0 || listen(listener, 8) < 0 ||
      getsockname(listener, (struct sockaddr *)&a, &len) < 0) {
    printf("[proxy] bind/listen failed\n");
    close(listener); listener = -1; return 0;
  }
  // libcurl's global init on this thread, before any connection thread needs it.
  net_prepare();
  if (pthread_create(&t, NULL, acceptLoop, NULL) != 0) {
    printf("[proxy] no thread\n");
    close(listener); listener = -1; return 0;
  }
  pthread_detach(t);
  port = ntohs(a.sin_port);
  printf("[proxy] listening on 127.0.0.1:%d\n", port);
  fflush(stdout);
  return 1;
}

const char *proxy_wrap(const char *url, const char *headers, char *dst,
                       unsigned size) {
  const char *auth, *rest;
  Route *r;
  size_t originLen;
  int written;
  if (!url || !headers || !*headers) return url;
  if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) return url;
  auth = strstr(url, "://") + 3;
  rest = auth + strcspn(auth, "/?#");
  originLen = (size_t)(rest - url);
  if (originLen >= sizeof routes[0].origin) return url;

  pthread_mutex_lock(&lock);
  if (!start()) { pthread_mutex_unlock(&lock); return url; }
  r = &routes[nextRoute];
  nextRoute = (nextRoute + 1) % PX_ROUTES;
  newToken(r->token);
  memcpy(r->origin, url, originLen);
  r->origin[originLen] = 0;
  snprintf(r->headers, sizeof r->headers, "%s", headers);
  written = snprintf(dst, size, "http://127.0.0.1:%d/p/%s%s%s", port, r->token,
                     *rest == '/' ? "" : "/", rest);
  if (written < 0 || (unsigned)written >= size) {
    r->token[0] = 0;
    pthread_mutex_unlock(&lock);
    return url;
  }
  pthread_mutex_unlock(&lock);
  return dst;
}
