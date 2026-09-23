// THE TV'S OWN KEYBOARD, for the one screen in this app that takes text.
//
// WHAT THIS REPLACES. search.c drew a 6x7 grid of letters and moved a cursor
// over it with the D-pad, because the top of that file said, correctly at the
// time: "this app is pure SDL and there is no IME to call". The web app on the
// same TV has no keyboard of its own — it puts an <input> on screen and the
// SYSTEM raises the LG keyboard over it, with the owner's layout, their
// history, their language and the voice button on the remote. That is the
// keyboard this screen should be using, and it is reachable from a native app:
// SDL's text-input API is the front door to whatever the platform provides.
//
// WHY IT IS A MODULE AND NOT FOUR LINES INSIDE search.c. Two reasons, and the
// second is the real one:
//
//   1. SDL_StartTextInput is global state on the SDL_Window. Leaving it on when
//      the screen closes means the next screen's D-pad presses may arrive as
//      SDL_TEXTINPUT as well as SDL_KEYDOWN, and a stray "d" typed into nothing
//      is the kind of defect that gets blamed on the remote.
//   2. A DEVICE MAY NOT HAVE ONE, so the question is asked at runtime and
//      nothing is lost when the answer is no: ime_usable() reports it and
//      search.c keeps its grid for exactly that case.
//
// MEASURED ON THE C3 (webOS 23, build 20260919-112930) — this half is no longer
// a hope, and the evidence is worth keeping because the first version of this
// file was written with the TV switched off and could only guess:
//
//   - /usr/lib/libSDL2-2.0.so.0 is LG's own build (2.0.14-90.webos4tv.43) and
//     its wayland backend EXPORTS the hooks: WebOSHasScreenKeyboardSupport,
//     WebOSShowScreenKeyboard, WebOSHideScreenKeyboard,
//     WebOSIsScreenKeyboardShown. Upstream SDL 2.0.14 has none of these on
//     wayland — this is LG's patch, and it is what makes the whole approach
//     possible.
//   - The app logs `[ime] screen keyboard support: yes` at boot.
//   - OK on the field raised the keyboard and the owner typed "goo" into it on
//     the remote: the text arrived as SDL_TEXTINPUT, the query updated and the
//     addons were queried. End to end, on the panel.
//
// THE KEYBOARD IS NOT IN A SCREENSHOT, and that costs hours if it is not
// written down. It is the compositor's own surface, like the splash and like
// the video plane — glReadPixels captures the app UNDERNEATH it and nothing
// else. A capture showing the search screen with no keyboard on it is NOT
// evidence that the keyboard failed to appear. Read the state instead: the
// absence of the give-up line below, or the query changing as someone types.
//
// HOW THE ANSWER IS OBTAINED, in order of how much it is worth trusting:
//   - SDL_HasScreenKeyboardSupport() says the backend has a screen keyboard at
//     all. On the device's SDL that is the one honest signal available before
//     trying, and it answers yes.
//   - ime_open() then asks for it and starts a stopwatch. If nothing comes back
//     — no SDL_IsScreenKeyboardShown, no SDL_TEXTINPUT — within NV_IME_GIVE_UP,
//     the module gives up FOR THE SESSION and ime_usable() turns 0. That is the
//     insurance against a backend that advertises support and then does
//     nothing, which would otherwise leave the owner on a search screen with no
//     way to type at all. It did not fire on this TV; it is there for the next
//     one.
#ifndef NV_IME_H
#define NV_IME_H
#include <SDL2/SDL.h>
#include "gfx.h"

// Call once, after the window exists. `window` is what SDL_IsScreenKeyboardShown
// needs; passing NULL only costs that one signal.
void ime_start(SDL_Window *window);

// 1 while the app should NOT draw a keyboard of its own.
//
// On the Mac this is 1 unconditionally and deliberately: there is no screen
// keyboard on macOS and there does not need to be, because the developer
// running the preview has a real one under their hands. Drawing a D-pad grid
// there would be noise, and text input works the same way through SDL either
// way.
//
// NUVIO_NO_IME=1 in the environment forces this to 0. It exists so the fallback
// half of the search screen can be seen at all on a machine that HAS a keyboard
// — see the note beside the check in ime.c.
int  ime_usable(void);

// Raises the keyboard and points it at `field`, the rectangle the text is being
// typed into, in the 1920x1080 layout canvas. The rectangle is a HINT — a
// platform is free to ignore it — but where it is honoured it is what stops the
// keyboard covering the field it belongs to.
void ime_open(GfxRect field);

// Lowers it. Safe to call when it is already down; call it on the way out of
// every screen that called ime_open, including the paths that leave through a
// Back key.
void ime_close(void);

// 1 between ime_open and ime_close, whether or not the platform actually put
// anything on screen.
int  ime_is_open(void);

// 1 while the platform's keyboard is actually ON SCREEN — not merely asked for.
// The LG keyboard can go down on its own (the owner steps off its top row, or
// presses its hide key) without the app calling ime_close; from then on the
// D-pad reaches the app again, and a screen that still gated its arrows on
// ime_is_open() left the field dead until Back. On the Mac there is no screen
// keyboard, so this is always 0 there.
int  ime_shown(void);

// Feeds one event to the text field. Returns 1 when the event was CONSUMED and
// the caller must not also act on it.
//
// `text` is the caller's buffer, `*len` its current length in bytes, `max` its
// size including the terminator. Handled here:
//   SDL_TEXTINPUT    appends, refusing anything that would not fit
//   SDL_KEYDOWN of BACKSPACE  removes one CHARACTER, not one byte
//
// WHY THE BACKSPACE LIVES HERE. The system keyboard sends real text, and real
// text from an LG keyboard is UTF-8: an accented letter is two bytes and an
// emoji is four. Deleting one byte from the end of those leaves a truncated
// sequence, which SDL_ttf renders as a replacement box that the next backspace
// then has to delete as well — the owner presses delete twice and the letter
// comes back as a square. Stepping back over the continuation bytes is the
// whole fix, and it belongs next to the code that appended them.
int  ime_edit(const SDL_Event *e, char *text, int *len, int max);

// Per-frame housekeeping: it is what notices that the keyboard never came up.
// Call once a frame while a screen has it open.
void ime_pump(void);

#endif
