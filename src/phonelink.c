#define _GNU_SOURCE   // strcasestr
#include "phonelink.h"
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

// A fixed port first, so the address in the QR reads the same from one visit to
// the next; any free one when something else holds it.
#define PL_PORT       8787
#define PL_TOKEN      32        // hex characters: 128 bits
#define PL_REQUEST    (16 * 1024)
#define PL_RECV_S     5         // a phone that connects and says nothing
#define PL_POLL_MS    250       // how soon a closed listener notices

// Everything the listener thread and the main thread share, under `lock`.
// `gen` names the listener that is current: open and close both move it on, and
// a thread whose generation is gone closes its socket and ends. The token is
// cleared on close and on a save, so a request that raced either finds no
// token to match.
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned gen;
static int listening, port, opened;
static char token[PL_TOKEN + 1];
static char url[96];
static IptvSource current;       // the page's prefill; its password never leaves
static IptvSource sent;
static int pending;

// --- small helpers -----------------------------------------------------------------
static void newToken(char *dst) {
  unsigned char raw[PL_TOKEN / 2];
  FILE *f = fopen("/dev/urandom", "rb");
  size_t got = f ? fread(raw, 1, sizeof raw, f) : 0;
  if (f) fclose(f);
  if (got != sizeof raw) {
    // Worse than urandom, and still 128 bits nobody on the network sees.
    struct timeval tv;
    unsigned v;
    gettimeofday(&tv, NULL);
    v = (unsigned)tv.tv_sec ^ (unsigned)tv.tv_usec * 2654435761u ^ (unsigned)getpid();
    for (int k = 0; k < (int)sizeof raw; k++) { v = v * 1103515245u + 12345u; raw[k] = (unsigned char)(v >> 16); }
  }
  for (int k = 0; k < (int)sizeof raw; k++) snprintf(dst + k * 2, 3, "%02x", raw[k]);
}

// The TV's address on the home network. A private range first: a TV with a
// second interface (a VPN, a docker bridge on the Mac) would otherwise offer
// one the phone cannot reach.
static int lanAddress(char *dst, size_t n) {
  struct ifaddrs *all, *i;
  int best = -1;
  if (getifaddrs(&all) != 0) return 0;
  for (i = all; i; i = i->ifa_next) {
    struct sockaddr_in *a;
    unsigned ip;
    int score;
    if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
    if (!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
    a = (struct sockaddr_in *)i->ifa_addr;
    ip = ntohl(a->sin_addr.s_addr);
    if ((ip >> 16) == 0xC0A8) score = 3;                   // 192.168/16
    else if ((ip >> 24) == 10 || (ip >> 20) == 0xAC1) score = 2;   // 10/8, 172.16/12
    else if ((ip >> 16) == 0xA9FE) score = 0;              // link-local
    else score = 1;
    if (score > best && inet_ntop(AF_INET, &a->sin_addr, dst, (socklen_t)n)) best = score;
  }
  freeifaddrs(all);
  return best >= 0;
}

static void sendAll(int fd, const char *p, size_t n) {
  while (n) {
    ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
    if (w <= 0) { if (w < 0 && errno == EINTR) continue; return; }
    p += w; n -= (size_t)w;
  }
}

// A growing buffer for the page: its size depends on the prefill.
typedef struct { char *p; size_t n, cap; } Buf;
static void put(Buf *b, const char *s) {
  size_t k = strlen(s);
  if (b->n + k + 1 > b->cap) {
    size_t cap = b->cap ? b->cap : 4096;
    while (b->n + k + 1 > cap) cap *= 2;
    char *q = realloc(b->p, cap);
    if (!q) return;
    b->p = q; b->cap = cap;
  }
  memcpy(b->p + b->n, s, k + 1);
  b->n += k;
}
// Text into an attribute value or an element.
static void putEsc(Buf *b, const char *s) {
  char one[2] = { 0, 0 };
  for (; *s; s++) {
    switch (*s) {
      case '&': put(b, "&amp;"); break;
      case '<': put(b, "&lt;"); break;
      case '>': put(b, "&gt;"); break;
      case '"': put(b, "&quot;"); break;
      case '\'': put(b, "&#39;"); break;
      default: one[0] = *s; put(b, one);
    }
  }
}

static int hexval(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// The value of `key` in an application/x-www-form-urlencoded string (a body, or
// a query), decoded into `dst`. 0 when absent.
static int formValue(const char *form, const char *key, char *dst, size_t n) {
  size_t kl = strlen(key);
  const char *p = form;
  if (!n) return 0;
  dst[0] = 0;
  while (p && *p) {
    const char *end = p + strcspn(p, "&");
    if ((size_t)(end - p) > kl && !strncmp(p, key, kl) && p[kl] == '=') {
      size_t k = 0;
      for (p += kl + 1; p < end && k + 1 < n; p++) {
        int h, l;
        if (*p == '+') dst[k++] = ' ';
        else if (*p == '%' && p + 2 < end && (h = hexval(p[1])) >= 0 && (l = hexval(p[2])) >= 0) {
          dst[k++] = (char)(h * 16 + l); p += 2;
        } else dst[k++] = *p;
      }
      dst[k] = 0;
      return 1;
    }
    p = *end ? end + 1 : end;
  }
  return 0;
}

// Leading and trailing blanks off: a pasted address often brings a newline.
static void trim(char *s) {
  size_t n = strlen(s), a = 0;
  while (n && (unsigned char)s[n - 1] <= ' ') s[--n] = 0;
  while (s[a] && (unsigned char)s[a] <= ' ') a++;
  if (a) memmove(s, s + a, n - a + 1);
}

// --- the page ------------------------------------------------------------------------
static const char *PAGE_HEAD =
  "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
  "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
  "<title>Nuvio Live TV</title><style>"
  ":root{color-scheme:dark}"
  "body{margin:0;background:#0d0d0d;color:#f5f6f8;font:16px/1.45 -apple-system,system-ui,sans-serif}"
  "main{max-width:520px;margin:0 auto;padding:28px 16px 40px}"
  "h1{font-size:24px;margin:0 0 6px}p{color:#9aa1a9;margin:0 0 20px}"
  ".kind{display:flex;gap:8px;margin:0 0 20px}"
  ".kind label{flex:1;text-align:center;padding:12px;border-radius:12px;background:#1a1d21;cursor:pointer}"
  ".kind input{position:absolute;opacity:0}"
  ".kind label:has(input:checked){background:#f0f2f4;color:#0a0c0e;font-weight:600}"
  "label.f{display:block;margin:0 0 16px;color:#9aa1a9;font-size:14px}"
  "label.f input{display:block;width:100%;box-sizing:border-box;margin-top:6px;padding:14px;"
  "border-radius:12px;border:1px solid #2a2e33;background:#16191d;color:#f5f6f8;font-size:16px}"
  "button{width:100%;padding:15px;border:0;border-radius:12px;background:#f0f2f4;color:#0a0c0e;"
  "font-size:17px;font-weight:600;margin-top:8px}"
  ".err{background:#3a1d19;color:#f3b4a9;padding:12px 14px;border-radius:12px}"
  ".ok{font-size:20px;color:#f5f6f8}"
  "</style></head><body><main>";

static void pageForm(Buf *b, const IptvSource *s, const char *error) {
  int x = s->kind == IPTV_SRC_XTREAM;
  put(b, PAGE_HEAD);
  put(b, "<h1>Live TV source</h1><p>Fill this in and press Save; the TV loads the channels.</p>");
  if (error) { put(b, "<p class=\"err\">"); putEsc(b, error); put(b, "</p>"); }
  // novalidate: the browser's own URL check would refuse "host:8080" with no
  // scheme, which an Xtream server accepts, and hide why on the other half.
  put(b, "<form method=\"post\" novalidate action=\"save?k=");
  put(b, token);
  put(b, "\"><div class=\"kind\">"
         "<label><input type=\"radio\" name=\"kind\" value=\"m3u\" onchange=\"sw()\"");
  put(b, x ? "" : " checked");
  put(b, ">M3U playlist</label>"
         "<label><input type=\"radio\" name=\"kind\" value=\"xtream\" onchange=\"sw()\"");
  put(b, x ? " checked" : "");
  put(b, ">Xtream Codes</label></div>");
  put(b, "<div id=\"m3u\"><label class=\"f\">Playlist address (M3U)"
         "<input name=\"url\" type=\"url\" inputmode=\"url\" autocapitalize=\"off\" autocorrect=\"off\" "
         "placeholder=\"http://provider.example/playlist.m3u\" value=\"");
  putEsc(b, s->url);
  put(b, "\"></label></div>");
  put(b, "<div id=\"xtream\"><label class=\"f\">Server"
         "<input name=\"server\" type=\"url\" inputmode=\"url\" autocapitalize=\"off\" autocorrect=\"off\" "
         "placeholder=\"http://provider.example:8080\" value=\"");
  putEsc(b, s->server);
  put(b, "\"></label><label class=\"f\">Username"
         "<input name=\"user\" autocapitalize=\"off\" autocorrect=\"off\" value=\"");
  putEsc(b, s->user);
  put(b, "\"></label><label class=\"f\">Password"
         "<input name=\"pass\" type=\"password\" autocomplete=\"off\" placeholder=\"");
  put(b, x && current.pass[0] ? "Leave empty to keep the saved one" : "");
  put(b, "\"></label></div>");
  put(b, "<label class=\"f\">TV guide address (XMLTV) \xC2\xB7 optional"
         "<input name=\"epg\" type=\"url\" inputmode=\"url\" autocapitalize=\"off\" autocorrect=\"off\" "
         "placeholder=\"Leave empty to use the source's own\" value=\"");
  putEsc(b, s->epg);
  put(b, "\"></label><button>Save</button></form>"
         // Without script both halves show, which still works.
         "<script>function sw(){var x=document.querySelector('input[value=xtream]').checked;"
         "document.getElementById('m3u').hidden=x;document.getElementById('xtream').hidden=!x}sw()</script>"
         "</main></body></html>");
}

static void pageMessage(Buf *b, const char *title, const char *text) {
  put(b, PAGE_HEAD);
  put(b, "<h1>"); putEsc(b, title); put(b, "</h1><p class=\"ok\">");
  putEsc(b, text); put(b, "</p></main></body></html>");
}

static void respond(int fd, int status, const Buf *b) {
  char head[256];
  const char *word = status == 200 ? "OK" : status == 403 ? "Forbidden" :
                     status == 404 ? "Not Found" : "Bad Request";
  size_t n = b && b->p ? b->n : 0;
  int k = snprintf(head, sizeof head,
                   "HTTP/1.1 %d %s\r\nContent-Type: text/html; charset=utf-8\r\n"
                   "Content-Length: %zu\r\nCache-Control: no-store\r\n"
                   "Referrer-Policy: no-referrer\r\nConnection: close\r\n\r\n",
                   status, word, n);
  sendAll(fd, head, (size_t)k);
  if (n) sendAll(fd, b->p, n);
}

// What the phone typed, checked by the TV's own rules (iptvui's draftComplete).
static const char *validate(const IptvSource *s) {
  if (s->kind == IPTV_SRC_XTREAM) {
    if (!s->server[0] || !s->user[0] || !s->pass[0])
      return "Fill in the server, username and password.";
    return NULL;
  }
  if (!strstr(s->url, "://")) return "Enter the playlist's full address, starting with http.";
  return NULL;
}

// --- one request ---------------------------------------------------------------------
static void serve(int fd, unsigned myGen, const char *peer) {
  char *req = malloc(PL_REQUEST + 1), *body, *q, target[2048], k[PL_TOKEN + 8];
  size_t n = 0;
  long want = -1;
  Buf b = { 0 };
  if (!req) return;
  { struct timeval tv = { PL_RECV_S, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); }
  // The headers, then as much body as Content-Length says.
  for (;;) {
    ssize_t r;
    req[n] = 0;
    body = strstr(req, "\r\n\r\n");
    if (body && want < 0) {
      const char *cl = strcasestr(req, "\r\nContent-Length:");
      want = cl && cl < body ? atol(cl + 17) : 0;
      if (want < 0 || want > PL_REQUEST) want = PL_REQUEST;
    }
    if (body && (long)(n - (size_t)(body + 4 - req)) >= want) break;
    if (n >= PL_REQUEST) break;
    r = recv(fd, req + n, PL_REQUEST - n, 0);
    if (r <= 0) { if (r < 0 && errno == EINTR) continue; break; }
    n += (size_t)r;
  }
  req[n] = 0;
  body = strstr(req, "\r\n\r\n");
  if (!body || sscanf(req, "%*15s %2047s", target) != 1) { respond(fd, 400, NULL); free(req); return; }
  body += 4;
  q = strchr(target, '?');
  if (q) *q++ = 0;

  pthread_mutex_lock(&lock);
  { int live = gen == myGen && token[0];
    int match = live && q && formValue(q, "k", k, sizeof k) && !strcmp(k, token);
    if (!match) {
      pthread_mutex_unlock(&lock);
      if (!strcmp(target, "/favicon.ico")) respond(fd, 404, NULL);
      else {
        pageMessage(&b, "This code has expired",
                    "Open Live TV's source screen on the TV and scan the code shown there again.");
        respond(fd, 403, &b);
      }
      printf("[phone] %s: %s refused (%s)\n", peer, target, live ? "wrong code" : "closed");
      fflush(stdout);
      free(b.p); free(req);
      return;
    } }

  if (!strncmp(req, "POST", 4) && !strcmp(target, "/save")) {
    IptvSource s;
    char kind[16];
    const char *why;
    memset(&s, 0, sizeof s);
    formValue(body, "kind", kind, sizeof kind);
    s.kind = !strcmp(kind, "xtream") ? IPTV_SRC_XTREAM : IPTV_SRC_M3U;
    formValue(body, "url", s.url, sizeof s.url);
    formValue(body, "epg", s.epg, sizeof s.epg);
    formValue(body, "server", s.server, sizeof s.server);
    formValue(body, "user", s.user, sizeof s.user);
    formValue(body, "pass", s.pass, sizeof s.pass);
    trim(s.url); trim(s.epg); trim(s.server); trim(s.user);
    // The page never shows the saved password. Left empty on the same login, it
    // means "keep it".
    if (s.kind == IPTV_SRC_XTREAM && !s.pass[0] && current.kind == IPTV_SRC_XTREAM &&
        !strcmp(s.server, current.server) && !strcmp(s.user, current.user))
      snprintf(s.pass, sizeof s.pass, "%s", current.pass);
    // Only the half the phone chose travels; the other half's fields stay empty
    // so the TV's form shows exactly what will load.
    if (s.kind == IPTV_SRC_XTREAM) s.url[0] = 0;
    else s.server[0] = s.user[0] = s.pass[0] = 0;
    why = validate(&s);
    if (why) {
      pageForm(&b, &s, why);
      pthread_mutex_unlock(&lock);
      respond(fd, 200, &b);
    } else {
      sent = s;
      pending = 1;
      token[0] = 0;           // one save: the code is spent
      pthread_mutex_unlock(&lock);
      pageMessage(&b, "Sent to the TV", "The TV is loading your channels. You can close this page.");
      respond(fd, 200, &b);
    }
    printf("[phone] %s: form %s\n", peer, why ? "incomplete" : "saved");
  } else if (!strcmp(target, "/")) {
    IptvSource shown = current;
    opened = 1;
    pageForm(&b, &shown, NULL);
    pthread_mutex_unlock(&lock);
    respond(fd, 200, &b);
    printf("[phone] %s: page opened\n", peer);
  } else {
    pthread_mutex_unlock(&lock);
    respond(fd, 404, NULL);
  }
  fflush(stdout);
  free(b.p);
  free(req);
}

typedef struct { int fd; unsigned gen; } Listener;

static void *acceptLoop(void *u) {
  Listener l = *(Listener *)u;
  free(u);
  for (;;) {
    struct pollfd p = { l.fd, POLLIN, 0 };
    int live;
    pthread_mutex_lock(&lock);
    live = gen == l.gen;
    pthread_mutex_unlock(&lock);
    if (!live) break;
    if (poll(&p, 1, PL_POLL_MS) <= 0) continue;
    { struct sockaddr_in a;
      socklen_t len = sizeof a;
      char peer[INET_ADDRSTRLEN] = "?";
      int fd = accept(l.fd, (struct sockaddr *)&a, &len);
      if (fd < 0) continue;
#ifdef SO_NOSIGPIPE
      { int one = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); }
#endif
      inet_ntop(AF_INET, &a.sin_addr, peer, sizeof peer);
      // One at a time: one phone fills one form.
      serve(fd, l.gen, peer);
      close(fd);
    }
  }
  close(l.fd);
  return NULL;
}

// --- the main thread's side -----------------------------------------------------------
int phonelink_open(const IptvSource *now) {
  struct sockaddr_in a;
  socklen_t len = sizeof a;
  char host[INET_ADDRSTRLEN];
  int fd, one = 1;
  Listener *l;
  pthread_t t;

  pthread_mutex_lock(&lock);
  if (listening) { pthread_mutex_unlock(&lock); return 1; }
  pthread_mutex_unlock(&lock);

  if (!lanAddress(host, sizeof host)) return 0;
  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return 0;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  a.sin_port = htons(PL_PORT);
  if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) {
    a.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return 0; }
  }
  if (listen(fd, 4) < 0 || getsockname(fd, (struct sockaddr *)&a, &len) < 0) { close(fd); return 0; }
  if (!(l = malloc(sizeof *l))) { close(fd); return 0; }

  pthread_mutex_lock(&lock);
  gen++;
  l->fd = fd; l->gen = gen;
  newToken(token);
  port = ntohs(a.sin_port);
  snprintf(url, sizeof url, "http://%s:%d/?k=%s", host, port, token);
  if (now) current = *now; else memset(&current, 0, sizeof current);
  opened = 0;
  pending = 0;
  if (pthread_create(&t, NULL, acceptLoop, l) != 0) {
    gen++; token[0] = 0; url[0] = 0; port = 0;
    pthread_mutex_unlock(&lock);
    free(l); close(fd);
    return 0;
  }
  pthread_detach(t);
  listening = 1;
  pthread_mutex_unlock(&lock);
  printf("[phone] listening on %s:%d\n", host, port);
  fflush(stdout);
  return 1;
}

void phonelink_close(void) {
  pthread_mutex_lock(&lock);
  if (listening) {
    // The thread sees the generation move within PL_POLL_MS and closes its
    // socket; nothing here waits for it.
    gen++;
    listening = 0;
    token[0] = 0; url[0] = 0; port = 0; opened = 0;
    memset(&current, 0, sizeof current);
    printf("[phone] closed\n");
    fflush(stdout);
  }
  pthread_mutex_unlock(&lock);
}

int phonelink_state(void) {
  int s;
  pthread_mutex_lock(&lock);
  s = !listening ? PL_OFF : opened ? PL_OPENED : PL_WAITING;
  pthread_mutex_unlock(&lock);
  return s;
}

const char *phonelink_url(void) { return url; }

int phonelink_port(void) {
  int p;
  pthread_mutex_lock(&lock);
  p = port;
  pthread_mutex_unlock(&lock);
  return p;
}

int phonelink_take(IptvSource *out) {
  int got;
  pthread_mutex_lock(&lock);
  got = pending;
  if (got) { *out = sent; pending = 0; memset(&sent, 0, sizeof sent); }
  pthread_mutex_unlock(&lock);
  return got;
}
