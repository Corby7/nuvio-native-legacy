// The source picked by hand, per title and per profile. See sourcepref.h for
// the match order and why the key cannot be the url.
#include "sourcepref.h"
#include "data.h"
#include "profiles.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static SourcePref table[SOURCEPREF_MAX];
static int nTab;
static int loadedProfile = -1;   // -1: nothing read yet

// ONE FILE PER PROFILE: on a living-room TV the dubbed source one person picked
// is not the one the next person wants.
#define SOURCEPREF_PROFILES 16

static const char *fileOf(int profile) {
  static char name[48];
  snprintf(name, sizeof name, "sourcepref-p%d.txt", profile);
  return name;
}

// --- the audio signature ----------------------------------------------------
//
// A FLAG IS TWO LETTERS, which is why it is decoded rather than compared as
// text: a flag is two "regional indicator" symbols, U+1F1E6..U+1F1FF, in UTF-8
// F0 9F 87 A6..BF. The last byte minus 0xA6 gives the ASCII letter.
static int flag(const unsigned char *p, char *dst) {
  if (p[0] != 0xF0 || p[1] != 0x9F || p[2] != 0x87) return 0;
  if (p[3] < 0xA6 || p[3] > 0xBF) return 0;
  if (p[4] != 0xF0 || p[5] != 0x9F || p[6] != 0x87) return 0;
  if (p[7] < 0xA6 || p[7] > 0xBF) return 0;
  dst[0] = (char)('A' + (p[3] - 0xA6));
  dst[1] = (char)('A' + (p[7] - 0xA6));
  dst[2] = 0;
  return 1;
}

// WRITTEN TERMS. Short on purpose: every extra entry is a chance for two
// sources that were the same to stop matching over a word only one carries.
// All without accents: the text goes through fold() first.
static const struct { const char *term; const char *code; } TERMS[] = {
  { "dual audio",  "DUAL" }, { "dual-audio", "DUAL" }, { "dualaudio", "DUAL" },
  { "multi audio", "MULTI" }, { "multi-audio", "MULTI" }, { "multiaudio", "MULTI" },
  { "multi-subs",  "MULTI" },
  { "dublado",     "DUB"  }, { "dublagem",  "DUB" }, { "dubbed", "DUB" },
  { "nacional",    "POR"  },
  { "portuguese",  "POR"  }, { "portugues", "POR" },
  { "brazilian",   "POR"  }, { "brasileiro", "POR" },
  { "pt-br",       "POR"  }, { "ptbr", "POR" }, { "pt_br", "POR" },
  { "english",     "ENG"  },
  { "spanish",     "SPA"  }, { "espanol", "SPA" },
  { "latino",      "SPA"  }, { "castellano", "SPA" },
  { "french",      "FRA"  }, { "francais", "FRA" },
  { "german",      "GER"  }, { "deutsch", "GER" },
  { "dutch",       "DUT"  }, { "nederlands", "DUT" },
  { "italian",     "ITA"  }, { "italiano", "ITA" },
  { "japanese",    "JPN"  },
  { "korean",      "KOR"  },
  { "hindi",       "HIN"  }, { "tamil", "TAM" }, { "telugu", "TEL" },
  { "russian",     "RUS"  },
  { "subbed",      "SUB"  }, { "legendado", "SUB" }
};
#define N_TERMS ((int)(sizeof TERMS / sizeof *TERMS))

// LOWER CASE AND NO ACCENTS. Addons write the same thing three ways ("Dual
// Audio", "DUAL AUDIO", "Dual Áudio"). Only UTF-8's Latin-1 range (C3 80..BE)
// is folded; every other byte passes through, flags included.
static void fold(const char *src, char *dst, size_t size) {
  static const char *BASE = "aaaaaaaceeeeiiiidnooooo*ouuuuy";  /* C0..DE */
  size_t k = 0;
  if (!size) return;
  for (; src && *src && k + 1 < size; src++) {
    unsigned char c = (unsigned char)*src;
    if (c == 0xC3 && (unsigned char)src[1] >= 0x80 && (unsigned char)src[1] <= 0xBE) {
      unsigned char d = (unsigned char)src[1];
      unsigned idx = (d >= 0xA0 ? d - 0xA0 : d - 0x80);
      if (idx < 30) { dst[k++] = BASE[idx]; src++; continue; }
    }
    dst[k++] = (char)tolower(c);
  }
  dst[k] = 0;
}

// A whole token: "dub" must not match inside "Dubai".
static int tokenIn(const char *s, const char *t) {
  size_t n = strlen(t);
  const char *p;
  for (p = s; *p; p++)
    if ((p == s || !isalnum((unsigned char)p[-1])) &&
        !strncmp(p, t, n) && !isalnum((unsigned char)p[n])) return 1;
  return 0;
}

#define AUDIO_MARKS 10

static int has(char marks[][6], int n, const char *code) {
  int i;
  for (i = 0; i < n; i++) if (!strcmp(marks[i], code)) return 1;
  return 0;
}

void sourcepref_audio(const Stream *s, char *dst, unsigned size) {
  char text[6000], raw[6000];
  char marks[AUDIO_MARKS][6];
  int n = 0, i, j, len;
  unsigned k = 0;
  if (!dst || !size) return;
  dst[0] = 0;
  if (!s) return;
  // LABEL + DESCRIPTION + FILE, because each addon writes the language in its
  // own place: Torrentio puts flags in the title (which becomes the
  // description), AIOStreams writes "Dual Audio" in the name, and some groups
  // only leave it in the file name.
  snprintf(raw, sizeof raw, "%s %s %s", s->label, s->description, s->file);
  fold(raw, text, sizeof text);
  len = (int)strlen(text);

  for (i = 0; i < len && n < AUDIO_MARKS; i++) {
    char code[4];
    // 8 bytes ahead: flag() reads the whole pair.
    if ((unsigned char)text[i] == 0xF0 && i + 8 <= len &&
        flag((const unsigned char *)text + i, code)) {
      if (!has(marks, n, code)) snprintf(marks[n++], 6, "%s", code);
      i += 7;
    }
  }
  for (j = 0; j < N_TERMS && n < AUDIO_MARKS; j++)
    if (tokenIn(text, TERMS[j].term) && !has(marks, n, TERMS[j].code))
      snprintf(marks[n++], 6, "%s", TERMS[j].code);

  // SORTED: the same group writes "GB / NL" on one episode and "NL / GB" on the
  // next. Insertion, because there are at most ten.
  for (i = 1; i < n; i++) {
    char tmp[6];
    snprintf(tmp, sizeof tmp, "%s", marks[i]);
    for (j = i; j > 0 && strcmp(marks[j - 1], tmp) > 0; j--)
      snprintf(marks[j], 6, "%s", marks[j - 1]);
    snprintf(marks[j], 6, "%s", tmp);
  }
  for (i = 0; i < n && k + 1 < size; i++)
    k += (unsigned)snprintf(dst + k, size - k, "%s%s", i ? "+" : "", marks[i]);
}

// --- the table ----------------------------------------------------------------

static void baseId(const char *id, char *dst, unsigned size) {
  dst[0] = 0;
  if (id) snprintf(dst, size, "%.*s", (int)strcspn(id, ":"), id);
}

static int find(const char *base) {
  int i;
  if (!base || !base[0]) return -1;
  for (i = 0; i < nTab; i++) if (!strcmp(table[i].id, base)) return i;
  return -1;
}

// Tabs and control bytes out: Torrentio's label is "Torrentio\n1080p", and a
// newline inside would split one line of the file into two.
static void clean(char *s) {
  for (; *s; s++) if ((unsigned char)*s < 32) *s = ' ';
}

static char *field(char **p) {
  char *start = *p, *t;
  if (!start) return (char *)"";
  t = strchr(start, '\t');
  if (t) { *t = 0; *p = t + 1; } else { *p = NULL; }
  return start;
}

static void save(void) {
  size_t cap = (size_t)SOURCEPREF_MAX * 560u + 64u;
  char *buf = malloc(cap);
  size_t k = 0;
  int i;
  if (!buf) return;
  k += (size_t)snprintf(buf + k, cap - k, "# nuvio sources v1\n");
  for (i = 0; i < nTab && k + 1 < cap; i++)
    k += (size_t)snprintf(buf + k, cap - k, "%s\t%lld\t%d\t%s\t%s\t%s\t%s\n",
                          table[i].id, table[i].whenS, table[i].height,
                          table[i].audio, table[i].provider, table[i].label,
                          table[i].bingeGroup);
  data_write(fileOf(loadedProfile), buf);
  free(buf);
}

// Reads the ACTIVE profile's file when it is not the one in memory. Cheap when
// it is: a switch of profile is the only thing that makes this read.
static void load(void) {
  char *b, *line, *next;
  int profile = profiles_active();
  if (profile == loadedProfile) return;
  loadedProfile = profile;
  nTab = 0;
  b = data_read(fileOf(profile));
  if (!b) return;
  for (line = b; line && *line && nTab < SOURCEPREF_MAX; line = next) {
    char *p, *id, *when, *height, *audio, *provider, *label, *binge;
    char *end = strchr(line, '\n');
    next = end ? end + 1 : NULL;
    if (end) *end = 0;
    if (line[0] == '#' || !line[0]) continue;
    p = line;
    id       = field(&p);
    when     = field(&p);
    height   = field(&p);
    audio    = field(&p);
    provider = field(&p);
    label    = field(&p);
    binge    = p ? p : (char *)"";
    if (strncmp(id, "tt", 2) || !provider[0]) continue;
    { SourcePref *f = &table[nTab++];
      memset(f, 0, sizeof *f);
      snprintf(f->id, sizeof f->id, "%s", id);
      snprintf(f->audio, sizeof f->audio, "%s", audio);
      snprintf(f->provider, sizeof f->provider, "%s", provider);
      snprintf(f->label, sizeof f->label, "%s", label);
      snprintf(f->bingeGroup, sizeof f->bingeGroup, "%s", binge);
      f->whenS = atoll(when);
      f->height = atoi(height); }
  }
  free(b);
  printf("[source] %d source preference(s) in profile %d\n", nTab, profile);
}

void sourcepref_forget(void) {
  int p;
  // EVERY profile's file: the profiles leave with the account, and a leftover
  // profile-3 file would be read by the next person who lands on profile 3.
  for (p = 0; p <= SOURCEPREF_PROFILES; p++) data_erase(fileOf(p));
  nTab = 0;
  loadedProfile = -1;
}

// EXPIRED? Two edges that bite on a TV:
//   whenS <= 0     never expires: a line without a stamp is not "ancient".
//   now < whenS    does not expire either: this TV boots with a wrong clock and
//                  only fixes it once the network is up, so "from the future"
//                  is the normal state of the first seconds.
static int expired(const SourcePref *f) {
  long long now = (long long)time(NULL);
  if (!f || f->whenS <= 0 || now <= f->whenS) return 0;
  return now - f->whenS > SOURCEPREF_VALID_S;
}

static const SourcePref *ofTitle(const char *id) {
  char base[24];
  int k;
  load();
  baseId(id, base, sizeof base);
  k = find(base);
  if (k < 0) return NULL;
  if (expired(&table[k])) {
    // Not deleted and not written: the line dies on the next pick of this title
    // or when the table fills and the oldest goes.
    printf("[source] preference for %s expired; going automatic\n", table[k].id);
    return NULL;
  }
  return &table[k];
}

int sourcepref_store(const char *id, const Stream *s) {
  char base[24];
  int k;
  SourcePref fresh;
  if (!s || !s->provider[0]) return 0;
  // Loaded first: writing over a table never read would ERASE the whole file.
  load();
  baseId(id, base, sizeof base);
  if (!base[0]) return 0;

  memset(&fresh, 0, sizeof fresh);
  snprintf(fresh.id, sizeof fresh.id, "%s", base);
  snprintf(fresh.provider, sizeof fresh.provider, "%s", s->provider);
  snprintf(fresh.label, sizeof fresh.label, "%s", s->label);
  snprintf(fresh.bingeGroup, sizeof fresh.bingeGroup, "%s", s->bingeGroup);
  sourcepref_audio(s, fresh.audio, sizeof fresh.audio);
  fresh.height = s->height;
  fresh.whenS = (long long)time(NULL);
  clean(fresh.provider);
  clean(fresh.label);
  clean(fresh.audio);
  clean(fresh.bingeGroup);

  k = find(base);
  if (k < 0) {
    if (nTab >= SOURCEPREF_MAX) {
      // Full: the oldest goes. Never fail to store — the alternative is a table
      // that stops learning at title 200 and nobody notices.
      int i, old = 0;
      for (i = 1; i < nTab; i++)
        if (table[i].whenS < table[old].whenS) old = i;
      k = old;
    } else {
      k = nTab++;
    }
  } else if (!strcmp(table[k].provider, fresh.provider) &&
             !strcmp(table[k].audio, fresh.audio) &&
             !strcmp(table[k].bingeGroup, fresh.bingeGroup) &&
             !strcmp(table[k].label, fresh.label) &&
             table[k].height == fresh.height && !expired(&table[k])) {
    return 0;   // the same pick again: no rewrite for a timestamp
  }
  table[k] = fresh;
  save();
  printf("[source] preference for %s: %s / %s / binge=%s\n", base, fresh.provider,
         fresh.audio[0] ? fresh.audio : "(no language mark)",
         fresh.bingeGroup[0] ? fresh.bingeGroup : "(the addon did not declare one)");
  return 1;
}

// TIE-BREAK, the same in both layers: several sources can share a bingeGroup
// (addons group by service and quality, not by file), and several can share
// provider + audio. Same height is worth 2, same label 4 — the label is what
// the person read in the sheet when they chose.
static int score(const Stream *s, const SourcePref *p) {
  int n = 0;
  if (s->height == p->height) n += 2;
  if (!strcmp(s->label, p->label)) n += 4;
  return n;
}

static int byBinge(const SourcePref *p) {
  int i, best = -1, bestScore = -1, total = stream_n();
  if (!p->bingeGroup[0]) return -1;
  for (i = 0; i < total; i++) {
    const Stream *s = stream_item(i);
    int sc;
    if (!s || !s->bingeGroup[0] || strcmp(s->bingeGroup, p->bingeGroup)) continue;
    sc = score(s, p);
    // `>` and not `>=`: on a tie the FIRST in the list stays, the addon's order.
    if (sc > bestScore) { bestScore = sc; best = i; }
  }
  return best;
}

// Does today's signature `t` CONTAIN every mark of the remembered one? "BR+DUB"
// contains "DUB"; "" contains nothing and is contained by everything.
static int containsMarks(const char *t, const char *remembered) {
  const char *p = remembered;
  while (*p) {
    const char *f = strchr(p, '+');
    size_t n = f ? (size_t)(f - p) : strlen(p);
    const char *q = t;
    int found = 0;
    while (*q) {
      const char *g = strchr(q, '+');
      size_t m = g ? (size_t)(g - q) : strlen(q);
      if (m == n && !strncmp(q, p, n)) { found = 1; break; }
      if (!g) break;
      q = g + 1;
    }
    if (!found) return 0;
    if (!f) break;
    p = f + 1;
  }
  return 1;
}

static int byAudio(const SourcePref *p) {
  int i, best = -1, bestScore = -1, total = stream_n(), pass;
  // Pass 0: provider and audio equal. Pass 1: same provider and today's marks
  // CONTAIN the remembered ones.
  for (pass = 0; pass < 2 && best < 0; pass++) {
    for (i = 0; i < total; i++) {
      const Stream *s = stream_item(i);
      char t[SOURCEPREF_AUDIO];
      int sc;
      if (!s || !s->provider[0] || strcasecmp(s->provider, p->provider)) continue;
      sourcepref_audio(s, t, sizeof t);
      if (pass == 0 ? strcmp(t, p->audio) != 0 : !containsMarks(t, p->audio)) continue;
      sc = score(s, p);
      if (sc > bestScore) { bestScore = sc; best = i; }
    }
  }
  return best;
}

int sourcepref_has(const char *id) { return ofTitle(id) != NULL; }

int sourcepref_pick(const char *id) {
  const SourcePref *p = ofTitle(id);
  int best;
  if (!p) return -1;
  best = byBinge(p);
  if (best >= 0) {
    printf("[source] preferred for %s at %d by bingeGroup (%s)\n", p->id, best, p->bingeGroup);
    return best;
  }
  // A stored bingeGroup that matched nothing does not end the search: an addon
  // that switches debrid service changes the whole group without the source
  // changing at all.
  best = byAudio(p);
  if (best >= 0)
    printf("[source] preferred for %s at %d (%s)\n", p->id, best,
           p->audio[0] ? p->audio : "no language mark");
  else
    printf("[source] preferred for %s (%s / %s) is not in today's list, going automatic\n",
           p->id, p->provider, p->audio[0] ? p->audio : "no mark");
  return best;
}
