// The account's profiles.
//
// Everything the sync asks for carries `p_profile_id`. Without choosing a
// profile the app would always sync profile 1 — and on a family account that
// means showing somebody else's list and, worse, WRITING their progress. That is
// why profiles come before any other surface.
//
// THE ACCOUNT OWNER is not necessarily whoever signed in: `get_sync_owner`
// returns the id of whoever actually owns the data (a shared account). MEASURED:
// the RPC exists and answers with a raw JSON string holding the uuid. Reading
// the addons table filters by THAT id, not by the token's `sub`.
#ifndef NV_PROFILES_H
#define NV_PROFILES_H

#define ACCOUNT_PROFILE_MAX 8

typedef struct {
  int  index_;          // profile_index (1..n) — this is what goes in p_profile_id
  char name[64];
  char colorHex[10];      // avatar_color_hex, "#1E88E5"
  // THE TWO WAYS A PROFILE CARRIES A PICTURE, and it took a wrong turn to find
  // the second. `avatar_url` is a plain address the person pasted in the web
  // app. `avatar_id` ("avatar_lalo") names one of Nuvio's own avatars, and the
  // note that used to sit here said those could not be fetched because the
  // `avatars` TABLE does not exist on this server (PGRST205) — which is true,
  // and beside the point: the web app never reads that table. It calls the RPC
  // `get_avatar_catalog` (avatarRepository.js), which answers 42 rows, and
  // builds the address out of `storage_path`. The picture was always there.
  //
  // Order of preference is the web app's resolveProfileAvatarUrl: the explicit
  // url first, the catalogue second, the initial only when there is neither.
  char avatarUrl[300];
  char avatarId[40];      // avatar_id; resolved through profiles_avatar()
  int  primary;        // is_primary
  int  hasPin;          // came from sync_pull_profile_locks
} AccountProfile;

// Fetches the profiles and the owner. BLOCKS — call from the sync thread.
// Returns how many it found. Zero is NOT an error: a new account may have no
// profile created, and in that case the app operates with an implicit
// profile 1.
int profiles_pull(void);

int           profiles_n(void);
const AccountProfile *profiles_item(int i);
// The profile in force, or NULL when the list has not arrived yet.
const AccountProfile *profiles_item_active(void);
const char   *profiles_owner(void);        // uuid from get_sync_owner; "" if it did not arrive

// The active AccountProfile. Persisted to disk: choosing again on every start
// would be a question the app already knows the answer to.
int  profiles_active(void);                // profile_index; 1 when nothing has been chosen
// Whose rows in `addons` the active profile uses: 1 when it runs the primary's
// addons, its own index otherwise — the rule of the web app and the Android TV
// app (AddonSyncService.getRemoteAddonUrls), switched by the TV's own setting
// ("Use primary profile's addons", on by default) rather than the account's
// `uses_primary_addons`. A second profile's own rows are whatever was copied
// when it was made, and go stale.
int  profiles_addon_profile(void);
void profiles_set_active(int index_);
void profiles_load_active(void);       // reads from disk; call at startup

// 1 when there is more than one profile and the user has not chosen yet on this
// account — it is what makes the picker screen appear once, and only once.
int  profiles_needs_choose(void);

// The address of the profile's picture, or 0 when it has none and the circle
// falls back to the initial. `out` is the caller's buffer — no static, because
// the picker resolves one profile while it is still drawing with the last one's
// address (tex_get_width and tex_aspect both take the path).
int profiles_avatar(const AccountProfile *p, char *out, unsigned long n);

// Validates the PIN of a locked profile. BLOCKS. 1 when the server accepted it.
int  profiles_verify_pin(int index_, const char *pin);

// Forgets the profiles, the owner and the recorded choice. Called on SIGNING
// OUT: keeping the choice would make the next account start syncing the
// previous account's `p_profile_id` — that is, WRITING progress to the wrong profile.
void profiles_forget(void);

#endif
