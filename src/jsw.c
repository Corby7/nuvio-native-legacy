#include "jsw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void per(Jsw *w, const char *s, size_t n) {
  if (w->error || !s) return;
  if (w->n + n + 1 > w->cap) {
    size_t new = w->cap ? w->cap * 2 : 256;
    char *q;
    while (new < w->n + n + 1) new *= 2;
    q = (char *)realloc(w->p, new);
    if (!q) { w->error = 1; return; }
    w->p = q;
    w->cap = new;
  }
  memcpy(w->p + w->n, s, n);
  w->n += n;
  w->p[w->n] = 0;
}

static void pct(Jsw *w, char c) { per(w, &c, 1); }

// Called BEFORE every value: writes the comma that separates it from the
// previous one. It is the only comma in the file, which is why one never ends up
// trailing at the end of a list.
static void split(Jsw *w) {
  if (w->error) return;
  if (w->depth > 0 && w->depth <= JSW_DEPTH) {
    if (w->first[w->depth - 1]) w->first[w->depth - 1] = 0;
    else pct(w, ',');
  }
}

static void enter(Jsw *w, char openCh) {
  split(w);
  pct(w, openCh);
  if (w->depth >= JSW_DEPTH) { w->error = 1; return; }
  w->first[w->depth] = 1;
  w->depth++;
}

static void exitLevel(Jsw *w, char closeCh) {
  if (w->depth > 0) w->depth--;
  pct(w, closeCh);
}

void jsw_start(Jsw *w) {
  memset(w, 0, sizeof *w);
}

void jsw_free(Jsw *w) {
  free(w->p);
  memset(w, 0, sizeof *w);
}

const char *jsw_text_final(const Jsw *w) {
  return (w->error || !w->p) ? NULL : w->p;
}

void jsw_obj_start(Jsw *w) { enter(w, '{'); }
void jsw_obj_end(Jsw *w) { exitLevel(w, '}'); }
void jsw_arr_start(Jsw *w) { enter(w, '['); }
void jsw_arr_end(Jsw *w) { exitLevel(w, ']'); }

// The string itself, WITHOUT going through the separator. It exists because of a
// real defect: jsw_key separated and then called jsw_str, which separated again
// — and since the first separation had already consumed the "first of the level"
// marker, the second wrote a comma. The body came out as {,"a":1} and the server
// answered "Empty or invalid json", without saying where.
static void writeStr(Jsw *w, const char *s) {
  const unsigned char *p;
  pct(w, '"');
  for (p = (const unsigned char *)s; *p; p++) {
    switch (*p) {
      case '"':  per(w, "\\\"", 2); break;
      case '\\': per(w, "\\\\", 2); break;
      case '\n': per(w, "\\n", 2);  break;
      case '\r': per(w, "\\r", 2);  break;
      case '\t': per(w, "\\t", 2);  break;
      default:
        // A control character below 0x20 MUST become \u00XX; loose, it makes
        // the document invalid and the server refuses the whole body. Above
        // 0x7F the byte passes through raw: the content here is UTF-8, which is
        // what JSON expects, and escaping byte by byte would break the
        // accents.
        if (*p < 0x20) {
          char u[7];
          snprintf(u, sizeof u, "\\u%04x", (unsigned)*p);
          per(w, u, 6);
        } else {
          pct(w, (char)*p);
        }
    }
  }
  pct(w, '"');
}

void jsw_str(Jsw *w, const char *s) {
  if (!s) { jsw_null(w); return; }
  split(w);
  writeStr(w, s);
}

// The key carries the level's comma; the value that follows it does not carry
// another, which is why the "first" marker is put back immediately after.
void jsw_key(Jsw *w, const char *name) {
  split(w);
  writeStr(w, name ? name : "");
  pct(w, ':');
  if (w->depth > 0 && w->depth <= JSW_DEPTH) w->first[w->depth - 1] = 1;
}

void jsw_num(Jsw *w, double v) {
  char b[40];
  split(w);
  // %.17g reproduces the double exactly; a plain %g rounds, and a playback
  // progress comes back different from what was sent.
  snprintf(b, sizeof b, "%.17g", v);
  per(w, b, strlen(b));
}

void jsw_int(Jsw *w, long long v) {
  char b[32];
  split(w);
  snprintf(b, sizeof b, "%lld", v);
  per(w, b, strlen(b));
}

void jsw_bool(Jsw *w, int v) {
  split(w);
  if (v) per(w, "true", 4); else per(w, "false", 5);
}

void jsw_null(Jsw *w) {
  split(w);
  per(w, "null", 4);
}

void jsw_raw(Jsw *w, const char *json) {
  if (!json || !*json) { jsw_null(w); return; }
  split(w);
  per(w, json, strlen(json));
}

void jsw_cs(Jsw *w, const char *key, const char *value) {
  jsw_key(w, key);
  jsw_str(w, value);
}

void jsw_ci(Jsw *w, const char *key, long long value) {
  jsw_key(w, key);
  jsw_int(w, value);
}

void jsw_cb(Jsw *w, const char *key, int value) {
  jsw_key(w, key);
  jsw_bool(w, value);
}
