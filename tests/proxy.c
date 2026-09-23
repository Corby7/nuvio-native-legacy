// proxy.c end to end, against tests/proxy.sh's upstream: a server that answers
// 401 unless `X-Auth: secret` is on the request, honours Range, and redirects
// /hop/<path> to /<path>. The requests go through libcurl exactly as the
// pipeline's would go through GStreamer: to 127.0.0.1, with no headers.
#include "proxy.h"
#include "net.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The upstream's file: byte i is (i * 7 + 3) & 255, 1 MB long.
static int expected(const char *p, long start, long n) {
  long i;
  for (i = 0; i < n; i++)
    if ((unsigned char)p[i] != (unsigned char)(((start + i) * 7 + 3) & 255)) return 0;
  return 1;
}

int main(int argc, char **argv) {
  char upstream[256], local[512], other[512];
  const char *u;
  char *body;
  long n = 0;
  int status = 0;
  assert(argc == 2);
  snprintf(upstream, sizeof upstream, "http://127.0.0.1:%s/media/film.mkv?sig=abc", argv[1]);

  // No headers: the URL goes to the pipeline untouched, as before.
  assert(proxy_wrap(upstream, "", local, sizeof local) == upstream);
  assert(proxy_wrap(upstream, NULL, local, sizeof local) == upstream);
  // The bare URL is refused upstream — the failure this module exists for.
  body = net_download_st(upstream, 10, NULL, &status);
  assert(status == 401); free(body);

  u = proxy_wrap(upstream, "X-Auth: secret\nX-Other: 1", local, sizeof local);
  assert(u == local && !strncmp(local, "http://127.0.0.1:", 17));
  assert(strstr(local, "/media/film.mkv?sig=abc") && !strstr(local, "secret"));
  printf("wrapped: %s\n", local);

  // A range from the middle, as a seek asks for it.
  body = net_download_chunk(local, 10, 500000, 500999, &n);
  assert(body && n == 1000 && expected(body, 500000, 1000)); free(body);
  // The whole file.
  body = net_download_bin(local, 20, &n);
  assert(body && n == 1048576 && expected(body, 0, n)); free(body);
  // A path next to it resolves through the same route: how an HLS playlist's
  // relative segments inherit the headers.
  { char seg[600]; char *slash = strrchr(local, '/');
    snprintf(seg, sizeof seg, "%.*s/other.bin", (int)(slash - local), local);
    body = net_download_chunk(seg, 10, 0, 99, &n);
    assert(body && n == 100 && expected(body, 0, 100)); free(body); }
  // Upstream redirects are followed, headers and all.
  snprintf(upstream, sizeof upstream, "http://127.0.0.1:%s/hop/media/film.mkv", argv[1]);
  u = proxy_wrap(upstream, "X-Auth: secret", other, sizeof other);
  body = net_download_chunk(u, 10, 10, 19, &n);
  assert(body && n == 10 && expected(body, 10, 10)); free(body);
  // A route with the wrong header passes the upstream's refusal on as it is.
  u = proxy_wrap(upstream, "X-Auth: wrong", other, sizeof other);
  body = net_download_st(u, 10, NULL, &status);
  assert(status == 401); free(body);
  // An unknown token is a 404 from the relay, never a request upstream.
  { char bad[600]; snprintf(bad, sizeof bad, "%s", local);
    memcpy(strstr(bad, "/p/") + 3, "0000000000000000", 16);
    body = net_download_st(bad, 10, NULL, &status);
    assert(status == 404); free(body); }
  // The first route still answers after others were made.
  body = net_download_chunk(local, 10, 0, 9, &n);
  assert(body && n == 10 && expected(body, 0, 10)); free(body);

  puts("PASS proxy: headers added upstream, Range, redirects, relative paths, refusals.");
  return 0;
}
