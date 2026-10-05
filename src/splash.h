// The startup animation: the logo the system splash left on screen comes alive
// and hands over to the app.
//
// webOS shows deploy/app/splash.png (appinfo.json's splashBackground) from the
// launch until the app's first frame. This module's first frame is THAT IMAGE,
// redrawn from the brand lockup at the same place and on the same background, so
// the moment the system's still gives way to the app is invisible. From there:
// the wordmark wipes away, the play mark glides to the centre, presses like a
// button with a burst of its own colours, and the camera flies through it into the
// app, which is already drawn underneath.
//
// It waits for the screen under it to have something to show (see
// app_ready_to_show) instead of landing on "Preparing your catalogue…", but
// never longer than NV_SPLASH_MAX_MS. Any key or click skips it, and is
// swallowed: a key pressed at an overlay you cannot see through must not move a
// focus you cannot see either.
#ifndef NV_SPLASH_H
#define NV_SPLASH_H
#include <SDL2/SDL.h>

// Loads the lockup SYNCHRONOUSLY, before the first frame. Through the texture
// cache it would arrive a few frames late, and the splash's logo would blink off
// and back on at exactly the moment this exists to hide. NUVIO_INTRO=0 in the
// environment turns it off (tools/dev.sh does, so captures are deterministic).
void splash_start(const char *dirArt);
// 1 while the overlay still covers anything.
int  splash_active(void);
// Keys and clicks during the animation: 1 = consumed. Also swallows the KEYUP
// of a key whose KEYDOWN it took, after the animation has ended.
int  splash_event(const SDL_Event *e);
// Ends it at once, with no exit animation: for the dev channel's injected keys.
void splash_finish(void);
// Over everything else, last in the frame. `ready` = the screen under it has
// something to show.
void splash_draw(Uint32 now, int ready);
#endif
