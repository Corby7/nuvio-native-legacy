// M3U and XMLTV, read into IptvList. See iptv_parse.h for the formats as they
// arrive, and tests/iptv_parse.c for the provider quirks each branch exists for.
#include "iptv_parse.h"
#include <ctype.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// --- The string arena ---------------------------------------------------------
// A playlist of 20 000 channels is 140 000 small strings; one malloc each is
// both slow on the TV's allocator and a leak waiting to happen. Blocks of 256 KB,
// a larger one for the rare string that does not fit.
#define ARENA_BLOCK (256 * 1024)
struct IptvBlock { IptvBlock *next; size_t used, cap; char data[]; };

static char *arenaAlloc(IptvList *l, size_t n) {
  IptvBlock *b = l->arena;
  if (!b || b->used + n > b->cap) {
    size_t cap = n > ARENA_BLOCK ? n : ARENA_BLOCK;
    b = malloc(sizeof *b + cap);
    if (!b) return NULL;
    b->next = l->arena; b->used = 0; b->cap = cap;
    l->arena = b;
  }
  { char *p = b->data + b->used; b->used += n; return p; }
}
static const char *arenaDup(IptvList *l, const char *s, size_t n) {
  char *p;
  if (!s || !n) return "";
  p = arenaAlloc(l, n + 1);
  if (!p) return "";
  memcpy(p, s, n); p[n] = 0;
  return p;
}

void iptv_list_init(IptvList *l) { memset(l, 0, sizeof *l); }

void iptv_list_free(IptvList *l) {
  IptvBlock *b = l->arena;
  while (b) { IptvBlock *n = b->next; free(b); b = n; }
  free(l->ch); free(l->pg); free(l->groups);
  memset(l, 0, sizeof *l);
}

static int grow(void **p, int *cap, int need, size_t size) {
  void *q;
  int c;
  if (need <= *cap) return 1;
  c = *cap ? *cap * 2 : 256;
  while (c < need) c *= 2;
  q = realloc(*p, (size_t)c * size);
  if (!q) return 0;
  *p = q; *cap = c;
  return 1;
}

// --- Entities -----------------------------------------------------------------
static int putUtf8(char *d, unsigned c) {
  if (c < 0x80)    { d[0] = (char)c; return 1; }
  if (c < 0x800)   { d[0] = (char)(0xC0 | c >> 6); d[1] = (char)(0x80 | (c & 63)); return 2; }
  if (c < 0x10000) { d[0] = (char)(0xE0 | c >> 12); d[1] = (char)(0x80 | ((c >> 6) & 63));
                     d[2] = (char)(0x80 | (c & 63)); return 3; }
  if (c < 0x110000){ d[0] = (char)(0xF0 | c >> 18); d[1] = (char)(0x80 | ((c >> 12) & 63));
                     d[2] = (char)(0x80 | ((c >> 6) & 63)); d[3] = (char)(0x80 | (c & 63)); return 4; }
  return 0;
}

// Every encoding is no longer than what it replaces ("&#x10FFFF;" is 10 bytes
// for 4), so decoding in place never overtakes the read cursor.
void iptv_xml_unescape(char *s) {
  char *r = s, *w = s;
  if (!s) return;
  while (*r) {
    if (*r == '&') {
      static const struct { const char *name; char c; } NAMED[] = {
        {"amp;", '&'}, {"lt;", '<'}, {"gt;", '>'}, {"quot;", '"'}, {"apos;", '\''}
      };
      int done = 0;
      for (size_t i = 0; i < sizeof NAMED / sizeof *NAMED && !done; i++) {
        size_t n = strlen(NAMED[i].name);
        if (!strncmp(r + 1, NAMED[i].name, n)) { *w++ = NAMED[i].c; r += 1 + n; done = 1; }
      }
      if (!done && r[1] == '#') {
        char *end;
        unsigned long c = (r[2] == 'x' || r[2] == 'X') ? strtoul(r + 3, &end, 16)
                                                       : strtoul(r + 2, &end, 10);
        int k;
        if (*end == ';' && c > 0 && (k = putUtf8(w, (unsigned)c)) > 0) {
          w += k; r = end + 1; done = 1;
        }
      }
      if (done) continue;
    }
    *w++ = *r++;
  }
  *w = 0;
}

// --- M3U ----------------------------------------------------------------------
// The value of attribute `key` in [p, end): key="value", key='value' or key=value.
// Case-insensitive on the key, and it must start a word — "tvg-id" is not found
// inside "xtvg-id".
static int attr(const char *p, const char *end, const char *key, const char **v, size_t *n) {
  size_t k = strlen(key);
  for (const char *q = p; q + k < end; q++) {
    if (strncasecmp(q, key, k) || q[k] != '=') continue;
    if (q > p && !isspace((unsigned char)q[-1]) && q[-1] != ',') continue;
    q += k + 1;
    if (*q == '"' || *q == '\'') {
      char quote = *q++;
      const char *e = q;
      while (e < end && *e != quote) e++;
      *v = q; *n = (size_t)(e - q);
    } else {
      const char *e = q;
      while (e < end && !isspace((unsigned char)*e) && *e != ',') e++;
      *v = q; *n = (size_t)(e - q);
    }
    return 1;
  }
  return 0;
}
static const char *attrDup(IptvList *l, const char *p, const char *end, const char *key) {
  const char *v; size_t n;
  if (!attr(p, end, key, &v, &n)) return "";
  while (n && isspace((unsigned char)*v)) { v++; n--; }
  while (n && isspace((unsigned char)v[n - 1])) n--;
  return arenaDup(l, v, n);
}

static int groupIndex(IptvList *l, const char *g) {
  if (!g[0]) return -1;
  // Consecutive channels nearly always share a group: try the last one first.
  if (l->nGroups && !strcmp(l->groups[l->nGroups - 1], g)) return l->nGroups - 1;
  for (int i = 0; i < l->nGroups; i++) if (!strcmp(l->groups[i], g)) return i;
  if (!grow((void **)&l->groups, &l->capGroups, l->nGroups + 1, sizeof *l->groups)) return -1;
  l->groups[l->nGroups] = g;
  return l->nGroups++;
}

// The last path segment of a URL, for a bare playlist line with no #EXTINF.
static void nameFromUrl(const char *u, size_t n, const char **v, size_t *vn) {
  const char *e = u + n, *s;
  for (const char *q = u; q < e; q++) if (*q == '?' || *q == '#') { e = q; break; }
  s = e;
  while (s > u && s[-1] != '/') s--;
  *v = s; *vn = (size_t)(e - s);
  if (!*vn) { *v = u; *vn = n; }
}

typedef struct {
  int have;                        // an #EXTINF is waiting for its URL
  const char *attrs, *attrsEnd;    // the #EXTINF line, attributes part
  const char *name; size_t nName;
  const char *grp;  size_t nGrp;   // #EXTGRP
  char headers[768]; size_t nHeaders;
} Pending;

static void addHeader(Pending *pd, const char *name, const char *v, size_t n) {
  size_t k = strlen(name);
  while (n && isspace((unsigned char)*v)) { v++; n--; }
  while (n && isspace((unsigned char)v[n - 1])) n--;
  if (!n || pd->nHeaders + k + n + 3 >= sizeof pd->headers) return;
  memcpy(pd->headers + pd->nHeaders, name, k); pd->nHeaders += k;
  pd->headers[pd->nHeaders++] = ':'; pd->headers[pd->nHeaders++] = ' ';
  memcpy(pd->headers + pd->nHeaders, v, n); pd->nHeaders += n;
  pd->headers[pd->nHeaders++] = '\n';
  pd->headers[pd->nHeaders] = 0;
}

// #EXTHTTP:{"User-Agent":"x","Referer":"y"} — flat string pairs only.
static void extHttp(Pending *pd, const char *p, const char *end) {
  while (p < end) {
    const char *k, *ke, *v, *ve;
    char name[64];
    while (p < end && *p != '"') p++;
    if (p >= end) return;
    k = ++p; while (p < end && *p != '"') p++;
    ke = p++;
    while (p < end && (*p == ':' || isspace((unsigned char)*p))) p++;
    if (p >= end || *p != '"') continue;
    v = ++p; while (p < end && *p != '"') p++;
    ve = p++;
    if ((size_t)(ke - k) >= sizeof name) continue;
    memcpy(name, k, (size_t)(ke - k)); name[ke - k] = 0;
    addHeader(pd, name, v, (size_t)(ve - v));
  }
}

static int addChannel(IptvList *l, Pending *pd, const char *url, size_t nUrl) {
  IptvChannel *c;
  const char *v; size_t n;
  if (!grow((void **)&l->ch, &l->capCh, l->nCh + 1, sizeof *l->ch)) return 0;
  c = &l->ch[l->nCh];
  memset(c, 0, sizeof *c);
  c->url = arenaDup(l, url, nUrl);
  if (pd->have) {
    c->tvgId   = attrDup(l, pd->attrs, pd->attrsEnd, "tvg-id");
    c->tvgName = attrDup(l, pd->attrs, pd->attrsEnd, "tvg-name");
    c->logo    = attrDup(l, pd->attrs, pd->attrsEnd, "tvg-logo");
    if (!c->logo[0]) c->logo = attrDup(l, pd->attrs, pd->attrsEnd, "logo");
    c->group   = attrDup(l, pd->attrs, pd->attrsEnd, "group-title");
    if (attr(pd->attrs, pd->attrsEnd, "tvg-chno", &v, &n) ||
        attr(pd->attrs, pd->attrsEnd, "channel-number", &v, &n))
      c->number = atoi(v);
    if (attr(pd->attrs, pd->attrsEnd, "catchup-days", &v, &n) ||
        attr(pd->attrs, pd->attrsEnd, "tvg-rec", &v, &n))
      c->catchupDays = atoi(v);
    c->name = arenaDup(l, pd->name, pd->nName);
  } else {
    c->tvgId = c->tvgName = c->logo = c->group = "";
  }
  if (!c->group[0] && pd->nGrp) c->group = arenaDup(l, pd->grp, pd->nGrp);
  if (!c->name || !c->name[0]) {
    if (c->tvgName[0]) c->name = c->tvgName;
    else { nameFromUrl(url, nUrl, &v, &n); c->name = arenaDup(l, v, n); }
  }
  c->headers = arenaDup(l, pd->headers, pd->nHeaders);
  if (c->number <= 0) c->number = l->nCh + 1;
  c->groupIndex = groupIndex(l, c->group);
  c->firstPg = c->nPg = 0;
  l->nCh++;
  memset(pd, 0, sizeof *pd);
  return 1;
}

// scheme://… with a scheme of letters, digits, '+', '-' or '.', as RFC 3986 has it.
static int isUrl(const char *p, const char *e) {
  const char *q = p;
  if (q >= e || !isalpha((unsigned char)*q)) return 0;
  while (q < e && (isalnum((unsigned char)*q) || *q == '+' || *q == '-' || *q == '.')) q++;
  return e - q > 3 && !strncmp(q, "://", 3);
}

int iptv_parse_m3u(IptvList *l, const char *text) {
  Pending pd;
  int before = l->nCh;
  const char *p = text;
  memset(&pd, 0, sizeof pd);
  if (!text) return 0;
  // A UTF-8 byte order mark, which some panels prepend.
  if (!strncmp(p, "\xEF\xBB\xBF", 3)) p += 3;
  while (*p) {
    const char *e = p, *next;
    while (*e && *e != '\n' && *e != '\r') e++;
    next = e;
    while (*next == '\n' || *next == '\r') next++;
    while (p < e && isspace((unsigned char)*p)) p++;
    { const char *t = e;
      while (t > p && isspace((unsigned char)t[-1])) t--;
      e = t; }

    if (p == e) { p = next; continue; }
    if (!strncasecmp(p, "#EXTM3U", 7)) {
      const char *v; size_t n;
      if (attr(p, e, "url-tvg", &v, &n) || attr(p, e, "x-tvg-url", &v, &n) ||
          attr(p, e, "tvg-url", &v, &n)) {
        size_t k = 0;
        while (k < n && v[k] != ',') k++;
        if (k >= sizeof l->epgUrl) k = sizeof l->epgUrl - 1;
        memcpy(l->epgUrl, v, k); l->epgUrl[k] = 0;
      }
    } else if (!strncasecmp(p, "#EXTINF:", 8)) {
      // The name follows the last comma that is not inside quotes.
      const char *q = p + 8, *comma = NULL;
      char quote = 0;
      for (; q < e; q++) {
        if (quote) { if (*q == quote) quote = 0; }
        else if (*q == '"') quote = '"';
        else if (*q == ',') comma = q;
      }
      pd.have = 1;
      pd.attrs = p + 8;
      pd.attrsEnd = comma ? comma : e;
      if (comma) {
        const char *s = comma + 1;
        while (s < e && isspace((unsigned char)*s)) s++;
        pd.name = s; pd.nName = (size_t)(e - s);
      } else {
        pd.name = ""; pd.nName = 0;
      }
    } else if (!strncasecmp(p, "#EXTGRP:", 8)) {
      pd.grp = p + 8; pd.nGrp = (size_t)(e - p - 8);
      while (pd.nGrp && isspace((unsigned char)*pd.grp)) { pd.grp++; pd.nGrp--; }
    } else if (!strncasecmp(p, "#EXTVLCOPT:", 11)) {
      const char *o = p + 11;
      if (!strncasecmp(o, "http-user-agent=", 16))
        addHeader(&pd, "User-Agent", o + 16, (size_t)(e - o - 16));
      else if (!strncasecmp(o, "http-referrer=", 14))
        addHeader(&pd, "Referer", o + 14, (size_t)(e - o - 14));
      else if (!strncasecmp(o, "http-referer=", 13))
        addHeader(&pd, "Referer", o + 13, (size_t)(e - o - 13));
      else if (!strncasecmp(o, "http-origin=", 12))
        addHeader(&pd, "Origin", o + 12, (size_t)(e - o - 12));
    } else if (!strncasecmp(p, "#EXTHTTP:", 9)) {
      extHttp(&pd, p + 9, e);
    } else if (*p == '#') {
      // #KODIPROP, #EXT-X-… and comments: nothing the TV pipeline can use.
    } else if (isUrl(p, e)) {
      // A stream line. One with no scheme is not: a stray word, or the JSON an
      // expired Xtream account answers with instead of a playlist.
      if (!addChannel(l, &pd, p, (size_t)(e - p))) break;
    }
    p = next;
  }
  return l->nCh - before;
}

// --- Time ---------------------------------------------------------------------
// Days since 1970-01-01 of a proleptic Gregorian date (Howard Hinnant's
// days_from_civil). timegm is not in C99 and mktime would apply the TV's zone.
static long long daysFromCivil(int y, int m, int d) {
  long long era, yoe, doy, doe;
  y -= m <= 2;
  era = (y >= 0 ? y : y - 399) / 400;
  yoe = y - era * 400;
  doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

static int digits(const char *s, int n) {
  int v = 0;
  for (int i = 0; i < n; i++) {
    if (!isdigit((unsigned char)s[i])) return -1;
    v = v * 10 + (s[i] - '0');
  }
  return v;
}

long long iptv_xmltv_time(const char *s) {
  int y, mo, d, h, mi, se = 0;
  long long t;
  if (!s) return 0;
  while (isspace((unsigned char)*s)) s++;
  // Twelve digits at least; checked before any of them is read, so a short
  // string is never read past its end.
  for (int i = 0; i < 12; i++) if (!isdigit((unsigned char)s[i])) return 0;
  y = digits(s, 4); mo = digits(s + 4, 2); d = digits(s + 6, 2);
  h = digits(s + 8, 2); mi = digits(s + 10, 2);
  if (y < 0 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59)
    return 0;
  s += 12;
  if (isdigit((unsigned char)s[0]) && isdigit((unsigned char)s[1])) { se = digits(s, 2); s += 2; }
  t = daysFromCivil(y, mo, d) * 86400LL + h * 3600 + mi * 60 + se;
  while (isspace((unsigned char)*s)) s++;
  if ((*s == '+' || *s == '-') && isdigit((unsigned char)s[1]) && isdigit((unsigned char)s[2])) {
    int oh = digits(s + 1, 2);
    int om = (isdigit((unsigned char)s[3]) && isdigit((unsigned char)s[4])) ? digits(s + 3, 2) : 0;
    if (oh >= 0) {
      long long off = oh * 3600LL + (om > 0 ? om * 60 : 0);
      t += (*s == '+') ? -off : off;
    }
  }
  return t;
}

// --- XMLTV --------------------------------------------------------------------
// Open-addressing table from a lower-cased key to the FIRST channel carrying it;
// the others with the same key are chained through `link` (an HD and an SD copy
// of one channel share a tvg-id, and both should show the guide).
typedef struct { const char **key; int *val; int cap; } Map;

static unsigned hashKey(const char *s, size_t n) {
  unsigned h = 2166136261u;
  for (size_t i = 0; i < n; i++) h = (h ^ (unsigned char)tolower((unsigned char)s[i])) * 16777619u;
  return h;
}
static int keyEq(const char *a, const char *b, size_t n) {
  return !strncasecmp(a, b, n) && a[n] == 0;
}
static int mapInit(Map *m, int n) {
  m->cap = 64;
  while (m->cap < n * 2) m->cap *= 2;
  m->key = calloc((size_t)m->cap, sizeof *m->key);
  m->val = malloc((size_t)m->cap * sizeof *m->val);
  return m->key && m->val;
}
static void mapFree(Map *m) { free(m->key); free(m->val); memset(m, 0, sizeof *m); }
static int mapGet(const Map *m, const char *k, size_t n) {
  unsigned i;
  if (!m->cap || !n) return -1;
  i = hashKey(k, n) & (unsigned)(m->cap - 1);
  while (m->key[i]) {
    if (keyEq(m->key[i], k, n)) return m->val[i];
    i = (i + 1) & (unsigned)(m->cap - 1);
  }
  return -1;
}
// Returns the channel already holding `k`, or -1 after inserting `v` for it.
static int mapPut(Map *m, const char *k, int v) {
  size_t n = strlen(k);
  unsigned i;
  if (!n) return -2;
  i = hashKey(k, n) & (unsigned)(m->cap - 1);
  while (m->key[i]) {
    if (keyEq(m->key[i], k, n)) return m->val[i];
    i = (i + 1) & (unsigned)(m->cap - 1);
  }
  m->key[i] = k; m->val[i] = v;
  return -1;
}

static const char *findIn(const char *a, const char *b, const char *needle) {
  size_t n = strlen(needle);
  for (const char *p = a; p + n <= b; p++) {
    p = memchr(p, needle[0], (size_t)(b - p));
    if (!p || p + n > b) return NULL;
    if (!memcmp(p, needle, n)) return p;
  }
  return NULL;
}

// The text of the first <tag …>…</tag> inside [a, b), CDATA stripped, into the
// arena and entity-decoded. "" when absent. `max` caps what is kept: a synopsis
// is drawn in three lines, never two kilobytes.
static const char *element(IptvList *l, const char *a, const char *b, const char *tag,
                           size_t max) {
  char open[32], close[32];
  const char *s, *e;
  char *out;
  size_t n;
  snprintf(open, sizeof open, "<%s", tag);
  snprintf(close, sizeof close, "</%s", tag);
  for (s = a; (s = findIn(s, b, open)); s++) {
    char c = s[strlen(open)];
    if (c == '>' || isspace((unsigned char)c)) break;
  }
  if (!s) return "";
  s = memchr(s, '>', (size_t)(b - s));
  if (!s || s[-1] == '/') return "";
  s++;
  e = findIn(s, b, close);
  if (!e) return "";
  if (e - s >= 9 && !memcmp(s, "<![CDATA[", 9)) {
    const char *ce = findIn(s, e, "]]>");
    s += 9;
    if (ce) e = ce;
  }
  while (s < e && isspace((unsigned char)*s)) s++;
  while (e > s && isspace((unsigned char)e[-1])) e--;
  n = (size_t)(e - s);
  if (n > max) {
    n = max;
    // Never split a UTF-8 sequence.
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
  }
  out = (char *)arenaDup(l, s, n);
  if (out[0]) iptv_xml_unescape(out);
  return out;
}

// An attribute of the tag spanning [a, b), copied into dst.
static int tagAttr(const char *a, const char *b, const char *key, char *dst, size_t size) {
  const char *v; size_t n;
  if (!attr(a, b, key, &v, &n)) { dst[0] = 0; return 0; }
  if (n >= size) n = size - 1;
  memcpy(dst, v, n); dst[n] = 0;
  iptv_xml_unescape(dst);
  return 1;
}

// Whether [a, b) — the inside of a trailing "(…)" — is a quality tag rather than
// part of the name: a resolution ("720p", "1080i") or HD / SD / FHD / UHD / 4K.
static int qualityTag(const char *a, const char *b) {
  static const char *words[] = { "hd", "sd", "fhd", "uhd", "4k" };
  size_t n = (size_t)(b - a), d = 0;
  while (d < n && isdigit((unsigned char)a[d])) d++;
  if (d && d + 1 == n && (a[d] == 'p' || a[d] == 'P' || a[d] == 'i' || a[d] == 'I')) return 1;
  for (size_t i = 0; i < sizeof words / sizeof *words; i++)
    if (strlen(words[i]) == n && !strncasecmp(a, words[i], n)) return 1;
  return 0;
}

// A channel name without the tags playlists hang off its end, into `dst`.
// iptv-org names a third of its channels "00s Replay (720p) [Geo-blocked]", and
// the guides that carry them say "00s Replay": matched as written, those
// channels got no programmes. A square bracket is always a tag ("[Not 24/7]");
// a parenthesis only when qualityTag says so — "(US)" and "(East)" are what tell
// two channels apart. Returns 1 when something was cut and a name is left.
static int bareName(const char *s, char *dst, size_t size) {
  size_t n = strlen(s);
  int cut = 0;
  if (n >= size) n = size - 1;
  memcpy(dst, s, n); dst[n] = 0;
  for (;;) {
    char close, *o;
    while (n && isspace((unsigned char)dst[n - 1])) dst[--n] = 0;
    if (!n || ((close = dst[n - 1]) != ')' && close != ']')) break;
    dst[n - 1] = 0;
    o = strrchr(dst, close == ')' ? '(' : '[');
    dst[n - 1] = close;
    if (!o || o == dst || (close == ')' && !qualityTag(o + 1, dst + n - 1))) break;
    n = (size_t)(o - dst); dst[n] = 0; cut = 1;
  }
  return cut && n;
}

// The guide's <icon src="…"> for a channel whose playlist line has no tvg-logo:
// iptv-org's source lists carry none, and the guides that match them do. Every
// channel chained to `ch` that has no logo of its own gets it.
static void takeIcon(IptvList *l, int ch, const int *link, const char *a, const char *b) {
  const char *s = findIn(a, b, "<icon"), *e, *logo = NULL;
  char src[1024];
  if (!s || !(e = memchr(s, '>', (size_t)(b - s)))) return;
  if (!tagAttr(s, e, "src", src, sizeof src) || !src[0]) return;
  for (int k = ch; k >= 0; k = link[k])
    if (!l->ch[k].logo[0]) {
      if (!logo) logo = arenaDup(l, src, strlen(src));
      l->ch[k].logo = logo;
    }
}

static int byChannelStart(const void *x, const void *y) {
  const IptvProgramme *a = x, *b = y;
  if (a->channel != b->channel) return a->channel < b->channel ? -1 : 1;
  if (a->start != b->start) return a->start < b->start ? -1 : 1;
  return 0;
}

static int addProgramme(IptvList *l, int ch, long long start, long long stop,
                        const char *title, const char *desc, const char *cat) {
  IptvProgramme *p;
  if (!grow((void **)&l->pg, &l->capPg, l->nPg + 1, sizeof *l->pg)) return 0;
  p = &l->pg[l->nPg++];
  p->channel = ch; p->start = start; p->stop = stop;
  p->title = title; p->desc = desc; p->category = cat;
  return 1;
}

int iptv_parse_xmltv(IptvList *l, const char *xml, long long from, long long to) {
  Map byId, byName, byXml;
  int *link;
  const char *p, *xmlEnd;
  // The XMLTV channel ids seen in <channel> blocks, with the channel they map to.
  struct XmlId { const char *id; int ch; } *xmlIds = NULL;
  int nXml = 0, capXml = 0;

  l->nPg = 0;
  for (int i = 0; i < l->nCh; i++) l->ch[i].firstPg = l->ch[i].nPg = 0;
  if (!xml || !l->nCh) return 0;
  // Every scan below is bounded by this end rather than by the terminator: a
  // guide is tens of megabytes, and nothing should walk to its end per element.
  xmlEnd = xml + strlen(xml);

  link = malloc((size_t)l->nCh * sizeof *link);
  if (!link || !mapInit(&byId, l->nCh) || !mapInit(&byName, l->nCh * 2)) {
    free(link); return 0;
  }
  // Chains: a channel's tvg-id first, then its names. `link` chains the id
  // table; the name table keeps only the first channel per name.
  for (int i = 0; i < l->nCh; i++) {
    int first;
    link[i] = -1;
    if (l->ch[i].tvgId[0] && (first = mapPut(&byId, l->ch[i].tvgId, i)) >= 0) {
      int k = first;
      while (link[k] >= 0) k = link[k];
      link[k] = i;
    }
    mapPut(&byName, l->ch[i].tvgName, i);
    mapPut(&byName, l->ch[i].name, i);
  }
  // The same names with their tags cut, after every name as written: a channel
  // really called "News (HD)" keeps that key over another's "News HD (HD)".
  for (int i = 0; i < l->nCh; i++) {
    char bare[256];
    if (bareName(l->ch[i].tvgName, bare, sizeof bare))
      mapPut(&byName, arenaDup(l, bare, strlen(bare)), i);
    if (bareName(l->ch[i].name, bare, sizeof bare))
      mapPut(&byName, arenaDup(l, bare, strlen(bare)), i);
  }
  memset(&byXml, 0, sizeof byXml);

  // Pass 1: <channel id="…"><display-name>…</display-name></channel>. A channel
  // whose tvg-id matches the XMLTV id needs nothing from here; one that only
  // shares a NAME with it (playlists that carry no tvg-id at all) is matched
  // through the display names.
  for (p = xml; (p = findIn(p, xmlEnd, "<channel")); ) {
    const char *tagEnd = memchr(p, '>', (size_t)(xmlEnd - p)), *end;
    char id[256];
    int ch = -1;
    if (!tagEnd) break;
    if (!isspace((unsigned char)p[8])) { p = tagEnd; continue; }
    end = findIn(tagEnd, xmlEnd, "</channel>");
    if (!end) end = tagEnd;
    tagAttr(p, tagEnd, "id", id, sizeof id);
    if (id[0] && (ch = mapGet(&byId, id, strlen(id))) >= 0) {
      takeIcon(l, ch, link, tagEnd, end);
    } else if (id[0]) {
      for (const char *d = tagEnd; d && d < end; ) {
        const char *s = findIn(d, end, "<display-name");
        const char *e;
        if (!s || !(s = memchr(s, '>', (size_t)(end - s)))) break;
        s++;
        if (!(e = findIn(s, end, "</display-name"))) break;
        { char name[256];
          size_t n = (size_t)(e - s);
          if (n >= sizeof name) n = sizeof name - 1;
          memcpy(name, s, n); name[n] = 0;
          iptv_xml_unescape(name);
          ch = mapGet(&byName, name, strlen(name));
          if (ch < 0) {
            char bare[256];
            if (bareName(name, bare, sizeof bare)) ch = mapGet(&byName, bare, strlen(bare));
          } }
        if (ch >= 0) break;
        d = e;
      }
      if (ch >= 0 && grow((void **)&xmlIds, &capXml, nXml + 1, sizeof *xmlIds)) {
        xmlIds[nXml].id = arenaDup(l, id, strlen(id));
        xmlIds[nXml++].ch = ch;
      }
      if (ch >= 0) takeIcon(l, ch, link, tagEnd, end);
    }
    p = end;
  }
  if (nXml && mapInit(&byXml, nXml))
    for (int i = 0; i < nXml; i++) mapPut(&byXml, xmlIds[i].id, i);

  // Pass 2: the programmes inside the window.
  for (p = xml; (p = findIn(p, xmlEnd, "<programme")); ) {
    const char *tagEnd = memchr(p, '>', (size_t)(xmlEnd - p)), *end;
    char sStart[48], sStop[48], chId[256];
    long long start, stop;
    int ch;
    if (!tagEnd) break;
    end = findIn(tagEnd, xmlEnd, "</programme>");
    if (!end) break;
    tagAttr(p, tagEnd, "start", sStart, sizeof sStart);
    tagAttr(p, tagEnd, "stop", sStop, sizeof sStop);
    tagAttr(p, tagEnd, "channel", chId, sizeof chId);
    start = iptv_xmltv_time(sStart);
    stop = sStop[0] ? iptv_xmltv_time(sStop) : 0;
    p = end + 12;
    if (!start || start >= to || (stop && stop <= from)) continue;
    ch = mapGet(&byId, chId, strlen(chId));
    if (ch < 0 && byXml.cap) {
      int x = mapGet(&byXml, chId, strlen(chId));
      if (x >= 0) ch = xmlIds[x].ch;
    }
    if (ch < 0) ch = mapGet(&byName, chId, strlen(chId));
    if (ch < 0) continue;
    { const char *title = element(l, tagEnd, end, "title", 200);
      const char *desc  = element(l, tagEnd, end, "desc", 600);
      const char *cat   = element(l, tagEnd, end, "category", 60);
      if (!title[0]) title = "Untitled";
      // Every channel sharing the id gets the programme; the strings are shared.
      for (int k = ch; k >= 0; k = link[k])
        if (!addProgramme(l, k, start, stop, title, desc, cat)) goto done;
    }
  }
done:
  free(link);
  mapFree(&byId); mapFree(&byName); mapFree(&byXml);
  free(xmlIds);

  if (!l->nPg) return 0;
  qsort(l->pg, (size_t)l->nPg, sizeof *l->pg, byChannelStart);
  // Duplicates (two feeds merged by the provider) and missing stops: a
  // programme with no stop ends where the next one starts, or after half an hour.
  { int w = 0;
    for (int r = 0; r < l->nPg; r++) {
      IptvProgramme *c = &l->pg[r];
      if (w && l->pg[w - 1].channel == c->channel && l->pg[w - 1].start == c->start) continue;
      l->pg[w++] = *c;
    }
    l->nPg = w; }
  for (int i = 0; i < l->nPg; i++) {
    IptvProgramme *c = &l->pg[i];
    int hasNext = i + 1 < l->nPg && l->pg[i + 1].channel == c->channel;
    if (c->stop <= c->start) c->stop = hasNext ? l->pg[i + 1].start : c->start + 1800;
  }
  for (int i = 0; i < l->nPg; i++) {
    IptvChannel *c = &l->ch[l->pg[i].channel];
    if (!c->nPg) c->firstPg = i;
    c->nPg++;
  }
  return l->nPg;
}

int iptv_programme_at(const IptvList *l, int ch, long long t) {
  int lo, hi, best = -1;
  if (!l || ch < 0 || ch >= l->nCh || !l->ch[ch].nPg) return -1;
  lo = l->ch[ch].firstPg; hi = lo + l->ch[ch].nPg - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    if (l->pg[mid].start <= t) { best = mid; lo = mid + 1; }
    else hi = mid - 1;
  }
  return (best >= 0 && l->pg[best].stop > t) ? best : -1;
}

int iptv_programme_after(const IptvList *l, int ch, long long t) {
  int lo, hi, best = -1;
  if (!l || ch < 0 || ch >= l->nCh || !l->ch[ch].nPg) return -1;
  lo = l->ch[ch].firstPg; hi = lo + l->ch[ch].nPg - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    if (l->pg[mid].start >= t) { best = mid; hi = mid - 1; }
    else lo = mid + 1;
  }
  return best;
}

// A gzipped guide inflates ten to twenty times. Past this it is not a guide.
#define IPTV_GUNZIP_MAX       (512L << 20)

// --- gzip, through the device's own libz ------------------------------------------
// zlib's z_stream, field for field. The layout has not changed since 1.0 and
// inflateInit2_ checks sizeof against its own, so a mismatch fails loudly
// rather than corrupting anything.
typedef struct {
  const unsigned char *next_in; unsigned avail_in; unsigned long total_in;
  unsigned char *next_out; unsigned avail_out; unsigned long total_out;
  const char *msg; void *state;
  void *zalloc, *zfree, *opaque;
  int data_type; unsigned long adler, reserved;
} ZStream;
typedef int (*ZInit)(ZStream *, int, const char *, int);
typedef int (*ZInflate)(ZStream *, int);
typedef int (*ZEnd)(ZStream *);
typedef int (*ZReset)(ZStream *);

char *iptv_gunzip(const char *in, long n, long *outN) {
  static void *lib;
  static ZInit zInit; static ZInflate zInflate; static ZEnd zEnd; static ZReset zReset;
  ZStream z;
  char *out = NULL;
  long cap, len = 0;
  int rc;
  if (outN) *outN = 0;
  if (!in || n < 2) return NULL;
  // gzip's magic, or a zlib header (CMF 0x78).
  if (!((unsigned char)in[0] == 0x1f && (unsigned char)in[1] == 0x8b) &&
      (unsigned char)in[0] != 0x78) return NULL;
  if (!lib) {
    lib = dlopen("libz.so.1", RTLD_NOW);
    if (!lib) lib = dlopen("libz.so", RTLD_NOW);
    if (!lib) lib = dlopen("libz.1.dylib", RTLD_NOW);   // Mac
    if (!lib) lib = dlopen("libz.dylib", RTLD_NOW);
    if (!lib) { fprintf(stderr, "[iptv] no libz on this device: gzipped guides unavailable\n"); return NULL; }
    zInit = (ZInit)dlsym(lib, "inflateInit2_");
    zInflate = (ZInflate)dlsym(lib, "inflate");
    zEnd = (ZEnd)dlsym(lib, "inflateEnd");
    zReset = (ZReset)dlsym(lib, "inflateReset");
  }
  if (!zInit || !zInflate || !zEnd || !zReset) return NULL;
  memset(&z, 0, sizeof z);
  // 15 + 32: detect gzip or zlib from the header.
  if (zInit(&z, 15 + 32, "1.2.11", (int)sizeof z) != 0) return NULL;
  cap = n * 8 + 4096;
  if (cap > IPTV_GUNZIP_MAX) cap = IPTV_GUNZIP_MAX;
  out = malloc((size_t)cap + 1);
  if (!out) { zEnd(&z); return NULL; }
  z.next_in = (const unsigned char *)in;
  z.avail_in = (unsigned)n;
  for (;;) {
    if (len == cap) {
      char *bigger;
      if (cap >= IPTV_GUNZIP_MAX) { rc = -1; break; }
      cap = cap * 2 > IPTV_GUNZIP_MAX ? IPTV_GUNZIP_MAX : cap * 2;
      bigger = realloc(out, (size_t)cap + 1);
      if (!bigger) { rc = -1; break; }
      out = bigger;
    }
    z.next_out = (unsigned char *)out + len;
    z.avail_out = (unsigned)(cap - len);
    rc = zInflate(&z, 0);
    len = cap - (long)z.avail_out;
    if (rc == 1) {                       // Z_STREAM_END
      // Some panels concatenate gzip members; carry on into the next one.
      if (z.avail_in > 0 && zReset(&z) == 0) continue;
      rc = 0; break;
    }
    if (rc != 0 && rc != -5) break;      // neither Z_OK nor Z_BUF_ERROR
    if (rc == -5 && z.avail_in == 0) { rc = 0; break; }   // truncated: keep what came
  }
  zEnd(&z);
  if (rc != 0 || !len) { free(out); return NULL; }
  out[len] = 0;
  if (outN) *outN = len;
  return out;
}

// --- Xtream Codes' JSON API ----------------------------------------------------------
// A small reader of its own rather than js.h: that one turns \uXXXX into a
// space, which is right for its synopses and wrong for a channel called
// "Türkiye 1".
static const char *jwhite(const char *p) {
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
  return p;
}

static unsigned jhex4(const char *p) {
  unsigned v = 0;
  for (int i = 0; i < 4; i++) {
    int c = p[i], d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                    : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    if (d < 0) return 0xFFFFFFFFu;
    v = v * 16 + (unsigned)d;
  }
  return v;
}

static size_t jutf8(unsigned cp, char *o) {
  if (cp < 0x80) { o[0] = (char)cp; return 1; }
  if (cp < 0x800) { o[0] = (char)(0xC0 | cp >> 6); o[1] = (char)(0x80 | (cp & 63)); return 2; }
  if (cp < 0x10000) {
    o[0] = (char)(0xE0 | cp >> 12); o[1] = (char)(0x80 | (cp >> 6 & 63));
    o[2] = (char)(0x80 | (cp & 63)); return 3;
  }
  o[0] = (char)(0xF0 | cp >> 18); o[1] = (char)(0x80 | (cp >> 12 & 63));
  o[2] = (char)(0x80 | (cp >> 6 & 63)); o[3] = (char)(0x80 | (cp & 63)); return 4;
}

// The string at `p` (on its opening quote), decoded into dst and cut to fit on
// a character boundary. Returns the position after the closing quote, NULL
// when malformed.
static const char *jstring(const char *p, char *dst, size_t n) {
  size_t k = 0;
  int full = 0;
  if (*p != '"') return NULL;
  for (p++; *p && *p != '"'; ) {
    char tmp[4];
    size_t w = 1;
    if (*p == '\\') {
      p++;
      switch (*p) {
        case 'n': case 'r': case 't': case 'b': case 'f': tmp[0] = ' '; p++; break;
        case 'u': {
          unsigned cp = jhex4(p + 1);
          if (cp == 0xFFFFFFFFu) return NULL;
          p += 5;
          if (cp >= 0xD800 && cp < 0xDC00 && p[0] == '\\' && p[1] == 'u') {
            unsigned lo = jhex4(p + 2);
            if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); p += 6; }
          }
          if (cp >= 0xD800 && cp < 0xE000) cp = 0xFFFD;   // a lone half
          if (cp < 0x20) cp = ' ';
          w = jutf8(cp, tmp);
          break;
        }
        case 0: return NULL;
        default: tmp[0] = *p++;   // \" \\ \/
      }
    } else {
      tmp[0] = *p++;
    }
    // Whole characters only, and nothing after the first that does not fit:
    // half a character would show as garbage.
    if (dst && !full) { if (k + w < n) { memcpy(dst + k, tmp, w); k += w; } else full = 1; }
  }
  if (*p != '"') return NULL;
  if (dst && n) dst[k] = 0;
  return p + 1;
}

// Past any value at `p`. NULL when malformed.
static const char *jskip(const char *p) {
  p = jwhite(p);
  if (*p == '"') return jstring(p, NULL, 0);
  if (*p == '{' || *p == '[') {
    int depth = 0;
    for (; *p; p++) {
      if (*p == '"') { if (!(p = jstring(p, NULL, 0))) return NULL; p--; continue; }
      if (*p == '{' || *p == '[') depth++;
      else if ((*p == '}' || *p == ']') && --depth == 0) return p + 1;
    }
    return NULL;
  }
  while (*p && *p != ',' && *p != '}' && *p != ']') p++;
  return p;
}

// A scalar at `p` as text: a string decoded, a number or literal as written
// ("null" and "false" as empty). Returns the position after it.
static const char *jscalar(const char *p, char *dst, size_t n) {
  const char *s;
  p = jwhite(p);
  dst[0] = 0;
  if (*p == '"') return jstring(p, dst, n);
  if (*p == '{' || *p == '[') return jskip(p);
  s = p;
  while (*p && *p != ',' && *p != '}' && *p != ']' && !isspace((unsigned char)*p)) p++;
  if (!(p - s == 4 && !strncmp(s, "null", 4)) && !(p - s == 5 && !strncmp(s, "false", 5))) {
    size_t k = (size_t)(p - s) < n - 1 ? (size_t)(p - s) : n - 1;
    memcpy(dst, s, k); dst[k] = 0;
  }
  return p;
}

// Calls `fn` for every object in the root array, with its members as a flat
// list of key/value pairs (scalars only; nested values are skipped).
#define XT_FIELDS 8
typedef struct { const char *key; char *val; size_t n; } XtField;

static int jeach(const char *json, XtField *f, int nf, void (*fn)(void *, XtField *), void *u) {
  const char *p = jwhite(json);
  int count = 0;
  if (*p != '[') return -1;
  p = jwhite(p + 1);
  while (*p && *p != ']') {
    if (*p == '{') {
      for (int i = 0; i < nf; i++) f[i].val[0] = 0;
      p = jwhite(p + 1);
      while (*p && *p != '}') {
        char key[40];
        int hit = -1;
        if (!(p = jstring(p, key, sizeof key))) return count;
        p = jwhite(p);
        if (*p != ':') return count;
        p = jwhite(p + 1);
        for (int i = 0; i < nf; i++) if (!strcmp(key, f[i].key)) { hit = i; break; }
        p = hit >= 0 ? jscalar(p, f[hit].val, f[hit].n) : jskip(p);
        if (!p) return count;
        p = jwhite(p);
        if (*p == ',') p = jwhite(p + 1);
      }
      if (*p != '}') return count;
      p++;
      fn(u, f);
      count++;
    } else if (!(p = jskip(p))) {
      return count;
    }
    p = jwhite(p);
    if (*p == ',') p = jwhite(p + 1);
  }
  return count;
}

typedef struct { char *p; size_t n, cap; int bad; } XtBuf;
static void xput(XtBuf *b, const char *s, size_t k) {
  if (b->bad) return;
  if (b->n + k + 1 > b->cap) {
    size_t cap = b->cap ? b->cap : 65536;
    char *q;
    while (b->n + k + 1 > cap) cap *= 2;
    if (!(q = realloc(b->p, cap))) { b->bad = 1; return; }
    b->p = q; b->cap = cap;
  }
  memcpy(b->p + b->n, s, k);
  b->n += k;
  b->p[b->n] = 0;
}
static void xputs(XtBuf *b, const char *s) { xput(b, s, strlen(s)); }
// An attribute value or a name: no quote to end the attribute early, no line
// break to end the entry, and in a name no comma, since the display name is
// what follows the last one.
static void xputClean(XtBuf *b, const char *s, int name) {
  for (; *s; s++) {
    if (*s == '"') xputs(b, "'");
    else if (*s == '\n' || *s == '\r' || *s == '\t') xputs(b, " ");
    else if (name && *s == ',') xputs(b, "\xE2\x80\x9A");   // U+201A, a low comma
    else xput(b, s, 1);
  }
}

typedef struct { char **id, **name; int n, cap; } XtCats;
static void addCat(void *u, XtField *f) {
  XtCats *c = u;
  if (!f[0].val[0]) return;
  if (c->n == c->cap) {
    int cap = c->cap ? c->cap * 2 : 64;
    char **a = realloc(c->id, cap * sizeof *a), **b;
    if (!a) return;
    c->id = a;
    if (!(b = realloc(c->name, cap * sizeof *b))) return;
    c->name = b; c->cap = cap;
  }
  c->id[c->n] = strdup(f[0].val);
  c->name[c->n] = strdup(f[1].val);
  if (c->id[c->n] && c->name[c->n]) c->n++;
  else { free(c->id[c->n]); free(c->name[c->n]); }
}

typedef struct { XtBuf out; XtCats cats; const char *base, *user, *pass, *ext; } XtJob;
enum { XS_NAME, XS_ID, XS_ICON, XS_EPG, XS_CAT, XS_NUM, XS_ARCHIVE, XS_ARCHIVE_DAYS };
static void addStream(void *u, XtField *f) {
  XtJob *j = u;
  const char *group = "";
  int num = atoi(f[XS_NUM].val), days = atoi(f[XS_ARCHIVE_DAYS].val);
  char line[64];
  if (!f[XS_ID].val[0] || !f[XS_NAME].val[0]) return;
  for (int i = 0; i < j->cats.n; i++)
    if (!strcmp(j->cats.id[i], f[XS_CAT].val)) { group = j->cats.name[i]; break; }
  xputs(&j->out, "#EXTINF:-1 tvg-id=\"");
  xputClean(&j->out, f[XS_EPG].val, 0);
  xputs(&j->out, "\" tvg-name=\"");
  xputClean(&j->out, f[XS_NAME].val, 0);
  xputs(&j->out, "\" tvg-logo=\"");
  xputClean(&j->out, f[XS_ICON].val, 0);
  xputs(&j->out, "\" group-title=\"");
  xputClean(&j->out, group, 0);
  xputs(&j->out, "\"");
  if (num > 0) { snprintf(line, sizeof line, " tvg-chno=\"%d\"", num); xputs(&j->out, line); }
  if (atoi(f[XS_ARCHIVE].val) && days > 0) {
    snprintf(line, sizeof line, " catchup-days=\"%d\"", days);
    xputs(&j->out, line);
  }
  xputs(&j->out, ",");
  xputClean(&j->out, f[XS_NAME].val, 1);
  xputs(&j->out, "\n");
  xputs(&j->out, j->base); xputs(&j->out, "/live/");
  xputs(&j->out, j->user); xputs(&j->out, "/");
  xputs(&j->out, j->pass); xputs(&j->out, "/");
  xputClean(&j->out, f[XS_ID].val, 0);
  xputs(&j->out, "."); xputs(&j->out, j->ext); xputs(&j->out, "\n");
}

char *iptv_xtream_m3u(const char *streams, const char *categories, const char *base,
                      const char *user, const char *pass, const char *ext) {
  XtJob j;
  char v[XT_FIELDS][512];
  int n;
  memset(&j, 0, sizeof j);
  j.base = base; j.user = user; j.pass = pass; j.ext = ext && *ext ? ext : "m3u8";
  if (!streams) return NULL;
  if (categories) {
    XtField cf[2] = { { "category_id", v[0], sizeof v[0] }, { "category_name", v[1], sizeof v[1] } };
    jeach(categories, cf, 2, addCat, &j.cats);
  }
  { XtField sf[XT_FIELDS] = {
      [XS_NAME] = { "name", v[0], sizeof v[0] },
      [XS_ID] = { "stream_id", v[1], sizeof v[1] },
      [XS_ICON] = { "stream_icon", v[2], sizeof v[2] },
      [XS_EPG] = { "epg_channel_id", v[3], sizeof v[3] },
      [XS_CAT] = { "category_id", v[4], sizeof v[4] },
      [XS_NUM] = { "num", v[5], sizeof v[5] },
      [XS_ARCHIVE] = { "tv_archive", v[6], sizeof v[6] },
      [XS_ARCHIVE_DAYS] = { "tv_archive_duration", v[7], sizeof v[7] },
    };
    xputs(&j.out, "#EXTM3U\n");
    n = jeach(streams, sf, XT_FIELDS, addStream, &j);
  }
  for (int i = 0; i < j.cats.n; i++) { free(j.cats.id[i]); free(j.cats.name[i]); }
  free(j.cats.id); free(j.cats.name);
  if (n < 0 || j.out.bad) { free(j.out.p); return NULL; }
  return j.out.p;
}
