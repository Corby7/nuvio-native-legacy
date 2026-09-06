#include "addons.h"
#include "streams.h"
#include "net.h"
#include "js.h"
#include "mark.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <pthread.h>
#include <stdatomic.h>

#define ADD_MAX 12

// `source` marks who actually delivers streams. Discovered from the manifest:
// Xperience declares the catalog/meta/subtitles resources and NO stream, so it
// answered {"streams":[]} to everything. Querying something that supplies
// nothing is a round trip thrown away on EVERY title opening.
// `id` is the identifier the addon declares in its OWN manifest, not something
// the account stores: the account's addon list carries a name and a URL, nothing
// more. It lives here because the owner's COLLECTIONS reference catalogues by
// addonId, and only the manifest can match the two (see addons_note_id).
static struct { char name[64]; char base[600]; char id[96];
                int source, catalog, subtitle; } addon[ADD_MAX];
static int nAddon;
static _Atomic AddState state = ADD_STOPPED;
static pthread_t thread;
static char targetId[64], targetKind[16];
static int threadAlive;
static Stream *result;
static int nResult;
static char pendingId[64], pendingKind[16];

// --- reading the configuration file ------------------------------------------

int addons_load(const char *dirArt) {
  char path[600], line[900];
  FILE *f;
  snprintf(path, sizeof path, "%s/addons.txt", dirArt ? dirArt : ".");
  f = fopen(path, "r");
  if (!f) { printf("[addons] no %s\n", path); return 0; }
  nAddon = 0;
  while (nAddon < ADD_MAX && fgets(line, sizeof line, f)) {
    char *tab = strchr(line, '\t');
    char *end;
    size_t n;
    // TAB and not "|" as the separator: an addon name really can contain "|"
    // ("AIOStreams | ElfHosted") and splitting at the first pipe corrupted the URL.
    if (!tab) continue;
    *tab = 0;
    end = tab + 1 + strlen(tab + 1);
    while (end > tab + 1 && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ')) *--end = 0;
    if (line[0] == '#' || !tab[1]) continue;
    // Third column (optional): 1 = supplies streams. Absent counts as 1, so an
    // older file keeps working.
    addon[nAddon].source = 1;
    addon[nAddon].catalog = 1;
    addon[nAddon].subtitle = 0;
    { char *tab2 = strchr(tab + 1, '\t');
      if (tab2) {
        char *tab3;
        *tab2 = 0;
        tab3 = strchr(tab2 + 1, '\t');
        if (tab3) {
          char *tab4 = strchr(tab3 + 1, '\t');
          *tab3 = 0;
          if (tab4) { *tab4 = 0; addon[nAddon].subtitle = atoi(tab4 + 1); }
          addon[nAddon].catalog = atoi(tab3 + 1);
        }
        addon[nAddon].source = atoi(tab2 + 1);
      } }
    snprintf(addon[nAddon].name, sizeof addon[nAddon].name, "%s", line);
    snprintf(addon[nAddon].base, sizeof addon[nAddon].base, "%s", tab + 1);
    // The stored URL points at the manifest; the base is it without that suffix.
    n = strlen(addon[nAddon].base);
    if (n > 14 && !strcmp(addon[nAddon].base + n - 14, "/manifest.json"))
      addon[nAddon].base[n - 14] = 0;
    else while (n && addon[nAddon].base[n - 1] == '/') addon[nAddon].base[--n] = 0;
    nAddon++;
  }
  fclose(f);
  { int f = 0, k;
    for (k = 0; k < nAddon; k++) f += addon[k].source;
    printf("[addons] %d configured, %d provide streams\n", nAddon, f); }
  return nAddon;
}

// Signature of the current list: a sum over the bases. It only has to answer
// "did this change", so FNV-1a is enough — nothing here is security.
static unsigned listSignature(void) {
  unsigned h = 2166136261u;
  int i;
  for (i = 0; i < nAddon; i++) {
    const char *s;
    for (s = addon[i].base; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    h ^= '\n'; h *= 16777619u;
  }
  return h;
}

static int listChanged;

int addons_took_change(void) { int v = listChanged; listChanged = 0; return v; }

int addons_set_list(const AddonRemote *new, int n) {
  int i, accepted = 0;
  unsigned before = listSignature();
  if (!new || n <= 0) {
    // Empty does not replace. See the comment in the header: an empty response
    // is indistinguishable from a deletion, and the difference between the two
    // is whether the person is left with no sources at all.
    printf("[addons] account list came back empty; keeping the local one (%d)\n", nAddon);
    return 0;
  }
  for (i = 0; i < n && accepted < ADD_MAX; i++) {
    size_t k;
    if (!new[i].url[0] || !new[i].active) continue;
    snprintf(addon[accepted].name, sizeof addon[accepted].name, "%s",
             new[i].name[0] ? new[i].name : "Addon");
    snprintf(addon[accepted].base, sizeof addon[accepted].base, "%s", new[i].url);
    k = strlen(addon[accepted].base);
    if (k > 14 && !strcmp(addon[accepted].base + k - 14, "/manifest.json"))
      addon[accepted].base[k - 14] = 0;
    else while (k && addon[accepted].base[k - 1] == '/') addon[accepted].base[--k] = 0;
    // The account does not say what each addon supplies; the manifest would,
    // and querying them all at startup would cost one round trip per addon.
    // Assuming it supplies everything costs at most one extra empty query per
    // title — the opposite (assuming it does not) would hide real sources.
    addon[accepted].source = 1;
    addon[accepted].catalog = 1;
    addon[accepted].subtitle = 0;
    accepted++;
  }
  if (accepted == 0) {
    printf("[addons] account had only disabled addons; keeping the local list\n");
    return 0;
  }
  nAddon = accepted;
  if (listSignature() != before) listChanged = 1;
  printf("[addons] %d from the account%s\n", nAddon,
         listChanged ? " (list changed; the home rows will be rebuilt)" : "");
  return nAddon;
}

int addons_export(AddonRemote *output, int max) {
  int i, k = 0;
  for (i = 0; i < nAddon && k < max; i++) {
    snprintf(output[k].name, sizeof output[k].name, "%s", addon[i].name);
    snprintf(output[k].url, sizeof output[k].url, "%s", addon[i].base);
    output[k].active = 1;
    k++;
  }
  return k;
}

void addons_forget(void) {
  memset(addon, 0, sizeof addon);
  nAddon = 0;
  printf("[addons] list forgotten (signed out)\n");
}

int addons_n(void) { return nAddon; }

const char *addons_base(int i) {
  return (i >= 0 && i < nAddon) ? addon[i].base : "";
}

// Learns an addon's id from the manifest that was just read. The caller is
// readManifest in discover.c, which already holds both the base and the id — so
// this costs no extra request.
void addons_note_id(const char *base, const char *id) {
  int i;
  if (!base || !*base || !id || !*id) return;
  for (i = 0; i < nAddon; i++)
    if (!strcmp(addon[i].base, base)) {
      if (strcmp(addon[i].id, id))
        printf("[addons] '%s' is id '%s'\n", addon[i].name, id);
      snprintf(addon[i].id, sizeof addon[i].id, "%s", id);
      return;
    }
}

// The base URL of the addon with this id, or "" when it is not installed.
//
// THIS is how a collection source becomes a URL. The collection stores a short
// addonId; the real address is the INSTALLED addon's, which is the same rule the
// web app follows (findAddonForSource matches on id before looking at the stored
// URL). The URL the collection itself carries runs past 4 KB in these addons,
// because they embed their whole configuration in it, and it fits nowhere here.
const char *addons_base_for_id(const char *id) {
  int i;
  if (!id || !*id) return "";
  for (i = 0; i < nAddon; i++)
    if (addon[i].id[0] && !strcmp(addon[i].id, id)) return addon[i].base;
  return "";
}

// The fourth column of addons.txt. Like the stream one, absent counts as 1 — an
// older file keeps working, it just makes one extra query that may come back empty.
int addons_has_catalog(int i) {
  return (i >= 0 && i < nAddon) ? addon[i].catalog : 0;
}
AddState addons_state(void) {
  AddState e = atomic_load(&state);
  // Publish on the UI thread: no drawing ever observes a half-written list.
  if (threadAlive && e != ADD_SEARCHING) {
    pthread_join(thread, NULL);
    threadAlive = 0;
    if (!pendingId[0]) stream_set_list(result, nResult);
    free(result); result = NULL; nResult = 0;
    if (pendingId[0]) {
      char id[64], kind[16];
      snprintf(id, sizeof id, "%s", pendingId);
      snprintf(kind, sizeof kind, "%s", pendingKind);
      pendingId[0] = 0;
      addons_fetch(id, kind);
      return ADD_SEARCHING;
    }
  }
  return e;
}

// --- tolerant JSON reading ---------------------------------------------------
// A complete parser does not pay for itself here: the format is known and
// shallow, and what matters is never falling over on a missing field. Each
// function returns what it found or nothing, and the caller decides.

static const char *skipSpace(const char *p) {
  while (*p && (unsigned char)*p <= ' ') p++;
  return p;
}

// Copies the text value of "key" inside the object beginning at `obj`,
// respecting escapes. Returns 1 if it found one.
static int fieldText(const char *obj, const char *endObj, const char *key,
                      char *dst, size_t size) {
  char search[48];
  const char *p;
  size_t k = 0;
  snprintf(search, sizeof search, "\"%s\"", key);
  p = strstr(obj, search);
  if (!p || p >= endObj) return 0;
  p = skipSpace(p + strlen(search));
  if (*p != ':') return 0;
  p = skipSpace(p + 1);
  if (*p != '"') return 0;
  p++;
  while (*p && *p != '"' && k + 1 < size) {
    if (*p == '\\' && p[1]) {
      p++;
      // \u..... becomes "?" on purpose: the names arrive full of emoji and the
      // text is for display only. Decoding UTF-16 here would be work with no return.
      if (*p == 'u') { p += 5; dst[k++] = ' '; continue; }
      if (*p == 'n' || *p == 't' || *p == 'r') { p++; dst[k++] = ' '; continue; }
    }
    dst[k++] = *p++;
  }
  dst[k] = 0;
  return k > 0;
}

// Finds the end of the JSON object beginning at `p` (which points at '{').
static const char *endObject(const char *p) {
  int depth = 0, text = 0;
  for (; *p; p++) {
    if (text) { if (*p == '\\') p++; else if (*p == '"') text = 0; continue; }
    if (*p == '"') text = 1;
    else if (*p == '{') depth++;
    else if (*p == '}' && --depth == 0) return p + 1;
  }
  return p;
}

// --- subtitles ---------------------------------------------------------------

static Subtitle subs[SUB_MAX];
static int nSubs;
static pthread_t threadSub;
static int threadSubAlive, threadSubCreated, subStop;
static char subId[64], subKind[16];
static unsigned subGeneration;
static pthread_mutex_t subLock = PTHREAD_MUTEX_INITIALIZER;

int addons_n_subtitles(void) {
  int n;
  pthread_mutex_lock(&subLock); n = nSubs; pthread_mutex_unlock(&subLock);
  return n;
}
const Subtitle *addons_subtitle(int i) {
  const Subtitle *r = NULL;
  pthread_mutex_lock(&subLock);
  if (i >= 0 && i < nSubs) r = &subs[i];
  pthread_mutex_unlock(&subLock);
  return r;
}

// The languages this household cares about, in the order they should appear.
// Bringing in the 70 OpenSubtitles returns would be a list impossible to walk
// with a remote control.
static const char *LANGUAGES_PT[] = {
  "pob", "pt-br", "pt_br", "ptb", "br", "por", "pt"
};
static const char *LANGUAGES_EN[] = {
  "eng", "en", "en-us", "en_us", "en-gb", "en_gb"
};

// 0 = Portuguese, 1 = English. The user asked explicitly for these two groups;
// Spanish no longer comes in as a silent fallback. Regional variants are
// normalised here, before taking up one of the TV's twelve rows.
static int groupLanguage(const char *l) {
  size_t i;
  for (i = 0; i < sizeof LANGUAGES_PT / sizeof *LANGUAGES_PT; i++)
    if (!strcasecmp(l, LANGUAGES_PT[i])) return 0;
  for (i = 0; i < sizeof LANGUAGES_EN / sizeof *LANGUAGES_EN; i++)
    if (!strcasecmp(l, LANGUAGES_EN[i])) return 1;
  return -1;
}

static const char *nameLanguage(const char *c) {
  if (!strcasecmp(c, "pob") || !strcasecmp(c, "pt-br") ||
      !strcasecmp(c, "pt_br") || !strcasecmp(c, "ptb") || !strcasecmp(c, "br"))
    return "Portuguese (BR)";
  if (!strcasecmp(c, "por") || !strcasecmp(c, "pt")) return "Portuguese";
  if (groupLanguage(c) == 1) return "English";
  return c;
}

static int requestChanged(unsigned generation) {
  int changed;
  pthread_mutex_lock(&subLock);
  changed = subStop || generation != subGeneration;
  pthread_mutex_unlock(&subLock);
  return changed;
}

static void episodeRequest(const char *id, int *season, int *episode) {
  const char *p = strchr(id, ':');
  *season = *episode = 0;
  if (p) sscanf(p + 1, "%d:%d", season, episode);
}

static int episodeCorrect(const char *obj, const char *end, int season, int episode) {
  int t, e;
  if (season <= 0 || episode <= 0) return 1;
  t = (int)js_num(obj, end, "season", -1);
  e = (int)js_num(obj, end, "episode", -1);
  // Some older addons do not return the fields. When they do, they are a
  // guarantee: never show S2E3 in a search for S2E4.
  if ((t >= 0 && t != season) || (e >= 0 && e != episode)) return 0;
  if (t >= 0 || e >= 0) return 1;
    // Some addons omit season/episode but return the episode in the file name.
    // We used to accept S02E03 in a search for S2E4 and then fabricate the
    // label S2E4 from the request, hiding the error. If the name carries a
    // verifiable identity it has to match; a name with no marker is still accepted.
  { char name[160] = "", bottom[160]; size_t i;
    if (!js_text(obj, end, "subtitleFileName", name, sizeof name))
      js_text(obj, end, "movieReleaseName", name, sizeof name);
    for (i = 0; name[i] && i + 1 < sizeof bottom; i++)
      bottom[i] = (char)tolower((unsigned char)name[i]);
    bottom[i] = 0;
    for (i = 0; bottom[i]; i++) {
      int nt = -1, ne = -1;
      if (sscanf(bottom + i, "s%2de%2d", &nt, &ne) == 2 ||
          sscanf(bottom + i, "%2dx%2d", &nt, &ne) == 2)
        return nt == season && ne == episode;
    }
  }
  return 1;
}

static void *fetchSubtitles(void *u) {
  (void)u;
  for (;;) {
    Subtitle found[SUB_MAX] = {{0}};
    char id[64], kind[16];
    unsigned generation;
    int nFound = 0, season, episode, i;

    pthread_mutex_lock(&subLock);
    if (subStop) { threadSubAlive = 0; pthread_mutex_unlock(&subLock); return NULL; }
    snprintf(id, sizeof id, "%s", subId);
    snprintf(kind, sizeof kind, "%s", subKind);
    generation = subGeneration;
    pthread_mutex_unlock(&subLock);
    episodeRequest(id, &season, &episode);

    for (i = 0; i < nAddon && nFound < SUB_MAX; i++) {
      char url[900], *body;
      const char *p;
    // An addon that does not declare subtitles is not queried: AIOStreams would
    // answer empty and so would Xperience, two round trips with no return.
      if (!addon[i].subtitle) continue;
      snprintf(url, sizeof url, "%s/subtitles/%s/%s.json",
               addon[i].base, kind, id);
      body = net_download(url, 25);
      if (requestChanged(generation)) { free(body); break; }
      if (!body) continue;
      p = js_array(body, NULL, "subtitles");
      {
        int group;
        // One pass per group guarantees the PT -> EN order and stops twelve
        // Portuguese results consuming the whole list before English.
        // Six per language is a deliberate limit for D-pad navigation.
        for (group = 0; group < 2; group++) {
          const char *q = p;
          int inGroup = 0, j;
          for (j = 0; j < nFound; j++)
            if (groupLanguage(found[j].language) == group) inGroup++;
          while (q && nFound < SUB_MAX && inGroup < SUB_MAX / 2) {
            const char *f = js_end(q);
            char l[16] = "", name[120] = "";
            Subtitle *d = &found[nFound];
            if (episodeCorrect(q, f, season, episode) &&
                js_text(q, f, "lang", l, sizeof l) && groupLanguage(l) == group &&
                js_text(q, f, "url", d->url, sizeof d->url)) {
              js_text(q, f, "subtitleFileName", name, sizeof name);
              if (!name[0]) js_text(q, f, "movieReleaseName", name, sizeof name);
              snprintf(d->language, sizeof d->language, "%s", l);
              if (season > 0 && episode > 0)
                snprintf(d->label, sizeof d->label, "S%dE%d  \xc2\xb7  %s%s%.22s",
                         season, episode, nameLanguage(l), name[0] ? "  \xc2\xb7  " : "", name);
              else
                snprintf(d->label, sizeof d->label, "%s%s%.36s",
                         nameLanguage(l), name[0] ? "  \xc2\xb7  " : "", name);
              nFound++; inGroup++;
            }
            q = js_next(f);
          }
        }
      }
      free(body);
    }

    pthread_mutex_lock(&subLock);
    if (subStop) { threadSubAlive = 0; pthread_mutex_unlock(&subLock); return NULL; }
    if (generation != subGeneration) { pthread_mutex_unlock(&subLock); continue; }
    memcpy(subs, found, sizeof found);
    nSubs = nFound;
    threadSubAlive = 0;
    pthread_mutex_unlock(&subLock);
    printf("[subtitles] %s: %d\n", id, nFound);
    fflush(stdout);
    return NULL;
  }
}

void addons_fetch_subtitles(const char *imdb, const char *kind) {
  int series, merge = 0;
  char id[64], tp[16];
  if (!nAddon || !imdb || !*imdb) return;
  series = kind && !strcmp(kind, "series");
  if (series && !strchr(imdb, ':'))
    snprintf(id, sizeof id, "%s:1:1", imdb);
  else
    snprintf(id, sizeof id, "%s", imdb);
  snprintf(tp, sizeof tp, "%s", series ? "series" : "movie");

  pthread_mutex_lock(&subLock);
  if (!strcmp(id, subId) && !strcmp(tp, subKind) && (threadSubAlive || nSubs > 0)) {
    pthread_mutex_unlock(&subLock);
    return;
  }
  snprintf(subId, sizeof subId, "%s", id);
  snprintf(subKind, sizeof subKind, "%s", tp);
  subGeneration++;
  nSubs = 0;
  if (threadSubAlive) { pthread_mutex_unlock(&subLock); return; }
  merge = threadSubCreated;
  pthread_mutex_unlock(&subLock);

  if (merge) pthread_join(threadSub, NULL);
  pthread_mutex_lock(&subLock);
  threadSubCreated = 0;
  subStop = 0;
  threadSubAlive = 1;
  if (pthread_create(&threadSub, NULL, fetchSubtitles, NULL) != 0) threadSubAlive = 0;
  else threadSubCreated = 1;
  pthread_mutex_unlock(&subLock);
}

// ONE THREAD PER SOURCE ADDON.
//
// MEASURED ON THE TV, in the owner's session: 16.5 s between opening the title
// and having a chosen source (detail_open 51098 -> source chosen 67659). They
// were SERIAL queries with a 25 s timeout each; one slow addon delays all the
// others, and the screen sits on "searching" the whole time.
//
// The addons are independent and `extract` only writes into the bucket it is
// given, so each one reads into its own. The ORDER is preserved on the join: it
// decides which source automatic mode sees first — changing it changes the pick.
#define ADD_THREADS 4

typedef struct {
  int    idx;                 // qual addon
  Stream *found;
  int    n;
} BucketSource;

static BucketSource *buckets;
static int nBuckets, nextBucket;
static pthread_mutex_t addLock = PTHREAD_MUTEX_INITIALIZER;

static void *threadSources(void *u) {
  (void)u;
  for (;;) {
    int mine, i;
    char url[900], *body;
    pthread_mutex_lock(&addLock);
    if (nextBucket >= nBuckets) { pthread_mutex_unlock(&addLock); return NULL; }
    mine = nextBucket++;
    pthread_mutex_unlock(&addLock);
    i = buckets[mine].idx;
    snprintf(url, sizeof url, "%s/stream/%s/%s.json",
             addon[i].base, targetKind, targetId);
    // 12 s and not 25: with the addons in parallel the timeout stops adding up,
    // but it is still the time the owner waits for the slowest one.
    body = net_download(url, 12);
    if (!body) { printf("[addons] %s: no response\n", addon[i].name); continue; }
    buckets[mine].n = stream_parse(body, addon[i].name, &buckets[mine].found);
    printf("[addons] %s: %d sources (%u bytes)\n",
           addon[i].name, buckets[mine].n, (unsigned)strlen(body));
    free(body);
  }
}

static void *fetch(void *u) {
  Stream *found = NULL;
  int n = 0, i;
  (void)u;
  mark("addons: query start");

  nBuckets = 0; nextBucket = 0;
  buckets = calloc((size_t)(nAddon > 0 ? nAddon : 1), sizeof(BucketSource));
  if (buckets)
    for (i = 0; i < nAddon; i++)
      if (addon[i].source) buckets[nBuckets++].idx = i;

  if (buckets && nBuckets > 0) {
    pthread_t threads[ADD_THREADS];
    int created = 0, q;
    for (q = 0; q < ADD_THREADS && q < nBuckets; q++)
      if (pthread_create(&threads[created], NULL, threadSources, NULL) == 0) created++;
    if (!created) threadSources(NULL);        // no threads: in series, same result
    for (q = 0; q < created; q++) pthread_join(threads[q], NULL);
    // Joins IN ADDON ORDER, which is the order the owner installed them in.
    for (q = 0; q < nBuckets; q++) {
      int k = buckets[q].n;
      if (k > 0) {
        Stream *tmp = realloc(found, sizeof(Stream) * (size_t)(n + k));
        if (tmp) { found = tmp;
          memcpy(found + n, buckets[q].found, sizeof(Stream) * (size_t)k);
          n += k;
        } else printf("[addons] not enough memory for %d sources\n", k);
      }
      free(buckets[q].found);
    }
  }
  free(buckets); buckets = NULL; nBuckets = 0;

  mark(n ? "addons: sources received" : "addons: no sources");
  result = found; nResult = n;
  printf("[addons] total %d\n", n);
  fflush(stdout);
  atomic_store(&state, n ? ADD_READY : ADD_EMPTY);
  return NULL;
}

void addons_fetch(const char *imdb, const char *kind) {
  int series;
  if (!imdb || !*imdb) return;
  if (!nAddon) { stream_set_list(NULL, 0); state = ADD_EMPTY; return; }
  if (threadAlive) {
    if (strcmp(imdb, targetId) || strcmp(kind ? kind : "movie", targetKind)) {
      snprintf(pendingId, sizeof pendingId, "%s", imdb);
      snprintf(pendingKind, sizeof pendingKind, "%s", kind ? kind : "movie");
    }
    return;
  }
  stream_set_list(NULL, 0);
  series = kind && !strcmp(kind, "series");
  // A series WITHOUT an episode returns an empty list, with HTTP 200 and no
  // error at all (measured: a 14-byte response). The identifier has to be
  // "tt1234567:season:episode". Since the catalogue does not carry an episode
  // list yet, it assumes S1E1 — the same place the real episode goes in when
  // there is one.
  if (series && !strchr(imdb, ':'))
    snprintf(targetId, sizeof targetId, "%s:1:1", imdb);
  else
    snprintf(targetId, sizeof targetId, "%s", imdb);
  snprintf(targetKind, sizeof targetKind, "%s", kind && *kind ? kind : "movie");
  state = ADD_SEARCHING;
  threadAlive = 1;
  if (pthread_create(&thread, NULL, fetch, NULL) != 0) { threadAlive = 0; state = ADD_STOPPED; }
}

void addons_shutdown(void) {
  int mergeSub;
  if (threadAlive) pthread_join(thread, NULL);
  threadAlive = 0;
  pthread_mutex_lock(&subLock);
  subStop = 1; subGeneration++; mergeSub = threadSubCreated;
  pthread_mutex_unlock(&subLock);
  if (mergeSub) pthread_join(threadSub, NULL);
  pthread_mutex_lock(&subLock);
  threadSubCreated = threadSubAlive = 0; nSubs = 0;
  pthread_mutex_unlock(&subLock);
  free(result); result = NULL; nResult = 0;
  state = ADD_STOPPED;
}
