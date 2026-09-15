#include "profile_select.h"
#include "profiles.h"
#include "sync.h"
#include "gfx.h"
#include "text.h"
#include "tex_cache.h"
#include "anim.h"
#include "layout.h"
#include "settings.h"
#include "session.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>

#define PS_PIN_MAX       8
#define PS_KEY         96.0f
#define PS_KEY_GAP     18.0f

static int focus;
static int done, wantsExit, retry;
static float animFocus[ACCOUNT_PROFILE_MAX];

// THE BACKGROUND IS THE FOCUSED PROFILE'S COLOUR. updateBackground() in
// profileSelectionScreen.js tweens the accent over 520ms whenever the cursor
// moves, and the gradient is rebuilt from it every frame (GFX_PROFILE_BG).
//
// It is tweened rather than sprung because the web app gives it a DURATION and a
// curve, not a spring, and the two do not look alike over half a second: a
// spring is already 95% there at 120ms.
static float bgR, bgG, bgB;                // what is on screen
static float bgFromR, bgFromG, bgFromB;    // where the tween started
static float bgToR, bgToG, bgToB;          // where it is going
static float bgT;                          // 0..1 across NV_PSEL_BG_MS
static int   bgHas;                        // 0 until the first colour lands

// PIN state: -1 = no profile is asking for a PIN.
static int pinOf = -1;
static char pin[PS_PIN_MAX + 1];
static int pinFocus;              // 0..9 digits, 10 = delete, 11 = confirm
static int pinWrong, pinNet;
static pthread_t threadPin;
static int verifying;
static _Atomic int resultPin; // 0 pending, 1 ok, -1 wrong PIN, -2 network
static _Atomic unsigned pinGeneration;
typedef struct { unsigned generation; int slot, index_; char value[PS_PIN_MAX + 1]; } PinTask;

static int colorOf(const char *hex, float *r, float *g, float *b) {
  unsigned v = 0;
  if (!hex || hex[0] != '#' || strlen(hex) < 7) return 0;
  if (sscanf(hex + 1, "%6x", &v) != 1) return 0;
  *r = ((v >> 16) & 255) / 255.0f;
  *g = ((v >> 8) & 255) / 255.0f;
  *b = (v & 255) / 255.0f;
  return 1;
}

// The colour the page is washed with for profile `i`, with the web app's
// fallback: DEFAULT_PROFILE_COLOR is --secondary-color #F5F5F5, used when the
// account stores no colour for the profile.
static void accentOf(int i, float *r, float *g, float *b) {
  const AccountProfile *p = profiles_item(i);
  *r = *g = *b = NV_PSEL_DEFAULT_RGB;
  if (p) colorOf(p->colorHex, r, g, b);
}

// The web app's fast-out-slow-in, ported as it is written there
// (profileSelectionScreen.js:217): the cubic x(s) = 0.4s^3 - 0.6s^2 + 1.2s is
// inverted by six Newton steps and y(s) = 3s^2 - 2s^3 read off the result.
//
// Not replaced by anim_smooth, which IS that y curve but parameterised by t
// instead of s: over 520ms the difference is a visible third of the travel.
static float easeBg(float t) {
  float s = t;
  int i;
  if (t <= 0.0f) return 0.0f;
  if (t >= 1.0f) return 1.0f;
  for (i = 0; i < 6; i++) {
    float x = ((0.4f * s - 0.6f) * s + 1.2f) * s - t;
    float dx = (1.2f * s - 1.2f) * s + 1.2f;
    if (fabsf(dx) < 1e-6f) break;
    s -= x / dx;
  }
  return (3.0f - 2.0f * s) * s * s;
}

// Aim the wash at a new colour. The FIRST one lands instantly: the web app
// starts its tween from the target when there is no current colour, so the
// screen's first frame is already tinted rather than fading up from grey.
static void bgAim(float r, float g, float b) {
  if (!bgHas) {
    bgHas = 1;
    bgR = bgFromR = bgToR = r;
    bgG = bgFromG = bgToG = g;
    bgB = bgFromB = bgToB = b;
    bgT = 1.0f;
    return;
  }
  if (r == bgToR && g == bgToG && b == bgToB) return;
  bgFromR = bgR; bgFromG = bgG; bgFromB = bgB;
  bgToR = r; bgToG = g; bgToB = b;
  bgT = 0.0f;
}

void profilesel_start(void) {
  int i;
  focus = 0;
  done = wantsExit = retry = 0;
  pinOf = -1;
  pin[0] = 0;
  pinFocus = 0;
  pinWrong = 0;
  pinNet = 0;
  verifying = 0;
  bgHas = 0;
  atomic_fetch_add(&pinGeneration, 1);
  atomic_store(&resultPin, 0);
  for (i = 0; i < ACCOUNT_PROFILE_MAX; i++) animFocus[i] = 0.0f;
  // If the active profile is already known, start the focus on it: reopening
  // the screen and finding the cursor on the first profile suggests the choice
  // was lost.
  for (i = 0; i < profiles_n(); i++)
    if (profiles_item(i)->index_ == profiles_active()) { focus = i; break; }
}

static void *threadVerify(void *u) {
  PinTask *t = u;
  char body[128]; int status = 0, result = -2;
  snprintf(body, sizeof body, "{\"p_profile_id\":%d,\"p_pin\":\"%s\"}", t->index_, t->value);
  { char *r = session_rpc("verify_profile_pin", body, &status);
    if (r && status >= 200 && status < 300) result = strstr(r, "true") ? 1 : -1;
    free(r); }
  if (t->generation == atomic_load(&pinGeneration) && pinOf == t->slot && verifying)
    atomic_store(&resultPin, result);
  free(t);
  return NULL;
}

static void choose(int i) {
  const AccountProfile *p = profiles_item(i);
  if (!p) return;
  if (p->hasPin) { pinOf = i; pin[0] = 0; pinFocus = 0; pinWrong = pinNet = 0; return; }
  profiles_set_active(p->index_);
  done = 1;
}

static void eventPin(SDL_Keycode k) {
  if (verifying) {
    if (k == SDLK_AC_BACK || k == SDLK_ESCAPE) {
      atomic_fetch_add(&pinGeneration, 1); atomic_store(&resultPin, 0);
      verifying = 0; pinNet = 0; pinWrong = 0;
    }
    return;
  }
  if (k == SDLK_AC_BACK || k == SDLK_ESCAPE) {
    if (pin[0]) { pin[strlen(pin) - 1] = 0; pinWrong = pinNet = 0; }
    else pinOf = -1;
    return;
  }
  if (k == SDLK_LEFT)  { if (pinFocus > 0)  pinFocus--; return; }
  if (k == SDLK_RIGHT) { if (pinFocus < 11) pinFocus++; return; }
  if (k == SDLK_UP)    { if (pinFocus >= 5) pinFocus -= 5; return; }
  if (k == SDLK_DOWN)  { if (pinFocus + 5 <= 11) pinFocus += 5; return; }
  if (k != SDLK_RETURN && k != SDLK_KP_ENTER) return;

  if (pinFocus == 10) { if (pin[0]) pin[strlen(pin) - 1] = 0; return; }
  if (pinFocus == 11) {
    if (!pin[0]) return;
    PinTask *t = malloc(sizeof *t);
    const AccountProfile *p = profiles_item(pinOf);
    if (!t || !p) { free(t); pinNet = 1; return; }
    t->generation = atomic_load(&pinGeneration); t->slot = pinOf; t->index_ = p->index_;
    snprintf(t->value, sizeof t->value, "%s", pin);
    verifying = 1;
    pinWrong = pinNet = 0;
    atomic_store(&resultPin, 0);
    // Verifying BLOCKS (a round trip to the server). On a thread, so the screen
    // does not freeze for a second on every attempt.
    if (pthread_create(&threadPin, NULL, threadVerify, t) == 0) pthread_detach(threadPin);
    else { free(t); verifying = 0; pinNet = 1; }
    return;
  }
  { size_t n = strlen(pin);
    if (n < PS_PIN_MAX) { pin[n] = (char)('0' + pinFocus); pin[n + 1] = 0; } }
}

void profilesel_event(const SDL_Event *e) {
  SDL_Keycode k;
  int n = profiles_n();
  if (e->type != SDL_KEYDOWN) return;
  k = e->key.keysym.sym;
  if (pinOf >= 0) { eventPin(k); return; }

  if (k == SDLK_ESCAPE || k == SDLK_AC_BACK || k == SDLK_BACKSPACE) { wantsExit = 1; return; }
  if (sync_state() == SYNC_FAILED && (k == SDLK_RETURN || k == SDLK_KP_ENTER)) { retry = 1; return; }
  // LEFT and RIGHT walk the whole list, clamped at the ends, and UP/DOWN do
  // NOTHING — moveProfileFocus() in the web app returns false for both and the
  // cards are one flat list, so a second row is crossed sideways. Grid movement
  // was the port's own invention and it disagrees with the reference on the only
  // case where the two differ: the wrap.
  if (k == SDLK_RIGHT) { if (focus < n - 1) focus++; }
  else if (k == SDLK_LEFT) { if (focus > 0) focus--; }
  else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) choose(focus);
}

void profilesel_update(float dt, Uint32 now) {
  int i;
  (void)now;
  int reduced=settings_animations_reduced();
  for (i = 0; i < ACCOUNT_PROFILE_MAX; i++) {
    float target = (i == focus && pinOf < 0) ? 1.0f : 0.0f;
    animFocus[i] = anim_spring(animFocus[i], target, dt,
                            target > animFocus[i] ? NV_SPRING_FOCUS : NV_SPRING_BLUR);
    if(reduced)animFocus[i]=target;
  }

  // A sync that lands while the screen is open can SHORTEN the list, and the
  // cursor does not know: it was last clamped by a keypress, against the old
  // count. Left where it was it would wash the page in the default grey and
  // choose a profile that is no longer there.
  if (profiles_n() > 0 && focus > profiles_n() - 1) focus = profiles_n() - 1;
  if (focus < 0) focus = 0;

  // The wash follows the cursor, and while a PIN is being asked for it stays on
  // the profile that is asking — render() re-aims it at the locked profile, not
  // at whatever the grid behind the overlay has focus on.
  if (profiles_n() > 0) {
    float r, g, b;
    accentOf(pinOf >= 0 ? pinOf : focus, &r, &g, &b);
    bgAim(r, g, b);
  }
  if (bgHas && bgT < 1.0f) {
    float e;
    bgT = reduced ? 1.0f : bgT + dt * (1000.0f / NV_PSEL_BG_MS);
    if (bgT > 1.0f) bgT = 1.0f;
    e = easeBg(bgT);
    bgR = bgFromR + (bgToR - bgFromR) * e;
    bgG = bgFromG + (bgToG - bgFromG) * e;
    bgB = bgFromB + (bgToB - bgFromB) * e;
  }

  { int result = atomic_load(&resultPin);
  if (verifying && result) {
    atomic_store(&resultPin, 0);
    verifying = 0;
    if (result == 1) {
      const AccountProfile *p = profiles_item(pinOf);
      if (p) profiles_set_active(p->index_);
      pinOf = -1;
      done = 1;
    } else if (result == -2) {
      pinNet = 1;
      pin[0] = 0;
    } else {
      pinWrong = 1;
      pin[0] = 0;
    }
  }
  }
  // Do NOT finish while the cycle that FETCHES the profiles is still running.
  //
  // The defect this fixes: app.c switches to this screen right after calling
  // sync_start(), which is asynchronous. On the first frame profiles_n() is 0
  // because the answer has not arrived — and "0 profiles" is indistinguishable
  // from "a single-person account". The screen dismissed itself BEFORE it
  // existed, and a two-person account silently fell into profile 1: the app
  // synced and WROTE progress to the wrong profile, without ever asking.
  if (sync_state() == SYNC_RUNNING) return;

  // Once the cycle is done, "none or one" is a real answer: go straight
  // through. A network error is never the same as "an account with no
  // profiles". Only finish automatically when the cycle succeeded; that way the
  // next person does not silently fall into the implicit profile 1.
  if (sync_state() == SYNC_READY && profiles_n() <= 1) done = 1;
}

int profilesel_wants_exit(void) { int v=wantsExit; wantsExit=0; return v; }
int profilesel_requested_retry(void) { int v=retry; retry=0; return v; }

// --- drawing ----------------------------------------------------------------

// A rasterised line drawn into an arbitrary box, which txt_draw cannot do: it
// puts the texture down at its native size, on the pixel grid. The focused card
// is 5% larger than the one next to it and every glyph on it has to follow, so
// the focus reads as one object growing and not as a ring inflating around
// stationary text.
//
// At scale 1 it hands back to txt_draw, and that matters: three of the four
// cards on screen are at rest, and there the snapping is what keeps the names
// sharp (see the note above txt_draw_alpha).
static void drawScaled(TxtLine l, float x, float y, float scale, float alpha) {
  if (!l.tex || alpha <= 0.004f) return;
  if (scale > 0.999f && scale < 1.001f) { txt_draw_alpha(l, x, y, alpha); return; }
  gfx_tex_aspect_current = 0.0f;
  gfx_rect((GfxRect){ x, y, l.w * scale, l.h * scale }, l.tex, GFX_TEXT,
           0, 0, 0, 0.0f, 1, 1, 1, alpha);
}

// A line CENTRED in a CSS line box. The web app's vertical rhythm is line boxes
// (font-size x line-height), not ink: `.profile-name` occupies 40.8px whatever
// the name is, and the glyphs sit in the middle of it by half-leading. Anchoring
// to the ink instead makes a name with no descender ride high.
static void drawCentred(TxtLine l, float cx, float boxY, float boxH,
                        float scale, float alpha) {
  drawScaled(l, cx - l.w * scale * 0.5f,
             boxY + (boxH - l.h * scale) * 0.5f, scale, alpha);
}

// A CIRCULAR BORDER, drawn where CSS puts it: `border: Npx` sits INSIDE the box,
// from radius R/2-N to R/2.
//
// The quad is PADDED past the ring so the stroke and both of its antialiasing
// ramps land inside it. Without the padding there is nowhere for the outer ramp
// to go: it is drawn beyond the quad, the rasteriser drops it, and what is left
// is a hard edge cut along the quad's border — flat at 12, 3, 6 and 9 o'clock,
// where the square is tangent to the circle, and running out into the corners in
// between. MEASURED on the TV before this: white stopping dead at radius 128.1
// at the top and reaching 134 on the diagonal.
//
// Do not measure a ring on a horizontal line through its centre to check this.
// That line crosses at 3 and 9 o'clock — the two tangent points — where a clipped
// ring reads as EXACTLY right. Scan it by angle instead.
#define PS_RING_PAD 3.0f
static void strokeCircle(float cx, float cy, float outer, float thick,
                         float r, float g, float b, float a) {
  float q = outer + PS_RING_PAD * 2.0f;
  if (outer <= 0.0f || thick <= 0.0f) return;
  gfx_rect((GfxRect){ cx - q * 0.5f, cy - q * 0.5f, q, q }, 0, GFX_RING_CSS,
           0, (outer * 0.5f) / q, thick / q, 0.0f, r, g, b, a);
}

// Where everything lands. The web app bottom-anchors the whole column with
// `.profile-title { margin: auto 0 0 0 }`, so the block is measured from
// NV_PSEL_BOTTOM upwards and the grid's height is what moves the header.
typedef struct {
  float k;                  // metric scale; 1 on a single row
  int   cols, rows;
  float cardW, gap, cardH, pitch;
  float titleY, subY, gridY, hintY, gridH;
  int   logo;
} PsLayout;

static void psLayout(PsLayout *L, int n) {
  const float chrome = NV_PSEL_TITLE_H + NV_PSEL_TITLE_SUB + NV_PSEL_SUB_H +
                       NV_PSEL_SUB_GRID + NV_PSEL_GRID_HINT + NV_PSEL_HINT_H;
  // 384.8: the ring's 12px bottom margin COLLAPSES with the name's 24px top
  // margin, so the gap under the ring is 24 and not 36. Measured, and the reason
  // a card is 384.8 tall rather than 396.8.
  const float card1 = NV_PSEL_PAD_TOP + NV_PSEL_RING_MARGIN + NV_PSEL_RING +
                      NV_PSEL_NAME_GAP + NV_PSEL_NAME_H + NV_PSEL_BADGE_GAP +
                      NV_PSEL_BADGE_H + NV_PSEL_PAD_BOTTOM;
  float grid1, top, minTop;

  if (n < 1) n = 1;
  L->cols = n < NV_PSEL_COLS ? n : NV_PSEL_COLS;
  L->rows = (n + NV_PSEL_COLS - 1) / NV_PSEL_COLS;
  // The wordmark goes when the cards need two rows. There is no room for both:
  // the web app never faces this — its grid is a fixed 400px tall and a second
  // row simply overflows the screen — and an overflowing row is not a layout.
  L->logo = (L->rows == 1);

  grid1 = L->rows * card1 + (L->rows - 1) * NV_PSEL_GAP + NV_PSEL_GRID_SLACK;
  L->k = 1.0f;
  L->gridH = grid1;
  top = NV_PSEL_BOTTOM - (chrome + L->gridH);

  minTop = L->logo ? (NV_PSEL_LOGO_Y + NV_PSEL_LOGO_H + NV_PSEL_LOGO_GAP)
                   : NV_PSEL_TOP;
  if (top < minTop) {
    // Shrink the CARDS, never the header: the title and the hint are the same
    // size on every account, and a screen whose type changes with how many
    // people share it reads as two different screens.
    L->k = (NV_PSEL_BOTTOM - minTop - chrome) / grid1;
    if (L->k < 0.35f) L->k = 0.35f;
    L->gridH = grid1 * L->k;
    top = NV_PSEL_BOTTOM - (chrome + L->gridH);
  }

  L->cardW = NV_PSEL_CARD_W * L->k;
  L->gap   = NV_PSEL_GAP * L->k;
  L->cardH = card1 * L->k;
  L->pitch = L->cardW + L->gap;
  L->titleY = top;
  L->subY   = L->titleY + NV_PSEL_TITLE_H + NV_PSEL_TITLE_SUB;
  L->gridY  = L->subY + NV_PSEL_SUB_H + NV_PSEL_SUB_GRID;
  L->hintY  = L->gridY + L->gridH + NV_PSEL_GRID_HINT;
}

// The brand lockup: `.profile-logo` is 380x88 with `object-fit: contain`, and
// the art is 1085x344, so what is actually on screen is 277.6x88 centred in that
// box. The file is the web app's own asset, drawn through GFX_TEXT — the play
// mark is a colour gradient and gfx_icon's GFX_BRAND would flatten it to one
// tint, the trap gfx.h records for brand_mark.
static void drawLogo(void) {
  const char *path = gfx_icon_path("brand_lockup");
  float aspect = tex_aspect(path);
  float w = aspect > 0.0f ? NV_PSEL_LOGO_H * aspect : NV_PSEL_LOGO_W;
  GLuint tex;
  if (w > NV_PSEL_LOGO_W) w = NV_PSEL_LOGO_W;
  // The drawn width EXACTLY. tex_get_width's ceiling put this 1085-wide file on the
  // GPU at 480 to be drawn at 278 — a minification of 1.73, past the 1.414 where
  // GL_LINEAR_MIPMAP_NEAREST stops sampling level 0 and takes level 1 instead, so
  // the lockup on the profile picker was a 240px copy stretched back to 278. The
  // first request cannot know the width (see the same note in menu.c drawBrand);
  // it asks for the box and re-decodes once.
  tex = tex_get_exact(path, w);
  if (!tex || aspect <= 0.0f) return;
  gfx_tex_aspect_current = 0.0f;
  gfx_rect((GfxRect){ (NV_SCREEN_W - w) * 0.5f, NV_PSEL_LOGO_Y, w, NV_PSEL_LOGO_H },
           tex, GFX_TEXT, 0, 0, 0, 0.0f, 1, 1, 1, 1.0f);
}

// One card, drawn in its RESTING coordinates and then scaled about (centre, top).
// That origin is not the `center center` `.profile-card` declares: the rule that
// actually wins is `[class*="-card"].focusable.focused` and it computes to
// `center top`. Growing downwards only is why the row of names does not jump
// when the cursor moves along it.
static void drawCard(int i, const PsLayout *L, float x, float y) {
  const AccountProfile *p = profiles_item(i);
  const float k = L->k;
  const float f = animFocus[i];
  const float s = 1.0f + NV_PSEL_SCALE_F * f;
  const float cx = x + L->cardW * 0.5f;
  // Text is rasterised at the RESTING 1080p size, so a glyph drawn on this card
  // carries both factors: k (the card metric, 1 unless the account needs a
  // second row) and s (the focus transform).
  const float ts = s * k;
  float ring, border, av, ringY, ringX, nameY, badgeY;
  float cr, cg, cb;

  if (!p) return;
  ring   = anim_blend(NV_PSEL_RING, NV_PSEL_RING_F, f) * k;
  border = anim_blend(NV_PSEL_BORDER, NV_PSEL_BORDER_F, f) * k;
  av     = anim_blend(NV_PSEL_AVATAR, NV_PSEL_AVATAR_F, f) * k;
  ringY  = y + (NV_PSEL_PAD_TOP + NV_PSEL_RING_MARGIN) * k;
  ringX  = cx - ring * 0.5f;
  nameY  = ringY + ring + NV_PSEL_NAME_GAP * k;
  badgeY = nameY + NV_PSEL_NAME_H * k + NV_PSEL_BADGE_GAP * k;

  // Everything below is laid out in the card's RESTING coordinates and mapped
  // through the focus transform on the way out: uniform scale about (cx, y).
#define PSX(v) (cx + ((v) - cx) * s)
#define PSY(v) (y  + ((v) - y ) * s)

  { float ax = PSX(cx - av * 0.5f), ay = PSY(ringY + (ring - av) * 0.5f);
    float as = av * s;
    char url[420];
    GLuint tex = 0;
    int has = profiles_avatar(p, url, sizeof url);

    // THE COLOUR GOES UNDER THE PHOTO, not instead of it. `.profile-avatar`
    // keeps its background while the <img> sits on top of it, and that is not a
    // detail here: these avatars are PNGs on a TRANSPARENT ground, so the
    // profile's colour is exactly what shows through them. Drawing the photo
    // alone leaves the subject floating on the page's wash.
    accentOf(i, &cr, &cg, &cb);
    gfx_rect((GfxRect){ ax, ay, as, as }, 0, GFX_DISK, 0, 0, 0, 0.5f, cr, cg, cb, 1.0f);

    // The decode cap is asked for at the FOCUSED size and does not move with the
    // animation: tex_get_width sizes the decode when the slot is created, and a
    // width that breathed 192..204 every time the cursor landed would ask the
    // cache a slightly different question on every frame.
    if (has) tex = tex_get_width(url, NV_PSEL_AVATAR_F * k);
    if (tex) {
      // GFX_AVATAR, not GFX_CARD: an exact radial mask and `cover`, which is
      // what `.profile-avatar-image { object-fit: cover }` does. The aspect has
      // to be handed over or the art stretches to the square.
      gfx_tex_aspect_current = tex_aspect(url);
      gfx_rect((GfxRect){ ax, ay, as, as }, tex, GFX_AVATAR, 0, 0, 0, 0.0f, 1, 1, 1, 1.0f);
      gfx_tex_aspect_current = 0.0f;
    } else if (!has || tex_failed(url)) {
      // The initial. The web app shows it only when there is no picture at all,
      // and while one is in flight it shows the bare colour — so this waits for
      // the cache to GIVE UP (tex_failed) rather than drawing a letter that
      // would be replaced a moment later. A dead address is the one case where
      // this app does better than the reference, which is left with a broken
      // <img> and nothing in the circle.
      char start[8] = { p->name[0] ? p->name[0] : '?', 0 };
      float isc = anim_blend(1.0f, NV_PSEL_INITIAL_F, f);
      TxtLine l;
      // A leading multi-byte character is one glyph, not one byte: cut after the
      // first byte and the font is handed an invalid sequence.
      if ((unsigned char)start[0] >= 0xC0 && p->name[1]) { start[1] = p->name[1]; start[2] = 0; }
      l = txt_line(TXT_PSEL_INITIAL, start, 255, 255, 255, 255);
      drawScaled(l, PSX(cx - l.w * k * isc * 0.5f),
                 PSY(ringY + ring * 0.5f - l.h * k * isc * 0.5f), ts * isc, 1.0f);
    }
  }

  // The ring. rgba(51,51,51,0.75) at rest, --focus-color white when focused, and
  // it thickens 2 -> 6 at the same time.
  { float t = anim_blend(NV_PSEL_RING_RGB, 1.0f, f);
    strokeCircle(PSX(cx), PSY(ringY + ring * 0.5f), ring * s, border * s,
                 t, t, t, anim_blend(NV_PSEL_RING_A, 1.0f, f)); }

  // The primary marker, a gold disc with a star, offset against the ring's
  // PADDING box — inside the border, which is why it shifts as the border grows.
  if (p->primary) {
    float d  = NV_PSEL_DOT * k;
    float dx = ringX + ring - border + NV_PSEL_DOT_RIGHT * k - d;
    float dy = ringY + ring - border + NV_PSEL_DOT_BOTTOM * k - d;
    float bd = NV_PSEL_DOT_BORDER * k * s;
    float sx = PSX(dx), sy = PSY(dy), sd = d * s;
    // The 4px border is the page showing through, so it is drawn as a slightly
    // larger disc in the background colour rather than as a stroke.
    gfx_rect((GfxRect){ sx - bd, sy - bd, sd + bd * 2.0f, sd + bd * 2.0f },
             0, GFX_DISK, 0, 0, 0, 0.5f,
             NV_COLOR_BACKGROUND_R, NV_COLOR_BACKGROUND_G, NV_COLOR_BACKGROUND_B, 1.0f);
    gfx_rect((GfxRect){ sx, sy, sd, sd }, 0, GFX_DISK, 0, 0, 0, 0.5f,
             NV_PSEL_GOLD_R, NV_PSEL_GOLD_G, NV_PSEL_GOLD_B, 1.0f);
    { TxtLine l = txt_line(TXT_PSEL_STAR, "\xE2\x98\x85", 255, 255, 255, 255);
      drawScaled(l, PSX(dx + (d - l.w * k) * 0.5f),
                 PSY(dy + (d - l.h * k) * 0.5f), ts, 1.0f); }
  }

  // The name. --text-secondary at rest, --text-color focused, and the weight
  // steps 500 -> 600 the instant the card takes focus: font-weight is not in the
  // rule's `transition` list, so the web app pops it too.
  { int c = (int)(255.0f * anim_blend(NV_PSEL_NAME_RGB, 1.0f, f) + 0.5f);
    // The web app WRAPS a long name to a second line, which pushes that one
    // card's badge down. Trimming keeps the row of badges on one baseline; the
    // names this account holds are far from the limit either way. The width is
    // in RASTER units, so the card's 20px padding is taken off and k divided out.
    TxtLine l = txt_line_trim(f > 0.5f ? TXT_PSEL_NAME_F : TXT_PSEL_NAME,
                              p->name, c, c, c, 255,
                              (L->cardW - 40.0f * k) / k);
    drawScaled(l, PSX(cx - l.w * k * 0.5f),
               PSY(nameY + (NV_PSEL_NAME_H - l.h) * k * 0.5f), ts, 1.0f); }

  // "PRIMARY", tracked out 1.6px. The non-primary cards keep the slot empty
  // (`.profile-badge-slot`), which is what keeps every name on one line.
  if (p->primary) {
    static const char W[] = "PRIMARY";
    const float track = NV_PSEL_BADGE_TRACK * k;
    float wid = 0.0f, adv = 0.0f, bx;
    int j;
    // Measured first, because the line is centred: the tracking sits BETWEEN the
    // glyphs, so the width is not the cached line's.
    for (j = 0; W[j]; j++)
      wid += txt_line(TXT_PSEL_BADGE, (char[2]){ W[j], 0 }, 255, 255, 255, 255).w * k + track;
    if (wid > 0.0f) wid -= track;
    bx = cx - wid * 0.5f;
    for (j = 0; W[j]; j++) {
      TxtLine l = txt_line(TXT_PSEL_BADGE, (char[2]){ W[j], 0 },
                           (int)(NV_PSEL_GOLD_R * 255.0f), (int)(NV_PSEL_GOLD_G * 255.0f),
                           (int)(NV_PSEL_GOLD_B * 255.0f), 255);
      drawScaled(l, PSX(bx + adv),
                 PSY(badgeY + (NV_PSEL_BADGE_LINE - l.h) * k * 0.5f), ts, 1.0f);
      adv += l.w * k + track;
    }
  }

#undef PSX
#undef PSY
}

static void drawPin(void) {
  static const char *ROT[12] = { "0","1","2","3","4","5","6","7","8","9","\xE2\x86\x90","OK" };
  const AccountProfile *p = profiles_item(pinOf);
  float widthGrid = 5 * PS_KEY + 4 * PS_KEY_GAP;
  float x0 = (NV_SCREEN_W - widthGrid) * 0.5f;
  float y0 = 520.0f;
  int i;
  char mask[PS_PIN_MAX + 1];
  size_t n = strlen(pin), k;

  { GfxRect screen = { 0, 0, NV_SCREEN_W, NV_SCREEN_H };
    gfx_color(screen, 0.0f, 0.0f, 0.0f, 0.0f, 0.72f); }

  { char t[128];
    TxtLine l;
    snprintf(t, sizeof t, "PIN for %s", p ? p->name : "profile");
    l = txt_line(TXT_TITLE3, t, 255, 255, 255, 255);
    txt_draw(l, (NV_SCREEN_W - l.w) * 0.5f, 350.0f); }

  // Dots, never the digits: someone walking through the room need not read the PIN.
  for (k = 0; k < n && k < PS_PIN_MAX; k++) mask[k] = '*';
  mask[k] = 0;
  { TxtLine l = txt_line(TXT_TITLE1, n ? mask : "\xE2\x80\x94", 255, 255, 255, 255);
    txt_draw(l, (NV_SCREEN_W - l.w) * 0.5f, 420.0f); }

  if (pinNet) {
    TxtLine l = txt_line(TXT_BODY, "No connection. Try again.", 236, 150, 150, 255);
    txt_draw(l, (NV_SCREEN_W - l.w) * 0.5f, 480.0f);
  } else if (pinWrong) {
    TxtLine l = txt_line(TXT_BODY, "Incorrect PIN", 236, 108, 108, 255);
    txt_draw(l, (NV_SCREEN_W - l.w) * 0.5f, 480.0f);
  }
  if (verifying) {
    TxtLine l = txt_line(TXT_BODY, "checking\xE2\x80\xA6", 176, 178, 186, 255);
    txt_draw(l, (NV_SCREEN_W - l.w) * 0.5f, 480.0f);
  }

  for (i = 0; i < 12; i++) {
    int col = i % 5, lin = i / 5;
    GfxRect r = { x0 + col * (PS_KEY + PS_KEY_GAP),
                  y0 + lin * (PS_KEY + PS_KEY_GAP), PS_KEY, PS_KEY };
    int f = (i == pinFocus);
    TxtLine l;
    gfx_color(r, 0.22f, 1.0f, 1.0f, 1.0f, f ? 0.92f : 0.10f);
    l = txt_line(TXT_TITLE3, ROT[i], f ? 24 : 235, f ? 24 : 235, f ? 26 : 240, 255);
    txt_draw(l, r.x + (r.w - l.w) * 0.5f, r.y + (r.h - l.h) * 0.5f);
  }
}

void profilesel_draw(Uint32 now) {
  int i, n = profiles_n();
  PsLayout L;
  (void)now;

  // The page. One quad: the accent's two gradients live in the shader, because
  // both of them are built out of the same colour.
  gfx_rect((GfxRect){ 0, 0, NV_SCREEN_W, NV_SCREEN_H }, 0, GFX_PROFILE_BG,
           0, 0, 0, 0.0f,
           bgHas ? bgR : NV_COLOR_BACKGROUND_R,
           bgHas ? bgG : NV_COLOR_BACKGROUND_G,
           bgHas ? bgB : NV_COLOR_BACKGROUND_B, 1.0f);

  psLayout(&L, n);
  if (L.logo) drawLogo();

  { TxtLine t = txt_line(TXT_PSEL_TITLE, "Who's watching?", 255, 255, 255, 255);
    drawCentred(t, NV_SCREEN_W * 0.5f, L.titleY, NV_PSEL_TITLE_H, 1.0f, 1.0f); }

  // While the list has not arrived, say so IN THE SUBTITLE'S PLACE. A screen with
  // a title and nothing below it reads as a hang — and the web app never shows
  // this state at all, because it does not route here until the profiles exist.
  if (n == 0) {
    int c = (int)(NV_PSEL_NAME_RGB * 255);
    const char *msg = sync_state() == SYNC_FAILED
                        ? "Could not load the profiles. OK: try again"
                        : "Loading the profiles on your account\xE2\x80\xA6";
    TxtLine e = txt_line(TXT_PSEL_SUB, msg, c, c, c, 255);
    drawCentred(e, NV_SCREEN_W * 0.5f, L.subY, NV_PSEL_SUB_H, 1.0f, 1.0f);
    return;
  }

  { int c = (int)(NV_PSEL_NAME_RGB * 255);
    TxtLine s = txt_line(TXT_PSEL_SUB, "Select a profile to continue", c, c, c, 255);
    drawCentred(s, NV_SCREEN_W * 0.5f, L.subY, NV_PSEL_SUB_H, 1.0f, 1.0f); }

  // The rows wrap like `flex-wrap: wrap` with `justify-content: center`: each row
  // is centred on ITS OWN count, so a trailing row of two sits in the middle
  // rather than hanging off the left.
  for (i = 0; i < n; i++) {
    int row = i / NV_PSEL_COLS, col = i % NV_PSEL_COLS;
    int inRow = n - row * NV_PSEL_COLS;
    float rowW, x;
    if (inRow > NV_PSEL_COLS) inRow = NV_PSEL_COLS;
    rowW = inRow * L.cardW + (inRow - 1) * L.gap;
    x = (NV_SCREEN_W - rowW) * 0.5f + col * L.pitch;
    drawCard(i, &L, x, L.gridY + row * (L.cardH + L.gap));
  }

  // NO HINT LINE. The web app puts "Hold to manage profile" here; this app has
  // no management to hint at, and the D-pad line that stood in its place was
  // explaining a remote the viewer is already holding.
  //
  // The SPACE it occupied stays reserved (psLayout still counts
  // NV_PSEL_GRID_HINT + NV_PSEL_HINT_H). Reclaiming it would drop the whole
  // bottom-anchored column 86px down the screen and every measurement in this
  // file against the reference with it — the text is what was unwanted, not the
  // margin under the cards.

  if (pinOf >= 0) drawPin();
}

int profilesel_done(void) { return done; }
