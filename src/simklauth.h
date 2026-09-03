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
#ifndef NV_SIMKLAUTH_H
#define NV_SIMKLAUTH_H

typedef enum {
  SMK_PARADO = 0,
  SMK_PEDINDO,
  SMK_AGUARDANDO,
  SMK_LIGADO,
  SMK_ERRO
} SmkEstado;

void simklauth_comecar(void);
void simklauth_passo(unsigned agoraMs);

SmkEstado   simklauth_estado(void);
const char *simklauth_codigo(void);
const char *simklauth_url(void);
const char *simklauth_erro(void);

void simklauth_cancelar(void);
int  simklauth_carregar(void);    // le o token guardado; 1 quando havia
void simklauth_esquecer(void);

#endif
