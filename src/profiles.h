// The account's profiles.
//
// Everything the sync asks for carries `p_profile_id`. Without choosing a
// profile the app would always sync profile 1 — and on a family account that
// means showing somebody else's list and, worse, WRITING their progress. That is
// why profiles come before any other surface.
//
// THE ACCOUNT OWNER is not necessarily whoever signed in: `get_sync_owner`
// returns the id of whoever actually owns the data (a shared account). MEASURED:
// the RPC exists and answers with a raw JSON string holding the uuid. Reading
// the addons table filters by THAT id, not by the token's `sub`.
#ifndef NV_PROFILES_H
#define NV_PROFILES_H

#define ACCOUNT_PROFILE_MAX 8

typedef struct {
  int  index_;          // profile_index (1..n) — e o que vai em p_profile_id
  char name[64];
  char colorHex[10];      // avatar_color_hex, "#1E88E5"
  // MEASURED on this account: `avatar_url` comes back NULL and `avatar_id`
  // ("avatar_lalo") only becomes an image through the `avatars` table, which
  // does NOT exist on this server (PGRST205). In other words: when there is no
  // url, there is no photo to fetch — the circle with the initial is the
  // representation, not a patch.
  char avatarUrl[300];
  int  primary;        // is_primary
  int  hasPin;          // veio de sync_pull_profile_locks
} AccountProfile;

// Fetches the profiles and the owner. BLOCKS — call from the sync thread.
// Returns how many it found. Zero is NOT an error: a new account may have no
// profile created, and in that case the app operates with an implicit
// profile 1.
int profiles_pull(void);

int           profiles_n(void);
const AccountProfile *profiles_item(int i);
// The profile in force, or NULL when the list has not arrived yet.
const AccountProfile *profiles_item_active(void);
const char   *profiles_owner(void);        // uuid de get_sync_owner; "" se nao veio

// The active AccountProfile. Persisted to disk: choosing again on every start
// would be a question the app already knows the answer to.
int  profiles_active(void);                // profile_index; 1 quando nada escolhido
void profiles_set_active(int index_);
void profiles_load_active(void);       // le do disco; chamar no arranque

// 1 when there is more than one profile and the user has not chosen yet on this
// account — it is what makes the picker screen appear once, and only once.
int  profiles_needs_choose(void);

// Valida o PIN de um perfil travado. BLOQUEIA. 1 quando o servidor aceitou.
int  profiles_verify_pin(int index_, const char *pin);

// Esquece os perfis, o dono e a escolha gravada. Chamado ao SAIR: manter a
// escolha faria a conta seguinte comecar sincronizando o `p_profile_id` da
// conta anterior — ou seja, ESCREVENDO progresso no perfil errado.
void profiles_forget(void);

#endif
