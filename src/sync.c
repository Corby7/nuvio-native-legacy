#include "mark.h"
#include "sync.h"
#include "watchedep.h"
#include "debrid.h"
#include "sourcepref.h"
#include "session.h"
#include "cloud.h"
#include "profiles.h"
#include "data.h"
#include "addons.h"
#include "trakt.h"
#include "catalog.h"
#include "settings.h"
#include "discover.h"
#include "extras.h"
#include "collections.h"
#include "homerows.h"
#include "acclib.h"
#include "js.h"
#include "jsw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#define SY_ADD_MAX   32
#define SY_PROGRESS_MAX 240

static pthread_t thread;
static int threadAlive, threadReady;
// THE EARLY HALF OF A CYCLE. Set by the sync thread once everything that shapes
// the home has landed (addons, keys, collections, settings, row order), so the
// main thread can apply it without waiting for the progress, the library and
// the pushes behind it. Read and written through __atomic: the main thread reads
// the blobs it guards only after seeing it set.
static int earlyReady, earlyApplied;
static SyncState state = SYNC_STOPPED;
static char summary[220] = "not synced";
static unsigned lastOk;
static int dirtyProgress, dirtyAddons;
// THE PROFILE A CYCLE WAS STARTED FOR. A switch made while one is running
// changes profiles_active() under it: its pulls are a mix of the two profiles,
// and its pushes would send the one being left's addons up to the new one's
// account. sync_step throws such a cycle away and starts another.
static int cycleProfile;
// Set by a profile switch: once the new profile's progress lands, the home is
// rebuilt if the file changed, so "Continue watching" is theirs and not the
// cache's.
static int switchRebuild;
// A cycle asked for while one ran (sync_resync): started when that one ends.
static int againWanted;

// The thread does NOT touch the app: it only fills these boxes, and sync_step
// applies them on the main loop. Without that separation, a network response
// would rewrite the addon list in the middle of a frame already reading from it.
static AddonRemote addonsRemote[SY_ADD_MAX];
static int nAddonsRemote, hasAddonsRemote;
// The raw bodies behind the addon list and the home's row order, handed to the
// main thread like collectionsBlob — see "THE LAST ACCOUNT STATE" below.
static char *addonsBlob, *homeBlob;
// homerows_generation() when the body in homeBlob was requested.
static unsigned homeGeneration;
static int  hasHomeBlob;

static char traktToken[300];
static int  hasTraktRemote;

// Service keys the account stores and the app used to read from the owner's
// file. MEASURED on the real account: the providers present are animeskip,
// debrid:*, introdb, mdblist and tmdb — and there is NO "trakt". The trakt
// reader stays here because the RPC is the same and the row will appear as soon
// as the web app writes it.
static char tmdbKey[120], mdbKey[120];
static int  hasTmdb, hasMdb;

// `ms` is when the record was last touched, and it travels in BOTH directions:
// out of progress.txt's sixth column on the push, and off the account's
// `updated_at`/`last_watched` on the pull. Without it the pull stamped the clock
// on every row it applied and the whole recency order of "Continue watching"
// was lost — see the note on cat_save_progress_at.
typedef struct { char imdb[40]; double pos, duration; int temp, ep; long long ms;
                 int origin; } ProgressItem;
static ProgressItem progressRemote[SY_PROGRESS_MAX];
static int nProgressRemote;
// 1 when the last pull returned the account's WHOLE list — an array, not cut
// short by SY_PROGRESS_MAX. Only then may a line missing from it be read as
// "deleted on another device"; see pruneDeleted in sync_step.
static int progressComplete;
// What the last push put on the account, so the main thread can hand those
// lines over to the account (origin 1 -> 2) once it has applied the pull.
static struct { char imdb[40]; long long ms; } pushedMark[SY_PROGRESS_MAX];
static int nPushedMark;

// Counts of what has been pulled but the app does not consume yet. They exist so
// the summary can tell the truth instead of saying "synced" without qualifying it.
static int cWatched, cLib, cCollections, hasSettingsProfile, hasCatHome;

// The profile's settings blob, raw, waiting to be applied on the main thread.
// `applySettings` starts on: at boot there is no local change to preserve, and
// that is when the account has to be in charge.
//
// ALLOCATED, and not a fixed array. MEASURED on the TV with a real account: the
// blob did not fit in 4096 bytes and the app refused to apply it — the refusal
// was right (applying half the options would bring half the account and half
// the defaults), but the effect was the feature simply not working. The web app
// keeps far more keys in that same object than this app knows about, and
// choosing a ceiling here means choosing an account that will not work.
static char *settingsBlob;
static int  hasSettingsBlob;
static int  applySettings = 1;

// The owner's COLLECTIONS, raw, waiting for the main thread. For the same reason
// as the settings blob and the addon list: `folders[]` in collections.c is read
// by the drawing code every frame, and rewriting it from another thread would
// corrupt the home in the middle of a frame already walking the list.
static char *collectionsBlob;
static int  hasCollectionsBlob;

// ---------------------------------------------------------------- utilitarios

static int ok2xx(const char *r, int st) { return r && st >= 200 && st < 300; }

// ---------------------------------------------------------------- addons

static int parseAddons(const char *r, AddonRemote *out, int max);

static void pullAddons(void) {
  char query[400], owner[80];
  char *r;
  int st = 0, k = 0;
  const char *p;

  if (!profiles_owner()[0]) return;
  cloud_url_escape(profiles_owner(), owner, sizeof owner);
  // MEASURED: `sync_pull_addons` DOES NOT EXIST on this server (PGRST202), and
  // neither does the `tv_addons` table (PGRST205). The only path that answers is
  // the `addons` table, which is exactly the web app's happy path.
  snprintf(query, sizeof query,
           "user_id=eq.%s&profile_id=eq.%d&select=*&order=sort_order.asc",
           owner, profiles_addon_profile());
  // With the anonymous key the RLS answers 401 "permission denied for table
  // addons": reading someone's rows requires the token of whoever is asking.
  r = session_table("addons", query, &st);
  if (!ok2xx(r, st)) {
    if (r && cloud_error_missing(r)) printf("[sync] addons table missing\n");
    else if (st) printf("[sync] addon read: HTTP %d\n", st);
    free(r);
    return;
  }
  // ADDONS.LOG: every row the account sent, in order, with what became of it —
  // the only way to see from outside why an installed addon never shows up.
  { int row = 0;
    for (p = js_root_array(r); p; p = js_next(js_end(p)), row++) {
      const char *f = js_end(p), *u = strstr(p, "\"url\"");
      char name[64] = "", b[16];
      size_t len = 0;
      js_text(p, f, "name", name, sizeof name);
      if (u && u < f) { u = strchr(u + 5, '"'); if (u && u < f) { const char *e = u + 1;
        while (e < f && *e != '"') { if (*e == '\\') e++; e++; } len = (size_t)(e - u - 1); } }
      data_log("addons.log", "account #%d '%s' url %zu bytes %s -> %s", row, name, len,
               js_raw(p, f, "enabled", b, sizeof b) && !strcmp(b, "false") ? "disabled" : "enabled",
               row >= SY_ADD_MAX ? "DROPPED (past the account cap)"
               : len >= sizeof addonsRemote[0].url ? "DROPPED (URL too long)" : "read");
    } }
  k = parseAddons(r, addonsRemote, SY_ADD_MAX);
  free(addonsBlob);
  addonsBlob = r;               // ownership passes to the box; do not free here
  nAddonsRemote = k;
  hasAddonsRemote = 1;
}

// The account's addon rows -> `out`. Separate from the pull because the same
// body is parsed again at startup, from the copy kept on disk.
static int parseAddons(const char *r, AddonRemote *out, int max) {
  const char *p;
  int k = 0;
  for (p = js_root_array(r); p && k < max; p = js_next(js_end(p))) {
    const char *f = js_end(p);
    char b[16];
    memset(&out[k], 0, sizeof out[k]);
    // THE SAME TRAP AS THE COLLECTIONS, and here it costs a whole addon: js_text
    // refuses silently when the value does not fit, and addons that keep their
    // configuration inside the URL run well past this field's 600 bytes. Without
    // this line the addon would simply not exist for the app, with not a word in
    // the log to say why.
    if (!js_text(p, f, "url", out[k].url, sizeof out[k].url)) {
      char name[64] = "";
      js_text(p, f, "name", name, sizeof name);
      printf("[sync] addon '%s' SKIPPED: its URL does not fit in %d bytes\n",
             name[0] ? name : "(unnamed)", (int)sizeof out[k].url);
      continue;
    }
    js_text(p, f, "name", out[k].name, sizeof out[k].name);
    // Absent counts as ON: that is how the web app reads it, and an addon that
    // disappears because of a field the server did not send is worse than an
    // extra one.
    out[k].active = js_raw(p, f, "enabled", b, sizeof b)
                    ? (strcmp(b, "false") != 0) : 1;
    k++;
  }
  return k;
}

static void pushAddons(void) {
  static AddonRemote current[SY_ADD_MAX];   // 32 x 668 bytes: too much for a stack
  Jsw w;
  char *r;
  int st = 0, n, i;

  // A profile on the primary's addons has no list of its own to write, and
  // writing to the primary's from here is not its call. The Android TV app skips
  // the push the same way.
  if (profiles_addon_profile() != profiles_active()) { dirtyAddons = 0; return; }
  n = addons_export(current, SY_ADD_MAX);
  // An empty local list does NOT become a push. An empty push wipes the
  // person's addons on every device of theirs, and "I have not loaded anything
  // yet" is indistinguishable from "the user removed everything" on this side.
  if (n <= 0) return;

  jsw_start(&w);
  jsw_obj_start(&w);
  jsw_ci(&w, "p_profile_id", profiles_active());
  jsw_key(&w, "p_addons");
  jsw_arr_start(&w);
  for (i = 0; i < n; i++) {
    jsw_obj_start(&w);
    jsw_cs(&w, "url", current[i].url);
    jsw_ci(&w, "sort_order", i);
    jsw_cb(&w, "enabled", current[i].active);
    if (current[i].name[0]) jsw_cs(&w, "name", current[i].name);
    jsw_obj_end(&w);
  }
  jsw_arr_end(&w);
  jsw_obj_end(&w);
  r = session_rpc("sync_push_addons", jsw_text_final(&w), &st);
  jsw_free(&w);
  if (!ok2xx(r, st)) printf("[sync] addon push failed (HTTP %d)\n", st);
  else dirtyAddons = 0;
  free(r);
}

// ---------------------------------------------------------------- credenciais

static void pullCredentials(void) {
  Jsw w;
  char *r;
  int st = 0;
  const char *p;

  jsw_start(&w);
  jsw_obj_start(&w);
  jsw_ci(&w, "p_profile_id", profiles_active());
  jsw_obj_end(&w);
  r = session_rpc("sync_pull_provider_credentials", jsw_text_final(&w), &st);
  jsw_free(&w);
  if (!ok2xx(r, st)) { free(r); return; }
  // The debrid keys are re-read whole on every successful pull: a key the
  // account no longer has (removed, or another profile's) must stop resolving.
  debrid_forget();

  // Trakt, debrid and mdblist share the SAME RPCs, separated only by the
  // `provider` field. One read serves all three.
  for (p = js_root_array(r); p; p = js_next(js_end(p))) {
    const char *f = js_end(p);
    char provider[48], cred[900];
    if (!js_text(p, f, "provider", provider, sizeof provider)) continue;
    if (!js_raw(p, f, "credential_json", cred, sizeof cred)) continue;
    if (!strcmp(provider, "trakt")) {
      // credential_json may arrive as an OBJECT or as a JSON string — the web
      // app handles both. Here it is enough to look for the key in the raw text.
      char tk[300];
      if (js_text(cred, cred + strlen(cred), "access_token", tk, sizeof tk)) {
        snprintf(traktToken, sizeof traktToken, "%s", tk);
        hasTraktRemote = 1;
      }
    }
    else if (!strcmp(provider, "tmdb")) {
      if (js_text(cred, cred + strlen(cred), "api_key", tmdbKey, sizeof tmdbKey))
        hasTmdb = 1;
    }
    else if (!strcmp(provider, "mdblist")) {
      if (js_text(cred, cred + strlen(cred), "api_key", mdbKey, sizeof mdbKey))
        hasMdb = 1;
    }
    else if (!strncmp(provider, "debrid:", 7)) {
      // The loose key resolves torrents that arrive with no url (debrid.c).
      // Addons with the key embedded in their URL are not affected: those
      // streams already come with a url.
      char k[200];
      if (js_text(cred, cred + strlen(cred), "api_key", k, sizeof k))
        debrid_set_key(provider + 7, k);
    }
  }
  free(r);
}

// ---------------------------------------------------------------- progresso

// 1 when the two ids name the same WORK: only the part before the ':' counts,
// because one side may carry an episode suffix and the other may not.
static int sameWorkId(const char *a, const char *b) {
  if (!a || !b) return 0;
  while (*a && *b && *a != ':' && *b != ':') { if (*a != *b) return 0; a++; b++; }
  return (!*a || *a == ':') && (!*b || *b == ':');
}

// WHEN A REMOTE ROW WAS LAST TOUCHED, in ms since the epoch, from whichever of
// the three spellings the server used. The web app reads exactly these, in this
// order (mapProgressRow, watchProgressSyncService.js).
//
// A number below 1e12 is SECONDS — that is the same test the web makes, and the
// reason for it is that the column has been written both ways over the life of
// the schema. A value that is not a number at all is an ISO timestamp, and the
// civil-date arithmetic below converts it without touching the C library: mktime
// applies the TV's timezone and timegm is a GNU extension this build does not
// otherwise rely on, so neither can answer this honestly.
static long long isoToMs(const char *s) {
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
  long long era, yoe, doy, doe, days;
  int yy;
  if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) < 3) return 0;
  if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
  // Howard Hinnant's days_from_civil: exact, branch-light and with no lookup
  // table of month lengths to get wrong in a leap year.
  yy = y - (mo <= 2);
  era = (yy >= 0 ? yy : yy - 399) / 400;
  yoe = yy - era * 400;
  doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  days = era * 146097 + doe - 719468;
  return ((days * 24 + h) * 60 + mi) * 60000LL + se * 1000LL;
}

static long long remoteInstantMs(const char *p, const char *f) {
  static const char *const KEYS[] = { "updated_at", "last_watched", "lastWatched" };
  unsigned k;
  for (k = 0; k < sizeof KEYS / sizeof *KEYS; k++) {
    char text[40] = "";
    double v = js_num(p, f, KEYS[k], -1.0);
    if (v > 0.0) return (long long)(v > 1e12 ? v : v * 1000.0);
    if (js_text(p, f, KEYS[k], text, sizeof text) && text[0]) {
      long long ms = isoToMs(text);
      if (ms > 0) return ms;
    }
  }
  return 0;
}

static void pullProgress(void) {
  Jsw w;
  char *r;
  int st = 0, k = 0;
  const char *p;

  progressComplete = 0;
  jsw_start(&w);
  jsw_obj_start(&w);
  jsw_ci(&w, "p_profile_id", profiles_active());
  jsw_obj_end(&w);
  r = session_rpc("sync_pull_watch_progress", jsw_text_final(&w), &st);
  jsw_free(&w);
  if (!ok2xx(r, st)) { free(r); return; }

  for (p = js_root_array(r); p && k < SY_PROGRESS_MAX; p = js_next(js_end(p))) {
    const char *f = js_end(p);
    double pos, duration;
    int temp, ep;
    char id[40];
    if (!js_text(p, f, "content_id", id, sizeof id)) continue;
    // The web app accepts position_ms/duration_ms and position/duration; the
    // first pair wins when present, because the second already arrives in
    // milliseconds from this RPC and mixing the two units makes everything 100%.
    pos = js_num(p, f, "position_ms", -1.0);
    duration = js_num(p, f, "duration_ms", -1.0);
    if (pos < 0) pos = js_num(p, f, "position", 0);
    if (duration < 0) duration = js_num(p, f, "duration", 0);
    pos /= 1000.0;
    duration /= 1000.0;
    if (duration <= 1.0) continue;
    temp = (int)js_num(p, f, "season", 0);
    ep   = (int)js_num(p, f, "episode", 0);
    // A row whose season and episode columns are empty may still name the
    // episode in `video_id`: that is where the web app puts it when the addon
    // has no video id of its own ("__nuvio_episode__:12:14"), and reading it
    // back is what lets a record written before the columns were filled in
    // still resume the right episode instead of the series at large.
    if (temp <= 0 || ep <= 0) {
      char video[64] = "";
      if (js_text(p, f, "video_id", video, sizeof video)) {
        const char *tail = strstr(video, "__nuvio_episode__:");
        if (tail) sscanf(tail + 18, "%d:%d", &temp, &ep);
      }
    }
    if (temp > 0 && ep > 0)
      snprintf(progressRemote[k].imdb, sizeof progressRemote[k].imdb, "%s:%d:%d", id, temp, ep);
    else
      snprintf(progressRemote[k].imdb, sizeof progressRemote[k].imdb, "%s", id);
    progressRemote[k].pos = pos;
    progressRemote[k].duration = duration;
    progressRemote[k].temp = temp;
    progressRemote[k].ep = ep;
    progressRemote[k].ms = remoteInstantMs(p, f);
    k++;
  }
  // Whole only when the body really was an array and the loop ran out of rows
  // rather than out of room.
  { const char *b = r;
    while (*b == ' ' || *b == '\n' || *b == '\r' || *b == '\t') b++;
    progressComplete = *b == '[' && !(p && k == SY_PROGRESS_MAX); }
  free(r);
  // Empty deletes nothing: the consumer only applies what arrived.
  nProgressRemote = k;
}

int sync_progress_keys(const char *work, char (*out)[40], int max) {
  Jsw w;
  char *r;
  int st = 0, n = 0;
  const char *p;
  if (!work || !work[0] || max <= 0 || !session_loggedin()) return 0;
  jsw_start(&w);
  jsw_obj_start(&w);
  jsw_ci(&w, "p_profile_id", profiles_active());
  jsw_obj_end(&w);
  r = session_rpc("sync_pull_watch_progress", jsw_text_final(&w), &st);
  jsw_free(&w);
  if (!ok2xx(r, st)) { free(r); return 0; }
  for (p = js_root_array(r); p && n < max; p = js_next(js_end(p))) {
    const char *f = js_end(p);
    char id[40];
    if (!js_text(p, f, "content_id", id, sizeof id) || strcmp(id, work)) continue;
    if (js_text(p, f, "progress_key", out[n], sizeof out[n]) && out[n][0]) n++;
  }
  free(r);
  return n;
}

// Reads the progress THIS device recorded. It is the only surface where the
// native app has real information of its own — which is why it is the only one,
// along with the addons, that it pushes.
static int readProgressLocal(ProgressItem *output, int max) {
  char *buf, *line, *ctx;
  int k = 0;
  { char name[64];
    buf = data_read(cat_progress_file(profiles_active(), name, sizeof name)); }
  if (!buf) return 0;
  for (line = strtok_r(buf, "\n", &ctx); line && k < max;
       line = strtok_r(NULL, "\n", &ctx)) {
    char id[40], *colon;
    double pos, duration;
    int season = 0, episode = 0, origin = 0, fields;
    long long ms = 0;
    // ALL SIX COLUMNS, and this is the whole of the first defect.
    //
    // It used to read three — id, position, duration — and nothing else, so the
    // season and episode a line carries in columns four and five never reached
    // the push below. An episode the player had recorded correctly as
    // "tt0121955  234  1323  12  14" therefore went up to the account as a
    // MOVIE with a null season, came back down with no episode on it, and
    // landed in "Continue watching" as a series that could not say which
    // episode it was resuming — let alone which one comes next.
    fields = sscanf(line, "%39s %lf %lf %d %d %lld %d",
                    id, &pos, &duration, &season, &episode, &ms, &origin);
    if (fields < 3) continue;
    if (duration <= 1.0) continue;
    // Lines an older build wrote carry the episode in the id and not in the
    // columns. Both spellings are read here so the push says the same thing
    // about either, and the id is reduced to the work in both cases.
    colon = strchr(id, ':');
    if (colon) {
      if (season <= 0 && episode <= 0) sscanf(colon + 1, "%d:%d", &season, &episode);
      *colon = 0;
    }
    if (!id[0]) continue;
    snprintf(output[k].imdb, sizeof output[k].imdb, "%s", id);
    output[k].pos = pos;
    output[k].duration = duration;
    output[k].temp = season > 0 ? season : 0;
    output[k].ep   = episode > 0 ? episode : 0;
    output[k].ms = ms;
    output[k].origin = origin;
    k++;
  }
  free(buf);
  return k;
}

static void pushProgress(void) {
  ProgressItem local[SY_PROGRESS_MAX];
  Jsw w;
  char *r;
  int n, i, st = 0;

  n = readProgressLocal(local, SY_PROGRESS_MAX);
  if (n <= 0) return;   // empty never becomes a push; deletion has its own RPC

  // A LINE WHOSE INSTANT THIS DEVICE CANNOT VOUCH FOR IS NOT PUSHED.
  //
  // Two kinds fail that test. origin 0 is a line from before the column existed,
  // carrying a stamp a previous build invented — the moment of the SYNC, not of
  // the watching. And an origin-2 line with no instant at all is a row the
  // account handed over without one, which is most of this TV's file today
  // BECAUSE the old push never sent last_watched: the server stored null, and
  // null is what comes back.
  //
  // The server resolves conflicts by last_watched, so pushing either would
  // overwrite the account's own dating — on every device the person owns,
  // permanently — with something this device made up. Neither has anything to
  // tell the account that the account does not already know: both came FROM it.
  // Playback here always qualifies, so nothing this TV actually did is lost.
  //
  // AND A LINE THAT CAME FROM THE ACCOUNT IS NEVER PUSHED BACK, instant or not.
  // It has nothing to tell the account — and the push upserts, so re-sending a
  // copy re-creates the row if another device has deleted it since. Every push
  // (after any playback here) used to re-send every copy in the file, which is
  // how a title removed from "Continue watching" on the phone came back.
  { int r, w;
    for (r = 0, w = 0; r < n; r++)
      if (local[r].origin == 1) { if (w != r) local[w] = local[r]; w++; }
    if (w != n) printf("[sync] %d local progress line(s) held back:"
                       " copies of the account, not playback here\n", n - w);
    n = w; }
  if (n <= 0) return;

  jsw_start(&w);
  jsw_obj_start(&w);
  jsw_ci(&w, "p_profile_id", profiles_active());
  jsw_cs(&w, "p_origin_client_id", data_client_id());
  jsw_key(&w, "p_entries");
  jsw_arr_start(&w);
  for (i = 0; i < n; i++) {
    // readProgressLocal has already reduced the id to the WORK and lifted the
    // season and episode out of whichever place the line kept them, so there is
    // nothing left to take apart here.
    const char *id = local[i].imdb;
    int temp = local[i].temp, ep = local[i].ep;
    char key[64], video[64];

    // THE TWO KEYS ARE THE WEB APP'S, byte for byte, and they have to be.
    //
    // The account's rows are unique on (content_id, video_id, season, episode)
    // and on progress_key, and the web writes `<id>_s<season>e<episode>` and
    // `__nuvio_episode__:<season>:<episode>` for an episode (toProgressKey and
    // toRemoteVideoId, watchProgressSyncService.js). This app used to send the
    // raw local id as progress_key and no video_id at all, so the same episode
    // watched on the phone and on the TV became two rows that neither client
    // could reconcile.
    if (temp > 0 && ep > 0) {
      snprintf(key,   sizeof key,   "%s_s%de%d", id, temp, ep);
      snprintf(video, sizeof video, "__nuvio_episode__:%d:%d", temp, ep);
    } else {
      snprintf(key,   sizeof key,   "%s", id);
      snprintf(video, sizeof video, "%s", id);
    }

    jsw_obj_start(&w);
    jsw_cs(&w, "content_id", id);
    jsw_cs(&w, "content_type", (temp > 0) ? "series" : "movie");
    jsw_cs(&w, "video_id", video);
    jsw_ci(&w, "position", (long long)(local[i].pos * 1000.0));
    jsw_ci(&w, "duration", (long long)(local[i].duration * 1000.0));
    if (temp > 0) { jsw_ci(&w, "season", temp); jsw_ci(&w, "episode", ep); }
    else          { jsw_key(&w, "season"); jsw_null(&w);
                    jsw_key(&w, "episode"); jsw_null(&w); }
    // WHEN, not "now". The server resolves a conflict between two clients by
    // this field (rowFreshness in the web app reads last_watched), so a device
    // that does not send it wins or loses at random.
    jsw_ci(&w, "last_watched",
           local[i].ms > 0 ? local[i].ms : (long long)time(NULL) * 1000);
    jsw_cs(&w, "progress_key", key);
    jsw_obj_end(&w);
  }
  jsw_arr_end(&w);
  jsw_obj_end(&w);
  r = session_rpc("sync_push_watch_progress", jsw_text_final(&w), &st);
  jsw_free(&w);
  if (!ok2xx(r, st)) printf("[sync] progress push failed (HTTP %d)\n", st);
  else {
    dirtyProgress = 0;
    // Now on the account: from here on the account's copy is the record. See
    // the hand-over at the end of the progress block in sync_step.
    for (i = 0; i < n && i < SY_PROGRESS_MAX; i++) {
      snprintf(pushedMark[i].imdb, sizeof pushedMark[i].imdb, "%s", local[i].imdb);
      pushedMark[i].ms = local[i].ms;
    }
    nPushedMark = n < SY_PROGRESS_MAX ? n : SY_PROGRESS_MAX;
  }
  free(r);
}

// ---------------------------------------------------------------- read only

// Counts the items of an RPC that returns an array. These surfaces are pulled
// but not consumed yet: the native app has no screen of its own for them, and
// PUSHING without having the screen would send an empty list — which wipes the
// data on the person's other devices. Counting and saying so in the summary is
// the honest behaviour until the screen exists.
// An RPC the server does not have is NOT asked for again. MEASURED:
// `sync_pull_saved_library` does not exist on this server, and without this
// list the app would spend one round trip per cycle, forever, against a 404
// that never changes.
#define SY_MISSING 8
static const char *missing[SY_MISSING];
static int nMissing;

static int alreadyMissing(const char *func) {
  int i;
  for (i = 0; i < nMissing; i++)
    if (!strcmp(missing[i], func)) return 1;
  return 0;
}

// TRAKT'S TOKEN DOES NOT COME FROM pullCredentials ANY MORE.
//
// The backend retired tracker storage from the provider-credential table: a
// push of { provider: "trakt" } answers 400 with PG 22023 "Unsupported provider
// credential: trakt", and nothing is ever written there, so the read above finds
// no `trakt` row no matter which device did the linking. The web app moved to a
// pair of dedicated RPCs (traktCredentialSyncService.js) and this follows it —
// otherwise the TV can push a link nobody can read and read a link nobody can
// push.
//
// tmdb and mdblist STAY in pullCredentials: those are API keys, not tracker
// tokens, and that table is still where they live.
static int trackerTakeTrakt(const char *p, const char *f) {
  char name[48], tk[300];
  // The row names the tracker in one of three ways depending on which view it
  // comes back from; the web app accepts all three (trackerNameOf).
  if (!js_text(p, f, "tracker", name, sizeof name) &&
      !js_text(p, f, "provider", name, sizeof name) &&
      !js_text(p, f, "tracker_name", name, sizeof name)) return 0;
  if (strcmp(name, "trakt")) return 0;
  if (!js_text(p, f, "access_token", tk, sizeof tk)) return 0;
  snprintf(traktToken, sizeof traktToken, "%s", tk);
  hasTraktRemote = 1;
  // Silence on this path is what made the old failure so hard to place: the pull
  // finding nothing and the pull not running look identical in a log that says
  // neither.
  printf("[sync] trakt token from the account\n");
  return 1;
}

static void pullTracker(void) {
  static const char *const FUNC = "get_tracker_tokens";
  Jsw w;
  char *r;
  int st = 0;
  const char *p;
  // The same brake the other optional RPCs use: a server without this function
  // would otherwise cost one round trip per cycle against a 404 that will never
  // change its mind.
  if (alreadyMissing(FUNC)) return;
  jsw_start(&w);
  jsw_obj_start(&w);
  jsw_ci(&w, "p_profile_id", profiles_active());
  jsw_obj_end(&w);
  r = session_rpc(FUNC, jsw_text_final(&w), &st);
  jsw_free(&w);
  if (!ok2xx(r, st)) {
    if (r && cloud_error_missing(r)) {
      printf("[sync] %s does not exist on this server\n", FUNC);
      if (nMissing < SY_MISSING) missing[nMissing++] = FUNC;
    }
    free(r);
    return;
  }
  for (p = js_root_array(r); p; p = js_next(js_end(p)))
    if (trackerTakeTrakt(p, js_end(p))) break;
  // js_root_array answers NULL for a body that is one bare object, and the
  // envelope is not pinned down by the schema — the web app accepts a row set OR
  // a single row for exactly this reason.
  if (!hasTraktRemote && r && *r == '{') trackerTakeTrakt(r, r + strlen(r));
  free(r);
}

// The settings blob is NOT counted, it is read: it is the person's layout. Until
// now this RPC only fed a number in the summary, and the ~40 preferences came
// from defaults transcribed by hand from the profile of whoever built the package.
static int pullSettingsProfile(const char *body) {
  char *r;
  int st = 0, ok = 0;
  const char *p;
  if (!applySettings) return hasSettingsProfile;   // nothing to do this time round
  r = session_rpc("sync_pull_profile_settings_blob", body, &st);
  if (!ok2xx(r, st)) { free(r); return 0; }
  // The response is [{ "settings_json": { ... } }]; what matters is the object
  // inside, raw and WHOLE.
  p = js_root_array(r);
  if (p) {
    const char *endObj = js_end(p);
    const char *k = strstr(p, "\"settings_json\"");
    if (k && k < endObj) {
      const char *v = strchr(k, ':');
      if (v) {
        v++;
        while (*v && (unsigned char)*v <= ' ') v++;
        if (*v == '{') {
          const char *f = js_end(v);
          size_t n = (size_t)(f - v);
          char *new = (char *)malloc(n + 1);
          if (new) {
            memcpy(new, v, n);
            new[n] = 0;
            free(settingsBlob);
            settingsBlob = new;
            hasSettingsBlob = 1;
            ok = 1;
            printf("[sync] settings blob: %d bytes\n", (int)n);
          }
        } else {
          // The web app also accepts the blob as a serialised JSON STRING. This
          // server returns an object; if one day it returns a string, the right
          // thing is to say so, not to apply a fragment.
          printf("[sync] settings blob did not arrive as an object; not applied\n");
        }
      }
    }
  }
  free(r);
  return ok;
}

// The owner's COLLECTIONS. Until now this RPC also only fed a number.
//
// It keeps the body raw instead of parsing here: the parsing belongs to
// collections.c, on the main thread, because the list it writes is the one the
// drawing code reads every frame.
static int pullCollections(const char *body) {
  static const char *FUNC = "sync_pull_collections";
  char *r;
  int st = 0, k = 0;
  const char *p;
  if (alreadyMissing(FUNC)) return -1;
  r = session_rpc(FUNC, body, &st);
  if (!ok2xx(r, st)) {
    if (r && cloud_error_missing(r)) {
      printf("[sync] %s does not exist on this server\n", FUNC);
      if (nMissing < SY_MISSING) missing[nMissing++] = FUNC;
    }
    free(r);
    return -1;
  }
  for (p = js_root_array(r); p; p = js_next(js_end(p))) k++;
  free(collectionsBlob);
  collectionsBlob = r;          // ownership passes to the box; do not free here
  hasCollectionsBlob = 1;
  return k;
}

// THE HOME'S ROW PREFERENCES, which until now were downloaded and thrown away.
//
// This RPC was already being called — by countRpc, which reads the whole body
// just to count how many items came back and return a number for the summary.
// The order the person chose in the web app, what they hid and the names they
// gave all reached the TV and died there. With 151 catalogues declared and a
// 16-row cap, what appeared on the home was the start of the addon's manifest,
// never the owner's choice.
//
// The item shape comes from the web's homeCatalogSettingsSyncService:
//   {"addon_id":..,"type":..,"catalog_id":..,"enabled":bool,"order":n,
//    "custom_title":"","is_collection":bool,"collection_id":""}
// and the key is addon_id + "_" + type + "_" + catalog_id, identical to the one
// discover builds for every declared catalogue.
//
// Fetched on the sync thread, APPLIED on the main one (applyHomeCatalog, from
// sync_step or the startup restore), like the collections.
static int pullHomeCatalog(const char *body) {
  static const char *FUNC = "sync_pull_home_catalog_settings";
  char *r;
  int st = 0;
  // Taken BEFORE the request: an edit committed while it is in flight bumps the
  // counter, and this body then carries the order from before that edit.
  unsigned generationAtPull = homerows_generation();
  if (alreadyMissing(FUNC)) return 0;
  r = session_rpc(FUNC, body, &st);
  if (!ok2xx(r, st)) {
    if (r && cloud_error_missing(r)) {
      printf("[sync] %s does not exist on this server\n", FUNC);
      if (nMissing < SY_MISSING) missing[nMissing++] = FUNC;
    }
    free(r);
    return 0;
  }
  free(homeBlob);
  homeBlob = r;                 // ownership passes to the box; do not free here
  homeGeneration = generationAtPull;
  hasHomeBlob = 1;
  return 1;
}

// The row order in `r` -> discover's preferences. The number of rows it placed;
// 0 leaves the preferences exactly as they were.
static int applyHomeCatalog(const char *r) {
  int n = 0, nCollection = 0;
  const char *root, *item;
  root = js_root_array(r);
  if (!root) return 0;
  // The body is [{"settings_json":{... ,"items":[...]}}].
  item = js_array(root, js_end(root), "items");
  if (!item) {
    printf("[sync] home catalog settings: no \"items\"\n");
    return 0;
  }
  disc_prefs_begin();
  for (; item; item = js_next(js_end(item))) {
    const char *f = js_end(item);
    char addonId[96] = "", type[16] = "", catId[96] = "", title[96] = "";
    char key[192];
    js_text(item, f, "custom_title", title, sizeof title);
    // A COLLECTION. It joins the SAME ordered list, under the key the web uses
    // (`collection_<id>`), so its row lands exactly where the owner placed it
    // among the catalogues. It used to be counted and discarded, which is why
    // collections could only ever appear at the end, behind everything else.
    if (js_flag(item, f, "is_collection", 0)) {
      char collectionId[96] = "";
      js_text(item, f, "collection_id", collectionId, sizeof collectionId);
      if (!collectionId[0]) continue;
      snprintf(key, sizeof key, "collection_%s", collectionId);
      disc_prefs_add(key, js_flag(item, f, "enabled", 1), title);
      nCollection++;
      n++;
      continue;
    }
    js_text(item, f, "addon_id",   addonId, sizeof addonId);
    js_text(item, f, "type",       type,    sizeof type);
    js_text(item, f, "catalog_id", catId,   sizeof catId);
    if (!addonId[0] || !type[0] || !catId[0]) continue;
    snprintf(key, sizeof key, "%s_%s_%s", addonId, type, catId);
    disc_prefs_add(key, js_flag(item, f, "enabled", 1), title);
    n++;
  }
  // The Trakt rows are not in the account's blob; they keep their place here.
  homerows_merge_trakt();
  disc_prefs_end();
  // The owner's collections take part in the SAME ordering in the web app, and
  // this app has nowhere to get their contents from yet. Counting them and
  // saying how many there are beats ignoring them silently: it is the
  // difference between "I have no collections" and "I have some and this app
  // does not show them yet".
  if (nCollection)
    printf("[sync] %d collection(s) placed in the home order\n", nCollection);
  return n;
}

// What shapes the HOME: the collections, the settings profile and the row order.
// Pulled in the early half of the cycle; see earlyReady.
static void pullHomeState(void) {
  char body[160];
  int profile = profiles_active();

  snprintf(body, sizeof body, "{\"p_profile_id\":%d}", profile);
  // COLLECTIONS: downloaded in full now, not merely counted. The body is kept
  // for the main thread to apply (see collectionsBlob).
  cCollections = pullCollections(body);

  snprintf(body, sizeof body,
           "{\"p_profile_id\":%d,\"p_platform\":\"tv\"}", profile);
  hasSettingsProfile = pullSettingsProfile(body);

  snprintf(body, sizeof body,
           "{\"p_profile_id\":%d,\"p_platform\":\"home_catalog_shared\"}", profile);
  hasCatHome = pullHomeCatalog(body);
}

static void pullSoRead(void) {
  char body[160];
  int profile = profiles_active();

  snprintf(body, sizeof body, "{\"p_profile_id\":%d}", profile);

  // THE LIBRARY AND THE WATCHED LIST. Without Trakt they are the account's, and
  // acclib reads them in full and pushes this TV's edits (see acclib.h for why
  // the library push has to be built on a complete pull). With Trakt linked,
  // Trakt is the source and they are only counted, as before.
  if (acclib_active()) {
    acclib_sync();
    cLib = acclib_count_library();
    cWatched = acclib_count_watched();
  } else {
    // NOT COUNTED ANY MORE. With Trakt linked these two lists are not what the
    // TV shows — Trakt is — and the only reader of the count was the summary
    // line in Settings. Counting meant downloading up to 200 items of each, on
    // every cycle, for two numbers; the summary now leaves them out.
    // (sync_pull_saved_library, counted here too, is gone for the same reason:
    // nothing read it, and the server reports it does not exist.)
    cLib = cWatched = -1;
  }
}

// ---------------------------------------------------------------- cycle

// A LIGHT cycle carries only the library and the watched list: it is what a
// gesture on the TV asks for (sync_soon), and the other eight requests of a full
// cycle have nothing to do with it.
static int light;

static void *run(void *u) {
  (void)u;
  if (light) {
    acclib_sync();
    state = SYNC_READY;
    threadReady = 1;
    return NULL;
  }
  profiles_pull();
  pullAddons();
  pullCredentials();
  pullHomeState();
  // Everything the home is built from is in hand: the main thread applies it now
  // (sync_step), and the rebuild it asks for runs beside the rest of this cycle
  // instead of after it. That rest used to be five or six more round trips.
  __atomic_store_n(&earlyReady, 1, __ATOMIC_RELEASE);
  pullTracker();
  pullProgress();
  pullSoRead();
  // Push AFTER pulling, like the web app's startupSyncService: pulling after
  // pushing would make the device overwrite with what it sent itself.
  // Not for a profile that has been left since the cycle began (see cycleProfile):
  // the flags stay up, and the cycle sync_step starts in its place pushes them.
  if (profiles_active() == cycleProfile) {
    if (dirtyAddons)    pushAddons();
    if (dirtyProgress) pushProgress();
  }

  { char lists[64] = "";
    // Unknown (-1) when Trakt is the source: see pullSoRead.
    if (cWatched >= 0 && cLib >= 0)
      snprintf(lists, sizeof lists, " · %d watched · %d in list", cWatched, cLib);
    snprintf(summary, sizeof summary,
             "%d addons · %d progress%s · %d collections%s",
             nAddonsRemote, nProgressRemote, lists,
             cCollections < 0 ? 0 : cCollections,
             hasTraktRemote ? " · Trakt" : ""); }
  state = SYNC_READY;
  threadReady = 1;
  return NULL;
}

int sync_delete_progress(const char *const *keys, int n) {
  Jsw w;
  char *r;
  int st = 0, i, ok;
  if (!session_loggedin()) return 1;
  if (!keys || n <= 0) return 1;
  jsw_start(&w);
  jsw_obj_start(&w);
  jsw_ci(&w, "p_profile_id", profiles_active());
  jsw_key(&w, "p_keys");
  jsw_arr_start(&w);
  for (i = 0; i < n; i++) jsw_str(&w, keys[i]);
  jsw_arr_end(&w);
  jsw_obj_end(&w);
  r = session_rpc("sync_delete_watch_progress", jsw_text_final(&w), &st);
  jsw_free(&w);
  ok = ok2xx(r, st);
  printf("[sync] progress delete (%d key(s)) -> HTTP %d\n", n, st);
  free(r);
  return ok;
}

// A gesture on the TV (a title saved, an episode marked) waiting for its cycle.
static int soon;
static unsigned soonSeen;

static void begin(int isLight, const char *why) {
  if (threadAlive || !session_loggedin()) return;
  if (cloud_brake_active()) return;
  light = isLight;
  cycleProfile = profiles_active();
  // A full cycle carries the library and watched list too, so it answers any
  // pending gesture as well.
  soon = 0;
  state = SYNC_RUNNING;
  threadReady = 0;
  __atomic_store_n(&earlyReady, 0, __ATOMIC_RELEASE);
  earlyApplied = 0;
  // SYNC.LOG in the data folder, readable over ssh: the periodic cycle was
  // implemented and never seen firing on the TV, because nothing on the device
  // recorded that it had.
  data_log("sync.log", "cycle start (%s%s)", why, isLight ? ", light" : "");
  if (pthread_create(&thread, NULL, run, NULL) == 0) { pthread_detach(thread); threadAlive = 1; }
  else { state = SYNC_FAILED; snprintf(summary, sizeof summary, "no thread to sync with"); }
}

void sync_start(void) { begin(0, "requested"); }

void sync_soon(void) { soon = 1; soonSeen = 0; }

// Coming back after this long without a frame counts as coming back to the app.
#define SYNC_RESUME_GAP_MS 20000u
// ...but not if a cycle finished this recently.
#define SYNC_RESUME_MIN_MS 30000u
// How long a gesture waits, so a burst of them (three titles saved in a row)
// travels in one cycle.
#define SYNC_SOON_DELAY_MS 1500u

// One automatic cycle, if one is due. Returns 1 when it fired.
// Separate from sync_step because the caller knows whether the moment is right:
// during playback it is NOT — a burst of HTTP in the middle of the video
// competes for CPU and network with the decoder, and one stutter costs more
// than 5 minutes of delay on the progress.
int sync_periodic(unsigned nowMs) {
  // THE TV COMING BACK. Two ways it shows here, and neither needs an event from
  // the system: a frozen app sees a long gap in its own clock between two
  // frames; a TV back from standby does NOT (CLOCK_MONOTONIC stops in suspend)
  // but the wall clock has jumped ahead of it. Either way what was done on the
  // phone meanwhile should be on screen now, not up to five minutes later.
  static unsigned lastTick;
  static time_t lastWall;
  time_t wall = time(NULL);
  int resumed = 0;
  if (lastTick) {
    unsigned tickGap = nowMs - lastTick;
    long long wallGapMs = (long long)(wall - lastWall) * 1000LL;
    if (tickGap > SYNC_RESUME_GAP_MS ||
        wallGapMs > (long long)tickGap + (long long)SYNC_RESUME_GAP_MS) resumed = 1;
  }
  lastTick = nowMs;
  lastWall = wall;

  if (!session_loggedin() || threadAlive) return 0;
  if (cloud_brake_active()) return 0;
  if (resumed && lastOk && nowMs - lastOk > SYNC_RESUME_MIN_MS) { begin(0, "resume"); return 1; }
  if (soon) {
    if (!soonSeen) soonSeen = nowMs ? nowMs : 1;
    if (nowMs - soonSeen >= SYNC_SOON_DELAY_MS) { begin(1, "local edit"); return 1; }
  }
  // With no successful cycle yet, whoever called sync_start is in charge — there
  // is no point insisting on top of a failure the brake is already holding.
  if (!lastOk) return 0;
  if (nowMs - lastOk < SYNC_INTERVAL_MS) return 0;
  begin(0, "periodic");
  return 1;
}

// --- THE LAST ACCOUNT STATE, KEPT ON DISK --------------------------------------
//
// MEASURED on the C3 (2026-09-24): the home was built TWICE on every launch.
// The first build ran with no addons (the package ships none) and the
// manifest's row order; ~1.1 s later the sync delivered the account's addons,
// row order and collections, and each of the three called disc_rebuild — so the
// real home arrived at ~5.5 s, after a second round of every catalogue request.
// The same three triggers fired on EVERY periodic cycle too, rebuilding the home
// every few minutes although nothing had changed.
//
// Now the last body of each is written to the data folder (per profile) and
// applied before the first build, and a sync rebuilds only when a body differs
// from the one already applied. The first launch after signing in still builds
// twice; every one after that builds once.
static char *lastAddons, *lastHome, *lastCollections;


static void stateName(char *dst, size_t size, const char *what) {
  snprintf(dst, size, "account-p%d-%s.json", profiles_active(), what);
}

// 1 when `body` differs from what was applied last (and becomes the new last,
// on disk too); 0 when it is the same text.
static int stateChanged(char **last, const char *what, const char *body) {
  char name[64];
  if (*last && !strcmp(*last, body)) return 0;
  free(*last);
  *last = strdup(body);
  stateName(name, sizeof name, what);
  data_write(name, body);
  return 1;
}

static char *stateRead(char **last, const char *what) {
  char name[64];
  free(*last);
  stateName(name, sizeof name, what);
  *last = data_read(name);
  return *last;
}

const char *sync_home_blob(void) { return lastHome; }
void sync_home_store(const char *body) {
  if (body && *body) stateChanged(&lastHome, "home-catalog", body);
}

void sync_restore(void) {
  const char *body;
  if (!session_loggedin()) return;
  if ((body = stateRead(&lastAddons, "addons"))) {
    static AddonRemote saved[SY_ADD_MAX];
    int n = parseAddons(body, saved, SY_ADD_MAX);
    if (n > 0) addons_set_list(saved, n);
    // The list is the one the build is about to use: nothing to rebuild for.
    addons_took_change();
  }
  if ((body = stateRead(&lastHome, "home-catalog"))) applyHomeCatalog(body);
  if ((body = stateRead(&lastCollections, "collections"))) col_load_account(body);
  // The account library too, so the first build already carries its cards.
  acclib_restore();
  mark("account state restored");
}

static void stateForget(void) {
  free(lastAddons); free(lastHome); free(lastCollections);
  lastAddons = lastHome = lastCollections = NULL;
}

// The early half: what the home is built from. Its flags are only ever written
// by the sync thread BEFORE earlyReady, so reading them here once it is set is
// safe while the thread goes on with the rest of the cycle.
static void applyEarly(void) {
  if (hasAddonsRemote) {
    addons_set_list(addonsRemote, nAddonsRemote);
    hasAddonsRemote = 0;
    if (addonsBlob) stateChanged(&lastAddons, "addons", addonsBlob);
    free(addonsBlob);
    addonsBlob = NULL;
    // The home was assembled BEFORE this list arrived, so with no addons at all:
    // zero manifests read, zero catalogues, and the home falling back to the
    // packaged catalogue. Now that there is a list, the rows are rebuilt.
    if (addons_took_change()) { mark("rebuild: addons changed"); disc_rebuild(); }
  }
  if (hasCollectionsBlob && collectionsBlob) {
    // Main thread: this is the only place the collection list can be rewritten
    // without racing the drawing code.
    if (stateChanged(&lastCollections, "collections", collectionsBlob) &&
        col_load_account(collectionsBlob) > 0) {
      mark("rebuild: collections");
      disc_rebuild();
    }
    free(collectionsBlob);
    collectionsBlob = NULL;
    hasCollectionsBlob = 0;
  }
  // A pull that started before the last edit on the Home rows screen, or one
  // that lands while that edit is still on its way to the account, holds the
  // order from BEFORE it. Applying it would undo the edit on screen; the next
  // cycle brings the account's copy with the edit in.
  if (hasHomeBlob && homeBlob &&
      (homeGeneration != homerows_generation() || homerows_push_in_flight())) {
    printf("[sync] home catalog pull predates a local edit; not applied\n");
    free(homeBlob);
    homeBlob = NULL;
    hasHomeBlob = 0;
  }
  if (hasHomeBlob && homeBlob) {
    // The home was built in whatever order it had. With the owner's order in
    // hand — and only if it is not the order already applied — the rows are
    // rebuilt, so the ones that fit are the ones they chose, not the first ones
    // the addon happened to declare.
    if (stateChanged(&lastHome, "home-catalog", homeBlob) && applyHomeCatalog(homeBlob) > 0) {
      mark("rebuild: home catalog order");
      disc_rebuild();
    }
    free(homeBlob);
    homeBlob = NULL;
    hasHomeBlob = 0;
  }
  // extras.c fetches the TMDB fact sheet with disc_key_tmdb(), so this key landing
  // changes what a fetch can return — and that module caches per title id. Without
  // the notice, any title opened in the second before the account answered kept an
  // empty status, runtime, release date and country list until the app restarted.
  if (hasTmdb)      { disc_tmdb_set(tmdbKey); extras_keys_changed(); hasTmdb = 0; }
  if (hasMdb)       { extras_set_key(mdbKey); hasMdb = 0; }
  if (hasSettingsBlob && settingsBlob) {
    settings_apply_blob(settingsBlob);
    free(settingsBlob);
    settingsBlob = NULL;
    hasSettingsBlob = 0;
    applySettings = 0;   // from here on, what the person changes on the TV stays
  }
}

// Empties the boxes a cycle filled, without applying any of it.
static void discardCycle(void) {
  nAddonsRemote = 0; hasAddonsRemote = 0;
  free(addonsBlob); addonsBlob = NULL;
  free(homeBlob); homeBlob = NULL; hasHomeBlob = 0;
  free(collectionsBlob); collectionsBlob = NULL; hasCollectionsBlob = 0;
  free(settingsBlob); settingsBlob = NULL; hasSettingsBlob = 0;
  traktToken[0] = 0; hasTraktRemote = 0;
  nProgressRemote = 0; progressComplete = 0;
  nPushedMark = 0;
}

void sync_resync(void) {
  if (threadAlive) againWanted = 1;
  else begin(0, "addon source changed");
}

int sync_profile_settled(void) {
  return !threadAlive && cycleProfile == profiles_active();
}

void sync_profile_switched(void) {
  switchRebuild = 1;
  // THE HOME IS BUILT ONCE, FROM THE NEXT PROFILE'S OWN STATE — as a launch on
  // that profile builds it. The rebuild used to go out with the previous
  // profile's addons, row order and collections still in memory, and the cycle
  // then delivered each of the new one's with a rebuild apiece: three builds in
  // a second and a half, the first showing the wrong home. What was last applied
  // for this profile comes off disk here, so the cycle finds it unchanged and
  // rebuilds only for what is really new.
  //
  // Reset first: a profile whose account holds no row order or collections has
  // an empty list, which applies nothing — the previous profile's stayed.
  disc_prefs_begin();
  disc_prefs_end();
  col_reset();
  stateForget();
  sync_restore();
  mark("rebuild: profile switched");
  disc_rebuild();
  // Refused while a cycle is running; sync_step starts it when that one ends.
  begin(0, "profile switched");
}

void sync_step(unsigned nowMs) {
  if (!threadAlive) return;
  if (cycleProfile != profiles_active()) {
    if (!threadReady) return;
    threadAlive = 0;
    threadReady = 0;
    discardCycle();
    data_log("sync.log", "cycle dropped: profile %d left for %d", cycleProfile, profiles_active());
    begin(0, "profile switched");
    return;
  }
  if (!earlyApplied && __atomic_load_n(&earlyReady, __ATOMIC_ACQUIRE)) {
    earlyApplied = 1;
    applyEarly();
  }
  if (!threadReady) return;
  threadAlive = 0;
  threadReady = 0;
  // A light cycle never reaches earlyReady; a full one has applied it above.
  if (!earlyApplied) applyEarly();

  if (hasTraktRemote) {
    // Only on the TRANSITION to active: the pull repeats on every cycle, and
    // rebuilding the whole home each time would throw the rows away every few
    // minutes. Going from "no credential" to "credential" is the one moment
    // the "continue watching" row can exist and does not.
    int wasOn = trakt_active();
    if (trakt_set(traktToken, cloud_trakt_client()) && !wasOn) { mark("rebuild: trakt on"); disc_rebuild(); }
    hasTraktRemote = 0;
  }
  char *progressBefore = NULL;
  if (switchRebuild && !light) {
    char name[64];
    progressBefore = data_read(cat_progress_file(profiles_active(), name, sizeof name));
  }
  if (nProgressRemote) {
    static CatProgress mine[CAT_PROGRESS_MAX];
    int nMine = cat_progress_read(mine, CAT_PROGRESS_MAX);
    int i, applied = 0, kept = 0;
    for (i = 0; i < nProgressRemote; i++) {
      int idx = cat_index_by_imdb(progressRemote[i].imdb), j;
      long long local = 0;
      int localOrigin = 0, newer = 0;
      // ONE ROW PER SERIES, THE NEWEST. The account keeps a row per EPISODE,
      // but progress.txt keeps one line per work, and every write below replaces
      // that line. Applying them all in turn let whichever row came LAST win —
      // and the RPC answers newest first, so that was the oldest episode.
      // Measured on Trigun: E8 at 2 min, then E7, E6, E5, E3, and finally an
      // 8-of-8-second S1E1 from days earlier, which is what landed on disk. Its
      // duration is under the row's 60 s floor, so the series dropped out of
      // "Continue watching"; when it did show, it resumed S1E1.
      for (j = 0; j < nProgressRemote; j++)
        if (j != i && sameWorkId(progressRemote[j].imdb, progressRemote[i].imdb) &&
            (progressRemote[j].ms > progressRemote[i].ms ||
             (progressRemote[j].ms == progressRemote[i].ms && j < i))) {
          newer = 1;
          break;
        }
      if (newer) continue;
      // THE REMOTE ROW ONLY LOSES TO PLAYBACK THAT HAPPENED HERE, AND ONLY WHEN
      // THAT IS NEWER.
      //
      // The pull repeats every few minutes and used to overwrite the local line
      // unconditionally. That was harmless only while every line claimed the
      // same instant anyway; now that the column means something, replaying an
      // hour-old row from the account over an episode watched on this TV five
      // minutes ago would push the series back down the row and resume it at
      // the older position.
      //
      // `origin` is what keeps the rule honest. Between the account's record and
      // a COPY of that record this device wrote down earlier, the account is
      // simply right — and the copy's instant is the moment of the sync, not of
      // the watching, so letting it win would freeze the very corruption this
      // change exists to clear. Only origin 1, playback here, may refuse.
      for (j = 0; j < nMine; j++)
        if (sameWorkId(mine[j].imdb, progressRemote[i].imdb)) {
          local = mine[j].lastWatchedMs;
          localOrigin = mine[j].origin;
          break;
        }
      if (localOrigin == 1 && local > 0 &&
          progressRemote[i].ms > 0 && progressRemote[i].ms <= local) {
        kept++;
        continue;
      }
      // This project's catalogue knows how to store progress PER EPISODE. Using
      // the version without season/episode would lose which episode the person
      // stopped on, which is the information that makes the "continue watching"
      // row worth anything on a series.
      //
      // A title the catalogue has not loaded still gets its line: "Continue
      // watching" is built from the file, not from the catalogue. Skipping it
      // is what kept Trigun's stale line frozen — that line hid the series from
      // the row, so it was never loaded, so the sync never replaced the line.
      if (idx >= 0)
        cat_save_progress_at(idx, progressRemote[i].pos, progressRemote[i].duration,
                                progressRemote[i].temp, progressRemote[i].ep,
                                progressRemote[i].ms, 2);
      else if (!cat_save_progress_id(progressRemote[i].imdb, progressRemote[i].pos,
                                     progressRemote[i].duration, progressRemote[i].temp,
                                     progressRemote[i].ep, progressRemote[i].ms, 2))
        continue;
      applied++;
    }
    printf("[sync] %d of %d progress entries applied"
           " (%d older than what is here)\n", applied, nProgressRemote, kept);

    // DELETED ON ANOTHER DEVICE: a copy of the account (origin 2) whose work
    // the account no longer has at all goes too. The pull only ever ADDED, so a
    // title removed from "Continue watching" on the phone lived on here for good.
    // Playback here (origin 1) is never dropped this way — it may simply not be
    // pushed yet. Only a whole pull may judge; `mine` was read before the rows
    // above were applied, so nothing written this pass is in it.
    if (progressComplete) {
      int dropped = 0;
      for (i = 0; i < nMine; i++) {
        int j, present = 0;
        if (mine[i].origin != 2) continue;
        for (j = 0; j < nProgressRemote && !present; j++)
          present = sameWorkId(mine[i].imdb, progressRemote[j].imdb);
        if (present) continue;
        cat_progress_remove(mine[i].imdb);
        dropped++;
      }
      if (dropped) printf("[sync] %d progress line(s) gone from the account: dropped\n", dropped);
    }
    nProgressRemote = 0;
  }
  progressComplete = 0;
  if (switchRebuild && !light) {
    char name[64];
    char *after = data_read(cat_progress_file(profiles_active(), name, sizeof name));
    if (strcmp(progressBefore ? progressBefore : "", after ? after : "")) {
      mark("rebuild: switched profile's progress");
      disc_rebuild();
    }
    free(after);
    switchRebuild = 0;
  }
  free(progressBefore);
  // The lines the push just put on the account are the account's now (origin 1
  // -> 2), so a delete made elsewhere later is followed here and never undone
  // by a second push. AFTER the apply above: the pull ran before the push, and
  // until this point origin 1 is what kept that older pull from overwriting them.
  if (nPushedMark) {
    int i;
    for (i = 0; i < nPushedMark; i++)
      cat_progress_mark_synced(pushedMark[i].imdb, pushedMark[i].ms);
    nPushedMark = 0;
  }
  // The account library: a title saved or removed on another device changes
  // which cards the catalogue has to carry.
  if (acclib_step()) { mark("rebuild: account library"); disc_rebuild(); }
  if (state == SYNC_READY) {
    lastOk = nowMs;
    data_log("sync.log", "cycle done: %s", light ? "library and watched" : summary);
  }
  if (againWanted) { againWanted = 0; begin(0, "addon source changed"); }
}

SyncState  sync_state(void)      { return state; }
const char *sync_summary(void)      { return summary; }
unsigned    sync_last_ok(void)   { return lastOk; }
void        sync_dirty_progress(void) { dirtyProgress = 1; }
void        sync_dirty_addons(void)    { dirtyAddons = 1; }
// Trakt's own token lifetime is 90 days; the fallback is the web app's, for a
// server answer that carries no expiry at all.
#define SY_TRACKER_LIFETIME_MAX  7776000L
#define SY_TRACKER_LIFETIME_DFLT   86400L

void sync_push_tracker(const char *tracker, const char *access, const char *refresh,
                       long lifetimeSeconds, const char *trackerUserId,
                       const char *username) {
  Jsw w;
  char *r;
  int st = 0;
  if (!session_loggedin() || !tracker || !*tracker || !access || !*access) return;
  if (lifetimeSeconds <= 0) lifetimeSeconds = SY_TRACKER_LIFETIME_DFLT;
  if (lifetimeSeconds > SY_TRACKER_LIFETIME_MAX) lifetimeSeconds = SY_TRACKER_LIFETIME_MAX;
  jsw_start(&w);
  jsw_obj_start(&w);
  jsw_ci(&w, "p_profile_id", profiles_active());
  jsw_cs(&w, "p_tracker", tracker);
  jsw_cs(&w, "p_access_token", access);
  jsw_cs(&w, "p_refresh_token", refresh ? refresh : "");
  // A LIFETIME, not an instant. Sending the original 90 days on every push would
  // walk the expiry 90 days further into the future each time and hide a dead
  // token from whoever tries to refresh it.
  jsw_ci(&w, "p_expires_in_seconds", lifetimeSeconds);
  // EVERY argument is required by the signature, so what this device does not
  // know goes as an empty string rather than being left out — the TV links
  // before it has ever asked Trakt who the user is.
  jsw_cs(&w, "p_tracker_user_id", trackerUserId ? trackerUserId : "");
  jsw_cs(&w, "p_username", username ? username : "");
  jsw_obj_end(&w);
  // NO p_origin_client_id: this is not a sync_push_* function and the signature
  // does not take one. The web app's client makes the same distinction.
  r = session_rpc("upsert_tracker_tokens", jsw_text_final(&w), &st);
  jsw_free(&w);
  if (!ok2xx(r, st)) {
    // THE SERVER'S OWN MESSAGE, not just the number. The old line printed
    // "(HTTP 400)" and stopped there, and 400 alone does not separate a retired
    // RPC from a rejected argument — which is the whole question when this
    // fails.
    printf("[sync] tracker push %s failed (HTTP %d)%s%.200s\n", tracker, st,
           r ? ": " : "", r ? r : "");
  } else {
    printf("[sync] tracker %s stored in the account\n", tracker);
  }
  free(r);
}

void sync_reapply_settings(void) { applySettings = 1; }

void sync_forget_user(void) {
  // The order matters little, but the SET does: every line here corresponds to
  // something that used to survive a sign-out.
  addons_forget();
  trakt_forget();
  profiles_forget();
  // Which episodes were watched belongs to the account that is leaving: kept,
  // it would tell the next person which episodes of their series are done.
  watchedep_forget();
  debrid_forget();
  // Which source someone picks, with which audio, is as personal as their list.
  sourcepref_forget();
  // Every profile's file: profiles_forget above has already reset the active one.
  { char name[64]; int p;
    for (p = 1; p <= ACCOUNT_PROFILE_MAX; p++)
      data_erase(cat_progress_file(p, name, sizeof name)); }

  // The boxes the thread fills too: a cycle that finished just before the
  // sign-out would apply the previous account's addons on the next sync_step.
  memset(addonsRemote, 0, sizeof addonsRemote);
  nAddonsRemote = 0; hasAddonsRemote = 0;
  traktToken[0] = 0; hasTraktRemote = 0;
  memset(tmdbKey, 0, sizeof tmdbKey); hasTmdb = 0;
  disc_tmdb_forget();
  memset(mdbKey, 0, sizeof mdbKey);   hasMdb = 0;
  nProgressRemote = 0;
  cWatched = cLib = cCollections = 0;
  hasSettingsProfile = hasCatHome = 0;
  state = SYNC_STOPPED;
  lastOk = 0;
  dirtyProgress = 0; dirtyAddons = 0;
  free(settingsBlob);
  settingsBlob = NULL;
  hasSettingsBlob = 0;
  free(addonsBlob); addonsBlob = NULL;
  free(homeBlob);   homeBlob = NULL; hasHomeBlob = 0;
  // The saved account state goes too, for every profile: the addon rows carry
  // debrid keys inside their URLs.
  stateForget();
  acclib_forget();
  soon = 0;
  { static const char *const WHAT[] = { "addons", "home-catalog", "collections" };
    char name[64];
    int p, w;
    for (p = 0; p <= 16; p++)
      for (w = 0; w < 3; w++) {
        snprintf(name, sizeof name, "account-p%d-%s.json", p, WHAT[w]);
        data_erase(name);
      } }
  applySettings = 1;
  snprintf(summary, sizeof summary, "no account");
  printf("[sync] user data erased from this device\n");
}

void        sync_shutdown(void)    { }
