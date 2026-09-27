// timeshift.c against a local upstream that plays an endless MPEG-TS: packets
// of 188 bytes, each carrying its own sequence number, at about 1 MB/s. The
// numbers are what the assertions read: consecutive across a pause means the
// pause resumed in place, lower after a rewind means it went back.
#include "timeshift.h"
#include "data.h"
#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void waitState(int want, int seconds) {
  for (int i = 0; i < seconds * 20 && timeshift_state() != want; i++) usleep(50000);
  if (timeshift_state() != want) { printf("state %d, wanted %d\n", timeshift_state(), want); abort(); }
}

// Opens the loopback URL and returns the socket past the response headers.
static int openUrl(const char *url) {
  int port; unsigned s; long long b;
  char req[256], c, last[4] = { 0 };
  struct sockaddr_in a;
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(sscanf(url, "http://127.0.0.1:%d/ts/%u/%lld", &port, &s, &b) == 3);
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET; a.sin_port = htons((unsigned short)port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(connect(fd, (struct sockaddr *)&a, sizeof a) == 0);
  snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: x\r\n\r\n", strstr(url, "/ts/"));
  assert(write(fd, req, strlen(req)) == (ssize_t)strlen(req));
  while (read(fd, &c, 1) == 1) {
    memmove(last, last + 1, 3); last[3] = c;
    if (!memcmp(last, "\r\n\r\n", 4)) return fd;
  }
  abort();
}

// Reads one packet; its sequence number.
static unsigned packet(int fd) {
  unsigned char p[188];
  size_t n = 0;
  while (n < sizeof p) { ssize_t r = read(fd, p + n, sizeof p - n); assert(r > 0); n += (size_t)r; }
  assert(p[0] == 0x47);
  return (unsigned)p[1] << 24 | (unsigned)p[2] << 16 | (unsigned)p[3] << 8 | p[4];
}

int main(int argc, char **argv) {
  char base[128], url[256], u[256];
  double lo, hi, lo2, hi2;
  int fd;
  unsigned a, b;
  assert(argc > 2);
  data_start(argv[2]);
  snprintf(base, sizeof base, "http://127.0.0.1:%s", argv[1]);

  // HLS is refused without a byte fetched; a playlist served as TS, after one.
  snprintf(url, sizeof url, "%s/live/1.m3u8", base);
  assert(!timeshift_begin(url, NULL, 8 << 20));
  snprintf(url, sizeof url, "%s/playlist", base);
  assert(timeshift_begin(url, NULL, 8 << 20));
  waitState(TS_REFUSED, 10);
  assert(!timeshift_url(0, u, sizeof u));

  // The stream, with a header the upstream insists on; 8 MB of ring.
  snprintf(url, sizeof url, "%s/live/1.ts", base);
  assert(timeshift_begin(url, "X-Auth: secret\n", 8 << 20));
  waitState(TS_RECORDING, 10);
  sleep(2);
  assert(timeshift_range(&lo, &hi) && hi > lo && hi - lo > 1.0);

  // Live: from the newest byte, consecutive packets.
  assert(timeshift_url(hi, u, sizeof u));
  fd = openUrl(u);
  a = packet(fd); b = packet(fd);
  assert(b == a + 1);
  // Paused: nothing read for three seconds while the recording goes on; the
  // next packet is still the next one.
  sleep(3);
  assert(packet(fd) == b + 1);
  close(fd);
  // Rewound: a second in, far behind the live edge.
  assert(timeshift_range(&lo, &hi));
  assert(timeshift_url(lo + 1.0, u, sizeof u));
  fd = openUrl(u);
  a = packet(fd);
  assert(packet(fd) == a + 1);
  assert(a + 1000 < b);    // well behind where live was
  close(fd);

  // The ring wraps (8 MB at ~1 MB/s): the oldest moves on, and a URL for a time
  // before it starts at what is left.
  sleep(10);
  assert(timeshift_range(&lo2, &hi2) && lo2 > lo + 1.0 && hi2 > hi);
  assert(timeshift_url(lo, u, sizeof u));
  fd = openUrl(u);
  a = packet(fd);
  assert(packet(fd) == a + 1);
  close(fd);

  // The upstream drops the connection every 2 MB: the recorder joins again and
  // the range keeps growing.
  snprintf(url, sizeof url, "%s/flaky/1.ts", base);
  assert(timeshift_begin(url, "X-Auth: secret\n", 8 << 20));
  waitState(TS_RECORDING, 10);
  sleep(4);
  assert(timeshift_range(&lo, &hi) && hi - lo > 3.0);

  // A dead upstream fails after its retries; the end removes the file.
  snprintf(url, sizeof url, "%s/404/1.ts", base);
  assert(timeshift_begin(url, "X-Auth: secret\n", 8 << 20));
  waitState(TS_FAILED, 30);
  timeshift_end();
  assert(timeshift_state() == TS_OFF);
  { char path[512]; data_path(path, sizeof path, "timeshift.ts"); assert(access(path, F_OK) != 0); }
  assert(timeshift_budget(0) == 0);
  puts("PASS timeshift: TS only, pause resumes in place, rewind, ring wrap, reconnect, failure, cleanup.");
  return 0;
}
