// Login screen: a large code, an address, and nothing else.
//
// It is the FIRST screen of the app when there is no account, and it covers
// everything. There is no text field: the keyboard in search.c only has
// "a-z0-9", no capitals, no "@" and no dot, so an e-mail and a password cannot
// be typed here. The phone does the typing; the TV only shows the code and
// waits.
#ifndef NV_LOGIN_H
#define NV_LOGIN_H
#include <SDL2/SDL.h>

void login_start(void);
void login_event(const SDL_Event *e);
void login_update(float dt, Uint32 now);
void login_draw(Uint32 now);

// 1 once the screen has finished its job (a session exists) and the app should move on.
int  login_done(void);

#endif
