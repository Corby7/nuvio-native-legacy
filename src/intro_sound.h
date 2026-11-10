// The startup sound: art/sound/intro.wav, played with the opening animation
// (splash.c). Its hit lands on the core's landing, 360 ms in, so it starts on the
// animation's first frame and plays to its end (ended early, it fades out),
// past the splash, while the camera dives into the app.
//
// On every platform. webOS was held back until it was measured, and on the C3
// (2026-11-10) SDL's audio device opens in 30-100 ms on its thread and the sound
// starts 0-31 ms after the animation's first frame, well inside the 300 ms limit.
// Whether the hit lands on the slam BY EAR on a TV's audio path is the lead
// (NV_INTRO_SOUND_LEAD_MS, 0 there), which nobody has tuned yet.
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
