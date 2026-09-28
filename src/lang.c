#include "lang.h"
#include <string.h>
#include <strings.h>

// The FIRST code of each is the one lang_code hands out, so it names the whole
// language: "por", not Brazil's "pob".
// Portuguese keeps Brazil inside it, as the old group did: a viewer who wants
// Portuguese takes either, and the sheet still names the two apart.
static const struct { const char *name; const char *codes[8]; } LANGS[] = {
  { "Portuguese", { "por", "pob", "pt-br", "pt_br", "ptb", "br", "pt", NULL } },
  { "English",    { "eng", "en", "en-us", "en_us", "en-gb", "en_gb", NULL } },
  { "Spanish",    { "spa", "es", "esp", "es-419", "es-es", "es-mx", NULL } },
  { "French",     { "fre", "fra", "fr", "fr-fr", "fr-ca", NULL } },
  { "German",     { "ger", "deu", "de", NULL } },
  { "Italian",    { "ita", "it", NULL } },
  { "Dutch",      { "dut", "nld", "nl", NULL } },
  { "Polish",     { "pol", "pl", NULL } },
  { "Swedish",    { "swe", "sv", NULL } },
  { "Danish",     { "dan", "da", NULL } },
  { "Norwegian",  { "nor", "no", "nob", "nb", "nno", "nn", NULL } },
  { "Finnish",    { "fin", "fi", NULL } },
  { "Russian",    { "rus", "ru", NULL } },
  { "Ukrainian",  { "ukr", "uk", NULL } },
  { "Czech",      { "cze", "ces", "cs", NULL } },
  { "Hungarian",  { "hun", "hu", NULL } },
  { "Romanian",   { "rum", "ron", "ro", NULL } },
  { "Greek",      { "gre", "ell", "el", NULL } },
  { "Turkish",    { "tur", "tr", NULL } },
  { "Arabic",     { "ara", "ar", NULL } },
  { "Hebrew",     { "heb", "he", "iw", NULL } },
  { "Hindi",      { "hin", "hi", NULL } },
  { "Japanese",   { "jpn", "ja", NULL } },
  { "Korean",     { "kor", "ko", NULL } },
  { "Chinese",    { "chi", "zho", "zh", "zh-cn", "zh-tw", "chs", "cht", NULL } },
  { "Thai",       { "tha", "th", NULL } },
  { "Vietnamese", { "vie", "vi", NULL } },
  { "Indonesian", { "ind", "id", NULL } },
};
#define N_LANGS (int)(sizeof LANGS / sizeof *LANGS)
typedef char lang_count_matches[(N_LANGS == LANG_COUNT) ? 1 : -1];

int lang_n(void) { return N_LANGS; }

const char *lang_name(int i) { return i >= 0 && i < N_LANGS ? LANGS[i].name : ""; }

static int exact(const char *code) {
  int i, k;
  for (i = 0; i < N_LANGS; i++)
    for (k = 0; LANGS[i].codes[k]; k++)
      if (!strcasecmp(code, LANGS[i].codes[k])) return i;
  return -1;
}

int lang_of(const char *code) {
  char base[8];
  size_t n;
  int i;
  if (!code || !*code) return -1;
  if ((i = exact(code)) >= 0) return i;
  // "pt-PT", "de_AT": the region is not in the table, the language is.
  n = strcspn(code, "-_");
  if (!code[n] || n == 0 || n >= sizeof base) return -1;
  memcpy(base, code, n);
  base[n] = 0;
  return exact(base);
}

const char *lang_code(int i) { return i >= 0 && i < N_LANGS ? LANGS[i].codes[0] : ""; }

// A name as a whole word, case-blind: "English Subtitles [VobSub]" names
// English, "Englishman" does not.
static int wordAt(const char *s, const char *w) {
  size_t n = strlen(w);
  const char *p;
  for (p = s; *p; p++) {
    if (strncasecmp(p, w, n)) continue;
    if (p > s && ((p[-1] | 32) >= 'a' && (p[-1] | 32) <= 'z')) continue;
    if ((p[n] | 32) >= 'a' && (p[n] | 32) <= 'z') continue;
    return 1;
  }
  return 0;
}

int lang_in_text(const char *s) {
  int i, found = -1;
  if (!s || !*s) return -1;
  for (i = 0; i < N_LANGS; i++) {
    if (!wordAt(s, LANGS[i].name)) continue;
    if (found >= 0) return -1;         // "English to Japanese": no single answer
    found = i;
  }
  return found;
}
