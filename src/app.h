// Screen router.
//
// Until now main.c decided between home and detail on its own, with an if. With
// a menu, search, library, settings and player, that if would turn into a
// tangle where every screen has to know about the others. Here the rule lives
// in one place: there is a CURRENT screen, a return STACK, and each screen only
// says "I want out" or "open this title".
#ifndef NV_APP_H
#define NV_APP_H
#include <SDL2/SDL.h>

// SCREEN_LOGIN and SCREEN_PROFILE_PICKER are the exception to the priority
// order: while either is active NOTHING else draws or receives a key. Without
// an account there is no user catalogue, no addons and no progress — letting
// home show through behind would be presenting the package's sample content as
// if it were theirs.
//
// SCREEN_PROFILE_PICKER is the ACCOUNT's profile chooser; SCREEN_PROFILE, which
// already existed, is the Trakt statistics screen. Close names, different
// things.
typedef enum {
  SCREEN_LOGIN, SCREEN_CHOICE_PROFILE,
  SCREEN_HOME, SCREEN_SEARCH, SCREEN_LIBRARY, SCREEN_PROFILE, SCREEN_SETTINGS,
  SCREEN_PLAYER, SCREEN_SOCIAL
} Screen;

int  app_start(const char *dirArt);
void app_event(const SDL_Event *e);
void app_update(float dt, Uint32 now);
void app_draw(Uint32 now);
int  app_wants_exit(void);
void app_shutdown(void);

#endif
