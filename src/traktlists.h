// Trakt lists on the Home: public lists the owner finds in Settings and adds as
// rows ("IMDB: Top Rated Movies", a director's filmography, a franchise).
//
// WHERE THEY COME FROM. Trakt's public list catalogue: /lists/popular for the
// page that opens, /search/list for a query. Both answer to the application's
// client id alone, so a profile that never linked Trakt can use them too.
//
// WHERE THEY LIVE. On this device, per profile, in trakt-lists-p<profile>.txt.
// The account's home order (home_catalog_shared) only knows addon catalogues and
// collections, and the web app would drop an entry it cannot resolve; a list row
// is device-local for the same reason the Trakt watchlist row is. Its POSITION
// and visibility are homerows.c's, in the same file as the watchlist's
// (key "traktlist_<id>"), so reordering works exactly as for any other row.
//
// The added set is read by the discovery thread during a build, hence the lock;
// the browse results belong to the main thread.
#ifndef NV_TRAKTLISTS_H
#define NV_TRAKTLISTS_H
#include <stddef.h>

#define TL_MAX   10    // lists on the Home at once
#define TL_ITEMS 100   // titles fetched per list row
#define TL_BROWSE_MAX 40

typedef struct {
  long id;                 // Trakt's numeric list id
  char name[96], user[64];
  int  items, likes;
  char desc[320];          // browse results only; not stored
} TlList;

// --- the added lists (any thread) ----------------------------------------------
int  tl_n(void);
int  tl_get(int i, TlList *out);        // 0 when i is out of range
int  tl_has(long id);
int  tl_add(const TlList *l);           // 0 when TL_MAX are already added
void tl_remove(long id);
// Bumped on every add and remove, so readers can tell their snapshot is stale.
unsigned tl_generation(void);

// The row key a list goes by in the Home order: "traktlist_<id>".
void tl_key(long id, char *dst, size_t n);
// The list id inside such a key, 0 for any other key.
long tl_id_of_key(const char *key);

// --- browsing (main thread) ------------------------------------------------------
enum { TLB_IDLE, TLB_LOADING, TLB_READY, TLB_FAILED };

// Starts a fetch on a thread of its own: the popular lists for an empty query,
// a search otherwise. A newer call wins; an older answer landing late is dropped.
void tl_browse(const char *query);
// Adopts a finished fetch. Call once a frame while the results are on screen.
void tl_browse_poll(void);
int  tl_browse_state(void);
int  tl_browse_n(void);
const TlList *tl_browse_item(int i);
const char *tl_browse_query(void);   // the query the results on screen answer

#endif
