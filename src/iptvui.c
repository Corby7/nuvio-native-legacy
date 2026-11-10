// The Live TV screen, built to the Y1 (guide), Y2 (channel list) and Y3
// (mechanics) mockups. See iptvui.h for its faces; the data comes from iptv.h,
// and the only thing this file knows about the network is that the list can
// change under it between two frames (iptv_step).
//
// ------------------------------------------------------------------------
// THE SHAPE, AS OF 2026-09-27
//
//   header    "Live TV", the source line, [Guide|Channels] [Source]
//   chips     Favourites (the landing), All channels, Recent, then the playlist's groups,
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
#include "notice.h"
#include "iptv.h"
#include "iptv_also.h"
#include "app.h"
#include "ctxmenu.h"
#include "data.h"
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
#include "tvlogos.h"
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
#define L_BTN_Y           50.0f
#define L_BTN_H           58.0f
#define L_CHIPS_Y        142.0f
// THE COLUMN OF GROUPS (the 2026-11 redesign): the chips' carousel stood up on
// the left, under the header, beside both views. The body starts after it.
#define L_BODY_Y         160.0f
#define L_COL_W          320.0f
#define L_COL_GAP         40.0f
#define L_COL_ITEM_H      62.0f
#define L_COL_STEP        68.0f
#define L_COL_ICON        28.0f
// Favourites, Recent and All channels, pinned above the line, a size up.
#define L_PIN_ITEM_H      72.0f
#define L_PIN_STEP        80.0f
#define L_CHIP_H          48.0f
#define L_CHIP_GAP        12.0f
#define L_CHIP_PAD        24.0f
#define L_CHIP_MAX_W     360.0f

// Y1, the guide.
#define G_DETAIL_Y       168.0f
// The preview stands level with the chips, which stop short of it in the
// guide: 480x270, and the ruler tucked under it, so the schedule starts right
// under the programme's description.
// In the redesign it stands at the body's left, the detail to its right.
#define G_PREV_W         420.0f
#define G_PREV_H         236.0f
#define G_PREV_Y         L_BODY_Y
#define G_PREV_R          16.0f
#define G_RULER_Y        440.0f
#define G_RULER_H         34.0f
#define G_GRID_Y         488.0f
#define G_BOTTOM        1040.0f
#define G_ROW_H           84.0f
#define G_ROW_STEP        96.0f
#define G_CH_W           300.0f
#define G_COL_GAP         10.0f
#define G_BLOCK_GAP        8.0f
#define G_BLOCK_R         12.0f
#define G_CH_R            14.0f
#define G_TILE            56.0f
// Y3's block tiers: title and both times, title and the start, or nothing.
#define G_TIER_FULL      200.0f
#define G_TIER_TEXT       96.0f

// Y2, the channel list.
#define C_LIST_Y         L_BODY_Y
#define C_ROW_H          118.0f
#define C_ROW_STEP       130.0f
#define C_ROW_R           18.0f
#define C_TILE            72.0f
#define C_DIM             1.0f
#define C_PANEL_W        620.0f
#define C_PREV_H         349.0f
#define C_PREV_R          18.0f

#define LIVE_SPAN_S       7200     // two hours across the guide
#define LIVE_SLOT_S       1800     // paged in half hours
#define LIVE_AHEAD_S      (28 * 3600)

// The full-screen view.
#define LIVE_TOAST_OV_MS  3000u   // the zap toast
#define LIVE_BAR_MS       6000u   // the bar, the walk
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
enum { ZONE_HEAD, ZONE_CHIPS, ZONE_BODY, ZONE_ACTIONS, ZONE_PREVIEW };
enum { HEAD_SEARCH, HEAD_CLEAR, HEAD_SWITCH, HEAD_SOURCE, HEAD_N };
enum { ACT_WATCH, ACT_FAV, ACT_RESTART, ACT_N };
// The pinned chips, in chip order: Live TV opens on Favourites.
enum { GROUP_FAV, GROUP_ALL, GROUP_RECENT, GROUP_FIRST };

static int mode, viewMode = VIEW_LIST, zone = ZONE_BODY, headSel = HEAD_SWITCH, actSel;
// GUIDE OR LIST, the last one the viewer was on, kept across launches in
// iptv_view.txt. Every change goes through setView; opening reads savedView.
static void setView(int v) {
  if (v == viewMode) return;
  viewMode = v;
  data_write("iptv_view.txt", v == VIEW_GUIDE ? "guide\n" : "list\n");
}
static int savedView(void) {
  char *s = data_read("iptv_view.txt");
  int v = s && !strncmp(s, "guide", 5) ? VIEW_GUIDE : VIEW_LIST;
  free(s);
  return v;
}
// SEARCH (the header's first button) is a mode, not a page: the circle grows
// into a field in its place, the source pill folds away and Guide | List slides
// over to keep its place at the edge, and the list or the guide, whichever is
// on show, narrows to what matches as it is typed on the TV's own keyboard.
// The field is HEAD_SEARCH, its clear button HEAD_CLEAR; the chips step aside.
static int searching;
static float searchT;           // 0 the circle, 1 the field
static char query[96];
static int queryLen;
static void closeSearch(void);
static int wantsExit, requestMenu;

// The chips and the channels they select.
static int group = GROUP_FAV;
static int *view;
static int nView, capView;
static float *chipW;            // measured once per list, not per frame
static float *chipFull;         // the same, uncapped: what a long label needs
static int nChipW;
// THE FOCUSED CHIP OPENS to its whole label when the cap cut it short, sprung
// (0 capped, 1 whole), its neighbours making way. Back to capped as it leaves.
// THE GROUPS' ORDER, the viewer's: chip position -> group, every chip in it,
// the pinned three (Favourites, All channels, Recent) too. Kept in
// iptv_group_order.txt by name (so a reloaded playlist keeps it), the pinned
// ones by a key of their own (PIN_KEY) that no playlist group is called. A
// pinned chip the file does not name goes first, as before they could move;
// a playlist group it does not know goes last, in the playlist's order.
static int *gOrder, nOrder;
// Moving a group: hold OK on it in the column. ▲ ▼ step it, ◀ sends it to the
// top, ▶ to the bottom, OK or Back puts it down. No dragging: a provider can
// carry hundreds.
static int movingGroup;
// MOVING A FAVOURITE: "Move" in a favourite's hold menu, in Favourites. ▲ ▼
// step it past its neighbour, ◀ sends it to the top, ▶ to the bottom, OK or
// Back puts it down. The order is iptv.c's, saved with the favourites.
static int movingChannel;
// The column of groups' scroll, in pixels (it was the chips' carousel's).
static float chipScroll, chipScrollV;

// The focus: see the note at the top.
static int fRow, fChan;
static long long fTime, winStart;
static float scrollRows, scrollRowsV;
static char focusName[256];
static Hold hold;
// HOLD OK ON A CHANNEL OR A PROGRAMME: the home's poster menu (ctxmenu.h),
// beside the focused plate. `plate` is that plate's outer edge this frame (its
// halo included), set by whichever draws the focus; the menu anchors on it and
// its scrim's hole follows it. What the menu was opened on is kept by NAME and
// start time, since the list can reload under an open menu.
static GfxRect plate;
static float plateR;
static int plateSet;
static char menuName[256];
static long long menuStart;
enum { M_WATCH, M_FROM_START, M_REMIND, M_GROUP, M_FAV, M_MOVE };

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

// THE OVERLAY OVER A PLAYING CHANNEL. The keys are the film player's:
//   nothing up  OK pauses and resumes, ▲▼ bring the controls up, ◀▶ (and
//               CH+/CH-) change channel, with a TOAST naming where it landed.
//   BAR         what is this and what is next, the controls under it. It also
//               shows on its own, arriving and while paused, as information
//               only (`barPassive`): the keys stay the nothing-up ones until
//               ▲▼ give it the focus, which lands on the buttons.
//               OK with nothing up pauses and puts the focus on Pause.
//               On the buttons: ◀▶ along them, OK presses, ▲ up to the
//               programme's track, ▼ puts it all away.
//               On the track: ◀▶ move through time where there is a past (and
//               ▶ at live walks the schedule), OK pauses or lands the chosen
//               instant, ▼ back to the buttons, ▲ up to the title.
//               On the title, carets beside it: OK opens the PEEK, ▼ back to
//               the track, ▲ puts it all away.
//   PEEK        the title rolls into a vertical carousel of channels by what
//               is on now; ▲▼ roll it, OK zaps to the one in the middle, and
//               until then the stream stays put. Back returns to the title.
//   WALK        the same block, a later programme; OK sets a reminder.
// Timers restart on every press and never stack; the bar goes after LIVE_BAR_MS
// unless paused, choosing an instant or under a sheet.
enum { OV_NONE, OV_TOAST, OV_BAR, OV_PEEK, OV_WALK };
// The controls, by id. Not all are shown at once (shownControls): Start over
// needs a past to start from, Go live a present to go back to.
enum { CTL_PLAY, CTL_RESTART, CTL_LIVE, CTL_GUIDE, CTL_CHANNELS, CTL_ALSO, CTL_SUBS, CTL_AUDIO,
       CTL_ASPECT, CTL_FAV, CTL_N };
static int ov;
static Uint32 ovUntil;
static float toastA, barA;
static float titleFocus;        // the bar's title has the focus: its plate
// THE TUNING LOGO, the film player's loading logo (player.c): the channel's
// mark breathing in the middle while its stream opens, filling as it goes.
#define LIVE_LOAD_OUTRO_MS 450u
static float tuneFill;
static Uint32 tuneBeganAt, tuneEndAt;
static int tuneWas, tuneFor = -1;
static void hiReset(void);       // the better logos, per channel (logoTex)
static int previewTuning(GfxRect r, float radius);   // a preview's opening logo
static float tagText(const char *s, unsigned hex, float x, float cy, float a);
static float tagWidth(const char *s);
// The channel number's purple: the accent's light ink, as "12 min in" wears it.
#define C_NUMBER 0xA896FA
// The same purple where the number stands on a white plate (a focused cell).
#define C_NUMBER_ON_PAPER 0x6A4FD6
// THE BAR BETWEEN FIELDS, the detail screen's (a 2px rule in the group grey):
// `h` tall, centred on `cy`, `gap` either side. The width it takes.
static float sepBar(float x, float cy, float h, float gap, float a) {
  gfx_color((GfxRect){ x + gap, cy - h * 0.5f, NV_DETW2_BAR_W, h }, 0.0f, 0.502f, 0.502f, 0.502f, a);
  return gap * 2.0f + NV_DETW2_BAR_W;
}
static void accent(float *r, float *g, float *b);
// The focused plate in the list or the guide: the hold's white sweep round its
// 3px halo while OK is held, and its edge kept for the menu. `r` is the plate's
// own corner radius in px.
static void focusPlate(GfxRect p, float r) {
  plate = (GfxRect){ p.x - 3.0f, p.y - 3.0f, p.w + 6.0f, p.h + 6.0f };
  plateR = r + 3.0f;
  plateSet = 1;
  if (movingChannel) {
    // Being moved: the chips' accent ring, and carets for the way it goes.
    float ar, ag, ab, cy = p.y + p.h * 0.5f;
    GfxRect o = { p.x - 5.0f, p.y - 5.0f, p.w + 10.0f, p.h + 10.0f };
    accent(&ar, &ag, &ab);
    gfx_rect(o, 0, GFX_RING_INSET, 0, 3.0f / o.h, 0, (r + 5.0f) / o.h, ar, ag, ab, 1.0f);
    gfx_icon((GfxRect){ o.x - 30.0f, cy - 22.0f, 20.0f, 20.0f }, "live_caret_up", ar, ag, ab, 1.0f);
    gfx_icon((GfxRect){ o.x - 30.0f, cy + 2.0f, 20.0f, 20.0f }, "live_caret_down", ar, ag, ab, 1.0f);
    return;
  }
  hold_sweep_inset(&hold, plate, (3.0f + NV_HOLD_RING_GROW * hold_grow(&hold)) / plate.h,
                   plateR / plate.h, 1.0f);
}
static int barCtl = -1;         // the focused control's id, or BAR_TRACK or BAR_TITLE
#define BAR_TRACK (-1)          // the programme's track
#define BAR_TITLE (-2)          // the title, which opens the channel carousel
static int barPassive = 1;      // the bar is up as information: no focus on it
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
// help, a live TS reports the broadcaster's clock, not the load's.
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
// (episodes.c) with channels for episodes, the same column at the right, veil,
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
// THE SAME BROADCAST ELSEWHERE (iptv_also.h), the panel's other list: with
// `alsoMode` its rows are alsoCh[], every channel airing what the tuned one
// airs, the best picture first, instead of the group's view[]. Built for
// channel `alsoFor` at minute `alsoAt`, and again when either moves (not while
// the list is open on it: its rows would shift under the focus).
static int alsoMode;
static int alsoCh[IPTV_ALSO_MAX], nAlso;
static int alsoFor = -1;
static long long alsoAt;
#define QN() (alsoMode ? nAlso : nView)
#define QCH(r) (alsoMode ? alsoCh[r] : view[r])
// THE SHARPER-COPY NOTICE: once per channel and programme, a few seconds into
// the picture, when a copy ranks a tier above what is decoding. While it
// shows, ▲ opens the list on that copy.
#define LIVE_NOTICE_MS 8000u
#define LIVE_SETTLE_MS 3000u   // a picture this old describes its own channel
static char alsoNoticeKey[300];
static int alsoNoticeCh = -1, alsoNoticeFor = -1;
static Uint32 alsoNoticeUntil;
static Uint32 qualityNotedFor;  // the tunedAt whose picture was measured
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
enum { F_URL, F_SERVER, F_PORT, F_USER, F_PASS, F_EPG, F_NAME, F_N };
static IptvSource draft;
static char draftPort[8];       // Xtream's port, split off draft.server for its own field
static int showPass;
static int setupRow, setupCol;
static int setupRows[8], nSetupRows;
static int editing = -1;        // the F_* the keyboard is filling
static Uint32 phoneRetryAt;     // no network yet: when to look for one again
static GfxRect refreshRect;     // where the card drew Refresh, for the pointer
// RENAMING THE SOURCE: the card's name is the field (F_NAME), filled by the
// keyboard into nameDraft; OK keeps it, Back leaves the name as it was.
static char nameDraft[96];
static GfxRect nameRect;

// --- Small helpers ---------------------------------------------------------------
static float X0(void) { return settings_content_x() - 8.0f; }
// Where the body (the list, the guide) starts: right of the column of groups.
static float BX(void) { return X0() + L_COL_W + L_COL_GAP; }
static int colIndexOf(int g);
static int colShownN(void);
static int colVisible(void);
static int colAt(int k);
static int colPinnedN(void);
static void playingRing(GfxRect tile, float radius);
static int colStep(int g, int dir, int sameKind);
static void headPill(GfxRect r, int focused, float a);
static float labelLine(const char *tag, const char *rest, float x, float y, float a);
static void enterChips(void);
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

// Every message Live TV has, in the app's one notice (notice.h): `icon` is the
// glyph in its ring badge, `kind` what the ring says — NOTE_GOOD for something
// that worked or a hint, NOTE_WARN for something that cannot be done, NOTE_ERROR
// for a channel that failed, NOTE_MODE for the aspect key.
static const char *toastIcon = "notice_check";
static int toastKind;
static Uint32 toastFrom;

static void say(const char *s, const char *icon, int kind) {
  Uint32 now = SDL_GetTicks();
  // Replacing a notice still up keeps its plate where it is; only one arriving
  // on an empty top edge drops in.
  if (!toast[0] || now >= toastUntil) toastFrom = now;
  snprintf(toast, sizeof toast, "%s", s);
  toastIcon = icon; toastKind = kind;
  toastUntil = now + LIVE_TOAST_MS;
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

// A chip's line in iptv_group_order.txt.
static const char *const PIN_KEY[GROUP_FIRST] = { "\x01" "favourites", "\x01" "all", "\x01" "recent" };
static const char *orderKey(int g) { return g < GROUP_FIRST ? PIN_KEY[g] : groupLabel(g); }

static void ensureOrder(void) {
  int n = nGroups(), k = 0;
  char *saved, *line;
  if (nOrder == n && gOrder) return;
  free(gOrder);
  gOrder = malloc((size_t)(n > 0 ? n : 1) * sizeof *gOrder);
  nOrder = 0;
  if (!gOrder) return;
  { unsigned char *used = calloc((size_t)(n > 0 ? n : 1), 1);
    if (!used) { free(gOrder); gOrder = NULL; return; }
    saved = data_read("iptv_group_order.txt");
    for (int g = 0; g < GROUP_FIRST && g < n; g++)
      if (!saved || !strstr(saved, PIN_KEY[g])) { gOrder[k++] = g; used[g] = 1; }
    if (saved) {
      for (line = strtok(saved, "\n"); line; line = strtok(NULL, "\n"))
        for (int g = 0; g < n; g++)
          if (!used[g] && !strcmp(orderKey(g), line)) { gOrder[k++] = g; used[g] = 1; break; }
      free(saved);
    }
    for (int g = 0; g < GROUP_FIRST && g < n; g++) if (!used[g]) gOrder[k++] = g;
    for (int g = GROUP_FIRST; g < n; g++) if (!used[g]) gOrder[k++] = g;
    free(used); }
  nOrder = k;
}
static int posOf(int g) {
  ensureOrder();
  for (int p = 0; p < nOrder; p++) if (gOrder[p] == g) return p;
  return 0;
}
static void saveOrder(void) {
  size_t cap = 1, len = 0;
  char *out;
  for (int p = 0; p < nOrder; p++) cap += strlen(orderKey(gOrder[p])) + 1;
  if (!(out = malloc(cap))) return;
  out[0] = 0;
  for (int p = 0; p < nOrder; p++) {
    size_t n = strlen(orderKey(gOrder[p]));
    memcpy(out + len, orderKey(gOrder[p]), n); len += n;
    out[len++] = '\n'; out[len] = 0;
  }
  data_write("iptv_group_order.txt", out);
  free(out);
}
// The moving chip from position `from` to `to`, the rest closing up behind it.
static void moveChip(int from, int to) {
  int g;
  if (to < 0) to = 0;
  if (to >= nOrder) to = nOrder - 1;
  if (from == to || from < 0) return;
  g = gOrder[from];
  if (to > from) memmove(gOrder + from, gOrder + from + 1, (size_t)(to - from) * sizeof *gOrder);
  else memmove(gOrder + to + 1, gOrder + to, (size_t)(from - to) * sizeof *gOrder);
  gOrder[to] = g;
  saveOrder();
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
  // Two slots, alternated: a caller can draw one row while it still holds a
  // pointer to the other within the same frame.
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
// A PROVIDER'S SECTION HEADER, not a channel: "##### ALGEMEEN HD/4K #####" at
// the top of a group, with a stream that plays a promo loop. Meant to divide a
// flat playlist; here every group is a chip already, so they are left out of
// every list (and so of zapping and the guide).
static int isDivider(int c) {
  const IptvList *l = iptv_list();
  const char *n;
  if (!l || c < 0 || c >= l->nCh) return 0;
  n = l->ch[c].name;
  while (*n == ' ') n++;
  return n[0] == '#' && n[1] == '#' && n[2] == '#';
}

static void pushView(int c) {
  if (isDivider(c)) return;
  if (nView == capView) {
    int cap = capView ? capView * 2 : 256;
    int *v = realloc(view, (size_t)cap * sizeof *v);
    if (!v) return;
    view = v; capView = cap;
  }
  view[nView++] = c;
}

// A GROUP'S LABEL IN TWO PARTS. Providers name groups "UK| SPORT HD", "4K|
// UHD 3840P": a short code, a bar, the name. The chip shows the code as a small
// tracked tag and the name as the label, so the bar and its crowding go. `code`
// is empty when the label has no short code before a bar.
static const char *splitLabel(const char *label, char *code, size_t n) {
  const char *bar = strchr(label, '|'), *a = label, *b;
  size_t len;
  code[0] = 0;
  if (!bar) return label;
  while (*a == ' ') a++;
  b = bar;
  while (b > a && b[-1] == ' ') b--;
  len = (size_t)(b - a);
  if (!len || len > 6 || len >= n) return label;
  for (size_t i = 0; i < len; i++) code[i] = (char)toupper((unsigned char)a[i]);
  code[len] = 0;
  bar++;
  while (*bar == ' ') bar++;
  return *bar ? bar : label;
}
#define CHIP_CODE_GAP 10.0f
// The pinned chips wear a glyph before the label (Phosphor, REGULAR); the
// playlist's groups do not.
#define CHIP_ICON     20.0f
#define CHIP_ICON_GAP  9.0f
static const char *chipIcon(int g) {
  return g == GROUP_FAV ? "chip_fav" : g == GROUP_ALL ? "chip_all" : g == GROUP_RECENT ? "chip_recent" : NULL;
}

// The chips' widths, measured when the groups change and not per frame: a
// provider can carry hundreds of groups, and only the handful on screen are drawn.
static void measureChips(void) {
  int n = nGroups();
  float *w = realloc(chipW, (size_t)n * sizeof *w), *f;
  if (!w) { nChipW = 0; return; }
  chipW = w;
  if (!(f = realloc(chipFull, (size_t)n * sizeof *f))) { nChipW = 0; return; }
  chipFull = f; nChipW = n;
  for (int g = 0; g < n; g++) {
    char code[8];
    const char *name = splitLabel(groupLabel(g), code, sizeof code);
    // Bold is wider than medium: size every chip for the bold, so choosing one
    // does not shove its neighbours along.
    float t = txt_width(TXT_LIVE_META_B, name) + (code[0] ? tagWidth(code) + CHIP_CODE_GAP : 0.0f)
            + (chipIcon(g) ? CHIP_ICON + CHIP_ICON_GAP : 0.0f);
    chipFull[g] = t + 2 * L_CHIP_PAD;
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
// An all-digit query asks for channel numbers, not names.
static int queryIsNumber(void) {
  for (int i = 0; i < queryLen; i++) if (!isdigit((unsigned char)query[i])) return 0;
  return queryLen > 0;
}

static void searchView(const IptvList *l) {
  unsigned char *tier = calloc((size_t)l->nCh, 1);
  long long now = nowSec();
  int digits = queryIsNumber();
  if (!tier) return;
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

// Favourites in the viewer's order (iptv_favourite_rank); channels sharing a
// name share a place, in playlist order.
typedef struct { int rank, ch; } FavRow;
static int favCmp(const void *a, const void *b) {
  const FavRow *x = a, *y = b;
  return x->rank != y->rank ? x->rank - y->rank : x->ch - y->ch;
}
static void favView(const IptvList *l) {
  FavRow *rows = malloc((size_t)(l->nCh > 0 ? l->nCh : 1) * sizeof *rows);
  int n = 0;
  if (!rows) return;
  for (int c = 0; c < l->nCh; c++) {
    int rank = iptv_favourite_rank(c);
    if (rank >= 0) rows[n++] = (FavRow){ rank, c };
  }
  qsort(rows, (size_t)n, sizeof *rows, favCmp);
  for (int i = 0; i < n; i++) pushView(rows[i].ch);
  free(rows);
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
  } else if (group == GROUP_FAV) {
    favView(l);
  } else {
    for (int c = 0; c < l->nCh; c++) {
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

// SEARCHING NARROWS THE CHIPS TOO: the groups whose name holds the query, in
// their places in the viewer's order; with no query (or a channel number) none.
static int chipShown(int g) {
  if (!searching) return 1;
  return queryLen && !queryIsNumber() && g >= GROUP_FIRST && foldFind(groupLabel(g), query) >= 0;
}
static int anyChipShown(void) {
  if (!searching) return 1;
  ensureOrder();
  for (int p = 0; p < nOrder; p++) if (chipShown(gOrder[p])) return 1;
  return 0;
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

// DOWN and UP keep the instant, unless it is a past the new channel has no
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
  if (fChan) { if (anyChipShown()) enterChips(); else requestMenu = 1; return; }
  cellAt(ch, fTime, &s, &e);
  // What is on now is the first column; left of it is the channel itself,
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
  say(m, "notice_alert", NOTE_ERROR);
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
  say("That's further back than this channel keeps", "live_ctl_restart", NOTE_WARN);
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
// resumes in place; a long one comes back to live, and says so, the stream
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
    else { beginLive(); say("Back to live", "live_ctl_live", NOTE_GOOD); }
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

static void refreshAlso(void) {
  long long m = nowMinute();
  if (tuned < 0) { nAlso = 0; alsoFor = -1; return; }
  if (alsoFor == tuned && (alsoAt == m || (alsoMode && quickOpen))) return;
  alsoFor = tuned; alsoAt = m;
  nAlso = iptv_also(tuned, m, alsoCh, IPTV_ALSO_MAX);
}

// The controls on show, in order; the count.
static int shownControls(int *out) {
  int n = 0;
  out[n++] = CTL_PLAY;
  if (restartable()) out[n++] = CTL_RESTART;
  if (!atLive() && srcKind != SRC_LIVE) out[n++] = CTL_LIVE;
  for (int i = CTL_GUIDE; i < CTL_N; i++)
    if (i != CTL_ALSO || (refreshAlso(), nAlso > 1)) out[n++] = i;
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
  { char m[64]; snprintf(m, sizeof m, "No channel %d", number); say(m, "notice_alert", NOTE_WARN); }
}

// Shows overlay `level`, restarting its clock. Called again at the same level
// it only restarts the clock: a run of presses never replays the entrance.
static void overlay(int level) {
  // The bar coming up on its own has no focus; keys give it one.
  if (level != OV_BAR || ov != OV_BAR) { barPassive = 1; barCtl = -1; }
  ov = level;
  ovUntil = SDL_GetTicks() + (level == OV_TOAST ? LIVE_TOAST_OV_MS : LIVE_BAR_MS);
  if (level != OV_WALK) walkPg = -1;
}

// The bar with the focus on control `ctl`, or on the track for -1.
static void focusBar(int ctl) {
  overlay(OV_BAR);
  barPassive = 0;
  barCtl = ctl;
}

// Arriving full screen, the bar says what this is, as information only.
static void enterFull(void) {
  full = 1;
  quickOpen = 0;
  ov = OV_NONE;
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
  { int fav = iptv_is_favourite(ch);
    say(fav ? "Added to Favourites" : "Removed from Favourites", fav ? "live_star_fill" : "live_star", NOTE_GOOD); }
  if (group == GROUP_FAV) rebuildView();
}

// --- Setup form ------------------------------------------------------------------------
// Save and load TRIES the draft (iptv_try_source): the current source stays
// until the new playlist has arrived, and the result, or which thing went
// wrong, is said on this screen, under the fields, never by sending the
// viewer to an empty guide. What they typed is never cleared.
#define SRC_CEILING_S 15

static int configured(void) { return iptv_configured(); }

static int rowCols(int row) {
  switch (row) {
    case SR_SERVER:  return 2;
    case SR_LOGIN:   return 3;
    case SR_BUTTONS: return configured() ? 2 : 1;
    case SR_REFRESH: return 2;            // Rename, Refresh
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
    case F_NAME:   *max = (int)sizeof nameDraft;    return nameDraft;
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
    case F_NAME:   return nameRect;
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
        snprintf(dst, n, "Couldn't reach it. Check the address, or the TV's network connection"); break;
      case IPTV_FAIL_NOT_FOUND:
        snprintf(dst, n, "Nothing there: the address returned 404"); break;
      case IPTV_FAIL_REFUSED:
        snprintf(dst, n, "Refused: the server answered %d. The subscription may have lapsed", status); break;
      case IPTV_FAIL_NOT_PLAYLIST:
        snprintf(dst, n, "Not a playlist: the address sent a web page, usually the provider's sign-in page"); break;
      case IPTV_FAIL_GUIDE:
        snprintf(dst, n, "That's a TV guide, not a playlist. It goes in the extra guide field"); break;
      case IPTV_FAIL_LOGIN:
        snprintf(dst, n, "Sign-in rejected. Check the username, password and port");
        *bad = (1u << F_USER) | (1u << F_PASS) | (1u << F_PORT); break;
      case IPTV_FAIL_EXPIRED:
        snprintf(dst, n, "This subscription has expired. Renew it with your provider");
        *bad = 1u << F_USER; break;
      case IPTV_FAIL_ACCOUNT:
        snprintf(dst, n, "%s", why); *bad = 1u << F_USER; break;
      case IPTV_FAIL_TIMEOUT:
        snprintf(dst, n, "Timed out: nothing came back within %d s", SRC_CEILING_S); break;
      case IPTV_FAIL_PROVIDER:
        snprintf(dst, n, "The provider turned the app away (%d). It may limit devices or connections", status); break;
      case IPTV_FAIL_SERVER:
        snprintf(dst, n, "The server had an error (%d). Try again in a while", status); *bad = 0; break;
      default:
        snprintf(dst, n, "%s", why[0] ? why : "That didn't load"); break;
    }
    return RS_FAIL;
  }
  // No try on this screen yet, or one that worked: what the current source has,
  // only while the form still shows that source, not a half-typed other one.
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
    snprintf(dst, n, "%d channels, no guide data. Add an extra TV guide above", l->nCh);
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
  viewMode = savedView();
  zone = ZONE_BODY;
}

// Opened from Settings' IPTV source: Back and Cancel return there, not to the
// channel list. "Go to channels" still goes to the channels.
static int fromSettings, wantsSettings;
static void leaveSetup(void) {
  if (fromSettings) {
    if (iptv_try_state() == IPTV_LOADING) iptv_try_cancel();
    iptv_try_forget();
    if (ime_is_open()) ime_close();
    editing = -1;
    fromSettings = 0;
    wantsSettings = 1;
    if (configured()) { mode = MODE_BROWSE; viewMode = savedView(); zone = ZONE_BODY; }
    return;
  }
  if (configured()) backToBrowse();
  else wantsExit = 1;
}

// A tried source that became the source: the list is somebody else's now.
static void freshList(void) {
  useAlt = 0;
  group = GROUP_FAV;
  focusName[0] = 0;
  tuned = -1; tunedName[0] = 0;
  nView = 0; fRow = 0;
}

static void saveSetup(void) {
  IptvSource d;
  if (!draftComplete()) {
    say(draft.kind == IPTV_SRC_XTREAM ? "Fill in the server, username and password"
                                      : "Enter the playlist's full address, starting with http",
        "notice_alert", NOTE_WARN);
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
  if (primaryIsDone()) { fromSettings = 0; backToBrowse(); rebuildView(); return; }
  saveSetup();
}

static void editField(int f) {
  if (!ime_usable()) {
    say("This TV has no on-screen keyboard \xC2\xB7 put the address in iptv.txt in the app's data folder",
        "notice_alert", NOTE_WARN);
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
      (k == SDLK_UP || k == SDLK_DOWN || k == SDLK_LEFT || k == SDLK_RIGHT)) {
    if (editing == F_NAME) iptv_set_source_name(nameDraft);
    ime_close(); editing = -1;
  }
  if (isBack(k)) {
    if (ime_is_open()) { ime_close(); editing = -1; return; }   // a rename is dropped
    if (iptv_try_state() == IPTV_LOADING) { iptv_try_cancel(); return; }
    leaveSetup();
    return;
  }
  if (ime_is_open()) {
    // OK with the keyboard up is "done with this field": on to the next stop.
    if (isOk(k)) {
      if (editing == F_NAME) { iptv_set_source_name(nameDraft); ime_close(); editing = -1; return; }
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
      if (row == SR_REFRESH && setupCol == 1) iptv_reload();
      else if (row == SR_REFRESH) {
        // Starts from what it is called now, the host included, to edit or clear.
        snprintf(nameDraft, sizeof nameDraft, "%s", iptv_source_label());
        editField(F_NAME);
      }
      else if (row == SR_KIND) {
        draft.kind = draft.kind == IPTV_SRC_XTREAM ? IPTV_SRC_M3U : IPTV_SRC_XTREAM;
        layoutSetup();
      } else if (row == SR_BUTTONS) {
        if (setupCol == 1) leaveSetup();
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
static void pointPreview(int a, int b) { (void)a; (void)b; zone = ZONE_PREVIEW; }
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
  viewMode = savedView();
  zone = ZONE_BODY;
  group = GROUP_FAV;
  fChan = 0;
  fTime = nowSec();
  winStart = slotFloor(fTime);
  if (!iptv_configured()) openSetup();
  else { mode = MODE_BROWSE; iptv_touch(); rebuildView(); measureChips(); }
  return 1;
}

void iptvui_open_source(void) {
  wantsExit = requestMenu = wantsSettings = 0;
  if (mode != MODE_SETUP) openSetup();
  fromSettings = 1;
}

int iptvui_requested_settings(void) { int v = wantsSettings; wantsSettings = 0; return v; }

void iptvui_resume(void) {
  wantsExit = requestMenu = 0;
  full = 0; quickOpen = 0;
  if (iptv_configured()) iptv_touch();
  if (fTime < nowSec()) fTime = nowSec();
  keepWindowOnFocus();
}

void iptvui_leave(void) {
  fromSettings = 0;
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
  free(chipW); chipW = NULL; free(chipFull); chipFull = NULL; nChipW = 0;
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
    say("Reminder removed", "ctx_bell_off", NOTE_GOOD);
  } else if (nReminders < REMIND_MAX) {
    snprintf(reminders[nReminders].name, sizeof reminders[0].name, "%s", c->name);
    snprintf(reminders[nReminders].title, sizeof reminders[0].title, "%s", l->pg[pg].title);
    reminders[nReminders].number = c->number;
    reminders[nReminders].start = l->pg[pg].start;
    nReminders++;
    say("We'll remind you when it starts", "ctx_bell", NOTE_GOOD);
  }
}

// THE HOLD MENU (see `plate`): on a channel, Watch and Favourites; in the
// guide on a programme, what that programme allows, Favourites last.
static void openChannelMenu(void) {
  const IptvList *l = iptv_list();
  int ch = focusedChannel(), n = 0, fav;
  const IptvChannel *c = chan(ch);
  long long now = nowSec();
  char title[256], meta[256], goTo[96], code[16];
  CtxOption o[7];
  if (!c || !l) return;
  fav = iptv_is_favourite(ch);
  snprintf(menuName, sizeof menuName, "%s", c->name);
  menuStart = 0;
  if (viewMode == VIEW_GUIDE && !fChan) {
    long long s, e;
    int p = cellAt(ch, fTime, &s, &e);
    if (p >= 0) {
      const IptvProgramme *pg = &l->pg[p];
      char a[16], z[16];
      clockText(pg->start, a, sizeof a); clockText(pg->stop, z, sizeof z);
      snprintf(title, sizeof title, "%s", pg->title);
      snprintf(meta, sizeof meta, "%d \xC2\xB7 %s \xC2\xB7 %s \xE2\x80\x93 %s", c->number, c->name, a, z);
      menuStart = pg->start;
      if (pg->stop <= now) {
        if (iptv_has_archive(ch, pg->start))
          o[n++] = (CtxOption){ "Watch from the start", "ctx_play", NULL, M_FROM_START };
        o[n++] = (CtxOption){ "Watch live", "live_ctl_live", NULL, M_WATCH };
      } else if (pg->start > now) {
        int set = reminderIndex(c->name, pg->start) >= 0;
        o[n++] = (CtxOption){ set ? "Remove reminder" : "Remind me", set ? "ctx_bell_off" : "ctx_bell", NULL, M_REMIND };
        o[n++] = (CtxOption){ "Watch the channel now", "ctx_play", NULL, M_WATCH };
      } else {
        o[n++] = (CtxOption){ "Watch", "ctx_play", NULL, M_WATCH };
        if (iptv_has_archive(ch, pg->start))
          o[n++] = (CtxOption){ "Start over", "ctx_restart", NULL, M_FROM_START };
      }
    }
  }
  if (!n) {
    const IptvProgramme *pg = programmeAt(ch, nowMinute());
    snprintf(title, sizeof title, "%s", c->name);
    if (pg) snprintf(meta, sizeof meta, "%d \xC2\xB7 On now \xC2\xB7 %s", c->number, pg->title);
    else snprintf(meta, sizeof meta, "%d", c->number);
    o[n++] = (CtxOption){ "Watch", "ctx_play", NULL, M_WATCH };
  }
  // Its group's chip, when this list is not already that group: "Go to Sport",
  // the provider's short code ("UK") beside it as on the chip.
  if (c->groupIndex >= 0 && (searching || group != GROUP_FIRST + c->groupIndex)) {
    const char *name = splitLabel(groupLabel(GROUP_FIRST + c->groupIndex), code, sizeof code);
    snprintf(goTo, sizeof goTo, "Go to %s", name);
    o[n++] = (CtxOption){ goTo, "ctx_group", code[0] ? code : NULL, M_GROUP };
  }
  // In Favourites, a favourite can change places.
  if (fav && group == GROUP_FAV && !searching && nView > 1)
    o[n++] = (CtxOption){ "Move", "ctx_move", NULL, M_MOVE };
  o[n++] = (CtxOption){ fav ? "Remove from Favourites" : "Add to Favourites", fav ? "ctx_minus" : "ctx_plus",
                        NULL, M_FAV };
  if (plateSet) ctx_set_anchor(plate, plateR);
  ctx_open_custom(title, meta, o, n);
}

static void channelMenuChosen(int id) {
  const IptvList *l = iptv_list();
  int ch = findByName(menuName);
  long long s, e;
  if (ch < 0 || !l) return;
  switch (id) {
    case M_WATCH:      watch(ch); break;
    case M_FROM_START: if (menuStart) watchFrom(ch, menuStart); break;
    case M_REMIND:     if (menuStart) toggleReminder(ch, cellAt(ch, menuStart, &s, &e)); break;
    case M_GROUP: {
      // The group's chip, the focus on the same channel in it.
      const IptvChannel *c = chan(ch);
      if (!c || c->groupIndex < 0) break;
      if (searching) closeSearch();
      chooseGroup(GROUP_FIRST + c->groupIndex);
      if (viewIndex(ch) >= 0) fRow = viewIndex(ch);
      rememberFocus();
      guideSettle();
      break; }
    case M_FAV:        toggleFavourite(ch); break;
    case M_MOVE:
      if (group != GROUP_FAV || searching || viewIndex(ch) < 0) break;
      fRow = viewIndex(ch);
      rememberFocus();
      zone = ZONE_BODY;
      movingChannel = 1;
      say("\xE2\x96\xB2 \xE2\x96\xBC to move \xC2\xB7 \xE2\x97\x80 to the top \xC2\xB7 OK when done", "ctx_move", NOTE_GOOD);
      break;
    default: break;
  }
}

// The panel on the same broadcast elsewhere, its focus on channel `on` (the
// playing one for -1).
static void openAlso(int on) {
  refreshAlso();
  if (nAlso < 2) { say("Not showing anywhere else right now", "live_ctl_also", NOTE_GOOD); return; }
  alsoMode = 1;
  quickOpen = 1;
  quickFromBar = barCtl >= 0;
  quickZone = Q_LIST; gmOpen = 0;
  quickRow = 0;
  for (int i = 0; i < nAlso; i++) if (alsoCh[i] == (on >= 0 ? on : tuned)) quickRow = i;
  quickSnap = 1;
}

static void control(int ctl) {
  switch (ctl) {
    case CTL_GUIDE:
      // Out to the guide, on this channel; the picture follows into the preview.
      full = 0; ov = OV_NONE;
      setView(VIEW_GUIDE); zone = ZONE_BODY; fChan = 0;
      if (viewIndex(tuned) >= 0) fRow = viewIndex(tuned);
      rememberFocus();
      fTime = nowSec(); winStart = slotFloor(fTime);
      break;
    case CTL_ALSO: openAlso(-1); break;
    case CTL_CHANNELS:
      alsoMode = 0;
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
      say(ASPECT_NAME[aspectMode], "aspect", NOTE_MODE);
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
  // From the tuned channel, `step` rows on.
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
// is in it, where it lands on that one, episodes.c's courtesy for a season.
static void quickGroup(int g) {
  if (g < 0 || g >= nGroups() || g == group) return;
  chooseGroup(g);
  quickRow = viewIndex(tuned) >= 0 ? viewIndex(tuned) : 0;
  quickSnap = 1;
}

static void closeQuick(void) {
  quickOpen = 0; gmOpen = 0;
  // From the bar's Channels button, Back returns to the bar and its button.
  if (quickFromBar) focusBar(alsoMode ? CTL_ALSO : CTL_CHANNELS); else ov = OV_NONE;
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
  if (alsoMode) {
    // One list, no pill: ▲▼ along it, OK switches to that copy.
    if (k == SDLK_UP && quickRow > 0) quickRow--;
    else if (k == SDLK_DOWN && quickRow + 1 < nAlso) quickRow++;
    else if (isOk(k) && quickRow < nAlso) {
      int c = alsoCh[quickRow], r = viewIndex(c);
      if (r >= 0) { fRow = r; rememberFocus(); }
      if (c != tuned) tune(c, 1);
      quickOpen = 0;
      overlay(OV_TOAST);
    }
    return;
  }
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

// THE MAGIC REMOTE ON THE FULL SCREEN, the film player's contract (player.c):
// a move wakes the bar, pointing at a control focuses it as the arrows would,
// and a click ACTS on what is under it — never the OK of whatever the keys had
// focused, so the picture between the buttons pauses and nothing else. The
// track seeks to the instant under the pointer, within what can be reached.
static float trackX, trackW;            // the rail, from drawBlock
static double trackStart, trackSpan;    // the programme it spans
static void pointBar(int ctl, int unused) {
  (void)unused;
  if (!full || ov != OV_BAR) return;
  if (!barPassive && barCtl == ctl) { overlay(OV_BAR); return; }
  if (scrubbing && ctl != BAR_TRACK) scrubbing = 0;
  focusBar(ctl);
}
static void clickBar(int ctl, int unused) {
  (void)unused;
  if (!full || ov != OV_BAR) return;
  if (scrubbing && ctl != BAR_TRACK) scrubbing = 0;
  focusBar(ctl);
  if (ctl == BAR_TITLE) { if (nView > 1) startPeek(0); return; }
  if (ctl == BAR_TRACK) { togglePause(); overlay(OV_BAR); return; }
  control(ctl);
  if (ov == OV_BAR) overlay(OV_BAR);
}
static void clickTrack(int unused1, int unused2) {
  double t, fl;
  (void)unused1; (void)unused2;
  if (!full || ov != OV_BAR || trackW <= 0.0f || trackSpan <= 0.0) return;
  t = trackStart + anim_clamp((pointer_x() - trackX) / trackW, 0.0f, 1.0f) * trackSpan;
  fl = rewindFloor();
  if (fl > 0 && t < fl) t = fl;
  seekTo(t);
  focusBar(BAR_TRACK);
}
// A row of the carousel: tuned, as OK on it would be.
static void clickPeek(int r, int unused) {
  (void)unused;
  if (!full || ov != OV_PEEK || !nView) return;
  peekPos = r;
  peekRow = ((r % nView) + nView) % nView;
  fRow = peekRow; rememberFocus();
  if (view[peekRow] != tuned) tune(view[peekRow], 1);
  focusBar(BAR_TITLE);
}
// Anywhere else on the picture: away from a sheet or the carousel, else pause.
static void clickPicture(int a, int b) {
  (void)a; (void)b;
  if (!full) return;
  if (quickOpen) {
    if (gmOpen) gmOpen = 0;
    else if (pointer_x() < NV_SCREEN_W - NV_EPL_VEIL_W) closeQuick();
    return;
  }
  if (ov == OV_PEEK) { focusBar(BAR_TITLE); return; }
  if (ov == OV_WALK) { focusBar(BAR_TRACK); return; }
  if (scrubbing) seekTo(scrubAt); else togglePause();
  if (ov == OV_BAR && !barPassive) overlay(OV_BAR); else focusBar(CTL_PLAY);
}

static void fullEvent(SDL_Keycode k) {
  int d = digitOf(k);
  int chUp = k == SDLK_PAGEUP, chDown = k == SDLK_PAGEDOWN;
  if (quickOpen) { quickEvent(k); return; }
  if (mediaKey(k)) return;
  if (d >= 0) { typeDigit(d); return; }
  // CH+ and CH- change channel whatever is up.
  if (chUp || chDown) { scrubbing = 0; zap(chUp ? +1 : -1); overlay(OV_TOAST); return; }

  if (ov == OV_PEEK) {
    if (k == SDLK_UP || k == SDLK_DOWN) {
      peekPos += k == SDLK_DOWN ? 1 : -1;
      peekRow = ((peekPos % nView) + nView) % nView;
      overlay(OV_PEEK);
    }
    else if (isOk(k) && peekRow < nView) {
      // The one press that retunes.
      fRow = peekRow; rememberFocus();
      if (view[peekRow] != tuned) tune(view[peekRow], 1);
      focusBar(BAR_TITLE);
    }
    else if (isBack(k)) focusBar(BAR_TITLE);
    else overlay(OV_PEEK);
    return;
  }

  if (ov == OV_WALK) {
    if (k == SDLK_RIGHT) { int p = walkNext(walkPg); if (p >= 0) walkPg = p; overlay(OV_WALK); }
    else if (k == SDLK_LEFT) {
      int p = walkPrev(walkPg);
      if (p >= 0) { overlay(OV_WALK); walkPg = p; } else focusBar(BAR_TRACK);
    }
    else if (isOk(k)) { int p = walkPg; toggleReminder(tuned, p); overlay(OV_WALK); walkPg = p; }
    else if (k == SDLK_UP) focusBar(BAR_TITLE);
    else if (k == SDLK_DOWN || isBack(k)) focusBar(BAR_TRACK);
    return;
  }

  if (ov != OV_BAR || barPassive) {
    // Nothing up, or the bar only informing.
    // OK pauses, and the focus lands on Pause: the next OK is Play.
    if (isOk(k)) {
      if (scrubbing) { seekTo(scrubAt); if (ov != OV_BAR) overlay(OV_BAR); }
      else { togglePause(); focusBar(CTL_PLAY); }
    }
    else if (k == SDLK_UP && alsoNoticeFor == tuned && SDL_GetTicks() < alsoNoticeUntil) {
      alsoNoticeUntil = 0; toastUntil = 0;
      openAlso(alsoNoticeCh);
    }
    else if (k == SDLK_UP || k == SDLK_DOWN || k == SDLK_i) focusBar(CTL_PLAY);
    else if (k == SDLK_LEFT || k == SDLK_RIGHT) { scrubbing = 0; zap(k == SDLK_RIGHT ? +1 : -1); overlay(OV_TOAST); }
    else if (isBack(k)) {
      if (scrubbing) { scrubbing = 0; return; }
      // Back to the screen; the picture carries on in the preview.
      full = 0; ov = OV_NONE;
      zone = ZONE_BODY; fChan = 0;
      if (viewIndex(tuned) >= 0) fRow = viewIndex(tuned);
      rememberFocus();
      fTime = nowSec();
      keepWindowOnFocus();
    }
    return;
  }

  if (barCtl >= 0) {
    // On the buttons.
    if (k == SDLK_LEFT) barStep(-1);
    else if (k == SDLK_RIGHT) barStep(+1);
    else if (isOk(k)) control(barCtl);
    else if (k == SDLK_UP) barCtl = BAR_TRACK;
    else if (k == SDLK_DOWN || isBack(k)) { ov = OV_NONE; return; }
    if (ov == OV_BAR) overlay(OV_BAR);
    return;
  }
  if (barCtl == BAR_TITLE) {
    if (isOk(k) && nView > 1) startPeek(0);
    else if (k == SDLK_DOWN) { barCtl = BAR_TRACK; overlay(OV_BAR); }
    else if (k == SDLK_UP || isBack(k)) ov = OV_NONE;
    else overlay(OV_BAR);
    return;
  }
  // On the track. Rewound, ◀▶ move through time; at live, ◀ rewinds when
  // there is a past to rewind into and ▶ walks the schedule, the future being
  // the only direction left.
  if (k == SDLK_RIGHT) {
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
  else if (isOk(k)) { if (scrubbing) seekTo(scrubAt); else togglePause(); overlay(OV_BAR); }
  else if (k == SDLK_DOWN) { if (scrubbing) seekTo(scrubAt); barCtl = shownFirst(); overlay(OV_BAR); }
  else if (k == SDLK_UP) { if (scrubbing) seekTo(scrubAt); barCtl = BAR_TITLE; overlay(OV_BAR); }
  else if (isBack(k)) { if (scrubbing) { scrubbing = 0; overlay(OV_BAR); } else ov = OV_NONE; }
}

// The field, where the circle was, at full size: laid out in drawHeader, where
// the header's widths are known, and kept for the keyboard to sit clear of.
static GfxRect searchRect = { 960.0f, L_BTN_Y, 720.0f, L_BTN_H };

static void raiseKeyboard(void) {
  if (!ime_usable()) { say("This TV has no on-screen keyboard", "notice_alert", NOTE_WARN); return; }
  if (!ime_is_open()) ime_open(searchRect);
}

static void openSearch(void) {
  searching = 1;
  query[0] = 0; queryLen = 0;
  zone = ZONE_HEAD; headSel = HEAD_SEARCH;
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
  if (headSel == HEAD_CLEAR) headSel = HEAD_SEARCH;
  rememberFocus();
  rebuildView();
}

static void clearQuery(void) {
  query[0] = 0; queryLen = 0;
  focusName[0] = 0;
  rebuildView();
  scrollRows = 0.0f; scrollRowsV = 0.0f;
  headSel = HEAD_SEARCH;
}

// What the header offers right now: the clear button only with something to
// clear, the source pill only when not searching.
static int headHas(int b) {
  if (b == HEAD_CLEAR) return searching && queryLen > 0;
  if (b == HEAD_SOURCE) return !searching;
  return b >= 0 && b < HEAD_N;
}

// Down (or OK on the keyboard) to the results; nowhere with none.
static void toResults(void) {
  if (!nView) return;
  zone = ZONE_BODY;
  fTime = nowSec();
  keepWindowOnFocus();
}

// The row under the header: the chips, or while searching the matching chips
// when there are any, else the results.
static void enterChips(void) {
  zone = ZONE_CHIPS;
  if (searching && !chipShown(group)) { int g = colAt(0); if (g >= 0) chooseGroup(g); }
}
// Down from the header: to the list (or the guide) under it; the column of
// groups is a step to the left from there.
static void belowHead(void) { if (nView) toResults(); else if (anyChipShown()) enterChips(); }

static void headEvent(SDL_Keycode k) {
  // Keys while the TV's keyboard is up belong to it, except OK ("done
  // typing": to the results) and arrows it did not take, which lower it.
  if (searching && headSel == HEAD_SEARCH && ime_is_open()) {
    if (isOk(k)) {
      ime_close();
      if (nView) { fRow = 0; rememberFocus(); toResults(); }
      return;
    }
    if (ime_shown() || !(k == SDLK_UP || k == SDLK_DOWN || k == SDLK_LEFT || k == SDLK_RIGHT)) return;
    ime_close();
  }
  if (!headHas(headSel)) headSel = HEAD_SEARCH;
  if (k == SDLK_LEFT) {
    int b = headSel - 1;
    while (b >= 0 && !headHas(b)) b--;
    if (b >= 0) headSel = b; else requestMenu = 1;
  }
  else if (k == SDLK_RIGHT) {
    int b = headSel + 1;
    while (b < HEAD_N && !headHas(b)) b++;
    if (b < HEAD_N) headSel = b;
  }
  else if (k == SDLK_DOWN) belowHead();
  else if (isOk(k)) {
    if (headSel == HEAD_SOURCE) openSetup();
    else if (headSel == HEAD_SEARCH) { if (searching) raiseKeyboard(); else openSearch(); }
    else if (headSel == HEAD_CLEAR) { clearQuery(); raiseKeyboard(); }
    else {
      // ONE STOP, OK FLIPS IT: a switch, not two buttons to aim between. The
      // same header either way, search included: switching changes only the body.
      setView(viewMode == VIEW_GUIDE ? VIEW_LIST : VIEW_GUIDE);
      fChan = 0;
      fTime = nowSec();
      winStart = slotFloor(fTime);
    }
  }
}

static void chipsEvent(SDL_Keycode k) {
  int p = posOf(group);
  if (movingGroup) {
    // A group moves among its own kind: the pinned three among themselves, the
    // provider's groups among theirs, so the line between them never moves.
    int up = colStep(group, -1, 1), down = colStep(group, +1, 1), g, last = group;
    if (k == SDLK_UP) { if (up >= 0) moveChip(p, posOf(up)); }
    else if (k == SDLK_DOWN) { if (down >= 0) moveChip(p, posOf(down)); }
    else if (k == SDLK_LEFT) { g = group; while ((g = colStep(g, -1, 1)) >= 0) last = g; if (last != group) moveChip(p, posOf(last)); }
    else if (k == SDLK_RIGHT) { g = group; while ((g = colStep(g, +1, 1)) >= 0) last = g; if (last != group) moveChip(p, posOf(last)); }
    else if (isOk(k) || isBack(k)) movingGroup = 0;
    return;
  }
  // THE COLUMN: ▲ ▼ walk the groups (each one applied at once), ▲ from the
  // first reaches the header, ◀ opens the side bar, ▶ and OK go to the list.
  if (k == SDLK_UP) {
    int g = colStep(group, -1, 0);
    if (g >= 0) chooseGroup(g); else { zone = ZONE_HEAD; headSel = HEAD_SWITCH; }
  }
  else if (k == SDLK_DOWN) { int g = colStep(group, +1, 0); if (g >= 0) chooseGroup(g); }
  else if (k == SDLK_LEFT) { if (!searching) requestMenu = 1; }
  else if (k == SDLK_PAGEUP || k == SDLK_PAGEDOWN) {
    int g = group, d = k == SDLK_PAGEUP ? -1 : +1;
    for (int i = 0; i < colVisible() - 1; i++) { int n = colStep(g, d, 0); if (n < 0) break; g = n; }
    if (g != group) chooseGroup(g);
  }
  // OK on a matching chip: that group, the search put away.
  else if (isOk(k) && searching) { closeSearch(); zone = nView ? ZONE_BODY : ZONE_CHIPS; fTime = nowSec(); }
  else if ((k == SDLK_RIGHT || isOk(k)) && nView) {
    zone = ZONE_BODY;
    fTime = nowSec();
    keepWindowOnFocus();
  }
}

// Moving a favourite: it takes the place of the one at `row` in the list.
static void moveFavouriteTo(int row) {
  if (row < 0 || row >= nView || row == fRow) return;
  iptv_move_favourite(view[fRow], view[row]);
  rememberFocus();
  rebuildView();
  guideSettle();
}
static void movingEvent(SDL_Keycode k) {
  if (k == SDLK_UP) moveFavouriteTo(fRow - 1);
  else if (k == SDLK_DOWN) moveFavouriteTo(fRow + 1);
  else if (k == SDLK_PAGEUP) moveFavouriteTo(fRow - 6 < 0 ? 0 : fRow - 6);
  else if (k == SDLK_PAGEDOWN) moveFavouriteTo(fRow + 6 >= nView ? nView - 1 : fRow + 6);
  else if (k == SDLK_LEFT) moveFavouriteTo(0);
  else if (k == SDLK_RIGHT) moveFavouriteTo(nView - 1);
  else if (isOk(k) || isBack(k)) movingChannel = 0;
}

// The panel's picture is a thing to choose while it shows the channel playing.
static int previewShowsTuned(void) {
  return viewMode == VIEW_LIST && (ownsVideo() || zapPending || bufWaiting) && chan(tuned);
}
static void previewEvent(SDL_Keycode k) {
  if (!previewShowsTuned()) { zone = ZONE_BODY; return; }
  if (k == SDLK_LEFT) { zone = ZONE_ACTIONS; actSel = ACT_FAV; }
  else if (k == SDLK_UP) { zone = ZONE_HEAD; headSel = HEAD_SWITCH; }
  else if (k == SDLK_DOWN) zone = ZONE_BODY;
  else if (isOk(k)) watch(tuned);
}

// THE ROW'S STAR (2026-11): the panel lost its buttons, Watch to OK on the row
// or on the picture, Start over to the hold menu, and Favourite to a star at
// the end of the focused row. ▶ from the row reaches it, ▶ again the picture;
// ▲ ▼ keep it on as the rows go by.
static void actionsEvent(SDL_Keycode k) {
  actSel = ACT_FAV;
  if (k == SDLK_LEFT) zone = ZONE_BODY;
  else if (k == SDLK_RIGHT) { if (previewShowsTuned()) zone = ZONE_PREVIEW; }
  else if (k == SDLK_UP) {
    if (fRow > 0) { fRow--; rememberFocus(); guideSettle(); }
    else { zone = ZONE_HEAD; headSel = HEAD_SWITCH; }
  }
  else if (k == SDLK_DOWN) { if (fRow + 1 < nView) { fRow++; rememberFocus(); guideSettle(); } }
  else if (isOk(k)) toggleFavourite(focusedChannel());
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
  // Moving a chip, every key is the move's (Back included). The OK that
  // started it is still down: its repeats are not "done", and its release
  // belongs to the hold, which swallows it. Arrows repeat, so holding ▶ walks.
  if (movingGroup && zone == ZONE_CHIPS) {
    if (e->type == SDL_KEYUP) { hold_event(&hold, e, 0, &tap); return; }
    if (e->type == SDL_KEYDOWN && !(e->key.repeat && isOk(e->key.keysym.sym)))
      chipsEvent(e->key.keysym.sym);
    return;
  }
  // Moving a favourite, the same: every key is the move's.
  if (movingChannel && zone == ZONE_BODY) {
    if (e->type == SDL_KEYUP) { hold_event(&hold, e, 0, &tap); return; }
    if (e->type == SDL_KEYDOWN && !(e->key.repeat && isOk(e->key.keysym.sym)))
      movingEvent(e->key.keysym.sym);
    return;
  }
  // Hold OK on a channel or programme: its menu; on a chip: move it. A tap is OK.
  if (hold_event(&hold, e, (zone == ZONE_BODY && nView > 0) || (zone == ZONE_CHIPS && anyChipShown()), &tap)) {
    if (tap && zone == ZONE_CHIPS) chipsEvent(SDLK_RETURN);
    else if (tap) {
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
    // Back climbs, one step at a time: the actions to their row; the list or
    // the guide up to the chips; the chips open the side bar. The preview
    // plays on until the screen is left. Searching, Back lowers the keyboard
    // and keeps what is typed; the next ends the search and brings every
    // channel back.
    if (searching && ime_is_open()) { ime_close(); return; }
    if (searching && zone == ZONE_ACTIONS) { zone = ZONE_BODY; return; }
    if (searching) { closeSearch(); if (zone == ZONE_HEAD) headSel = HEAD_SEARCH; return; }
    if (zone == ZONE_ACTIONS || zone == ZONE_PREVIEW) { zone = ZONE_BODY; return; }
    if (zone == ZONE_HEAD) { zone = ZONE_CHIPS; return; }
    if (zone == ZONE_BODY) { zone = ZONE_CHIPS; fChan = 0; return; }
    requestMenu = 1;
    return;
  }
  if (zone == ZONE_HEAD) { headEvent(k); return; }
  if (zone == ZONE_CHIPS && searching && !anyChipShown()) zone = ZONE_HEAD;
  // A held ◀ on the column never opens the side bar: only a fresh press does.
  if (zone == ZONE_CHIPS && k == SDLK_LEFT && e->key.repeat) return;
  if (zone == ZONE_CHIPS) { chipsEvent(k); return; }
  if (zone == ZONE_ACTIONS) { actionsEvent(k); return; }
  if (zone == ZONE_PREVIEW) { previewEvent(k); return; }

  { int d = digitOf(k); if (d >= 0) { typeDigit(d); return; } }
  switch (k) {
    case SDLK_UP:
      if (fRow > 0) { fRow--; rememberFocus(); guideSettle(); }
      else { zone = ZONE_HEAD; headSel = searching ? HEAD_SEARCH : HEAD_SWITCH; }
      break;
    case SDLK_DOWN: if (fRow + 1 < nView) { fRow++; rememberFocus(); guideSettle(); } break;
    case SDLK_PAGEUP:   pageRows(-6); break;
    case SDLK_PAGEDOWN: pageRows(+6); break;
    case SDLK_LEFT:
      if (viewMode == VIEW_GUIDE) guideLeft();
      else if (anyChipShown()) enterChips();
      else requestMenu = 1;
      break;
    case SDLK_RIGHT:
      if (viewMode == VIEW_GUIDE) guideRight(); else { zone = ZONE_ACTIONS; actSel = ACT_FAV; }
      break;
    default: break;
  }
}

// --- Update -----------------------------------------------------------------------------
static float followRow(float current, int row, int visible, int n) {
  // From the NEAREST row, not the one below: the spring settles from beneath
  // and stops a float step short (4.9999998), which truncated to the row
  // above and sent the grid bobbing between the two about once a second.
  float target = floorf(current + 0.5f);
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
  return row >= 0 && row < QN() ? programmeAt(QCH(row), nowMinute()) : NULL;
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
  if (quickOpenedCap < QN()) {
    float *o = realloc(quickOpened, (size_t)QN() * sizeof *o);
    if (!o) return;
    for (int i = quickOpenedCap; i < QN(); i++) o[i] = 0.0f;
    quickOpened = o; quickOpenedCap = QN();
  }
  if (quickRow >= QN()) quickRow = QN() ? QN() - 1 : 0;
  // Only the focused row opens, and none while the pill has the cursor.
  for (int i = 0; i < QN(); i++) {
    float want = i == quickRow && quickZone == Q_LIST ? 1.0f : 0.0f;
    if (quickSnap) quickOpened[i] = want;
    else if (quickOpened[i] > 0.0f || want > 0.0f)
      quickOpened[i] = anim_spring(quickOpened[i], want, dt, NV_SPRING_GRID);
  }
  // The focused row held in the window's middle, whole rows at the top, the
  // list stopping at both ends, episodes.c's offset, measured where the rows
  // are heading.
  { float fh = NV_EPL_ROW_H + quickOpenExtra(quickProgramme(quickRow));
    float total = QN() * Q_PITCH - NV_EPL_ROW_GAP + (fh - NV_EPL_ROW_H);
    float max = total - Q_VIEW_H;
    float target = quickRow * Q_PITCH - (Q_VIEW_H - fh) * 0.5f;
    target = roundf(target / Q_PITCH) * Q_PITCH;
    if (target > max) target = max;
    if (target < 0) target = 0;
    quickScroll = quickSnap ? target : anim_spring(quickScroll, target, dt, NV_SPRING_GRID); }
  if (QN()) quickSnap = 0;
}

// What is decoding, noted against the channel (iptv_quality_note); then, full
// screen, the notice when a copy elsewhere has a better picture than this one.
static void alsoStep(Uint32 t) {
  const IptvChannel *c = chan(tuned);
  const IptvProgramme *pg;
  char key[300], m[300], q[16];
  int mine, best = -1;
  if (!c || !ownsVideo() || playFailed || !srcReadyAt || srcKind == SRC_ARCHIVE) return;
  // Before then video_height() may still be the channel before.
  if (t - srcReadyAt < LIVE_SETTLE_MS || video_height() <= 0) return;
  if (qualityNotedFor != tunedAt) {
    qualityNotedFor = tunedAt;
    iptv_quality_note(tuned, video_width(), video_height(), video_frame_rate_milli());
    alsoFor = -1;
  }
  if (!full) return;
  pg = programmeAt(tuned, nowSec());
  snprintf(key, sizeof key, "%s\x01%lld", c->name, pg ? pg->start : 0LL);
  if (!strcmp(key, alsoNoticeKey)) return;
  snprintf(alsoNoticeKey, sizeof alsoNoticeKey, "%s", key);
  refreshAlso();
  mine = iptv_height_quality(video_height());
  // Best first: the first that is not this one is the best of the rest.
  for (int i = 0; i < nAlso && best < 0; i++) if (alsoCh[i] != tuned) best = alsoCh[i];
  if (best < 0 || iptv_quality(best) <= mine) return;
  snprintf(m, sizeof m, "Sharper elsewhere: %s on %s \xC2\xB7 \xE2\x96\xB2 to see",
           iptv_quality_label(best, q, sizeof q), chan(best)->name);
  say(m, "live_ctl_also", NOTE_GOOD);
  toastUntil = t + LIVE_NOTICE_MS;
  alsoNoticeCh = best; alsoNoticeFor = tuned; alsoNoticeUntil = toastUntil;
  printf("[live] %s: %s is sharper (%s)\n", c->name, chan(best)->name, q);
  fflush(stdout);
}

void iptvui_update(float dt, Uint32 now) {
  long long t = nowSec();
  int reduced = settings_animations_reduced();
  if (iptv_step()) {
    // Same playlist text: same indices. A different one: find them by name.
    int c = findByName(tunedName);
    hiReset();
    if (c < 0 && tuned >= 0) { stopStream(); tuned = -1; full = 0; }
    else tuned = c;
    rebuildView();
    alsoFor = -1;
    nOrder = 0; movingGroup = 0; movingChannel = 0;
    measureChips();
  }
  phoneStep(now);
  if (mode != MODE_BROWSE) return;

  hold_animate(&hold, dt, now);
  { int id = ctx_requested_custom(); if (id >= 0) channelMenuChosen(id); }
  if (hold_fired(&hold, now)) {
    if (zone == ZONE_BODY) openChannelMenu();
    else if (zone == ZONE_CHIPS) {
      movingGroup = 1;
      say("\xE2\x96\xB2 \xE2\x96\xBC to move \xC2\xB7 \xE2\x97\x80 to the top \xC2\xB7 OK when done", "ctx_move", NOTE_GOOD);
    }
  }
  if (zone != ZONE_CHIPS) movingGroup = 0;
  if (zone != ZONE_BODY || group != GROUP_FAV || searching) movingChannel = 0;

  // Time moves on under a screen left open, unless the focus is on a past
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
  alsoStep(now);
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
      say("Catch-up isn't available for this programme", "live_ctl_restart", NOTE_WARN);
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

  // The overlay's clock, the film player's: it goes when the keys rest,
  // focus or not, except under a sheet it opened.
  if (barCtl >= 0 && shownIndex(barCtl) < 0) barCtl = CTL_PLAY;
  // THE POINTER WAKES THE BAR as a key would (player.c does the same for its
  // transport): up with no focus until it points at something, and resting on
  // the control it focused keeps it up — aiming must not time out under it.
  { static Uint32 seenMove;
    Uint32 moved = pointer_moved_at();
    if (moved != seenMove) {
      seenMove = moved;
      if (full && pointer_active() && !quickOpen && !tracks_is_open()) {
        if (ov == OV_NONE || ov == OV_TOAST) overlay(OV_BAR);
        else ovUntil = now + LIVE_BAR_MS;
      }
    }
    if (full && ov == OV_BAR && !barPassive && pointer_active() && pointer_over(pointBar, barCtl, 0))
      ovUntil = now + LIVE_BAR_MS; }
  // Paused or choosing an instant, the bar stays: it is what says so.
  if (full && ov != OV_NONE && now >= ovUntil && !tracks_is_open() && !quickOpen &&
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
  { float target = full && ov == OV_BAR && !barPassive && barCtl == BAR_TITLE && nView > 1 ? 1.0f : 0.0f;
    titleFocus = anim_spring(titleFocus, target, dt, target > titleFocus ? NV_SPRING_FOCUS : NV_SPRING_BLUR); }
  // The fill has stages as the film player's does: the keys still moving, the
  // recorder connecting, the pipeline opening. Slow inside one (tau 1.6s), brisk
  // at the finish (0.12s), and back to empty on every new channel.
  // Full screen or in a preview: whenever this screen's stream is on its way.
  { int loading = tuned >= 0 && !playFailed && (zapPending || bufWaiting || (ownsVideo() && !video_ready()));
    float cap = zapPending ? 0.15f : bufWaiting ? 0.40f : loading ? 0.92f : 1.0f;
    if (loading && (tuned != tuneFor || !tuneWas)) {
      if (tuned != tuneFor || now - tuneEndAt >= LIVE_LOAD_OUTRO_MS) { tuneFill = 0.0f; tuneBeganAt = now; }
      tuneFor = tuned; tuneEndAt = 0;
    }
    tuneFill += (cap - tuneFill) * (1.0f - expf(-dt / (loading ? 1.6f : 0.12f)));
    if (tuneWas && !loading) tuneEndAt = playFailed ? 0 : now;
    tuneWas = loading; }
  peekScroll = anim_spring2_reduced(&peekScrollV, peekScroll, (float)peekPos, dt, NV_SPRING2_PAGE, reduced);
  for (int i = 0; i < CTL_N; i++) {
    float target = (full && ov == OV_BAR && !barPassive && barCtl == i && shownIndex(i) >= 0) ? 1.0f : 0.0f;
    ctlFocus[i] = anim_spring(ctlFocus[i], target, dt, target > ctlFocus[i] ? NV_SPRING_FOCUS : NV_SPRING_BLUR);
  }

  // Reminders due: said once, within ten minutes of the start.
  for (int i = 0; i < nReminders; i++) {
    if (t < reminders[i].start) continue;
    if (t < reminders[i].start + 600) {
      char m[300];
      snprintf(m, sizeof m, "Starting now on %d \xC2\xB7 %s", reminders[i].number, reminders[i].title);
      say(m, "ctx_bell", NOTE_GOOD);
    }
    reminders[i--] = reminders[--nReminders];
  }

  { int visible = viewMode == VIEW_GUIDE ? (int)((G_BOTTOM - G_GRID_Y) / G_ROW_STEP)
                                         : (int)((NV_SCREEN_H - 60.0f - C_LIST_Y) / C_ROW_STEP);
    scrollRows = anim_spring2_reduced(&scrollRowsV, scrollRows,
                                      followRow(scrollRows, fRow, visible, nView), dt,
                                      NV_SPRING2_PAGE, reduced); }

  // The column of groups scrolls to keep the chosen one a line from either edge.
  // Typing can hide the chosen group: onto the first that still matches.
  if (searching && zone == ZONE_CHIPS && !chipShown(group)) {
    if (anyChipShown()) enterChips(); else { zone = ZONE_HEAD; movingGroup = 0; }
  }
  { int np = colPinnedN(), k = colIndexOf(group) - np, n = colShownN() - np, vis = colVisible();
    float target = k < 0 ? chipScroll : followRow(chipScroll / L_COL_STEP, k, vis, n) * L_COL_STEP;
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

// A thin progress bar: the track, and the accent up to `frac`.
static void progressBar(float x, float y, float w, float h, float frac, float track, float a) {
  float r, g, b;
  if (frac < 0.0f) frac = 0.0f;
  if (frac > 1.0f) frac = 1.0f;
  accent(&r, &g, &b);
  gfx_color((GfxRect){ x, y, w, h }, 0.5f, 1.0f, 1.0f, 1.0f, track * a);
  if (frac * w >= 1.0f) gfx_color((GfxRect){ x, y, w * frac, h }, 0.5f, r, g, b, a);
}

// THE REDESIGN'S BAR: Nuvio's ramp ONLY on the channel that is playing; every
// other one a quiet white (or ink, on the white of a focused row). Colour then
// says one thing on this screen: this is what is on.
static void liveBar(float x, float y, float w, float h, float frac, int playing, int onPaper, float a) {
  float fw;
  if (frac < 0.0f) frac = 0.0f;
  if (frac > 1.0f) frac = 1.0f;
  fw = w * frac;
  if (onPaper) gfx_color((GfxRect){ x, y, w, h }, 0.5f, 0.0f, 0.0f, 0.0f, 0.10f * a);
  else gfx_color((GfxRect){ x, y, w, h }, 0.5f, 1.0f, 1.0f, 1.0f, 0.10f * a);
  if (fw < 1.0f) return;
  if (fw < h) fw = h;
  if (playing) gfx_seek_brand((GfxRect){ x, y, fw, h }, 0.5f, gfx_seek_brand_span(fw, w), a);
  else if (onPaper) gfx_color((GfxRect){ x, y, fw, h }, 0.5f, 0.0f, 0.0f, 0.0f, 0.45f * a);
  else gfx_color((GfxRect){ x, y, fw, h }, 0.5f, 1.0f, 1.0f, 1.0f, 0.50f * a);
}

// A vertical line in Nuvio's ramp, cyan at the top to violet at the foot: the
// guide's now line. Stacked flat slices, overlapping a pixel so no seam shows.
static void brandLineV(float x, float top, float w, float h, float a) {
  const int N = 16;
  for (int i = 0; i < N; i++) {
    float r, g, b, y0 = top + h * i / N;
    gfx_seek_brand_at((float)i / (float)(N - 1), &r, &g, &b);
    gfx_color((GfxRect){ x, y0, w, h / N + 1.0f }, 0.0f, r, g, b, a);
  }
}

// --- What a provider calls a channel, made readable --------------------------
// "4K: SKY SPORTS DARTS UHD 3840P" shows as "Sky Sports Darts", and the 4K goes
// to its own mark after it. What goes: a leading code with a colon or a bar
// ("NL:", "UK|", "|UK|", "4K:"), anything in brackets, and quality words at the
// end (UHD, 3840P, FHD, HD, RAW, HEVC, 50FPS...). A name all in capitals is set
// in title case, short words kept as they are (BBC, TNT, NPO, PL): those are
// acronyms far more often than not, and so are the four-letter ones listed.
// Whatever cannot be cleaned stays as it came; a name never comes out empty.
static int qualityWord(const char *w, size_t n, int *uhd) {
  static const char *const UHD[] = { "4K", "UHD", "2160P", "3840P", "4K+" };
  static const char *const GONE[] = { "FHD", "HD", "SD", "HEVC", "H265", "H.265", "H264", "RAW",
                                      "1080P", "1080", "720P", "50FPS", "60FPS", "HDR", "HDR10", "DOLBY",
                                      "VISION", "DV", "HQ", "LQ", "BACKUP", "ᴴᴰ", "ᵁᴴᴰ" };
  if (n == 2 && !strncasecmp(w, "8K", 2)) { *uhd = 2; return 1; }
  for (size_t i = 0; i < sizeof UHD / sizeof *UHD; i++)
    if (strlen(UHD[i]) == n && !strncasecmp(w, UHD[i], n)) { if (*uhd < 1) *uhd = 1; return 1; }
  for (size_t i = 0; i < sizeof GONE / sizeof *GONE; i++)
    if (strlen(GONE[i]) == n && !strncasecmp(w, GONE[i], n)) return 1;
  return 0;
}
// PROVIDERS DECORATE WITH LOOK-ALIKES: "ZIGGO SPORT 2 ᴿᴬᵂ", "ᴴᴰ", "⁶⁰ᶠᵖˢ",
// the modifier letters and superscripts Unicode keeps for phonetics. A word made
// only of those is spelled out in plain letters (0 if any character is not one),
// so the quality test can read it; a word that is all decoration goes either way.
static int plainOf(const char *w, size_t n, char *out, size_t on) {
  static const struct { unsigned cp; char c; } MAP[] = {
    { 0x1D2C, 'A' }, { 0x1D2E, 'B' }, { 0x1D30, 'D' }, { 0x1D31, 'E' }, { 0x1D33, 'G' }, { 0x1D34, 'H' },
    { 0x1D35, 'I' }, { 0x1D36, 'J' }, { 0x1D37, 'K' }, { 0x1D38, 'L' }, { 0x1D39, 'M' }, { 0x1D3A, 'N' },
    { 0x1D3C, 'O' }, { 0x1D3E, 'P' }, { 0x1D3F, 'R' }, { 0x1D40, 'T' }, { 0x1D41, 'U' }, { 0x1D42, 'W' },
    { 0x1D43, 'a' }, { 0x1D47, 'b' }, { 0x1D48, 'd' }, { 0x1D49, 'e' }, { 0x1D4D, 'g' }, { 0x1D4F, 'k' },
    { 0x1D50, 'm' }, { 0x1D52, 'o' }, { 0x1D56, 'p' }, { 0x1D57, 't' }, { 0x1D58, 'u' }, { 0x1D5B, 'v' },
    { 0x02B0, 'h' }, { 0x02B3, 'r' }, { 0x02B7, 'w' }, { 0x02E2, 's' }, { 0x02E3, 'x' }, { 0x1DA0, 'f' },
    { 0x2071, 'i' }, { 0x207F, 'n' }, { 0x2070, '0' }, { 0x00B9, '1' }, { 0x00B2, '2' }, { 0x00B3, '3' },
    { 0x2074, '4' }, { 0x2075, '5' }, { 0x2076, '6' }, { 0x2077, '7' }, { 0x2078, '8' }, { 0x2079, '9' },
    { 0x207A, '+' }, { 0x1D00, 'A' }, { 0x0299, 'B' }, { 0x1D04, 'C' }, { 0x1D05, 'D' }, { 0x1D07, 'E' },
    { 0x0262, 'G' }, { 0x029C, 'H' }, { 0x026A, 'I' }, { 0x1D0B, 'K' }, { 0x029F, 'L' }, { 0x1D0D, 'M' },
    { 0x0274, 'N' }, { 0x1D0F, 'O' }, { 0x1D18, 'P' }, { 0x0280, 'R' }, { 0x1D1B, 'T' }, { 0x1D1C, 'U' },
    { 0x1D20, 'V' }, { 0x1D21, 'W' }, { 0x028F, 'Y' }, { 0x1D22, 'Z' } };
  size_t k = 0, i = 0;
  int fancy = 0;
  while (i < n && k + 1 < on) {
    unsigned char b = (unsigned char)w[i];
    unsigned cp;
    int len;
    if (b < 0x80) { out[k++] = (char)b; i++; continue; }
    if ((b & 0xE0) == 0xC0 && i + 1 < n) { cp = (b & 0x1Fu) << 6 | ((unsigned char)w[i + 1] & 0x3Fu); len = 2; }
    else if ((b & 0xF0) == 0xE0 && i + 2 < n) {
      cp = (b & 0x0Fu) << 12 | ((unsigned char)w[i + 1] & 0x3Fu) << 6 | ((unsigned char)w[i + 2] & 0x3Fu); len = 3;
    } else return 0;
    { size_t m;
      for (m = 0; m < sizeof MAP / sizeof *MAP && MAP[m].cp != cp; m++) {}
      if (m == sizeof MAP / sizeof *MAP) return 0;
      out[k++] = MAP[m].c; }
    fancy = 1;
    i += (size_t)len;
  }
  out[k] = 0;
  return fancy ? 2 : 1;
}
// A word that is only symbols: "◉", "★", "⚡", "🔴". Only the symbol and emoji
// blocks count, never letters of another script: a channel named in Hebrew or
// Arabic keeps every word.
static int decoWord(const char *w, size_t n) {
  size_t i = 0;
  if (!n) return 0;
  while (i < n) {
    unsigned char b = (unsigned char)w[i];
    unsigned cp;
    int len;
    if (b < 0x80) { if (isalnum(b)) return 0; i++; continue; }
    if ((b & 0xF0) == 0xE0 && i + 2 < n) {
      cp = (b & 0x0Fu) << 12 | ((unsigned char)w[i + 1] & 0x3Fu) << 6 | ((unsigned char)w[i + 2] & 0x3Fu); len = 3;
    } else if ((b & 0xF8) == 0xF0 && i + 3 < n) {
      cp = (b & 0x07u) << 18 | ((unsigned char)w[i + 1] & 0x3Fu) << 12 | ((unsigned char)w[i + 2] & 0x3Fu) << 6 |
           ((unsigned char)w[i + 3] & 0x3Fu); len = 4;
    } else return 0;
    if (!((cp >= 0x2000 && cp <= 0x2BFF) || (cp >= 0x1F000 && cp <= 0x1FAFF) || cp == 0xFE0F || cp == 0x200D))
      return 0;
    i += (size_t)len;
  }
  return 1;
}
static int keepCaps(const char *w, size_t n) {
  static const char *const ACR[] = { "ESPN", "DAZN", "CNBC", "MSNBC", "HBO2", "VTM", "VRT", "ZDF", "ARD",
                                     "RTBF", "BVN", "NHK", "KBS", "WDR", "NDR", "SWR", "ORF", "SRF" };
  // Short words are acronyms (BBC, TNT, NPO, PL) unless they are ordinary words.
  static const char *const WORD[] = { "ONE", "TWO", "SIX", "TEN", "AIR", "ON", "THE", "AND", "OF", "FOR",
                                      "NEW", "ALL", "MAX", "PRO", "BOX", "FUN", "HIT", "SKY", "BOX", "DE",
                                      "EN", "LA", "LE", "EL", "DU", "DIE", "DER", "DAS", "UND", "TOP", "BIG",
                                      "RED", "GOLD", "ART", "OUT", "LIVE", "PLAY", "STAR", "NOW" };
  for (size_t i = 0; i < sizeof WORD / sizeof *WORD; i++)
    if (strlen(WORD[i]) == n && !strncasecmp(w, WORD[i], n)) return 0;
  if (n <= 3) return 1;
  for (size_t i = 0; i < n; i++) if (isdigit((unsigned char)w[i])) return 1;
  for (size_t i = 0; i < sizeof ACR / sizeof *ACR; i++)
    if (strlen(ACR[i]) == n && !strncasecmp(w, ACR[i], n)) return 1;
  return 0;
}
static void titleCase(char *s) {
  int lower = 0;
  for (char *p = s; *p; p++) if (islower((unsigned char)*p)) { lower = 1; break; }
  if (lower) return;
  for (char *p = s; *p;) {
    char *w = p;
    size_t n;
    while (*p && *p != ' ' && *p != '-' && *p != '/') p++;
    n = (size_t)(p - w);
    if (n && !keepCaps(w, n) && ((unsigned char)w[0] & 0x80) == 0)
      for (size_t i = 1; i < n; i++)
        if (((unsigned char)w[i] & 0x80) == 0) w[i] = (char)tolower((unsigned char)w[i]);
    while (*p == ' ' || *p == '-' || *p == '/') p++;
  }
}
static const char *cleanName(const char *raw, int *uhd) {
  static char out[4][160];
  static int next;
  char *o = out[next++ & 3], tmp[160];
  const char *p = raw;
  size_t k = 0;
  int u = 0;
  if (uhd) *uhd = 0;
  if (!raw || !raw[0]) return raw ? raw : "";
  // The leading codes, up to three of them: "4K: NL:", "|UK| ".
  for (int round = 0; round < 3; round++) {
    const char *q = p, *e;
    while (*q == ' ' || *q == '|') q++;
    e = q;
    while (*e && *e != ':' && *e != '|' && *e != ' ' && e - q <= 6) e++;
    if (e > q && e - q <= 5 && (*e == ':' || *e == '|')) {
      if (qualityWord(q, (size_t)(e - q), &u)) {}
      p = e + 1;
      while (*p == ' ' || *p == '|' || *p == '-') p++;
      continue;
    }
    break;
  }
  // Brackets out; the words kept, with their spacing collapsed.
  for (int depth = 0; *p && k + 1 < sizeof tmp; p++) {
    if (*p == '[' || *p == '(') {
      const char *e = strchr(p, *p == '[' ? ']' : ')');
      if (e) {
        if (qualityWord(p + 1, (size_t)(e - p - 1), &u)) {}
        p = e;
        continue;
      }
    }
    if (*p == '|') continue;
    if (*p == ' ' && (k == 0 || tmp[k - 1] == ' ')) continue;
    tmp[k++] = *p;
    (void)depth;
  }
  while (k && (tmp[k - 1] == ' ' || tmp[k - 1] == '-' || tmp[k - 1] == '|' || tmp[k - 1] == ':')) k--;
  tmp[k] = 0;
  // Quality words off the end, as many as there are, the look-alike ones too.
  for (;;) {
    char *sp = strrchr(tmp, ' ');
    char *w = sp ? sp + 1 : tmp, plain[48];
    int kind;
    if (!sp) break;
    kind = plainOf(w, strlen(w), plain, sizeof plain);
    if (!(kind && qualityWord(plain, strlen(plain), &u)) && kind != 2 && !decoWord(w, strlen(w))) break;
    *sp = 0;
    while (sp > tmp && (sp[-1] == ' ' || sp[-1] == '-' || sp[-1] == '|')) *--sp = 0;
  }
  if (!tmp[0]) snprintf(tmp, sizeof tmp, "%s", raw);
  titleCase(tmp);
  snprintf(o, sizeof out[0], "%s", tmp);
  if (!u) {
    // A quality word the name kept in its middle still counts.
    const char *q = raw;
    while (*q) {
      const char *w;
      while (*q && !isalnum((unsigned char)*q)) q++;
      w = q;
      while (*q && (isalnum((unsigned char)*q) || *q == '+')) q++;
      if (q > w && qualityWord(w, (size_t)(q - w), &u) && u) break;
    }
  }
  if (uhd) *uhd = u;
  return o;
}

// A vertical fade into the background, in bands: `top` is transparent,
// `top + h` is the page. For the bottom edge the mockups dissolve lists into.
// `amount` scales it: a list that ends on screen has nothing to dissolve into.
static void fadeBottom(float top, float h, float amount) {
  const int N = 13;
  for (int i = 0; i < N; i++) {
    float t = ((float)i + 0.5f) / (float)N;
    float a = (t < 0.56f ? t / 0.56f * 0.9f : 0.9f + (t - 0.56f) / 0.44f * 0.1f) * amount;
    gfx_color((GfxRect){ 0.0f, top + h * i / N, NV_SCREEN_W, h / N + 0.5f }, 0.0f,
              NV_COLOR_BACKGROUND_R, NV_COLOR_BACKGROUND_G, NV_COLOR_BACKGROUND_B, a);
  }
}

// --- Channel identity, Y3's three tiers -----------------------------------------------------
// Two or three letters from the name: the first letter of its first two words,
// and a number word's digits ("News 24" -> N24). Provider noise, "|UK|",
// "[FHD]", "UK:", HD/FHD/4K tags, is not a word.
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

// THE BETTER LOGO (tvlogos.h): the tv-logos mark for a channel the index knows,
// resolved the first time the channel is drawn and kept per channel, since a
// playlist can be tens of thousands of them. "" when there is none.
static const IptvList *hiList;
static char **hiLogo;
static int hiN;

static void hiReset(void) {
  for (int i = 0; i < hiN; i++) if (hiLogo[i] && hiLogo[i][0]) free(hiLogo[i]);
  free(hiLogo);
  hiLogo = NULL; hiN = 0; hiList = NULL;
}

// The logo to draw and its texture at `w`: the tv-logos one, else the
// provider's, which also stands in while the better one downloads. 0 when
// neither is here yet.
static GLuint logoTex(const IptvChannel *c, float w, const char **path) {
  const IptvList *l = iptv_list();
  const char *hi = "";
  GLuint tex = 0;
  int i = l ? (int)(c - l->ch) : -1;
  if (l && (l != hiList || l->nCh != hiN)) {
    hiReset();
    if ((hiLogo = calloc((size_t)l->nCh, sizeof *hiLogo))) { hiList = l; hiN = l->nCh; }
  }
  if (i >= 0 && i < hiN) {
    if (!hiLogo[i]) {
      char u[512];
      hiLogo[i] = tvlogos_find(c->name, u, sizeof u) ? strdup(u) : NULL;
      if (!hiLogo[i]) hiLogo[i] = (char *)"";
    }
    hi = hiLogo[i];
  }
  *path = c->logo;
  if (hi[0] && !tex_failed(hi)) {
    *path = hi;
    if ((tex = tex_get_width(hi, w))) return tex;
  }
  if (c->logo[0] && !tex_failed(c->logo) && (tex = tex_get_width(c->logo, w))) *path = c->logo;
  return tex;
}

// The tile is ALWAYS drawn and ALWAYS filled: a logo on the plate with 9px of
// air, else the monogram, else the bare number. A logo that failed looks exactly
// like a channel that never had one, never a broken-image glyph, and the tex
// cache remembers the failure, so a dead URL is asked for once.
static void identity(const IptvChannel *c, GfxRect r, float radius, unsigned plate, unsigned inkHex,
                     float a) {
  // A DARK MARK, DRAWN WHITE. Many providers' logos are black or grey
  // lettering on a transparent ground, made for a white page; on the dark plate
  // they all but vanish. Those (tex_brand_dark: dark and colourless where
  // solid, with a transparent margin, so not an opaque picture) are tinted to
  // the ink by their alpha, as the film player draws a title's dark logo. They
  // used to sit on a light plate, which left a grey mark on a grey tile.
  const char *logo;
  GLuint tex = logoTex(c, r.w - 18.0f, &logo);
  int tint = 0;
  if (tex && tex_brand_dark(logo)) {
    float vis[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    if (tex_content_box(logo, vis) &&
        (vis[0] > 0.01f || vis[1] > 0.01f || vis[2] < 0.99f || vis[3] < 0.99f))
      tint = 1;
  }
  gfx_color(r, radius / r.h, HEXF(plate), a);
  {
    if (tex) {
      // THE MARK, NOT THE FILE. IPTV logos come with any amount of transparent
      // margin, one provider's are padded to a square, the next one's are
      // cropped to the letters, so the same box drew some marks at a third of
      // the size of others. The visible part (tex_content_box) is what gets
      // fitted, through the texture cell, with air in proportion to the tile.
      float air = r.h * 0.14f < 9.0f ? 9.0f : r.h * 0.14f;
      GfxRect box = { r.x + air, r.y + air, r.w - air * 2.0f, r.h - air * 2.0f };
      float asp = tex_aspect(logo), w, h, vis[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
      if (asp <= 0.0f) asp = 1.0f;
      if (!tex_content_box(logo, vis) || vis[2] - vis[0] < 0.02f || vis[3] - vis[1] < 0.02f) {
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
      { GfxRect m = { box.x + (box.w - w) * 0.5f, box.y + (box.h - h) * 0.5f, w, h };
        if (tint) gfx_rect(m, tex, GFX_BRAND, 0, 0, 0, 0.0f, 0.95f, 0.95f, 0.97f, 1.0f);
        else gfx_texture(m, tex); }
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
// The header's glyphs: 22 beside a label (the switch, the source), 24 alone in
// the search circle. At 18 and 20 they read undersized next to the 21px text.
#define HD_ICON          22.0f
#define HD_ICON_SEARCH   24.0f
#define HD_DISC          44.0f   // the glyph's own disc in a header button
static void headRing(GfxRect r) {
  gfx_rect((GfxRect){ r.x - HD_RING - 2.0f, r.y - HD_RING - 2.0f, r.w + (HD_RING + 2.0f) * 2.0f,
                      r.h + (HD_RING + 2.0f) * 2.0f },
           0, GFX_RING_INSET, 0, HD_RING / (r.h + (HD_RING + 2.0f) * 2.0f), 0, 0.5f, 1, 1, 1, 0.92f);
}

// The search control: at e = 0 the circle, at 1 the field, and between the two
// one pill growing, never two crossfading. The magnifier stays at the left.
// SEARCH AT REST IS A BUTTON LIKE ITS NEIGHBOURS: the glass in a disc and the
// word, where it was a bare circle; open, it is a field across the header.
static float searchRestW(void) { return 7.0f + HD_DISC + 14.0f + txt_width(TXT_LIVE_META_B, "Search") + 26.0f; }
static void headField(GfxRect r, float e, int focused) {
  float ca = e < 0.5f ? 0.0f : (e - 0.5f) * 2.0f;   // the field's contents
  float la = e < 0.5f ? 1.0f - e * 2.0f : 0.0f;     // the resting label
  float ix = r.x + 7.0f + (HD_DISC - HD_ICON_SEARCH) * 0.5f + (20.0f - 7.0f - (HD_DISC - HD_ICON_SEARCH) * 0.5f) * e;
  float right = r.x + r.w - 8.0f;
  if (focused && !searching) headPill(r, 1, 1.0f);
  else {
    gfx_color(r, 0.5f, 1.0f, 1.0f, 1.0f, focused ? 0.16f : searching ? 0.10f : 0.07f);
    gfx_rect(r, 0, GFX_RING_INSET, 0, 1.5f / r.h, 0, 0.5f, 1, 1, 1, 0.09f);
    if (focused) headRing(r);
  }
  if (la > 0.0f) {
    int on = focused && !searching;
    GfxRect d = { r.x + 7.0f, r.y + (r.h - HD_DISC) * 0.5f, HD_DISC, HD_DISC };
    if (on) gfx_color(d, 0.5f, HEXF(C_INK), 0.08f * la);
    else gfx_color(d, 0.5f, 1, 1, 1, 0.10f * la);
    inkMid(TXT_LIVE_META_B, "Search", on ? C_INK : 0xF5F6F8, r.x + 7.0f + HD_DISC + 14.0f, r.y + r.h * 0.5f,
           r.w - (7.0f + HD_DISC + 14.0f), la);
  }
  gfx_icon((GfxRect){ ix, r.y + (r.h - HD_ICON_SEARCH) * 0.5f, HD_ICON_SEARCH, HD_ICON_SEARCH }, "search_glass",
           HEXF(focused && !searching ? C_INK : 0xF5F6F8), 1.0f);
  pointer_zone(r.x, r.y, r.w, r.h, pointHead, HEAD_SEARCH, 0);
  if (ca <= 0.0f) return;
  // The clear button, then how many match: the count answers every key, even
  // with the list off to the side.
  if (queryLen) {
    GfxRect x = { right - 34.0f, r.y + (r.h - 34.0f) * 0.5f, 34.0f, 34.0f };
    int on = zone == ZONE_HEAD && headSel == HEAD_CLEAR;
    const IptvList *l = iptv_list();
    char count[40];
    unsigned chipBg = 0, chipInk = 0xC1C7CD;
    gfx_color(x, 0.5f, 1.0f, 1.0f, 1.0f, (on ? 0.24f : 0.10f) * ca);
    if (on) headRing(x);
    gfx_icon((GfxRect){ x.x + 10.0f, x.y + 10.0f, 14.0f, 14.0f }, "search_clear", HEXF(on ? 0xF5F6F8 : 0xC1C7CD), ca);
    pointer_zone(x.x, x.y, x.w, x.h, pointHead, HEAD_CLEAR, 0);
    right = x.x - 10.0f;
    if (nView) snprintf(count, sizeof count, "%d of %d", nView, l ? l->nCh : nView);
    else { snprintf(count, sizeof count, "none"); chipBg = 0x4A3027; chipInk = 0xF0BFA8; }
    { TxtLine t = txt_line(TXT_LIVE_META_B, count, HEXI(chipInk), 255);
      GfxRect c = { right - (t.w + 28.0f), r.y + (r.h - 32.0f) * 0.5f, t.w + 28.0f, 32.0f };
      if (chipBg) gfx_color(c, 0.5f, HEXF(chipBg), ca);
      else gfx_color(c, 0.5f, 1.0f, 1.0f, 1.0f, 0.10f * ca);
      txt_draw_alpha(t, c.x + 14.0f, c.y + (c.h - t.h) * 0.5f, ca);
      right = c.x - 16.0f; }
  }
  { float tx = r.x + 20.0f + HD_ICON_SEARCH + 16.0f, maxW = right - tx;
    int digits = queryIsNumber();
    if (maxW < 40.0f) return;
    if (queryLen) {
      TxtLine t = txt_line(TXT_SRCP_VALUE, query, HEXI(0xF5F6F8), 255);
      const char *q = query;
      // The end is what is being typed: trim from the left.
      while (t.w > maxW && *q) {
        q++;
        while (((unsigned char)*q & 0xC0) == 0x80) q++;
        t = txt_line(TXT_SRCP_VALUE, q, HEXI(0xF5F6F8), 255);
      }
      txt_draw_alpha(t, tx, r.y + (r.h - t.h) * 0.5f, ca);
      tx += t.w;
      // Digits are a different question: say which, inline.
      if (digits && right - tx > 200.0f)
        inkMid(TXT_LIVE_NAME, "channel number", 0x6E757D, tx + 22.0f, r.y + r.h * 0.5f, right - tx - 22.0f, ca);
    } else {
      inkMid(TXT_LIVE_NAME, "Name or number", 0x6E757D, tx, r.y + r.h * 0.5f, maxW, ca);
    }
    if (ime_is_open() && (SDL_GetTicks() / 530u) % 2u == 0u)
      gfx_color((GfxRect){ tx + (queryLen ? 1.0f : -2.0f), r.y + 13.0f, 2.0f, r.h - 26.0f }, 0.0f, 1, 1, 1, ca); }
}

// THE VIEW BUTTON: ONE button, naming the view it goes TO ("Guide" in the list,
// "List" in the guide). It was a two-segment switch, Guide | List with a pill
// under the one on show; the owner found the switch heavy for a single press,
// which is all it ever was (OK flips it). The glyph sits in its own disc, as the
// source's does, so the header reads as a row of buttons, not labels.
static const char *toggleLabel(void) { return viewMode == VIEW_GUIDE ? "List" : "Guide"; }
static float toggleWidth(void) {
  return 7.0f + HD_DISC + 14.0f + txt_width(TXT_LIVE_META_B, "Guide") + 26.0f;
}
static void headDisc(float x, float cy, const char *icon, int focused, float a) {
  GfxRect d = { x, cy - HD_DISC * 0.5f, HD_DISC, HD_DISC };
  gfx_color(d, 0.5f, 1, 1, 1, (focused ? 0.0f : 0.10f) * a);
  if (focused) gfx_color(d, 0.5f, HEXF(C_INK), 0.08f * a);
  gfx_icon((GfxRect){ d.x + (HD_DISC - HD_ICON) * 0.5f, cy - HD_ICON * 0.5f, HD_ICON, HD_ICON }, icon,
           HEXF(focused ? C_INK : 0xF5F6F8), a);
}
// A header button's ground: white with the focus, glass with a hairline without.
static void headPill(GfxRect r, int focused, float a) {
  if (focused) {
    gfx_drop_shadow(r, r.h * 0.5f, 24.0f, 8.0f, 0.5f * a);
    gfx_color(r, 0.5f, HEXF(C_PAPER), a);
  } else {
    gfx_color(r, 0.5f, 1, 1, 1, 0.07f * a);
    gfx_rect(r, 0, GFX_RING_INSET, 0, 1.5f / r.h, 0, 0.5f, 1, 1, 1, 0.09f * a);
  }
}
static GfxRect headSwitch(float right, float dt) {
  int focused = zone == ZONE_HEAD && headSel == HEAD_SWITCH;
  float w = toggleWidth();
  GfxRect box = { right - w, L_BTN_Y, w, L_BTN_H };
  float cy = box.y + box.h * 0.5f;
  (void)dt;
  headPill(box, focused, 1.0f);
  headDisc(box.x + 7.0f, cy, viewMode == VIEW_GUIDE ? "live_list" : "live_grid", focused, 1.0f);
  inkMid(TXT_LIVE_META_B, toggleLabel(), focused ? C_INK : 0xF5F6F8, box.x + 7.0f + HD_DISC + 14.0f, cy, w, 1.0f);
  pointer_zone(box.x, box.y, box.w, box.h, pointHead, HEAD_SWITCH, 0);
  return box;
}

// The source and what it holds ("Strong · 56,708 channels"); while it loads,
// what it is doing instead.
static GfxRect headSource(float right, float maxW, int focused, float a) {
  const IptvList *l = iptv_list();
  const char *st = iptv_status();
  char more[200];
  TxtLine name, rest;
  float w, restMax;
  unsigned nameInk = focused ? C_INK : 0xF5F6F8, restInk = focused ? 0x55555C : 0x8E8E95;
  if (st[0]) snprintf(more, sizeof more, "%s", st);
  else if (l) {
    // Thousands grouped: 56,708 reads at a glance, 56708 does not.
    char n[24], g[32];
    int len, o = 0;
    snprintf(n, sizeof n, "%d", l->nCh);
    len = (int)strlen(n);
    for (int i = 0; i < len; i++) { if (i && (len - i) % 3 == 0) g[o++] = ','; g[o++] = n[i]; }
    g[o] = 0;
    snprintf(more, sizeof more, "%s channels", g);
  } else more[0] = 0;
  name = txt_line(TXT_LIVE_META_B, iptv_source_label(), HEXI(nameInk), 255);
  restMax = maxW - (7.0f + HD_DISC + 14.0f + name.w + 14.0f + 26.0f);
  if (restMax < 60.0f) restMax = 60.0f;
  rest = txt_line_trim(TXT_SRC_TEXT, more, HEXI(restInk), 255, restMax);
  w = 7.0f + HD_DISC + 14.0f + name.w + (more[0] ? 14.0f + rest.w : 0.0f) + 26.0f;
  { GfxRect r = { right - w, L_BTN_Y, w, L_BTN_H };
    float x = r.x + 7.0f, cy = r.y + r.h * 0.5f;
    if (a <= 0.0f) return r;
    headPill(r, focused, a);
    headDisc(x, cy, "live_source", focused, a);
    x += HD_DISC + 14.0f;
    txt_draw_alpha(name, x, cy - name.h * 0.5f, a);
    x += name.w + 14.0f;
    if (more[0]) txt_draw_alpha(rest, x, cy - rest.h * 0.5f, a);
    if (a >= 1.0f) pointer_zone(r.x, r.y, r.w, r.h, pointHead, HEAD_SOURCE, 0);
    return r; }
}

static void drawHeader(void) {
  float x = X0(), right = L_RIGHT, e, dt;
  static Uint32 last;
  Uint32 now = SDL_GetTicks();
  GfxRect src, field, rest, open;
  float switchW = toggleWidth();
  float sw0, sw1, fieldR, fieldW;
  dt = last ? (float)(now - last) / 1000.0f : 1.0f;
  last = now;
  searchT = anim_ramp(searchT, searching ? 1.0f : 0.0f, dt, 220.0f);
  e = searchT * searchT * (3.0f - 2.0f * searchT);
  { TxtLine t = txt_line(TXT_TITLE3, "Live TV", 245, 246, 248, 255);
    txt_draw(t, x, L_BTN_Y + L_BTN_H * 0.5f - t.h * 0.5f);
    x += t.w + 60.0f; }
  // Laid out right to left; the source pill gives up width to a long status.
  // Searching, it folds away, Guide | List takes its place at the edge and the
  // circle grows into what that frees and more.
  src = headSource(right, right - x - switchW - 16.0f - searchRestW() - 16.0f,
                   zone == ZONE_HEAD && headSel == HEAD_SOURCE && !searching, 1.0f - e);
  sw0 = src.x - 16.0f - switchW;
  sw1 = right - switchW;
  rest = (GfxRect){ sw0 - 16.0f - searchRestW(), L_BTN_Y, searchRestW(), L_BTN_H };
  fieldR = sw1 - 16.0f;
  // Open, it takes the header from the title to the view button.
  fieldW = fieldR - x;
  open = (GfxRect){ fieldR - fieldW, L_BTN_Y, fieldW, L_BTN_H };
  searchRect = open;
  headSwitch(sw0 + (sw1 - sw0) * e + switchW, dt);
  field = (GfxRect){ rest.x + (open.x - rest.x) * e, L_BTN_Y, rest.w + (open.w - rest.w) * e, L_BTN_H };
  headField(field, e, zone == ZONE_HEAD && headSel == HEAD_SEARCH);
}

// THE COLUMN OF GROUPS, which replaced the chips' carousel (2026-11): Favourites,
// Recent and All channels, then the provider's groups, one per line down the
// left, in the viewer's order. A provider's code ("NL|", "UK|") is a small tag
// in the icon's place; the pinned three carry their glyph. The chosen group is
// a light plate; with the column focused it is the white plate, as a focused
// row is. It scrolls to keep the chosen group a line from either edge.
// THE COLUMN'S ORDER: the pinned three first (Favourites, Recent, All channels,
// in the viewer's order among themselves), then the provider's groups in theirs.
// The pinned ones stay put while the rest scroll, with a line between.
static int *colArr, colCap, colN;
static void colBuild(void) {
  ensureOrder();
  if (colCap < nOrder) {
    int *a = realloc(colArr, (size_t)(nOrder > 0 ? nOrder : 1) * sizeof *a);
    if (!a) { colN = 0; return; }
    colArr = a; colCap = nOrder;
  }
  colN = 0;
  for (int pass = 0; pass < 2; pass++)
    for (int p = 0; p < nOrder; p++) {
      int g = gOrder[p];
      if ((g < GROUP_FIRST) != (pass == 0) || !chipShown(g)) continue;
      colArr[colN++] = g;
    }
}
static int colAt(int k) { colBuild(); return k >= 0 && k < colN ? colArr[k] : -1; }
static int colIndexOf(int g) {
  colBuild();
  for (int k = 0; k < colN; k++) if (colArr[k] == g) return k;
  return 0;
}
static int colShownN(void) { colBuild(); return colN; }
static int colPinnedN(void) {
  int n = 0;
  colBuild();
  while (n < colN && colArr[n] < GROUP_FIRST) n++;
  return n;
}
// The group after (or before) `g` in the column; with `sameKind`, only among
// its own kind. -1 at the end.
static int colStep(int g, int dir, int sameKind) {
  int k = colIndexOf(g) + dir;
  colBuild();
  if (k < 0 || k >= colN) return -1;
  if (sameKind && (colArr[k] < GROUP_FIRST) != (g < GROUP_FIRST)) return -1;
  return colArr[k];
}
// Where the scrolling part starts, and how many of its lines fit.
#define L_COL_SEP_GAP 22.0f
static float colScrollTop(void) {
  int np = colPinnedN();
  return np ? L_BODY_Y + np * L_PIN_STEP + L_COL_SEP_GAP : L_BODY_Y;
}
static int colVisible(void) { return (int)((NV_SCREEN_H - 40.0f - colScrollTop()) / L_COL_STEP); }

static void drawGroupItem(int g, float y) {
  float x = X0();
  int on = g == group, focus = on && zone == ZONE_CHIPS, paper = focus && !movingGroup;
  int big = g < GROUP_FIRST;
  float ih = big ? L_PIN_ITEM_H : L_COL_ITEM_H, icn = big ? 30.0f : L_COL_ICON;
  GfxRect r = { x, y, L_COL_W, ih };
  char code[8], name[160];
  const char *icon = chipIcon(g);
  float ix = r.x + 20.0f, cy = r.y + r.h * 0.5f, tx = r.x + 20.0f + icn + 18.0f;
  float ia = on ? 1.0f : 0.45f, ta = on ? 1.0f : 0.62f;
  unsigned ink = paper ? C_INK : 0xFFFFFF;
  snprintf(name, sizeof name, "%s", splitLabel(groupLabel(g), code, sizeof code));
  titleCase(name);
  if (paper) {
    gfx_drop_shadow(r, 14.0f, 30.0f, 12.0f, 0.5f);
    gfx_color(r, 14.0f / r.h, HEXF(C_PAPER), 1.0f);
    // No ring: the group turns white. The ring is the hold's own sweep, and only
    // while OK is held (moving it starts when the sweep closes).
    hold_sweep_inset(&hold, r, (3.0f + NV_HOLD_RING_GROW * hold_grow(&hold)) / r.h, 14.0f / r.h, 1.0f);
  } else if (on) {
    gfx_color(r, 14.0f / r.h, 1, 1, 1, focus ? 0.16f : 0.09f);
  }
  if (focus && movingGroup) {
    float ar, ag, ab;
    accent(&ar, &ag, &ab);
    gfx_rect((GfxRect){ r.x - 5.0f, r.y - 5.0f, r.w + 10.0f, r.h + 10.0f }, 0, GFX_RING_INSET, 0,
             3.0f / (r.h + 10.0f), 0, 19.0f / (r.h + 10.0f), ar, ag, ab, 1.0f);
    gfx_icon((GfxRect){ r.x + r.w - 40.0f, cy - 22.0f, 20.0f, 20.0f }, "live_caret_up", ar, ag, ab, 1.0f);
    gfx_icon((GfxRect){ r.x + r.w - 40.0f, cy + 2.0f, 20.0f, 20.0f }, "live_caret_down", ar, ag, ab, 1.0f);
  }
  if (icon) gfx_icon((GfxRect){ ix, cy - icn * 0.5f, icn, icn }, icon, HEXF(ink), ia);
  else if (code[0]) {
    float cw = tagWidth(code);
    tagText(code, paper ? 0x55555C : 0xFFFFFF, ix + (L_COL_ICON - cw) * 0.5f, cy, paper ? 1.0f : 0.45f);
  }
  { TxtLine t = txt_line_trim(big ? (on ? TXT_LIVE_H2 : TXT_LIVE_PIN) : on ? TXT_LIVE_BODY_B : TXT_LIVE_BODY, name,
                              HEXI(ink), 255,
                              r.x + r.w - 16.0f - (movingGroup && focus ? 40.0f : 0.0f) - tx);
    txt_draw_alpha(t, tx, cy - t.h * 0.5f, paper ? 1.0f : ta); }
  pointer_zone(r.x, r.y, r.w, r.h, pointChip, g, 0);
}

static void drawGroups(void) {
  float x = X0(), top, bottom = NV_SCREEN_H;
  int np;
  if (searching && !anyChipShown()) {
    TxtLine t = txt_line(TXT_LIVE_LABEL, "x", 255, 255, 255, 255);
    txt_tracking(TXT_LIVE_LABEL, queryLen ? "MATCHES" : "ALL CHANNELS", 255, 255, 255, x + 20.0f,
                 L_BODY_Y + (L_COL_ITEM_H - t.h) * 0.5f, 0.45f, 2.0f);
    return;
  }
  colBuild();
  np = colPinnedN();
  // The pinned ones, where they always are; then the line.
  pointer_clip(0.0f, L_BODY_Y - 4.0f, x + L_COL_W + 8.0f, bottom - L_BODY_Y + 4.0f);
  for (int k = 0; k < np; k++) drawGroupItem(colArr[k], L_BODY_Y + k * L_PIN_STEP);
  pointer_no_clip();
  top = colScrollTop();
  if (np && np < colN)
    gfx_color((GfxRect){ x + 20.0f, top - L_COL_SEP_GAP * 0.5f - 1.0f - 3.0f, L_COL_W - 40.0f, 2.0f }, 0.5f, 1, 1, 1, 0.10f);
  // The provider's groups, scrolling under the line and never over it.
  gfx_crop(0.0f, top - 8.0f, x + L_COL_W + 24.0f, bottom - top + 8.0f);
  pointer_clip(0.0f, top - 4.0f, x + L_COL_W + 8.0f, bottom - top + 4.0f);
  for (int k = np; k < colN; k++) {
    float y = top + (k - np) * L_COL_STEP - chipScroll;
    if (y + L_COL_ITEM_H < top - 8.0f) continue;
    if (y > bottom) break;
    drawGroupItem(colArr[k], y);
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
  float left;
  if (!toast[0] || now >= toastUntil) return;
  // Top centre in the guide and full screen alike, as in the film player, and
  // fading out over its last 200 ms.
  left = (float)(toastUntil - now);
  notice_draw(toast, toastIcon, toastKind, left < 200.0f ? left / 200.0f : 1.0f, toastFrom, now);
}

static void drawDigits(void) {
  TxtLine t;
  GfxRect r;
  if (!digits[0]) return;
  t = txt_line(TXT_TITLE1, digits, 255, 255, 255, 255);
  // The picture's top right: the screen's full screen, the header's otherwise.
  r = (GfxRect){ L_RIGHT - t.w - 64.0f, iptvui_fullscreen() ? 56.0f : L_BODY_Y, t.w + 64.0f,
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

// A darkening from the bottom of a picture up, for the text laid over it: one
// shader gradient (GFX_VEIL_BOTTOM), rounded like the picture's own corners.
// It was eight flat strips, each a step darker, and on a bright picture the
// steps showed as bands.
static void pictureShade(GfxRect r, float h, float radius, float a) {
  gfx_rect((GfxRect){ r.x, r.y + r.h - h, r.w, h }, 0, GFX_VEIL_BOTTOM, 0, 0, 0, radius / h, 0, 0, 0, a);
}

// What stands in for a picture when nothing is tuned: the plate, the focused
// channel's tile large.
static void pictureIdle(GfxRect r, float radius, int ch, float tile) {
  const IptvChannel *c = chan(ch);
  gfx_color(r, radius / r.h, HEXF(0x14171B), 1.0f);
  if (c) identity(c, (GfxRect){ r.x + (r.w - tile) * 0.5f, r.y + r.h * 0.42f - tile * 0.5f, tile, tile },
                  tile * 0.2f, C_PLATE, 0xC1C7CD, 1.0f);
  if (playFailed) {
    TxtLine t = txt_line(TXT_LIVE_NOTE, "This channel isn't available right now", HEXI(0x8A9199), 255);
    txt_draw(t, r.x + (r.w - t.w) * 0.5f, r.y + r.h * 0.42f + tile * 0.5f + 18.0f);
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
  else if (group == GROUP_FAV) { l1 = "No favourites yet"; l2 = "Channels you favourite show up here."; }
  else if (group == GROUP_RECENT) { l1 = "Nothing watched yet"; l2 = "Channels you watch appear here."; }
  else { l1 = "No channels in this group"; l2 = ""; }
  { TxtLine a = txt_line(TXT_LIVE_TITLE, l1, HEXI(0xF5F6F8), 255);
    TxtLine b = txt_line(TXT_PG_END, l2, HEXI(0x9AA1A9), 255);
    float cx = BX() + (L_RIGHT - BX()) * 0.5f;
    txt_draw(a, cx - a.w * 0.5f, top + 160.0f);
    if (l2[0]) txt_draw(b, cx - b.w * 0.5f, top + 172.0f + a.h); }
  return 1;
}

// --- Y1, the guide ---------------------------------------------------------------------------
static float gridX(void) { return BX() + G_CH_W + G_COL_GAP; }
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

// THE BAND OVER THE GUIDE: the picture at the body's left, and beside it the
// focused programme: when (a tracked label and the hours), its title at the
// first level, the channel, and two lines of what it is.
static void guideDetail(void) {
  const IptvList *l = iptv_list();
  int ch = focusedChannel();
  const IptvChannel *c = chan(ch);
  long long now = nowMinute();
  float x = BX() + G_PREV_W + 40.0f, y = G_DETAIL_Y, w = L_RIGHT - x;
  long long s, e;
  int p;
  char line[300];
  if (!c) return;
  p = cellAt(ch, fChan ? nowSec() : fTime, &s, &e);
  { const IptvProgramme *pg = p >= 0 ? &l->pg[p] : NULL;
    float kx = x;
    if (pg && pg->stop <= now && iptv_has_archive(ch, pg->start)) {
      // CATCH-UP, outlined: this ended programme plays from the archive.
      float tw = tagWidth("CATCH-UP") + 22.0f;
      gfx_color((GfxRect){ kx, y - 6.0f, tw, 30.0f }, 8.0f / 30.0f, 1, 1, 1, 0.28f);
      gfx_color((GfxRect){ kx + 1.0f, y - 5.0f, tw - 2.0f, 28.0f }, 7.0f / 28.0f, 10 / 255.0f, 11 / 255.0f, 14 / 255.0f, 1.0f);
      tagText("CATCH-UP", 0xC1C7CD, kx + 11.0f, y + 9.0f, 1.0f);
      kx += tw + 14.0f;
    }
    if (pg) {
      char a1[16], b1[16];
      const char *tag = pg->start <= now && now < pg->stop ? "ON NOW" : pg->stop <= now ? "EARLIER" : "LATER";
      clockText(pg->start, a1, sizeof a1); clockText(pg->stop, b1, sizeof b1);
      snprintf(line, sizeof line, "%s \xE2\x80\x93 %s", a1, b1);
      labelLine(tag, line, kx, y, 1.0f);
    }
    y += 32.0f;
    { TxtLine t = txt_line_trim(TXT_LIVE_H1, pg ? pg->title : cleanName(c->name, NULL), HEXI(0xFFFFFF), 255, w);
      txt_draw(t, x, y);
      y += t.h + 6.0f; }
    if (pg) {
      snprintf(line, sizeof line, "%s", cleanName(c->name, NULL));
      inkTrim(TXT_LIVE_BODY, line, 0x9C9CA3, x, y, w, 1.0f);
      y += 38.0f;
      if (pg->desc[0]) txt_block_trim(TXT_PG_END, pg->desc, HEXI(0x7E7E86), x, y, w < 980.0f ? w : 980.0f, 28.0f, 1.0f, 2);
    } else {
      ink(TXT_LIVE_BODY, iptv_guide_state() == IPTV_LOADING ? "The TV guide is still loading."
                         : c->nPg ? "No information for this time." : "No guide for this channel.",
          0x9C9CA3, x, y, 1.0f);
    } }
}

static void guidePreview(void) {
  GfxRect r = { BX(), G_PREV_Y, G_PREV_W, G_PREV_H };
  const IptvChannel *tc = chan(tuned);
  char s[32];
  if ((ownsVideo() || zapPending || bufWaiting) && tc) {
    float cx = r.x + 18.0f, cy = r.y + r.h - 30.0f;
    if (ownsVideo()) drawVideo(r, G_PREV_R);
    // While it opens, the channel's logo breathes and fills, as in full screen.
    previewTuning(r, G_PREV_R);
    pictureShade(r, 84.0f, G_PREV_R, 0.88f);
    snprintf(s, sizeof s, "%d", tc->number);
    cx += inkMid(TXT_SRC_META, s, 0xA8A8AF, cx, cy, 120.0f, 1.0f) + 10.0f;
    inkMid(TXT_SRC_STATE, cleanName(tc->name, NULL), 0xF5F6F8, cx, cy, r.x + r.w - 18.0f - cx, 1.0f);
    if (playFailed) {
      TxtLine m = txt_line(TXT_LIVE_NOTE, "Channel unavailable", 255, 255, 255, 255);
      txt_draw(m, r.x + (r.w - m.w) * 0.5f, r.y + (r.h - m.h) * 0.5f);
    }
  } else {
    pictureIdle(r, G_PREV_R, focusedChannel(), 84.0f);
  }
}

static void guideRuler(double ws, double pps, float nowX) {
  float tx = gridX(), tw = gridW();
  char day[48];
  { time_t tt = (time_t)nowSec(); struct tm tmv; localtime_r(&tt, &tmv);
    strftime(day, sizeof day, "%a %d %b", &tmv); }
  { char up[48];
    size_t i;
    for (i = 0; day[i] && i + 1 < sizeof up; i++) up[i] = (char)toupper((unsigned char)day[i]);
    up[i] = 0;
    txt_tracking(TXT_LIVE_LABEL, up, 0x74, 0x74, 0x7B, BX(), G_RULER_Y + 6.0f, 1.0f, 2.0f); }
  gfx_crop(tx, G_RULER_Y, tw, G_RULER_H);
  for (long long s = slotFloor((long long)ws); s < (long long)ws + LIVE_SPAN_S; s += LIVE_SLOT_S) {
    float sx = tx + (float)((double)(s - ws) * pps);
    char c[16];
    if ((double)s < ws) continue;
    clockText(s, c, sizeof c);
    // The now line's cap sits on the ruler: a half hour it would cover is left out.
    if (nowX > 0.0f && sx + 12.0f + txt_width(TXT_SRC_TEXT, c) > nowX - 44.0f && sx < nowX + 44.0f) continue;
    gfx_color((GfxRect){ sx, G_RULER_Y, 1.0f, G_RULER_H }, 0.0f, 1, 1, 1, 0.10f);
    ink(TXT_SRC_TEXT, c, 0x8A8A92, sx + 12.0f, G_RULER_Y + 4.0f, 1.0f);
  }
  gfx_no_crop();
}

// One programme block, in its tier.
static void guideBlock(const IptvProgramme *pg, GfxRect b, int focused, long long now, float nowX, int row,
                       double ws) {
  int airing = pg->start <= now && now < pg->stop, past = pg->stop <= now;
  // Ended, but in the archive: still watchable, so not greyed like the rest.
  int kept = past && iptv_has_archive(pg->channel, pg->start);
  unsigned titleHex = focused ? C_INK : kept ? 0xC1C7CD : past ? 0x6E6E75 : airing ? 0xF2F2F4 : 0x9C9CA3;
  unsigned timeHex = focused ? 0x55555C : airing ? 0x8A8A92 : 0x64646B;
  int playingHere = airing && pg->channel == tuned && (ownsVideo() || zapPending);
  if (focused) {
    gfx_drop_shadow(b, G_BLOCK_R, 30.0f, 12.0f, 0.5f);
    gfx_color(b, G_BLOCK_R / b.h, HEXF(C_PAPER), 1.0f);
    focusPlate(b, G_BLOCK_R);
  } else {
    gfx_color(b, G_BLOCK_R / b.h, 1, 1, 1, airing ? 0.085f : 0.045f);
  }
  // What has aired of it, tinted: the seek bar's read, in the schedule.
  // Cropped to the block AND the grid: a row scrolling out above the grid is
  // clipped by the grid's crop, and replacing that crop with the block's let
  // its tint paint over the ruler.
  if (airing && nowX > b.x) {
    float cw = (nowX < b.x + b.w ? nowX : b.x + b.w) - b.x;
    float top = b.y > G_GRID_Y - 4.0f ? b.y : G_GRID_Y - 4.0f;
    float bot = b.y + b.h < G_BOTTOM + 4.0f ? b.y + b.h : G_BOTTOM + 4.0f;
    if (bot > top) {
      gfx_crop(b.x, top, cw, bot - top);
      // Nuvio's ramp on the channel that is playing, a quiet white on the rest.
      if (playingHere) gfx_seek_brand(b, G_BLOCK_R / b.h, 1.0f, focused ? 0.34f : 0.26f);
      else if (focused) gfx_color(b, G_BLOCK_R / b.h, 0, 0, 0, 0.08f);
      else gfx_color(b, G_BLOCK_R / b.h, 1, 1, 1, 0.07f);
    }
    gfx_crop(gridX(), G_GRID_Y - 4.0f, gridW(), G_BOTTOM - G_GRID_Y + 8.0f);
  }
  // Y3's tiers: text only where it says something whole.
  if (b.w >= G_TIER_TEXT) {
    char when[40], a[16], z[16];
    float pad = b.w >= G_TIER_FULL ? 20.0f : 16.0f;
    TxtLine t = txt_line_trim(focused || airing ? TXT_LIVE_BODY_B : TXT_LIVE_BODY, pg->title, HEXI(titleHex), 255,
                              b.w - 2 * pad);
    TxtLine m;
    clockText(pg->start, a, sizeof a);
    clockText(pg->stop, z, sizeof z);
    if (b.w >= G_TIER_FULL) snprintf(when, sizeof when, "%s \xE2\x80\x93 %s", a, z);
    else snprintf(when, sizeof when, "%s", a);
    m = txt_line(TXT_SRC_TEXT, when, HEXI(timeHex), 255);
    { float top = b.y + (b.h - (t.h + 2.0f + m.h)) * 0.5f;
      txt_draw(t, b.x + pad, top);
      txt_draw(m, b.x + pad, top + t.h + 2.0f); }
  } else if (b.w >= 12.0f) {
    gfx_color((GfxRect){ b.x + b.w * 0.5f - 3.0f, b.y + b.h * 0.5f - 3.0f, 6.0f, 6.0f }, 0.5f,
              focused ? 0.0f : 1.0f, focused ? 0.0f : 1.0f, focused ? 0.0f : 1.0f, focused ? 0.35f : 0.22f);
  }
  { int mins = (int)(((pg->start > (long long)ws ? pg->start : (long long)ws) - winStart) / 60);
    pointer_zone(b.x, b.y, b.w, b.h, pointCell, row, mins < 0 ? 0 : mins); }
}

// A channel's name with the search's hit lit: the matched span in `on`, the rest
// dropped to `off`, which reads better on a dark row than brightening the match.
// What follows the hit is trimmed to the room left, so a long IPTV name still
// shows its hit. 0, drawing nothing, when there is no hit or it would not show.
static int nameHit(TxtStyle st, const char *name, unsigned on, unsigned off, float x, float top, float maxW, float a) {
  char pre[256], mid[128];
  int at;
  size_t n = (size_t)queryLen;
  float end = x + maxW;
  if (!searching || !queryLen || queryIsNumber() || (at = foldFind(name, query)) < 0) return 0;
  if ((size_t)at >= sizeof pre || n >= sizeof mid) return 0;
  memcpy(pre, name, (size_t)at); pre[at] = 0;
  memcpy(mid, name + at, n); mid[n] = 0;
  // The hit, and an ellipsis after it if the rest is cut, must fit.
  if (txt_width(st, pre) + txt_width(st, mid) + (name[at + n] ? txt_width(st, "\xE2\x80\xA6") : 0.0f) > maxW)
    return 0;
  if (pre[0]) { TxtLine t = txt_line(st, pre, HEXI(off), 255); txt_draw_alpha(t, x, top, a); x += t.w; }
  // The hit keeps nearly full strength on a dimmed row: it is what the eye looks for.
  { TxtLine t = txt_line(st, mid, HEXI(on), 255); txt_draw_alpha(t, x, top, a < 0.9f ? 0.9f : a); x += t.w; }
  if (name[at + n]) { TxtLine t = txt_line_trim(st, name + at + n, HEXI(off), 255, end - x); txt_draw_alpha(t, x, top, a); }
  return 1;
}

// A row's channel card: the logo, the name at the bold body size, and under it
// the number in purple, the 4K and, on the one playing, the equaliser.
static void guideChannelCell(const IptvChannel *c, int ch, int r, float y) {
  GfxRect cr = { BX(), y, G_CH_W, G_ROW_H };
  int isTuned = ch == tuned && (ownsVideo() || zapPending);
  int focused = zone == ZONE_BODY && r == fRow && fChan;
  int uhd = 0;
  const char *name = cleanName(c->name, &uhd);
  char num[16];
  float nameX = cr.x + 16.0f + G_TILE + 16.0f, room = cr.x + cr.w - 14.0f - nameX;
  // Raised, not white, with the focus: the list's rule.
  gfx_color(cr, G_CH_R / cr.h, 1, 1, 1, focused ? 0.13f : isTuned ? 0.07f : 0.045f);
  if (focused) focusPlate(cr, G_CH_R);
  if (isTuned) playingRing((GfxRect){ cr.x + 16.0f, y + (G_ROW_H - G_TILE) * 0.5f, G_TILE, G_TILE }, 13.0f);
  identity(c, (GfxRect){ cr.x + 16.0f, y + (G_ROW_H - G_TILE) * 0.5f, G_TILE, G_TILE }, 13.0f,
           C_PLATE, 0xC1C7CD, 1.0f);
  { unsigned on = 0xF2F2F4;
    if (!nameHit(TXT_LIVE_BODY_B, name, on, 0x8A9199, nameX, y + 14.0f, room, 1.0f))
      inkTrim(TXT_LIVE_BODY_B, name, on, nameX, y + 14.0f, room, 1.0f); }
  { float lx = nameX, cy = y + G_ROW_H - 24.0f;
    snprintf(num, sizeof num, "%d", c->number);
    lx += inkMid(TXT_LIVE_Q, num, C_NUMBER, lx, cy, 100.0f, 1.0f);
    if (uhd) {
      lx += 9.0f;
      lx += inkMid(TXT_LIVE_Q, "\xC2\xB7", 0x5E5E66, lx, cy, 40.0f, 1.0f) + 9.0f;
      lx += inkMid(TXT_LIVE_Q, uhd == 2 ? "8K" : "4K", 0x7A7A82, lx, cy, 40.0f, 1.0f);
    }
    (void)lx; }
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
  guideRuler(ws, pps, (double)now >= ws && (double)now < we ? nowX : -1.0f);
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
                                                           : "No guide data";
      if (rowFocus) {
        gfx_color((GfxRect){ b.x - 3.0f, b.y - 3.0f, b.w + 6.0f, b.h + 6.0f }, (G_BLOCK_R + 3.0f) / (b.h + 6.0f),
                  1, 1, 1, 0.30f);
        gfx_color(b, G_BLOCK_R / b.h, HEXF(C_PAPER), 1.0f);
        focusPlate(b, G_BLOCK_R);
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
          focusPlate(b, G_BLOCK_R);
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
    brandLineV(nowX - 1.5f, G_GRID_Y - 4.0f, 3.0f, bottom - G_GRID_Y + 4.0f, 1.0f);
    gfx_seek_brand_at(1.0f, &r, &g, &b);
    clockText(now, c, sizeof c);
    { float w = tagWidth(c) + 22.0f;
      GfxRect cap = { nowX - w * 0.5f, G_GRID_Y - 34.0f, w, 30.0f };
      gfx_color(cap, 9.0f / 30.0f, r, g, b, 1.0f);
      tagText(c, 0xFFFFFF, cap.x + 11.0f, cap.y + 15.0f, 1.0f); }
  }
  // The guide's last row reaches into the fade, and there it striped the white
  // focus plate: the fade goes as the rows below it run out, a row's worth.
  { float below = G_GRID_Y + (nView - scrollRows) * G_ROW_STEP - G_BOTTOM;
    if (below > 0.0f) fadeBottom(NV_SCREEN_H - 130.0f, 130.0f, anim_clamp(below / G_ROW_STEP, 0.0f, 1.0f)); }
  pager(NV_SCREEN_W - 54.0f, G_GRID_Y, G_BOTTOM - G_GRID_Y, nView, perPage, fRow);
}

// --- Y2, the channel list (the 2026-11 redesign) ------------------------------------------
static float panelX(void) { return L_RIGHT - C_PANEL_W; }
static float listW(void) { return panelX() - 40.0f - BX(); }

// "1h 10m left", "10m left".
static void leftText(const IptvProgramme *pg, long long now, char *dst, size_t n) {
  long long left = (pg->stop - now + 59) / 60;
  if (left >= 60) snprintf(dst, n, "%lldh %lldm left", left / 60, left % 60);
  else snprintf(dst, n, "%lldm left", left);
}

// A channel's logo with its NUMBER as a tag on the bottom-left corner, like a
// broadcaster's own bug: the number used to take a column of its own left of
// the logo, room the name needed. The tag wears a ring in `ground` (the row's
// colour) so it reads as sitting on the corner.
static void logoTagged(const IptvChannel *c, GfxRect r, float radius, unsigned plate, unsigned ground, float a) {
  char num[16];
  identity(c, r, radius, plate, 0xC1C7CD, a);
  snprintf(num, sizeof num, "%d", c->number);
  { TxtLine t = txt_line(TXT_LIVE_NUM, num, 255, 255, 255, 255);
    GfxRect tg = { r.x - 6.0f, r.y + r.h - 22.0f, t.w + 16.0f, 28.0f };
    gfx_color((GfxRect){ tg.x - 3.0f, tg.y - 3.0f, tg.w + 6.0f, tg.h + 6.0f }, 11.0f / (tg.h + 6.0f), HEXF(ground), a);
    gfx_color(tg, 8.0f / tg.h, HEXF(0x202026), a);
    txt_draw_alpha(t, tg.x + 8.0f, tg.y + (tg.h - t.h) * 0.5f, a); }
}

// THE PLAYING CHANNEL: a ring in Nuvio's colours round its logo, drawn as the
// hold ring and the notices draw theirs (GFX_RING_INSET_FILL with the brand
// flag, all the way round). It replaced an equaliser after the name, which the
// owner turned down twice (2026-11-10); with the playing row's bar already in
// Nuvio's colours, the ring is all the name line needs.
// The colours TURN round it, a lap every LIVE_RING_LAP_MS, so it reads as
// something going on rather than a frame; still with Reduced animations, and
// stopped where it was while the channel is paused.
#define LIVE_RING_LAP_MS 2400u
static void playingRing(GfxRect tile, float radius) {
  const float gap = 4.0f, th = 3.5f;
  GfxRect o = { tile.x - gap - th, tile.y - gap - th, tile.w + (gap + th) * 2.0f, tile.h + (gap + th) * 2.0f };
  Uint32 t = paused ? pausedAt : SDL_GetTicks();
  float phase = settings_animations_reduced() ? 0.0f : (float)(t % LIVE_RING_LAP_MS) / (float)LIVE_RING_LAP_MS;
  gfx_rect(o, 0, GFX_RING_INSET_FILL, 1.0f + 0.999f * phase, 1.0f, th / o.h, (radius + gap + th) / o.h, 1, 1, 1, 1.0f);
}

static GfxRect starTipRect;
static char starTipLabel[40];

// THE STAR'S TOOLTIP: the detail screen's, above the star, but its RIGHT edge
// on the star's (centred, it ran into the picture beside the list), and drawn
// last, after the list's crop is lifted, so no row or edge cuts it.
static void starTooltip(void) {
  static float tip;
  static Uint32 last;
  Uint32 t = SDL_GetTicks();
  float dt = last ? (float)(t - last) / 1000.0f : 1.0f;
  int on = zone == ZONE_ACTIONS && viewMode == VIEW_LIST && starTipRect.w > 0.0f;
  last = t;
  tip = anim_ramp(tip, on ? 1.0f : 0.0f, dt, NV_DETWEB_TIP_MS);
  if (tip <= 0.01f || !starTipLabel[0]) return;
  { float al = tip * NV_DETWEB_TIP_ALPHA;
    TxtLine l = txt_line(TXT_DETWEB_TIP, starTipLabel, 255, 255, 255, 255);
    TxtLine sh = txt_line(TXT_DETWEB_TIP, starTipLabel, 0, 0, 0, 255);
    GfxRect b = starTipRect;
    float x = b.x + b.w - l.w;
    float y = b.y - NV_DETWEB_TIP_GAP - NV_DETWEB_TIP_H + NV_DETWEB_TIP_PADY + (NV_DETWEB_TIP_LH - l.h) * 0.5f
            + (1.0f - tip) * NV_DETWEB_TIP_RISE;
    txt_draw_alpha(sh, x, y + 2.0f, al * 0.80f);
    txt_draw_alpha(sh, x - 1.0f, y + 3.0f, al * 0.40f);
    txt_draw_alpha(sh, x + 1.0f, y + 3.0f, al * 0.40f);
    txt_draw_alpha(l, x, y, al); }
}

static void listRow(const IptvList *l, int r, float y, float lw) {
  int ch = view[r];
  const IptvChannel *c = &l->ch[ch];
  // THE FOCUSED ROW IS RAISED, NOT WHITE (the owner, 2026-11-10): the light
  // plate it always wore while the focus was beside it, in the panel. `lit` is
  // the focus on the row itself or on its star; the white stays for buttons.
  int focused = zone != ZONE_CHIPS && zone != ZONE_HEAD && r == fRow;
  int lit = focused && (zone == ZONE_BODY || zone == ZONE_ACTIONS), paper = 0;
  int starOn = focused && zone == ZONE_ACTIONS, fav = iptv_is_favourite(ch);
  float starW = lit ? 52.0f + 18.0f : 0.0f;
  int isTuned = ch == tuned && (ownsVideo() || zapPending);
  float x = BX();
  GfxRect row = { x, y, lw, C_ROW_H };
  long long now = nowMinute();
  const IptvProgramme *pg = programmeAt(ch, now);
  int uhd = 0;
  const char *name = cleanName(c->name, &uhd);
  unsigned nameInk = paper ? C_INK : 0xF5F6F8;
  float nameX = x + 22.0f + C_TILE + 26.0f, right = x + lw - 24.0f - starW;
  float leftW = 0.0f;
  char left[32] = "";

  if (focused) {
    gfx_color(row, C_ROW_R / row.h, 1, 1, 1, lit ? 0.11f : 0.08f);
    if (lit) focusPlate(row, C_ROW_R);
  }
  if (isTuned) playingRing((GfxRect){ x + 22.0f, y + (C_ROW_H - C_TILE) * 0.5f, C_TILE, C_TILE }, 16.0f);
  logoTagged(c, (GfxRect){ x + 22.0f, y + (C_ROW_H - C_TILE) * 0.5f, C_TILE, C_TILE }, 16.0f,
             C_PLATE, focused ? (lit ? 0x1C1C1C : 0x141414) : 0x000000, 1.0f);
  if (pg && focused) {
    leftText(pg, now, left, sizeof left);
    leftW = txt_width(TXT_SRC_META, left);
  }
  // The name, its 4K and the equaliser on one line; the programme under it.
  { TxtLine probe = txt_line(TXT_LIVE_H2, "Hg", 255, 255, 255, 255);
    float qw = uhd ? txt_width(TXT_LIVE_Q, "4K") + 14.0f : 0.0f, ew = 0.0f;
    float room = right - nameX - qw - ew;
    float top = pg ? y + 16.0f : y + (C_ROW_H - probe.h) * 0.5f, nx;
    TxtLine t = txt_line_trim(TXT_LIVE_H2, name, HEXI(nameInk), 255, room);
    if (!nameHit(TXT_LIVE_H2, name, nameInk, paper ? 0x6E757D : 0x8A9199, nameX, top, room, 1.0f))
      txt_draw(t, nameX, top);
    nx = nameX + (t.w < room ? t.w : room) + 14.0f;
    if (uhd) nx += inkMid(TXT_LIVE_Q, uhd == 2 ? "8K" : "4K", paper ? 0x6E6E75 : 0x7A7A82, nx, top + probe.h * 0.5f + 1.0f, 60.0f, 1.0f) + 14.0f;
    (void)nx; }
  // The star: quiet on the raised row, white while it is chosen.
  if (lit) {
    GfxRect st = { x + lw - 24.0f - 52.0f, y + (C_ROW_H - 52.0f) * 0.5f, 52.0f, 52.0f };
    if (starOn) gfx_color(st, 0.5f, HEXF(C_PAPER), 1.0f);
    else gfx_color(st, 0.5f, 1, 1, 1, 0.08f);
    gfx_icon((GfxRect){ st.x + 14.0f, st.y + 14.0f, 24.0f, 24.0f }, fav ? "live_star_fill" : "live_star",
             HEXF(starOn ? C_INK : 0xC8C8CE), 1.0f);
    pointer_zone(st.x, st.y, st.w, st.h, pointAct, ACT_FAV, 0);
    // Its tooltip is drawn by drawList once the list's crop is lifted: here it
    // was cut by the list's edges and covered by the rows and the picture.
    starTipRect = st;
    snprintf(starTipLabel, sizeof starTipLabel, "%s", fav ? "Remove from Favourites" : "Add to Favourites");
  }
  if (pg) {
    float pw = right - nameX - (leftW > 0.0f ? leftW + 20.0f : 0.0f);
    inkTrim(TXT_LIVE_BODY, pg->title, paper ? 0x55555C : 0x9C9CA3, nameX, y + 54.0f, pw, 1.0f);
    if (leftW > 0.0f)
      inkMid(TXT_SRC_META, left, paper ? 0x6E6E75 : 0x8E8E95, right - leftW, y + 68.0f, leftW + 1.0f, 1.0f);
    liveBar(nameX, y + C_ROW_H - 18.0f, right - nameX, 4.0f,
            (float)(now - pg->start) / (float)(pg->stop - pg->start), isTuned, paper, 1.0f);
  } else if (iptv_guide_state() == IPTV_LOADING) {
    // While the guide loads the name sits centred: a line that will fill in.
  }
  pointer_zone(row.x, row.y, row.w, row.h, pointRow, r, 0);
}

// The picture at the top of the panel: the TUNED channel, named in its corner;
// or, with nothing tuned, the focused channel's tile waiting for OK.
static float listPreview(float y) {
  GfxRect r = { panelX(), y, C_PANEL_W, C_PREV_H };
  int showTuned = (ownsVideo() || zapPending || bufWaiting) && chan(tuned);
  if (showTuned) {
    const IptvChannel *tc = chan(tuned);
    char num[16];
    float cx = r.x + 20.0f, cy = r.y + r.h - 34.0f;
    // THE PICTURE TAKES THE FOCUS (▲ from the buttons): OK on it goes full screen.
    // A picture cannot turn white, so it wears the buttons' 4px white ring.
    if (zone == ZONE_PREVIEW)
      gfx_color((GfxRect){ r.x - NV_DETW_RING, r.y - NV_DETW_RING, r.w + NV_DETW_RING * 2, r.h + NV_DETW_RING * 2 },
                (C_PREV_R + NV_DETW_RING) / (r.h + NV_DETW_RING * 2), 1, 1, 1, 1.0f);
    if (ownsVideo()) drawVideo(r, C_PREV_R);
    // While it opens, the channel's logo breathes and fills, as in full screen.
    previewTuning(r, C_PREV_R);
    pictureShade(r, 96.0f, C_PREV_R, 0.85f);
    pointer_zone(r.x, r.y, r.w, r.h, pointPreview, 0, 0);
    snprintf(num, sizeof num, "%d", tc->number);
    cx += inkMid(TXT_SRC_META, num, 0xA8A8AF, cx, cy, 120.0f, 1.0f) + 12.0f;
    inkMid(TXT_LIVE_BODY_B, cleanName(tc->name, NULL), 0xF5F6F8, cx, cy, r.x + r.w - 20.0f - cx, 1.0f);
    if (playFailed) {
      TxtLine m = txt_line(TXT_LIVE_NOTE, "Channel unavailable", 255, 255, 255, 255);
      txt_draw(m, r.x + (r.w - m.w) * 0.5f, r.y + (r.h - m.h) * 0.5f);
    }
  } else {
    pictureIdle(r, C_PREV_R, focusedChannel(), 112.0f);
  }
  return r.y + r.h;
}

// A tracked label ("ON NOW") and, after it, plain text in the label's grey.
static float labelLine(const char *tag, const char *rest, float x, float y, float a) {
  TxtLine probe = txt_line(TXT_LIVE_LABEL, "0", 255, 255, 255, 255);
  float w = txt_tracking(TXT_LIVE_LABEL, tag, 0x74, 0x74, 0x7B, x, y, a, 2.0f);
  if (rest && rest[0]) {
    TxtLine t = txt_line(TXT_SRC_TEXT, rest, HEXI(0xA6A6AD), 255);
    txt_draw_alpha(t, x + w + 14.0f, y + (probe.h - t.h) * 0.5f, a);
    w += 14.0f + t.w;
  }
  return w;
}


// The focused channel: what is on, how far and what follows.
static void listDetail(float y) {
  int ch = focusedChannel();
  const IptvChannel *c = chan(ch);
  long long now = nowMinute();
  const IptvProgramme *pg = c ? programmeAt(ch, now) : NULL;
  int playing = ch == tuned && (ownsVideo() || zapPending);
  float x = panelX(), w = C_PANEL_W;
  char line[300];
  if (!c) return;
  if (pg) {
    char a[16], b[16];
    clockText(pg->start, a, sizeof a); clockText(pg->stop, b, sizeof b);
    snprintf(line, sizeof line, "%s \xE2\x80\x93 %s", a, b);
    labelLine("ON NOW", line, x, y, 1.0f);
    y += 34.0f;
  }
  y += txt_block_trim(TXT_LIVE_H1, pg ? pg->title : cleanName(c->name, NULL), HEXI(0xFFFFFF), x, y, w, 48.0f, 1.0f, 2);
  y += 8.0f;
  if (pg) {
    char left[32];
    leftText(pg, now, left, sizeof left);
    snprintf(line, sizeof line, "%s  \xC2\xB7  %s", cleanName(c->name, NULL), left);
  } else {
    snprintf(line, sizeof line, "%s", iptv_guide_state() == IPTV_LOADING ? "The TV guide is still loading."
                                                                       : "No guide for this channel.");
  }
  inkTrim(TXT_LIVE_BODY, line, 0x9C9CA3, x, y, w, 1.0f);
  y += 40.0f;
  if (pg) {
    liveBar(x, y, w, 4.0f, (float)(now - pg->start) / (float)(pg->stop - pg->start), playing, 0, 1.0f);
    y += 30.0f;
    // What it is: the panel's empty half put to use, three lines at most.
    if (pg->desc[0])
      y += txt_block_trim(TXT_LIVE_BODY, pg->desc, HEXI(0x8A8A92), x, y, w, 33.0f, 1.0f, 3) + 22.0f;
    else y += 6.0f;
  }
  // LATER: the next two, the time quieter than the title.
  { const IptvProgramme *n1 = programmeAfter(ch, now, 0), *n2 = programmeAfter(ch, now, 1);
    const IptvProgramme *nx[4] = { n1, n2, programmeAfter(ch, now, 2), programmeAfter(ch, now, 3) };
    if (n1 && y < NV_SCREEN_H - 150.0f) {
      // One line each, the duration at the end in the label's grey.
      labelLine("LATER", NULL, x, y, 1.0f);
      y += 34.0f;
      for (int i = 0; i < 4 && nx[i] && y < NV_SCREEN_H - 90.0f; i++) {
        char a[16], d[16];
        float tw, dw;
        clockText(nx[i]->start, a, sizeof a);
        snprintf(d, sizeof d, "%lld min", (nx[i]->stop - nx[i]->start) / 60);
        tw = ink(TXT_LIVE_BODY, a, 0x6A6A72, x, y, 1.0f);
        dw = txt_width(TXT_SRC_META, d);
        inkTrim(TXT_LIVE_BODY, nx[i]->title, 0xB4B4BA, x + tw + 18.0f, y, w - tw - 18.0f - dw - 20.0f, 1.0f);
        ink(TXT_SRC_META, d, 0x64646B, x + w - dw, y + 2.0f, 1.0f);
        y += 40.0f;
      }
    } }
}

static void drawList(void) {
  const IptvList *l = iptv_list();
  float lw = listW();
  starTipRect = (GfxRect){ 0 };
  if (drawEmpty(C_LIST_Y)) return;
  // The focused row's shadow reaches past the list's top: crop a little above it.
  gfx_crop(BX() - 30.0f, C_LIST_Y - 24.0f, panelX() - 10.0f - (BX() - 30.0f), NV_SCREEN_H - C_LIST_Y + 24.0f);
  pointer_clip(BX() - 8.0f, C_LIST_Y - 8.0f, panelX() - 20.0f - (BX() - 8.0f), NV_SCREEN_H - C_LIST_Y + 8.0f);
  // Unfocused rows first, the raised one last so its shadow falls over them.
  for (int pass = 0; pass < 2; pass++)
    for (int r = (int)scrollRows; r < nView; r++) {
      float y = C_LIST_Y + (r - scrollRows) * C_ROW_STEP;
      if (y > NV_SCREEN_H) break;
      if ((pass == 1) != (r == fRow)) continue;
      listRow(l, r, y, lw);
    }
  gfx_no_crop();
  pointer_no_clip();
  fadeBottom(NV_SCREEN_H - 110.0f, 110.0f, 1.0f);
  { float y = listPreview(L_BODY_Y);
    listDetail(y + 34.0f); }
  starTooltip();
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
  float px = spX() ? 22.0f : 26.0f;
  gfx_color(r, 12.0f / r.h, 1, 1, 1, focused ? 0.07f : 0.05f);
  if (focused) spRing(r, 12.0f, 3.0f, 3.0f, 0xF0F2F4, 1.0f);
  else if (bad) spRing(r, 12.0f, 0.0f, 1.5f, 0xE88A6E, 0.85f);
  { float maxW = r.w - px * 2.0f;
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
  int onRow = setupRows[setupRow] == SR_REFRESH, focused = onRow && setupCol == 1, renaming = onRow && setupCol == 0;
  int typing = editing == F_NAME && ime_is_open();
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
    pointer_zone(b.x, b.y, b.w, b.h, pointSetup, rowIndex(SR_REFRESH), 1);
    right = b.x - 12.0f; }
  // Rename, beside it: the same pill without a glyph.
  { TxtLine t = txt_line(TXT_SRC_CHIP, "Rename", HEXI(renaming && !typing ? C_INK : 0xE4E7EA), 255);
    float w = 24.0f + t.w + 24.0f;
    GfxRect b = { right - w, cy - 29.0f, w, 58.0f };
    if (renaming && !typing) gfx_color(b, 0.5f, HEXF(C_PAPER), 1.0f);
    else gfx_color(b, 0.5f, 1, 1, 1, 0.09f);
    txt_draw(t, b.x + 24.0f, cy - t.h * 0.5f);
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
  // A name of the viewer's own wins; while it is typed, the draft stands there.
  if (typing) snprintf(name, sizeof name, "%s", nameDraft);
  else if (iptv_source_name()[0]) snprintf(name, sizeof name, "%s", iptv_source_name());
  { float maxW = right - 40.0f - x - 90.0f;
    TxtLine n = txt_line_trim(TXT_SRCP_NAME, name[0] ? name : " ", HEXI(0xF5F6F8), 255, maxW);
    TxtLine h = txt_line_trim(TXT_SRC_TEXT, sub, HEXI(0x7C838B), 255, maxW + 90.0f);
    float top = cy - (n.h + 6.0f + h.h) * 0.5f;
    nameRect = (GfxRect){ x - 12.0f, top - 8.0f, maxW + 24.0f, n.h + 16.0f };
    // Typing: the name on a field's plate with the caret after it.
    if (typing) {
      gfx_color(nameRect, 10.0f / nameRect.h, 1, 1, 1, 0.10f);
      gfx_rect(nameRect, 0, GFX_RING_INSET, 0, 2.0f / nameRect.h, 0, 10.0f / nameRect.h, 1, 1, 1, 0.9f);
      if ((SDL_GetTicks() / 530u) % 2u == 0u)
        gfx_color((GfxRect){ x + (nameDraft[0] ? n.w + 3.0f : 0.0f), top + 4.0f, 2.0f, n.h - 8.0f }, 0.0f, 1, 1, 1, 1.0f);
    }
    if (name[0]) txt_draw(n, x, top);
    if (!typing) { const char *k = s->kind == IPTV_SRC_XTREAM ? "XTREAM" : "M3U";
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
    // "show": a stop of its own, right of the password's label, a d-pad
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
    TxtLine n = txt_line(TXT_SRCP_NOTE, "optional, only for channels the playlist misses", HEXI(0x565C63), 255);
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
// The state tag: TUNING while the stream opens, PAUSED, how far behind live,
// CATCH-UP, the failure; nothing at all at live, which is what IPTV is unless
// one of those says otherwise. 0 when it drew nothing.
static float streamTag(float x, float cy, float a) {
  int tuning = zapPending || bufWaiting || !video_ready();
  int onLive = !playFailed && !tuning && atLive();
  char behind[32];
  const char *word = playFailed ? "UNAVAILABLE" : tuning ? "TUNING" : paused ? "PAUSED"
                   : onLive ? "LIVE" : srcKind == SRC_ARCHIVE ? "CATCH-UP" : behind;
  int live = onLive && !paused;
  if (live) return 0.0f;
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
  { float tw = txt_width(TXT_PLR_EPCODE, pg ? pg->title : cleanName(c->name, NULL));
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
      TxtLine pr = txt_line(TXT_LIVE_META_B, "0", 255, 255, 255, 255);
      kx += ink(TXT_LIVE_META_B, kick, C_NUMBER, kx, y - 2.0f, a);
      kx += sepBar(kx, y - 2.0f + pr.h * 0.5f, 16.0f, 12.0f, a);
      inkTrim(TXT_LIVE_META, cleanName(c->name, NULL), 0x8A9199, kx, y - 2.0f, textW - (kx - textX), a); }
    inkTrim(TXT_PLR_EPCODE, pg ? pg->title : cleanName(c->name, NULL), 0xF5F6F8, textX, y + 26.0f, textW, a);
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
// 48px glyphs, no circle at rest, the white puck IS the focus, fading in on the
// player's spring while the glyph crosses from white to black under it, and
// the focused button's name 16px under its circle, faded and risen the same way.
#define CTL_D     90.0f
#define CTL_GAP   14.0f
#define CTL_ICON  48.0f
#define CTL_TIP   16.0f
static void drawControls(float x, float cy, float a) {
  // The live glyphs at the film player's weight (live_ctl_*: stroke 24 on 256,
  // Phosphor's Bold, as subtitles/audio/aspect are); the header keeps its
  // lighter live_grid/live_list. Go live is Phosphor's clock-clockwise (Bold).
  static const char *ICON[CTL_N] = { "pause", "live_ctl_restart", "live_ctl_live", "live_ctl_grid", "live_ctl_list", "live_ctl_also",
                                     "subtitles", "audio", "aspect", "live_ctl_star" };
  static const char *NAME[CTL_N] = { "Pause", "Start over", "Go live", "Guide", "Channels", "Also showing", "Subtitles",
                                     "Audio", "Aspect Ratio", "Favourite" };
  float step = CTL_D + CTL_GAP, x0 = x + CTL_D * 0.5f;
  int fav = iptv_is_favourite(tuned), ids[CTL_N], n = shownControls(ids);
  int zones = ov == OV_BAR && a > 0.5f;
  for (int k = 0; k < n; k++) {
    int i = ids[k];
    float f = ctlFocus[i], cx = x0 + k * step, luma = 0.94f + (0.13f - 0.94f) * f;
    if (zones) pointer_zone_act(cx - CTL_D * 0.5f, cy - CTL_D * 0.5f, CTL_D, CTL_D, pointBar, clickBar, i, 0);
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

// THE STREAM'S FACTS, the film player's line (player.c streamFacts): "4K ·
// Dolby Vision · Dolby Atmos", from the pipeline, never a guess. Only with a
// picture: before one, video_width() still describes the previous channel.
static int liveFacts(char *dst, size_t n) {
  const char *f[3];
  int nf = 0;
  if (!ownsVideo() || !video_ready()) return 0;
  if (video_width() >= 3840)      f[nf++] = "4K";
  else if (video_width() >= 1920) f[nf++] = "1080p";
  else if (video_width() >= 1280) f[nf++] = "720p";
  else if (video_width() > 0)     f[nf++] = "SD";
  if (video_has_dolby_vision())               f[nf++] = "Dolby Vision";
  else if (!strcasecmp(video_hdr(), "HDR10")) f[nf++] = "HDR10";
  if (video_has_atmos())                      f[nf++] = "Dolby Atmos";
  dst[0] = 0;
  for (int i = 0; i < nf; i++) {
    if (i) strncat(dst, " \xC2\xB7 ", n - strlen(dst) - 1);
    strncat(dst, f[i], n - strlen(dst) - 1);
  }
  return nf;
}

// Phosphor's carets, one above the other, centred on `cy`: the title rolls.
static void titleCarets(float x, float cy, float a) {
  const float d = 28.0f;
  gfx_icon((GfxRect){ x, cy - d + 2.0f, d, d }, "live_caret_up", HEXF(0xF5F6F8), a);
  gfx_icon((GfxRect){ x, cy - 2.0f, d, d }, "live_caret_down", HEXF(0xF5F6F8), a);
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
  char line[300], a1[16], b1[16], facts[80];
  if (!c) return;
  gfx_opacity_group = a;
  identity(c, (GfxRect){ x, idBottom - tile, tile, tile }, 14.0f, C_PLATE, 0xE4E7EA, walking ? a * 0.6f : a);
  { float tx = x + tile + 26.0f, titleW = right - tx - 520.0f;
    // THE TITLE WITH THE FOCUS is a size up (48 for 42), no plate: the same
    // voice as the times beside the track. The carets say OK rolls it.
    TxtLine title = txt_line_trim(!walking && titleFocus > 0.5f ? TXT_LIVE_TITLE_F : TXT_LIVE_TITLE,
                                  pg ? pg->title : c->name, HEXI(0xF5F6F8), 255, titleW);
    float titleY = idBottom - title.h + 6.0f, kickY = titleY - 9.0f - 16.0f;
    if (!walking && titleFocus > 0.004f) {
      titleCarets(tx + title.w + 22.0f, titleY + title.h * 0.5f + 2.0f, a * titleFocus);
    }
    if (!walking && ov == OV_BAR && a > 0.5f && nView > 1)
      pointer_zone_act(tx - 8.0f, titleY - 8.0f, title.w + 22.0f + 28.0f + 16.0f, title.h + 16.0f,
                       pointBar, clickBar, BAR_TITLE, 0);
    txt_draw_alpha(title, tx, titleY, a);
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
      kx += inkMid(TXT_DETWEB_EP_BADGE, num, C_NUMBER, kx, kickY, 120.0f, a);
      if (pg && !guessed) {
        kx += sepBar(kx, kickY, NV_DETW2_BAR_H, 14.0f, a);
        kx += inkMid(TXT_LIVE_NAME, cleanName(c->name, NULL), 0x9AA1A9, kx, kickY, 520.0f, a) + 14.0f;
      } else kx += 14.0f;
      { float sw = streamTag(kx, kickY, a); if (sw > 0.0f) kx += sw + 14.0f; }
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
  }
  if (!walking && next) {
    TxtLine t;
    float nb = idBottom - 4.0f;
    clockText(next->start, a1, sizeof a1);
    snprintf(line, sizeof line, "%s \xE2\x80\x82%s", a1, next->title);
    t = txt_line_trim(TXT_PLR_META3, line, HEXI(0xE4E7EA), 255, 480.0f);
    txt_draw_alpha(t, right - t.w, nb - t.h, a);
    { float w = tagWidth("NEXT");
      txt_tracking(TXT_LIVE_TAG, "NEXT", HEXI(0xA9B0B8), right - w, nb - t.h - 8.0f - 16.0f, a, 2.2f); }
  }
  // The programme, on the film player's rail (player.c) between its clock
  // times: 8px and 12px with the focus, the track 0.26 -> 0.34, the fill in the Settings
  // seek colour, and the knob only while the rail has the focus. At plain live
  // there is no knob and no band: live cannot be moved. With a past to move
  // through (the pause buffer, catch-up) it can: then the lighter band is what
  // can be reached, the player's buffer layer, and a tick marks where now is.
  if (pg) {
    float span = (float)(pg->stop - pg->start);
    float frac = walking ? 0.0f : (float)((at - (double)pg->start) / span);
    float fa = walking ? a * 0.4f : a;
    float tx, tw, sr, sg, sb;
    int movable = !walking && ownsVideo() && (canRewind() || !atLive() || paused || scrubbing);
    int onTrack = !walking && ov == OV_BAR && !barPassive && barCtl == BAR_TRACK;
    float th = onTrack ? 12.0f : 8.0f, top = trackY - 4.0f, fx0;
    settings_seek_color(&sr, &sg, &sb);
    // The clock times either side, a programme and not a file: the time
    // readout's Medium 30, near white so they read over a bright picture,
    // centred on the resting rail.
    clockText(pg->start, a1, sizeof a1); clockText(pg->stop, b1, sizeof b1);
    // Small at rest; the readout's 30 once ◀▶ are the track's (focused, or
    // walking the schedule), when they are what is being moved through.
    { int stepping = onTrack || walking;
      TxtStyle ts = stepping ? TXT_PLR_TIME_T : TXT_PLR_META3;
      TxtLine ls = txt_line(ts, a1, 255, 255, 255, 255);
      TxtLine le = txt_line(ts, b1, 255, 255, 255, 255);
      txt_draw_alpha(ls, x, trackY - ls.h * 0.5f, fa * 0.92f);
      txt_draw_alpha(le, right - le.w, trackY - le.h * 0.5f, fa * 0.92f);
      // WHICH WAY IT MOVES, with the track focused: a chevron outside each
      // time, only where ◀ or ▶ would do something (the keys' own rules, in
      // fullEvent: back into the buffer or archive; forward towards live, or
      // at live along the schedule).
      // Walking the schedule, ◀ always goes somewhere (the one before, or back
      // to the track) and ▶ while there is a later one.
      if (stepping) {
        double fl = rewindFloor();
        int canL = walking || ((scrubbing || !atLive() || canRewind()) && fl > 0 && at > fl + 5.0);
        int canR = walking ? walkNext(walkPg) >= 0 : (scrubbing || !atLive() || walkNext(-1) >= 0);
        if (canL) gfx_icon((GfxRect){ x - 34.0f, trackY - 12.0f, 24.0f, 24.0f }, "chevron_left", 1, 1, 1, a * 0.92f);
        if (canR) gfx_icon((GfxRect){ right + 10.0f, trackY - 12.0f, 24.0f, 24.0f }, "chevron_right", 1, 1, 1, a * 0.92f);
      }
      // The rail keeps the room of the large times either way: it does not
      // jump sideways as the focus arrives.
      tx = x + txt_width(TXT_PLR_TIME_T, a1) + 24.0f;
      tw = right - txt_width(TXT_PLR_TIME_T, b1) - 24.0f - tx; }
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    fx0 = tx + tw * frac;
    // The pointer's rail: a band taller than the track, as the film player's.
    // Where there is a past to move through, a click goes there; at plain live
    // it is the track's OK.
    if (!walking && ov == OV_BAR && a > 0.5f) {
      trackX = tx; trackW = tw; trackStart = (double)pg->start; trackSpan = (double)span;
      pointer_zone_act(tx, trackY - 22.0f, tw, 44.0f, pointBar, movable ? clickTrack : clickBar, BAR_TRACK, 0);
    }
    gfx_color((GfxRect){ tx, top, tw, th }, 0.5f, 1, 1, 1, (onTrack ? 0.34f : 0.26f) * fa);
    if (movable) {
      double floor = rewindFloor(), wn = wallNow();
      float lo = (float)((floor - (double)pg->start) / span), hi = (float)((wn - (double)pg->start) / span);
      if (lo < 0) lo = 0;
      if (hi > 1) hi = 1;
      if (hi > lo) gfx_color((GfxRect){ tx + tw * lo, top, tw * (hi - lo), th }, 0.5f, 1, 1, 1, 0.30f * fa);
      if (hi < 1.0f && hi > 0.0f)
        gfx_color((GfxRect){ tx + tw * hi - 2.0f, top + th * 0.5f - 11.0f, 4.0f, 22.0f }, 0.5f, 1, 1, 1, 0.85f * fa);
    }
    if (fx0 - tx > 0.5f) {
      GfxRect fr = { tx, top, fx0 - tx, th };
      if (settings_seek_brand()) {
        float span = gfx_seek_brand_span(fr.w, tw);
        gfx_seek_brand(fr, 0.5f, span, fa);
        gfx_seek_brand_at(span, &sr, &sg, &sb);
      } else gfx_color(fr, 0.5f, sr, sg, sb, fa);
    }
    if (movable && (onTrack || scrubbing))
      gfx_color((GfxRect){ fx0 - 15.0f, top + th * 0.5f - 15.0f, 30.0f, 30.0f }, 0.5f, sr, sg, sb, fa);
    // Over the playhead, only when it says something the readout does not:
    // how far behind live, or that the hour is a guess.
    if (!walking && (scrubbing || wallNow() - at >= LIVE_EDGE_S || guessed)) {
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
      } else {
        snprintf(in, sizeof in, "Live \xC2\xB7 no guide for this channel");
      }
      t = txt_line(TXT_SRC_STATE, in, HEXI(0xE4E7EA), 255);
      fx = fx0 - t.w * 0.5f;
      if (fx < tx) fx = tx;
      if (fx + t.w > tx + tw) fx = tx + tw - t.w;
      txt_draw_alpha(t, fx, top - 16.0f - t.h, a);
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
    // The facts at the right end of the controls row, in the film player's
    // voice: 22px Medium at 0.72, centred on the buttons.
    if (liveFacts(facts, sizeof facts)) {
      TxtLine lf = txt_line_trim(TXT_PLR_META3, facts, 255, 255, 255, 255, (right - x) * 0.30f);
      txt_draw_alpha(lf, right - lf.w, ctlY - lf.h * 0.5f, a * 0.72f);
    }
  }
  gfx_opacity_group = 1.0f;
}

// 3 · THE PEEK: the bar's title rolls into a vertical carousel. The channel in
// the middle stands where the title stood, at the title's size, with its tile,
// number and name above and its programme's progress under; its neighbours
// above and below shrink and fade with distance. ▲▼ roll it, OK zaps to the
// middle, Back puts the bar back, on its title. The stream stays put until OK.
#define PK_STEP 112.0f
// The chosen one's plate reaches further than a row: its neighbours step away
// from it, more above (the plate's kicker) than below, so none sits on it.
#define PK_GAP_UP 40.0f
#define PK_GAP_DN 24.0f
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
    float y = cy + d * PK_STEP + (d < 0.0f ? -PK_GAP_UP : PK_GAP_DN) * (ad < 1.0f ? ad : 1.0f);
    float f = ad < 1.0f ? 1.0f - ad : 0.0f;              // 1 in the middle
    float ca = ad < 1.0f ? 1.0f - 0.5f * ad : ad < 3.0f ? 0.5f - 0.18f * (ad - 1.0f)
             : 0.14f * (4.0f - ad);
    // Below the middle there is room for one row: the next fades out sooner.
    if (d > 1.0f) ca *= 1.0f - (d - 1.0f) * 2.0f;
    // The neighbours step back further, so the middle one reads as chosen.
    if (ad >= 0.5f) ca *= 0.7f;
    float tile = 58.0f + 26.0f * f, tx = x + tile + 26.0f, titleW = right - tx - 520.0f;
    int idx = ((r % nView) + nView) % nView, mid = ad < 0.5f;
    const IptvChannel *c;
    const IptvProgramme *pg;
    char kick[300];
    // A short list shows each channel once, never wrapped round to itself.
    if (ad > half + 0.5f || ca <= 0.01f) continue;
    // Above, up to the clock; below, one row, clear of the STILL ON line.
    if (y < 190.0f || y > cy + PK_STEP * 1.5f + PK_GAP_DN) continue;
    c = &l->ch[view[idx]];
    if (ov == OV_PEEK && a > 0.5f)
      pointer_zone_click(x, y - PK_STEP * 0.5f, right - x, PK_STEP, clickPeek, r, 0);
    pg = mid ? programmeOrHour(view[idx], now, NULL) : programmeAt(view[idx], now);
    identity(c, (GfxRect){ x, y - tile * 0.5f, tile, tile }, 10.0f + 4.0f * f, mid ? C_PLATE_F : C_PLATE,
             mid ? 0xF5F6F8 : 0xC1C7CD, a * ca);
    snprintf(kick, sizeof kick, "%d", c->number);
    if (mid) {
      // The chosen one a size up, its channel in bold and bright; no plate.
      TxtLine t = txt_line_trim(TXT_LIVE_TITLE_F, pg ? pg->title : cleanName(c->name, NULL), HEXI(0xF5F6F8), 255, titleW);
      { TxtLine pr = txt_line(TXT_DETWEB_EP_BADGE, "0", 255, 255, 255, 255);
        float ky = y - t.h * 0.5f - 32.0f, nw = ink(TXT_DETWEB_EP_BADGE, kick, C_NUMBER, tx, ky, a * ca);
        nw += sepBar(tx + nw, ky + pr.h * 0.5f, 16.0f, 12.0f, a * ca);
        inkTrim(TXT_DETWEB_EP_BADGE, cleanName(c->name, NULL), 0xF5F6F8, tx + nw, y - t.h * 0.5f - 32.0f, titleW - nw, a * ca); }
      txt_draw_alpha(t, tx, y - t.h * 0.5f, a * ca);
      titleCarets(tx + t.w + 22.0f, y, a * ca);
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
          n = txt_line_trim(TXT_PLR_META3, line, HEXI(0xE4E7EA), 255, 480.0f);
          txt_draw_alpha(n, right - n.w, y - n.h * 0.5f + 10.0f, a * ca);
          txt_tracking(TXT_LIVE_TAG, "NEXT", HEXI(0xA9B0B8), right - wN, y - n.h * 0.5f - 14.0f, a * ca, 2.2f);
        } }
    } else {
      // A neighbour: its name and now on one line each, smaller.
      { TxtLine pr = txt_line(TXT_LIVE_NAME, "0", 255, 255, 255, 255);
        float nw = ink(TXT_LIVE_NAME, kick, C_NUMBER, tx, y - 26.0f, a * ca);
        nw += sepBar(tx + nw, y - 26.0f + pr.h * 0.5f, 14.0f, 10.0f, a * ca);
        inkTrim(TXT_LIVE_NAME, cleanName(c->name, NULL), 0x8A9199, tx + nw, y - 26.0f, titleW - nw, a * ca); }
      inkTrim(TXT_PLR_EPCODE, pg ? pg->title : "No guide data", pg ? 0xE4E7EA : 0x8A9199, tx, y + 2.0f,
              titleW, a * ca);
    }
  }
  // Under it: still on 103.
  if (tc) {
    char still[40];
    float r, g, b, w, by = NV_SCREEN_H - 55.0f - 38.0f;
    accent(&r, &g, &b);
    snprintf(still, sizeof still, "STILL ON %d", tc->number);
    w = tagWidth(still) + 30.0f;
    gfx_color((GfxRect){ 96.0f, by, w, 38.0f }, 10.0f / 38.0f, r, g, b, 0.20f * a);
    tagText(still, 0xA896FA, 96.0f + 15.0f, by + 19.0f, a);
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
  int ch = QCH(row);
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

  // The number in purple, then the name.
  { TxtLine nt;
    snprintf(line, sizeof line, "%d", c->number);
    nt = txt_line(TXT_TRK_VALUE, line, HEXI(C_NUMBER), 255);
    txt_draw_alpha(nt, tx, bt, a * (lit ? 1.0f : 0.75f));
    { float sw = sepBar(tx + nt.w, bt + nt.h * 0.5f, 16.0f, 12.0f, a);
      txt_draw_alpha(txt_line_trim(TXT_TRK_VALUE, cleanName(c->name, NULL), inkC, inkC, inkC, 255, right - tx - nt.w - sw),
                     tx + nt.w + sw, bt, a); } }

  // The detail line: on the playing channel the equaliser and "Playing"; then
  // the programme on now and what is left of it.
  { float my = bt + NV_EPL_SUB_DY, x = tx;
    int first = 1;
    // In the same-broadcast list, each copy's picture before anything else.
    if (alsoMode) {
      char q[16];
      if (iptv_quality_label(ch, q, sizeof q)[0]) { x = quickMeta(TXT_SRC_STATE, q, x, my, 1, lit ? 244 : sub, dim, a); first = 0; }
    }
    if (current) {
      quickEq(x, my + txt_baseline(TXT_SRC_META), a);
      x += Q_EQ_W + 9.0f;
      x = quickMeta(TXT_SRC_STATE, "Playing", x, my, first, 244, dim, a);
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
  { TxtLine t = txt_line(TXT_PANEL_TITLE, alsoMode ? "Also showing" : "Channels", 240, 241, 243, 255);
    txt_draw_alpha(t, cx + NV_EPL_PADX, NV_TRK_TITLE_Y, an);
    if (QN()) {
      char c[32];
      float capT = txt_cap_inset(TXT_PANEL_TITLE), capC = txt_cap_inset(TXT_SRC_COUNT);
      float mid = NV_TRK_TITLE_Y + (capT + txt_baseline(TXT_PANEL_TITLE)) * 0.5f;
      snprintf(c, sizeof c, "%d channel%s", QN(), QN() == 1 ? "" : "s");
      txt_draw_alpha(txt_line(TXT_SRC_COUNT, c, 132, 135, 142, 255), cx + NV_EPL_PADX + (float)t.w + 18.0f,
                     mid - (capC + txt_baseline(TXT_SRC_COUNT)) * 0.5f, an);
    } }
  if (alsoMode) {
    // Where the pill would be: what they all have on.
    const IptvProgramme *pg = programmeAt(tuned, nowMinute());
    const IptvChannel *c = chan(tuned);
    TxtLine t = txt_line_trim(TXT_TRK_VALUE, pg ? pg->title : c ? cleanName(c->name, NULL) : "", 178, 178, 184, 255,
                              NV_EPL_W - NV_EPL_PADX * 2);
    pill = (GfxRect){ cx + NV_EPL_PADX, NV_TRK_TABS_Y, (float)t.w, NV_EPL_PILL_H };
    txt_draw_alpha(t, pill.x, pill.y + (pill.h - t.h) * 0.5f, an);
  } else {
    pill = quickPill(cx + NV_EPL_PADX, NV_TRK_TABS_Y, an);
    pointer_zone(pill.x, pill.y, pill.w, pill.h, pointQuickPill, 0, 0);
  }

  if (!QN()) {
    txt_block(TXT_TRK_OPTSUB, group == GROUP_FAV ? "No favourites yet." : "No channels in this group.",
              133, 134, 136, cx + NV_EPL_PADX, vt + 16.0f, NV_EPL_W - NV_EPL_PADX * 2, 28.0f, an, 3);
  } else if (quickOpenedCap >= QN()) {
    float fadeTop = NV_TRK_TABS_Y + NV_EPL_PILL_H + NV_RING_FOCUS;
    gfx_crop(0, fadeTop, NV_SCREEN_W, NV_SCREEN_H - fadeTop);
    pointer_clip(0, fadeTop, NV_SCREEN_W, NV_SCREEN_H - fadeTop);
    // Only the rows near the window: a playlist can be thousands of channels.
    { int first = (int)(quickScroll / Q_PITCH) - 1;
      if (first < 0) first = 0;
      y = vt - quickScroll + first * Q_PITCH;
      // The rows above `first` are closed, bar the focused one which is below it.
      for (int i = first; i < QN() && y < NV_SCREEN_H; i++) {
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
  if (gmOpen && !alsoMode) quickGroupMenu(pill, an);
}

// While the stream opens, the channel stands in for the picture: its logo in
// the middle, the film player's opening screen. The whole mark translucent,
// the loaded part at full over it, and the whole of it on the web's pulse,
// scale 1 -> 1.04 over 2s and back. The fill spans the mark, not the file's
// transparent margin, so it starts and ends where the ink does. Without a
// logo the tile stands in, breathing in opacity.
//
// Centred on (cx, cy) in a box of at most bw x bh: the full screen's, or a
// preview's, which shows the same opening on the tuned channel.
static float tuningOutro(void) {
  Uint32 now = SDL_GetTicks();
  if (tuneWas) return 1.0f;
  if (tuneEndAt && now - tuneEndAt < LIVE_LOAD_OUTRO_MS)
    return 1.0f - anim_clamp(((float)(now - tuneEndAt) - 150.0f) / 300.0f, 0.0f, 1.0f);
  return 0.0f;
}
static void drawTuningAt(const IptvChannel *c, float cx, float cy, float bw, float bh, float a) {
  Uint32 now = SDL_GetTicks();
  float outro = tuningOutro(), tAnim = (float)(now - tuneBeganAt), wave = 0.0f, fadeIn, la;
  float tile = bh * 0.9f < 160.0f ? bh * 0.9f : 160.0f;
  const char *logo;
  GLuint tex;
  if (outro <= 0.0f || a <= 0.01f) return;
  fadeIn = anim_clamp(tAnim / 300.0f, 0.0f, 1.0f);
  if (!settings_animations_reduced()) {
    float ph = fmodf(tAnim, 4000.0f);
    wave = ph < 2000.0f ? ph / 2000.0f : (4000.0f - ph) / 2000.0f;
  }
  la = a * fadeIn * outro;
  tex = logoTex(c, 360.0f, &logo);
  // In flight: nothing for a moment, rather than the tile for a few frames; a
  // slow host gets the tile after that.
  if (!tex && logo[0] && !tex_failed(logo) && now - tuneBeganAt < 600u) return;
  if (tex) {
    float vis[4] = { 0.0f, 0.0f, 1.0f, 1.0f }, asp = tex_aspect(logo), vw, vh;
    GfxRect r;
    int brand = tex_brand_dark(logo);
    if (asp <= 0.0f) asp = 1.0f;
    if (!tex_content_box(logo, vis) || vis[2] - vis[0] < 0.02f || vis[3] - vis[1] < 0.02f) {
      vis[0] = vis[1] = 0.0f; vis[2] = vis[3] = 1.0f;
    }
    // The mark's own aspect, fitted to the box, and the file's rect around it.
    // NEVER PAST TWICE THE FILE: a 96px logo stretched to 360 came out as a
    // blur with a glow round it. The box shrinks to what the pixels can hold.
    { int sw = tex_source_width(logo);
      if (sw > 0) {
        float mw = 2.0f * (float)sw * (vis[2] - vis[0]), mh = 2.0f * ((float)sw / asp) * (vis[3] - vis[1]);
        if (bw > mw) bw = mw;
        if (bh > mh) bh = mh;
      } }
    vw = asp * (vis[2] - vis[0]) / (vis[3] - vis[1]);
    if (vw > bw / bh) { vh = bw / vw; vw = bw; } else { vw = bh * vw; vh = bh; }
    vw *= 1.0f + 0.04f * wave; vh *= 1.0f + 0.04f * wave;
    r.w = vw / (vis[2] - vis[0]); r.h = vh / (vis[3] - vis[1]);
    r.x = cx - vw * 0.5f - vis[0] * r.w;
    r.y = cy - vh * 0.5f - vis[1] * r.h;
    gfx_logo(r, tex, brand, .95f, .95f, .97f, la * 0.32f);
    gfx_logo_fill(r, tex, brand, vis[0] + tuneFill * (vis[2] - vis[0]), .95f, .95f, .97f, la);
  } else {
    identity(c, (GfxRect){ cx - tile * 0.5f, cy - tile * 0.5f, tile, tile }, tile * 0.175f,
             C_PLATE, 0xC1C7CD, la * (1.0f - 0.20f * wave));
  }
}
static void drawTuning(const IptvChannel *c, float a) {
  drawTuningAt(c, NV_SCREEN_W * 0.5f, NV_SCREEN_H * 0.5f - 60.0f, 360.0f, 180.0f, a);
}

// A preview while its stream opens: the plate, and the tuned channel's logo
// breathing and filling in the middle, as full screen does. 1 when it drew.
static int previewTuning(GfxRect r, float radius) {
  const IptvChannel *tc = chan(tuned);
  if (!tc || tuneFor != tuned || tuningOutro() <= 0.0f) return 0;
  if (!ownsVideo() || !video_ready()) gfx_color(r, radius / r.h, HEXF(0x14171B), 1.0f);
  drawTuningAt(tc, r.x + r.w * 0.5f, r.y + r.h * 0.46f, r.w * 0.44f, r.h * 0.36f, 1.0f);
  return 1;
}

static void drawFull(void) {
  const IptvChannel *c = chan(tuned);
  // Under every other zone: the picture itself (clickPicture).
  pointer_zone_click(0.0f, 0.0f, NV_SCREEN_W, NV_SCREEN_H, clickPicture, 0, 0);
  drawVideo((GfxRect){ 0.0f, 0.0f, NV_SCREEN_W, NV_SCREEN_H }, 0.0f);
  // Failed, the tile stays where the logo was.
  if (c && playFailed && barA < 0.5f && toastA < 0.5f)
    identity(c, (GfxRect){ NV_SCREEN_W * 0.5f - 80.0f, NV_SCREEN_H * 0.5f - 80.0f, 160.0f, 160.0f }, 28.0f,
             C_PLATE, 0xC1C7CD, 1.0f);
  // Tuning, under the toast and the bar, which both clear the middle; only the
  // peek reaches up there.
  else if (c) drawTuning(c, ov == OV_PEEK ? 1.0f - barA : 1.0f);
  if (barA > 0.01f) {
    // The player's own scrims, one quad each: a shader gradient, never a blur
    // and never stacked bands (this is live video under them).
    gfx_rect((GfxRect){ 0.0f, 0.0f, NV_SCREEN_W, 190.0f }, 0, GFX_VEIL_TOP, 0, 0, 0, 0.0f, 0, 0, 0, 0.72f * barA);
    // Tall enough to take NEXT and the title's kicker, which sat on the
    // picture where the gradient had already run out.
    { float h = ov == OV_PEEK ? 860.0f : 720.0f;
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
  drawGroups();
  plateSet = 0;
  if (viewMode == VIEW_GUIDE) drawGuide(); else drawList();
  if (plateSet) hold_track(&hold, plate, plateR);
  drawDigits();
  drawToast();
}
