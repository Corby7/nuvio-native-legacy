#include "js.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *skip(const char *p) {
  while (*p && (unsigned char)*p <= ' ') p++;
  return p;
}

const char *js_end(const char *p) {
  int depth = 0, text = 0;
  char openCh = *p, closeCh = (openCh == '[') ? ']' : '}';
  if (openCh != '[' && openCh != '{') return p;
  for (; *p; p++) {
    if (text) { if (*p == '\\') p++; else if (*p == '"') text = 0; continue; }
    if (*p == '"') text = 1;
    else if (*p == openCh) depth++;
    else if (*p == closeCh && --depth == 0) return p + 1;
  }
  return p;
}

// Acha `"chave"` dentro da faixa, ignorando ocorrencias dentro de textos.
static const char *findsKey(const char *start, const char *end, const char *key) {
  char search[64];
  const char *p = start;
  size_t n;
  snprintf(search, sizeof search, "\"%s\"", key);
  n = strlen(search);
  while ((p = strstr(p, search)) != NULL) {
    if (end && p >= end) return NULL;
    { const char *q = skip(p + n);
      if (*q == ':') return q + 1; }
    p += n;
  }
  return NULL;
}

int js_text(const char *start, const char *end, const char *key,
             char *dst, size_t size) {
  const char *p = findsKey(start, end, key);
  size_t k = 0;
  if (!p) return 0;
  p = skip(p);
  if (*p != '"') return 0;
  p++;
  while (*p && *p != '"' && k + 1 < size) {
    if (*p == '\\' && p[1]) {
      p++;
      if (*p == 'u') { p += 5; dst[k++] = ' '; continue; }
      if (*p == 'n' || *p == 't' || *p == 'r') { p++; dst[k++] = ' '; continue; }
      if (*p == '/' ) { p++; dst[k++] = '/'; continue; }
    }
    dst[k++] = *p++;
  }
  dst[k] = 0;
  return k > 0;
}

double js_num(const char *start, const char *end, const char *key, double dflt) {
  char search[64];
  const char *p = start;
  size_t n;
  snprintf(search, sizeof search, "\"%s\"", key);
  n = strlen(search);
  while ((p = strstr(p, search)) != NULL) {
    const char *q;
    if (end && p >= end) break;
    q = skip(p + n);
    if (*q == ':') {
      q = skip(q + 1);
      // The value can arrive IN QUOTES. Cinemeta sends `"imdbRating": "8.1"`
      // as a string, and refusing the quote here made js_num return the
      // default — which is why the score was always 0: neither the IMDb badge
      // on the hero nor the ratings tab ever appeared, with no error anywhere
      // along the way.
      if (*q == '"') q++;
      if ((*q >= '0' && *q <= '9') || *q == '-' || *q == '.') return atof(q);
    }
    p += n;
  }
  return dflt;
}

const char *js_array(const char *start, const char *end, const char *key) {
  const char *p = findsKey(start, end, key);
  if (!p) return NULL;
  p = skip(p);
  if (*p != '[') return NULL;
  p = skip(p + 1);
  return (*p == '{' || *p == '"') ? p : NULL;
}

const char *js_next(const char *endPrevious) {
  const char *p = skip(endPrevious);
  if (*p == ',') {
    p = skip(p + 1);
    return (*p == '{' || *p == '"') ? p : NULL;
  }
  return NULL;
}

const char *js_root_array(const char *body) {
  const char *p;
  if (!body) return NULL;
  p = skip(body);
  if (*p != '[') return NULL;
  p = skip(p + 1);
  return (*p == '{' || *p == '"') ? p : NULL;
}

int js_raw(const char *start, const char *end, const char *key,
             char *dst, size_t size) {
  const char *p = findsKey(start, end, key);
  const char *f;
  size_t n;
  if (!p) return 0;
  p = skip(p);
  if (*p == '{' || *p == '[') {
    f = js_end(p);
  } else if (*p == '"') {
    // String: the value may be the serialised JSON itself (the web app accepts
    // both forms). Returned with the quotes; the consumer decides.
    const char *q = p + 1;
    while (*q && *q != '"') { if (*q == '\\' && q[1]) q++; q++; }
    f = *q ? q + 1 : q;
  } else {
    const char *q = p;
    while (*q && *q != ',' && *q != '}' && *q != ']') q++;
    f = q;
  }
  n = (size_t)(f - p);
  if (n + 1 > size) return 0;
  memcpy(dst, p, n);
  dst[n] = 0;
  return 1;
}

// "key": true|false within [start,end).
//
// It exists because `enabled` and `pinToTop` are the first booleans this app
// has had to read, and js_num cannot stand in: it requires a digit or a sign
// after the key precisely so that it never matches an object, so on `true` it
// silently returns the default. A preference that reads as its default no
// matter what the server said is the kind of bug that looks like the server's.
int js_flag(const char *start, const char *end, const char *key, int dflt) {
  char needle[64];
  const char *p;
  if (!start || !end || !key) return dflt;
  snprintf(needle, sizeof needle, "\"%s\"", key);
  p = strstr(start, needle);
  if (!p || p >= end) return dflt;
  p += strlen(needle);
  while (p < end && (*p == ' ' || *p == ':')) p++;
  if (p + 4 <= end && !strncmp(p, "true", 4))  return 1;
  if (p + 5 <= end && !strncmp(p, "false", 5)) return 0;
  return dflt;
}
