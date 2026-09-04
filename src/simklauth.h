// Linking Simkl on the TV itself, through Simkl's PIN flow.
//
// THE FLOW (the same as the web app's, in
// js/data/repository/simklAuthService.js), and it is NOT the same as Trakt's —
// these are GETs, and there is no client_secret:
//   GET https://api.simkl.com/oauth/pin?client_id=..&app-name=..&app-version=..
//     -> {"result":"OK","user_code":"ABC123","verification_url":"...",
//         "expires_in":900}
//   GET /oauth/pin/<user_code>?client_id=..   (the same query)
//     -> {"result":"KO"}                        not authorised yet
//     -> {"result":"OK","access_token":"..."}   done
//
// AN HONEST LIMIT: today NO screen in this app consumes Simkl — there is no
// simkl.c the way there is a trakt.c. Linking here exists so THE CREDENTIAL
// REACHES THE ACCOUNT, and from there the web app and the phone. When the native
// app gains Simkl reading, the token will already be in place.
// A KNOWN LIMIT, and different from Trakt: here the pending request does NOT
// survive an app restart — the PIN lives only in memory. On Trakt that was fixed
// because it actually bit (the owner authorised and the app had restarted in the
// middle); here it is written down rather than implemented unused, since nothing
// in this app consumes Simkl yet. If it becomes a problem, it is the same patch
// as traktauth.c's.
#ifndef NV_SIMKLAUTH_H
#define NV_SIMKLAUTH_H

typedef enum {
  SMK_STOPPED = 0,
  SMK_REQUESTING,
  SMK_WAITING,
  SMK_ON,
  SMK_ERROR
} SmkState;

void simklauth_begin(void);
void simklauth_step(unsigned nowMs);

SmkState   simklauth_state(void);
const char *simklauth_code(void);
const char *simklauth_url(void);
const char *simklauth_error(void);

void simklauth_cancel(void);
int  simklauth_load(void);    // le o token guardado; 1 quando havia
void simklauth_forget(void);

#endif
