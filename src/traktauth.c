#include "traktauth.h"
#include "nuvem.h"
#include "dados.h"
#include "rede.h"
#include "trakt.h"
#include "sync.h"
#include "js.h"
#include "jsw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define TRA_ARQ "trakt.txt"
#define TRA_BASE "https://api.trakt.tv"
// Quando o Trakt nao manda `interval`, 5s e o que a documentacao dele sugere.
#define TRA_POLL_PADRAO 5000u

static TraEstado estado = TRA_PARADO;
static char deviceCode[128];
static char userCode[32];
static char url[160];
static char erro[200];
static char token[300], refresh[300];
static unsigned pollMs = TRA_POLL_PADRAO;
static unsigned proximoPoll, comecouMs, limiteMs;

static pthread_t fio;
static int fioVivo, fioPronto;
// 1 quando o fio acabou de conseguir o token e o laco principal ainda nao o
// aplicou. Aplicar dentro do fio mexeria em trakt.c enquanto a UI le dele.
static int tokenNovo;

static char *postar(const char *caminho, const char *corpo, int *status) {
  char completo[300];
  const char *cab[2];
  snprintf(completo, sizeof completo, "%s%s", TRA_BASE, caminho);
  // O Trakt exige o cabecalho de versao da API; sem ele responde 412.
  cab[0] = "trakt-api-version: 2";
  cab[1] = NULL;
  return rede_postar_st(completo, 20, cab, corpo, status);
}

// ---------------------------------------------------------------- disco

static void gravar(void) {
  char buf[400];
  // Mesmo formato do art/trakt.txt de antes ("token<TAB>clientId"), para o
  // arquivo continuar legivel por quem ja conhecia o de la. A diferenca e o
  // LUGAR: aqui e a pasta da instalacao, nao o pacote.
  snprintf(buf, sizeof buf, "%s\t%s\n", token, nuvem_trakt_cliente());
  dados_gravar(TRA_ARQ, buf);
}

int traktauth_carregar(void) {
  char *b = dados_ler(TRA_ARQ);
  char *tab;
  if (!b) return 0;
  tab = strchr(b, '\t');
  if (tab) *tab = 0;
  { char *fim = b + strlen(b);
    while (fim > b && (fim[-1] == '\n' || fim[-1] == '\r')) *--fim = 0; }
  if (b[0]) {
    snprintf(token, sizeof token, "%s", b);
    trakt_definir(token, nuvem_trakt_cliente());
    estado = TRA_LIGADO;
  }
  free(b);
  return token[0] != 0;
}

void traktauth_esquecer(void) {
  token[0] = refresh[0] = deviceCode[0] = userCode[0] = url[0] = erro[0] = 0;
  estado = TRA_PARADO;
  dados_apagar(TRA_ARQ);
}

// ---------------------------------------------------------------- fluxo

static void *fioPedir(void *u) {
  Jsw w;
  char *r;
  int st = 0;
  (void)u;
  erro[0] = userCode[0] = deviceCode[0] = 0;

  if (!nuvem_trakt_cliente()[0] || !nuvem_trakt_segredo()[0]) {
    // Caso de COMPILACAO, nao do usuario: o pacote saiu sem as chaves do
    // aplicativo. Dizer isso evita a pessoa tentar de novo para sempre.
    snprintf(erro, sizeof erro, "pacote sem as chaves do Trakt");
    estado = TRA_ERRO;
    fioPronto = 1;
    return NULL;
  }

  jsw_iniciar(&w);
  jsw_obj_ini(&w);
  jsw_cs(&w, "client_id", nuvem_trakt_cliente());
  jsw_obj_fim(&w);
  r = postar("/oauth/device/code", jsw_texto_final(&w), &st);
  jsw_livre(&w);

  if (r && st >= 200 && st < 300) {
    const char *fim = r + strlen(r);
    double intervalo, expira;
    js_texto(r, fim, "device_code", deviceCode, sizeof deviceCode);
    js_texto(r, fim, "user_code", userCode, sizeof userCode);
    js_texto(r, fim, "verification_url", url, sizeof url);
    intervalo = js_num(r, fim, "interval", 0);
    expira = js_num(r, fim, "expires_in", 0);
    if (intervalo >= 1.0 && intervalo <= 60.0) pollMs = (unsigned)(intervalo * 1000.0);
    limiteMs = (expira > 30.0 && expira < 3600.0) ? (unsigned)(expira * 1000.0) : 600000u;
  }
  if (!deviceCode[0] || !userCode[0]) {
    if (st == 429) snprintf(erro, sizeof erro, "o Trakt pediu para esperar; tente daqui a pouco");
    else snprintf(erro, sizeof erro, "nao consegui pedir o codigo ao Trakt (HTTP %d)", st);
    estado = TRA_ERRO;
  } else {
    if (!url[0]) snprintf(url, sizeof url, "https://trakt.tv/activate");
    estado = TRA_AGUARDANDO;
  }
  free(r);
  fioPronto = 1;
  return NULL;
}

static void *fioPoll(void *u) {
  Jsw w;
  char *r;
  int st = 0;
  (void)u;

  jsw_iniciar(&w);
  jsw_obj_ini(&w);
  jsw_cs(&w, "code", deviceCode);
  jsw_cs(&w, "client_id", nuvem_trakt_cliente());
  jsw_cs(&w, "client_secret", nuvem_trakt_segredo());
  jsw_obj_fim(&w);
  r = postar("/oauth/device/token", jsw_texto_final(&w), &st);
  jsw_livre(&w);

  if (r && st >= 200 && st < 300) {
    char t[300];
    if (js_texto(r, r + strlen(r), "access_token", t, sizeof t)) {
      snprintf(token, sizeof token, "%s", t);
      // O refresh vai junto para a conta: sem ele, o vinculo morre no dia em
      // que o access token vencer e o app web nao teria como renovar.
      if (!js_texto(r, r + strlen(r), "refresh_token", refresh, sizeof refresh))
        refresh[0] = 0;
      tokenNovo = 1;
      estado = TRA_LIGADO;
    } else {
      snprintf(erro, sizeof erro, "o Trakt respondeu sem token");
      estado = TRA_ERRO;
    }
  } else if (st == 400) {
    /* ainda nao autorizado: seguir perguntando */
  } else if (st == 429) {
    // O Trakt mandou ir mais devagar. Subir o intervalo, com teto.
    pollMs += 5000u;
    if (pollMs > 60000u) pollMs = 60000u;
  } else if (st == 409) {
    snprintf(erro, sizeof erro, "este codigo ja foi usado");
    estado = TRA_ERRO;
  } else if (st == 410) {
    snprintf(erro, sizeof erro, "o codigo expirou");
    estado = TRA_ERRO;
  } else if (st == 418) {
    snprintf(erro, sizeof erro, "autorizacao negada no Trakt");
    estado = TRA_ERRO;
  } else if (st) {
    snprintf(erro, sizeof erro, "falha ao trocar o codigo (HTTP %d)", st);
    estado = TRA_ERRO;
  }
  free(r);
  fioPronto = 1;
  return NULL;
}

static void soltar(void *(*rotina)(void *)) {
  if (fioVivo) return;
  fioPronto = 0;
  if (pthread_create(&fio, NULL, rotina, NULL) == 0) { pthread_detach(fio); fioVivo = 1; }
  else { snprintf(erro, sizeof erro, "sem fio para falar com o Trakt"); estado = TRA_ERRO; }
}

void traktauth_comecar(void) {
  if (estado == TRA_PEDINDO || estado == TRA_AGUARDANDO) return;
  erro[0] = 0;
  comecouMs = 0;
  pollMs = TRA_POLL_PADRAO;
  estado = TRA_PEDINDO;
  soltar(fioPedir);
}

void traktauth_passo(unsigned agoraMs) {
  if (fioVivo && fioPronto) { fioVivo = 0; fioPronto = 0; }
  if (fioVivo) return;

  // Aplicar o token no LACO PRINCIPAL, nunca no fio: trakt.c e lido pela UI.
  if (tokenNovo) {
    tokenNovo = 0;
    trakt_definir(token, nuvem_trakt_cliente());
    gravar();
    // E manda para a CONTA, para os outros aparelhos da pessoa herdarem o
    // vinculo — e a linha `trakt` que hoje nao existe la.
    { // A forma do credential_json e a que o app web grava, para os dois lados
      // lerem a mesma coisa.
      Jsw c;
      jsw_iniciar(&c);
      jsw_obj_ini(&c);
      jsw_cs(&c, "access_token", token);
      if (refresh[0]) jsw_cs(&c, "refresh_token", refresh);
      jsw_cs(&c, "token_type", "bearer");
      jsw_obj_fim(&c);
      sync_empurrar_credencial("trakt", jsw_texto_final(&c));
      jsw_livre(&c); }
    printf("[trakt] vinculado nesta TV\n");
    fflush(stdout);
  }

  if (estado != TRA_AGUARDANDO) return;
  if (!comecouMs) comecouMs = agoraMs;
  if (limiteMs && agoraMs - comecouMs > limiteMs) {
    snprintf(erro, sizeof erro, "o codigo expirou");
    estado = TRA_ERRO;
    return;
  }
  if (agoraMs >= proximoPoll) {
    proximoPoll = agoraMs + pollMs;
    soltar(fioPoll);
  }
}

void traktauth_cancelar(void) {
  if (estado == TRA_PEDINDO || estado == TRA_AGUARDANDO || estado == TRA_ERRO)
    estado = token[0] ? TRA_LIGADO : TRA_PARADO;
}

TraEstado   traktauth_estado(void) { return estado; }
const char *traktauth_codigo(void) { return userCode; }
const char *traktauth_url(void)    { return url; }
const char *traktauth_erro(void)   { return erro; }
