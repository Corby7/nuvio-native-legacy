#include "homerows.h"
#include "discover.h"
#include "collections.h"
#include "catalog.h"
#include "session.h"
#include "settings.h"
#include "profiles.h"
#include "sync.h"
#include "data.h"
#include "js.h"
#include "jsw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define HR_MAX 512

enum { HR_CATALOG, HR_COLLECTION, HR_TRAKT };

typedef struct {
  char key[192];
  char title[96];
  char custom[96];      // the account's custom_title, kept as it came
  char source[64];      // the addon's name, for the side panel
  char addonId[96], type[16], catId[96], collectionId[96];
  int  kind, enabled;
  // 0 for an account entry this session cannot resolve: an addon whose manifest
  // did not load, a collection that was deleted. It is not listed, but it keeps
  // its place in what is sent back — dropping it would cost that row its
  // position for good over one failed request, the trap the web's
  // withPreservedRemoteItems exists for.
  int  present;
} HomeRow;

static HomeRow rows[HR_MAX];
static int nRows, dirty;
// The listed rows, as indices into rows[]: moving swaps with the nearest LISTED
// neighbour, so an unresolved entry never makes a press look like it did nothing.
static int listed[HR_MAX], nListed;

static const struct { const char *key, *title; } TRAKT_ROWS[] = {
  { "trakt_watchlist",       "Watchlist" },
  { "trakt_recommendations", "Recommended for You" },
};
#define N_TRAKT (int)(sizeof TRAKT_ROWS / sizeof *TRAKT_ROWS)

// --- the Trakt rows' local state ----------------------------------------------
//
// One line per row, "<key> <position> <shown>". The position is the index in the
// full order — the account's entries plus these — so re-inserting at it rebuilds
// exactly the order that was saved. Read by the discovery thread too, hence the
// lock; the file is tiny and read once per profile.
static pthread_mutex_t lockTrakt = PTHREAD_MUTEX_INITIALIZER;
static struct { int pos, on; } trakt[N_TRAKT];
static int traktProfile = -1;

static void traktName(char *dst, size_t n) {
  snprintf(dst, n, "home-rows-p%d.txt", profiles_active());
}

static void traktLoadLocked(void) {
  char name[64], *body, *line, *save = NULL;
  int i;
  if (traktProfile == profiles_active()) return;
  traktProfile = profiles_active();
  // Opt-in: both start hidden, at the top, so switching one on puts it where it
  // can be seen.
  for (i = 0; i < N_TRAKT; i++) { trakt[i].pos = 0; trakt[i].on = 0; }
  traktName(name, sizeof name);
  body = data_read(name);
  if (!body) return;
  for (line = strtok_r(body, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
    char key[64]; int pos, on;
    if (sscanf(line, "%63s %d %d", key, &pos, &on) != 3) continue;
    for (i = 0; i < N_TRAKT; i++)
      if (!strcmp(key, TRAKT_ROWS[i].key)) {
        trakt[i].pos = pos < 0 ? 0 : pos;
        trakt[i].on = on != 0;
      }
  }
  free(body);
}

static int traktOn(int i) {
  int v;
  pthread_mutex_lock(&lockTrakt);
  traktLoadLocked();
  v = trakt[i].on;
  pthread_mutex_unlock(&lockTrakt);
  return v;
}

int homerows_trakt_watchlist(void) { return traktOn(0); }
int homerows_trakt_recs(void)      { return traktOn(1); }

void homerows_merge_trakt(void) {
  int order[N_TRAKT], i, j;
  pthread_mutex_lock(&lockTrakt);
  traktLoadLocked();
  // Ascending position, so each insert lands where the saved order had it.
  for (i = 0; i < N_TRAKT; i++) order[i] = i;
  for (i = 0; i < N_TRAKT; i++)
    for (j = i + 1; j < N_TRAKT; j++)
      if (trakt[order[j]].pos < trakt[order[i]].pos) { int t = order[i]; order[i] = order[j]; order[j] = t; }
  for (i = 0; i < N_TRAKT; i++)
    disc_prefs_insert(trakt[order[i]].pos, TRAKT_ROWS[order[i]].key, trakt[order[i]].on);
  pthread_mutex_unlock(&lockTrakt);
}

// --- building the list ----------------------------------------------------------

static int find(const char *key) {
  int i;
  for (i = 0; i < nRows; i++) if (!strcmp(rows[i].key, key)) return i;
  return -1;
}

static HomeRow *append(const char *key, int kind) {
  HomeRow *r;
  if (nRows >= HR_MAX) return NULL;
  r = &rows[nRows++];
  memset(r, 0, sizeof *r);
  snprintf(r->key, sizeof r->key, "%s", key);
  r->kind = kind;
  r->enabled = 1;
  return r;
}

static void insertAt(int at, const HomeRow *row) {
  if (nRows >= HR_MAX) return;
  if (at < 0) at = 0;
  if (at > nRows) at = nRows;
  memmove(rows + at + 1, rows + at, sizeof(HomeRow) * (size_t)(nRows - at));
  rows[at] = *row;
  nRows++;
}

static void relist(void) {
  int i;
  nListed = 0;
  for (i = 0; i < nRows; i++) if (rows[i].present) listed[nListed++] = i;
}

// The account's entries, in its order. The same parse as sync.c's
// applyHomeCatalog, keeping every field so the push can send them back intact.
static void readAccount(void) {
  const char *body = sync_home_blob(), *root, *item;
  if (!body) return;
  root = js_root_array(body);
  if (!root) return;
  item = js_array(root, js_end(root), "items");
  for (; item; item = js_next(js_end(item))) {
    const char *f = js_end(item);
    char key[192];
    HomeRow tmp;
    memset(&tmp, 0, sizeof tmp);
    js_text(item, f, "custom_title", tmp.custom, sizeof tmp.custom);
    tmp.enabled = js_flag(item, f, "enabled", 1);
    if (js_flag(item, f, "is_collection", 0)) {
      js_text(item, f, "collection_id", tmp.collectionId, sizeof tmp.collectionId);
      if (!tmp.collectionId[0]) continue;
      snprintf(key, sizeof key, "collection_%s", tmp.collectionId);
      tmp.kind = HR_COLLECTION;
    } else {
      js_text(item, f, "addon_id",   tmp.addonId, sizeof tmp.addonId);
      js_text(item, f, "type",       tmp.type,    sizeof tmp.type);
      js_text(item, f, "catalog_id", tmp.catId,   sizeof tmp.catId);
      if (!tmp.addonId[0] || !tmp.type[0] || !tmp.catId[0]) continue;
      snprintf(key, sizeof key, "%s_%s_%s", tmp.addonId, tmp.type, tmp.catId);
      tmp.kind = HR_CATALOG;
    }
    if (find(key) >= 0) continue;
    snprintf(tmp.key, sizeof tmp.key, "%s", key);
    if (nRows < HR_MAX) rows[nRows++] = tmp;
  }
}

void homerows_open(void) {
  static DiscDecl decls[256];
  int nd, i;
  nRows = 0; dirty = 0;
  readAccount();

  // The catalogues the addons declare. One the account does not order yet goes
  // at the END, shown — where the web and discover both put a new key.
  nd = disc_decls_copy(decls, (int)(sizeof decls / sizeof *decls));
  for (i = 0; i < nd; i++) {
    int k = find(decls[i].key);
    HomeRow *r = k >= 0 ? &rows[k] : append(decls[i].key, HR_CATALOG);
    size_t lk, lsuffix;
    if (!r) break;
    r->present = 1;
    snprintf(r->title, sizeof r->title, "%s", r->custom[0] ? r->custom : decls[i].title);
    snprintf(r->source, sizeof r->source, "%s", decls[i].nameAddon);
    if (k < 0) {
      // The key is <addonId>_<type>_<catalogId> and an addon id may itself hold
      // underscores, so the addon id is what is left after the known tail.
      snprintf(r->type, sizeof r->type, "%s", decls[i].kind);
      snprintf(r->catId, sizeof r->catId, "%s", decls[i].id);
      lk = strlen(decls[i].key);
      lsuffix = strlen(decls[i].kind) + strlen(decls[i].id) + 2;
      if (lk > lsuffix)
        snprintf(r->addonId, sizeof r->addonId, "%.*s", (int)(lk - lsuffix), decls[i].key);
      else
        r->present = 0;   // a key this cannot split is not one the account could store
    }
  }

  // The account's collections, one row per collection (a collection is a group
  // of folders sharing its id).
  for (i = 0; i < col_n(); i++) {
    const ColFolder *folder = col_folder(i);
    char key[192];
    int k;
    HomeRow *r;
    if (!folder || !folder->groupId[0] || !folder->group[0]) continue;
    snprintf(key, sizeof key, "collection_%s", folder->groupId);
    k = find(key);
    r = k >= 0 ? &rows[k] : append(key, HR_COLLECTION);
    if (!r) break;
    if (r->present) continue;
    r->present = 1;
    snprintf(r->collectionId, sizeof r->collectionId, "%s", folder->groupId);
    snprintf(r->title, sizeof r->title, "%s", r->custom[0] ? r->custom : folder->group);
    snprintf(r->source, sizeof r->source, "%s",
             col_group_pinned(folder->group) ? "Pinned to the top" : "");
  }

  // The Trakt rows, at their saved positions.
  pthread_mutex_lock(&lockTrakt);
  traktLoadLocked();
  { int order[N_TRAKT], a, b;
    for (a = 0; a < N_TRAKT; a++) order[a] = a;
    for (a = 0; a < N_TRAKT; a++)
      for (b = a + 1; b < N_TRAKT; b++)
        if (trakt[order[b]].pos < trakt[order[a]].pos) { int t = order[a]; order[a] = order[b]; order[b] = t; }
    for (a = 0; a < N_TRAKT; a++) {
      HomeRow r;
      int t = order[a];
      memset(&r, 0, sizeof r);
      snprintf(r.key, sizeof r.key, "%s", TRAKT_ROWS[t].key);
      snprintf(r.title, sizeof r.title, "%s", TRAKT_ROWS[t].title);
      snprintf(r.source, sizeof r.source, "Trakt");
      r.kind = HR_TRAKT;
      r.enabled = trakt[t].on;
      r.present = 1;
      insertAt(trakt[t].pos, &r);
    } }
  pthread_mutex_unlock(&lockTrakt);

  relist();
  printf("[homerows] %d rows listed (%d in the order)\n", nListed, nRows);
  fflush(stdout);
}

int homerows_n(void) { return nListed; }

static HomeRow *at(int i) {
  return (i >= 0 && i < nListed) ? &rows[listed[i]] : NULL;
}

const char *homerows_title(int i) { HomeRow *r = at(i); return r ? r->title : ""; }
const char *homerows_source(int i) { HomeRow *r = at(i); return r ? r->source : ""; }
const char *homerows_kind(int i) {
  HomeRow *r = at(i);
  if (!r) return "";
  return r->kind == HR_COLLECTION ? "Collection" : r->kind == HR_TRAKT ? "Trakt" : "Catalogue";
}
int homerows_enabled(int i) { HomeRow *r = at(i); return r ? r->enabled : 0; }

// The cap is discover's: CAT_FILTER_MAX rows of catalogue, "Continue watching"
// included. Collections are placed by the Home and do not
// count. An estimate — a catalogue that comes back empty frees its slot — but
// it is the difference between "hidden" and "shown, and still not there".
int homerows_over_cap(int i) {
  int slots = CAT_FILTER_MAX - (settings_cw_on() ? 1 : 0);
  int k, used = 0;
  HomeRow *r = at(i);
  if (!r || !r->enabled || r->kind == HR_COLLECTION) return 0;
  // The Trakt rows have slots set aside for them, so they always fit.
  if (r->kind == HR_TRAKT) return 0;
  for (k = 0; k < nRows; k++) if (rows[k].kind == HR_TRAKT && rows[k].enabled) slots--;
  for (k = 0; k < nRows; k++) {
    if (!rows[k].present || !rows[k].enabled || rows[k].kind != HR_CATALOG) continue;
    used++;
    if (&rows[k] == r) return used > slots;
  }
  return 0;
}

void homerows_toggle(int i) {
  HomeRow *r = at(i);
  if (!r) return;
  r->enabled = !r->enabled;
  dirty = 1;
}

int homerows_move(int i, int dir) {
  int j = i + (dir < 0 ? -1 : 1);
  HomeRow tmp;
  if (!at(i) || !at(j)) return i;
  tmp = rows[listed[i]];
  rows[listed[i]] = rows[listed[j]];
  rows[listed[j]] = tmp;
  dirty = 1;
  return j;
}

// --- committing -----------------------------------------------------------------

static volatile unsigned generation;
static volatile int pushing;

unsigned homerows_generation(void) { return generation; }
int homerows_push_in_flight(void)  { return pushing; }

// The account's items, in the list's order — every entry but the Trakt rows,
// unresolved ones included. The web's buildLocalPayload shape, field for field.
static void writeItems(Jsw *w) {
  int i, order = 0;
  jsw_arr_start(w);
  for (i = 0; i < nRows; i++) {
    const HomeRow *r = &rows[i];
    if (r->kind == HR_TRAKT) continue;
    jsw_obj_start(w);
    jsw_cs(w, "addon_id",   r->kind == HR_COLLECTION ? "" : r->addonId);
    jsw_cs(w, "type",       r->kind == HR_COLLECTION ? "" : r->type);
    jsw_cs(w, "catalog_id", r->kind == HR_COLLECTION ? "" : r->catId);
    jsw_cb(w, "enabled", r->enabled);
    jsw_ci(w, "order", order++);
    jsw_cs(w, "custom_title", r->custom);
    jsw_cb(w, "is_collection", r->kind == HR_COLLECTION);
    jsw_cs(w, "collection_id", r->kind == HR_COLLECTION ? r->collectionId : "");
    jsw_obj_end(w);
  }
  jsw_arr_end(w);
}

static void writeSettings(Jsw *w) {
  jsw_obj_start(w);
  jsw_cb(w, "hide_unreleased_content", settings_hide_unreleased());
  jsw_key(w, "items");
  writeItems(w);
  jsw_obj_end(w);
}

static void *threadPush(void *u) {
  char *body = (char *)u, *r;
  int st = 0;
  r = session_rpc("sync_push_home_catalog_settings", body, &st);
  if (r && st >= 200 && st < 300)
    printf("[homerows] row order sent to the account\n");
  else
    // The Home already shows the edit. The next pull will bring the account's
    // old order back, and saying so here is what makes that explainable.
    printf("[homerows] row order push FAILED (HTTP %d); the account keeps the old order\n", st);
  fflush(stdout);
  free(r);
  free(body);
  pushing = 0;
  return NULL;
}

void homerows_commit(void) {
  int i;
  if (!dirty) return;
  dirty = 0;
  generation++;

  // 1. The Trakt rows' positions and visibility, on this device.
  { char name[64], text[256];
    size_t w = 0;
    for (i = 0; i < nRows && w < sizeof text; i++) {
      if (rows[i].kind != HR_TRAKT) continue;
      w += (size_t)snprintf(text + w, sizeof text - w, "%s %d %d\n",
                            rows[i].key, i, rows[i].enabled);
    }
    pthread_mutex_lock(&lockTrakt);
    traktName(name, sizeof name);
    data_write(name, text);
    traktProfile = -1;   // re-read on the next question
    pthread_mutex_unlock(&lockTrakt); }

  // 2. Discover's preferences, at once, in the full order.
  disc_prefs_begin();
  for (i = 0; i < nRows; i++)
    disc_prefs_add(rows[i].key, rows[i].enabled, rows[i].custom[0] ? rows[i].custom : NULL);
  disc_prefs_end();

  // 3. The account. Stored as the last applied body too, in the shape a pull
  //    returns, so a restart before the next sync starts from the edit.
  if (session_loggedin()) {
    Jsw w;
    char *body;
    jsw_start(&w);
    jsw_arr_start(&w);
    jsw_obj_start(&w);
    jsw_key(&w, "settings_json");
    writeSettings(&w);
    jsw_obj_end(&w);
    jsw_arr_end(&w);
    if (!w.error) sync_home_store(jsw_text_final(&w));
    jsw_free(&w);

    jsw_start(&w);
    jsw_obj_start(&w);
    jsw_ci(&w, "p_profile_id", profiles_active());
    jsw_cs(&w, "p_platform", "home_catalog_shared");
    jsw_key(&w, "p_settings_json");
    writeSettings(&w);
    jsw_obj_end(&w);
    body = w.error ? NULL : strdup(jsw_text_final(&w));
    jsw_free(&w);
    if (body) {
      pthread_t th;
      pushing = 1;
      if (pthread_create(&th, NULL, threadPush, body) == 0) pthread_detach(th);
      else threadPush(body);
    }
  }

  printf("[homerows] order applied: %d entries\n", nRows);
  fflush(stdout);
  disc_rebuild();
}
