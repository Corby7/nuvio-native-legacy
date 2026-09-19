#include "ime.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// How long the platform gets to put something on screen before this module
// decides it never will. Long enough that a compositor busy with the first
// frames of the screen is not accused unfairly; short enough that the owner is
// not left staring at a field they cannot type into.
#define NV_IME_GIVE_UP 1500

static SDL_Window *win;
static int   open_;
static int   refused;        // the platform was asked and did not deliver
static int   sawText;        // an SDL_TEXTINPUT has arrived at least once
static Uint32 openedAt;
static int   saidSo;         // the one log line, not one per frame

void ime_start(SDL_Window *window) {
  win = window;
  open_ = refused = sawText = 0;
  openedAt = 0;
  saidSo = 0;
  // NOT SDL_StartTextInput here, even though SDL starts it by default on some
  // backends. SDL_StopTextInput at boot is the state this app wants everywhere
  // except the search field: every other screen reads the D-pad as keys, and a
  // backend that also delivers them as text would have the home reacting twice
  // to one press.
  SDL_StopTextInput();
#ifndef __APPLE__
  printf("[ime] screen keyboard support: %s\n",
         SDL_HasScreenKeyboardSupport() ? "yes" : "no");
#endif
}

int ime_usable(void) {
  // NUVIO_NO_IME=1 forces the fallback, and it is the ONLY way to see that half
  // of the search screen without a device that lacks a keyboard. The Mac always
  // has one, the TV is expected to have one, and the branch that runs when
  // neither is true would otherwise ship having never been drawn. Read fresh
  // every call rather than cached, so it can also be flipped at run time from a
  // debugger.
  if (getenv("NUVIO_NO_IME")) return 0;
#ifdef __APPLE__
  // See the note in ime.h: the preview has a real keyboard, so the app never
  // needs to draw one.
  return 1;
#else
  if (refused) return 0;
  return SDL_HasScreenKeyboardSupport() ? 1 : 0;
#endif
}

void ime_open(GfxRect field) {
  SDL_Rect r;
  if (open_) return;
  open_ = 1;
  openedAt = SDL_GetTicks();
  // The rectangle is in the layout canvas and SDL wants window pixels. On this
  // TV they are the same (the window IS 1920x1080; see applySurface in main.c),
  // and where they are not the hint being a few pixels out costs nothing —
  // which is why this does not reach into gfx.c's letterbox transform for a
  // value that is only ever advisory.
  r.x = (int)field.x;
  r.y = (int)field.y;
  r.w = (int)field.w;
  r.h = (int)field.h;
  SDL_SetTextInputRect(&r);
  SDL_StartTextInput();
}

void ime_close(void) {
  if (!open_) return;
  open_ = 0;
  SDL_StopTextInput();
}

int ime_is_open(void) { return open_; }

void ime_pump(void) {
#ifndef __APPLE__
  if (!open_ || refused || sawText) return;
  if (SDL_IsScreenKeyboardShown(win)) { sawText = 1; return; }
  if (SDL_GetTicks() - openedAt < NV_IME_GIVE_UP) return;
  // The platform said it had a keyboard, was asked for it, and produced
  // nothing. Refuse it for the rest of the session so the caller can put its
  // own keyboard back up; trying again on the next keypress would flicker
  // between the two.
  refused = 1;
  open_ = 0;
  SDL_StopTextInput();
  if (!saidSo) {
    saidSo = 1;
    printf("[ime] asked for the system keyboard and nothing came up after %d ms"
           " -- falling back to the drawn one for this session\n", NV_IME_GIVE_UP);
  }
#endif
}

int ime_edit(const SDL_Event *e, char *text, int *len, int max) {
  if (!text || !len) return 0;

  if (e->type == SDL_TEXTINPUT) {
    int add = (int)strlen(e->text.text);
    sawText = 1;
    if (add <= 0) return 1;
    // Refuse the WHOLE insertion rather than as much of it as fits: half a
    // UTF-8 sequence is not a shorter string, it is a broken one.
    if (*len + add + 1 > max) return 1;
    memcpy(text + *len, e->text.text, (size_t)add);
    *len += add;
    text[*len] = 0;
    return 1;
  }

  if (e->type == SDL_KEYDOWN && e->key.keysym.sym == SDLK_BACKSPACE) {
    int i = *len;
    if (i <= 0) return 1;
    // Step back over the continuation bytes (10xxxxxx) to the head of the last
    // character. See the note in ime.h for why one byte is not enough.
    do { i--; } while (i > 0 && ((unsigned char)text[i] & 0xC0) == 0x80);
    *len = i;
    text[i] = 0;
    return 1;
  }

  return 0;
}
