#include "catalog.h"
#include "discover.h"
#include "data.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

// Allocated as it arrives, not sized by a guessed number.
static CatItem *items;
static CatRow filters[CAT_FILTER_MAX];
static int nFilters;
static int nAllocated;
// 1 while the catalogue on screen came from the on-disk cache and not from this
// session's network. See the note in catalog.h.
static int cameOfCache;

void cat_backdrop_shrink(char *url, unsigned size, unsigned width) {
  char *o;
  char new[1024];
  if (!url || !*url) return;
  o = strstr(url, "/t/p/original/");
  if (!o) return;
  if (size > sizeof new) size = sizeof new;
  snprintf(new, sizeof new, "%.*s/t/p/w%u/%s", (int)(o - url), url, width, o + 14);
  snprintf(url, size, "%s", new);
}

// Makes sure there is room for `want` items. Returns 0 if it could not (and the
// caller carries on with what it had, which beats losing everything).
static void ensureTracks(int count);

static int ensureSpace(int want) {
  CatItem *new;
  int target;
  if (want <= nAllocated) return 1;
  if (want > CAT_MAX) want = CAT_MAX;
  target = nAllocated ? nAllocated * 2 : 64;
  while (target < want) target *= 2;
  new = realloc(items, sizeof(CatItem) * (size_t)target);
  if (!new) return 0;
  memset(new + nAllocated, 0, sizeof(CatItem) * (size_t)(target - nAllocated));
  items = new;
  nAllocated = target;
  return 1;
}
static char dirWriting[512];

// Episodes of every title in a single array, with a range per title. A
// [title][episode] matrix would spend memory on the worst case across 40 titles,
// most of which are films with no episodes at all.
#define CAT_EP_MAX 600
static CatEp eps[CAT_EP_MAX];
// Episode ranges per title, the same size as the item array — which now grows,
// so these do too.
static int  *epStart, *epCount, nEps;

// Playback progress is a position, not proof that the title was marked as
// watched. Trakt's history is kept separate, by stable identity, so that a
// change of catalogue does not turn an index into an identity and so that high
// progress does not mask a known real history.
typedef struct {
  char imdb[32];
  char kind[8];
  int known;
  int watched;
} CatHistory;

static CatHistory history[CAT_MAX];
static int nHistory;

static void id_base(const char *origin, char *destination, size_t size) {
  size_t n = 0;
  if (!destination || size == 0) return;
  if (origin) {
    while (origin[n] && origin[n] != ':' && n + 1 < size) n++;
    memcpy(destination, origin, n);
  }
  destination[n] = 0;
}

static const char *kind_base(const char *kind) {
  if (kind && (!strcmp(kind, "series") || !strcmp(kind, "show"))) return "series";
  return "movie";
}

static int history_pos(const char *imdb, const char *kind, int create) {
  char id[32];
  int i;
  id_base(imdb, id, sizeof id);
  if (!id[0]) return -1;
  for (i = 0; i < nHistory; i++)
    if (!strcmp(history[i].imdb, id) &&
        !strcmp(history[i].kind, kind_base(kind))) return i;
  if (!create || nHistory >= CAT_MAX) return -1;
  snprintf(history[nHistory].imdb, sizeof history[nHistory].imdb, "%s", id);
  snprintf(history[nHistory].kind, sizeof history[nHistory].kind, "%s", kind_base(kind));
  return nHistory++;
}

// The modal's internal reading: -1 = history not queried yet, 0 = confirmed
// unwatched, 1 = confirmed watched.
int cat_history_state_item(int index_) {
  const CatItem *it = cat_item(index_);
  int p;
  if (!it || !it->imdb[0]) return -1;
  p = history_pos(it->imdb, it->kind, 0);
  return p >= 0 && history[p].known ? history[p].watched : -1;
}

// Updates the history snapshot only after a 2xx from Trakt. The key is the IMDb
// id with no episode suffix, never the array index.
void cat_history_set_id(const char *imdb, const char *kind, int watched) {
  int p = history_pos(imdb, kind, 1);
  if (p < 0) return;
  history[p].known = 1;
  history[p].watched = watched ? 1 : 0;
}

// Compatibility for older callers that only know the IMDb id. The series is
// inferred from the catalogue itself where possible; the episode suffix is the
// fallback for items that have not entered the array yet.
const char *cat_kind_by_imdb(const char *imdb) {
  int i = cat_index_by_imdb(imdb);
  if (i >= 0 && cat_item(i)) return cat_item(i)->kind;
  return (imdb && strchr(imdb, ':')) ? "series" : "movie";
}

static void ensureTracks(int count) {
  int *a, *b;
  if (count < 1) return;
  a = realloc(epStart, sizeof(int) * (size_t)count);
  b = realloc(epCount, sizeof(int) * (size_t)count);
  if (a) epStart = a;
  if (b) epCount = b;
  if (epStart) memset(epStart, 0, sizeof(int) * (size_t)count);
  if (epCount) memset(epCount, 0, sizeof(int) * (size_t)count);
}
static int n = 0;

// Copies the field up to the next '|' (or end of line), without overflowing the
// destination.
static const char *field(const char *p, char *destination, size_t size) {
  size_t k = 0;
  while (*p && *p != '|' && *p != '\n') {
    if (k + 1 < size) destination[k++] = *p;
    p++;
  }
  destination[k] = 0;
  return (*p == '|') ? p + 1 : p;
}

int cat_load(const char *dirArt) {
  char path[600];
  snprintf(path, sizeof path, "%s/catalog.txt", dirArt);
  FILE *f = fopen(path, "r");
  if (!f) { printf("catalog: %s missing, carrying on without it\n", path); return 0; }

  char line[2048];
  n = 0;
  while (n < CAT_MAX && ensureSpace(n + 1) && fgets(line, sizeof line, f)) {
    if (line[0] == '\n' || line[0] == '#') continue;
    CatItem *it = &items[n];
    char rel[512];
    const char *p = line;
    p = field(p, rel, sizeof rel);
    // the paths in the file are relative to the art folder
    if (rel[0]) snprintf(it->backdrop, sizeof it->backdrop, "%s/%s", dirArt, rel);
    else it->backdrop[0] = 0;
    p = field(p, rel, sizeof rel);
    if (rel[0]) snprintf(it->poster, sizeof it->poster, "%s/%s", dirArt, rel);
    else it->poster[0] = 0;
    p = field(p, rel, sizeof rel);
    if (rel[0]) snprintf(it->logo, sizeof it->logo, "%s/%s", dirArt, rel);
    else it->logo[0] = 0;
    p = field(p, it->title, sizeof it->title);
    p = field(p, it->genre, sizeof it->genre);
    // The package's catalogue stores the genre ALREADY COMPOSED and in English
    // ("Movie  ·  Science Fiction  ·  Action"). It translates each piece between
    // the separators; the first one ("Movie"/"TV Show") passes through the table
    // unchanged. Done here, on reading, because `genre` is read by several
    // screens and translating while drawing would leave each of them to work it
    // out on its own.
    { char output[sizeof it->genre]; size_t o = 0;
      const char *q = it->genre;
      const char *SEP = "  \xc2\xb7  ";
      while (*q && o + 1 < sizeof output) {
        const char *sp = strstr(q, SEP);
        char part[64]; size_t n = sp ? (size_t)(sp - q) : strlen(q);
        const char *pt;
        if (n >= sizeof part) n = sizeof part - 1;
        memcpy(part, q, n); part[n] = 0;
        pt = disc_genre_label(part);
        o += (size_t)snprintf(output + o, sizeof output - o, "%s%s",
                              o ? SEP : "", pt);
        if (!sp) break;
        q = sp + strlen(SEP);
      }
      if (o) snprintf(it->genre, sizeof it->genre, "%s", output); }
    p = field(p, it->meta, sizeof it->meta);
    p = field(p, it->age_rating, sizeof it->age_rating);
    field(p, it->synopsis, sizeof it->synopsis);
    it->imdb[0] = 0;
    snprintf(it->kind, sizeof it->kind, "movie");
    n++;
  }
  fclose(f);

  // ids.txt is a SEPARATE file, one "tt1234567<TAB>movie|series" line per title,
  // in the same order. It stayed out of catalog.txt so as not to disturb the
  // column order of a file that already has a parser and data. Without it the app
  // runs the same, it just cannot ask the addons for sources.
  snprintf(path, sizeof path, "%s/ids.txt", dirArt);
  f = fopen(path, "r");
  if (f) {
    int i = 0;
    while (i < n && fgets(line, sizeof line, f)) {
      char *tab = strchr(line, '\t');
      char *end;
      if (tab) {
        *tab = 0;
        snprintf(items[i].kind, sizeof items[i].kind, "%s", tab + 1);
        end = items[i].kind + strlen(items[i].kind);
        while (end > items[i].kind && (end[-1] == '\n' || end[-1] == '\r')) *--end = 0;
      }
      snprintf(items[i].imdb, sizeof items[i].imdb, "%s", line);
      { char *e = items[i].imdb + strlen(items[i].imdb);
        while (e > items[i].imdb && (e[-1] == '\n' || e[-1] == '\r')) *--e = 0; }
      i++;
    }
    fclose(f);
    printf("catalog: %d ids\n", i);
  }

  // The cast comes in a separate file, one line per title, in the same order:
  // "name~role~photo;name~role~photo|directing". Separate because it is a very
  // different size from the rest and would change the catalogue's line with every
  // extra actor.
  snprintf(path, sizeof path, "%s/cast.txt", dirArt);
  FILE *fe = fopen(path, "r");
  if (fe) {
    for (int i = 0; i < n && fgets(line, sizeof line, fe); i++) {
      char *bar = strchr(line, '|');
      if (bar) {
        *bar = 0;
        char *d = bar + 1, *end = d + strlen(d);
        while (end > d && (end[-1] == '\n' || end[-1] == '\r')) *--end = 0;
        snprintf(items[i].directing, sizeof items[i].directing, "%s", d);
      }
      char *p2 = line;
      while (*p2 && items[i].nCast < 6) {
        char *pv = strchr(p2, ';');
        if (pv) *pv = 0;
        char *t1 = strchr(p2, '~');
        if (t1) {
          *t1 = 0;
          char *t2 = strchr(t1 + 1, '~');
          if (t2) *t2 = 0;
          int k = items[i].nCast;
          snprintf(items[i].cast[k].name, 64, "%s", p2);
          snprintf(items[i].cast[k].role, 64, "%s", t1 + 1);
          if (t2 && t2[1] && t2[1] != '\n')
            snprintf(items[i].cast[k].photo, 512, "%s/%s", dirArt, t2 + 1);
          items[i].nCast++;
        }
        if (!pv) break;
        p2 = pv + 1;
      }
    }
    fclose(fe);
  }

  // extra.txt: "score|providerLogo|providerName|progress|season|episode|remainingMin",
  // in the same order. Progress went in as the FOURTH column so as not to
  // invalidate older files: missing, the field stays 0 and the bar disappears,
  // which is the right behaviour for anyone who never started the title.
  snprintf(path, sizeof path, "%s/extra.txt", dirArt);
  FILE *fx = fopen(path, "r");
  if (fx) {
    for (int i = 0; i < n && fgets(line, sizeof line, fx); i++) {
      char c1[32] = "", c2[512] = "", c3[64] = "", c4[16] = "";
      char c5[16] = "", c6[16] = "", c7[16] = "";
      const char *q = line;
      q = field(q, c1, sizeof c1);
      q = field(q, c2, sizeof c2);
      q = field(q, c3, sizeof c3);
      q = field(q, c4, sizeof c4);
      q = field(q, c5, sizeof c5);
      q = field(q, c6, sizeof c6);
      field(q, c7, sizeof c7);
      items[i].score = atoi(c1);
      items[i].progress = atoi(c4);
      items[i].season = atoi(c5);
      items[i].episode  = atoi(c6);
      items[i].remainingMin = atoi(c7);
      if (c2[0]) snprintf(items[i].providerLogo, sizeof items[i].providerLogo, "%s/%s", dirArt, c2);
      snprintf(items[i].providerName, sizeof items[i].providerName, "%s", c3);
    }
    fclose(fx);
  }

  // progress.txt: "tt1234567<TAB>positionSec<TAB>durationSec<TAB>season<TAB>
  // episode<TAB>lastWatchedMs" per line, which is what THIS app recorded. The
  // scan tolerates a short line: everything from the fourth field on was added
  // later, and a file written by an older build still loads. It comes after extra.txt on purpose — what was
  // watched here is more recent than the snapshot brought from the web app.
  snprintf(path, sizeof path, "%s/progress.txt", dirArt);
  { FILE *fp = fopen(path, "r");
    int applied = 0;
    if (fp) {
      while (fgets(line, sizeof line, fp)) {
        char id[24]; double pos, duration; int i;
        int season = 0, episode = 0;
        long long ms = 0;
        if (sscanf(line, "%23s %lf %lf %d %d %lld", id, &pos, &duration, &season, &episode, &ms) < 3 || duration <= 1.0) continue;
        for (i = 0; i < n; i++) {
          // The catalogue's id may carry an episode ("tt123:4:9"); compare only
          // the title's prefix, which is what identifies the work.
          if (!strncmp(items[i].imdb, id, strlen(id)) &&
              (items[i].imdb[strlen(id)] == 0 || items[i].imdb[strlen(id)] == ':')) {
            items[i].progress = cat_pct(pos, duration);
            items[i].remainingMin = (int)((duration - pos) / 60.0 + 0.5);
            items[i].resumedMs = ms;
            if(season>0 && episode>0) {
              if(items[i].season!=season || items[i].episode!=episode) {
                items[i].nameEpisode[0]=0; items[i].thumbEp[0]=0;
              }
              items[i].season=season; items[i].episode=episode;
            }
            applied++;
            break;
          }
        }
      }
      fclose(fp);
      if (applied) printf("catalog: %d progress entries from this app\n", applied);
    }
    snprintf(dirWriting, sizeof dirWriting, "%s", dirArt);
  }

  // episodes.txt: "index|season|episode|name|duration|date|synopsis".
  // The index goes first because only some titles have episodes — one line per
  // title, as in the other files, would waste most of the lines.
  snprintf(path, sizeof path, "%s/episodes.txt", dirArt);
  { FILE *fe2 = fopen(path, "r");
    nEps = 0;
    ensureTracks(nAllocated);
    if (fe2) {
      while (nEps < CAT_EP_MAX && fgets(line, sizeof line, fe2)) {
        char c1[8], c2[8], c3[8];
        const char *q = line;
        int target;
        CatEp *ep = &eps[nEps];
        memset(ep, 0, sizeof *ep);
        q = field(q, c1, sizeof c1);
        target = atoi(c1);
        if (target < 0 || target >= n) continue;
        q = field(q, c2, sizeof c2);
        q = field(q, c3, sizeof c3);
        ep->season = atoi(c2);
        ep->episode  = atoi(c3);
        q = field(q, ep->name, sizeof ep->name);
        q = field(q, ep->duration, sizeof ep->duration);
        q = field(q, ep->date, sizeof ep->date);
        q = field(q, ep->synopsis, sizeof ep->synopsis);
        { char rel[512] = "";
          field(q, rel, sizeof rel);
          if (rel[0]) snprintf(ep->thumb, sizeof ep->thumb, "%s/%s", dirArt, rel); }
        if (!epCount[target]) epStart[target] = nEps;
        epCount[target]++;
        nEps++;
      }
      fclose(fe2);
      printf("catalog: %d episodes\n", nEps);
    }
  }

  int comCast = 0;
  for (int i = 0; i < n; i++) if (items[i].nCast) comCast++;
  printf("catalog: %d titles, %d with cast (item0: %d actors, dir='%s')\n",
         n, comCast, n ? items[0].nCast : 0, n ? items[0].directing : "");
  return n;
}

// --- ON-DISK CACHE -----------------------------------------------------------
//
// See the note in catalog.h. The header carries the version AND sizeof(CatItem):
// it is the sizeof that really protects, because adding a field to the struct
// changes the layout without anyone remembering to bump the version by hand.
#define CACHE_MAGIC  0x4E56434Bu   /* "NVCK" */
#define CACHE_VERSION 1

typedef struct {
  unsigned magic, version, sizeItem, sizeRow;
  int nItems, nRows;
} CacheHeader;

// THE CACHE GOES TO THE USER'S FOLDER, NEVER TO THE PACKAGE'S.
//
// It was written next to the art, and on the TV that silently never happened:
// the installed folder is root:root and the app runs as uid 5514, so the fopen
// failed, cat_write_cache returned 0 WITHOUT a line in the log, and on the next
// start cat_read_cache found nothing. The visible effect was the packaged
// catalogue — 40 titles that are not the owner's — on screen at EVERY launch,
// which is the one thing this cache exists to prevent. Verified on the device:
// no catalog-net.bin, and "cached catalog on screen" absent from every boot.
//
// data_dir() is the folder data_start already PROVED writable (it writes a file
// there and reads it back), the same one holding the session and the progress.
// dirArt stays as the fallback for when no candidate accepted a write — there
// the package folder is as good a guess as any, and on the Mac it works.
static void pathCache(const char *dirArt, char *dst, size_t size) {
  const char *dir = data_dir();
  if (!dir || !dir[0]) dir = (dirArt && *dirArt) ? dirArt : ".";
  snprintf(dst, size, "%s/catalog-net.bin", dir);
}

// Called by discovery when the COMPLETE catalogue from the network replaces the
// cached one. From here on the screen is this session's.
void cat_cache_replaced(void) { cameOfCache = 0; }

int cat_write_cache(const char *dirArt) {
  char path[600], tmp[620];
  CacheHeader c;
  FILE *f;
  if (n < 1) return 0;
  pathCache(dirArt, path, sizeof path);
  // Writes to a temporary file and renames: whoever reads on the next start
  // never picks up a half-written file if the app is closed mid-write.
  snprintf(tmp, sizeof tmp, "%s.tmp", path);
  f = fopen(tmp, "wb");
  if (!f) return 0;
  c.magic = CACHE_MAGIC; c.version = CACHE_VERSION;
  c.sizeItem = (unsigned)sizeof(CatItem);
  c.sizeRow = (unsigned)sizeof(CatRow);
  c.nItems = n; c.nRows = nFilters;
  if (fwrite(&c, sizeof c, 1, f) != 1 ||
      fwrite(items, sizeof(CatItem), (size_t)n, f) != (size_t)n ||
      (nFilters > 0 &&
       fwrite(filters, sizeof(CatRow), (size_t)nFilters, f) != (size_t)nFilters)) {
    fclose(f); remove(tmp); return 0;
  }
  fclose(f);
  if (rename(tmp, path) != 0) { remove(tmp); return 0; }
  printf("[cat] cache written: %d titles, %d rows\n", n, nFilters);
  fflush(stdout);
  return 1;
}

int cat_read_cache(const char *dirArt) {
  char path[600];
  CacheHeader c;
  FILE *f;
  CatItem *new;
  CatRow readRows[CAT_FILTER_MAX];
  int nRead = 0;
  pathCache(dirArt, path, sizeof path);
  f = fopen(path, "rb");
  if (!f) return 0;
  if (fread(&c, sizeof c, 1, f) != 1) { fclose(f); return 0; }
  // REFUSE rather than read it crooked. A different struct = a file from another build.
  if (c.magic != CACHE_MAGIC || c.version != CACHE_VERSION ||
      c.sizeItem != sizeof(CatItem) || c.sizeRow != sizeof(CatRow) ||
      c.nItems < 1 || c.nItems > CAT_MAX ||
      c.nRows < 0 || c.nRows > CAT_FILTER_MAX) {
    fclose(f);
    printf("[cat] cache discarded (format from another build)\n");
    remove(path);
    return 0;
  }
  new = malloc(sizeof(CatItem) * (size_t)c.nItems);
  if (!new) { fclose(f); return 0; }
  if (fread(new, sizeof(CatItem), (size_t)c.nItems, f) != (size_t)c.nItems) {
    free(new); fclose(f); remove(path); return 0;
  }
  if (c.nRows > 0) {
    if (fread(readRows, sizeof(CatRow), (size_t)c.nRows, f)
        != (size_t)c.nRows) {
      free(new); fclose(f); remove(path); return 0;
    }
    nRead = c.nRows;
  }
  fclose(f);
  // Reuses the block-swap path, which is already the safe one for the drawing
  // thread — and it is what trims the row windows to the real size.
  cat_set_all(new, c.nItems, readRows, nRead);
  cameOfCache = 1;
  free(new);
  printf("[cat] cache read: %d titles, %d rows\n", c.nItems, nRead);
  fflush(stdout);
  return 1;
}

int cat_do_cache(void) { return cameOfCache; }

int cat_n(void) { return n; }

const CatItem *cat_item(int i) {
  if (!items || n <= 0) return NULL;
  return &items[((i % n) + n) % n];
}

// Compares only up to the first ':' — the catalogue stores "tt123:2:1" for a
// series with progress, and the searcher has only the title's id.
static int sameTitle(const char *a, const char *b) {
  while (*a && *b && *a != ':' && *b != ':') { if (*a != *b) return 0; a++; b++; }
  return (!*a || *a == ':') && (!*b || *b == ':');
}

void cat_dir_writing(const char *dir) {
  if (dir && *dir) snprintf(dirWriting, sizeof dirWriting, "%s", dir);
}

// See the note on the declaration in catalog.h.
int cat_pct(double posSeg, double durationSeg) {
  int pct;
  if (durationSeg <= 1.0 || posSeg <= 0.0) return 0;
  pct = (int)(100.0 * posSeg / durationSeg);
  return pct < 1 ? 1 : pct;
}

int cat_index_by_imdb(const char *imdb) {
  int i;
  if (!imdb || !imdb[0]) return -1;
  for (i = 0; i < cat_n(); i++) {
    const CatItem *c = cat_item(i);
    if (c && c->imdb[0] && sameTitle(c->imdb, imdb)) return i;
  }
  return -1;
}

void cat_save_progress(int index_, double posSeg, double durationSeg) {
  cat_save_progress_ep(index_,posSeg,durationSeg,0,0);
}

void cat_save_progress_ep(int index_, double posSeg, double durationSeg, int season, int episode) {
  cat_save_progress_at(index_, posSeg, durationSeg, season, episode, 0, 1);
}

void cat_save_progress_at(int index_, double posSeg, double durationSeg,
                          int season, int episode, long long whenMs, int origin) {
  char path[600], tmp[600], line[256], work[24];
  FILE *e, *s;
  const CatItem *it;
  int i;
  if (durationSeg <= 1.0 || !dirWriting[0]) return;
  i = cat_n(); if (i < 1) return;
  index_ = ((index_ % i) + i) % i;
  it = &items[index_];
  if (!it->imdb[0]) return;

  // THE LINE IS KEYED BY THE WORK, never by the composite id.
  //
  // `it->imdb` is "tt123" on an item that came from a catalogue and
  // "tt123:4:9" on one built for "Continue watching", and both name the same
  // series. The dedupe below used to compare the whole string, so the two
  // spellings never matched each other and the file accumulated a second line
  // per series — measured on the owner's TV, "tt0121955 … 12 14" and
  // "tt0121955:12:14 … 0 0" side by side, the second written by the sync and
  // newer, so it was the one "Continue watching" read and it carried no episode
  // at all. Writing the work's id and dropping every line that names that work
  // collapses both spellings onto one record and cleans up the duplicates a
  // previous build left behind.
  //
  // The season and episode keep their own columns, which is where they are
  // reliable; see cat_progress_read.
  snprintf(work, sizeof work, "%.*s", (int)strcspn(it->imdb, ":"), it->imdb);
  if (!work[0]) return;
  // A caller that does not know the episode but holds a composite id knows it
  // after all — it is in the id. This is what stops the sync, which learns
  // season and episode only when the account bothered to send them, from
  // writing 0/0 over an episode the id itself names.
  if (season <= 0 && episode <= 0) {
    const char *dp = strchr(it->imdb, ':');
    if (dp) sscanf(dp + 1, "%d:%d", &season, &episode);
  }

  // Rewrites the whole file, swapping this title's line. It is a file of a few
  // dozen lines: reading it all and writing it back costs nothing and avoids the
  // duplicate a plain append would accumulate.
  snprintf(path, sizeof path, "%s/progress.txt", dirWriting);
  snprintf(tmp, sizeof tmp, "%s/progress.tmp", dirWriting);
  s = fopen(tmp, "w");
  if (!s) return;
  e = fopen(path, "r");
  if (e) {
    while (fgets(line, sizeof line, e)) {
      char id[24];
      if (sscanf(line, "%23s", id) == 1 && sameTitle(id, work)) continue;
      fputs(line, s);
    }
    fclose(e);
  }
  // A SIXTH COLUMN: when this happened, in ms since the epoch.
  //
  // "Continue watching" merges this file with what Trakt reports, and merging
  // two histories needs an instant on both sides — without it the row falls
  // back to the order a source happened to answer in. Appending a column is
  // backwards compatible in both directions: every reader here scans with a
  // field count it tolerates being short, so a file written by an older build
  // still loads and simply reports "instant unknown".
  //
  // `whenMs` carries the instant the caller already knows — the account's
  // `updated_at` on a synced row. Only playback that happened HERE has no
  // instant to carry, and that is the one case the clock answers for.
  //
  // AND A SEVENTH: who the record belongs to. It is a parameter and not
  // something inferred from `whenMs` being absent, because the two are genuinely
  // independent — the account can hand over a row whose instant it never stored,
  // and guessing "no instant means it happened here" would file that row under
  // this device's own playback and let it outrank the account forever after.
  //
  // AND ONLY PLAYBACK HERE MAY FALL BACK TO THE CLOCK. A record replayed from
  // the account with no instant of its own is written as 0 — "not known" — and
  // not as "now". The readers have always tolerated 0 and order it after
  // everything that knows its own instant, which is the honest placement; the
  // clock would instead launder a guess into a fact, and the fact would then be
  // pushed back and outrank the true dating on every other device. It is also
  // recoverable: Trakt's half of "Continue watching" carries a real paused_at
  // for most of these works, and the merge takes the newer of the two, so an
  // unknown local instant simply lets the source that DOES know decide.
  fprintf(s, "%s\t%.0f\t%.0f\t%d\t%d\t%lld\t%d\n", work, posSeg, durationSeg,
          season, episode,
          whenMs > 0 ? whenMs
                     : (origin == 1 ? (long long)time(NULL) * 1000 : 0),
          origin);
  fclose(s);
  // Write to a temporary and rename: a power cut mid-write would leave the file
  // half-written and the app would come up with no progress at all.
  rename(tmp, path);

  items[index_].progress = cat_pct(posSeg, durationSeg);
  items[index_].remainingMin = (int)((durationSeg - posSeg) / 60.0 + 0.5);
  // The instant goes onto the item as well as into the file. The line just
  // written is what the next build will read, but the item in memory is what
  // the screen holds until then, and leaving it stale means the same record
  // reports two different instants depending on who asks.
  items[index_].resumedMs = whenMs > 0 ? whenMs
                          : (origin == 1 ? (long long)time(NULL) * 1000 : 0);
  if(season>0 && episode>0) {
    if (items[index_].season != season || items[index_].episode != episode) {
      items[index_].nameEpisode[0] = 0;
      items[index_].thumbEp[0] = 0;
    }
    items[index_].season=season;
    items[index_].episode=episode;
    for (int e = 0; e < cat_n_episodes(index_); e++) {
      const CatEp *ep = cat_episode(index_, e);
      if (ep && ep->season == season && ep->episode == episode) {
        snprintf(items[index_].nameEpisode, sizeof items[index_].nameEpisode, "%s", ep->name);
        // The still travels with the name. This is the path that feeds the
        // "Resume now" band, which points at a CATALOGUE item and so never went
        // through trakt.c's decorate: without this it drew the series' backdrop
        // while the row below it, built from the same progress, drew the episode.
        snprintf(items[index_].thumbEp, sizeof items[index_].thumbEp, "%s", ep->thumb);
        break;
      }
    }
  }
}

int cat_n_episodes(int indexItem) {
  int m = cat_n();
  if (m < 1) return 0;
  indexItem = ((indexItem % m) + m) % m;
  return epCount[indexItem];
}

int cat_set_ep_score(int indexItem, int season, int episode, int tenths) {
  int m = cat_n();
  if (m < 1 || tenths <= 0) return 0;
  indexItem = ((indexItem % m) + m) % m;
  for (int i = 0; i < epCount[indexItem]; i++) {
    CatEp *e = &eps[epStart[indexItem] + i];
    if (e->season == season && e->episode == episode) { e->imdb = tenths; return 1; }
  }
  return 0;
}

const CatEp *cat_episode(int indexItem, int i) {
  int m = cat_n();
  if (m < 1) return NULL;
  indexItem = ((indexItem % m) + m) % m;
  if (i < 0 || i >= epCount[indexItem]) return NULL;
  return &eps[epStart[indexItem] + i];
}

int cat_n_rows(void) { return nFilters; }
const CatRow *cat_row(int r) {
  return (r >= 0 && r < nFilters) ? &filters[r] : NULL;
}

// APPENDS ONE title to the end of the catalogue and returns its index.
//
// It exists for a title that came from OUTSIDE: a credit in an actor's
// filmography or a "More like this" item the owner's catalogue does not have.
// Without this the item was greyed out and would not open, which left the
// filmography decorative.
//
// It uses the SAME block swap as cat_set_all, for the same reason (a reader on
// the drawing thread inside the old block), with two differences:
//   - it appends at the END, so the rows' (start,n) windows still hold and do
//     not need to be torn down;
//   - `n` is NOT zeroed: raising the count after the new block is already
//     published is safe, and zeroing would make the home flicker on every title
//     opened.
void cat_set_in_list(int i, int inList) {
  if (!items || n <= 0 || i < 0 || i >= n) return;
  items[i].inList = inList ? 1 : 0;
}

// Updates an item's mirror only while the index still belongs to the currently
// published block. The modal may get the worker's answer after discovery has
// swapped the catalogue; ignoring it in that case is safe, whereas writing
// through an old index could alter another title.
void cat_update_item(int i, const CatItem *item) {
  if (!item || !items || n <= 0 || i < 0 || i >= n) return;
  items[i] = *item;
}

// Appends N AT ONCE. cat_append copies the whole catalogue on every call, and
// the search called it PER RESULT: with 300 titles in the collection that is
// ~2.3 MB per copy, times 40 results, on the DRAWING thread, on every keypress.
// It was the freeze that showed up as "search stutters when I type".
//
// A single block swap, following the same order as cat_set_all: it zeroes `n`
// before swapping the pointer (the drawing sees an empty catalogue for one frame
// instead of reading freed memory) and does not free the old block here — a
// reader may be inside it; it dies on the next swap.
int cat_append_batch(const CatItem *v, int count, int *outputIdx) {
  static CatItem *garbageLote;
  CatItem *new;
  int newN, k;
  if (!v || count < 1 || n < 1) return 0;
  if (n + count > CAT_MAX) count = CAT_MAX - n;
  if (count < 1) return 0;
  newN = n + count;
  new = malloc(sizeof(CatItem) * (size_t)newN);
  if (!new) return 0;
  memcpy(new, items, sizeof(CatItem) * (size_t)n);
  memcpy(&new[n], v, sizeof(CatItem) * (size_t)count);
  if (outputIdx) for (k = 0; k < count; k++) outputIdx[k] = n + k;
  free(garbageLote);
  garbageLote = items;
  items = new;
  nAllocated = newN;
  n = newN;
  ensureTracks(nAllocated);
  return count;
}

int cat_append(const CatItem *item) {
  static CatItem *garbageAccum;
  CatItem *new;
  int newN;
  if (!item || n < 1) return -1;
  if (n >= CAT_MAX) return -1;
  newN = n + 1;
  new = malloc(sizeof(CatItem) * (size_t)newN);
  if (!new) return -1;
  memcpy(new, items, sizeof(CatItem) * (size_t)n);
  memcpy(&new[n], item, sizeof(CatItem));
  free(garbageAccum);
  garbageAccum = items;
  items = new;
  nAllocated = newN;
  n = newN;
  ensureTracks(nAllocated);
  return newN - 1;
}

void cat_set(const CatItem *list, int count) {
  cat_set_all(list, count, NULL, 0);
}

void cat_set_all(const CatItem *list, int count,
                      const CatRow *newFilters, int nNew) {
  int i;
  if (!list || count < 1) return;
  // A BLOCK SWAP, with no realloc in place.
  //
  // cat_set_all runs on the discovery thread while the drawing reads items[] on
  // the main thread. With realloc the old block is FREED and the drawing starts
  // reading dead memory — that is how the app began dying in home_draw as soon
  // as the catalogue grew from 40 to 303. While it was a static array the
  // address never changed and the problem did not exist.
  //
  // The order of the three lines below is what makes this safe without a lock:
  // zeroing `n` first makes the drawing treat the catalogue as empty for one
  // frame (it draws nothing), and only then do the pointer and the count go up.
  // The old block is NOT freed here: a reader may be inside it at this very
  // moment. It dies on the next swap, when nothing can reach it any more.
  {
    int newN = count > CAT_MAX ? CAT_MAX : count;
    CatItem *new = malloc(sizeof(CatItem) * (size_t)newN);
    static CatItem *garbage;
    if (!new) return;
    memcpy(new, list, sizeof(CatItem) * (size_t)newN);
    // The rows fall TOGETHER with `n`. They are (start,n) windows into the item
    // array; leaving the old ones standing for one frame while the array swaps
    // makes the drawing read out of range.
    n = 0;
    nFilters = 0;
    free(garbage);
    garbage = items;
    items = new;
    nAllocated = newN;
    n = newN;
    if (newFilters && nNew > 0) {
      int k, q = nNew > CAT_FILTER_MAX ? CAT_FILTER_MAX : nNew;
      int v = 0;
      for (k = 0; k < q; k++) {
        CatRow f = newFilters[k];
        // Trims the window to what really remains. A catalogue that answered
        // with fewer items than expected would leave the row pointing at its
        // neighbour.
        if (f.start < 0 || f.start >= n) continue;
        if (f.start + f.n > n) f.n = n - f.start;
        if (f.n < 1) continue;
        filters[v++] = f;
      }
      nFilters = v;
    }
  }
  // Episodes from the previous catalogue do not apply to the new one: the
  // indices have changed.
  nEps = 0;
  ensureTracks(nAllocated);
  (void)0;
  // Progress comes from a file and is keyed by imdb, so it survives the swap —
  // but it has to be reapplied, because the new items were born zeroed.
  if (dirWriting[0]) {
    char path[600], line[256];
    FILE *fp;
    snprintf(path, sizeof path, "%s/progress.txt", dirWriting);
    fp = fopen(path, "r");
    if (fp) {
      while (fgets(line, sizeof line, fp)) {
        char id[24]; double pos, duration;
        int season = 0, episode = 0;
        long long ms = 0;
        if (sscanf(line, "%23s %lf %lf %d %d %lld", id, &pos, &duration, &season, &episode, &ms) < 3 || duration <= 1.0) continue;
        for (i = 0; i < n; i++) {
          size_t L = strlen(id);
          if (!strncmp(items[i].imdb, id, L) &&
              (items[i].imdb[L] == 0 || items[i].imdb[L] == ':')) {
            items[i].progress = cat_pct(pos, duration);
            items[i].remainingMin = (int)((duration - pos) / 60.0 + 0.5);
            items[i].resumedMs = ms;
            if(season>0 && episode>0) {
              if(items[i].season!=season || items[i].episode!=episode) {
                items[i].nameEpisode[0]=0; items[i].thumbEp[0]=0;
              }
              items[i].season=season; items[i].episode=episode;
            }
            break;
          }
        }
      }
      fclose(fp);
    }
  }
}

void cat_set_episodes(int indexItem, const CatEp *list, int count) {
  int m = cat_n();
  if (!list || count < 1 || m < 1) return;
  indexItem = ((indexItem % m) + m) % m;
  if (count > CAT_EP_MAX) count = CAT_EP_MAX;
  // Appends at the end of the shared array. Switching season several times
  // accumulates, but the CAT_EP_MAX ceiling holds it and compacting does not pay
  // for itself.
  if (nEps + count > CAT_EP_MAX) {
    nEps = 0;
    // Invalidate the indices before reusing the storage: otherwise another
    // series starts showing the episodes of the work that has just been loaded.
    memset(epCount,0,(size_t)nAllocated*sizeof *epCount);
    memset(epStart,0,(size_t)nAllocated*sizeof *epStart);
  }
  memcpy(&eps[nEps], list, sizeof(CatEp) * (size_t)count);
  epStart[indexItem] = nEps;
  epCount[indexItem] = count;
  nEps += count;
}

// An item's genres, as a list of pieces separated by " · ". The first field is
// always "Movie"/"TV Show" and does not count as a genre.
static int sharesGenre(const CatItem *a, const CatItem *b) {
  const char *p = a->genre;
  int first = 1;
  while (p && *p) {
    const char *sep = strstr(p, "\xc2\xb7");
    char term[64];
    size_t n;
    if (!sep) break;
    p = sep + 2;
    while (*p == ' ') p++;
    sep = strstr(p, "\xc2\xb7");
    n = sep ? (size_t)(sep - p) : strlen(p);
    while (n && (p[n - 1] == ' ')) n--;
    if (n && n < sizeof term) {
      memcpy(term, p, n);
      term[n] = 0;
      if (strstr(b->genre, term)) return 1;
    }
    first = 0;
    if (!sep) break;
  }
  (void)first;
  return 0;
}

int cat_similar(int index_, int *output, int max) {
  int m = cat_n(), i, k = 0;
  const CatItem *base;
  if (m < 1 || !output || max < 1) return 0;
  index_ = ((index_ % m) + m) % m;
  base = &items[index_];
  for (i = 0; i < m && k < max; i++) {
    if (i == index_) continue;
    if (base->kind[0] && items[i].kind[0] && strcmp(base->kind, items[i].kind)) continue;
    if (!sharesGenre(base, &items[i])) continue;
    output[k++] = i;
  }
  // With no genre in common the row would be empty; then it is worth more to
  // show the neighbours of the same type than to lose the section.
  for (i = 0; i < m && k < max; i++) {
    int j, already = 0;
    if (i == index_) continue;
    for (j = 0; j < k; j++) if (output[j] == i) { already = 1; break; }
    if (already) continue;
    if (base->kind[0] && items[i].kind[0] && strcmp(base->kind, items[i].kind)) continue;
    output[k++] = i;
  }
  // High score first.
  { int a, b, t;
    for (a = 0; a < k; a++)
      for (b = a + 1; b < k; b++)
        if (items[output[b]].score > items[output[a]].score) {
          t = output[a]; output[a] = output[b]; output[b] = t;
        } }
  return k;
}

// --- THE PROGRESS RECORDS, READ AS HISTORY ------------------------------------

static int progressNewestFirst(const void *a, const void *b) {
  const CatProgress *x = a, *y = b;
  if (x->lastWatchedMs > y->lastWatchedMs) return -1;
  if (x->lastWatchedMs < y->lastWatchedMs) return 1;
  return 0;
}

int cat_progress_read(CatProgress *out, int max) {
  char path[600], line[256];
  FILE *fp;
  int n = 0;
  if (!out || max <= 0 || !dirWriting[0]) return 0;
  snprintf(path, sizeof path, "%s/progress.txt", dirWriting);
  fp = fopen(path, "r");
  if (!fp) return 0;
  while (fgets(line, sizeof line, fp) && n < max) {
    char id[24];
    double pos = 0.0, duration = 0.0;
    int season = 0, episode = 0, origin = 0, fields;
    long long ms = 0;
    CatProgress *r;
    char *colon;
    fields = sscanf(line, "%23s %lf %lf %d %d %lld %d",
                    id, &pos, &duration, &season, &episode, &ms, &origin);
    // Three is the oldest shape this file ever had. Fewer than that is not a
    // record, and a duration of zero would make the percentage below a division
    // by zero rather than a number nobody can use.
    if (fields < 3 || duration <= 1.0) continue;
    r = &out[n];
    memset(r, 0, sizeof *r);
    // The id may carry an episode suffix ("tt123:4:9") because that is what the
    // catalogue item held when the line was written. The WORK is what identifies
    // a row in Continue watching — a series appears once — so the suffix is cut
    // here and the season and episode come from their own columns, which is
    // where they are reliable.
    snprintf(r->imdb, sizeof r->imdb, "%s", id);
    colon = strchr(r->imdb, ':');
    if (colon) *colon = 0;
    if (!r->imdb[0]) continue;
    r->posSeg = pos;
    r->durationSeg = duration;
    r->season = season;
    r->episode = episode;
    r->lastWatchedMs = ms;
    r->origin = origin;
    n++;
  }
  fclose(fp);
  // Most recent first. Lines from before the sixth column carry 0 and therefore
  // land at the end, which is the honest place for "instant unknown" — it is not
  // claimed to be old, it is merely not claimed to be recent.
  if (n > 1) qsort(out, (size_t)n, sizeof *out, progressNewestFirst);
  return n;
}
