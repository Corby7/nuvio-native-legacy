// Resolving a torrent through a debrid service, on the TV: Real-Debrid, TorBox
// and Premiumize. The web app's directDebridResolver.js; ported from upstream
// (iqui27/nuvio-native-legacy, debrid.c, 1.4).
//
// Addons like Torrentio or Comet with no debrid key in their URL answer with
// streams that carry only an `infoHash` and no `url`. The web app takes the
// debrid key from the ACCOUNT (sync_pull_provider_credentials, provider
// "debrid:<service>") and turns the hash into a direct link at play time.
// Without this, every one of those streams was thrown away by the parser and
// the list came back empty.
//
// One rule for all three: ONLY WHAT IS ALREADY CACHED on the service resolves.
// Asking the service to start downloading and waiting for it is not "play
// now", and the TV would sit on a network thread with nothing to show.
#ifndef NV_DEBRID_H
#define NV_DEBRID_H

// A key from the account. `service` is the suffix of "debrid:<service>":
// realdebrid/real-debrid, torbox, premiumize (with or without a hyphen). One
// key per service; calling it for another service does NOT clear the first.
void debrid_set_key(const char *service, const char *key);
int  debrid_active(void);        // there is a key for a service we can resolve
void debrid_forget(void);        // sign-out

// BLOCKS. Returns 1 and writes a direct, playable link to `url`; 0 when no
// service with a key has this torrent cached. `season`/`episode` pick the file
// inside a whole-season torrent (0,0 for a film).
int  debrid_resolve(const char *infoHash, int fileIdx, int season, int episode,
                    char *url, unsigned n);

#endif
