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

// Registers on the bus and brings up the event loop. 1 on success.
// Failing here is not fatal: the app carries on without video.
int  video_start(void);

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

double video_pos(void);      // segundos decorridos
double video_duration(void);  // 0 enquanto desconhecida
double video_buffer_end(void); // ate onde o buffer cobre (s); 0 se desconhecido
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
int    video_playing(void);
int    video_ready(void);   // 1 depois do loadCompleted
int    video_active(void);    // 1 assim que ha mediaId — e o que abre o furo

// --- tracks ------------------------------------------------------------------
// All of this comes from the sourceInfo event on the uMS subscription: the addon
// reports none of it, and only the pipeline knows what is INSIDE the file.

#define NV_TRACK_MAX 12

typedef struct {
  char label[48];   // "Ingles · Atmos 5.1" ou "Legenda 3"
  char language[8];    // "en"; vazio quando o arquivo nao etiqueta
  int  number;       // indice que o selectTrack espera
} VideoTrack;

int  video_n_audio(void);
int  video_n_subtitle(void);
const VideoTrack *video_audio(int i);
const VideoTrack *video_subtitle(int i);
int  video_audio_current(void);
int  video_subtitle_current(void);   // -1 = desligada

void video_choose_audio(int i);
void video_choose_subtitle(int i);   // -1 desliga

// Legenda de arquivo externo (OpenSubtitles). O uMS baixa e sincroniza
// sozinho; o app so passa a URL.
void video_subtitle_external(const char *url);
// The uMS identifies the format from the URI's extension. Addons often serve
// /file/123 with no .srt; this function makes the URI recognisable without
// changing the file.
void video_normalize_url_subtitle(const char *url, char *dst, unsigned size);

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
  int size;    // 50..200%, passo 10 (120 = padrao)
  int color;        // indice em VIDEO_LEG_CORES
  int background;      // 0 nenhum; 1..4 = escuro 25/50/75/100%
  int position;    // 0..7  -> position -3..4 no uMS
  int border;      // 0 nenhuma, 1 contorno, 2 sombra
  int delayMs;   // negativo adianta
  int opacity;  // 0..3 = texto 100/75/50/25%
  int family;    // TxtFamilia; aplicada ao overlay externo (OpenSubtitles)
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

// The truth about the stream, so the screen's badges do not lie.
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

void video_shutdown(void);

#endif
