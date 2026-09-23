#include "lang.h"
#include <string.h>
#include <strings.h>

// Portuguese keeps Brazil inside it, as the old group did: a viewer who wants
// Portuguese takes either, and the sheet still names the two apart.
static const struct { const char *name; const char *codes[8]; } LANGS[] = {
  { "Portuguese", { "pob", "pt-br", "pt_br", "ptb", "br", "por", "pt", NULL } },
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
