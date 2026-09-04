// Perfis da conta.
//
// Tudo que o sync pede leva `p_profile_id`. Sem escolher um perfil o app
// sincronizaria o perfil 1 sempre — e numa conta de familia isso significa
// mostrar a lista de outra pessoa e, pior, ESCREVER o progresso dela. Por isso
// os perfis vem antes de qualquer outra superficie.
//
// O DONO DA CONTA nao e necessariamente quem logou: `get_sync_owner` devolve o
// id de quem realmente possui os dados (conta compartilhada). MEDIDO: a RPC
// existe e responde uma string JSON crua com o uuid. A leitura da tabela de
// addons filtra por ESSE id, nao pelo `sub` do token.
#ifndef NV_PROFILES_H
#define NV_PROFILES_H

#define ACCOUNT_PROFILE_MAX 8

typedef struct {
  int  index_;          // profile_index (1..n) — e o que vai em p_profile_id
  char name[64];
  char colorHex[10];      // avatar_color_hex, "#1E88E5"
  // MEDIDO nesta conta: `avatar_url` vem NULO e o `avatar_id` ("avatar_lalo")
  // so vira imagem pela tabela `avatars`, que NAO existe neste servidor
  // (PGRST205). Ou seja: quando nao ha url, nao ha foto para buscar — o
  // circulo com a inicial e a representacao, nao um remendo.
  char avatarUrl[300];
  int  primary;        // is_primary
  int  temPin;          // veio de sync_pull_profile_locks
} AccountProfile;

// Busca os perfis e o dono. BLOQUEIA — chamar do fio de sync.
// Devolve quantos achou. Zero NAO e erro: conta nova pode nao ter perfil
// nenhum criado, e nesse caso o app opera com o perfil 1 implicito.
int profiles_pull(void);

int           profiles_n(void);
const AccountProfile *profiles_item(int i);
// O perfil em vigor, ou NULL quando a lista ainda nao chegou.
const AccountProfile *profiles_item_active(void);
const char   *profiles_owner(void);        // uuid de get_sync_owner; "" se nao veio

// ContaPerfil ativo. Persistido em disco: reescolher a cada arranque seria uma
// pergunta que o app ja sabe responder.
int  profiles_active(void);                // profile_index; 1 quando nada escolhido
void profiles_set_active(int index_);
void profiles_load_active(void);       // le do disco; chamar no arranque

// 1 quando ha mais de um perfil e o usuario ainda nao escolheu nesta conta —
// e o que faz a tela de escolha aparecer uma vez, e so uma.
int  profiles_needs_choose(void);

// Valida o PIN de um perfil travado. BLOQUEIA. 1 quando o servidor aceitou.
int  profiles_verify_pin(int index_, const char *pin);

// Esquece os perfis, o dono e a escolha gravada. Chamado ao SAIR: manter a
// escolha faria a conta seguinte comecar sincronizando o `p_profile_id` da
// conta anterior — ou seja, ESCREVENDO progresso no perfil errado.
void profiles_forget(void);

#endif
