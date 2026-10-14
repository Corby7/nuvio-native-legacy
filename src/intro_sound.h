// The startup sound: art/sound/intro.wav, played with the opening animation
// (splash.c). Its hit lands on the pyramid's slam, 360 ms in, so it starts on the
// animation's first frame and plays to its end (ended early, it fades out),
// past the splash, while the camera dives into the app.
//
// Only on the Mac and Android for now: on webOS the media pipeline owns the
// audio, and whether a short sound can play at launch without delaying the first
// frame has not been measured there yet.
#ifndef NV_INTRO_SOUND_H
#define NV_INTRO_SOUND_H

// Starts loading the sound and opening the audio device, on a thread: that takes
// 200 ms and more, and the first frame must not wait for it. Does nothing where
// the sound is off (see above).
void intro_sound_load(const char *dirArt);
// On the animation's first frame: starts it now, or as soon as it is loaded,
// that far into the file so it stays in step; not at all if that is too late.
void intro_sound_play(void);
// The animation ended early (the dev channel): fades it out over a few frames
// instead of cutting it (a click).
void intro_sound_stop(void);
// Not played after all (reduced motion, the setting off, no animation): frees it.
void intro_sound_drop(void);

#endif
