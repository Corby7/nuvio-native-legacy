#include "net.h"
#include <stdio.h>
#include <string.h>

// THIS FUNCTION ONLY, and in a file of its own on purpose.
//
// It is the credential redaction for the logs (see net.h) and it touches no
// network at all: no socket, no curl, no dlopen, no state. The rest of net.c
// has all of those.
//
// WHAT THAT BUYS: a test that fakes the transport defines its own net_download
// / net_post and therefore CANNOT link net.c — the symbols would collide. With
// this function living there, such a test either fails to link or has to stub
// the redaction itself, and the stub is the worse of the two outcomes: the one
// place that hides a credential from the log would become code the test does
// NOT exercise, and a regression that leaked a key would come out green.
//
// Anything that links src/*.c (Mac, ARM — both glob) sees no difference: same
// function, same header.
const char *net_url_public(const char *url, char *dst, unsigned size) {
  const char *e, *h;
  unsigned n;
  if (!dst || size == 0) return "";
  dst[0] = 0;
  if (!url || !*url) return dst;
  e = strstr(url, "://");
  if (!e) { snprintf(dst, size, "%.*s", (int)size - 1, url); return dst; }
  h = e + 3;
  while (*h && *h != '/' && *h != '?' && *h != '#') h++;
  n = (unsigned)(h - url);
  if (n >= size) n = size - 1;
  memcpy(dst, url, n);
  dst[n] = 0;
  // The "/..." says there WAS a path: without it a log line showing a bare host
  // reads as a request to the server's root, which is the wrong conclusion.
  if (*h && n + 4 < size) { memcpy(dst + n, "/...", 4); dst[n + 4] = 0; }
  return dst;
}
