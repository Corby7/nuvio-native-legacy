// The startup animation: the play mark arrives as a 3D stepped pyramid, lands as
// the logo and hands over to the app.
//
// webOS shows deploy/app/splash.png (appinfo.json's splashBackground) from the
// launch until the app's first frame. That image is the bare ground (navy, with a
// purple glow from the bottom), and so is this module's first frame, so the moment
// the system's still gives way to the app is invisible. From there: the pyramid
// slams down from close to the camera, tilted to show its sides, and lands flat
// in its place in the lockup (frames rendered in Blender, played from
// intro_atlas.png); the hit holds a beat, kicks the camera and lights the core
// from within; a wave runs through the letters; the light breathes while the app
// gets ready; and the camera dives through the "o" into the app, drawn underneath.
//
// It waits for the screen under it to have something to show (see
// app_ready_to_show) instead of landing on "Preparing your catalogue…", but
// never longer than NV_SPLASH_MAX_MS. It cannot be skipped: keys and clicks
// during it are swallowed, because a key pressed at an overlay you cannot see
// through must not move a focus you cannot see either.
#ifndef NV_SPLASH_H
#define NV_SPLASH_H
#include <SDL2/SDL.h>

// Loads the lockup and the intro atlas SYNCHRONOUSLY, before the first frame.
// Through the texture cache they would arrive a few frames late, and the opening
// would start with frames missing. NUVIO_INTRO=0 in the
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
