#include "addons.h"
#include "streams.h"
#include "net.h"
#include "js.h"
#include "mark.h"
#include "lang.h"
#include "settings.h"
#include "data.h"
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
static void preForget(void);

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
    //
    // `subtitle` used to be the one field this paragraph did not apply to: it was
    // born 0 here while source and catalog were born 1, and the only thing that
    // could raise it was the fifth column of art/addons.txt — a file the .ipk
    // strips on purpose (tools/arm.sh, PERSONAL_FILES). On every package built
    // since the login, then, fetchSubtitles skipped EVERY addon and the
    // OpenSubtitles half of the subtitle sheet was empty by construction, on
    // every title. It follows the same rule as the other two now.
    addon[accepted].source = 1;
    addon[accepted].catalog = 1;
    addon[accepted].subtitle = 1;
    accepted++;
  }
  for (; i < n; i++)
    if (new[i].url[0] && new[i].active)
      data_log("addons.log", "app DROPPED '%s' (past the app's %d-addon cap)", new[i].name, ADD_MAX);
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
  // The prefetched list came through the previous person's addons.
  preForget();
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
// The file extra for subId (addons_subtitles_file), already URL-encoded; "" for none.
static char subExtra[1800];
static unsigned subGeneration;
static void addons_fetch_subtitles_extra(const char *imdb, const char *kind, const char *extra);
static pthread_mutex_t subLock = PTHREAD_MUTEX_INITIALIZER;

int addons_n_subtitles(void) {
  int n;
  pthread_mutex_lock(&subLock); n = nSubs; pthread_mutex_unlock(&subLock);
  return n;
}
// 1 while the search is still out. addons_n_subtitles() answers 0 both for "none
// found" and for "not back yet", and the auto-selection has to tell them apart:
// giving up on the first frame would skip a list that arrives a second later.
int addons_subtitles_busy(void) {
  int busy;
  pthread_mutex_lock(&subLock); busy = threadSubAlive; pthread_mutex_unlock(&subLock);
  return busy;
}

const Subtitle *addons_subtitle(int i) {
  const Subtitle *r = NULL;
  pthread_mutex_lock(&subLock);
  if (i >= 0 && i < nSubs) r = &subs[i];
  pthread_mutex_unlock(&subLock);
  return r;
}

// A code's language as lang.h's index: 0 Portuguese, 1 English, and the rest of
// that table after them; -1 when it is none of them. Regional variants fold into
// their language here, before taking up one of the TV's twelve rows.
static int groupLanguage(const char *l) { return lang_of(l); }

// The same answer, for whoever is OUTSIDE this file. The auto-selection in
// tracks.c has to group an EMBEDDED track's language ("por", "eng", read from
// the MKV header) by the same rule as an addon's, and copying the tables there
// would give two rules that drift apart on the first regional variant added.
int addons_language_group(const char *code) {
  return code && *code ? groupLanguage(code) : -1;
}

static const char *nameLanguage(const char *c) {
  if (!strcasecmp(c, "pob") || !strcasecmp(c, "pt-br") ||
      !strcasecmp(c, "pt_br") || !strcasecmp(c, "ptb") || !strcasecmp(c, "br"))
    return "Portuguese (BR)";
  if (!strcasecmp(c, "por") || !strcasecmp(c, "pt")) return "Portuguese";
  if (groupLanguage(c) >= 0) return lang_name(groupLanguage(c));
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
    // On the heap: 200 of them are ~180 KB, too much for a thread's stack.
    Subtitle *found = calloc(SUB_MAX, sizeof *found);
    char id[64], kind[16], extra[sizeof subExtra];
    unsigned generation;
    int nFound = 0, season, episode, i;

    if (!found) { pthread_mutex_lock(&subLock); threadSubAlive = 0;
                  pthread_mutex_unlock(&subLock); return NULL; }
    pthread_mutex_lock(&subLock);
    if (subStop) { threadSubAlive = 0; pthread_mutex_unlock(&subLock); free(found); return NULL; }
    snprintf(id, sizeof id, "%s", subId);
    snprintf(kind, sizeof kind, "%s", subKind);
    snprintf(extra, sizeof extra, "%s", subExtra);
    generation = subGeneration;
    pthread_mutex_unlock(&subLock);
    episodeRequest(id, &season, &episode);

    for (i = 0; i < nAddon && nFound < SUB_MAX; i++) {
      char url[2600], *body;
      const char *p;
    // An addon that does not declare subtitles is not queried: AIOStreams would
    // answer empty and so would Xperience, two round trips with no return.
      if (!addon[i].subtitle) continue;
      // The Stremio path for an extra is one more segment: /subtitles/<type>/<id>/<extra>.json
      snprintf(url, sizeof url, "%s/subtitles/%s/%s%s%s.json",
               addon[i].base, kind, id, extra[0] ? "/" : "", extra);
      body = net_download(url, 25);
      if (requestChanged(generation)) { free(body); break; }
      if (!body) { data_log("addons.log", "subtitles %s from '%s': no answer", id, addon[i].name); continue; }
      p = js_array(body, NULL, "subtitles");
      {
        // ENGLISH, plus the Subtitles row's language when it names another.
        // Downloads used to be Portuguese then English, six of each — the app's
        // first owner's pair — and a list in a language nobody here reads is
        // one the remote has to walk past. The file's own tracks are not
        // filtered: this is only what gets FETCHED.
        const int group = LANG_ENGLISH, extra = settings_subtitle_language();
        const char *q = p;
        int seen = 0, before = nFound, nHosts = 0, h;
        // Where each file is served from, tallied for the log: an aggregator
        // merges several providers into one answer, and the host is the one
        // thing that still says which provider a row came from.
        struct { char host[48]; int n; } hosts[6];
        // Walks the WHOLE answer, full list or not, so the log says how much there was.
        while (q) {
          const char *f = js_end(q);
          seen++;
          { char u[600] = "", host[48] = "";
            if (js_text(q, f, "url", u, sizeof u) && strstr(u, "://")) {
              const char *a = strstr(u, "://") + 3;
              snprintf(host, sizeof host, "%.*s", (int)strcspn(a, "/:?"), a);
              for (h = 0; h < nHosts && strcmp(hosts[h].host, host); h++) {}
              if (h == nHosts && nHosts < 6) {
                snprintf(hosts[nHosts].host, sizeof hosts[0].host, "%s", host);
                hosts[nHosts++].n = 0;
              }
              if (h < nHosts) hosts[h].n++;
            } }
          if (nFound >= SUB_MAX) { q = js_next(f); continue; }
          char l[16] = "", name[120] = "";
          Subtitle *d = &found[nFound];
          if (episodeCorrect(q, f, season, episode) &&
              js_text(q, f, "lang", l, sizeof l) &&
              (groupLanguage(l) == group || (extra >= 0 && groupLanguage(l) == extra)) &&
              js_text(q, f, "url", d->url, sizeof d->url)) {
            js_text(q, f, "subtitleFileName", name, sizeof name);
            if (!name[0]) js_text(q, f, "movieReleaseName", name, sizeof name);
            snprintf(d->language, sizeof d->language, "%s", l);
            snprintf(d->release, sizeof d->release, "%s", name);
            snprintf(d->source, sizeof d->source, "%s", addon[i].name);
            { char m[4] = "";
              js_text(q, f, "m", m, sizeof m);
              d->hashMatch = !strcmp(m, "h");
              d->fpsMilli = (int)js_num(q, f, "fpsMilli", 0); }
            if (season > 0 && episode > 0)
              snprintf(d->label, sizeof d->label, "S%dE%d  \xc2\xb7  %s%s%.22s",
                       season, episode, nameLanguage(l), name[0] ? "  \xc2\xb7  " : "", name);
            else
              snprintf(d->label, sizeof d->label, "%s%s%.36s",
                       nameLanguage(l), name[0] ? "  \xc2\xb7  " : "", name);
            nFound++;
          }
          q = js_next(f);
        }
        { char tally[400] = ""; size_t tk = 0;
          for (h = 0; h < nHosts && tk + 60 < sizeof tally; h++)
            tk += (size_t)snprintf(tally + tk, sizeof tally - tk, "%s%s x%d", h ? ", " : "",
                                   hosts[h].host, hosts[h].n);
          data_log("addons.log", "subtitles %s from '%s': %d read, %d kept%s | %s", id, addon[i].name,
                   seen, nFound - before, nFound >= SUB_MAX ? " (list FULL)" : "", tally); }
      }
      free(body);
    }
    for (; i < nAddon; i++)
      if (addon[i].subtitle)
        data_log("addons.log", "subtitles %s from '%s': NOT ASKED (list full)", id, addon[i].name);

    pthread_mutex_lock(&subLock);
    if (subStop) { threadSubAlive = 0; pthread_mutex_unlock(&subLock); free(found); return NULL; }
    if (generation != subGeneration) { pthread_mutex_unlock(&subLock); free(found); continue; }
    memcpy(subs, found, SUB_MAX * sizeof *found);
    free(found);
    nSubs = nFound;
    threadSubAlive = 0;
    pthread_mutex_unlock(&subLock);
    printf("[subtitles] %s: %d\n", id, nFound);
    fflush(stdout);
    return NULL;
  }
}

void addons_fetch_subtitles(const char *imdb, const char *kind) {
  addons_fetch_subtitles_extra(imdb, kind, "");
}

// addons_fetch_subtitles with the file extra already decided: "" for a new title
// (the previous file's hash is not this title's), the playing file's for a restart.
static void addons_fetch_subtitles_extra(const char *imdb, const char *kind, const char *extra) {
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
  snprintf(subExtra, sizeof subExtra, "%s", extra);
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

// encodeURIComponent, as buildExtraParams uses it: everything but the unreserved
// characters is %XX. 0 when it does not fit.
static int encodeComponent(const char *src, char *dst, size_t size) {
  static const char HEX[] = "0123456789ABCDEF";
  size_t k = 0;
  for (; *src; src++) {
    unsigned char c = (unsigned char)*src;
    if (isalnum(c) || strchr("-_.!~*'()", c)) {
      if (k + 1 >= size) return 0;
      dst[k++] = (char)c;
    } else {
      if (k + 3 >= size) return 0;
      dst[k++] = '%'; dst[k++] = HEX[c >> 4]; dst[k++] = HEX[c & 15];
    }
  }
  dst[k] = 0;
  return 1;
}

void addons_subtitles_file(const char *videoHash, long long videoSize, const char *filename) {
  char extra[sizeof subExtra], part[1400], id[64], kind[16];
  size_t k = 0;
  extra[0] = 0;
  // Same order and same omissions as the web's buildExtraParams: an empty value
  // is left out, and a size is sent only when there is one.
  if (videoHash && *videoHash && encodeComponent(videoHash, part, sizeof part))
    k += (size_t)snprintf(extra + k, sizeof extra - k, "videoHash=%s", part);
  if (videoSize > 0 && k < sizeof extra)
    k += (size_t)snprintf(extra + k, sizeof extra - k, "%svideoSize=%lld", k ? "&" : "", videoSize);
  if (filename && *filename && k < sizeof extra) {
    // The name only: an addon's filename is sometimes a path inside the torrent.
    const char *base = strrchr(filename, '/');
    base = base ? base + 1 : filename;
    if (encodeComponent(base, part, sizeof part))
      k += (size_t)snprintf(extra + k, sizeof extra - k, "%sfilename=%s", k ? "&" : "", part);
  }
  if (k >= sizeof extra) return;   // truncated would be a wrong extra: send none
  pthread_mutex_lock(&subLock);
  if (!subId[0] || !strcmp(extra, subExtra)) { pthread_mutex_unlock(&subLock); return; }
  snprintf(subExtra, sizeof subExtra, "%s", extra);
  snprintf(id, sizeof id, "%s", subId);
  snprintf(kind, sizeof kind, "%s", subKind);
  // Clearing subId makes addons_fetch_subtitles take this as a new request — and
  // it would then forget the extra, so it is put back straight after.
  subId[0] = 0;
  pthread_mutex_unlock(&subLock);
  printf("[subtitles] searching for the playing file%s%s\n",
         videoHash && *videoHash ? " (hash)" : "", filename && *filename ? " (name)" : "");
  fflush(stdout);
  addons_fetch_subtitles_extra(id, kind, extra);
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
  int idx;                 // qual addon
  Stream *found;
  int    n;
} BucketSource;

// One search's worth of work, on the heap and not in globals: the normal fetch
// and the next-episode prefetch can be running at the same time.
typedef struct {
  const char *id, *kind;
  BucketSource *buckets;
  int nBuckets, nextBucket;
  pthread_mutex_t lock;
} Round;

static void *threadSources(void *u) {
  Round *r = (Round *)u;
  for (;;) {
    int mine, i;
    char url[900], *body;
    pthread_mutex_lock(&r->lock);
    if (r->nextBucket >= r->nBuckets) { pthread_mutex_unlock(&r->lock); return NULL; }
    mine = r->nextBucket++;
    pthread_mutex_unlock(&r->lock);
    i = r->buckets[mine].idx;
    snprintf(url, sizeof url, "%s/stream/%s/%s.json",
             addon[i].base, r->kind, r->id);
    // 12 s and not 25: with the addons in parallel the timeout stops adding up,
    // but it is still the time the owner waits for the slowest one.
    body = net_download(url, 12);
    if (!body) { printf("[addons] %s: no response\n", addon[i].name); continue; }
    r->buckets[mine].n = stream_parse(body, addon[i].name, &r->buckets[mine].found);
    printf("[addons] %s: %d sources (%u bytes)\n",
           addon[i].name, r->buckets[mine].n, (unsigned)strlen(body));
    free(body);
  }
}

// Asks every source addon for `id` and returns the merged list in *out.
static int gather(const char *id, const char *kind, Stream **out) {
  Stream *found = NULL;
  int n = 0, i;
  Round r;
  memset(&r, 0, sizeof r);
  r.id = id; r.kind = kind;
  pthread_mutex_init(&r.lock, NULL);
  r.buckets = calloc((size_t)(nAddon > 0 ? nAddon : 1), sizeof(BucketSource));
  if (r.buckets)
    for (i = 0; i < nAddon; i++)
      if (addon[i].source) r.buckets[r.nBuckets++].idx = i;

  if (r.buckets && r.nBuckets > 0) {
    pthread_t threads[ADD_THREADS];
    int created = 0, q;
    for (q = 0; q < ADD_THREADS && q < r.nBuckets; q++)
      if (pthread_create(&threads[created], NULL, threadSources, &r) == 0) created++;
    if (!created) threadSources(&r);          // no threads: in series, same result
    for (q = 0; q < created; q++) pthread_join(threads[q], NULL);
    // Joins IN ADDON ORDER, which is the order the owner installed them in.
    for (q = 0; q < r.nBuckets; q++) {
      int k = r.buckets[q].n;
      if (k > 0) {
        Stream *tmp = realloc(found, sizeof(Stream) * (size_t)(n + k));
        if (tmp) { found = tmp;
          memcpy(found + n, r.buckets[q].found, sizeof(Stream) * (size_t)k);
          n += k;
        } else printf("[addons] not enough memory for %d sources\n", k);
      }
      free(r.buckets[q].found);
    }
  }
  free(r.buckets);
  pthread_mutex_destroy(&r.lock);
  *out = found;
  return n;
}

static void *fetch(void *u) {
  Stream *found = NULL;
  int n;
  (void)u;
  mark("addons: query start");
  n = gather(targetId, targetKind, &found);
  mark(n ? "addons: sources received" : "addons: no sources");
  result = found; nResult = n;
  printf("[addons] total %d\n", n);
  fflush(stdout);
  atomic_store(&state, n ? ADD_READY : ADD_EMPTY);
  return NULL;
}

// THE NEXT EPISODE'S SOURCES, FETCHED BEFORE THEY ARE ASKED FOR.
//
// Next episode used to start its search only once the current one was over, so
// every episode of a binge opened with the full addon wait — as long as the
// slowest addon, up to 12 s — on a black screen. Near the end of an episode the
// player asks for the next one's list ahead of time, and addons_fetch hands it
// over at once when the id matches and the list is still fresh.
//
// Fresh means under a minute old, the same rule the title page uses
// (NV_LINK_VALID_MS in app.c): a debrid link expires in minutes. That is why
// the prefetch is REPEATED while the end of the episode lasts, not done once —
// credits can run past a minute. Whatever comes out of it is still checked
// source by source before playing, like any other list.
#define PRE_VALID_MS   60000u
#define PRE_REFRESH_MS 45000u

static pthread_t threadPre;
static int preAlive, preDiscard;      // UI thread only
static _Atomic int preDone;
static char preWantId[64], preWantKind[16];
static Stream *preNew; static int preNewN;
static char preId[64], preKind[16];
static Stream *preList; static int preN;
static Uint32 preAt;

static void *prefetch(void *u) {
  (void)u;
  preNewN = gather(preWantId, preWantKind, &preNew);
  printf("[addons] prefetched %s: %d\n", preWantId, preNewN);
  fflush(stdout);
  atomic_store(&preDone, 1);
  return NULL;
}

// Collects a finished prefetch into the cache. UI thread.
static void preReap(void) {
  if (!preAlive || !atomic_load(&preDone)) return;
  pthread_join(threadPre, NULL);
  preAlive = 0;
  free(preList); preList = NULL; preN = 0;
  if (!preDiscard && preNewN > 0) {
    preList = preNew; preN = preNewN; preAt = SDL_GetTicks();
    snprintf(preId, sizeof preId, "%s", preWantId);
    snprintf(preKind, sizeof preKind, "%s", preWantKind);
  } else free(preNew);
  preNew = NULL; preNewN = 0; preDiscard = 0;
}

static void preClear(void) {
  free(preList); preList = NULL; preN = 0; preId[0] = 0;
}

static void preForget(void) {
  preClear();
  if (preAlive) preDiscard = 1;
}

void addons_prefetch(const char *id, const char *kind) {
  if (!nAddon || !id || !*id) return;
  preReap();
  if (preAlive) return;                              // one in flight already
  if (preN > 0 && !strcmp(id, preId) && SDL_GetTicks() - preAt < PRE_REFRESH_MS)
    return;                                          // still fresh
  snprintf(preWantId, sizeof preWantId, "%s", id);
  snprintf(preWantKind, sizeof preWantKind, "%s", kind && *kind ? kind : "movie");
  atomic_store(&preDone, 0);
  preAlive = 1;
  if (pthread_create(&threadPre, NULL, prefetch, NULL) != 0) preAlive = 0;
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
  preReap();
  if (preN > 0 && !strcmp(imdb, preId) && !strcmp(kind ? kind : "movie", preKind) &&
      SDL_GetTicks() - preAt < PRE_VALID_MS) {
    printf("[addons] %s: %d prefetched sources, %ums old\n",
           imdb, preN, (unsigned)(SDL_GetTicks() - preAt));
    mark("addons: prefetched sources used");
    stream_set_list(preList, preN);
    preClear();
    state = ADD_READY;
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
  if (preAlive) { pthread_join(threadPre, NULL); preAlive = 0; free(preNew); preNew = NULL; }
  preClear();
  state = ADD_STOPPED;
}
