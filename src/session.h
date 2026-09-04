// The account: tokens on disk, TV login by code, refresh and sign-out.
//
// This is the module that swaps the owner's secret files (art/trakt.txt,
// art/addons.txt) for a real session. Until it exists, the .ipk cannot be
// distributed: it hands over the Trakt token and the debrid keys of whoever
// built it.
//
// THE FLOW, read from js/core/auth/qrLoginService.js. It is not the web's
// email-and-password login, and not by preference: the keyboard in search.c only
// has "a-z0-9", no capitals, no "@" and no dot. An email is not typeable in this
// app today.
//   1. an ANONYMOUS session (POST /auth/v1/signup; if refused, /auth/v1/token?
//      grant_type=anonymous). Without it the server answers 401 to the next two
//      RPCs.
//   2. start_tv_login_session -> returns a CODE.
//   3. the person opens the URL on their phone and types the code.
//   4. poll_tv_login_session until authorised.
//   5. /functions/v1/tv-logins-exchange -> access_token + refresh_token.
//
// Step 1 is the surprising one: it looks like the login starts from nothing, but
// the TV already has to be authenticated as SOMEBODY to ask for a code.
#ifndef NV_SESSION_H
#define NV_SESSION_H

typedef enum {
  SESS_LOGGEDOUT = 0,
  SESS_REQUESTING,      // buscando codigo
  SESS_WAITING,   // codigo na tela, esperando a pessoa autorizar
  SESS_SWITCHING,     // autorizado, trocando pelo token
  SESS_LOGGEDIN,
  SESS_ERROR
} SessState;

// Carrega a sessao gravada, se houver. Chamar depois de dados_iniciar e
// nuvem_configurar.
void session_start(void);

SessState   session_state(void);
int         session_loggedin(void);          // 1 so com sessao de USUARIO
const char *session_code(void);          // codigo a exibir; "" fora do fluxo
const char *session_url_login(void);       // URL a exibir
const char *session_error(void);            // ultima falha, para a tela mostrar
const char *session_user(void);         // `sub` do JWT; "" quando deslogado

// Comeca o fluxo de login num fio proprio (as chamadas bloqueiam). Idempotente
// enquanto um fluxo estiver em andamento.
void session_login_begin(void);

// Takes one step of the flow. Call once per frame; does not block. This is where
// the poll is rescheduled — there is no hidden timer inside the module.
void session_step(unsigned nowMs);

void session_cancel(void);

// Deletes tokens from disk and from memory. After this the app has no account at
// all again — and that is what the user expects from "sign out".
void session_exit(void);

// An RPC AUTHENTICATED as the user. Returns the body (caller frees) or NULL. On a
// 401 it refreshes the token and retries ONCE; if the refresh fails, the session
// drops to SESS_LOGGEDOUT rather than sitting in a limbo where the app looks
// signed in and nothing syncs. BLOCKS — call from a sync thread, never from the
// draw loop.
char *session_rpc(const char *func, const char *bodyJson, int *status);

// The same thing for /functions/v1/<name>.
char *session_func(const char *name, const char *bodyJson, int *status);

// A table READ as the user. It exists because not every surface has an RPC: the
// addon list only comes from the `addons` table, and reading with the anonymous
// key returns 401 "permission denied for table addons" — the RLS needs the token
// of whoever is asking to know which rows are theirs.
char *session_table(const char *table, const char *query, int *status);

#endif
