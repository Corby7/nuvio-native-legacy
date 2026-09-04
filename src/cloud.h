// Supabase transport, knowing nothing about the session.
//
// A FACT read out of the web app (js/data/remote/supabase/supabaseApi.js):
// ALL of the sync goes through a single shape — POST to /rest/v1/rpc/<function>
// with a JSON body and two headers, `apikey` and `Authorization`. There is no
// WebSocket and no SDK: what the native app needs is POST with headers (net.c
// already has it) and JSON (js.c reads, jsw.c writes). That is why this module
// fits in dozens of lines instead of demanding a library.
//
// The caller picks the Bearer. This layer deliberately does NOT know the
// session token: session.c is what decides between the anonymous key and the
// user's token, and that is where the "401 means refresh and retry" rule
// lives. Splitting it this way avoids the obvious circular dependency (session
// needs HTTP, HTTP would need the session).
#ifndef NV_CLOUD_H
#define NV_CLOUD_H

// Reads the configuration. The order is: compile-time values (-DNV_SUPABASE_URL
// and -DNV_SUPABASE_ANON_KEY, generated from local.properties by tools/env.sh)
// and, if it exists, `art/cloud.txt` overriding them — that file is for
// development, not for the package. Format: three lines, url / anon key / login
// base. Returns 1 when there is both a url and a key.
int cloud_configure(const char *dirArt);

int         cloud_ready(void);
const char *cloud_url(void);
const char *cloud_anon(void);
// Base of the web page a person opens on their phone to authorise the TV
// (TV_LOGIN_WEB_BASE_URL on the web). "" when not configured.
const char *cloud_base_login(void);
// The APPLICATION's Trakt client id (not the person's). Compiled in, as in the
// web app; the user's token comes from the sync.
const char *cloud_trakt_client(void);
// The APPLICATION's Trakt secret. Only the device-code flow needs it, when
// exchanging the code for a token; no other call uses it.
const char *cloud_trakt_secret(void);
// The application's Simkl key and name. Simkl uses no secret: both travel in
// the QUERY of every request, and without them the response is an unexplained
// error.
const char *cloud_simkl_client(void);
const char *cloud_simkl_app(void);

// POST with an explicit Bearer. Returns the body (caller frees) or NULL when
// the request never left. `status` receives the HTTP code — 0 when there was no
// response. The error body COMES BACK: it is where PostgREST says which
// function does not exist, and that string is the only way to tell "old server"
// from "wrong parameter".
char *cloud_post(const char *path, const char *bodyJson,
                 const char *bearer, int *status);

// Shorthand for /rest/v1/rpc/<function>.
char *cloud_rpc_com(const char *func, const char *bodyJson,
                    const char *bearer, int *status);

// GET on /rest/v1/<table>?<query>. MEASURED: not every surface has an RPC —
// `sync_pull_addons` DOES NOT EXIST on this server (PGRST202), and the addon
// list only comes out by reading the `addons` table directly, as the web app
// does on the happy path.
char *cloud_table(const char *table, const char *query,
                   const char *bearer, int *status);

// Escapes a value so it can go into a URL query. Without this an id containing
// "+" or "&" would break the filter silently and the read would come back as
// the whole table or as nothing — neither of which looks like an error.
void cloud_url_escape(const char *value, char *dst, unsigned size);

// 1 when the error response says the FUNCTION or the TABLE does not exist on
// this server (PGRST202/PGRST205). The web uses this to fall through to a
// legacy table; here it serves to say "this surface does not exist" instead of
// retrying forever.
int cloud_error_missing(const char *bodyError);

// A shared brake. One network failure takes ALL the surfaces down together for
// a while: without it, a server that is down turns into a dozen retry loops in
// parallel, each consuming the same CPU time the drawing needs.
void cloud_failed(void);
void cloud_ok(void);
int  cloud_brake_active(void);

#endif
