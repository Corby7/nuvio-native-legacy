// Real video playback on this TV (OLED55C32LA, webOS 23), over LS2 directly.
//
// The module draws NOTHING. The video lives on a separate HARDWARE PLANE, behind
// the app's GL surface; what the app does is open a transparent hole through
// which that plane shows (see gfx_hole). A consequence that saves hours:
// glReadPixels and the TV's own capture service will NEVER photograph the video.
// That is the model, not a defect — verifying playback is done through the
// state, or through the access log of whoever serves the file.
//
// Why LS2 directly and not StarfishMediaAPIs, measured on the C9 this port was
// born on: the StarfishMediaAPIs constructor calls exit(0) when the process does
// not match the "exeName" of its LS2 role (it is not a crash: atexit fires and
// the journal stays silent), and even with the right role it never got as far as
// talking to com.webos.media — it answered error 202 "Media Not Found", a string
// internal to the library itself. The sequence below came out of an ls-monitor
// capture of the TV's browser playing the same file. On the C3 the question is
// moot: libStarfishMediaAPIs is not on the device at all.
//
// WHAT CHANGED WITH THE C3 (webOS 23), and it is the whole reason this file was
// touched: until webOS 4 the video PLANE was driven with libAcbAPI. That was
// never about the plane — the app registers on LS2 as
// `com.webos.media.client.nuvio`, and that role is not permitted to send to
// com.webos.service.tv.display, so libAcbAPI was used as a proxy that could.
// libAcbAPI.so.1 DOES NOT EXIST on this TV (`find /` returns nothing), and
// requiring it used to take the LS2 half down with it even though the LS2 half
// works perfectly.
//
// The plane now comes from the compositor instead: the app exports its own
// surface (plane.c, wl_webos_foreign), gets a window id back, and that id
// travels in the com.webos.media `load` payload. No display service is
// addressed, so the permission problem simply does not arise. NDL_DirectMedia
// was considered and rejected — NDL_DirectMediaLoad takes codec parameters and
// elementary-stream buffers, not a URL, so using it would mean writing a
// demuxer.
#ifndef NV_VIDEO_H
#define NV_VIDEO_H
#include "mkv.h"

// Registers on the bus and brings up the event loop. 1 on success.
// Failing here is not fatal: the app carries on without video.
int  video_start(void);

// Hands a YouTube video to the TV's own YouTube app (applicationManager/launch,
// contentTarget "v=<id>"). It is how trailers play: this port has no YouTube
// player and no stream extractor, and the TV's app does 4K/HDR without either.
// It goes out through luna-send-pub (see video.c for why), asynchronously — a
// refusal lands in the failure log. 0 when the request could not even leave (an
// id that is not a YouTube id, or no thread).
int  video_launch_youtube(const char *videoId);

// Starts playing. `url` is http(s):// or file://. Do NOT send
// mediaTransportType: the transport comes from the URL's prefix, and sending the
// field makes the load accept, return a mediaId and never fetch the file — a
// silent failure.
int  video_play(const char *url);

// Call ONCE PER FRAME. Today it serves the Dolby Vision fallback deadline (see
// the comment in video.c): without this tick, a file the TV refuses with
// DolbyHdrInfo would stay imageless until the user gave up and left.
void video_pump(void);
void video_stop(void);
void video_pause(int paused);
void video_fetch(double seconds);

// The video plane's rectangle, in 1920x1080 screen coordinates. It is pinned to
// the screen: asking for a negative origin or a size larger than the panel
// BLANKS the plane — a hardware plane does not clip the excess. To enlarge, use
// the function below.
void video_window(int x, int y, int w, int h);

// Real zoom: it crops the SOURCE (decoded-frame coordinates, see
// video_width/video_height) and draws into the destination (screen coordinates).
// Asking for a smaller piece of the source for the same destination is what
// enlarges the image, and what takes the frame's baked-in black bars out of
// view.
void video_window_source(int sx, int sy, int sw, int sh,
                        int dx, int dy, int dw, int dh);

double video_pos(void);      // seconds elapsed
double video_duration(void);  // 0 while unknown
double video_buffer_end(void); // how far the buffer reaches (s); 0 if unknown
// The chosen SOURCE's Dolby Vision claim (the addon describes the file). Call it
// BEFORE video_play/video_set_source: it is what decides the hdrType the ACB
// describes to tv.display.
void video_set_dv(int dv);

// Is the source an MP4? Call BEFORE video_play, alongside video_set_dv.
//
// It exists so we do NOT probe for a Matroska header in a file that will never
// have one. That probe downloads the start of the file over the SAME connection
// that is streaming, and on an MP4 it is guaranteed wasted work — the log itself
// said "no track read" every time.
void video_set_mp4(int isMp4);
// Which load is current. It changes on every video_play and video_stop, so a
// caller that started a video can tell, by keeping the number it saw right
// after, whether the plane is still its own or someone else has loaded over it.
unsigned video_session(void);
int    video_playing(void);
int    video_ready(void);   // 1 after loadCompleted
int    video_active(void);    // 1 as soon as there is a mediaId — this is what opens the hole

// --- tracks ------------------------------------------------------------------
// All of this comes from the sourceInfo event on the uMS subscription: the addon
// reports none of it, and only the pipeline knows what is INSIDE the file.

#define NV_TRACK_MAX 12

typedef struct {
  char label[48];   // "English · Atmos 5.1" or "Subtitle 3"
  char language[8];    // "en"; empty when the file does not tag it
  int  number;       // index selectTrack expects
  // The Matroska CodecID, when the header read got one: "S_TEXT/UTF8",
  // "S_HDMV/PGS", "S_VOBSUB". The PIPELINE does not report this — subtitleTrackInfo
  // carries trackNum, language, type and periodStart and nothing else (see mkv.h) —
  // so it is filled by the same header read that supplies the languages, and stays
  // empty on anything that is not an MKV.
  //
  // It is the only way to tell a TEXT subtitle from a BITMAP one, which is not a
  // detail: a PGS track cannot be restyled, repositioned or resized, so "why does
  // this subtitle ignore my settings" has its answer here.
  char codec[24];
  // Matroska's FlagForced, from the same header read: a track that carries only
  // signs and foreign dialogue. 0 on anything that is not an MKV — tracks.c also
  // reads the word "Forced" in the label, which is how most releases say it.
  int  forced;
} VideoTrack;

int  video_n_audio(void);
int  video_n_subtitle(void);
const VideoTrack *video_audio(int i);
const VideoTrack *video_subtitle(int i);
int  video_audio_current(void);
int  video_subtitle_current(void);   // -1 = off
// A language code as a name for the screen: "eng" -> "English", "pob" ->
// "Portuguese (BR)", an unknown code in capitals, "" for none. The returned
// pointer may be a shared buffer — copy it before asking again.
const char *video_language_name(const char *code);
// The video's frame rate in thousandths (23976 for 23.976 fps), 0 until the
// pipeline has said. tracks.c compares it with an addon subtitle's own frame rate:
// a subtitle timed at 29.97 cannot follow a 23.976 film.
int video_frame_rate_milli(void);

void video_choose_audio(int i);
void video_choose_subtitle(int i);   // -1 turns it off

// Subtitle from an external file (OpenSubtitles). The uMS downloads and syncs
// it on its own; the app only passes the URL.
void video_subtitle_external(const char *url);
// The uMS identifies the format from the URI's extension. Addons often serve
// /file/123 with no .srt; this function makes the URI recognisable without
// changing the file.
void video_normalize_url_subtitle(const char *url, char *dst, unsigned size);

// The playing MKV's header, once the background probe has read it: copied into
// `copy`, with the URL it was read from. 0 before then, for a file that is not
// MKV, and from the moment another playback starts.
int video_mkv_head(MkvHead *copy, char *url, unsigned urlSize);
// 1 while that probe is armed or running: the file is MKV with subtitles and
// its header is on the way. video_mkv_hurry asks for it sooner — AutoSync has
// a subtitle on screen waiting for it.
int  video_mkv_waiting(void);
void video_mkv_hurry(void);

// --- SUBTITLE STYLE ----------------------------------------------------------
//
// PROVEN ON THE DEVICE (LG C9, webOS 4.10, 2026-09-02 — the superseded target;
// NOT re-measured on the C3) with a film playing: the
// five methods below really do change the subtitle on screen. The test was
// visual and not by return value, because the uMS answers `returnValue:true` to
// ANYTHING — it even accepted values I invented for `charEdgeType`. In this API
// the return code is evidence of nothing.
//
// The subtitle is drawn by the PIPELINE, below the GL surface: it does not appear
// in glReadPixels, just like the video. Verifying a change here means looking at
// the screen.
//
// The values are the indices of the options offered on the sheet, not the uMS's:
// the translation into the device's vocabulary lives in video.c, which is what
// knows the pipeline.
typedef struct {
  int size;    // 50..200%, step 10 (120 = default)
  int color;        // index into VIDEO_SUB_COLORS
  int background;      // 0 none; 1..4 = dark 25/50/75/100%
  int position;    // 0..7  -> position -3..4 in the uMS
  int border;      // 0 none, 1 outline, 2 shadow
  int delayMs;   // negative brings it forward
  int opacity;  // 0..3 = text 100/75/50/25%
  int family;    // TxtFamilia; applied to the external overlay (OpenSubtitles)
} VideoSubtitleStyle;

#define VIDEO_SUB_NCOLORS 6
// The names the uMS accepts in charColor. Exposed because the sheet draws the
// labels and needs the same order.
extern const char *const VIDEO_SUB_COLORS[VIDEO_SUB_NCOLORS];
extern const char *const VIDEO_SUB_COLORS_LABEL[VIDEO_SUB_NCOLORS];

// Applies now, if there is a session. The style is KEPT and reapplied on every
// load: the pipeline is born again with each video and does not carry the
// previous setting.
void video_subtitle_style(const VideoSubtitleStyle *e);
// Raises the EMBEDDED subtitle by `steps` of the uMS's position scale on top of the
// viewer's own setting, without changing that setting; 0 puts it back. The pipeline
// draws those cues, so this is the only way to move them. Cheap to call every
// frame: it only reaches the pipeline when the value changes.
void video_subtitle_lift(int steps);

// The truth about the stream, so the screen's badges do not lie.
// THE FAILURE SIGNALS the player's log reads. A count and not a flag, so the
// reader notices EACH new error by comparing against the count it last saw.
// video_last_error is the most recent one as "text (code N)".
int  video_error_count(void);
void video_last_error(char *dst, unsigned n);
int  video_eos_count(void);   // how many times the pipeline reported endOfStream
int  video_has_atmos(void);
// 1 only when the PIPELINE's hdrType says DolbyVision. The source's claim
// (video_set_dv) deliberately does not count here: see the comment in
// video.c.
int  video_has_dolby_vision(void);
const char *video_hdr(void);   // hdrType cru: "none", "HDR10", "DolbyVision"...
// The decoded FRAME's dimensions, from videoInfo. They are what gives the aspect
// the player's zoom modes use.
int  video_width(void);
int  video_height(void);
// What is decoding. "webOS uMS" on the device, and said plainly on the Mac, where
// there is no pipeline at all — the stats panel shows this, and a panel that
// invented an engine name on the preview build would be the one screen in the app
// you cannot trust to tell you what is actually happening.
const char *video_engine(void);

void video_shutdown(void);

#endif
