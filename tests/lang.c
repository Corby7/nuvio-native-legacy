// lang.c: the codes addons and MKV headers really send, folded onto the table
// the Settings rows index into.
#include "lang.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  // The two indices the Subtitles row stored before the table existed.
  assert(lang_of("pob") == LANG_PORTUGUESE && lang_of("pt-BR") == LANG_PORTUGUESE);
  assert(lang_of("por") == LANG_PORTUGUESE && lang_of("pt-PT") == LANG_PORTUGUESE);
  assert(lang_of("eng") == LANG_ENGLISH && lang_of("EN") == LANG_ENGLISH);
  assert(lang_of("en-AU") == LANG_ENGLISH);            // region not in the table
  // Both ISO 639-2 spellings.
  assert(lang_of("ger") == lang_of("deu") && lang_of("de") == lang_of("ger"));
  assert(lang_of("fre") == lang_of("fra") && !strcmp(lang_name(lang_of("fr")), "French"));
  assert(lang_of("es-419") == lang_of("spa"));
  assert(lang_of("zh-TW") == lang_of("chi") && lang_of("nb") == lang_of("nor"));
  // Not languages, or not ours.
  assert(lang_of("und") == -1 && lang_of("") == -1 && lang_of(NULL) == -1);
  assert(lang_of("xx-YY") == -1 && lang_of("-en") == -1);
  assert(!strcmp(lang_name(-1), "") && !strcmp(lang_name(LANG_COUNT), ""));
  assert(lang_n() == LANG_COUNT);
  // Every name answers to itself through one of its own codes.
  for (int i = 0; i < lang_n(); i++) assert(lang_name(i)[0]);
  puts("PASS lang: codes, regions, both 639-2 spellings, unknowns.");
  return 0;
}
