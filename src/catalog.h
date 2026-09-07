// A catalogue of titles coming from a file, in place of lists hard-coded in the
// source.
//
// It exists because testing a layout with made-up names hides real problems:
// real titles have wildly different lengths ("CODA" against "Killers of the
// Flower Moon"), accents, and synopses that do not fit in three lines. Each item
// also carries the title's LOGO, which is what the Apple app draws in place of
// the name in text.
#ifndef NV_CATALOG_H
#define NV_CATALOG_H

// 40 titles today (14 from the owner's history + 26 from the catalogues). The
// slack avoids the silent truncation that already happened once: with 32 the
// last eight disappeared without warning.
// There is no catalogue ceiling any more: the array grows as the network
// delivers. A fixed number here was always arbitrary — it started at 32, became
// 48, 160, 260, and the owner's watchlist kept hitting the limit. What really
// limits things is the TEXTURE cache, which has a ceiling of its own and only
// keeps what is on screen; the item itself costs ~3.5 KB of text.
//
// CAT_MAX survives only as a safety ceiling against an absurd response.
#define CAT_MAX 2000

typedef struct {
  char backdrop[512];
  char poster[512];
  char logo[512];      // empty when the title has no logo
  char title[160];
  char genre[160];    // "TV Show · Drama · Mystery"
  char meta[96];       // "2022 · 3 seasons"
  char age_rating[8];
  char synopsis[900];
  // The real cast: name, role and the photo (when TMDB has one). Without this
  // the "Cast and crew" section is filled with invented names, and invented
  // names do not test the layout — real ones have lengths that break the column.
  // `tmdb` is the PERSON's id on TMDB, not the title's: it is the key to opening
  // their filmography (/person/<id>?append_to_response=combined_credits), which
  // is what the web app does in `openCastDetail`. Without it the only route would
  // be searching by name, which goes wrong on namesakes and on accented names.
  struct { char name[64]; char role[64]; char photo[512]; long tmdb; } cast[6];
  int nCast;
  char directing[128];
  // The critics' score as a percentage and the logo of the service the title is
  // on. They are the two things the Apple app's technical line shows beyond the
  // year and the duration — without them the line carries half the information.
  int  score;              // 0 = unknown
  // The production country, for the detail screen's last meta line (the web app
  // shows 'United States of America' there). It comes from /meta, not from the
  // catalogue.
  char country[64];
  char providerLogo[512];
  char providerName[64];
  // Where to watch beyond a subscription: rental and purchase in TMDB's BR
  // region. Empty = the service does not offer the title that way here. The
  // detail screen shrinks the section accordingly — a card with no data is worse
  // than its absence.
  char rentLogo[512];
  char rentName[64];
  char compLogo[512];
  char compName[64];
  // The title's IMDb identifier ("tt11280740") and the type the addons use
  // ("movie"/"series"). It comes from art/ids.txt, resolved by Cinemeta —
  // without it there is no way to ask any addon for sources.
  // How much of the title the owner has watched, 0..100. It comes from the web
  // app (the watchProgressItems key), the fourth column of extra.txt. 0 = not started.
  int  progress;
  // The card's caption in "Continue Watching". A series shows "S1, E8 · 16 min";
  // a film shows only the time remaining. Season/episode stay at 0 on a film,
  // and that is what separates the two cases while drawing.
  int  season, episode;
  int  remainingMin;
  char nameEpisode[120]; // the title of the episode in progress, never the file name
  // The seasons the series has, in order. It comes out of Cinemeta's `videos`
  // field, fetched when the title opens. 0 = not known yet (or it is a film), and
  // the tabs fall back to the fixed 3 that used to be there.
  int  seasons[12];
  int  nSeasons;
  // From Trakt: 1 if it is on the owner's watchlist, 1 if it is in their
  // collection. They live on the item and not in a separate library table
  // because the catalogue is rebuilt from the network — a per-index table would
  // point at a different title after the first refresh.
  int  inList, inCollection;
  char imdb[16];
  char kind[8];
  // Authorship of the social feed, kept separate from the film's metadata.
  char socialName[96], socialSlug[128], socialAvatar[768], socialAction[64];
  // The title's id on TMDB, when the search by imdb_id has already resolved it
  // (see castPhotos in discover.c). It used to be discarded; it is the route to
  // the film's COLLECTION, which TMDB only exposes by its own id.
  long tmdb;
} CatItem;

// One episode of a series. It comes from art/episodes.txt, generated from
// Cinemeta's `videos` field (/meta/series/<id>.json) — the same episodes the
// addons index, so what the screen lists is what a source can be asked for.
typedef struct {
  int  season, episode;
  char name[120];
  char duration[16];    // "38 min"; empty when Cinemeta does not say
  // The date SPELLED OUT, like the web app: "27 January 2023". It uses
  // toLocaleDateString with {month:"long", day:"numeric", year:"numeric"}
  // (metaDetailsScreen.js:1387) — "27/01/2023" was the port's invention. 16 bytes
  // did not fit: "15 November 2024" and its longer siblings overflow.
  //
  // Whoever draws shortens it to the year alone when `showFullReleaseDate` is off
  // (settings_date_full()); the year is the last 4 characters.
  char date[40];
  char synopsis[420];
  char thumb[512];     // the episode's still; empty falls back to the title's art
} CatEp;

// TAKES A BACKDROP OFF TMDB'S `original` AND ONTO w1280, IN PLACE.
//
// Cinemeta's `background` is often a TMDB url at /t/p/original/, which is
// 3840x2160. The download is not the problem (268 KB against w1280's 201 KB) —
// the DECODE is: 8.3 MP become 33 MB in RAM, plus another 33 MB in the format
// conversion, before SDL_BlitScaled reduces it to the cache's 1920 ceiling. On a
// weak core that is ~0.5 s of one of the two decode threads, per piece of art,
// and on the home it lands on every hero change. At w1280 it is 3.7 MB and ~9x
// less work; the hero is drawn at 1920, so it is enlarged 1.5x, and under the
// gradient and the text the difference does not show. The jolt did.
//
// It lives here, and not where the url is parsed, because EVERY filler of a
// CatItem needs it and only one of them had it: discover.c did this inline while
// trakt.c's decorate() — the same Cinemeta /meta endpoint, feeding Continue
// watching and the whole Library, which is the top of the home — did not, and
// paid the half second on every swap.
//
// A url that is not TMDB `original` is left exactly as it is.
void cat_backdrop_shrink(char *url, unsigned size);

// Reads <dir>/catalog.txt. Returns how many items it loaded (0 = none, and the
// caller should carry on with whatever it has).
int  cat_load(const char *dirArt);

// --- ON-DISK CACHE OF THE CATALOGUE ASSEMBLED FROM THE NETWORK ---------------
//
// Measured on the TV: 14.5 s between opening the app and the network catalogue
// being complete, and EVERY opening redid the ~30 requests. The PACKAGE's
// catalogue (catalog.txt) covered that gap with 40 static titles that are not
// the owner's.
//
// Here what discovery assembled is written out as it stands in memory and read
// back on the next opening, before any network. The network still runs on top
// and replaces it when it arrives — the cache is not the truth, it is what to
// show while the truth has not arrived.
//
// A BINARY format and not text: CatItem is POD (only char arrays and integers,
// no pointers), so writing it as a block is correct and saves a serialiser that
// would have to be kept in sync with the struct on every new field. The header
// stores `sizeof(CatItem)` and a version: if the struct changes, the file is
// REFUSED rather than read crooked. Reading rubbish here would be worse than
// having no cache.
int  cat_write_cache(const char *dirArt);
// Returns 1 if it loaded. Call it AFTER cat_load: it replaces the package's
// catalogue when the cache exists and is valid.
int  cat_read_cache(const char *dirArt);
// 1 while what is on screen came from the CACHE, and not from this session's
// network.
//
// Discovery publishes each row as soon as it arrives, which is right on an empty
// screen and WRONG over the cache: the home would go from 16 rows to 1 and grow
// back again in front of the owner. With the cache up it waits for the complete
// catalogue. With no cache, it publishes in pieces as before.
int  cat_do_cache(void);
void cat_cache_replaced(void);
int  cat_n(void);
const CatItem *cat_item(int i);

// The index of the title with this IMDb id, or -1. The catalogue's id may carry
// an episode ("tt123:2:1"); the comparison stops at the first ':' on both sides.
//
// It exists to open a title from an id that came from OUTSIDE the catalogue —
// an actor's filmography and the "More like this" tab return tt..., and without
// this search there would be no way to know whether that title is one we already
// have metadata for.
// Where progress.txt is written. It became necessary with login: progress is the
// USER's data and cannot live in the package folder, which is the same for
// everyone using the device. Call it after cat_load.
void cat_dir_writing(const char *dir);

int cat_index_by_imdb(const char *imdb);

// Appends a title at the END and returns the index, or -1. For a title that came
// from outside the catalogue (an actor's filmography, "More like this"). See the
// note about the block swap in catalog.c.
int cat_append(const CatItem *item);

// Appends `count` items in a SINGLE block swap and writes the indices into
// `outputIdx` (which may be NULL). Returns how many went in.
//
// Use this, and not cat_append in a loop, whenever there is more than one: that
// one copies the whole catalogue per call, and the search was moving tens of MB
// on the drawing thread on every keypress.
int cat_append_batch(const CatItem *v, int count, int *outputIdx);

// Updates the local mirror of "is on the watchlist". The truth is Trakt, but
// waiting for the next discovery cycle for the button to change would make the
// press look as though it had no effect.
void cat_set_in_list(int i, int inList);

// Records where the owner stopped IN THIS APP. Until now progress was only READ
// (from the web app); without this, watching in the native app changed nothing
// on screen.
//
// It goes to <art>/progress.txt and not to the web app's SQLite: that file
// belongs to another process, which keeps it open and cached — writing to it
// from outside would corrupt its state. Merging the two sources is a separate
// piece of work; for now what this app writes beats what came from there, which
// is right, because it is more recent.
void cat_save_progress(int index_, double posSeg, double durationSeg);
void cat_save_progress_ep(int index_, double posSeg, double durationSeg, int season, int episode);

// Episodes of title `indexItem`. A film returns 0 — which is what the screen uses
// to decide whether to show the episodes section.
// Replaces the whole catalogue with what came from the network. The art paths
// become URLs — tex_cache downloads and stores them on disk by itself.
void cat_set(const CatItem *list, int n);

// --- HOME ROWS ---------------------------------------------------------------
// The web app has no fixed row. Each row is ONE CATALOGUE of ONE addon, and the
// list comes from `homeCatalogPrefs` (per profile), applied in
// `sortAndFilterRowsInternal` (js/ui/screens/home/homeScreen.js:9856):
//
//   1. it merges catalogues and collections into a map indexed by `homeCatalogKey`
//   2. `ensureOrderKeysWithPrefs` returns the saved order with the NEW keys
//      appended at the END — a catalogue that appeared later goes last
//   3. it drops the disabled ones, checking TWO keys: `homeCatalogDisableKey`
//      (<baseUrl>_<type>_<catalogId>_<name>) and `homeCatalogKey`
//   4. it applies `customTitles[homeCatalogKey]` over the catalogue's name
//   5. collections with `pinToTop` go first and are never cut
//   6. it truncates the total at `getHomeRowLimit()`
//
// The web uses 16 here (`HOME_MAX_ROWS_LEGACY_TV` in homeConstants.js, the
// branch `isLegacyTvRuntime()` picks; the 40 of `HOME_MAX_ROWS_DEFAULT` is for
#define CAT_FILTER_MAX 24

typedef struct {
  char key[192];   // homeCatalogKey: <addonId>_<type>_<catalogId>
  char title[96];   // already formatted, with the type suffix
  char kind[8];      // "movie" | "series"
  // WHERE THE ROW CAME FROM. The key above identifies the catalogue but is no use
  // for CALLING it again: it carries the ADDON's id, not its address.
  // Without these two there is no way to ask for the continuation of the list,
  // which is what the "See all" screen does — it calls the same catalogue with `skip`.
  // 600 and not 300: Xperience embeds a JWT in the PATH and its base is 367
  // characters long. At 300 it was silently truncated, the URL assembled here
  // became something else and the catalogue answered with no `metas` — the "See
  // all" screen opened empty with no error at all. addons.c already uses 600 for
  // the same reason.
  char base[600];
  char catId[96];
  // A window into the item array. The rows have NO array of their own: they point
  // into the single catalogue, which is what the library and the search sweep.
  // Duplicating the items per row would cost ~3.5 KB per repeated title.
  int  start, n;
} CatRow;

int cat_n_rows(void);
const CatRow *cat_row(int r);   // NULL outside the range

// Swaps items AND rows at once. It has to be a single call: with two, the
// drawing thread catches a frame with the new rows pointing at the old items,
// and the (start,n) window falls outside the array.
void cat_set_all(const CatItem *list, int count,
                      const CatRow *filters, int nFilters);


// Replaces the episodes of ONE title. Called when the detail screen opens.
void cat_set_episodes(int indexItem, const CatEp *list, int n);

// Replaces ONE item, preserving the rest. Used when the detail screen opens and
// brings cast, directing and seasons the row's catalogue did not have.
void cat_update_item(int index_, const CatItem *new);

// Titles similar to the one at `index`: the same type (film/series) and at least
// one genre in common, highest-scoring first. Returns how many it wrote.
//
// There is no "similar" endpoint in the addon protocol — Cinemeta has none and
// Xperience only offers "More Like X" for the owner's recent titles, not for an
// arbitrary one. Crossing genres inside the catalogue that is already loaded
// answers instantly, with no network, and is right often enough for the row to
// be worth having.
int           cat_similar(int index_, int *output, int max);

int           cat_n_episodes(int indexItem);
const CatEp  *cat_episode(int indexItem, int i);   // circular index; NULL if the catalogue is empty

#endif
