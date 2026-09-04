#include "profiles.h"
#include "session.h"
#include "cloud.h"
#include "data.h"
#include "js.h"
#include "jsw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FILE_ACTIVE "profile.txt"

static AccountProfile list[ACCOUNT_PROFILE_MAX];
static int n;
static char owner[64];
static int active = 1;
static int chosen;      // 1 depois que o usuario decidiu nesta instalacao

static void readOwner(void) {
  char *r;
  int st = 0;
  if (owner[0]) return;
  r = session_rpc("get_sync_owner", "{}", &st);
  if (r && st >= 200 && st < 300) {
    // MEDIDO: a resposta e uma string JSON CRUA — "441bf572-…" — e nao um
    // objeto. js_texto nao serve aqui; o valor esta entre as aspas do corpo
    // inteiro.
    const char *a = strchr(r, '"');
    const char *b = a ? strchr(a + 1, '"') : NULL;
    if (a && b && b > a + 1 && (size_t)(b - a - 1) < sizeof owner) {
      memcpy(owner, a + 1, (size_t)(b - a - 1));
      owner[b - a - 1] = 0;
    }
  }
  free(r);
  if (!owner[0]) {
    // Sem o dono, a leitura da tabela de addons nao tem por quem filtrar. Cair
    // no `sub` do token e a aproximacao correta: numa conta que nao e
    // compartilhada os dois sao a mesma coisa.
    snprintf(owner, sizeof owner, "%s", session_user());
    printf("[profiles] get_sync_owner did not answer; using the token sub\n");
  }
}

int profiles_pull(void) {
  char *r;
  int st = 0;
  const char *p;

  readOwner();

  r = session_rpc("sync_pull_profiles", "{}", &st);
  if (!r || st < 200 || st >= 300) { free(r); return n; }

  // Lista vazia NAO apaga o que ja esta em memoria: e a mesma regra que o app
  // web aplica em toda superficie. Resposta vazia pode ser perfil errado, 401
  // mal tratado ou servidor fora do ar, e nenhum desses e "o usuario apagou os
  // perfis".
  { int new = 0;
    AccountProfile tmp[ACCOUNT_PROFILE_MAX];
    memset(tmp, 0, sizeof tmp);
    for (p = js_root_array(r); p && new < ACCOUNT_PROFILE_MAX; p = js_next(js_end(p))) {
      const char *f = js_end(p);
      double idx = js_num(p, f, "profile_index", 0);
      if (idx <= 0) idx = js_num(p, f, "id", 0);
      if (idx <= 0) continue;
      tmp[new].index_ = (int)idx;
      if (!js_text(p, f, "name", tmp[new].name, sizeof tmp[new].name))
        snprintf(tmp[new].name, sizeof tmp[new].name, "ContaPerfil %d", (int)idx);
      js_text(p, f, "avatar_url", tmp[new].avatarUrl, sizeof tmp[new].avatarUrl);
      if (!js_text(p, f, "avatar_color_hex", tmp[new].colorHex, sizeof tmp[new].colorHex))
        snprintf(tmp[new].colorHex, sizeof tmp[new].colorHex, "#1E88E5");
      { char b[16];
        // Sem o campo, o perfil 1 e o primario — e a mesma regra do web.
        tmp[new].primary = js_raw(p, f, "is_primary", b, sizeof b)
                              ? (strcmp(b, "true") == 0) : ((int)idx == 1); }
      new++;
    }
    if (new > 0) { memcpy(list, tmp, sizeof list); n = new; }
  }
  free(r);

  // Travas: um perfil com PIN nao pode ser aberto so por estar na lista.
  r = session_rpc("sync_pull_profile_locks", "{}", &st);
  if (r && st >= 200 && st < 300) {
    for (p = js_root_array(r); p; p = js_next(js_end(p))) {
      const char *f = js_end(p);
      int idx = (int)js_num(p, f, "profile_id", 0);
      char b[16];
      int i, locked;
      if (!idx) idx = (int)js_num(p, f, "profile_index", 0);
      // MEDIDO: esta RPC devolve UMA LINHA POR PERFIL, com `pin_enabled` false
      // quando nao ha PIN — nao e uma lista so dos travados. Marcar todo perfil
      // que aparece aqui trancava TODOS eles, e como nenhum tem PIN nenhuma
      // digitacao seria aceita: ninguem entraria na propria conta.
      locked = js_raw(p, f, "pin_enabled", b, sizeof b)
                ? (strcmp(b, "true") == 0) : 0;
      for (i = 0; i < n; i++)
        if (list[i].index_ == idx) list[i].temPin = locked;
    }
  }
  free(r);

  printf("[profiles] %d profile(s), owner=%s, active=%d\n", n, owner, active);
  return n;
}

int           profiles_n(void)         { return n; }
const AccountProfile *profiles_item(int i)     { return (i >= 0 && i < n) ? &list[i] : NULL; }

const AccountProfile *profiles_item_active(void) {
  int i;
  for (i = 0; i < n; i++) if (list[i].index_ == active) return &list[i];
  return NULL;
}
const char   *profiles_owner(void)      { return owner; }
int           profiles_active(void)     { return active > 0 ? active : 1; }

void profiles_load_active(void) {
  char *b = data_read(FILE_ACTIVE);
  if (!b) return;
  { int v = atoi(b);
    if (v > 0) { active = v; chosen = 1; } }
  free(b);
}

void profiles_set_active(int index_) {
  char line[32];
  if (index_ <= 0) return;
  active = index_;
  chosen = 1;
  snprintf(line, sizeof line, "%d\n", index_);
  data_write(FILE_ACTIVE, line);
  printf("[profiles] active profile: %d\n", index_);
}

int profiles_needs_choose(void) {
  return n > 1 && !chosen;
}

int profiles_verify_pin(int index_, const char *pin) {
  Jsw w;
  char *r;
  int st = 0, ok = 0;
  jsw_start(&w);
  jsw_obj_start(&w);
  jsw_ci(&w, "p_profile_id", index_);
  jsw_cs(&w, "p_pin", pin ? pin : "");
  jsw_obj_end(&w);
  r = session_rpc("verify_profile_pin", jsw_text_final(&w), &st);
  jsw_free(&w);
  // A RPC devolve um booleano; aceitar so o HTTP 200 deixaria passar um PIN
  // errado, que responde 200 com `false`.
  if (r && st >= 200 && st < 300) ok = (strstr(r, "true") != NULL);
  free(r);
  return ok;
}

void profiles_forget(void) {
  memset(list, 0, sizeof list);
  n = 0;
  owner[0] = 0;
  active = 1;
  chosen = 0;
  data_erase(FILE_ACTIVE);
  printf("[profiles] profiles forgotten (signed out)\n");
}
