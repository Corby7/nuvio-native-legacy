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
#include "detail.h"
#include "anim.h"
#include "gfx.h"
#include "hold.h"
#include "ime.h"
#include "layout.h"
#include "phonelink.h"
#include "pointer.h"
#include "qr.h"
#include "settings.h"
#include "tex_cache.h"
#include "text.h"
#include "timeshift.h"
#include "tracks.h"
#include "video.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
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
#define LIVE_TOAST_OV_MS  3000u   // the zap toast
#define LIVE_BAR_MS       6000u   // the bar, the peek, the walk
#define LIVE_ZAP_MS        350u    // CH+/CH- held down tunes only where it stops
#define LIVE_DIGITS_MS    1600u
#define LIVE_START_MS    15000u   // a stream with no picture by then has failed
#define LIVE_TOAST_MS     2600u
#define LIVE_EDGE_S         12.0   // this close to now is "live"
#define LIVE_BUF_LEAD_S      2.0   // live from the buffer starts this far back: data at once
#define LIVE_BUF_WAIT_MS  4000u    // the recorder's first bytes, before playing direct
#define LIVE_RESUME_MS    5000u    // a live pause shorter than this resumes in place
#define LIVE_ARCH_RESUME_MS 60000u // an archive paused longer reconnects where it was
#define LIVE_SCRUB_S        30.0   // a ◀▶ step while rewound, growing while held
#define LIVE_SCRUB_MS      800u    // the scrub lands when the keys rest this long
#define LIVE_PREVIEW_MS    900u    // resting on a channel this long previews it

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
enum { HEAD_SEARCH, HEAD_GUIDE, HEAD_LIST, HEAD_SOURCE, HEAD_N };
enum { ACT_WATCH, ACT_FAV, ACT_N };
enum { GROUP_ALL, GROUP_FAV, GROUP_RECENT, GROUP_FIRST };

static int mode, viewMode = VIEW_LIST, zone = ZONE_BODY, headSel = HEAD_LIST, actSel;
// SEARCH (the header's first button): a field where the chips were, the TV's
// keyboard over it, and the list and the guide narrowed to what matches as it
// is typed. While `searching`, ZONE_CHIPS is the field.
static int searching;
static char query[96];
static int queryLen;
static void closeSearch(void);
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
static Uint32 zapAt, tunedAt, zapKeyAt;
// THE OTHER CONTAINER (iptv_play_url_alt). `usingAlt`: this tune is playing
// it; `altTried`: this tune already switched once; `useAlt`: it is what played
// last time the first choice failed, so every tune starts there until the
// source changes. `streamAt` starts the LIVE_START_MS clock.
static int usingAlt, altTried, useAlt;
static Uint32 streamAt;

// THE OVERLAY OVER A PLAYING CHANNEL, Y5's four amounts, each one more press:
//   TOAST  ▲▼ with nothing showing: a card bottom-left, identity only, 3 s.
//   BAR    OK: what is this and what is next, the controls under it, 6 s.
//   PEEK   ▲▼ with the bar open: the title rolls into a vertical carousel of
//          channels by what is on now; OK zaps to the one in the middle, and
//          until then the stream stays put.
//   WALK   ◀▶ with the bar open: the same block, a later programme.
// Only OK retunes (peek) or changes anything (walk: the reminder). Timers
// restart on every press and never stack; the bar stays while a control has
// the focus.
enum { OV_NONE, OV_TOAST, OV_BAR, OV_PEEK, OV_WALK };
// The controls, by id. Not all are shown at once (shownControls): Start over
// needs a past to start from, Go live a present to go back to.
enum { CTL_PLAY, CTL_RESTART, CTL_LIVE, CTL_GUIDE, CTL_CHANNELS, CTL_SUBS, CTL_AUDIO,
       CTL_ASPECT, CTL_FAV, CTL_N };
static int ov;
static Uint32 ovUntil;
static float toastA, barA;
static int barCtl = -1;         // the focused control's id, -1 while the block has it
static float ctlFocus[CTL_N];   // each control's puck, faded like the film player's
static int peekRow;               // into view[]: the channel in the middle
static int peekPos;               // the same, unwrapped, so a wrap animates one row
static float peekScroll, peekScrollV;
static int walkPg = -1;         // the walked programme's index, -1 = now
// The picture's zoom, cycled by the Aspect control. Live has no per-title
// memory to keep it in, so it lasts the session.
static const float ASPECT_ZOOM[3] = { 1.0f, 1.15f, 1.34f };
static const char *ASPECT_NAME[3] = { "Fit", "Slight zoom", "Cinema zoom" };
static int aspectMode;
static float lastZoom = -1.0f;

// THE TIMELINE: where in time the picture is. Three kinds of load:
//   SRC_LIVE     the provider's stream, direct: now, and nothing before it
//   SRC_BUFFER   the pause buffer's loopback (timeshift.c): any instant it holds
//   SRC_ARCHIVE  the provider's catch-up: any instant its archive reaches
// A load plays from `srcBase` on at normal speed, so the instant on screen is
// srcBase plus the time it has played: counted here by the clock from the
// first ready frame, less the time paused. The pipeline's own position is no
// help — a live TS reports the broadcaster's clock, not the load's.
enum { SRC_LIVE, SRC_BUFFER, SRC_ARCHIVE };
static int srcKind;
static double srcBase;
static Uint32 srcReadyAt, pausedAt, pausedMs;
static int paused, eosSeen;
static int bufWaiting;          // the recorder is connecting; nothing plays yet
static Uint32 bufWaitUntil;
static int noteOnPlay;          // the load that starts counts as watched
// ◀▶ while rewound: the instant being chosen, landed when the keys rest.
static int scrubbing, scrubRun;
static double scrubAt;
static Uint32 scrubCommitAt, scrubLastKey;
// Preview on focus: the channel the browse focus rests on, and since when.
static int restCh = -1;
static Uint32 restSince;

// "Remind me", from walking the schedule. In memory for the session: a toast
// when the programme starts, while Live TV is open.
#define REMIND_MAX 32
static struct { char name[256]; char title[200]; int number; long long start; } reminders[REMIND_MAX];
static int nReminders;
// THE CHANNELS PANEL, over a playing channel: the film player's episode selector
// (episodes.c) with channels for episodes — the same column at the right, veil,
// slide, heading, pill, rows, opening row and list motion, all from its NV_EPL_*
// measures, so the two players' panels are one design. The group pill sits where
// the season pill does and is walked the same way: ◀▶ from anywhere, OK for its
// menu. OK on a row is the one press that retunes.
static int quickOpen, quickRow;
static float quickA, quickScroll;
static int quickZone;              // Q_PILL or Q_LIST
static int quickFromBar;           // opened from the bar's controls: close back to it
static int gmOpen, gmCursor, gmScroll;
static float *quickOpened;         // each row's opening, 0..1, as episodes.c's `opened`
static int quickOpenedCap;
static int quickSnap;
enum { Q_PILL, Q_LIST };
static int sheetWasUp;             // a tracks sheet or this panel was up last frame
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
// THE SOURCE SCREEN (the Z1 and Z2 mockups). Rows, top to bottom: the current
// source card's Refresh (only with a source), the M3U | Xtream switch, the
// fields (one address; or server and port, username, password and its "show"),
// the extra guide, and the buttons. A row holds up to three stops across.
enum { SR_REFRESH, SR_KIND, SR_URL, SR_SERVER, SR_LOGIN, SR_EPG, SR_BUTTONS };
enum { F_URL, F_SERVER, F_PORT, F_USER, F_PASS, F_EPG, F_N };
static IptvSource draft;
static char draftPort[8];       // Xtream's port, split off draft.server for its own field
static int showPass;
static int setupRow, setupCol;
static int setupRows[8], nSetupRows;
static int editing = -1;        // the F_* the keyboard is filling
static Uint32 phoneRetryAt;     // no network yet: when to look for one again
static GfxRect refreshRect;     // where the card drew Refresh, for the pointer

// --- Small helpers ---------------------------------------------------------------
static float X0(void) { return settings_content_x() - 8.0f; }
static long long nowSec(void) { return (long long)time(NULL); }
static double wallNow(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec + tv.tv_usec / 1e6;
}
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
// THE BAR IS ALWAYS THERE. A channel the guide knows nothing about at `t` still
// gets a track: the clock hour around `t`, titled with the channel's name, so
// the bar keeps its shape, its times and the pause buffer's reach. `*guessed`
// says the programme is this stand-in, for the words around it.
static const IptvProgramme *programmeOrHour(int ch, long long t, int *guessed) {
  static IptvProgramme hour[2];
  static int turn;
  const IptvProgramme *pg = programmeAt(ch, t);
  const IptvChannel *c = chan(ch);
  if (guessed) *guessed = 0;
  if (pg || !c) return pg;
  // Two slots, alternated: the peek draws a row with one while the bar still
  // holds a pointer to the other within the same frame.
  turn ^= 1;
  hour[turn].channel = ch;
  hour[turn].start = t - ((t % 3600) + 3600) % 3600;
  hour[turn].stop = hour[turn].start + 3600;
  hour[turn].title = c->name;
  hour[turn].desc = "";
  hour[turn].category = "";
  if (guessed) *guessed = 1;
  return &hour[turn];
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
// Where `needle` first occurs in `hay`, ASCII case folded; -1 when it does not.
static int foldFind(const char *hay, const char *needle) {
  size_t n = strlen(needle);
  if (!n) return 0;
  for (size_t i = 0; hay[i]; i++) {
    size_t k = 0;
    while (k < n && hay[i + k] && tolower((unsigned char)hay[i + k]) == tolower((unsigned char)needle[k])) k++;
    if (k == n) return (int)i;
  }
  return -1;
}

// The search's view, best first, each tier in playlist order: a name (or a
// number) that starts with the query; a word in the name that does; a name
// that has it anywhere; then a channel whose programme on now or later in the
// guide has it in its title. An empty query is every channel.
static void searchView(const IptvList *l) {
  unsigned char *tier = calloc((size_t)l->nCh, 1);
  long long now = nowSec();
  int digits = queryLen > 0;
  if (!tier) return;
  for (int i = 0; i < queryLen; i++) if (!isdigit((unsigned char)query[i])) digits = 0;
  for (int c = 0; c < l->nCh; c++) {
    int at;
    if (!queryLen) { tier[c] = 1; continue; }
    if (digits) {
      char num[16];
      snprintf(num, sizeof num, "%d", l->ch[c].number);
      if (!strncmp(num, query, (size_t)queryLen)) { tier[c] = 1; continue; }
    }
    at = foldFind(l->ch[c].name, query);
    if (at == 0) tier[c] = 1;
    else if (at > 0) tier[c] = isalnum((unsigned char)l->ch[c].name[at - 1]) ? 3 : 2;
  }
  if (queryLen >= 2)
    for (int p = 0; p < l->nPg; p++) {
      const IptvProgramme *pg = &l->pg[p];
      if (!tier[pg->channel] && pg->stop > now && foldFind(pg->title, query) >= 0) tier[pg->channel] = 4;
    }
  for (int t = 1; t <= 4; t++)
    for (int c = 0; c < l->nCh; c++) if (tier[c] == t) pushView(c);
  free(tier);
}

static void rebuildView(void) {
  const IptvList *l = iptv_list();
  int keep = -1;
  nView = 0;
  if (!l) { fRow = 0; return; }
  if (group >= nGroups()) group = GROUP_ALL;
  if (searching) {
    searchView(l);
  } else if (group == GROUP_RECENT) {
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
  long long now = nowSec(), floor = slotFloor(fTime < now ? fTime : now);
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
  // Walking forward out of the past, the programme on now is "now" again.
  if (fTime <= now) { cellAt(ch, fTime, &s, &e); if (e > now) fTime = now; }
  keepWindowOnFocus();
}

// DOWN and UP keep the instant — unless it is a past the new channel has no
// archive of; then it is now.
static void guideSettle(void) {
  long long now = nowSec();
  if (viewMode == VIEW_GUIDE && fTime < now - 90 && !iptv_has_archive(focusedChannel(), fTime)) {
    fTime = now;
    keepWindowOnFocus();
  }
}

static void guideLeft(void) {
  long long s, e, now = nowSec();
  int ch = focusedChannel();
  if (fChan) { requestMenu = 1; return; }
  cellAt(ch, fTime, &s, &e);
  // What is on now is the first column; left of it is the channel itself —
  // unless the channel keeps an archive: then the past is more columns.
  if (s <= now) {
    long long ps, pe;
    const IptvChannel *c = chan(ch);
    if (c && c->catchup && s > -LIVE_FOREVER) {
      cellAt(ch, s - 1, &ps, &pe);
      if (ps > -LIVE_FOREVER && iptv_has_archive(ch, ps)) { fTime = ps; keepWindowOnFocus(); return; }
    }
    fChan = 1;
    return;
  }
  cellAt(ch, s - 1, &s, &e);
  fTime = s < now ? now : s;
  keepWindowOnFocus();
}

// --- Playback ------------------------------------------------------------------------
static int ownsVideo(void) { return playing && video_session() == playSession; }

static void stopStream(void) {
  if (ownsVideo()) video_stop();
  timeshift_end();
  playing = 0; playFailed = 0; zapPending = 0; bufWaiting = 0;
  paused = 0; scrubbing = 0; srcKind = SRC_LIVE;
  lastWin[0] = -1;
}

// Hands `url` to the pipeline as a load of `kind` starting at wall time `base`.
static void playUrl(const char *url, int kind, double base) {
  // A live stream is neither Dolby Vision nor an MKV worth probing: the probe
  // would open a second connection, and IPTV accounts allow one.
  video_set_dv(0);
  video_set_mp4(1);
  errSeen = video_error_count();
  eosSeen = video_eos_count();
  playing = video_play(url);
  playSession = video_session();
  srcKind = kind; srcBase = base;
  srcReadyAt = 0; paused = 0; pausedMs = 0; pausedAt = 0;
  playFailed = !playing;
  streamAt = SDL_GetTicks();
  lastWin[0] = -1;
  if (noteOnPlay && playing) { iptv_note_watched(tuned); noteOnPlay = 0; }
  printf("[live] %s %d %s%s\n", kind == SRC_ARCHIVE ? "catch-up" : kind == SRC_BUFFER ? "buffered" : "tuned",
         chan(tuned) ? chan(tuned)->number : 0, chan(tuned) ? chan(tuned)->name : "?",
         playing ? "" : " (pipeline refused)");
  fflush(stdout);
}

// The stream as listed, or in the other container once that is what played
// (useAlt).
static void playDirect(void) {
  char relay[4200];
  const char *url = NULL;
  usingAlt = useAlt && (url = iptv_play_url_alt(tuned, relay, sizeof relay)) != NULL;
  if (!url) url = iptv_play_url(tuned, relay, sizeof relay);
  if (!url) { playFailed = 1; return; }
  playUrl(url, SRC_LIVE, wallNow());
}

// The live stream failed (`why`): play the same channel in the other
// container, once per tune. 0 when there is no other one or it was tried.
static int tryOther(const char *why) {
  char relay[4200];
  const char *url;
  if (altTried || srcKind != SRC_LIVE) return 0;
  url = usingAlt ? iptv_play_url(tuned, relay, sizeof relay)
                 : iptv_play_url_alt(tuned, relay, sizeof relay);
  if (!url) return 0;
  printf("[live] %s on %s; trying the other container\n", why, tunedName);
  fflush(stdout);
  altTried = 1;
  usingAlt = !usingAlt;
  playUrl(url, SRC_LIVE, wallNow());
  return playing;
}

// A failure with nothing left to try: said on screen with the pipeline's own
// words, which is what anyone reporting it needs to pass on.
static void failStream(const char *why) {
  char m[200];
  playFailed = 1;
  printf("[live] %s gave up: %s\n", tunedName, why);
  fflush(stdout);
  snprintf(m, sizeof m, "Couldn't play %s \xC2\xB7 %s", tunedName, why);
  say(m);
}

// Live, from the top: the pause buffer when it is on and the stream is one it
// can keep (MPEG-TS), the stream itself otherwise.
static void beginLive(void) {
  const IptvChannel *c = chan(tuned);
  long long budget;
  if (ownsVideo()) video_stop();
  playing = 0;
  timeshift_end();
  bufWaiting = 0; paused = 0; scrubbing = 0;
  if (!c) return;
  budget = settings_live_buffer_minutes() > 0 ? timeshift_budget(settings_live_buffer_minutes()) : 0;
  if (budget > 0 && timeshift_begin(c->url, c->headers, budget)) {
    // The recorder opens the one connection; the pipeline follows it once
    // bytes arrive (iptvui_update), or plays direct if none do.
    bufWaiting = 1;
    bufWaitUntil = SDL_GetTicks() + LIVE_BUF_WAIT_MS;
    srcKind = SRC_LIVE;
    return;
  }
  playDirect();
}

static void startStream(void) {
  zapPending = 0;
  playFailed = 0;
  altTried = 0;
  if (!chan(tuned)) return;
  tracks_reset();
  noteOnPlay = 1;
  beginLive();
}

// The instant on screen, as wall-clock seconds.
static double playAt(void) {
  Uint32 t = SDL_GetTicks();
  double now = wallNow(), v;
  long played;
  if (srcKind == SRC_LIVE) return paused ? now - (double)(t - pausedAt) / 1000.0 : now;
  if (!srcReadyAt) return srcBase;
  played = (long)(t - srcReadyAt) - (long)pausedMs - (paused ? (long)(t - pausedAt) : 0);
  v = srcBase + (played > 0 ? played : 0) / 1000.0;
  return v > now ? now : v;
}
static double shownAt(void) { return scrubbing ? scrubAt : playAt(); }
static int atLive(void) {
  return !paused && (srcKind == SRC_LIVE || wallNow() - playAt() < LIVE_EDGE_S);
}

// The earliest instant within reach: the buffer's oldest, or the archive's.
// 0 when there is none.
static double rewindFloor(void) {
  double lo = 0, hi, best = 0, now = wallNow();
  const IptvChannel *c = chan(tuned);
  if (timeshift_range(&lo, &hi)) best = lo;
  if (c && c->catchup) {
    double a = now - (double)c->catchupDays * 86400.0 + 120.0;
    if (!best || a < best) best = a;
  }
  return best;
}
static int canRewind(void) {
  double f = rewindFloor();
  // More past than the live edge's own width, or ◀ would land back on live.
  return ownsVideo() && !playFailed && f > 0 && f < wallNow() - (LIVE_EDGE_S + 5.0);
}

static void goLive(void);

// Plays from wall time `t`: from the buffer when it holds `t`, else from the
// archive, else it cannot.
static void seekTo(double t) {
  double now = wallNow(), lo, hi;
  char buf[4200];
  const char *u;
  scrubbing = 0;
  if (t >= now - LIVE_EDGE_S) { goLive(); return; }
  if (timeshift_range(&lo, &hi) && t >= lo - 1.0) {
    if (t < lo) t = lo;
    if ((u = timeshift_url(t, buf, sizeof buf))) { playUrl(u, SRC_BUFFER, t); return; }
  }
  if (iptv_has_archive(tuned, (long long)t)) {
    const IptvProgramme *pg = programmeAt(tuned, (long long)t);
    long long from = (long long)t, stop = pg ? pg->stop : from + 3600;
    if (stop - from < 120) stop = from + 3600;
    // The recorder's connection goes first: many accounts allow one.
    timeshift_end();
    if ((u = iptv_archive_url(tuned, from, stop, buf, sizeof buf))) {
      if (ownsVideo()) video_stop();
      playUrl(u, SRC_ARCHIVE, (double)from);
      return;
    }
  }
  say("That's further back than this channel keeps");
}

static void goLive(void) {
  double lo, hi;
  char buf[256];
  const char *u;
  scrubbing = 0;
  if (timeshift_range(&lo, &hi)) {
    double from = hi - LIVE_BUF_LEAD_S > lo ? hi - LIVE_BUF_LEAD_S : lo;
    if ((u = timeshift_url(from, buf, sizeof buf))) { playUrl(u, SRC_BUFFER, from); return; }
  }
  beginLive();
}

// Pause and play. Paused with a past to come back from (the buffer, the
// archive) it resumes where it froze. Paused with neither, a short pause
// resumes in place; a long one comes back to live, and says so — the stream
// went on without the picture, and pretending otherwise would stall it.
static void togglePause(void) {
  Uint32 t = SDL_GetTicks(), held;
  double at;
  if (!ownsVideo() || playFailed) return;
  if (!paused) {
    scrubbing = 0;
    video_pause(1);
    paused = 1; pausedAt = t;
    return;
  }
  at = playAt();
  held = t - pausedAt;
  paused = 0;
  if (srcKind == SRC_LIVE) {
    if (held < LIVE_RESUME_MS) video_pause(0);
    else if (iptv_has_archive(tuned, (long long)at)) seekTo(at);
    else { beginLive(); say("Back to live"); }
    return;
  }
  pausedMs += held;
  if (srcKind == SRC_BUFFER) {
    double lo, hi;
    // Paused for longer than the ring keeps: the loopback has already skipped
    // ahead to the oldest it has, so the picture is reloaded there, knowingly.
    if (timeshift_range(&lo, &hi) && at < lo - 0.5) seekTo(lo + 5.0);
    else video_pause(0);
  } else if (held > LIVE_ARCH_RESUME_MS) {
    seekTo(at);
  } else {
    video_pause(0);
  }
}

// ◀▶ while rewound: moves the instant being chosen; it lands when the keys
// rest. Steps grow while the key is held, as the film player's do.
static void scrub(int dir) {
  Uint32 t = SDL_GetTicks();
  double now = wallNow(), floor = rewindFloor(), step;
  if (!scrubbing) { scrubAt = playAt(); scrubbing = 1; scrubRun = 0; }
  scrubRun = t - scrubLastKey < 450u ? scrubRun + 1 : 0;
  scrubLastKey = t;
  step = LIVE_SCRUB_S * (scrubRun < 4 ? 1 : scrubRun < 10 ? 4 : 20);
  scrubAt += dir * step;
  if (floor > 0 && scrubAt < floor) scrubAt = floor;
  if (scrubAt > now) scrubAt = now;
  scrubCommitAt = t + LIVE_SCRUB_MS;
}

// The start of what is on screen, reachable?
static const IptvProgramme *restartable(void) {
  const IptvProgramme *pg = programmeAt(tuned, (long long)playAt());
  double f = rewindFloor();
  if (!pg || !ownsVideo() || f <= 0 || (double)pg->start < f - 1.0) return NULL;
  return pg;
}

// The controls on show, in order; the count.
static int shownControls(int *out) {
  int n = 0;
  out[n++] = CTL_PLAY;
  if (restartable()) out[n++] = CTL_RESTART;
  if (!atLive() && srcKind != SRC_LIVE) out[n++] = CTL_LIVE;
  for (int i = CTL_GUIDE; i < CTL_N; i++) out[n++] = i;
  return n;
}
static int shownIndex(int id) {
  int ids[CTL_N], n = shownControls(ids);
  for (int i = 0; i < n; i++) if (ids[i] == id) return i;
  return -1;
}
static int shownFirst(void) { int ids[CTL_N]; shownControls(ids); return ids[0]; }
static void barStep(int dir) {
  int ids[CTL_N], n = shownControls(ids), i = shownIndex(barCtl);
  if (i < 0) { barCtl = ids[0]; return; }
  i += dir;
  if (i >= 0 && i < n) barCtl = ids[i];
}

// A preview: tuned like any channel, but not counted as watched.
static void previewTune(int ch) {
  const IptvChannel *c = chan(ch);
  if (!c) return;
  if (ch != tuned) timeshift_end();
  tuned = ch;
  snprintf(tunedName, sizeof tunedName, "%s", c->name);
  tunedAt = SDL_GetTicks();
  zapPending = 0; playFailed = 0;
  tracks_reset();
  noteOnPlay = 0;
  beginLive();
}

// Tunes `ch`. `immediate` starts the stream at once; otherwise it waits
// LIVE_ZAP_MS for the keys to stop, so a run of CH+ presses opens one stream
// (zap decides which).
static void tune(int ch, int immediate) {
  const IptvChannel *c = chan(ch);
  if (!c) return;
  if (ch != tuned) timeshift_end();
  tuned = ch;
  snprintf(tunedName, sizeof tunedName, "%s", c->name);
  tunedAt = SDL_GetTicks();
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
  // A LONE PRESS TUNES AT ONCE. Only a press that follows another within
  // LIVE_ZAP_MS waits for the keys to stop: the single CH+ used to pay the
  // whole wait before the stream even started loading. A run of presses costs
  // one extra load, the first one, which the next tune unloads.
  { Uint32 t = SDL_GetTicks();
    int alone = !zapPending && t - zapKeyAt >= LIVE_ZAP_MS;
    zapKeyAt = t;
    tune(view[r], alone); }
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

// Shows overlay `level`, restarting its clock. Called again at the same level
// it only restarts the clock: a run of presses never replays the entrance.
static void overlay(int level) {
  ov = level;
  ovUntil = SDL_GetTicks() + (level == OV_TOAST ? LIVE_TOAST_OV_MS : LIVE_BAR_MS);
  if (level != OV_BAR) barCtl = -1;
  if (level != OV_WALK) walkPg = -1;
}

// Arriving full screen, the bar says what this is.
static void enterFull(void) {
  full = 1;
  quickOpen = 0;
  barCtl = -1;
  overlay(OV_BAR);
}

// OK on a channel: watch it full screen. OK on the one already playing just
// takes it full screen; it is never retuned.
static void watch(int ch) {
  if (ch < 0) return;
  if (!(ch == tuned && (ownsVideo() || zapPending || bufWaiting))) tune(ch, 1);
  else if (!noteOnPlay) iptv_note_watched(ch);   // a preview becomes a viewing
  enterFull();
}

// OK on a programme that has ended, on a channel with catch-up: from its start.
static void watchFrom(int ch, long long start) {
  const IptvChannel *c = chan(ch);
  if (!c) return;
  if (ownsVideo()) video_stop();
  playing = 0; zapPending = 0; bufWaiting = 0;
  if (ch != tuned) timeshift_end();
  tuned = ch;
  snprintf(tunedName, sizeof tunedName, "%s", c->name);
  tunedAt = SDL_GetTicks();
  tracks_reset();
  noteOnPlay = 1;
  seekTo((double)start);
  if (!ownsVideo()) { beginLive(); return; }   // it said why; live instead
  enterFull();
}

static void toggleFavourite(int ch) {
  if (ch < 0) return;
  iptv_toggle_favourite(ch);
  say(iptv_is_favourite(ch) ? "Added to Favourites" : "Removed from Favourites");
  if (group == GROUP_FAV) rebuildView();
}

// --- Setup form ------------------------------------------------------------------------
// Save and load TRIES the draft (iptv_try_source): the current source stays
// until the new playlist has arrived, and the result — or which thing went
// wrong — is said on this screen, under the fields, never by sending the
// viewer to an empty guide. What they typed is never cleared.
#define SRC_CEILING_S 15

static int configured(void) { return iptv_configured(); }

static int rowCols(int row) {
  switch (row) {
    case SR_SERVER:  return 2;
    case SR_LOGIN:   return 3;
    case SR_BUTTONS: return configured() ? 2 : 1;
    default:         return 1;
  }
}

static int fieldAt(int row, int col) {
  switch (row) {
    case SR_URL:    return F_URL;
    case SR_SERVER: return col ? F_PORT : F_SERVER;
    case SR_LOGIN:  return col == 0 ? F_USER : col == 1 ? F_PASS : -1;
    case SR_EPG:    return F_EPG;
    default:        return -1;
  }
}

// The rows for the draft's kind, keeping the focus on the same row where it
// still exists (the switch, the guide, the buttons).
static void layoutSetup(void) {
  int was = nSetupRows ? setupRows[setupRow] : SR_KIND;
  nSetupRows = 0;
  if (configured()) setupRows[nSetupRows++] = SR_REFRESH;
  setupRows[nSetupRows++] = SR_KIND;
  if (draft.kind == IPTV_SRC_XTREAM) {
    setupRows[nSetupRows++] = SR_SERVER;
    setupRows[nSetupRows++] = SR_LOGIN;
  } else {
    setupRows[nSetupRows++] = SR_URL;
  }
  setupRows[nSetupRows++] = SR_EPG;
  setupRows[nSetupRows++] = SR_BUTTONS;
  setupRow = 0;
  for (int i = 0; i < nSetupRows; i++) if (setupRows[i] == was) setupRow = i;
  if (setupCol >= rowCols(setupRows[setupRow])) setupCol = rowCols(setupRows[setupRow]) - 1;
}

static int rowIndex(int row) {
  for (int i = 0; i < nSetupRows; i++) if (setupRows[i] == row) return i;
  return -1;
}

// "http://tv.example.net:8080" -> "tv.example.net" and "8080" for the two
// fields; https keeps its scheme, which is not the default.
static void splitServer(void) {
  char *h = draft.server, *c, *end;
  draftPort[0] = 0;
  if (!strncasecmp(h, "http://", 7)) memmove(h, h + 7, strlen(h + 7) + 1);
  { char *start = strstr(h, "://") ? strstr(h, "://") + 3 : h;
    c = strchr(start, ':');
    if (!c) return;
    for (end = c + 1; isdigit((unsigned char)*end); end++) {}
    if (end == c + 1 || end - c - 1 >= (long)sizeof draftPort || (*end && *end != '/')) return;
    memcpy(draftPort, c + 1, (size_t)(end - c - 1));
    draftPort[end - c - 1] = 0;
    memmove(c, end, strlen(end) + 1); }
}

// The draft as the loader takes it: the server whole again, only the chosen
// kind's fields.
static void draftSource(IptvSource *out) {
  *out = draft;
  if (draft.kind == IPTV_SRC_XTREAM) {
    const char *h = draft.server;
    while (*h == ' ') h++;
    snprintf(out->server, sizeof out->server, "%s%s%s%s", strstr(h, "://") ? "" : "http://", h,
             draftPort[0] ? ":" : "", draftPort);
    if (!h[0]) out->server[0] = 0;
    out->url[0] = 0;
  } else {
    out->server[0] = out->user[0] = out->pass[0] = 0;
  }
}

static int draftComplete(void) {
  if (draft.kind == IPTV_SRC_XTREAM) return draft.server[0] && draft.user[0] && draft.pass[0];
  return strstr(draft.url, "://") != NULL;
}

// Nothing changed since the source was saved.
static int draftIsSource(void) {
  IptvSource d;
  const IptvSource *s = iptv_source();
  draftSource(&d);
  return d.kind == s->kind && !strcmp(d.epg, s->epg) &&
         (d.kind == IPTV_SRC_XTREAM ? !strcmp(d.server, s->server) && !strcmp(d.user, s->user) &&
                                          !strcmp(d.pass, s->pass)
                                    : !strcmp(d.url, s->url));
}

static void openSetup(void) {
  draft = *iptv_source();
  if (draft.kind == IPTV_SRC_NONE) draft.kind = IPTV_SRC_M3U;
  splitServer();
  showPass = 0;
  mode = MODE_SETUP;
  editing = -1;
  phoneRetryAt = 0;
  iptv_try_forget();
  nSetupRows = 0;
  layoutSetup();
  // Onto the first field: the address is what this screen is for.
  setupRow = rowIndex(draft.kind == IPTV_SRC_XTREAM ? SR_SERVER : SR_URL);
  setupCol = 0;
  stopStream();
  full = 0;
}

static char *fieldText(int f, int *max) {
  switch (f) {
    case F_URL:    *max = (int)sizeof draft.url;    return draft.url;
    case F_SERVER: *max = (int)sizeof draft.server; return draft.server;
    case F_PORT:   *max = (int)sizeof draftPort;    return draftPort;
    case F_USER:   *max = (int)sizeof draft.user;   return draft.user;
    case F_PASS:   *max = (int)sizeof draft.pass;   return draft.pass;
    case F_EPG:    *max = (int)sizeof draft.epg;    return draft.epg;
    default: *max = 0; return NULL;
  }
}

static const char *fieldHint(int f) {
  switch (f) {
    case F_URL:    return "http://provider.example/playlist.m3u";
    case F_SERVER: return "tv.example.net";
    case F_PORT:   return "8080";
    case F_USER:   return "username";
    case F_PASS:   return "password";
    case F_EPG:    return "https://\xE2\x80\xA6/guide.xml.gz";
    default: return "";
  }
}

// --- geometry: Z1's, 1920x1080. Without a source there is no card, and the
// rest moves up into its place.
#define SP_COL_X     700.0f        // the "type it" column
#define SP_TOP       402.0f        // both columns' top
#define SP_LIFT      174.0f        // the card and its gap, when there is no card
static float spLift(void) { return configured() ? 0.0f : SP_LIFT; }
static float spRX(void) { return X0() + (SP_COL_X - 96.0f); }
static float spRW(void) { return L_RIGHT - spRX(); }
static int   spX(void) { return draft.kind == IPTV_SRC_XTREAM; }
// The label's top for each field row; the field sits 26 under it.
static float labelY(int row) {
  float t = SP_TOP - spLift();
  if (!spX()) return row == SR_URL ? t + 136.0f : t + 274.0f;
  return row == SR_SERVER ? t + 136.0f : row == SR_LOGIN ? t + 258.0f : t + 380.0f;
}
static float fieldH(void) { return spX() ? 76.0f : 92.0f; }
static GfxRect fieldRect(int f) {
  float x = spRX(), w = spRW(), h = fieldH();
  switch (f) {
    case F_URL:    return (GfxRect){ x, labelY(SR_URL) + 26.0f, w, h };
    case F_SERVER: return (GfxRect){ x, labelY(SR_SERVER) + 26.0f, w - 150.0f - 14.0f, h };
    case F_PORT:   return (GfxRect){ x + w - 150.0f, labelY(SR_SERVER) + 26.0f, 150.0f, h };
    case F_USER:   return (GfxRect){ x, labelY(SR_LOGIN) + 26.0f, (w - 14.0f) * 0.5f, h };
    case F_PASS:   return (GfxRect){ x + (w + 14.0f) * 0.5f, labelY(SR_LOGIN) + 26.0f, (w - 14.0f) * 0.5f, h };
    default:       return (GfxRect){ x, labelY(SR_EPG) + 26.0f, w, h };
  }
}
static float resultCY(void) { return labelY(SR_EPG) + 26.0f + fieldH() + 22.0f + 17.0f; }
static float buttonsY(void) { return resultCY() + 17.0f + 26.0f; }
static GfxRect kindRect(void) { return (GfxRect){ spRX(), SP_TOP - spLift() + 54.0f, 0.0f, 56.0f }; }

// --- the outcome under the fields ----------------------------------------------------
enum { RS_NONE, RS_BUSY, RS_OK, RS_WARN, RS_FAIL };
static int guidedChannels(void) {
  static const IptvList *countedFor;
  static int countedPg = -1, with;
  const IptvList *l = iptv_list();
  if (!l) return 0;
  if (l != countedFor || l->nPg != countedPg) {
    countedFor = l; countedPg = l->nPg; with = 0;
    for (int i = 0; i < l->nCh; i++) with += l->ch[i].nPg > 0;
  }
  return with;
}

// What the line says, how (RS_*), and which fields (1 << F_*) carry the red
// hairline. Z2's wording: what is wrong, never a status code alone.
static int setupResult(char *dst, size_t n, unsigned *bad) {
  int t = iptv_try_state(), x = spX();
  unsigned where = x ? (1u << F_SERVER) | (1u << F_PORT) : (1u << F_URL);
  const IptvList *l = iptv_list();
  *bad = 0;
  dst[0] = 0;
  if (t == IPTV_LOADING) {
    snprintf(dst, n, x ? "Signing in\xE2\x80\xA6" : "Fetching the playlist\xE2\x80\xA6");
    return RS_BUSY;
  }
  if (t == IPTV_FAILED) {
    int status = 0, kind;
    char why[160];
    kind = iptv_try_failure(&status, why, sizeof why);
    *bad = where;
    switch (kind) {
      case IPTV_FAIL_UNREACHABLE:
        snprintf(dst, n, "Couldn't reach it \xE2\x80\x94 check the address, or the TV's network connection"); break;
      case IPTV_FAIL_NOT_FOUND:
        snprintf(dst, n, "Nothing there \xE2\x80\x94 the address returned 404"); break;
      case IPTV_FAIL_REFUSED:
        snprintf(dst, n, "Refused \xE2\x80\x94 the server answered %d; the subscription may have lapsed", status); break;
      case IPTV_FAIL_NOT_PLAYLIST:
        snprintf(dst, n, "Not a playlist \xE2\x80\x94 the address sent a web page, usually the provider's sign-in page"); break;
      case IPTV_FAIL_GUIDE:
        snprintf(dst, n, "That's a TV guide, not a playlist \xE2\x80\x94 it goes in the extra guide field"); break;
      case IPTV_FAIL_LOGIN:
        snprintf(dst, n, "Sign-in rejected \xE2\x80\x94 check the username, password and port");
        *bad = (1u << F_USER) | (1u << F_PASS) | (1u << F_PORT); break;
      case IPTV_FAIL_EXPIRED:
        snprintf(dst, n, "This subscription has expired \xE2\x80\x94 renew it with your provider");
        *bad = 1u << F_USER; break;
      case IPTV_FAIL_ACCOUNT:
        snprintf(dst, n, "%s", why); *bad = 1u << F_USER; break;
      case IPTV_FAIL_TIMEOUT:
        snprintf(dst, n, "Timed out \xE2\x80\x94 nothing came back within %d s", SRC_CEILING_S); break;
      case IPTV_FAIL_PROVIDER:
        snprintf(dst, n, "The provider turned the app away (%d) \xE2\x80\x94 it may limit devices or connections", status); break;
      case IPTV_FAIL_SERVER:
        snprintf(dst, n, "The server had an error (%d) \xE2\x80\x94 try again in a while", status); *bad = 0; break;
      default:
        snprintf(dst, n, "%s", why[0] ? why : "That didn't load"); break;
    }
    return RS_FAIL;
  }
  // No try on this screen yet, or one that worked: what the current source has
  // — only while the form still shows that source, not a half-typed other one.
  if (!draftIsSource()) return RS_NONE;
  if (!configured() || !l) {
    if (configured() && iptv_state() == IPTV_FAILED) { snprintf(dst, n, "%s", iptv_status()); return RS_FAIL; }
    if (configured() && iptv_state() == IPTV_LOADING) { snprintf(dst, n, "Loading channels\xE2\x80\xA6"); return RS_BUSY; }
    return RS_NONE;
  }
  if (iptv_guide_state() == IPTV_LOADING) {
    snprintf(dst, n, "%d channels \xC2\xB7 matching them to the TV guide\xE2\x80\xA6", l->nCh);
    return RS_BUSY;
  }
  if (!guidedChannels()) {
    snprintf(dst, n, "%d channels, no guide data \xE2\x80\x94 add an extra TV guide above", l->nCh);
    *bad = 0;
    return RS_WARN;
  }
  snprintf(dst, n, "Reachable \xC2\xB7 %d channels, %d matched to the guide", l->nCh, guidedChannels());
  return RS_OK;
}

static void backToBrowse(void) {
  if (iptv_try_state() == IPTV_LOADING) iptv_try_cancel();
  iptv_try_forget();
  mode = MODE_BROWSE;
  viewMode = VIEW_LIST;
  zone = ZONE_BODY;
}

// A tried source that became the source: the list is somebody else's now.
static void freshList(void) {
  useAlt = 0;
  group = GROUP_ALL;
  focusName[0] = 0;
  tuned = -1; tunedName[0] = 0;
  nView = 0; fRow = 0;
}

static void saveSetup(void) {
  IptvSource d;
  if (!draftComplete()) {
    say(draft.kind == IPTV_SRC_XTREAM ? "Fill in the server, username and password"
                                      : "Enter the playlist's full address, starting with http");
    return;
  }
  ime_close();
  editing = -1;
  draftSource(&d);
  iptv_try_source(&d, SRC_CEILING_S);
}

// After a try that worked, the primary button takes the viewer to the channels.
static int primaryIsDone(void) { return iptv_try_state() == IPTV_READY && draftIsSource(); }

static void primary(void) {
  if (primaryIsDone()) { backToBrowse(); rebuildView(); return; }
  saveSetup();
}

static void editField(int f) {
  if (!ime_usable()) {
    say("This TV has no on-screen keyboard: put the address in iptv.txt in the app's data folder");
    return;
  }
  editing = f;
  ime_open(fieldRect(f));
}

static void setupEvent(const SDL_Event *e) {
  SDL_Keycode k;
  int row = setupRows[setupRow], cols = rowCols(row);
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
    if (iptv_try_state() == IPTV_LOADING) { iptv_try_cancel(); return; }
    if (configured()) { backToBrowse(); return; }
    wantsExit = 1;
    return;
  }
  if (ime_is_open()) {
    // OK with the keyboard up is "done with this field": on to the next stop.
    if (isOk(k)) {
      ime_close(); editing = -1;
      if (setupCol + 1 < cols && fieldAt(row, setupCol + 1) >= 0) setupCol++;
      else if (setupRow + 1 < nSetupRows) { setupRow++; setupCol = 0; }
    }
    return;
  }
  switch (k) {
    case SDLK_UP:
    case SDLK_DOWN:
      if (k == SDLK_UP ? setupRow > 0 : setupRow + 1 < nSetupRows) {
        setupRow += k == SDLK_UP ? -1 : 1;
        // Straight up or down: the stop under the one left, where there is one.
        if (setupCol >= rowCols(setupRows[setupRow])) setupCol = rowCols(setupRows[setupRow]) - 1;
      }
      break;
    case SDLK_LEFT:
      if (row == SR_KIND && draft.kind == IPTV_SRC_XTREAM) { draft.kind = IPTV_SRC_M3U; layoutSetup(); }
      else if (setupCol > 0) setupCol--;
      else requestMenu = 1;
      break;
    case SDLK_RIGHT:
      if (row == SR_KIND && draft.kind != IPTV_SRC_XTREAM) { draft.kind = IPTV_SRC_XTREAM; layoutSetup(); }
      else if (setupCol + 1 < cols) setupCol++;
      break;
    default:
      if (!isOk(k)) break;
      if (row == SR_REFRESH) iptv_reload();
      else if (row == SR_KIND) {
        draft.kind = draft.kind == IPTV_SRC_XTREAM ? IPTV_SRC_M3U : IPTV_SRC_XTREAM;
        layoutSetup();
      } else if (row == SR_BUTTONS) {
        if (setupCol == 1) backToBrowse();
        else primary();
      } else if (row == SR_LOGIN && setupCol == 2) {
        showPass = !showPass;
      } else if (fieldAt(row, setupCol) >= 0) {
        editField(fieldAt(row, setupCol));
      }
      break;
  }
}

// THE PHONE FORM (phonelink.h): open while the setup form is, closed the moment
// it is not. A form saved on the phone lands in the draft and is tried exactly
// as Save and load would, its outcome on the same line.
static int listFresh = 1;   // the browse state already belongs to the source
static void phoneStep(Uint32 now) {
  IptvSource s;
  if (mode != MODE_SETUP) { phonelink_close(); return; }
  // A tried source became the source: the list on the browse screen is its.
  if (iptv_try_state() == IPTV_LOADING) listFresh = 0;
  else if (iptv_try_state() == IPTV_READY && !listFresh) { freshList(); listFresh = 1; }
  // With no network address yet (the TV still joining the Wi-Fi), look again
  // every few seconds rather than every frame.
  if (phonelink_state() == PL_OFF && now >= phoneRetryAt) {
    phoneRetryAt = now + 3000u;
    phonelink_open(iptv_source());
  }
  if (!phonelink_take(&s)) return;
  if (ime_is_open()) ime_close();
  editing = -1;
  draft = s;
  splitServer();
  layoutSetup();
  setupRow = rowIndex(SR_BUTTONS);
  setupCol = 0;
  saveSetup();
}

// --- Pointer ---------------------------------------------------------------------------
static void pointHead(int b, int unused) { (void)unused; zone = ZONE_HEAD; headSel = b; }
static void pointChip(int g, int unused) { (void)unused; zone = ZONE_CHIPS; chooseGroup(g); }
static void pointSearch(int a, int b) { (void)a; (void)b; zone = ZONE_CHIPS; }
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
static void pointSetup(int i, int col) {
  if (ime_is_open() || i < 0) return;
  setupRow = i;
  setupCol = col < rowCols(setupRows[i]) ? col : 0;
}
static void pointKind(int k, int unused) {
  (void)unused;
  if (ime_is_open()) return;
  if ((k == 1) != (draft.kind == IPTV_SRC_XTREAM)) { draft.kind = k ? IPTV_SRC_XTREAM : IPTV_SRC_M3U; layoutSetup(); }
  setupRow = rowIndex(SR_KIND);
  setupCol = 0;
}
static void pointQuick(int r, int unused) { (void)unused; if (!gmOpen) { quickRow = r; quickZone = Q_LIST; } }
static void pointQuickPill(int a, int b) { (void)a; (void)b; if (!gmOpen) quickZone = Q_PILL; }
static void pointGroupMenu(int g, int unused) { (void)unused; if (gmOpen && g >= 0) gmCursor = g; }

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
  phonelink_close();
  if (searching) closeSearch();
  stopStream();
  full = 0; quickOpen = 0;
  if (ime_is_open()) ime_close();
  editing = -1;
}

void iptvui_background(void) {
  phonelink_close();
  if (playing || zapPending || bufWaiting) { stopStream(); full = 0; quickOpen = 0; }
}

void iptvui_shutdown(void) {
  iptvui_leave();
  free(view); view = NULL; nView = capView = 0;
  free(chipW); chipW = NULL; nChipW = 0;
  free(quickOpened); quickOpened = NULL; quickOpenedCap = 0;
  if (dashTex) { gfx_tex_forget(dashTex); glDeleteTextures(1, &dashTex); dashTex = 0; }
  iptv_shutdown();
}

int iptvui_wants_exit(void) { int v = wantsExit; wantsExit = 0; return v; }
int iptvui_requested_menu(void) { int v = requestMenu; requestMenu = 0; return v; }
int iptvui_fullscreen(void) { return full && tuned >= 0; }

// --- Events ---------------------------------------------------------------------------
// Whether a longer channel number starts with the digits typed so far. When
// none does, the number is complete and waiting LIVE_DIGITS_MS for another
// digit is only a delay: "7" on a list of 1-20 tunes at once, "1" waits for a
// possible "12". A full buffer is complete too.
static int digitsCanGrow(void) {
  const IptvList *l = iptv_list();
  size_t n = strlen(digits);
  char num[16];
  if (!l || n + 1 >= sizeof digits) return 0;
  for (int c = 0; c < l->nCh; c++) {
    snprintf(num, sizeof num, "%d", l->ch[c].number);
    if (strlen(num) > n && !strncmp(num, digits, n)) return 1;
  }
  return 0;
}

static void typeDigit(int d) {
  size_t n = strlen(digits);
  if (n + 1 < sizeof digits) { digits[n] = (char)('0' + d); digits[n + 1] = 0; }
  digitsAt = SDL_GetTicks();
  if (!digitsCanGrow()) digitsAt -= LIVE_DIGITS_MS;
}

static int viewIndex(int ch) {
  for (int r = 0; r < nView; r++) if (view[r] == ch) return r;
  return -1;
}

// The walk: the programmes of the tuned channel after the one on now.
static int walkNext(int from) {
  const IptvList *l = iptv_list();
  const IptvChannel *c = chan(tuned);
  long long now = nowSec();
  int p;
  if (!l || !c || !c->nPg) return -1;
  if (from < 0) p = iptv_programme_after(l, tuned, now);
  else p = from + 1 < c->firstPg + c->nPg ? from + 1 : -1;
  return p;
}
static int walkPrev(int from) {
  const IptvList *l = iptv_list();
  const IptvChannel *c = chan(tuned);
  if (!l || !c || from < 0) return -1;
  // Back past the first programme after now is now itself.
  return from - 1 >= c->firstPg && l->pg[from - 1].start > nowSec() ? from - 1 : -1;
}

static int reminderIndex(const char *name, long long start) {
  for (int i = 0; i < nReminders; i++)
    if (reminders[i].start == start && !strcmp(reminders[i].name, name)) return i;
  return -1;
}
static void toggleReminder(int ch, int pg) {
  const IptvList *l = iptv_list();
  const IptvChannel *c = chan(ch);
  int i;
  if (!c || !l || pg < 0) return;
  i = reminderIndex(c->name, l->pg[pg].start);
  if (i >= 0) {
    reminders[i] = reminders[--nReminders];
    say("Reminder removed");
  } else if (nReminders < REMIND_MAX) {
    snprintf(reminders[nReminders].name, sizeof reminders[0].name, "%s", c->name);
    snprintf(reminders[nReminders].title, sizeof reminders[0].title, "%s", l->pg[pg].title);
    reminders[nReminders].number = c->number;
    reminders[nReminders].start = l->pg[pg].start;
    nReminders++;
    say("We'll remind you when it starts");
  }
}

static void control(int ctl) {
  switch (ctl) {
    case CTL_GUIDE:
      // Out to the guide, on this channel; the picture follows into the preview.
      full = 0; ov = OV_NONE;
      viewMode = VIEW_GUIDE; zone = ZONE_BODY; fChan = 0;
      if (viewIndex(tuned) >= 0) fRow = viewIndex(tuned);
      rememberFocus();
      fTime = nowSec(); winStart = slotFloor(fTime);
      break;
    case CTL_CHANNELS:
      quickOpen = 1;
      quickFromBar = barCtl >= 0;
      quickZone = Q_LIST; gmOpen = 0;
      // Opens on the playing channel, as the episode panel opens on the
      // playing season: a group without it gives way to All channels.
      if (viewIndex(tuned) < 0) chooseGroup(GROUP_ALL);
      quickRow = viewIndex(tuned) >= 0 ? viewIndex(tuned) : 0;
      quickSnap = 1;
      break;
    case CTL_SUBS: case CTL_AUDIO:
      tracks_embedded_only(1);
      tracks_open_at(ctl == CTL_SUBS ? 1 : 0);
      break;
    case CTL_ASPECT:
      aspectMode = (aspectMode + 1) % 3;
      say(ASPECT_NAME[aspectMode]);
      break;
    case CTL_FAV: toggleFavourite(tuned); break;
    case CTL_PLAY: togglePause(); break;
    case CTL_RESTART: {
      const IptvProgramme *pg = restartable();
      if (pg) seekTo((double)pg->start);
      break;
    }
    case CTL_LIVE: goLive(); barCtl = CTL_PLAY; break;
    default: break;
  }
}

static void startPeek(int step) {
  int r = viewIndex(tuned);
  if (!nView) return;
  if (r < 0) r = 0;
  // From the tuned channel, one row on: the title visibly rolls.
  peekPos = r + step;
  peekScroll = (float)r;
  peekScrollV = 0.0f;
  peekRow = ((peekPos % nView) + nView) % nView;
  overlay(OV_PEEK);
}

static int gmVisible(void) { int n = nGroups(); return n < NV_EPL_SMENU_VIS ? n : NV_EPL_SMENU_VIS; }
static void gmKeep(void) {
  int vis = gmVisible();
  if (gmCursor < gmScroll) gmScroll = gmCursor;
  if (gmCursor >= gmScroll + vis) gmScroll = gmCursor - vis + 1;
  if (gmScroll < 0) gmScroll = 0;
}

// Another group in the panel: its list from the top, unless the playing channel
// is in it, where it lands on that one — episodes.c's courtesy for a season.
static void quickGroup(int g) {
  if (g < 0 || g >= nGroups() || g == group) return;
  chooseGroup(g);
  quickRow = viewIndex(tuned) >= 0 ? viewIndex(tuned) : 0;
  quickSnap = 1;
}

static void closeQuick(void) {
  quickOpen = 0; gmOpen = 0;
  // From the bar's Channels button, Back returns to the bar and its button.
  if (quickFromBar) overlay(OV_BAR); else ov = OV_NONE;
}

static void quickEvent(SDL_Keycode k) {
  if (gmOpen) {
    if (k == SDLK_UP && gmCursor > 0) gmCursor--;
    else if (k == SDLK_DOWN && gmCursor < nGroups() - 1) gmCursor++;
    else if (isOk(k)) { quickGroup(gmCursor); gmOpen = 0; quickZone = Q_LIST; }
    else if (isBack(k)) gmOpen = 0;
    gmKeep();
    return;
  }
  if (isBack(k)) { closeQuick(); return; }
  if (k == SDLK_LEFT)  { quickGroup(group - 1); return; }
  if (k == SDLK_RIGHT) { quickGroup(group + 1); return; }
  if (k == SDLK_UP) { if (quickZone == Q_LIST && quickRow > 0) quickRow--; else quickZone = Q_PILL; return; }
  if (k == SDLK_DOWN) { if (quickZone == Q_PILL) quickZone = Q_LIST; else if (quickRow + 1 < nView) quickRow++; return; }
  if (k == SDLK_PAGEUP)   { quickZone = Q_LIST; quickRow = quickRow > 6 ? quickRow - 6 : 0; return; }
  if (k == SDLK_PAGEDOWN) { quickZone = Q_LIST; quickRow = quickRow + 6 < nView ? quickRow + 6 : (nView ? nView - 1 : 0); return; }
  if (!isOk(k)) return;
  if (quickZone == Q_PILL) { gmOpen = 1; gmCursor = group; gmScroll = 0; gmKeep(); return; }
  if (quickRow < nView) {
    fRow = quickRow; rememberFocus();
    if (view[quickRow] != tuned) tune(view[quickRow], 1);
    quickOpen = 0; gmOpen = 0;
    overlay(OV_TOAST);
  }
}

// The remote's own transport keys, wherever the focus is.
static int mediaKey(SDL_Keycode k) {
  if (k == SDLK_AUDIOPLAY || k == SDLK_PAUSE || k == SDLK_AUDIOSTOP) {
    togglePause();
    overlay(OV_BAR);
    return 1;
  }
#if SDL_VERSION_ATLEAST(2, 0, 6)
  if (k == SDLK_AUDIOREWIND || k == SDLK_AUDIOFASTFORWARD) {
    int back = k == SDLK_AUDIOREWIND;
    if (back ? canRewind() : (scrubbing || !atLive())) scrub(back ? -1 : +1);
    overlay(OV_BAR);
    return 1;
  }
#endif
  return 0;
}

static void fullEvent(SDL_Keycode k) {
  int d = digitOf(k);
  int up = k == SDLK_UP || k == SDLK_PAGEDOWN, down = k == SDLK_DOWN || k == SDLK_PAGEUP;
  if (quickOpen) { quickEvent(k); return; }
  if (mediaKey(k)) return;
  if (d >= 0) { typeDigit(d); return; }

  switch (ov) {
    case OV_NONE: case OV_TOAST:
      // Channel hopping: ▲▼ (and CH+/CH-, CH+ being the next number) zap, and
      // the toast says where you landed. The retune waits for the keys to stop.
      if (up || down) { zap(down ? +1 : -1); overlay(OV_TOAST); }
      else if (isOk(k) || k == SDLK_i || k == SDLK_RIGHT) overlay(OV_BAR);
      else if (k == SDLK_LEFT) { barCtl = -1; control(CTL_CHANNELS); }
      else if (isBack(k)) {
        // Back to the screen; the picture carries on in the preview.
        full = 0; ov = OV_NONE;
        zone = ZONE_BODY; fChan = 0;
        if (viewIndex(tuned) >= 0) fRow = viewIndex(tuned);
        rememberFocus();
        fTime = nowSec();
        keepWindowOnFocus();
      }
      return;
    case OV_BAR:
      if (barCtl >= 0) {
        if (k == SDLK_LEFT) barStep(-1);
        else if (k == SDLK_RIGHT) barStep(+1);
        else if (isOk(k)) control(barCtl);
        else if (k == SDLK_UP || isBack(k)) barCtl = -1;
        overlay(OV_BAR);
        if (ov == OV_BAR && barCtl < 0 && isBack(k)) barCtl = -1;
        return;
      }
      // Rewound, ◀▶ move through time; at live, ◀ rewinds when there is a
      // past to rewind into and ▶ walks the schedule, the future being the
      // only direction left.
      if (scrubbing && isOk(k)) { seekTo(scrubAt); overlay(OV_BAR); }
      else if (scrubbing && isBack(k)) { scrubbing = 0; overlay(OV_BAR); }
      else if (up || down) { scrubbing = 0; startPeek(down ? +1 : -1); }
      else if (k == SDLK_RIGHT) {
        if (scrubbing || !atLive()) { scrub(+1); overlay(OV_BAR); }
        else {
          int p = walkNext(-1);
          if (p >= 0) { overlay(OV_WALK); walkPg = p; } else overlay(OV_BAR);
        }
      }
      else if (k == SDLK_LEFT) {
        if (scrubbing || !atLive() || canRewind()) scrub(-1);
        overlay(OV_BAR);
      }
      else if (isOk(k) || k == SDLK_DOWN) { barCtl = shownFirst(); overlay(OV_BAR); }
      else if (isBack(k)) ov = OV_NONE;
      return;
    case OV_PEEK:
      if (up || down) {
        peekPos += down ? 1 : -1;
        peekRow = ((peekPos % nView) + nView) % nView;
      }
      else if (isOk(k) && peekRow < nView) {
        // The one press that retunes.
        fRow = peekRow; rememberFocus();
        tune(view[peekRow], 1);
        overlay(OV_BAR);
        return;
      }
      else if (isBack(k)) { overlay(OV_BAR); return; }
      overlay(OV_PEEK);
      return;
    case OV_WALK:
      if (k == SDLK_RIGHT) { int p = walkNext(walkPg); if (p >= 0) walkPg = p; overlay(OV_WALK); walkPg = p >= 0 ? p : walkPg; }
      else if (k == SDLK_LEFT) {
        int p = walkPrev(walkPg);
        if (p >= 0) { overlay(OV_WALK); walkPg = p; } else overlay(OV_BAR);
      }
      else if (isOk(k)) { int p = walkPg; toggleReminder(tuned, p); overlay(OV_WALK); walkPg = p; }
      else if (up || down) startPeek(down ? +1 : -1);
      else if (isBack(k)) overlay(OV_BAR);
      return;
    default: return;
  }
}

// The field, where the chips stand.
static GfxRect searchField(void) { return (GfxRect){ X0(), L_CHIPS_Y, 760.0f, L_CHIP_H }; }

static void raiseKeyboard(void) {
  if (!ime_usable()) { say("This TV has no on-screen keyboard"); return; }
  if (!ime_is_open()) ime_open(searchField());
}

static void openSearch(void) {
  searching = 1;
  query[0] = 0; queryLen = 0;
  zone = ZONE_CHIPS;
  focusName[0] = 0;
  rebuildView();
  scrollRows = 0.0f; scrollRowsV = 0.0f;
  fChan = 0;
  raiseKeyboard();
}

static void closeSearch(void) {
  if (ime_is_open()) ime_close();
  searching = 0;
  query[0] = 0; queryLen = 0;
  rememberFocus();
  rebuildView();
}

// Keys while the field has the focus. The keyboard's own text comes through
// ime_edit in iptvui_event, before this.
static void searchEvent(SDL_Keycode k) {
  if (ime_is_open()) {
    // OK is "done typing": to the results, as the setup form's fields go on.
    if (isOk(k)) {
      ime_close();
      if (nView) { zone = ZONE_BODY; fRow = 0; rememberFocus(); fTime = nowSec(); keepWindowOnFocus(); }
      return;
    }
    // Arrows with the keyboard not on screen close it and move as usual.
    if (ime_shown() || !(k == SDLK_UP || k == SDLK_DOWN || k == SDLK_LEFT || k == SDLK_RIGHT)) return;
    ime_close();
  }
  if (isOk(k)) raiseKeyboard();
  else if (k == SDLK_UP) zone = ZONE_HEAD;
  else if (k == SDLK_DOWN && nView) {
    zone = ZONE_BODY;
    fTime = nowSec();
    keepWindowOnFocus();
  }
  else if (k == SDLK_LEFT) requestMenu = 1;
}

static void headEvent(SDL_Keycode k) {
  if (k == SDLK_LEFT) { if (headSel > 0) headSel--; else requestMenu = 1; }
  else if (k == SDLK_RIGHT) { if (headSel + 1 < HEAD_N) headSel++; }
  else if (k == SDLK_DOWN) zone = ZONE_CHIPS;
  else if (isOk(k)) {
    if (headSel == HEAD_SOURCE) openSetup();
    else if (headSel == HEAD_SEARCH) { if (searching) { zone = ZONE_CHIPS; raiseKeyboard(); } else openSearch(); }
    else {
      // The same header and chips either way: switching changes only the body.
      int want = headSel == HEAD_GUIDE ? VIEW_GUIDE : VIEW_LIST;
      if (want == viewMode) return;
      viewMode = want;
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
  guideSettle();
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
    if (tap) {
      long long cs, ce;
      int ch = focusedChannel();
      // A programme in the past plays from the archive, from its start.
      if (viewMode == VIEW_GUIDE && !fChan && fTime < nowSec() - 90 &&
          cellAt(ch, fTime, &cs, &ce) >= 0 && iptv_has_archive(ch, cs))
        watchFrom(ch, cs);
      else watch(ch);
    }
    return;
  }
  // The search field's text, from the TV's keyboard.
  if (searching && ime_is_open() && ime_edit(e, query, &queryLen, (int)sizeof query)) {
    focusName[0] = 0;
    rebuildView();
    scrollRows = 0.0f; scrollRowsV = 0.0f;
    return;
  }
  if (e->type != SDL_KEYDOWN) return;
  k = e->key.keysym.sym;

  if (isBack(k)) {
    // Back climbs, one step at a time: the actions to their row; the guide to
    // the list, which is the landing; a playing preview stops; then the chips;
    // then out. Searching, the field is where the chips were: Back there
    // lowers the keyboard, then ends the search.
    if (searching && ime_is_open()) { ime_close(); return; }
    if (searching && zone == ZONE_CHIPS) { closeSearch(); return; }
    if (zone == ZONE_ACTIONS) { zone = ZONE_BODY; return; }
    if (zone == ZONE_HEAD) { zone = ZONE_CHIPS; return; }
    if (zone == ZONE_BODY && viewMode == VIEW_GUIDE) { viewMode = VIEW_LIST; fChan = 0; return; }
    if (zone == ZONE_BODY && ownsVideo()) { stopStream(); tuned = -1; tunedName[0] = 0; return; }
    if (zone == ZONE_BODY) { zone = ZONE_CHIPS; return; }
    wantsExit = 1;
    return;
  }
  if (zone == ZONE_HEAD) { headEvent(k); return; }
  if (zone == ZONE_CHIPS) { if (searching) searchEvent(k); else chipsEvent(k); return; }
  if (zone == ZONE_ACTIONS) { actionsEvent(k); return; }

  { int d = digitOf(k); if (d >= 0) { typeDigit(d); return; } }
  switch (k) {
    case SDLK_UP:
      if (fRow > 0) { fRow--; rememberFocus(); guideSettle(); }
      else zone = ZONE_CHIPS;
      break;
    case SDLK_DOWN: if (fRow + 1 < nView) { fRow++; rememberFocus(); guideSettle(); } break;
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

// The panel's layout, episodes.c's: a closed row's pitch, where the text starts
// (further in on the focused row, whose plate has grown), the list's window.
#define Q_PITCH      (NV_EPL_ROW_H + NV_EPL_ROW_GAP)
#define Q_VIEW_H     (NV_EPL_VIEW_BOTTOM - NV_EPL_VIEW_TOP)
#define Q_TEXT_X(f)  (NV_EPL_PADX + NV_EPL_THUMB_W * (1.0f + (NV_EPL_THUMB_GROW - 1.0f) * (f)) \
                      + NV_EPL_TEXT_GAP + NV_EPL_TEXT_GROW * (f))
#define Q_TEXT_W(f)  (NV_EPL_W - NV_EPL_PADX - Q_TEXT_X(f))

static const IptvProgramme *quickProgramme(int row) {
  return row >= 0 && row < nView ? programmeAt(view[row], nowMinute()) : NULL;
}
static int quickSynLines(const IptvProgramme *pg) {
  int n;
  if (!pg || !pg->desc[0]) return 0;
  n = txt_block_lines(TXT_TRK_OPTSUB, pg->desc, Q_TEXT_W(1.0f));
  return n > NV_EPL_SYN_LINES ? NV_EPL_SYN_LINES : n;
}
static float quickFullBlock(const IptvProgramme *pg) {
  int n = quickSynLines(pg);
  return n > 0 ? NV_EPL_SYN_DY + n * NV_EPL_SYN_LD - (NV_EPL_SYN_LD - 22.0f) : NV_EPL_BLOCK_H;
}
static float quickOpenExtra(const IptvProgramme *pg) {
  float h = quickFullBlock(pg) + NV_EPL_PADY * 2 - NV_EPL_ROW_H;
  return h > 0.0f ? h : 0.0f;
}

static void quickUpdate(float dt) {
  quickA = anim_spring(quickA, quickOpen ? 1.0f : 0.0f, dt, NV_SPRING_SCREEN);
  if (!quickOpen && quickA < 0.005f) return;
  if (quickOpenedCap < nView) {
    float *o = realloc(quickOpened, (size_t)nView * sizeof *o);
    if (!o) return;
    for (int i = quickOpenedCap; i < nView; i++) o[i] = 0.0f;
    quickOpened = o; quickOpenedCap = nView;
  }
  if (quickRow >= nView) quickRow = nView ? nView - 1 : 0;
  // Only the focused row opens, and none while the pill has the cursor.
  for (int i = 0; i < nView; i++) {
    float want = i == quickRow && quickZone == Q_LIST ? 1.0f : 0.0f;
    if (quickSnap) quickOpened[i] = want;
    else if (quickOpened[i] > 0.0f || want > 0.0f)
      quickOpened[i] = anim_spring(quickOpened[i], want, dt, NV_SPRING_GRID);
  }
  // The focused row held in the window's middle, whole rows at the top, the
  // list stopping at both ends — episodes.c's offset, measured where the rows
  // are heading.
  { float fh = NV_EPL_ROW_H + quickOpenExtra(quickProgramme(quickRow));
    float total = nView * Q_PITCH - NV_EPL_ROW_GAP + (fh - NV_EPL_ROW_H);
    float max = total - Q_VIEW_H;
    float target = quickRow * Q_PITCH - (Q_VIEW_H - fh) * 0.5f;
    target = roundf(target / Q_PITCH) * Q_PITCH;
    if (target > max) target = max;
    if (target < 0) target = 0;
    quickScroll = quickSnap ? target : anim_spring(quickScroll, target, dt, NV_SPRING_GRID); }
  if (nView) quickSnap = 0;
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
  phoneStep(now);
  if (mode != MODE_BROWSE) return;

  hold_animate(&hold, dt, now);
  if (hold_fired(&hold, now) && zone == ZONE_BODY) toggleFavourite(focusedChannel());

  // Time moves on under a screen left open — unless the focus is on a past
  // programme, which catch-up made a place to be.
  if (fTime < t && fTime >= t - 90) fTime = t;
  if (fTime >= t - 90 && winStart < slotFloor(t)) winStart = slotFloor(t);

  if (zapPending && now >= zapAt) startStream();
  // The recorder's first bytes, or not: the pipeline follows it, or goes direct.
  if (bufWaiting) {
    int st = timeshift_state();
    double lo, hi;
    char buf[256];
    const char *u;
    if (st == TS_RECORDING && timeshift_range(&lo, &hi) &&
        (u = timeshift_url(hi - LIVE_BUF_LEAD_S > lo ? hi - LIVE_BUF_LEAD_S : lo, buf, sizeof buf))) {
      bufWaiting = 0;
      playUrl(u, SRC_BUFFER, hi - LIVE_BUF_LEAD_S > lo ? hi - LIVE_BUF_LEAD_S : lo);
    } else if (st == TS_REFUSED || st == TS_FAILED || now >= bufWaitUntil) {
      printf("[live] no pause buffer for %s (%s)\n", tunedName,
             st == TS_REFUSED ? "not MPEG-TS" : st == TS_FAILED ? "recorder failed" : "no bytes yet");
      bufWaiting = 0;
      timeshift_end();
      playDirect();
    }
  }
  if (ownsVideo() && !srcReadyAt && video_ready()) srcReadyAt = now ? now : 1;
  if (scrubbing && now >= scrubCommitAt) seekTo(scrubAt);
  // A programme from the archive ends: on with what follows, or live.
  if (ownsVideo() && srcKind == SRC_ARCHIVE && video_eos_count() > eosSeen) {
    eosSeen = video_eos_count();
    seekTo(playAt() + 1.0);
  }
  if (ownsVideo() && video_error_count() > errSeen) {
    char why[160] = "";
    video_last_error(why, sizeof why);
    printf("[live] stream error on %s (%s): %s\n", tunedName,
           srcKind == SRC_ARCHIVE ? "catch-up" : srcKind == SRC_BUFFER ? "buffer" : "live",
           why[0] ? why : "?");
    fflush(stdout);
    errSeen = video_error_count();
    if (srcKind == SRC_ARCHIVE) {
      // The archive refused: back to what is on, rather than a dead picture.
      say("Catch-up isn't available for this programme");
      beginLive();
    } else if (srcKind == SRC_BUFFER) {
      timeshift_end();
      playDirect();
    } else if (!tryOther(why[0] ? why : "stream error")) {
      failStream(why[0] ? why : "the TV reported an error");
      // Full screen, the failure is said in the bar, where the channel is named.
      if (full && ov != OV_PEEK && ov != OV_WALK) overlay(OV_BAR);
    }
  }
  // No error and no picture either: a live stream that never starts.
  if (ownsVideo() && srcKind == SRC_LIVE && !zapPending && !playFailed && !paused &&
      !video_ready() && now - streamAt >= LIVE_START_MS && !tryOther("no picture after 15 s"))
    failStream("no picture after 15 seconds");
  // It plays: if that took the other container, start there from now on.
  if (ownsVideo() && srcKind == SRC_LIVE && video_ready() && usingAlt != useAlt) {
    useAlt = usingAlt;
    printf("[live] this source plays as %s; using that from now on\n", useAlt ? "the other container" : "listed");
    fflush(stdout);
  }
  // Preview on focus: resting on a channel while browsing plays it in the
  // preview. It is not "watched" until OK.
  if (!full && zone == ZONE_BODY && settings_live_preview()) {
    int f = focusedChannel();
    if (f != restCh) { restCh = f; restSince = now; }
    else if (f >= 0 && f != tuned && now - restSince >= LIVE_PREVIEW_MS && !zapPending) {
      previewTune(f);
    }
  } else {
    restCh = -1;
  }
  if (digits[0] && now - digitsAt >= LIVE_DIGITS_MS) {
    int n = atoi(digits);
    digits[0] = 0;
    if (n > 0) {
      tuneNumber(n);
      if (!full && tuned >= 0) enterFull();
      else if (full) overlay(OV_TOAST);
    }
  }

  // The overlay's clock. The bar stays while a control holds the focus, or
  // while the tracks sheet it opened is up.
  if (barCtl >= 0 && shownIndex(barCtl) < 0) barCtl = CTL_PLAY;
  // Paused or choosing an instant, the bar stays: it is what says so.
  if (full && ov != OV_NONE && now >= ovUntil && !(ov == OV_BAR && barCtl >= 0) && !tracks_is_open() &&
      !paused && !scrubbing)
    ov = OV_NONE;
  if (full && (paused || scrubbing) && ov == OV_NONE) overlay(OV_BAR);
  if (!full) ov = OV_NONE;
  // ANY SHEET OVER THE PICTURE HIDES THE BAR, as the film player hides its
  // transport under its sheets: subtitles, audio, the channels panel. When the
  // sheet goes the bar comes back with its clock restarted and its focus where
  // it was, so Back from Subtitles lands on the Subtitles button.
  { int sheet = quickOpen || tracks_is_open();
    if (sheetWasUp && !sheet && full && ov == OV_BAR) overlay(OV_BAR);
    sheetWasUp = sheet;
    toastA = anim_spring(toastA, full && ov == OV_TOAST && !sheet ? 1.0f : 0.0f, dt, NV_SPRING_FOCUS);
    barA = anim_spring(barA, full && ov >= OV_BAR && !sheet ? 1.0f : 0.0f, dt, NV_SPRING_FOCUS); }
  peekScroll = anim_spring2_reduced(&peekScrollV, peekScroll, (float)peekPos, dt, NV_SPRING2_PAGE, reduced);
  for (int i = 0; i < CTL_N; i++) {
    float target = (full && ov == OV_BAR && barCtl == i && shownIndex(i) >= 0) ? 1.0f : 0.0f;
    ctlFocus[i] = anim_spring(ctlFocus[i], target, dt, target > ctlFocus[i] ? NV_SPRING_FOCUS : NV_SPRING_BLUR);
  }

  // Reminders due: said once, within ten minutes of the start.
  for (int i = 0; i < nReminders; i++) {
    if (t < reminders[i].start) continue;
    if (t < reminders[i].start + 600) {
      char m[300];
      snprintf(m, sizeof m, "Starting now on %d \xC2\xB7 %s", reminders[i].number, reminders[i].title);
      say(m);
    }
    reminders[i--] = reminders[--nReminders];
  }

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

  quickUpdate(dt);
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
  // A DARK MARK ON A LIGHT PLATE. Many providers' logos are black lettering on
  // a transparent ground, made for a white page; on the dark plate they all but
  // vanish. Those (tex_brand_dark: dark and colourless where solid, with a
  // transparent margin, so not an opaque picture) sit on a light plate instead,
  // their own colours untouched.
  if (c->logo[0] && tex_brand_dark(c->logo)) {
    float vis[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    if (tex_content_box(c->logo, vis) &&
        (vis[0] > 0.01f || vis[1] > 0.01f || vis[2] < 0.99f || vis[3] < 0.99f))
      plate = 0xE4E7EA;
  }
  gfx_color(r, radius / r.h, HEXF(plate), a);
  if (c->logo[0] && !tex_failed(c->logo)) {
    GLuint tex = tex_get_width(c->logo, r.w - 18.0f);
    if (tex) {
      // THE MARK, NOT THE FILE. IPTV logos come with any amount of transparent
      // margin — one provider's are padded to a square, the next one's are
      // cropped to the letters — so the same box drew some marks at a third of
      // the size of others. The visible part (tex_content_box) is what gets
      // fitted, through the texture cell, with air in proportion to the tile.
      float air = r.h * 0.14f < 9.0f ? 9.0f : r.h * 0.14f;
      GfxRect box = { r.x + air, r.y + air, r.w - air * 2.0f, r.h - air * 2.0f };
      float asp = tex_aspect(c->logo), w, h, vis[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
      if (asp <= 0.0f) asp = 1.0f;
      if (!tex_content_box(c->logo, vis) || vis[2] - vis[0] < 0.02f || vis[3] - vis[1] < 0.02f) {
        vis[0] = vis[1] = 0.0f; vis[2] = vis[3] = 1.0f;
      }
      asp *= (vis[2] - vis[0]) / (vis[3] - vis[1]);
      if (asp > box.w / box.h) { w = box.w; h = box.w / asp; } else { h = box.h; w = box.h * asp; }
      gfx_opacity_group = a;
      // The rectangle already has the mark's shape. The shader's own crop reads
      // a global the last art drawn left behind (the menu's avatar, a poster),
      // and a logo cropped to THAT shape is the squashed look: say "as is".
      gfx_tex_aspect_current = 0.0f;
      gfx_tex_cell_current = (GfxRect){ vis[0], vis[1], vis[2] - vis[0], vis[3] - vis[1] };
      gfx_texture((GfxRect){ box.x + (box.w - w) * 0.5f, box.y + (box.h - h) * 0.5f, w, h }, tex);
      gfx_tex_cell_current = (GfxRect){ 0.0f, 0.0f, 1.0f, 1.0f };
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

// THE HEADER'S RIGHT: the search circle, the Guide | List switch, and the
// source pill ("IPTV · 49 channels"), which opens the source screen. Focus is a
// ring; the view on show is the switch's white segment whether focused or not.
#define HD_RING   3.0f
static void headRing(GfxRect r) {
  gfx_rect((GfxRect){ r.x - HD_RING - 2.0f, r.y - HD_RING - 2.0f, r.w + (HD_RING + 2.0f) * 2.0f,
                      r.h + (HD_RING + 2.0f) * 2.0f },
           0, GFX_RING_INSET, 0, HD_RING / (r.h + (HD_RING + 2.0f) * 2.0f), 0, 0.5f, 1, 1, 1, 0.92f);
}

static GfxRect headSearch(float right, int focused) {
  GfxRect r = { right - L_BTN_H, L_BTN_Y, L_BTN_H, L_BTN_H };
  float d = 20.0f;
  gfx_color(r, 0.5f, 1.0f, 1.0f, 1.0f, focused ? 0.16f : 0.07f);
  if (focused) headRing(r);
  gfx_icon((GfxRect){ r.x + (r.w - d) * 0.5f, r.y + (r.h - d) * 0.5f, d, d }, "search_glass",
           HEXF(focused ? 0xF5F6F8 : 0xC1C7CD), 1.0f);
  pointer_zone(r.x, r.y, r.w, r.h, pointHead, HEAD_SEARCH, 0);
  return r;
}

// One segment of the switch; `active` is the view on show.
static float segWidth(const char *label, int active) {
  return 18.0f + 18.0f + 10.0f + txt_width(active ? TXT_LIVE_META_B : TXT_SRC_TEXT, label) + 20.0f;
}
static void headSegment(GfxRect r, const char *icon, const char *label, int active, int focused, int idx) {
  unsigned ink = active ? C_INK : focused ? 0xF5F6F8 : 0x9AA1A9;
  TxtLine t = txt_line(active ? TXT_LIVE_META_B : TXT_SRC_TEXT, label, HEXI(ink), 255);
  if (active) gfx_color(r, 0.5f, HEXF(C_PAPER), 1.0f);
  else if (focused) gfx_color(r, 0.5f, 1.0f, 1.0f, 1.0f, 0.14f);
  if (focused) headRing(r);
  gfx_icon((GfxRect){ r.x + 18.0f, r.y + (r.h - 18.0f) * 0.5f, 18.0f, 18.0f }, icon, HEXF(ink), 1.0f);
  txt_draw(t, r.x + 18.0f + 18.0f + 10.0f, r.y + (r.h - t.h) * 0.5f);
  pointer_zone(r.x, r.y, r.w, r.h, pointHead, idx, 0);
}

static GfxRect headSwitch(float right) {
  const float pad = 5.0f;
  int guide = viewMode == VIEW_GUIDE;
  float wg = segWidth("Guide", guide), wl = segWidth("List", !guide);
  GfxRect box = { right - (pad + wg + wl + pad), L_BTN_Y, pad + wg + wl + pad, L_BTN_H };
  gfx_color(box, 0.5f, 1.0f, 1.0f, 1.0f, 0.07f);
  headSegment((GfxRect){ box.x + pad, box.y + pad, wg, box.h - pad * 2.0f }, "live_grid", "Guide", guide,
              zone == ZONE_HEAD && headSel == HEAD_GUIDE, HEAD_GUIDE);
  headSegment((GfxRect){ box.x + pad + wg, box.y + pad, wl, box.h - pad * 2.0f }, "live_list", "List", !guide,
              zone == ZONE_HEAD && headSel == HEAD_LIST, HEAD_LIST);
  return box;
}

// The source and what it holds; while it loads, what it is doing instead.
static GfxRect headSource(float right, float maxW, int focused) {
  const IptvList *l = iptv_list();
  const char *st = iptv_status();
  char more[200];
  TxtLine name = txt_line(TXT_LIVE_META_B, iptv_source_label(), HEXI(0xF5F6F8), 255), dot, rest;
  float w, restMax;
  if (st[0]) snprintf(more, sizeof more, "%s", st);
  else if (l) snprintf(more, sizeof more, "%d channels", l->nCh);
  else more[0] = 0;
  dot = txt_line(TXT_SRC_TEXT, "\xC2\xB7", HEXI(0x5C636B), 255);
  restMax = maxW - (22.0f + 18.0f + 12.0f + name.w + 12.0f + dot.w + 12.0f + 24.0f);
  if (restMax < 60.0f) restMax = 60.0f;
  rest = txt_line_trim(TXT_SRC_TEXT, more, HEXI(0x9AA1A9), 255, restMax);
  w = 22.0f + 18.0f + 12.0f + name.w + (more[0] ? 12.0f + dot.w + 12.0f + rest.w : 0.0f) + 24.0f;
  { GfxRect r = { right - w, L_BTN_Y, w, L_BTN_H };
    float x = r.x + 22.0f, cy = r.y + r.h * 0.5f;
    gfx_color(r, 0.5f, 1.0f, 1.0f, 1.0f, focused ? 0.16f : 0.07f);
    if (focused) headRing(r);
    gfx_icon((GfxRect){ x, cy - 9.0f, 18.0f, 18.0f }, "live_source", HEXF(0xE4E7EA), 1.0f);
    x += 18.0f + 12.0f;
    txt_draw(name, x, cy - name.h * 0.5f);
    x += name.w + 12.0f;
    if (more[0]) {
      txt_draw(dot, x, cy - dot.h * 0.5f);
      x += dot.w + 12.0f;
      txt_draw(rest, x, cy - rest.h * 0.5f);
    }
    pointer_zone(r.x, r.y, r.w, r.h, pointHead, HEAD_SOURCE, 0);
    return r; }
}

// "Search" over the circle while it has the focus: the detail page's tooltip
// (detail_tooltip), with its fade. An icon alone says less than a word, and
// the word is only needed while the icon is the one chosen.
static void headTooltip(GfxRect btn, int shown) {
  static float a;
  static Uint32 last;
  Uint32 t = SDL_GetTicks();
  float dt = last ? (float)(t - last) / 1000.0f : 1.0f;
  last = t;
  a = anim_ramp(a, shown ? 1.0f : 0.0f, dt, NV_DETWEB_TIP_MS);
  detail_tooltip(btn, "Search", a, 1.0f);
}

static void drawHeader(void) {
  float x = X0(), right = L_RIGHT;
  GfxRect search;
  { TxtLine t = txt_line(TXT_TITLE3, "Live TV", 245, 246, 248, 255);
    txt_draw(t, x, L_BTN_Y + L_BTN_H * 0.5f - t.h * 0.5f);
    x += t.w + 60.0f; }
  // Laid out right to left; the source pill gives up width to a long status.
  { GfxRect sw;
    float switchW = 5.0f * 2.0f + segWidth("Guide", viewMode == VIEW_GUIDE) + segWidth("List", viewMode != VIEW_GUIDE);
    GfxRect r = headSource(right, right - x - switchW - 24.0f - L_BTN_H - 24.0f,
                           zone == ZONE_HEAD && headSel == HEAD_SOURCE);
    right = r.x - 24.0f;
    sw = headSwitch(right);
    right = sw.x - 24.0f; }
  search = headSearch(right, zone == ZONE_HEAD && headSel == HEAD_SEARCH);
  headTooltip(search, zone == ZONE_HEAD && headSel == HEAD_SEARCH && mode != MODE_SETUP);
}

// The search field, in the chips' place: the glass, what is typed (or what can
// be), a caret while the keyboard is up, and how many channels match.
static void drawSearchField(void) {
  GfxRect f = searchField();
  int focused = zone == ZONE_CHIPS, typing = ime_is_open();
  char count[48];
  if (focused) gfx_color((GfxRect){ f.x - 3.0f, f.y - 3.0f, f.w + 6.0f, f.h + 6.0f }, 0.5f, 1, 1, 1, 0.30f);
  gfx_color(f, 0.5f, 1, 1, 1, focused ? 0.14f : 0.07f);
  gfx_icon((GfxRect){ f.x + 20.0f, f.y + (f.h - 20.0f) * 0.5f, 20.0f, 20.0f }, "search_glass", HEXF(0xC1C7CD), 1.0f);
  { float tx = f.x + 54.0f, maxW = f.w - 54.0f - 24.0f;
    if (queryLen) {
      TxtLine t = txt_line(TXT_LIVE_META_B, query, HEXI(0xF5F6F8), 255);
      const char *q = query;
      // The end is what is being typed: trim from the left.
      while (t.w > maxW && *q) {
        q++;
        while (((unsigned char)*q & 0xC0) == 0x80) q++;
        t = txt_line(TXT_LIVE_META_B, q, HEXI(0xF5F6F8), 255);
      }
      txt_draw(t, tx, f.y + (f.h - t.h) * 0.5f);
      tx += t.w;
    } else {
      inkMid(TXT_LIVE_META, "Channels and programmes", 0x8A9199, tx, f.y + f.h * 0.5f, maxW, 1.0f);
    }
    if (typing && (SDL_GetTicks() / 530u) % 2u == 0u)
      gfx_color((GfxRect){ tx + 3.0f, f.y + 12.0f, 2.0f, f.h - 24.0f }, 0.0f, 1, 1, 1, 1); }
  if (queryLen) snprintf(count, sizeof count, nView == 1 ? "1 match" : "%d matches", nView);
  else snprintf(count, sizeof count, "Type to search \xC2\xB7 Back to close");
  inkMid(TXT_LIVE_META, count, 0x7C838B, f.x + f.w + 24.0f, f.y + f.h * 0.5f, L_RIGHT - f.x - f.w - 24.0f, 1.0f);
  pointer_zone(f.x, f.y, f.w, f.h, pointSearch, 0, 0);
}

static void drawChips(void) {
  float x = X0() - chipScroll, y = L_CHIPS_Y;
  int n = nGroups();
  if (searching) { drawSearchField(); return; }
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
  // Full screen takes the Aspect control's zoom: the frame is enlarged about its
  // centre and the slice that still falls on screen is sent as the source
  // rectangle, the way the film player crops (applyAspect in player.c).
  float zoom = (radius <= 0.0f && full) ? ASPECT_ZOOM[aspectMode] : 1.0f;
  if (memcmp(w, lastWin, sizeof w) || zoom != lastZoom) {
    float qw = (float)video_width(), qh = (float)video_height();
    if (zoom > 1.0f && qw > 2.0f && qh > 2.0f) {
      float vw = r.w * zoom, vh = r.h * zoom, vx = r.x + (r.w - vw) * 0.5f, vy = r.y + (r.h - vh) * 0.5f;
      int sx = (int)((r.x - vx) / vw * qw + 0.5f) & ~1, sy = (int)((r.y - vy) / vh * qh + 0.5f) & ~1;
      int sw = (int)(r.w / vw * qw + 0.5f) & ~1, sh = (int)(r.h / vh * qh + 0.5f) & ~1;
      video_window_source(sx, sy, sw, sh, w[0], w[1], w[2], w[3]);
    } else {
      video_window(w[0], w[1], w[2], w[3]);
    }
    // Until the stream reports its size a zoom cannot be applied; try again then.
    lastZoom = (zoom > 1.0f && (qw <= 2.0f || qh <= 2.0f)) ? -1.0f : zoom;
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
  else if (searching) { l1 = "Nothing matches"; l2 = "Try part of a channel's name, its number, or a programme's title."; }
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
    else if (pg && pg->stop <= now && iptv_has_archive(ch, pg->start)) {
      // CATCH-UP, outlined: this ended programme plays from the archive.
      float tw = tagWidth("CATCH-UP") + 22.0f;
      gfx_color((GfxRect){ kx, y, tw, 32.0f }, 8.0f / 32.0f, 1, 1, 1, 0.28f);
      gfx_color((GfxRect){ kx + 1.0f, y + 1.0f, tw - 2.0f, 30.0f }, 7.0f / 30.0f, 10 / 255.0f, 11 / 255.0f, 14 / 255.0f, 1.0f);
      tagText("CATCH-UP", 0xC1C7CD, kx + 11.0f, y + 16.0f, 1.0f);
      kx += tw + 14.0f;
    }
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
  // Ended, but in the archive: still watchable, so not greyed like the rest.
  int kept = past && iptv_has_archive(pg->channel, pg->start);
  float ar, ag, ab;
  unsigned titleHex = focused ? C_INK : kept ? 0xC1C7CD : past ? 0x8A9199 : airing ? 0xE4E7EA : 0xA9B0B8;
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
        if (dash) { gfx_tex_aspect_current = 0.0f; gfx_texture(b, dash); }
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
// --- The Source screen, drawn -------------------------------------------------------------
// A ring hugging `r` from outside (Z1's `box-shadow: 0 0 0 3px`), or a hairline
// on its edge (`thick` 1, inside).
static void spRing(GfxRect r, float radius, float out, float thick, unsigned hex, float a) {
  GfxRect o = { r.x - out, r.y - out, r.w + out * 2.0f, r.h + out * 2.0f };
  gfx_rect(o, 0, GFX_RING_INSET, 0, thick / o.h, 0, (radius + out) / o.h, HEXF(hex), a);
}

// Tracked capitals, top at `y`: the kicker, the section labels, field labels.
static float spCaps(TxtStyle st, const char *s, unsigned hex, float x, float y, float tracking) {
  return txt_tracking(st, s, HEXI(hex), x, y, 1.0f, tracking);
}

// Words wrapped to `maxW` and each line centred on `cx`.
static void spCentred(TxtStyle st, const char *s, unsigned hex, float cx, float y, float maxW, float lh) {
  char line[256] = "", next[256];
  const char *p = s;
  while (*p) {
    const char *w = p;
    size_t k;
    while (*p && *p != ' ') p++;
    k = (size_t)(p - w);
    snprintf(next, sizeof next, "%s%s%.*s", line, line[0] ? " " : "", (int)k, w);
    if (line[0] && txt_width(st, next) > maxW) {
      TxtLine t = txt_line(st, line, HEXI(hex), 255);
      txt_draw(t, cx - t.w * 0.5f, y);
      y += lh;
      snprintf(line, sizeof line, "%.*s", (int)k, w);
    } else {
      snprintf(line, sizeof line, "%s", next);
    }
    while (*p == ' ') p++;
  }
  if (line[0]) { TxtLine t = txt_line(st, line, HEXI(hex), 255); txt_draw(t, cx - t.w * 0.5f, y); }
}

// The busy mark: a faint ring and a bright arc going round.
static void spSpinner(float cx, float cy, float d) {
  float r = d * 0.5f - 1.5f, t = (float)(SDL_GetTicks() % 900u) / 900.0f * 6.2831853f;
  spRing((GfxRect){ cx - d * 0.5f, cy - d * 0.5f, d, d }, d * 0.5f, 0.0f, 3.0f, 0xFFFFFF, 0.18f);
  for (int i = 0; i < 7; i++) {
    float a = t + (float)i * 0.26f;
    gfx_color((GfxRect){ cx + cosf(a) * r - 1.5f, cy + sinf(a) * r - 1.5f, 3.0f, 3.0f }, 0.5f, HEXF(0xF0F2F4), 1.0f);
  }
}

// The outcome's dot: its colour and its glyph, dark on it.
static void spDot(float cx, float cy, float d, unsigned fill, const char *icon, unsigned ink) {
  float g = d * 0.58f;
  gfx_color((GfxRect){ cx - d * 0.5f, cy - d * 0.5f, d, d }, 0.5f, HEXF(fill), 1.0f);
  gfx_icon((GfxRect){ cx - g * 0.5f, cy - g * 0.5f, g, g }, icon, HEXF(ink), 1.0f);
}

// "raw.githubusercontent.com" and "/iptv-org/…/us_pluto.m3u" from an address.
static void splitUrl(const char *u, char *host, size_t nh, char *path, size_t np) {
  const char *h = strstr(u, "://"), *slash;
  h = h ? h + 3 : u;
  slash = strchr(h, '/');
  if (!slash) slash = h + strlen(h);
  snprintf(host, nh, "%.*s", (int)(slash - h), h);
  snprintf(path, np, "%s", slash);
}

// A field: its value (an address as host over path), or its hint; while the
// keyboard is filling it, the text itself on one line with the caret.
static void spField(int f, int focused, int bad) {
  GfxRect r = fieldRect(f);
  int max, isEditing = editing == f && ime_is_open();
  const char *text = fieldText(f, &max);
  float px = spX() ? 22.0f : 26.0f, chipW = 0.0f;
  gfx_color(r, 12.0f / r.h, 1, 1, 1, focused ? 0.07f : 0.05f);
  if (focused) spRing(r, 12.0f, 3.0f, 3.0f, 0xF0F2F4, 1.0f);
  else if (bad) spRing(r, 12.0f, 0.0f, 1.5f, 0xE88A6E, 0.85f);
  // "OK to edit" on the focused field, while it is not being edited.
  if (focused && !isEditing && r.w > 400.0f) {
    TxtLine t = txt_line(TXT_SRCP_CHIP, "OK to edit", HEXI(0xC1C7CD), 255);
    GfxRect c;
    chipW = 14.0f + 17.0f + 9.0f + t.w + 14.0f;
    c = (GfxRect){ r.x + r.w - 22.0f - chipW, r.y + (r.h - 38.0f) * 0.5f, chipW, 38.0f };
    gfx_color(c, 10.0f / 38.0f, 10 / 255.0f, 12 / 255.0f, 14 / 255.0f, 0.55f);
    gfx_icon((GfxRect){ c.x + 14.0f, c.y + (c.h - 17.0f) * 0.5f, 17.0f, 17.0f }, "src_keyboard", HEXF(0xC1C7CD), 1.0f);
    txt_draw(t, c.x + 14.0f + 17.0f + 9.0f, c.y + (c.h - t.h) * 0.5f);
    chipW += 22.0f + 16.0f;
  }
  { float maxW = r.w - px * 2.0f - chipW;
    char shown[1100];
    if (f == F_PASS && !showPass && text[0]) {
      size_t n = strlen(text), k = 0;
      for (size_t j = 0; j < n && k + 4 < sizeof shown; j++)
        if (((unsigned char)text[j] & 0xC0) != 0x80) { memcpy(shown + k, "\xE2\x80\xA2", 3); k += 3; }
      shown[k] = 0;
    } else {
      snprintf(shown, sizeof shown, "%s", text);
    }
    if (isEditing) {
      // The end is what is being typed: trim from the left.
      const char *s = shown;
      TxtLine t = txt_line(TXT_SRCP_VALUE, s, HEXI(0xF5F6F8), 255);
      while (t.w > maxW - 8.0f && *s) {
        s++;
        while (((unsigned char)*s & 0xC0) == 0x80) s++;
        t = txt_line(TXT_SRCP_VALUE, s, HEXI(0xF5F6F8), 255);
      }
      txt_draw(t, r.x + px, r.y + (r.h - t.h) * 0.5f);
      if ((SDL_GetTicks() / 530u) % 2u == 0u)
        gfx_color((GfxRect){ r.x + px + t.w + 2.0f, r.y + r.h * 0.5f - 15.0f, 3.0f, 30.0f }, 0.0f, 1, 1, 1, 1);
    } else if (!shown[0]) {
      TxtLine t = txt_line_trim(TXT_SRCP_PATH, fieldHint(f), HEXI(0x565C63), 255, maxW);
      txt_draw(t, r.x + px, r.y + (r.h - t.h) * 0.5f);
    } else if (f == F_URL || f == F_EPG) {
      char host[256], path[1024], first[1024];
      const char *sp = strchr(shown, ' ');
      int more = 0;
      snprintf(first, sizeof first, "%.*s", sp ? (int)(sp - shown) : (int)strlen(shown), shown);
      for (const char *q = shown; (q = strchr(q, ' ')); q++) if (q[1] && q[1] != ' ') more++;
      splitUrl(first, host, sizeof host, path, sizeof path);
      if (more) { size_t k = strlen(path); snprintf(path + k, sizeof path - k, "  \xC2\xB7  +%d more", more); }
      { TxtLine a = txt_line_trim(TXT_SRCP_VALUE, host[0] ? host : path, HEXI(focused ? 0xF5F6F8 : 0xE4E7EA), 255, maxW);
        TxtLine b = txt_line_trim(TXT_SRCP_PATH, path, HEXI(0x8A9199), 255, maxW);
        if (host[0] && path[0] && path[1]) {
          float top = r.y + (r.h - (a.h + 3.0f + b.h)) * 0.5f;
          txt_draw(a, r.x + px, top);
          txt_draw(b, r.x + px, top + a.h + 3.0f);
        } else {
          txt_draw(a, r.x + px, r.y + (r.h - a.h) * 0.5f);
        } }
    } else {
      TxtLine t = txt_line_trim(TXT_SRCP_VALUE, shown, HEXI(focused ? 0xF5F6F8 : 0xE4E7EA), 255, maxW);
      txt_draw(t, r.x + px, r.y + (r.h - t.h) * 0.5f);
    }
  }
  { int row = f == F_URL ? SR_URL : f == F_EPG ? SR_EPG : f <= F_PORT ? SR_SERVER : SR_LOGIN;
    int col = f == F_PORT || f == F_PASS ? 1 : 0;
    pointer_zone(r.x, r.y, r.w, r.h, pointSetup, rowIndex(row), col); }
}

// A field's label: tracked capitals, 26 above the field.
static float spLabel(const char *s, float x, float y) {
  return spCaps(TXT_CWC_KICKER, s, 0x7C838B, x, y, 1.36f);
}

// The card's name for the source: the playlist file's own name when it has a
// telling one, else the server.
static void sourceName(const IptvSource *s, char *name, size_t nn, char *sub, size_t ns) {
  char host[256], path[1024];
  splitUrl(s->kind == IPTV_SRC_XTREAM ? s->server : s->url, host, sizeof host, path, sizeof path);
  if (s->kind == IPTV_SRC_XTREAM) {
    snprintf(name, nn, "%s", host);
    snprintf(sub, ns, "Signed in as %s", s->user);
    return;
  }
  { const char *q = strchr(path, '?'), *e = q ? q : path + strlen(path), *b = e;
    char stem[128];
    while (b > path && b[-1] != '/') b--;
    snprintf(stem, sizeof stem, "%.*s", (int)(e - b), b);
    if (strrchr(stem, '.')) *strrchr(stem, '.') = 0;
    for (char *c = stem; *c; c++) if (*c == '_' || *c == '-') *c = ' ';
    // "get", "playlist", "index": the file says nothing, the server does.
    if (!stem[0] || !strcasecmp(stem, "get") || !strcasecmp(stem, "playlist") ||
        !strcasecmp(stem, "index") || !strcasecmp(stem, "tv") || !strcasecmp(stem, "iptv") ||
        !strcasecmp(stem, "list") || !strcasecmp(stem, "channels"))
      snprintf(name, nn, "%s", host[0] ? host : "IPTV playlist");
    else snprintf(name, nn, "%s", stem);
    snprintf(sub, ns, "%s", host[0] ? host : "A playlist on this TV");
  }
}

// THE CURRENT SOURCE: what it is, what it holds, how fresh, and Refresh.
static void drawSourceCard(void) {
  const IptvSource *s = iptv_source();
  const IptvList *l = iptv_list();
  GfxRect c = { X0(), 184.0f, L_RIGHT - X0(), 128.0f };
  float x = c.x + 30.0f, cy = c.y + c.h * 0.5f, right = c.x + c.w - 30.0f;
  char name[256], sub[300], ago[64];
  int focused = setupRows[setupRow] == SR_REFRESH;
  gfx_color(c, 14.0f / c.h, 1, 1, 1, 0.05f);
  gfx_color((GfxRect){ x, cy - 34.0f, 68.0f, 68.0f }, 13.0f / 68.0f, HEXF(0x1A1D21), 1.0f);
  gfx_icon((GfxRect){ x + 20.0f, cy - 14.0f, 28.0f, 28.0f }, "live_source", HEXF(0xC1C7CD), 1.0f);
  x += 68.0f + 26.0f;

  // Refresh, from the right.
  { TxtLine t = txt_line(TXT_SRC_CHIP, "Refresh", HEXI(focused ? C_INK : 0xE4E7EA), 255);
    float w = 24.0f + 19.0f + 11.0f + t.w + 24.0f;
    GfxRect b = { right - w, cy - 29.0f, w, 58.0f };
    if (focused) gfx_color(b, 0.5f, HEXF(C_PAPER), 1.0f);
    else gfx_color(b, 0.5f, 1, 1, 1, 0.09f);
    gfx_icon((GfxRect){ b.x + 24.0f, cy - 9.5f, 19.0f, 19.0f }, "src_refresh", HEXF(focused ? C_INK : 0xE4E7EA), 1.0f);
    txt_draw(t, b.x + 24.0f + 19.0f + 11.0f, cy - t.h * 0.5f);
    refreshRect = b;
    pointer_zone(b.x, b.y, b.w, b.h, pointSetup, rowIndex(SR_REFRESH), 0);
    right = b.x - 12.0f - 26.0f; }
  // How fresh, then the two numbers, each after a divider.
  { long long at = iptv_refreshed_at(), age = at ? (long long)time(NULL) - at : -1;
    if (iptv_state() == IPTV_LOADING || iptv_guide_state() == IPTV_LOADING) snprintf(ago, sizeof ago, "Refreshing\xE2\x80\xA6");
    else if (age < 0) snprintf(ago, sizeof ago, "Saved copy");
    else if (age < 60) snprintf(ago, sizeof ago, "Refreshed just now");
    else if (age < 3600) snprintf(ago, sizeof ago, "Refreshed %lld min ago", age / 60);
    else if (age < 86400) snprintf(ago, sizeof ago, "Refreshed %lld h ago", age / 3600);
    else snprintf(ago, sizeof ago, "Refreshed %lld d ago", age / 86400);
    { TxtLine t = txt_line(TXT_LIVE_NOTE, ago, HEXI(0x8A9199), 255);
      right -= t.w;
      txt_draw(t, right, cy - t.h * 0.5f); } }
  if (l) {
    char num[24];
    const char *unit[2] = { "channels", "with guide" };
    int val[2] = { l->nCh, guidedChannels() };
    for (int k = 1; k >= 0; k--) {
      TxtLine n, u;
      right -= 26.0f;
      gfx_color((GfxRect){ right - 1.0f, cy - 20.0f, 1.0f, 40.0f }, 0.0f, 1, 1, 1, 0.12f);
      right -= 26.0f + 1.0f;
      snprintf(num, sizeof num, "%d", val[k]);
      n = txt_line(TXT_SRCP_NUM, num, HEXI(0xF5F6F8), 255);
      u = txt_line(TXT_LIVE_NOTE, unit[k], HEXI(0x8A9199), 255);
      right -= u.w;
      // On a shared baseline: the unit's bottom sits with the number's.
      txt_draw(u, right, cy + n.h * 0.5f - u.h - 3.0f);
      right -= 10.0f + n.w;
      txt_draw(n, right, cy - n.h * 0.5f);
    }
  }
  // Name and kind, host under.
  sourceName(s, name, sizeof name, sub, sizeof sub);
  { float maxW = right - 40.0f - x - 90.0f;
    TxtLine n = txt_line_trim(TXT_SRCP_NAME, name, HEXI(0xF5F6F8), 255, maxW);
    TxtLine h = txt_line_trim(TXT_SRC_TEXT, sub, HEXI(0x7C838B), 255, maxW + 90.0f);
    float top = cy - (n.h + 6.0f + h.h) * 0.5f;
    txt_draw(n, x, top);
    { const char *k = s->kind == IPTV_SRC_XTREAM ? "XTREAM" : "M3U";
      float w = txt_tracking(TXT_SRCP_BADGE, k, 255, 255, 255, -1.0f, 0.0f, 0.0f, 1.2f) + 20.0f;
      GfxRect b = { x + n.w + 12.0f, top + n.h * 0.5f - 14.0f, w, 28.0f };
      TxtLine probe = txt_line(TXT_SRCP_BADGE, "M", 255, 255, 255, 255);
      spRing(b, 7.0f, 0.0f, 1.0f, 0xFFFFFF, 0.28f);
      txt_tracking(TXT_SRCP_BADGE, k, HEXI(0xC1C7CD), b.x + 10.0f, b.y + (b.h - probe.h) * 0.5f, 1.0f, 1.2f); }
    txt_draw(h, x, top + n.h + 6.0f); }
}

// A step's heading: its number on a square, and what it is.
static void spStep(const char *n, const char *title, float x, float y, int first) {
  TxtLine d = txt_line(TXT_CWC_KICKER, n, HEXI(first ? C_INK : 0xC1C7CD), 255);
  TxtLine t = txt_line(TXT_SRCP_STEP, title, HEXI(0xF5F6F8), 255);
  if (first) gfx_color((GfxRect){ x, y, 32.0f, 32.0f }, 9.0f / 32.0f, HEXF(C_PAPER), 1.0f);
  else gfx_color((GfxRect){ x, y, 32.0f, 32.0f }, 9.0f / 32.0f, 1, 1, 1, 0.12f);
  txt_draw(d, x + (32.0f - d.w) * 0.5f, y + (32.0f - d.h) * 0.5f);
  txt_draw(t, x + 32.0f + 13.0f, y + (32.0f - t.h) * 0.5f);
}

// 1 · THE PHONE. The QR, and for a camera that will not focus across the room
// the address and four digits to type instead.
#define P_QR 256.0f
static void drawPhoneCard(void) {
  GfxRect c = { X0(), SP_TOP - spLift(), 560.0f, 578.0f };
  float cx = c.x + c.w * 0.5f, y = c.y + 30.0f;
  int st = phonelink_state();
  const char *url = phonelink_url();
  GLuint tex = st != PL_OFF && url[0] ? qr_texture(url) : 0;
  char addr[64], digits[8];
  phonelink_short(addr, sizeof addr, digits, sizeof digits);
  gfx_color(c, 16.0f / c.h, 1, 1, 1, 0.055f);
  spStep("1", "Send it from your phone", c.x + 30.0f, y, 1);
  y += 32.0f + 20.0f;
  { GfxRect q = { cx - 150.0f, y, 300.0f, 300.0f };
    if (tex) {
      gfx_color(q, 16.0f / q.h, 1, 1, 1, 1);
      gfx_tex_aspect_current = 0.0f;
      gfx_rect((GfxRect){ q.x + 22.0f, q.y + 22.0f, P_QR, P_QR }, tex, GFX_SNAP, 0, 0.0f, 0.0f, 0.0f, 0, 0, 0, 1.0f);
    } else {
      gfx_color(q, 16.0f / q.h, 1, 1, 1, 0.04f);
      spCentred(TXT_SRCP_DESC, "Connect the TV to your home network to send it from a phone.", 0x8A9199,
                cx, q.y + 120.0f, 240.0f, 24.0f);
    } }
  y += 300.0f + 20.0f;
  if (tex && addr[0]) {
    TxtLine a = txt_line(TXT_SRCP_ADDR, addr, HEXI(0xE4E7EA), 255);
    TxtLine k = txt_line(TXT_LIVE_TIME, "code", HEXI(0x7C838B), 255);
    float dw = txt_tracking(TXT_SRCP_CODE, digits, 255, 255, 255, -1.0f, 0.0f, 0.0f, 12.0f);
    TxtLine probe = txt_line(TXT_SRCP_CODE, "0", 255, 255, 255, 255);
    float rowW = k.w + 10.0f + dw, ry;
    txt_draw(a, cx - a.w * 0.5f, y);
    ry = y + a.h + 12.0f;
    txt_draw(k, cx - rowW * 0.5f, ry + (probe.h - k.h) * 0.5f + 2.0f);
    txt_tracking(TXT_SRCP_CODE, digits, HEXI(0xF5F6F8), cx - rowW * 0.5f + k.w + 10.0f, ry, 1.0f, 12.0f);
    y = ry + probe.h + 20.0f;
  }
  spCentred(TXT_SRCP_DESC,
            st == PL_OPENED ? "A phone has the page open. Paste the URL there and press Save; it lands here."
                            : "Scan it, or open that address on any device on the same network. "
                              "Paste the URL there and it lands here.",
            st == PL_OPENED ? 0xE4E7EA : 0x7C838B, cx, y, 400.0f, 24.0f);
}

// 2 · THE TV'S OWN FORM.
static void drawTypeIt(void) {
  float x = spRX(), top = SP_TOP - spLift();
  int row = setupRows[setupRow];
  char line[300];
  unsigned bad = 0;
  int rs = setupResult(line, sizeof line, &bad);
  spStep("2", "Or type it on the TV", x, top, 0);

  // M3U playlist | Xtream Codes.
  { static const char *KIND[2] = { "M3U playlist", "Xtream Codes" };
    GfxRect k = kindRect();
    float w0 = 24.0f * 2.0f + txt_width(TXT_SRC_CHIP, KIND[0]), w1 = 24.0f * 2.0f + txt_width(TXT_SRC_CHIP, KIND[1]);
    int x1 = draft.kind == IPTV_SRC_XTREAM;
    k.w = 5.0f + w0 + w1 + 5.0f;
    gfx_color(k, 0.5f, 1, 1, 1, 0.06f);
    for (int i = 0; i < 2; i++) {
      GfxRect s = { k.x + 5.0f + (i ? w0 : 0.0f), k.y + 5.0f, i ? w1 : w0, 46.0f };
      int on = i == x1;
      TxtLine t = txt_line(on ? TXT_SRC_CHIP : TXT_SRC_TEXT, KIND[i], HEXI(on ? C_INK : 0x8A9199), 255);
      if (on) gfx_color(s, 0.5f, HEXF(C_PAPER), 1.0f);
      if (on && row == SR_KIND) spRing(s, 23.0f, 5.0f, 3.0f, 0xF0F2F4, 1.0f);
      txt_draw(t, s.x + (s.w - t.w) * 0.5f, s.y + (s.h - t.h) * 0.5f);
      pointer_zone(s.x, s.y, s.w, s.h, pointKind, i, 0);
    } }

  if (!spX()) {
    spLabel("PLAYLIST ADDRESS", x, labelY(SR_URL));
    spField(F_URL, row == SR_URL, (bad >> F_URL) & 1);
  } else {
    GfxRect port = fieldRect(F_PORT), pass = fieldRect(F_PASS);
    spLabel("SERVER", x, labelY(SR_SERVER));
    spLabel("PORT", port.x, labelY(SR_SERVER));
    spField(F_SERVER, row == SR_SERVER && setupCol == 0, (bad >> F_SERVER) & 1);
    spField(F_PORT, row == SR_SERVER && setupCol == 1, (bad >> F_PORT) & 1);
    spLabel("USERNAME", x, labelY(SR_LOGIN));
    spLabel("PASSWORD", pass.x, labelY(SR_LOGIN));
    spField(F_USER, row == SR_LOGIN && setupCol == 0, (bad >> F_USER) & 1);
    spField(F_PASS, row == SR_LOGIN && setupCol == 1, (bad >> F_PASS) & 1);
    // "show": a stop of its own, right of the password's label — a d-pad
    // keyboard makes typos, and dots hide them.
    { int on = row == SR_LOGIN && setupCol == 2;
      const char *w = showPass ? "hide" : "show";
      TxtLine t = txt_line(on ? TXT_SRC_CHIP : TXT_LIVE_TIME, w, HEXI(on ? C_INK : 0x7C838B), 255);
      GfxRect b = { pass.x + pass.w - t.w - 24.0f, labelY(SR_LOGIN) - 7.0f, t.w + 24.0f, 32.0f };
      if (on) gfx_color(b, 0.5f, HEXF(C_PAPER), 1.0f);
      txt_draw(t, b.x + 12.0f, b.y + (b.h - t.h) * 0.5f);
      pointer_zone(b.x, b.y, b.w, b.h, pointSetup, rowIndex(SR_LOGIN), 2); }
  }
  { float lx = x + spLabel("EXTRA TV GUIDE", x, labelY(SR_EPG)) + 12.0f;
    TxtLine n = txt_line(TXT_SRCP_NOTE, "optional \xE2\x80\x94 only for channels the playlist misses", HEXI(0x565C63), 255);
    TxtLine probe = txt_line(TXT_CWC_KICKER, "E", 255, 255, 255, 255);
    txt_draw(n, lx, labelY(SR_EPG) + (probe.h - n.h) * 0.5f); }
  spField(F_EPG, row == SR_EPG, 0);

  // The outcome.
  if (rs != RS_NONE) {
    float cy = resultCY(), tx = x + 24.0f + 12.0f;
    TxtLine t = txt_line_trim(TXT_LIVE_META, line, HEXI(rs == RS_FAIL ? 0xF0C0AE : 0xA9B0B8), 255,
                              spRW() - 24.0f - 12.0f - 36.0f);
    if (rs == RS_FAIL) {
      GfxRect band = { x - 16.0f, cy - 25.0f, 24.0f + 12.0f + t.w + 32.0f, 50.0f };
      gfx_color(band, 12.0f / band.h, 232 / 255.0f, 138 / 255.0f, 110 / 255.0f, 0.08f);
      spRing(band, 12.0f, 0.0f, 1.0f, 0xE88A6E, 0.24f);
    }
    if (rs == RS_BUSY) spSpinner(x + 12.0f, cy, 24.0f);
    else if (rs == RS_OK) spDot(x + 12.0f, cy, 24.0f, 0x5FD29A, "src_ok", 0x0A2A1B);
    else if (rs == RS_WARN) spDot(x + 12.0f, cy, 24.0f, 0xE8B65C, "src_warn", 0x3A2A0C);
    else spDot(x + 12.0f, cy, 24.0f, 0xE88A6E, "src_fail", 0x3A150C);
    txt_draw(t, tx, cy - t.h * 0.5f);
  }

  // Save and load (Go to channels once it has), and Cancel.
  { float bx = x, by = buttonsY();
    const char *label[2] = { primaryIsDone() ? "Go to channels" : "Save and load", "Cancel" };
    for (int b = 0; b < rowCols(SR_BUTTONS); b++) {
      int on = row == SR_BUTTONS && setupCol == b;
      TxtLine t = txt_line(b ? TXT_SRCP_GHOST : TXT_SRCP_PRIMARY, label[b],
                           HEXI(b ? (on ? 0xF5F6F8 : 0x8A9199) : C_INK), 255);
      GfxRect r = { bx, by, t.w + (b ? 60.0f : 68.0f), 64.0f };
      if (!b) gfx_color(r, 0.5f, HEXF(C_PAPER), 1.0f);
      else if (on) gfx_color(r, 0.5f, 1, 1, 1, 0.09f);
      if (on) spRing(r, 32.0f, 5.0f, 3.0f, 0xF0F2F4, 1.0f);
      txt_draw(t, r.x + (r.w - t.w) * 0.5f, r.y + (r.h - t.h) * 0.5f);
      pointer_zone(r.x, r.y, r.w, r.h, pointSetup, rowIndex(SR_BUTTONS), b);
      bx += r.w + 14.0f;
    } }
}

static void drawSetup(void) {
  float x = X0(), y = 52.0f;
  { TxtLine probe = txt_line(TXT_SRC_CHIP, "L", 255, 255, 255, 255);
    spCaps(TXT_SRC_CHIP, "LIVE TV", 0x7C838B, x, y, 2.9f);
    y += probe.h + 10.0f; }
  { TxtLine t = txt_line(TXT_TITLE3, configured() ? "Change your IPTV source" : "Connect your IPTV service",
                         HEXI(0xF5F6F8), 255);
    txt_draw(t, x, y); }
  if (configured()) {
    drawSourceCard();
    spCaps(TXT_SRCP_SECTION, "REPLACE IT", 0x7C838B, x, 358.0f, 2.56f);
  }
  drawPhoneCard();
  drawTypeIt();
}

// --- Full screen ---------------------------------------------------------------------------------
// The live tag: LIVE on its red tint; TUNING while the stream opens; the
// failure in the same place, where the eye already is.
static float streamTag(float x, float cy, float a) {
  int tuning = zapPending || bufWaiting || !video_ready();
  int onLive = !playFailed && !tuning && atLive();
  char behind[32];
  const char *word = playFailed ? "UNAVAILABLE" : tuning ? "TUNING" : paused ? "PAUSED"
                   : onLive ? "LIVE" : srcKind == SRC_ARCHIVE ? "CATCH-UP" : behind;
  int live = onLive && !paused;
  if (!playFailed && !tuning && !paused && !onLive) {
    long sec = (long)(wallNow() - playAt() + 0.5);
    // Behind live: −0:40 under ten minutes, −25 MIN past them.
    if (sec < 600) snprintf(behind, sizeof behind, "\xE2\x88\x92%ld:%02ld", sec / 60, sec % 60);
    else snprintf(behind, sizeof behind, "\xE2\x88\x92%ld MIN", (sec + 30) / 60);
  }
  float w = tagWidth(word) + 24.0f + (live ? 16.0f : 0.0f);
  GfxRect r = { x, cy - 15.0f, w, 30.0f };
  if (live || playFailed) gfx_color(r, 8.0f / 30.0f, 232 / 255.0f, 96 / 255.0f, 76 / 255.0f, 0.16f * a);
  else gfx_color(r, 8.0f / 30.0f, 1, 1, 1, 0.10f * a);
  if (live) gfx_color((GfxRect){ x + 12.0f, cy - 4.0f, 8.0f, 8.0f }, 0.5f, HEXF(C_LIVE), a);
  tagText(word, live || playFailed ? 0xF0A090 : 0xC1C7CD, x + 12.0f + (live ? 16.0f : 0.0f), cy, a);
  return w;
}

// 1 · THE ZAP TOAST. Identity and nothing else, bottom-left, no scrim.
static void drawZapToast(float a) {
  const IptvChannel *c = chan(tuned);
  const IptvProgramme *pg;
  long long now = nowMinute();
  char kick[300], until[40] = "";
  float tile = 80.0f, pad = 22.0f, textX, textW = 400.0f, h = tile + 2 * pad;
  if (!c || a < 0.01f) return;
  pg = programmeAt(tuned, now);
  snprintf(kick, sizeof kick, "%d", c->number);
  if (pg) { char z[16]; clockText(pg->stop, z, sizeof z); snprintf(until, sizeof until, "until %s", z); }
  { float tw = txt_width(TXT_PLR_EPCODE, pg ? pg->title : c->name);
    if (tw > textW) textW = tw > 720.0f ? 720.0f : tw; }
  { float uw = until[0] ? txt_width(TXT_LIVE_META, until) + 24.0f : 0.0f;
    GfxRect card = { 96.0f, NV_SCREEN_H - 62.0f - h, pad + tile + 24.0f + textW + uw + 36.0f, h };
    float y = card.y + pad;
    gfx_opacity_group = a;
    gfx_drop_shadow(card, 20.0f, 34.0f, 16.0f, 0.5f);
    gfx_color((GfxRect){ card.x - 1.0f, card.y - 1.0f, card.w + 2.0f, card.h + 2.0f }, 21.0f / (card.h + 2.0f), 1, 1, 1, 0.08f);
    gfx_color(card, 20.0f / card.h, 8 / 255.0f, 10 / 255.0f, 13 / 255.0f, 0.86f);
    identity(c, (GfxRect){ card.x + pad, y, tile, tile }, 14.0f, C_PLATE, 0xE4E7EA, a);
    textX = card.x + pad + tile + 24.0f;
    { float kx = textX;
      kx += ink(TXT_LIVE_META_B, kick, 0xC1C7CD, kx, y - 2.0f, a) + 10.0f;
      kx += ink(TXT_LIVE_META, "\xC2\xB7", 0x4D535A, kx, y - 2.0f, a) + 10.0f;
      inkTrim(TXT_LIVE_META, c->name, 0x8A9199, kx, y - 2.0f, textW - (kx - textX), a); }
    inkTrim(TXT_PLR_EPCODE, pg ? pg->title : c->name, 0xF5F6F8, textX, y + 26.0f, textW, a);
    if (pg) progressBar(textX, y + tile - 6.0f, textW, 4.0f,
                        (float)(now - pg->start) / (float)(pg->stop - pg->start), 0.20f, a);
    if (playFailed) {
      TxtLine t = txt_line(TXT_LIVE_META, "Unavailable", HEXI(0xF0A090), 255);
      txt_draw_alpha(t, card.x + card.w - 36.0f - t.w, y + tile - t.h, a);
    } else if (until[0]) {
      TxtLine t = txt_line(TXT_LIVE_META, until, HEXI(0x7C838B), 255);
      txt_draw_alpha(t, card.x + card.w - 36.0f - t.w, y + tile - t.h, a);
    }
    gfx_opacity_group = 1.0f; }
}

// THE ROW OF CONTROLS, the film player's own (player.c): 90px circles 14 apart,
// 48px glyphs, no circle at rest — the white puck IS the focus, fading in on the
// player's spring while the glyph crosses from white to black under it — and
// the focused button's name 16px under its circle, faded and risen the same way.
#define CTL_D     90.0f
#define CTL_GAP   14.0f
#define CTL_ICON  48.0f
#define CTL_TIP   16.0f
static void drawControls(float x, float cy, float a) {
  static const char *ICON[CTL_N] = { "pause", "live_restart", "live_edge", "live_grid", "live_list",
                                     "subtitles", "audio", "aspect", "live_star" };
  static const char *NAME[CTL_N] = { "Pause", "Start over", "Go live", "Guide", "Channels", "Subtitles",
                                     "Audio", "Aspect Ratio", "Favourite" };
  float step = CTL_D + CTL_GAP, x0 = x + CTL_D * 0.5f;
  int fav = iptv_is_favourite(tuned), ids[CTL_N], n = shownControls(ids);
  for (int k = 0; k < n; k++) {
    int i = ids[k];
    float f = ctlFocus[i], cx = x0 + k * step, luma = 0.94f + (0.13f - 0.94f) * f;
    const char *icon = i == CTL_FAV && fav ? "live_star_fill" : i == CTL_PLAY && paused ? "play" : ICON[i];
    if (f > 0.004f)
      gfx_color((GfxRect){ cx - CTL_D * 0.5f, cy - CTL_D * 0.5f, CTL_D, CTL_D }, 0.5f, 1, 1, 1, f * a);
    gfx_icon((GfxRect){ cx - CTL_ICON * 0.5f, cy - CTL_ICON * 0.5f, CTL_ICON, CTL_ICON }, icon,
             luma, luma, luma, a * 0.94f);
  }
  // The label, "Remove favourite" when that is what OK would do.
  for (int k = 0; k < n; k++) {
    int i = ids[k];
    float f = ctlFocus[i];
    const char *name = i == CTL_FAV && fav ? "Remove favourite" : i == CTL_PLAY && paused ? "Play" : NAME[i];
    if (f > 0.004f) {
      TxtLine label = txt_line(TXT_PLR_TIP, name, 255, 255, 255, 255);
      TxtLine sh = txt_line(TXT_PLR_TIP, name, 0, 0, 0, 255);
      float lx = x0 + k * step - label.w * 0.5f;
      float ly = cy + CTL_D * 0.5f + CTL_TIP + (1.0f - f) * 4.0f;
      txt_draw_alpha(sh, lx, ly + 2.0f, a * 0.80f * f);
      txt_draw_alpha(label, lx, ly, a * 0.92f * f);
    }
  }
}

// What the stream is: its resolution, outlined, and the audio track.
static void drawStreamFacts(float right, float cy, float a) {
  int h = video_height(), ai = video_audio_current();
  const VideoTrack *au = ai >= 0 ? video_audio(ai) : NULL;
  float x = right;
  if (au && au->label[0]) {
    TxtLine t = txt_line_trim(TXT_SRC_TEXT, au->label, HEXI(0x9AA1A9), 255, 360.0f);
    x -= t.w;
    txt_draw_alpha(t, x, cy - t.h * 0.5f, a);
    x -= 14.0f;
  }
  if (ownsVideo() && h > 0) {
    const char *res = h >= 2000 ? "4K" : h >= 1000 ? "1080p" : h >= 700 ? "720p" : "SD";
    TxtLine t = txt_line(TXT_PLR_BADGE, res, HEXI(0xE4E7EA), 255);
    float w = t.w + 28.0f;
    x -= w;
    gfx_color((GfxRect){ x, cy - 19.0f, w, 38.0f }, 10.0f / 38.0f, 1, 1, 1, 0.30f * a);
    gfx_color((GfxRect){ x + 1.0f, cy - 18.0f, w - 2.0f, 36.0f }, 9.0f / 36.0f, 10 / 255.0f, 11 / 255.0f, 14 / 255.0f, a);
    txt_draw_alpha(t, x + 14.0f, cy - t.h * 0.5f, a);
  }
}

// 2 · THE BAR, and 4 · the walk, which is the same block on a later programme.
static void drawBlock(float a) {
  const IptvList *l = iptv_list();
  const IptvChannel *c = chan(tuned);
  long long now = nowMinute();
  int walking = ov == OV_WALK && walkPg >= 0 && l && walkPg < l->nPg && l->pg[walkPg].channel == tuned;
  // Rewound, the bar is about what is on screen, not what is on now.
  double at = shownAt();
  int guessed = 0;
  const IptvProgramme *pg = walking ? &l->pg[walkPg] : programmeOrHour(tuned, (long long)at, &guessed);
  const IptvProgramme *next = walking ? NULL : programmeAfter(tuned, (long long)at, 0);
  float x = 96.0f, right = NV_SCREEN_W - 96.0f;
  float ctlY = NV_SCREEN_H - 64.0f - CTL_D * 0.5f; // the controls' centre: the film player's row
  float trackY = ctlY - CTL_D * 0.5f - 26.0f - 12.0f;   // the progress track's centre
  float idBottom = trackY - 12.0f - 54.0f + 24.0f - 26.0f;
  float tile = 84.0f;
  char line[300], a1[16], b1[16];
  if (!c) return;
  gfx_opacity_group = a;
  identity(c, (GfxRect){ x, idBottom - tile, tile, tile }, 14.0f, C_PLATE, 0xE4E7EA, walking ? a * 0.6f : a);
  { float tx = x + tile + 26.0f, titleW = right - tx - 520.0f;
    TxtLine title = txt_line_trim(TXT_LIVE_TITLE, pg ? pg->title : c->name, HEXI(0xF5F6F8), 255, titleW);
    float titleY = idBottom - title.h + 6.0f, kickY = titleY - 9.0f - 16.0f;
    txt_draw_alpha(title, tx, titleY, a);
    // The title has the focus: ▲▼ beside it say it rolls (the peek).
    if (ov == OV_BAR && barCtl < 0 && !scrubbing && nView > 1) {
      TxtLine u = txt_line(TXT_LIVE_TAG, "\xE2\x96\xB2", HEXI(0xC1C7CD), 255);
      TxtLine d = txt_line(TXT_LIVE_TAG, "\xE2\x96\xBC", HEXI(0xC1C7CD), 255);
      float ax = tx + title.w + 20.0f, cy = titleY + title.h * 0.5f + 2.0f;
      txt_draw_alpha(u, ax, cy - 11.0f - u.h * 0.5f, 0.6f * a);
      txt_draw_alpha(d, ax, cy + 11.0f - d.h * 0.5f, 0.6f * a);
    }
    if (walking) {
      // LATER, outlined, and when.
      float w = tagWidth("LATER") + 22.0f, kx = tx;
      long long until = (pg->start - nowSec() + 59) / 60;
      gfx_color((GfxRect){ kx, kickY - 15.0f, w, 30.0f }, 8.0f / 30.0f, 1, 1, 1, 0.28f * a);
      gfx_color((GfxRect){ kx + 1.0f, kickY - 14.0f, w - 2.0f, 28.0f }, 7.0f / 28.0f, 10 / 255.0f, 11 / 255.0f, 14 / 255.0f, a);
      tagText("LATER", 0xC1C7CD, kx + 11.0f, kickY, a);
      kx += w + 14.0f;
      clockText(pg->start, a1, sizeof a1); clockText(pg->stop, b1, sizeof b1);
      if (until >= 60) snprintf(line, sizeof line, "%s \xE2\x80\x93 %s \xC2\xB7 in %lld h %02lld min", a1, b1, until / 60, until % 60);
      else snprintf(line, sizeof line, "%s \xE2\x80\x93 %s \xC2\xB7 in %lld min", a1, b1, until);
      inkMid(TXT_LIVE_NAME, line, 0x8A9199, kx, kickY, 900.0f, a);
    } else {
      float kx = tx;
      char num[16];
      snprintf(num, sizeof num, "%d", c->number);
      kx += inkMid(TXT_DETWEB_EP_BADGE, num, 0xC1C7CD, kx, kickY, 120.0f, a) + 14.0f;
      if (pg && !guessed) {
        kx += inkMid(TXT_LIVE_NAME, "\xC2\xB7", 0x4D535A, kx, kickY, 30.0f, a) + 14.0f;
        kx += inkMid(TXT_LIVE_NAME, c->name, 0x9AA1A9, kx, kickY, 520.0f, a) + 14.0f;
      }
      kx += streamTag(kx, kickY, a) + 14.0f;
      // Paused with nothing to keep the stream in: say what resuming will do.
      if (paused && srcKind == SRC_LIVE && !iptv_has_archive(tuned, (long long)playAt()))
        inkMid(TXT_LIVE_NAME, "No pause buffer \xC2\xB7 resumes live", 0x8A9199, kx, kickY, 520.0f, a);
    } }
  // The right of the identity row: NEXT, or in the walk, the reminder.
  if (walking) {
    int set = reminderIndex(c->name, pg->start) >= 0;
    const char *w = set ? "Reminder set" : "Remind me";
    TxtLine t = txt_line(TXT_DETWEB_EP_BADGE, w, HEXI(0xE4E7EA), 255);
    GfxRect r = { right - t.w - 44.0f, idBottom - 52.0f, t.w + 44.0f, 52.0f };
    gfx_color(r, 0.5f, 1, 1, 1, set ? 0.18f : 0.10f * a);
    txt_draw_alpha(t, r.x + 22.0f, r.y + (r.h - t.h) * 0.5f, a);
  } else if (next) {
    TxtLine t;
    clockText(next->start, a1, sizeof a1);
    snprintf(line, sizeof line, "%s \xE2\x80\x82%s", a1, next->title);
    t = txt_line_trim(TXT_PLR_META3, line, HEXI(0xA9B0B8), 255, 480.0f);
    txt_draw_alpha(t, right - t.w, idBottom - 4.0f - t.h, a);
    { float w = tagWidth("NEXT");
      txt_tracking(TXT_LIVE_TAG, "NEXT", HEXI(0x7C838B), right - w, idBottom - 4.0f - t.h - 8.0f - 16.0f, a, 2.2f); }
  }
  // The programme. At plain live there is no playhead dot and no band — the
  // dot is the app's promise that a thing can be moved, and live cannot be.
  // With a past to move through (the pause buffer, catch-up) it can: then the
  // dot is the picture's instant, the lighter band what can be reached, and a
  // tick where now is. Clock times either side: a programme, not a file.
  if (pg) {
    float span = (float)(pg->stop - pg->start);
    float frac = walking ? 0.0f : (float)((at - (double)pg->start) / span);
    float fa = walking ? a * 0.4f : a;
    float tx = x + 62.0f + 20.0f, tw = right - 62.0f - 20.0f - tx;
    int movable = !walking && ownsVideo() && (canRewind() || !atLive() || paused || scrubbing);
    clockText(pg->start, a1, sizeof a1); clockText(pg->stop, b1, sizeof b1);
    inkMid(TXT_LIVE_META, a1, 0x8A9199, x, trackY, 80.0f, fa);
    { TxtLine t = txt_line(TXT_LIVE_META, b1, HEXI(0x8A9199), 255);
      txt_draw_alpha(t, right - t.w, trackY - t.h * 0.5f, fa); }
    if (movable) {
      double floor = rewindFloor(), wn = wallNow();
      float lo = (float)((floor - (double)pg->start) / span), hi = (float)((wn - (double)pg->start) / span);
      if (lo < 0) lo = 0;
      if (hi > 1) hi = 1;
      gfx_color((GfxRect){ tx, trackY - 4.0f, tw, 8.0f }, 0.5f, 1, 1, 1, 0.12f * fa);
      if (hi > lo) gfx_color((GfxRect){ tx + tw * lo, trackY - 4.0f, tw * (hi - lo), 8.0f }, 0.5f, 1, 1, 1, 0.16f * fa);
      progressBar(tx, trackY - 4.0f, tw, 8.0f, frac, 0.0f, fa);
      if (hi < 1.0f && hi > 0.0f)
        gfx_color((GfxRect){ tx + tw * hi - 1.5f, trackY - 9.0f, 3.0f, 18.0f }, 0.5f, 1, 1, 1, 0.85f * fa);
      { float cxDot = tx + tw * (frac < 0 ? 0 : frac > 1 ? 1 : frac), d = scrubbing ? 26.0f : 22.0f;
        gfx_color((GfxRect){ cxDot - d * 0.5f, trackY - d * 0.5f, d, d }, 0.5f, 1, 1, 1, fa); }
    } else {
      progressBar(tx, trackY - 4.0f, tw, 8.0f, frac, walking ? 0.16f : 0.22f, fa);
    }
    if (!walking) {
      char in[64];
      TxtLine t;
      float fx;
      double back = wallNow() - at;
      if (scrubbing || back >= LIVE_EDGE_S) {
        char c1[16];
        clockText((long long)at, c1, sizeof c1);
        if (back >= 3600.0) snprintf(in, sizeof in, "%s \xC2\xB7 %ld h %02ld min behind live", c1,
                                     (long)(back / 3600.0), (long)(back / 60.0) % 60);
        else if (back >= 60.0) snprintf(in, sizeof in, "%s \xC2\xB7 %ld min behind live", c1, (long)(back / 60.0));
        else if (back >= LIVE_EDGE_S) snprintf(in, sizeof in, "%s \xC2\xB7 %ld s behind live", c1, (long)back);
        else snprintf(in, sizeof in, "%s \xC2\xB7 live", c1);
      } else if (guessed) {
        snprintf(in, sizeof in, "Live \xC2\xB7 no guide for this channel");
      } else {
        snprintf(in, sizeof in, "%lld min in", (now - pg->start) / 60);
      }
      t = txt_line(TXT_SRC_STATE, in, HEXI(0xE4E7EA), 255);
      fx = tx + tw * (frac < 0 ? 0 : frac > 1 ? 1 : frac) - t.w * 0.5f;
      if (fx < tx) fx = tx;
      if (fx + t.w > tx + tw) fx = tx + tw - t.w;
      txt_draw_alpha(t, fx, trackY - 4.0f - 12.0f - t.h, a);
    }
  }
  // The controls and the stream's facts; in the walk, what comes after it.
  if (walking) {
    int an = walkNext(walkPg);
    const IptvProgramme *after = an >= 0 ? &l->pg[an] : NULL;
    if (after) {
      float hx = x;
      hx += txt_tracking(TXT_SRC_TIER, "THEN", HEXI(0x7C838B), hx, ctlY - 12.0f, a, 1.7f) + 12.0f;
      clockText(after->start, a1, sizeof a1);
      snprintf(line, sizeof line, "%s %s", a1, after->title);
      inkTrim(TXT_LIVE_META, line, 0xA9B0B8, hx, ctlY - 12.0f, 900.0f, a);
    }
  } else {
    drawControls(x, ctlY, a);
    drawStreamFacts(right, ctlY, a);
  }
  gfx_opacity_group = 1.0f;
}

// 3 · THE PEEK: the bar's title rolls into a vertical carousel. The channel in
// the middle stands where the title stood, at the title's size, with its tile,
// number and name above and its programme's progress under; its neighbours
// above and below shrink and fade with distance. ▲▼ roll it, OK zaps to the
// middle, Back puts the bar back. The stream stays put until OK.
#define PK_STEP 112.0f
static void drawPeek(float a) {
  const IptvList *l = iptv_list();
  const IptvChannel *tc = chan(tuned);
  long long now = nowMinute();
  float x = 96.0f, right = NV_SCREEN_W - 96.0f;
  float cy = NV_SCREEN_H - 300.0f;          // the middle row's centre
  float half = (float)(nView - 1) * 0.5f;
  if (!l || !nView) return;
  gfx_opacity_group = a;
  for (int k = -5; k <= 4; k++) {
    int r = (int)floorf(peekScroll) + k;
    float d = (float)r - peekScroll, ad = fabsf(d);
    float y = cy + d * PK_STEP;
    float f = ad < 1.0f ? 1.0f - ad : 0.0f;              // 1 in the middle
    float ca = ad < 1.0f ? 1.0f - 0.5f * ad : ad < 3.0f ? 0.5f - 0.18f * (ad - 1.0f)
             : 0.14f * (4.0f - ad);
    // Below the middle there is room for one row: the next fades out sooner.
    if (d > 1.0f) ca *= 1.0f - (d - 1.0f) * 2.0f;
    float tile = 58.0f + 26.0f * f, tx = x + tile + 26.0f, titleW = right - tx - 520.0f;
    int idx = ((r % nView) + nView) % nView, mid = ad < 0.5f;
    const IptvChannel *c;
    const IptvProgramme *pg;
    char kick[300];
    // A short list shows each channel once, never wrapped round to itself.
    if (ad > half + 0.5f || ca <= 0.01f) continue;
    // Above, up to the clock; below, one row, clear of the STILL ON line.
    if (y < 190.0f || y > cy + PK_STEP * 1.5f) continue;
    c = &l->ch[view[idx]];
    pg = mid ? programmeOrHour(view[idx], now, NULL) : programmeAt(view[idx], now);
    identity(c, (GfxRect){ x, y - tile * 0.5f, tile, tile }, 10.0f + 4.0f * f, mid ? C_PLATE_F : C_PLATE,
             mid ? 0xF5F6F8 : 0xC1C7CD, a * ca);
    snprintf(kick, sizeof kick, "%d \xC2\xB7 %s", c->number, c->name);
    if (mid) {
      TxtLine t = txt_line_trim(TXT_LIVE_TITLE, pg ? pg->title : c->name, HEXI(0xF5F6F8), 255, titleW);
      inkTrim(TXT_LIVE_NAME, kick, 0x9AA1A9, tx, y - t.h * 0.5f - 30.0f, titleW, a * ca);
      txt_draw_alpha(t, tx, y - t.h * 0.5f, a * ca);
      if (pg) {
        char a1[16], b1[16], w[48];
        float bw = titleW < 640.0f ? titleW : 640.0f, by = y + t.h * 0.5f + 16.0f;
        progressBar(tx, by, bw, 5.0f, (float)(now - pg->start) / (float)(pg->stop - pg->start), 0.22f, a * ca);
        clockText(pg->start, a1, sizeof a1); clockText(pg->stop, b1, sizeof b1);
        snprintf(w, sizeof w, "%s \xE2\x80\x93 %s", a1, b1);
        inkMid(TXT_LIVE_META, w, 0x8A9199, tx + bw + 16.0f, by + 2.5f, 240.0f, a * ca);
      }
      // The right: what follows on it, as the bar's NEXT.
      { const IptvProgramme *nx = programmeAfter(view[idx], now, 0);
        if (nx) {
          char a1[16], line[300];
          TxtLine n;
          float wN = tagWidth("NEXT");
          clockText(nx->start, a1, sizeof a1);
          snprintf(line, sizeof line, "%s \xE2\x80\x82%s", a1, nx->title);
          n = txt_line_trim(TXT_PLR_META3, line, HEXI(0xA9B0B8), 255, 480.0f);
          txt_draw_alpha(n, right - n.w, y - n.h * 0.5f + 10.0f, a * ca);
          txt_tracking(TXT_LIVE_TAG, "NEXT", HEXI(0x7C838B), right - wN, y - n.h * 0.5f - 14.0f, a * ca, 2.2f);
        } }
    } else {
      // A neighbour: its name and now on one line each, smaller.
      inkTrim(TXT_LIVE_NAME, kick, 0x8A9199, tx, y - 26.0f, titleW, a * ca);
      inkTrim(TXT_PLR_EPCODE, pg ? pg->title : "No guide data", pg ? 0xE4E7EA : 0x8A9199, tx, y + 2.0f,
              titleW, a * ca);
    }
  }
  // Under it: still on 103, and what OK does.
  if (tc) {
    char still[40];
    float r, g, b, w, by = NV_SCREEN_H - 55.0f - 38.0f;
    accent(&r, &g, &b);
    snprintf(still, sizeof still, "STILL ON %d", tc->number);
    w = tagWidth(still) + 30.0f;
    gfx_color((GfxRect){ 96.0f, by, w, 38.0f }, 10.0f / 38.0f, r, g, b, 0.20f * a);
    tagText(still, 0xA896FA, 96.0f + 15.0f, by + 19.0f, a);
    inkMid(TXT_LIVE_NOTE, "OK to watch \xC2\xB7 Back to stay", 0x8A9199, 96.0f + w + 18.0f, by + 19.0f, 600.0f, a);
  }
  gfx_opacity_group = 1.0f;
}

static void drawClock(float a) {
  char c[16], d[48];
  time_t tt = (time_t)nowSec();
  struct tm tmv;
  localtime_r(&tt, &tmv);
  strftime(c, sizeof c, "%H:%M", &tmv);
  strftime(d, sizeof d, "%a %d %b", &tmv);
  { TxtLine t = txt_line(TXT_PG_CLOCK, c, HEXI(0xF5F6F8), 255), u = txt_line(TXT_LIVE_NOTE, d, HEXI(0x8A9199), 255);
    txt_draw_alpha(t, NV_SCREEN_W - 96.0f - t.w, 66.0f, a);
    txt_draw_alpha(u, NV_SCREEN_W - 96.0f - u.w, 66.0f + t.h + 2.0f, a); }
}

// The episode selector's equaliser: white, on the detail line's baseline.
static void quickEq(float x, float base, float a) {
  static const float SPEED[3] = { 0.0091f, 0.0067f, 0.0113f };
  static const float PHASE[3] = { 0.0f, 2.1f, 4.2f };
  Uint32 now = SDL_GetTicks();
  for (int i = 0; i < 3; i++) {
    float s = 0.5f + 0.5f * sinf((float)now * SPEED[i] + PHASE[i]);
    float h = NV_SRC_EQ_H * (NV_SRC_EQ_MIN + (1.0f - NV_SRC_EQ_MIN) * s);
    gfx_color((GfxRect){ x + i * (NV_SRC_EQ_W + NV_SRC_EQ_GAP), base - h, NV_SRC_EQ_W, h },
              NV_SRC_EQ_R / h, 1, 1, 1, a);
  }
}
#define Q_EQ_W (3 * NV_SRC_EQ_W + 2 * NV_SRC_EQ_GAP)

static float quickMeta(TxtStyle st, const char *text, float x, float y, int first, int c, int dim, float a) {
  if (!text || !text[0]) return x;
  if (!first) {
    TxtLine d = txt_line(TXT_SRC_META, "\xC2\xB7", 255, 255, 255, dim);
    txt_draw_alpha(d, x, y, a);
    x += (float)d.w + 10.0f;
  }
  { TxtLine l = txt_line(st, text, c, c, c, 255);
    txt_draw_alpha(l, x, y, a);
    return x + (float)l.w + 10.0f; }
}

// One channel row, laid out as an episode row: the channel's plate where the
// still is (the logo, or its monogram, on a 16:9 plate), its number and name,
// the programme on now as the detail line, and the synopsis as the row opens.
static void quickRowDraw(int row, float cx, float y, float h, int sel, float open, float a, float vt, float vb) {
  const IptvList *l = iptv_list();
  int ch = view[row];
  const IptvChannel *c = &l->ch[ch];
  const IptvProgramme *pg = quickProgramme(row);
  long long now = nowMinute();
  float sc = 1.0f + (NV_EPL_THUMB_GROW - 1.0f) * open;
  GfxRect th = { cx + NV_EPL_PADX, y + (h - NV_EPL_THUMB_H * sc) * 0.5f, NV_EPL_THUMB_W * sc, NV_EPL_THUMB_H * sc };
  float tx = cx + Q_TEXT_X(open), right = cx + NV_EPL_W - NV_EPL_PADX;
  float blockH = NV_EPL_BLOCK_H + (quickFullBlock(pg) - NV_EPL_BLOCK_H) * open;
  float bt = y + (h - blockH) * 0.5f;
  int current = ch == tuned;
  int lit = sel || current;
  int inkC = lit ? 255 : NV_EPL_DIM, sub = lit ? 178 : NV_EPL_DIM_SUB, dim = lit ? 104 : 80;
  char line[300];

  if (sel) {
    GfxRect band = { cx - NV_EPL_BAND_LEAD, y, NV_SCREEN_W - cx + NV_EPL_BAND_LEAD, h };
    gfx_rect(band, 0, GFX_MENU_FEATHER, 0, NV_EPL_BAND_FEATHER / band.w, 0, 0, 1, 1, 1, NV_EPL_BAND * a);
    { float g = NV_RING_FOCUS;
      GfxRect r = { th.x - g, th.y - g, th.w + g * 2, th.h + g * 2 };
      gfx_rect(r, 0, GFX_RING_INSET, 0, g / r.h, 0, (NV_EPL_THUMB_R + g) / r.h,
               1, 1, 1, 0.96f * a * (open > 0.2f ? 1.0f : open * 5.0f)); }
  }
  // The plate: identity() at the thumb's size, dimmed with a resting row.
  identity(c, th, NV_EPL_THUMB_R, C_PLATE, 0xC1C7CD, a * (lit ? 1.0f : NV_EPL_DIM_THUMB));
  // What has aired of the programme on now, along the plate's base.
  if (pg) {
    float f = (float)(now - pg->start) / (float)(pg->stop - pg->start);
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    gfx_color((GfxRect){ th.x, th.y + th.h - NV_EPL_PROG_H, th.w, NV_EPL_PROG_H }, 0, 0, 0, 0, 0.55f * a);
    gfx_color((GfxRect){ th.x, th.y + th.h - NV_EPL_PROG_H, th.w * f, NV_EPL_PROG_H }, 0, 1, 1, 1,
              a * (lit ? 1.0f : NV_EPL_DIM_THUMB));
  }
  // A favourite's star on the plate's corner, where the watched mark sits.
  if (iptv_is_favourite(ch)) {
    float d = NV_EPL_CHECK, in = 8.0f;
    GfxRect st = { th.x + th.w - in - d, th.y + in, d, d };
    gfx_color(st, 0.5f, 0.05f, 0.05f, 0.06f, 0.72f * a);
    gfx_icon((GfxRect){ st.x + d * 0.2f, st.y + d * 0.2f, d * 0.6f, d * 0.6f }, "live_star_fill", 1, 1, 1, 0.9f * a);
  }

  snprintf(line, sizeof line, "%d \xC2\xB7 %s", c->number, c->name);
  txt_draw_alpha(txt_line_trim(TXT_TRK_VALUE, line, inkC, inkC, inkC, 255, right - tx), tx, bt, a);

  // The detail line: on the playing channel the equaliser and "Playing"; then
  // the programme on now and what is left of it.
  { float my = bt + NV_EPL_SUB_DY, x = tx;
    int first = 1;
    if (current) {
      quickEq(x, my + txt_baseline(TXT_SRC_META), a);
      x += Q_EQ_W + 9.0f;
      x = quickMeta(TXT_SRC_STATE, "Playing", x, my, 1, 244, dim, a);
      first = 0;
    }
    if (pg) {
      TxtLine t;
      if (!first) {
        TxtLine d = txt_line(TXT_SRC_META, "\xC2\xB7", 255, 255, 255, dim);
        txt_draw_alpha(d, x, my, a);
        x += (float)d.w + 10.0f;
      }
      snprintf(line, sizeof line, "%lld min left", (pg->stop - now + 59) / 60);
      { float leftW = txt_width(TXT_SRC_META, line) + 30.0f;
        t = txt_line_trim(TXT_SRC_META, pg->title, sub, sub, sub, 255, right - x - leftW);
        txt_draw_alpha(t, x, my, a);
        x += (float)t.w + 10.0f; }
      quickMeta(TXT_SRC_META, line, x, my, 0, sub, dim, a);
    } else {
      quickMeta(TXT_SRC_META, iptv_guide_state() == IPTV_LOADING ? "Loading guide\xE2\x80\xA6" : "No guide data",
                x, my, first, sub, dim, a);
    } }

  // The synopsis, faded in as the row opens and cut to the row's height.
  if (open > 0.01f && pg && pg->desc[0]) {
    float cy0 = y > vt ? y : vt, cy1 = y + h < vb ? y + h : vb;
    if (cy1 > cy0) {
      gfx_crop(0, cy0, NV_SCREEN_W, cy1 - cy0);
      txt_block_trim(TXT_TRK_OPTSUB, pg->desc, 176, 178, 184, tx, bt + NV_EPL_SYN_DY, right - tx,
                     NV_EPL_SYN_LD, a * open * open, NV_EPL_SYN_LINES);
      gfx_crop(0, vt, NV_SCREEN_W, vb - vt);
    }
  }
}

// The group pill, the season pill's twin (pillDraw in episodes.c).
static GfxRect quickPill(float x, float y, float a) {
  float f = quickZone == Q_PILL || gmOpen ? 1.0f : 0.0f;
  GfxRect r = { x, y, NV_EPL_PILL_W, NV_EPL_PILL_H };
  TxtLine l = txt_line_trim(TXT_TRK_VALUE, groupLabel(group), 255, 255, 255, 255,
                            NV_EPL_PILL_W - NV_EPL_PILL_PADX * 2 - 16.0f - NV_EPL_PILL_CHEV);
  { float luma = NV_DETWEB_REST + (NV_DETWEB_SEA_FOCUS_BG - NV_DETWEB_REST) * f;
    gfx_color(r, NV_RADIUS_PILL, luma, luma, luma, NV_PLR_DD_A * a); }
  if (f < 0.99f) gfx_rect(r, 0, GFX_RING, 0, NV_DETWEB_SEA_BORDER / r.h, 0, NV_RADIUS_PILL, 1, 1, 1, 0.10f * a);
  else gfx_rect(r, 0, GFX_RING_INSET, 0, NV_DETWEB_SEA_RING / r.h, 0, NV_RADIUS_PILL, 1, 1, 1, 0.96f * a);
  txt_draw_alpha(l, x + NV_EPL_PILL_PADX, y + (r.h - l.h) * 0.5f, a);
  gfx_icon((GfxRect){ r.x + r.w - NV_EPL_PILL_PADX - NV_EPL_PILL_CHEV, y + (r.h - NV_EPL_PILL_CHEV) * 0.5f,
                      NV_EPL_PILL_CHEV, NV_EPL_PILL_CHEV }, "chevron_down", 0.702f, 0.702f, 0.702f, a);
  return r;
}

// Its menu, the season menu's (seasonMenu in episodes.c).
static void quickGroupMenu(GfxRect pill, float a) {
  int vis = gmVisible();
  float w = pill.w, radius;
  GfxRect box;
  for (int i = gmScroll; i < gmScroll + vis; i++) {
    float need = txt_width(TXT_TRK_OPT, groupLabel(i)) + (NV_DETWEB_SEA_MENU_PADX + NV_DETWEB_SEA_OPT_PADX) * 2;
    if (need > w) w = need > 620.0f ? 620.0f : need;
  }
  box = (GfxRect){ pill.x, pill.y + pill.h + NV_DETWEB_SEA_MENU_GAP, w,
                   NV_DETWEB_SEA_MENU_PADY * 2 + vis * NV_EPL_SMENU_ROW };
  radius = 32.0f / box.h;
  gfx_color(box, radius, NV_DETWEB_REST, NV_DETWEB_REST, NV_DETWEB_REST, NV_PLR_DD_A * a);
  gfx_rect(box, 0, GFX_RING, 0, 1.0f / box.h, 0, radius, 1, 1, 1, 0.08f * a);
  for (int i = 0; i < vis; i++) {
    int g = gmScroll + i, on = g == gmCursor, ink = on ? 17 : 255;
    GfxRect op = { box.x + NV_DETWEB_SEA_MENU_PADX, box.y + NV_DETWEB_SEA_MENU_PADY + i * NV_EPL_SMENU_ROW,
                   box.w - NV_DETWEB_SEA_MENU_PADX * 2, NV_EPL_SMENU_ROW };
    pointer_zone(op.x, op.y, op.w, op.h, pointGroupMenu, g, 0);
    if (on) gfx_color(op, NV_RADIUS_PILL, NV_DETWEB_FOCUS, NV_DETWEB_FOCUS, NV_DETWEB_FOCUS, a);
    { TxtLine t = txt_line_trim(TXT_TRK_OPT, groupLabel(g), ink, ink, ink, 255, op.w - NV_DETWEB_SEA_OPT_PADX * 2);
      txt_draw_alpha(t, op.x + NV_DETWEB_SEA_OPT_PADX, op.y + (op.h - t.h) * 0.5f, a); }
  }
}

static void drawQuick(void) {
  float an = quickA, slide = (1.0f - an) * NV_EPL_VEIL_W * NV_TRK_SLIDE;
  float cx = NV_SCREEN_W - NV_EPL_PAD - NV_EPL_W + slide, vt = NV_EPL_VIEW_TOP, y;
  GfxRect pill;
  if (an < 0.005f) return;
  // The episode panel's veil, heading and count.
  gfx_rect((GfxRect){ NV_SCREEN_W - NV_EPL_VEIL_W + slide, 0, NV_EPL_VEIL_W, NV_SCREEN_H }, 0, GFX_SRC_VEIL, 0,
           1, 0, 0, NV_SRC_INK_R, NV_SRC_INK_G, NV_SRC_INK_B, an * NV_SRC_VEIL_A);
  { TxtLine t = txt_line(TXT_PANEL_TITLE, "Channels", 240, 241, 243, 255);
    txt_draw_alpha(t, cx + NV_EPL_PADX, NV_TRK_TITLE_Y, an);
    if (nView) {
      char c[32];
      float capT = txt_cap_inset(TXT_PANEL_TITLE), capC = txt_cap_inset(TXT_SRC_COUNT);
      float mid = NV_TRK_TITLE_Y + (capT + txt_baseline(TXT_PANEL_TITLE)) * 0.5f;
      snprintf(c, sizeof c, "%d channel%s", nView, nView == 1 ? "" : "s");
      txt_draw_alpha(txt_line(TXT_SRC_COUNT, c, 132, 135, 142, 255), cx + NV_EPL_PADX + (float)t.w + 18.0f,
                     mid - (capC + txt_baseline(TXT_SRC_COUNT)) * 0.5f, an);
    } }
  pill = quickPill(cx + NV_EPL_PADX, NV_TRK_TABS_Y, an);
  pointer_zone(pill.x, pill.y, pill.w, pill.h, pointQuickPill, 0, 0);

  if (!nView) {
    txt_block(TXT_TRK_OPTSUB, group == GROUP_FAV ? "No favourites yet." : "No channels in this group.",
              133, 134, 136, cx + NV_EPL_PADX, vt + 16.0f, NV_EPL_W - NV_EPL_PADX * 2, 28.0f, an, 3);
  } else if (quickOpenedCap >= nView) {
    float fadeTop = NV_TRK_TABS_Y + NV_EPL_PILL_H + NV_RING_FOCUS;
    gfx_crop(0, fadeTop, NV_SCREEN_W, NV_SCREEN_H - fadeTop);
    pointer_clip(0, fadeTop, NV_SCREEN_W, NV_SCREEN_H - fadeTop);
    // Only the rows near the window: a playlist can be thousands of channels.
    { int first = (int)(quickScroll / Q_PITCH) - 1;
      if (first < 0) first = 0;
      y = vt - quickScroll + first * Q_PITCH;
      // The rows above `first` are closed, bar the focused one which is below it.
      for (int i = first; i < nView && y < NV_SCREEN_H; i++) {
        float op = quickOpened[i], h = NV_EPL_ROW_H, edge;
        if (op > 0.001f) h += quickOpenExtra(quickProgramme(i)) * op;
        edge = anim_edge(y, fadeTop, vt - fadeTop);
        if (y + h > fadeTop && edge > 0.004f) {
          gfx_opacity_group = edge;
          quickRowDraw(i, cx, y, h, quickZone == Q_LIST && i == quickRow, op, an, fadeTop, NV_SCREEN_H);
          gfx_opacity_group = 1.0f;
          pointer_zone(cx, y, NV_EPL_W, h, pointQuick, i, 0);
        }
        y += h + NV_EPL_ROW_GAP;
      } }
    gfx_no_crop();
    pointer_no_clip();
  }
  if (gmOpen) quickGroupMenu(pill, an);
}

static void drawFull(void) {
  const IptvChannel *c = chan(tuned);
  drawVideo((GfxRect){ 0.0f, 0.0f, NV_SCREEN_W, NV_SCREEN_H }, 0.0f);
  // While the stream opens or has failed, the channel stands in for the picture.
  if (c && (!video_ready() || zapPending || playFailed) && barA < 0.5f && toastA < 0.5f)
    identity(c, (GfxRect){ NV_SCREEN_W * 0.5f - 80.0f, NV_SCREEN_H * 0.5f - 80.0f, 160.0f, 160.0f }, 28.0f,
             C_PLATE, 0xC1C7CD, 1.0f);
  if (barA > 0.01f) {
    // The player's own scrims, one quad each: a shader gradient, never a blur
    // and never stacked bands (this is live video under them).
    gfx_rect((GfxRect){ 0.0f, 0.0f, NV_SCREEN_W, 190.0f }, 0, GFX_VEIL_TOP, 0, 0, 0, 0.0f, 0, 0, 0, 0.72f * barA);
    { float h = ov == OV_PEEK ? 860.0f : 560.0f;
      gfx_rect((GfxRect){ 0.0f, NV_SCREEN_H - h, NV_SCREEN_W, h }, 0, GFX_VEIL_PLAYER, 0, 0, 0, 0.0f, 0, 0, 0, barA); }
    drawClock(barA);
    if (ov == OV_PEEK) drawPeek(barA); else drawBlock(barA);
  }
  drawZapToast(toastA);
  drawQuick();
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
