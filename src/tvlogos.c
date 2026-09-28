// Channel logos from tv-logo/tv-logos. See tvlogos.h.
#include "tvlogos.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TVL_BASE "https://raw.githubusercontent.com/tv-logo/tv-logos/"
#define TVL_MAX_SUFFIXES 128

typedef struct { const char *slug, *suffix, *path; } TvlEntry;

static char dir[512];
static int loaded;
static char *buf;
static TvlEntry *ent;
static int nEnt;
static const char *commit = "";
static const char *suffixes[TVL_MAX_SUFFIXES];
static int nSuffixes;

void tvlogos_dir(const char *dirArt) {
  snprintf(dir, sizeof dir, "%s", dirArt ? dirArt : "");
}

static int knownSuffix(const char *s) {
  for (int i = 0; i < nSuffixes; i++) if (!strcmp(suffixes[i], s)) return 1;
  return 0;
}

// The file, split in place: "@commit" then "slug\tsuffix\tpath" lines, sorted.
static void load(void) {
  char p[640];
  FILE *f;
  long size;
  int cap = 0;
  char *s, *e;
  loaded = 1;
  if (!dir[0]) return;
  snprintf(p, sizeof p, "%s/tv-logos.txt", dir);
  if (!(f = fopen(p, "rb"))) { printf("[tvlogos] no index at %s\n", p); return; }
  fseek(f, 0, SEEK_END);
  size = ftell(f);
  fseek(f, 0, SEEK_SET);
  buf = size > 0 ? malloc((size_t)size + 1) : NULL;
  if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size) { fclose(f); free(buf); buf = NULL; return; }
  fclose(f);
  buf[size] = 0;
  for (s = buf; *s; s++) if (*s == '\n') cap++;
  ent = malloc(sizeof *ent * (size_t)(cap + 1));
  if (!ent) return;
  for (s = buf; *s; s = e) {
    char *t1, *t2;
    e = strchr(s, '\n');
    if (e) *e++ = 0; else e = s + strlen(s);
    if (*s == '@') { commit = s + 1; continue; }
    if (!(t1 = strchr(s, '\t')) || !(t2 = strchr(t1 + 1, '\t'))) continue;
    *t1 = 0; *t2 = 0;
    ent[nEnt].slug = s; ent[nEnt].suffix = t1 + 1; ent[nEnt].path = t2 + 1;
    if (!knownSuffix(t1 + 1) && nSuffixes < TVL_MAX_SUFFIXES) suffixes[nSuffixes++] = t1 + 1;
    nEnt++;
  }
  printf("[tvlogos] %d logos @%.10s\n", nEnt, commit);
}

// The first entry for `slug`, and how many there are.
static int range(const char *slug, int *count) {
  int lo = 0, hi = nEnt, first;
  while (lo < hi) {
    int mid = (lo + hi) / 2;
    if (strcmp(ent[mid].slug, slug) < 0) lo = mid + 1; else hi = mid;
  }
  first = lo;
  while (lo < nEnt && !strcmp(ent[lo].slug, slug)) lo++;
  *count = lo - first;
  return first;
}

// Must match STOP in tools/build-tv-logos.mjs.
static int stopWord(const char *w) {
  static const char *STOP[] = { "hd", "fhd", "uhd", "sd", "4k", "8k", "raw", "hevc", "h265", "h264",
                                "50fps", "60fps", "vip", "lq", "hq", "eon", "backup" };
  for (size_t i = 0; i < sizeof STOP / sizeof *STOP; i++) if (!strcmp(w, STOP[i])) return 1;
  return 0;
}

// "UK: SKY SPORTS F1 ᴿᴬᵂ HD" -> prefix "UK", slug "sky-sports-f1". Words are
// ASCII letters and digits; anything else parts them (the superscript tags
// included), & is "and" and + is "plus", as in the repo's file names, and
// whatever is in brackets is a note about the feed, not the name.
static void normalise(const char *name, char *prefix, size_t np, char *slug, size_t ns) {
  const char *p = name, *sep;
  size_t k = 0;
  int depth = 0;
  prefix[0] = 0;
  while (*p == ' ') p++;
  sep = strpbrk(p, ":|");
  if (sep) {
    const char *a = p, *b = sep;
    while (b > a && b[-1] == ' ') b--;
    if (b - a >= 1 && b - a <= 5 && (size_t)(b - a) < np) {
      int ok = 1;
      for (const char *q = a; q < b; q++) if (!isalnum((unsigned char)*q)) ok = 0;
      if (ok) {
        for (size_t i = 0; i < (size_t)(b - a); i++) prefix[i] = (char)toupper((unsigned char)a[i]);
        prefix[b - a] = 0;
        p = sep + 1;
      }
    }
  }
  slug[0] = 0;
  while (*p) {
    char w[32];
    size_t n = 0;
    if (*p == '(' || *p == '[') { depth++; p++; continue; }
    if (*p == ')' || *p == ']') { if (depth) depth--; p++; continue; }
    if (depth) { p++; continue; }
    if (*p == '&' || *p == '+') {
      snprintf(w, sizeof w, "%s", *p == '&' ? "and" : "plus");
      p++;
    } else if ((unsigned char)*p < 128 && isalnum((unsigned char)*p)) {
      while ((unsigned char)*p < 128 && isalnum((unsigned char)*p)) {
        if (n + 1 < sizeof w) w[n++] = (char)tolower((unsigned char)*p);
        p++;
      }
      w[n] = 0;
    } else { p++; continue; }
    if (stopWord(w)) continue;
    if (k && k + 1 < ns) slug[k++] = '-';
    for (size_t i = 0; w[i] && k + 1 < ns; i++) slug[k++] = w[i];
    slug[k] = 0;
  }
}

// The suffixes a prefix allows, in order, "int" always last. Most prefixes
// are the country's code as the repo spells it; the rest are the playlist's
// own words for a country or a language.
static int allowed(const char *prefix, const char **out, int max) {
  static const struct { const char *pre, *suf[6]; } MAP[] = {
    { "UK", { "uk" } }, { "GB", { "uk" } }, { "ENG", { "uk" } }, { "NOW", { "uk" } },
    { "ZG", { "nl" } }, { "MEO", { "pt" } }, { "ARG", { "ar", "lam" } },
    { "AR", { "mea", "ae", "sa", "qa", "eg", "lb" } }, { "ARB", { "mea", "ae", "sa", "qa", "eg", "lb" } },
    { "LAT", { "lam" } }, { "LATAM", { "lam" } },
    { "MX", { "mx", "lam" } }, { "CO", { "co", "lam" } }, { "CL", { "cl", "lam" } }, { "PE", { "pe", "lam" } },
  };
  int n = 0;
  char low[8];
  for (size_t i = 0; i < sizeof MAP / sizeof *MAP; i++)
    if (!strcmp(prefix, MAP[i].pre)) {
      for (int j = 0; j < 6 && MAP[i].suf[j] && n < max - 1; j++) out[n++] = MAP[i].suf[j];
      out[n++] = "int";
      return n;
    }
  // A code the repo has files for is a country; anything else (VIP, GOLD, TV,
  // a provider's group) says nothing about one.
  if (strlen(prefix) < 2 || strlen(prefix) >= sizeof low) return 0;
  for (size_t i = 0; prefix[i]; i++) low[i] = (char)tolower((unsigned char)prefix[i]);
  low[strlen(prefix)] = 0;
  for (int i = 0; i < nSuffixes; i++)
    if (!strcmp(suffixes[i], low)) { out[n++] = suffixes[i]; out[n++] = "int"; return n; }
  return 0;
}

static const TvlEntry *pick(const char *slug, const char **allow, int nAllow) {
  int count, first = range(slug, &count);
  if (!count) return NULL;
  if (nAllow) {
    for (int a = 0; a < nAllow; a++)
      for (int i = first; i < first + count; i++)
        if (!strcmp(ent[i].suffix, allow[a])) return &ent[i];
    return NULL;
  }
  // No country: one that exists in one place only, else the international one.
  if (count == 1) return &ent[first];
  for (int i = first; i < first + count; i++) if (!strcmp(ent[i].suffix, "int")) return &ent[i];
  return NULL;
}

// "bbc-1" is bbc-one in the repo; a lone digit becomes its word.
static void spellDigits(const char *slug, char *out, size_t n) {
  static const char *W[] = { "zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine" };
  size_t k = 0;
  const char *p = slug;
  out[0] = 0;
  while (*p && k + 1 < n) {
    const char *e = strchr(p, '-');
    size_t len = e ? (size_t)(e - p) : strlen(p);
    if (len == 1 && isdigit((unsigned char)*p)) k += (size_t)snprintf(out + k, n - k, "%s", W[*p - '0']);
    else k += (size_t)snprintf(out + k, n - k, "%.*s", (int)len, p);
    if (k >= n) { out[n - 1] = 0; return; }
    p += len;
    if (*p == '-') { if (k + 1 < n) out[k++] = '-'; p++; }
    out[k] = 0;
  }
}

// "npo-1" is npo1 in the repo: a number joins the word before it.
static void joinDigits(const char *slug, char *out, size_t n) {
  size_t k = 0;
  for (const char *p = slug; *p && k + 1 < n; p++) {
    if (*p == '-' && p > slug && isalpha((unsigned char)p[-1]) && isdigit((unsigned char)p[1])) {
      const char *q = p + 1;
      while (isdigit((unsigned char)*q)) q++;
      if (!*q || *q == '-') continue;
    }
    out[k++] = *p;
  }
  out[k] = 0;
}

int tvlogos_find(const char *name, char *out, size_t n) {
  char prefix[8], slug[160], alt[200];
  const char *allow[8];
  const TvlEntry *e;
  int nAllow;
  size_t len;
  out[0] = 0;
  if (!loaded) load();
  if (!nEnt || !name) return 0;
  normalise(name, prefix, sizeof prefix, slug, sizeof slug);
  if (!slug[0]) return 0;
  nAllow = prefix[0] ? allowed(prefix, allow, 8) : 0;
  e = pick(slug, allow, nAllow);
  if (!e) { spellDigits(slug, alt, sizeof alt); if (strcmp(alt, slug)) e = pick(alt, allow, nAllow); }
  if (!e) { joinDigits(slug, alt, sizeof alt); if (strcmp(alt, slug)) e = pick(alt, allow, nAllow); }
  // "Ziggo Sport 1" is the repo's ziggo-sport: the first of a numbered set
  // often has no number.
  if (!e && (len = strlen(slug)) > 2 && !strcmp(slug + len - 2, "-1")) {
    slug[len - 2] = 0;
    e = pick(slug, allow, nAllow);
  }
  if (!e) return 0;
  snprintf(out, n, TVL_BASE "%s/countries/%s.png", commit, e->path);
  return 1;
}
