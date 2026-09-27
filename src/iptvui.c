// The Live TV screen, built to the Y1 (guide), Y2 (channel list) and Y3
// (mechanics) mockups. See iptvui.h for its faces; the data comes from iptv.h,
// and the only thing this file knows about the network is that the list can
// change under it between two frames (iptv_step).
//
// ------------------------------------------------------------------------
// THE SHAPE, AS OF 2026-09-27
//
//   header    "Live TV", the source line, [Guide|Channels] [Source]
//   chips     All channels, Favourites, Recent, then the playlist's groups —
//             the same two-axis pattern as the Library's strip: categories live
//             ABOVE the body, never beside it, so LEFT only ever means "earlier"
//             or "the side bar" and never "the third column".
//   body      LIST (the landing): what is on every channel right now, with the
//             tuned channel's live preview and the focused channel's detail on
//             the right. GUIDE: channels down, two hours across.
//
// WATCHING IS NOT POINTING. The preview always plays the TUNED channel; moving
// the focus changes text, never the stream. Only OK tunes. The equaliser marks
// the tuned channel wherever it is listed, the white plate marks the focus.
//
// THE GUIDE'S FOCUS is a (row, TIME) pair, not (row, column): programmes do not
// line up between channels, so DOWN from a film starting at 21:00 lands on what
// the next channel shows at 21:00. `fTime` is that instant, never before now.
// LEFT from what is on now lands on the channel's own cell (`fChan`); LEFT
// again is the side bar. The window pages in 30-minute jumps and never scrolls
// sideways (Y3: smooth horizontal scrolling of rows x blocks is what drops
// frames on a C3).
//
// THE NOW LINE is the seek bar's colour (Settings -> Seek bar colour, violet
// by default): in the player it says where we are in the programme, here where
// we are in the schedule. It and every "min in" are snapped to the minute.
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
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

// --- Layout (1920x1080 design space), read off the mockups --------------------
#define L_RIGHT          (NV_SCREEN_W - 96.0f)
#define L_BTN_Y           58.0f
#define L_BTN_H           50.0f
#define L_CHIPS_Y        142.0f
#define L_CHIP_H          48.0f
#define L_CHIP_GAP        12.0f
#define L_CHIP_PAD        24.0f
#define L_CHIP_MAX_W     360.0f

// Y1, the guide.
#define G_DETAIL_Y       226.0f
#define G_PREV_W         352.0f
#define G_PREV_H         198.0f
#define G_PREV_Y         210.0f
#define G_PREV_R          12.0f
#define G_RULER_Y        452.0f
#define G_RULER_H         34.0f
#define G_GRID_Y         500.0f
#define G_BOTTOM        1040.0f
#define G_ROW_H           82.0f
#define G_ROW_STEP        92.0f
#define G_CH_W           304.0f
#define G_COL_GAP         10.0f
#define G_BLOCK_GAP        6.0f
#define G_BLOCK_R          8.0f
#define G_CH_R            10.0f
#define G_TILE            54.0f
// Y3's block tiers: title and both times, title and the start, or nothing.
#define G_TIER_FULL      200.0f
#define G_TIER_TEXT       96.0f

// Y2, the channel list.
#define C_LIST_Y         232.0f
#define C_ROW_H           96.0f
#define C_ROW_STEP       104.0f
#define C_ROW_R           12.0f
#define C_TILE            58.0f
#define C_DIM             0.52f
#define C_PANEL_W        784.0f
#define C_PREV_H         441.0f
#define C_PREV_R          14.0f

#define LIVE_SPAN_S       7200     // two hours across the guide
#define LIVE_SLOT_S       1800     // paged in half hours
#define LIVE_AHEAD_S      (28 * 3600)

// The full-screen view.
#define LIVE_BANNER_MS    5000u
#define LIVE_ZAP_MS        350u    // CH+/CH- held down tunes only where it stops
#define LIVE_DIGITS_MS    1600u
#define LIVE_TOAST_MS     2600u
#define LIVE_QUICK_W      720.0f
#define LIVE_QUICK_ROW    84.0f

// Colours, as hex from the mockups. HEXF for gfx (floats), HEXI for text (ints).
#define HEXF(h) ((h) >> 16 & 255) / 255.0f, ((h) >> 8 & 255) / 255.0f, ((h) & 255) / 255.0f
#define HEXI(h) (int)((h) >> 16 & 255), (int)((h) >> 8 & 255), (int)((h) & 255)
#define C_PAPER   0xF0F2F4   // the focused surface
#define C_INK     0x0A0C0E   // text on it
#define C_PLATE   0x1A1D21   // a channel's identity tile
#define C_PLATE_F 0x23262B
#define C_LIVE    0xE8604C   // the LIVE tag's dot

enum { MODE_BROWSE, MODE_SETUP };
enum { VIEW_LIST, VIEW_GUIDE };
enum { ZONE_HEAD, ZONE_CHIPS, ZONE_BODY, ZONE_ACTIONS };
enum { HEAD_TOGGLE, HEAD_SOURCE, HEAD_N };
enum { ACT_WATCH, ACT_FAV, ACT_N };
enum { GROUP_ALL, GROUP_FAV, GROUP_RECENT, GROUP_FIRST };

static int mode, viewMode = VIEW_LIST, zone = ZONE_BODY, headSel, actSel;
static int wantsExit, requestMenu;

// The chips and the channels they select.
static int group = GROUP_ALL;
static int *view;
static int nView, capView;
static float *chipW;            // measured once per list, not per frame
static int nChipW;
static float chipScroll, chipScrollV;

// The focus: see the note at the top.
static int fRow, fChan;
static long long fTime, winStart;
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
static float bannerA;
static int quickOpen, quickRow;
static float quickScroll, quickScrollV;
static char digits[6];
static Uint32 digitsAt;
static int lastWin[4] = { -1, -1, -1, -1 };

static char toast[160];
static Uint32 toastUntil;

// The dashed "no guide data" outline: one texture for the row's size, drawn in
// one call, rather than a hundred dash rects per row.
static GLuint dashTex;
static int dashW, dashH;

// --- Setup ---------------------------------------------------------------------
enum { ROW_TYPE, ROW_URL, ROW_SERVER, ROW_USER, ROW_PASS, ROW_EPG, ROW_SAVE };
enum { BTN_SAVE, BTN_RELOAD, BTN_CANCEL };
static IptvSource draft;
static int setupRow, setupBtn;
static int setupRows[8], nSetupRows;
static int editing = -1;    // the ROW_* whose text the keyboard is filling

// --- Small helpers ---------------------------------------------------------------
static float X0(void) { return settings_content_x() - 8.0f; }
static long long nowSec(void) { return (long long)time(NULL); }
// Every "now" the screen DRAWS is the minute: the line moves once a minute
// rather than creeping a pixel at a time (Y3).
static long long nowMinute(void) { long long t = nowSec(); return t - t % 60; }
static long long slotFloor(long long t) { return t - (((t % LIVE_SLOT_S) + LIVE_SLOT_S) % LIVE_SLOT_S); }

static void clockText(long long t, char *dst, size_t n) {
  time_t tt = (time_t)t;
  struct tm tmv;
  localtime_r(&tt, &tmv);
  strftime(dst, n, "%H:%M", &tmv);
}

static void accent(float *r, float *g, float *b) { settings_seek_color(r, g, b); }

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

// The programme on now, and the one after it, for channel `ch` at minute `t`.
static const IptvProgramme *programmeAt(int ch, long long t) {
  const IptvList *l = iptv_list();
  int p = iptv_programme_at(l, ch, t);
  return p >= 0 ? &l->pg[p] : NULL;
}
static const IptvProgramme *programmeAfter(int ch, long long t, int skip) {
  const IptvList *l = iptv_list();
  const IptvChannel *c = chan(ch);
  int p = iptv_programme_after(l, ch, t);
  if (p < 0 || !c) return NULL;
  p += skip;
  return p < c->firstPg + c->nPg ? &l->pg[p] : NULL;
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

// The chips' widths, measured when the groups change and not per frame: a
// provider can carry hundreds of groups, and only the handful on screen are drawn.
static void measureChips(void) {
  int n = nGroups();
  float *w = realloc(chipW, (size_t)n * sizeof *w);
  if (!w) { nChipW = 0; return; }
  chipW = w; nChipW = n;
  for (int g = 0; g < n; g++) {
    float t = txt_width(g == group ? TXT_LIVE_META_B : TXT_LIVE_META, groupLabel(g));
    // Bold is wider than medium: size every chip for the bold, so choosing one
    // does not shove its neighbours along.
    float b = txt_width(TXT_LIVE_META_B, groupLabel(g));
    if (b > t) t = b;
    if (t > L_CHIP_MAX_W - 2 * L_CHIP_PAD) t = L_CHIP_MAX_W - 2 * L_CHIP_PAD;
    chipW[g] = t + 2 * L_CHIP_PAD;
  }
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
  fChan = 0;
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

// PAGED, not scrolled: the window only ever moves in whole half hours.
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
  if (fChan) { fChan = 0; fTime = now; keepWindowOnFocus(); return; }
  cellAt(ch, fTime, &s, &e);
  if (!c || !c->nPg || e >= LIVE_FOREVER || e >= now + LIVE_AHEAD_S) return;
  fTime = e;
  keepWindowOnFocus();
}

static void guideLeft(void) {
  long long s, e, now = nowSec();
  int ch = focusedChannel();
  if (fChan) { requestMenu = 1; return; }
  cellAt(ch, fTime, &s, &e);
  // What is on now is the first column; left of it is the channel itself.
  if (s <= now) { fChan = 1; return; }
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

// Tunes `ch`. `immediate` starts the stream at once; otherwise it waits
// LIVE_ZAP_MS for the keys to stop, so a run of CH+ presses opens one stream.
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
      rememberFocus();
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

// OK on a channel: watch it full screen. OK on the one already playing just
// takes it full screen; it is never retuned.
static void watch(int ch) {
  if (ch < 0) return;
  if (!(ch == tuned && (ownsVideo() || zapPending))) tune(ch, 1);
  enterFull();
}

static void toggleFavourite(int ch) {
  if (ch < 0) return;
  iptv_toggle_favourite(ch);
  say(iptv_is_favourite(ch) ? "Added to Favourites" : "Removed from Favourites");
  if (group == GROUP_FAV) rebuildView();
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
  setupRow = 0; setupBtn = BTN_SAVE; editing = -1;
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

// The type chips, then the fields at 118 apiece, then the buttons. Xtream's
// four fields are the tallest form, and it ends above 970.
static float setupRowY(int i) {
  float y = 300.0f;
  for (int k = 0; k < i; k++) y += setupRows[k] == ROW_TYPE ? 100.0f : 118.0f;
  return y;
}
static GfxRect setupField(int i) {
  return (GfxRect){ X0(), setupRowY(i) + 34.0f, 1040.0f, 70.0f };
}
static int setupButtons(void) { return iptv_configured() ? 3 : 1; }

static int draftComplete(void) {
  if (draft.kind == IPTV_SRC_XTREAM) return draft.server[0] && draft.user[0] && draft.pass[0];
  return strstr(draft.url, "://") != NULL;
}

static void backToBrowse(void) {
  mode = MODE_BROWSE;
  viewMode = VIEW_LIST;
  zone = ZONE_BODY;
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
  backToBrowse();
  group = GROUP_ALL;
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
    if (iptv_configured()) { backToBrowse(); return; }
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
      else if (row == ROW_SAVE && setupBtn + 1 < setupButtons()) setupBtn++;
      break;
    default:
      if (!isOk(k)) break;
      if (row == ROW_TYPE) {
        draft.kind = draft.kind == IPTV_SRC_XTREAM ? IPTV_SRC_M3U : IPTV_SRC_XTREAM;
        layoutSetup();
      } else if (row == ROW_SAVE) {
        if (setupBtn == BTN_CANCEL) backToBrowse();
        else if (setupBtn == BTN_RELOAD) {
          iptv_reload();
          backToBrowse();
          say("Reloading channels and guide\xE2\x80\xA6");
        } else saveSetup();
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
static void pointChip(int g, int unused) { (void)unused; zone = ZONE_CHIPS; chooseGroup(g); }
static void pointRow(int row, int unused) {
  (void)unused;
  zone = ZONE_BODY;
  if (row != fRow) { fRow = row; rememberFocus(); }
}
static void pointChan(int row, int unused) { pointRow(row, unused); fChan = 1; }
static void pointCell(int row, int t) {
  pointRow(row, 0);
  fChan = 0;
  // `t` is minutes from winStart: the pointer's cell, not the focus's instant.
  { long long at = winStart + (long long)t * 60, now = nowSec();
    fTime = at < now ? now : at; }
}
static void pointAct(int a, int unused) { (void)unused; zone = ZONE_ACTIONS; actSel = a; }
static void pointSetup(int i, int unused) { (void)unused; if (!ime_is_open()) setupRow = i; }
static void pointQuick(int r, int unused) { (void)unused; quickRow = r; }

// --- Lifecycle ---------------------------------------------------------------------------
int iptvui_start(void) {
  static int started;
  if (!started) { iptv_start(); started = 1; }
  wantsExit = requestMenu = 0;
  full = 0; quickOpen = 0;
  viewMode = VIEW_LIST;
  zone = ZONE_BODY;
  fChan = 0;
  fTime = nowSec();
  winStart = slotFloor(fTime);
  if (!iptv_configured()) openSetup();
  else { mode = MODE_BROWSE; iptv_touch(); rebuildView(); measureChips(); }
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
  free(chipW); chipW = NULL; nChipW = 0;
  if (dashTex) { gfx_tex_forget(dashTex); glDeleteTextures(1, &dashTex); dashTex = 0; }
  iptv_shutdown();
}

int iptvui_wants_exit(void) { int v = wantsExit; wantsExit = 0; return v; }
int iptvui_requested_menu(void) { int v = requestMenu; requestMenu = 0; return v; }
int iptvui_fullscreen(void) { return full && tuned >= 0; }

// --- Events ---------------------------------------------------------------------------
static void typeDigit(int d) {
  size_t n = strlen(digits);
  if (n + 1 < sizeof digits) { digits[n] = (char)('0' + d); digits[n + 1] = 0; }
  digitsAt = SDL_GetTicks();
}

static void fullEvent(SDL_Keycode k) {
  int d = digitOf(k);
  Uint32 now = SDL_GetTicks();
  if (d >= 0 && !quickOpen) { typeDigit(d); return; }
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
        // Back to the screen; the picture carries on in the preview.
        full = 0;
        zone = ZONE_BODY;
        fChan = 0;
        for (int r = 0; r < nView; r++) if (view[r] == tuned) { fRow = r; break; }
        rememberFocus();
        fTime = nowSec();
        keepWindowOnFocus();
      }
      break;
  }
}

static void headEvent(SDL_Keycode k) {
  if (k == SDLK_LEFT) { if (headSel > 0) headSel--; else requestMenu = 1; }
  else if (k == SDLK_RIGHT) { if (headSel + 1 < HEAD_N) headSel++; }
  else if (k == SDLK_DOWN) zone = ZONE_CHIPS;
  else if (isOk(k)) {
    if (headSel == HEAD_SOURCE) openSetup();
    else {
      // The same header and chips either way: switching changes only the body.
      viewMode = viewMode == VIEW_LIST ? VIEW_GUIDE : VIEW_LIST;
      fChan = 0;
      fTime = nowSec();
      winStart = slotFloor(fTime);
    }
  }
}

static void chipsEvent(SDL_Keycode k) {
  if (k == SDLK_LEFT) { if (group > 0) chooseGroup(group - 1); else requestMenu = 1; }
  else if (k == SDLK_RIGHT) { if (group + 1 < nGroups()) chooseGroup(group + 1); }
  else if (k == SDLK_UP) zone = ZONE_HEAD;
  else if ((k == SDLK_DOWN || isOk(k)) && nView) {
    zone = ZONE_BODY;
    fTime = nowSec();
    keepWindowOnFocus();
  }
}

static void actionsEvent(SDL_Keycode k) {
  if (k == SDLK_LEFT) { if (actSel > 0) actSel--; else zone = ZONE_BODY; }
  else if (k == SDLK_RIGHT) { if (actSel + 1 < ACT_N) actSel++; }
  else if (k == SDLK_UP) zone = ZONE_CHIPS;
  else if (isOk(k)) {
    if (actSel == ACT_WATCH) watch(focusedChannel());
    else toggleFavourite(focusedChannel());
  }
}

static void pageRows(int step) {
  fRow += step;
  if (fRow < 0) fRow = 0;
  if (fRow >= nView) fRow = nView ? nView - 1 : 0;
  rememberFocus();
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
  if (hold_event(&hold, e, zone == ZONE_BODY && nView > 0, &tap)) {
    if (tap) watch(focusedChannel());
    return;
  }
  if (e->type != SDL_KEYDOWN) return;
  k = e->key.keysym.sym;

  if (isBack(k)) {
    // Back climbs, one step at a time: the actions to their row; the guide to
    // the list, which is the landing; a playing preview stops; then the chips;
    // then out.
    if (zone == ZONE_ACTIONS) { zone = ZONE_BODY; return; }
    if (zone == ZONE_HEAD) { zone = ZONE_CHIPS; return; }
    if (zone == ZONE_BODY && viewMode == VIEW_GUIDE) { viewMode = VIEW_LIST; fChan = 0; return; }
    if (zone == ZONE_BODY && ownsVideo()) { stopStream(); tuned = -1; tunedName[0] = 0; return; }
    if (zone == ZONE_BODY) { zone = ZONE_CHIPS; return; }
    wantsExit = 1;
    return;
  }
  if (zone == ZONE_HEAD) { headEvent(k); return; }
  if (zone == ZONE_CHIPS) { chipsEvent(k); return; }
  if (zone == ZONE_ACTIONS) { actionsEvent(k); return; }

  { int d = digitOf(k); if (d >= 0) { typeDigit(d); return; } }
  switch (k) {
    case SDLK_UP:
      if (fRow > 0) { fRow--; rememberFocus(); }
      else zone = ZONE_CHIPS;
      break;
    case SDLK_DOWN: if (fRow + 1 < nView) { fRow++; rememberFocus(); } break;
    case SDLK_PAGEUP:   pageRows(-6); break;
    case SDLK_PAGEDOWN: pageRows(+6); break;
    case SDLK_LEFT:
      if (viewMode == VIEW_GUIDE) guideLeft(); else requestMenu = 1;
      break;
    case SDLK_RIGHT:
      if (viewMode == VIEW_GUIDE) guideRight(); else { zone = ZONE_ACTIONS; actSel = ACT_WATCH; }
      break;
    default: break;
  }
}

// --- Update -----------------------------------------------------------------------------
static float followRow(float current, int row, int visible, int n) {
  float target = current;
  // The focused row stays a row away from either edge while there are rows beyond.
  if (row < target + 1) target = (float)row - 1;
  if (row > target + visible - 2) target = (float)(row - visible + 2);
  if (target > (float)(n - visible)) target = (float)(n - visible);
  if (target < 0) target = 0;
  return (float)(int)target;
}

void iptvui_update(float dt, Uint32 now) {
  long long t = nowSec();
  int reduced = settings_animations_reduced();
  if (iptv_step()) {
    // Same playlist text: same indices. A different one: find them by name.
    int c = findByName(tunedName);
    if (c < 0 && tuned >= 0) { stopStream(); tuned = -1; full = 0; }
    else tuned = c;
    rebuildView();
    measureChips();
  }
  if (mode != MODE_BROWSE) return;

  hold_animate(&hold, dt, now);
  if (hold_fired(&hold, now) && zone == ZONE_BODY) toggleFavourite(focusedChannel());

  // Time moves on under a screen left open.
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

  { int visible = viewMode == VIEW_GUIDE ? (int)((G_BOTTOM - G_GRID_Y) / G_ROW_STEP)
                                         : (int)((NV_SCREEN_H - 60.0f - C_LIST_Y) / C_ROW_STEP);
    scrollRows = anim_spring2_reduced(&scrollRowsV, scrollRows,
                                      followRow(scrollRows, fRow, visible, nView), dt,
                                      NV_SPRING2_PAGE, reduced); }

  // The chips slide to keep the chosen one on screen.
  { float x = 0.0f, target = chipScroll, room = L_RIGHT - X0();
    for (int g = 0; g < group && g < nChipW; g++) x += chipW[g] + L_CHIP_GAP;
    if (group < nChipW) {
      if (x - target < 0.0f) target = x - (group ? 96.0f : 0.0f);
      if (x + chipW[group] - target > room) target = x + chipW[group] - room + 96.0f;
      if (target < 0.0f) target = 0.0f;
    }
    chipScroll = anim_spring2_reduced(&chipScrollV, chipScroll, target, dt, NV_SPRING2_PAGE, reduced); }

  { float q = quickScroll;
    int rows = (int)((NV_SCREEN_H - 200.0f) / LIVE_QUICK_ROW);
    if (quickRow < q + 1) q = (float)quickRow - 1;
    if (quickRow > q + rows - 2) q = (float)(quickRow - rows + 2);
    if (q > (float)(nView - rows)) q = (float)(nView - rows);
    if (q < 0) q = 0;
    quickScroll = anim_spring2_reduced(&quickScrollV, quickScroll, (float)(int)q, dt,
                                       NV_SPRING2_PAGE, reduced); }
}

// --- Drawing: shared parts ----------------------------------------------------------------
// A line of text in a hex colour; returns its width.
static float ink(TxtStyle s, const char *str, unsigned hex, float x, float y, float a) {
  TxtLine l = txt_line(s, str, HEXI(hex), 255);
  txt_draw_alpha(l, x, y, a);
  return (float)l.w;
}
static float inkTrim(TxtStyle s, const char *str, unsigned hex, float x, float y, float maxW, float a) {
  TxtLine l = txt_line_trim(s, str, HEXI(hex), 255, maxW);
  txt_draw_alpha(l, x, y, a);
  return (float)l.w;
}
// Text vertically centred on `cy`.
static float inkMid(TxtStyle s, const char *str, unsigned hex, float x, float cy, float maxW, float a) {
  TxtLine l = txt_line_trim(s, str, HEXI(hex), 255, maxW);
  txt_draw_alpha(l, x, cy - l.h * 0.5f, a);
  return (float)l.w;
}

// The LIVE / ON NOW / cap tags: tracked-out caps, the tracking the mockups give
// as 0.12em.
static float tagText(const char *s, unsigned hex, float x, float cy, float a) {
  TxtLine probe = txt_line(TXT_LIVE_TAG, "0", 255, 255, 255, 255);
  return txt_tracking(TXT_LIVE_TAG, s, HEXI(hex), x, cy - probe.h * 0.5f, a, 1.6f);
}
// Measured the way tagText draws it, glyph by glyph: a kerned width is a few
// pixels short of a tracked one, which clipped the now cap's last digit.
static float tagWidth(const char *s) {
  return txt_tracking(TXT_LIVE_TAG, s, 255, 255, 255, -1.0f, 0.0f, 0.0f, 1.6f);
}

// ON NOW, on the accent.
static float onNow(float x, float y, float a) {
  float r, g, b, w = tagWidth("ON NOW") + 24.0f;
  accent(&r, &g, &b);
  gfx_color((GfxRect){ x, y, w, 32.0f }, 8.0f / 32.0f, r, g, b, a);
  tagText("ON NOW", 0xFFFFFF, x + 12.0f, y + 16.0f, a);
  return w;
}

// LIVE, with its dot, on a dark glass tag.
static void liveTag(float x, float y, float h, float a) {
  float w = tagWidth("LIVE") + 11.0f + 8.0f + 8.0f + 11.0f;
  gfx_color((GfxRect){ x, y, w, h }, 8.0f / h, 10 / 255.0f, 12 / 255.0f, 14 / 255.0f, 0.72f * a);
  gfx_color((GfxRect){ x + 11.0f, y + h * 0.5f - 4.0f, 8.0f, 8.0f }, 0.5f, HEXF(C_LIVE), a);
  tagText("LIVE", 0xF5F6F8, x + 27.0f, y + h * 0.5f, a);
}

// The equaliser from the Sources panel, in the accent: the tuned channel,
// wherever it is listed. Bottom-aligned on `base`.
static void equaliser(float x, float base, float h, float a) {
  static const float SPEED[3] = { 0.0091f, 0.0067f, 0.0113f };
  static const float PHASE[3] = { 0.0f, 2.1f, 4.2f };
  static const float MIN[3] = { 0.30f, 0.24f, 0.52f };
  float r, g, b;
  Uint32 now = SDL_GetTicks();
  accent(&r, &g, &b);
  for (int i = 0; i < 3; i++) {
    float s = 0.5f + 0.5f * sinf((float)now * SPEED[i] + PHASE[i]);
    float bh = h * (MIN[i] + (1.0f - MIN[i]) * s);
    gfx_color((GfxRect){ x + i * 7.0f, base - bh, 4.0f, bh }, 2.0f / bh, r, g, b, a);
  }
}
#define EQ_W 18.0f

// A thin progress bar: the track, and the accent up to `frac`.
static void progressBar(float x, float y, float w, float h, float frac, float track, float a) {
  float r, g, b;
  if (frac < 0.0f) frac = 0.0f;
  if (frac > 1.0f) frac = 1.0f;
  accent(&r, &g, &b);
  gfx_color((GfxRect){ x, y, w, h }, 0.5f, 1.0f, 1.0f, 1.0f, track * a);
  if (frac * w >= 1.0f) gfx_color((GfxRect){ x, y, w * frac, h }, 0.5f, r, g, b, a);
}

// A vertical fade into the background, in bands: `top` is transparent,
// `top + h` is the page. For the bottom edge the mockups dissolve lists into.
static void fadeBottom(float top, float h) {
  const int N = 13;
  for (int i = 0; i < N; i++) {
    float t = ((float)i + 0.5f) / (float)N;
    float a = t < 0.56f ? t / 0.56f * 0.9f : 0.9f + (t - 0.56f) / 0.44f * 0.1f;
    gfx_color((GfxRect){ 0.0f, top + h * i / N, NV_SCREEN_W, h / N + 0.5f }, 0.0f,
              NV_COLOR_BACKGROUND_R, NV_COLOR_BACKGROUND_G, NV_COLOR_BACKGROUND_B, a);
  }
}

// --- Channel identity, Y3's three tiers -----------------------------------------------------
// Two or three letters from the name: the first letter of its first two words,
// and a number word's digits ("News 24" -> N24). Provider noise — "|UK|",
// "[FHD]", "UK:", HD/FHD/4K tags — is not a word.
static void monogram(const char *name, char *out, size_t n) {
  size_t k = 0;
  int words = 0;
  const char *p = name;
  out[0] = 0;
  while (*p && k + 1 < n && k < 3) {
    const char *w;
    size_t len;
    while (*p && (isspace((unsigned char)*p) || *p == '-' || *p == '|')) p++;
    w = p;
    while (*p && !isspace((unsigned char)*p) && *p != '|') p++;
    len = (size_t)(p - w);
    if (!len) break;
    if (w[0] == '[' || w[0] == '(' || w[len - 1] == ':') continue;
    if ((len == 2 && !strncasecmp(w, "HD", 2)) || (len == 3 && (!strncasecmp(w, "FHD", 3) ||
        !strncasecmp(w, "UHD", 3))) || (len == 2 && !strncasecmp(w, "4K", 2)) ||
        (len == 2 && !strncasecmp(w, "SD", 2)))
      continue;
    if (isdigit((unsigned char)w[0])) {
      for (size_t i = 0; i < len && k < 3 && isdigit((unsigned char)w[i]); i++) out[k++] = w[i];
    } else if (isalpha((unsigned char)w[0]) && words < 2) {
      out[k++] = (char)toupper((unsigned char)w[0]);
      words++;
    }
  }
  out[k] = 0;
}

// The tile is ALWAYS drawn and ALWAYS filled: a logo on the plate with 9px of
// air, else the monogram, else the bare number. A logo that failed looks exactly
// like a channel that never had one — never a broken-image glyph — and the tex
// cache remembers the failure, so a dead URL is asked for once.
static void identity(const IptvChannel *c, GfxRect r, float radius, unsigned plate, unsigned inkHex,
                     float a) {
  gfx_color(r, radius / r.h, HEXF(plate), a);
  if (c->logo[0] && !tex_failed(c->logo)) {
    GLuint tex = tex_get_width(c->logo, r.w - 18.0f);
    if (tex) {
      GfxRect box = { r.x + 9.0f, r.y + 9.0f, r.w - 18.0f, r.h - 18.0f };
      float asp = tex_aspect(c->logo), w, h;
      if (asp <= 0.0f) asp = 1.0f;
      if (asp > box.w / box.h) { w = box.w; h = box.w / asp; } else { h = box.h; w = box.h * asp; }
      gfx_opacity_group = a;
      gfx_texture((GfxRect){ box.x + (box.w - w) * 0.5f, box.y + (box.h - h) * 0.5f, w, h }, tex);
      gfx_opacity_group = 1.0f;
      return;
    }
  }
  { char m[8];
    monogram(c->name, m, sizeof m);
    if (!m[0]) snprintf(m, sizeof m, "%d", c->number);
    { TxtLine t = txt_line(TXT_SRC_TIER, m, HEXI(inkHex), 255);
      txt_draw_alpha(t, r.x + (r.w - t.w) * 0.5f, r.y + (r.h - t.h) * 0.5f, a); } }
}

// A header pill: an icon and a word.
static GfxRect headButton(float right, const char *icon, const char *label, int focused, int idx) {
  TxtLine t = txt_line(TXT_SRC_TEXT, label, HEXI(focused ? C_INK : 0xC1C7CD), 255);
  float w = 22.0f + 18.0f + 11.0f + t.w + 22.0f;
  GfxRect r = { right - w, L_BTN_Y, w, L_BTN_H };
  if (focused) gfx_color(r, 0.5f, HEXF(C_PAPER), 1.0f);
  else gfx_color(r, 0.5f, 1.0f, 1.0f, 1.0f, 0.07f);
  gfx_icon((GfxRect){ r.x + 22.0f, r.y + (r.h - 18.0f) * 0.5f, 18.0f, 18.0f }, icon,
           focused ? 10 / 255.0f : 0xC1 / 255.0f, focused ? 12 / 255.0f : 0xC7 / 255.0f,
           focused ? 14 / 255.0f : 0xCD / 255.0f, 1.0f);
  txt_draw(t, r.x + 22.0f + 18.0f + 11.0f, r.y + (r.h - t.h) * 0.5f);
  pointer_zone(r.x, r.y, r.w, r.h, pointHead, idx, 0);
  return r;
}

static void drawHeader(void) {
  float x = X0(), right = L_RIGHT;
  const IptvList *l = iptv_list();
  char line[256];
  { TxtLine t = txt_line(TXT_TITLE3, "Live TV", 245, 246, 248, 255);
    txt_draw(t, x, L_BTN_Y + L_BTN_H * 0.5f - t.h * 0.5f); }
  { GfxRect r = headButton(right, "live_source", "Source", zone == ZONE_HEAD && headSel == HEAD_SOURCE, HEAD_SOURCE);
    right = r.x - 24.0f; }
  { GfxRect r = headButton(right, viewMode == VIEW_LIST ? "live_guide" : "live_list",
                           viewMode == VIEW_LIST ? "Guide" : "Channels",
                           zone == ZONE_HEAD && headSel == HEAD_TOGGLE, HEAD_TOGGLE);
    right = r.x - 28.0f; }
  { const char *st = iptv_status();
    if (l) snprintf(line, sizeof line, "%s%s%s\xE2\x80\xAF\xC2\xB7\xE2\x80\xAF%d channels",
                    st[0] ? st : "", st[0] ? "  \xC2\xB7  " : "", iptv_source_label(), l->nCh);
    else snprintf(line, sizeof line, "%s%s%s", iptv_source_label(), st[0] ? "  \xC2\xB7  " : "", st);
    { TxtLine t = txt_line_trim(TXT_LIVE_META, line, HEXI(0x7C838B), 255, right - x - 360.0f);
      txt_draw(t, right - t.w, L_BTN_Y + (L_BTN_H - t.h) * 0.5f); } }
}

static void drawChips(void) {
  float x = X0() - chipScroll, y = L_CHIPS_Y;
  int n = nGroups();
  if (nChipW != n) measureChips();
  gfx_crop(X0() - 8.0f, y - 8.0f, L_RIGHT - X0() + 16.0f, L_CHIP_H + 16.0f);
  pointer_clip(X0() - 8.0f, y - 8.0f, L_RIGHT - X0() + 16.0f, L_CHIP_H + 16.0f);
  for (int g = 0; g < n && g < nChipW; g++) {
    float w = chipW[g];
    GfxRect r = { x, y, w, L_CHIP_H };
    int on = g == group;
    if (x > L_RIGHT + 8.0f) break;
    if (x + w >= X0() - 8.0f) {
      if (on && zone == ZONE_CHIPS)
        gfx_color((GfxRect){ r.x - 3.0f, r.y - 3.0f, r.w + 6.0f, r.h + 6.0f }, 0.5f, 1, 1, 1, 0.30f);
      if (on) gfx_color(r, 0.5f, HEXF(C_PAPER), 1.0f);
      else gfx_color(r, 0.5f, 1.0f, 1.0f, 1.0f, 0.06f);
      inkMid(on ? TXT_LIVE_META_B : TXT_LIVE_META, groupLabel(g), on ? C_INK : 0x8A9199,
             r.x + L_CHIP_PAD, r.y + r.h * 0.5f, w - 2 * L_CHIP_PAD + 1.0f, 1.0f);
      pointer_zone(r.x, r.y, r.w, r.h, pointChip, g, 0);
    }
    x += w + L_CHIP_GAP;
  }
  gfx_no_crop();
  pointer_no_clip();
}

// The row pager on the right edge, Library's segmented scrollbar: one segment
// a page, the current one lit. Past what segments can show, a thumb on a track.
static void pager(float x, float top, float h, int rows, int perPage, int row) {
  int pages;
  if (perPage < 1 || rows <= perPage) return;
  pages = (rows + perPage - 1) / perPage;
  { float seg = (h - (pages - 1) * 6.0f) / pages;
    int cur = row / perPage;
    if (seg > 60.0f) seg = 60.0f;
    if (seg >= 10.0f) {
      for (int p = 0; p < pages; p++) {
        float y = top + p * (seg + 6.0f);
        if (p == cur) {
          GfxRect r = { x, y, 10.0f, seg };
          gfx_glow(r, 5.0f, 20.0f, 1, 1, 1, 0.28f);
          gfx_color(r, 0.5f * 10.0f / seg, HEXF(0xF5F6F8), 1.0f);
        } else {
          gfx_color((GfxRect){ x + 2.0f, y, 6.0f, seg }, 3.0f / seg, 1, 1, 1, 0.12f);
        }
      }
      return;
    } }
  { float th = h * perPage / rows, y;
    if (th < 40.0f) th = 40.0f;
    y = top + (h - th) * row / (rows > 1 ? rows - 1 : 1);
    gfx_color((GfxRect){ x + 2.0f, top, 6.0f, h }, 3.0f / h, 1, 1, 1, 0.12f);
    gfx_glow((GfxRect){ x, y, 10.0f, th }, 5.0f, 20.0f, 1, 1, 1, 0.28f);
    gfx_color((GfxRect){ x, y, 10.0f, th }, 5.0f / th, HEXF(0xF5F6F8), 1.0f); }
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
  // The picture's top right: the screen's full screen, the header's otherwise.
  r = (GfxRect){ L_RIGHT - t.w - 64.0f, iptvui_fullscreen() ? 56.0f : L_CHIPS_Y - 4.0f, t.w + 64.0f,
                 t.h + 24.0f };
  gfx_color(r, NV_RADIUS_BADGE, 0.06f, 0.06f, 0.07f, 0.92f);
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

// A darkening from the bottom of a picture up, for the text laid over it.
static void pictureShade(GfxRect r, float h, float a) {
  const int N = 8;
  for (int i = 0; i < N; i++) {
    float t = ((float)i + 0.5f) / (float)N;
    gfx_color((GfxRect){ r.x, r.y + r.h - h + h * i / N, r.w, h / N + 0.5f }, 0.0f,
              5 / 255.0f, 6 / 255.0f, 9 / 255.0f, a * t);
  }
}

// What stands in for a picture when nothing is tuned: the plate, the focused
// channel's tile large, and how to start one.
static void pictureIdle(GfxRect r, float radius, int ch, float tile) {
  const IptvChannel *c = chan(ch);
  gfx_color(r, radius / r.h, HEXF(0x14171B), 1.0f);
  if (c) identity(c, (GfxRect){ r.x + (r.w - tile) * 0.5f, r.y + r.h * 0.42f - tile * 0.5f, tile, tile },
                  tile * 0.2f, C_PLATE, 0xC1C7CD, 1.0f);
  { TxtLine t = txt_line(TXT_LIVE_NOTE, playFailed ? "This channel isn't available right now"
                                                   : "Press OK to watch", HEXI(0x8A9199), 255);
    txt_draw(t, r.x + (r.w - t.w) * 0.5f, r.y + r.h * 0.42f + tile * 0.5f + 18.0f); }
}

// The line of time under a programme: "20:15 – 21:00 · 9 min in" and friends.
static void whenText(const IptvProgramme *pg, long long now, int longForm, char *dst, size_t n) {
  char a[16], b[16];
  clockText(pg->start, a, sizeof a);
  clockText(pg->stop, b, sizeof b);
  if (pg->start <= now && now < pg->stop) {
    long long in = (now - pg->start) / 60, left = (pg->stop - now + 59) / 60;
    if (longForm) snprintf(dst, n, "%s \xE2\x80\x93 %s \xC2\xB7 %lld min", a, b, (pg->stop - pg->start) / 60);
    else snprintf(dst, n, "%s \xE2\x80\x93 %s \xC2\xB7 %lld min in", a, b, in);
    (void)left;
  } else if (pg->start > now) {
    long long until = (pg->start - now + 59) / 60;
    if (until < 90) snprintf(dst, n, "%s \xE2\x80\x93 %s \xC2\xB7 in %lld min", a, b, until);
    else snprintf(dst, n, "%s \xE2\x80\x93 %s \xC2\xB7 %lld min", a, b, (pg->stop - pg->start) / 60);
  } else {
    snprintf(dst, n, "%s \xE2\x80\x93 %s \xC2\xB7 ended", a, b);
  }
}

// --- Empty and loading states ---------------------------------------------------------------
static int drawEmpty(float top) {
  const IptvList *l = iptv_list();
  const char *l1, *l2;
  if (l && nView) return 0;
  if (!l && iptv_state() == IPTV_FAILED) { l1 = iptv_status(); l2 = "Check the address or login under Source, above."; }
  else if (!l) { l1 = "Loading channels\xE2\x80\xA6"; l2 = "Large playlists can take a few seconds."; }
  else if (group == GROUP_FAV) { l1 = "No favourites yet"; l2 = "Hold OK on a channel to add it here."; }
  else if (group == GROUP_RECENT) { l1 = "Nothing watched yet"; l2 = "Channels you watch appear here."; }
  else { l1 = "No channels in this group"; l2 = ""; }
  { TxtLine a = txt_line(TXT_LIVE_TITLE, l1, HEXI(0xF5F6F8), 255);
    TxtLine b = txt_line(TXT_PG_END, l2, HEXI(0x9AA1A9), 255);
    float cx = X0() + (L_RIGHT - X0()) * 0.5f;
    txt_draw(a, cx - a.w * 0.5f, top + 160.0f);
    if (l2[0]) txt_draw(b, cx - b.w * 0.5f, top + 172.0f + a.h); }
  return 1;
}

// --- Y1, the guide ---------------------------------------------------------------------------
static float gridX(void) { return X0() + G_CH_W + G_COL_GAP; }
static float gridW(void) { return L_RIGHT - gridX(); }

// The dashed outline of a row with no guide, baked once for the row's size.
// CSS's 1px dashed: 4 on, 4 off, round corners of G_BLOCK_R.
static GLuint dashOutline(int w, int h) {
  unsigned char *px;
  if (dashTex && dashW == w && dashH == h) return dashTex;
  if (dashTex) { gfx_tex_forget(dashTex); glDeleteTextures(1, &dashTex); dashTex = 0; }
  if (w < 16 || h < 16 || !(px = calloc((size_t)w * h, 4))) return 0;
  { float R = G_BLOCK_R;
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++) {
        float fx = x + 0.5f, fy = y + 0.5f;
        float qx = fabsf(fx - w * 0.5f) - (w * 0.5f - R), qy = fabsf(fy - h * 0.5f) - (h * 0.5f - R);
        float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
        float d = sqrtf(ox * ox + oy * oy) + (qx > qy ? (qx < 0 ? qx : 0) : (qy < 0 ? qy : 0)) - R;
        float cov = 1.0f - fabsf(d + 0.5f);
        int corner = qx > 0 && qy > 0, along = qx > qy ? y : x;
        if (cov <= 0.0f) continue;
        if (!corner && (along / 4) % 2) continue;
        { unsigned char *p = px + ((size_t)y * w + x) * 4;
          p[0] = p[1] = p[2] = 255;
          p[3] = (unsigned char)(cov > 1.0f ? 26 : 26.0f * cov); }
      } }
  glGenTextures(1, &dashTex);
  glBindTexture(GL_TEXTURE_2D, dashTex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
  gfx_tex_forget(0);
  free(px);
  dashW = w; dashH = h;
  return dashTex;
}

static void guideDetail(void) {
  const IptvList *l = iptv_list();
  int ch = focusedChannel();
  const IptvChannel *c = chan(ch);
  long long now = nowMinute();
  float x = X0(), y = G_DETAIL_Y, w = L_RIGHT - G_PREV_W - 56.0f - x;
  long long s, e;
  int p;
  char line[300];
  if (!c) return;
  p = cellAt(ch, fChan ? nowSec() : fTime, &s, &e);
  if (w > 1080.0f) w = 1080.0f;
  // The kicker: ON NOW, the channel, the time.
  { float kx = x;
    const IptvProgramme *pg = p >= 0 ? &l->pg[p] : NULL;
    if (pg && pg->start <= now && now < pg->stop) kx += onNow(kx, y, 1.0f) + 14.0f;
    snprintf(line, sizeof line, "%d \xC2\xB7 %s%s", c->number, c->name, iptv_is_favourite(ch) ? "  \xE2\x98\x85" : "");
    kx += inkMid(TXT_LIVE_META, line, 0x9AA1A9, kx, y + 16.0f, w * 0.5f, 1.0f) + 14.0f;
    if (pg) {
      kx += inkMid(TXT_LIVE_META, "\xC2\xB7", 0x4D535A, kx, y + 16.0f, 40.0f, 1.0f) + 14.0f;
      whenText(pg, now, 0, line, sizeof line);
      inkMid(TXT_LIVE_META, line, 0x9AA1A9, kx, y + 16.0f, x + w - kx, 1.0f);
    } }
  y += 32.0f + 12.0f;
  if (p >= 0) {
    const IptvProgramme *pg = &l->pg[p];
    TxtLine t = txt_line_trim(TXT_LIVE_TITLE, pg->title, HEXI(0xF5F6F8), 255, w);
    txt_draw(t, x, y);
    y += t.h + 8.0f;
    if (pg->desc[0]) txt_block_trim(TXT_PG_END, pg->desc, HEXI(0x9AA1A9), x, y, w < 940.0f ? w : 940.0f, 29.0f, 1.0f, 2);
  } else {
    TxtLine t = txt_line_trim(TXT_LIVE_TITLE, c->name, HEXI(0xF5F6F8), 255, w);
    txt_draw(t, x, y);
    y += t.h + 8.0f;
    ink(TXT_PG_END, iptv_guide_state() == IPTV_LOADING ? "The TV guide is still loading."
                    : c->nPg ? "No information for this time." : "No guide data for this channel \xE2\x80\x94 press OK to watch.",
        0x9AA1A9, x, y, 1.0f);
  }
}

static void guidePreview(void) {
  GfxRect r = { L_RIGHT - G_PREV_W, G_PREV_Y, G_PREV_W, G_PREV_H };
  const IptvChannel *tc = chan(tuned);
  char s[300];
  if (ownsVideo() && tc) {
    drawVideo(r, G_PREV_R);
    pictureShade(r, 56.0f, 0.86f);
    liveTag(r.x + 14.0f, r.y + 14.0f, 30.0f, 1.0f);
    snprintf(s, sizeof s, "%d \xC2\xB7 %s", tc->number, tc->name);
    inkTrim(TXT_PLR_BADGE, s, 0xF5F6F8, r.x + 14.0f, r.y + r.h - 12.0f - 20.0f, r.w - 28.0f, 1.0f);
    if (playFailed) {
      TxtLine m = txt_line(TXT_LIVE_NOTE, "Channel unavailable", 255, 255, 255, 255);
      txt_draw(m, r.x + (r.w - m.w) * 0.5f, r.y + (r.h - m.h) * 0.5f);
    }
    // Under it: which channel this is, and what OK does.
    equaliser(r.x, r.y + r.h + 12.0f + 18.0f, 18.0f, 1.0f);
    ink(TXT_LIVE_NOTE, tuned == focusedChannel() && !fChan ? "Watching \xE2\x80\x94 press OK to go full screen"
                       : "Watching \xE2\x80\x94 OK on another channel switches",
        0x8A9199, r.x + EQ_W + 10.0f, r.y + r.h + 10.0f, 1.0f);
  } else {
    pictureIdle(r, G_PREV_R, focusedChannel(), 72.0f);
  }
}

static void guideRuler(double ws, double pps) {
  float tx = gridX(), tw = gridW();
  char day[48];
  { time_t tt = (time_t)nowSec(); struct tm tmv; localtime_r(&tt, &tmv);
    strftime(day, sizeof day, "%a %d %b", &tmv); }
  ink(TXT_SRC_CHIP, day, 0x7C838B, X0(), G_RULER_Y + 4.0f, 1.0f);
  gfx_crop(tx, G_RULER_Y, tw, G_RULER_H);
  for (long long s = slotFloor((long long)ws); s < (long long)ws + LIVE_SPAN_S; s += LIVE_SLOT_S) {
    float sx = tx + (float)((double)(s - ws) * pps);
    char c[16];
    if ((double)s < ws) continue;
    clockText(s, c, sizeof c);
    gfx_color((GfxRect){ sx, G_RULER_Y, 1.0f, G_RULER_H }, 0.0f, 1, 1, 1, 0.10f);
    ink(TXT_SRC_TEXT, c, 0x7C838B, sx + 14.0f, G_RULER_Y + 4.0f, 1.0f);
  }
  gfx_no_crop();
}

// One programme block, in its tier.
static void guideBlock(const IptvProgramme *pg, GfxRect b, int focused, long long now, float nowX, int row,
                       double ws) {
  int airing = pg->start <= now && now < pg->stop, past = pg->stop <= now;
  float ar, ag, ab;
  unsigned titleHex = focused ? C_INK : past ? 0x8A9199 : airing ? 0xE4E7EA : 0xA9B0B8;
  unsigned timeHex = focused ? 0x4A5058 : airing ? 0x7C838B : 0x636A71;
  accent(&ar, &ag, &ab);
  if (focused) {
    gfx_color((GfxRect){ b.x - 3.0f, b.y - 3.0f, b.w + 6.0f, b.h + 6.0f }, (G_BLOCK_R + 3.0f) / (b.h + 6.0f),
              1, 1, 1, 0.30f);
    gfx_color(b, G_BLOCK_R / b.h, HEXF(C_PAPER), 1.0f);
  } else {
    gfx_color(b, G_BLOCK_R / b.h, 1, 1, 1, 0.05f);
  }
  // What has aired of it, tinted: the seek bar's read, in the schedule.
  if (airing && nowX > b.x) {
    float cw = (nowX < b.x + b.w ? nowX : b.x + b.w) - b.x;
    gfx_crop(b.x, b.y, cw, b.h);
    gfx_color(b, G_BLOCK_R / b.h, ar, ag, ab, focused ? 0.26f : 0.14f);
    gfx_crop(gridX(), G_GRID_Y, gridW(), G_BOTTOM - G_GRID_Y);
  }
  // Y3's tiers: text only where it says something whole.
  if (b.w >= G_TIER_TEXT) {
    char when[40], a[16], z[16];
    float pad = b.w >= G_TIER_FULL ? 18.0f : 16.0f;
    TxtLine t = txt_line_trim(focused ? TXT_SRC_STATE : TXT_SRC_META, pg->title, HEXI(titleHex), 255, b.w - 2 * pad);
    TxtLine m;
    clockText(pg->start, a, sizeof a);
    clockText(pg->stop, z, sizeof z);
    if (b.w >= G_TIER_FULL) snprintf(when, sizeof when, "%s \xE2\x80\x93 %s", a, z);
    else snprintf(when, sizeof when, "%s", a);
    m = txt_line(TXT_LIVE_TIME, when, HEXI(timeHex), 255);
    { float top = b.y + (b.h - (t.h + 4.0f + m.h)) * 0.5f;
      txt_draw(t, b.x + pad, top);
      txt_draw(m, b.x + pad, top + t.h + 4.0f); }
  } else if (b.w >= 12.0f) {
    gfx_color((GfxRect){ b.x + b.w * 0.5f - 3.0f, b.y + b.h * 0.5f - 3.0f, 6.0f, 6.0f }, 0.5f,
              focused ? 0.0f : 1.0f, focused ? 0.0f : 1.0f, focused ? 0.0f : 1.0f, focused ? 0.35f : 0.22f);
  }
  { int mins = (int)(((pg->start > (long long)ws ? pg->start : (long long)ws) - winStart) / 60);
    pointer_zone(b.x, b.y, b.w, b.h, pointCell, row, mins < 0 ? 0 : mins); }
}

static void guideChannelCell(const IptvChannel *c, int ch, int r, float y) {
  GfxRect cr = { X0(), y, G_CH_W, G_ROW_H };
  int isTuned = ch == tuned && (ownsVideo() || zapPending);
  int focused = zone == ZONE_BODY && r == fRow && fChan;
  char num[16];
  float nameX = cr.x + 16.0f + 34.0f + 14.0f + G_TILE + 14.0f;
  if (focused) {
    gfx_color((GfxRect){ cr.x - 3.0f, cr.y - 3.0f, cr.w + 6.0f, cr.h + 6.0f }, (G_CH_R + 3.0f) / (cr.h + 6.0f),
              1, 1, 1, 0.30f);
    gfx_color(cr, G_CH_R / cr.h, HEXF(C_PAPER), 1.0f);
  } else {
    gfx_color(cr, G_CH_R / cr.h, 1, 1, 1, isTuned ? 0.075f : 0.04f);
  }
  snprintf(num, sizeof num, "%d", c->number);
  inkMid(TXT_LIVE_META_B, num, focused ? 0x4A5058 : isTuned ? 0xC1C7CD : 0x7C838B, cr.x + 16.0f,
         y + G_ROW_H * 0.5f, 40.0f, 1.0f);
  identity(c, (GfxRect){ cr.x + 64.0f, y + (G_ROW_H - G_TILE) * 0.5f, G_TILE, G_TILE }, 10.0f,
           focused ? C_PLATE_F : C_PLATE, isTuned || focused ? 0xC1C7CD : 0x9AA1A9, 1.0f);
  { float room = cr.x + cr.w - 16.0f - nameX - (isTuned ? EQ_W + 12.0f : 0.0f);
    inkMid(isTuned || focused ? TXT_DETWEB_EP_BADGE : TXT_LIVE_NAME, c->name,
           focused ? C_INK : isTuned ? 0xF5F6F8 : 0xC1C7CD, nameX, y + G_ROW_H * 0.5f, room, 1.0f); }
  if (isTuned) equaliser(cr.x + cr.w - 16.0f - EQ_W, y + G_ROW_H * 0.5f + 9.0f, 18.0f, 1.0f);
  pointer_zone(cr.x, cr.y, cr.w, cr.h, pointChan, r, 0);
}

static void drawGuide(void) {
  const IptvList *l = iptv_list();
  float tx = gridX(), tw = gridW();
  double ws = (double)winStart, we = ws + LIVE_SPAN_S;
  double pps = tw / (double)LIVE_SPAN_S;
  long long now = nowMinute();
  float nowX = (float)(int)(tx + (float)(((double)now - ws) * pps) + 0.5f);
  int focusPg = -1, perPage = (int)((G_BOTTOM - G_GRID_Y) / G_ROW_STEP);
  long long fs = 0, fe = 0;
  float lastY = G_GRID_Y;

  guideDetail();
  guidePreview();
  if (drawEmpty(G_RULER_Y - 120.0f)) return;
  guideRuler(ws, pps);
  if (zone == ZONE_BODY && !fChan) focusPg = cellAt(focusedChannel(), fTime, &fs, &fe);

  gfx_crop(0.0f, G_GRID_Y - 4.0f, NV_SCREEN_W, G_BOTTOM - G_GRID_Y + 8.0f);
  pointer_clip(0.0f, G_GRID_Y - 4.0f, NV_SCREEN_W, G_BOTTOM - G_GRID_Y + 8.0f);
  // Only the rows on screen, and in them only the blocks inside the window.
  for (int r = (int)scrollRows; r < nView; r++) {
    float y = G_GRID_Y + (r - scrollRows) * G_ROW_STEP;
    int ch = view[r], rowFocus = zone == ZONE_BODY && r == fRow && !fChan;
    const IptvChannel *c = &l->ch[ch];
    if (y > G_BOTTOM) break;
    lastY = y + G_ROW_H;
    guideChannelCell(c, ch, r, y);

    gfx_crop(tx, G_GRID_Y - 4.0f, tw, G_BOTTOM - G_GRID_Y + 8.0f);
    if (!c->nPg) {
      GfxRect b = { tx, y, tw, G_ROW_H };
      const char *msg = iptv_guide_state() == IPTV_LOADING ? "Loading guide\xE2\x80\xA6"
                                                           : "No guide data \xE2\x80\x94 press OK to watch";
      if (rowFocus) {
        gfx_color((GfxRect){ b.x - 3.0f, b.y - 3.0f, b.w + 6.0f, b.h + 6.0f }, (G_BLOCK_R + 3.0f) / (b.h + 6.0f),
                  1, 1, 1, 0.30f);
        gfx_color(b, G_BLOCK_R / b.h, HEXF(C_PAPER), 1.0f);
      } else {
        // Dashed, not filled: absence, not a very long programme.
        GLuint dash = dashOutline((int)b.w, (int)b.h);
        if (dash) gfx_texture(b, dash);
      }
      inkMid(TXT_LIVE_META, msg, rowFocus ? 0x4A5058 : 0x565C63, b.x + 18.0f, y + G_ROW_H * 0.5f, b.w - 36.0f, 1.0f);
      pointer_zone(b.x, b.y, b.w, b.h, pointCell, r, 0);
    } else {
      int p = iptv_programme_at(l, ch, (long long)ws);
      int end = c->firstPg + c->nPg;
      if (p < 0) p = iptv_programme_after(l, ch, (long long)ws);
      for (; p >= 0 && p < end && (double)l->pg[p].start < we; p++) {
        const IptvProgramme *pg = &l->pg[p];
        double s = (double)pg->start < ws ? ws : (double)pg->start;
        double e = (double)pg->stop > we ? we : (double)pg->stop;
        float x0 = tx + (float)((s - ws) * pps), x1 = tx + (float)((e - ws) * pps) - G_BLOCK_GAP;
        if (x1 - x0 < 2.0f) continue;
        guideBlock(pg, (GfxRect){ x0, y, x1 - x0, G_ROW_H }, rowFocus && p == focusPg, now, nowX, r, ws);
      }
      // A focused gap between two programmes.
      if (rowFocus && focusPg < 0) {
        double s = (double)fs < ws ? ws : (double)fs, e = (double)fe > we ? we : (double)fe;
        float x0 = tx + (float)((s - ws) * pps), x1 = tx + (float)((e - ws) * pps) - G_BLOCK_GAP;
        if (x1 - x0 > 4.0f) {
          GfxRect b = { x0, y, x1 - x0, G_ROW_H };
          gfx_color((GfxRect){ b.x - 3.0f, b.y - 3.0f, b.w + 6.0f, b.h + 6.0f }, (G_BLOCK_R + 3.0f) / (b.h + 6.0f),
                    1, 1, 1, 0.30f);
          gfx_color(b, G_BLOCK_R / b.h, HEXF(C_PAPER), 1.0f);
          if (b.w >= G_TIER_TEXT)
            inkMid(TXT_SRC_STATE, "No information", C_INK, b.x + 16.0f, y + G_ROW_H * 0.5f, b.w - 32.0f, 1.0f);
        }
      }
    }
    gfx_crop(0.0f, G_GRID_Y - 4.0f, NV_SCREEN_W, G_BOTTOM - G_GRID_Y + 8.0f);
  }
  gfx_no_crop();
  pointer_no_clip();

  // Now: one line down the grid, and its cap on the ruler with the minute.
  if ((double)now >= ws && (double)now < we) {
    float r, g, b;
    char c[16];
    float bottom = lastY < G_BOTTOM ? lastY : G_BOTTOM;
    accent(&r, &g, &b);
    gfx_color((GfxRect){ nowX - 1.0f, G_GRID_Y, 2.0f, bottom - G_GRID_Y }, 0.0f, r, g, b, 1.0f);
    clockText(now, c, sizeof c);
    { float w = tagWidth(c) + 18.0f;
      GfxRect cap = { nowX - w * 0.5f, G_GRID_Y - 24.0f, w, 26.0f };
      gfx_color(cap, 6.0f / 26.0f, r, g, b, 1.0f);
      tagText(c, 0xFFFFFF, cap.x + 9.0f, cap.y + 13.0f, 1.0f); }
  }
  fadeBottom(NV_SCREEN_H - 130.0f, 130.0f);
  pager(NV_SCREEN_W - 54.0f, G_GRID_Y, G_BOTTOM - G_GRID_Y, nView, perPage, fRow);
}

// --- Y2, the channel list ---------------------------------------------------------------------
static float panelX(void) { return L_RIGHT - C_PANEL_W; }

static void listRow(const IptvList *l, int r, float y, float listW) {
  int ch = view[r];
  const IptvChannel *c = &l->ch[ch];
  // The row stays raised while its actions have the focus — it is still what
  // they act on — but only one thing wears the white ring at a time.
  int focused = zone != ZONE_CHIPS && zone != ZONE_HEAD && r == fRow;
  int ringed = focused && zone == ZONE_BODY;
  int isTuned = ch == tuned && (ownsVideo() || zapPending);
  float a = focused ? 1.0f : C_DIM;
  float x = X0();
  GfxRect row = { x, y, listW, C_ROW_H };
  long long now = nowMinute();
  const IptvProgramme *pg = programmeAt(ch, now), *next = programmeAfter(ch, now, 0);
  char num[16], nextLine[300];
  float nameX = x + 20.0f + 38.0f + 18.0f + C_TILE + 18.0f;
  float progX = nameX + 216.0f + 18.0f;
  float nextW = 0.0f, rightEdge = x + listW - 20.0f;

  if (focused) {
    gfx_drop_shadow(row, C_ROW_R, 38.0f, 18.0f, 0.55f);
    gfx_color((GfxRect){ row.x - 3.0f, row.y - 3.0f, row.w + 6.0f, row.h + 6.0f },
              (C_ROW_R + 3.0f) / (row.h + 6.0f), HEXF(C_PAPER), ringed ? 1.0f : 0.30f);
    gfx_color(row, C_ROW_R / row.h, NV_COLOR_BACKGROUND_R, NV_COLOR_BACKGROUND_G, NV_COLOR_BACKGROUND_B, 1.0f);
    gfx_color(row, C_ROW_R / row.h, 1, 1, 1, 0.10f);
  }
  snprintf(num, sizeof num, "%d", c->number);
  inkMid(TXT_LIVE_META_B, num, focused ? 0xC1C7CD : 0x9AA1A9, x + 20.0f, y + C_ROW_H * 0.5f, 44.0f, a);
  identity(c, (GfxRect){ x + 76.0f, y + (C_ROW_H - C_TILE) * 0.5f, C_TILE, C_TILE }, 11.0f,
           focused ? C_PLATE_F : C_PLATE, focused ? 0xF5F6F8 : 0xC1C7CD, a);
  // The name, and under it the equaliser when this is the channel playing.
  { TxtLine t = txt_line_trim(TXT_SRC_TAB, c->name, HEXI(0xF5F6F8), 255, 216.0f);
    float top = isTuned ? y + (C_ROW_H - (t.h + 5.0f + 14.0f)) * 0.5f : y + (C_ROW_H - t.h) * 0.5f;
    txt_draw_alpha(t, nameX, top, a);
    if (isTuned) equaliser(nameX, top + t.h + 5.0f + 14.0f, 14.0f, a); }

  if (next) {
    snprintf(nextLine, sizeof nextLine, "Next \xC2\xB7 %s", next->title);
    nextW = txt_width(TXT_LIVE_NOTE, nextLine);
    if (nextW > 280.0f) nextW = 280.0f;
    // The programme keeps at least 200px (its bar is 300 when there is room);
    // "Next" takes what is left, truncating, and gives way altogether when that
    // is too little to read — the rail open narrows the list by 144.
    if (nextW > rightEdge - 18.0f - progX - 200.0f) nextW = rightEdge - 18.0f - progX - 200.0f;
    if (nextW < 120.0f) nextW = 0.0f;
  }
  { float progW = rightEdge - (nextW > 0.0f ? nextW + 18.0f : 0.0f) - progX;
    if (pg) {
      TxtLine t = txt_line_trim(focused ? TXT_SRC_STATE : TXT_SRC_META, pg->title,
                                HEXI(focused ? 0xF5F6F8 : 0xC1C7CD), 255, progW);
      float barW = progW < 300.0f ? progW : 300.0f;
      float top = y + (C_ROW_H - (t.h + 8.0f + 3.0f)) * 0.5f;
      txt_draw_alpha(t, progX, top, a);
      progressBar(progX, top + t.h + 8.0f, barW, 3.0f,
                  (float)(now - pg->start) / (float)(pg->stop - pg->start), focused ? 0.20f : 0.16f, a);
    } else {
      inkMid(TXT_PG_END, iptv_guide_state() == IPTV_LOADING ? "Loading guide\xE2\x80\xA6" : "No guide data",
             0x636A71, progX, y + C_ROW_H * 0.5f, progW, a);
    } }
  if (nextW > 0.0f) {
    TxtLine t = txt_line_trim(TXT_LIVE_NOTE, nextLine, HEXI(focused ? 0x8A9199 : 0x636A71), 255, nextW + 1.0f);
    txt_draw_alpha(t, rightEdge - t.w, y + (C_ROW_H - t.h) * 0.5f, a);
  }
  pointer_zone(row.x, row.y, row.w, row.h, pointRow, r, 0);
}

// The picture at the top of the panel: the TUNED channel, with its programme's
// progress; or, with nothing tuned, the focused channel waiting for OK.
static float listPreview(float y) {
  GfxRect r = { panelX(), y, C_PANEL_W, C_PREV_H };
  int showTuned = ownsVideo() && chan(tuned);
  int ch = showTuned ? tuned : focusedChannel();
  long long now = nowMinute();
  const IptvProgramme *pg = ch >= 0 ? programmeAt(ch, now) : NULL;
  if (showTuned) {
    drawVideo(r, C_PREV_R);
    pictureShade(r, r.h * 0.46f, 0.62f);
    liveTag(r.x + 20.0f, r.y + 20.0f, 34.0f, 1.0f);
  } else {
    pictureIdle(r, C_PREV_R, ch, 132.0f);
  }
  if (pg) {
    char a[16], line[300];
    float by = r.y + r.h - 18.0f - 16.0f - 10.0f - 20.0f, lx = r.x + 20.0f;
    long long in = (now - pg->start) / 60, left = (pg->stop - now + 59) / 60;
    progressBar(r.x + 20.0f, by, r.w - 40.0f, 4.0f, (float)(now - pg->start) / (float)(pg->stop - pg->start),
                0.22f, 1.0f);
    if (showTuned && tuned != focusedChannel()) {
      const IptvChannel *tc = chan(tuned);
      lx += ink(TXT_LIVE_TIME, tc->name, 0xE4E7EA, lx, by + 14.0f, 1.0f) + 12.0f;
      lx += ink(TXT_LIVE_TIME, "\xC2\xB7", 0x565C63, lx, by + 14.0f, 1.0f) + 12.0f;
    }
    clockText(pg->start, a, sizeof a);
    lx += ink(TXT_LIVE_TIME, a, 0xE4E7EA, lx, by + 14.0f, 1.0f) + 12.0f;
    lx += ink(TXT_LIVE_TIME, "\xC2\xB7", 0x565C63, lx, by + 14.0f, 1.0f) + 12.0f;
    snprintf(line, sizeof line, "%lld min in, %lld left", in, left);
    ink(TXT_LIVE_TIME, line, 0xA4AAB2, lx, by + 14.0f, 1.0f);
  }
  return r.y + r.h;
}

// The focused channel's detail, E1's hero pattern with the preview for artwork.
static void listDetail(float y) {
  const IptvList *l = iptv_list();
  int ch = focusedChannel();
  const IptvChannel *c = chan(ch);
  long long now = nowMinute();
  const IptvProgramme *pg = c ? programmeAt(ch, now) : NULL;
  float x = panelX(), w = C_PANEL_W;
  char line[300];
  (void)l;
  if (!c) return;
  // ON NOW, the time, and the picture's resolution when this is the one playing.
  { float kx = x;
    if (pg) {
      char a[16], b[16];
      kx += onNow(kx, y, 1.0f) + 14.0f;
      clockText(pg->start, a, sizeof a); clockText(pg->stop, b, sizeof b);
      snprintf(line, sizeof line, "%s \xE2\x80\x93 %s \xC2\xB7 %lld min", a, b, (pg->stop - pg->start) / 60);
    } else {
      snprintf(line, sizeof line, "%d \xC2\xB7 %s", c->number, c->name);
    }
    kx += inkMid(TXT_LIVE_META, line, 0x9AA1A9, kx, y + 16.0f, w * 0.7f, 1.0f) + 14.0f;
    if (ch == tuned && ownsVideo() && video_height() > 0) {
      int h = video_height();
      const char *res = h >= 2000 ? "4K" : h >= 1000 ? "1080P" : h >= 700 ? "720P" : "SD";
      float bw = tagWidth(res) + 22.0f;
      kx += inkMid(TXT_LIVE_META, "\xC2\xB7", 0x4D535A, kx, y + 16.0f, 40.0f, 1.0f) + 14.0f;
      gfx_color((GfxRect){ kx, y, bw, 32.0f }, 8.0f / 32.0f, 1, 1, 1, 0.30f);
      gfx_color((GfxRect){ kx + 1.0f, y + 1.0f, bw - 2.0f, 30.0f }, 7.0f / 30.0f,
                NV_COLOR_BACKGROUND_R, NV_COLOR_BACKGROUND_G, NV_COLOR_BACKGROUND_B, 1.0f);
      tagText(res, 0xE4E7EA, kx + 11.0f, y + 16.0f, 1.0f);
    } }
  y += 32.0f + 14.0f;
  { TxtLine t = txt_line_trim(TXT_LIVE_TITLE, pg ? pg->title : c->name, HEXI(0xF5F6F8), 255, w);
    txt_draw(t, x, y);
    y += t.h + 10.0f; }
  if (pg && pg->desc[0]) y += txt_block_trim(TXT_PG_END, pg->desc, HEXI(0x9AA1A9), x, y, w, 29.0f, 1.0f, 2);
  else if (!pg) {
    ink(TXT_PG_END, iptv_guide_state() == IPTV_LOADING ? "The TV guide is still loading."
                                                       : "No guide data for this channel.",
        0x9AA1A9, x, y, 1.0f);
    y += 29.0f;
  }
  y += 14.0f;
  // THEN: the next two.
  { const IptvProgramme *n1 = programmeAfter(ch, now, 0), *n2 = programmeAfter(ch, now, 1);
    if (n1) {
      float tx = x;
      char a[16];
      tx += txt_tracking(TXT_SRC_TIER, "THEN", HEXI(0x7C838B), tx, y + 2.0f, 1.0f, 1.7f) + 12.0f;
      clockText(n1->start, a, sizeof a);
      snprintf(line, sizeof line, "%s %s", a, n1->title);
      tx += inkTrim(TXT_LIVE_META, line, 0xA9B0B8, tx, y, (x + w - tx) * (n2 ? 0.6f : 1.0f), 1.0f) + 12.0f;
      if (n2 && x + w - tx > 160.0f) {
        tx += ink(TXT_LIVE_META, "\xC2\xB7", 0x4D535A, tx, y, 1.0f) + 12.0f;
        clockText(n2->start, a, sizeof a);
        snprintf(line, sizeof line, "%s %s", a, n2->title);
        inkTrim(TXT_LIVE_META, line, 0xA9B0B8, tx, y, x + w - tx, 1.0f);
      }
      y += 32.0f;
    } }
  y += 16.0f;
  if (y > NV_SCREEN_H - 62.0f - 36.0f) y = NV_SCREEN_H - 62.0f - 36.0f;
  // Watch, Favourite.
  { int fav = iptv_is_favourite(ch);
    const char *label[ACT_N] = { "Watch", fav ? "Favourited" : "Favourite" };
    const char *icon[ACT_N] = { "live_play", fav ? "live_star_fill" : "live_star" };
    float bx = x;
    for (int i = 0; i < ACT_N; i++) {
      int f = zone == ZONE_ACTIONS && actSel == i;
      TxtLine t = txt_line(i == ACT_WATCH || f ? TXT_DETWEB_EP_BADGE : TXT_LIVE_NAME, label[i],
                           HEXI(f ? C_INK : i == ACT_WATCH ? 0xE4E7EA : 0xA9B0B8), 255);
      float bw = (i == ACT_WATCH ? 24.0f : 26.0f) + 20.0f + 12.0f + t.w + (i == ACT_WATCH ? 28.0f : 26.0f);
      GfxRect r = { bx, y, bw, 62.0f };
      unsigned ic = f ? C_INK : i == ACT_WATCH ? 0xE4E7EA : 0xA9B0B8;
      if (f) gfx_color(r, 0.5f, HEXF(C_PAPER), 1.0f);
      else gfx_color(r, 0.5f, 1, 1, 1, i == ACT_WATCH ? 0.10f : 0.06f);
      gfx_icon((GfxRect){ r.x + (i == ACT_WATCH ? 24.0f : 26.0f), r.y + 21.0f, 20.0f, 20.0f }, icon[i], HEXF(ic), 1.0f);
      txt_draw(t, r.x + (i == ACT_WATCH ? 24.0f : 26.0f) + 32.0f, r.y + (r.h - t.h) * 0.5f);
      pointer_zone(r.x, r.y, r.w, r.h, pointAct, i, 0);
      bx += bw + 14.0f;
    } }
}

static void drawList(void) {
  const IptvList *l = iptv_list();
  float listW = panelX() - 44.0f - X0();
  if (drawEmpty(C_LIST_Y)) return;
  // The focused row's shadow reaches past the list's top: crop a little above it.
  gfx_crop(0.0f, C_LIST_Y - 24.0f, panelX() - 20.0f, NV_SCREEN_H - C_LIST_Y + 24.0f);
  pointer_clip(0.0f, C_LIST_Y - 8.0f, panelX() - 20.0f, NV_SCREEN_H - C_LIST_Y + 8.0f);
  // Unfocused rows first, the raised one last so its shadow falls over them.
  for (int pass = 0; pass < 2; pass++)
    for (int r = (int)scrollRows; r < nView; r++) {
      float y = C_LIST_Y + (r - scrollRows) * C_ROW_STEP;
      if (y > NV_SCREEN_H) break;
      if ((pass == 1) != (r == fRow)) continue;
      listRow(l, r, y, listW);
    }
  gfx_no_crop();
  pointer_no_clip();
  fadeBottom(NV_SCREEN_H - 110.0f, 110.0f);
  { float y = listPreview(C_LIST_Y);
    listDetail(y + 26.0f); }
}

// --- Setup ---------------------------------------------------------------------------------------
static GfxRect pillButton(float x, float y, const char *label, int focused, int on) {
  TxtLine t = txt_line(on || focused ? TXT_LIVE_META_B : TXT_LIVE_META, label,
                       HEXI(focused || on ? C_INK : 0xC1C7CD), 255);
  GfxRect r = { x, y, t.w + 2 * L_CHIP_PAD, 56.0f };
  if (focused) gfx_color((GfxRect){ r.x - 3.0f, r.y - 3.0f, r.w + 6.0f, r.h + 6.0f }, 0.5f, 1, 1, 1, 0.30f);
  if (focused || on) gfx_color(r, 0.5f, HEXF(C_PAPER), 1.0f);
  else gfx_color(r, 0.5f, 1, 1, 1, 0.07f);
  txt_draw(t, r.x + L_CHIP_PAD, r.y + (r.h - t.h) * 0.5f);
  return r;
}

static void drawSetup(void) {
  float x = X0();
  { TxtLine t = txt_line(TXT_TITLE3, "Live TV", 245, 246, 248, 255);
    txt_draw(t, x, L_BTN_Y + L_BTN_H * 0.5f - t.h * 0.5f); }
  ink(TXT_LIVE_TITLE, iptv_configured() ? "Change your IPTV source" : "Connect your IPTV service",
      0xF5F6F8, x, 132.0f, 1.0f);
  txt_block(TXT_PG_END,
            "Use the M3U playlist address your provider gave you, or sign in with an Xtream "
            "Codes login. The TV guide is found automatically when the source names one.",
            HEXI(0x9AA1A9), x, 196.0f, 1040.0f, 29.0f, 1.0f, 3);

  for (int i = 0; i < nSetupRows; i++) {
    int row = setupRows[i], focused = i == setupRow;
    float y = setupRowY(i);
    if (row == ROW_TYPE) {
      static const char *KIND[2] = { "M3U playlist", "Xtream Codes" };
      float bx = x;
      for (int k = 0; k < 2; k++) {
        int on = (k == 0) == (draft.kind != IPTV_SRC_XTREAM);
        GfxRect r = pillButton(bx, y, KIND[k], focused && on, on);
        pointer_zone(r.x, r.y, r.w, r.h, pointSetup, i, 0);
        bx += r.w + L_CHIP_GAP;
      }
    } else if (row == ROW_SAVE) {
      static const char *BTN[3] = { "Save and load", "Reload now", "Cancel" };
      float bx = x;
      for (int b = 0; b < setupButtons(); b++) {
        GfxRect r = pillButton(bx, y + 8.0f, BTN[b], focused && setupBtn == b, 0);
        pointer_zone(r.x, r.y, r.w, r.h, pointSetup, i, 0);
        bx += r.w + L_CHIP_GAP;
      }
    } else {
      int max;
      const char *text = fieldText(row, &max);
      GfxRect f = setupField(i);
      int isEditing = editing == row && ime_is_open();
      char shown[1100];
      ink(TXT_LIVE_META, fieldLabel(row), 0x9AA1A9, x + 4.0f, y, 1.0f);
      if (focused) gfx_color((GfxRect){ f.x - 3.0f, f.y - 3.0f, f.w + 6.0f, f.h + 6.0f },
                             13.0f / (f.h + 6.0f), HEXF(C_PAPER), isEditing ? 1.0f : 0.85f);
      gfx_color(f, 10.0f / f.h, HEXF(0x16191D), 1.0f);
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
        TxtLine t = txt_line_trim(TXT_DD_SEL, fieldHint(row), HEXI(0x636A71), 255, f.w - 48.0f);
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
  long long now = nowMinute();
  GfxRect plate = { 64.0f, NV_SCREEN_H - 64.0f - 232.0f, NV_SCREEN_W - 128.0f, 232.0f };
  float x, ar, ag, ab;
  char s[320], a1[16], b1[16];
  if (!c || a < 0.01f) return;
  accent(&ar, &ag, &ab);
  gfx_opacity_group = a;
  gfx_color(plate, 28.0f / plate.h, 0.05f, 0.05f, 0.06f, 0.88f);
  { char num[16]; TxtLine t;
    snprintf(num, sizeof num, "%d", c->number);
    t = txt_line(TXT_TITLE2, num, 255, 255, 255, 255);
    txt_draw(t, plate.x + 40.0f, plate.y + 30.0f); }
  identity(c, (GfxRect){ plate.x + 40.0f, plate.y + 110.0f, 92.0f, 92.0f }, 16.0f, C_PLATE, 0xC1C7CD, a);
  x = plate.x + 200.0f;
  { TxtLine t = txt_line_trim(TXT_HEADLINE, c->name, 255, 255, 255, 255, 900.0f);
    txt_draw(t, x, plate.y + 30.0f);
    if (iptv_is_favourite(tuned))
      gfx_icon((GfxRect){ x + t.w + 16.0f, plate.y + 30.0f + (t.h - 30.0f) * 0.5f, 30.0f, 30.0f },
               "live_star_fill", 0.98f, 0.78f, 0.29f, a); }
  { char clock[16]; clockText(nowSec(), clock, sizeof clock);
    TxtLine t = txt_line(TXT_PAUSE_CLOCK, clock, 235, 235, 240, 255);
    txt_draw(t, plate.x + plate.w - 40.0f - t.w, plate.y + 32.0f); }

  if (playFailed) {
    ink(TXT_DET_META, "This channel isn't available right now. Try another, or reload the list under Source.",
        0xFF8C8C, x, plate.y + 100.0f, a);
  } else if (!video_ready() || zapPending) {
    ink(TXT_DET_META, "Tuning\xE2\x80\xA6", 0xBEBEC3, x, plate.y + 100.0f, a);
  }
  { long long s0, e0;
    int p = cellAt(tuned, now, &s0, &e0), nx;
    float y = plate.y + ((playFailed || !video_ready() || zapPending) ? 140.0f : 100.0f);
    if (p >= 0) {
      const IptvProgramme *pg = &l->pg[p];
      float kw = tagWidth("NOW") + 20.0f;
      gfx_color((GfxRect){ x, y + 2.0f, kw, 28.0f }, 7.0f / 28.0f, ar, ag, ab, a);
      tagText("NOW", 0xFFFFFF, x + 10.0f, y + 16.0f, a);
      clockText(pg->start, a1, sizeof a1); clockText(pg->stop, b1, sizeof b1);
      inkTrim(TXT_SRC_TAB, pg->title, 0xFFFFFF, x + kw + 16.0f, y, 880.0f, a);
      snprintf(s, sizeof s, "%s \xE2\x80\x93 %s", a1, b1);
      { TxtLine t = txt_line(TXT_SRC_META, s, 180, 180, 185, 255);
        float bx = x + kw + 16.0f + 900.0f;
        txt_draw(t, plate.x + plate.w - 40.0f - t.w, y + 2.0f);
        progressBar(bx, y + 14.0f, plate.x + plate.w - 64.0f - t.w - bx, 4.0f,
                    (float)(now - pg->start) / (float)(pg->stop - pg->start), 0.18f, a); }
      y += 44.0f;
      nx = (p + 1 < c->firstPg + c->nPg) ? p + 1 : -1;
    } else {
      ink(TXT_SRC_META, iptv_guide_state() == IPTV_LOADING ? "Loading guide\xE2\x80\xA6" : "No guide data",
          0xA0A0A5, x, y, a);
      y += 44.0f;
      nx = iptv_programme_after(l, tuned, now);
    }
    if (nx >= 0 && y < plate.y + plate.h - 30.0f) {
      const IptvProgramme *pg = &l->pg[nx];
      float kw = tagWidth("NEXT");
      tagText("NEXT", 0x969699, x, y + 16.0f, a);
      clockText(pg->start, a1, sizeof a1);
      snprintf(s, sizeof s, "%s  %s", a1, pg->title);
      inkTrim(TXT_SRC_TAB, s, 0xC8C8CD, x + kw + 36.0f, y, 1200.0f, a);
    }
  }
  gfx_opacity_group = 1.0f;
}

static void drawQuick(void) {
  const IptvList *l = iptv_list();
  long long now = nowMinute();
  int rows = (int)((NV_SCREEN_H - 200.0f) / LIVE_QUICK_ROW);
  gfx_color((GfxRect){ 0.0f, 0.0f, LIVE_QUICK_W, NV_SCREEN_H }, 0.0f, 0.04f, 0.04f, 0.05f, 0.92f);
  ink(TXT_HEADLINE, groupLabel(group), 0xFFFFFF, 64.0f, 64.0f, 1.0f);
  gfx_crop(0.0f, 140.0f, LIVE_QUICK_W, NV_SCREEN_H - 170.0f);
  pointer_clip(0.0f, 140.0f, LIVE_QUICK_W, NV_SCREEN_H - 170.0f);
  for (int r = (int)quickScroll; r < nView && r < (int)quickScroll + rows + 2; r++) {
    float y = 150.0f + (r - quickScroll) * LIVE_QUICK_ROW;
    int ch = view[r], f = r == quickRow;
    const IptvChannel *c = &l->ch[ch];
    GfxRect row = { 40.0f, y, LIVE_QUICK_W - 80.0f, LIVE_QUICK_ROW - 8.0f };
    char num[16];
    int p = iptv_programme_at(l, ch, now);
    float nx = row.x + 172.0f;
    if (f) gfx_color(row, 12.0f / row.h, HEXF(C_PAPER), 1.0f);
    snprintf(num, sizeof num, "%d", c->number);
    inkMid(TXT_LIVE_META_B, num, f ? 0x4A5058 : 0x9AA1A9, row.x + 20.0f, y + row.h * 0.5f, 60.0f, 1.0f);
    identity(c, (GfxRect){ row.x + 90.0f, y + (row.h - 58.0f) * 0.5f, 58.0f, 58.0f }, 11.0f,
             f ? C_PLATE_F : C_PLATE, f ? 0xF5F6F8 : 0xC1C7CD, 1.0f);
    { TxtLine t = txt_line_trim(TXT_SRC_TAB, c->name, HEXI(f ? C_INK : 0xF5F6F8), 255,
                                row.w - 200.0f - (ch == tuned ? EQ_W + 12.0f : 0.0f));
      float top = y + (p >= 0 ? 12.0f : (row.h - t.h) * 0.5f);
      txt_draw(t, nx, top);
      if (ch == tuned) equaliser(nx + t.w + 12.0f, top + t.h - 4.0f, 16.0f, 1.0f);
      if (p >= 0)
        inkTrim(TXT_LIVE_NOTE, l->pg[p].title, f ? 0x4A5058 : 0x8A9199, nx, top + t.h + 4.0f, row.w - 200.0f, 1.0f); }
    pointer_zone(row.x, row.y, row.w, row.h, pointQuick, r, 0);
  }
  gfx_no_crop();
  pointer_no_clip();
}

static void drawFull(void) {
  const IptvChannel *c = chan(tuned);
  drawVideo((GfxRect){ 0.0f, 0.0f, NV_SCREEN_W, NV_SCREEN_H }, 0.0f);
  if (c && (!video_ready() || zapPending || playFailed) && bannerA < 0.5f)
    identity(c, (GfxRect){ NV_SCREEN_W * 0.5f - 80.0f, NV_SCREEN_H * 0.5f - 80.0f, 160.0f, 160.0f }, 28.0f,
             C_PLATE, 0xC1C7CD, 1.0f);
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
  drawChips();
  if (viewMode == VIEW_GUIDE) drawGuide(); else drawList();
  drawDigits();
  drawToast();
}
