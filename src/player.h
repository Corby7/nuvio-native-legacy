// Interface de reproducao nativa, seguindo o Nuvio oficial.
// O video e fornecido por video.c na LG; no Mac ha somente a interface.
#ifndef NV_PLAYER_H
#define NV_PLAYER_H

// VideoSubtitleStyle comes from there: the subtitle style is kept in the
// player's preferences, but the video module is what defines it.
#include "video.h"
#include "catalog.h"
#include <SDL2/SDL.h>

// Opens playback of title `catalogIndex` (a circular index, the same as the
// catalogue's). Title, logo, synopsis and art all come from there.
//
// With NULL, it waits for the source query; it does not fake a playback.
void player_open(int indexCatalog, const char *url);
void player_set_episode(int season, int episode);
void player_episode_current(int *season, int *episode);
int player_index(void);
const char *player_line_episode(void);
int player_requested_sources(void);
int player_requested_next(int *season, int *episode);
const CatEp *player_next_episode(void);
void player_error_source(void);

// 1 when there is real video behind this session. The drawing uses this so it
// does not paint the key art over the video plane.
int  player_com_video(void);
int  player_requested_tracks(void);   // CIMA no player abre audio/legendas

// 1 while the source is opening. The screen shows the key art and an indicator;
// without it the user presses Play and stares at a still screen with no idea
// whether it worked.
int  player_loading(void);
// Exposed for the D-pad regression: DOWN on the row should close the bar.
int  player_controls_visible(void);

// Attaches the source to an already-open session. It exists because the link can
// only be requested at the last moment (see stream_age_ms), so the screen opens
// before there is a URL and the video comes in when it arrives.
void player_set_source(const char *url);

int  player_is_open(void);   // 1 enquanto a tela existe, inclusive durante o fade de saida
void player_event(const SDL_Event *e);
void player_update(float dt, Uint32 now);
void player_draw(Uint32 now);
int  player_wants_exit(void);  // 1 assim que o Back foi apertado
void player_shutdown(void);

// --- ASPECT MODES ------------------------------------------------------------
// The web app's EIGHT modes, in the same order and with the same factors
// (js/core/player/playerAspect.js). The order matters: it is the one the cycle
// walks, and changing it here changes what the owner finds when they press the
// key.
//
// WHY ZOOM, and not object-fit: a widescreen film's black bars are BAKED INTO
// the frame. A 2.39:1 delivered as 3840x2160 has a frame aspect of 1.778 — the
// same as the screen — so "fit" and "fill" give exactly the same image and
// neither crops anything. Cropping requires ENLARGING and letting the excess run
// off the screen.
//
// The factors are 16/9 divided by the film's aspect, not numbers picked to
// taste:  2.35:1 -> 1.32,  2.39:1 -> 1.34,  2.76:1 -> 1.55. ULTRA exists because
// CINEMA (1.34) still leaves a visible bar on a 2.76:1 — observed on the owner's
// TV, not deduced.
//
// In the native app the video is NOT an HTML element: it is a hardware plane
// behind the GL surface, positioned by video_window(). So each mode becomes a
// RECTANGLE, and the web's "excess that leaves the viewport" becomes a rectangle
// with negative coordinates and a size larger than the screen.
typedef enum {
  PLR_ASPECT_ORIGINAL = 0,   // "Fit (Original)"  contain, sem zoom  — PADRAO
  PLR_ASPECT_CROP,           // "Crop"            cover
  PLR_ASPECT_STRETCH,        // "Stretch"         fill
  PLR_ASPECT_ZOOM_LIGHT,      // "Slight Zoom"     cover x 1.15
  PLR_ASPECT_ZOOM_CINEMA,    // "Cinema Zoom"     cover x 1.34
  PLR_ASPECT_ZOOM_ULTRA,     // "Ultra Zoom"      contain x 1.55
  PLR_ASPECT_FIT_HEIGHT,     // "Fit Height"      cover
  PLR_ASPECT_FIT_WIDTH,    // "Fit Width"       contain
  PLR_ASPECT_N
} PlrAspect;

// Fatores de zoom, iguais aos do resolveAspectScale do web.
#define PLR_ZOOM_LIGHT    1.15f
#define PLR_ZOOM_CINEMA  1.34f
#define PLR_ZOOM_ULTRA   1.55f
// How long the mode-change notice stays on screen. 1400ms is the setTimeout in
// the web's showAspectToast.
#define PLR_TOAST_MS     1400u

int         player_aspect(void);              // modo atual (PlrAspecto)
const char *player_aspect_label(int mode);   // "Cinema Zoom", "Encaixar"...
void        player_aspect_set(int mode);  // aplica e grava
void        player_aspect_cycle(void);       // proximo modo + aviso na tela

// THE SUBTITLE STYLE, stored in art/player.txt alongside the aspect: it is a
// DEVICE preference and not a title's. The tracks sheet edits the struct and
// calls player_sub_style_changed(), which applies it to the pipeline and
// saves.
VideoSubtitleStyle *player_sub_style(void);
void player_sub_style_changed(void);

#endif
