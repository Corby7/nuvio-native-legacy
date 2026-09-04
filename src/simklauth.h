// Vincular o Simkl na propria TV, pelo fluxo de PIN do Simkl.
//
// FLUXO (o mesmo do app web, em js/data/repository/simklAuthService.js), e ele
// NAO e igual ao do Trakt — sao GETs, e nao ha client_secret:
//   GET https://api.simkl.com/oauth/pin?client_id=..&app-name=..&app-version=..
//     -> {"result":"OK","user_code":"ABC123","verification_url":"...",
//         "expires_in":900}
//   GET /oauth/pin/<user_code>?client_id=..   (mesma query)
//     -> {"result":"KO"}                        ainda nao autorizado
//     -> {"result":"OK","access_token":"..."}   pronto
//
// LIMITE HONESTO: hoje NENHUMA tela deste app consome Simkl — nao ha um
// simkl.c como ha o trakt.c. Vincular aqui serve para a CREDENCIAL CHEGAR NA
// CONTA, e dali para o app web e o celular. Quando o nativo ganhar leitura de
// Simkl, o token ja vai estar no lugar.
// LIMITE CONHECIDO, e diferente do Trakt: aqui o pedido pendente NAO sobrevive
// a um reinicio do app — o PIN vive so na memoria. No Trakt isso foi corrigido
// porque mordeu de verdade (o dono autorizou e o app tinha reiniciado no meio);
// aqui fica anotado em vez de implementado sem uso, ja que nada neste app
// consome Simkl ainda. Se virar problema, e o mesmo remendo do traktauth.c.
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
