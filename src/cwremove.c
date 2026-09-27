#include "cwremove.h"
#include "catalog.h"
#include "discover.h"
#include "trakt.h"
#include "sync.h"
#include <SDL2/SDL.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

enum { CW_NONE, CW_PENDING, CW_CONFIRMED, CW_FAILURE };

// The account's progress_key for every row this work may have, collected from
// progress.txt BEFORE the lines are dropped: the account holds one row per
// episode, and the file is the only place that says which episodes those were.
#define CW_KEYS_MAX 24
static char keyBuf[CW_KEYS_MAX][40];
static const char *keys[CW_KEYS_MAX];
static int nKeys;
static char work[24];
static volatile int state, alive;
// The local half is still owed: the card stays in the row, its ring circling,
// until the remote deletes have answered.
static int localOwed;
// The ring keeps circling past that, until the card has actually left the row:
// the rebuild the local half asks for publishes a moment later, and an arc that
// stopped first left a still card sitting there looking done.
static int spinning;
static Uint32 localDoneAt;
static pthread_t thread;

static void stateWrite(int v) { __atomic_store_n(&state, v, __ATOMIC_RELEASE); }
int cw_remove_state(void) { return __atomic_load_n(&state, __ATOMIC_ACQUIRE); }

static void addKey(int season, int episode) {
  int i;
  char k[40];
  if (season > 0 && episode > 0) snprintf(k, sizeof k, "%s_s%de%d", work, season, episode);
  else snprintf(k, sizeof k, "%s", work);
  for (i = 0; i < nKeys; i++) if (!strcmp(keyBuf[i], k)) return;
  if (nKeys == CW_KEYS_MAX) return;
  snprintf(keyBuf[nKeys], sizeof keyBuf[nKeys], "%s", k);
  keys[nKeys] = keyBuf[nKeys];
  nKeys++;
}

static int okAccount;

static void *runAccount(void *u) {
  (void)u;
  // EVERY row the account has for this work, not only the ones progress.txt
  // names. The file keeps one line per series, so it knew one episode; the
  // account keeps one row per episode, and the rest survived the delete. They
  // came straight back on the phone, and on this TV the moment the removal
  // list was lost (a fresh install starts without it).
  { static char remote[CW_KEYS_MAX][40];
    int i, n = sync_progress_keys(work, remote, CW_KEYS_MAX);
    for (i = 0; i < n; i++) {
      int j, dup = 0;
      for (j = 0; j < nKeys && !dup; j++) dup = !strcmp(keyBuf[j], remote[i]);
      if (dup || nKeys == CW_KEYS_MAX) continue;
      snprintf(keyBuf[nKeys], sizeof keyBuf[nKeys], "%s", remote[i]);
      keys[nKeys] = keyBuf[nKeys];
      nKeys++;
    } }
  okAccount = sync_delete_progress(keys, nKeys);
  return NULL;
}

static void *run(void *u) {
  int okTrakt;
  pthread_t account;
  int split;
  (void)u;
  // The two sources side by side: they share nothing, and in a row the card
  // waited for the sum of both (two round trips on the account side alone).
  split = pthread_create(&account, NULL, runAccount, NULL) == 0;
  if (!split) runAccount(NULL);
  okTrakt = trakt_playback_remove(work);
  if (split) pthread_join(account, NULL);
  printf("[cw] removed %s: trakt %s, account %s\n", work,
         okTrakt ? "ok" : "FAILED", okAccount ? "ok" : "FAILED");
  fflush(stdout);
  stateWrite(okTrakt && okAccount ? CW_CONFIRMED : CW_FAILURE);
  __atomic_store_n(&alive, 0, __ATOMIC_RELEASE);
  return NULL;
}

int cw_remove(int index_) {
  const CatItem *ci = cat_item(index_);
  CatProgress regs[CAT_PROGRESS_MAX];
  int i, n;
  if (!ci || !ci->imdb[0]) return 0;
  if (__atomic_load_n(&alive, __ATOMIC_ACQUIRE)) return 0;
  snprintf(work, sizeof work, "%.*s", (int)strcspn(ci->imdb, ":"), ci->imdb);

  nKeys = 0;
  addKey(ci->season, ci->episode);
  n = cat_progress_read(regs, CAT_PROGRESS_MAX);
  for (i = 0; i < n; i++)
    if (!strcmp(regs[i].imdb, work)) addKey(regs[i].season, regs[i].episode);

  // THE LOCAL HALF WAITS FOR THE REMOTE ONE. It used to run here, and the card
  // left the row the instant the menu closed, with nothing to say the deletes
  // were still going. Now the card stays, its ring circling (cw_remove_busy),
  // and cw_remove_step drops it once the network has answered — either way: the
  // dismissal hides it even when a remote delete failed.
  localOwed = 1;
  spinning = 1;
  stateWrite(CW_PENDING);
  __atomic_store_n(&alive, 1, __ATOMIC_RELEASE);
  if (pthread_create(&thread, NULL, run, NULL) != 0) {
    __atomic_store_n(&alive, 0, __ATOMIC_RELEASE);
    stateWrite(CW_FAILURE);
    cw_remove_step();
    return 1;
  }
  pthread_detach(thread);
  return 1;
}

void cw_remove_step(void) {
  if (!localOwed || __atomic_load_n(&alive, __ATOMIC_ACQUIRE)) return;
  localOwed = 0;
  localDoneAt = SDL_GetTicks();
  cat_cw_dismiss(work);
  cat_progress_remove(work);
  // OFF THE ROW NOW, not when the rebuild below publishes: that is the whole
  // home again, every catalogue included, and on the C3 it took ~20 s — all of
  // it spent with the card sitting there circling.
  { int r, nr = cat_n_rows();
    for (r = 0; r < nr; r++) {
      const CatRow *c = cat_row(r);
      if (c && !strcmp(c->key, "continue_watching")) { cat_row_drop(r, work); break; }
    } }
  // Built again now the remote copies are gone too: the row settles on what the
  // sources now say.
  disc_rebuild();
}

// A rebuild that never drops the card (a dismissal the build ignores) must not
// leave it circling for ever.
#define CW_SPIN_AFTER_MS 10000u

const char *cw_remove_busy(void) {
  if (spinning && !localOwed && SDL_GetTicks() - localDoneAt > CW_SPIN_AFTER_MS)
    spinning = 0;
  return spinning ? work : NULL;
}

void cw_remove_gone(void) { if (!localOwed) spinning = 0; }
