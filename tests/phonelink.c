// phonelink.c over a real socket: the code in the QR is required, the page is
// prefilled without the password, a save reaches the main thread once and
// spends the code, and closing stops the listener.
#include "phonelink.h"
#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

// One request to the listener on loopback; the whole response in `out`.
// Returns the status code, or -1 when nothing accepted the connection.
static int http(const char *method, const char *target, const char *body, char *out, size_t n) {
  struct sockaddr_in a;
  char req[4096];
  size_t got = 0;
  int fd = socket(AF_INET, SOCK_STREAM, 0), k, status = 0;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons((unsigned short)phonelink_port());
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return -1; }
  k = snprintf(req, sizeof req,
               "%s %s HTTP/1.1\r\nHost: tv\r\nContent-Type: application/x-www-form-urlencoded\r\n"
               "Content-Length: %zu\r\n\r\n%s", method, target, body ? strlen(body) : 0, body ? body : "");
  assert(send(fd, req, (size_t)k, 0) == k);
  for (;;) {
    ssize_t r = recv(fd, out + got, n - 1 - got, 0);
    if (r <= 0) break;
    got += (size_t)r;
  }
  out[got] = 0;
  close(fd);
  sscanf(out, "HTTP/1.1 %d", &status);
  return status;
}

static void pause_ms(int ms) { struct timespec t = { 0, ms * 1000000L }; nanosleep(&t, NULL); }

int main(void) {
  static char res[65536];
  char target[256];
  char code[64], old[64];
  IptvSource now, got;

  memset(&now, 0, sizeof now);
  now.kind = IPTV_SRC_XTREAM;
  snprintf(now.server, sizeof now.server, "http://tv.example:8080");
  snprintf(now.user, sizeof now.user, "alice");
  snprintf(now.pass, sizeof now.pass, "s3cret-pass");

  if (!phonelink_open(&now)) {
    // A machine with no network interface but loopback cannot offer an address.
    puts("SKIP phonelink: no network address on this machine");
    return 0;
  }
  printf("url: %s\n", phonelink_url());
  assert(!strncmp(phonelink_url(), "http://", 7));
  assert(strstr(phonelink_url(), "?k="));
  snprintf(code, sizeof code, "%s", strstr(phonelink_url(), "?k="));
  assert(strlen(code + 3) == 32);
  assert(phonelink_state() == PL_WAITING);
  assert(phonelink_open(&now));                    // idempotent: same address
  assert(strstr(phonelink_url(), code + 3));

  // No code, or the wrong one: refused, and the TV does not count it as opened.
  assert(http("GET", "/", NULL, res, sizeof res) == 403);
  assert(http("GET", "/?k=00000000000000000000000000000000", NULL, res, sizeof res) == 403);
  assert(phonelink_state() == PL_WAITING);

  // The page: prefilled with the saved login, never its password.
  snprintf(target, sizeof target, "/%s", code);
  assert(http("GET", target, NULL, res, sizeof res) == 200);
  assert(strstr(res, "value=\"http://tv.example:8080\""));
  assert(strstr(res, "value=\"alice\""));
  assert(!strstr(res, "s3cret"));
  assert(phonelink_state() == PL_OPENED);

  // Incomplete: the form again, with the reason; nothing reaches the TV.
  snprintf(target, sizeof target, "/save%s", code);
  assert(http("POST", target, "kind=m3u&url=provider.example%2Flist", res, sizeof res) == 200);
  assert(strstr(res, "starting with http"));
  assert(!phonelink_take(&got));

  // The same login with the password left empty keeps the saved one; the
  // pasted newline and the encoded characters come out clean.
  assert(http("POST", target,
              "kind=xtream&url=ignored&server=http%3A%2F%2Ftv.example%3A8080%0D%0A&user=alice&pass=&epg=",
              res, sizeof res) == 200);
  assert(strstr(res, "Sent to the TV"));
  assert(phonelink_take(&got));
  assert(got.kind == IPTV_SRC_XTREAM);
  assert(!strcmp(got.server, "http://tv.example:8080"));
  assert(!strcmp(got.pass, "s3cret-pass"));
  assert(!got.url[0]);
  assert(!phonelink_take(&got));                   // once

  // The code is spent: the page it opened no longer answers.
  snprintf(target, sizeof target, "/%s", code);
  assert(http("GET", target, NULL, res, sizeof res) == 403);

  // Closed: the socket goes within the listener's poll interval.
  { int p = phonelink_port();
    phonelink_close();
    assert(phonelink_state() == PL_OFF && !phonelink_url()[0]);
    pause_ms(400);
    { struct sockaddr_in a; int fd = socket(AF_INET, SOCK_STREAM, 0);
      memset(&a, 0, sizeof a);
      a.sin_family = AF_INET; a.sin_port = htons((unsigned short)p);
      a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      assert(connect(fd, (struct sockaddr *)&a, sizeof a) < 0);
      close(fd); } }

  // An M3U save from a fresh open, with a new code.
  memset(&now, 0, sizeof now);
  assert(phonelink_open(&now));
  snprintf(old, sizeof old, "%s", code);
  snprintf(code, sizeof code, "%s", strstr(phonelink_url(), "?k="));
  assert(strcmp(code, old));
  snprintf(target, sizeof target, "/save%s", code);
  assert(http("POST", target,
              "kind=m3u&url=+http%3A%2F%2Fp.example%2Fget.php%3Fa%3D1%26b%3D2+&epg=http%3A%2F%2Fp.example%2Fepg.xml%0D%0A%0D%0A+https%3A%2F%2Fg.example%2Fuk.xml.gz%2C"
              "&server=x&user=y&pass=z", res, sizeof res) == 200);
  assert(phonelink_take(&got));
  assert(got.kind == IPTV_SRC_M3U);
  assert(!strcmp(got.url, "http://p.example/get.php?a=1&b=2"));
  assert(!strcmp(got.epg, "http://p.example/epg.xml https://g.example/uk.xml.gz"));
  assert(!got.server[0] && !got.user[0] && !got.pass[0]);
  phonelink_close();

  puts("PASS phonelink: code required, prefill without the password, one save, closes.");
  return 0;
}
