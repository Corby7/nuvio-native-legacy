#include "video.h"
#include "plane.h"
#include <SDL2/SDL.h>
#include "mark.h"
#include "mkv.h"
#include "js.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <ctype.h>
#include <stdint.h>

// The names the uMS accepts in charColor, and the labels the sheet shows.
//
// OUTSIDE the device's #if: the tracks sheet draws the labels on the Mac too,
// where the rest of the module is a stub. Leaving them on the TV side broke the
// development build's link — and that is where the interface is checked.
const char *const VIDEO_SUB_COLORS[VIDEO_SUB_NCOLORS] = {
  "white", "yellow", "green", "blue", "red", "black"
};
const char *const VIDEO_SUB_COLORS_LABEL[VIDEO_SUB_NCOLORS] = {
  "White", "Yellow", "Green", "Blue", "Red", "Black"
};

static int extSubtitle(const char *start, const char *end) {
  static const char *const ext[] = { ".srt", ".vtt", ".smi", ".ass", ".ssa", ".sub" };
  size_t i;
  for (i = 0; i < sizeof ext / sizeof *ext; i++) {
    size_t n = strlen(ext[i]);
    const char *p;
    if ((size_t)(end - start) < n) continue;
    p = end - n;
    { size_t k; for (k = 0; k < n; k++)
        if (tolower((unsigned char)p[k]) != ext[i][k]) break;
      if (k == n) return 1; }
  }
  return 0;
}

void video_normalize_url_subtitle(const char *url, char *dst, unsigned size) {
  const char *q;
  size_t before, suffix, fits;
  if (!dst || !size) return;
  dst[0] = 0;
  if (!url) return;
  q = strchr(url, '?');
  if (!q) q = url + strlen(url);
  if (extSubtitle(url, q)) { snprintf(dst, size, "%s", url); return; }
  before = (size_t)(q - url); suffix = strlen(q);
  fits = before + 4 + suffix;
  if (fits + 1 > size) { snprintf(dst, size, "%s", url); return; }
  memcpy(dst, url, before);
  memcpy(dst + before, ".srt", 4);
  memcpy(dst + before + 4, q, suffix + 1);
}

// Declared here because loadCompleted calls it long before it is defined. The
// Mac's clang accepts the implicit declaration; the ARM gcc refuses — and the ARM
// is right.
static void applyStyle(void);

// Declared here because loadCompleted calls it long before it is defined. The
// Mac's clang accepts the implicit declaration and the ARM gcc refuses — and the
// ARM is right.
static void applyStyle(void);


// Defined further down (alongside urlCurrent, which is what the thread consumes);
// declared here because the sourceInfo parse, well above, is what starts the thread.
static char  urlCurrent[1024];   // the current playback's URL
// Recovery from a destroyed pipeline: asked for by luna's response thread and
// carried out on the main thread (video_pump), because reloading from inside the
// event handler re-enters the very path that has just failed.
static int    recovering;
static double resumeIn;
// The position to apply as soon as the load finishes. A seek before loadCompleted
// is sent to a pipeline that does not exist yet and disappears with no error.
static double posOnLoad;
// TRACKS to restore after a pipeline drop. Without this the video came back with
// DIFFERENT audio — a new pipeline always starts on track 0, and the owner, who
// had chosen theirs, saw the choice undone by itself. `-1` = nothing to restore.
static int   audioOnLoad = -1, subOnLoad = -1;
static char  subUrlOnLoad[1024];
// The URL of the EXTERNAL subtitle in use. subCurrent does not represent it:
// choosing an OpenSubtitles subtitle touches none of the file's tracks, it only
// points setSubtitleSource. Without storing the URL, recovery brought back the
// embedded subtitle from before, or none at all.
static char  subUrlCurrent[1024];
// A pending seek: the target and when to send it. See SEEK_REST_MS.
static int    pauseRequested;   // 1 while the pause was asked for by us
// An MKV probe requested, waiting for the buffer. See the note in sourceInfo.
static int    mkvPending;
// 1 when the source was announced as MP4. See video_set_mp4.
static int    sourceMp4;
static double seekTarget;
static Uint32 seekIn;
// Declared here because video_pump calls it before its definition. The Mac's clang
// accepts the implicit declaration; the ARM gcc refuses — and the ARM is right.
// Third time in this file.
static void seekNow(double seconds);
static void *readMkv(void *arg);
static pthread_t threadMkv;
static int       threadMkvAlive;
// The pipeline's monotonic identity. LS2 callbacks can outlive the unload; without
// a generation, an old response can take over the next opening's state and make
// the correct load be ignored.
static unsigned  session;

#ifdef __APPLE__
// On the Mac there is no bus and no video plane. The stubs let the rest of the app
// compile and run the same, only with no moving image.
int  video_start(void) { return 0; }
int  video_play(const char *u) { (void)u; return 0; }
void video_pump(void) {}
void video_stop(void) {}
void video_pause(int p) { (void)p; }
void video_fetch(double s) { (void)s; }
void video_window(int x,int y,int w,int h) { (void)x;(void)y;(void)w;(void)h; }
// The stub that was MISSING: the function existed only in the device's branch, so
// the Mac build broke at link time with "_video_window_source, referenced from
// _applyAspect". It is the mirror of the trap already known — the Mac does not
// compile half the pipeline, and so does not validate `video.c`; here it demands
// the declaration the other half does not have. Every new video function has to
// appear in BOTH branches.
void video_window_source(int sx,int sy,int sw,int sh,int dx,int dy,int dw,int dh) {
  (void)sx;(void)sy;(void)sw;(void)sh;(void)dx;(void)dy;(void)dw;(void)dh;
}
double video_pos(void) { return 0; }
double video_duration(void) { return 0; }
double video_buffer_end(void) { return 0; }
void video_set_dv(int dv) { (void)dv; }
int  video_playing(void) { return 0; }
int  video_ready(void) { return 0; }
int  video_active(void) { return 0; }
int  video_n_audio(void) { return 0; }
int  video_n_subtitle(void) { return 0; }
const VideoTrack *video_audio(int i) { (void)i; return 0; }
const VideoTrack *video_subtitle(int i) { (void)i; return 0; }
int  video_audio_current(void) { return 0; }
int  video_subtitle_current(void) { return -1; }
void video_choose_audio(int i) { (void)i; }
void video_choose_subtitle(int i) { (void)i; }
void video_subtitle_external(const char *u) { (void)u; }
void video_subtitle_style(const VideoSubtitleStyle *e) { (void)e; }
void video_set_mp4(int m) { (void)m; }
int  video_has_atmos(void) { return 0; }
int  video_has_dolby_vision(void) { return 0; }
const char *video_hdr(void) { return "none"; }
int  video_width(void) { return 0; }
int  video_height(void) { return 0; }
void video_shutdown(void) {}
#else
#include <dlfcn.h>

typedef struct LSHandle LSHandle;
typedef struct LSMessage LSMessage;
typedef int (*Filter)(LSHandle *, LSMessage *, void *);

// LSError is a struct by value and there is no C header in the SDK. A generous
// buffer avoids corrupting the stack when the library writes the error into it.
static char ERROR[256];

static int         (*lsRegister)(const char *, LSHandle **, void *);
static int         (*lsAttach)(LSHandle *, void *, void *);
static int         (*lsCall)(LSHandle *, const char *, const char *, Filter, void *, unsigned long *, void *);
static const char *(*lsPayload)(LSMessage *);
static void *(*loopNew)(void *, int);
static void  (*loopRun)(void *);
static void  (*loopStop)(void *);

// THE VIDEO PLANE NO LONGER LIVES HERE. Up to webOS 4 it was driven by
// libAcbAPI, and the reason was never the plane itself: the app registers on LS2
// as com.webos.media.client.nuvio, and that role does NOT reach
// com.webos.service.tv.display ("Not permitted to send to ..."). libAcbAPI did
// reach it, and served as a proxy.
//
// On this TV (OLED55C32LA, webOS 23) libAcbAPI.so.1 DOES NOT EXIST — `find /`
// returns nothing — and dlopening it took down the LS2 half with it, which works
// perfectly well. The replacement is not another proxy: the app's surface EXPORTS
// itself through the compositor (plane.c), which hands back a windowId, and that
// id travels in the com.webos.media load. The display service is never addressed,
// so the permission problem stops existing.
//
// What is left in this file is only the pipeline: LS2 -> com.webos.media.
static LSHandle *bus;
static void     *loop;
static pthread_t thread;
// The rectangle the UI asked for. Kept because the window can only be applied
// once there is media attached, which arrives long after whoever asked.
static int       windowX, windowY, windowW = 1920, windowH = 1080;
// A request to reapply the rectangle, raised from an LS2 callback. Wayland
// belongs to the drawing thread and LS2 callbacks run on the GMainLoop thread, so
// what crosses between them is THIS flag, read by video_pump.
static volatile int windowDirty;
// The last source/destination pair applied by the uMS's setDisplayWindow, so as
// not to repeat the same call every frame. fontX = -1 means "nothing applied".
static int       fontX = -1, fontY, fontW, fontH, dstX = -1, dstY, dstW, dstH;
// The stream's characteristics, taken from the videoInfo event of the uMS
// subscription. The ACB needs them to describe the video to the display pipeline.
static int       vidW = 1920, vidH = 1080, vidRate = 30;
static long      vidBits;
static char      vidScan[24] = "progressive";
// The real hdrType the uMS reports for the layer that reached the decoder. This
// beats the addon's label: a file marked HDR-DV may deliver only the HDR10 layer
// on this TV/profile.
static char      vidHdr[24] = "none";
static long      seiX0, seiX1, seiX2, seiY0, seiY1, seiY2;
static long      seiWhiteX, seiWhiteY, seiMinLuma, seiMaxLuma;
static long      seiMaxCLL, seiMaxFALL;
static int       vuiFirst = 2, vuiTrans = 2, vuiMatrix = 2;
static int       vidAtmos, vidDV;
// The state of the Dolby Vision fallback (see the block in video_play). Declared
// HERE and not next to the function because the videoInfo parser, well above, sets
// sawVideo — and in C the declaration order is what counts.
static int       dvInLoad, dvInset, sawVideo;

// Tracks read from the sourceInfo. Kept because the screen needs them every frame
// and reprocessing the JSON while drawing would be wasteful.
static VideoTrack trackAudio[NV_TRACK_MAX], trackSub[NV_TRACK_MAX];
static int nAudio, nSub, audioCurrent, subCurrent = -1;

// The chosen source's DV claim. Set by video_set_dv BEFORE playing, because
// video_play zeroes vidDV when starting a new session.
static int dvRequest;

// A readable name for the language. Only those that actually turn up in this
// collection; the rest keep the code, which is better than "Unknown" — the code at
// least identifies it.
static const char *languageReadable(const char *c) {
  // A table WITH ACCENTS — it is a language name on screen, not an identifier. And
  // with the three-letter codes (ISO 639-2) as well as the two-letter ones, because
  // a release MKV almost always tags with the three-letter ones.
  static const struct { const char *cod, *name; } T[] = {
    { "pt", "Portuguese" },  { "pob", "Portuguese (BR)" }, { "por", "Portuguese" },
    { "pt-br", "Portuguese (BR)" }, { "ptb", "Portuguese (BR)" },
    { "en", "English" },     { "eng", "English" },
    { "es", "Spanish" },   { "spa", "Spanish" }, { "esp", "Spanish" },
    { "fr", "French" },    { "fre", "French" },  { "fra", "French" },
    { "de", "German" },     { "ger", "German" },   { "deu", "German" },
    { "it", "Italian" },   { "ita", "Italian" },
    { "ja", "Japanese" },    { "jpn", "Japanese" },
    { "ko", "Korean" },    { "kor", "Korean" },
    { "zh", "Chinese" },     { "chi", "Chinese" },   { "zho", "Chinese" },
    { "ru", "Russian" },      { "rus", "Russian" },
    { "ar", "Arabic" },      { "ara", "Arabic" },
    { "hi", "Hindi" },      { "hin", "Hindi" },
    { "nl", "Dutch" },   { "dut", "Dutch" }, { "nld", "Dutch" },
    { "sv", "Swedish" },      { "swe", "Swedish" },
    { "no", "Norwegian" },  { "nor", "Norwegian" },
    { "da", "Danish" },{ "dan", "Danish" },
    { "fi", "Finnish" },  { "fin", "Finnish" },
    { "pl", "Polish" },    { "pol", "Polish" },
    { "tr", "Turkish" },      { "tur", "Turkish" },
    { "he", "Hebrew" },   { "heb", "Hebrew" },
    { "th", "Thai" },  { "tha", "Thai" },
    { "cs", "Czech" },     { "cze", "Czech" },
    { "el", "Greek" },      { "gre", "Greek" },
    { "hu", "Hungarian" },    { "hun", "Hungarian" },
    { "ro", "Romanian" },     { "rum", "Romanian" },
    { "uk", "Ukrainian" },  { "ukr", "Ukrainian" },
    { "vi", "Vietnamese" }, { "vie", "Vietnamese" },
    { "id", "Indonesian" },  { "ind", "Indonesian" },
  };
  size_t i;
  if (!c || !*c) return "";
  for (i = 0; i < sizeof T / sizeof *T; i++)
    if (!strcasecmp(c, T[i].cod)) return T[i].name;
  // With no name in the table, it returns the CODE IN CAPITALS — which is what the
  // web app does when it cannot name it ("ENG", "POR"). Showing the code says
  // something; falling back to "Subtitle 3" says nothing.
  { static char cx[16]; size_t k;
    for (k = 0; c[k] && k + 1 < sizeof cx; k++)
      cx[k] = (c[k] >= 'a' && c[k] <= 'z') ? (char)(c[k] - 32) : c[k];
    cx[k] = 0;
    return cx; }
}
static char      media[64];
static double    posSeg, durationSeg;
static int       playing, ready, on;

// Looks for the key and requires what follows to be a NUMBER.
//
// The event is {"currentTime":{"currentTime":8580,...}}: the key's first
// occurrence is the outer object, and atof("{...") returns 0. The bar sat at 0:00
// with the correct duration beside it — the kind of error that looks like "the
// player does not update" and is really reading the wrong field.
static double numberOf(const char *p, const char *key) {
  const char *q = p;
  size_t n = strlen(key);
  while ((q = strstr(q, key)) != NULL) {
    const char *v = q + n;
    while (*v == ' ') v++;
    if ((*v >= '0' && *v <= '9') || *v == '-' || *v == '.') return atof(v);
    q += n;
  }
  return -1.0;
}

// The pipeline's latency: the load request -> loadCompleted -> the first frame.
// These are the numbers that say whether the start and the buffer are healthy;
// without them "it's slow" is an impression.
static struct timespec t0Request;
static int cronRequested, cronLoad, cronFrame;
static long msSinceRequest(void) {
  struct timespec a;
  clock_gettime(CLOCK_MONOTONIC, &a);
  return (a.tv_sec - t0Request.tv_sec) * 1000L + (a.tv_nsec - t0Request.tv_nsec) / 1000000L;
}
// How far the pipeline's buffer already reaches (seconds), from the bufferRange event.
static double bufferSeg;


static int onEvent(LSHandle *h, LSMessage *m, void *u) {
  const char *p = lsPayload(m);
  unsigned mySession = (unsigned)(uintptr_t)u;
  (void)h;
  if (mySession != session) return 1;
  if (!p) return 1;
  printf("[video] ev %s\n", p); fflush(stdout);
  if (strstr(p, "sourceInfo")) {
    const char *q;
    nAudio = nSub = 0;
    vidAtmos = 0;
    // It walks audioTrackInfo item by item. The sourceInfo is a single object, so
    // walking the "{"s after the array's key is enough here.
    q = strstr(p, "\"audioTrackInfo\"");
    if (q) {
      const char *endVet = strchr(q, ']');
      const char *o = strchr(q, '{');
      while (o && nAudio < NV_TRACK_MAX && (!endVet || o < endVet)) {
        const char *fo = strchr(o, '}');
        VideoTrack *f = &trackAudio[nAudio];
        char cod[16] = "", ch[8] = "", imm[16] = "";
        memset(f, 0, sizeof *f);
        f->number = nAudio;
        { const char *l = strstr(o, "\"language\":\"");
          if (l && (!fo || l < fo)) {
            size_t k = 0; l += 12;
            while (*l && *l != '"' && k + 1 < sizeof f->language) f->language[k++] = *l++;
            f->language[k] = 0;
            if (!strcmp(f->language, "(null)")) f->language[0] = 0;
          } }
        { const char *c2 = strstr(o, "\"codec\":\"");
          if (c2 && (!fo || c2 < fo)) {
            size_t k = 0; c2 += 9;
            while (*c2 && *c2 != '"' && k + 1 < sizeof cod) cod[k++] = *c2++;
            cod[k] = 0;
          } }
        { const char *m = strstr(o, "\"immersive\":\"");
          if (m && (!fo || m < fo)) {
            size_t k = 0; m += 13;
            while (*m && *m != '"' && k + 1 < sizeof imm) imm[k++] = *m++;
            imm[k] = 0;
            if (!strcasecmp(imm, "ATMOS")) vidAtmos = 1;
          } }
        { double c3 = numberOf(o, "\"channels\":");
          if (c3 == 6) snprintf(ch, sizeof ch, "5.1");
          else if (c3 == 8) snprintf(ch, sizeof ch, "7.1");
          else if (c3 == 2) snprintf(ch, sizeof ch, "2.0"); }
        snprintf(f->label, sizeof f->label, "%s%s%s%s%s",
                 f->language[0] ? languageReadable(f->language) : "Track",
                 imm[0] ? "  \xc2\xb7  " : (ch[0] ? "  \xc2\xb7  " : ""),
                 imm[0] ? "Atmos" : "",
                 (imm[0] && ch[0]) ? " " : "", ch);
        nAudio++;
        o = fo ? strchr(fo, '{') : NULL;
      }
    }
    // DIAGNOSTIC: it dumps the RAW sourceInfo once per title. The TV does not
    // return a subtitle language on the owner's files (they all come out as
    // "Subtitle N"), and without seeing the real JSON any fix is a guess — it could
    // be a different field name, or the pipeline may genuinely not tag them.
    // Read it with: sshpass ... scp root@TV:/tmp/nuvio-tracks.json .
    { static int evicted;
      if (!evicted) {
        FILE *fd = fopen("/tmp/nuvio-tracks.json", "w");
        if (fd) { fputs(p, fd); fclose(fd); evicted = 1; }
      } }

    q = strstr(p, "\"subtitleTrackInfo\"");
    if (q) {
      const char *endVet = strchr(q, ']');
      const char *o = strchr(q, '{');
      while (o && nSub < NV_TRACK_MAX && (!endVet || o < endVet)) {
        const char *fo = strchr(o, '}');
        VideoTrack *f = &trackSub[nSub];
        memset(f, 0, sizeof *f);
        f->number = (int)numberOf(o, "\"trackNum\":");
        { const char *l = strstr(o, "\"language\":\"");
          if (l && (!fo || l < fo)) {
            size_t k = 0; l += 12;
            while (*l && *l != '"' && k + 1 < sizeof f->language) f->language[k++] = *l++;
            f->language[k] = 0;
            if (!strcmp(f->language, "(null)")) f->language[0] = 0;
          } }
        // A file with no language tag is the common case in a release MKV.
        // Numbering is honest; inventing "English" would be worse.
        if (f->language[0])
          snprintf(f->label, sizeof f->label, "%s", languageReadable(f->language));
        else
          snprintf(f->label, sizeof f->label, "Subtitle %d", f->number + 1);
        nSub++;
        o = fo ? strchr(fo, '{') : NULL;
      }
    }
    printf("[video] tracks: audio=%d subtitle=%d atmos=%d\n", nAudio, nSub, vidAtmos);
    fflush(stdout);

    // THE PIPELINE DOES NOT GIVE A SUBTITLE LANGUAGE. Measured on this TV, on a
    // file with 43 subtitles: audioTrackInfo comes with "en"/"es"/"fr"/"it" and
    // EVERY subtitleTrackInfo entry comes with "language":"(null)". There is no
    // other field there — the information does not come out of the pipeline, and
    // the list became "Subtitle 1..43", which helps nobody choose.
    //
    // The way to know is to read the file itself, which is what the browser does
    // for free in the web app. It fires a thread that downloads the first 2 MB by
    // Range and reads the Matroska Tracks element; when it comes back, it matches
    // by trackNum and rewrites the labels. It does not block playback: if it fails,
    // or if the file is not an MKV, what was already there stays.
    { int missing = 0, i;
      for (i = 0; i < nSub; i++) if (!trackSub[i].language[0]) missing = 1;
      // IT ONLY NOTES IT DOWN. What fires it is video_pump, once the buffer is
      if (missing && !sourceMp4) mkvPending = 1;
      else if (missing) mark("mkv: source is MP4, probe skipped"); }
  }

  if (strstr(p, "videoInfo")) {
    sawVideo = 1;   // closes the DV fallback's deadline
    double v;
    int wasW = vidW, wasH = vidH;
    v = numberOf(p, "\"width\":");      if (v > 0) vidW = (int)v;
    v = numberOf(p, "\"height\":");     if (v > 0) vidH = (int)v;
    // THE REAL FRAME SIZE has arrived. Until now vidW/vidH were 1920x1080 for
    // want of anything better, and the SOURCE region sent to the compositor used
    // that guess: on a 3840x1606 file that is not "no crop", it is a crop of the
    // top-left corner, blown up — a wrong picture that looks like a zoom bug
    // rather than a missing measurement. Flagging it makes video_pump resend the
    // pair with the right size, on the drawing thread.
    if (vidW != wasW || vidH != wasH) windowDirty = 1;
    v = numberOf(p, "\"frameRate\":");  if (v > 0) vidRate = (int)v;
    v = numberOf(p, "\"bitRate\":");    if (v > 0) vidBits = (long)v;
    { const char *q = strstr(p, "\"scanType\":\"");
      if (q) { const char *f; q += 12; f = strchr(q, '"');
        if (f && f - q < (int)sizeof vidScan) {
          memcpy(vidScan, q, f - q); vidScan[f - q] = 0; } } }
    { const char *q = strstr(p, "\"hdrType\":\"");
      if (q) { const char *f; q += 11; f = strchr(q, '"');
        if (f && f - q < (int)sizeof vidHdr) {
          memcpy(vidHdr, q, f - q); vidHdr[f - q] = 0;
          // Alongside what the SOURCE claimed. On its own, the hdrType does not
          // answer the question that matters on MKV: "did we ask for Dolby Vision
          // and did the TV deliver Dolby Vision, or did it downgrade to HDR10?".
          // The reports from elsewhere (Kodi, Plex, UMS) say webOS engages native
          // DV on MP4 profiles 5 and 8 and falls back to HDR10 on Matroska; this
          // line is what lets us confirm or refute that ON THIS TV, by measurement
          // rather than by reputation.
          printf("[video] pipeline HDR: %s (source claimed DV=%d)\n",
                 vidHdr, dvRequest);
          // It goes to the MILESTONES too, which are readable on the device: the
          // stdout of an app launched by applicationManager reaches nowhere, and
          // that is why this measurement — the only one that answers whether the TV
          // honoured or downgraded Dolby Vision — existed only in theory.
          { char m[64];
            snprintf(m, sizeof m, "pipeline hdr: %s (source DV=%d)",
                     vidHdr, dvRequest);
            mark(m); } } } }
    // The web Nuvio that plays correctly passes these values through unchanged.
    // For DolbyVision it omits both blocks; buildVideoData does the same.
    { double x;
#define READ_SEI(name, dst) do { x = numberOf(p, "\"" name "\":"); if (x >= 0) dst = (long)x; } while (0)
      READ_SEI("displayPrimariesX0", seiX0); READ_SEI("displayPrimariesX1", seiX1);
      READ_SEI("displayPrimariesX2", seiX2); READ_SEI("displayPrimariesY0", seiY0);
      READ_SEI("displayPrimariesY1", seiY1); READ_SEI("displayPrimariesY2", seiY2);
      READ_SEI("whitePointX", seiWhiteX); READ_SEI("whitePointY", seiWhiteY);
      READ_SEI("minDisplayMasteringLuminance", seiMinLuma);
      READ_SEI("maxDisplayMasteringLuminance", seiMaxLuma);
      READ_SEI("maxContentLightLevel", seiMaxCLL);
      READ_SEI("maxPicAverageLightLevel", seiMaxFALL);
#undef READ_SEI
      x = numberOf(p, "\"colorPrimaries\":"); if (x >= 0) vuiFirst = (int)x;
      x = numberOf(p, "\"transferCharacteristics\":"); if (x >= 0) vuiTrans = (int)x;
      x = numberOf(p, "\"matrixCoeffs\":"); if (x >= 0) vuiMatrix = (int)x;
    }
  }
  if (strstr(p, "loadCompleted")) {
    mark("video loadCompleted");
    // ORDER: tracks first, position after. Switching track restarts the decode in
    // the pipeline; doing that AFTER the seek would throw the position away.
    if (audioOnLoad >= 0) {
      int a2 = audioOnLoad; audioOnLoad = -1;
      if (a2 > 0) video_choose_audio(a2);
    }
    if (subUrlOnLoad[0]) {
      char u[1024];
      snprintf(u, sizeof u, "%s", subUrlOnLoad);
      subUrlOnLoad[0] = 0; subOnLoad = -1;
      video_subtitle_external(u);
    } else if (subOnLoad >= 0) {
      int l2 = subOnLoad; subOnLoad = -1;
      video_choose_subtitle(l2);
    }
    if (posOnLoad > 1.0) {
      double target = posOnLoad;
      posOnLoad = 0.0;
      video_fetch(target);
      mark("resumed after the pipeline died");
    }
    // The pipeline is new: the subtitle style does not survive the previous load.
    applyStyle();
    ready = 1;
    if (cronRequested && !cronLoad) {
      cronLoad = 1;
      printf("[video] load->loadCompleted %lums\n", msSinceRequest());
    }
    // THERE IS NO PER-SESSION BIND ANY MORE. The plane has been attached to the
    // exported surface since startup, and the pipeline was pointed at it by the
    // windowId that travelled in the load itself. The bind thread with pauses
    // between steps, which existed because every AcbAPI call was asynchronous,
    // went away with the ACB — and with it the "second playback, black with
    // sound" bug, which was the ACB still pointing at the previous session's dead
    // mediaId.
    //
    // The rectangle IS reapplied, though: see windowDirty below.
    windowDirty = 1;
  }
  if (strstr(p, "bufferRange")) {
    double e = numberOf(p, "\"endTime\":");
    if (e >= 0) bufferSeg = e;
  }
  // A PAUSE FOR WANT OF DATA. The owner reported "it keeps pausing" and the
  // milestones recorded NOTHING — because filling and emptying the buffer raises no
  // event on this side, and a pause like that goes through neither `paused` nor an
  // error. Without this, all that is left is guessing.
  //
  // It stamps how much buffer there was at that instant: it is the number that
  // separates "the source is not delivering" from "the decoder choked".
  if (strstr(p, "bufferingStart")) {
    char m[64];
    snprintf(m, sizeof m, "buffering START (buffer %+.1fs ahead)",
             bufferSeg - posSeg);
    mark(m);
  }
  if (strstr(p, "bufferingEnd")) {
    char m[64];
    snprintf(m, sizeof m, "buffering END (buffer %+.1fs ahead)",
             bufferSeg - posSeg);
    mark(m);
  }
  if (strstr(p, "playing")) {
    playing = 1;
    // Do NOT apply the rectangle HERE. This callback runs on LS2's GMainLoop
    // thread, and Wayland belongs to the drawing thread: sending a request from
    // the wrong thread corrupts the compositor's queue, and the symptom is not an
    // error — it is a disconnected client, i.e. the whole app dies with no log.
    // Flag it, and video_pump applies it on the right thread.
    windowDirty = 1;
  }
  if (strstr(p, "paused")) {
    // It only stamps when it was NOT us who paused: a pause by the owner is
    // expected, a pause coming from the pipeline is the defect.
    if (playing && !pauseRequested) mark("paused BY THE PIPELINE");
    playing = 0;
  }
  if (strstr(p, "endOfStream")) { playing = 0; mark("endOfStream"); }

  // A PIPELINE ERROR. There was no handling at all: when the uMS refused a seek or
  // lost the source, the app simply stopped and nobody knew why — "I skipped ahead
  // and it never carried on" is exactly the shape of that silence. There is nothing
  // to fix without knowing the cause, and the cause comes in the event itself.
  // A REAL ERROR, and not "errorCode: 0".
  //
  // The first version stamped anything with an `errorCode`, and the uMS sends that
  // field in a NORMAL response — the milestones filled up with
  // `pipeline error: errorText":"No Error"`, which is noise hiding the signal.
  if (strstr(p, "errorText") && !strstr(p, "\"No Error\"")) {
    const char *q = strstr(p, "errorText");
    char m[96];
    snprintf(m, sizeof m, "pipeline error: %.60s", q);
    { char *n2; for (n2 = m; *n2; n2++) if (*n2 == '\n' || *n2 == '\r') *n2 = ' '; }
    mark(m);
    // THE PIPELINE DESTROYED. Measured twice on the owner's TV: ~71 s after a seek,
    // the uMS answers "com.webos.pipeline.<id> is not running" and the video simply
    // stops — the app did NOTHING, and that is what they described as "I skipped
    // ahead and it never carried on".
    //
    // It reloads the same source and goes back to where it was. It is not a fix for
    // the CAUSE (the pipeline dies from something between the seek and the debrid
    // source, which this side cannot see), but for not leaving the owner on a
    // frozen screen.
    if (strstr(p, "is not running") && urlCurrent[0] && !recovering) {
      recovering = 1;
      resumeIn = posSeg;
      // IT KEEPS THE CHOICES. The position alone is not enough: the new pipeline is
      // born on track 0 with the subtitle off.
      audioOnLoad = audioCurrent;
      subOnLoad   = subCurrent;
      snprintf(subUrlOnLoad, sizeof subUrlOnLoad, "%s", subUrlCurrent);
      mark("pipeline died: reloading");
    }
  }
  { double v = numberOf(p, "\"currentTime\":");
    if (v >= 0) {
      posSeg = v / 1000.0;
      // The first frame after a seek: the real start-up number.
      if (cronRequested && !cronFrame && posSeg > 0.0) {
        cronFrame = 1;
        printf("[video] load->first frame %lums\n", msSinceRequest());
        fflush(stdout);
      }
    } }
  { double v = numberOf(p, "\"duration\":");
    if (v >= 0) durationSeg = v / 1000.0; }
  return 1;
}

static int soLog(LSHandle *h, LSMessage *m, void *u) {
  (void)h; (void)u;
  printf("[video] %s\n", lsPayload(m)); fflush(stdout);
  return 1;
}

static void call(const char *method, const char *load, Filter cb) {
  char uri[128]; unsigned long token = 0;
  snprintf(uri, sizeof uri, "luna://com.webos.media/%s", method);
  if (!lsCall(bus, uri, load, cb, NULL, &token, ERROR))
    printf("[video] %s failed\n", method);
}

// A variant for callbacks that need to know which session they belong to. The
// context is an integer cast to a pointer; there is no allocation to leak and no
// memory whose lifetime could end before the asynchronous response.
static void callCtx(const char *method, const char *load, Filter cb,
                      void *ctx) {
  char uri[128]; unsigned long token = 0;
  snprintf(uri, sizeof uri, "luna://com.webos.media/%s", method);
  if (!lsCall(bus, uri, load, cb, ctx, &token, ERROR))
    printf("[video] %s failed\n", method);
}

// THERE IS NO SECOND-SERVICE CALL ANY MORE. There used to be a generic `callIn`
// here, written to reach com.webos.service.tv.display: the source crop does not
// live in com.webos.media (it answers `Unknown method "setDisplayWindow" for
// category "/"`, and `ls-monitor -i com.webos.media` confirms it). That path was
// never actually used, because the hub refuses this app on tv.display — which is
// why libAcbAPI became the proxy in the first place.
//
// The crop is now a property of the exported surface and the compositor applies
// it (plane.c), so there is no second service to call.

static int onLoad(LSHandle *h, LSMessage *m, void *u) {
  const char *p = lsPayload(m), *q;
  char b[256];
  unsigned mySession = (unsigned)(uintptr_t)u;
  (void)h;
  printf("[video] load: %s\n", p ? p : "(null)"); fflush(stdout);
  if (mySession != session) {
    printf("[video] stale load ignored (session %u, current %u)\n",
           mySession, session);
    // A cancelled load can still create a pipeline in the uMS. Releasing that
    // resource avoids leaving the decoder busy when the user reopens the film.
    char old[96] = "";
    if (p) js_text(p, NULL, "mediaId", old, sizeof old);
    if (old[0] && strcmp(old, media)) {
      snprintf(b, sizeof b, "{\"mediaId\":\"%s\"}", old);
      call("unload", b, soLog);
    }
    return 1;
  }
  if (!p || media[0]) return 1;
  q = strstr(p, "\"mediaId\":\"");
  if (!q) return 1;
  q += 11;
  { const char *f = strchr(q, '"');
    if (!f || f - q >= (int)sizeof media) return 1;
    memcpy(media, q, f - q); media[f - q] = 0; }

  snprintf(b, sizeof b, "{\"connectionId\":\"%s\"}", media);
  call("notifyForeground", b, soLog);
  snprintf(b, sizeof b, "{\"mediaId\":\"%s\"}", media);
  callCtx("subscribe", b, onEvent, (void *)(uintptr_t)mySession);
  snprintf(b, sizeof b, "{\"mediaId\":\"%s\",\"type\":\"video\",\"index\":0}", media);
  call("selectTrack", b, soLog);
  snprintf(b, sizeof b, "{\"mediaId\":\"%s\"}", media);
  call("play", b, soLog);
  return 1;
}

static void *runLoop(void *u) { (void)u; loopRun(loop); return NULL; }

#define SIM(h, v, n) do { \
    *(void **)(&v) = dlsym(h, n); \
    if (!v) { printf("[video] missing %s\n", n); return 0; } \
  } while (0)

int video_start(void) {
  void *L, *G;
  if (on) return 1;
  L = dlopen("libluna-service2.so.3", RTLD_NOW);
  if (!L) L = dlopen("libluna-service2.so", RTLD_NOW);
  G = dlopen("libglib-2.0.so.0", RTLD_NOW);
  // libAcbAPI IS GONE FROM HERE. It used to be required alongside these two, and
  // on this TV it does not exist: the dlopen failed and took the working LS2 half
  // down with it. The video plane now comes from the compositor (plane.c), and
  // whoever has no plane still has a pipeline — the failure becomes "no picture",
  // not "no video".
  if (!L || !G) { printf("[video] libs: %s\n", dlerror()); return 0; }

  SIM(L, lsRegister, "LSRegister");
  SIM(L, lsAttach,   "LSGmainAttach");
  SIM(L, lsCall,     "LSCall");
  SIM(L, lsPayload,  "LSMessageGetPayload");
  SIM(G, loopNew,   "g_main_loop_new");
  SIM(G, loopRun,  "g_main_loop_run");
  SIM(G, loopStop,  "g_main_loop_quit");

  // The name MUST match the pattern of the app's LS2 role
  // (allowedNames: "com.webos.media.client.*"). Any other name is refused by the
  // hub and nothing after that happens.
  if (!lsRegister("com.webos.media.client.nuvio", &bus, ERROR)) {
    printf("[video] LSRegister refused\n"); return 0;
  }
  loop = loopNew(NULL, 0);
  if (!lsAttach(bus, loop, ERROR)) { printf("[video] attach failed\n"); return 0; }
  // A loop of its own: LS2 requires a GMainLoop spinning, and spinning it in the
  // drawing loop would cost frames. The responses arrive on this thread and only
  // touch simple variables, read by the drawing without a lock.
  pthread_create(&thread, NULL, runLoop, NULL);

  on = 1;
  // The windowId comes from plane.c, which exported the surface at startup. If it
  // is empty here the load still GOES OUT — and fails silently, which is what this
  // line exists to make visible in the log.
  printf("[video] ready (window id '%s')\n", plane_window_id()); fflush(stdout);
  return 1;
}

// MEASURED on the OLED65C9, two DV files in MKV:
//   file A: without DolbyHdrInfo it plays in HDR10; WITH the block it engages Dolby Vision.
//   file B: without the block it plays normally (HDR10, with a picture); WITH the
//           block there is ONLY AUDIO, and the pipeline never reports videoInfo.
// 8/"single" and 7/"dual" were tested on file B: both break the same way.
//
// Since we do not demux, there is no way to know in advance which of the two cases
// a source falls into — declaring blind wins DV on one file and loses the PICTURE
// on the other, which is a bad trade. So the declaration becomes a BET WITH A
// DEADLINE: if the pipeline does not report videoInfo within NV_DV_DEADLINE_MS, it
// reloads the same URL without the block. The cost is a few seconds on the file
// that does not accept it; the gain is never being left with no picture because of
// a claim of ours.
#define NV_DV_DEADLINE_MS 7000


// --- the subtitles' language read from the file itself -----------------------
// See the note at the trigger point, just below the sourceInfo parse.
static void *readMkv(void *arg) {
  MkvTrack fx[MKV_MAX_TRACKS];
  char url[1024];
  int n, i, j, matched = 0;
  (void)arg;

  snprintf(url, sizeof url, "%s", urlCurrent);

  n = mkv_tracks(url, fx, MKV_MAX_TRACKS);
  if (n < 1) {
    // Without this the only sign was a line on stdout, which on the TV reaches
    // nowhere — and the list stayed at "Subtitle 1, Subtitle 2" with nobody
    // knowing whether the file is not an MKV, whether the Range failed or whether
    // the header runs past the 2 MB we downloaded.
    mark("mkv: no track read (not an MKV, or Range failed)");
    threadMkvAlive = 0; return NULL;
  }

  // NO MUTEX, and deliberately: this file has none. trackSub is already written by
  // luna's response thread and read by the drawing with no lock at all, and
  // introducing a lock only here would give false safety — it would protect the
  // write and not the read. The possible damage is a half-read label in ONE frame;
  // that is why each field is filled in one go, with a single snprintf, and the
  // label (which is what shows) is written LAST, after the language.
  // The trackNum in LG's sourceInfo is Matroska's TrackNumber: match by it, and not
  // by order. The two lists do not arrive in the same order (this TV's sourceInfo
  // started at 42, 40, 41, 32...), and matching by position would swap the
  // languages around — worse than having no language at all.
  for (i = 0; i < nSub; i++) {
    if (trackSub[i].language[0]) continue;
    for (j = 0; j < n; j++) {
      if (fx[j].number != trackSub[i].number) continue;
      if (fx[j].language[0] && strcmp(fx[j].language, "und")) {
        snprintf(trackSub[i].language, sizeof trackSub[i].language, "%s", fx[j].language);
        matched++;
      }
      // The track's NAME ("Forced", "SDH", "Full") is what separates two subtitles
      // in the SAME language. Without it the owner sees "Portuguese" three times
      // and chooses in the dark — and that is precisely the list they complained about.
      if (fx[j].name[0])
        snprintf(trackSub[i].label, sizeof trackSub[i].label, "%s%s%s",
                 trackSub[i].language[0] ? languageReadable(trackSub[i].language) : "",
                 trackSub[i].language[0] ? "  \xc2\xb7  " : "", fx[j].name);
      else if (trackSub[i].language[0])
        snprintf(trackSub[i].label, sizeof trackSub[i].label, "%s",
                 languageReadable(trackSub[i].language));
      break;
    }
  }
  { char m[64];
    snprintf(m, sizeof m, "mkv: %d tracks read, %d subtitles with a language", n, matched);
    mark(m); }
  printf("[mkv] %d subtitles gained a language\n", matched);
  fflush(stdout);
  threadMkvAlive = 0;
  return NULL;
}

static long  msOfLoad = 0;

static long nowMs(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int playInternal(const char *url, int comDV);
static void pushWindow(void);

int video_play(const char *url) {
  dvInset = 0;
  snprintf(urlCurrent, sizeof urlCurrent, "%s", url ? url : "");
  return playInternal(url, 1);
}

// Called once per frame. It exists only for the deadline above: without it the
// fallback would depend on the user noticing there is no picture and leaving the screen.
void video_pump(void) {
  // Reapplies a rectangle requested from inside an LS2 callback. Here we are on
  // the drawing thread, which owns the Wayland connection — the only place a
  // request may be sent from. plane_forget clears the "same rectangle already"
  // memory, because the request came from something that suspects the plane lost
  // it.
  if (windowDirty) {
    windowDirty = 0;
    if (media[0]) { plane_forget(); pushWindow(); }
  }
  // AN MKV PROBE only with buffer to spare. 20 s ahead is the sign that the source
  // is delivering faster than the decoder consumes, and therefore that there is
  // bandwidth left for the header's 320 KB.
  if (mkvPending && !threadMkvAlive && urlCurrent[0] && bufferSeg - posSeg >= 20.0) {
    mkvPending = 0;
    threadMkvAlive = 1;
    if (pthread_create(&threadMkv, NULL, readMkv, NULL) != 0) threadMkvAlive = 0;
    else pthread_detach(threadMkv);
  }
  // A pending seek that has already settled.
  if (seekIn && SDL_GetTicks() >= seekIn) {
    Uint32 q = seekIn; seekIn = 0; (void)q;
    seekNow(seekTarget);
  }
  // RECUPERACAO DO PIPELINE, no fio principal. Ver a nota em `recuperando`.
  if (recovering) {
    double target = resumeIn;
    recovering = 0;
    mark("reloading the source");
    if (playInternal(urlCurrent, 1) && target > 1.0) {
      // The seek only counts after the load; storing the target and letting
      // loadCompleted apply it avoids sending a position to a pipeline that does
      // not exist yet.
      posOnLoad = target;
    }
  }
  // The deadline fallback was REMOVED because it did not work: the trigger was "the
  // pipeline did not report videoInfo", and the uMS reports videoInfo, sourceInfo
  // and loadCompleted normally even on files that end up with no picture. Measured:
  // videoInfo 3840x1606 hdrType=DolbyVision and loadCompleted at 3212ms, a black
  // screen with the audio running. There is no FRAME DISPLAYED signal in the uMS —
  // currentTime advances pulled along by the audio.
  //
  // The function stays because the per-frame tick is useful as soon as a better
  // signal exists (a frame counter, or the profile itself read from the MKV).
  (void)dvInLoad; (void)dvInset; (void)sawVideo;
  (void)msOfLoad; (void)urlCurrent; (void)nowMs; (void)playInternal;
}

static int playInternal(const char *url, int comDV) {
  char load[2048];
  unsigned mySession;
  if (!on && !video_start()) return 0;
  video_stop();
  mySession = ++session;
  sawVideo = 0;
  // The applied rectangle belongs to the SESSION: without zeroing it, a new session
  // that computes the same rect would fall into "it is already that" and would
  // never send anything to the plane.
  // (noUms is NOT zeroed: if this TV does not understand source cropping, it will
  // not start understanding on the next title, and insisting only risks the picture
  // again.)
  fontX = -1; dstX = dstY = dstW = dstH = -1;
  posSeg = durationSeg = bufferSeg = 0; playing = ready = 0; media[0] = 0;
  nAudio = nSub = 0; audioCurrent = 0; subCurrent = -1; vidAtmos = 0;
  subUrlCurrent[0] = 0; mkvPending = 0;
  snprintf(vidHdr, sizeof vidHdr, "none");
  seiX0 = seiX1 = seiX2 = seiY0 = seiY1 = seiY2 = 0;
  seiWhiteX = seiWhiteY = seiMinLuma = seiMaxLuma = seiMaxCLL = seiMaxFALL = 0;
  vuiFirst = vuiTrans = vuiMatrix = 2;
  // The chosen source's DV claim survives the reset: it is what decides the
  // DolbyHdrInfo of the load below. Without it every session would start "none".
  vidDV = dvRequest;
  cronRequested = 1; cronLoad = 0; cronFrame = 0;
  clock_gettime(CLOCK_MONOTONIC, &t0Request);
  // DolbyHdrInfo: this is how Kodi announces Dolby Vision to this same pipeline
  // (xbmc/cores/VideoPlayer/MediaPipelineWebOS.cpp):
  //   contents["DolbyHdrInfo"]["encryptionType"] = "clear"
  //   contents["DolbyHdrInfo"]["profileId"]      = dovi.dv_profile
  //   contents["DolbyHdrInfo"]["trackType"]      = el_present_flag ? "dual" : "single"
  //
  // A DIFFERENCE THAT MAY INVALIDATE ALL OF THIS, and that is why it is an
  // EXPERIMENT: Kodi demuxes with ffmpeg and DELIVERS BUFFERS through
  // option.externalStreamingInfo, which is where that block lives. We pass a URI
  // and the TV does the HTTP, the demux and the decode. Declaring the block in URI
  // mode may be silently ignored — and the only way to know is to measure the
  // hdrType that comes back.
  //
  // profileId 8 / "single" is what Kodi declares AFTER converting profile 7, not
  // what the file has. Since we do not demux, we do not know the real profile; that
  // is why the values are adjustable by environment variable, so 7/"dual" can be
  // tested against 8/"single" on the same file without recompiling.
  char dolby[192] = "";
  dvInLoad = 0;
  if (dvRequest && comDV) {
    // The values also come from /tmp/nuvio-dv.conf ("<profile> <track>", e.g.
    // "7 dual"), because the app is launched by SAM and there is no way to pass an
    // environment variable through it. Without the file, the environment applies
    // and then the default.
    static char pFile[16], tFile[16];
    const char *profile = getenv("NUVIO_DV_PROFILE");
    const char *track = getenv("NUVIO_DV_TRACK");
    { FILE *f = fopen("/tmp/nuvio-dv.conf", "r");
      if (f) {
        pFile[0] = tFile[0] = 0;
        if (fscanf(f, "%15s %15s", pFile, tFile) >= 1) {
          if (pFile[0]) profile = pFile;
          if (tFile[0]) track = tFile;
        }
        fclose(f);
      } }
    // OFF BY DEFAULT, and the reason is measured:
    //
    //   file A: without the block it plays in HDR10; WITH the block it engages Dolby Vision.
    //   several others: without the block they play normally; WITH the block they
    //                   end up WITH NO PICTURE (one showed the first frame and
    //                   froze, with the audio running).
    //
    // Gaining DV on one file and losing the picture on several is a bad trade. And
    // there is no way to decide on its own: I tried a deadline that would reload
    // without the block if the pipeline did not report video, and it DOES NOT WORK
    // — the uMS reports videoInfo (3840x1606, hdrType DolbyVision) and
    // loadCompleted normally even when no frame reaches the plane. "Reported" is
    // not "displayed", and there is no signal in the uMS for a frame advancing:
    // currentTime moves with the audio.
    //
    // So it becomes OPT-IN, to experiment file by file:
    //   echo "8 single" > /tmp/nuvio-dv.conf   (or "7 dual")
    //   rm /tmp/nuvio-dv.conf                  back to safe
    // The definitive route is knowing the file's real profile before claiming
    // anything: reading the MKV header over HTTP Range and finding the
    // BlockAdditionMapping with dvcC/dvvC, which is where Matroska stores it.
    if (!profile || !*profile || !strcmp(profile, "off")) {
      /* no declaration: known, safe behaviour */
    } else {
      snprintf(dolby, sizeof dolby,
               "\"externalStreamingInfo\":{\"contents\":{\"DolbyHdrInfo\":{"
               "\"encryptionType\":\"clear\",\"profileId\":%s,\"trackType\":\"%s\"}}},",
               (profile && *profile) ? profile : "8",
               (track && *track) ? track : "single");
      printf("[video] DolbyHdrInfo declarado: %s\n", dolby); fflush(stdout);
      dvInLoad = 1;
    }
  }
  // The REAL windowId, the one the compositor handed back for the exported
  // surface. It is what tells the pipeline to draw on this app's plane. If it is
  // empty the load answers returnValue:true, allocates a mediaId and never fetches
  // the file — exactly the silent failure "window_id_dummy" worked around on
  // webOS 4. Hence the refusal HERE, by name, rather than later with no picture.
  if (!plane_ready()) {
    printf("[video] no window id from the compositor; refusing to load "
           "(it would report success and fetch nothing)\n");
    fflush(stdout);
    return 0;
  }
  snprintf(load, sizeof load,
      "{\"payload\":{\"option\":{\"useSeekableRanges\":true,"
      "\"appId\":\"space.nuvio.native.legacy\","
      "%s"
      "\"bufferControl\":{\"userBufferCtrl\":false},"
      "\"windowId\":\"%s\"}},"
      "\"uri\":\"%s\",\"type\":\"media\"}", dolby, plane_window_id(), url);
  printf("[video] URL: %s (window '%s')\n", url, plane_window_id()); fflush(stdout);
  msOfLoad = nowMs();
  callCtx("load", load, onLoad, (void *)(uintptr_t)mySession);
  return 1;
}

void video_stop(void) {
  char b[128];
  // It also invalidates the session that is still waiting for the load's return.
  // That was the open -> exit -> open case that froze: there was no mediaId to
  // unload, but the old callback stayed alive and contaminated the new one.
  session++;
  windowDirty = 0;
  recovering = 0; resumeIn = posOnLoad = 0.0;
  audioOnLoad = subOnLoad = -1;
  subUrlOnLoad[0] = 0;
  pauseRequested = 0; seekIn = 0; mkvPending = 0;
  if (on && media[0]) {
    snprintf(b, sizeof b, "{\"mediaId\":\"%s\"}", media);
    call("unload", b, soLog);
  }
  media[0] = 0; playing = ready = 0;
}

void video_pause(int paused) {
  char b[128];
  if (!on || !media[0]) return;
  snprintf(b, sizeof b, "{\"mediaId\":\"%s\"}", media);
  call(paused ? "pause" : "play", b, soLog);
  playing = !paused;
  pauseRequested = paused;
}

// Measured on the TV: holding the arrow produced FOUR seeks in 0.8 s (16 s, 26 s,
// 36 s, 46 s) — four consecutive seek requests to the same source, and ~71 s later
// the pipeline died. It is not proven that one causes the other, but sending four
// positions when the owner wanted ONE is wasteful either way: the first three are
// discarded as soon as the fourth arrives.
//
// The DISPLAYED position changes at once (otherwise the bar does not respond to the
// press); what waits for the rest is the command to the pipeline.
#define SEEK_IDLE_MS 350

void video_fetch(double seconds) {
  if (!on || !media[0]) return;
  if (seconds < 0) seconds = 0;
  posSeg = seconds;
  seekTarget = seconds;
  seekIn = SDL_GetTicks() + SEEK_IDLE_MS;
}

// Actually sends it. Called by video_pump when the rest period expires.
static void seekNow(double seconds) {
  char b[192];
  if (!on || !media[0]) return;
  snprintf(b, sizeof b, "{\"mediaId\":\"%s\",\"position\":%d}",
           media, (int)(seconds * 1000.0));
  call("seek", b, soLog);
  { char m[48]; snprintf(m, sizeof m, "seek to %ds", (int)seconds); mark(m); }
}

// The hardware plane's rectangle. This is where the zoom modes happen: the video
// is not an element with `transform: scale()` as in the web app — it is a plane
// behind the GL surface, and enlarging means sending a rectangle LARGER than the
// screen, with negative x/y, and letting the excess run off the edge. It is the
// same result as the web's transform: the black bar baked into the frame leaves
// the visible area instead of being (impossibly) cropped by object-fit.
//
// The rectangle MUST NOT run off the screen. MEASURED on webOS 4 and kept here
// because the reason was never the ACB's: sending the plane a rectangle with a
static void pushWindow(void) {
  int sx = fontX, sy = fontY, sw = fontW, sh = fontH;
  if (fontX < 0) {
    sx = 0; sy = 0;
    sw = vidW > 1 ? vidW : 1920;
    sh = vidH > 1 ? vidH : 1080;
  }
  plane_window(sx, sy, sw, sh, windowX, windowY, windowW, windowH);
}

void video_window(int x, int y, int w, int h) {
  if (w < 1 || h < 1) return;
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > 1920) w = 1920 - x;
  if (y + h > 1080) h = 1080 - y;
  if (w < 1 || h < 1) return;
  if (x == windowX && y == windowY && w == windowW && h == windowH) return;  // do not repeat the same rect every frame
  windowX = x; windowY = y; windowW = w; windowH = h;
  fontX = -1;   // a destination with no crop was asked for: the source is whole again
  if (!on || !media[0]) return;   // with no media attached, applying it would go nowhere
  pushWindow();
}

// REAL ZOOM: it crops the SOURCE and keeps the destination inside the screen.
//
// The compositor takes both rectangles in the same call — `source_region` in
// DECODED-FRAME coordinates and `destination_region` in screen coordinates. So
// zooming is not inflating the destination (which blanks the plane), it is asking
// for a SMALLER piece of the source for the same destination: that is how the
// black bar baked into the frame leaves the visible area. It is the same picture
// the web produces with transform: scale(), only computed on the right side of
// the scaler.
//
// The webOS 4 path needed AcbAPI_setCustomDisplayWindow because this app's LS2
// role does not reach com.webos.service.tv.display. Here there is no service in
// the middle at all: the crop is a property of the exported surface, and the
// compositor applies it. There is no refusal left to handle, which is why the old
// `withoutUms` — the permanent give-up after a returnValue:false — is gone.
void video_window_source(int sx, int sy, int sw, int sh,
                        int dx, int dy, int dw, int dh) {
  if (sw < 2 || sh < 2 || dw < 1 || dh < 1) return;
  // Destination pinned to the screen: the same limit as always.
  if (dx < 0) { dw += dx; dx = 0; }
  if (dy < 0) { dh += dy; dy = 0; }
  if (dx + dw > 1920) dw = 1920 - dx;
  if (dy + dh > 1080) dh = 1080 - dy;
  if (dw < 1 || dh < 1) return;
  if (sx == fontX && sy == fontY && sw == fontW && sh == fontH &&
      dx == dstX && dy == dstY && dw == dstW && dh == dstH) return;
  fontX = sx; fontY = sy; fontW = sw; fontH = sh;
  dstX = dx; dstY = dy; dstW = dw; dstH = dh;
  windowX = dx; windowY = dy; windowW = dw; windowH = dh;   // the reapply uses these
  if (!on || !media[0]) return;
  pushWindow();
}

double video_pos(void)      { return posSeg; }
double video_duration(void)  { return durationSeg; }
double video_buffer_end(void) { return bufferSeg; }
int    video_playing(void)  { return playing; }
int    video_ready(void)   { return ready; }
// There is media loaded. The hole in the surface uses THIS and not loadCompleted:
// opening the hole early costs nothing (behind it there is only the video plane)
// and waiting for the event would leave the screen drawn over the video if the
// event changed name or did not arrive.
int    video_active(void)    { return media[0] != 0; }

int  video_n_audio(void)   { return nAudio; }
int  video_n_subtitle(void) { return nSub; }
const VideoTrack *video_audio(int i)   { return (i >= 0 && i < nAudio) ? &trackAudio[i] : NULL; }
const VideoTrack *video_subtitle(int i) { return (i >= 0 && i < nSub) ? &trackSub[i] : NULL; }
int  video_audio_current(void)   { return audioCurrent; }
int  video_subtitle_current(void) { return subCurrent; }
int  video_has_atmos(void)        { return vidAtmos; }
// THE BADGE now comes from the PIPELINE, not from the source's claim.
//
// `vidDV` is what the addon CLAIMED about the URL, and it is still what the bind
// describes to tv.display (buildVideoData reads vidDV, not this function) — there
// the claim is the only information available before there is a picture, and
// without it there is no way to ask for Dolby Vision. But for the BADGE it is the
// wrong source: it is MEASURED on this TV that an MKV advertised as DV comes back
// with hdrType "HDR10" in the videoInfo. Wiring the badge to the claim made the
// screen announce Dolby Vision over an HDR10 stream — and a badge that lies is
// worse than no badge, because it is what the owner trusts to know whether they got
// the good version.
//
// Before the videoInfo arrives, vidHdr is "none" and the answer is 0: no badge for
// a few seconds is honest; a badge that appears and then contradicts itself is not.
int  video_has_dolby_vision(void) {
  return !strcasecmp(vidHdr, "DolbyVision") || !strcasecmp(vidHdr, "dolby_vision");
}
// The pipeline's raw hdrType, so the screen can say "HDR10" when it is HDR10
// instead of staying quiet. "none" when the stream is SDR or it is not known yet.
const char *video_hdr(void)       { return vidHdr; }
int  video_width(void)          { return vidW; }
int  video_height(void)           { return vidH; }

void video_set_dv(int dv) { dvRequest = dv ? 1 : 0; }

void video_choose_audio(int i) {
  char b[192];
  const VideoTrack *f = video_audio(i);
  if (!on || !media[0] || !f) return;
  snprintf(b, sizeof b, "{\"mediaId\":\"%s\",\"type\":\"audio\",\"index\":%d}",
           media, f->number);
  call("selectTrack", b, soLog);
  audioCurrent = i;
}

void video_choose_subtitle(int i) {
  char b[192];
  if (!on || !media[0]) return;
  if (i < 0) {
    snprintf(b, sizeof b, "{\"mediaId\":\"%s\",\"enable\":false}", media);
    call("setSubtitleEnable", b, soLog);
    subCurrent = -1;
    return;
  }
  { const VideoTrack *f = video_subtitle(i);
    if (!f) return;
    snprintf(b, sizeof b, "{\"mediaId\":\"%s\",\"enable\":true}", media);
    call("setSubtitleEnable", b, soLog);
    snprintf(b, sizeof b, "{\"mediaId\":\"%s\",\"type\":\"text\",\"index\":%d}",
             media, f->number);
    call("selectTrack", b, soLog);
    subCurrent = i;
    subUrlCurrent[0] = 0;   // back to a track from the file
    applyStyle(); }
}

// The chosen style, kept because THE PIPELINE IS BORN ON EVERY LOAD and inherits
// nothing from the previous video. Reapplied on loadCompleted and whenever the
// subtitle is (re)selected.
static VideoSubtitleStyle style = { 120, 0, 0, 3, 1, 0, 0, 0 };
static int hasStyle;

static void applyStyle(void) {
  char b[256];
  if (!on || !media[0] || !hasStyle) return;
  /* An embedded one still belongs to the uMS: reduce the percentage to five steps. */
  { int p=style.size, t=p<=70?0:p<=100?1:p<=130?2:p<=165?3:4;
    snprintf(b, sizeof b, "{\"mediaId\":\"%s\",\"fontSize\":%d}", media, t);
    call("setSubtitleFontSize", b, soLog); }
  { int c = style.color;
    if (c < 0 || c >= VIDEO_SUB_NCOLORS) c = 0;
    snprintf(b, sizeof b, "{\"mediaId\":\"%s\",\"charColor\":\"%s\"}",
             media, VIDEO_SUB_COLORS[c]);
    call("setSubtitleCharacterColor", b, soLog); }
  // The LETTER's opacity, separate from the background's. The handler exists in the
  // C9's firmware (`setSubtitleCharacterOpacity`) and takes 0..255. Three levels
  // avoid an endless sheet on the remote control and keep the text legible over video.
  { int op = style.opacity == 3 ? 64 : style.opacity == 2 ? 128
           : style.opacity == 1 ? 191 : 255;
    snprintf(b, sizeof b, "{\"mediaId\":\"%s\",\"charOpacity\":%d}", media, op);
    call("setSubtitleCharacterOpacity", b, soLog); }
  // The background is the colour+opacity pair: without declaring the colour,
  // changing only the opacity has nothing to reveal.
  { int f = style.background; if (f < 0) f = 0; if (f > 4) f = 4;
    int op = f == 4 ? 255 : f * 64;
    snprintf(b, sizeof b, "{\"mediaId\":\"%s\",\"bgColor\":\"black\"}", media);
    call("setSubtitleBackgroundColor", b, soLog);
    snprintf(b, sizeof b, "{\"mediaId\":\"%s\",\"bgOpacity\":%d}", media, op);
    call("setSubtitleBackgroundOpacity", b, soLog); }
  // A folha oferece 0..7; o uMS quer -3..4.
  { int p = style.position; if (p < 0) p = 0; if (p > 7) p = 7;
    snprintf(b, sizeof b, "{\"mediaId\":\"%s\",\"position\":%d}", media, p - 3);
    call("setSubtitlePosition", b, soLog); }
  // CHECKED ON SCREEN: "uniform" draws an outline around the letters. "none" is the
  // borderless one. The uMS's return value is no proof here — it answered
  // returnValue:true even to values I invented.
  { const char *ed = style.border == 2 ? "dropShadow"
                   : (style.border == 1 ? "uniform" : "none");
    snprintf(b, sizeof b, "{\"mediaId\":\"%s\",\"charEdgeType\":\"%s\"}",
             media, ed);
    call("setSubtitleCharacterEdge", b, soLog); }
  if (style.delayMs) {
    snprintf(b, sizeof b, "{\"mediaId\":\"%s\",\"sync\":%d}", media, style.delayMs);
    call("setSubtitleSync", b, soLog);
  }
}

void video_subtitle_style(const VideoSubtitleStyle *e) {
  if (!e) return;
  style = *e;
  hasStyle = 1;
  applyStyle();
}

void video_set_mp4(int isMp4) { sourceMp4 = isMp4; }

void video_subtitle_external(const char *url) {
  char b[1400], recognizable[1024];
  if (!on || !media[0] || !url || !*url) return;
  // The uMS downloads and syncs it by itself — the app only points. That is what
  // allows an OpenSubtitles subtitle on a file that carries none embedded. On this
  // LG, a /file/123 URI produced errorCode 210 "Unknown Subtitle"; the SAME file
  // served as /file/123.srt is recognised by its format.
  video_normalize_url_subtitle(url, recognizable, sizeof recognizable);
  snprintf(b, sizeof b,
           "{\"mediaId\":\"%s\",\"uri\":\"%s\",\"preferredEncodings\":[\"UTF-8\"]}",
           media, recognizable);
  call("setSubtitleSource", b, soLog);
  snprintf(subUrlCurrent, sizeof subUrlCurrent, "%s", recognizable);
  snprintf(b, sizeof b, "{\"mediaId\":\"%s\",\"enable\":true}", media);
  call("setSubtitleEnable", b, soLog);
  applyStyle();
  printf("[video] external subtitle: %.80s\n", recognizable);
  fflush(stdout);
}

void video_shutdown(void) {
  if (!on) return;
  video_stop();
  if (loop) loopStop(loop);
  on = 0;
}
#endif
