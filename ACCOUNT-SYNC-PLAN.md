# Account and synchronisation in the native app — parity with the web app

Goal (DECIDED BY THE OWNER, route B): someone else installs the `.ipk`, signs in
to their own account and the device starts behaving like the official app — same
addons, same Trakt, same debrid, same progress, same profiles, and what they
watch here shows up on their other devices.

Today the native app is a ONE-owner app: every credential is a text file inside
the package (`art/trakt.txt`, `art/addons.txt`, `art/tmdb.txt`). The current
`.ipk` CANNOT be distributed — it hands the owner's Trakt token and debrid keys
to whoever installs it. This document is the path to replacing those files with a
real session.

The source for everything here: reading `NuvioWeb-0.3.38-beta` (`js/core/auth/`,
`js/core/profile/`, `js/data/remote/supabase/supabaseApi.js`). Where the text
says FACT, it was read in the web's code; where it says ASSUMPTION, it has not
yet been measured against the server.

---

## Part 1 — The network contract

### 1.1 Transport

FACT: all of the web's sync goes through ONE single shape — `POST` to
`<SUPABASE_URL>/rest/v1/rpc/<function>` with a JSON body and headers:

    apikey: <SUPABASE_ANON_KEY>
    Authorization: Bearer <the session's access_token, or the anon key itself>
    Content-Type: application/json

The response is always a JSON array of rows (or an object, on the blob RPCs).
There is no WebSocket and no raw `postgrest` on the happy path — direct table
access (`tv_profiles`, `tv_addons`, `plugins`, `addons`) only appears as a
FALLBACK when the RPC answers "function not found" (PGRST202) or "table not
found" (PGRST205). The native app implements the RPC path and treats the fallback
as "this surface is not available on this server", without trying to reproduce
the web's two compatibility ladders.

PRACTICAL CONSEQUENCE: the native app needs no Supabase library. It needs POST
with headers (already in `net_post`), JSON reading (already in `js.c`) and JSON
WRITING (did not exist — new module `jsw.c`).

### 1.2 TV login (no keyboard)

FACT, `js/core/auth/qrLoginService.js` and `authManager.js`:

1. **Anonymous session first.** The login RPCs run authenticated as an anonymous
   user; without that the server answers 401. `ensureQrSessionAuthenticated`.
2. `POST /rest/v1/rpc/start_tv_login_session`
   `{ p_device_nonce, p_redirect_base_url, p_device_name }` -> returns the `code`.
   `p_device_name` is omitted and the call repeated when the server refuses the
   parameter (an old server).
3. `POST /rest/v1/rpc/poll_tv_login_session` `{ p_code, p_device_nonce }` ->
   `status`. While it is pending, repeat.
4. `POST /functions/v1/tv-logins-exchange` `{ code, device_nonce }` ->
   `access_token` + `refresh_token`. It is the only endpoint outside `/rest/v1/`.

`p_redirect_base_url` comes from `TV_LOGIN_WEB_BASE_URL`: it is the page the
person opens on their phone. The web's QR encodes that URL with the code.

#### MEASURED against `https://api.nuvio.tv` (2026-09-03)

Three things the web's code did not say, and that only the real call showed:

```
start_tv_login_session ->
[{"code":"fa0010cad8b5d2f512e58646ab82ca6b",
  "web_url":"https://nuvio.tv/tv-login?code=fa0010cad8b5d2f512e58646ab82ca6b",
  "expires_at":"2026-09-03T03:49:55Z","poll_interval_seconds":3}]
poll_tv_login_session  -> [{"status":"pending", ...}]
```

1. **`p_device_nonce` HAS to be a UUID.** An identifier of our own, however
   unique, returns `400 {"message":"Invalid device nonce"}`.
2. **An empty `p_redirect_base_url` is refused** (`Invalid TV login redirect base
   URL`). Without `TV_LOGIN_WEB_BASE_URL` configured there is no login.
3. **The response already carries a ready `web_url` and `poll_interval_seconds`**
   (3, not the web's 2). Using both avoids building the URL by hand and
   hammering the server.
4. Polling with the wrong nonce returns 400 `Invalid TV login session` — the
   session really is tied to the device that asked for it.

#### REVISED DECISION: the QR is mandatory

The plan said "show the code in big letters, QR later". The measurement killed
that: **the code is 32 hexadecimal digits**. Nobody reads that off the TV and
types it into a phone without a mistake. The QR stopped being decoration and
became the only way in — which is why `qr.c` landed in Phase A and not in a
polishing phase.

### 1.3 Refresh and expiry

FACT: the web checks the JWT's `exp` with 30s of slack and uses the anon key as
the Bearer when the token has expired. Supabase's own refresh is
`POST /auth/v1/token?grant_type=refresh_token` with `{ refresh_token }`.

RULE in the native app: `cloud_rpc` tries once; on **401**, it refreshes and
retries ONCE. If the refresh fails, the session becomes signed out and the UI
returns to the login screen — it never sits in a limbo where the app looks
signed in and nothing syncs.

### 1.4 The RPCs, by surface

MEASURED against the server (2026-09-03) — the table below came from the web
app, and not everything it promises exists:

- `sync_pull_addons` **DOES NOT EXIST** (PGRST202) and neither does the
  `tv_addons` table (PGRST205). Addons only come from the `addons` table,
  filtered by `user_id` + `profile_id` and ordered by `sort_order`.
- Reading the `addons` table with the ANONYMOUS key returns
  `401 permission denied for table addons`: the RLS needs the user's token. This
  was a real defect in this code — the read went without the token and came back
  empty, which looked like "the account has no addons".
- `sync_pull_watched_items` uses `p_page` starting at **1**. With 0 it answers
  `400 OFFSET must not be negative`.
- `sync_pull_saved_library` **DOES NOT EXIST** on this server.
- `get_sync_owner` returns a **raw JSON string** (`"441bf572-…"`), not an object
  — and it is the OWNER's id, which may not be the `sub` of whoever signed in.

| Surface | pull | push | delete | parameters beyond `p_profile_id` |
|---|---|---|---|---|
| Profiles | `sync_pull_profiles` | `sync_push_profiles` | `sync_delete_profile_data` | push takes `p_client_max_profiles`, `p_profiles` |
| Profile locks | `sync_pull_profile_locks` | `set_profile_pin` / `clear_profile_pin` | — | `verify_profile_pin` validates |
| Addons | `sync_pull_addons` | `sync_push_addons` | — | `p_addons: [{url, sort_order, enabled, name?}]` |
| Plugins | (reads the `plugins` table) | `sync_push_plugins` | — | `p_plugins`, `p_origin_client_id` |
| Library | `sync_pull_library` | `sync_push_library` | — | — |
| Saved library | `sync_pull_...` paginated | `sync_push_...` | — | `p_limit`, `p_offset`, `p_items` |
| Collections | `sync_pull_collections` | `sync_push_collections` | — | `p_collections_json` |
| Progress | `sync_pull_watch_progress` | `sync_push_watch_progress` | `sync_delete_watch_progress` | `p_entries`, `p_keys`, `p_origin_client_id` |
| Watched | `sync_pull_watched_items` | `sync_push_watched_items` | `sync_delete_watched_items` | `p_page`, `p_page_size`, `p_items`, `p_keys` |
| Profile settings | `sync_pull_profile_settings_blob` | `sync_push_profile_settings_blob` | — | `p_platform: "tv"`, `p_settings_json` |
| Home catalogues | `sync_pull_home_catalog_settings` | `sync_push_home_catalog_settings` | — | `p_platform: "home_catalog_shared"` |
| Credentials (debrid) | `sync_pull_provider_credentials` | `sync_push_provider_credentials` | `sync_delete_provider_credentials` | `p_credentials: [{provider, credential_json}]` |
| Trakt | same credentials RPC | same | same | `provider = "trakt"` |
| Simkl | same credentials RPC | same | same | `provider = "simkl"` |

IMPORTANT FACT: Trakt, Simkl and debrid share the SAME three credential RPCs,
distinguished by the `provider` field. One implementation serves all three.

### 1.5 Row formats

Profile (`mapProfileRow`): `profile_index`, `name`, `avatar_color_hex`,
`avatar_id`, `avatar_url`, `profile_background_id`, `profile_background_url`,
`uses_primary_addons`, `uses_primary_plugins`, `is_primary`.

Addon: `{ url, sort_order, enabled, name? }`.

Plugin: `{ url, name, enabled, sort_order, repo_type }` — `repo_type` only
accepts `nuvio_js` and `external_dex`.

Progress: `{ content_id, content_type, video_id, season, episode, position,
duration, last_watched, progress_key }`. When READING, also accept
`position_ms`/`duration_ms` and `updated_at`, and treat a `last_watched` below
1e12 as SECONDS (the web multiplies by 1000).

Watched: `{ content_id, content_type, title, season, episode, watched_at }`.
Removal key: `{ content_id, season?, episode? }`.

Saved item: `{ content_id, content_type, name, poster, poster_shape, background,
description, release_info, imdb_rating, genres, addon_base_url, added_at }`.

Credential: `{ provider, credential_json }` — `credential_json` is an OBJECT, and
the reader also has to accept a JSON string (the web does `JSON.parse` when a
string arrives).

### 1.6 Sync safety rules that are NOT optional

These are written in the web's code with a comment explaining them; repeat them
in the native app, because each one exists because of real data loss:

1. **An empty remote list is not a deletion.** If the pull returns zero items and
   the local side has items, KEEP the local ones and do not advance the
   baseline. An empty response can be the wrong profile, a mishandled 401 or a
   server outage.
2. **An empty push does not delete.** Deletion happens only through the explicit
   delete RPC, with keys.
3. **Never omit an unknown row from a push.** `pluginSyncService` refuses the
   whole push when there is a row of a type it cannot represent — omitting it
   would become a silent deletion.
4. **`p_origin_client_id`** identifies this device so the server does not echo
   its own write back. Generate a stable id per installation and persist it.
5. **Backoff.** A sync failure enters `syncBackoffPolicy` and every surface stops
   together. Without it, a server that is down becomes a retry storm.

---

## Part 2 — New modules in the native app

| File | Responsibility |
|---|---|
| `jsw.c/h` | JSON WRITING: correct escaping, objects, arrays. `js.c` only reads. |
| `cloud.c/h` | Supabase transport: `cloud_rpc`, HTTP status, 401 -> refresh -> retry once, shared backoff. |
| `session.c/h` | Tokens on disk, anonymous session, TV login (start/poll/exchange), refresh, the JWT's `sub`/`exp`, sign out. |
| `login.c/h` | Login screen: large code, URL, state, error. |
| `profiles.c/h` | Profile list, active profile, PIN. |
| `sync.c/h` | The surfaces, in `startupSyncService` order, on a thread of their own. |
| `data.c/h` | Where it writes: discovers a WRITABLE directory and is the only answer to that question in the app. |
| `qr.c/h` | QR generator (byte mode, level L, versions 1-6). Mandatory, see 1.2. |

### A QR cannot be checked by eye

A wrong QR does NOT raise an error: it draws, the finder patterns sit in the
right place, the screen looks fine, and no phone decodes it. It happened here —
the format bits were in reverse order and the second copy was badly mapped, and
none of that is visible. That is why `tools/qr_check.py` DECODES (OpenCV)
instead of comparing against another implementation: two matrices can differ and
both be valid, so comparing matrices proves nothing. There is only one useful
question: does a reader read it?

### Where to write

A PROBLEM MEASURED IN THE CURRENT CODE: `settings.c` and `catalog.c` write inside
`dirArt`, which is the package folder. In developer mode that works; in a
properly installed app it is the wrong place, and it is the same directory for
every user of the device.

`data.c` solves this with a PROBE, not a guess: it tries `$NUVIO_DATA`,
`$HOME/.nuvio`, `/media/developer/temp/nuvio` and `dirArt` in order, creating and
writing a test file in each, and uses the first that accepts. The chosen path
goes to the log. This is deliberate: there is no reliable public documentation of
where a NATIVE webOS app may write, and guessing a fixed path would turn "nothing
was saved" into a silent defect.

### Compile-time secrets

`SUPABASE_URL` and `SUPABASE_ANON_KEY` are the same ones the web app already
publishes in its bundle (the anon key is public per project — it is what lets the
RLS decide). They enter through `-D` at build time, generated from
`local.properties` by `tools/env.sh`, and are never written into a versioned
source file. For development, an `art/cloud.txt` overrides them at runtime.

---

## Part 3 — Order of execution

- **Phase A — FOUNDATION. DONE (not verified on the device).** `jsw`, `data`,
  `cloud`, `session`, `qr`, login screen.
  VERIFIED on the Mac: anonymous session and code request against the production
  server; QR decoded by a real reader FROM THE APP'S OWN SCREEN CAPTURE
  (`https://nuvio.tv/tv-login?code=882d18…`), not just from the generator.
  NOT VERIFIED: the final exchange for the token (it needs someone authorising on
  a phone), the refresh-token renewal, and which folder the device accepts for
  writing — `data.c`'s probe decides that at startup and writes it to the log.
- **Phase B — PROFILES. DONE.** `profiles.c` (profile pull + locks + owner +
  persisted active profile) and `profile_select.c` (the "Who is watching?"
  screen, with a PIN keypad validated ON THE SERVER — keeping the PIN on the
  device would defeat the purpose of a locked profile).
- **Phase C — WHAT MAKES THE APP WORK. DONE.** VERIFIED in the running app:
  `[addons] 2 from the account` — the account's list replaced the file's. The
  Trakt credential comes from the same credentials RPC (provider `trakt`); the
  client id belongs to the APPLICATION and is compiled in
  (`-DNV_TRAKT_CLIENT_ID`).
- **Phase D — CONTINUITY. DONE (the pull cannot be verified without an account
  that has data).** Progress pulled and pushed, with season/episode separate from
  `content_id`. The player marks `sync_dirty_progress()` on close.
- **Phase E — THE REST. PULLED, NOT PUSHED — and that is deliberate.** Library,
  watched, collections, profile settings and home catalogues are read and counted
  in the summary, but receive NO push. The native app has no screen that edits
  any of them, so the only possible push would be an empty list — and an empty
  list erases the data on the person's other devices (rule 2 of section 1.6).
  Taking part in two-way sync only on the surfaces the app actually owns is the
  only safe way to do this without having every screen.
- **Phase F — RELEASE. DONE in the code; the package build remains.**
  `home_start` returning 0 no longer takes the app down (that was the behaviour
  of EVERYONE's first run on a package with no art: it opened and closed before
  the login screen); there is an empty state, "Preparing your catalogue…"; "Sign
  out" landed in Settings > Account, next to the active profile and the sync
  summary.
  WHAT REMAINS, and it is the decision of whoever builds the `.ipk`: **do not
  include `art/addons.txt`, `art/trakt.txt` or `art/tmdb.txt`**. The code no
  longer depends on them — they are still read only as a development fallback —
  but while they sit inside the package, the `.ipk` keeps handing over the
  credentials of whoever built it.

---

## Part 4 — MIGRATED to the right repository (2026-09-03)

The migration to `nuvio-native-legacy` **was carried out and verified**. What
follows describes what was moved and the differences the legacy code imposed. The
plan's original text comes afterwards, as a record.

### Differences the legacy code imposed (unforeseen)

1. **The legacy `net.h` ALREADY DECLARED `net_post_st` and `net_download_st`** —
   with my comments — but `net.c` **implemented neither**. A declaration with no
   body does not break the build while nobody calls it. I implemented them.
2. **The legacy `net_download_internal` KILLS 4xx**: it returns NULL and logs.
   For Supabase that would erase the only clue (`PGRST202`/`PGRST205` arrive in
   the body of the 404). The new implementation skips that cut **only when
   `status` was asked for**.
3. **Type collision:** the legacy `trakt.h` already used `Profile`. I renamed
   mine to `AccountProfile` / `ACCOUNT_PROFILE_MAX`.
4. **Screen collision:** the legacy already has `SCREEN_PROFILE` (Trakt
   statistics). Mine became `SCREEN_PROFILE_PICKER`.
5. **The legacy already had `cat_index_by_imdb`, and better than mine** (it
   compares up to the `:` without computing a length). Its version was kept; I
   ported only `cat_dir_writing`.
6. **The legacy has `cat_save_progress_ep`** (with season and episode). Its
   `sync.c` now uses that version: without it, a series' progress would lose
   which episode the person stopped on.
7. **The legacy `js.c` has a fix mine does not** (`js_num` accepting a number in
   quotes, which is how Cinemeta sends `imdbRating`). That is why the file was
   **added to**, never overwritten.
8. **The legacy `settings.c` is a different program** (10 sections, `OP_CHOICE`/
   `OP_NUMBER`/`OP_READ` types with macros). I added `OP_ACTION` and the
   `ACTION(...)` macro in its style, rather than copying my version.

### Verification in the legacy repo

| Step | Result |
|---|---|
| Reference build BEFORE touching anything | 0 errors, **0 warnings** |
| Build after the migration | 0 errors, **0 warnings** — no new warning |
| `tools/qr_check.py` | ALL READABLE |
| App opened with no session | opens on the login screen |
| QR decoded **from the legacy build's screen capture** | `https://nuvio.tv/tv-login?code=ea053e96…` |
| Full sync cycle | `[addons] 2 from the account`, applied to the app's list |
| Log | `[data] writing to /tmp/nvl-data`, `[cloud] https://api.nuvio.tv` |

No commit, no stash, no checkout: the owner's 85 uncommitted changes remained
intact. The counter went from 85 to 109 (21 new files + 3 that were still clean).

---

## Part 4 (record) — why this code was born in the WRONG repository

Phase A was written in `nuvio-native/`, which is stalled: 10.8 thousand lines and
NO commits. The living project is `nuvio-native-legacy/` — 22 thousand lines,
commits up to 2026-09-01, and modules that do not even exist there (`parental`,
`collections`, `extras`, `episodes`, `badges`, `mkv`, `seeall`, `person`, `mark`,
`ctxmenu`).

The migration is cheap because the new modules barely depend on the app:

- COPY unchanged: `jsw.[ch]`, `data.[ch]`, `cloud.[ch]`, `session.[ch]`,
  `qr.[ch]`, `login.[ch]`, `profiles.[ch]`, `profile_select.[ch]`, `sync.[ch]`,
  `tools/env.sh`, `tools/qr_check.py`, `tools/qr_dump.c`.
- APPLY to `net.[ch]`: `net_post_st` and `net_download_st` (POST and GET that
  return the HTTP code). The legacy `net.c` has extras (`net_download_chunk`,
  `net_cap`) — apply the hunk, do not swap the file.
- APPLY to `js.[ch]`: `js_root_array` (the RPCs answer with an array at the ROOT,
  and `js_array` only looks for an array by NAME) and `js_raw` (a raw JSON value,
  so `credential_json` passes through whole instead of being rebuilt field by
  field).
- APPLY to `addons.[ch]`: `addons_set_list` and `addons_export`.
- APPLY to `trakt.[ch]`: `trakt_set`.
- APPLY to `catalog.[ch]`: `cat_dir_writing` and `cat_index_by_imdb`.
- APPLY to `player.c`: `sync_dirty_progress()` alongside `trakt_mark`.
- APPLY to `settings.c`: the "Account" section (profile, sync summary, sign out)
  and the `action` field in the `Option` struct — an action row responds to OK
  and has to have the same highlight as one that changes a value, otherwise the
  user presses OK expecting nothing to happen.
- WIRE UP: `SCREEN_LOGIN` and `SCREEN_PROFILES` in `app.h`, the branches in
  `app.c` (event, update, draw, home's empty state) and the order
  `data_start` / `cloud_configure` / `session_start` / `profiles_load_active`
  before `app_start` in `main.c`, with `cat_dir_writing(data_dir())` afterwards.
- CHECK: `js.h` was IDENTICAL in both before these additions, and `gfx_color`,
  `gfx_rect`, `gfx_tex_aspect_current`, `GFX_SNAP`, `txt_line`, `txt_line_trim`,
  `txt_draw_alpha`, `anim_spring`, `js_text` and `js_num` all exist in the legacy
  code under the same name. There is no adaptation to do.

The legacy `settings_dir` also writes inside the package folder and should move
to `data_dir()` during the migration.

---

## Part 4b — After the migration (2026-09-03)

Two open items closed, both about LEAKAGE BETWEEN PEOPLE.

### The `.ipk` carried credentials (closed)

`tools/arm.sh` ran `ares-package deploy/app` — the WHOLE folder. Inside `art/`
travelled `trakt.txt` (token), `addons.txt` (URLs with the debrid key embedded),
`tmdb.txt`, `mdblist.txt` (mode 0600) and `settings.txt` (the layout of whoever
built it). Packaging now runs from a CLEAN copy and the script CHECKS the
finished package, aborting and deleting the `.ipk` if any of them comes back.

A MEASURED TRAP that almost made the check useless: the `.ipk` is a Debian
package. `tar tzf package.ipk` lists, WITH NO ERROR, only `debian-binary`,
`control.tar.gz` and `data.tar.gz` — never the app's files. A check written that
way always passes, secret included. You have to unpack the `ar` and list the
`data.tar.gz`.

`tools/test-ipk.sh` proves this without docker, and was verified BOTH WAYS: with
the exclusion the package comes out clean; letting `trakt.txt` in on purpose, the
test catches it and returns 1.

### Signing out did not erase the user (closed)

`session_exit()` deleted the token and nothing else. Left behind for the next
person:

- the addon list in memory — and with it the debrid keys embedded in the URLs,
  which is to say the departing person's subscription;
- the Trakt token, which would keep WRITING whatever the next person watched into
  the departing person's account;
- the saved active profile, so the next account's first sync would write progress
  under the previous `p_profile_id`;
- the previous person's `progress.txt`, on disk.

On a living-room TV, "sign out" is the only barrier between two people. There is
now `sync_forget_user()`, called alongside `session_exit()`, and it also clears
the boxes the sync thread fills — otherwise a cycle that finished just before the
sign-out would reapply the previous account's addons on the next `sync_step`.

`tests/account.sh` builds a real signed-in state (2 addons from the account,
Trakt on, profile 3 chosen, progress on disk), signs out, and checks ten items.
All ten pass.

### Profile settings: applied (closed)

`settings.c` has ~40 options whose keys are the SAME as the web app's
(`heroSectionEnabled`, `continueWatchingCardStyle`, `cardDepthEnabled`,
`posterCardWidthDp`...) and whose defaults were transcribed BY HAND from the
profile of whoever built the package — the code itself said "owner's profile;
factory: off". The `sync_pull_profile_settings_blob` RPC returns exactly that
object, and `sync.c` merely COUNTED it. Now it reads the raw `settings_json` and
`settings_apply_blob()` applies it.

The mapping is the dangerous part, because getting it wrong is SILENT — no error,
no crash, nothing in the log; the person just thinks the TV "came with different
options". Every literal was checked against the web's code:

| key | web values | index |
|---|---|---|
| booleans | `true` / `false` | 0 / 1 (the first label is "On") |
| `collapseSidebar` | `true` / `false` | 0 "Collapsed" / 1 "Fixed" |
| `discoverLocation` | `in_search`, `in_sidebar`, `off` | 0, 1, 2 |
| `homeImdbRatingsVisibility` | `SHOW_ALL`, `HIDE_ALL` | 0, 1 |
| `continueWatchingCardStyle` | `card`, `wide`, `poster` | 0, 1, 2 |
| `continueWatchingSortMode` | `default`, `streaming_style`, `split_upcoming` | 0, 1, 2 |
| numbers | the value itself | clamped to the option's range |

Two deliberate refusals: a MISSING key does not touch the option, and neither
does a text value this app does not recognise (a newer web version) — choosing a
default there would invent a preference the person never set, and they would see
the TV change on its own.

WHEN it applies: only on the FIRST pass after signing in or switching profile
(`sync_reapply_settings()`). The native app reads the settings but does not write
them back; reapplying on every cycle would undo, seconds later, anything the
person changed on the TV itself.

`tests/account_settings.c` feeds a blob with every value at the OPPOSITE of the
default — so that an option that was not applied shows up as a failure rather
than a coincidence — and checks 23 items, including the two refusal cases. All 23
pass.

### Verified ON THE TV, with a real account (2026-09-03)

`[session] signed in as 88376366-…` — the QR login was COMPLETED on the device,
with someone authorising on a phone. It was the only path that had never been
exercised. Alongside it: `[profiles] 2 profile(s)`, `[addons] 3 from the
account`, `[settings] account blob: 33 key(s) recognised`, `[disc] tmdb: key from
the account`, `27 of 51 progress entries matched the catalog`, 60.0 fps / 0
janks. The writable folder stopped being a probe: `.nuvio/` inside the app's
folder, with `session.txt`, `progress.txt` and `settings.txt`.

#### The settings blob was nothing like the web's code suggested

Three discoveries in a chain, all visible only with the real account:

1. **12232 bytes** — it did not fit in the 4096 buffer. Refusing to apply it
   half-way was right; the effect was that the feature did not work. It became
   heap-allocated.
2. **It is not a flat camelCase map.** It is
   `{"version":1,"features":{"layout_settings":{"hero_section_enabled":
   {"type":"boolean","value":true}, ...}}}` — the key in SNAKE_CASE, nested by
   feature, the value WRAPPED in `{type,value}`. Looking for `heroSectionEnabled`
   the app found ZERO keys in 12 KB of settings.
3. **The enums arrive in UPPERCASE** from the server (`IN_SEARCH`, `CARD`,
   `DEFAULT`) while the web app's JS writes them in lowercase. The comparison now
   ignores case: from 30 to 33 recognised keys.

LESSON: reading the code of whoever PRODUCES the data does not replace looking at
the data. The test that existed used the ASSUMED format and passed green while
the app applied nothing on the device. It was rewritten in the server's format.

#### Trakt is NOT in the account (measured, and not an app defect)

`sync_pull_provider_credentials` returns, for this account, on BOTH profiles:
`animeskip`, `debrid:premiumize`, `debrid:realdebrid`, `debrid:torbox`,
`introdb`, `mdblist`, `tmdb` — and **no `trakt`**. The Trakt reader stays in
`sync.c` because the RPC is the same and the row will appear on its own as soon
as the web app writes it. Until then, this app's Trakt link comes from
`art/trakt.txt`, which CANNOT go in the package — which is to say, **whoever
installs it today has no Trakt** until they sign in to the web app once.

The same survey produced a gain: `tmdb` and `mdblist` ARE in the account, and the
app was reading both from the owner's files. They now come from the account
(`disc_tmdb_set`, `extras_set_key`). On the tested account `mdblist` arrived with
an empty `api_key`, so the app kept the local value — the correct behaviour.

#### One UNEXPLAINED crash

Two crashes on the device during the session:

- **16:46, `libc index+0xe0`** — MY defect, understood and fixed: I had added
  three options to the settings enum without adding the three keys to `KEY[]`.
  The last entries were left NULL and every key after the insertion point was
  MISALIGNED. It does not show up immediately: only once `settings.txt` starts
  existing, and then `strcmp(NULL, ...)` takes the app down at startup. `KEY[]`
  lost its explicit size and gained a compile-time check — forgetting a key is
  now a COMPILE ERROR (verified by removing one on purpose).
- **16:57, `libSDL2` on a free path (refcount decrement, pointer 0x1020)** —
  NOT explained. It happened on the round where 5 settings CHANGED, including
  `posterCardWidthDp` (126 -> 116), which feeds the card size and the texture
  decode; the hypothesis is that applying a dimension while home is already
  drawing leaves the texture cache inconsistent. NOT REPRODUCED: on the Mac, with
  the same session and the same 5 settings changing, the app stays up; and so
  does the next round on the TV (0 settings changing). Recorded as open, with the
  report in `/var/log/reports/librdx/`.

#### Operational traps that cost time

- `luna://com.webos.applicationManager/launch` **does not restart** an app that
  is already running: it only brings it to the front. Two deploys were read from
  the log of an old process, and nearly concluded the fix had not worked. Kill
  the PID first.
- The ARM build in `arm.sh` **was not receiving the `-D` flags** (the
  configuration had only been added to `mac.sh`). The binary came out with no
  server, compiled, installed, opened — and only the screen said "package built
  without a server". The variables now enter through `docker run --env-file` and
  the script ABORTS if `api.nuvio.tv` is not in the binary.
- The app's log **loses its last lines in a crash**. The files in `.nuvio/` are
  the reliable evidence, not the log.

### What is still missing

1. **Periodic sync** (5 min, never with the player open) was implemented but has
   not been observed firing on the TV.
2. ~~Sync only ran at three moments~~ (startup, post-login, profile switch).
   Without periodic repetition, stopping on the phone does not show up on the TV
   without reopening.
4. **Surfaces with no push** (library, watched, collections, home catalogues):
   deliberate while there is no screen that edits them — a push with no screen
   would send an empty list and erase the data on the other devices.
5. **The package is 172 MB** of prebaked art. Not a secret, but it is weight.

---

## Part 5 — What was verified, and how

The method matters as much as the result: almost everything here fails SILENTLY.

| What | How it was verified | Result |
|---|---|---|
| Anonymous session | a real call to `api.nuvio.tv` | HTTP 200 |
| Code request | same | `code` + `web_url` + `poll_interval_seconds` |
| Poll | same | `status: "pending"`; with the wrong nonce, 400 |
| QR | decoded by a real reader (OpenCV), **from the running app's screen capture** | read the right URL |
| JSON writer | a test with nesting, escaping and `null` | exact output |
| Token refresh | access token replaced with junk, refresh kept | 401 -> refreshed -> 200, new token written |
| Impossible refresh | both tokens invalid | signed out and DELETED the file |
| Sync cycle | anonymous session written as a user session | profiles, addons, credentials, progress and the read-only RPCs, all exercised |
| Account addons | running app, with the log | `[addons] 2 from the account` |
| Settings > Account | screen capture of the app | profile, sync summary and "Sign out" |

NOT verified, and not verifiable alone: the **final exchange of the code for the
token**, which needs a person authorising on a phone. Everything after that point
has already been exercised with a real session.
