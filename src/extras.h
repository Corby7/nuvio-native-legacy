// What the TABS on the title screen show beyond the cast: the Trakt score,
// Trakt comments and related titles.
//
// All of this already exists in the web app (renderExternalRatingsRow, the
// comments section and the "More like this" tab), and none of the three had a
// source in the port — which is why those tabs fell back to "No information".
// Every request talks to api.trakt.tv by IMDb id, with no identifier
// translation in between: Trakt resolves "tt1234567" directly, and TMDB does not.
#ifndef NV_EXTRAS_H
#define NV_EXTRAS_H

#define EX_COMMENT_MAX 8
#define EX_REL_MAX   12

// Asks for everything about a title. Does not block: it fires a thread.
// Repeating with the same `imdb` does not repeat the request. `series` decides
// between /shows and /movies. `tmdbId` is the title's id on TMDB (0 when
// unknown). It serves only the COLLECTION, which TMDB exposes by its own id
// alone — there is no path via IMDb.
void extras_request(const char *imdb, int series, long tmdbId);

// The Trakt score in 0..100 (0 = it has not arrived or does not exist) and how
// many voted. The web app shows the same score mdbList returns for "trakt".
int  extras_score_trakt(void);
int  extras_votes_trakt(void);

// SCORE SOURCES, in the order the web app lists them (renderExternalRatingsRow,
// metaDetailsScreen.js:3410). All but IMDb and Trakt come from mdbList, which
// needs the owner's key in art/mdblist.txt; without the file they stay at 0 and
// the row shows only the two we have on our own.
typedef enum {
  EX_TRAKT, EX_IMDB, EX_TMDB, EX_TOMATOES, EX_AUDIENCE, EX_METACRITIC,
  EX_LETTERBOXD, EX_NSOURCES
} ExSource;

// Reads art/mdblist.txt. Without it the module works with Trakt and IMDb only.
void extras_load(const char *dirArt);

// The mdblist key coming from the ACCOUNT. Same reason as TMDB: as long as it
// comes from art/mdblist.txt (which is even mode 0600), the package distributes
// the key of whoever built it.
void extras_set_key(const char *key);

// Tell this module that a key it fetches with has changed. extras.c refuses to
// fetch the same title twice, and the TMDB half of a fetch is silently skipped when
// disc_key_tmdb() is still empty — so a title opened before the account answered
// kept its missing status/runtime/release/countries for the rest of the session.
// Call it whenever a credential lands; the next request for that title goes out
// again. See the note on keyGen in extras.c.
void extras_keys_changed(void);

// 1 when nothing is in flight: whatever this module is going to say about the current
// title, it has already said. 0 while the fetch thread is running.
//
// The title screen uses it to know whether a group that is MISSING is missing because
// the title has no such value, or because the answer has not landed yet — which is the
// difference between drawing nothing and holding a place for it.
int extras_settled(void);


// The source's score, RAW multiplied by 10 (imdb comes with one decimal place
// and has to fit in an integer). 0 = there is none. Divide by 10 and use
// extras_source_percentual() to know whether the result is "6.2" or "66%".
int  extras_score(int source);
int  extras_source_percentual(int source);
const char *extras_source_brand(int source);
// The ABSOLUTE path of the brand file. See the note in extras.c: a relative one
// does not work because the app's working directory is not the art folder.
const char *extras_path_brand(int source);
// A brand by FILE NAME, for those that are not a score source (the Trakt
// wordmark, "trakt_wordmark").
const char *extras_path_brand_name(const char *name);

// Comments: the most-liked first, which is the order of `comments/likes`.
int  extras_n_comments(void);
const char *extras_comment_user(int i);
const char *extras_comment_text(int i);
int  extras_comment_likes(int i);
// The COMMENTER's score (Trakt's user_rating), 0..10; 0 when they did not rate.
// The reference shows "10/10  17 likes" in the card's footer.
int  extras_comment_score(int i);

// EPISODE COMMENTS, for the "Series | Episode" selector the reference shows
// above the cards.
//
// They are a SEPARATE Trakt query
// (/shows/<id>/seasons/<t>/episodes/<e>/comments/likes), not a slice of the
// series list — the two sets do not overlap. Made ON DEMAND: one round trip per
// episode, and only when the owner picks the "Episode" tab.
//
// `imdbSeries` accepts both "tt1234567" and the "tt1234567:2:4" the episode
// list uses; the part after the first ':' is ignored.
void extras_request_comments_ep(const char *imdbSeries, int season, int episode);
int  extras_n_comments_ep(void);
// 1 while the query is in flight. Whoever draws uses this to show "loading"
// instead of "no comments" — both states are the empty list, and confusing them
// makes the episode look as though it has no comments at all.
int  extras_comments_ep_loading(void);
const char *extras_comment_ep_user(int i);
const char *extras_comment_ep_text(int i);
int  extras_comment_ep_likes(int i);
int  extras_comment_ep_score(int i);

// PER-EPISODE SCORES, for the panel the web app shows on a SERIES in place of
// the cards (renderSeriesRatingsPanel, metaDetailsScreen.js:3843): a row of
// seasons and a grid of "E<n> / score" pills, coloured by band.
//
// It comes from ONE call: /shows/<id>/seasons?extended=episodes,full returns
// every season with each episode's score alongside. Asking episode by episode
// would be dozens of calls to draw one tab.
#define EX_TEMP_MAX 12
#define EX_EP_MAX   30
int  extras_n_seasons(void);
int  extras_season_number(int t);
int  extras_n_eps(int t);
int  extras_ep_number(int t, int i);
// The episode's score in TENTHS (72 = 7.2); 0 = no score.
int  extras_ep_score(int t, int i);

// The film's COLLECTION (franchise), for the tab the web app names after it.
// It comes from /movie/<id> -> belongs_to_collection -> /collection/<id>. The
// parts carry only the TMDB id, so opening one goes down the same path as an
// actor's credit (disc_request_title_tmdb).
#define EX_COL_MAX 12
const char *extras_collection_name(void);
int  extras_n_collection(void);
const char *extras_collection_title(int i);
const char *extras_collection_year(int i);
long extras_collection_tmdb(int i);

// EPISODES ALREADY WATCHED, from Trakt (/shows/<id>/progress/watched). The
// episode card gains a mask and a tick once the owner has seen it. Without
// this, someone following a series had no way to see where they stopped just
// by looking at the list.
int  extras_ep_watched(int season, int episode);
// 1 only after this work's history has arrived. With no answer, do not infer
// that every episode is unwatched.
int extras_progress_ready(void);
int extras_next_episode(int *season, int *episode);

// The film's FACT SHEET, for the "Film Details" section.
//
// All of it comes out of the SAME /movie/<id> call the collection already made
// — the body has carried status, runtime, release_date and the countries all
// along, and the parse read only belongs_to_collection and threw the rest away.
// With `append_to_response=release_dates,videos` the same round trip also
// brings the age rating and the trailers. No new network request.
//
// Empty ("" or 0) when it has not arrived or TMDB does not have the field.
// Whoever draws must OMIT the line in that case, never write a fallback value:
// see the note about the hard-coded age rating in discover.c.
const char *extras_profile_status(void);          // "Released", "Post Production"
int         extras_profile_duration(void);         // minutes; 0 = none
const char *extras_profile_countries(void);          // "United States of America, Canada"
const char *extras_profile_age_rating(void);   // "R", "PG-13", "14"
const char *extras_profile_release(void);      // "2026-01-15"

// TRAILERS. Only what can be shown: the YouTube id, the name and a thumbnail.
//
// THERE IS NO WAY TO PLAY THEM. The web app opens a YouTube iframe; this port
// has neither a player nor a stream extractor, and the decision already
// recorded in detail.c and gfx.c was to remove the trailer button rather than
// leave a control that promises what it cannot deliver. The same rule holds
// here: the card takes part in the composition, but takes no focus while
#define EX_TRAILER_MAX 6
int         extras_n_trailers(void);
const char *extras_trailer_yt(int i);        // the video id ("dQw4w9WgXcQ")
const char *extras_trailer_name(int i);      // "Official Trailer"
// The thumbnail's URL. Predictable from the id, with no API call — it is the
// same path the web app uses (metaDetailsScreen.js:5728). Pass it straight to
// tex_get: the texture cache downloads and stores any URL on its own.
const char *extras_trailer_thumb(int i);

// Related titles, for the "More like this" tab.
int  extras_n_related(void);
const char *extras_related_title(int i);
const char *extras_related_year(int i);
const char *extras_related_imdb(int i);
// The related title's poster (URL). It comes from Trakt's `extended=images`,
// which returns the path with no scheme — the https is added here.
const char *extras_related_poster(int i);

#endif
