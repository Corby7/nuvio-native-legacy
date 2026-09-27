// The Live TV screen, driven without a TV: a playlist and a gzipped guide
// served over file:// (tests/iptv_ui.sh writes both around the current time),
// the D-pad fed as SDL events, and a capture of each face into /tmp for review.
//
// It asserts what can be asserted without eyes: the load reaches the guide, the
// focus model moves as documented at the top of iptvui.c, favourites and the
// setup form round-trip through the data folder. The captures are for the eyes.
#include "iptvui.h"
#include "iptv.h"
#include "data.h"
#include "gfx.h"
#include "ime.h"
#include "net.h"
#include "phonelink.h"
#include "pointer.h"
#include "tex_cache.h"
#include "settings.h"
#include "text.h"
#include "tracks.h"
#include <SDL2/SDL_image.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static SDL_Window *win;

// tests/video_stub.c's inspection hooks.
const char *video_stub_url(void);
int video_stub_plays(void);
int video_stub_paused(void);

static void key(SDL_Keycode k) {
  SDL_Event e;
  memset(&e, 0, sizeof e);
  e.type = SDL_KEYDOWN; e.key.keysym.sym = k;
  // As app.c routes them: the tracks sheet, when open, owns the keys.
  if (tracks_is_open()) { tracks_event(&e); e.type = SDL_KEYUP; tracks_event(&e); return; }
  iptvui_event(&e);
  e.type = SDL_KEYUP;
  iptvui_event(&e);
}

static void frames(int n, const char *capture) {
  for (int i = 0; i < n; i++) {
    SDL_PumpEvents();
    pointer_frame_begin();
    txt_new_frame(); tex_new_frame(); tex_pump(6);
    iptvui_update(1.0f / 60, SDL_GetTicks());
    tracks_update(1.0f / 60, SDL_GetTicks());
    glClearColor(0.051f, 0.051f, 0.051f, 1); glClear(GL_COLOR_BUFFER_BIT);
    iptvui_draw(SDL_GetTicks());
    tracks_draw(SDL_GetTicks());
    if (capture && i == n - 1) {
      unsigned char *pix = malloc(1920 * 1080 * 4);
      SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, 1920, 1080, 32, SDL_PIXELFORMAT_RGBA32);
      assert(pix && s);
      glReadPixels(0, 0, 1920, 1080, GL_RGBA, GL_UNSIGNED_BYTE, pix);
      for (int y = 0; y < 1080; y++)
        memcpy((char *)s->pixels + y * s->pitch, pix + (1079 - y) * 1920 * 4, 1920 * 4);
      assert(SDL_SaveBMP(s, capture) == 0);
      SDL_FreeSurface(s); free(pix);
      printf("capture: %s\n", capture);
    }
    SDL_GL_SwapWindow(win);
    SDL_Delay(4);
  }
}

// Frames for `ms` of real time: the screen's timers run on the clock.
static void waitMs(Uint32 ms) {
  Uint32 t0 = SDL_GetTicks();
  while (SDL_GetTicks() - t0 < ms) frames(1, NULL);
}

static void number(const char *n) {
  for (; *n; n++) key(SDLK_0 + (*n - '0'));
  waitMs(1800);   // the digits land when the typing stops
}

// The byte a pause-buffer URL starts at.
static long long tsOffset(const char *u) {
  const char *p = strstr(u, "/ts/");
  unsigned s; long long b;
  assert(p && sscanf(p, "/ts/%u/%lld", &s, &b) == 2);
  return b;
}

// The phone's side: one form POST to the TV's listener on loopback. Returns
// the HTTP status.
static int phonePost(const char *body) {
  struct sockaddr_in a;
  char req[4096], res[8192], target[128];
  size_t got = 0;
  int fd = socket(AF_INET, SOCK_STREAM, 0), k, status = 0;
  const char *code = strstr(phonelink_url(), "?k=");
  assert(code);
  snprintf(target, sizeof target, "/save%s", code);
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons((unsigned short)phonelink_port());
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(connect(fd, (struct sockaddr *)&a, sizeof a) == 0);
  k = snprintf(req, sizeof req, "POST %s HTTP/1.1\r\nContent-Length: %zu\r\n\r\n%s",
               target, strlen(body), body);
  assert(send(fd, req, (size_t)k, 0) == k);
  for (ssize_t r; (r = recv(fd, res + got, sizeof res - 1 - got, 0)) > 0; ) got += (size_t)r;
  res[got] = 0;
  close(fd);
  sscanf(res, "HTTP/1.1 %d", &status);
  return status;
}

// Frames until `cond` holds, or fail after ~20 s.
#define WAIT_FOR(cond) do { int _i = 0; \
    while (!(cond) && _i++ < 1200) frames(1, NULL); \
    assert(cond); } while (0)

int main(int argc, char **argv) {
  const char *dataDir = argc > 1 ? argv[1] : "/tmp/nuvio-iptv-test";
  const char *out = argc > 2 ? argv[2] : "/tmp";
  char path[512];
  assert(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) == 0);
  IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG);
#ifdef __APPLE__
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
#else
  // The shaders are GLSL ES everywhere but the Mac (gl_compat.h), as on the TV.
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#endif
  win = SDL_CreateWindow("Nuvio: live tv review", 0, 0, 1920, 1080, SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
  assert(win);
  assert(SDL_GL_CreateContext(win));
  SDL_GL_SetSwapInterval(0);
  glViewport(0, 0, 1920, 1080); gfx_size_target(0, 0, 1920, 1080); assert(gfx_start());
  assert(txt_start("deploy/app", 1)); tex_start(64);
  gfx_icons_dir("deploy/app/art");
  data_start(dataDir);
  net_prepare();
  ime_start(win);

  // --- The guide ---------------------------------------------------------------
  // The pause buffer on (Settings → Playback → Live TV), for the local TS channel.
  settings_set_live(15, 0);
  iptvui_start();
  assert(iptv_configured());
  WAIT_FOR(iptv_guide_state() == IPTV_READY || iptv_guide_state() == IPTV_FAILED);
  printf("status: '%s'\n", iptv_status());
  assert(iptv_guide_state() == IPTV_READY);
  frames(2, NULL);   // the list lands on the next iptv_step
  { const IptvList *l = iptv_list();
    assert(l && l->nCh == 49 && l->nGroups == 5);
    assert(l->ch[12].catchup == IPTV_CATCHUP_SHIFT && !l->ch[0].catchup);
    assert(l->ch[0].nPg > 0); }
  frames(40, NULL);
  // The landing is the channel list (Y2).
  snprintf(path, sizeof path, "%s/nuvio-live-list.bmp", out); frames(30, path);
  // DOWN twice, then RIGHT: the Watch / Favourite actions beside the list.
  key(SDLK_DOWN); key(SDLK_DOWN); key(SDLK_RIGHT);
  snprintf(path, sizeof path, "%s/nuvio-live-list-actions.bmp", out); frames(30, path);
  key(SDLK_LEFT); key(SDLK_UP); key(SDLK_UP);

  // UP from the first row is the chips, UP again the header, whose first
  // button is the Guide toggle (Y1).
  key(SDLK_UP); key(SDLK_UP); key(SDLK_RETURN);
  key(SDLK_DOWN); key(SDLK_DOWN);
  snprintf(path, sizeof path, "%s/nuvio-live-guide.bmp", out); frames(30, path);

  // The focus model: DOWN keeps the instant, RIGHT walks programmes, LEFT
  // walks back to what is on now, then onto the channel's own cell, then the
  // side bar — two axes, no third rail (Y3).
  key(SDLK_DOWN); key(SDLK_DOWN);
  key(SDLK_RIGHT); key(SDLK_RIGHT);
  snprintf(path, sizeof path, "%s/nuvio-live-guide-later.bmp", out); frames(40, path);
  key(SDLK_LEFT); key(SDLK_LEFT);
  key(SDLK_LEFT);
  snprintf(path, sizeof path, "%s/nuvio-live-guide-channel.bmp", out); frames(20, path);
  assert(!iptvui_requested_menu());
  key(SDLK_LEFT);
  assert(iptvui_requested_menu());

  // The chips: UP from the top row, RIGHT through Favourites (empty) and Recent
  // (empty) to the playlist's News.
  key(SDLK_UP); key(SDLK_UP); key(SDLK_UP);
  key(SDLK_RIGHT);
  snprintf(path, sizeof path, "%s/nuvio-live-empty.bmp", out); frames(30, path);
  key(SDLK_RIGHT); key(SDLK_RIGHT);
  key(SDLK_DOWN);
  snprintf(path, sizeof path, "%s/nuvio-live-group.bmp", out); frames(40, path);

  // Favourites: a hold on OK. The hold fires on time, so run frames while held.
  { SDL_Event e; memset(&e, 0, sizeof e);
    e.type = SDL_KEYDOWN; e.key.keysym.sym = SDLK_RETURN; iptvui_event(&e);
    { Uint32 t0 = SDL_GetTicks(); while (SDL_GetTicks() - t0 < 800) frames(1, NULL); }
    e.type = SDL_KEYUP; iptvui_event(&e); }
  { int favs = 0;
    for (int c = 0; c < iptv_list()->nCh; c++) favs += iptv_is_favourite(c);
    assert(favs == 1); }
  { char *f = data_read("iptv_favourites.txt"); assert(f && f[0]); free(f); }

  // --- Watching: Y4's bar and Y5's four levels ---------------------------------
  // (tests/video_stub.c plays everything, with a generated picture.)
  key(SDLK_RETURN);
  assert(iptvui_fullscreen());
  // Arriving, the bar.
  snprintf(path, sizeof path, "%s/nuvio-live-bar.bmp", out); frames(30, path);
  { char *before = data_read("iptv_recent.txt"), *after;
    // 3 · Peek: ▲▼ with the bar open browses the channels, and the stream stays.
    key(SDLK_DOWN); key(SDLK_DOWN);
    snprintf(path, sizeof path, "%s/nuvio-live-peek.bmp", out); frames(40, path);
    after = data_read("iptv_recent.txt");
    assert(before && after && !strcmp(before, after));
    free(after);
    // BACK to stay: still the same channel.
    key(SDLK_AC_BACK);
    frames(10, NULL);
    after = data_read("iptv_recent.txt");
    assert(!strcmp(before, after));
    free(after);
    // 4 · Walk: ◀▶ with the bar open shows a later programme; OK reminds.
    key(SDLK_RIGHT);
    snprintf(path, sizeof path, "%s/nuvio-live-walk.bmp", out); frames(30, path);
    key(SDLK_RIGHT);
    key(SDLK_RETURN);
    snprintf(path, sizeof path, "%s/nuvio-live-walk-remind.bmp", out); frames(20, path);
    after = data_read("iptv_recent.txt");
    assert(!strcmp(before, after));
    free(after);
    key(SDLK_LEFT); key(SDLK_LEFT);
    // OK on the bar: the controls.
    key(SDLK_RETURN);
    snprintf(path, sizeof path, "%s/nuvio-live-bar-controls.bmp", out); frames(20, path);
    key(SDLK_AC_BACK);
    // BACK hides the bar; ▲▼ now zaps, and the toast names where it landed.
    key(SDLK_AC_BACK);
    frames(20, NULL);
    // A lone press tunes on the frame it lands, not after the zap wait.
    SDL_Delay(400);
    key(SDLK_DOWN);
    frames(1, NULL);
    after = data_read("iptv_recent.txt");
    assert(strcmp(before, after));
    free(after);
    // A run of presses tunes only where it stops.
    free(before);
    before = data_read("iptv_recent.txt");
    key(SDLK_DOWN);
    frames(1, NULL);
    after = data_read("iptv_recent.txt");
    assert(!strcmp(before, after));
    free(after);
    snprintf(path, sizeof path, "%s/nuvio-live-toast.bmp", out); frames(30, path);
    after = data_read("iptv_recent.txt");
    assert(strcmp(before, after));   // the zap retuned, once the keys stopped
    free(after);
    // A number no longer number starts with tunes as its last digit lands:
    // the list runs 101-148, so 148 cannot grow and 14 can.
    free(before);
    before = data_read("iptv_recent.txt");
    key(SDLK_1); key(SDLK_4);
    frames(2, NULL);
    after = data_read("iptv_recent.txt");
    assert(!strcmp(before, after));
    free(after);
    key(SDLK_8);
    frames(2, NULL);
    after = data_read("iptv_recent.txt");
    assert(strcmp(before, after) && !strncmp(after, "Family", 6));
    free(after);
    // The peek's OK is the other thing that retunes.
    free(before);
    before = data_read("iptv_recent.txt");
    key(SDLK_RETURN); key(SDLK_UP); key(SDLK_RETURN);
    frames(10, NULL);
    after = data_read("iptv_recent.txt");
    assert(strcmp(before, after));
    free(after); free(before); }
  key(SDLK_AC_BACK);
  frames(10, NULL);
  // The channels panel: LEFT with nothing showing slides it in, the episode
  // panel's way — mid-slide, then settled, then walked and the group menu.
  key(SDLK_LEFT);
  snprintf(path, sizeof path, "%s/nuvio-live-channels-slide.bmp", out); frames(6, path);
  snprintf(path, sizeof path, "%s/nuvio-live-channels.bmp", out); frames(30, path);
  { char *before = data_read("iptv_recent.txt"), *after;
    key(SDLK_DOWN); key(SDLK_DOWN); key(SDLK_DOWN);
    snprintf(path, sizeof path, "%s/nuvio-live-channels-down.bmp", out); frames(30, path);
    for (int i = 0; i < 60; i++) key(SDLK_UP);   // past the first row: the pill
    key(SDLK_RETURN);
    snprintf(path, sizeof path, "%s/nuvio-live-channels-groups.bmp", out); frames(20, path);
    key(SDLK_AC_BACK);
    key(SDLK_RIGHT);   // ◀▶ step the group without the menu
    snprintf(path, sizeof path, "%s/nuvio-live-channels-group.bmp", out); frames(30, path);
    after = data_read("iptv_recent.txt");
    assert(before && after && !strcmp(before, after));   // browsing never retunes
    free(before); free(after); }
  key(SDLK_AC_BACK);
  frames(30, NULL);
  assert(iptvui_fullscreen());
  // From the bar's Channels control; BACK returns to the bar.
  key(SDLK_RETURN); key(SDLK_RETURN); key(SDLK_RIGHT); key(SDLK_RIGHT); key(SDLK_RETURN);
  snprintf(path, sizeof path, "%s/nuvio-live-channels-from-bar.bmp", out); frames(30, path);
  key(SDLK_AC_BACK);
  snprintf(path, sizeof path, "%s/nuvio-live-channels-back-to-bar.bmp", out); frames(30, path);
  // Subtitles: the sheet slides in and the bar steps out of its way.
  key(SDLK_RIGHT); key(SDLK_RETURN);
  assert(tracks_is_open());
  snprintf(path, sizeof path, "%s/nuvio-live-subs-sheet.bmp", out); frames(30, path);
  key(SDLK_AC_BACK);
  frames(30, NULL);
  assert(!tracks_is_open());
  key(SDLK_AC_BACK); key(SDLK_AC_BACK);
  frames(10, NULL);
  key(SDLK_AC_BACK);
  assert(!iptvui_fullscreen());

  // --- Pause and rewind ------------------------------------------------------------
  // 1 · A channel with neither a pause buffer (HLS) nor catch-up: a pause is
  // honest about resuming live, and a long one does.
  number("102");
  assert(iptvui_fullscreen() && strstr(video_stub_url(), "/live/2.m3u8"));
  key(SDLK_RETURN);            // the controls, Pause first
  key(SDLK_RETURN);            // pause
  assert(video_stub_paused());
  snprintf(path, sizeof path, "%s/nuvio-live-paused.bmp", out); frames(30, path);
  { int plays = video_stub_plays();
    waitMs(5300);
    key(SDLK_RETURN);          // play, past the short-pause window: a new live load
    assert(video_stub_plays() == plays + 1 && !video_stub_paused()); }
  key(SDLK_AC_BACK); key(SDLK_AC_BACK); frames(10, NULL);
  key(SDLK_AC_BACK);
  assert(!iptvui_fullscreen());

  // 2 · Catch-up (Sport keeps a day, "shift"): ◀ on the bar rewinds into the
  // archive, OK lands it; Go live and Start over from the controls.
  number("113");
  assert(iptvui_fullscreen() && !strstr(video_stub_url(), "utc="));
  key(SDLK_LEFT); key(SDLK_LEFT); key(SDLK_LEFT); key(SDLK_LEFT);
  snprintf(path, sizeof path, "%s/nuvio-live-scrub.bmp", out); frames(8, path);
  key(SDLK_RETURN);
  assert(strstr(video_stub_url(), "/live/13.m3u8?utc=") && strstr(video_stub_url(), "&lutc="));
  { long long utc = atoll(strstr(video_stub_url(), "utc=") + 4), now = (long long)time(NULL);
    assert(utc < now - 100 && utc > now - 3600); }
  snprintf(path, sizeof path, "%s/nuvio-live-catchup.bmp", out); frames(30, path);
  key(SDLK_RETURN);            // the controls: Pause, Start over, Go live, …
  key(SDLK_RIGHT); key(SDLK_RIGHT);
  snprintf(path, sizeof path, "%s/nuvio-live-catchup-controls.bmp", out); frames(20, path);
  key(SDLK_RETURN);            // Go live
  assert(!strstr(video_stub_url(), "utc="));
  key(SDLK_RIGHT); key(SDLK_RETURN);   // Start over
  { const IptvList *l = iptv_list();
    long long now = (long long)time(NULL), utc;
    int p = iptv_programme_at(l, 12, now);
    assert(p >= 0 && strstr(video_stub_url(), "utc="));
    utc = atoll(strstr(video_stub_url(), "utc=") + 4);
    assert(utc == l->pg[p].start); }
  // Out to the guide from the controls, still on Start over (then Go live,
  // Guide), then ◀ from now: the programme before, which the archive keeps;
  // OK plays it.
  key(SDLK_RIGHT); key(SDLK_RIGHT); key(SDLK_RETURN);
  assert(!iptvui_fullscreen());
  key(SDLK_LEFT);
  snprintf(path, sizeof path, "%s/nuvio-live-guide-catchup.bmp", out); frames(30, path);
  key(SDLK_RETURN);
  assert(iptvui_fullscreen() && strstr(video_stub_url(), "utc="));
  { const IptvList *l = iptv_list();
    long long utc = atoll(strstr(video_stub_url(), "utc=") + 4);
    int p = iptv_programme_at(l, 12, utc);
    assert(p >= 0 && l->pg[p].start == utc && l->pg[p].stop <= (long long)time(NULL) + 1); }
  key(SDLK_AC_BACK); frames(10, NULL); key(SDLK_AC_BACK);
  assert(!iptvui_fullscreen());

  // 3 · The pause buffer, on the local MPEG-TS channel: the pipeline plays the
  // loopback; a pause resumes in place; ◀ rewinds within what was kept.
  number("149");
  WAIT_FOR(strstr(video_stub_url(), "/ts/"));
  { long long live = tsOffset(video_stub_url()), back;
    int plays;
    waitMs(3000);
    key(SDLK_RETURN); key(SDLK_RETURN);        // pause
    assert(video_stub_paused());
    plays = video_stub_plays();
    waitMs(6500);                              // longer than a plain live pause may be
    key(SDLK_RETURN);                          // play: the same load, resumed
    assert(!video_stub_paused() && video_stub_plays() == plays);
    waitMs(10000);
    key(SDLK_AC_BACK);                         // the block, not the controls
    key(SDLK_LEFT);                            // back as far as the buffer goes
    key(SDLK_RETURN);
    back = tsOffset(video_stub_url());
    // Back to the oldest kept: where playing began, within a sample (0.5 s).
    assert(video_stub_plays() == plays + 1 && back < live + 188 * 5000);
    snprintf(path, sizeof path, "%s/nuvio-live-buffer.bmp", out); frames(30, path);
    key(SDLK_RETURN); key(SDLK_RIGHT);
    snprintf(path, sizeof path, "%s/nuvio-live-buffer-controls.bmp", out); frames(20, path);
    // Start over is not offered: the buffer began mid-programme. Go live is.
    key(SDLK_RETURN);
    assert(tsOffset(video_stub_url()) > back + 188 * 1000); }
  key(SDLK_AC_BACK); key(SDLK_AC_BACK); frames(10, NULL); key(SDLK_AC_BACK);
  assert(!iptvui_fullscreen());

  // 4 · Preview on focus: resting on a channel plays it, without counting it
  // as watched until OK.
  settings_set_live(15, 1);
  key(SDLK_AC_BACK);           // the guide (from the catch-up above) back to the list
  { char *before = data_read("iptv_recent.txt"), *after;
    const char *was;
    char seen[256];
    snprintf(seen, sizeof seen, "%s", video_stub_url());
    key(SDLK_UP);              // the list's focus is on 149, its last row
    waitMs(1400);
    was = video_stub_url();
    assert(strcmp(seen, was));                 // it tuned
    after = data_read("iptv_recent.txt");
    assert(!strcmp(before, after));            // and did not count it
    free(after);
    snprintf(path, sizeof path, "%s/nuvio-live-preview.bmp", out); frames(20, path);
    key(SDLK_RETURN);
    assert(iptvui_fullscreen());
    after = data_read("iptv_recent.txt");
    assert(strcmp(before, after));             // OK does
    free(before); free(after); }
  key(SDLK_AC_BACK); frames(10, NULL); key(SDLK_AC_BACK);
  assert(!iptvui_fullscreen());
  key(SDLK_AC_BACK);           // stop the preview, as Back does in the list
  settings_set_live(15, 0);

  // --- Setup ------------------------------------------------------------------------
  // To the header, RIGHT to Source, OK; then Xtream Codes.
  for (int i = 0; i < 60; i++) key(SDLK_UP);
  key(SDLK_RIGHT);
  key(SDLK_RETURN);
  key(SDLK_RIGHT);
  snprintf(path, sizeof path, "%s/nuvio-live-setup.bmp", out); frames(30, path);
  key(SDLK_AC_BACK);
  assert(!iptvui_wants_exit());
  frames(2, NULL);
  assert(phonelink_state() == PL_OFF);   // the form closed, and the listener with it

  // --- The phone form ----------------------------------------------------------------
  // Source again: the listener opens with the form. A phone saves the same
  // playlist as an M3U; the TV takes it as the Save button would.
  for (int i = 0; i < 12; i++) key(SDLK_UP);
  key(SDLK_RIGHT);
  key(SDLK_RETURN);
  snprintf(path, sizeof path, "%s/nuvio-live-setup-phone.bmp", out); frames(10, path);
  if (phonelink_state() == PL_OFF) {
    puts("note: no network address here, phone form not exercised");
    key(SDLK_AC_BACK);
  } else {
    char body[2400], enc[2200];
    size_t k = 0;
    for (const char *p = iptv_source()->url; *p && k + 4 < sizeof enc; p++)
      k += (size_t)snprintf(enc + k, sizeof enc - k, "%%%02X", (unsigned char)*p);
    snprintf(body, sizeof body, "kind=m3u&url=%s&epg=", enc);
    assert(phonePost(body) == 200);
    frames(3, NULL);
    assert(phonelink_state() == PL_OFF);             // saved, so the form left
    assert(iptv_source()->kind == IPTV_SRC_M3U);
    WAIT_FOR(iptv_list() && iptv_list()->nCh == 49);  // and the channels load again
  }

  iptvui_shutdown();
  puts("PASS iptv_ui: list and guide from file://, focus model, favourite, live bar levels (peek and walk never retune), "
       "pause (live, buffer), catch-up (scrub, go live, start over, guide), preview on focus, setup, phone form.");
  return 0;
}
