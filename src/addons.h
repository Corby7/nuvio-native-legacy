// The bridge to the addons (the Stremio protocol) — the real sources.
//
// An addon is a base URL; a title's sources come from
//   <base>/stream/<movie|series>/<id>.json
// and arrive as {"streams":[{name,title|description,url,behaviorHints},...]}.
// There is no authentication of its own: the key, where there is one, is already
// embedded in the addon's URL path (which is why addons.txt is the owner's
// sensitive content and must not go into any repository).
//
// The fetch BLOCKS and runs on a thread of its own. The result comes in through
// stream_set_list, and the screen only needs to look at addons_state().
#ifndef NV_ADDONS_H
#define NV_ADDONS_H

typedef enum { ADD_STOPPED = 0, ADD_SEARCHING, ADD_READY, ADD_EMPTY } AddState;

// Reads art/addons.txt (name<TAB>url per line). Without it the list stays empty.
int  addons_load(const char *dirArt);

// The list from the ACCOUNT, replacing the file. This is what makes the package
// distributable: as long as the list comes from art/addons.txt, the .ipk carries
// the debrid keys of whoever built it, embedded in the URLs.
//
// An EMPTY list is ignored on purpose. The server can answer empty because of
// the wrong profile, a mishandled 401 or an outage — and none of those is "the
// user removed all their addons". Replacing with empty would leave the person
// with no source at all and no idea why.
typedef struct { char name[64]; char url[600]; int active; } AddonRemote;
int  addons_set_list(const AddonRemote *list, int n);

// The current list, so the sync can push back what this device has.
int  addons_export(AddonRemote *output, int max);

// Forgets the account's list. Called on SIGN-OUT: without it, the next person to
// use this TV browses with the previous person's addons — and since the debrid
// keys travel embedded in the URLs, they also consume the previous person's
// subscription — until the first sync finishes. Having no source for a few
// seconds is the correct behaviour for "nobody signed in".
void addons_forget(void);
int  addons_n(void);
const char *addons_base(int i);   // URL base, sem /manifest.json
int  addons_has_catalog(int i);  // 1 quando o addon fornece catalogo

// Fires the fetch for `imdb`'s sources ("tt1234567", or "tt1234567:1:2" for an
// episode). Returns immediately; the result arrives through stream_set_list.
void addons_fetch(const char *imdb, const char *kind);

// --- external subtitles (OpenSubtitles) --------------------------------------
// A subtitle addon answers at /subtitles/<type>/<id>.json with
// {"subtitles":[{lang,url,subtitleFileName,...}]}. There are dozens per title,
// most in languages that are of no interest — which is why the list is FILTERED
// by language before it reaches the screen: 70 rows to scroll through would be
// worse than none.
#define SUB_MAX 12

typedef struct {
  char label[64];   // "Portugues (BR)  ·  Silo.S01E05.WEB"
  char language[8];
  char url[600];
} Subtitle;

void addons_fetch_subtitles(const char *imdb, const char *kind);
int  addons_n_subtitles(void);
const Subtitle *addons_subtitle(int i);

AddState addons_state(void);
void addons_shutdown(void);

#endif
