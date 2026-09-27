// Assembles the catalogue AT RUNTIME, from the network.
//
// Everything used to come from hand-generated files (catalog.txt, ids.txt,
// episodes.txt) with the art downloaded alongside the package. It worked, but it
// froze: yesterday's recommendation, yesterday's season episode, and every
// change meant regenerating and reinstalling. Now the rows come from the owner's
// addons' catalogues and the episodes come from Cinemeta the moment the title
// opens.
//
// The files still serve as a FALLBACK: with no network, the app opens with what
// came in the package instead of opening empty.
#ifndef NV_DISCOVER_H
#define NV_DISCOVER_H

// How long a response may be answered from net_download_cached's memory.
//
// META: Cinemeta itself says three hours (Cache-Control: public, max-age=10800
// on /meta), and every module that reads a title's /meta goes through this, so
// Continue watching, the detail page and Next Up share one download.
// ADDON: manifests and catalogues. Long enough to cover the rebuilds the account
// sync triggers seconds after the first build (same addons, same URLs); short
// enough that a catalogue that changes during the day is seen on the next home.
#define DISC_META_TTL_S  10800
#define DISC_ADDON_TTL_S 600
#include <stddef.h>
#include "catalog.h"

// Kicks off building the catalogue on a thread of its own. Returns immediately.
void disc_start(void);

// --- HOME ROW PREFERENCES, FROM THE ACCOUNT ---------------------------------
//
// The order of the home rows, which of them are hidden and what they are called
// belong to the PERSON, not to the addon: the web app keeps them in
// `homeCatalogPrefs` and syncs them through `sync_pull_home_catalog_settings`.
// Until this existed the native app only read a local `rows.txt` that nothing
// ever wrote, so the rows came out in whatever order the manifest happened to
// declare — and with 151 catalogues declared and only 16 rows shown, what the
// person had actually chosen was usually below the cut.
//
// The key is `<addonId>_<type>_<catalogId>`, byte for byte the same key the web
// app builds (`buildCatalogOrderKey`), which is what lets the two agree.
//
// Call begin, then add once per item IN ORDER, then end.
void disc_prefs_begin(void);
void disc_prefs_add(const char *key, int enabled, const char *customTitle);
void disc_prefs_end(void);
// Inserts a key at position `index` of the order (clamped to the end). For rows
// the account's blob does not carry — the Trakt rows, see homerows.h — merged in
// between add and end.
void disc_prefs_insert(int index, const char *key, int enabled);

// The owner's order, read back RAW so the home can assemble it.
//
// The list interleaves catalogues (`<addonId>_<type>_<catalogId>`) and
// collections (`collection_<id>`). This module only resolves catalogues, so it
// cannot apply the order alone — the home is the only place that can turn both
// kinds into rows, and it needs the whole sequence to do it in the right order.
int         disc_prefs_n(void);
const char *disc_prefs_key(int i);
int         disc_prefs_hidden(const char *key);
const char *disc_prefs_title(const char *key);   // NULL when not renamed

// The catalogues the addons declared in the last build, for the Home rows editor.
// `title` is the formatted row name; `nameAddon` is the addon's display name.
typedef struct {
  char key[192], title[96], kind[8], id[96], nameAddon[64];
} DiscDecl;
int disc_decls_copy(DiscDecl *out, int max);

// Asks for the rows to be built AGAIN, once whatever is running has finished.
// Call it when the addon list changes — the account's list arrives from the sync
// long after the first build, which ran with no addons at all.
void disc_rebuild(void);

// Call ONCE PER FRAME. Starts a requested rebuild as soon as no build is in
// flight. Without it a rebuild asked for during the first build is simply lost,
// which is exactly the race it has to survive.
void disc_step(void);

// Reads the TMDB key (art/tmdb.txt). Without it the cast has only names, no
// photo and no character.
void disc_tmdb(const char *dirArt);

// The TMDB key from the ACCOUNT, in place of art/tmdb.txt. MEASURED on the
// owner's account: `sync_pull_provider_credentials` returns the "tmdb" provider
// with an `api_key` field. As long as the key comes from the file, it travels
// inside the .ipk and it is the key of whoever built the package — their quota,
// for everyone who installs it.
void disc_tmdb_set(const char *key);
// Sign-out: the account's key goes, from memory and from disk.
void disc_tmdb_forget(void);

// The TMDB key as already loaded. Returns "" when art/tmdb.txt does not exist.
// The `person` module needs it for the filmography, and reading the file twice
// would give two sources of truth for the same secret.
const char *disc_key_tmdb(void);

// A title's TMDB id from its IMDb id. Cinemeta's /meta already carries it
// (`moviedb_id`: 1396 for Breaking Bad, 278 for The Shawshank Redemption), and
// that body is usually in the shared cache already, so this is normally free;
// TMDB's /find is the fallback for the titles Cinemeta has no id for. 0 when
// neither knows. `imdb` may carry an episode suffix. BLOCKS.
long disc_tmdb_id(const char *imdb, int series);

// ONE TMDB request per title, shared by every module that reads it: the details
// with `append_to_response` carrying what each of them needs — the credits and
// watch providers (discover.c's cast photos and streaming badge), the full cast
// (person.c's cast page: aggregate_credits on a series, credits on a film), and
// release_dates and videos (extras.c's fact sheet and trailers, films only).
// These were five separate requests, three of them in series. Answered from
// net_download_cached; the caller frees. NULL without a key. BLOCKS.
char *disc_tmdb_title(long tmdbId, int series);

// "2026-07-29" -> "29 July 2026". It lives here because discovery already needed
// it for the episode date; the "Movie Details" table is the second consumer, and
// duplicating the month list would be asking for the two to drift apart. Input
// outside the ISO format comes back as it arrived.
void disc_date_long(const char *iso, char *dst, size_t size);

// Canonical genre label. Cinemeta and the packaged catalogue store genres in
// English, but not consistently ("Sci-Fi" from one source, "Science Fiction"
// from another, hyphenated forms that read as identifiers). The table
// normalises those; a genre outside it comes back as it arrived.
const char *disc_genre_label(const char *g);

// --- search by title ---------------------------------------------------------
// Queries Cinemeta for both film and series. DOES NOT BLOCK: it starts a thread
// and returns immediately; calling again with the same term does not repeat the
// request, and with a different term the in-flight thread discards the old result
// and goes after the new one (the owner keeps typing while the network answers).
//
// It exists because the search screen only filtered what was already in memory,
// and looking for anything outside the first rows of each catalogue found
// nothing.
void disc_fetch(const char *term);

// How many results there are FOR THIS TERM. Returns 0 when what arrived belongs
// to an earlier query — so the screen never shows another word's results.
int  disc_search_n(const char *term);

// Copies result `i`. 1 if it copied.
int  disc_search_item(int i, CatItem *dst);

// --- searching ACROSS SEVERAL catalogues -------------------------------------
//
// A "target" is a catalogue that declares search in its manifest. There are
// Cinemeta's 2 plus whatever the owner's addons declare (today 8: Xperience,
// AIOStreams by TMDB and by TVDB, and Akashi TV — film and series in each).
//
// The screen draws ONE ROW PER TARGET, in the order the targets were registered,
// skipping those that have not answered yet or came back empty. That way the
// first catalogue to answer appears straight away, instead of the screen waiting
// on the slowest of ten.
int  disc_search_n_targets(void);
const char *disc_search_target_title(int target);   // "Movies", "Series"
const char *disc_search_target_addon(int target);    // "Cinemeta", "Xperience"
// The CATALOGUE behind a search row, for the "See All" button at the end of it.
// The three together are what seeall_open takes; without them the button could
// be drawn and could do nothing, which is worse than not drawing it.
const char *disc_search_target_base(int target);
const char *disc_search_target_kind(int target);   // "movie" | "series"
const char *disc_search_target_id(int target);
int  disc_search_target_n(int target, const char *term);
int  disc_search_target_item(int target, int i, CatItem *dst);
// Goes up with every new term. Anything that keeps a focus position between
// frames should readjust when this number changes.
int  disc_search_generation(void);

// --- THE GENRES A CATALOGUE DECLARES --------------------------------------
// Stremio puts them in the catalogue's own `extra` block, as
// `{"name":"genre","options":["Action","Comedy",...]}`. Nothing read them until
// the Discover screen needed a Genre picker: the home only ever asks a
// catalogue for its first page, and the search asks by title.
//
// Registered from the manifest sweep, alongside the search targets, because
// that is the one place in the app where a catalogue's `extra` block is already
// in hand. Keyed by the same three fields that identify a catalogue
// everywhere else, so the screen can ask about whichever one the picker is on.
void disc_catalog_genres(const char *base, const char *kind, const char *id,
                         const char *options, const char *end);
// The manifest's name for one catalogue of an installed addon, or "" when none
// declares it. It is the WHOLE declared set and not the home's rows: a
// collection routinely points at a catalogue the owner has switched off the
// home, and it still has to be named on the collection screen. See the note on
// the implementation for why the kind is not part of the key.
const char *disc_catalog_title(const char *base, const char *kind,
                               const char *catId);

int  disc_genres_n(const char *base, const char *kind, const char *id);
// Option `i`, or "" out of range. The list does NOT include the "Default"
// entry the picker shows first — that one means "no genre parameter at all",
// which is a different request and not a genre.
const char *disc_genre_at(const char *base, const char *kind, const char *id, int i);

// Registers the targets, called by the manifest loading. `reset` empties the
// list; `fallback`, called once every manifest is in, adds Cinemeta only when
// no installed addon declared a search catalogue.
void disc_targets_search_reset(void);
void disc_targets_search_fallback(void);
void disc_target_search(const char *base, const char *kind, const char *id,
                     const char *title, const char *addon);

// 1 while searching; the home can use this for an indicator.
int  disc_searching(void);

// Requests the episodes of title `itemIndex` in season `season` (0 = the season
// the owner stopped on, or the first). Idempotent: asking twice for the same
// thing does not repeat the fetch.
// --- GROWING A HOME ROW ------------------------------------------------------
//
// A home row no longer stops at the twelve it was built with and no longer ends
// in a card that leads somewhere else: reaching its end asks the catalogue for
// the page after what it holds, and the row gets longer in place. See the long
// note in discover.c.
//
// `have` is how many the row has RIGHT NOW, which is the skip to ask for. One
// request is in flight for the whole home at a time; a second call while it runs,
// or on a row whose catalogue has run out, does nothing.
void disc_row_more(const char *key, const char *base, const char *kind,
                   const char *catId, int have);
// 1 while this row's page is on its way. Whatever draws can say so.
int  disc_row_loading(const char *key);
// 1 once this row's catalogue has answered with nothing new. It stays that way
// for the session.
int  disc_row_ended(const char *key);
// Puts a page that has landed into its row, ON THE DRAWING THREAD, and returns
// how many items went in. Called once a frame by the home.
int  disc_row_collect(void);

// --- SEE ALL: a whole catalogue, in pages ------------------------------------
//
// The home shows 12 items per row (MAX_PER_ROW). The catalogue has more, and the
// Stremio protocol pages by `skip`:
//   <base>/catalog/<type>/<id>/skip=<n>.json
// It is the same path as the search, with a different filter in place of the
// term.
//
// Asynchronous, like everything else: it fires and returns immediately. Whatever
// draws asks how many have arrived.
#define SEEALL_MAX 1000

// Starts (or continues) reading the catalogue. `page` 0 is the beginning; each
// following page asks for skip = page * SEEALL_STEP. Repeating the same page does
// not repeat the request.
void disc_seeall_open(const char *base, const char *kind, const char *catId);
void disc_seeall_filter(const char *base, const char *kind, const char *catId, const char *genre);
// The grid behind a search row: every page carries `search=<term>`.
void disc_seeall_search(const char *base, const char *kind, const char *catId, const char *term);
int disc_seeall_error(void);
// Asks for the next page, if there is one. Nothing happens if the last one came
// back short — the protocol's end-of-list signal.
void disc_seeall_more(void);
int  disc_seeall_n(void);
int  disc_seeall_item(int i, CatItem *dst);
int  disc_seeall_loading(void);
// 1 when the last page came back short: there is nothing more to ask for.
int  disc_seeall_end(void);
void disc_seeall_close(void);

void disc_episodes(int indexItem, int season);
// Releases the episode request that arrived while another was loading. Call it
// per frame; without this a season change made during a load hangs and the list
// never reaches the chosen season.
void disc_episodes_pending(void);
int disc_episodes_loading(int indexItem);

// Fetches the meta of a title the catalogue does NOT have and appends it to the
// end. Does not block. It serves an actor's credit and the "More like this"
// item: without it, anything outside the owner's catalogue would not open.
void disc_request_title(const char *imdb);
// The same, when the caller KNOWS the kind ("movie" or "series") — a "More like
// this" item is always the kind of the title it was listed under. Without it
// the meta is tried as a film first and, failing that, as a series: two round
// trips for every series.
void disc_request_title_kind(const char *imdb, const char *kind);
// The same thing starting from the TMDB id, which is what an actor's credit
// carries. `type` is "movie" or "tv". It resolves the IMDb id through
// external_ids before asking for the meta — one extra call, only when the owner
// opens the credit.
void disc_request_title_tmdb(long tmdbId, const char *kind);
// Index of the title that has just been added, or -1. CONSUMES the result.
int  disc_title_ready(void);
int  disc_title_searching(void);

#endif
