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
  SCREEN_PLAYER, SCREEN_SOCIAL,
  // APPENDED, not inserted. Nothing indexes this enum by position today, but
  // the menu's destinations already learned that lesson once (see menu.h) and
  // the cost of appending is nothing.
  //
  // It is not a menu destination: the side bar has no Discover entry, exactly
  // as the web app's has none. The only way in is the compass in the search
  // header, and Back from it returns THERE and not to the home — which is why
  // app.c gives it a line of its own rather than the generic close.
  SCREEN_DISCOVER
} Screen;

int  app_start(const char *dirArt);
void app_event(const SDL_Event *e);
void app_update(float dt, Uint32 now);
void app_draw(Uint32 now);
int  app_wants_exit(void);
void app_shutdown(void);

// --- THE DEV CHANNEL --------------------------------------------------------
//
// Both of these exist because DRIVING THE APP BY INJECTED ARROW KEYS IS THE
// EXPENSIVE PART OF WORKING ON IT. Reaching one screen meant a blind sequence of
// up/down/ok followed by a screenshot to find out where it had actually landed,
// and the app restores a different focus on every launch, so the sequence is not
// even repeatable. Most captures taken while porting the detail screen were
// orientation, not evidence.
//
// app_goto opens a title's detail directly. `imdb` is the id ("tt12637874"); it
// answers 0 when the catalogue has no such title, which on a cold start simply
// means the rows have not arrived yet and the caller should try again.
int  app_goto_detail(const char *imdb);
// A one-line description of where the interface IS: screen, overlay, and the
// detail's own row/column when it is open. It changes rarely, so main.c prints it
// only when it differs from the last — which turns "where am I" from a screenshot
// into a grep.
void app_where(char *out, size_t n);
// Prints the named regions of whatever is on screen, so a capture can be cropped
// to one of them instead of by guesswork. See detail_regions in detail.h.
void app_regions(void);

#endif
