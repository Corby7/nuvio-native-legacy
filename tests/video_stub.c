// A stand-in for video.c in the review harnesses: a pipeline that always
// plays. It reports a 1080p stream with one EAC3 5.1 audio track, and hands
// the drawing a generated picture through video_frame_texture — the same hook
// the Mac preview uses for its decoded frame — so the overlays over a PLAYING
// channel can be captured off the TV. Nothing here is linked into the app.
#include "video.h"
#include "gl_compat.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The subtitle colour tables, the same as video.c's.
const char *const VIDEO_SUB_COLORS[VIDEO_SUB_NCOLORS] = {
  "white", "yellow", "green", "blue", "red", "black"
};
const char *const VIDEO_SUB_COLORS_LABEL[VIDEO_SUB_NCOLORS] = {
  "White", "Yellow", "Green", "Blue", "Red", "Black"
};

static unsigned session, tex;
static int active, paused, plays;
static char lastUrl[4200];
static VideoTrack audio = { "English \xC2\xB7 EAC3 5.1", "en", 1, "", 0 };

int  video_start(void) { return 1; }
int  video_launch_youtube(const char *id) { (void)id; return 0; }
int  video_play(const char *url) {
  session++; active = 1; paused = 0; plays++;
  snprintf(lastUrl, sizeof lastUrl, "%s", url ? url : "");
  return 1;
}
// For the harnesses: what was last handed over, how many loads, paused or not.
const char *video_stub_url(void) { return lastUrl; }
int video_stub_plays(void) { return plays; }
int video_stub_paused(void) { return paused; }
void video_pump(void) {}
void video_stop(void) { active = 0; }
void video_pause(int p) { paused = p; }
void video_fetch(double s) { (void)s; }
void video_window(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }
void video_window_source(int sx, int sy, int sw, int sh, int x, int y, int w, int h) {
  (void)sx; (void)sy; (void)sw; (void)sh; (void)x; (void)y; (void)w; (void)h;
}
double video_pos(void) { return 0; }
double video_duration(void) { return 0; }
double video_buffer_end(void) { return 0; }
void video_set_dv(int dv) { (void)dv; }
void video_set_mp4(int m) { (void)m; }
unsigned video_session(void) { return session; }
int video_playing(void) { return active; }
int video_ready(void) { return active; }
int video_active(void) { return active; }
int video_n_audio(void) { return 1; }
int video_n_subtitle(void) { return 0; }
const VideoTrack *video_audio(int i) { return i == 0 ? &audio : NULL; }
const VideoTrack *video_subtitle(int i) { (void)i; return NULL; }
int video_audio_current(void) { return 0; }
int video_subtitle_current(void) { return -1; }
const char *video_language_name(const char *c) { (void)c; return "English"; }
int video_frame_rate_milli(void) { return 50000; }
void video_choose_audio(int i) { (void)i; }
void video_choose_subtitle(int i) { (void)i; }
void video_subtitle_external(const char *u) { (void)u; }
void video_normalize_url_subtitle(const char *u, char *d, unsigned n) { (void)u; if (n) d[0] = 0; }
int video_mkv_head(MkvHead *c, char *u, unsigned n) { (void)c; (void)u; (void)n; return 0; }
int video_mkv_waiting(void) { return 0; }
void video_mkv_hurry(void) {}
void video_subtitle_style(const VideoSubtitleStyle *e) { (void)e; }
void video_subtitle_lift(int s) { (void)s; }
int video_error_count(void) { return 0; }
void video_last_error(char *d, unsigned n) { if (n) d[0] = 0; }
int video_eos_count(void) { return 0; }
int video_has_atmos(void) { return 0; }
int video_has_dolby_vision(void) { return 0; }
const char *video_hdr(void) { return "none"; }
int video_width(void) { return active ? 1920 : 0; }
int video_height(void) { return active ? 1080 : 0; }
const char *video_engine(void) { return "stub"; }
void video_shutdown(void) {}

// A floodlit pitch at night: green radial falloff, two soft figures. Made once.
unsigned video_frame_texture(void) {
  enum { W = 480, H = 270 };
  unsigned char *p;
  if (!active) return 0;
  if (tex) return tex;
  if (!(p = malloc(W * H * 4))) return 0;
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      float dx = (x - W * 0.5f) / (W * 0.6f), dy = (y - H * 0.78f) / (H * 0.65f);
      float r = sqrtf(dx * dx + dy * dy), g = 1.0f - r;
      float top = y < H * 0.4f ? (H * 0.4f - y) / (H * 0.4f) : 0.0f;
      float R = 7 + 40 * g, G = 19 + 88 * g, B = 12 + 50 * g;
      unsigned char *q = p + (y * W + x) * 4;
      if (g < 0) { R = 7; G = 19; B = 12; }
      R *= 1 - 0.5f * top; G *= 1 - 0.5f * top; B *= 1 - 0.5f * top;
      // Two figures.
      { float f1 = (x - 110) * (x - 110) / 90.0f + (y - 150) * (y - 150) / 900.0f;
        float f2 = (x - 300) * (x - 300) / 70.0f + (y - 140) * (y - 140) / 800.0f;
        if (f1 < 1) { R = 220; G = 230; B = 232; }
        if (f2 < 1) { R = 200; G = 120; B = 90; } }
      q[0] = (unsigned char)(R > 255 ? 255 : R); q[1] = (unsigned char)(G > 255 ? 255 : G);
      q[2] = (unsigned char)(B > 255 ? 255 : B); q[3] = 255;
    }
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, p);
  free(p);
  return tex;
}
