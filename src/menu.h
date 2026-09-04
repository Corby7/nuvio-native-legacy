// The side menu (the Apple TV app's "sidebar" on the LG).
//
// It is not a screen: it is a layer that appears OVER the content and takes the
// D-pad while it is visible. That is why the API departs from the screen pattern
// in two places, on purpose:
//   - there is no menu_shutdown: text and icons use text.c's and gfx.c's caches;
//     the module owns no allocations of its own.
//   - there is no menu_wants_exit: closing the menu never closes the app. Back
//     here only hands focus back to the content, and the home remains the one
//     that decides about leaving.
//
// The cycle on the device: focus is on the first column of a row, the user
// presses LEFT, the bar slides in from the edge and takes focus. RIGHT or OK
// picks the destination and hands focus back to the content.
#ifndef NV_MENU_H
#define NV_MENU_H
#include <SDL2/SDL.h>

// The app's destinations, in the order they appear on the bar. MENU_N closes the
// enum for anyone who wants to size an array per destination without repeating
// the number 4.
// THE REFERENCE'S ORDER: Home first, then Search. Search used to come before
// Home, which puts the secondary action above the default destination — and on a
// D-pad that means getting back to the home costs one step more than going to
// search.
typedef enum {
  MENU_START,
  MENU_FETCH,
  MENU_LIBRARY,
  MENU_PROFILE,
  MENU_SETTINGS,
  MENU_N
} MenuDestination;

// Resets the destination and the animation. It is only needed if the app
// reinitialises the UI; the initial state is already valid without calling it
// (destination = MENU_START, bar out).
int  menu_start(void);

// Slides the bar in AND hands it the focus. Calling it with the menu already
// open does nothing, so it is safe to wire straight to the home's LEFT.
void menu_open(void);
// Closes without choosing: the highlight goes back to the current destination.
void menu_close(void);

// 1 while the bar owns the D-pad. It becomes 0 at the instant of the choice,
// with the exit animation still running — that is the signal the content should
// use to start responding to keys again, otherwise the D-pad is dead during the
// retraction.
int  menu_is_open(void);
// 1 while there is still a pixel to draw (the exit included). It lets the loop
// decide whether calling menu_draw is worth it at all.
int  menu_visible(void);

// The destination in force (a MenuDestination). menu_set_destination exists so
// the app can impose the initial state or react to navigation that did not come
// from the bar.
int  menu_destination(void);
void menu_set_destination(int destination);
// 1 exactly once, on the frame where the user chose a destination DIFFERENT from
// the one in force. Consumes the flag: whoever reads it, handles it. Without
// this the app would have to keep the previous destination just to discover it
// had changed.
int  menu_changed_destination(void);

const char *menu_label(int destination);

// 1 exactly once, on the frame where the user chose the footer ("switch user").
// Consumes the flag, like menu_changed_destination. It is deliberately not a
// MenuDestination: switching profile is not a tab of the app, it is an action
// that returns the person to the picker screen.
int  menu_requested_swap(void);

void menu_event(const SDL_Event *e);
void menu_update(float dt, Uint32 now);
// Desenhe por ULTIMO: o menu escurece e cobre tudo que veio antes.
void menu_draw(Uint32 now);

#endif
