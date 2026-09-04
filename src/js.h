// Shared, tolerant JSON reading.
//
// It is not a complete parser and does not intend to be: the formats here
// (Stremio, Cinemeta, Trakt) are known and shallow, and what matters is NEVER
// falling over on a missing field or an unexpected type. Each function returns
// what it found or nothing, and the caller decides. This logic started
// duplicated in addons.c and video.c; it became a module when the third
// consumer appeared.
#ifndef NV_JS_H
#define NV_JS_H
#include <stddef.h>

// End of the object/array starting at `p` (which points at '{' or '['),
// respecting quotes and escapes.
const char *js_end(const char *p);

// Text value of "key" within [start,end). 1 if found. \uXXXX escapes become a
// space on purpose: the texts arrive full of emoji and are for display only —
// decoding UTF-16 here would be work with no return.
int js_text(const char *start, const char *end, const char *key,
             char *dst, size_t size);

// Number for "key". Requires the character after the key to be a digit or a
// sign, which avoids matching an OBJECT of the same name — the real case is
// {"currentTime":{"currentTime":8580}}, where the first occurrence gives 0.
double js_num(const char *start, const char *end, const char *key, double dflt);

// First element of the array named `key`; NULL if there is none. Advance with
// js_next.
const char *js_array(const char *start, const char *end, const char *key);

// Proximo elemento do array a partir do fim do anterior; NULL no fim.
const char *js_next(const char *endPrevious);

// First element of an array that is the ROOT of the document. Every Supabase
// RPC answers `[{...},{...}]` with no key around it, and js_array — which
// searches by name — has nothing to search for there. Advance with js_next.
const char *js_root_array(const char *body);

// Copies the value of `key` as RAW JSON TEXT, braces and brackets included.
// It exists for `credential_json`: the app passes that object on to the server
// uninterpreted, and rebuilding it field by field would lose everything this
// version of the app does not know about. 1 if found and it fitted.
int js_raw(const char *start, const char *end, const char *key,
             char *dst, size_t size);

#endif
