// The native playback interface, following the official Nuvio.
// The video is supplied by video.c on LG; on the Mac there is only the interface.
#ifndef NV_PLAYER_H
#define NV_PLAYER_H

// VideoSubtitleStyle comes from there: the subtitle style is kept in the
// player's preferences, but the video module is what defines it.
#include "video.h"
#include "catalog.h"
#include "gfx.h"
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
// The next episode, once this one is close enough to its end that the next
// one's sources are worth fetching ahead of time; NULL otherwise.
const CatEp *player_prefetch_next(void);
void player_error_source(void);
// RECORDS A PLAYBACK FAILURE in the failure log (failures.h) and puts a short
// notice on screen. `stage` is where it died — "source", "load" or "playback" —
// and `reason` what is known about why. The title, episode and the current
// source's facts are added here. Repeats of the same failure in one playback
// are logged once.
void player_report_failure(const char *stage, const char *reason);

// 1 when there is real video behind this session. The drawing uses this so it
// does not paint the key art over the video plane.
int  player_has_video(void);
int  player_requested_tracks(void);   // UP in the player opens audio/subtitles

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

int  player_is_open(void);   // 1 while the screen exists, including during the exit fade
void player_event(const SDL_Event *e);
void player_update(float dt, Uint32 now);
void player_draw(Uint32 now);
// The downloaded subtitle, drawn AFTER the track sheets while the subtitle Style
// bar is up, so it sits over the bar's gradient; a no-op otherwise.
void player_draw_subtitle_over(void);
int  player_wants_exit(void);  // 1 as soon as Back was pressed
void player_shutdown(void);
// THE HANDOFF FROM THE TITLE SCREEN. Called right after player_open when Play/Resume
// was pressed there: the player's entrance then crossfades over the page instead of
// rising out of black, and the title's logo flies from `from` (the rect the detail
// last drew it at; w <= 0 when it had none) to the loading screen's centre.
void player_open_from_detail(GfxRect from);
// 1 while that entrance is still running and the page underneath still shows through
// it — the router keeps drawing the detail for exactly that long.
int  player_handing_off(void);
int  player_logo_in_flight(void);   // the page hides its own logo while this is 1

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
  PLR_ASPECT_ORIGINAL = 0,   // "Fit (Original)"  contain, no zoom  — DEFAULT
  PLR_ASPECT_CROP,           // "Crop"            cover
  PLR_ASPECT_STRETCH,        // "Stretch"         fill
  PLR_ASPECT_ZOOM_LIGHT,      // "Slight Zoom"     cover x 1.15
  PLR_ASPECT_ZOOM_CINEMA,    // "Cinema Zoom"     cover x 1.34
  PLR_ASPECT_ZOOM_ULTRA,     // "Ultra Zoom"      contain x 1.55
  PLR_ASPECT_FIT_HEIGHT,     // "Fit Height"      cover
  PLR_ASPECT_FIT_WIDTH,    // "Fit Width"       contain
  PLR_ASPECT_N
} PlrAspect;

// Zoom factors, the same as the web's resolveAspectScale.
#define PLR_ZOOM_LIGHT    1.15f
#define PLR_ZOOM_CINEMA  1.34f
#define PLR_ZOOM_ULTRA   1.55f
// How long the mode-change notice stays on screen. 1400ms is the setTimeout in
// the web's showAspectToast.
#define PLR_TOAST_MS     1400u

int         player_aspect(void);              // current mode (PlrAspect)
const char *player_aspect_label(int mode);   // "Cinema Zoom", "Fit"...
void        player_aspect_set(int mode);  // applies and saves
void        player_aspect_cycle(void);       // next mode + on-screen notice

// THE SUBTITLE STYLE, stored in art/player.txt alongside the aspect: it is a
// DEVICE preference and not a title's. The tracks sheet edits the struct and
// calls player_sub_style_changed(), which applies it to the pipeline and
// saves.
VideoSubtitleStyle *player_sub_style(void);
void player_sub_style_changed(void);

#endif
