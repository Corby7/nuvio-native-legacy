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
#include <stddef.h>
#include "catalog.h"

// Dispara a montagem do catalogo num fio proprio. Volta na hora.
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

// The TMDB key as already loaded. Returns "" when art/tmdb.txt does not exist.
// The `person` module needs it for the filmography, and reading the file twice
// would give two sources of truth for the same secret.
const char *disc_key_tmdb(void);

// "2026-07-29" -> "29 July 2026". It lives here because discovery already needed
// it for the episode date; the "Film Details" table is the second consumer, and
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

// Copia o resultado `i`. 1 se copiou.
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
const char *disc_search_target_title(int target);   // "Filmes", "Séries"
const char *disc_search_target_addon(int target);    // "Cinemeta", "Xperience"
int  disc_search_target_n(int target, const char *term);
int  disc_search_target_item(int target, int i, CatItem *dst);
// Goes up with every new term. Anything that keeps a focus position between
// frames should readjust when this number changes.
int  disc_search_generation(void);

// Registers the targets, called by the manifest loading. `reset` puts back
// Cinemeta only.
void disc_targets_search_reset(void);
void disc_target_search(const char *base, const char *kind, const char *id,
                     const char *title, const char *addon);

// 1 while searching; the home can use this for an indicator.
int  disc_searching(void);

// Requests the episodes of title `itemIndex` in season `season` (0 = the season
// the owner stopped on, or the first). Idempotent: asking twice for the same
// thing does not repeat the fetch.
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
int disc_seeall_error(void);
// Pede a proxima pagina, se houver. Nada acontece se a ultima veio curta — sinal
// de fim de lista no protocolo.
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
// The same thing starting from the TMDB id, which is what an actor's credit
// carries. `type` is "movie" or "tv". It resolves the IMDb id through
// external_ids before asking for the meta — one extra call, only when the owner
// opens the credit.
void disc_request_title_tmdb(long tmdbId, const char *kind);
// Index of the title that has just been added, or -1. CONSUMES the result.
int  disc_title_ready(void);
int  disc_title_searching(void);

#endif
