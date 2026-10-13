// Lists on the Home: public lists the owner finds and adds as rows ("IMDB: Top
// Rated Movies", a director's filmography, a franchise). Two sources, Trakt and
// MDBList; the file keeps the Trakt name because Trakt came first.
//
// WHERE THEY COME FROM.
//   Trakt:   /lists/popular for the page that opens, /search/list for a query.
//            Both answer to the application's client id alone, so a profile that
//            never linked Trakt can use them too.
//   MDBList: /lists/top and /lists/search, with the owner's mdbList key (the one
//            the ratings already use, extras.h). Without a key there is no MDBList.
//
// WHERE THEY LIVE. On this device, per profile, in trakt-lists-p<profile>.txt.
// The account's home order (home_catalog_shared) only knows addon catalogues and
// collections, and the web app would drop an entry it cannot resolve; a list row
// is device-local for the same reason the Trakt watchlist row is. Its POSITION
// and visibility are homerows.c's, in the same file as the watchlist's (keys
// "traktlist_<id>" and "mdblist_<id>"), so reordering works as for any other row.
//
// The added set is read by the discovery thread during a build, hence the lock;
// the browse results belong to the main thread.
#ifndef NV_TRAKTLISTS_H
#define NV_TRAKTLISTS_H
#include <stddef.h>
#include "catalog.h"

#define TL_MAX   10    // lists on the Home at once
#define TL_ITEMS 100   // titles fetched per list row
#define TL_BROWSE_MAX 40

enum { TL_TRAKT, TL_MDB, TL_NSRC };

typedef struct {
  int  src;                // TL_TRAKT | TL_MDB
  long id;                 // the source's numeric list id
  char name[96], user[64];
  int  items, likes;
  char desc[320];          // browse results only; not stored
} TlList;

// --- the sources -----------------------------------------------------------------
const char *tl_source_name(int src);    // "Trakt", "MDBList"
// 0 when the source cannot be asked: no Trakt client id in the build, no mdbList key.
int  tl_source_usable(int src);
// A list's titles in its own rank order, films and series mixed, with art from
// the IMDb id. BLOCKS — call from a thread of your own. Returns how many.
int  tl_items(int src, long id, CatItem *output, int max);

// --- the added lists (any thread) ----------------------------------------------
int  tl_n(void);
int  tl_get(int i, TlList *out);        // 0 when i is out of range
int  tl_has(int src, long id);
int  tl_add(const TlList *l);           // 0 when TL_MAX are already added
void tl_remove(int src, long id);
// Bumped on every add and remove, so readers can tell their snapshot is stale.
unsigned tl_generation(void);

// The row key a list goes by in the Home order: "traktlist_<id>" / "mdblist_<id>".
void tl_key(int src, long id, char *dst, size_t n);
// The list id inside such a key (and its source into *src when not NULL), 0 for
// any other key.
long tl_id_of_key(const char *key, int *src);

// --- browsing (main thread) ------------------------------------------------------
enum { TLB_IDLE, TLB_LOADING, TLB_READY, TLB_FAILED };

// Starts a fetch on a thread of its own: the source's popular lists for an empty
// query, a search otherwise. A newer call wins; an older answer landing late is
// dropped — whichever screen asked last owns the results.
void tl_browse(int src, const char *query);
// Adopts a finished fetch. Call once a frame while the results are on screen.
void tl_browse_poll(void);
int  tl_browse_state(void);
int  tl_browse_n(void);
const TlList *tl_browse_item(int i);
const char *tl_browse_query(void);   // the query the results on screen answer
int  tl_browse_src(void);            // the source they came from (or are coming from)

#endif
