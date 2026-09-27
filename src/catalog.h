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
// How many seasons of one series the item can carry. It is a NAMED constant
// because the number has to agree in two files — this struct's array and the loop
// in discover.c that fills it — and when it was a bare 12 in both, the guard was
// the only thing standing between the loop and the end of the array. See the note
// on `seasons` below for what it cost.
#define CAT_MAX_SEASONS 40

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
  // WHEN this title was last watched, in ms since the epoch. 0 = not known.
  //
  // It travels ON THE ITEM and not in an array indexed by position, and that is
  // the whole point: trakt_resume COMPACTS the batch after decorating, dropping
  // whatever Cinemeta does not know, so a parallel array desynchronises exactly
  // there — in silence, with a wrongly ordered row as the only symptom.
  //
  // Two producers stamp it: the Trakt path from `paused_at` in /sync/playback,
  // and the local path from the last column of progress.txt. It is what lets
  // "Continue watching" merge both sources and order them by recency instead of
  // by whichever answered first.
  long long resumedMs;
  // The card's caption in "Continue Watching". A series shows "S1, E8 · 16 min";
  // a film shows only the time remaining. Season/episode stay at 0 on a film,
  // and that is what separates the two cases while drawing.
  int  season, episode;
  int  remainingMin;
  char nameEpisode[120]; // the title of the episode in progress, never the file name
  // THE EPISODE'S OWN STILL, when Cinemeta has one. The Continue watching card
  // used to draw `backdrop`, which is the SERIES' `background` — the very image
  // the hero above it is already showing. The row came out as the hero repeated
  // four times over, and nothing on the card said which episode it resumes.
  //
  // Empty is the normal state for a film, and for a series Cinemeta has no still
  // for; whoever draws falls back to `backdrop`.
  char thumbEp[512];
  // The seasons the series has, in order. It comes out of Cinemeta's `videos`
  // field, fetched when the title opens. 0 = not known yet (or it is a film), and
  // the tabs fall back to the fixed 3 that used to be there.
  //
  // IT WAS 12, and the 12 was written twice — here and as a bare literal in the
  // loop in discover.c that fills it. South Park is 29 seasons in Cinemeta and the
  // picker stopped at 12, having silently dropped 13-29. The array does not
  // overflow when that happens and nothing is logged; the series simply looks
  // shorter than it is, which is why it went unnoticed.
  //
  // 40 covers the realistic worst case (The Simpsons, 36). It is not free — the
  // catalogue holds CAT_MAX of these — but at 28 extra ints it is ~224 KB across
  // the whole array, against a CatItem that is already several KB on its own.
  //
  // WHOEVER CHANGES IT must check N_ITEMS in detail.c: nSeasonsOf() clamps the
  // picker to that, so a number larger than N_ITEMS is truncated again one layer
  // further down, with the same silence.
  int  seasons[CAT_MAX_SEASONS];
  int  nSeasons;
  // From Trakt: 1 if it is on the owner's watchlist, 1 if it is in their
  // collection. They live on the item and not in a separate library table
  // because the catalogue is rebuilt from the network — a per-index table would
  // point at a different title after the first refresh.
  int  inList, inCollection;
  // When the title went onto that list, in ms since the epoch: Trakt's
  // `listed_at` (watchlist) or `collected_at` / `last_collected_at` (collection).
  // 0 for anything that did not come from a Trakt list. The Library sorts by it.
  long long added;
  // The release year from Trakt's own record, 0 when unknown. `meta` carries the
  // year for catalogue items, but a Trakt list item arrives with `meta` empty —
  // which left the Library's year sort with nothing to sort by.
  int  year;
  // 24 AND NOT 16. The id of an item in "Continue watching" is COMPOSITE —
  // resumeLocal and the next-up resolver in discover.c write "<work>:<season>:
  // <episode>" here — and 16 bytes fit that only while the pieces stay short.
  // "tt26545992:12:14" is exactly 16 characters, so it was truncated to
  // "tt26545992:12:1" with no warning: the id then named a different episode,
  // the file dedupe stopped matching it, and the row could resume the wrong
  // thing. Ten-digit ids with two-digit seasons are ordinary on long-running
  // series, so this was reachable, not theoretical.
  //
  // The catalogue holds CAT_MAX of these, so the 8 bytes cost ~16 KB in the
  // worst case — against a CatItem already several KB wide. The on-disk cache
  // stores sizeof(CatItem) in its header and REFUSES a file whose struct does
  // not match, so an old cache is discarded rather than read crooked.
  char imdb[24];
  char kind[8];
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
  // The episode's IMDb score in TENTHS (85 = 8.5); 0 = none. It is the field the web
  // app puts on the card (`episode.imdbRating`, metaDetailsScreen.js:496), and it comes
  // from the SAME Cinemeta `videos` entry as everything else here — so it costs no
  // extra request.
  //
  // IT IS OFTEN 0, and that is Cinemeta and not a parse fault: every Fallout and
  // Gentlemen episode answers `"rating": "0"`. The web renders that as a literal
  // "IMDb 0.0" on every card, because its guard is `rating != null` and "0" is not
  // null. This port draws the badge only when the number means something — see the
  // note in detail.c.
  int  imdb;
} CatEp;

// TAKES A BACKDROP OFF TMDB'S `original` AND ONTO A GIVEN RUNG, IN PLACE.
//
// Cinemeta's `background` is often a TMDB url at /t/p/original/, which is
// 3840x2160. The download is not the problem — the DECODE is: 8.3 MP become 33 MB
// in RAM, plus another 33 MB in the format conversion, before the box filter
// reduces it to the cache's 1920 ceiling. On a weak core that is ~0.5 s of one of
// the two decode threads, per piece of art, and on the home it lands on every hero
// change.
//
// `width` IS THE RUNG, AND THE CALLER PICKS IT BY WHAT IT DRAWS AT. This used to be
// a fixed w1280 for everyone, which was wrong in both directions:
//
//   - THE HERO IS DRAWN AT 1920, so w1280 was enlarged 1.5x. The note that used to
//     be here said the gradient and the text hide it. They hide it on a hero that
//     is mostly gradient; they do not hide it on one that is mostly picture. And
//     the cost of being right was misjudged, because w1280 was taken as the top of
//     the ladder: /configuration lists w300/w780/w1280/original, but the CDN also
//     serves w1920, at exactly 1920x1080. MEASURED, same backdrop path:
//         w780  780x439   116 KB     w1920  1920x1080   550 KB
//         w1280 1280x720  281 KB     orig   3840x2160  1580 KB
//     w1920 is the only rung that MEETS the ceiling: the decode lands at 1920 and
//     `conv->w > limit` is false, so there is no resampling pass at all and no
//     enlargement on screen. It costs 8.3 MB of texture against w1280's 3.7 MB.
//     That is affordable, and not as a guess: TVDB's fanart/original and metahub's
//     background/medium ARE 1920x1080 and match nothing here, so they have been
//     going through the cache untouched, at that exact cost, the whole time.
//     `original` remains wrong — 4x the decode to reach the same 1920 texture,
//     since the box filter would throw the rest away.
//
//   - THE EPISODE STILL IS DRAWN AT 640 (NV_DETP_EP_W), so w1280 was decoded at
//     twice the width and box-filtered straight back down. w780 is the smallest
//     rung that still covers it.
//
// It lives here, and not where the url is parsed, because EVERY filler of a
// CatItem needs it and only one of them had it: discover.c did this inline while
// trakt.c's decorate() — the same Cinemeta /meta endpoint, feeding Continue
// watching and the whole Library, which is the top of the home — did not, and
// paid the half second on every swap.
//
// A url that is not TMDB `original` is left exactly as it is.
#define CAT_BACKDROP_HERO_W 1920
#define CAT_BACKDROP_THUMB_W 780
void cat_backdrop_shrink(char *url, unsigned size, unsigned width);

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

// The same, with the instant SUPPLIED rather than taken from the clock.
//
// It exists for the sync. `cat_save_progress_ep` stamps time(NULL) on every line
// it writes, which is right for playback that has just happened here and wrong
// for a record the account is replaying back at this device: the pull applies a
// dozen rows in one pass, every one of them claimed "just now", and the whole
// order of "Continue watching" — which is a sort by this very column — collapsed
// into a twelve-way tie broken by qsort. Measured on the owner's TV: thirteen of
// fourteen lines carried the identical stamp 1789764663000, so the row could not
// put the series watched an hour ago in front of one watched last month.
//
// `whenMs` of 0 means "the instant is not known", and the clock answers for it.
// `origin` says WHO the record belongs to — 1 playback on this device, 2 a row
// replayed from the account — and goes into the file's seventh column; see
// CatProgress.origin for the conflict it exists to settle. The two wrappers
// above pass 0 and 1: they are only ever called for playback here.
void cat_save_progress_at(int index_, double posSeg, double durationSeg,
                          int season, int episode, long long whenMs, int origin);

// Only the progress.txt line, by id, with no catalogue item to update. For a
// synced row whose title is not loaded right now: "Continue watching" is built
// from this file, so a title missing from the catalogue still needs its line —
// skipping it froze whatever stale line was there, and a stale line that falls
// outside the row's limits is exactly what keeps the title out of the catalogue.
// Returns 1 when the line was written.
int cat_save_progress_id(const char *imdb, double posSeg, double durationSeg,
                         int season, int episode, long long whenMs, int origin);

// ONE LINE of progress.txt, as it was RECORDED — not as it was applied to the
// catalogue.
//
// The difference matters. Applying progress walks the catalogue and drops any id
// it cannot find (cat_index_by_imdb), which is right for painting a card that is
// on screen and wrong for the question "what was this person watching?": what
// the phone sent for a title this TV's catalogue never loaded is real history,
// and it used to be thrown away without a word. "Continue watching" asks the
// second question, so it reads the records.
typedef struct {
  char   imdb[24];        // the WORK's id, episode suffix stripped
  double posSeg, durationSeg;
  int    season, episode; // 0/0 on a film
  // 0 on a line written before this column existed. Those lines still work
  // everywhere; they simply order after the ones that know their instant.
  long long lastWatchedMs;
  // WHO WROTE THE LINE: 1 playback on this device, 2 a record replayed from the
  // account, 0 a line from before this column existed.
  //
  // The sync needs it to decide a conflict honestly. "Do not let the account
  // overwrite something newer" is only a sensible rule about playback that
  // happened HERE — between two copies of the account's own record the account
  // is simply right, and the instant a previous build stamped on its copy is not
  // evidence of anything: it recorded the moment of the SYNC, not the moment of
  // the watching. Without this column the TV's own corrupted stamps (all of them
  // "now", see cat_save_progress_at) would outrank every true instant the
  // account holds and the order could never recover. With it, a legacy line
  // yields, the account's real instants come back, and the file heals itself on
  // the first pull.
  int origin;
} CatProgress;

#define CAT_PROGRESS_MAX 64

// Drops every progress.txt line for the WORK `imdb` names (an episode suffix is
// ignored) and zeroes the progress of the matching items in memory. The first
// half of "Remove from Continue watching"; the account and Trakt are cwremove.c's.
void cat_progress_remove(const char *imdb);

// Hands a line this device pushed over to the account: origin 1 -> 2, for the
// line of `imdb` whose instant is `ms`. From then on the account's copy is the
// record, so a delete made on another device is followed here instead of being
// undone by the next push. A line rewritten since (newer playback) is left alone.
void cat_progress_mark_synced(const char *imdb, long long ms);

// "REMOVED FROM CONTINUE WATCHING", remembered with the instant it happened
// (cw-removed.txt in the writing folder). Deleting the resume points is not
// enough on its own: a series also comes back as a next-up card from its watch
// history, and a remote delete that fails would bring the entry back on the next
// build. The web app keeps the same list (ContinueWatchingPreferences.addRemovedKey).
//
// cat_cw_dismissed answers 1 while the work was last seen at or before its
// removal — so watching it again brings it back, as it should. Thread-safe: the
// discovery thread asks, the UI thread records.
void cat_cw_dismiss(const char *imdb);
int  cat_cw_dismissed(const char *imdb, long long whenMs);

// Reads progress.txt into `out`, MOST RECENT FIRST, and returns how many. A
// missing or unreadable file is 0 and not an error: a fresh install has no
// history.
int cat_progress_read(CatProgress *out, int max);

// The percentage a recorded position REPORTS, and the one place that decides it.
//
// It used to be an inline `(int)(100.0 * pos / duration)` at each of the three
// sites that apply progress.txt, and the truncation made "started" mean something
// different for every runtime: 1% of a 110-minute film is 66 seconds, 1% of a
// 22-minute episode is 13, so a film someone had genuinely sat down to reported 0
// and looked untouched while a mis-tapped episode reported 1 and looked started.
//
// A position that is recorded at all is a position: the web app's rule is
// `positionMs > 0` (hasWatchProgressStarted in domain/model/watchProgress.js) and
// nothing about the runtime. So anything above zero reports at least 1, which is
// what keeps every `progress > 0` test in this port — the Resume button, the
// player's resume point, the hero's "N MINUTES LEFT" — agreeing with the app they
// were ported from. The card's bar is NOT affected: it opens at
// NV_CW_BAR_MIN_PCT (2), deliberately, so a title a few seconds in still draws no
// stub. See the note in resume.c.
int cat_pct(double posSeg, double durationSeg);

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


// Grows ONE home row by `count` items, which land at the end of its window and
// push everything after them along. It is how a row keeps going when the focus
// reaches its last poster — see the long note on the implementation. Returns how
// many it took; less than asked (or 0) means the row can grow no further.
int cat_row_grow(int r, const CatItem *v, int count);

// The opposite of cat_row_grow: takes every card of `imdb`'s work out of row
// `r`, moving the windows after it. ON THE DRAWING THREAD, like cat_row_grow.
// It lets a card removed from "Continue watching" leave at once instead of
// waiting for a full rebuild (~20 s on the C3). Returns how many cards went.
int cat_row_drop(int r, const char *imdb);

// Replaces the episodes of ONE title. Called when the detail screen opens.
void cat_set_episodes(int indexItem, const CatEp *list, int n);
// Fills ONE episode's IMDb score (tenths) in place, matched on season+episode.
// It exists because the score arrives from a DIFFERENT request than the episodes
// themselves — the ratings API is called after the row is already on screen — and
// republishing the whole list to add a number would restart the thumbnails.
// Returns 1 when an episode matched, so the caller can log what LANDED rather
// than what it parsed.
int  cat_set_ep_score(int indexItem, int season, int episode, int tenths);
// The same for ONE episode's runtime, written as "62 min" into `duration` when it is
// empty. Cinemeta's `videos` carry no runtime at all, so without this the episode
// list had no length for any episode; the ratings API that brings the scores has it.
int  cat_set_ep_runtime(int indexItem, int season, int episode, int minutes);

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
