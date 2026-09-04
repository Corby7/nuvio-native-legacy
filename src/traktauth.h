// Linking Trakt on the TV itself, through Trakt's DEVICE CODE flow.
//
// WHY THIS EXISTS. MEASURED on the owner's account: Nuvio's credentials RPC
// (`sync_pull_provider_credentials`) returns tmdb, mdblist, animeskip, introdb
// and the debrid providers — and NO `trakt`. Since `art/trakt.txt` stopped
// shipping in the package (it was the packager's credential), whoever installed
// it would have no Trakt, and Trakt is where "Continue watching" comes from.
// Waiting for the web app to write the row into the account would only help
// people who already use the web app.
//
// The flow is Trakt's own, the same one the web app uses:
//   POST https://api.trakt.tv/oauth/device/code  {client_id}
//     -> {device_code, user_code, verification_url, expires_in, interval}
//   POST /oauth/device/token  {code, client_id, client_secret}
//     -> 200 with the token; 400 not authorised yet; 409 already used;
//        410 expired; 418 denied; 429 slow down.
//
// AN IMPORTANT DIFFERENCE FROM THE ACCOUNT LOGIN SCREEN: Trakt's `user_code` is
// SHORT (8 characters) and the address is fixed. That can be read off the TV and
// typed into a phone — it needs no QR, unlike the 32 hexadecimal digits of the
// account login.
#ifndef NV_TRAKTAUTH_H
#define NV_TRAKTAUTH_H

typedef enum {
  TRA_STOPPED = 0,
  TRA_REQUESTING,     // buscando o codigo
  TRA_WAITING,  // codigo na tela, esperando a pessoa autorizar
  TRA_ON,      // token obtido
  TRA_ERROR
} TraState;

// Comeca o fluxo num fio proprio. Idempotente enquanto um estiver em andamento.
void traktauth_begin(void);

// One step. Call once per frame; does not block. This is where the poll is
// rescheduled — the interval comes from Trakt itself, and goes up when it sends
// a 429.
void traktauth_step(unsigned nowMs);

TraState   traktauth_state(void);
const char *traktauth_code(void);    // user_code, para exibir
const char *traktauth_url(void);       // verification_url
const char *traktauth_error(void);

void traktauth_cancel(void);

// Loads the token saved on this installation and applies it in trakt.c. Call at
// startup, after data_start. 1 when there was a token.
int  traktauth_load(void);

// Esquece o vinculo (chamado ao sair da conta).
void traktauth_forget(void);

#endif
