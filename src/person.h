// The title's CAST and each cast member's profile and filmography.
//
// Two things live here and both come from TMDB:
//
//   the CAST LIST — the whole credited cast of the open title. The catalogue keeps
//     six faces per title (CatItem.cast[6], multiplied across every title it
//     holds), and the cast page lists everyone, so the page asks for the rest
//     when it opens. A series asks for /tv/<id>/aggregate_credits, which is also
//     where the per-actor episode count comes from.
//
//   the PERSON — /person/<id>?append_to_response=combined_credits, the web
//     app's `openCastDetail` (metaDetailsScreen.js:6165). The cast page shows it
//     beside the list for whichever face is focused, so walking the list asks for
//     a new person on every step: the answers are kept in a small cache, and
//     walking back up the list costs no network.
#ifndef NV_PERSON_H
#define NV_PERSON_H

#define PES_MAX  24
#define CAST_MAX 40

// --- the cast list ------------------------------------------------------------
// Requests the cast of the TMDB title `titleTmdb`. Does not block. Repeating with
// the same title does not refetch.
void cast_request(long titleTmdb, int series);
// The members, once the list for `titleTmdb` has arrived; 0 before that (and for
// any other title), so the caller falls back to the catalogue's six.
int  cast_n(long titleTmdb);
const char *cast_name(int i);
const char *cast_role(int i);
const char *cast_photo(int i);
long cast_person(int i);
// How many episodes they are credited in, on a series; 0 on a film.
int  cast_episodes(int i);

// --- the person ---------------------------------------------------------------
// Makes `tmdbId` the current person and fetches them unless already cached. Does
// not block.
void person_request(long tmdbId);

// 1 once the profile for `tmdbId` is in and it is the current person. The
// getters below read the current person and answer empty before that.
int  person_ready(long tmdbId);
const char *person_photo(void);
const char *person_bio(void);
// "Born 1974 in Oslo, Norway" — the fallback for a missing biography.
const char *person_born(void);

// Filmography, already sorted by popularity (the same order as the web).
int  person_n_credits(void);
const char *person_credit_title(int i);
const char *person_credit_role(int i);
const char *person_credit_year(int i);
const char *person_credit_poster(int i);
// The title's IMDb id, when TMDB provides one; empty when it does not.
const char *person_credit_imdb(int i);
// The title's TMDB id and "movie"/"tv". A combined_credits credit does NOT
// carry imdb_id; these two are how you reach the id Cinemeta understands.
long person_credit_tmdb(int i);
const char *person_credit_kind(int i);

#endif
