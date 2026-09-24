// TRAILERS THAT PLAY INSIDE THE APP, from IMDb.
//
// TMDB only lists YouTube trailers, and YouTube cannot be played here: its
// streams are ciphered, need proof-of-origin tokens, and above 360p come as
// separate audio and video. IMDb hosts its own trailers as plain MP4 (H.264
// High + AAC stereo, 1080p on recent titles, 480p on old ones), which
// video_play takes as they are. MEASURED 2026-09-24 over 18 titles, films and
// series, from the catalogue and outside it: every one had at least one.
//
// One POST to IMDb's GraphQL API per title (api.graphql.imdb.com). It needs no
// key, but it DOES need the `x-imdb-client-name` header — without it the answer
// is a 403. It is not a published API, so it can change without notice; when
// it returns nothing the title page falls back to the YouTube list (extras.h).
//
// THE PLAYBACK URLS ARE SIGNED AND LAST 24 HOURS. They are fetched each time a
// title page opens and never cached to disk.
//
// Independent of Trakt, unlike extras.c, whose whole fetch is gated on a Trakt
// session — and independent of the title's type: series have trailers too.
#ifndef NV_TRAILERS_H
#define NV_TRAILERS_H

#define TR_MAX 6

// Asks for `imdb`'s trailers ("tt1375666"; an episode suffix ":2:1" is
// dropped). The previous title's list is cleared at once. Asking again for the
// title already loaded or in flight does nothing.
void trailers_request(const char *imdb);

// How many arrived, 0 while in flight or when IMDb has none. Index 0 is the
// MAIN trailer — the one the hero button plays: the first "Official Trailer"
// of a minute or more, else the first trailer of a minute or more, else the
// first. IMDb's own first entry is often a 30-second spot ("Final Trailer" on
// Dune: Part Two), which is why it is not simply taken.
int         trailers_n(void);
const char *trailers_name(int i);    // "Official Trailer"
const char *trailers_kind(int i);    // "Trailer" or "Teaser"
int         trailers_seconds(int i); // 0 when IMDb does not say
const char *trailers_thumb(int i);   // a 520-wide JPEG
const char *trailers_url(int i);     // the best MP4
const char *trailers_title(void);    // the imdb id the list belongs to

#endif
