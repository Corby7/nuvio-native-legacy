#include "extras.h"
#include "trakt.h"
#include "net.h"
#include "js.h"
#include "discover.h"
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static int  scoreTrakt, votesTrakt;
static int  scores[EX_NSOURCES];
static char mdbKey[80];
static char dirArtEx[512];

// The provider's name in the mdbList API AND the brand file's name in
// art/brands. The order is the enum's, which is the web app's renderExternalRatingsRow order.
static const char *SOURCE[EX_NSOURCES] = {
  "trakt", "imdb", "tmdb", "tomatoes", "audience", "metacritic", "letterboxd"
};

// THE SCALE VARIES BY PROVIDER, and not in the way it looks. CHECKED against
// the API with tt9737326: imdb=6.2 (0..10), but trakt=66 and tmdb=70 — both as
// PERCENTAGES, alongside tomatoes=59, audience=46 and metacritic=53. I had
// assumed trakt and tmdb came in 0..10 like imdb, and the result was "10.0" for
// both (66 x 10 overflowed the ceiling and stuck at the maximum).
//
// We store the RAW value multiplied by 10, so imdb fits in an integer without
// losing its decimal place; whoever draws divides again according to the provider.
//
// DIVERGENCE NOTED: the web app rescales nothing — formatMdbListRating
// (metaDetailsScreen.js:732) prints the number as it came, so there TMDB shows
// up as "70.0" beside an IMDb "6.2". Here TMDB becomes "70%", which is what the
// number actually is.
static int inTenths(double v) {
  int n = (int)(v * 10.0 + 0.5);
  if (n < 0) n = 0;
  if (n > 1000) n = 1000;
  return n;
}

// 1 when the source's score is a PERCENTAGE; 0 when it is a score out of 10.
int extras_source_percentual(int source) {
  return source != EX_IMDB && source != EX_LETTERBOXD;
}

void extras_set_key(const char *key) {
  if (!key || !*key) return;
  snprintf(mdbKey, sizeof mdbKey, "%s", key);
  printf("[extras] mdblist: key from the account\n");
  fflush(stdout);
}

void extras_load(const char *dirArt) {
  char path[600];
  FILE *f;
  snprintf(dirArtEx, sizeof dirArtEx, "%s", dirArt ? dirArt : ".");
  snprintf(path, sizeof path, "%s/mdblist.txt", dirArt ? dirArt : ".");
  f = fopen(path, "r");
  if (!f) { printf("[extras] mdblist missing\n"); fflush(stdout); return; }
  if (fgets(mdbKey, sizeof mdbKey, f)) {
    char *end = mdbKey + strlen(mdbKey);
    while (end > mdbKey && (end[-1] == '\n' || end[-1] == '\r')) *--end = 0;
  }
  fclose(f);
  printf("[extras] mdblist %s\n", mdbKey[0] ? "ok" : "empty"); fflush(stdout);
}

int extras_score(int source) {
  return (source >= 0 && source < EX_NSOURCES) ? scores[source] : 0;
}
const char *extras_source_brand(int source) {
  return (source >= 0 && source < EX_NSOURCES) ? SOURCE[source] : "";
}

const char *extras_path_brand(int source) {
  // ABSOLUTE. The texture cache calls IMG_Load with the path as it came, and
  // the app's working directory is not the art folder — with a relative
  // "brands/x.png" the file was simply not found and the card came out with no
  // logo and no error at all. The catalogue already does it this way (catalog.c:79).
  static char cam[600];
  if (source < 0 || source >= EX_NSOURCES) return "";
  snprintf(cam, sizeof cam, "%s/brands/%s.png", dirArtEx, SOURCE[source]);
  return cam;
}

// The path of a brand that is NOT a score source — the Trakt wordmark, today.
// It exists for the same absolute-path reason as above: a relative path makes
// IMG_Load fail silently, and the drawing vanishes with no error at all.
const char *extras_path_brand_name(const char *name) {
  static char cam[600];
  if (!name || !name[0]) return "";
  snprintf(cam, sizeof cam, "%s/brands/%s.png", dirArtEx, name);
  return cam;
}
// `score` is Trakt's user_rating (0..10); 0 when the commenter did not rate it.
// The reference shows "10/10  17 likes" in the card's footer, and without the
// score the footer had only the like count — half the information.
static struct { char user[40]; char text[420]; int likes; int score; } comment[EX_COMMENT_MAX];

// EPISODE COMMENTS, the other side of the "Series | Episode" selector the
// reference puts above the cards. They are a DIFFERENT query
// (/shows/<id>/seasons/<t>/episodes/<e>/comments/likes), not a filter of the
// series list: Trakt keeps the two separate, and an episode comment never turns
// up in the series list.
//
// It comes on demand — only when the owner picks "Episode" — because the cost
// is one round trip per episode and most visits never change tab.
static struct { char user[40]; char text[420]; int likes; int score; } commentEp[EX_COMMENT_MAX];
static int  nCommentEp;
static int  epTempCurrent, epNumCurrent;    // which episode the list above belongs to
static int  epThreadAlive;
static char epShow[24];
static int  epReqTemp, epReqNum;
static int  nComment;
static struct { char title[120], year[8], imdb[16], poster[200]; } rel[EX_REL_MAX];
// Watched: one bit per episode, up to 40 episodes across 20 seasons. A fixed
// array because the lookup happens while DRAWING each card, every frame — a
// list search there would cost more than the answer.
#define EX_VIS_T 20
#define EX_VIS_E 40
static unsigned char watched[EX_VIS_T][EX_VIS_E];
static int progressReady, nextT, nextE;
static int  nRel;
static struct { int number; int nEps; struct { int ep, score; } eps[EX_EP_MAX]; }
            seasons[EX_TEMP_MAX];
static int  nSeasons;
static char colName[80];
// Fact sheet and trailers: the same /movie/<id> round trip as the collection.
static char profileStatus[32], profileCountries[160], profileCert[12], profileRelease[16];
static int  profileDuration;
static struct { char yt[16], name[80], mini[80]; } trailer[EX_TRAILER_MAX];
static int  nTrailer;
static struct { char title[120], year[8]; long tmdb; } col[EX_COL_MAX];
static int  nCol;
static long tmdbInProgress;

static char idRequest[24], idInProgress[24];
static int  seriesInProgress, seriesRequest, threadAlive;
static long tmdbRequest;
static pthread_t thread;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static void *fetch(void *arg);

static int requestStillCurrent(const char *id) {
  int current;
  pthread_mutex_lock(&lock);
  current = !strcmp(id, idRequest);
  pthread_mutex_unlock(&lock);
  return current;
}

// Finishes a request and, if the user opened another title during the query,
// immediately starts the most recent one. idRequest used to be swapped without
// any new thread being born: the next screen stayed empty indefinitely.
static void finishSearch(const char *id) {
  int resume = 0;
  pthread_mutex_lock(&lock);
  if (strcmp(idRequest, id)) {
    snprintf(idInProgress, sizeof idInProgress, "%s", idRequest);
    seriesInProgress = seriesRequest;
    tmdbInProgress = tmdbRequest;
    resume = 1;
  } else {
    threadAlive = 0;
  }
  pthread_mutex_unlock(&lock);
  if (resume) {
    if (pthread_create(&thread, NULL, fetch, NULL) != 0) {
      pthread_mutex_lock(&lock); threadAlive = 0; pthread_mutex_unlock(&lock);
    } else pthread_detach(thread);
  }
}

// Trakt returns the score as a fraction of 0 to 10 with decimals ("7.83521");
// the rest of the app stores scores as an integer 0..100, like the catalogue.
static int to100(double v) {
  int n = (int)(v * 10.0 + 0.5);
  if (n < 0) n = 0;
  if (n > 100) n = 100;
  return n;
}

// A comment may contain line breaks and escaped quotes; the drawing is a single
// running text box, so everything is swapped for a space. js_text already
// handles the quote escaping and turns \\uXXXX into a space.
static void numaLine(char *s) {
  for (; *s; s++) if (*s == '\n' || *s == '\r' || *s == '\t') *s = ' ';
}

static void *fetch(void *arg) {
  const char *header[4];
  char auth[200], key[140], url[200], id[24];
  const char *kind;
  char *body;
  int series;
  long tmdbId;
  (void)arg;

  pthread_mutex_lock(&lock);
  snprintf(id, sizeof id, "%s", idInProgress);
  series = seriesInProgress;
  tmdbId = tmdbInProgress;
  kind = series ? "shows" : "movies";
  pthread_mutex_unlock(&lock);

  if (!trakt_headers(header, auth, sizeof auth, key, sizeof key)) {
    finishSearch(id);
    return NULL;
  }

  // WHAT HAS ALREADY BEEN WATCHED COMES FIRST.
  //
  // It used to be LAST, after ratings, comments and a
  // `seasons?extended=episodes,full` that brings the whole series — four round
  // trips before the watched mark appeared on the episode card. And it is the
  // most visible piece of data on the screen and the one that decides the
  // primary button's label ("Resume"/"Next"), so it was exactly the last to
  // arrive and the first the owner notices missing.
  // --- episodes already watched (series only) ---
  if (series) {
    snprintf(url, sizeof url,
             "https://api.trakt.tv/shows/%s/progress/watched", id);
    body = net_download_headers(url, 20, header);
    if (body) {
      unsigned char new[EX_VIS_T][EX_VIS_E];
      const char *p = js_array(body, NULL, "seasons");
      memset(new, 0, sizeof new);
      while (p) {
        const char *f = js_end(p);
        int t = (int)js_num(p, f, "number", -1.0);
        if (t >= 0 && t < EX_VIS_T) {
          const char *q = js_array(p, f, "episodes");
          while (q) {
            const char *qf = js_end(q);
            int en = (int)js_num(q, qf, "number", -1.0);
            // "completed" is a boolean; js_num does not read true/false, so the
            // read goes through the text — that is how the first version marked
            // everything as unwatched with no error at all.
            const char *c = strstr(q, "\"completed\"");
            int watched = 0;
            if (c && c < qf) { const char *v = c + 12;
                               while (*v == ' ' || *v == ':') v++;
                               watched = (*v == 't'); }
            if (watched && en > 0 && en < EX_VIS_E) new[t][en] = 1;
            q = js_next(qf);
          }
        }
        p = js_next(f);
      }
      int pt = 0, pe = 0;
      const char *next = strstr(body, "\"next_episode\"");
      if (next && (next = strchr(next, ':'))) {
        next++;
        while (*next == ' ' || *next == '\n' || *next == '\r' || *next == '\t') next++;
        if (*next == '{') {
          const char *end = js_end(next);
          pt = (int)js_num(next, end, "season", 0);
          pe = (int)js_num(next, end, "number", 0);
        }
      }
      int valid = strstr(body, "\"seasons\"") != NULL;
      free(body);
      pthread_mutex_lock(&lock);
      if (!strcmp(id, idRequest) && valid) {
        memcpy(watched, new, sizeof watched);
        nextT = pt; nextE = pe; progressReady = 1;
      }
      pthread_mutex_unlock(&lock);
    }
  }

  // The user has already opened another title. Do not spend several optional
  // round trips on a screen that no longer exists; hand the thread over.
  if (!requestStillCurrent(id)) { finishSearch(id); return NULL; }


  // --- nota ---
  snprintf(url, sizeof url, "https://api.trakt.tv/%s/%s/ratings", kind, id);
  body = net_download_headers(url, 12, header);
  if (body) {
    int n = to100(js_num(body, NULL, "rating", 0.0));
    int v = (int)js_num(body, NULL, "votes", 0.0);
    free(body);
    pthread_mutex_lock(&lock);
    if (!strcmp(id, idRequest)) {
      scoreTrakt = n; votesTrakt = v;
      // Without an mdbList key this is the ONLY Trakt score we will have; with
      // a key, the next step overwrites it with whatever mdbList returns, which
      // is the same source the web app shows.
      // The Trakt score is in 0..100 (the Trakt API returns 0..10). In the array
      // the scale is "raw x 10" and the trakt source is a percentage, so 6.7 ->
      // 67% -> 670. Without this conversion the card showed 6.7% when there was
      // no mdbList.
      if (!scores[EX_TRAKT]) scores[EX_TRAKT] = n * 10;
    }
    pthread_mutex_unlock(&lock);
  }

  // --- mdbList scores, if the owner has a key ---
  if (series && requestStillCurrent(id)) {
    snprintf(url,sizeof url,"https://api.trakt.tv/shows/%s?extended=full",id);
    body=net_download_headers(url,8,header);
    if(body) {
      char state[32]="";
      js_text(body,NULL,"status",state,sizeof state);
      free(body);
      pthread_mutex_lock(&lock);
      if(!strcmp(id,idRequest)) snprintf(profileStatus,sizeof profileStatus,"%s",state);
      pthread_mutex_unlock(&lock);
    }
  }

  //
  // One POST per provider, as the web app does (fetchProviderRating): the API
  // accepts "ids" in bulk but only one provider per call. That is seven short
  // calls; the thread is already our own, so it does not hold up the drawing.
  if (mdbKey[0]) {
    const char *headerJ[3];
    char kj[64];
    char bodyPost[80];
    int k;
    snprintf(kj, sizeof kj, "content-type: application/json");
    headerJ[0] = kj; headerJ[1] = NULL; headerJ[2] = NULL;
    snprintf(bodyPost, sizeof bodyPost,
             "{\"ids\":[\"%s\"],\"provider\":\"imdb\"}", id);
    for (k = 0; k < EX_NSOURCES; k++) {
      char u[300], *rp;
      snprintf(u, sizeof u, "https://api.mdblist.com/rating/%s/%s?apikey=%s",
               series ? "show" : "movie", SOURCE[k], mdbKey);
      rp = net_post(u, 12, headerJ, bodyPost);
      if (!rp) continue;
      { double v = js_num(rp, NULL, "rating", -1.0);
        free(rp);
        if (v >= 0.0) {
          int c = inTenths(v);
          pthread_mutex_lock(&lock);
          if (!strcmp(id, idRequest)) scores[k] = c;
          pthread_mutex_unlock(&lock);
        } }
    }
  }

  // --- comments, the most-liked first ---
  snprintf(url, sizeof url,
           "https://api.trakt.tv/%s/%s/comments/likes?limit=%d", kind, id,
           EX_COMMENT_MAX);
  body = net_download_headers(url, 12, header);
  if (body) {
    struct { char u[40]; char t[420]; int c; int score; } found[EX_COMMENT_MAX];
    int n = 0;
    // p+1 and not js_next: js_next takes the END of the previous element, and
    // here there is no previous one yet. With js_next the first item was skipped
    // and, in a three-item response, rubbish was left over — both lists came back empty.
    const char *p = strchr(body, '[');
    p = p ? p + 1 : NULL;
    while (p && n < EX_COMMENT_MAX) {
      const char *f = js_end(p);
      found[n].u[0] = found[n].t[0] = 0;
      js_text(p, f, "comment", found[n].t, sizeof found[n].t);
      // "username" is inside the `user` object; js_text sweeps the whole range
      // and that is the only occurrence of that key in the item.
      js_text(p, f, "username", found[n].u, sizeof found[n].u);
      found[n].c = (int)js_num(p, f, "likes", 0.0);
      found[n].score = (int)js_num(p, f, "user_rating", 0.0);
      numaLine(found[n].t);
      if (found[n].t[0]) n++;
      p = js_next(f);
    }
    free(body);
    pthread_mutex_lock(&lock);
    if (!strcmp(id, idRequest)) {
      int k;
      for (k = 0; k < n; k++) {
        snprintf(comment[k].user, sizeof comment[k].user, "%s", found[k].u);
        snprintf(comment[k].text, sizeof comment[k].text, "%s", found[k].t);
        comment[k].likes = found[k].c;
        comment[k].score = found[k].score;
      }
      nComment = n;
    }
    pthread_mutex_unlock(&lock);
  }

  // --- per-episode scores, series only ---
  if (series) {
    snprintf(url, sizeof url,
             "https://api.trakt.tv/shows/%s/seasons?extended=episodes,full", id);
    body = net_download_headers(url, 20, header);
    if (body) {
      int nt = 0;
      const char *p = strchr(body, '[');
      p = p ? p + 1 : NULL;
      while (p && nt < EX_TEMP_MAX) {
        const char *f = js_end(p);
        int num = (int)js_num(p, f, "number", -1.0);
        // Season 0 is "specials"; the web app filters on `value > 0`.
        if (num > 0) {
          const char *q = js_array(p, f, "episodes");
          int ne = 0;
          while (q && ne < EX_EP_MAX) {
            const char *qf = js_end(q);
            int en = (int)js_num(q, qf, "number", -1.0);
            double r = js_num(q, qf, "rating", 0.0);
            if (en > 0) {
              seasons[nt].eps[ne].ep = en;
              seasons[nt].eps[ne].score = (int)(r * 10.0 + 0.5);
              ne++;
            }
            q = js_next(qf);
          }
          if (ne > 0) { seasons[nt].number = num; seasons[nt].nEps = ne; nt++; }
        }
        p = js_next(f);
      }
      free(body);
      pthread_mutex_lock(&lock);
      if (!strcmp(id, idRequest)) nSeasons = nt;
      pthread_mutex_unlock(&lock);
    }
  }

  // --- collection (film only, and only once we know the TMDB id) ---
  if (!series) {
    const char *key = disc_key_tmdb();
    long idCol = 0, idMovie = tmdbId;
    char name[80] = "";
    // The TMDB id only lands in the catalogue AFTER the cast enrichment; on the
    // FIRST opening of a title it is still 0, and the tab would not appear on
    // precisely the visit the owner is looking at. /find resolves it on the spot.
    if (key && key[0] && idMovie <= 0) {
      snprintf(url, sizeof url,
               "https://api.themoviedb.org/3/find/%s?api_key=%s"
               "&external_source=imdb_id", id, key);
      body = net_download(url, 15);
      if (body) {
        const char *v = js_array(body, NULL, "movie_results");
        if (v) idMovie = (long)js_num(v, js_end(v), "id", 0.0);
        free(body);
      }
    }
    if (key && key[0] && idMovie > 0) {
      // `append_to_response` makes TMDB return release_dates and videos INSIDE
      // this same body. This call already happened before and the parse read
      // only belongs_to_collection: status, runtime, release_date and the
      // countries arrived and were thrown away. Now the whole fact sheet and
      // the trailers come from here, with no extra round trip.
      snprintf(url, sizeof url,
               "%s/movie/%ld?api_key=%s&language=en-US"
               "&append_to_response=release_dates,videos",
               "https://api.themoviedb.org/3", idMovie, key);
      body = net_download(url, 15);
      if (body) {
        // The fact sheet below writes several globals. It holds the same lock
        // extras_request uses so that a change of title cannot clear the fields
        // mid-parse and then receive half of the old sheet.
        pthread_mutex_lock(&lock);
        if (strcmp(id, idRequest)) {
          pthread_mutex_unlock(&lock);
          free(body);
          finishSearch(id);
          return NULL;
        }
        const char *endC = body + strlen(body);
        const char *b = strstr(body, "\"belongs_to_collection\"");
        if (b) {
          const char *o = strchr(b, '{');
          if (o) { const char *of = js_end(o);
                   idCol = (long)js_num(o, of, "id", 0.0);
                   js_text(o, of, "name", name, sizeof name); }
        }

        // --- ficha tecnica ---
        js_text(body, endC, "status", profileStatus, sizeof profileStatus);
        js_text(body, endC, "release_date", profileRelease, sizeof profileRelease);
        profileDuration = (int)js_num(body, endC, "runtime", 0.0);

        // production_countries is an array of objects; it joins the names with
        // commas, as the reference shows ("United States of America, Canada").
        // It stops appending when the field fills, instead of cutting a name in half.
        // um nome pela metade.
        { const char *p2 = js_array(body, endC, "production_countries");
          profileCountries[0] = 0;
          while (p2) {
            char pn[80] = "";
            const char *pf = js_end(p2);
            js_text(p2, pf, "name", pn, sizeof pn);
            if (pn[0]) {
              size_t used = strlen(profileCountries);
              size_t fits  = sizeof profileCountries - used;
              size_t wants  = strlen(pn) + (used ? 2 : 0) + 1;
              if (wants > fits) break;
              snprintf(profileCountries + used, fits, "%s%s", used ? ", " : "", pn);
            }
            p2 = js_next(pf);
          } }

        // Age rating: release_dates.results[] has one block per country, and
        // each block has release_dates[] with `certification`. We prefer BR;
        // failing that, US; failing both, the first non-empty one that turns up.
        // Many countries carry the key with an EMPTY string, and accepting the
        // first occurrence without looking at the contents filled the badge with nothing.
        { const char *res = js_array(body, endC, "results");
          char br[12] = "", us[12] = "", qq[12] = "";
          while (res) {
            const char *rf = js_end(res);
            char country[8] = "", c[12] = "";
            js_text(res, rf, "iso_3166_1", country, sizeof country);
            { const char *d = js_array(res, rf, "release_dates");
              while (d && !c[0]) {
                const char *df = js_end(d);
                js_text(d, df, "certification", c, sizeof c);
                d = js_next(df);
              } }
            if (c[0]) {
              if      (!strcmp(country, "BR")) snprintf(br, sizeof br, "%s", c);
              else if (!strcmp(country, "US")) snprintf(us, sizeof us, "%s", c);
              else if (!qq[0])              snprintf(qq, sizeof qq, "%s", c);
            }
            res = js_next(rf);
          }
          snprintf(profileCert, sizeof profileCert, "%s",
                   br[0] ? br : us[0] ? us : qq); }

        // Trailers: videos.results[]. YouTube only (the only host whose
        // thumbnail is obtainable from a predictable URL) and only what is a
        // Trailer or a Teaser — TMDB mixes featurettes, clips and behind-the-scenes in there.
        { const char *v = js_array(body, endC, "results");
          // `results` appears twice in the body (release_dates and videos); it
          // searches from the videos block so as not to pick the wrong one.
          const char *vid = strstr(body, "\"videos\"");
          if (vid) v = js_array(vid, endC, "results");
          while (v && nTrailer < EX_TRAILER_MAX) {
            const char *vf = js_end(v);
            char site[24] = "", kind[24] = "", key[16] = "", nm[80] = "";
            js_text(v, vf, "site", site, sizeof site);
            js_text(v, vf, "type", kind, sizeof kind);
            js_text(v, vf, "key",  key,  sizeof key);
            js_text(v, vf, "name", nm,   sizeof nm);
            if (key[0] && !strcmp(site, "YouTube") &&
                (!strcmp(kind, "Trailer") || !strcmp(kind, "Teaser"))) {
              int k = nTrailer++;
              snprintf(trailer[k].yt,   sizeof trailer[k].yt,   "%s", key);
              snprintf(trailer[k].name, sizeof trailer[k].name, "%s",
                       nm[0] ? nm : "Trailer");
              snprintf(trailer[k].mini, sizeof trailer[k].mini,
                       "https://img.youtube.com/vi/%s/hqdefault.jpg", key);
            }
            v = js_next(vf);
          } }

        pthread_mutex_unlock(&lock);
        free(body);
      }
    }
    if (idCol > 0) {
      snprintf(url, sizeof url, "%s/collection/%ld?api_key=%s&language=en-US",
               "https://api.themoviedb.org/3", idCol, key);
      body = net_download(url, 15);
      if (body) {
        struct { char t[120], a[8]; long id; } ach[EX_COL_MAX];
        int nc = 0;
        const char *p = js_array(body, NULL, "parts");
        while (p && nc < EX_COL_MAX) {
          const char *f = js_end(p);
          char date[16] = "";
          ach[nc].t[0] = ach[nc].a[0] = 0;
          js_text(p, f, "title", ach[nc].t, sizeof ach[nc].t);
          js_text(p, f, "release_date", date, sizeof date);
          if (strlen(date) >= 4) { memcpy(ach[nc].a, date, 4); ach[nc].a[4] = 0; }
          ach[nc].id = (long)js_num(p, f, "id", 0.0);
          if (ach[nc].t[0] && ach[nc].id > 0) nc++;
          p = js_next(f);
        }
        free(body);
        pthread_mutex_lock(&lock);
        if (!strcmp(id, idRequest)) {
          int k;
          snprintf(colName, sizeof colName, "%s", name);
          for (k = 0; k < nc; k++) {
            snprintf(col[k].title, sizeof col[k].title, "%s", ach[k].t);
            snprintf(col[k].year, sizeof col[k].year, "%s", ach[k].a);
            col[k].tmdb = ach[k].id;
          }
          nCol = nc;
        }
        pthread_mutex_unlock(&lock);
      }
    }
  }

  // Related titles are optional and may cost another round trip. If the user
  // has already opened another work, chain the most recent one now instead of
  // prolonging the wait with data that will be discarded.
  if (!requestStillCurrent(id)) { finishSearch(id); return NULL; }

  // --- related ---
  snprintf(url, sizeof url,
           "https://api.trakt.tv/%s/%s/related?limit=%d&extended=images",
           kind, id, EX_REL_MAX);
  body = net_download_headers(url, 15, header);
  if (body) {
    struct { char t[120], a[8], i[16], po[200]; } found[EX_REL_MAX];
    int n = 0;
    // p+1 and not js_next: js_next takes the END of the previous element, and
    // here there is no previous one yet. With js_next the first item was skipped
    // and, in a three-item response, rubbish was left over — both lists came back empty.
    const char *p = strchr(body, '[');
    p = p ? p + 1 : NULL;
    while (p && n < EX_REL_MAX) {
      const char *f = js_end(p);
      double year;
      found[n].t[0] = found[n].i[0] = 0;
      js_text(p, f, "title", found[n].t, sizeof found[n].t);
      js_text(p, f, "imdb", found[n].i, sizeof found[n].i);
      // Searching for "poster" in the whole item picks the WRONG field: Trakt
      // sends `"colors":{"poster":["#D8D5CB",...]}` BEFORE
      // `"images":{"poster":[...]}`, and the first version's log showed
      // `poster=https://#D8D5CB` — the art's average colour, not the art. The
      // search starts inside the `images` object.
      { const char *img = strstr(p, "\"images\"");
        const char *v = (img && img < f) ? js_array(img, f, "poster") : NULL;
        found[n].po[0] = 0;
        if (v && *v == '"') {
          const char *e = strchr(v + 1, '"');
          size_t k = e ? (size_t)(e - v - 1) : 0;
          // Trakt returns the path WITHOUT a scheme ("media.trakt.tv/..."); with
          // no https the texture cache treats it as a local file and finds nothing.
          if (k > 0 && k + 9 < sizeof found[n].po) {
            memcpy(found[n].po, "https://", 8);
            memcpy(found[n].po + 8, v + 1, k);
            found[n].po[8 + k] = 0;
          }
        } }
      year = js_num(p, f, "year", 0.0);
      if (year > 1800.0) snprintf(found[n].a, sizeof found[n].a, "%d", (int)year);
      else found[n].a[0] = 0;
      if (found[n].t[0] && found[n].i[0]) n++;
      p = js_next(f);
    }
    free(body);
    pthread_mutex_lock(&lock);
    if (!strcmp(id, idRequest)) {
      int k;
      for (k = 0; k < n; k++) {
        snprintf(rel[k].title, sizeof rel[k].title, "%s", found[k].t);
        snprintf(rel[k].year, sizeof rel[k].year, "%s", found[k].a);
        snprintf(rel[k].imdb, sizeof rel[k].imdb, "%s", found[k].i);
        snprintf(rel[k].poster, sizeof rel[k].poster, "%s", found[k].po);
      }
      nRel = n;
    }
    pthread_mutex_unlock(&lock);
  }

  { int k, q = 0;
    for (k = 0; k < EX_NSOURCES; k++) if (scores[k]) q++;
    printf("[extras] %s -> scores=%d/%d comments=%d rel=%d seasons=%d\n", id, q,
           EX_NSOURCES, nComment, nRel, nSeasons); }
  printf("[extras] collection \"%s\" -> %d | rel[0] poster=%s\n", colName, nCol,
         nRel ? rel[0].poster : "(none)"); fflush(stdout);
  fflush(stdout);
  finishSearch(id);
  return NULL;
}

void extras_request(const char *imdb, int series, long tmdbId) {
  char id[24];
  const char *dp;
  if (!imdb || imdb[0] != 't' || !trakt_active()) return;
  // The catalogue's field may arrive with an episode ("tt9737326:2:1"), which
  // is the format the source addons use. Trakt only knows the TITLE's id — with
  // the suffix it answers 404 and the three tabs came out empty on every series.
  dp = strchr(imdb, ':');
  if (dp) { size_t n = (size_t)(dp - imdb);
            if (n >= sizeof id) n = sizeof id - 1;
            memcpy(id, imdb, n); id[n] = 0; }
  else snprintf(id, sizeof id, "%s", imdb);
  imdb = id;
  pthread_mutex_lock(&lock);
  if (!strcmp(idRequest, imdb)) { pthread_mutex_unlock(&lock); return; }
  snprintf(idRequest, sizeof idRequest, "%s", imdb);
  seriesRequest = series;
  tmdbRequest = tmdbId;
  scoreTrakt = votesTrakt = nComment = nRel = nSeasons = nCol = 0;
  colName[0] = 0;
  nTrailer = profileDuration = 0;
  profileStatus[0] = profileCountries[0] = profileCert[0] = profileRelease[0] = 0;
  memset(watched, 0, sizeof watched);
  progressReady = nextT = nextE = 0;
  memset(scores, 0, sizeof scores);
  if (threadAlive) { pthread_mutex_unlock(&lock); return; }
  snprintf(idInProgress, sizeof idInProgress, "%s", imdb);
  seriesInProgress = series;
  tmdbInProgress = tmdbId;
  threadAlive = 1;
  pthread_mutex_unlock(&lock);
  if (pthread_create(&thread, NULL, fetch, NULL) != 0) threadAlive = 0;
  else pthread_detach(thread);
}

int extras_score_trakt(void)  { return scoreTrakt; }
int extras_votes_trakt(void) { return votesTrakt; }

int extras_n_comments(void) { return nComment; }
const char *extras_comment_user(int i) {
  return (i >= 0 && i < nComment) ? comment[i].user : "";
}
const char *extras_comment_text(int i) {
  return (i >= 0 && i < nComment) ? comment[i].text : "";
}
int extras_comment_likes(int i) {
  return (i >= 0 && i < nComment) ? comment[i].likes : 0;
}

// --- EPISODE comments ---------------------------------------------------------

static void *fetchEpComment(void *arg) {
  const char *header[4];
  char auth[200], key[140], url[260], show[24];
  char *body;
  int t, e;
  (void)arg;

  pthread_mutex_lock(&lock);
  snprintf(show, sizeof show, "%s", epShow);
  t = epReqTemp; e = epReqNum;
  pthread_mutex_unlock(&lock);

  if (!trakt_headers(header, auth, sizeof auth, key, sizeof key)) {
    pthread_mutex_lock(&lock); epThreadAlive = 0; pthread_mutex_unlock(&lock);
    return NULL;
  }
  snprintf(url, sizeof url,
           "https://api.trakt.tv/shows/%s/seasons/%d/episodes/%d/comments/likes?limit=%d",
           show, t, e, EX_COMMENT_MAX);
  body = net_download_headers(url, 12, header);
  if (body) {
    struct { char u[40]; char t[420]; int c; int score; } found[EX_COMMENT_MAX];
    int n = 0;
    // p+1 and not js_next, for the same reason as the series list: there is no
    // previous element to start from yet.
    const char *p = strchr(body, '[');
    p = p ? p + 1 : NULL;
    while (p && n < EX_COMMENT_MAX) {
      const char *f = js_end(p);
      found[n].u[0] = found[n].t[0] = 0;
      js_text(p, f, "comment", found[n].t, sizeof found[n].t);
      js_text(p, f, "username", found[n].u, sizeof found[n].u);
      found[n].c = (int)js_num(p, f, "likes", 0.0);
      found[n].score = (int)js_num(p, f, "user_rating", 0.0);
      numaLine(found[n].t);
      if (found[n].t[0]) n++;
      p = js_next(f);
    }
    free(body);
    pthread_mutex_lock(&lock);
    // Only publish if the owner is still on the same episode: changing episode
    // while this comes back would show the old list under the new label.
    if (t == epReqTemp && e == epReqNum) {
      int k;
      for (k = 0; k < n; k++) {
        snprintf(commentEp[k].user, sizeof commentEp[k].user, "%s", found[k].u);
        snprintf(commentEp[k].text, sizeof commentEp[k].text, "%s", found[k].t);
        commentEp[k].likes = found[k].c;
        commentEp[k].score = found[k].score;
      }
      nCommentEp = n;
      epTempCurrent = t; epNumCurrent = e;
    }
    pthread_mutex_unlock(&lock);
  }
  pthread_mutex_lock(&lock); epThreadAlive = 0; pthread_mutex_unlock(&lock);
  return NULL;
}

void extras_request_comments_ep(const char *imdbSeries, int season, int episode) {
  pthread_t f;
  if (!imdbSeries || !imdbSeries[0] || season <= 0 || episode <= 0) return;
  pthread_mutex_lock(&lock);
  // The same episode already loaded (or in flight): do not repeat the round trip.
  if (epThreadAlive ||
      (season == epTempCurrent && episode == epNumCurrent && nCommentEp > 0)) {
    pthread_mutex_unlock(&lock);
    return;
  }
  // The id may arrive as "tt123:2:4" from the episode list; Trakt wants only
  // the series.
  { const char *dp;
    snprintf(epShow, sizeof epShow, "%s", imdbSeries);
    dp = strchr(epShow, ':');
    if (dp) *(char *)dp = 0; }
  epReqTemp = season; epReqNum = episode;
  nCommentEp = 0;                 // clears it: the old list belongs to another episode
  epTempCurrent = epNumCurrent = 0;
  epThreadAlive = 1;
  pthread_mutex_unlock(&lock);
  if (pthread_create(&f, NULL, fetchEpComment, NULL) != 0) {
    pthread_mutex_lock(&lock); epThreadAlive = 0; pthread_mutex_unlock(&lock);
  } else {
    pthread_detach(f);
  }
}

int extras_n_comments_ep(void) { return nCommentEp; }
int extras_comments_ep_loading(void) { return epThreadAlive; }
const char *extras_comment_ep_user(int i) {
  return (i >= 0 && i < nCommentEp) ? commentEp[i].user : "";
}
const char *extras_comment_ep_text(int i) {
  return (i >= 0 && i < nCommentEp) ? commentEp[i].text : "";
}
int extras_comment_ep_likes(int i) {
  return (i >= 0 && i < nCommentEp) ? commentEp[i].likes : 0;
}
int extras_comment_ep_score(int i) {
  return (i >= 0 && i < nCommentEp) ? commentEp[i].score : 0;
}

int extras_comment_score(int i) {
  return (i >= 0 && i < nComment) ? comment[i].score : 0;
}

const char *extras_profile_status(void)        { return profileStatus; }
int         extras_profile_duration(void)       { return profileDuration; }
const char *extras_profile_countries(void)        { return profileCountries; }
const char *extras_profile_age_rating(void) { return profileCert; }
const char *extras_profile_release(void)    { return profileRelease; }

int extras_n_trailers(void) { return nTrailer; }
const char *extras_trailer_yt(int i) {
  return (i >= 0 && i < nTrailer) ? trailer[i].yt : "";
}
const char *extras_trailer_name(int i) {
  return (i >= 0 && i < nTrailer) ? trailer[i].name : "";
}
const char *extras_trailer_thumb(int i) {
  return (i >= 0 && i < nTrailer) ? trailer[i].mini : "";
}

const char *extras_collection_name(void) { return colName; }
int extras_n_collection(void) { return nCol; }
const char *extras_collection_title(int i) {
  return (i >= 0 && i < nCol) ? col[i].title : "";
}
const char *extras_collection_year(int i) {
  return (i >= 0 && i < nCol) ? col[i].year : "";
}
long extras_collection_tmdb(int i) { return (i >= 0 && i < nCol) ? col[i].tmdb : 0; }

int extras_n_seasons(void) { return nSeasons; }
int extras_season_number(int t) {
  return (t >= 0 && t < nSeasons) ? seasons[t].number : 0;
}
int extras_n_eps(int t) { return (t >= 0 && t < nSeasons) ? seasons[t].nEps : 0; }
int extras_ep_number(int t, int i) {
  return (t >= 0 && t < nSeasons && i >= 0 && i < seasons[t].nEps) ? seasons[t].eps[i].ep : 0;
}
int extras_ep_score(int t, int i) {
  return (t >= 0 && t < nSeasons && i >= 0 && i < seasons[t].nEps) ? seasons[t].eps[i].score : 0;
}

int extras_n_related(void) { return nRel; }
const char *extras_related_title(int i) {
  return (i >= 0 && i < nRel) ? rel[i].title : "";
}
const char *extras_related_year(int i) {
  return (i >= 0 && i < nRel) ? rel[i].year : "";
}
const char *extras_related_imdb(int i) {
  return (i >= 0 && i < nRel) ? rel[i].imdb : "";
}
const char *extras_related_poster(int i) {
  return (i >= 0 && i < nRel) ? rel[i].poster : "";
}

int extras_ep_watched(int season, int episode) {
  if (season < 0 || season >= EX_VIS_T) return 0;
  if (episode < 0 || episode >= EX_VIS_E) return 0;
  pthread_mutex_lock(&lock);
  int v = watched[season][episode];
  pthread_mutex_unlock(&lock);
  return v;
}

int extras_progress_ready(void) {
  pthread_mutex_lock(&lock);
  int ready = progressReady;
  pthread_mutex_unlock(&lock);
  return ready;
}
int extras_next_episode(int *t, int *e) {
  pthread_mutex_lock(&lock);
  int ok = progressReady && nextT > 0 && nextE > 0;
  if (ok) { *t = nextT; *e = nextE; }
  pthread_mutex_unlock(&lock);
  return ok;
}
