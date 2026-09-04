// Does signing out REALLY erase the previous user?
//
// The defect: session_exit() deleted only the token. This test builds a genuine
// signed-in state (session, addons from the account, saved active profile,
// progress on disk), signs out, and checks item by item. Compiling proves
// nothing here — the defect was precisely absent code, which compiles
// perfectly.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "data.h"
#include "cloud.h"
#include "session.h"
#include "profiles.h"
#include "sync.h"
#include "addons.h"
#include "trakt.h"
#include "js.h"

static int failures;
static void checks(const char *what, int ok, const char *detail) {
  printf("  %-42s %s%s%s\n", what, ok ? "ok" : "FAILED",
         detail && *detail ? "  " : "", detail ? detail : "");
  if (!ok) failures++;
}
static int exists(const char *name) {
  char *b = data_read(name);
  int e = b != NULL;
  free(b);
  return e;
}

int main(void) {
  char *r, access_[3000] = {0}, renew[3000] = {0}, file[6400];
  int st = 0, i;

  data_start(getenv("NUVIO_DATA"));
  if (!cloud_configure(NULL)) return 2;
  r = cloud_post("/auth/v1/signup", "{\"data\":{\"tv_client\":\"webos\"}}", NULL, &st);
  if (!r || st >= 300) { printf("anonymous session failed\n"); return 3; }
  js_text(r, r + strlen(r), "access_token", access_, sizeof access_);
  js_text(r, r + strlen(r), "refresh_token", renew, sizeof renew);
  free(r);
  snprintf(file, sizeof file, "%s\n%s\n0\n", access_, renew);
  data_write("session.txt", file);
  session_start();

  // The state of "a user using the app": sync brings addons, they pick a
  // profile and watch something.
  sync_start();
  for (i = 0; i < 60 && sync_state() == SYNC_RUNNING; i++) usleep(500000);
  sync_step(1000);
  profiles_set_active(3);
  trakt_set("token-of-whoever-was-signed-in", "app-client");
  data_write("progress.txt", "tt0111161\t1200\t8520\n");

  printf("\nBEFORE signing out:\n");
  printf("  addons=%d  trakt=%d  active profile=%d  profile.txt=%d  progress.txt=%d\n",
         addons_n(), trakt_active(), profiles_active(),
         exists("profile.txt"), exists("progress.txt"));
  if (addons_n() == 0)
    printf("  WARNING: the test account came with no addon; the addon check proves nothing\n");

  session_exit();
  sync_forget_user();

  printf("\nAFTER signing out:\n");
  checks("session ended",              !session_loggedin(), "");
  checks("session.txt erased",            !exists("session.txt"), "");
  checks("addon list empty",         addons_n() == 0, "");
  checks("Trakt desligado",               !trakt_active(), "");
  checks("no profile in memory",      profiles_n() == 0, "");
  checks("active profile back to 1",    profiles_active() == 1, "");
  checks("profile.txt erased",            !exists("profile.txt"), "");
  checks("progress.txt erased",         !exists("progress.txt"), "");
  checks("account owner forgotten",       profiles_owner()[0] == 0, "");

  // The next cycle must NOT reapply what was left in the thread's box.
  sync_step(2000);
  checks("sync_step does not resurrect addons", addons_n() == 0, "");

  printf("\n%s\n", failures ? "HAS FAILURES" : "EVERYTHING ERASED");
  return failures ? 1 : 0;
}
