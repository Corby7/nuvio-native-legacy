// The Live TV screen: the viewer's IPTV channels, a TV guide, and live playback.
//
// THREE FACES, one module:
//
//   SETUP    no source yet (or "Source" chosen from the header): M3U playlist or
//            Xtream Codes login, typed on the TV's own keyboard (ime.h).
//
//   GUIDE    the grid every IPTV app converges on. A header ("Live TV", the
//            source, Source / Reload); an info band with the focused programme
//            and, at its right, the PREVIEW — the channel last tuned keeps
//            playing there; below, the groups (All, Favourites, Recent, then the
//            playlist's own) beside the guide: channels down, time across, two
//            hours on screen, a line at now.
//
//   LIVE     the tuned channel full screen, with the channel banner (number,
//            logo, now and next with the programme's progress), zapping, number
//            entry and a quick channel list on LEFT. Back returns to the guide
//            and the picture shrinks into the preview; Back again from the guide
//            stops it.
//
// It follows the screen contract of library.h (start/resume/event/update/draw,
// wants_exit, requested_menu), plus two things no other screen has: it can
// cover the side rail (LIVE is full screen), and it owns the video pipeline
// while it is on screen — iptvui_leave hands it back.
#ifndef NV_IPTVUI_H
#define NV_IPTVUI_H
#include <SDL2/SDL.h>

int  iptvui_start(void);
void iptvui_resume(void);
void iptvui_event(const SDL_Event *e);
void iptvui_update(float dt, Uint32 now);
void iptvui_draw(Uint32 now);
int  iptvui_wants_exit(void);       // Back from the guide's first column
int  iptvui_requested_menu(void);   // LEFT at the left edge
// 1 while the tuned channel fills the screen: the router hides the rail and
// sends every key here, the side menu included.
int  iptvui_fullscreen(void);
// The screen is being left: stop the stream if it is ours, lower the keyboard.
void iptvui_leave(void);
// Called every frame while ANOTHER screen is current: stops a stream this screen
// left behind (a profile switch or sign-out changes screen without leaving it
// through the router's swap). Touches nothing else — the keyboard may be
// another screen's by then.
void iptvui_background(void);
void iptvui_shutdown(void);

#endif
