#include "perfis.h"
#include "sessao.h"
#include "nuvem.h"
#include "dados.h"
#include "js.h"
#include "jsw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARQ_ATIVO "perfil.txt"

static ContaPerfil lista[CONTA_PERFIL_MAX];
static int n;
static char dono[64];
static int ativo = 1;
static int escolhido;      // 1 depois que o usuario decidiu nesta instalacao

static void lerDono(void) {
  char *r;
  int st = 0;
  if (dono[0]) return;
  r = sessao_rpc("get_sync_owner", "{}", &st);
  if (r && st >= 200 && st < 300) {
    // MEDIDO: a resposta e uma string JSON CRUA — "441bf572-…" — e nao um
    // objeto. js_texto nao serve aqui; o valor esta entre as aspas do corpo
    // inteiro.
    const char *a = strchr(r, '"');
    const char *b = a ? strchr(a + 1, '"') : NULL;
    if (a && b && b > a + 1 && (size_t)(b - a - 1) < sizeof dono) {
      memcpy(dono, a + 1, (size_t)(b - a - 1));
      dono[b - a - 1] = 0;
    }
  }
  free(r);
  if (!dono[0]) {
    // Sem o dono, a leitura da tabela de addons nao tem por quem filtrar. Cair
    // no `sub` do token e a aproximacao correta: numa conta que nao e
    // compartilhada os dois sao a mesma coisa.
    snprintf(dono, sizeof dono, "%s", sessao_usuario());
    printf("[perfis] get_sync_owner nao respondeu; usando o sub do token\n");
  }
}

int perfis_puxar(void) {
  char *r;
  int st = 0;
  const char *p;

  lerDono();

  r = sessao_rpc("sync_pull_profiles", "{}", &st);
  if (!r || st < 200 || st >= 300) { free(r); return n; }

  // Lista vazia NAO apaga o que ja esta em memoria: e a mesma regra que o app
  // web aplica em toda superficie. Resposta vazia pode ser perfil errado, 401
  // mal tratado ou servidor fora do ar, e nenhum desses e "o usuario apagou os
  // perfis".
  { int novos = 0;
    ContaPerfil tmp[CONTA_PERFIL_MAX];
    memset(tmp, 0, sizeof tmp);
    for (p = js_raiz_array(r); p && novos < CONTA_PERFIL_MAX; p = js_prox(js_fim(p))) {
      const char *f = js_fim(p);
      double idx = js_num(p, f, "profile_index", 0);
      if (idx <= 0) idx = js_num(p, f, "id", 0);
      if (idx <= 0) continue;
      tmp[novos].indice = (int)idx;
      if (!js_texto(p, f, "name", tmp[novos].nome, sizeof tmp[novos].nome))
        snprintf(tmp[novos].nome, sizeof tmp[novos].nome, "ContaPerfil %d", (int)idx);
      if (!js_texto(p, f, "avatar_color_hex", tmp[novos].corHex, sizeof tmp[novos].corHex))
        snprintf(tmp[novos].corHex, sizeof tmp[novos].corHex, "#1E88E5");
      { char b[16];
        // Sem o campo, o perfil 1 e o primario — e a mesma regra do web.
        tmp[novos].primario = js_bruto(p, f, "is_primary", b, sizeof b)
                              ? (strcmp(b, "true") == 0) : ((int)idx == 1); }
      novos++;
    }
    if (novos > 0) { memcpy(lista, tmp, sizeof lista); n = novos; }
  }
  free(r);

  // Travas: um perfil com PIN nao pode ser aberto so por estar na lista.
  r = sessao_rpc("sync_pull_profile_locks", "{}", &st);
  if (r && st >= 200 && st < 300) {
    for (p = js_raiz_array(r); p; p = js_prox(js_fim(p))) {
      const char *f = js_fim(p);
      int idx = (int)js_num(p, f, "profile_id", 0);
      int i;
      if (!idx) idx = (int)js_num(p, f, "profile_index", 0);
      for (i = 0; i < n; i++)
        if (lista[i].indice == idx) lista[i].temPin = 1;
    }
  }
  free(r);

  printf("[perfis] %d perfil(is), dono=%s, ativo=%d\n", n, dono, ativo);
  return n;
}

int           perfis_n(void)         { return n; }
const ContaPerfil *perfis_item(int i)     { return (i >= 0 && i < n) ? &lista[i] : NULL; }
const char   *perfis_dono(void)      { return dono; }
int           perfis_ativo(void)     { return ativo > 0 ? ativo : 1; }

void perfis_carregar_ativo(void) {
  char *b = dados_ler(ARQ_ATIVO);
  if (!b) return;
  { int v = atoi(b);
    if (v > 0) { ativo = v; escolhido = 1; } }
  free(b);
}

void perfis_definir_ativo(int indice) {
  char linha[32];
  if (indice <= 0) return;
  ativo = indice;
  escolhido = 1;
  snprintf(linha, sizeof linha, "%d\n", indice);
  dados_gravar(ARQ_ATIVO, linha);
  printf("[perfis] perfil ativo: %d\n", indice);
}

int perfis_precisa_escolher(void) {
  return n > 1 && !escolhido;
}

int perfis_verificar_pin(int indice, const char *pin) {
  Jsw w;
  char *r;
  int st = 0, ok = 0;
  jsw_iniciar(&w);
  jsw_obj_ini(&w);
  jsw_ci(&w, "p_profile_id", indice);
  jsw_cs(&w, "p_pin", pin ? pin : "");
  jsw_obj_fim(&w);
  r = sessao_rpc("verify_profile_pin", jsw_texto_final(&w), &st);
  jsw_livre(&w);
  // A RPC devolve um booleano; aceitar so o HTTP 200 deixaria passar um PIN
  // errado, que responde 200 com `false`.
  if (r && st >= 200 && st < 300) ok = (strstr(r, "true") != NULL);
  free(r);
  return ok;
}

void perfis_esquecer(void) {
  memset(lista, 0, sizeof lista);
  n = 0;
  dono[0] = 0;
  ativo = 1;
  escolhido = 0;
  dados_apagar(ARQ_ATIVO);
  printf("[perfis] perfis esquecidos (saiu da conta)\n");
}
