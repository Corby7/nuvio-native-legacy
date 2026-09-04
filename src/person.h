// The profile of a cast MEMBER and their filmography.
//
// This is the web app's `openCastDetail` (metaDetailsScreen.js:6165), which
// opens the castDetailScreen with TMDB's
// /person/<id>?append_to_response=combined_credits. The port showed the cast
// and stopped there: clicking a face led nowhere.
#ifndef NV_PERSON_H
#define NV_PERSON_H

#define PES_MAX 24

// Requests the profile for `tmdbId`. Does not block. Repeating with the same id does not refetch.
void person_request(long tmdbId, const char *nameKnown, const char *photo);

// 1 once there is something to draw.
int  person_ready(void);
const char *person_name(void);
const char *person_photo(void);
const char *person_bio(void);
// "Atuacao", "Direcao"... o known_for_department do TMDB, traduzido.
const char *person_area(void);

// Filmography, already sorted by popularity (the same order as the web).
int  person_n_credits(void);
const char *person_credit_title(int i);
const char *person_credit_role(int i);
const char *person_credit_year(int i);
const char *person_credit_poster(int i);
// The title's IMDb id, when TMDB provides one; empty when it does not. Without
// it there is no way to open the title page, so the card is information only.
const char *person_credit_imdb(int i);
// The title's TMDB id and "movie"/"tv". A combined_credits credit does NOT
// carry imdb_id; these two are how you reach the id Cinemeta understands.
long person_credit_tmdb(int i);
const char *person_credit_kind(int i);

#endif
