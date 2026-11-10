#include "intro_sound.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>

// On every platform, webOS included (see intro_sound.h for what was measured).
#define NV_INTRO_SOUND 1

// HOW EARLY THE SOUND STARTS, in ms: that much of the file's head is skipped, so
// the hit still lands on the slam when the platform's audio path is slower than
// its picture. The head is a near-silent rush (the first ~110 ms are silence).
// NUVIO_INTRO_SOUND_LEAD overrides it, for tuning by ear.
#ifdef __ANDROID__
#define NV_INTRO_SOUND_LEAD_MS 60
#else
#define NV_INTRO_SOUND_LEAD_MS 0
#endif
#define NV_INTRO_SOUND_FADE_MS 80    // ended early, it fades out over this
// LATER THAN THIS INTO THE ANIMATION, NO SOUND. Opening the audio device takes
// 200 ms and more (measured on the Mac: 212), so it happens on a thread and the
// first frame never waits for it. A sound that is ready a little late starts that
// far into the file, still in step; one ready after this, just before the hit at
// 360 ms, would land off the slam, and silence is better than that.
#define NV_INTRO_SOUND_LATEST_MS 300

static SDL_AudioDeviceID dev;
static Uint8 *wav;
static Uint32 wavLen;
static SDL_AudioSpec spec;
static char artDir[600];
// The loader thread and the main thread meet here: whoever comes second starts it.
static SDL_mutex *lock;
static int ready, wanted, dropped, started;
static Uint32 wantedAt;
// Read and written by the audio callback, and set from the main thread: SDL_atomic
// rather than plain ints, the callback runs on the audio thread.
static SDL_atomic_t pos;        // bytes played
static SDL_atomic_t fading;     // 1 once an early end asked for the fade
static SDL_atomic_t finished;   // 1 once the callback has run out of sound
static int fadeLeft, fadeTotal; // in frames; the callback's own

static void callback(void *user, Uint8 *out, int len) {
  const int frame = (SDL_AUDIO_BITSIZE(spec.format) / 8) * spec.channels;
  int p = SDL_AtomicGet(&pos), n = len, i;
  (void)user;
  SDL_memset(out, spec.silence, len);
  if (SDL_AtomicGet(&finished)) return;
  if (p + n > (int)wavLen) n = (int)wavLen - p;
  if (n > 0) SDL_memcpy(out, wav + p, n);
  if (SDL_AtomicGet(&fading) && spec.format == AUDIO_S16SYS) {
    Sint16 *s = (Sint16 *)out;
    if (!fadeTotal) fadeTotal = fadeLeft = spec.freq * NV_INTRO_SOUND_FADE_MS / 1000;
    for (i = 0; i < n / frame; i++) {
      int c;
      float g = fadeLeft > 0 ? (float)fadeLeft / fadeTotal : 0.0f;
      for (c = 0; c < spec.channels; c++) s[i * spec.channels + c] = (Sint16)(s[i * spec.channels + c] * g);
      if (fadeLeft > 0) fadeLeft--;
    }
    if (fadeLeft <= 0) n = (int)wavLen - p;   // faded out: done
  }
  SDL_AtomicSet(&pos, p + (n > 0 ? n : 0));
  if (n <= 0 || p + n >= (int)wavLen) SDL_AtomicSet(&finished, 1);
}

static void closeAll(void) {
  if (dev) SDL_CloseAudioDevice(dev);
  if (wav) SDL_FreeWAV(wav);
  dev = 0; wav = NULL;
}

// Closing the device must not happen on the audio thread, and the main loop has
// moved on by the time the sound ends: this thread waits for the end and closes it.
static int closer(void *unused) {
  int i;
  (void)unused;
  for (i = 0; i < 400 && !SDL_AtomicGet(&finished); i++) SDL_Delay(10);
  SDL_LockMutex(lock);
  closeAll();
  SDL_UnlockMutex(lock);
  return 0;
}

// Under the lock, once the sound is loaded AND the first frame has been drawn.
static void startLocked(void) {
  const char *env = getenv("NUVIO_INTRO_SOUND_LEAD");
  int lead = env ? atoi(env) : NV_INTRO_SOUND_LEAD_MS;
  int late = (int)(SDL_GetTicks() - wantedAt);
  int at = late + (lead < 0 ? 0 : lead > 100 ? 100 : lead);
  int frame = (SDL_AUDIO_BITSIZE(spec.format) / 8) * spec.channels;
  SDL_Thread *th;
  if (at > NV_INTRO_SOUND_LATEST_MS) {
    printf("[intro-sound] ready %d ms into the animation: too late, silent\n", late);
    closeAll();
    return;
  }
  started = 1;
  SDL_AtomicSet(&pos, spec.freq * at / 1000 * frame);
  SDL_PauseAudioDevice(dev, 0);
  printf("[intro-sound] playing from %d ms (%d ms after the first frame)\n", at, late);
  fflush(stdout);
  th = SDL_CreateThread(closer, "intro-sound-close", NULL);
  if (th) SDL_DetachThread(th);
}

static int loader(void *unused) {
  char path[640];
  Uint32 t = SDL_GetTicks();
  SDL_AudioDeviceID d = 0;
  (void)unused;
  if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
    printf("[intro-sound] no audio: %s\n", SDL_GetError());
    return 0;
  }
  snprintf(path, sizeof path, "%s/sound/intro.wav", artDir);
  if (!SDL_LoadWAV(path, &spec, &wav, &wavLen)) {
    printf("[intro-sound] no sound at %s: %s\n", path, SDL_GetError());
    return 0;
  }
  spec.callback = callback;
  spec.userdata = NULL;
  spec.samples = 1024;
  d = SDL_OpenAudioDevice(NULL, 0, &spec, NULL, 0);
  SDL_LockMutex(lock);
  dev = d;
  if (!dev) {
    printf("[intro-sound] no audio device: %s\n", SDL_GetError());
    closeAll();
  } else if (dropped) {
    closeAll();
  } else {
    ready = 1;
    printf("[intro-sound] ready in %u ms\n", (unsigned)(SDL_GetTicks() - t));
    if (wanted) startLocked();
  }
  SDL_UnlockMutex(lock);
  fflush(stdout);
  return 0;
}

void intro_sound_load(const char *dirArt) {
  SDL_Thread *th;
  if (!NV_INTRO_SOUND || lock) return;
  lock = SDL_CreateMutex();
  if (!lock) return;
  snprintf(artDir, sizeof artDir, "%s", dirArt ? dirArt : ".");
  th = SDL_CreateThread(loader, "intro-sound", NULL);
  if (th) SDL_DetachThread(th);
}

void intro_sound_play(void) {
  if (!lock) return;
  SDL_LockMutex(lock);
  if (!dropped && !wanted) {
    wanted = 1;
    wantedAt = SDL_GetTicks();
    if (ready) startLocked();
  }
  SDL_UnlockMutex(lock);
}

void intro_sound_stop(void) {
  if (!lock) return;
  SDL_LockMutex(lock);
  if (started) SDL_AtomicSet(&fading, 1);
  else { dropped = 1; if (ready) closeAll(); }
  SDL_UnlockMutex(lock);
}

void intro_sound_drop(void) {
  if (!lock) return;
  SDL_LockMutex(lock);
  dropped = 1;
  if (ready && !started) closeAll();
  SDL_UnlockMutex(lock);
}
