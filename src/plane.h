// The video plane on webOS 5 and newer (measured on an OLED55C32LA, webOS 23).
//
// WHY THIS FILE EXISTS. On webOS 4 the plane was driven with libAcbAPI, and the
// reason was never the plane itself: the app registers on LS2 as
// `com.webos.media.client.nuvio`, and that role is NOT permitted to send to
// com.webos.service.tv.display ("Not permitted to send to ..."). libAcbAPI could
// reach it, so it was used as a proxy. On this TV libAcbAPI.so.1 DOES NOT EXIST
// — `find /` turns up nothing — and dlopen failing on it used to take the whole
// video subsystem down with it, LS2 half included.
//
// The replacement is not another proxy. The app EXPORTS its own surface through
// the compositor (wl_webos_foreign), the compositor hands back a window id, and
// that id goes into the com.webos.media `load` payload. The display service is
// never addressed, so the permission problem does not exist here at all.
//
// The module draws NOTHING, exactly like the ACB path before it. The video sits
// on a hardware plane BEHIND the GL surface and shows through the hole the app
// opens in it (gfx_hole, plus the non-opaque surface set in main.c). The old
// consequence still holds and still saves hours: glReadPixels and the TV's
// capture service will NEVER photograph the video. Verify through state and
// through the log, never through a screenshot.
//
// EVERYTHING IS dlopen/dlsym, including the wl_interface structs, which are
// DATA symbols. libwayland-webos-client is not in the buildroot-nc4 sysroot, so
// there is no header to compile against and linking would create a DT_NEEDED
// for a library the build machine does not have. This is the same idiom the
// rest of the video path already uses.
#ifndef NV_PLANE_H
#define NV_PLANE_H

// Exports the app's surface as a video object. `display` and `surface` are the
// wl_display and wl_surface that main.c digs out of SDL's SysWM info. Returns 1
// when the export request went out. Call ONCE, from the thread that owns the
// Wayland connection (the render thread) — never from the LS2 GMainLoop thread.
//
// Failing here is not fatal to the app: the UI carries on, and playback then
// fails with a source error instead of a silent black screen.
int plane_start(void *display, void *surface);

// 1 once window_id_assigned has arrived. The `load` MUST NOT be sent before
// this is true: com.webos.media answers returnValue:true to a load carrying an
// unusable windowId, allocates a mediaId and then never fetches the file. That
// is a silent failure, and it is the same trap the old "window_id_dummy" comment
// in video.c warns about for the empty string.
int plane_ready(void);

// The id the compositor assigned, or "" while it has not arrived. Goes into the
// load payload verbatim.
const char *plane_window_id(void);

// Places the video: `s*` crops the SOURCE (decoded-frame coordinates), `d*` is
// the rectangle on the 1920x1080 screen. Asking for a smaller piece of the
// source for the same destination is what zooms in, and what takes a frame's
// baked-in black bars out of view.
//
// BOTH regions are always sent as real wl_region objects. The protocol marks
// neither argument allow-null, and libwayland catches that on the CLIENT side:
// it logs "null value passed for arg N" and DROPS the request. So a NULL is not
// an error you can see happening — it is a call that silently never occurs, and
// the plane keeps whatever rectangle it had. When the whole frame is wanted,
// pass the full source size rather than nothing.
//
// Render thread only, like plane_start.
void plane_window(int sx, int sy, int sw, int sh,
                  int dx, int dy, int dw, int dh);

// Call ONCE PER FRAME, from the render thread, until the id arrives.
//
// WHY IT IS NOT ENOUGH TO ASK AT STARTUP: plane_start runs before the GL context
// exists and long before the first buffer is attached to the surface, and a
// compositor is entitled to withhold the window id until the surface actually
// has content. If that is what this TV does, the id would simply never arrive,
// `assigned` would stay 0, and the single line printed at boot — where nobody
// looks — would be the only trace. This keeps asking for a few seconds and then
// says so, loudly, once.
void plane_pump(void);

// Forgets the last rectangle applied, so the next plane_window sends its
// request even if it asks for the same numbers. Used when something suggests
// the compositor may no longer be holding the region the app thinks it set.
void plane_forget(void);

// Drops the exported window. Safe to call when nothing was ever exported.
void plane_stop(void);

#endif
