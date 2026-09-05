#ifndef NV_COLLECTIONS_H
#define NV_COLLECTIONS_H
#define COL_MAX 256
#define COL_SOURCE_MAX 32
// `base` is the addon's address; `addonId` is the id it declares in its manifest.
//
// A collection from the ACCOUNT fills addonId and leaves base empty, because the
// URL it carries is over 4 KB (these addons embed their whole configuration in
// it). A collection from the package fills base and leaves addonId empty. Read
// either one through col_source_base(), never directly.
typedef struct { char title[128], base[600], addonId[96], type[8], catId[96], genre[96]; } ColSource;
typedef struct {
  // `group` is the collection's TITLE, which is what the row shows. `groupId` is
  // the collection's id, which is how the ACCOUNT refers to it in the home order
  // (`collection_<id>`); the two are needed together to place a collection at
  // the position the owner chose. Empty for packaged collections, which the
  // account's order never mentions.
  char groupId[96];
  char id[96], title[128], group[64], cover[512], hero[512], logo[512];
  char frameDir[600];
  char detailHero[512];
  int editorial;
  int frames, hideTitle, nSources;
  ColSource sources[COL_SOURCE_MAX];
} ColFolder;
// Where to fetch this source from. Prefers the INSTALLED addon that owns
// `addonId`, and falls back to the address stored in the source itself — the
// same order the web app uses (`firstNonEmpty(addon?.baseUrl, source.addonBaseUrl)`).
// Returns "" when neither is available, which means the addon is not installed.
//
// Resolved on USE and not on load, on purpose: addon ids are learned while the
// manifests are read, which can happen AFTER the collections arrive from the
// account. Resolving at load time would miss on the first sync of every session.
const char *col_source_base(const ColSource *s);

int col_load(const char *dir);

// The OWNER'S collections, from the account (`sync_pull_collections`), added to
// whatever came in the package.
//
// Same model, different source: a collection is a GROUP and each of its folders
// is a card pointing at one or more addon catalogues. The web app's
// `catalogSources` maps onto ColSource one field at a time — addonBaseUrl to
// base, catalogId to catId — which is no accident: this struct was derived from
// it.
//
// TWO LIMITS, both visible in the log rather than silent:
//  - a folder whose sources are not addon catalogues (a TMDB-filtered folder,
//    say) has nothing this app can fetch, and is skipped;
//  - the artwork is a CDN URL, not a file. tex_get handles http(s), so the
//    cards do get art, but only once the download lands.
//
// Returns how many folders were added.
int col_load_account(const char *body);

// Changes whenever the folder list changes. The home's row builder keys its
// "do I need to rebuild" check on the CATALOGUE, so without this a collection
// arriving from the account would sit in memory and never reach the screen.
unsigned col_revision(void);

// The group (the collection's title) for the collection with this id, or NULL.
// It is what turns an account order entry `collection_<id>` into a home row,
// since rows are grouped by title.
const char *col_group_by_id(const char *id);

// 1 if the group came from a collection the owner pinned. The web app puts
// pinned collections first and never cuts them (`pinToTop`); the home row
// builder needs to ask.
int col_group_pinned(const char *group);

int col_n(void);
const ColFolder *col_folder(int i);
int col_group(const char *name, int *indices, int max);
const ColFolder *col_by_catalog(const char *base, const char *type, const char *id);
void col_color(const ColFolder *f, float *r, float *g, float *b);
#endif
