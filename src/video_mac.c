// Video on the Mac preview build, through libmpv.
//
// The TV plays on a hardware plane behind the GL surface (video.c); the Mac has
// neither the plane nor the pipeline, so until this file the player there was a
// black hole with a frozen bar. This backend implements the same video.h
// contract with mpv, which plays everything the addons hand out (MKV, HEVC,
// AC3/DTS, debrid links) where AVFoundation would refuse most of it.
//
// mpv renders in SOFTWARE, on a thread of its own, into a buffer that the main
// thread uploads as a texture; player.c/home.c draw that texture where the TV
// punches its hole. The GL route was not taken on purpose: the Mac context is
// legacy OpenGL 2.1 (gl_compat.h), below what mpv's GPU renderer can count on.
// Rendering on the main thread was tried first and MEASURED: a 2160p source
// took the UI from 60 to ~40 fps. Off the main thread, the UI pays only the
// upload.
//
// Only compiled with NV_MPV, which tools/mac.sh defines when libmpv is installed
// (brew install mpv). Without it video.c keeps its stubs. The TV never builds
// this file's body.
//
// What does NOT carry over from the TV: HDR / Dolby Vision (always "none"
// here), Atmos detection, and the look of embedded subtitles, which mpv draws
// with its own renderer instead of the uMS.
#if defined(__APPLE__) && defined(NV_MPV)

#include "video.h"
#include "gl_compat.h"

#include <mpv/client.h>
#include <mpv/render.h>

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static mpv_handle         *mpv;
static mpv_render_context *render;

static unsigned session;
static int      active, ready, paused, seeking;
static double   posSeg, durationSeg, bufferSeg;
static int      vidW, vidH, rateMilli;
static int      errCount, eosCount;
static char     errLast[160];

static VideoTrack trackAudio[NV_TRACK_MAX], trackSub[NV_TRACK_MAX];
static int nAudio, nSub, audioCurrent, subCurrent = -1;

// Where the app wants the picture: the destination on the 1920x1080 screen and,
// for the zoom modes, the piece of the decoded frame to show (-1 = all of it).
static int dstW = 1920, dstH = 1080;
static int srcX = -1, srcY, srcW, srcH;
static char cropApplied[48];

// Two frame buffers: the render thread fills `back` and swaps it with `front`,
// which the main thread uploads into `tex`. Everything between the two threads
// goes through `lock`.
static pthread_t       worker;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  wake = PTHREAD_COND_INITIALIZER;
static int      workerRun, dirty, enabled, frontNew;
static int      wantW = 1920, wantH = 1080;
static uint8_t *back, *front;
static int      backW, backH, frontW, frontH;
static GLuint   tex;
static int      texW, texH, frameValid;

static void renderLoopStart(void);

static VideoSubtitleStyle style = { 120, 0, 0, 3, 1, 0, 0, 0 };
static int lift;

enum { OBS_TIME = 1, OBS_DURATION, OBS_CACHE, OBS_PAUSE, OBS_TRACKS, OBS_AID, OBS_SID };

static void setString(const char *name, const char *value) {
  int r = mpv_set_property_string(mpv, name, value);
  if (r < 0) printf("[mpv] %s=%s refused: %s\n", name, value, mpv_error_string(r));
}

int video_start(void) {
  mpv_render_param params[2];
  if (mpv) return 1;
  mpv = mpv_create();
  if (!mpv) { printf("[mpv] mpv_create failed\n"); return 0; }
  mpv_set_option_string(mpv, "vo", "libmpv");
  // The software renderer reads frames from system memory: a copy-back hwdec
  // keeps decoding on VideoToolbox without handing mpv a GPU surface it cannot use.
  mpv_set_option_string(mpv, "hwdec", "auto-copy");
  // The app already sizes the destination to the video's aspect (aspectVisible),
  // so mpv fills it instead of letterboxing a second time.
  mpv_set_option_string(mpv, "keepaspect", "no");
  mpv_set_option_string(mpv, "idle", "yes");
  // Addon URLs are direct files; a failed one should fail, not go to youtube-dl.
  mpv_set_option_string(mpv, "ytdl", "no");
  mpv_set_option_string(mpv, "terminal", "no");
  mpv_set_option_string(mpv, "input-default-bindings", "no");
  mpv_set_option_string(mpv, "audio-client-name", "Nuvio");
  mpv_set_option_string(mpv, "cache", "yes");
  mpv_set_option_string(mpv, "demuxer-max-bytes", "150MiB");
  if (mpv_initialize(mpv) < 0) {
    printf("[mpv] initialize failed\n");
    mpv_terminate_destroy(mpv); mpv = NULL;
    return 0;
  }
  mpv_request_log_messages(mpv, "warn");
  params[0].type = MPV_RENDER_PARAM_API_TYPE; params[0].data = MPV_RENDER_API_TYPE_SW;
  params[1].type = MPV_RENDER_PARAM_INVALID;  params[1].data = NULL;
  if (mpv_render_context_create(&render, mpv, params) < 0) {
    printf("[mpv] software render context refused\n");
    mpv_terminate_destroy(mpv); mpv = NULL; render = NULL;
    return 0;
  }
  renderLoopStart();
  mpv_observe_property(mpv, OBS_TIME,     "time-pos",           MPV_FORMAT_DOUBLE);
  mpv_observe_property(mpv, OBS_DURATION, "duration",           MPV_FORMAT_DOUBLE);
  mpv_observe_property(mpv, OBS_CACHE,    "demuxer-cache-time", MPV_FORMAT_DOUBLE);
  mpv_observe_property(mpv, OBS_PAUSE,    "pause",              MPV_FORMAT_FLAG);
  mpv_observe_property(mpv, OBS_TRACKS,   "track-list",         MPV_FORMAT_NONE);
  mpv_observe_property(mpv, OBS_AID,      "aid",                MPV_FORMAT_NONE);
  mpv_observe_property(mpv, OBS_SID,      "sid",                MPV_FORMAT_NONE);
  { char *v = mpv_get_property_string(mpv, "mpv-version");
    printf("[mpv] ready (%s)\n", v ? v : "?"); fflush(stdout); mpv_free(v); }
  video_subtitle_style(&style);
  return 1;
}

int video_launch_youtube(const char *id) {
  printf("[video] would launch YouTube v=%s\n", id ? id : ""); fflush(stdout);
  return 0;
}

static void resetSession(void) {
  ready = paused = seeking = 0;
  posSeg = durationSeg = bufferSeg = 0;
  vidW = vidH = rateMilli = 0;
  nAudio = nSub = 0; audioCurrent = 0; subCurrent = -1;
  srcX = -1; cropApplied[0] = 0;
  frameValid = 0;
  pthread_mutex_lock(&lock);
  enabled = 0; frontNew = 0;
  pthread_mutex_unlock(&lock);
}

int video_play(const char *url) {
  const char *cmd[4];
  if (!url || !*url) return 0;
  if (!video_start()) return 0;
  video_stop();
  session++;
  resetSession();
  setString("video-crop", "");
  setString("pause", "no");
  cmd[0] = "loadfile"; cmd[1] = url; cmd[2] = "replace"; cmd[3] = NULL;
  if (mpv_command(mpv, cmd) < 0) return 0;
  active = 1;
  printf("[mpv] load %.100s\n", url); fflush(stdout);
  return 1;
}
// The Mac preview does not reload on a network drop; the TV build does (video.c).
void video_reconnect(int on) { (void)on; }

void video_stop(void) {
  session++;
  if (mpv && active) {
    const char *cmd[] = { "stop", NULL };
    mpv_command(mpv, cmd);
  }
  active = 0;
  resetSession();
}

void video_pause(int p) {
  if (!mpv || !active) return;
  setString("pause", p ? "yes" : "no");
  paused = p ? 1 : 0;
}

void video_fetch(double seconds) {
  char s[32];
  const char *cmd[] = { "seek", s, "absolute", NULL };
  if (!mpv || !active) return;
  if (seconds < 0) seconds = 0;
  snprintf(s, sizeof s, "%.3f", seconds);
  // The bar moves at once, and holds there until mpv says the seek landed:
  // otherwise the time-pos updates still in flight snap it back for a frame.
  posSeg = seconds;
  seeking = 1;
  mpv_command_async(mpv, 0, cmd);
}

static void applyCrop(void) {
  char c[48] = "";
  if (!mpv || !active) return;
  if (srcX >= 0 && vidW > 0 && vidH > 0 &&
      !(srcX == 0 && srcY == 0 && srcW == vidW && srcH == vidH))
    snprintf(c, sizeof c, "%dx%d+%d+%d", srcW, srcH, srcX, srcY);
  if (!strcmp(c, cropApplied)) return;
  snprintf(cropApplied, sizeof cropApplied, "%s", c);
  setString("video-crop", c);
}

void video_window(int x, int y, int w, int h) {
  (void)x; (void)y;
  if (w < 1 || h < 1) return;
  dstW = w; dstH = h;
  srcX = -1;
  applyCrop();
}

void video_window_source(int sx, int sy, int sw, int sh, int dx, int dy, int dw, int dh) {
  (void)dx; (void)dy;
  if (dw < 1 || dh < 1 || sw < 1 || sh < 1) return;
  dstW = dw; dstH = dh;
  srcX = sx; srcY = sy; srcW = sw; srcH = sh;
  applyCrop();
}

// --- tracks ------------------------------------------------------------------

static int64_t propInt(const char *name, int64_t fallback) {
  int64_t v;
  return mpv_get_property(mpv, name, MPV_FORMAT_INT64, &v) >= 0 ? v : fallback;
}

static void propCopy(const char *name, char *dst, size_t n) {
  char *s = mpv_get_property_string(mpv, name);
  snprintf(dst, n, "%s", s ? s : "");
  mpv_free(s);
}

// mpv names the codec by its FFmpeg decoder; tracks.c tells text from bitmap
// subtitles by the Matroska CodecID the TV's header read supplies. Translate the
// ones that matter to that vocabulary.
static void subtitleCodec(const char *ff, char *dst, size_t n) {
  const char *m = "";
  if (!strcmp(ff, "subrip") || !strcmp(ff, "text")) m = "S_TEXT/UTF8";
  else if (!strcmp(ff, "ass") || !strcmp(ff, "ssa")) m = "S_TEXT/ASS";
  else if (!strcmp(ff, "webvtt")) m = "S_TEXT/WEBVTT";
  else if (!strcmp(ff, "hdmv_pgs_subtitle")) m = "S_HDMV/PGS";
  else if (!strcmp(ff, "dvd_subtitle")) m = "S_VOBSUB";
  else if (!strcmp(ff, "dvb_subtitle")) m = "S_DVBSUB";
  snprintf(dst, n, "%s", m);
}

static void readTracks(void) {
  int64_t count = propInt("track-list/count", 0), i;
  int64_t aid = propInt("aid", -1), sid = propInt("sid", -1);
  char key[64], type[16], lang[8], title[64], codec[32], ext[8];
  nAudio = nSub = 0; audioCurrent = 0; subCurrent = -1;
  for (i = 0; i < count; i++) {
    VideoTrack *f = NULL;
    int id;
    snprintf(key, sizeof key, "track-list/%lld/type", (long long)i);
    propCopy(key, type, sizeof type);
    snprintf(key, sizeof key, "track-list/%lld/id", (long long)i);
    id = (int)propInt(key, 0);
    snprintf(key, sizeof key, "track-list/%lld/lang", (long long)i);
    propCopy(key, lang, sizeof lang);
    snprintf(key, sizeof key, "track-list/%lld/title", (long long)i);
    propCopy(key, title, sizeof title);
    snprintf(key, sizeof key, "track-list/%lld/codec", (long long)i);
    propCopy(key, codec, sizeof codec);
    snprintf(key, sizeof key, "track-list/%lld/external", (long long)i);
    propCopy(key, ext, sizeof ext);

    if (!strcmp(type, "audio") && nAudio < NV_TRACK_MAX) {
      const char *ch = "";
      int64_t channels;
      f = &trackAudio[nAudio];
      memset(f, 0, sizeof *f);
      snprintf(key, sizeof key, "track-list/%lld/demux-channel-count", (long long)i);
      channels = propInt(key, 0);
      if (channels == 6) ch = "5.1";
      else if (channels == 8) ch = "7.1";
      else if (channels == 2) ch = "2.0";
      snprintf(f->language, sizeof f->language, "%s", lang);
      // The same shape the TV builds from sourceInfo: "English  ·  5.1".
      snprintf(f->label, sizeof f->label, "%s%s%s",
               lang[0] ? video_language_name(lang) : "Track",
               ch[0] ? "  \xc2\xb7  " : "", ch);
      f->number = id;
      if (id == aid) audioCurrent = nAudio;
      nAudio++;
    } else if (!strcmp(type, "sub") && strcmp(ext, "yes") && nSub < NV_TRACK_MAX) {
      // External subtitles (sub-add) stay out of this list, as on the TV:
      // tracks.c keeps the addon subtitle apart from the file's own.
      f = &trackSub[nSub];
      memset(f, 0, sizeof *f);
      snprintf(f->language, sizeof f->language, "%s", lang);
      subtitleCodec(codec, f->codec, sizeof f->codec);
      snprintf(key, sizeof key, "track-list/%lld/forced", (long long)i);
      { char forced[8]; propCopy(key, forced, sizeof forced); f->forced = !strcmp(forced, "yes"); }
      if (title[0])
        snprintf(f->label, sizeof f->label, "%s%s%s",
                 lang[0] ? video_language_name(lang) : "",
                 lang[0] ? "  \xc2\xb7  " : "", title);
      else if (lang[0])
        snprintf(f->label, sizeof f->label, "%s", video_language_name(lang));
      else
        snprintf(f->label, sizeof f->label, "Subtitle %d", nSub + 1);
      f->number = id;
      if (id == sid) subCurrent = nSub;
      nSub++;
    }
  }
}

int  video_n_audio(void)    { return nAudio; }
int  video_n_subtitle(void) { return nSub; }
const VideoTrack *video_audio(int i)    { return (i >= 0 && i < nAudio) ? &trackAudio[i] : NULL; }
const VideoTrack *video_subtitle(int i) { return (i >= 0 && i < nSub) ? &trackSub[i] : NULL; }
int  video_audio_current(void)    { return audioCurrent; }
int  video_subtitle_current(void) { return subCurrent; }

void video_choose_audio(int i) {
  const VideoTrack *f = video_audio(i);
  int64_t id;
  if (!mpv || !f) return;
  id = f->number;
  mpv_set_property(mpv, "aid", MPV_FORMAT_INT64, &id);
  audioCurrent = i;
}

void video_choose_subtitle(int i) {
  if (!mpv || !active) return;
  if (i < 0) { setString("sid", "no"); subCurrent = -1; return; }
  { const VideoTrack *f = video_subtitle(i);
    int64_t id;
    if (!f) return;
    id = f->number;
    mpv_set_property(mpv, "sid", MPV_FORMAT_INT64, &id);
    subCurrent = i; }
}

void video_subtitle_external(const char *url) {
  const char *cmd[] = { "sub-add", url, "select", NULL };
  if (!mpv || !active || !url || !*url) return;
  mpv_command_async(mpv, 0, cmd);
  printf("[mpv] external subtitle: %.80s\n", url); fflush(stdout);
}

// --- subtitle style ----------------------------------------------------------
// The sheet's indices, translated into mpv's options. mpv draws the cues itself,
// so this is an approximation of what the uMS shows, not a copy of it.

static void applyStyle(void) {
  static const char *const hex[VIDEO_SUB_NCOLORS] = {
    "FFFFFF", "FFFF00", "00FF00", "0000FF", "FF0000", "000000"
  };
  static const char *const alphaText[4] = { "FF", "BF", "80", "40" };
  static const char *const alphaBack[5] = { "00", "40", "80", "BF", "FF" };
  char v[32];
  int c = style.color, p;
  if (!mpv) return;
  if (c < 0 || c >= VIDEO_SUB_NCOLORS) c = 0;
  snprintf(v, sizeof v, "%.2f", (style.size > 0 ? style.size : 120) / 120.0);
  setString("sub-scale", v);
  snprintf(v, sizeof v, "#%s%s", alphaText[(style.opacity & 3)], hex[c]);
  setString("sub-color", v);
  if (style.background > 0 && style.background <= 4) {
    snprintf(v, sizeof v, "#%s000000", alphaBack[style.background]);
    setString("sub-back-color", v);
    setString("sub-border-style", "background-box");
  } else {
    setString("sub-border-style", "outline-and-shadow");
  }
  setString("sub-outline-size", style.border == 1 ? "1.65" : "0");
  setString("sub-shadow-offset", style.border == 2 ? "2" : "0");
  // The TV's scale moves the cue 48px a step from position 3 (the bottom);
  // on a 1080-line frame that is ~4.4% of mpv's sub-pos per step.
  p = style.position + lift;
  if (p < 0) p = 0;
  if (p > 7) p = 7;
  snprintf(v, sizeof v, "%d", 100 - (p - 3) * 44 / 10);
  setString("sub-pos", v);
  snprintf(v, sizeof v, "%.3f", style.delayMs / 1000.0);
  setString("sub-delay", v);
}

void video_subtitle_style(const VideoSubtitleStyle *e) {
  if (!e) return;
  style = *e;
  applyStyle();
}

void video_subtitle_lift(int steps) {
  if (steps == lift) return;
  lift = steps;
  applyStyle();
}

// --- events and state --------------------------------------------------------

void video_pump(void) {
  if (!mpv) return;
  for (;;) {
    mpv_event *ev = mpv_wait_event(mpv, 0);
    if (ev->event_id == MPV_EVENT_NONE) break;
    switch (ev->event_id) {
    case MPV_EVENT_FILE_LOADED:
      ready = 1;
      readTracks();
      printf("[mpv] loaded: audio=%d subtitle=%d\n", nAudio, nSub); fflush(stdout);
      break;
    case MPV_EVENT_VIDEO_RECONFIG: {
      double fps = 0;
      vidW = (int)propInt("width", 0);
      vidH = (int)propInt("height", 0);
      { char *hw = mpv_get_property_string(mpv, "hwdec-current");
        printf("[mpv] video %dx%d, hwdec %s\n", vidW, vidH, hw ? hw : "none");
        fflush(stdout); mpv_free(hw); }
      if (mpv_get_property(mpv, "container-fps", MPV_FORMAT_DOUBLE, &fps) >= 0)
        rateMilli = (int)(fps * 1000.0 + 0.5);
      applyCrop();
      break; }
    case MPV_EVENT_PLAYBACK_RESTART:
      seeking = 0;
      break;
    case MPV_EVENT_END_FILE: {
      mpv_event_end_file *end = ev->data;
      if (end->reason == MPV_END_FILE_REASON_EOF) {
        eosCount++;
      } else if (end->reason == MPV_END_FILE_REASON_ERROR) {
        errCount++;
        snprintf(errLast, sizeof errLast, "%s (code %d)",
                 mpv_error_string(end->error), end->error);
        printf("[mpv] error: %s\n", errLast); fflush(stdout);
      }
      break; }
    case MPV_EVENT_LOG_MESSAGE: {
      mpv_event_log_message *m = ev->data;
      printf("[mpv] %s: %s", m->prefix, m->text);
      break; }
    case MPV_EVENT_PROPERTY_CHANGE: {
      mpv_event_property *pr = ev->data;
      if (ev->reply_userdata == OBS_TIME && pr->format == MPV_FORMAT_DOUBLE) {
        if (!seeking) posSeg = *(double *)pr->data;
      } else if (ev->reply_userdata == OBS_DURATION && pr->format == MPV_FORMAT_DOUBLE) {
        durationSeg = *(double *)pr->data;
      } else if (ev->reply_userdata == OBS_CACHE && pr->format == MPV_FORMAT_DOUBLE) {
        bufferSeg = *(double *)pr->data;
      } else if (ev->reply_userdata == OBS_PAUSE && pr->format == MPV_FORMAT_FLAG) {
        paused = *(int *)pr->data;
      } else if ((ev->reply_userdata == OBS_TRACKS || ev->reply_userdata == OBS_AID ||
                  ev->reply_userdata == OBS_SID) && ready) {
        readTracks();
      }
      break; }
    default:
      break;
    }
  }
}

// --- the picture -------------------------------------------------------------

// mpv calls this from its own threads whenever there is something new to draw.
static void onUpdate(void *ctx) {
  (void)ctx;
  pthread_mutex_lock(&lock);
  dirty = 1;
  pthread_cond_signal(&wake);
  pthread_mutex_unlock(&lock);
}

// The render thread: the only caller of mpv_render_context_update/render, which
// must never run concurrently with each other.
static void *renderLoop(void *arg) {
  int lastW = 0, lastH = 0;
  (void)arg;
  pthread_mutex_lock(&lock);
  while (workerRun) {
    int w, h, on;
    uint64_t flags;
    while (workerRun && !dirty) pthread_cond_wait(&wake, &lock);
    if (!workerRun) break;
    dirty = 0; w = wantW; h = wantH; on = enabled;
    pthread_mutex_unlock(&lock);

    flags = mpv_render_context_update(render);
    // A new size redraws the current frame even with nothing new decoded:
    // otherwise a paused video would stay at the old size.
    if (on && ((flags & MPV_RENDER_UPDATE_FRAME) || w != lastW || h != lastH)) {
      size_t stride = (size_t)w * 4;
      if (w != backW || h != backH) {
        free(back); back = NULL; backW = backH = 0;
        if (posix_memalign((void **)&back, 64, stride * (size_t)h) == 0) { backW = w; backH = h; }
      }
      if (back) {
        int size[2] = { w, h };
        mpv_render_param params[] = {
          { MPV_RENDER_PARAM_SW_SIZE,    size },
          { MPV_RENDER_PARAM_SW_FORMAT,  "rgb0" },
          { MPV_RENDER_PARAM_SW_STRIDE,  &stride },
          { MPV_RENDER_PARAM_SW_POINTER, back },
          { MPV_RENDER_PARAM_INVALID,    NULL },
        };
        if (mpv_render_context_render(render, params) >= 0) {
          uint8_t *t;
          int tw, th;
          lastW = w; lastH = h;
          pthread_mutex_lock(&lock);
          t = front; front = back; back = t;
          tw = frontW; th = frontH; frontW = backW; frontH = backH; backW = tw; backH = th;
          frontNew = 1;
          pthread_mutex_unlock(&lock);
        }
      }
    }
    pthread_mutex_lock(&lock);
  }
  pthread_mutex_unlock(&lock);
  return NULL;
}

static void renderLoopStart(void) {
  workerRun = 1;
  if (pthread_create(&worker, NULL, renderLoop, NULL) != 0) {
    workerRun = 0;
    printf("[mpv] no render thread: video stays black\n");
    return;
  }
  mpv_render_context_set_update_callback(render, onUpdate, NULL);
}

unsigned video_frame_texture(void) {
  int w = dstW, h = dstH;
  if (!render || !active) return 0;
  if (w > 1920) w = 1920;
  if (h > 1080) h = 1080;
  if (w < 16) w = 16;
  if (h < 16) h = 16;
  pthread_mutex_lock(&lock);
  if (w != wantW || h != wantH || enabled != ready) {
    wantW = w; wantH = h; enabled = ready;
    dirty = 1;
    pthread_cond_signal(&wake);
  }
  if (frontNew && front) {
    if (!tex) {
      glGenTextures(1, &tex);
      glBindTexture(GL_TEXTURE_2D, tex);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    // "rgb0" leaves the fourth byte undefined. An RGB internal format makes the
    // sampler read alpha as 1, so the card shader does not fade the frame out.
    if (texW != frontW || texH != frontH) {
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, frontW, frontH, 0, GL_RGBA, GL_UNSIGNED_BYTE, front);
      texW = frontW; texH = frontH;
    } else {
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frontW, frontH, GL_RGBA, GL_UNSIGNED_BYTE, front);
    }
    frontNew = 0;
    frameValid = 1;
  }
  pthread_mutex_unlock(&lock);
  return frameValid ? tex : 0;
}

double video_pos(void)        { return posSeg; }
double video_duration(void)   { return durationSeg; }
double video_buffer_end(void) { return bufferSeg; }
unsigned video_session(void)  { return session; }
int  video_playing(void)      { return active && ready && !paused; }
int  video_ready(void)        { return active && ready; }
int  video_active(void)       { return active; }
int  video_frame_rate_milli(void) { return rateMilli; }
int  video_width(void)        { return vidW; }
int  video_height(void)       { return vidH; }
const char *video_engine(void) { return "mpv (Mac preview)"; }
const char *video_hdr(void)   { return "none"; }
int  video_has_atmos(void)    { return 0; }
int  video_has_dolby_vision(void) { return 0; }
int  video_error_count(void)  { return errCount; }
int  video_eos_count(void)    { return eosCount; }
void video_last_error(char *dst, unsigned n) { if (n) snprintf(dst, n, "%s", errLast); }

// The TV-only knobs: the DV claim and the MP4 hint steer the uMS load, and the
// MKV header probe exists because the uMS does not report subtitle codecs. mpv
// reports all of that itself.
void video_set_dv(int dv) { (void)dv; }
void video_set_mp4(int m) { (void)m; }
int  video_mkv_head(MkvHead *h, char *u, unsigned n) { (void)h; (void)u; (void)n; return 0; }
int  video_mkv_waiting(void) { return 0; }
void video_mkv_hurry(void) {}

void video_shutdown(void) {
  if (render) mpv_render_context_set_update_callback(render, NULL, NULL);
  if (workerRun) {
    pthread_mutex_lock(&lock);
    workerRun = 0;
    pthread_cond_signal(&wake);
    pthread_mutex_unlock(&lock);
    pthread_join(worker, NULL);
  }
  if (render) { mpv_render_context_free(render); render = NULL; }
  if (mpv) { mpv_terminate_destroy(mpv); mpv = NULL; }
  free(back); free(front); back = front = NULL;
}

#endif
