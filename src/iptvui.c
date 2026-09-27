// The Live TV screen. See iptvui.h for its three faces; the data comes from
// iptv.h, and the only thing this file knows about the network is that the
// list can change under it between two frames (iptv_step).
//
// ------------------------------------------------------------------------
// THE GUIDE'S FOCUS, which is the part worth reading before changing anything:
//
// A focused cell is a (row, TIME) pair, not a (row, column) pair. Programmes do
// not line up between channels, so a column index means nothing one row down;
// what the viewer expects is that DOWN from a film starting at 21:00 lands on
// whatever the next channel shows at 21:00. `fTime` is that instant. RIGHT moves
// it to the end of the focused programme (the start of the next), LEFT to the
// start of the previous one, and UP/DOWN keep it. It is never before now: what
// already aired is shown, dimmed, but there is nothing to do with it yet (no
// catch-up — see IPTV.md), and LEFT from the programme on air leaves the grid
// for the groups column instead.
//
// The window (`winStart`, two hours wide, snapped to half hours) follows the
// focus; the drawing follows `winAnim`, a spring onto it, so paging through the
// evening glides instead of jumping.
// ------------------------------------------------------------------------
#include "iptvui.h"
#include "iptv.h"
#include "app.h"
#include "anim.h"
#include "gfx.h"
#include "hold.h"
#include "ime.h"
#include "layout.h"
#include "pointer.h"
#include "settings.h"
#include "tex_cache.h"
#include "text.h"
#include "video.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// --- Layout (1920x1080 design space) ----------------------------------------
#define LIVE_RIGHT        (NV_SCREEN_W - 64.0f)
#define LIVE_HEAD_Y        48.0f
#define LIVE_HEAD_BTN_H    56.0f
#define LIVE_HEAD_BTN_GAP  16.0f
#define LIVE_INFO_Y       136.0f
#define LIVE_PREV_W       544.0f   // 16:9
#define LIVE_PREV_H       306.0f
#define LIVE_PREV_R        18.0f
#define LIVE_GUIDE_Y      474.0f   // the time ruler's top
#define LIVE_RULER_H       44.0f
#define LIVE_ROWS_Y       (LIVE_GUIDE_Y + LIVE_RULER_H + 8.0f)
#define LIVE_BOTTOM       (NV_SCREEN_H - 24.0f)
#define LIVE_ROW_H         76.0f
#define LIVE_ROW_STEP      84.0f
#define LIVE_CELL_GAP       6.0f
#define LIVE_CELL_R        12.0f
#define LIVE_GROUPS_W     264.0f
#define LIVE_GROUP_H       60.0f
#define LIVE_GROUP_STEP    66.0f
#define LIVE_COL_GAP       28.0f
#define LIVE_CH_W         392.0f
#define LIVE_LOGO_W        80.0f
#define LIVE_LOGO_H        50.0f
#define LIVE_SPAN_S       7200     // two hours across the guide
#define LIVE_SLOT_S       1800     // snapped to half hours
// How far ahead the guide can be paged; the loader keeps 30 h.
#define LIVE_AHEAD_S      (28 * 3600)

// The full-screen view.
#define LIVE_BANNER_MS    5000u
#define LIVE_ZAP_MS        350u    // CH+/CH- held down tunes only where it stops
#define LIVE_DIGITS_MS    1600u
#define LIVE_TOAST_MS     2600u
#define LIVE_QUICK_W      720.0f
#define LIVE_QUICK_ROW    84.0f

// Colours. The focus follows the detail screen's primary button: white, with
// dark text — on a grid of dark cells it is the only thing that reads as
// "here" from across the room.
#define LIVE_CELL_RGB      0.110f, 0.110f, 0.122f
#define LIVE_CELL_AIR_RGB  0.153f, 0.153f, 0.169f
#define LIVE_FOCUS_RGB     0.961f, 0.961f, 0.961f
#define LIVE_RED_RGB       0.937f, 0.267f, 0.267f
#define LIVE_GOLD_RGB      0.980f, 0.780f, 0.290f

enum { MODE_GUIDE, MODE_SETUP };
enum { ZONE_HEAD, ZONE_GROUPS, ZONE_GUIDE };
enum { HEAD_RELOAD, HEAD_SOURCE, HEAD_N };
enum { GROUP_ALL, GROUP_FAV, GROUP_RECENT, GROUP_FIRST };
static const char *HEAD_LABEL[HEAD_N] = { "Reload", "Source" };

static int mode, zone = ZONE_GUIDE, headSel = HEAD_SOURCE;
static int wantsExit, requestMenu;

// The groups column and the channels it selects.
static int group = GROUP_ALL, groupFocus = GROUP_ALL;
static int *view;
static int nView, capView;
static float groupScroll, groupScrollV;

// The guide's focus: see the note at the top.
static int fRow;
static long long fTime, winStart;
static double winAnim;
static float winAnimV;
static float scrollRows, scrollRowsV;
static char focusName[256];
static Hold hold;

// What is tuned. `tunedName` re-finds it after a reload changes the indices.
static int tuned = -1;
static char tunedName[256];
static int full;
static unsigned playSession;
static int playing, playFailed, errSeen;
static int zapPending;
static Uint32 zapAt, bannerUntil, tunedAt;
static float bannerA, fullA;
static int quickOpen, quickRow;
static float quickScroll, quickScrollV;
static char digits[6];
static Uint32 digitsAt;
static int lastWin[4] = { -1, -1, -1, -1 };

static char toast[160];
static Uint32 toastUntil;

// --- Setup ---------------------------------------------------------------------
enum { ROW_TYPE, ROW_URL, ROW_SERVER, ROW_USER, ROW_PASS, ROW_EPG, ROW_SAVE };
static IptvSource draft;
static int setupRow, setupBtn;
static int setupRows[8], nSetupRows;
static int editing = -1;    // the ROW_* whose text the keyboard is filling

// --- Small helpers ---------------------------------------------------------------
static long long nowSec(void) { return (long long)time(NULL); }
static long long slotFloor(long long t) { return t - (((t % LIVE_SLOT_S) + LIVE_SLOT_S) % LIVE_SLOT_S); }

static void clockText(long long t, char *dst, size_t n) {
  time_t tt = (time_t)t;
  struct tm tmv;
  localtime_r(&tt, &tmv);
  strftime(dst, n, "%H:%M", &tmv);
}

static void say(const char *s) {
  snprintf(toast, sizeof toast, "%s", s);
  toastUntil = SDL_GetTicks() + LIVE_TOAST_MS;
}

static int isOk(SDL_Keycode k) { return k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE; }
static int isBack(SDL_Keycode k) {
  return k == SDLK_ESCAPE || k == SDLK_AC_BACK || k == SDLK_BACKSPACE || k == SDLK_DELETE;
}
static int digitOf(SDL_Keycode k) {
  if (k >= SDLK_0 && k <= SDLK_9) return (int)(k - SDLK_0);
  if (k >= SDLK_KP_1 && k <= SDLK_KP_9) return (int)(k - SDLK_KP_1) + 1;
  if (k == SDLK_KP_0) return 0;
  return -1;
}

static const IptvChannel *chan(int c) {
  const IptvList *l = iptv_list();
  return (l && c >= 0 && c < l->nCh) ? &l->ch[c] : NULL;
}
static int focusedChannel(void) { return (fRow >= 0 && fRow < nView) ? view[fRow] : -1; }

static int findByName(const char *name) {
  const IptvList *l = iptv_list();
  if (!l || !name[0]) return -1;
  for (int i = 0; i < l->nCh; i++) if (!strcmp(l->ch[i].name, name)) return i;
  return -1;
}

static int nGroups(void) {
  const IptvList *l = iptv_list();
  return GROUP_FIRST + (l ? l->nGroups : 0);
}
static const char *groupLabel(int g) {
  const IptvList *l = iptv_list();
  if (g == GROUP_ALL) return "All channels";
  if (g == GROUP_FAV) return "Favourites";
  if (g == GROUP_RECENT) return "Recent";
  return (l && g - GROUP_FIRST < l->nGroups) ? l->groups[g - GROUP_FIRST] : "";
}

// --- The filtered view -----------------------------------------------------------
static void pushView(int c) {
  if (nView == capView) {
    int cap = capView ? capView * 2 : 256;
    int *v = realloc(view, (size_t)cap * sizeof *v);
    if (!v) return;
    view = v; capView = cap;
  }
  view[nView++] = c;
}

// Rebuilds the channels of `group`, keeping the focus on the channel it was on
// when that channel is still in the list.
static void rebuildView(void) {
  const IptvList *l = iptv_list();
  int keep = -1;
  nView = 0;
  if (!l) { fRow = 0; return; }
  if (group >= nGroups()) group = GROUP_ALL;
  if (group == GROUP_RECENT) {
    // iptv_recent answers -1 for a channel the current playlist no longer has,
    // as well as past the end: walk the whole history, skipping the gone.
    for (int i = 0; i < 30; i++) {
      int c = iptv_recent(i);
      if (c >= 0) pushView(c);
    }
  } else {
    for (int c = 0; c < l->nCh; c++) {
      if (group == GROUP_FAV && !iptv_is_favourite(c)) continue;
      if (group >= GROUP_FIRST && l->ch[c].groupIndex != group - GROUP_FIRST) continue;
      pushView(c);
    }
  }
  for (int r = 0; r < nView && focusName[0]; r++)
    if (!strcmp(l->ch[view[r]].name, focusName)) { keep = r; break; }
  fRow = keep >= 0 ? keep : 0;
  if (fRow >= nView) fRow = nView ? nView - 1 : 0;
}

static void rememberFocus(void) {
  const IptvChannel *c = chan(focusedChannel());
  snprintf(focusName, sizeof focusName, "%s", c ? c->name : "");
}

static void chooseGroup(int g) {
  if (g < 0 || g >= nGroups() || g == group) return;
  group = g;
  focusName[0] = 0;
  rebuildView();
  scrollRows = 0.0f; scrollRowsV = 0.0f;
}

// --- The guide's cells -------------------------------------------------------------
// The cell on `ch` at `t`: the programme's index and bounds, or -1 and the bounds
// of the gap around `t` (between two programmes, or the whole of time for a
// channel with no guide at all).
#define LIVE_FOREVER (1LL << 40)
static int cellAt(int ch, long long t, long long *s, long long *e) {
  const IptvList *l = iptv_list();
  int p, nx, first, last;
  *s = -LIVE_FOREVER; *e = LIVE_FOREVER;
  if (!l || ch < 0 || ch >= l->nCh || !l->ch[ch].nPg) return -1;
  if ((p = iptv_programme_at(l, ch, t)) >= 0) { *s = l->pg[p].start; *e = l->pg[p].stop; return p; }
  first = l->ch[ch].firstPg; last = first + l->ch[ch].nPg - 1;
  nx = iptv_programme_after(l, ch, t);
  if (nx >= 0) *e = l->pg[nx].start;
  { int prev = nx >= 0 ? nx - 1 : last;
    if (prev >= first && l->pg[prev].stop <= t) *s = l->pg[prev].stop; }
  return -1;
}

static void keepWindowOnFocus(void) {
  long long floor = slotFloor(nowSec());
  if (fTime < winStart) winStart = slotFloor(fTime);
  while (fTime >= winStart + LIVE_SPAN_S - LIVE_SLOT_S / 2) winStart += LIVE_SLOT_S;
  if (winStart < floor) winStart = floor;
}

static void guideRight(void) {
  long long s, e, now = nowSec();
  int ch = focusedChannel();
  const IptvChannel *c = chan(ch);
  int p = cellAt(ch, fTime, &s, &e);
  if (!c || !c->nPg || e >= LIVE_FOREVER || e >= now + LIVE_AHEAD_S) return;
  (void)p;
  fTime = e;
  keepWindowOnFocus();
}

static void guideLeft(void) {
  long long s, e, now = nowSec();
  int ch = focusedChannel();
  cellAt(ch, fTime, &s, &e);
  if (s <= now) { zone = ZONE_GROUPS; groupFocus = group; return; }
  cellAt(ch, s - 1, &s, &e);
  fTime = s < now ? now : s;
  keepWindowOnFocus();
}

// --- Playback ------------------------------------------------------------------------
static int ownsVideo(void) { return playing && video_session() == playSession; }

static void stopStream(void) {
  if (ownsVideo()) video_stop();
  playing = 0; playFailed = 0; zapPending = 0;
  lastWin[0] = -1;
}

static void startStream(void) {
  char relay[4200];
  const char *url = iptv_play_url(tuned, relay, sizeof relay);
  zapPending = 0;
  playFailed = 0;
  if (!url) return;
  // A live stream is neither Dolby Vision nor an MKV worth probing: the probe
  // would open a second connection, and IPTV accounts allow one.
  video_set_dv(0);
  video_set_mp4(1);
  errSeen = video_error_count();
  playing = video_play(url);
  playSession = video_session();
  lastWin[0] = -1;
  if (!playing) playFailed = 1;
  iptv_note_watched(tuned);
  printf("[live] tuned %d %s%s\n", chan(tuned) ? chan(tuned)->number : 0,
         chan(tuned) ? chan(tuned)->name : "?", playing ? "" : " (pipeline refused)");
  fflush(stdout);
}

// Tunes `ch`. `now` starts the stream at once; otherwise it waits LIVE_ZAP_MS for
// the keys to stop, so a run of CH+ presses opens one stream, not six.
static void tune(int ch, int immediate) {
  const IptvChannel *c = chan(ch);
  if (!c) return;
  tuned = ch;
  snprintf(tunedName, sizeof tunedName, "%s", c->name);
  tunedAt = SDL_GetTicks();
  bannerUntil = tunedAt + LIVE_BANNER_MS;
  playFailed = 0;
  if (immediate) startStream();
  else { zapPending = 1; zapAt = tunedAt + LIVE_ZAP_MS; }
}

// The next channel in the list being browsed, wrapping. Tuned from the full
// screen, it follows the group the viewer came from, not the whole playlist.
static void zap(int step) {
  int r = -1;
  if (!nView) return;
  for (int i = 0; i < nView; i++) if (view[i] == tuned) { r = i; break; }
  r = r < 0 ? 0 : ((r + step) % nView + nView) % nView;
  fRow = r;
  rememberFocus();
  tune(view[r], 0);
}

static void tuneNumber(int number) {
  const IptvList *l = iptv_list();
  if (!l) return;
  for (int c = 0; c < l->nCh; c++)
    if (l->ch[c].number == number) {
      for (int r = 0; r < nView; r++) if (view[r] == c) { fRow = r; break; }
      tune(c, 1);
      return;
    }
  { char m[64]; snprintf(m, sizeof m, "No channel %d", number); say(m); }
}

static void enterFull(void) {
  full = 1;
  quickOpen = 0;
  bannerUntil = SDL_GetTicks() + LIVE_BANNER_MS;
}

// --- Setup form ------------------------------------------------------------------------
static void layoutSetup(void) {
  nSetupRows = 0;
  setupRows[nSetupRows++] = ROW_TYPE;
  if (draft.kind == IPTV_SRC_XTREAM) {
    setupRows[nSetupRows++] = ROW_SERVER;
    setupRows[nSetupRows++] = ROW_USER;
    setupRows[nSetupRows++] = ROW_PASS;
  } else {
    setupRows[nSetupRows++] = ROW_URL;
  }
  setupRows[nSetupRows++] = ROW_EPG;
  setupRows[nSetupRows++] = ROW_SAVE;
  if (setupRow >= nSetupRows) setupRow = nSetupRows - 1;
}

static void openSetup(void) {
  draft = *iptv_source();
  if (draft.kind == IPTV_SRC_NONE) draft.kind = IPTV_SRC_M3U;
  mode = MODE_SETUP;
  setupRow = 0; setupBtn = 0; editing = -1;
  layoutSetup();
  stopStream();
  full = 0;
}

static char *fieldText(int row, int *max) {
  switch (row) {
    case ROW_URL:    *max = (int)sizeof draft.url;    return draft.url;
    case ROW_SERVER: *max = (int)sizeof draft.server; return draft.server;
    case ROW_USER:   *max = (int)sizeof draft.user;   return draft.user;
    case ROW_PASS:   *max = (int)sizeof draft.pass;   return draft.pass;
    case ROW_EPG:    *max = (int)sizeof draft.epg;    return draft.epg;
    default: *max = 0; return NULL;
  }
}
static const char *fieldLabel(int row) {
  switch (row) {
    case ROW_URL:    return "Playlist address (M3U)";
    case ROW_SERVER: return "Server";
    case ROW_USER:   return "Username";
    case ROW_PASS:   return "Password";
    case ROW_EPG:    return "TV guide address (XMLTV) \xC2\xB7 optional";
    default: return "";
  }
}
static const char *fieldHint(int row) {
  switch (row) {
    case ROW_URL:    return "http://provider.example/playlist.m3u";
    case ROW_SERVER: return "http://provider.example:8080";
    case ROW_USER:   return "username";
    case ROW_PASS:   return "password";
    case ROW_EPG:    return draft.kind == IPTV_SRC_XTREAM ? "Leave empty to use the provider's guide"
                                                           : "Leave empty to use the playlist's own";
    default: return "";
  }
}

// The type switch, then the fields at 118 apiece, then the buttons. Xtream's
// four fields are the tallest form, and it ends above 970.
static float setupRowY(int i) {
  float y = 300.0f;
  for (int k = 0; k < i; k++) y += setupRows[k] == ROW_TYPE ? 100.0f : 118.0f;
  return y;
}
static GfxRect setupField(int i) {
  return (GfxRect){ settings_content_x(), setupRowY(i) + 34.0f, 1040.0f, 70.0f };
}

static int draftComplete(void) {
  if (draft.kind == IPTV_SRC_XTREAM) return draft.server[0] && draft.user[0] && draft.pass[0];
  return strstr(draft.url, "://") != NULL;
}

static void saveSetup(void) {
  if (!draftComplete()) {
    say(draft.kind == IPTV_SRC_XTREAM ? "Fill in the server, username and password"
                                      : "Enter the playlist's full address, starting with http");
    return;
  }
  ime_close();
  editing = -1;
  iptv_set_source(&draft);
  mode = MODE_GUIDE;
  zone = ZONE_GUIDE;
  group = groupFocus = GROUP_ALL;
  focusName[0] = 0;
  tuned = -1; tunedName[0] = 0;
  nView = 0; fRow = 0;
  say("Loading your channels\xE2\x80\xA6");
}

static void setupEvent(const SDL_Event *e) {
  SDL_Keycode k;
  int row = setupRows[setupRow];
  if (editing >= 0 && ime_is_open()) {
    int max, len;
    char *t = fieldText(editing, &max);
    len = (int)strlen(t);
    if (ime_edit(e, t, &len, max)) return;
  }
  if (e->type != SDL_KEYDOWN) return;
  k = e->key.keysym.sym;
  if (editing >= 0 && ime_is_open() && !ime_shown() &&
      (k == SDLK_UP || k == SDLK_DOWN || k == SDLK_LEFT || k == SDLK_RIGHT))
    { ime_close(); editing = -1; }
  if (isBack(k)) {
    if (ime_is_open()) { ime_close(); editing = -1; return; }
    if (iptv_configured()) { mode = MODE_GUIDE; return; }
    wantsExit = 1;
    return;
  }
  if (ime_is_open()) {
    // OK with the keyboard up is "done with this field".
    if (isOk(k)) { ime_close(); editing = -1;
      if (setupRow + 1 < nSetupRows) setupRow++; }
    return;
  }
  switch (k) {
    case SDLK_UP:   if (setupRow > 0) setupRow--; break;
    case SDLK_DOWN: if (setupRow + 1 < nSetupRows) setupRow++; break;
    case SDLK_LEFT:
      if (row == ROW_TYPE && draft.kind == IPTV_SRC_XTREAM) { draft.kind = IPTV_SRC_M3U; layoutSetup(); }
      else if (row == ROW_SAVE && setupBtn > 0) setupBtn--;
      else requestMenu = 1;
      break;
    case SDLK_RIGHT:
      if (row == ROW_TYPE && draft.kind != IPTV_SRC_XTREAM) { draft.kind = IPTV_SRC_XTREAM; layoutSetup(); }
      else if (row == ROW_SAVE && iptv_configured() && setupBtn < 1) setupBtn++;
      break;
    default:
      if (!isOk(k)) break;
      if (row == ROW_TYPE) {
        draft.kind = draft.kind == IPTV_SRC_XTREAM ? IPTV_SRC_M3U : IPTV_SRC_XTREAM;
        layoutSetup();
      } else if (row == ROW_SAVE) {
        if (setupBtn == 1) mode = MODE_GUIDE; else saveSetup();
      } else if (!ime_usable()) {
        say("This TV has no on-screen keyboard: put the address in iptv.txt in the app's data folder");
      } else {
        editing = row;
        ime_open(setupField(setupRow));
      }
      break;
  }
}

// --- Pointer ---------------------------------------------------------------------------
static void pointHead(int b, int unused) { (void)unused; zone = ZONE_HEAD; headSel = b; }
static void pointGroup(int g, int unused) { (void)unused; zone = ZONE_GROUPS; groupFocus = g; }
static void pointCell(int row, int t) {
  zone = ZONE_GUIDE;
  if (row != fRow) { fRow = row; rememberFocus(); }
  // `t` is minutes from winStart: the pointer's cell, not the focus's instant.
  { long long at = winStart + (long long)t * 60, now = nowSec();
    fTime = at < now ? now : at; }
}
static void pointSetup(int i, int unused) { (void)unused; if (!ime_is_open()) setupRow = i; }
static void pointQuick(int r, int unused) { (void)unused; quickRow = r; }

// --- Lifecycle ---------------------------------------------------------------------------
int iptvui_start(void) {
  static int started;
  if (!started) { iptv_start(); started = 1; }
  wantsExit = requestMenu = 0;
  full = 0; quickOpen = 0;
  zone = ZONE_GUIDE;
  fTime = nowSec();
  winStart = slotFloor(fTime);
  winAnim = (double)winStart; winAnimV = 0.0f;
  if (!iptv_configured()) openSetup();
  else { mode = MODE_GUIDE; iptv_touch(); rebuildView(); }
  return 1;
}

void iptvui_resume(void) {
  wantsExit = requestMenu = 0;
  full = 0; quickOpen = 0;
  if (iptv_configured()) iptv_touch();
  if (fTime < nowSec()) fTime = nowSec();
  keepWindowOnFocus();
}

void iptvui_leave(void) {
  stopStream();
  full = 0; quickOpen = 0;
  if (ime_is_open()) ime_close();
  editing = -1;
}

void iptvui_background(void) {
  if (playing || zapPending) { stopStream(); full = 0; quickOpen = 0; }
}

void iptvui_shutdown(void) {
  iptvui_leave();
  free(view); view = NULL; nView = capView = 0;
  iptv_shutdown();
}

int iptvui_wants_exit(void) { int v = wantsExit; wantsExit = 0; return v; }
int iptvui_requested_menu(void) { int v = requestMenu; requestMenu = 0; return v; }
int iptvui_fullscreen(void) { return full && tuned >= 0; }

// --- Events ---------------------------------------------------------------------------
static void fullEvent(SDL_Keycode k) {
  int d = digitOf(k);
  Uint32 now = SDL_GetTicks();
  if (d >= 0 && !quickOpen) {
    size_t n = strlen(digits);
    if (n + 1 < sizeof digits) { digits[n] = (char)('0' + d); digits[n + 1] = 0; }
    digitsAt = now;
    return;
  }
  if (quickOpen) {
    if (k == SDLK_UP)        { if (quickRow > 0) quickRow--; }
    else if (k == SDLK_DOWN) { if (quickRow + 1 < nView) quickRow++; }
    else if (k == SDLK_PAGEUP)   { quickRow = quickRow > 8 ? quickRow - 8 : 0; }
    else if (k == SDLK_PAGEDOWN) { quickRow = quickRow + 8 < nView ? quickRow + 8 : nView - 1; }
    else if (isOk(k) && quickRow < nView) { fRow = quickRow; rememberFocus(); tune(view[quickRow], 1); quickOpen = 0; }
    else if (isBack(k) || k == SDLK_RIGHT) quickOpen = 0;
    return;
  }
  switch (k) {
    case SDLK_UP:       zap(-1); break;
    case SDLK_DOWN:     zap(+1); break;
    // CH+ is the higher number, the next one down the list.
    case SDLK_PAGEUP:   zap(+1); break;
    case SDLK_PAGEDOWN: zap(-1); break;
    case SDLK_LEFT:
      quickOpen = 1;
      quickRow = 0;
      for (int r = 0; r < nView; r++) if (view[r] == tuned) { quickRow = r; break; }
      quickScroll = (float)quickRow;
      break;
    case SDLK_RIGHT: bannerUntil = now + LIVE_BANNER_MS; break;
    default:
      if (isOk(k)) {
        // OK over the banner opens the list; OK over the bare picture calls the banner.
        if (now < bannerUntil) { fullEvent(SDLK_LEFT); bannerUntil = 0; }
        else bannerUntil = now + LIVE_BANNER_MS;
      } else if (isBack(k)) {
        // Back to the guide; the picture carries on in the preview.
        full = 0;
        zone = ZONE_GUIDE;
        for (int r = 0; r < nView; r++) if (view[r] == tuned) { fRow = r; break; }
        rememberFocus();
        fTime = nowSec();
        keepWindowOnFocus();
      }
      break;
  }
}

void iptvui_event(const SDL_Event *e) {
  SDL_Keycode k;
  int tap = 0;
  if (mode == MODE_SETUP) { setupEvent(e); return; }
  if (iptvui_fullscreen()) {
    if (e->type == SDL_KEYDOWN) fullEvent(e->key.keysym.sym);
    return;
  }
  // Hold OK on a channel: favourite. A tap is OK: watch.
  if (hold_event(&hold, e, zone == ZONE_GUIDE && nView > 0, &tap)) {
    if (tap && focusedChannel() >= 0) {
      int ch = focusedChannel();
      if (ch == tuned && ownsVideo()) enterFull();
      else { tune(ch, 1); enterFull(); }
    }
    return;
  }
  if (e->type != SDL_KEYDOWN) return;
  k = e->key.keysym.sym;

  if (isBack(k)) {
    // Back climbs: guide -> groups -> leave. A picture in the preview stops on
    // the first Back from the guide rather than following the viewer out.
    if (zone == ZONE_GUIDE && ownsVideo()) { stopStream(); tuned = -1; return; }
    if (zone == ZONE_GUIDE) { zone = ZONE_GROUPS; groupFocus = group; return; }
    if (zone == ZONE_HEAD) { zone = ZONE_GUIDE; return; }
    wantsExit = 1;
    return;
  }

  if (zone == ZONE_HEAD) {
    if (k == SDLK_LEFT) { if (headSel > 0) headSel--; else requestMenu = 1; }
    else if (k == SDLK_RIGHT) { if (headSel + 1 < HEAD_N) headSel++; }
    else if (k == SDLK_DOWN) zone = nView ? ZONE_GUIDE : ZONE_GROUPS;
    else if (isOk(k)) {
      if (headSel == HEAD_SOURCE) openSetup();
      else { iptv_reload(); say("Reloading channels and guide\xE2\x80\xA6"); }
    }
    return;
  }

  if (zone == ZONE_GROUPS) {
    int n = nGroups();
    if (k == SDLK_UP) { if (groupFocus > 0) chooseGroup(--groupFocus); else zone = ZONE_HEAD; }
    else if (k == SDLK_DOWN) { if (groupFocus + 1 < n) chooseGroup(++groupFocus); }
    else if (k == SDLK_PAGEUP) { groupFocus = groupFocus > 6 ? groupFocus - 6 : 0; chooseGroup(groupFocus); }
    else if (k == SDLK_PAGEDOWN) { groupFocus = groupFocus + 6 < n ? groupFocus + 6 : n - 1; chooseGroup(groupFocus); }
    else if (k == SDLK_LEFT) requestMenu = 1;
    else if ((k == SDLK_RIGHT || isOk(k)) && nView) {
      zone = ZONE_GUIDE;
      fTime = nowSec();
      keepWindowOnFocus();
    }
    return;
  }

  // The guide.
  { int d = digitOf(k);
    if (d >= 0) {
      size_t n = strlen(digits);
      if (n + 1 < sizeof digits) { digits[n] = (char)('0' + d); digits[n + 1] = 0; }
      digitsAt = SDL_GetTicks();
      return;
    } }
  switch (k) {
    case SDLK_UP:
      if (fRow > 0) { fRow--; rememberFocus(); }
      else zone = ZONE_HEAD;
      break;
    case SDLK_DOWN:
      if (fRow + 1 < nView) { fRow++; rememberFocus(); }
      break;
    case SDLK_PAGEUP:
      fRow = fRow > 6 ? fRow - 6 : 0; rememberFocus(); break;
    case SDLK_PAGEDOWN:
      fRow = fRow + 6 < nView ? fRow + 6 : (nView ? nView - 1 : 0); rememberFocus(); break;
    case SDLK_LEFT:  guideLeft(); break;
    case SDLK_RIGHT: guideRight(); break;
    default: break;
  }
}

// --- Update -----------------------------------------------------------------------------
void iptvui_update(float dt, Uint32 now) {
  long long t = nowSec();
  if (iptv_step()) {
    // Same playlist text: same indices. A different one: find them by name.
    int c = findByName(tunedName);
    if (c < 0 && tuned >= 0) { stopStream(); tuned = -1; full = 0; }
    else tuned = c;
    rebuildView();
  }
  if (mode != MODE_GUIDE) return;

  hold_animate(&hold, dt, now);
  if (hold_fired(&hold, now) && zone == ZONE_GUIDE && focusedChannel() >= 0) {
    int ch = focusedChannel();
    iptv_toggle_favourite(ch);
    say(iptv_is_favourite(ch) ? "Added to Favourites" : "Removed from Favourites");
    if (group == GROUP_FAV) rebuildView();
  }

  // Time moves on under a guide left open.
  if (fTime < t) fTime = t;
  if (winStart < slotFloor(t)) winStart = slotFloor(t);

  if (zapPending && now >= zapAt) startStream();
  if (ownsVideo() && video_error_count() > errSeen) {
    char why[160] = "";
    video_last_error(why, sizeof why);
    printf("[live] stream error on %s: %s\n", tunedName, why[0] ? why : "?");
    fflush(stdout);
    errSeen = video_error_count();
    playFailed = 1;
    bannerUntil = now + LIVE_BANNER_MS * 2;
  }
  if (digits[0] && now - digitsAt >= LIVE_DIGITS_MS) {
    int n = atoi(digits);
    digits[0] = 0;
    if (n > 0) { tuneNumber(n); if (!full && tuned >= 0) enterFull(); }
  }

  // The quick list replaces the banner rather than sitting over it.
  bannerA = anim_spring(bannerA, (full && !quickOpen && (now < bannerUntil || playFailed)) ? 1.0f : 0.0f,
                        dt, NV_SPRING_FOCUS);
  fullA = anim_spring(fullA, full ? 1.0f : 0.0f, dt, NV_SPRING_FOCUS);

  { int reduced = settings_animations_reduced();
    int visible = (int)((LIVE_BOTTOM - LIVE_ROWS_Y) / LIVE_ROW_STEP);
    float target = scrollRows;
    // The focused row stays a row away from either edge while there are rows beyond.
    if (fRow < target + 1) target = (float)fRow - 1;
    if (fRow > target + visible - 2) target = (float)(fRow - visible + 2);
    if (target > (float)(nView - visible)) target = (float)(nView - visible);
    if (target < 0) target = 0;
    target = (float)(int)target;
    scrollRows = anim_spring2_reduced(&scrollRowsV, scrollRows, target, dt, NV_SPRING2_PAGE, reduced);
    winAnim = anim_spring2_reduced(&winAnimV, (float)(winAnim - winStart), 0.0f, dt,
                                   NV_SPRING2_PAGE, reduced) + (double)winStart;

    { int visibleG = (int)((LIVE_BOTTOM - LIVE_GUIDE_Y) / LIVE_GROUP_STEP);
      int sel = zone == ZONE_GROUPS ? groupFocus : group;
      float g = groupScroll;
      if (sel < g + 1) g = (float)sel - 1;
      if (sel > g + visibleG - 2) g = (float)(sel - visibleG + 2);
      if (g > (float)(nGroups() - visibleG)) g = (float)(nGroups() - visibleG);
      if (g < 0) g = 0;
      groupScroll = anim_spring2_reduced(&groupScrollV, groupScroll, (float)(int)g, dt,
                                         NV_SPRING2_PAGE, reduced); }
    { float q = quickScroll;
      int rows = (int)((NV_SCREEN_H - 200.0f) / LIVE_QUICK_ROW);
      if (quickRow < q + 1) q = (float)quickRow - 1;
      if (quickRow > q + rows - 2) q = (float)(quickRow - rows + 2);
      if (q > (float)(nView - rows)) q = (float)(nView - rows);
      if (q < 0) q = 0;
      quickScroll = anim_spring2_reduced(&quickScrollV, quickScroll, (float)(int)q, dt,
                                         NV_SPRING2_PAGE, reduced); }
  }
}

// --- Drawing helpers -----------------------------------------------------------------------
static void logoFit(const char *url, GfxRect box, float alpha) {
  GLuint tex;
  float a, w, h;
  if (!url || !url[0] || tex_failed(url)) return;
  tex = tex_get_width(url, box.w);
  if (!tex) return;
  a = tex_aspect(url);
  if (a <= 0.0f) a = box.w / box.h;
  if (a > box.w / box.h) { w = box.w; h = box.w / a; } else { h = box.h; w = box.h * a; }
  gfx_opacity_group = alpha;
  gfx_texture((GfxRect){ box.x + (box.w - w) * 0.5f, box.y + (box.h - h) * 0.5f, w, h }, tex);
  gfx_opacity_group = 1.0f;
}

// The channel's initials, where a logo is missing or still loading.
static void logoOrInitials(const IptvChannel *c, GfxRect box, float alpha) {
  if (c->logo[0] && !tex_failed(c->logo) && tex_get_width(c->logo, box.w)) {
    logoFit(c->logo, box, alpha);
    return;
  }
  { char ini[8] = "";
    int n = 0;
    for (const char *p = c->name; *p && n < 3; p++)
      if ((p == c->name || p[-1] == ' ') && ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                                              (*p >= '0' && *p <= '9')))
        ini[n++] = (char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
    ini[n] = 0;
    gfx_color(box, NV_RADIUS_BADGE, 0.2f, 0.2f, 0.22f, alpha);
    if (n) {
      TxtLine t = txt_line(TXT_SRC_TAB, ini, 230, 230, 235, 255);
      txt_draw_alpha(t, box.x + (box.w - t.w) * 0.5f, box.y + (box.h - t.h) * 0.5f, alpha);
    } }
}

static void button(GfxRect r, const char *label, int focused) {
  TxtLine t = focused ? txt_line(TXT_SRC_TAB, label, 18, 18, 20, 255)
                      : txt_line(TXT_SRC_TAB, label, 235, 235, 240, 255);
  if (focused) gfx_color(r, NV_RADIUS_PILL, LIVE_FOCUS_RGB, 1.0f);
  else gfx_color(r, NV_RADIUS_PILL, 1.0f, 1.0f, 1.0f, 0.10f);
  txt_draw(t, r.x + (r.w - t.w) * 0.5f, r.y + (r.h - t.h) * 0.5f);
}
static float buttonW(const char *label) {
  TxtLine t = txt_line(TXT_SRC_TAB, label, 235, 235, 240, 255);
  return (float)t.w + 64.0f;
}

static void progressBar(float x, float y, float w, float frac, float alpha) {
  if (frac < 0.0f) frac = 0.0f;
  if (frac > 1.0f) frac = 1.0f;
  gfx_color((GfxRect){ x, y, w, 6.0f }, NV_RADIUS_PILL, 1.0f, 1.0f, 1.0f, 0.18f * alpha);
  if (frac > 0.0f)
    gfx_color((GfxRect){ x, y, w * frac, 6.0f }, NV_RADIUS_PILL, LIVE_RED_RGB, alpha);
}

static void drawToast(void) {
  Uint32 now = SDL_GetTicks();
  TxtLine t;
  GfxRect r;
  if (!toast[0] || now >= toastUntil) return;
  t = txt_line_trim(TXT_SRC_TAB, toast, 240, 240, 245, 255, 1200.0f);
  // Full screen, the bottom belongs to the channel banner.
  r = (GfxRect){ (NV_SCREEN_W - t.w) * 0.5f - 32.0f, iptvui_fullscreen() ? 56.0f : NV_SCREEN_H - 120.0f,
                 t.w + 64.0f, 64.0f };
  gfx_color(r, NV_RADIUS_PILL, 0.12f, 0.12f, 0.14f, 0.96f);
  txt_draw(t, r.x + 32.0f, r.y + (r.h - t.h) * 0.5f);
}

static void drawDigits(void) {
  TxtLine t;
  GfxRect r;
  if (!digits[0]) return;
  t = txt_line(TXT_TITLE1, digits, 255, 255, 255, 255);
  // Top right of the picture: full screen that is the screen's corner; on the
  // guide it is the preview's, below the header's buttons.
  r = (GfxRect){ LIVE_RIGHT - t.w - 64.0f - (iptvui_fullscreen() ? 0.0f : 16.0f),
                 iptvui_fullscreen() ? 56.0f : LIVE_INFO_Y + 16.0f, t.w + 64.0f, t.h + 24.0f };
  gfx_color(r, NV_RADIUS_BADGE, 0.06f, 0.06f, 0.07f, 0.88f);
  txt_draw(t, r.x + 32.0f, r.y + 12.0f);
}

// The picture: a hole onto the hardware plane on the TV, the decoded frame on
// the Mac preview. The window is only re-sent when it moves.
static void drawVideo(GfxRect r, float radius) {
  GLuint frame;
  int w[4] = { (int)(r.x + 0.5f), (int)(r.y + 0.5f), (int)(r.w + 0.5f), (int)(r.h + 0.5f) };
  if (memcmp(w, lastWin, sizeof w)) {
    video_window(w[0], w[1], w[2], w[3]);
    memcpy(lastWin, w, sizeof w);
  }
  frame = video_frame_texture();
  if (frame) {
    gfx_tex_aspect_current = 0.0f;
    gfx_rect(r, frame, GFX_CARD, 0, 0, 0, radius / r.h, 0, 0, 0, 1);
  } else if (radius > 0.0f) {
    gfx_hole_round(r, radius / r.h);
  } else {
    gfx_hole(r);
  }
}

// --- The guide ---------------------------------------------------------------------------------
static void drawHeader(void) {
  float x = settings_content_x(), right = LIVE_RIGHT;
  const IptvList *l = iptv_list();
  char line[256];
  txt_tracking(TXT_TITLE3, "Live TV", 255, 255, 255, x, LIVE_HEAD_Y, 1.0f, NV_DSC_TITLE_LS);
  for (int b = HEAD_N - 1; b >= 0; b--) {
    float w = buttonW(HEAD_LABEL[b]);
    GfxRect r = { right - w, LIVE_HEAD_Y - 4.0f, w, LIVE_HEAD_BTN_H };
    button(r, HEAD_LABEL[b], zone == ZONE_HEAD && headSel == b);
    pointer_zone(r.x, r.y, r.w, r.h, pointHead, b, 0);
    right -= w + LIVE_HEAD_BTN_GAP;
  }
  { const char *st = iptv_status();
    if (l) snprintf(line, sizeof line, "%s  \xC2\xB7  %d channels%s%s", iptv_source_label(), l->nCh,
                    st[0] ? "  \xC2\xB7  " : "", st);
    else snprintf(line, sizeof line, "%s%s%s", iptv_source_label(), st[0] ? "  \xC2\xB7  " : "", st);
    { TxtLine t = txt_line_trim(TXT_SRCH_NAME, line, 128, 128, 128, 255, right - x - 420.0f);
      txt_draw_alpha(t, right - 16.0f - t.w, LIVE_HEAD_Y + 10.0f, 0.95f); } }
}

static void drawInfo(void) {
  float x = settings_content_x();
  GfxRect prev = { LIVE_RIGHT - LIVE_PREV_W, LIVE_INFO_Y, LIVE_PREV_W, LIVE_PREV_H };
  float w = prev.x - 56.0f - x;
  const IptvList *l = iptv_list();
  int ch = zone == ZONE_GUIDE || tuned < 0 ? focusedChannel() : tuned;
  const IptvChannel *c = chan(ch);
  long long now = nowSec();

  // The preview.
  if (ownsVideo()) {
    drawVideo(prev, LIVE_PREV_R);
    { const IptvChannel *tc = chan(tuned);
      if (tc) {
        char s[300];
        TxtLine t;
        snprintf(s, sizeof s, "%d  %s", tc->number, tc->name);
        t = txt_line_trim(TXT_SRC_META, s, 255, 255, 255, 255, prev.w - 120.0f);
        gfx_color((GfxRect){ prev.x + 16.0f, prev.y + prev.h - 52.0f, t.w + 92.0f, 36.0f },
                  NV_RADIUS_PILL, 0.0f, 0.0f, 0.0f, 0.62f);
        gfx_color((GfxRect){ prev.x + 30.0f, prev.y + prev.h - 38.0f, 8.0f, 8.0f }, 0.5f,
                  LIVE_RED_RGB, 1.0f);
        { TxtLine live = txt_line(TXT_SRC_TIER, "LIVE", 255, 255, 255, 255);
          txt_draw(live, prev.x + 46.0f, prev.y + prev.h - 34.0f - live.h * 0.5f);
          txt_draw(t, prev.x + 56.0f + live.w, prev.y + prev.h - 34.0f - t.h * 0.5f); }
        if (playFailed) {
          TxtLine m = txt_line(TXT_SRC_TAB, "Channel unavailable", 255, 255, 255, 255);
          txt_draw(m, prev.x + (prev.w - m.w) * 0.5f, prev.y + (prev.h - m.h) * 0.5f);
        }
      } }
  } else if (c) {
    gfx_color(prev, LIVE_PREV_R / prev.h, 0.08f, 0.08f, 0.09f, 1.0f);
    {
      logoOrInitials(c, (GfxRect){ prev.x + prev.w * 0.5f - 90.0f, prev.y + prev.h * 0.5f - 70.0f,
                                   180.0f, 100.0f }, 1.0f);
      { TxtLine t = txt_line(TXT_SRCH_META, "Press OK to watch", 150, 150, 155, 255);
        txt_draw(t, prev.x + (prev.w - t.w) * 0.5f, prev.y + prev.h * 0.5f + 52.0f); }
    }
  }

  if (!c) return;
  { char kicker[300], when[160], a[16], b[16];
    long long s, e;
    int p = cellAt(ch, zone == ZONE_GUIDE ? fTime : now, &s, &e);
    float y = LIVE_INFO_Y + 4.0f;
    snprintf(kicker, sizeof kicker, "%d  \xC2\xB7  %s%s", c->number, c->name,
             iptv_is_favourite(ch) ? "  \xE2\x98\x85" : "");
    { TxtLine t = txt_line_trim(TXT_SRC_META, kicker, 170, 170, 175, 255, w);
      txt_draw(t, x, y); y += t.h + 10.0f; }
    if (p >= 0) {
      const IptvProgramme *pg = &l->pg[p];
      TxtLine t = txt_line_trim(TXT_TITLE2, pg->title, 255, 255, 255, 255, w);
      txt_draw(t, x, y); y += t.h + 8.0f;
      clockText(pg->start, a, sizeof a); clockText(pg->stop, b, sizeof b);
      if (pg->start <= now && now < pg->stop)
        snprintf(when, sizeof when, "%s \xE2\x80\x93 %s  \xC2\xB7  %lld min left%s%s", a, b,
                 (pg->stop - now + 59) / 60, pg->category[0] ? "  \xC2\xB7  " : "", pg->category);
      else
        snprintf(when, sizeof when, "%s \xE2\x80\x93 %s  \xC2\xB7  %lld min%s%s", a, b,
                 (pg->stop - pg->start) / 60, pg->category[0] ? "  \xC2\xB7  " : "", pg->category);
      { TxtLine m = txt_line_trim(TXT_DET_META, when, 190, 190, 195, 255, w);
        txt_draw(m, x, y); y += m.h + 12.0f; }
      if (pg->start <= now && now < pg->stop) {
        progressBar(x, y, 420.0f, (float)(now - pg->start) / (float)(pg->stop - pg->start), 1.0f);
        y += 22.0f;
      }
      if (pg->desc[0])
        txt_block_trim(TXT_DET_SIN, pg->desc, 200, 200, 205, x, y, w, 36.0f, 0.9f, 3);
    } else {
      TxtLine t = txt_line_trim(TXT_TITLE2, c->name, 255, 255, 255, 255, w);
      const char *why = iptv_guide_state() == IPTV_LOADING ? "The TV guide is still loading"
                      : c->nPg ? "No information for this time" : "No guide information for this channel";
      txt_draw(t, x, y); y += t.h + 8.0f;
      { TxtLine m = txt_line(TXT_DET_META, why, 160, 160, 165, 255); txt_draw(m, x, y); }
    }
  }
}

static void drawGroups(void) {
  float x = settings_content_x();
  int n = nGroups();
  gfx_crop(0.0f, LIVE_GUIDE_Y, NV_SCREEN_W, LIVE_BOTTOM - LIVE_GUIDE_Y);
  pointer_clip(0.0f, LIVE_GUIDE_Y, NV_SCREEN_W, LIVE_BOTTOM - LIVE_GUIDE_Y);
  for (int g = 0; g < n; g++) {
    float y = LIVE_GUIDE_Y + (g - groupScroll) * LIVE_GROUP_STEP;
    int focused = zone == ZONE_GROUPS && g == groupFocus, chosen = g == group;
    GfxRect r = { x, y, LIVE_GROUPS_W, LIVE_GROUP_H };
    if (y + LIVE_GROUP_H < LIVE_GUIDE_Y - 4.0f || y > LIVE_BOTTOM) continue;
    if (focused) gfx_color(r, NV_RADIUS_PILL, LIVE_FOCUS_RGB, 1.0f);
    else if (chosen) gfx_color(r, NV_RADIUS_PILL, NV_COLOR_FOCUS_R, NV_COLOR_FOCUS_G, NV_COLOR_FOCUS_B, 1.0f);
    { int v = focused ? 18 : chosen ? 255 : 190;
      TxtLine t = txt_line_trim(chosen || focused ? TXT_MENU_SEL : TXT_MENU_ITEM, groupLabel(g),
                                v, v, focused ? 20 : v, 255, LIVE_GROUPS_W - 48.0f);
      txt_draw(t, x + 24.0f, y + (LIVE_GROUP_H - t.h) * 0.5f); }
    pointer_zone(r.x, r.y, r.w, r.h, pointGroup, g, 0);
  }
  gfx_no_crop();
  pointer_no_clip();
}

static void drawRuler(float tx, float tw, double ws, double pps, long long now) {
  float y = LIVE_GUIDE_Y;
  float gx = settings_content_x() + LIVE_GROUPS_W + LIVE_COL_GAP;
  char day[48];
  { time_t tt = (time_t)now; struct tm tmv; localtime_r(&tt, &tmv);
    strftime(day, sizeof day, "%a %d %b", &tmv); }
  { TxtLine t = txt_line(TXT_SRC_META, day, 150, 150, 155, 255);
    txt_draw(t, gx + 4.0f, y + (LIVE_RULER_H - t.h) * 0.5f); }
  gfx_crop(tx, y, tw, LIVE_BOTTOM - y);
  for (long long s = slotFloor((long long)ws); s < (long long)ws + LIVE_SPAN_S + LIVE_SLOT_S; s += LIVE_SLOT_S) {
    float sx = tx + (float)((double)(s - ws) * pps);
    char c[16];
    clockText(s, c, sizeof c);
    gfx_color((GfxRect){ sx, y + 8.0f, 2.0f, LIVE_RULER_H - 16.0f }, 0.0f, 1.0f, 1.0f, 1.0f, 0.16f);
    { TxtLine t = txt_line(TXT_SRC_META, c, 190, 190, 195, 255);
      txt_draw(t, sx + 12.0f, y + (LIVE_RULER_H - t.h) * 0.5f); }
  }
  gfx_no_crop();
}

static void drawGuide(void) {
  const IptvList *l = iptv_list();
  float gx = settings_content_x() + LIVE_GROUPS_W + LIVE_COL_GAP;
  float tx = gx + LIVE_CH_W + 12.0f, tw = LIVE_RIGHT - tx;
  double ws = winAnim, we = ws + LIVE_SPAN_S;
  double pps = tw / (double)LIVE_SPAN_S;
  long long now = nowSec();
  int focusPg = -1;
  long long fs = 0, fe = 0;

  if (!l || !nView) {
    const char *l1, *l2;
    int st = iptv_state();
    if (!l && st == IPTV_FAILED) { l1 = iptv_status(); l2 = "Check the address or login under Source, above."; }
    else if (!l) { l1 = "Loading channels\xE2\x80\xA6"; l2 = "Large playlists can take a few seconds."; }
    else if (group == GROUP_FAV) { l1 = "No favourites yet"; l2 = "Hold OK on a channel in the guide to add it here."; }
    else if (group == GROUP_RECENT) { l1 = "Nothing watched yet"; l2 = "Channels you watch appear here."; }
    else { l1 = "No channels in this group"; l2 = ""; }
    { TxtLine a = txt_line(TXT_TITLE2, l1, 255, 255, 255, 255);
      TxtLine b = txt_line(TXT_SRCH_EMPTY, l2, 170, 170, 175, 255);
      float cx = gx + (LIVE_RIGHT - gx) * 0.5f;
      txt_draw(a, cx - a.w * 0.5f, LIVE_ROWS_Y + 120.0f);
      if (l2[0]) txt_draw(b, cx - b.w * 0.5f, LIVE_ROWS_Y + 132.0f + a.h); }
    return;
  }

  drawRuler(tx, tw, ws, pps, now);
  if (zone == ZONE_GUIDE) focusPg = cellAt(focusedChannel(), fTime, &fs, &fe);

  gfx_crop(0.0f, LIVE_ROWS_Y - 4.0f, NV_SCREEN_W, LIVE_BOTTOM - LIVE_ROWS_Y + 4.0f);
  pointer_clip(0.0f, LIVE_ROWS_Y - 4.0f, NV_SCREEN_W, LIVE_BOTTOM - LIVE_ROWS_Y + 4.0f);
  for (int r = (int)scrollRows; r < nView; r++) {
    float y = LIVE_ROWS_Y + (r - scrollRows) * LIVE_ROW_STEP;
    int ch = view[r], rowFocus = zone == ZONE_GUIDE && r == fRow;
    const IptvChannel *c = &l->ch[ch];
    if (y > LIVE_BOTTOM) break;

    // The channel.
    { GfxRect cr = { gx, y, LIVE_CH_W, LIVE_ROW_H };
      char num[16];
      if (rowFocus) gfx_color(cr, LIVE_CELL_R / cr.h, NV_COLOR_FOCUS_R, NV_COLOR_FOCUS_G, NV_COLOR_FOCUS_B, 1.0f);
      else gfx_color(cr, LIVE_CELL_R / cr.h, LIVE_CELL_RGB, 1.0f);
      if (ch == tuned && ownsVideo())
        gfx_color((GfxRect){ gx, y + 14.0f, 5.0f, LIVE_ROW_H - 28.0f }, NV_RADIUS_PILL, LIVE_RED_RGB, 1.0f);
      snprintf(num, sizeof num, "%d", c->number);
      { TxtLine t = txt_line(TXT_SRCH_META, num, 150, 150, 155, 255);
        txt_draw(t, gx + 18.0f, y + (LIVE_ROW_H - t.h) * 0.5f); }
      logoOrInitials(c, (GfxRect){ gx + 76.0f, y + (LIVE_ROW_H - LIVE_LOGO_H) * 0.5f, LIVE_LOGO_W, LIVE_LOGO_H }, 1.0f);
      { float nx = gx + 76.0f + LIVE_LOGO_W + 16.0f;
        float nw = LIVE_CH_W - (nx - gx) - (iptv_is_favourite(ch) ? 44.0f : 16.0f);
        TxtLine t = txt_line_trim(TXT_SRCH_NAME, c->name, 235, 235, 240, 255, nw);
        txt_draw(t, nx, y + (LIVE_ROW_H - t.h) * 0.5f);
        if (iptv_is_favourite(ch)) {
          TxtLine s = txt_line(TXT_SRCH_NAME, "\xE2\x98\x85", 250, 199, 74, 255);
          txt_draw(s, gx + LIVE_CH_W - 16.0f - s.w, y + (LIVE_ROW_H - s.h) * 0.5f);
        } }
      // Pointing at the channel is pointing at what is on now.
      pointer_zone(cr.x, cr.y, cr.w, cr.h, pointCell, r, 0); }

    // The programmes.
    gfx_crop(tx, LIVE_ROWS_Y - 4.0f, tw, LIVE_BOTTOM - LIVE_ROWS_Y + 4.0f);
    if (!c->nPg) {
      GfxRect cell = { tx, y, tw - LIVE_CELL_GAP, LIVE_ROW_H };
      int f = rowFocus;
      const char *msg = iptv_guide_state() == IPTV_LOADING ? "Loading guide\xE2\x80\xA6"
                                                           : "No programme information";
      if (f) gfx_color(cell, LIVE_CELL_R / cell.h, LIVE_FOCUS_RGB, 1.0f);
      else gfx_color(cell, LIVE_CELL_R / cell.h, LIVE_CELL_RGB, 1.0f);
      { TxtLine t = f ? txt_line(TXT_SRCH_NAME, msg, 18, 18, 20, 255)
                      : txt_line(TXT_SRCH_NAME, msg, 140, 140, 145, 255);
        txt_draw(t, cell.x + 20.0f, y + (LIVE_ROW_H - t.h) * 0.5f); }
      pointer_zone(cell.x, cell.y, cell.w, cell.h, pointCell, r, 0);
    } else {
      int p = iptv_programme_at(l, ch, (long long)ws);
      int end = c->firstPg + c->nPg;
      if (p < 0) p = iptv_programme_after(l, ch, (long long)ws);
      for (; p >= 0 && p < end && (double)l->pg[p].start < we; p++) {
        const IptvProgramme *pg = &l->pg[p];
        double s = (double)pg->start < ws ? ws : (double)pg->start;
        double e = (double)pg->stop > we ? we : (double)pg->stop;
        float x0 = tx + (float)((s - ws) * pps), x1 = tx + (float)((e - ws) * pps) - LIVE_CELL_GAP;
        int f = rowFocus && p == focusPg;
        int airing = pg->start <= now && now < pg->stop, past = pg->stop <= now;
        GfxRect cell = { x0, y, x1 - x0, LIVE_ROW_H };
        if (cell.w < 4.0f) continue;
        if (f) gfx_color(cell, LIVE_CELL_R / cell.h, LIVE_FOCUS_RGB, 1.0f);
        else if (airing) gfx_color(cell, LIVE_CELL_R / cell.h, LIVE_CELL_AIR_RGB, 1.0f);
        else gfx_color(cell, LIVE_CELL_R / cell.h, LIVE_CELL_RGB, past ? 0.6f : 1.0f);
        if (cell.w > 44.0f) {
          int v = f ? 18 : past ? 120 : 235;
          TxtLine t = txt_line_trim(TXT_SRCH_NAME, pg->title, v, v, f ? 20 : v, 255, cell.w - 32.0f);
          float ty = y + (cell.w > 150.0f ? 10.0f : (LIVE_ROW_H - t.h) * 0.5f);
          txt_draw(t, cell.x + 16.0f, ty);
          if (cell.w > 150.0f) {
            char a[16], b[16], when[40];
            int m = f ? 70 : 140;
            clockText(pg->start, a, sizeof a); clockText(pg->stop, b, sizeof b);
            snprintf(when, sizeof when, "%s \xE2\x80\x93 %s", a, b);
            { TxtLine w2 = txt_line_trim(TXT_SRCH_META, when, m, m, m + 4, 255, cell.w - 32.0f);
              txt_draw(w2, cell.x + 16.0f, ty + t.h + 4.0f); }
          }
        }
        { int mins = (int)(((pg->start > (long long)ws ? pg->start : (long long)ws) - winStart) / 60);
          pointer_zone(cell.x, cell.y, cell.w, cell.h, pointCell, r, mins < 0 ? 0 : mins); }
      }
      // A focused gap between two programmes.
      if (rowFocus && focusPg < 0) {
        double s = (double)fs < ws ? ws : (double)fs, e = (double)fe > we ? we : (double)fe;
        float x0 = tx + (float)((s - ws) * pps), x1 = tx + (float)((e - ws) * pps) - LIVE_CELL_GAP;
        if (x1 - x0 > 4.0f) {
          GfxRect cell = { x0, y, x1 - x0, LIVE_ROW_H };
          gfx_color(cell, LIVE_CELL_R / cell.h, LIVE_FOCUS_RGB, 1.0f);
          if (cell.w > 60.0f) {
            TxtLine t = txt_line_trim(TXT_SRCH_NAME, "No information", 18, 18, 20, 255, cell.w - 32.0f);
            txt_draw(t, cell.x + 16.0f, y + (LIVE_ROW_H - t.h) * 0.5f);
          }
        }
      }
    }
    gfx_crop(0.0f, LIVE_ROWS_Y - 4.0f, NV_SCREEN_W, LIVE_BOTTOM - LIVE_ROWS_Y + 4.0f);
  }
  gfx_no_crop();
  pointer_no_clip();

  // Now: a line down the guide, with its dot on the ruler.
  if ((double)now >= ws && (double)now < we) {
    // Snapped to a whole pixel: at a fractional x the 2 px line rasterises as
    // one faint pixel, and it is the one mark on the guide that must not fade.
    float nx = (float)(int)(tx + (float)(((double)now - ws) * pps) + 0.5f);
    float bottom = LIVE_ROWS_Y + (nView - scrollRows) * LIVE_ROW_STEP;
    if (bottom > LIVE_BOTTOM) bottom = LIVE_BOTTOM;
    gfx_color((GfxRect){ nx - 1.0f, LIVE_GUIDE_Y + LIVE_RULER_H - 6.0f, 2.0f,
                         bottom - (LIVE_GUIDE_Y + LIVE_RULER_H - 6.0f) }, 0.0f, LIVE_RED_RGB, 0.9f);
    gfx_color((GfxRect){ nx - 6.0f, LIVE_GUIDE_Y + LIVE_RULER_H - 12.0f, 12.0f, 12.0f }, 0.5f,
              LIVE_RED_RGB, 1.0f);
  }
}

// --- Setup ---------------------------------------------------------------------------------------
static void drawSetup(void) {
  float x = settings_content_x();
  txt_tracking(TXT_TITLE3, "Live TV", 255, 255, 255, x, LIVE_HEAD_Y, 1.0f, NV_DSC_TITLE_LS);
  { TxtLine t = txt_line(TXT_HEADLINE, iptv_configured() ? "Change your IPTV source"
                                                         : "Connect your IPTV service", 255, 255, 255, 255);
    txt_draw(t, x, 128.0f); }
  txt_block(TXT_BODY,
            "Use the M3U playlist address your provider gave you, or sign in with an Xtream "
            "Codes login. The TV guide is found automatically when the source names one.",
            170, 170, 175, x, 186.0f, 1040.0f, 40.0f, 1.0f, 3);

  for (int i = 0; i < nSetupRows; i++) {
    int row = setupRows[i], focused = i == setupRow;
    float y = setupRowY(i);
    if (row == ROW_TYPE) {
      static const char *KIND[2] = { "M3U playlist", "Xtream Codes" };
      float bx = x;
      for (int k = 0; k < 2; k++) {
        int on = (k == 0) == (draft.kind != IPTV_SRC_XTREAM);
        float w = buttonW(KIND[k]);
        GfxRect r = { bx, y, w, LIVE_HEAD_BTN_H + 4.0f };
        if (focused && on) button(r, KIND[k], 1);
        else {
          gfx_color(r, NV_RADIUS_PILL, 1.0f, 1.0f, 1.0f, on ? 0.22f : 0.06f);
          { TxtLine t = txt_line(TXT_SRC_TAB, KIND[k], on ? 255 : 150, on ? 255 : 150, on ? 255 : 155, 255);
            txt_draw(t, r.x + (r.w - t.w) * 0.5f, r.y + (r.h - t.h) * 0.5f); }
        }
        pointer_zone(r.x, r.y, r.w, r.h, pointSetup, i, 0);
        bx += w + 12.0f;
      }
    } else if (row == ROW_SAVE) {
      float w = buttonW("Save and load");
      GfxRect r = { x, y + 8.0f, w, LIVE_HEAD_BTN_H + 8.0f };
      button(r, "Save and load", focused && setupBtn == 0);
      pointer_zone(r.x, r.y, r.w, r.h, pointSetup, i, 0);
      if (iptv_configured()) {
        GfxRect c = { x + w + 16.0f, r.y, buttonW("Cancel"), r.h };
        button(c, "Cancel", focused && setupBtn == 1);
      }
    } else {
      int max;
      const char *text = fieldText(row, &max);
      GfxRect f = setupField(i);
      int isEditing = editing == row && ime_is_open();
      char shown[1100];
      TxtLine lab = txt_line(TXT_TRK_LABEL, fieldLabel(row), 160, 160, 165, 255);
      txt_draw(lab, x + 4.0f, y);
      if (focused) gfx_color((GfxRect){ f.x - 3.0f, f.y - 3.0f, f.w + 6.0f, f.h + 6.0f },
                             NV_RADIUS_BADGE, 1.0f, 1.0f, 1.0f, isEditing ? 1.0f : 0.85f);
      gfx_color(f, NV_RADIUS_BADGE, 0.13f, 0.13f, 0.15f, 1.0f);
      if (row == ROW_PASS && text[0]) {
        size_t n = strlen(text), k = 0;
        for (size_t j = 0; j < n && k + 4 < sizeof shown; j++)
          if (((unsigned char)text[j] & 0xC0) != 0x80) { memcpy(shown + k, "\xE2\x80\xA2", 3); k += 3; }
        shown[k] = 0;
      } else {
        snprintf(shown, sizeof shown, "%s", text);
      }
      if (shown[0] || isEditing) {
        // The end of a long address is the part being typed: trim from the left.
        const char *s = shown;
        TxtLine t = txt_line(TXT_DD_SEL, s, 245, 245, 250, 255);
        while (t.w > f.w - 64.0f && *s) {
          s++;
          while (((unsigned char)*s & 0xC0) == 0x80) s++;
          t = txt_line(TXT_DD_SEL, s, 245, 245, 250, 255);
        }
        txt_draw(t, f.x + 24.0f, f.y + (f.h - t.h) * 0.5f);
        if (isEditing && (SDL_GetTicks() / 530u) % 2u == 0u)
          gfx_color((GfxRect){ f.x + 26.0f + t.w, f.y + 18.0f, 3.0f, f.h - 36.0f }, 0.0f, 1, 1, 1, 1);
      } else {
        TxtLine t = txt_line_trim(TXT_DD_SEL, fieldHint(row), 110, 110, 115, 255, f.w - 48.0f);
        txt_draw(t, f.x + 24.0f, f.y + (f.h - t.h) * 0.5f);
      }
      pointer_zone(f.x, f.y, f.w, f.h, pointSetup, i, 0);
    }
  }
}

// --- Full screen ---------------------------------------------------------------------------------
static void drawBanner(float a) {
  const IptvList *l = iptv_list();
  const IptvChannel *c = chan(tuned);
  long long now = nowSec();
  GfxRect plate = { 64.0f, NV_SCREEN_H - 64.0f - 232.0f, NV_SCREEN_W - 128.0f, 232.0f };
  float x;
  char s[320], a1[16], b1[16];
  if (!c || a < 0.01f) return;
  gfx_opacity_group = a;
  gfx_color(plate, 28.0f / plate.h, 0.05f, 0.05f, 0.06f, 0.88f);
  { char num[16]; TxtLine t;
    snprintf(num, sizeof num, "%d", c->number);
    t = txt_line(TXT_TITLE2, num, 255, 255, 255, 255);
    txt_draw(t, plate.x + 40.0f, plate.y + 36.0f); }
  logoOrInitials(c, (GfxRect){ plate.x + 40.0f, plate.y + 108.0f, 150.0f, 84.0f }, a);
  x = plate.x + 232.0f;
  { TxtLine t = txt_line_trim(TXT_HEADLINE, c->name, 255, 255, 255, 255, 900.0f);
    txt_draw(t, x, plate.y + 30.0f);
    if (iptv_is_favourite(tuned)) {
      TxtLine st = txt_line(TXT_HEADLINE, "\xE2\x98\x85", 250, 199, 74, 255);
      txt_draw(st, x + t.w + 16.0f, plate.y + 30.0f);
    } }
  { char clock[16]; clockText(now, clock, sizeof clock);
    TxtLine t = txt_line(TXT_PAUSE_CLOCK, clock, 235, 235, 240, 255);
    txt_draw(t, plate.x + plate.w - 40.0f - t.w, plate.y + 32.0f); }

  if (playFailed) {
    TxtLine t = txt_line(TXT_DET_META, "This channel isn't available right now. Try another, or Reload the list.",
                         255, 140, 140, 255);
    txt_draw(t, x, plate.y + 100.0f);
  } else if (!video_ready() || zapPending) {
    TxtLine t = txt_line(TXT_DET_META, "Tuning\xE2\x80\xA6", 190, 190, 195, 255);
    txt_draw(t, x, plate.y + 100.0f);
  }
  { long long s0, e0;
    int p = cellAt(tuned, now, &s0, &e0), nx;
    float y = plate.y + ((playFailed || !video_ready() || zapPending) ? 140.0f : 100.0f);
    if (p >= 0) {
      const IptvProgramme *pg = &l->pg[p];
      TxtLine k = txt_line(TXT_SRC_TIER, "NOW", 239, 68, 68, 255);
      clockText(pg->start, a1, sizeof a1); clockText(pg->stop, b1, sizeof b1);
      txt_draw(k, x, y + 6.0f);
      snprintf(s, sizeof s, "%s", pg->title);
      { TxtLine t = txt_line_trim(TXT_SRC_TAB, s, 255, 255, 255, 255, 900.0f);
        txt_draw(t, x + 72.0f, y); }
      snprintf(s, sizeof s, "%s \xE2\x80\x93 %s", a1, b1);
      { TxtLine t = txt_line(TXT_SRC_META, s, 180, 180, 185, 255);
        txt_draw(t, plate.x + plate.w - 40.0f - t.w, y + 2.0f);
        progressBar(x + 72.0f + 920.0f, y + 14.0f, plate.x + plate.w - 64.0f - t.w - (x + 72.0f + 920.0f),
                    (float)(now - pg->start) / (float)(pg->stop - pg->start), a); }
      y += 44.0f;
      nx = (p + 1 < c->firstPg + c->nPg) ? p + 1 : -1;
    } else {
      TxtLine t = txt_line(TXT_SRC_META, iptv_guide_state() == IPTV_LOADING ? "Loading guide\xE2\x80\xA6"
                                                                             : "No programme information",
                           160, 160, 165, 255);
      txt_draw(t, x, y);
      y += 44.0f;
      nx = iptv_programme_after(l, tuned, now);
    }
    if (nx >= 0 && y < plate.y + plate.h - 30.0f) {
      const IptvProgramme *pg = &l->pg[nx];
      TxtLine k = txt_line(TXT_SRC_TIER, "NEXT", 150, 150, 155, 255);
      clockText(pg->start, a1, sizeof a1);
      txt_draw(k, x, y + 6.0f);
      snprintf(s, sizeof s, "%s  %s", a1, pg->title);
      { TxtLine t = txt_line_trim(TXT_SRC_TAB, s, 200, 200, 205, 255, 1200.0f);
        txt_draw(t, x + 72.0f, y); }
    }
  }
  gfx_opacity_group = 1.0f;
}

static void drawQuick(void) {
  const IptvList *l = iptv_list();
  long long now = nowSec();
  int rows = (int)((NV_SCREEN_H - 200.0f) / LIVE_QUICK_ROW);
  gfx_color((GfxRect){ 0.0f, 0.0f, LIVE_QUICK_W, NV_SCREEN_H }, 0.0f, 0.04f, 0.04f, 0.05f, 0.92f);
  { TxtLine t = txt_line(TXT_HEADLINE, groupLabel(group), 255, 255, 255, 255);
    txt_draw(t, 64.0f, 64.0f); }
  gfx_crop(0.0f, 140.0f, LIVE_QUICK_W, NV_SCREEN_H - 170.0f);
  pointer_clip(0.0f, 140.0f, LIVE_QUICK_W, NV_SCREEN_H - 170.0f);
  for (int r = (int)quickScroll; r < nView && r < (int)quickScroll + rows + 2; r++) {
    float y = 150.0f + (r - quickScroll) * LIVE_QUICK_ROW;
    int ch = view[r], f = r == quickRow;
    const IptvChannel *c = &l->ch[ch];
    GfxRect row = { 40.0f, y, LIVE_QUICK_W - 80.0f, LIVE_QUICK_ROW - 8.0f };
    char num[16];
    int p = iptv_programme_at(l, ch, now);
    if (f) gfx_color(row, LIVE_CELL_R / row.h, LIVE_FOCUS_RGB, 1.0f);
    if (ch == tuned)
      gfx_color((GfxRect){ row.x, y + 16.0f, 5.0f, row.h - 32.0f }, NV_RADIUS_PILL, LIVE_RED_RGB, 1.0f);
    snprintf(num, sizeof num, "%d", c->number);
    { int v = f ? 70 : 150; TxtLine t = txt_line(TXT_SRCH_META, num, v, v, v + 4, 255);
      txt_draw(t, row.x + 20.0f, y + (row.h - t.h) * 0.5f); }
    logoOrInitials(c, (GfxRect){ row.x + 84.0f, y + (row.h - 44.0f) * 0.5f, 78.0f, 44.0f }, 1.0f);
    { int v = f ? 18 : 240;
      TxtLine t = txt_line_trim(TXT_SRCH_NAME, c->name, v, v, f ? 20 : v, 255, row.w - 200.0f);
      txt_draw(t, row.x + 180.0f, y + (p >= 0 ? 10.0f : (row.h - t.h) * 0.5f));
      if (p >= 0) {
        int m = f ? 80 : 150;
        TxtLine d = txt_line_trim(TXT_SRCH_META, l->pg[p].title, m, m, m + 4, 255, row.w - 200.0f);
        txt_draw(d, row.x + 180.0f, y + 14.0f + t.h);
      } }
    pointer_zone(row.x, row.y, row.w, row.h, pointQuick, r, 0);
  }
  gfx_no_crop();
  pointer_no_clip();
}

static void drawFull(void) {
  const IptvChannel *c = chan(tuned);
  drawVideo((GfxRect){ 0.0f, 0.0f, NV_SCREEN_W, NV_SCREEN_H }, 0.0f);
  if (c && (!video_ready() || zapPending || playFailed) && bannerA < 0.5f) {
    logoOrInitials(c, (GfxRect){ NV_SCREEN_W * 0.5f - 120.0f, NV_SCREEN_H * 0.5f - 90.0f, 240.0f, 130.0f }, 1.0f);
  }
  drawBanner(bannerA);
  if (quickOpen) drawQuick();
  drawDigits();
  drawToast();
}

void iptvui_draw(Uint32 now) {
  (void)now;
  if (mode == MODE_SETUP) { drawSetup(); drawToast(); return; }
  if (iptvui_fullscreen()) { drawFull(); return; }
  drawHeader();
  drawInfo();
  drawGroups();
  drawGuide();
  drawDigits();
  drawToast();
}
