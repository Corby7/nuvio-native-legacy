#include "profiles.h"
#include "catalog.h"
#include "traktauth.h"
#include "simklauth.h"
#include "settings.h"
#include "session.h"
#include "cloud.h"
#include "data.h"
#include "js.h"
#include "jsw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FILE_ACTIVE "profile.txt"

static AccountProfile list[ACCOUNT_PROFILE_MAX];
static int n;
static char owner[64];
static int active = 1;
static int chosen;      // 1 once the user has decided on this installation

static void readOwner(void) {
  char *r;
  int st = 0;
  if (owner[0]) return;
  r = session_rpc("get_sync_owner", "{}", &st);
  if (r && st >= 200 && st < 300) {
    // MEASURED: the response is a RAW JSON string — "441bf572-…" — and not an
    // object. js_text is no use here; the value sits between the quotes of the
    // whole body.
    const char *a = strchr(r, '"');
    const char *b = a ? strchr(a + 1, '"') : NULL;
    if (a && b && b > a + 1 && (size_t)(b - a - 1) < sizeof owner) {
      memcpy(owner, a + 1, (size_t)(b - a - 1));
      owner[b - a - 1] = 0;
    }
  }
  free(r);
  if (!owner[0]) {
    // Without the owner, reading the addons table has nobody to filter by.
    // Falling back to the token's `sub` is the right approximation: on an
    // account that is not shared the two are the same thing.
    snprintf(owner, sizeof owner, "%s", session_user());
    printf("[profiles] get_sync_owner did not answer; using the token sub\n");
  }
}

// NUVIO'S OWN AVATARS. Static content — 42 rows today — so it is read ONCE per
// run and only when a profile actually names one. An account where everybody
// pasted their own address never pays for this request.
//
// The address is assembled exactly as avatarRepository.js assembles it: the web
// app's AVATAR_PUBLIC_BASE_URL is configured to `<supabase>/storage/v1/object/
// public/avatars`, which is also the fallback it computes when that property is
// empty. Both spellings agree, so the base is derived from cloud_url() and no
// new build flag is needed.
#define AVATAR_BUCKET_PATH "/storage/v1/object/public/avatars/"
#define AVATAR_CATALOG_MAX 64
typedef struct { char id[40]; char path[80]; } AvatarArt;
static AvatarArt catalog[AVATAR_CATALOG_MAX];
static int catalogN;
static int catalogTried;

static void readCatalog(void) {
  char *r;
  int st = 0;
  const char *p;
  // Tried, not loaded: a server that answers 500 must not be asked again on
  // every sync cycle for a picture.
  if (catalogTried) return;
  catalogTried = 1;
  r = session_rpc("get_avatar_catalog", "{}", &st);
  if (!r || st < 200 || st >= 300) {
    printf("[profiles] avatar catalogue unavailable (HTTP %d)\n", st);
    free(r);
    return;
  }
  for (p = js_root_array(r); p && catalogN < AVATAR_CATALOG_MAX; p = js_next(js_end(p))) {
    const char *f = js_end(p);
    AvatarArt a;
    memset(&a, 0, sizeof a);
    if (!js_text(p, f, "id", a.id, sizeof a.id)) continue;
    if (!js_text(p, f, "storage_path", a.path, sizeof a.path)) continue;
    catalog[catalogN++] = a;
  }
  free(r);
  printf("[profiles] avatar catalogue: %d entries\n", catalogN);
}

int profiles_avatar(const AccountProfile *p, char *out, unsigned long n) {
  int i;
  if (!p || !out || n < 2) return 0;
  out[0] = 0;
  // The web app's order: whatever the person pasted wins over the catalogue.
  if (p->avatarUrl[0]) { snprintf(out, n, "%s", p->avatarUrl); return 1; }
  if (!p->avatarId[0]) return 0;
  for (i = 0; i < catalogN; i++)
    if (strcmp(catalog[i].id, p->avatarId) == 0) {
      snprintf(out, n, "%s" AVATAR_BUCKET_PATH "%s", cloud_url(), catalog[i].path);
      return 1;
    }
  return 0;
}

int profiles_pull(void) {
  char *r;
  int st = 0;
  const char *p;

  readOwner();

  r = session_rpc("sync_pull_profiles", "{}", &st);
  if (!r || st < 200 || st >= 300) { free(r); return n; }

  // An empty list does NOT erase what is already in memory: it is the same rule
  // the web app applies on every surface. An empty response can be the wrong
  // profile, a mishandled 401 or a server that is down, and none of those is
  // "the user deleted their profiles".
  { int new = 0;
    AccountProfile tmp[ACCOUNT_PROFILE_MAX];
    memset(tmp, 0, sizeof tmp);
    for (p = js_root_array(r); p && new < ACCOUNT_PROFILE_MAX; p = js_next(js_end(p))) {
      const char *f = js_end(p);
      double idx = js_num(p, f, "profile_index", 0);
      if (idx <= 0) idx = js_num(p, f, "id", 0);
      if (idx <= 0) continue;
      tmp[new].index_ = (int)idx;
      if (!js_text(p, f, "name", tmp[new].name, sizeof tmp[new].name))
        snprintf(tmp[new].name, sizeof tmp[new].name, "Profile %d", (int)idx);
      js_text(p, f, "avatar_url", tmp[new].avatarUrl, sizeof tmp[new].avatarUrl);
      js_text(p, f, "avatar_id", tmp[new].avatarId, sizeof tmp[new].avatarId);
      if (!js_text(p, f, "avatar_color_hex", tmp[new].colorHex, sizeof tmp[new].colorHex))
        snprintf(tmp[new].colorHex, sizeof tmp[new].colorHex, "#1E88E5");
      { char b[16];
        // Without the field, profile 1 is the primary one — the same rule as the web app.
        tmp[new].primary = js_raw(p, f, "is_primary", b, sizeof b)
                              ? (strcmp(b, "true") == 0) : ((int)idx == 1); }
      new++;
    }
    if (new > 0) { memcpy(list, tmp, sizeof list); n = new; }
  }
  free(r);

  // Only if a profile actually names one of Nuvio's avatars. Fetching it for an
  // account that has none is a round trip whose answer nobody reads.
  { int i;
    for (i = 0; i < n; i++)
      if (list[i].avatarId[0] && !list[i].avatarUrl[0]) { readCatalog(); break; } }

  // Locks: a profile with a PIN cannot be opened merely by being in the list.
  r = session_rpc("sync_pull_profile_locks", "{}", &st);
  if (r && st >= 200 && st < 300) {
    for (p = js_root_array(r); p; p = js_next(js_end(p))) {
      const char *f = js_end(p);
      int idx = (int)js_num(p, f, "profile_id", 0);
      char b[16];
      int i, locked;
      if (!idx) idx = (int)js_num(p, f, "profile_index", 0);
      // MEASURED: this RPC returns ONE ROW PER PROFILE, with `pin_enabled`
      // false when there is no PIN — it is not a list of only the locked ones.
      // Marking every profile that appears here locked ALL of them, and since
      // none has a PIN no typing would be accepted: nobody would get into their
      // own account.
      locked = js_raw(p, f, "pin_enabled", b, sizeof b)
                ? (strcmp(b, "true") == 0) : 0;
      for (i = 0; i < n; i++)
        if (list[i].index_ == idx) list[i].hasPin = locked;
    }
  }
  free(r);

  printf("[profiles] %d profile(s), owner=%s, active=%d\n", n, owner, active);
  return n;
}

int           profiles_n(void)         { return n; }
const AccountProfile *profiles_item(int i)     { return (i >= 0 && i < n) ? &list[i] : NULL; }

const AccountProfile *profiles_item_active(void) {
  int i;
  for (i = 0; i < n; i++) if (list[i].index_ == active) return &list[i];
  return NULL;
}
const char   *profiles_owner(void)      { return owner; }
int           profiles_active(void)     { return active > 0 ? active : 1; }

int profiles_addon_profile(void) {
  const AccountProfile *p = profiles_item_active();
  return p && !p->primary && settings_use_primary_addons() ? 1 : profiles_active();
}

// Everything kept per profile on this TV follows the active one.
static void setProfile(int index_) {
  cat_set_profile(index_);
  traktauth_set_profile(index_);
  simklauth_set_profile(index_);
  settings_set_profile(index_);
}

void profiles_load_active(void) {
  char *b = data_read(FILE_ACTIVE);
  if (!b) return;
  { int v = atoi(b);
    if (v > 0) { active = v; chosen = 1; } }
  setProfile(active);
  free(b);
}

void profiles_set_active(int index_) {
  char line[32];
  if (index_ <= 0) return;
  active = index_;
  chosen = 1;
  setProfile(index_);
  snprintf(line, sizeof line, "%d\n", index_);
  data_write(FILE_ACTIVE, line);
  printf("[profiles] active profile: %d\n", index_);
}

int profiles_needs_choose(void) {
  return n > 1 && !chosen;
}

int profiles_verify_pin(int index_, const char *pin) {
  Jsw w;
  char *r;
  int st = 0, ok = 0;
  jsw_start(&w);
  jsw_obj_start(&w);
  jsw_ci(&w, "p_profile_id", index_);
  jsw_cs(&w, "p_pin", pin ? pin : "");
  jsw_obj_end(&w);
  r = session_rpc("verify_profile_pin", jsw_text_final(&w), &st);
  jsw_free(&w);
  // The RPC returns a boolean; accepting only the HTTP 200 would let a wrong
  // PIN through, since that answers 200 with `false`.
  if (r && st >= 200 && st < 300) ok = (strstr(r, "true") != NULL);
  free(r);
  return ok;
}

void profiles_forget(void) {
  catalogN = 0;
  catalogTried = 0;
  memset(list, 0, sizeof list);
  n = 0;
  owner[0] = 0;
  active = 1;
  chosen = 0;
  setProfile(1);
  data_erase(FILE_ACTIVE);
  printf("[profiles] profiles forgotten (signed out)\n");
}
