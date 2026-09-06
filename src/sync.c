#include "sync.h"
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
#include "js.h"
#include "jsw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define SY_ADD_MAX   16
#define SY_PROGRESS_MAX 240
#define FILE_PROGRESS "progress.txt"

static pthread_t thread;
static int threadAlive, threadReady;
static SyncState state = SYNC_STOPPED;
static char summary[220] = "not synced";
static unsigned lastOk;
static int dirtyProgress, dirtyAddons;

// The thread does NOT touch the app: it only fills these boxes, and sync_step
// applies them on the main loop. Without that separation, a network response
// would rewrite the addon list in the middle of a frame already reading from it.
static AddonRemote addonsRemote[SY_ADD_MAX];
static int nAddonsRemote, hasAddonsRemote;

static char traktToken[300];
static int  hasTraktRemote;

// Service keys the account stores and the app used to read from the owner's
// file. MEASURED on the real account: the providers present are animeskip,
// debrid:*, introdb, mdblist and tmdb — and there is NO "trakt". The trakt
// reader stays here because the RPC is the same and the row will appear as soon
// as the web app writes it.
static char tmdbKey[120], mdbKey[120];
static int  hasTmdb, hasMdb;

typedef struct { char imdb[40]; double pos, duration; int temp, ep; } ProgressItem;
static ProgressItem progressRemote[SY_PROGRESS_MAX];
static int nProgressRemote;

// Counts of what has been pulled but the app does not consume yet. They exist so
// the summary can tell the truth instead of saying "synced" without qualifying it.
static int cWatched, cLib, cSaved, cCollections, hasSettingsProfile, hasCatHome;

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
           owner, profiles_active());
  // With the anonymous key the RLS answers 401 "permission denied for table
  // addons": reading someone's rows requires the token of whoever is asking.
  r = session_table("addons", query, &st);
  if (!ok2xx(r, st)) {
    if (r && cloud_error_missing(r)) printf("[sync] addons table missing\n");
    else if (st) printf("[sync] addon read: HTTP %d\n", st);
    free(r);
    return;
  }
  for (p = js_root_array(r); p && k < SY_ADD_MAX; p = js_next(js_end(p))) {
    const char *f = js_end(p);
    char b[16];
    memset(&addonsRemote[k], 0, sizeof addonsRemote[k]);
    // THE SAME TRAP AS THE COLLECTIONS, and here it costs a whole addon: js_text
    // refuses silently when the value does not fit, and addons that keep their
    // configuration inside the URL run well past this field's 600 bytes. Without
    // this line the addon would simply not exist for the app, with not a word in
    // the log to say why.
    if (!js_text(p, f, "url", addonsRemote[k].url, sizeof addonsRemote[k].url)) {
      char name[64] = "";
      js_text(p, f, "name", name, sizeof name);
      printf("[sync] addon '%s' SKIPPED: its URL does not fit in %d bytes\n",
             name[0] ? name : "(unnamed)", (int)sizeof addonsRemote[k].url);
      continue;
    }
    js_text(p, f, "name", addonsRemote[k].name, sizeof addonsRemote[k].name);
    // Absent counts as ON: that is how the web app reads it, and an addon that
    // disappears because of a field the server did not send is worse than an
    // extra one.
    addonsRemote[k].active = js_raw(p, f, "enabled", b, sizeof b)
                         ? (strcmp(b, "false") != 0) : 1;
    k++;
  }
  free(r);
  nAddonsRemote = k;
  hasAddonsRemote = 1;
}

static void pushAddons(void) {
  AddonRemote current[SY_ADD_MAX];
  Jsw w;
  char *r;
  int st = 0, n, i;

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
    // debrid:* is deliberately NOT applied today: the debrid keys this app uses
    // already come embedded in the addon's URL (see addons.h), so applying the
    // loose key would change nothing and would give the false impression that
    // the app talks to the provider on its own.
  }
  free(r);
}

// ---------------------------------------------------------------- progresso

static void pullProgress(void) {
  Jsw w;
  char *r;
  int st = 0, k = 0;
  const char *p;

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
    if (temp > 0 && ep > 0)
      snprintf(progressRemote[k].imdb, sizeof progressRemote[k].imdb, "%s:%d:%d", id, temp, ep);
    else
      snprintf(progressRemote[k].imdb, sizeof progressRemote[k].imdb, "%s", id);
    progressRemote[k].pos = pos;
    progressRemote[k].duration = duration;
    progressRemote[k].temp = temp;
    progressRemote[k].ep = ep;
    k++;
  }
  free(r);
  // Empty deletes nothing: the consumer only applies what arrived.
  nProgressRemote = k;
}

// Reads the progress THIS device recorded. It is the only surface where the
// native app has real information of its own — which is why it is the only one,
// along with the addons, that it pushes.
static int readProgressLocal(ProgressItem *output, int max) {
  char *buf, *line, *ctx;
  int k = 0;
  buf = data_read(FILE_PROGRESS);
  if (!buf) return 0;
  for (line = strtok_r(buf, "\n", &ctx); line && k < max;
       line = strtok_r(NULL, "\n", &ctx)) {
    char id[40];
    double pos, duration;
    if (sscanf(line, "%39s %lf %lf", id, &pos, &duration) != 3) continue;
    if (duration <= 1.0) continue;
    snprintf(output[k].imdb, sizeof output[k].imdb, "%s", id);
    output[k].pos = pos;
    output[k].duration = duration;
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

  jsw_start(&w);
  jsw_obj_start(&w);
  jsw_ci(&w, "p_profile_id", profiles_active());
  jsw_cs(&w, "p_origin_client_id", data_client_id());
  jsw_key(&w, "p_entries");
  jsw_arr_start(&w);
  for (i = 0; i < n; i++) {
    // "tt123:4:9" carries the season and episode; the server wants the three
    // fields separately, and sending the composite id in content_id would make
    // every episode a different title on the account.
    char id[40];
    int temp = 0, ep = 0;
    char *dp;
    snprintf(id, sizeof id, "%s", local[i].imdb);
    dp = strchr(id, ':');
    if (dp) { sscanf(dp + 1, "%d:%d", &temp, &ep); *dp = 0; }

    jsw_obj_start(&w);
    jsw_cs(&w, "content_id", id);
    jsw_cs(&w, "content_type", (temp > 0) ? "series" : "movie");
    jsw_ci(&w, "position", (long long)(local[i].pos * 1000.0));
    jsw_ci(&w, "duration", (long long)(local[i].duration * 1000.0));
    if (temp > 0) { jsw_ci(&w, "season", temp); jsw_ci(&w, "episode", ep); }
    else          { jsw_key(&w, "season"); jsw_null(&w);
                    jsw_key(&w, "episode"); jsw_null(&w); }
    jsw_cs(&w, "progress_key", local[i].imdb);
    jsw_obj_end(&w);
  }
  jsw_arr_end(&w);
  jsw_obj_end(&w);
  r = session_rpc("sync_push_watch_progress", jsw_text_final(&w), &st);
  jsw_free(&w);
  if (!ok2xx(r, st)) printf("[sync] progress push failed (HTTP %d)\n", st);
  else dirtyProgress = 0;
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

static int countRpc(const char *func, const char *body) {
  char *r;
  int st = 0, k = 0;
  const char *p;
  if (alreadyMissing(func)) return -1;
  r = session_rpc(func, body, &st);
  if (!ok2xx(r, st)) {
    if (r && cloud_error_missing(r)) {
      printf("[sync] %s does not exist on this server\n", func);
      if (nMissing < SY_MISSING) missing[nMissing++] = func;
    }
    free(r);
    return -1;
  }
  for (p = js_root_array(r); p; p = js_next(js_end(p))) k++;
  free(r);
  return k;
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
static int pullHomeCatalog(const char *body) {
  static const char *FUNC = "sync_pull_home_catalog_settings";
  char *r;
  int st = 0, n = 0, nCollection = 0;
  const char *root, *item;
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
  root = js_root_array(r);
  if (!root) { free(r); return 0; }
  // The body is [{"settings_json":{... ,"items":[...]}}].
  item = js_array(root, js_end(root), "items");
  if (!item) {
    printf("[sync] home catalog settings: no \"items\"\n");
    free(r);
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
  disc_prefs_end();
  // The owner's collections take part in the SAME ordering in the web app, and
  // this app has nowhere to get their contents from yet. Counting them and
  // saying how many there are beats ignoring them silently: it is the
  // difference between "I have no collections" and "I have some and this app
  // does not show them yet".
  if (nCollection)
    printf("[sync] %d collection(s) placed in the home order\n", nCollection);
  free(r);
  return n;
}

static void pullSoRead(void) {
  char body[160];
  int profile = profiles_active();

  snprintf(body, sizeof body, "{\"p_profile_id\":%d}", profile);
  cLib   = countRpc("sync_pull_library", body);
  // COLLECTIONS: downloaded in full now, not merely counted. The body is kept
  // for the main thread to apply (see collectionsBlob).
  cCollections = pullCollections(body);

  // MEASURED: `p_page` starts at 1. With 0 the server answers 400 "OFFSET must
  // not be negative" — its arithmetic is (p_page - 1) * p_page_size.
  snprintf(body, sizeof body,
           "{\"p_profile_id\":%d,\"p_page\":1,\"p_page_size\":200}", profile);
  cWatched = countRpc("sync_pull_watched_items", body);

  snprintf(body, sizeof body,
           "{\"p_profile_id\":%d,\"p_limit\":200,\"p_offset\":0}", profile);
  cSaved = countRpc("sync_pull_saved_library", body);

  snprintf(body, sizeof body,
           "{\"p_profile_id\":%d,\"p_platform\":\"tv\"}", profile);
  hasSettingsProfile = pullSettingsProfile(body);

  snprintf(body, sizeof body,
           "{\"p_profile_id\":%d,\"p_platform\":\"home_catalog_shared\"}", profile);
  hasCatHome = pullHomeCatalog(body) > 0;
  // The home was already assembled in the manifest's order. With the owner's
  // order in hand the rows are rebuilt — and now the ones that fit are the ones
  // they chose, not the first ones the addon happened to declare.
  if (hasCatHome) disc_rebuild();
}

// ---------------------------------------------------------------- cycle

static void *run(void *u) {
  (void)u;
  profiles_pull();
  pullAddons();
  pullCredentials();
  pullProgress();
  pullSoRead();
  // Push AFTER pulling, like the web app's startupSyncService: pulling after
  // pushing would make the device overwrite with what it sent itself.
  if (dirtyAddons)    pushAddons();
  if (dirtyProgress) pushProgress();

  snprintf(summary, sizeof summary,
           "%d addons · %d progress · %d watched · %d in list · %d collections%s",
           nAddonsRemote, nProgressRemote, cWatched < 0 ? 0 : cWatched,
           cLib < 0 ? 0 : cLib, cCollections < 0 ? 0 : cCollections,
           hasTraktRemote ? " · Trakt" : "");
  state = SYNC_READY;
  threadReady = 1;
  return NULL;
}

void sync_start(void) {
  if (threadAlive || !session_loggedin()) return;
  if (cloud_brake_active()) return;
  state = SYNC_RUNNING;
  threadReady = 0;
  if (pthread_create(&thread, NULL, run, NULL) == 0) { pthread_detach(thread); threadAlive = 1; }
  else { state = SYNC_FAILED; snprintf(summary, sizeof summary, "no thread to sync with"); }
}

// One automatic cycle, if the interval has passed. Returns 1 when it fired.
// Separate from sync_step because the caller knows whether the moment is right:
// during playback it is NOT — a burst of HTTP in the middle of the video
// competes for CPU and network with the decoder, and one stutter costs more
// than 5 minutes of delay on the progress.
int sync_periodic(unsigned nowMs) {
  if (!session_loggedin() || threadAlive) return 0;
  if (cloud_brake_active()) return 0;
  // With no successful cycle yet, whoever called sync_start is in charge — there
  // is no point insisting on top of a failure the brake is already holding.
  if (!lastOk) return 0;
  if (nowMs - lastOk < SYNC_INTERVAL_MS) return 0;
  sync_start();
  return 1;
}

void sync_step(unsigned nowMs) {
  if (!threadAlive || !threadReady) return;
  threadAlive = 0;
  threadReady = 0;

  if (hasAddonsRemote) {
    addons_set_list(addonsRemote, nAddonsRemote);
    hasAddonsRemote = 0;
    // The home was assembled BEFORE this list arrived, so with no addons at all:
    // zero manifests read, zero catalogues, and the home falling back to the
    // packaged catalogue. Now that there is a list, the rows are rebuilt.
    if (addons_took_change()) disc_rebuild();
  }
  if (hasCollectionsBlob && collectionsBlob) {
    // Main thread: this is the only place the collection list can be rewritten
    // without racing the drawing code.
    if (col_load_account(collectionsBlob) > 0) disc_rebuild();
    free(collectionsBlob);
    collectionsBlob = NULL;
    hasCollectionsBlob = 0;
  }
  if (hasTraktRemote) {
    // Only on the TRANSITION to active: the pull repeats on every cycle, and
    // rebuilding the whole home each time would throw the rows away every few
    // minutes. Going from "no credential" to "credential" is the one moment
    // the "continue watching" row can exist and does not.
    int wasOn = trakt_active();
    if (trakt_set(traktToken, cloud_trakt_client()) && !wasOn) disc_rebuild();
    hasTraktRemote = 0;
  }
  if (hasTmdb)      { disc_tmdb_set(tmdbKey);   hasTmdb = 0; }
  if (hasMdb)       { extras_set_key(mdbKey); hasMdb = 0; }
  if (hasSettingsBlob && settingsBlob) {
    settings_apply_blob(settingsBlob);
    free(settingsBlob);
    settingsBlob = NULL;
    hasSettingsBlob = 0;
    applySettings = 0;   // from here on, what the person changes on the TV stays
  }
  if (nProgressRemote) {
    int i, applied = 0;
    for (i = 0; i < nProgressRemote; i++) {
      int idx = cat_index_by_imdb(progressRemote[i].imdb);
      if (idx < 0) continue;
      // This project's catalogue knows how to store progress PER EPISODE. Using
      // the version without season/episode would lose which episode the person
      // stopped on, which is the information that makes the "continue watching"
      // row worth anything on a series.
      cat_save_progress_ep(idx, progressRemote[i].pos, progressRemote[i].duration,
                              progressRemote[i].temp, progressRemote[i].ep);
      applied++;
    }
    printf("[sync] %d of %d progress entries matched the catalog\n", applied, nProgressRemote);
    nProgressRemote = 0;
  }
  if (state == SYNC_READY) lastOk = nowMs;
}

SyncState  sync_state(void)      { return state; }
const char *sync_summary(void)      { return summary; }
unsigned    sync_last_ok(void)   { return lastOk; }
void        sync_dirty_progress(void) { dirtyProgress = 1; }
void        sync_dirty_addons(void)    { dirtyAddons = 1; }
void sync_push_credential(const char *provider, const char *credJson) {
  Jsw w;
  char *r;
  int st = 0;
  if (!session_loggedin() || !provider || !*provider || !credJson || !*credJson) return;
  jsw_start(&w);
  jsw_obj_start(&w);
  jsw_ci(&w, "p_profile_id", profiles_active());
  jsw_cs(&w, "p_origin_client_id", data_client_id());
  jsw_key(&w, "p_credentials");
  jsw_arr_start(&w);
  jsw_obj_start(&w);
  jsw_cs(&w, "provider", provider);
  jsw_key(&w, "credential_json");
  jsw_raw(&w, credJson);
  jsw_obj_end(&w);
  jsw_arr_end(&w);
  jsw_obj_end(&w);
  r = session_rpc("sync_push_provider_credentials", jsw_text_final(&w), &st);
  jsw_free(&w);
  if (!ok2xx(r, st)) printf("[sync] credential push %s failed (HTTP %d)\n", provider, st);
  else printf("[sync] credential %s stored in the account\n", provider);
  free(r);
}

void sync_reapply_settings(void) { applySettings = 1; }

void sync_forget_user(void) {
  // The order matters little, but the SET does: every line here corresponds to
  // something that used to survive a sign-out.
  addons_forget();
  trakt_forget();
  profiles_forget();
  data_erase(FILE_PROGRESS);

  // The boxes the thread fills too: a cycle that finished just before the
  // sign-out would apply the previous account's addons on the next sync_step.
  memset(addonsRemote, 0, sizeof addonsRemote);
  nAddonsRemote = 0; hasAddonsRemote = 0;
  traktToken[0] = 0; hasTraktRemote = 0;
  memset(tmdbKey, 0, sizeof tmdbKey); hasTmdb = 0;
  memset(mdbKey, 0, sizeof mdbKey);   hasMdb = 0;
  nProgressRemote = 0;
  cWatched = cLib = cSaved = cCollections = 0;
  hasSettingsProfile = hasCatHome = 0;
  state = SYNC_STOPPED;
  lastOk = 0;
  dirtyProgress = 0; dirtyAddons = 0;
  free(settingsBlob);
  settingsBlob = NULL;
  hasSettingsBlob = 0;
  applySettings = 1;
  snprintf(summary, sizeof summary, "no account");
  printf("[sync] user data erased from this device\n");
}

void        sync_shutdown(void)    { }
