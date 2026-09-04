// Profile picker — the screen between login and home.
//
// It appears ONCE per installation (the choice is saved) and only when the
// account has more than one profile. On a single-person account, showing this
// would be a question with one possible answer.
//
// The PIN is verified ON THE SERVER (verify_profile_pin). Keeping the PIN here
// to compare locally would mean keeping the secret on the device — and a locked
// profile exists precisely so that the device cannot open it on its own.
#ifndef NV_PROFILESEL_H
#define NV_PROFILESEL_H
#include <SDL2/SDL.h>

void profilesel_start(void);
void profilesel_event(const SDL_Event *e);
void profilesel_update(float dt, Uint32 now);
void profilesel_draw(Uint32 now);
int  profilesel_done(void);
int  profilesel_wants_exit(void);
int  profilesel_requested_retry(void);

#endif
