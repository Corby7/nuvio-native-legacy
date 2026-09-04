// Writing JSON. js.c reads; this one writes.
//
// It was born with the sync: every Supabase push sends an array of objects, and
// assembling that with snprintf — the way the app would have done it until now
// — means getting the escaping wrong the first time a title contains a quote or
// a backslash, and the server refuses the whole body, not the bad line. A
// 100-line writer costs less than debugging a push that fails for only some
// users.
//
// The comma is automatic: the caller only says "open object, key, value". The
// classic mistake when assembling JSON by hand is the trailing comma on the
// last element, and it disappears when nobody writes commas.
#ifndef NV_JSW_H
#define NV_JSW_H
#include <stddef.h>

#define JSW_DEPTH 16   // profundidade maxima de aninhamento

typedef struct {
  char  *p;
  size_t n, cap;
  int    error;                 // 1 depois de qualquer falha; o resto vira no-op
  int    depth;
  char   first[JSW_DEPTH];   // 1 enquanto o nivel atual nao tem elemento
} Jsw;

void jsw_start(Jsw *w);
void jsw_free(Jsw *w);

// Texto pronto (0 em falha). O buffer continua sendo do escritor: copie ou use
// antes de jsw_livre.
const char *jsw_text_final(const Jsw *w);

void jsw_obj_start(Jsw *w);
void jsw_obj_end(Jsw *w);
void jsw_arr_start(Jsw *w);
void jsw_arr_end(Jsw *w);

// Chave dentro de um objeto. O proximo jsw_* escreve o valor dela.
void jsw_key(Jsw *w, const char *name);

void jsw_str(Jsw *w, const char *s);     // string com escape; NULL vira null
void jsw_num(Jsw *w, double v);
void jsw_int(Jsw *w, long long v);
void jsw_bool(Jsw *w, int v);
void jsw_null(Jsw *w);

// An ALREADY-FORMED JSON value (an object that came from the server, say).
// It exists for credential_json, which the app passes through uninterpreted.
void jsw_raw(Jsw *w, const char *json);

// Key/value shorthands, which is what almost every caller wants.
void jsw_cs(Jsw *w, const char *key, const char *value);
void jsw_ci(Jsw *w, const char *key, long long value);
void jsw_cb(Jsw *w, const char *key, int value);

#endif
