// The languages the player can PREFER, and how a track's code maps onto one.
//
// Subtitle and audio preferences used to be two hard-wired groups — Portuguese
// and English, the app's first owner's pair — spread over addons.c and tracks.c.
// This is the one table both read now. The ORDER is part of the contract: index
// 0 is Portuguese and 1 is English, which is what the Subtitles row stored as its
// values 2 and 3 before this table existed, and settings.c spells the names out
// again in this same order (it checks the count at compile time).
//
// Matching is by code, case-insensitive: the two- and three-letter ISO 639 codes
// (both 639-2 spellings, "ger" and "deu"), the regional forms addons send
// ("pt-br", "es-419"), and as a last resort the part before a '-' or '_'.
#ifndef NV_LANG_H
#define NV_LANG_H

#define LANG_PORTUGUESE 0
#define LANG_ENGLISH    1
// How many there are. A constant, so settings.c can check its name lists
// against it at compile time.
#define LANG_COUNT      28

int         lang_n(void);
const char *lang_name(int index);        // "Spanish"; "" outside the table
// The table index for a track's language code, or -1 when it is not one of them.
int         lang_of(const char *code);
// A code that names language `index` ("eng", "por"); "" outside the table.
const char *lang_code(int index);
// The one language a piece of text names as a word — a track's title, say
// "English Subtitles [VobSub]" — or -1 when it names none or more than one.
int         lang_in_text(const char *text);

#endif
