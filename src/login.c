#include "login.h"
#include "session.h"
#include "cloud.h"
#include "qr.h"
#include "gfx.h"
#include "text.h"
#include "anim.h"
#include "layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The QR code is the primary path, and not out of taste. MEASURED from the
// server response: the code is 32 hex digits and the URL ~63 characters. Nobody
// transcribes that from the TV to their phone without a typo — without the QR
// code this screen does not work.
#define LG_QR_SIDE       440.0f
#define LG_BLOCK_W      1100.0f
#define LG_PILL_W        360.0f
#define LG_PILL_H         76.0f

static float animButton;
static float pulse;

void login_start(void) {
  animButton = 0.0f;
  pulse = 0.0f;
  // Ask for the code NOW, without waiting for OK: someone who has just installed
  // the app has nothing to decide on this screen, and a "sign in" button before
  // the code only adds one keypress and a few seconds of waiting after it.
  if (!session_loggedin()) session_login_begin();
}

void login_event(const SDL_Event *e) {
  if (e->type != SDL_KEYDOWN) return;
  { SDL_Keycode k = e->key.keysym.sym;
    if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) {
      // OK only makes sense when there is something to redo. With the code on
      // screen it deliberately does nothing: restarting the flow here would
      // swap out the code the user just typed on their phone.
      if (session_state() == SESS_ERROR || session_state() == SESS_LOGGEDOUT)
        session_login_begin();
    } }
}

void login_update(float dt, Uint32 now) {
  session_step((unsigned)now);
  animButton = anim_spring(animButton, 1.0f, dt, NV_SPRING_FOCUS);
  pulse += dt;
}

static void lineCentered(TxtStyle st, const char *s, int r, int g, int b,
                          float y, float alpha) {
  TxtLine l = txt_line_trim(st, s, r, g, b, 255, LG_BLOCK_W);
  txt_draw_alpha(l, (NV_SCREEN_W - l.w) * 0.5f, y, alpha);
}

void login_draw(Uint32 now) {
  SessState st = session_state();
  float y;
  (void)now;

  { GfxRect screen = { 0, 0, NV_SCREEN_W, NV_SCREEN_H };
    gfx_color(screen, 0.0f, NV_COLOR_BACKGROUND_R, NV_COLOR_BACKGROUND_G, NV_COLOR_BACKGROUND_B, 1.0f); }

  y = 118.0f;
  lineCentered(TXT_TITLE1, "Sign in to your account", 255, 255, 255, y, 1.0f);
  y += 118.0f;

  if (!cloud_ready()) {
    // This case is a BUILD problem, not a user one: the package shipped without
    // the server configuration. Saying "sign-in error" would send the user
    // retrying forever against something that will never work.
    lineCentered(TXT_HEADLINE, "This package was built without a server.",
                  236, 108, 108, y, 1.0f);
    lineCentered(TXT_BODY,
                  "Whoever built the .ipk needs to supply the project URL and key.",
                  176, 178, 186, y + 62.0f, 1.0f);
    return;
  }

  switch (st) {
    case SESS_REQUESTING:
      lineCentered(TXT_HEADLINE, "Preparing the code…", 210, 212, 220, y, 1.0f);
      break;

    case SESS_WAITING: {
      const char *url = session_url_login();
      // Cached by qr.c and keyed on the text, so calling it every frame with the
      // same URL uploads nothing.
      GLuint texQr = qr_texture(url);
      lineCentered(TXT_BODY, "Point your phone camera at the code:",
                    176, 178, 186, y, 1.0f);
      y += 62.0f;

      if (texQr) {
        // A light frame slightly larger than the symbol: against the screen's
        // dark background the texture's own quiet zone would already be enough,
        // but the rounded frame makes the block read as a card rather than a
        // white hole in the middle of the screen.
        GfxRect frame = { (NV_SCREEN_W - LG_QR_SIDE - 32.0f) * 0.5f, y - 16.0f,
                            LG_QR_SIDE + 32.0f, LG_QR_SIDE + 32.0f };
        GfxRect r = { (NV_SCREEN_W - LG_QR_SIDE) * 0.5f, y, LG_QR_SIDE, LG_QR_SIDE };
        gfx_color(frame, 0.06f, 1.0f, 1.0f, 1.0f, 1.0f);
        gfx_tex_aspect_current = 0.0f;   // 1:1, no cropping
        gfx_rect(r, texQr, GFX_SNAP, 0, 0.0f, 0.0f, 0.0f, 0, 0, 0, 1.0f);
      } else {
        lineCentered(TXT_HEADLINE, "could not draw the code",
                      236, 108, 108, y + 100.0f, 1.0f);
      }
      y += LG_QR_SIDE + 42.0f;

      // The address in plain text is the escape hatch for anyone without a
      // camera — it is not the primary path, hence the small type.
      if (url[0]) lineCentered(TXT_CAPTION, url, 150, 152, 160, y, 1.0f);
      y += 46.0f;

      // A sign of life. Without it the screen sits still for minutes and looks
      // frozen — and the user restarts the app mid-login. A slow breath (2s
      // cycle), not a blink: blinking on waiting text reads as an alert.
      { float a = 0.5f + 0.5f * sinf(pulse * 3.14159f);
        lineCentered(TXT_CAPTION, "Waiting for authorisation…",
                      150, 152, 160, y, 0.45f + 0.40f * a); }
      break;
    }

    case SESS_SWITCHING:
      lineCentered(TXT_HEADLINE, "Authorised. Signing in…", 210, 212, 220, y, 1.0f);
      break;

    case SESS_LOGGEDIN:
      lineCentered(TXT_HEADLINE, "Ready.", 210, 212, 220, y, 1.0f);
      break;

    case SESS_ERROR:
    case SESS_LOGGEDOUT:
    default: {
      const char *msg = session_error();
      lineCentered(TXT_HEADLINE, msg[0] ? msg : "Could not reach the server.",
                    236, 108, 108, y, 1.0f);
      y += 96.0f;
      { GfxRect pill = { (NV_SCREEN_W - LG_PILL_W) * 0.5f, y, LG_PILL_W, LG_PILL_H };
        TxtLine t;
        gfx_color(pill, NV_RADIUS_PILL, 1.0f, 1.0f, 1.0f, 0.92f * animButton);
        t = txt_line(TXT_BODY, "Try again", 24, 24, 26, 255);
        txt_draw_alpha(t, (NV_SCREEN_W - t.w) * 0.5f,
                           y + (LG_PILL_H - t.h) * 0.5f, animButton); }
      break;
    }
  }
}

int login_done(void) { return session_loggedin(); }
