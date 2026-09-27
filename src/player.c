// The playback screen, in OUR WEB APP's format.
//
// The reference has changed: this legacy variant follows the web app's player (the
// #playerUiRoot block in css/components.css), not the Apple TV app's that the
// nuvio-native prototype draws. What came from there is the MECHANICS — the focus
// spring, auto-hide, the pipeline's hole — because that part is not a matter of
// style. The arrangement and the measurements are the web app's, recorded one by
// one below.
//
// Concrete differences from what used to be here: the buttons sit on the LEFT and
// are not centred; the time is ONE "elapsed / total" label at the right-hand end and
// not two with a negative remainder; the subtitle sits BELOW the title; the bar is
// 6px and not 8, with no head marker; the three informational pills
// ("Information", "In Focus", "Continue Watching") have gone, being furniture from
// the Apple app that does not exist in ours.
//
// There are three behaviours observed on the device, and each of them changes the
// whole design:
//
//   1. While it plays, the screen is JUST the frame. Zero interface. No residual
//      bar, no corner clock — anything that appears over the image when nobody
//      asked for it is noise.
//   2. Any direction on the D-pad RAISES the controls from the base. They do not
//      flash into place: they come in with a spring, sliding up from below,
//      together with the veil.
//   3. Left alone for a few seconds they disappear on their own — but not while
//      the video is paused. Paused with no controls, the user is left staring at a
//      frozen frame with no idea what happened.
#include "player.h"
#include "acclib.h"
#include "video.h"
#include "tracks.h"
#include "gfx.h"
#include "text.h"
#include "tex_cache.h"
#include "anim.h"
#include "layout.h"
#include "catalog.h"
#include "discover.h"
#include "trakt.h"
#include "sync.h"
#include "parental.h"
#include "episodes.h"
#include "streams.h"
#include "autosync.h"
#include "subtitle.h"
#include "intro.h"
#include "watchedep.h"
#include "home.h"
#include "settings.h"
#include "failures.h"
#include "pointer.h"
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>   // strcasecmp, para comparar o hdrType do pipeline
#include <math.h>

// How long the controls stay up without receiving a key. Measured by eye on the
// device: close to 4s. Less than that and the user loses the bar mid-read; much
// more and the interface disappears too late and gets in the way of the scene.
#define PLR_HIDES_MS   4000u
// The 10s jump of skip forward/back. It is the Apple remote's step, and it only
// counts with the controls up: blind, an arrow would be an invisible jump.
#define PLR_JUMP_SEG    10.0f
// A fallback duration, in seconds, for when the catalogue's `meta` carries no film
// running time (series carry "3 seasons", which is nobody's duration).
// 1h54 is just a plausible number so the layout has something to show — as soon as
// the real video comes in, the duration comes from the decoder and this constant dies.
#define PLR_DURATION_DEFAULT   (114.0f * 60.0f)
// The geometry of the controls block, from the bottom up. Everything anchored to
// the BOTTOM of the screen: it is what does not move when the block slides in.
// ---------------------------------------------------------------------------
// THE WEB APP'S TRANSPORT, RE-MEASURED 2026-09-17
//
// The numbers below no longer come from the ATV port near the top of the player
// section in components.css. They come from the block that ends that section —
// "Transport - clock, title block, scrubber" (css/components.css:15768) — which
// says in its own header that it deliberately sits last and supersedes the port,
// "which sized the transport for a phone-sized preview rather than a 1080p screen
// viewed from a couch". Reading the port and stopping there is how this file ended
// up half a size out on the bar and a third out on the button spacing.
//
// Resolved at 1920x1080, which is where this app runs — in the sheet they are
// min(Xvw, Ypx) and the TV always hits the ceiling:
//
//   --player-controls-x            64    .player-controls-overlay padding, BOTH axes
//   --player-scrub-height           8    resting track   (the port said 12)
//   --player-scrub-height-focused  12    focused track   (the port said 20)
//   --player-scrub-knob            30    the playhead, scale(0) until focus
//   --player-transport-gap         40    title block -> bar
//   --player-transport-row-gap     36    bar -> button row
//   --player-control-size          90    the focus circle (the port said 96)
//   .player-control-buttons gap    14    (the port said 8)
//   --player-control-icon          48    the glyph inside the circle
//   .player-control-btn::after     the label, 16 below the circle, 20/bold
#define PLR_PAD_X         64.0f
#define PLR_PAD_Y         48.0f
// The transport's own bottom inset. It is --player-controls-x (64) and not
// PLR_PAD_Y: the web app's overlay pads BOTH axes with -x, and the focused
// button's label hangs 16px below the circle, so at 48 the label landed 8px from
// the panel's edge — inside the strip the TV eats by overscan.
#define PLR_PAD_BOTTOM    64.0f
// The side margin of the footer's CONTENT — title, bar, buttons and clock, which
// in the web app all share one left edge (the overlay's padding box). 96 is this
// app's measured safe gutter, wider than the web's 64 because the TV cuts more
// than a browser does; what matters for the LOOK is that the bar starts exactly
// where the play button's circle starts, and that is what this shares.
#define PLR_MARGIN        96.0f
#define PLR_BTN_D         90.0f
#define PLR_BTN_GAP       14.0f
// The label under the FOCUSED button: `top: calc(100% + 16px)` on
// .player-control-btn::after, so 16px below the circle's bottom edge.
#define PLR_BTN_TIP       16.0f
// THE SCRUBBER. 8px at rest, 12px focused, and a full pill at both — not the
// square-cornered hairline that was here.
//
// The square corner was a workaround for a real bug, and the workaround outlived
// it: `radius` in this API is a fraction measured in the rect's HEIGHT (see the
// SDF in gfx.c — `b = vec2(0.5*asp, 0.5) - r`), so the 6.0 that was once passed
// here was not "6px", it was twelve times the whole rect, and the SDF degenerated.
// 0.5 is the correct spelling of a pill. The one case that still needs care is the
// FILL while it is narrower than it is tall, where 0.5 drives b.x negative — see
// railRadius below, which is the same correction parental_guide already applies to
// its vertical bar.
#define PLR_RAIL_H         8.0f
// The bar as the Magic Remote's pointer sees it: a band this tall centred on the
// rail, which is too thin to land on from across the room.
#define PLR_BAR_HIT_H     48.0f
#define PLR_RAIL_H_FOCUS  12.0f
// The playhead. "Wants to be ~2.5x the focused bar. Closer than that and it reads
// as a bump in the bar rather than a playhead you are holding" — the sheet's own
// note. Hidden at rest (transform: scale(0)) so the resting bar stays a hairline.
#define PLR_RAIL_KNOB     30.0f
// .player-meta is a flex column with `gap: min(0.42vw, 8px)` — the air BETWEEN the
// title, the episode line and the stream facts. It was a bare 6 for the one gap
// that existed.
#define PLR_META_GAP       8.0f
// THE SEEK DELTA (.player-seek-delta), the pill that says how far this burst of
// presses has moved you. In the web app it lives at the right end of the controls
// row, immediately after the time it qualifies.
//
//   min-width 116, padding 6/16, radius 999, background rgba(255,255,255,0.14)
//   font 24/700 in the row (.player-controls-row .player-seek-delta)
//
// The flex `gap: clamp(24px, 1.8vw, 36px)` between the readout and the pill.
#define PLR_PILL_MINW    116.0f
#define PLR_PILL_PADX     16.0f
#define PLR_PILL_PADY      6.0f
#define PLR_ROW_GAP       34.0f
// A HELD SEEK IS ONE GESTURE, not N seeks.
//
// Each press used to commit immediately — posSeg += 10 and video_fetch(). That is
// the bug behind "when holding seek too long it seems to reset": player_update
// overwrites posSeg from video_pos() every frame, and the pipeline is still
// somewhere behind the target when the next frame arrives, so the bar leapt forward
// and snapped back, repeatedly, and a long hold landed wherever the decoder had got
// to rather than where you aimed. It was also firing one seek per key repeat at the
// pipeline, which is what made it lag further behind the longer you held.
//
// The web app's answer, ported: a PREVIEW. Presses move a target, the bar draws the
// target, playback carries on untouched underneath, and ONE seek is issued 1000ms
// after the last press (scheduleSeekPreviewCommit).
// The stats panel's corner. The web app hangs it at left ~38 / top 24. It used
// to borrow the jump prompts' 64/60 margins, which the owner found too far from
// the corner; 32 sits with the web's. An older note here warned that overscan
// eats the edge strip — if the panel ever looks clipped, this is what to raise.
#define PLR_STATS_X         32.0f
#define PLR_STATS_Y         32.0f
#define PLR_SEEK_COMMIT_MS  1000u
// The pill's fade once the seek is committed. The web app simply clears the text;
// 200ms of fade costs nothing and stops it popping out of existence.
#define PLR_DELTA_FADE_MS    200u
// AFTER committing, video_pos() still reports the OLD position until the pipeline
// lands. Believing it there would re-introduce exactly the snap-back this preview
// exists to remove, so posSeg holds the target until the decoder arrives within
// PLR_SEEK_LAND_S of it — or until PLR_SEEK_SETTLE_MS is up, because a seek that
// never lands (a dead link, a source that refuses) must not freeze the readout
// forever.
#define PLR_SEEK_LAND_S      2.0f
#define PLR_SEEK_SETTLE_MS  6000u
// The step escalates with the repeat count, exactly as the web app's does:
// >=18 -> 120s, >=12 -> 60s, >=7 -> 30s, >=3 -> 20s, else 10s. Holding the key is
// how you cross an hour of film without 360 presses.
static float seekStepFor(int repeats) {
  return repeats >= 18 ? 120.0f : repeats >= 12 ? 60.0f
       : repeats >=  7 ?  30.0f : repeats >=  3 ? 20.0f : 10.0f;
}
#define PLR_GAP_BAR       40.0f   // --player-transport-gap:     title -> bar
#define PLR_GAP_ROW       36.0f   // --player-transport-row-gap: bar -> buttons
// THE TWO SCRIMS, now the web app's own.
//
//   .player-controls-gradient-bottom  min(31.25vw, 600px)  -> 600
//   --player-top-scrim                ellipse 600 x 360 at the top-right corner
//
// The bottom one was 400 tall AND drawn with GFX_VEIL_BOTTOM's squared smoothstep,
// which between them put only about a fifth of its density at the height the
// scrubber sits. Six hundred tall with the sheet's own stops (GFX_VEIL_PLAYER)
// roughly triples it there, which is what the bar and the buttons were missing.
//
// The top one is no longer a band. As a full-width strip it shaded the whole top of
// the frame for the sake of two short lines in one corner; now it is a pool anchored
// to that corner and the rest of the top is untouched picture.
#define PLR_GRADIENT_BOTTOM   600.0f
#define PLR_POOL_W          600.0f
#define PLR_POOL_H          360.0f
// #f5f5f5 = --secondary-color, which is what fills the bar in the web app.
#define PLR_FILL_C      (245.0f / 255.0f)

#define PLR_ICON_H       48.0f
// How far the block slides down when hidden. Deliberately small: what makes the
// movement read is not the distance, it is the spring plus the fade.
#define PLR_SLIDE       46.0f
// The parental guide (.player-parental-*): a 6 bar, the list inset by 20, a 36 line
// with a 4 gap. They do not go through the ATV block's x2 conversion — the base rule
// is not redone there.
#define PG_BAR_W         6.0f
// How long the parental guide stays on screen, counting from the first frame with
// a picture, and how long the final fade lasts. Seven seconds is enough to read four
// short lines without becoming furniture — after that it does not come back during
// this playback.
#define PG_SEG_TOTAL       7.0f
#define PG_SEG_OUTPUT       0.8f
#define PG_LIST_PADX     20.0f
#define PG_LINE_H        42.0f
#define PG_LINE_GAP       4.0f
// Either side of the "·" between category and severity. The web app's separator
// is " · " — a space each side — and the port drew the dot bare, so the three
// parts ran together ("Profanity·Severe"). A fixed gap rather than a space glyph:
// the space's width depends on the font's metrics, and a little more than one
// space is what keeps the dot reading as a separator from the sofa.
#define PG_SEP_GAP       10.0f
// The veil became the web app's two scrims — GFX_VEIL_PLAYER along the base and
// GFX_VEIL_POOL in the clock's corner. They exist so the text reads over the image:
// without them a bright scene wipes out the title's name.

// THE ROW'S ORDER, from getControlDefinitions() in playerScreen.js: play, then
// subtitles, then audio, then episodes (only for a series), and the rest last.
//
// Aspect and Sources were second and fifth here, which is nobody's order. In the
// web app they are not even in the resting row: they live behind the "More
// Actions" button, which swaps the tail of the row for source/aspect/stats/speed.
// That collapse has NOT been ported — a menu that hides two of six actions is a
// behaviour change, not a visual one, and with six buttons the row still reads in
// one glance. What is ported is where each one SITS, so the hand finds subtitles
// and audio in the first three, as it does in the web app.
//
// The order is also why the row is no longer walked with a bare index: episodes
// only exists for a series, and it is no longer the last entry, so the old
// `PLR_NBTNS - (epT > 0 ? 0 : 1)` (which only ever worked because episodes
// happened to be last) is gone in favour of the visible list below.
//
// NEXT sits straight after play, as getControlDefinitions() puts "playNextEpisode":
// it is the other half of "what plays now", and it only exists while there IS a
// next episode.
//
// DETAILS closes the row: it leaves the player, so it is the one action that
// ends what the others adjust, and it sits where the hand reaches it last.
enum { PLR_PLAY, PLR_NEXT, PLR_CC, PLR_AUDIO, PLR_EPISODES,
       PLR_SOURCES, PLR_ASPECT, PLR_STATS, PLR_DETAILS, PLR_NBTNS };

static int   is_open = 0, exiting = 0, requestedExit = 0;
static int   idx = 0;
static int   playing = 1;
// The focused button in the transport row. It starts on PLAY because that is the
// answer nine out of ten openings want: the finger stops in the centre and OK decides.
static int   button = PLR_PLAY;
// The progress bar is a focus target, as in the web app: `.player-progress-shell`
// thickens from 6 to 10px and lightens the track when focused. It sits OUTSIDE the
// buttons' enum because it is not a button — OK on it 'presses' nothing, and
// LEFT/RIGHT change meaning (seeking, rather than moving focus).
static int   barFocus = 0;
// THE CARD ABOVE THE BAR IS A FOCUS TARGET TOO, when there is one. The transport
// is a ladder — buttons, bar, then whatever is floating above it — and UP walks it
// to the top and then puts the whole thing away. Without this rung the card was
// only reachable with the controls DOWN, which meant the one moment you could not
// take the offer was while you were looking at the controls.
static int   cardFocus = 0;
static int   visible = 0;          // the controls' target (1 = up)
static float anim = 0.0f;          // 0..1 following `visible`, by spring
static float focusB[PLR_NBTNS];     // each button's focus spring
// The BAR's focus spring. It used to have none: `barFocus` was a bare int and the
// track jumped between its two heights in one frame. The web app's track carries
// `transition: height 180ms cubic-bezier(0.22, 1, 0.36, 1)` and the playhead the
// same curve on its scale, so a hard switch is visibly not the same control.
static float focusBarAnim = 0.0f;
// The accumulated jump of the CURRENT burst, and when it last grew. Zeroed when
// the burst ends, when the bar loses focus, and when the screen closes.
// The seek PREVIEW. `seekActive` says a burst is in flight; `seekPreview` is where
// it is aiming; `seekAt` is the last press. After the commit, `settleAt` guards the
// window in which video_pos() is not yet to be believed.
static int    seekActive = 0, seekDir = 0, seekRepeats = 0;
static float  seekPreview = 0.0f;
static Uint32 seekAt = 0, seekEndAt = 0;
static Uint32 settleAt = 0;
static float  settleTarget = 0.0f;
// THE QUICK SEEK: LEFT/RIGHT with the controls DOWN. Each press is a flat
// PLR_QUICK_STEP_S — one press ten seconds, two twenty — with no ramp, because
// here you count presses rather than hold. It rides the same preview and the same
// single commit as the bar; what it adds is how it is SHOWN: "+ 20 >" at the side
// of the frame and the bar with its time alone, not the whole transport.
//
// `quickDelta` is the burst's sum, kept apart from seekPreview - posSeg because
// playback runs on underneath and would make "+ 10" count down while you read it.
// It outlives the commit so the readout can linger; the next burst zeroes it.
#define PLR_QUICK_STEP_S   10.0f
// How long the readout and the bar stay after the LAST press: the commit goes out
// at PLR_SEEK_COMMIT_MS, and the rest is time to see where it landed.
#define PLR_QUICK_HOLD_MS  2000u
// The quick layout's gap between the time and the bar under it.
#define PLR_QUICK_TIME_GAP   14.0f
// The quick layout's bottom scrim: just tall enough to seat the time and the bar.
#define PLR_QUICK_GRADIENT  200.0f
static Uint32 quickAt = 0;
static float  quickDelta = 0.0f;
static float  quickAnim = 0.0f;    // 0..1, the quick overlay's spring
static float  quickPulse = 0.0f;   // 1 on each press, decays: the chevron's nudge
static int    statsOpen = 0;   // the stream stats panel (#playerStatsOverlay)
static float entry = 0.0f;       // 0..1 the screen's opening/closing fade
static float entryV = 0.0f;      // its velocity: the opening runs on anim_spring2
// THE HANDOFF FROM THE TITLE SCREEN (player_open_from_detail). `fromDetail` says the
// page is still underneath and the entrance crossfades over it; `flyFrom` is where the
// page last drew the logo, w 0 when it drew none.
static int     fromDetail = 0;
static GfxRect flyFrom;
static int     flying = 0;   // the last frame drew the logo on its flight path
static Uint32 lastInput = 0;
// When the screen opened: the loading logo's pulse counts from here, so it always
// starts from rest instead of from wherever the clock happened to be.
static Uint32 openedAt = 0;
// THE LOADING LOGO AS A PROGRESS BAR, the way Stremio does it: the logo sits dim
// and a bright copy fills it from the left as the source opens.
//
// There is no byte count to show — the pipeline reports no load progress, only
// events — so the fill is STAGED on what the player can see:
//   searching the addons for a source     creeps toward 0.40
//   URL handed to the pipeline            creeps toward 0.60
//   pipeline has a mediaId                creeps toward 0.92
//   loadCompleted                         runs to 1.00
// Within a stage it eases toward the ceiling and never reaches it, so it never
// looks frozen, and each stage lifts the ceiling. The last 8% is kept for the
// real finish: a fill that reached the end while the screen still waited would
// be a lie the owner catches every time.
static float  loadFill = 0.0f;
// When loading ENDED with a picture: the fill runs home and the logo fades out
// over the first frames instead of vanishing on the frame the video arrives.
static Uint32 loadEndAt = 0;
static int    wasLoading = 0;
#define PLR_LOAD_OUTRO_MS 450u
// The instant the PICTURE started (not the screen's opening: between the two there
// is the source search, which can take seconds). Zero while there has been none.
// The parental guide relies on this to appear ONCE, at the start, and disappear.
static Uint32 startImage = 0;
// How much of the player's chrome is showing, 0..1 — set while drawing (see
// player_draw), read by the update that runs before it.
static float chrome;
// THE TWO MEDIA VARIABLES. All the rest of the file reads only from here — when the
// real video comes in, they are the ones the decoder starts filling.
static int   hasVideo = 0;
static int   reqTracks = 0;
static int   waitingSource = 0;   // opened with no URL, waiting for the addon to answer
static float posSeg = 0.0f;
static float durationSeg = PLR_DURATION_DEFAULT;

// A TRAILER SESSION (player_open_trailer). The same screen and pipeline as a
// film, minus everything that treats the file as the title itself: no progress
// saved, no Trakt scrobble, no resume, no next episode, no skip-intro, no
// Sources or subtitles (both would be the FILM's). `trailerName` is the line
// under the title.
static int  trailerMode;
static char trailerName[96];
static char lineEp[220];          // "T1, E1 · <sinopse curta>", montada na abertura

static const CatItem *item(void) { return cat_item(idx); }
static int epT, epE, reqSources, errorSource, reqNextT, reqNextE;
static int reqDetails;   // the Details button: leave, and land on the title page
// THE CARD CAN BE SENT AWAY. It is offered for the last two minutes of an episode,
// or for the whole of the credits, and that is a long time to keep a panel in the
// corner of a frame somebody is still watching.
//
// The flag belongs to THIS PLAYBACK, not to the session: it is cleared in the same
// reset that clears reqNext* when a title opens, so dismissing the offer on one
// episode does not silently suppress it on the next.
static int nextDismissed;
// Which of the card's two pills has the focus: 0 = Play now, 1 = Not now. Play is
// the primary and is where it starts — the same reasoning as the track menu's
// steppers starting on the plus.
enum { NEXT_PLAY, NEXT_DISMISS, NEXT_NPILLS };
static int nextFocus = NEXT_PLAY;
// THE CARD PLAYS THE NEXT EPISODE ON ITS OWN once the countdown runs out; its
// length is the "Next episode countdown" setting. The "Playing in 8s" on the card
// and the bar across its base are the same clock. It
// holds while the video is paused or a sheet covers the card: nobody is ignoring
// the offer then, and it would be spent while nobody could see it.
#define PLR_NEXT_MS (settings_next_countdown() * 1000.0f)
static float nextElapsed;
static void reportReset(void);   // see REPORTING WHILE IT PLAYS

// THE SKIP BUTTON HIDES ITSELF after ten seconds — the web app's
// SKIP_INTRO_COUNTDOWN_MS. An intro can run two minutes and the offer is answered
// in the first few; left up for the whole of it, it is a panel parked over the
// picture long after the moment it belonged to. The bar across the button's base
// is that countdown made visible, so the disappearance is announced rather than
// just happening.
//
// It is PAUSED while the transport is up, not reset: somebody reading the scrubber
// is not ignoring the offer, and a countdown that ran underneath the controls
// would eat the ten seconds while nobody could see it.
#define PLR_SKIP_MS 10000.0f
static float  skipElapsed;
static double skipChunkEnd;   // which chunk the count belongs to
static int    skipAutoHidden;
// HOW LONG THE PICTURE HAS BEEN ROLLING, in ms of unbroken forward play. The skip
// button waits for PLR_SKIP_LEAD_MS of it. startImage alone is loadCompleted, not
// playback: the button came up over the loading logo's fade, and on a resume it
// flashed at 0:00 before the seek to the saved position landed. Any jump (a seek,
// the resume, a skip) starts the count again; a pause only holds it.
#define PLR_SKIP_LEAD_MS 1000.0f
static float  rolledMs;
// Has THIS load rolled for PLR_SKIP_LEAD_MS at least once? rolledMs drops back
// to 0 on every seek; this does not, so seeking near the end does not blink
// the Up next card away.
static int    rolledOnce;
static int introIdx=-1, introT=-1, introE=-1;
static int resumeApplied, resumePct;

static void fmtTime(char *b, size_t n, float seg, int negative);

// --- THE FAILURE LOG ---------------------------------------------------------
// What the player watches for, and when each is called a failure:
//   source    no addon returned a source, or none of the best ones resolved
//   load      the pipeline refused the URL, reported an error before the first
//             frame, or took PLR_FAIL_LOAD_MS without finishing the load
//   playback  a pipeline error after the picture started, the position frozen
//             for PLR_FAIL_STALL_MS while playing, or endOfStream well short of
//             the file's duration (a truncated or cut-off file)
// Each is logged ONCE per playback (per source, for a source change).
// PLR_FAIL_SEARCH_MS is the exception: a search that slow is logged but gets no
// notice, because it usually ends in a source that plays. If it does not, the
// "source" failure above is logged and shown when the search gives up.
#define PLR_FAIL_LOAD_MS    45000u
#define PLR_FAIL_SEARCH_MS  90000u
#define PLR_FAIL_STALL_MS   20000u
#define PLR_FAIL_NOTICE_MS   7000u
static char   failNotice[400];
static Uint32 failNoticeAt;
static char   failLast[160];          // the last stage+reason logged, for dedupe
static int    failErrSeen, failEosSeen;
static Uint32 loadStartAt, stallAt;
static float  stallPos;
static int    failLoadLogged, failSearchLogged, failStallLogged;

// Milliseconds from `then` to `now`, and 0 when `then` is LATER. The frame's `now`
// is read before app_update, and an open or source change inside app_update stamps
// loadStartAt with a fresh SDL_GetTicks — a millisecond past `now`. The plain
// unsigned subtraction wrapped to ~49 days, and "no picture after 45s" fired on
// the very first frame of a title that went on to play at once.
static Uint32 msSince(Uint32 now, Uint32 then) {
  return (Sint32)(now - then) > 0 ? now - then : 0;
}

// The URL's HOST only. The rest of a debrid link is a signed token, which has no
// business in a log file and says nothing about why the file failed.
static void urlHost(const char *url, char *dst, size_t n) {
  const char *p = url ? strstr(url, "://") : NULL;
  size_t k = 0;
  dst[0] = 0;
  if (!p) return;
  for (p += 3; *p && *p != '/' && *p != ':' && *p != '?' && k < n - 1; p++) dst[k++] = *p;
  dst[k] = 0;
}

static void reportFailure(const char *stage, const char *reason, int notify) {
  const CatItem *c = item();
  // A trailer's source is IMDb's MP4, not the film's stream list: describing the
  // film's current source here would blame a file that was never opened.
  const Stream *st = (stream_n() > 0 && !trailerMode) ? stream_item(stream_current()) : NULL;
  char key[160], line[1400], who[200], name[160], src[600] = "no source chosen";
  if (trailerMode) snprintf(src, sizeof src, "trailer: %s (IMDb MP4)", trailerName);
  snprintf(key, sizeof key, "%s|%s|%d", stage, reason, stream_current());
  if (!strcmp(key, failLast)) return;
  snprintf(failLast, sizeof failLast, "%s", key);

  if (epT > 0) snprintf(name, sizeof name, "%s S%dE%d", c ? c->title : "?", epT, epE);
  else         snprintf(name, sizeof name, "%s", c ? c->title : "?");
  snprintf(who, sizeof who, "%s (%s)", name, c && c->imdb[0] ? c->imdb : "no id");
  if (st) {
    char host[96], size[24] = "size ?";
    const char *box = st->mp4 || strstr(st->url, ".mp4") ? "MP4"
                    : (strstr(st->url, ".mkv") || strstr(st->file, ".mkv")) ? "MKV"
                    : strstr(st->url, ".m3u8") ? "HLS" : "container ?";
    urlHost(st->url, host, sizeof host);
    if (st->sizeMB >= 1024) snprintf(size, sizeof size, "%.1f GB", st->sizeMB / 1024.0);
    else if (st->sizeMB > 0) snprintf(size, sizeof size, "%ld MB", st->sizeMB);
    snprintf(src, sizeof src, "%s%s%s | %s %s %s %s %s %s | %s | %s | host %s",
             st->provider[0] ? st->provider : "?",
             st->service[0] ? " / " : "", st->service,
             st->res[0] ? st->res : "res ?", st->range[0] ? st->range : "SDR",
             st->source, st->codec, st->audio, box, size,
             st->file[0] ? st->file : st->label, host[0] ? host : "?");
  }
  snprintf(line, sizeof line, "%-8s | %s | %s | %s", stage, who, reason, src);
  failure_log(line);
  if (!notify) return;

  // It names the title, so a notice can never be mistaken for one about another.
  snprintf(failNotice, sizeof failNotice, "Playback problem logged · %s%s · %s",
           name, trailerMode ? " trailer" : "", reason);
  failNoticeAt = SDL_GetTicks();
}
void player_report_failure(const char *stage, const char *reason) {
  reportFailure(stage, reason, 1);
}

// Called on every open and every source change: the watches start over.
static void failWatchReset(void) {
  failErrSeen = video_error_count();
  failEosSeen = video_eos_count();
  loadStartAt = SDL_GetTicks();
  stallAt = 0; stallPos = -1.0f;
  failLoadLogged = failSearchLogged = failStallLogged = 0;
  // A notice about the previous title or source is stale the moment a new one loads.
  failNoticeAt = 0;
}
int player_index(void) { return idx; }
const char *player_line_episode(void) { return lineEp; }
void player_episode_current(int *t, int *e) { *t = epT; *e = epE; }
int player_requested_sources(void) { int p = reqSources; reqSources = 0; return p; }
int player_requested_details(void) {
  int p = reqDetails ? idx : -1; reqDetails = 0; return p;
}
int player_requested_next(int *t,int *e) {
  if(!reqNextT||!reqNextE)return 0;
  if(t)*t=reqNextT;if(e)*e=reqNextE;reqNextT=reqNextE=0;return 1;
}
const CatEp *player_next_episode(void) {
  const CatEp *best=NULL;
  if (trailerMode) return NULL;
  for(int i=0;i<cat_n_episodes(idx);i++) {
    const CatEp *p=cat_episode(idx,i);if(!p)continue;
    if(p->season<epT||(p->season==epT&&p->episode<=epE))continue;
    if(!best||p->season<best->season||
       (p->season==best->season&&p->episode<best->episode))best=p;
  }
  return best;
}
void player_error_source(void) { waitingSource = 0; errorSource = 1; visible = 1; playing = 0; }
void player_start_over(void) { resumePct = 0; }
void player_set_episode(int t, int e) {
  const CatItem *c = item();
  epT = t; epE = e; lineEp[0] = 0;
  resumePct = 0;
  if (trailerMode) { epT = epE = 0; intro_off(); return; }
  if (c && c->progress > 0 && c->progress < 90 &&
      (strcmp(c->kind,"series") || (t==c->season && e==c->episode))) resumePct=c->progress;
  if (!c || strcmp(c->kind, "series")) { epT = epE = 0; intro_off(); return; }
  if (epT < 1) epT = c->season > 0 ? c->season : 1;
  if (epE < 1) epE = c->episode > 0 ? c->episode : 1;
  snprintf(lineEp, sizeof lineEp, "S%dE%d", epT, epE);
  if (epT == c->season && epE == c->episode && c->nameEpisode[0])
    snprintf(lineEp, sizeof lineEp, "S%dE%d · %s", epT, epE, c->nameEpisode);
  for (int i = 0; i < cat_n_episodes(idx); i++) {
    const CatEp *ep = cat_episode(idx, i);
    if (ep && ep->season == epT && ep->episode == epE) {
      snprintf(lineEp, sizeof lineEp, "S%dE%d · %s", epT, epE, ep->name);
      break;
    }
  }
  if(idx!=introIdx||epT!=introT||epE!=introE){
    introIdx=idx;introT=epT;introE=epE;intro_request(c->imdb,epT,epE);
  }
}

// --- duration from the catalogue's free text ---------------------------------
// The `meta` field is prose, not data: "2023 · 3 h 28 min" on a film and
// "2022 · 3 seasons" on a series. Instead of a positional parser (which breaks on
// the first title with a different format), I look only for the two number+unit
// pairs anywhere in the string. Finding NEITHER, I return 0 and the caller falls
// back to the default — which is the correct case for a series.
static float durationOfMeta(const char *meta) {
  if (!meta) return 0.0f;
  float h = 0.0f, m = 0.0f;
  int found = 0;
  for (const char *p = meta; *p; p++) {
    if (*p < '0' || *p > '9') continue;
    float v = 0.0f;
    while (*p >= '0' && *p <= '9') { v = v * 10.0f + (*p - '0'); p++; }
    while (*p == ' ') p++;
    // "min" has to be tested BEFORE "m": otherwise every "min" becomes a minute by
    // accident of the prefix — which would even work here, but would hide the bug
    // on the day a new unit starting with m turns up.
    if (!strncmp(p, "min", 3))    { m = v; found = 1; p += 2; }
    else if (*p == 'h')           { h = v; found = 1; }
    if (!*p) break;
  }
  return found ? (h * 3600.0f + m * 60.0f) : 0.0f;
}

// Cuts the synopsis at the first sentence, without passing `maxBytes`. The cut
// respects UTF-8: catalogue titles carry accents and cutting in the middle of a "ç"
// or an "ã" produces an empty rectangle in the font, not a missing accent.
static void fraseFirst(char *dst, size_t n, const char *src, size_t maxBytes) {
  if (!src || !*src) { dst[0] = 0; return; }
  if (maxBytes > n - 4) maxBytes = n - 4;
  size_t i = 0, cut = 0;
  for (; src[i] && i < maxBytes; i++)
    if (src[i] == '.') { cut = i; break; }
  if (!cut) {
    cut = i;
    // go back to the start of a character (continuation bytes are 10xxxxxx)
    while (cut > 0 && ((unsigned char)src[cut] & 0xC0) == 0x80) cut--;
    while (cut > 0 && src[cut - 1] == ' ') cut--;
  }
  memcpy(dst, src, cut);
  dst[cut] = 0;
  if (src[i] && src[i] != '.') strncat(dst, "\xe2\x80\xa6", n - strlen(dst) - 1);
}

// --- ASPECT MODES ------------------------------------------------------------
// The bridge from the web app to the native one. In the web app the mode touches two
// things on the <video> element: `object-fit` and a `transform: scale()`. Here there
// is no element — there is a hardware plane positioned by video_window() — so the
// two become ONE thing: the plane's rectangle.
//
// The translation is literal and in this order, just like the web's
// resolveAspectRender:
//   1. the rectangle the mode's object-fit would produce (contain/cover/fill);
//   2. multiplied by the mode's scale (resolveAspectScale), about the CENTRE of the
//      screen — which is its `transform-origin: center center`.
// The rectangle here is VIRTUAL: it may run off the screen, and running off the
// screen is what "crop" means. But it is NOT what is sent to the plane — see
// applyAspect, which converts it into a source + destination.
//
// A MEASURED MISTAKE, and it is worth writing down because reading the web app leads
// you to it: I sent this rectangle straight to the ACB, with negative x/y and a size
// larger than the screen. The ACB accepted all four calls without complaint and the
// log looked fine —
//   [video] window -144,-81  2208x1242 full=0   <- Light zoom  (1.15)
//   [video] window -326,-184 2573x1447 full=0   <- Cinema zoom (1.34)
//   [video] window -528,-297 2976x1674 full=0   <- Ultra zoom  (1.55)
// — matching the web's resolveAspectRender to the pixel. And THE SCREEN WAS BLACK in
// all three. Accepting the call is not displaying: a hardware plane does not discard
// the excess the way the browser's compositor does with transform: scale(), so a
// rectangle outside the panel does not become a crop, it becomes an invalid
// rectangle and the plane goes dark. Only ORIGINAL showed a picture, being the only
// one at scale 1. The lesson: `resolveAspectScale` was precisely the part of the web
// app that does NOT translate, because the half that did the cropping in the web app
// is not even in the file.
//
// This CANNOT be checked by screenshot: during playback /tmp/nuvio-shot.bmp comes
// out BLACK where the video is, because the plane sits behind the GL surface and
// glReadPixels does not see it. And it was that blindness that let the mistake
// through — the log said success, the capture was black either way, and only someone
// looking at the TV saw. Checking zoom means looking at the device.
static int    aspect = PLR_ASPECT_ORIGINAL;
static Uint32 toastAte = 0;      // until when the mode notice stays up
static char   toastText[96];     // its text when it is not the aspect mode's
static int    toastGood;         // its dot: 1 green, 0 amber
static char   dirPrefs[512];

// Short labels for the on-screen notice. The web app's are "Fit (Original)",
// "Crop", "Stretch", "Slight/Cinema/Ultra Zoom", "Fit Height", "Fit Width" — here
// they are trimmed so the notice reads at a glance from the sofa.
static const char *ASPECT_LABEL[PLR_ASPECT_N] = {
  "Original", "Crop", "Stretch", "Light zoom",
  "Cinema zoom", "Ultra zoom", "Fit height", "Fit width"
};

const char *player_aspect_label(int mode) {
  if (mode < 0 || mode >= PLR_ASPECT_N) mode = PLR_ASPECT_ORIGINAL;
  return ASPECT_LABEL[mode];
}
int player_aspect(void) { return aspect; }

// Where the chosen mode is stored. The same directory main.c passes to the rest of
// the app (SDL_GetBasePath()+"art", with /tmp/art as a fallback). The web app keeps
// this in DeviceLocalPlayerPreferences, per device: choosing "Cinema zoom" and
// finding "Original" again on the next film would turn the mode into a toy.
static const char *prefsFile(void) {
  static char path[600];
  if (!dirPrefs[0]) {
    char *base = SDL_GetBasePath();
    if (base) { snprintf(dirPrefs, sizeof dirPrefs, "%sart", base); SDL_free(base); }
    else      snprintf(dirPrefs, sizeof dirPrefs, "/tmp/art");
  }
  snprintf(path, sizeof path, "%s/player.txt", dirPrefs);
  return path;
}

// THE SUBTITLE STYLE: a DEVICE preference, like the aspect — it does not go into
// settings.txt, which mirrors the web app's layout keys. Default: size 2 (the
// device's), white, no background, centre position, outline.
static VideoSubtitleStyle subStyle = { 120, 0, 0, 3, 1, 0, 0, TXT_FAMILY_INTER };

// Keys written by 1.0.1 and earlier. Reading them keeps the device's aspect
// and subtitle style across the rename instead of silently resetting to the
// defaults; the file is rewritten with the new names on the next change.
static const char *canonicalKey(const char *k) {
  static const struct { const char *old, *new; } T[] = {
    { "aspect",       "aspect"         }, { "leg_tamanho", "sub_size"     },
    { "leg_cor",       "sub_color"      }, { "leg_fundo",   "sub_background" },
    { "leg_pos",       "sub_position"   }, { "leg_borda",   "sub_border"   },
    { "leg_atraso",    "sub_delay"      }, { "leg_opacidade","sub_opacity" },
    { "leg_familia",   "sub_family"     },
  };
  size_t i;
  for (i = 0; i < sizeof T / sizeof *T; i++)
    if (!strcmp(k, T[i].old)) return T[i].new;
  return k;
}

static void prefsRead(void) {
  FILE *f = fopen(prefsFile(), "r");
  char raw[64]; const char *key; int v;
  if (!f) return;
  while (fscanf(f, "%63s %d", raw, &v) == 2) {
    key = canonicalKey(raw);
    // A value from another version (or a hand-edited file) falls back to the default
    // instead of indexing outside the label array.
    if (!strcmp(key, "aspect") && v >= 0 && v < PLR_ASPECT_N) aspect = v;
    else if (!strcmp(key, "sub_size")) {
      /* Migrates the old 0..4 file without losing the device's preference. */
      static const int old[5]={60,80,120,160,200};
      if(v>=0&&v<=4)subStyle.size=old[v];
      else if(v>=50&&v<=200)subStyle.size=(v/10)*10;
    }
    else if (!strcmp(key, "sub_color")     && v >= 0 && v < VIDEO_SUB_NCOLORS) subStyle.color = v;
    else if (!strcmp(key, "sub_background")   && v >= 0 && v <= 4)  subStyle.background = v;
    else if (!strcmp(key, "sub_position")     && v >= 0 && v <= 7)  subStyle.position = v;
    else if (!strcmp(key, "sub_border")   && v >= 0 && v <= 2)  subStyle.border = v;
    else if (!strcmp(key, "sub_delay")  && v > -10000 && v < 10000) subStyle.delayMs = v;
    else if (!strcmp(key, "sub_opacity") && v >= 0 && v <= 3) subStyle.opacity = v;
    else if (!strcmp(key, "sub_family") && v >= 0 && v < TXT_FAMILY_N) subStyle.family = v;
  }
  fclose(f);
}

static void prefsWrite(void) {
  FILE *f = fopen(prefsFile(), "w");
  if (!f) return;
  fprintf(f, "aspect %d\n", aspect);
  fprintf(f, "sub_size %d\n", subStyle.size);
  fprintf(f, "sub_color %d\n",     subStyle.color);
  fprintf(f, "sub_background %d\n",   subStyle.background);
  fprintf(f, "sub_position %d\n",     subStyle.position);
  fprintf(f, "sub_border %d\n",   subStyle.border);
  fprintf(f, "sub_delay %d\n",  subStyle.delayMs);
  fprintf(f, "sub_opacity %d\n", subStyle.opacity);
  fprintf(f, "sub_family %d\n", subStyle.family);
  fclose(f);
}

// Read by the tracks sheet, which is what draws the controls.
VideoSubtitleStyle *player_sub_style(void) { return &subStyle; }
void player_sub_style_changed(void) {
  video_subtitle_style(&subStyle);
  prefsWrite();
}

// The decoded FRAME's aspect ratio. With no videoInfo yet, 16:9 — which is the
// aspect of almost every file delivered, and the assumption that makes "Original"
// open full screen instead of flashing a wrong band for a second.
static float aspectFrame(void) {
  int w = video_width(), h = video_height();
  if (w > 0 && h > 0) return (float)w / (float)h;
  return NV_SCREEN_W / NV_SCREEN_H;
}

typedef struct { float x, y, w, h; } PlrRect;

static PlrRect aspectRect(int mode) {
  const float screen = NV_SCREEN_W / NV_SCREEN_H;
  float q = aspectFrame();
  float bw, bh, sx = 1.0f, sy = 1.0f;
  PlrRect r;
  if (q <= 0.0f) q = screen;

  // 1) the mode's object-fit. The three cases are those of ASPECT_MODE_DEFINITIONS.
  switch (mode) {
    case PLR_ASPECT_STRETCH:                       // fill
      bw = NV_SCREEN_W; bh = NV_SCREEN_H;
      break;
    case PLR_ASPECT_CROP:                          // cover
    case PLR_ASPECT_ZOOM_LIGHT:
    case PLR_ASPECT_ZOOM_CINEMA:
    case PLR_ASPECT_FIT_HEIGHT:
      if (q > screen) { bh = NV_SCREEN_H; bw = bh * q; }
      else          { bw = NV_SCREEN_W; bh = bw / q; }
      break;
    default:                                    // contain
      if (q > screen) { bw = NV_SCREEN_W; bh = bw / q; }
      else          { bh = NV_SCREEN_H; bw = bh * q; }
      break;
  }

  // 2) the mode's scale, copied line by line from resolveAspectScale.
  switch (mode) {
    case PLR_ASPECT_CROP:        sx = sy = (q > screen) ? q / screen : screen / q; break;
    case PLR_ASPECT_STRETCH:     if (q > screen) sy = q / screen; else sx = screen / q; break;
    case PLR_ASPECT_ZOOM_LIGHT:   sx = sy = PLR_ZOOM_LIGHT;   break;
    case PLR_ASPECT_ZOOM_CINEMA: sx = sy = PLR_ZOOM_CINEMA; break;
    case PLR_ASPECT_ZOOM_ULTRA:  sx = sy = PLR_ZOOM_ULTRA;  break;
    case PLR_ASPECT_FIT_HEIGHT:  if (q > screen) sx = sy = q / screen; break;
    case PLR_ASPECT_FIT_WIDTH: if (q < screen) sx = sy = screen / q; break;
    default: break;   // ORIGINAL: contain and nothing more
  }

  r.w = bw * sx;
  r.h = bh * sy;
  r.x = (NV_SCREEN_W - r.w) * 0.5f;
  r.y = (NV_SCREEN_H - r.h) * 0.5f;
  return r;
}

// The mode's VISIBLE rectangle: the virtual rectangle clipped by the screen. It is
// what the GL hole follows and what becomes the plane's destination.
static PlrRect aspectVisible(int mode) {
  PlrRect r = aspectRect(mode), d;
  d.x = r.x < 0.0f ? 0.0f : r.x;
  d.y = r.y < 0.0f ? 0.0f : r.y;
  d.w = (r.x + r.w > NV_SCREEN_W ? NV_SCREEN_W : r.x + r.w) - d.x;
  d.h = (r.y + r.h > NV_SCREEN_H ? NV_SCREEN_H : r.y + r.h) - d.y;
  if (d.w < 0.0f) d.w = 0.0f;
  if (d.h < 0.0f) d.h = 0.0f;
  return d;
}

// Sends the mode to the hardware plane. Called on opening, on a mode change and
// when the videoInfo arrives — before that the frame's aspect ratio is a guess, and
// a mode computed from the guess would be wrong precisely on widescreen films,
// which are the reason all of this exists.
//
// HERE WAS THE MISTAKE that left the screen black in every zoomed mode. I sent the
// VIRTUAL rectangle straight to the plane — with negative x/y and a size larger
// than the screen — on the assumption that the excess would run off the edge, as it
// does in the web app. In the web app what discards the excess is the browser's
// compositor; a hardware plane has no such step, and a rectangle outside the panel
// is not a crop, it is an invalid rectangle: the plane goes dark. Only ORIGINAL
// survived, being the only one at scale 1.
//
// The right arithmetic is the INVERSE: the destination never leaves the screen, and
// the zoom becomes a SMALLER piece of the SOURCE. The virtual rectangle is still the
// web app's — it simply stops being what is sent and becomes what is USED TO WORK
// OUT which slice of the frame falls inside the screen.
static void applyAspect(void) {
  PlrRect r, d;
  float qw, qh;
  int sx, sy, sw, sh;
  if (!hasVideo) return;

  r = aspectRect(aspect);
  d = aspectVisible(aspect);
  if (d.w < 1.0f || d.h < 1.0f || r.w < 1.0f || r.h < 1.0f) return;

  qw = (float)video_width();
  qh = (float)video_height();
  // Without the frame's dimensions there is no way to speak in source coordinates.
  // It falls back to the old path, which serves the case with no crop — the only one
  // where it works. As soon as the videoInfo arrives, applyAspect runs again.
  if (qw < 2.0f || qh < 2.0f) {
    video_window((int)(d.x + 0.5f), (int)(d.y + 0.5f),
                 (int)(d.w + 0.5f), (int)(d.h + 0.5f));
    return;
  }

  // Which slice of the frame falls inside the destination: the whole frame maps onto
  // the virtual rectangle `r`, so the slice is the proportion of `d` within `r`.
  sx = (int)((d.x - r.x) / r.w * qw + 0.5f);
  sy = (int)((d.y - r.y) / r.h * qh + 0.5f);
  sw = (int)(d.w / r.w * qw + 0.5f);
  sh = (int)(d.h / r.h * qh + 0.5f);
  // Even: the scaler works in 4:2:0 and an odd origin or size gives half a pixel of
  // chroma shift at the crop's edge.
  sx &= ~1; sy &= ~1; sw &= ~1; sh &= ~1;
  if (sx < 0) sx = 0;
  if (sy < 0) sy = 0;
  if (sx + sw > (int)qw) sw = (int)qw - sx;
  if (sy + sh > (int)qh) sh = (int)qh - sy;

  video_window_source(sx, sy, sw, sh,
                     (int)(d.x + 0.5f), (int)(d.y + 0.5f),
                     (int)(d.w + 0.5f), (int)(d.h + 0.5f));
}

void player_aspect_set(int mode) {
  if (mode < 0 || mode >= PLR_ASPECT_N) mode = PLR_ASPECT_ORIGINAL;
  aspect = mode;
  prefsWrite();
  applyAspect();
}

void player_aspect_cycle(void) {
  player_aspect_set((aspect + 1) % PLR_ASPECT_N);
  toastText[0] = 0;
  toastAte = SDL_GetTicks() + PLR_TOAST_MS;
}

// Twice the aspect notice's time: that one confirms a key just pressed, this one
// announces something nobody asked for, and has to be read from the sofa.
void player_toast(const char *text, int good) {
  snprintf(toastText, sizeof toastText, "%s", text ? text : "");
  toastGood = good;
  toastAte = SDL_GetTicks() + 2 * PLR_TOAST_MS;
}

static void openSession(int indexCatalog, const char *url) {
  int n = cat_n(); if (n < 1) n = 1;
  idx = ((indexCatalog % n) + n) % n;
  is_open = 1; exiting = 0; requestedExit = 0; barFocus = 0;
  // The title's parental guide: requested HERE and not while drawing, so the answer
  // has already arrived when the controls appear for the first time.
  { const CatItem *ci = cat_item(idx);
    if (ci && ci->imdb[0]) parental_request(ci->imdb); }
  playing = 1; visible = 1; anim = 0.0f; entry = 0.0f; entryV = 0.0f;
  fromDetail = 0; flyFrom = (GfxRect){ 0, 0, 0, 0 }; flying = 0;
  reqSources = errorSource = reqTracks = reqNextT = reqNextE = reqDetails = 0;
  startImage = 0;
  nextDismissed = 0; nextFocus = NEXT_PLAY; nextElapsed = 0;
  reportReset();
  skipElapsed = 0; skipChunkEnd = 0; skipAutoHidden = 0; rolledMs = 0; rolledOnce = 0;
  resumeApplied=0;
  button = PLR_PLAY;
  memset(focusB, 0, sizeof focusB);
  posSeg = 0.0f;
  lastInput = openedAt = SDL_GetTicks();
  loadFill = 0.0f; loadEndAt = 0; wasLoading = 1;
  failLast[0] = 0; failWatchReset();
  waitingSource = (url == NULL);
  // An external subtitle belongs to the session that has just ended, not to this one.
  tracks_reset();
  autosync_reset();
  // The aspect mode belongs to the DEVICE, not to the session: rereading here is
  // what makes "Cinema zoom" still apply on the next film, as in the web app.
  prefsRead();
  toastAte = 0;
  hasVideo = (url && *url && video_play(url));
  // The saved style has to REACH THE PIPELINE, not just this file's copy of it.
  // It only ever went over when a setting was changed, so every title opened with
  // the file's own subtitles in the TV's defaults — and anything that adjusts the
  // pipeline's subtitle waited for a change that never came. Sent after play so it
  // carries the new media id; the pipeline reapplies it once the load completes.
  video_subtitle_style(&subStyle);
  applyAspect();

  const CatItem *c = item();
  float d = c ? durationOfMeta(c->meta) : 0.0f;
  durationSeg = d > 1.0f ? d : PLR_DURATION_DEFAULT;

  // The episode's identity is independent of the focus in the navigation panel.
  player_set_episode(c ? c->season : 0, c ? c->episode : 0);
  if (url && *url && !hasVideo) {
    player_report_failure("load", "the pipeline refused the URL");
    player_error_source();
  }
}

void player_open(int indexCatalog, const char *url) {
  trailerMode = 0; trailerName[0] = 0;
  openSession(indexCatalog, url);
}

void player_open_trailer(int indexCatalog, const char *url, const char *name) {
  trailerMode = 1;
  snprintf(trailerName, sizeof trailerName, "%s", name && name[0] ? name : "Trailer");
  // A progressive MP4 with no Dolby Vision. Both are the LAST SOURCE's claims
  // otherwise, and a DV claim left over from a film costs the trailer its picture
  // for NV_DV_DEADLINE_MS.
  video_set_dv(0);
  video_set_mp4(1);
  openSession(indexCatalog, url);
}

int player_is_trailer(void) { return is_open && trailerMode; }

int player_is_open(void)    { return is_open; }

void player_open_from_detail(GfxRect from) {
  if (!is_open) return;
  fromDetail = 1;
  flyFrom = from;
}
// Until the art has all but covered the page. Past 0.98 what is left of the page is
// under two percent of the art's opacity, and the veil over both darkens it further.
//
// AND FOR THE WHOLE WAY BACK, when Back cancels a source that never opened. Leaving
// the page out of the exit faded the loading screen to BLACK, and then the title
// screen arrived whole in one frame — with the logo, which had been flying back at
// full opacity, cutting over to the page's copy of itself: the flicker. With the page
// underneath, the exit is the entrance played backwards.
//
// NOT ONCE THERE IS A PICTURE. The video is a hardware plane behind this surface and
// the player punches a hole to show it; a page drawn underneath would be erased by
// that hole wherever the frame is, so leaving playback keeps its plain fade.
// Whether the page should hide its logo: only while THIS screen is drawing one in
// flight. The error screen has none, and a page hiding its logo under it would bring
// the title back without one and then pop it in at the end.
int player_logo_in_flight(void) { return player_handing_off() && flying; }

int player_handing_off(void) {
  if (!is_open || !fromDetail || player_has_video()) return 0;
  return exiting || entry < 0.98f;
}
int player_wants_exit(void) { return requestedExit; }
// Only after loadCompleted. Before that the pipeline has put nothing on the hardware
// plane, and punching the surface early swapped the art for a BLACK rectangle while
// the stream opened — which was the "you press play and it goes black".
void player_set_source(const char *url) {
  if (!is_open || !url || !*url) return;
  // A source CHANGE mid-playback starts a new load from empty; the first source
  // of an opening carries on from the search stage it just finished.
  if (!waitingSource) { loadFill = 0.0f; loadEndAt = 0; }
  rolledOnce = 0;
  waitingSource = 0;
  errorSource = 0;
  failWatchReset();
  hasVideo = video_play(url);
  if (!hasVideo) {
    player_report_failure("load", "the pipeline refused the URL");
    player_error_source();
  }
  applyAspect();
}

// Consumes the request to open the tracks sheet: whoever reads it, clears it.
int  player_requested_tracks(void) { int v = reqTracks; reqTracks = 0; return v; }

int  player_has_video(void) { return hasVideo && video_ready(); }

// The stream is opening: video has been requested, but there is no picture yet.
int  player_loading(void) { return waitingSource || (hasVideo && !video_ready()); }
int  player_controls_visible(void) { return visible; }

// HAS THE TITLE BEEN WATCHED TO THE END? One answer for the whole player.
//
// It used to be two. The "Up next" card declares the episode over at the credits
// or in the last 120 s (offerNext), while closing only rounded up in the last
// 60 s. In an 18-minute episode the card comes up at 89%, and accepting the next
// episode the app itself offered saved 89%: Trakt only marks at 90%, so nothing
// was marked and the episode stayed unwatched (upstream issue #100). The last
// 60 s stay in, for a title with no credits marker too short for 120 s to mean
// anything.
static int watchedToEnd(double pos, double dur) {
  double credits = intro_credits_start();
  if (dur <= 1.0) return 0;
  if (pos >= dur - 60.0) return 1;
  if (credits > 0 && pos >= credits) return 1;
  return dur - pos <= 120.0;
}

void player_shutdown(void) {
  // Save BEFORE stopping: video_stop unloads the pipeline and the position goes with
  // it. A title watched to the end counts as watched in full — going back to a card
  // saying "2 min left" when it has actually finished is worse than rounding.
  if (hasVideo && video_ready() && durationSeg > 1.0f && !trailerMode) {
    int done = watchedToEnd(posSeg, durationSeg);
    float pos = done ? durationSeg : posSeg;
    const CatItem *ci = cat_item(idx);
    home_record_return(idx, pos, durationSeg);
    cat_save_progress_ep(idx, pos, durationSeg,epT,epE);
    // And to Trakt too, which is where "continue watching" comes from: recording only
    // here would leave this app disagreeing with the owner's other devices.
    if (ci && ci->imdb[0]) {
      char id[64];
      if (epT > 0 && epE > 0) snprintf(id, sizeof id, "%.*s:%d:%d", (int)strcspn(ci->imdb,":"),ci->imdb, epT, epE);
      else snprintf(id, sizeof id, "%s", ci->imdb);
      trakt_mark(id, pos, durationSeg);
      // The episode list's check mark is a separate record from the progress bar,
      // and nothing here wrote it: it only appeared after the next Trakt read.
      // Set it now; that read corrects it if the server refused.
      if (done && epT > 0 && epE > 0) watchedep_set(ci->imdb, epT, epE, 1);
      // Without Trakt, the finished film or episode goes into the ACCOUNT's
      // watched list — the one the web and the phone read when Trakt is off.
      if (done && acclib_active()) {
        if (epT > 0 && epE > 0) acclib_watched(ci->imdb, "series", epT, epE, 1);
        else if (strcmp(ci->kind, "series")) acclib_watched(ci->imdb, "movie", 0, 0, 1);
      }
      // And to the ACCOUNT. Trakt and the account are two different destinations: not
      // every user turns Trakt on, and the official app's progress comes from the account.
      sync_dirty_progress();
    }
    // AND THE HOME IS ASKED TO REBUILD, which is what makes the row agree with
    // what has just happened.
    //
    // "Continue watching" is assembled once, on the discovery thread, and
    // nothing ever reconsidered it: closing the player updated this item's
    // progress bar in place and left the ROW in the order and the membership it
    // had at launch. So the series just watched stayed wherever it was, and the
    // episode just finished stayed on the card instead of giving way to the
    // next one — both only corrected themselves on the following launch.
    //
    // A rebuild and not a partial refresh: the row is a WINDOW into the
    // catalogue array (start and count), so changing what is in it means
    // rebuilding that array, and disc_rebuild is the path that already does it
    // safely. It is not the flicker it would have been either — partialAllowed
    // is false once a complete catalogue is on screen, so the rebuild publishes
    // once, at the end, as a single swap.
    disc_rebuild();
  }
  if (hasVideo) video_stop();
  reportReset();
  hasVideo = 0; waitingSource = 0; is_open = 0; exiting = 0; requestedExit = 0;
  statsOpen = 0;
  seekActive = 0; seekRepeats = 0; seekDir = 0;
  seekAt = seekEndAt = settleAt = 0;
  quickAt = 0; quickDelta = 0.0f; quickAnim = 0.0f; quickPulse = 0.0f;
  startImage = 0; rolledMs = 0; rolledOnce = 0;
  episodes_close();
  intro_off(); introIdx=introT=introE=-1;
  subtitle_off();
}

// THE VISIBLE ROW. Episodes only exists for a series, and it sits in the middle of
// the order rather than at the end, so "which buttons are on screen" can no longer
// be expressed as a count. Everything that walks the row — the draw, LEFT/RIGHT,
// and the label — goes through this, so there is one answer and not three.
static int rowButtons(int *out) {
  int n = 0;
  out[n++] = PLR_PLAY;
  if (epT > 0 && player_next_episode()) out[n++] = PLR_NEXT;
  if (!trailerMode) out[n++] = PLR_CC;
  out[n++] = PLR_AUDIO;
  if (epT > 0) out[n++] = PLR_EPISODES;
  if (!trailerMode) out[n++] = PLR_SOURCES;
  out[n++] = PLR_ASPECT;
  out[n++] = PLR_STATS;
  if (!trailerMode) out[n++] = PLR_DETAILS;
  return n;
}

// Where the focused action sits in that row. -1 cannot happen from the row's own
// navigation, but it can from a title changing under the screen (a series episode
// followed by a film), and the callers clamp on it rather than indexing past the end.
static int rowSlot(const int *row, int n, int action) {
  for (int i = 0; i < n; i++) if (row[i] == action) return i;
  return -1;
}

// THE STREAM'S OWN FACTS, as ONE line: "4K · Dolby Vision · Dolby Atmos".
//
// They come from the STREAM and not from a constant: the two used to be hard-coded
// and announced Dolby Vision on an HDR10 file and Atmos on a stereo track. A badge
// that lies is worse than no badge, because it is what the owner trusts to know
// whether they got the good version.
//
// "Dolby Vision" only when the PIPELINE returned DolbyVision in the videoInfo —
// video_has_dolby_vision no longer reads the addon's claim. MEASURED on this TV,
// from the log while playing an MKV the addon advertised as Dolby Vision:
//   [video] pipeline HDR: HDR10 (source claimed DV=1)
// When the pipeline says HDR10 the line says HDR10; staying quiet would hide half
// the answer.
//
// ONE line, not one TxtLine per fact. That is what killed the stagger these used to
// need: three separate lines hit the rasteriser's TXT_PER_FRAME budget (text.c:40)
// and arrived over three frames, which read as a defect, and the fix at the time
// was to OWN the staggering with a curve. A single line has nothing to stagger —
// it rasterises once and is then a cache hit for the rest of the playback.
static int streamFacts(char *dst, size_t n) {
  const char *f[3];
  int nf = 0;
  char res[16] = "";
  if (video_width() >= 3840)      snprintf(res, sizeof res, "4K");
  else if (video_width() >= 1920) snprintf(res, sizeof res, "HD");
  if (res[0]) f[nf++] = res;
  if (video_has_dolby_vision())               f[nf++] = "Dolby Vision";
  else if (!strcasecmp(video_hdr(), "HDR10")) f[nf++] = "HDR10";
  if (video_has_atmos())                      f[nf++] = "Dolby Atmos";
  dst[0] = 0;
  for (int i = 0; i < nf; i++) {
    if (i) strncat(dst, " \xc2\xb7 ", n - strlen(dst) - 1);
    strncat(dst, f[i], n - strlen(dst) - 1);
  }
  return nf;
}

// THE PLAY BUTTON WHILE THE SOURCE OPENS: a dark disc with a thin rim, a dimmed
// play glyph, and a bright quarter arc running round inside the rim — about one
// turn a second. It replaces the white focus puck for as long as it spins: there
// is nothing to play or pause yet, and a lit button would promise an action.
//
// gfx has no arc mode, so the arc is a run of overlapping round dots. At a 1.4px
// step against a 5px dot the steps are invisible and the ends come out round,
// which is the stroke-linecap the design's arc has.
static void iconFile(float cx, float cy, float a, float luma,
                     const char *name, float size);
#define PLR_SPIN_ARC   1.55f   // radians, a little under a quarter turn
#define PLR_SPIN_TURN  1100.0f // ms per revolution
static void drawStartingButton(float cx, float cy, Uint32 now, float a) {
  const float d = PLR_BTN_D, rArc = d * 0.5f - 9.0f, dot = 5.0f;
  float head = (float)(now - openedAt) * (6.2831853f / PLR_SPIN_TURN);
  int k, n = (int)(PLR_SPIN_ARC * rArc / 1.4f);
  gfx_color((GfxRect){ cx - d * 0.5f, cy - d * 0.5f, d, d }, 0.5f,
            0.16f, 0.16f, 0.17f, 0.92f * a);
  gfx_rect((GfxRect){ cx - d * 0.5f, cy - d * 0.5f, d, d }, 0, GFX_RING_INSET, 0,
           2.0f / d, 0, 0.5f, 1, 1, 1, 0.85f * a);
  for (k = 0; k <= n; k++) {
    float ang = head - PLR_SPIN_ARC * k / n;
    gfx_color((GfxRect){ cx + cosf(ang) * rArc - dot * 0.5f,
                         cy + sinf(ang) * rArc - dot * 0.5f, dot, dot },
              0.5f, 1, 1, 1, a);
  }
  iconFile(cx, cy, a * 0.45f, 0.94f, "play", PLR_ICON_H * 0.6f);
}

// WHEN THE CARD COMES UP: the credits marker when there is one, else the lead the
// Settings rows give — a fixed time before the end or a share of the episode,
// the web app's two nextEpisodeThresholdMode values. The extra second covers a
// lead of 0: the position stops a fraction short of the duration, and "at the
// end" has to be reachable.
static int offerNext(void) {
  const CatEp *p=player_next_episode();double end;int kind;
  if(!p||durationSeg<=1)return 0;
  if(intro_active(posSeg,&end,&kind)&&kind==INTRO_CREDITS)return 1;
  return durationSeg-posSeg<=settings_next_lead(durationSeg)+1.0;
}

// Is the card actually on screen? offerNext() alone is not that question any more,
// and the difference matters to more than the drawing: the subtitle line is lifted
// to clear this card, and a dismissed card must not go on pushing it up.
// NOTHING IS OFFERED BEFORE THERE IS A PICTURE. startImage is stamped on the first
// frame that actually has one (see player_update), so it is the honest answer to
// "has this episode started". The web app gates its own skip button the same way,
// on isSkipIntroPlaybackReady.
//
// Without it both prompts were decided from posSeg while posSeg was still 0: an
// intro chunk beginning at 0 made the skip button appear over the loading spinner,
// before the show it was offering to skip had drawn a frame.
static int playbackReady(void) { return startImage != 0 || !hasVideo; }

// A little ahead of the card: the prefetch takes as long as the slowest addon,
// and it should be in hand by the time the card offers Next.
// A minute ahead of a longer lead: at 90% of an hour the card is up six minutes
// before the end, and the sources should be there when it is.
#define PLR_PREFETCH_S 180.0f
const CatEp *player_prefetch_next(void) {
  double end, ahead; int kind;
  if (!is_open || waitingSource || errorSource || !playbackReady()) return NULL;
  if (durationSeg <= 1.0f) return NULL;
  ahead = settings_next_lead(durationSeg) + 60.0;
  if (ahead < PLR_PREFETCH_S) ahead = PLR_PREFETCH_S;
  if (!(intro_active(posSeg, &end, &kind) && kind == INTRO_CREDITS) &&
      durationSeg - posSeg > ahead) return NULL;
  return player_next_episode();
}

static int nextCardUp(void) {
  // NOT WHILE AN EPISODE IS LOADING. playbackReady() is true for the whole
  // source search (there is no pipeline yet, so !hasVideo), and on the TV a
  // picture that has only just landed may still be settling a resume seek. The
  // card waits until this episode has actually rolled, as the skip button does.
  if (!playbackReady() || player_loading() || errorSource) return 0;
  if (hasVideo && !rolledOnce) return 0;
  return offerNext() && !nextDismissed && player_next_episode() != NULL;
}

// Is there a skip offer, and has it not already timed out? The card outranks it,
// so this answers only for the state in which the skip button is the thing on
// screen — which is also the state in which OK belongs to it.
static int skipUp(double *end, int *kind) {
  double e; int k;
  if (!playbackReady() || rolledMs < PLR_SKIP_LEAD_MS) return 0;
  if (nextCardUp()) return 0;
  if (!intro_active(posSeg, &e, &k) || k == INTRO_CREDITS) return 0;
  if (skipAutoHidden) return 0;
  if (end) *end = e;
  if (kind) *kind = k;
  return 1;
}

// Is there anything floating above the bar to focus? Either prompt counts — they
// occupy the same corner and only one is ever up at a time.
static int cardPresent(void) { return nextCardUp() || skipUp(NULL, NULL); }

// Does the card read as focused? With the controls DOWN it is what OK acts on, so
// it is focused by default; with them up it is focused only once UP has walked the
// ladder onto it.
static int cardHot(void) { return !visible || cardFocus; }

// OK on whichever prompt is up. It is reached from two places — the controls-down
// path, where the card is what OK means by default, and the top rung of the
// ladder with the controls up — so it lives here rather than being written twice
// and drifting.
static int activateCard(void) {
  double end; int kind;
  if (nextCardUp()) {
    const CatEp *p;
    if (nextFocus == NEXT_DISMISS) { nextDismissed = 1; return 1; }
    p = player_next_episode();
    if (p) { reqNextT = p->season; reqNextE = p->episode; }
    return 1;
  }
  if (skipUp(&end, &kind)) {
    posSeg = (float)end + .25f;
    if (hasVideo) video_fetch(posSeg);
    return 1;
  }
  return 0;
}

// Advances the countdown. Called once a frame from player_update.
static void skipTick(float dt) {
  double end; int kind;
  if (!intro_active(posSeg, &end, &kind) || kind == INTRO_CREDITS) {
    skipChunkEnd = 0; skipElapsed = 0; skipAutoHidden = 0;
    return;
  }
  // A DIFFERENT CHUNK IS A DIFFERENT OFFER. Without this the recap later in the
  // episode would inherit the intro's spent countdown and never appear at all.
  if (end != skipChunkEnd) {
    skipChunkEnd = end; skipElapsed = 0; skipAutoHidden = 0;
  }
  if (visible || skipAutoHidden || !playbackReady() || rolledMs < PLR_SKIP_LEAD_MS)
    return;
  skipElapsed += dt * 1000.0f;
  if (skipElapsed >= PLR_SKIP_MS) {
    skipAutoHidden = 1;
    // The countdown is a ten-second animation on a button nobody can photograph:
    // the plane cannot be captured and neither can a moment. One line at the end
    // of it is the only way to tell "the timer ran" from "the timer never
    // started", which is exactly the pair that looked identical on screen while
    // the bar was drawing white on white.
    printf("[player] skip offer timed out after %.1fs\n", skipElapsed / 1000.0f);
    fflush(stdout);
  }
}

// Advances the next-episode countdown and, when it runs out, asks for the next
// episode exactly as OK on Play would. Called once a frame from player_update.
//
// With "Autoplay next episode" off the card still comes up, and waits: nothing
// counts, and only Play now moves on — the web app's autoplayNextEpisode.
// A playback that has REACHED ITS END still counts, although it no longer plays:
// with a short lead the file can end inside the countdown, and a clock that
// stopped there would leave the viewer on the last frame for good.
static void nextTick(float dt) {
  const CatEp *p;
  int ended = durationSeg > 1.0f && posSeg >= durationSeg - 1.0f;
  if (!nextCardUp()) { nextElapsed = 0; return; }
  if (!settings_next_autoplay()) { nextElapsed = 0; return; }
  if ((!playing && !ended) || episodes_shown() > 0.0f || tracks_shown() > 0.0f ||
      stream_sheet_shown() > 0.0f) return;
  nextElapsed += dt * 1000.0f;
  if (nextElapsed < PLR_NEXT_MS) return;
  nextElapsed = PLR_NEXT_MS;
  p = player_next_episode();
  if (p && !reqNextT) {
    reqNextT = p->season; reqNextE = p->episode;
    printf("[player] next-episode countdown ran out, playing S%dE%d\n",
           p->season, p->episode);
    fflush(stdout);
  }
}

// No wrap-around, as on the button row.
static void nextStep(int dir) {
  int f = nextFocus + dir;
  if (f >= 0 && f < NEXT_NPILLS) nextFocus = f;
}

// Every key wakes the controls, including one that has already carried out an
// action: on the device there is no command that happens with the bar hidden without
// bringing the bar along — the user needs to see the effect of what they pressed.
// THE PAUSE OVERLAY (NuvioTV's PauseOverlay): paused, and nobody has touched the
// remote for the Settings row's delay (settings_pause_overlay_ms, 0 = off), the frame goes behind a blur and the title
// introduces itself again. `pauseAnim` follows the target; any input drops it —
// wake() moves lastInput, and lastInput is the whole trigger.
static float pauseAnim;
static int   pauseOn;

static int pauseWanted(Uint32 now) {
  return !playing && !trailerMode && !exiting && !errorSource && !seekActive &&
         playbackReady() && !player_loading() && !statsOpen &&
         !episodes_is_open() && !stream_sheet_is_open() && !tracks_is_open() &&
         settings_pause_overlay_ms() > 0 &&
         msSince(now, lastInput) > (Uint32)settings_pause_overlay_ms();
}

static void wake(void) { visible = 1; lastInput = SDL_GetTicks(); }

static void togglePlaying(void) {
  playing = !playing;
  if (hasVideo) video_pause(!playing);
  // THE FOCUS FOLLOWS THE THING THAT ANSWERED. Play/pause is reachable three ways
  // that are not the play button — OK with the controls hidden, OK while the bar
  // has focus, and OK on the button itself — and only the third left the puck
  // anywhere near the glyph that had just changed. Pausing from the bar, or from a
  // clean frame with the row last parked on Subtitles, raised the controls with a
  // white circle sitting on a button that had done nothing.
  //
  // It is also the answer to "what do I press to undo that": the puck is already on
  // it, so resuming is one press of OK rather than a hunt along the row.
  button = PLR_PLAY;
  barFocus = 0;
}

// The bar's seek, with the ramp. The controls-down arrows go through quickSeek.
// Moves the TARGET. Nothing is sent to the pipeline here — see commitSeek.
static void seekBegin(void) {
  if (seekActive) return;
  seekActive = 1;
  seekPreview = posSeg;
  seekRepeats = 0;
  seekDir = 0;
  quickDelta = 0.0f;
}

static void jump(int dir) {
  seekBegin();
  // A change of direction restarts the ramp, like the web app's
  // `direction !== this.seekPreviewDirection` — reversing is a correction, and
  // correcting at 120s a press would be unusable.
  if (dir != seekDir) seekRepeats = 0;
  seekDir = dir;
  seekRepeats++;
  seekPreview = anim_clamp(seekPreview + dir * seekStepFor(seekRepeats),
                              0.0f, durationSeg);
  seekAt = SDL_GetTicks();
}

// The controls-down seek (see THE QUICK SEEK). The sum is taken from what the
// clamp let through, so at the end of the film the readout stops with the bar.
static void quickSeek(int dir) {
  float from;
  seekBegin();
  from = seekPreview;
  seekDir = dir;
  seekPreview = anim_clamp(seekPreview + dir * PLR_QUICK_STEP_S, 0.0f, durationSeg);
  quickDelta += seekPreview - from;
  seekAt = quickAt = SDL_GetTicks();
  quickPulse = 1.0f;
}

// Sends the burst's target to the pipeline, once.
static void commitSeek(void) {
  if (!seekActive) return;
  seekActive = 0; seekRepeats = 0; seekDir = 0;
  seekEndAt = SDL_GetTicks();
  posSeg = seekPreview;
  settleTarget = seekPreview;
  settleAt = seekEndAt;
  if (hasVideo) video_fetch(seekPreview);
}

// --- THE MAGIC REMOTE'S POINTER -------------------------------------------
//
// Moving it raises the controls, as any key does (player_update). Pointing at a
// button focuses it and a click presses it, through the same OK as the remote.
// The bar lights under the pointer, and a click on it SEEKS THERE — held, it
// drags the aim along and lets go where the button comes up. A click on the
// picture itself, on nothing, pauses or resumes, the web player's gesture.

// Where the bar was drawn last frame, for turning a pointer x into a time.
static float barX, barW;
// 1 while a click that landed on the bar is still held: the aim follows the
// pointer until the button comes up, and only then is the seek sent.
static int barDrag;
// THE HOVER READOUT (.player-progress-hover): a marker on the bar under the
// pointer and a bubble above it with the time a click there would seek to —
// YouTube's scrub preview, minus the thumbnail. hoverFrac is the last aim, kept
// while the bubble fades so it fades where it was and not at 0:00.
static float hoverFrac, hoverAnim;

static float barFracAt(float x) {
  return barW > 0.0f ? anim_clamp((x - barX) / barW, 0.0f, 1.0f) : 0.0f;
}

static void pointButton(int act, int unused) {
  (void)unused;
  if (!is_open || exiting || !visible) return;
  barFocus = 0; cardFocus = 0; button = act;
  wake();
}
static void pointBar(int a, int b) {
  (void)a; (void)b;
  if (!is_open || exiting || !visible) return;
  cardFocus = 0; barFocus = 1;
  wake();
}
static void clickBar(int a, int b) {
  (void)a; (void)b;
  if (!is_open || exiting || durationSeg <= 0.0f || !playbackReady()) return;
  cardFocus = 0; barFocus = 1;
  seekBegin();
  seekPreview = barFracAt(pointer_x()) * durationSeg;
  seekAt = SDL_GetTicks();
  barDrag = 1;
  wake();
}
// The prompt's pills: "Play now" / "Not now", or the one "Skip intro". With the
// controls up they take the card rung, as UP from the bar would; down, the card
// is already what OK acts on.
static void pointCard(int pill, int unused) {
  (void)unused;
  if (!is_open || exiting) return;
  if (nextCardUp() && pill >= 0 && pill < NEXT_NPILLS) nextFocus = pill;
  if (visible) { barFocus = 0; cardFocus = 1; wake(); }
}
static void clickPicture(int a, int b) {
  (void)a; (void)b;
  if (!is_open || exiting || player_loading()) return;
  togglePlaying();
  wake();
}

void player_event(const SDL_Event *e) {
  if (!is_open || exiting || e->type != SDL_KEYDOWN) return;
  SDL_Keycode k = e->key.keysym.sym;

  // THE PAUSE OVERLAY TAKES THE FIRST KEY. OK resumes straight from it — the one
  // thing a paused viewer coming back most likely wants — and anything else,
  // Back included, only puts it away and brings the controls back, so the key
  // that dismisses it does not also seek, leave or change the aspect.
  if (pauseOn) {
    pauseOn = 0;
    if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) {
      if (!playing) togglePlaying();
    }
    wake();
    return;
  }

  if (k == SDLK_ESCAPE || k == SDLK_AC_BACK || k == SDLK_BACKSPACE ||
      k == SDLK_DELETE) {
    exiting = 1; requestedExit = 1;
    return;
  }

  // CONTROLS HIDDEN: UP/DOWN wake the interface, LEFT/RIGHT skip (see THE QUICK
  // SEEK). OK straight away
  // pauses/resumes without navigating anything — it is the device's gesture: one
  // press in the centre and the video obeys, with no steps in between.
  // THE ASPECT KEY works always, with the controls up or hidden. In the web app the
  // mode is only changed through a button inside "More Actions" — two steps with a
  // cursor that does not exist here. On a TV the gesture has to be one press, and the
  // notice that rises on the change already says which mode you have entered, so the
  // key does not even need the interface open. 0 is the free key on the LG remote.
  if (k == SDLK_0 || k == SDLK_KP_0) { player_aspect_cycle(); return; }

  if (!visible) {
    // THE CARD'S TWO PILLS ARE REAL TARGETS while the transport is down, so
    // left/right belong to them rather than to waking the bar. This is the only
    // state in which the card is what OK acts on, which is why the interception
    // lives here and not above the `visible` test.
    if (nextCardUp() && (k == SDLK_LEFT || k == SDLK_RIGHT)) {
      nextStep(k == SDLK_RIGHT ? 1 : -1);
      return;
    }
    if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) {
      if (activateCard()) return;
      // OK mid quick-seek is "go there now", as it is on the bar — pausing
      // would throw the aim away.
      if (seekActive) { commitSeek(); return; }
      togglePlaying(); wake(); return;
    }
    // LEFT/RIGHT SKIP ten seconds a press without raising the controls. Only once
    // there is a picture: while the source opens there is nothing to skip through,
    // and the arrows wake the controls as they used to.
    if ((k == SDLK_LEFT || k == SDLK_RIGHT) && playbackReady() && !player_loading()) {
      quickSeek(k == SDLK_RIGHT ? 1 : -1);
      return;
    }
    if (k == SDLK_UP || k == SDLK_DOWN || k == SDLK_LEFT || k == SDLK_RIGHT)
      wake();
    return;
  }

  // CONTROLS UP: the focus moves along the buttons and OK presses the focused one.
  if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) {
    // On the bar, OK pauses/resumes: it is what is left that is useful, since the bar
    // has no action of its own in the web app.
    // On the bar, OK ends a seek in flight instead of pausing: you have been
    // aiming with left/right and OK is "go there". Pausing at that moment would
    // throw the aim away. With nothing in flight it pauses, as before.
    if (cardFocus) {
      // Whatever the prompt did, it is done with: taking the offer plays or skips,
      // refusing it removes the card. Either way the rung is gone, so the focus
      // steps back down to the bar rather than being left on nothing.
      activateCard();
      cardFocus = 0; barFocus = 1;
      wake();
      return;
    }
    if (barFocus) { if (seekActive) commitSeek(); else togglePlaying(); wake(); return; }
    switch (button) {
      case PLR_PLAY:    togglePlaying(); break;
      // The same request the card's Play pill makes; app.c picks it up.
      case PLR_NEXT: { const CatEp *p = player_next_episode();
                       if (p) { reqNextT = p->season; reqNextE = p->episode; } }
                     break;
      case PLR_ASPECT: player_aspect_cycle(); break;
      // CC and AUDIO open the SAME sheet, but on different columns: pressing
      // "subtitles" and landing on audio made the two buttons look like one.
      case PLR_CC:      reqTracks = 2;     break;   // 2 = the subtitle column
      case PLR_SOURCES:  reqSources = 1; break;
      case PLR_EPISODES: if (epT > 0) episodes_open(idx, epT, epE); break;
      case PLR_STATS:    statsOpen = !statsOpen; break;
      // The same exit as Back — the fade, the progress saved on the way out — and
      // then the title page, which app.c opens unless it is already underneath.
      case PLR_DETAILS:  exiting = 1; requestedExit = 1; reqDetails = 1; return;
      default:          reqTracks = 1;     break;   // PLR_AUDIO, 1 = the audio column
    }
    wake();
    return;
  }
  // UP goes up to the BAR, which in the web app is a real focus target
  // (`.player-progress-shell.focused` thickens the track from 6 to 10px). Without
  // that there was no way to skip ahead in the film with the bar — only the buttons'
  // 10s jumps, which is the defect the owner reported.
  //
  // The tracks sheet is NOT lost: it is still on UP, one level higher. From the
  // button row the first UP takes the bar and the second opens the sheet. Swapping
  // the gesture for another (one more button, a menu) would be worse: on a device
  // "up reveals subtitles and audio" is what the hand already knows.
  if (k == SDLK_UP) {
    // UP WALKS THE LADDER AND THEN PUTS IT AWAY: buttons -> bar -> the card above
    // it, if one is up -> controls hidden.
    //
    // It used to open the tracks sheet on audio at the top, on the grounds that
    // "up reveals subtitles and audio is what the hand already knows". That was
    // written when the sheet had no button of its own; it has TWO now, PLR_CC and
    // PLR_AUDIO, one row down. So the gesture was a second invisible door into a
    // panel already reachable, and it fired exactly when you reached for the
    // scrubber and pressed up once more out of habit.
    //
    // Hiding at the top does NOT call wake() — the same reasoning as DOWN from the
    // button row: waking in the very event that hid the bar makes the command look
    // broken.
    if (!barFocus && !cardFocus) { barFocus = 1; wake(); return; }
    if (barFocus && cardPresent()) { barFocus = 0; cardFocus = 1; wake(); return; }
    barFocus = 0; cardFocus = 0; visible = 0;
    lastInput = SDL_GetTicks();
    return;
  }
  // --- on the card, one rung above the bar ---------------------------------
  if (cardFocus) {
    if (k == SDLK_DOWN) { cardFocus = 0; barFocus = 1; wake(); return; }
    if (nextCardUp() && (k == SDLK_LEFT || k == SDLK_RIGHT)) {
      nextStep(k == SDLK_RIGHT ? 1 : -1);
      wake();
      return;
    }
    wake();
    return;
  }
  if (barFocus) {
    // On the bar, LEFT and RIGHT seek through the film instead of changing button.
    if (k == SDLK_LEFT)       jump(-1);
    else if (k == SDLK_RIGHT) jump(1);
    else if (k == SDLK_DOWN) { commitSeek(); barFocus = 0; }
    wake();
    return;
  }
  if (k == SDLK_DOWN) {
    // DOWN from the row means "get the controls out of the way".
    // It does not call wake(): that would put the bar back in the same event and make
    // the command look broken. The next directional press reveals it again.
    barFocus = 0;
    cardFocus = 0;
    visible = 0;
    lastInput = SDL_GetTicks();
    return;
  }
  // No wrap-around at the ends: the row is short and fits in a single glance; going
  // round at the end reads as an error, not as a shortcut.
  //
  // It steps through the VISIBLE row, not through the enum: with episodes no longer
  // last, walking the enum would stop on a button that is not drawn for a film.
  { int row[PLR_NBTNS], n = rowButtons(row), slot = rowSlot(row, n, button);
    if (slot < 0) slot = 0;
    if (k == SDLK_LEFT  && slot > 0)     button = row[slot - 1];
    else if (k == SDLK_RIGHT && slot < n - 1) button = row[slot + 1]; }
  wake();
}

// --- REPORTING WHILE IT PLAYS ------------------------------------------------
//
// Trakt hears "watching now" when playback starts or resumes and "paused here"
// when it pauses — not only the closing call in player_shutdown, which left the
// owner's other devices blind for the whole film. A state has to hold for
// PLR_REPORT_HOLD_MS before it is reported: a seek or a rebuffer drops `playing`
// for a moment, and each blip would otherwise be a pause and a start on the wire.
//
// AND A CHECKPOINT of the local progress every PLR_CHECKPOINT_MS of playing, and
// on every pause. The position used to be written only by player_shutdown, so a
// TV switched off at the wall, or an app killed mid-film, came back at wherever
// the title had last been closed properly. Local only, with the account marked
// dirty: pushing mid-film would start a full sync cycle, the burst of HTTP the
// app keeps away from an open player (app.c), and the next cycle pushes it.
#define PLR_REPORT_HOLD_MS  2000u
#define PLR_CHECKPOINT_MS  60000u
static int    reported = -1;     // what Trakt was last told: 1 playing, 0 paused
static int    heldState = -1;
static Uint32 heldSince, checkpointAt;

static void reportReset(void) { reported = heldState = -1; heldSince = checkpointAt = 0; }

static int traktId(char *id, size_t size) {
  const CatItem *ci = cat_item(idx);
  if (!ci || !ci->imdb[0]) return 0;
  if (epT > 0 && epE > 0)
    snprintf(id, size, "%.*s:%d:%d", (int)strcspn(ci->imdb, ":"), ci->imdb, epT, epE);
  else snprintf(id, size, "%s", ci->imdb);
  return 1;
}

static void checkpoint(Uint32 now, const char *why) {
  checkpointAt = now;
  if (posSeg < 5.0f) return;   // nothing worth resuming yet
  cat_save_progress_ep(idx, posSeg, durationSeg, epT, epE);
  sync_dirty_progress();
  printf("[player] checkpoint (%s) at %.0f/%.0f s\n", why, posSeg, durationSeg);
  fflush(stdout);
}

static void reportPlayback(Uint32 now) {
  char id[64];
  int state = playing ? 1 : 0;
  if (!hasVideo || !video_ready() || !startImage || durationSeg <= 1.0f ||
      waitingSource || errorSource || trailerMode) return;
  if (state != heldState) { heldState = state; heldSince = now; }
  if (state != reported && now - heldSince >= PLR_REPORT_HOLD_MS) {
    if (traktId(id, sizeof id)) {
      if (state) trakt_scrobble_start(id, posSeg, durationSeg);
      else if (reported == 1) trakt_scrobble_pause(id, posSeg, durationSeg);
    }
    if (!state && reported == 1) checkpoint(now, "paused");
    reported = state;
  }
  if (!checkpointAt) checkpointAt = now;
  if (state && now - checkpointAt >= PLR_CHECKPOINT_MS) checkpoint(now, "periodic");
}

void player_update(float dt, Uint32 now) {
  skipTick(dt);
  nextTick(dt);
  // THE RUNG CAN DISAPPEAR UNDER THE FOCUS: the skip offer times out on its own,
  // and the next-episode card goes when the episode does. Leaving cardFocus set
  // would strand the cursor on nothing — every key would land on a card that is no
  // longer drawn — so it steps back down to the bar the frame the prompt leaves.
  if (cardFocus && !cardPresent()) { cardFocus = 0; barFocus = 1; }
  if (!is_open) return;
  autosync_pump();

  // THE OPENING RUNS ON THE DETAIL'S OWN CURVE. The first-order spring left at full
  // speed on the first frame — a third of the fade inside one frame at 60Hz, which
  // read as a cut, and with the title screen still underneath (the handoff) it read
  // as the page blinking out. anim_spring2 starts at rest and lands with no overshoot,
  // the same movement as home -> detail, so the two steps into a title feel like one
  // gesture. The exit keeps its brisk first-order curve: that one is an instruction
  // already given.
  // Back to the title screen it runs on the same curve backwards — the logo flying
  // home has to start at rest too, or it leaves with a jerk.
  if (exiting && fromDetail && !player_has_video())
    entry = anim_spring2_reduced(&entryV, entry, 0.0f, dt, NV_SPRING2_SCREEN,
                                 settings_animations_reduced());
  else if (exiting) entry = anim_spring(entry, 0.0f, dt, NV_SPRING_SCREEN);
  else entry = anim_spring2_reduced(&entryV, entry, 1.0f, dt, NV_SPRING2_SCREEN,
                                    settings_animations_reduced());
  // Marks the first frame WITH A PICTURE. It is from here that the parental guide
  // counts its time — counting from the screen's opening would make the guide spend
  // its allowance while the app was still looking for a source, and it would
  // disappear before the film appeared.
  if (!startImage && hasVideo && video_ready()) { startImage = now; wake(); }
  // The loading fill's stage ceiling, and the ease toward it: slow inside a stage
  // (tau 1.6s), brisk at the finish (tau 0.12s).
  { int loading = player_loading();
    float cap = waitingSource ? 0.40f
              : (hasVideo && !video_active()) ? 0.60f
              : loading ? 0.92f : 1.0f;
    float tau = loading ? 1.6f : 0.12f;
    loadFill += (cap - loadFill) * (1.0f - expf(-dt / tau));
    if (wasLoading && !loading && player_has_video()) loadEndAt = now;
    wasLoading = loading; }

  // THE FAILURE WATCHES — see "THE FAILURE LOG" at the top of the file.
  if (!exiting) {
    char r[200], t1[24], t2[24];
    int ec = video_error_count(), eo = video_eos_count();
    if (ec != failErrSeen) {
      char e[128];
      failErrSeen = ec;
      video_last_error(e, sizeof e);
      snprintf(r, sizeof r, "pipeline error: %s", e);
      player_report_failure(startImage ? "playback" : "load", r);
    }
    if (hasVideo && !video_ready() && !failLoadLogged && msSince(now, loadStartAt) > PLR_FAIL_LOAD_MS) {
      failLoadLogged = 1;
      snprintf(r, sizeof r, "no picture after %us (%s)", PLR_FAIL_LOAD_MS / 1000u,
               video_active() ? "pipeline loaded, loadCompleted never came"
                              : "pipeline never took the media");
      player_report_failure("load", r);
    }
    if (waitingSource && !failSearchLogged && msSince(now, loadStartAt) > PLR_FAIL_SEARCH_MS) {
      failSearchLogged = 1;
      snprintf(r, sizeof r, "still looking for a source after %us",
               PLR_FAIL_SEARCH_MS / 1000u);
      reportFailure("source", r, 0);
    }
    // A STALL: playing, not aiming a seek, and the position has not moved. Paused
    // or seeking resets it — neither is the file's fault.
    if (startImage && hasVideo && playing && !seekActive && !settleAt) {
      if (stallPos < 0.0f || fabsf(posSeg - stallPos) > 0.25f) { stallPos = posSeg; stallAt = now; }
      else if (!failStallLogged && now - stallAt > PLR_FAIL_STALL_MS) {
        failStallLogged = 1;
        fmtTime(t1, sizeof t1, posSeg, 0);
        snprintf(r, sizeof r, "stuck at %s for %us while playing", t1,
                 PLR_FAIL_STALL_MS / 1000u);
        player_report_failure("playback", r);
      }
    } else stallPos = -1.0f;
    // The END, three minutes or more before the duration says it should come: a
    // truncated file, or a source that cut the connection and called it done.
    if (eo != failEosSeen) {
      failEosSeen = eo;
      if (durationSeg > 300.0f && posSeg < durationSeg - 180.0f) {
        fmtTime(t1, sizeof t1, posSeg, 0);
        fmtTime(t2, sizeof t2, durationSeg, 0);
        snprintf(r, sizeof r, "ended early at %s of %s", t1, t2);
        player_report_failure("playback", r);
      }
    }
  }
  // A HANDOFF CLOSES LATER. At 0.02 the logo flying home is still 2% of the way from
  // the centre — ~10px on a wide wordmark — and the page's own copy then takes over in
  // place: a visible jump on the last frame. At 0.003 the gap is under a pixel.
  if (exiting && entry < (fromDetail ? 0.003f : 0.02f)) {
    is_open = 0; exiting = 0; entry = 0.0f; flying = 0; return; }

  // With a pipeline, the position and duration come FROM IT; dt only serves the
  // animations. The added-up clock still exists for when there is no video (on the
  // Mac, or if the source fails): without it the bar would sit at zero and the screen
  // would lie by saying nothing is happening.
  // The rectangle depends on the FRAME's aspect ratio, and that only exists once the
  // videoInfo arrives — seconds after the opening. Without this reread the mode would
  // stay computed from the 16:9 guess forever, and on a 3840x1606 file (which is the
  // real case measured on this TV) "Original" would crop the image.
  if (hasVideo) {
    static int lastWidth, lastHeight;
    int lw = video_width(), lh = video_height();
    if (lw != lastWidth || lh != lastHeight) { lastWidth = lw; lastHeight = lh; applyAspect(); }
  }

  // The burst is over once the presses stop. ONE seek goes out, here.
  if (seekActive && now - seekAt > PLR_SEEK_COMMIT_MS) commitSeek();

  float posBefore = posSeg;
  if (hasVideo && video_active()) {
    double d = video_duration();
    double vp = video_pos();
    // WHILE SETTLING, video_pos() is still reporting where the decoder WAS. Taking
    // it would snap the bar back to the pre-seek position for as long as the
    // pipeline takes to land — which is the whole defect this preview replaced.
    // Hold the target until it arrives, or until the settle window expires.
    if (settleAt) {
      if (fabs(vp - (double)settleTarget) <= (double)PLR_SEEK_LAND_S ||
          now - settleAt > PLR_SEEK_SETTLE_MS) settleAt = 0;
    }
    posSeg = settleAt ? settleTarget : (float)vp;
    if (d > 1.0) durationSeg = (float)d;
    if (!resumeApplied && video_ready() && d>1.0) {
      resumeApplied=1;
      if(resumePct>0) video_fetch(d*resumePct/100.0);
    }
    playing = video_playing();
  } else if (playing && !waitingSource && !errorSource) {
    posSeg += dt;
    if (posSeg >= durationSeg) { posSeg = durationSeg; playing = 0; }
  }
  { float step = posSeg - posBefore;
    if (step < 0.0f || step > 1.0f || settleAt) rolledMs = 0;
    else if (playing && playbackReady() && !player_loading()) rolledMs += dt * 1000.0f;
    if (rolledMs >= PLR_SKIP_LEAD_MS) rolledOnce = 1; }

  // THE SUBTITLE CHOOSES ITSELF, once per playback, from the Settings row. Held
  // back until the first frame with a picture: before that the pipeline has no
  // mediaId, and selecting one of the FILE's tracks would be dropped without a
  // word. With no pipeline at all (the Mac) there is no frame to wait for, and
  // the addon's subtitle is drawn by our own overlay anyway.
  if (!waitingSource && !errorSource && (startImage || !hasVideo) && !trailerMode)
    tracks_auto(now);
  reportPlayback(now);

  // THE FILE'S OWN SUBTITLE IS LIFTED CLEAR of whatever takes the bottom of the
  // screen: the transport while the controls are up, the subtitle Style bar while
  // that is. The pipeline draws those cues, out of the overlay's reach, so the
  // raise goes to it — the viewer's Height is left alone, and it comes off the
  // moment the controls or the bar go. `chrome` is last frame's: under a sheet
  // the controls are faded out even while `visible` holds.
  { int up = tracks_style_shown() > 0.5f || (visible && chrome > 0.5f);
    video_subtitle_lift(up ? NV_TRK_EMBED_LIFT : 0); }

  // THE POINTER WAKES THE CONTROLS as a key would, and the drag along the bar
  // follows it until the button comes up.
  { static Uint32 seenMove;
    Uint32 moved = pointer_moved_at();
    if (moved != seenMove) {
      seenMove = moved;
      if (pointer_active() && !exiting && !episodes_is_open() &&
          !stream_sheet_is_open() && !tracks_is_open()) wake();
    }
    if (barDrag) {
      if (pointer_held() && seekActive) {
        seekPreview = barFracAt(pointer_x()) * durationSeg;
        seekAt = now;
        lastInput = now;
      } else {
        barDrag = 0;
        commitSeek();
      }
    }
    // Over the bar, or dragging along it: the readout follows, and the controls
    // stay up — resting the pointer on the bar is aiming, and the transport must
    // not time out from under it.
    { int on = durationSeg > 0.0f &&
               (barDrag || (pointer_active() && visible && pointer_over(pointBar, 0, 0)));
      if (on) {
        hoverFrac = barDrag ? anim_clamp(seekPreview / durationSeg, 0.0f, 1.0f)
                            : barFracAt(pointer_x());
        lastInput = now;
      }
      hoverAnim = anim_spring(hoverAnim, on ? 1.0f : 0.0f, dt,
                              on ? NV_SPRING_FOCUS : NV_SPRING_BLUR); } }

  // Paused, the controls stay. Making them disappear would leave the user in front of
  // a still frame with no clue that it was they who paused.
  if (visible && playing && !player_loading() && !episodes_is_open() &&
      !stream_sheet_is_open() && !tracks_is_open() && msSince(now, lastInput) > PLR_HIDES_MS) visible = 0;
  if (epT > 0 && !strstr(lineEp, " · ")) player_set_episode(epT, epE);

  anim = anim_spring(anim, visible ? 1.0f : 0.0f, dt,
                   visible ? NV_SPRING_FOCUS : NV_SPRING_BLUR);
  // `barFocus` counts here now. The resting button has NO circle any more (the web
  // app's is `background: transparent` until focus), so this spring is the only
  // thing drawing it — and without the guard, moving UP to the bar left the button's
  // white puck lit behind a bar that also claimed to have focus.
  for (int i = 0; i < PLR_NBTNS; i++) {
    float target = (visible && !barFocus && button == i) ? 1.0f : 0.0f;
    focusB[i] = anim_spring(focusB[i], target, dt,
                         target > focusB[i] ? NV_SPRING_FOCUS : NV_SPRING_BLUR);
  }
  { float target = (visible && barFocus) ? 1.0f : 0.0f;
    focusBarAnim = anim_spring(focusBarAnim, target, dt,
                         target > focusBarAnim ? NV_SPRING_FOCUS : NV_SPRING_BLUR); }
  // The quick overlay hands over to the full transport the moment the controls
  // come up: the bar is shared, so it does not blink on the swap.
  { int q = !visible && quickAt && now - quickAt < PLR_QUICK_HOLD_MS;
    quickAnim = anim_spring(quickAnim, q ? 1.0f : 0.0f, dt,
                            q ? NV_SPRING_FOCUS : NV_SPRING_BLUR);
    quickPulse *= expf(-dt / 0.12f); }

  // THE PAUSE OVERLAY. Rising, it takes the transport down with it: the controls
  // are what "paused" looked like until now, and the two stacked read as clutter.
  // The first key brings them back (player_event), so the paused viewer is never
  // left without them for longer than the overlay is up.
  pauseOn = pauseWanted(now);
  if (pauseOn) visible = 0;
  pauseAnim = pauseOn ? fminf(1.0f, pauseAnim + dt / NV_PAUSE_IN_S)
                      : fmaxf(0.0f, pauseAnim - dt / NV_PAUSE_OUT_S);
}

// hh:mm:ss only when it passes an hour — "0:03:12" on a short episode reads as a
// formatting error, not as a time.
static void fmtTime(char *b, size_t n, float seg, int negative) {
  if (seg < 0.0f) seg = 0.0f;
  int t = (int)(seg + 0.5f);
  int h = t / 3600, m = (t / 60) % 60, s = t % 60;
  const char *sinal = negative ? "-" : "";
  if (h > 0) snprintf(b, n, "%s%d:%02d:%02d", sinal, h, m, s);
  else       snprintf(b, n, "%s%d:%02d", sinal, m, s);
}

// --- icons -------------------------------------------------------------------
// REAL FILES, from art/icons (the web app's .svg rasterised at 128px).
// Each glyph used to be assembled from the primitives — play from a triangle, pause
// from two rectangles, subtitles from bars, aspect from four 3px lines — and each
// was an approximation of the original.
//
// The colour still comes from here: gfx_icon draws with GFX_BRAND, which takes the
// shape from the file's ALPHA, so the same PNG serves dark over the focus's white
// circle and light over the translucent one.
//
static void iconFile(float cx, float cy, float a, float luma,
                         const char *name, float size) {
  GfxRect r = { cx - size * 0.5f, cy - size * 0.5f, size, size };
  gfx_icon(r, name, luma, luma, luma, a * 0.94f);
}

// The glyphs are drawn at --player-control-icon (48) flat. They were at
// PLR_ICON_H * 1.15 = 55, which is the size the ATV port gives the PRIMARY button
// alone; every other button in the web app is 48, and at 55 the row's five
// secondaries were each a size larger than the sheet says.
static void iconPlayPause(float cx, float cy, float a, int pause, float luma) {
  iconFile(cx, cy, a, luma, pause ? "pause" : "play", PLR_ICON_H);
}

static void iconSubtitles(float cx, float cy, float a, float luma) {
  iconFile(cx, cy, a, luma, "subtitles", PLR_ICON_H);
}

static void iconAudio(float cx, float cy, float a, float luma) {
  iconFile(cx, cy, a, luma, "audio", PLR_ICON_H);
}

static void iconAspect(float cx, float cy, float a, float luma) {
  iconFile(cx, cy, a, luma, "aspect", PLR_ICON_H);
}

// THE FOCUS PUCK, and nothing else.
//
// The resting button used to carry a dark translucent circle of its own, so the row
// read as six grey coins laid on the picture whether or not anything was focused.
// The web app's is `background: transparent` at rest and `background: #ffffff` on
// focus, with a 180ms colour transition and NO change of size — the circle is not a
// button's body, it is the focus itself arriving. `f` is that transition.
//
// Dropping the 1.09 scale with it is deliberate: the web app has none, and a puck
// that both appears and grows reads as two cues for one event.
static void buttonCircle(float cx, float cy, float f, float a) {
  GfxRect r = { cx - PLR_BTN_D * 0.5f, cy - PLR_BTN_D * 0.5f, PLR_BTN_D, PLR_BTN_D };
  if (f > 0.004f) gfx_color(r, 0.5f, 1.0f, 1.0f, 1.0f, f * a);
}

// A PILL, spelled correctly for this API. `radius` is a fraction measured in the
// rect's HEIGHT (gfx.c's sdf: `b = vec2(0.5*asp, 0.5) - r`), so 0.5 is a full pill
// only while the rect is at least as wide as it is tall. The fill starts a frame
// after the film does, when it is two pixels wide and twelve tall, and there 0.5
// drives the SDF's b.x negative and the shape collapses — which is the bubble the
// owner saw at the start of every film, misdiagnosed once as "the radius is too
// big" and answered by squaring the corners off entirely.
//
// Scaling by w/h on that side is the same correction the parental guide already
// applies to its vertical bar, and it keeps the ends round all the way down to the
// first pixel of progress.
static float railRadius(float w, float h) {
  if (h <= 0.0f) return 0.0f;
  return 0.5f * (w < h ? w / h : 1.0f);
}

// THE INTRO'S EDGES as cuts in the bar, the way YouTube marks chapters: a gap
// where the opening starts and one where it ends, and nothing drawn on top.
// Each layer (track, buffer, fill) is still ONE rounded bar, drawn once per
// piece under a crop — so the bar's outer ends keep their rounding and the cuts
// come out square, and a translucent layer never overlaps itself.
#define PLR_RAIL_GAP 4.0f
static float railCut[2];
static int nRailCut;

static void railLayer(GfxRect r, float cr, float cg, float cb, float ca) {
  float rad = railRadius(r.w, r.h);
  if (!nRailCut) { gfx_color(r, rad, cr, cg, cb, ca); return; }
  float from = barX;
  for (int i = 0; i <= nRailCut; i++) {
    float to = i < nRailCut ? railCut[i] - PLR_RAIL_GAP * 0.5f : barX + barW;
    if (to > from) {
      gfx_crop(from, r.y - r.h, to - from, r.h * 3.0f);
      gfx_color(r, rad, cr, cg, cb, ca);
    }
    if (i < nRailCut) from = railCut[i] + PLR_RAIL_GAP * 0.5f;
  }
  gfx_no_crop();
}

static void colorSubtitle(int i,int *r,int *g,int *b){
  static const unsigned char c[VIDEO_SUB_NCOLORS][3]={
    {255,255,255},{255,222,48},{64,224,112},{78,156,255},{255,80,80},{18,18,18}};
  if(i<0||i>=VIDEO_SUB_NCOLORS)i=0;*r=c[i][0];*g=c[i][1];*b=c[i][2];
}

/* The C9's uMS limits the font and the scale. OpenSubtitles goes through this
 * SDL/GLES overlay, exactly like the web app's HTML overlay. */

// Up to four lines of cue text, measured, in the viewer's subtitle style.
typedef struct { TxtLine color[4],border[4]; int n; float w,h; } SubBlock;

static int subBlockBuild(SubBlock *b,char *text,TxtStyle st,int r,int g,int bl){
  char *line,*salva;TxtFamily fam=(TxtFamily)subStyle.family;
  b->n=0;b->w=b->h=0;
  line=strtok_r(text,"\n",&salva);
  while(line&&b->n<4){
    int i=b->n;
    b->color[i]=txt_line_trim_family(st,line,r,g,bl,255,1660,fam);
    b->border[i]=subStyle.border?txt_line_trim_family(st,line,0,0,0,255,1660,fam):(TxtLine){0};
    if(b->color[i].w>b->w)b->w=(float)b->color[i].w;
    b->h+=b->color[i].h+(i?5:0);
    b->n++;line=strtok_r(NULL,"\n",&salva);
  }
  return b->n;
}

// Draws a block with its top-left at (x, y); `column` lines each line up inside
// it: 0 left, 1 centre, 2 right.
static void subBlockDraw(const SubBlock *b,float x,float y,int column,float alpha){
  if(alpha<=0.f)return;  // the layout pass: the lines are asked for, nothing drawn
  for(int i=0;i<b->n;i++){
    TxtLine l=b->color[i];
    float lx=column==0?x:column==2?x+b->w-l.w:x+(b->w-l.w)*.5f;
    if(subStyle.background){float fa=subStyle.background*.16f*alpha;gfx_color((GfxRect){lx-18,y-6,l.w+36,l.h+12},.16f,0,0,0,fa);}
    if(b->border[i].tex){float d=subStyle.border==2?4.f:2.f;
      txt_draw_alpha(b->border[i],lx+d,y+d,.82f*alpha);
      if(subStyle.border==1){txt_draw_alpha(b->border[i],lx-d,y,.82f*alpha);txt_draw_alpha(b->border[i],lx,y-d,.82f*alpha);}
    }
    txt_draw_alpha(l,lx,y,alpha);y+=l.h+5;
  }
}

// An ASS line placed by the file: at its \pos point, or against the edge its
// alignment names. The alignment also says which part of the text sits on the
// point — 7 is its top-left corner, 2 the middle of its bottom edge — as in
// libass. Always kept whole on the picture.
//
// Both are measured on the PICTURE, not the screen: a point is a fraction of the
// video frame, and libass puts it there. Mapped onto the whole screen, a 4:3
// show (Trigun) had its signs pushed out into the pillarbox bars, and a
// left-aligned line sat 80px from the panel's edge — in the black, well clear of
// the picture it belongs to. The frame is the aspect mode's rectangle; the edges
// are its visible part, so a zoomed mode keeps the text on screen.
#define SUB_EDGE_X .042f
#define SUB_EDGE_Y .056f
static void drawSubtitlePlaced(const SubtitleCue *c,TxtStyle st,int r,int g,int bl,float alpha){
  char text[768];SubBlock b;
  int align=c->align?c->align:2,column=(align-1)%3,row=(align-1)/3;
  PlrRect f=aspectRect(aspect),v=aspectVisible(aspect);
  float ax,ay,x,y;
  snprintf(text,sizeof text,"%s",c->text);
  if(!subBlockBuild(&b,text,st,r,g,bl))return;
  if(v.w<1.f||v.h<1.f){f=v=(PlrRect){0,0,NV_SCREEN_W,NV_SCREEN_H};}
  if(c->positioned){ax=f.x+c->x*f.w;ay=f.y+c->y*f.h;}
  else{
    float ex=v.w*SUB_EDGE_X,ey=v.h*SUB_EDGE_Y;
    ax=column==0?v.x+ex:column==2?v.x+v.w-ex:v.x+v.w*.5f;
    ay=row==2?v.y+ey:row==1?v.y+v.h*.5f:v.y+v.h-ey;
  }
  x=column==0?ax:column==2?ax-b.w:ax-b.w*.5f;
  y=row==2?ay:row==1?ay-b.h*.5f:ay-b.h;
  if(x>v.x+v.w-b.w)x=v.x+v.w-b.w;if(x<v.x)x=v.x;
  if(y>v.y+v.h-b.h)y=v.y+v.h-b.h;if(y<v.y)y=v.y;
  if(x<0)x=0;if(y<0)y=0;
  subBlockDraw(&b,x,y,column,alpha);
}

// `alpha` 0 lays the cues out without drawing them — see drawSubtitleExternal.
static void drawCues(const SubtitleCue *shown,int n,float alpha){
  char text[768];size_t used=0;SubBlock band;int r,g,b;
  if(!n)return;
  int pct=subStyle.size;if(pct<50)pct=50;if(pct>200)pct=200;pct=(pct/10)*10;
  TxtStyle st=(TxtStyle)(TXT_SUB_50+(pct-50)/10);colorSubtitle(subStyle.color,&r,&g,&b);
  // Everything the file does not place goes to the usual band at the bottom:
  // SRT and VTT, and ASS lines that are bottom-aligned with no point of their own.
  text[0]=0;
  for(int i=0;i<n;i++){
    const SubtitleCue *c=&shown[i];
    if(c->positioned||c->align>3){drawSubtitlePlaced(c,st,r,g,b,alpha);continue;}
    int w=snprintf(text+used,sizeof text-used,"%s%s",used?"\n":"",c->text);
    if(w>0&&(size_t)w<sizeof text-used)used+=(size_t)w;
  }
  if(!used||!subBlockBuild(&band,text,st,r,g,b))return;
  // The lift the subtitle takes when the controls come up. 760 was the clearance
  // over the OLD transport; the ported one is ~66px taller (a 64 bottom inset
  // instead of 48, and the web app's 40/36 gaps instead of 12/32), which left the
  // cue a dozen pixels under the title. 700 restores roughly the clearance that
  // value was chosen for.
  // With the chrome faded out under a sheet there is no transport to clear, so
  // the cue settles back to where it sits with the controls hidden.
  float gone=stream_sheet_shown();if(tracks_shown()>gone)gone=tracks_shown();
  if(episodes_shown()>gone)gone=episodes_shown();
  float base=visible?700.f+300.f*gone:1000.f;
  base-=(subStyle.position-3)*48.f;
  // Kept clear of the Style bar's tiles while it is up, on the bar's own curve.
  { float bar=tracks_style_shown();
    if(bar>0.f&&base>NV_TRK_PREVIEW_FLOOR)base+=(NV_TRK_PREVIEW_FLOOR-base)*bar; }
  subBlockDraw(&band,(NV_SCREEN_W-band.w)*.5f,base-band.h,1,alpha);
}

// A NEW CUE COMES IN WHOLE. text.c rasterises two lines a frame (TXT_PER_FRAME)
// and hands back an empty one past that; a two-line cue with an outline is four.
// Drawn as it came, its first frame had the top line alone, a line lower, and the
// next frame jumped it up — one frame, seen as a flicker nobody could describe.
// So the cues are laid out first, undrawn; if any line was still missing, the
// previous frame's cues are drawn instead (their lines are cached) and the new
// ones take over once every line is in. The hold gives up after a few frames, so
// a line that never rasterises cannot freeze the old cue on screen.
#define SUB_HOLD_FRAMES 6
static void drawSubtitleExternal(void){
  static SubtitleCue last[8];static int nLast,held;
  SubtitleCue shown[8];int n,missBefore=txt_misses;
  float alpha=(subStyle.opacity==3?.25f:subStyle.opacity==2?.5f:subStyle.opacity==1?.75f:1.f)*entry;
  n=subtitle_shown(posSeg,subStyle.delayMs,shown,8);
  drawCues(shown,n,0.f);
  if(txt_misses!=missBefore&&held<SUB_HOLD_FRAMES){held++;drawCues(last,nLast,alpha);return;}
  held=0;
  memcpy(last,shown,(size_t)n*sizeof *shown);nLast=n;
  drawCues(shown,n,alpha);
}

// --- THE STREAM STATS PANEL (#playerStatsOverlay) ----------------------------
// A label/value list on a dark plate, hung in the top-left CORNER.
//
// WHAT IT DOES NOT SHOW, and why. The web app's list is read off
// PlayerController.getPlaybackStats(), which includes a dropped-frames counter this
// pipeline has no equivalent of. There is no Dropped frames row rather than one
// reading "--" forever or, worse, filled from somewhere else — the app's own render
// telemetry (main.c's FPS/janks) is NOT the decoder's dropped frames, and putting it
// under that label would be the same lie the format badges used to tell.
//
// The bitrate IS derivable and honest: the chosen stream carries sizeMB, so
// size/duration is a genuine AVERAGE for the file. It is labelled "(avg)" for
// exactly the reason the web app labels its own estimate that way — it is not the
// instantaneous rate, and at a tense moment in a VBR encode the real figure is well
// above it. The quality badge rates it against the web's own thresholds, which
// scale with the picture's height: 15 Mbps is thin for 2160p and lavish for 480p.

// The CodecID, shortened for reading. `bitmap` comes back 1 for the formats that
// are PICTURES rather than text — which is not trivia: a PGS track cannot be
// restyled, repositioned or resized, so every subtitle setting in this app silently
// does nothing on one. Seeing "PGS" here is the answer to why.
static const char *subKind(const char *codec, int *bitmap) {
  if (bitmap) *bitmap = 0;
  if (!codec || !codec[0]) return NULL;
  if (strstr(codec, "PGS"))    { if (bitmap) *bitmap = 1; return "PGS"; }
  if (strstr(codec, "VOBSUB")) { if (bitmap) *bitmap = 1; return "VobSub"; }
  if (strstr(codec, "DVBSUB")) { if (bitmap) *bitmap = 1; return "DVB"; }
  if (strstr(codec, "WEBVTT")) return "WebVTT";
  if (strstr(codec, "ASS"))    return "ASS";
  if (strstr(codec, "SSA"))    return "SSA";
  if (strstr(codec, "UTF8"))   return "SRT";
  return codec;
}

// The web app's getBitrateQualityRating, thresholds and all. Returns NULL when
// there is nothing to rate — no bitrate, or no height to rate it against.
static const char *bitrateBadge(double mbps, int height, float *r, float *g, float *b) {
  double good;
  if (mbps <= 0.0 || height <= 0) return NULL;
  good = height <= 480 ? 1.5 : height <= 720 ? 3.0
       : height <= 1080 ? 5.0 : height <= 1440 ? 9.0 : 15.0;
  if (mbps >= good * 2.0) { *r = 125/255.0f; *g = 211/255.0f; *b = 252/255.0f; return "EXCELLENT"; }
  if (mbps >= good)       { *r = 134/255.0f; *g = 239/255.0f; *b = 172/255.0f; return "GOOD"; }
  *r = 252/255.0f; *g = 165/255.0f; *b = 165/255.0f; return "LOW";
}

static void drawStats(float alpha) {
  const char *labels[7];
  char values[7][72];
  const char *badge = NULL;
  float br = 0, bg = 0, bb = 0;
  int n = 0, i;
  float lw = 0.0f, vw = 0.0f, lineH = 0.0f, badgeW = 0.0f;
  TxtLine lb = (TxtLine){0};

  { const Stream *st = stream_n() > 0 ? stream_item(stream_current()) : NULL;
    double dur = durationSeg > 1.0f ? durationSeg : 0.0;
    double mbps = (st && st->sizeMB > 0 && dur > 0.0)
                ? (double)st->sizeMB * 8.0 / dur : 0.0;
    labels[n] = "Bitrate";
    if (mbps > 0.0) snprintf(values[n], sizeof values[n], "%.1f Mbps (avg)", mbps);
    else            snprintf(values[n], sizeof values[n], "--");
    badge = bitrateBadge(mbps, video_height(), &br, &bg, &bb);
    n++;
    labels[n] = "Engine";
    snprintf(values[n], sizeof values[n], "%s", video_engine());
    n++;
    labels[n] = "Source";
    snprintf(values[n], sizeof values[n], "%s%s",
             st && st->provider[0] ? st->provider : "--",
             st ? (st->mp4 ? " \xc2\xb7 MP4" : " \xc2\xb7 HLS") : "");
    n++; }

  labels[n] = "Resolution";
  if (video_width() > 0 && video_height() > 0)
    snprintf(values[n], sizeof values[n], "%dx%d (%s)", video_width(), video_height(),
             video_height() >= 2160 ? "4K" : video_height() >= 1080 ? "1080p"
             : video_height() >= 720 ? "720p" : "SD");
  else snprintf(values[n], sizeof values[n], "--");
  n++;

  labels[n] = "HDR";
  { const char *h = video_hdr();
    snprintf(values[n], sizeof values[n], "%s",
             h && h[0] && strcasecmp(h, "none") ? h : "SDR"); }
  n++;

  labels[n] = "Audio";
  // Atmos or SILENCE about the codec. video.h answers exactly one question about
  // this track — video_has_atmos() — and filling the gap with "PCM/other" asserts a
  // codec nothing here knows: the stream is as likely EAC3 or DTS. The track count
  // IS known, so that is what the row says.
  if (video_has_atmos())
    snprintf(values[n], sizeof values[n], "Dolby Atmos \xc2\xb7 %d track%s",
             video_n_audio(), video_n_audio() == 1 ? "" : "s");
  else
    snprintf(values[n], sizeof values[n], "%d track%s",
             video_n_audio(), video_n_audio() == 1 ? "" : "s");
  n++;

  // WHAT SUBTITLES THE FILE HOLDS, by kind. The kinds come from the MKV header read
  // (video.h's VideoTrack.codec) and not from the pipeline, which reports no codec
  // at all — so on anything that is not an MKV this degrades to the bare count,
  // which is still the truth.
  labels[n] = "Subtitles";
  { const char *kinds[6]; int counts[6], nk = 0, bmp[6], k, total = video_n_subtitle();
    for (i = 0; i < total; i++) {
      const VideoTrack *t = video_subtitle(i);
      int isBmp = 0;
      const char *kind = t ? subKind(t->codec, &isBmp) : NULL;
      if (!kind) continue;
      for (k = 0; k < nk; k++) if (!strcmp(kinds[k], kind)) { counts[k]++; break; }
      if (k == nk && nk < 6) { kinds[nk] = kind; counts[nk] = 1; bmp[nk] = isBmp; nk++; }
    }
    if (!total) snprintf(values[n], sizeof values[n], "none");
    else if (!nk) snprintf(values[n], sizeof values[n], "%d track%s",
                            total, total == 1 ? "" : "s");
    else {
      int off = snprintf(values[n], sizeof values[n], "%d \xc2\xb7 ", total);
      for (k = 0; k < nk && off < (int)sizeof values[n] - 1; k++)
        off += snprintf(values[n] + off, sizeof values[n] - off, "%s%s %d%s",
                        k ? ", " : "", kinds[k], counts[k], bmp[k] ? "*" : "");
    } }
  n++;

  // Measured first, drawn second: the value column is right-aligned against the
  // widest of the two, so the readings line up whatever the labels say.
  for (i = 0; i < n; i++) {
    TxtLine l = txt_line(TXT_PLR_STATL, labels[i], 255, 255, 255, 255);
    TxtLine v = txt_line(TXT_PLR_STAT,  values[i], 255, 255, 255, 255);
    if (l.w > lw) lw = (float)l.w;
    if (v.w > vw) vw = (float)v.w;
    if (v.h > lineH) lineH = (float)v.h;
  }
  if (badge) {
    lb = txt_line(TXT_PLR_BADGE, badge, 255, 255, 255, 255);
    badgeW = (float)lb.w + 20.0f + 10.0f;   // pill padding + the gap to the value
  }
  // THE NEXT-EPISODE CARD'S SURFACE, so the player's floating panels read as one
  // family: the same ink plate at the same radius, the same inset hairline (a
  // GFX_RING straddles the edge and the quad clips its outer half), and a tracked
  // grey kicker over the content the way "UP NEXT" heads the card. Labels are the
  // kicker's quiet grey rather than white at half alpha, and a hairline parts the
  // rows so the eye can run along one from label to reading.
  { float gap = 48.0f, rowGap = 14.0f, padX = NV_NEXT_PADX + 4.0f, padY = NV_NEXT_PAD;
    float kickH = (float)txt_line(TXT_NEXT_KICK, "STREAM STATS", 150, 152, 158, 255).h;
    float headGap = 18.0f;
    float w = padX * 2.0f + lw + gap + vw + badgeW;
    float h = padY * 2.0f + kickH + headGap + n * lineH + (n - 1) * rowGap;
    float x = PLR_STATS_X, y = PLR_STATS_Y;
    float rad = NV_NEXT_R / (w < h ? w : h);
    GfxRect r = { x, y, w, h };
    // Translucent, not the card's 0.92: the picture shows through, so the panel
    // reads as an overlay on the film rather than a slab over it.
    gfx_color(r, rad, NV_TRK_INK_R, NV_TRK_INK_G, NV_TRK_INK_B, 0.55f * alpha);
    gfx_rect(r, 0, GFX_RING_INSET, 0, 1.0f / (w < h ? w : h), 0, rad,
             1, 1, 1, 0.10f * alpha);
    txt_tracking(TXT_NEXT_KICK, "STREAM STATS", 150, 152, 158,
                 x + padX, y + padY, alpha, NV_NEXT_KICK_TRACK);
    for (i = 0; i < n; i++) {
      float ry = y + padY + kickH + headGap + i * (lineH + rowGap);
      TxtLine l = txt_line(TXT_PLR_STATL, labels[i], 150, 152, 158, 255);
      TxtLine v = txt_line(TXT_PLR_STAT,  values[i], 255, 255, 255, 255);
      float vright = x + w - padX;
      if (i > 0)
        gfx_color((GfxRect){ x + padX, ry - rowGap * 0.5f - 0.5f, w - padX * 2.0f, 1.0f },
                  0.0f, 1, 1, 1, 0.06f * alpha);
      // The badge rides at the right end of the row it rates, and the reading sits
      // to its left — the web app's `margin-left: 8px` inside the value span.
      if (i == 0 && badge) {
        float pw = (float)lb.w + 20.0f, ph = (float)lb.h + 8.0f;
        GfxRect p = { vright - pw, ry + (lineH - ph) * 0.5f, pw, ph };
        gfx_color(p, 0.5f, br, bg, bb, 0.18f * alpha);
        txt_draw_alpha(txt_line(TXT_PLR_BADGE, badge,
                                (int)(br * 255), (int)(bg * 255), (int)(bb * 255), 255),
                       p.x + 10.0f, p.y + (ph - (float)lb.h) * 0.5f, alpha);
        vright -= pw + 10.0f;
      }
      txt_draw_alpha(l, x + padX, ry + (lineH - (float)l.h) * 0.5f, alpha);
      txt_draw_alpha(v, vright - v.w, ry, alpha);
    } }
}

// THE TWO JUMP-AHEAD PROMPTS. Both hang off the SAME corner at the SAME two
// heights — see the note at NV_PJ_RIGHT for why that is the design and not an
// accident. This returns the bottom edge either one should sit on.
//
// `visible` is the transport. While it is down these prompts are what OK acts on
// (see the !visible branch in the key handler), so they are drawn in their
// FOCUSED state then and in their resting state when the controls are up and OK
// belongs to the button row instead. That is the honest reading of the web app's
// focus classes on a player that has no cursor.
static float jumpBottom(void) {
  return NV_SCREEN_H - (visible ? NV_PJ_BOTTOM_UP : NV_PJ_BOTTOM);
}

static void drawSkipIntro(int kind) {
  const char *rot = kind == INTRO_SUMMARY ? "Skip recap" : "Skip intro";
  int hot = cardHot();
  // Focused: --secondary-color #F5F5F5 with dark ink. Resting: rgba(30,30,30,.85).
  int ink = hot ? 11 : 255;
  TxtLine t = txt_line(TXT_SKIP, rot, ink, ink, ink, 255);
  // The box is twice the ink (see NV_SKIP_ICON), so the layout counts the INK and
  // the box is hung centred on it — otherwise the transparent quarter either side
  // opens a gap the eye reads as extra spacing.
  float h = NV_SKIP_PADY * 2 + (float)t.h;
  float w = NV_SKIP_PADX * 2 + NV_SKIP_ICON_INK + NV_SKIP_GAP + (float)t.w;
  float x = NV_SCREEN_W - NV_PJ_RIGHT - w, y = jumpBottom() - h;
  float c = hot ? 0.961f : NV_SKIP_BG, a = hot ? 1.0f : NV_SKIP_BG_A;
  float pad = (NV_SKIP_ICON - NV_SKIP_ICON_INK) * 0.5f;
  gfx_color((GfxRect){ x, y, w, h }, NV_SKIP_R / h, c, c, c, a * entry);
  pointer_zone(x, y, w, h, pointCard, -1, 0);
  { float g = ink / 255.0f;
    gfx_icon((GfxRect){ x + NV_SKIP_PADX - pad, y + (h - NV_SKIP_ICON) * 0.5f,
                        NV_SKIP_ICON, NV_SKIP_ICON }, "forward", g, g, g, entry); }
  txt_draw_alpha(t, x + NV_SKIP_PADX + NV_SKIP_ICON_INK + NV_SKIP_GAP,
                 y + (h - (float)t.h) * 0.5f, entry);

  // THE COUNTDOWN, across the button's base — and it is the SAME BAR the Continue
  // Watching card carries, GFX_CW_BAR: a rgba(255,255,255,0.16) track with a
  // #F5F5F5 fill over it, full-bleed against the bottom edge.
  //
  // Using the mode rather than two rectangles is what makes it fit. The mode cuts
  // the band with the HOST'S OWN corner SDF, so both ends round exactly as the
  // button's 24px radius does. Drawn as plain rects it had to be inset by half the
  // radius to keep its square corners from poking out through the pill, which left
  // a bar visibly narrower than the thing it measures.
  // IT INVERTS WITH THE BUTTON. The countdown only runs while the transport is
  // down, which is exactly when this button is focused and therefore white — so
  // the bar's own white-on-white was invisible in the one state it is ever
  // animating in. It read as a bar that never moved, when it was moving the whole
  // time. Tinted to the ink on a light button, left at the card's own colours on
  // the dark one.
  { float p = skipElapsed / PLR_SKIP_MS;
    float br = hot ? NV_TRK_INK_R : 1.0f;
    float bg = hot ? NV_TRK_INK_G : 1.0f;
    float bb = hot ? NV_TRK_INK_B : 1.0f;
    if (p < 0.0f) p = 0.0f;
    if (p > 1.0f) p = 1.0f;
    gfx_rect((GfxRect){ x, y, w, h }, 0, GFX_CW_BAR, 0,
             NV_SKIP_BAR / h, p, NV_SKIP_R / h, br, bg, bb, entry); }
}

static void drawNextCard(const CatEp *next) {
  struct { const char *label, *icon; } pill[NEXT_NPILLS] = {
    { "Play now", "play" }, { "Not now", NULL } };
  float pw[NEXT_NPILLS], lw[NEXT_NPILLS], rowW = 0, copyW, w, h, x, y, tx0, ty0, cx;
  int hot = cardHot(), i;
  char code[24];
  float titleW;
  TxtLine codeL, dotL, title, l[NEXT_NPILLS];

  // Measured first: the card is as wide as its copy needs.
  for (i = 0; i < NEXT_NPILLS; i++) {
    int sel = hot && nextFocus == i;
    int ink = sel ? NV_TRK_FOCUS_INK : 226;
    // Not now is Bold only while it is selected. Its width is always the Bold
    // one's, so moving the focus does not resize the pill and shift the card.
    TxtLine bold = txt_line(TXT_NEXT_PILL, pill[i].label, ink, ink, ink, 255);
    l[i] = (i == NEXT_DISMISS && !sel)
         ? txt_line(TXT_NEXT_PILL_MED, pill[i].label, ink, ink, ink, 255) : bold;
    lw[i] = (float)bold.w;
    pw[i] = NV_NEXT_PILL_PADX * 2 + lw[i]
          + (pill[i].icon ? NV_NEXT_PLAY_INK_W + NV_NEXT_PILL_ICON_GAP : 0.0f);
    rowW += pw[i] + (i ? NV_NEXT_PILL_GAP : 0.0f);
  }
  // "S1 E12", a dot, then the name, as three pieces so the dot gets real air on
  // both sides instead of the font's own narrow spaces.
  snprintf(code, sizeof code, "S%d E%d", next->season, next->episode);
  codeL = txt_line(TXT_NEXT_TITLE, code, 255, 255, 255, 255);
  dotL  = txt_line(TXT_NEXT_TITLE, "·", 150, 152, 158, 255);
  { float cap = (NV_NEXT_COPY_MAX > rowW ? NV_NEXT_COPY_MAX : rowW)
              - (float)codeL.w - (float)dotL.w - NV_NEXT_TITLE_DOT_GAP * 2;
    title = txt_line_trim(TXT_NEXT_TITLE, next->name, 255, 255, 255, 255, cap); }
  titleW = (float)codeL.w + (float)dotL.w + NV_NEXT_TITLE_DOT_GAP * 2 + (float)title.w;
  copyW = titleW > rowW ? titleW : rowW;

  w = NV_NEXT_PADX * 2 + NV_NEXT_THUMB_W + NV_NEXT_GAP + copyW;
  h = NV_NEXT_PAD * 2 + NV_NEXT_THUMB_H;
  x = NV_SCREEN_W - NV_PJ_RIGHT - w; y = jumpBottom() - h;
  tx0 = x + NV_NEXT_PADX; ty0 = y + NV_NEXT_PAD;
  cx = tx0 + NV_NEXT_THUMB_W + NV_NEXT_GAP;

  gfx_color((GfxRect){ x, y, w, h }, NV_NEXT_R / h,
            NV_TRK_INK_R, NV_TRK_INK_G, NV_TRK_INK_B, 0.97f * entry);
  // A hairline edge, so the card separates from a dark frame behind it.
  gfx_rect((GfxRect){ x, y, w, h }, 0, GFX_RING_INSET, 0, 1.0f / h, 0, NV_NEXT_R / h,
           1, 1, 1, 0.10f * entry);

  // The thumbnail, inset and rounded on all four corners. Only the PLACEHOLDER
  // carries a fill: tinting the wrapper put a visible box behind every real still.
  { GfxRect tr = { tx0, ty0, NV_NEXT_THUMB_W, NV_NEXT_THUMB_H };
    float tr_r = NV_NEXT_THUMB_R / NV_NEXT_THUMB_H;
    const char *art = next->thumb[0] ? next->thumb
                    : (item() && item()->backdrop[0] ? item()->backdrop : NULL);
    GLuint tx = art ? tex_get_width(art, (int)NV_NEXT_THUMB_W) : 0;
    if (tx) {
      gfx_tex_aspect_current = tex_aspect(art);
      gfx_rect(tr, tx, GFX_CARD, 0, 0, 0, tr_r, 0, 0, 0, entry);
      gfx_tex_aspect_current = 0;
    } else {
      gfx_color(tr, tr_r, 1, 1, 1, 0.06f * entry);
    } }

  // "UP NEXT · Playing in 8s". The caption stays quiet grey and the countdown
  // beside it is the one piece of the line in full white, because it is the one
  // that changes.
  // With autoplay off there is no countdown to show: the kicker stands alone.
  { char count[32];
    int secs = (int)ceilf((PLR_NEXT_MS - nextElapsed) / 1000.0f);
    float kx;
    TxtLine dot, cl;
    if (secs < 1) secs = 1;
    snprintf(count, sizeof count, "Playing in %ds", secs);
    kx = cx + txt_tracking(TXT_NEXT_KICK, "UP NEXT", 150, 152, 158,
                           cx, ty0, entry, NV_NEXT_KICK_TRACK);
    if (settings_next_autoplay()) {
      dot = txt_line(TXT_NEXT_COUNT, "·", 80, 82, 88, 255);
      cl  = txt_line(TXT_NEXT_COUNT, count, 255, 255, 255, 255);
      txt_draw_alpha(dot, kx + 6.0f, ty0, entry);
      txt_draw_alpha(cl, kx + 6.0f + (float)dot.w + 10.0f, ty0, entry);
    } }

  { float tx = cx, ty = ty0 + NV_NEXT_TITLE_Y;
    txt_draw_alpha(codeL, tx, ty, entry);
    tx += (float)codeL.w + NV_NEXT_TITLE_DOT_GAP;
    txt_draw_alpha(dotL, tx, ty, entry);
    tx += (float)dotL.w + NV_NEXT_TITLE_DOT_GAP;
    txt_draw_alpha(title, tx, ty, entry); }

  // THE PAIR, on the thumbnail's bottom edge. The selected one inverts to white,
  // and only while OK reaches this card at all; at rest both wear the same dark
  // outlined fill.
  { float py = ty0 + NV_NEXT_THUMB_H - NV_NEXT_PILL_H, px = cx, ph = NV_NEXT_PILL_H;
    for (i = 0; i < NEXT_NPILLS; i++) {
      int sel = hot && nextFocus == i;
      float tx = px + NV_NEXT_PILL_PADX;
      GfxRect pr = { px, py, pw[i], ph };
      pointer_zone(pr.x, pr.y, pr.w, pr.h, pointCard, i, 0);
      if (sel) {
        gfx_color(pr, 0.5f, 0.961f, 0.961f, 0.961f, entry);
      } else {
        // INSET, not GFX_RING: that band straddles the edge and the quad clips
        // its outer half, which left the outline broken along the pill.
        gfx_color(pr, 0.5f, 0.110f, 0.114f, 0.129f, entry);
        gfx_rect(pr, 0, GFX_RING_INSET, 0, NV_NEXT_PILL_RING / ph, 0, 0.5f,
                 0.227f, 0.235f, 0.259f, entry);
      }
      if (pill[i].icon) {
        float g = (sel ? NV_TRK_FOCUS_INK : 226) / 255.0f;
        // Hung so the glyph's INK starts at tx — see NV_NEXT_PLAY_BOX.
        // DECODED AT THE TRANSPORT'S SIZE, not its own. The cache keeps one
        // texture per file at an exact width, and the transport draws this same
        // play.png at PLR_ICON_H while paused: two widths in one frame re-decoded
        // it every frame, and the transport's button came out blurry and jumping.
        gfx_icon_at((GfxRect){ tx - NV_NEXT_PLAY_BOX * NV_NEXT_PLAY_INK_X,
                               py + (ph - NV_NEXT_PLAY_BOX) * 0.5f,
                               NV_NEXT_PLAY_BOX, NV_NEXT_PLAY_BOX },
                    pill[i].icon, PLR_ICON_H, g, g, g, entry);
        tx += NV_NEXT_PLAY_INK_W + NV_NEXT_PILL_ICON_GAP;
      }
      txt_draw_alpha(l[i], tx + (lw[i] - (float)l[i].w) * 0.5f,
                     py + (ph - (float)l[i].h) * 0.5f, entry);
      px += pw[i] + NV_NEXT_PILL_GAP;
    } }

  // THE COUNTDOWN along the card's base: the Continue Watching bar, white on a
  // faint track, cut by the card's own corner SDF so both ends round with it.
  if (settings_next_autoplay()) {
    float p = nextElapsed / PLR_NEXT_MS;
    if (p < 0.0f) p = 0.0f;
    if (p > 1.0f) p = 1.0f;
    gfx_rect((GfxRect){ x, y, w, h }, 0, GFX_CW_BAR, 0,
             NV_NEXT_BAR / h, p, NV_NEXT_R / h, 1, 1, 1, entry); }
}

static void drawActionsEpisode(void){
  const CatEp *next=player_next_episode();int kind=0;
  if(nextCardUp()&&next)      drawNextCard(next);
  else if(skipUp(NULL,&kind)) drawSkipIntro(kind);
}

// THE SHEET TAKES THE SCREEN. With Sources open, everything the player draws over
// the picture — the transport, its gradients, the prompts, the stats, the loading
// logo — fades out on the sheet's own curve, so the list is the one thing to read.
// The picture stays, and so do the subtitles: those are content, not chrome.
// THE QUICK SEEK'S READOUT: "+ 20 >" at the right edge going forward,
// "< − 20" at the left going back, centred on the frame's height. The chevron
// nudges outward on each press so a press that does not change the number (the
// film's end) still shows it was heard. Past a minute it switches to m:ss, the
// transport's own format. A black copy 2px down stands in for a text shadow, the
// way the button labels do it — there is no scrim at mid-height.
static void drawQuickSeek(float q) {
  int s = (int)(fabsf(quickDelta) + 0.5f), fwd = quickDelta > 0.0f;
  char num[24], text[32];
  if (q <= 0.005f || s == 0) return;
  if (s < 60) snprintf(num, sizeof num, "%d", s);
  else        fmtTime(num, sizeof num, (float)s, 0);
  snprintf(text, sizeof text, "%s %s", fwd ? "+" : "\xe2\x88\x92", num);
  { TxtLine l  = txt_line(TXT_PLR_QUICK, text, 255, 255, 255, 255);
    TxtLine sh = txt_line(TXT_PLR_QUICK, text, 0, 0, 0, 255);
    const float icon = 72.0f, gap = 2.0f, nudge = 10.0f * quickPulse;
    const char *chev = fwd ? "chevron_right" : "chevron_left";
    float cy = NV_SCREEN_H * 0.5f, ty = cy - (float)l.h * 0.5f, tx, ix;
    if (fwd) {
      ix = NV_SCREEN_W - PLR_MARGIN - icon * 0.5f;
      tx = ix - icon * 0.5f - gap - (float)l.w;
      ix += nudge;
    } else {
      ix = PLR_MARGIN + icon * 0.5f;
      tx = ix + icon * 0.5f + gap;
      ix -= nudge;
    }
    txt_draw_alpha(sh, tx, ty + 2.0f, q * 0.55f);
    txt_draw_alpha(l, tx, ty, q);
    iconFile(ix, cy + 2.0f, q * 0.55f, 0.0f, chev, icon);
    iconFile(ix, cy, q, 1.0f, chev, icon); }
}

static float chrome = 1.0f;
static void drawPlayer(Uint32 now);
void player_draw_subtitle_over(void) {
  if (!is_open || tracks_style_shown() <= 0.0f) return;
  gfx_opacity_group = 1.0f;
  drawSubtitleExternal();
}

// --- the pause overlay ---------------------------------------------------------
//
// WHY THE BLUR IS THE TITLE'S ART AND NOT THE PAUSED FRAME: the video is a hardware
// plane BEHIND the GL surface, and GL cannot read a single pixel of it — what this
// surface holds over the video is a hole. So the frame is covered, not filtered:
// the title's backdrop, run through the gaussian targets (gfx_blur_*, pair 0, which
// nothing else draws into any more), stands in for it. It is the same art the
// loading screen showed, so tex_get_hero is a cache hit and the blur runs once per
// title, not per frame.
static char pauseBlurFor[512];

static const CatEp *currentEp(void) {
  if (epT < 1) return NULL;
  for (int i = 0; i < cat_n_episodes(idx); i++) {
    const CatEp *ep = cat_episode(idx, i);
    if (ep && ep->season == epT && ep->episode == epE) return ep;
  }
  return NULL;
}

// The next " · "-separated piece of `*s`, trimmed, into `out`; 0 when none is left.
// strtok would not do: its delimiters are single BYTES, and the dot's two (C2 B7)
// also occur inside other characters.
static int pauseNextPiece(const char **s, char *out, size_t n) {
  const char *a = *s;
  if (!a || !*a) return 0;
  const char *b = strstr(a, "\xc2\xb7");
  size_t len = b ? (size_t)(b - a) : strlen(a);
  *s = b ? b + 2 : a + len;
  while (len && *a == ' ') { a++; len--; }
  while (len && a[len - 1] == ' ') len--;
  if (len >= n) len = n - 1;
  memcpy(out, a, len); out[len] = 0;
  return 1;
}

// THE META LINE AS THE HOME HERO'S TOKENS: "Action • Adventure • Drama  • 2024 • 14+".
// Two groups, as there — what the title IS (up to three genres), then its NUMBERS
// (year, a film's running time, the age rating) — with *nLead the index where the
// second begins. The type word ("TV Show", "Movie") at the head of `genre` is
// dropped, and so is a series' season count: the episode line below already says
// where in the series this is.
#define PAUSE_META_MAXTOK 8
static int pauseMeta(const CatItem *c, char tok[][64], int *nLead) {
  char t[64];
  const char *s;
  int n = 0, genres = 0;
  for (s = c->genre; genres < 3 && n < PAUSE_META_MAXTOK && pauseNextPiece(&s, t, sizeof t); ) {
    if (!t[0] || !strcmp(t, "TV Show") || !strcmp(t, "Movie") ||
        !strcmp(t, "Series") || !strcmp(t, "Film")) continue;
    snprintf(tok[n++], 64, "%s", t); genres++;
  }
  *nLead = n;
  if (c->year > 0 && n < PAUSE_META_MAXTOK) snprintf(tok[n++], 64, "%d", c->year);
  for (s = c->meta; n < PAUSE_META_MAXTOK && pauseNextPiece(&s, t, sizeof t); ) {
    if (!t[0] || strstr(t, "season") || strstr(t, "Season")) continue;
    if (c->year > 0 && atoi(t) == c->year) continue;
    snprintf(tok[n++], 64, "%s", t);
  }
  // A bare "14" reads as a count, not a rating; "14+" does not.
  if (c->age_rating[0] && n < PAUSE_META_MAXTOK) {
    const char *r = c->age_rating;
    int numeric = 1;
    for (const char *q = r; *q; q++) if (*q < '0' || *q > '9') numeric = 0;
    snprintf(tok[n++], 64, numeric ? "%s+" : "%s", r);
  }
  return n;
}

static void drawPauseOverlay(Uint32 now) {
  (void)now;
  float p = pauseAnim * entry;
  if (p <= 0.004f) return;
  const CatItem *c = item();
  if (!c) return;
  gfx_opacity_group = 1.0f;
  float e  = anim_smooth(p);
  // The copy comes in behind the veil, not with it: first the picture softens, then
  // the title settles into it.
  float ec = anim_smooth((p - 0.30f) / 0.70f);
  GfxRect screen = { 0, 0, NV_SCREEN_W, NV_SCREEN_H };

  // --- the blur ---
  const char *art = c->backdrop[0] ? c->backdrop : NULL;
  if (art && strcmp(pauseBlurFor, art)) {
    GLuint tex = tex_get_hero(art);
    if (tex) {
      gfx_blur_generate(0, tex, tex_aspect(art));
      snprintf(pauseBlurFor, sizeof pauseBlurFor, "%s", art);
    }
  }
  if (art && !strcmp(pauseBlurFor, art)) gfx_blur_draw(0, screen, e);
  else gfx_color(screen, 0.0f, 0.02f, 0.02f, 0.025f, 0.86f * e);
  // Darker to the left and the base, where the copy sits, and a band along the top
  // for the clock — the blurred art alone is too bright for white type to hold on.
  gfx_color(screen, 0.0f, 0, 0, 0, 0.30f * e);
  gfx_rect(screen, 0, GFX_VEIL, 0, 0, 0, 0.0f, 0, 0, 0, 0.92f * e);
  { GfxRect top = { 0, 0, NV_SCREEN_W, 220 };
    gfx_rect(top, 0, GFX_VEIL_TOP, 0, 0, 0, 0.0f, 0, 0, 0, 0.55f * e); }

  // --- the clock, top right ---
  { time_t nowT = time(NULL);
    struct tm lt;
    char hhmm[8];
    localtime_r(&nowT, &lt);
    strftime(hhmm, sizeof hhmm, "%H:%M", &lt);
    TxtLine lc = txt_line(TXT_PAUSE_CLOCK, hhmm, 255, 255, 255, 255);
    txt_draw_alpha(lc, NV_SCREEN_W - NV_PAUSE_X - lc.w, NV_PAUSE_TOP, 0.90f * ec); }

  // --- where playback stopped, bottom right, on the copy's base line ---
  // No separator: the weight and the dimmer time already split "Paused" from
  // "0:03 / 52:00", and a dot between them was one more mark on a quiet screen.
  { char at[16], total[16], status[40];
    fmtTime(at, sizeof at, posSeg, 0);
    fmtTime(total, sizeof total, durationSeg, 0);
    snprintf(status, sizeof status, "%s / %s", at, total);
    TxtLine lp = txt_line(TXT_PAUSE_META, "Paused", 255, 255, 255, 255);
    TxtLine ls = txt_line(TXT_PAUSE_META, status, 255, 255, 255, 255);
    float icon = 22.0f, gap = 18.0f;
    float base = NV_SCREEN_H - NV_PAUSE_BOTTOM + (1.0f - ec) * NV_PAUSE_RISE;
    float mid = base - lp.h * 0.5f;
    float xs = NV_SCREEN_W - NV_PAUSE_X - (icon + 12.0f + lp.w + gap + ls.w);
    iconFile(xs + icon * 0.5f, mid, 0.90f * ec, 1.0f, "pause", icon);
    xs += icon + 12.0f;
    txt_draw_alpha(lp, xs, mid - lp.h * 0.5f, 0.90f * ec);
    xs += lp.w + gap;
    txt_draw_alpha(ls, xs, mid - ls.h * 0.5f, NV_HERO_META_INK * ec); }

  // --- the title, stacked from the base up: logo, meta line, episode, synopsis ---
  const CatEp *ep = currentEp();
  char meta[PAUSE_META_MAXTOK][64], epCode[24];
  int nLead, nMeta = pauseMeta(c, meta, &nLead);
  const char *epName = NULL;
  if (epT > 0) {
    snprintf(epCode, sizeof epCode, "S%d E%d", epT, epE);
    epName = (ep && ep->name[0]) ? ep->name
           : (epT == c->season && epE == c->episode && c->nameEpisode[0]) ? c->nameEpisode
           : NULL;
  }
  const char *sin = (ep && ep->synopsis[0]) ? ep->synopsis : c->synopsis;
  int nSin = sin[0] ? txt_block_lines(TXT_PAUSE_SIN, sin, NV_PAUSE_COPY_W) : 0;
  if (nSin > NV_PAUSE_SIN_LINES) nSin = NV_PAUSE_SIN_LINES;

  GLuint logo = 0;
  float lw = NV_PAUSE_LOGO_W, lh = 0.0f;
  if (c->logo[0]) {
    // 640, the loading screen's request: the same width is a cache hit.
    logo = tex_get_width(c->logo, 640);
    float ar = tex_aspect(c->logo);
    if (logo && ar > 0.0f) {
      lh = lw / ar;
      if (lh > NV_PAUSE_LOGO_H) { lh = NV_PAUSE_LOGO_H; lw = lh * ar; }
    } else logo = 0;
  }
  TxtLine title = txt_line_trim(TXT_PAUSE_TITLE, c->title, 255, 255, 255, 255, NV_PAUSE_COPY_W);
  float hMeta = nMeta ? (float)txt_line(TXT_HERO_META, meta[0], 255, 255, 255, 255).h : 0.0f;
  // "S1 E1   The End": no separator, the dimmer code and a wider gap do its job.
  const float epGap = 16.0f;
  TxtLine lcode = {0}, lname = {0};
  if (epT > 0) {
    lcode = txt_line(TXT_PAUSE_EP, epCode, 255, 255, 255, 255);
    if (epName)
      lname = txt_line_trim(TXT_PAUSE_EP, epName, 255, 255, 255, 255,
                            NV_PAUSE_COPY_W - lcode.w - epGap);
  }

  float hIdent = logo ? lh : (float)title.h;
  float gIdent = nMeta ? 28.0f : 0.0f, gEp = 30.0f, gSin = 14.0f;
  float h = hIdent + gIdent + hMeta;
  if (epT > 0) h += gEp + lcode.h;
  if (nSin)    h += (epT > 0 ? gSin : gEp) + nSin * NV_PAUSE_SIN_LD;

  float x = NV_PAUSE_X;
  float y = NV_SCREEN_H - NV_PAUSE_BOTTOM - h + (1.0f - ec) * NV_PAUSE_RISE;

  if (logo) {
    // Bottom-aligned in its slot, so a short wide logo sits on the meta line
    // instead of floating above it.
    GfxRect r = { x, y + hIdent - lh, lw, lh };
    gfx_logo(r, logo, tex_brand_dark(c->logo), .95f, .95f, .97f, ec);
  } else {
    txt_draw_alpha(title, x, y, ec);
  }
  y += hIdent + gIdent;
  // The home hero's line, drawn the way home.c draws it: TXT_HERO_META, the tokens
  // at 62% and every "•" at 34% with NV_HERO_META_SEP either side, and the wider
  // NV_HERO_META_GROUP before the numbers. A token that does not fit is dropped whole.
  { int ink = (int)(255.0f * NV_HERO_META_INK + 0.5f);
    int dim = (int)(255.0f * NV_HERO_META_DOT + 0.5f);
    float cx = x, limit = x + NV_PAUSE_COPY_W;
    int drawn = 0;
    for (int i = 0; i < nMeta; i++) {
      TxtLine lt = txt_line(TXT_HERO_META, meta[i], ink, ink, ink, 255);
      TxtLine ld = { 0, 0, 0 };
      float lead = 0.0f, sep = 0.0f;
      if (drawn) {
        lead = (i == nLead) ? NV_HERO_META_GROUP : NV_HERO_META_SEP;
        ld = txt_line(TXT_HERO_META, "\xe2\x80\xa2", dim, dim, dim, 255);
        sep = lead + (float)ld.w + NV_HERO_META_SEP;
      }
      if (cx + sep + (float)lt.w > limit) break;
      if (drawn) {
        txt_draw_alpha(ld, cx + lead, y + ((float)lt.h - (float)ld.h) * 0.5f, ec);
        cx += sep;
      }
      txt_draw_alpha(lt, cx, y, ec);
      cx += (float)lt.w;
      drawn++;
    } }
  y += hMeta;
  // The episode and its synopsis are ONE block, a wider gap above it than inside it,
  // so the stack reads as two things — the show, then this episode — not five lines.
  if (epT > 0) {
    float xe = x;
    y += gEp;
    txt_draw_alpha(lcode, xe, y, 0.55f * ec);
    xe += lcode.w;
    if (epName) txt_draw_alpha(lname, xe + epGap, y, 0.95f * ec);
    y += lcode.h;
  }
  if (nSin) {
    y += epT > 0 ? gSin : gEp;
    txt_block_trim(TXT_PAUSE_SIN, sin, 255, 255, 255, x, y, NV_PAUSE_COPY_W,
                   NV_PAUSE_SIN_LD, 0.70f * ec, NV_PAUSE_SIN_LINES);
  }
}

void player_draw(Uint32 now) {
  if (!is_open) return;
  // The audio and subtitle sheets, and the episode list, take the screen the
  // same way.
  { float sheet = stream_sheet_shown(), tracks = tracks_shown(), eps = episodes_shown();
    if (tracks > sheet) sheet = tracks;
    if (eps > sheet) sheet = eps;
    chrome = 1.0f - sheet; }
  drawPlayer(now);
  drawPauseOverlay(now);
  gfx_opacity_group = 1.0f;
}
static void drawPlayer(Uint32 now) {
  (void)now;
  const CatItem *c = item();

  // --- the video frame ---
  // With a pipeline there is nothing to draw: the video is on a hardware plane BEHIND
  // this surface, and what is done here is opening the hole it shows through.
  // The hole has to come from HERE and not at the end of the frame: done last it
  // would erase the controls themselves. Everything that comes after (veil, bar,
  // text) draws over the hole and stays visible, because the blend's alpha is added
  // — a veil at 60% over the hole gives back 0.6 of opacity, which is exactly the
  // darkening wanted over the video.
  //
  // With no pipeline, the frame's place holds the still key art. GFX_CARD with radius
  // 0 is the full-screen quad: the shader's crop (cover) is what stops the 16:9 art
  // stretching when the screen is not exactly 16:9.
  GfxRect screen = { 0, 0, NV_SCREEN_W, NV_SCREEN_H };
  // THE SECOND ACT OF THE ENTRANCE: 0 until the art has mostly covered what was under
  // it, then up to 1. What is NEW on this screen waits for it; see the staging note at
  // the loading indicator.
  float late = anim_smooth((entry - 0.70f) / 0.30f);
  if (player_has_video()) {
    // The hole follows the SAME rectangle that went to the hardware plane, clipped by
    // the screen. Always punching the whole screen, as before, left a black band in
    // the modes that do not fill everything ("Original" on a 2.39:1 delivered as
    // 2.39:1): the hole showed the nothing behind the plane instead of showing the plane.
    PlrRect r = aspectVisible(aspect);
    GfxRect hole;
    hole.x = r.x; hole.y = r.y; hole.w = r.w; hole.h = r.h;
    // Outside the hole is BLACK, and not the key art: it is what the TV shows beside
    // the video plane, and painting something else there would create a border that
    // does not exist on the device.
    if (hole.w < NV_SCREEN_W - 0.5f || hole.h < NV_SCREEN_H - 0.5f)
      gfx_color(screen, 0.0f, 0, 0, 0, 1.0f);
    if (hole.w > 0.0f && hole.h > 0.0f) {
      // The Mac preview has a frame to draw instead of a plane to reveal.
      GLuint frame = video_frame_texture();
      if (frame) gfx_texture(hole, frame);
      else gfx_hole(hole);
    }
  } else {
    const char *art = (c && c->backdrop[0]) ? c->backdrop : NULL;
    GLuint tex = art ? tex_get_hero(art) : 0;   // it fills the whole screen
    if (tex) {
      gfx_tex_aspect_current = tex_aspect(art);
      gfx_rect(screen, tex, GFX_CARD, 0, 0, 0, 0.0f, 0, 0, 0, entry);
      gfx_tex_aspect_current = 0.0f;
    } else {
      gfx_color(screen, 0.0f, 0.04f, 0.04f, 0.05f, entry);
    }
  }

  // THE OPENING SCREEN: the title's logo in the middle of the darkened art, breathing.
  //
  // It used to be the logo above a ring of dots, "Opening source" and the episode
  // line — a spinner stacked under an identity. The web app has no spinner here at
  // all (.player-loading-identity): the logo itself pulses, scale 1 -> 1.04 over 2s
  // and back (playerLoadingIdentityPulse), and that is the whole "still working"
  // signal. The spinner itself moved onto the transport's play button, which
  // turns while the source opens (drawStartingButton).
  gfx_opacity_group = chrome;   // the frame and the hole above are not chrome
  flying = 0;
  // THE OUTRO: once the picture is up, the logo stays a moment longer so its fill
  // is SEEN to finish, then fades with the veil — 150ms to land, 300ms to go.
  int loadingNow = player_loading();
  float outro = 1.0f;
  if (!loadingNow && loadEndAt && now - loadEndAt < PLR_LOAD_OUTRO_MS)
    outro = 1.0f - anim_clamp(((float)(now - loadEndAt) - 150.0f) / 300.0f, 0.0f, 1.0f);
  else if (!loadingNow) outro = 0.0f;
  if (outro > 0.0f) {
    GfxRect dark = { 0, 0, NV_SCREEN_W, NV_SCREEN_H };
    gfx_color(dark, 0.0f, 0, 0, 0, 0.55f * entry * outro);
    // THE STAGING. The logo is the one thing the viewer was already looking at, so it
    // leads; anything new waits until it has nearly arrived and then rises the last
    // few pixels into place.
    float rise = (1.0f - late) * 18.0f;
    // THE WEB APP'S TIMING, exactly (.player-loading-identity):
    //   playerLoadingIdentityFade   700ms linear, 400ms delay   opacity 0 -> 1
    //   playerLoadingIdentityPulse 2000ms linear, 400ms delay,
    //                              infinite alternate           scale 1 -> 1.04
    // so a triangle wave: 2s growing, 2s shrinking, at constant speed. An eased
    // curve was tried first and read as a different, softer animation than the
    // web app's; the owner asked for the web's.
    //
    // The amplitude still rides `late`, so a logo flying in from the title screen
    // lands on its exact rect before it grows, and settles before it flies home.
    // Off with reduced animations.
    float tAnim = (float)(now - openedAt) - 400.0f;
    float fadeIn = anim_clamp(tAnim / 700.0f, 0.0f, 1.0f);
    float wave = 0.0f;
    if (!settings_animations_reduced() && tAnim > 0.0f) {
      float ph = fmodf(tAnim, 4000.0f);
      wave = late * (ph < 2000.0f ? ph / 2000.0f : (4000.0f - ph) / 2000.0f);
    }
    float pulse = 1.0f + 0.04f * wave;
    GLuint logo = 0;
    // .player-loading-logo at the TV block's size: min(33.33vw, 640px) wide,
    // min(18.75vw, 360px) tall — 640 x 360 at 1080p. 520 x 160 was the ATV port's.
    float w = 640, h = 120, ar = 0.0f;
    if (c && c->logo[0]) {
      // ON A HANDOFF, THE DETAIL'S TEXTURE. The page asked for this file with
      // tex_get_exact at the width it drew it; asking for another width here while
      // both screens draw would re-decode the PNG every frame (see the logo note in
      // detail.c). The same request is a cache hit, and 810 -> 520 is a downscale.
      logo = (fromDetail && flyFrom.w > 0.0f) ? tex_get_exact(c->logo, flyFrom.w)
                                              : tex_get_width(c->logo, 640);
      ar = tex_aspect(c->logo);
      h = ar > 0 ? w / ar : 120;
      if (h > 360) { h = 360; w = h * ar; }
    }
    if (logo) {
      // Centred on the screen's middle, lifted a little: the transport occupies the
      // bottom third while the source opens, and dead centre sat the logo visibly
      // low against it.
      GfxRect r = { (NV_SCREEN_W - w) * .5f, NV_SCREEN_H * .5f - h * .5f - 60, w, h };
      // Coming from anywhere but the title screen the logo has nowhere to fly
      // from, so it fades in on the web's clock instead.
      float la = entry * fadeIn * outro;
      // THE FLIGHT: from the rect the title screen drew it at to the centre, on
      // `entry` itself, so it lands exactly as the art finishes covering the page. It
      // flies at full opacity — the fade is for things with nowhere to come from, and
      // the page's own copy is hidden for as long as this one is in the air.
      if (loadingNow && fromDetail && flyFrom.w > 0.0f) {
        float p = entry;
        r.x = flyFrom.x + (r.x - flyFrom.x) * p;
        r.y = flyFrom.y + (r.y - flyFrom.y) * p;
        r.w = flyFrom.w + (r.w - flyFrom.w) * p;
        r.h = flyFrom.h + (r.h - flyFrom.h) * p;
        la = 1.0f;
        flying = 1;
      }
      // Scaled about its own centre, which is the sheet's transform-origin.
      { float pw = r.w * pulse, ph = r.h * pulse;
        r.x -= (pw - r.w) * 0.5f; r.y -= (ph - r.h) * 0.5f; r.w = pw; r.h = ph; }
      // GFX_LOGO and not GFX_TEXT/GFX_BRAND: those end the art on the quad's
      // hard edge, and at this slow a scale the tight-cropped letters crept
      // outward a whole pixel at a time — the pulse looked stepped.
      //
      // Two passes: the whole logo TRANSLUCENT, then the loaded part at full over
      // it. The empty part keeps its own colours with the backdrop showing through
      // — a flat grey silhouette was tried and the owner preferred this.
      //
      // The dimming rides `late`, so a logo flying in from the title screen
      // arrives as bright as the page's copy it replaced and only then empties to
      // show the progress — and flying home it brightens back before it lands.
      { int brand = tex_brand_dark(c->logo);
        gfx_logo(r, logo, brand, .95f, .95f, .97f, la * (1.0f - 0.68f * late));
        gfx_logo_fill(r, logo, brand, loadFill, .95f, .95f, .97f, la); }
    } else {
      // No logo: the name stands in for it, in the same place, on the same fade.
      // Text cannot be scaled here, so it breathes in opacity instead.
      TxtLine t = txt_line_trim(TXT_PLR_TITLE,c?c->title:"Playing",240,241,244,255,680);
      float breathe = 1.0f - 0.20f * wave;
      txt_draw_alpha(t,(NV_SCREEN_W-t.w)*.5f,NV_SCREEN_H*.5f-t.h*.5f-60+rise,
                     entry*late*fadeIn*breathe*outro);
    }
  }
  if (errorSource) {
    // With the screen's fade: at a flat .65 it stayed on through the whole exit and
    // then vanished in one frame, over the title screen coming back underneath.
    gfx_color(screen,0,.02f,.02f,.025f,.65f*entry);
    TxtLine er=txt_line(TXT_CALLOUT,"Could not open the source",240,241,243,255);
    txt_draw_alpha(er,(NV_SCREEN_W-er.w)*.5f,400,entry);
    TxtLine aj=txt_line(TXT_PG_END,"Open Sources to choose another option or reload.",192,194,200,255);
    txt_draw_alpha(aj,(NV_SCREEN_W-aj.w)*.5f,448,entry);
  }

  // --- the aspect-mode change notice ---------------------------------------
  // The web app's #playerAspectToast, with the measurements of the CSS's TV block:
  //   top min(8.33vw,160px)=160  height min(6.67vw,128px)=128
  //   side padding min(3.33vw,64px)=64  font min(2.92vw,56px)=56
  //   background rgba(9,13,20,0.88), border rgba(255,255,255,0.18), radius 999 (a pill)
  // It is drawn BEFORE the cut by `a`: the aspect key works with the controls hidden,
  // and a notice that only appeared with the bar up would leave the change with no
  // confirmation at all in the most common case.
  if (toastAte > now) {
    // It fades out over the last 200ms, which is the TV block's
    // `transition: opacity 200ms`. Appearing and disappearing instantly reads as a
    // drawing fault.
    float remains = (float)(toastAte - now);
    float at = (remains < 200.0f ? remains / 200.0f : 1.0f) * entry;
    if (toastText[0]) {
      // Not a mode change but a report, so the failure notice's size and place:
      // it takes over from the "Syncing subtitles" pill without jumping.
      TxtLine l = txt_line_trim(TXT_PLR_STAT, toastText, 255, 255, 255, 255, 1100);
      float dot = 12.0f, padX = 28.0f, gap = 14.0f;
      float pw = padX * 2.0f + dot + gap + (float)l.w, ph = (float)l.h + 28.0f;
      GfxRect pil = { (NV_SCREEN_W - pw) * 0.5f, 48.0f, pw, ph };
      gfx_color(pil, 0.5f, 9.0f / 255.0f, 13.0f / 255.0f, 20.0f / 255.0f, 0.90f * at);
      if (toastGood)
        gfx_color((GfxRect){ pil.x + padX, pil.y + (ph - dot) * 0.5f, dot, dot }, 0.5f,
                  90 / 255.0f, 200 / 255.0f, 120 / 255.0f, at);
      else
        gfx_color((GfxRect){ pil.x + padX, pil.y + (ph - dot) * 0.5f, dot, dot }, 0.5f,
                  240 / 255.0f, 160 / 255.0f, 60 / 255.0f, at);
      txt_draw_alpha(l, pil.x + padX + dot + gap, pil.y + (ph - (float)l.h) * 0.5f, at);
    } else {
      TxtLine l = txt_line(TXT_PLR_TITLE, player_aspect_label(aspect),
                             243, 248, 255, 242);
      float pw = (float)l.w + 128.0f, ph = 128.0f;
      GfxRect pil = { (NV_SCREEN_W - pw) * 0.5f, 160.0f, pw, ph };
      // The radius is a FRACTION of the smaller side (see gfx.h): 0.5 is the full pill.
      gfx_color(pil, 0.5f, 9.0f / 255.0f, 13.0f / 255.0f, 20.0f / 255.0f, 0.88f * at);
      txt_draw_alpha(l, pil.x + (pw - l.w) * 0.5f,
                         pil.y + (ph - (float)l.h) * 0.5f, at);
    }
  }

  // --- the FAILURE NOTICE ----------------------------------------------------
  // A pill at the top centre, the aspect notice's plate at a quieter size, with
  // an amber dot so it reads as a report and not as a mode change. It is drawn
  // outside the controls' alpha for the same reason as that notice: most
  // failures happen with the controls hidden, and a notice tied to them would
  // be missed exactly when it matters. It only says THAT something was logged
  // and the short reason — the full record is in the log.
  // --- AUTOSYNC AT WORK ------------------------------------------------------
  // The failure notice's pill, with a slow pulsing blue dot instead of the amber
  // one: something is under way, nothing is wrong. It gives way to any other
  // notice, and AutoSync's own result replaces it as a toast.
  { static Uint32 syncSince;
    const char *st = autosync_status();
    if (!st || toastAte > now || (failNoticeAt && now - failNoticeAt < PLR_FAIL_NOTICE_MS)) {
      if (!st) syncSince = 0;
    } else {
      float na, pulse;
      TxtLine l = txt_line(TXT_PLR_STAT, st, 255, 255, 255, 255);
      float dot = 12.0f, padX = 28.0f, gap = 14.0f;
      float pw = padX * 2.0f + dot + gap + (float)l.w, ph = (float)l.h + 28.0f;
      GfxRect pil = { (NV_SCREEN_W - pw) * 0.5f, 48.0f, pw, ph };
      if (!syncSince) syncSince = now;
      na = anim_clamp((float)(now - syncSince) / 200.0f, 0.0f, 1.0f) * entry;
      pulse = 0.45f + 0.55f * (0.5f + 0.5f * sinf((float)(now - syncSince) * 0.005f));
      gfx_color(pil, 0.5f, 9.0f / 255.0f, 13.0f / 255.0f, 20.0f / 255.0f, 0.90f * na);
      gfx_color((GfxRect){ pil.x + padX, pil.y + (ph - dot) * 0.5f, dot, dot }, 0.5f,
                110 / 255.0f, 170 / 255.0f, 255 / 255.0f, na * pulse);
      txt_draw_alpha(l, pil.x + padX + dot + gap, pil.y + (ph - (float)l.h) * 0.5f, na);
    }
  }

  if (failNoticeAt && now - failNoticeAt < PLR_FAIL_NOTICE_MS) {
    float el = (float)(now - failNoticeAt), rest = (float)PLR_FAIL_NOTICE_MS - el;
    float na = anim_clamp(el / 200.0f, 0.0f, 1.0f) * anim_clamp(rest / 300.0f, 0.0f, 1.0f)
             * entry;
    TxtLine l = txt_line_trim(TXT_PLR_STAT, failNotice, 255, 255, 255, 255, 1100);
    float dot = 12.0f, padX = 28.0f, gap = 14.0f;
    float pw = padX * 2.0f + dot + gap + (float)l.w, ph = (float)l.h + 28.0f;
    GfxRect pil = { (NV_SCREEN_W - pw) * 0.5f, 48.0f, pw, ph };
    gfx_color(pil, 0.5f, 9.0f / 255.0f, 13.0f / 255.0f, 20.0f / 255.0f, 0.90f * na);
    gfx_color((GfxRect){ pil.x + padX, pil.y + (ph - dot) * 0.5f, dot, dot }, 0.5f,
              240 / 255.0f, 160 / 255.0f, 60 / 255.0f, na);
    txt_draw_alpha(l, pil.x + padX + dot + gap, pil.y + (ph - (float)l.h) * 0.5f, na);
  }

  /* They stay when the controls disappear: they are content, not player chrome. */
  // While the subtitle Style bar is up the cue is drawn OVER it instead, by
  // player_draw_subtitle_over: it is what the bar is styling, and under the bar's
  // gradient it would be judged darker than it plays.
  gfx_opacity_group = 1.0f;
  if (tracks_style_shown() <= 0.0f) drawSubtitleExternal();
  gfx_opacity_group = chrome;
  // The stats panel is DELIBERATELY outside the controls' alpha. You open it to
  // watch a number move — the buffer draining, the bitrate on a new source — and
  // tying it to a bar that hides itself after four seconds would mean holding the
  // remote down to keep reading it. It closes the way it opened: the button.
  if (statsOpen) drawStats(entry);

  // ON A HANDOFF THE TRANSPORT WAITS TOO. It came up with the art, and for the first
  // half of the crossfade its title, bar and buttons sat over the title screen's own
  // synopsis and meta lines — two layers of type at once, neither readable.
  float a = anim * entry * (fromDetail ? late : 1.0f);
  // THE QUICK SEEK'S ALPHA, and the bar's: the bar and its time are drawn at
  // whichever of the two is higher, everything else at the controls' own.
  float q = quickAnim * entry;
  float ab = a > q ? a : q;
  // How much of the transport's own layout applies — 0 with the quick overlay
  // alone, 1 with the controls up. The bar, the time and the scrim blend on it.
  float full = ab > 0.0f ? a / ab : 1.0f;

  // Two gradients, as in the web app: .player-controls-gradient-top (150px, 0.7 -> 0)
  // and .player-controls-gradient-bottom (200px, 0 -> 0.8). The bottom one supports
  // the title and the bar; the top one exists because the badges and the age rating
  // sit up there and without it they would disappear over a bright scene. Both follow
  // the controls' animation: fixed, they would leave a permanent shadow over every scene.
  if (ab > 0.005f) {
    // The quick seek's bar and time are two thin lines at the very bottom; the
    // transport's 600px scrim over them darkened half the frame for nothing.
    float gh = PLR_QUICK_GRADIENT + (PLR_GRADIENT_BOTTOM - PLR_QUICK_GRADIENT) * full;
    GfxRect veil = { 0, NV_SCREEN_H - gh, NV_SCREEN_W, gh };
    // GFX_VEIL_PLAYER carries the sheet's five stops itself, so the alpha here is 1
    // and not a density multiplier: scaling it would flatten the curve the mode
    // exists to reproduce. It still fades with the controls through `a`.
    gfx_rect(veil, 0, GFX_VEIL_PLAYER, 0, 0, 0, 0.0f, 0, 0, 0, ab);
    // The pool, hung from the top-right corner where the clock is.
    { GfxRect pool = { NV_SCREEN_W - PLR_POOL_W, 0, PLR_POOL_W, PLR_POOL_H };
      gfx_rect(pool, 0, GFX_VEIL_POOL, 0, 0, 0, 0.0f, 0, 0, 0, a); }
  }

  // THE PROMPTS GO OVER THE GRADIENT, not under it. They were drawn before the
  // veil, which put the transport's bottom scrim across them the moment the
  // controls came up: two opaque cards visibly dimming — they read as sitting
  // BEHIND the picture's shading rather than on top of the frame.
  //
  // They are drawn here rather than back with the subtitles because they are not
  // content: they are offers, and an offer that the chrome shades is an offer the
  // chrome looks like it owns. The subtitle line stays where it was — it IS
  // content, and it is lifted clear of this band anyway.
  // FIRST, so every control registered after it wins: a click on the picture.
  pointer_zone_click(0, 0, NV_SCREEN_W, NV_SCREEN_H, clickPicture, 0, 0);
  drawActionsEpisode();
  drawQuickSeek(q);
  if (ab <= 0.005f) return;   // playing clean: nothing more over the image

  // The whole block slides together: title, bar and icons are ONE object that rises.
  // Animating each line on its own produces a staggering the device does not have.
  // The slide follows BOTH things: the OSD appearing/disappearing (`anim`) and the
  // SCREEN opening (`entry`). Before, only the first came in here, so opening the
  // player was a plain fade — the controls were born in their final place, only
  // transparent. With the opening sliding too, the block comes in from below and the
  // screen stops "flashing" to its final state.
  //
  // The opening's curve is a deceleration (1-(1-t)^3) and not the raw spring: the
  // spring overshoots and comes back, and on a block 200px tall that bounce reads as
  // a wobble.
  float eEnt  = 1.0f - (1.0f - entry) * (1.0f - entry) * (1.0f - entry);
  // The quick seek holds the block up too, so the bar it shows sits where the
  // full transport will put it if the controls come up over it.
  float lift = anim > quickAnim ? anim : quickAnim;
  float slideDown = (1.0f - lift) * PLR_SLIDE
              + (1.0f - eEnt) * PLR_SLIDE * 1.8f;

  // Anchored from the bottom up, in the order of the web app's
  // .player-controls-bottom column read backwards: the button row rests against the
  // bottom inset, the bar --player-transport-row-gap (36) above it and the title
  // block --player-transport-gap (40) above the bar.
  //
  // The bar's baseline is fixed at its RESTING height and the focused track grows
  // downwards from it, into the gap. Centring the growth would move the title with
  // it, which is anchored above; growing upwards would do the same. Only the 4px it
  // gains eat into a 36px gap, so the row never crowds.
  float yRowTop = NV_SCREEN_H - PLR_PAD_BOTTOM - PLR_BTN_D + slideDown;
  float cyButtons = yRowTop + PLR_BTN_D * 0.5f;
  float yBar   = yRowTop - PLR_GAP_ROW - PLR_RAIL_H;
  // THE QUICK SEEK'S LAYOUT is lower: the bar on the bottom inset and the time
  // above it, so the pair stays out of the band the subtitles use.
  // `full` (above) slides the bar and time between the two.
  yBar += (NV_SCREEN_H - PLR_PAD_BOTTOM - PLR_RAIL_H + slideDown - yBar) * (1.0f - full);

  // --- the SCRUBBER ---------------------------------------------------------
  // .player-progress-shell, from the transport block at the end of components.css.
  //
  // It was edge to edge, square-cornered, 4px, and its left end lined up with
  // nothing. In the web app it is an item of the overlay's padding box like
  // everything else in the block: it starts exactly where the play button's circle
  // starts and ends where the time readout ends, and that shared left edge is most
  // of what makes the footer read as ONE control rather than a bar with a row of
  // coins beneath it. Flush to the screen it read as the video's edge — which is a
  // defensible thing for a bar to be, but it is not this app's bar.
  float bx = 0.0f, bw = NV_SCREEN_W;
  // A SAFE MARGIN for everything in the footer — title, bar, buttons and clock.
  //
  // 96 is the same side margin as the title page's (NV_DETP_X, the web app's
  // --tv-safe-gutter-width), so the player stops being the only place in the app
  // with an edge rule of its own. It stays a local constant because player.c does
  // not include detail.h — and should not, just for one number.
  float cx = bx + PLR_MARGIN;
  float cw = bw - PLR_MARGIN * 2.0f;
  // While a burst is in flight the bar draws the TARGET, not playback — playback
  // carries on underneath and gets its own tick below (.player-seek-origin). It is
  // the web app's split exactly: `effectiveProgressSeconds` for the fill,
  // `current` for the origin.
  float shown = seekActive ? seekPreview : posSeg;
  float frac = durationSeg > 0.0f ? anim_clamp(shown / durationSeg, 0.0f, 1.0f) : 0.0f;
  // Focus is a spring now, not a switch: the track eases between 8 and 12 and the
  // ground between 0.18 and 0.26, which is the sheet's
  // `transition: height 180ms cubic-bezier(0.22, 1, 0.36, 1)`.
  float fBar  = focusBarAnim;
  float hRail = PLR_RAIL_H + (PLR_RAIL_H_FOCUS - PLR_RAIL_H) * fBar;
  // 0.26 -> 0.34, above the sheet's 0.18 -> 0.26. The web app's track is read
  // through a browser's own compositing over an <video> element; here it sits on a
  // hardware plane the GL surface only punches a hole in, and the same white at the
  // same alpha came back visibly thinner — "the progress bar almost seems
  // transparent". The scrim fix under it does most of the work; this closes the
  // rest, and the focused step keeps the same 0.08 span so the two states stay as
  // far apart as the sheet has them.
  float aRail = 0.26f + (0.34f - 0.26f) * fBar;
  barX = cx; barW = cw;
  nRailCut = 0;
  if (durationSeg > 0.0f) {
    IntroChunk ch[3];
    int n = intro_chunks(ch, 3);
    for (int i = 0; i < n; i++) {
      if (ch[i].kind != INTRO_OPENING) continue;
      double edge[2] = { ch[i].start, ch[i].end };
      // A cut within a gap of either end would leave a sliver; an intro that
      // starts at 0:00 only needs its end marked.
      for (int k = 0; k < 2; k++) {
        float x = cx + cw * anim_clamp((float)edge[k] / durationSeg, 0.0f, 1.0f);
        if (x > cx + PLR_RAIL_GAP * 2.0f && x < cx + cw - PLR_RAIL_GAP * 2.0f &&
            (!nRailCut || x > railCut[nRailCut - 1] + PLR_RAIL_GAP * 2.0f))
          railCut[nRailCut++] = x;
      }
      break;
    }
  }
  GfxRect rail = { cx, yBar, cw, hRail };
  railLayer(rail, 1, 1, 1, aRail * ab);
  // Taller than the rail, which is a few pixels: a pointer held in the air has to
  // be able to land on it.
  if (a > 0.3f)
    pointer_zone_act(cx, yBar + hRail * 0.5f - PLR_BAR_HIT_H * 0.5f, cw, PLR_BAR_HIT_H,
                     pointBar, clickBar, 0, 0);
  // The pipeline's buffer: what is decoded ahead of the playhead. In the web app
  // (.player-progress-buffered) it is WHITE at 0.3 and it runs from ZERO, under the
  // fill — not a segment starting where the fill ends, which is what was here. The
  // difference shows the moment the buffer falls BEHIND the playhead on a stalling
  // source: as a segment it simply vanished; as a layer the fill overruns it, which
  // is the thing you want to be able to see.
  { float bufFrac = durationSeg > 0.0f ? anim_clamp(video_buffer_end() / durationSeg, 0.0f, 1.0f) : 0.0f;
    float bwid = cw * bufFrac;
    if (bwid > 0.5f)
      railLayer((GfxRect){ cx, yBar, bwid, hRail }, 1, 1, 1, 0.30f * ab); }
  // Half a pixel already counts: with the test at 1.0 the start of the film drew
  // nothing, and the bar seemed only to start moving after a while.
  // The fill and the playhead take the Settings colour (Playback -> Seek bar
  // colour); the track and the buffer stay neutral white, so only what has been
  // played carries it.
  { float fwid = cw * frac, sr, sg, sb;
    settings_seek_color(&sr, &sg, &sb);
    if (fwid > 0.5f)
      railLayer((GfxRect){ cx, yBar, fwid, hRail }, sr, sg, sb, ab);
    // THE PLAYHEAD. `transform: scale(0)` at rest and `scale(1)` when the shell has
    // focus, so the resting bar stays a hairline and the knob is what says the bar
    // is now the thing LEFT and RIGHT are driving. Centred on the track's middle,
    // which moves as the track grows.
    { float d = PLR_RAIL_KNOB * fBar;
      if (d > 0.5f)
        gfx_color((GfxRect){ cx + fwid - d * 0.5f, yBar + hRail * 0.5f - d * 0.5f, d, d },
                  0.5f, sr, sg, sb, ab); } }
  // WHERE PLAYBACK STILL IS (.player-seek-origin): a 4px tick at the position the
  // film is actually at while you aim somewhere else. Without it a long hold gives
  // you a bar full of numbers and no way to tell how far you have strayed from
  // where you were — which is the question you are answering when you decide
  // whether to commit or press back the other way.
  if (seekActive && durationSeg > 0.0f) {
    float of = anim_clamp(posSeg / durationSeg, 0.0f, 1.0f);
    float oh = 22.0f, ow = 4.0f;
    GfxRect t = { cx + cw * of - ow * 0.5f, yBar + hRail * 0.5f - oh * 0.5f, ow, oh };
    gfx_color(t, 0.5f, 1, 1, 1, 0.85f * ab);
  }
  // The hover MARKER, 3px and 8px taller than the focused track, on the true
  // pointer position. Its bubble is drawn with the time readout below, last, so
  // it covers the title block rather than sitting under it.
  float hoverA = hoverAnim * ab;
  if (hoverA > 0.004f) {
    float mw = 3.0f, mh = PLR_RAIL_H_FOCUS + 8.0f;
    GfxRect m = { cx + cw * hoverFrac - mw * 0.5f, yBar + hRail * 0.5f - mh * 0.5f, mw, mh };
    gfx_color(m, 0.5f, 1, 1, 1, hoverA);
  }

  // A film: the name only. A series: the name, then the episode line.
  float yMetaBase = yBar - PLR_GAP_BAR;

  // THE STREAM'S FACTS, right-aligned above the bar's far end — over the time
  // readout, not under the title.
  //
  // Under the title they made a third line in the block that names what is on
  // screen, and "4K · Dolby Vision" is not part of the name: it read as a second
  // episode subtitle. At the right end it sits on the same baseline band as the
  // title block's last line, on the side of the frame that holds the other
  // readings about the file (the clock, the time), and the left edge stays the
  // title's alone.
  { char facts[80];
    // Only with a picture: until then video_width() and the HDR flags still
    // describe the PREVIOUS playback, and the line announced its 4K/Dolby Vision
    // over a source that had not opened yet. Not while the next-episode card is
    // up either: it sits in that same corner.
    if (player_has_video() && !nextCardUp() && streamFacts(facts, sizeof facts) > 0) {
      TxtLine lf = txt_line_trim(TXT_PLR_META3, facts, 255, 255, 255, 255, cw * .30f);
      // 0.72, not the sheet's 0.50: this is the line the owner reads to know
      // whether they got the good version, and at half white it was too faint.
      txt_draw_alpha(lf, cx + cw - lf.w, yMetaBase - lf.h, a * 0.72f);
    } }

  // THE EPISODE LINE, in two weights: "S1 E3" Bold at half white, then the name
  // Medium and near white.
  //
  // It was "S1E3 · Name" as one grey 32px string, which gave the code and the name
  // the same voice and made the line read as a filename under the title. Split, the
  // code becomes the label and the name is what the eye lands on — the same
  // division the time readout makes between the elapsed time and its " / total".
  // The spaced "S1 E3" is easier to read at distance than the run-together "S1E3".
  if (epT > 0) {
    char code[24];
    const char *name = strstr(lineEp, " \xc2\xb7 ");
    name = name ? name + 4 : "";
    snprintf(code, sizeof code, "S%d E%d", epT, epE);
    { TxtLine lc = txt_line(TXT_PLR_EPCODE, code, 255, 255, 255, 255);
      float gap = name[0] ? 16.0f : 0.0f;
      TxtLine ln = name[0] ? txt_line_trim(TXT_PLR_EPNAME, name, 255, 255, 255, 255,
                                           cw * .67f - lc.w - gap)
                           : (TxtLine){ 0 };
      float h = (float)(lc.h > ln.h ? lc.h : ln.h);
      yMetaBase -= h;
      txt_draw_alpha(lc, cx, yMetaBase, a * 0.55f);
      if (ln.tex) txt_draw_alpha(ln, cx + lc.w + gap, yMetaBase, a * 0.92f);
      yMetaBase -= PLR_META_GAP; }
  } else if (trailerMode) {
    // The trailer's own name where a series puts its episode, in the same voice.
    TxtLine ln = txt_line_trim(TXT_PLR_EPNAME, trailerName, 255, 255, 255, 255,
                               cw * .67f);
    yMetaBase -= ln.h;
    txt_draw_alpha(ln, cx, yMetaBase, a * 0.92f);
    yMetaBase -= PLR_META_GAP;
  }

  // THE FILM'S NAME, IN TEXT. Here the player used to prefer the title's LOGO when
  // there was one, and fell back to text only in its absence. Two things went wrong:
  // the logo has a height and aspect ratio of its own, so the block jumped from title
  // to title; and when TMDB delivered the dark variant the name disappeared over the
  // scene. The owner asked directly: "the film title that shows in the player can go
  // back to being written like it was... just the film's name".
  //
  // Text is also what the rest of the screen uses (the clock, the time, the badges),
  // so the corner now has ONE grammar.
  float hTitle, yTitle;
  { const char *name = (c && c->title[0]) ? c->title : "Playing";
    TxtLine lt = txt_line_trim(TXT_PLR_TITLE, name, 255, 255, 255, 255,
                                  cw * 0.62f);
    hTitle = (float)lt.h;
    yTitle = yMetaBase - hTitle;
    txt_draw_alpha(lt, cx, yTitle, a); }

  // --- the BUTTON row: the device's transport ------------------------------
  // .player-controls-row is space-between: the group of buttons on the LEFT, 14px
  // apart, and the time readout pushed to the right end by margin-left:auto. It is
  // not the Apple app's centred transport, and there are no redundant jump buttons
  // — the arrows on the bar are what seeks.
  {
    int row[PLR_NBTNS], n = rowButtons(row);
    float step = PLR_BTN_D + PLR_BTN_GAP;
    // The row's left edge is the BAR's left edge; the first circle is centred half
    // a diameter in from it, so its rim and the bar's end line up.
    float x0   = cx + PLR_BTN_D * 0.5f;
    for (int i = 0; i < n; i++) {
      int act = row[i];
      float bcx = x0 + i * step;
      float f = focusB[act];
      // The glyph crosses with the puck under it: white on the picture, black on
      // the white circle, both on the same 180ms the circle fades in on. Switching
      // it on a boolean left one frame of white-on-white at the crossing.
      float luma = 0.94f + (0.13f - 0.94f) * f;
      if (act == PLR_PLAY && player_loading()) {
        drawStartingButton(bcx, cyButtons, now, a);
        continue;
      }
      buttonCircle(bcx, cyButtons, f, a);
      if (a > 0.3f)
        pointer_zone(bcx - PLR_BTN_D * 0.5f, cyButtons - PLR_BTN_D * 0.5f,
                     PLR_BTN_D, PLR_BTN_D, pointButton, act, 0);
      switch (act) {
        case PLR_PLAY:     iconPlayPause(bcx, cyButtons, a, playing, luma); break;
        case PLR_CC:       iconSubtitles(bcx, cyButtons, a, luma); break;
        case PLR_AUDIO:    iconAudio(bcx, cyButtons, a, luma); break;
        case PLR_NEXT:     iconFile(bcx, cyButtons, a, luma, "skip_next", PLR_ICON_H); break;
        case PLR_EPISODES: iconFile(bcx, cyButtons, a, luma, "episodes", PLR_ICON_H); break;
        // Phosphor's stack in BOLD, the weight of every other glyph on this row,
        // and like them it does not fill on focus: the puck is the focus.
        case PLR_SOURCES:  iconFile(bcx, cyButtons, a, luma, "stack", PLR_ICON_H); break;
        case PLR_STATS:    iconFile(bcx, cyButtons, a, luma, "stats", PLR_ICON_H); break;
        case PLR_DETAILS:  iconFile(bcx, cyButtons, a, luma, "details", PLR_ICON_H); break;
        default:           iconAspect(bcx, cyButtons, a, luma); break;   // PLR_ASPECT
      }
    }
    // The label under the focused button (.player-control-btn::after): 20/bold at
    // 92% white, 16px below the circle, and it FADES with the focus rather than
    // appearing the instant the index changes — the sheet transitions its opacity
    // and its 4px rise together, and stepping it read as a caption flicking between
    // buttons while the puck slid.
    //
    // Its wording follows the web app's `title`, which is the play button's live
    // state and not a slash-joined pair: "Pause" while it plays, "Play" when it
    // does not.
    { int slot = rowSlot(row, n, button);
      float f = slot >= 0 ? focusB[button] : 0.0f;
      if (f > 0.004f) {
        static const char *NAMES[PLR_NBTNS] = {
          "Play", "Next episode", "Subtitles", "Audio", "Episodes", "Sources",
          "Aspect Ratio", "Stream stats", "Details" };
        // While the source opens, play has nothing to toggle yet: the design
        // labels it "Starting…" until there is a picture.
        const char *name = button != PLR_PLAY ? NAMES[button]
                         : player_loading() ? "Starting\xe2\x80\xa6"
                         : playing ? "Pause" : "Play";
        TxtLine label = txt_line(TXT_PLR_TIP, name, 255, 255, 255, 255);
        float lx = x0 + slot * step - label.w * 0.5f;
        float ly = cyButtons + PLR_BTN_D * 0.5f + PLR_BTN_TIP + (1.0f - f) * 4.0f;
        // `text-shadow: 0 2px 8px rgba(0,0,0,0.8)`. A blur is not available here,
        // so it is a black copy 2px down at the sheet's own 0.8 — the same way the
        // subtitle overlay fakes its outline a few functions up. It earns its place
        // even over the bottom scrim: the label is the ONE piece of the transport
        // that hangs below the gradient's densest part, where a bright frame edge
        // can still take a 20px word with it.
        { TxtLine sh = txt_line(TXT_PLR_TIP, name, 0, 0, 0, 255);
          txt_draw_alpha(sh, lx, ly + 2.0f, a * 0.80f * f); }
        txt_draw_alpha(label, lx, ly, a * 0.92f * f);
      } }

    // --- the time readout, at the right-hand end of the same row -------------
    // "3:00 / 2:22:33", like the web app's #playerTimeLabel — but in TWO weights.
    //
    // The sheet sets the whole label at 600. On the device that is one grey block
    // of digits: the elapsed time is the number you actually read, and the
    // duration is only the thing it is measured against, so they are split here —
    // Bold for the elapsed, Medium at a lower alpha for the " / total" tail. One
    // step of weight AND one of opacity, because at this size on a shaded frame
    // either alone is too subtle to register from the sofa.
    //
    // Both halves are drawn from ONE baseline (the elapsed line's), not centred
    // independently: the two rasters can differ by a pixel in height, and centring
    // each on its own would sit the tail a pixel off the number it follows.
    {
      char t1[24], t2[24], tail[32], pill[24];
      float right = cx + cw;
      float ty;
      TxtLine le, lt;
      // The elapsed half reads the TARGET while aiming — the web app's
      // `formatTime(effectiveProgressSeconds)`. The number under your thumb has to
      // be the one you are moving, or the pill is the only feedback you get.
      fmtTime(t1, sizeof t1, seekActive ? seekPreview : posSeg, 0);
      fmtTime(t2, sizeof t2, durationSeg, 0);
      snprintf(tail, sizeof tail, " / %s", t2);

      // THE SEEK PILL, drawn first because it owns the right edge and the readout
      // is laid out to its left — the same order as the web app's flex row, where
      // .player-time-label's margin-left:auto pushes the pair over together.
      //
      // WHAT IT SHOWS is the burst's accumulated jump, formatted like the web's
      // formatSeekDelta: a sign, then the same m:ss the rest of the transport uses.
      // The sign is U+2212 MINUS and not a hyphen, which is what that function
      // emits — a hyphen next to tabular digits reads as a dash in the text.
      pill[0] = 0;
      { float delta = seekPreview - posSeg;
        float pa = seekActive ? 1.0f
                 : (seekEndAt && now - seekEndAt < PLR_DELTA_FADE_MS)
                   ? 1.0f - (float)(now - seekEndAt) / (float)PLR_DELTA_FADE_MS
                   : 0.0f;
        int rounded = (int)(delta >= 0.0f ? delta + 0.5f : delta - 0.5f);
        // Empty at zero, like formatSeekDelta: seeking ten seconds forward and ten
        // back has moved you nowhere, and "+0:00" is a worse answer than silence.
        // The quick seek has its own readout at the side; the pill is the bar's.
        if (a > 0.005f && pa > 0.004f && rounded != 0) {
          char mag[24];
          fmtTime(mag, sizeof mag, (float)(rounded < 0 ? -rounded : rounded), 0);
          snprintf(pill, sizeof pill, "%s%s",
                   rounded > 0 ? "+" : "\xe2\x88\x92", mag);
          { TxtLine lp = txt_line(TXT_PLR_DELTA, pill, 255, 255, 255, 255);
            float pw = (float)lp.w + PLR_PILL_PADX * 2.0f;
            float ph = (float)lp.h + PLR_PILL_PADY * 2.0f;
            if (pw < PLR_PILL_MINW) pw = PLR_PILL_MINW;
            { GfxRect r = { right - pw, cyButtons - ph * 0.5f, pw, ph };
              gfx_color(r, 0.5f, 1, 1, 1, 0.14f * a * pa);
              txt_draw_alpha(lp, r.x + (pw - (float)lp.w) * 0.5f,
                                 r.y + (ph - (float)lp.h) * 0.5f, a * pa); }
            right -= pw + PLR_ROW_GAP; }
        }
      }

      le = txt_line(TXT_PLR_TIME,   t1,   255, 255, 255, 255);
      lt = txt_line(TXT_PLR_TIME_T, tail, 255, 255, 255, 255);
      ty = cyButtons - (float)le.h * 0.5f;
      ty += (yBar - PLR_QUICK_TIME_GAP - (float)le.h - ty) * (1.0f - full);
      txt_draw_alpha(lt, right - lt.w,        ty, ab * 0.60f);
      txt_draw_alpha(le, right - lt.w - le.w, ty, ab * 0.96f);
    }

    // THE HOVER BUBBLE (.player-progress-hover-time): 20px above the bar's middle,
    // near-black at 0.92, radius 8. Centred on the marker but held inside the
    // bar's span, so it never hangs off either end while the marker still does.
    if (hoverA > 0.004f) {
      char ht[24];
      fmtTime(ht, sizeof ht, hoverFrac * durationSeg, 0);
      { TxtLine lh = txt_line(TXT_PLR_DELTA, ht, 255, 255, 255, 255);
        float pw = (float)lh.w + 24.0f, ph = (float)lh.h + 12.0f;
        float px = anim_clamp(cx + cw * hoverFrac - pw * 0.5f, cx, cx + cw - pw);
        GfxRect r = { px, yBar + hRail * 0.5f - 20.0f - ph, pw, ph };
        gfx_color(r, 8.0f / ph, 10.0f / 255.0f, 12.0f / 255.0f, 16.0f / 255.0f, 0.92f * hoverA);
        txt_draw_alpha(lh, r.x + 12.0f, r.y + (ph - (float)lh.h) * 0.5f, hoverA); }
    }
  }

  // --- the CLOCK cluster, top right ----------------------------------------
  // .player-controls-top: a right-aligned column, clock over "Ends at", 8px apart.
  //
  //   .player-clock    36 / 600, full white
  //   .player-ends-at  24 / 500, white 55%
  //
  // The sheet's own note explains the pairing: the clock "keeps the larger size it
  // gained ... the size difference is what separates the two lines now that the
  // hairline divider is gone". At the old 26/20 with a 2px gap the two sat too
  // close in both size and weight to separate at all — they read as one grey smudge
  // in the corner, which is what "out of line" meant.
  //
  // ALIGNED WITH THE FOOTER, not with the screen. This cluster was the one thing in
  // the overlay still measured from PLR_PAD_X (64) while the title, the bar and the
  // time readout all sit at PLR_MARGIN (96) — so its right edge overhung theirs by
  // 32px, and the corner looked knocked out of true against the time directly below
  // it. One margin for everything in the overlay; the web app has exactly one too.
  {
    time_t nowT = time(NULL);
    struct tm lt;
    char hora[8], end[32];
    float yRel = PLR_PAD_BOTTOM + slideDown;
    float right = NV_SCREEN_W - PLR_MARGIN;
    localtime_r(&nowT, &lt);
    strftime(hora, sizeof hora, "%H:%M", &lt);
    { double missing = durationSeg - posSeg;
      time_t t2 = nowT + (time_t)(missing > 0.0 ? missing : 0.0);
      struct tm lf; char h2[8];
      localtime_r(&t2, &lf);
      strftime(h2, sizeof h2, "%H:%M", &lf);
      snprintf(end, sizeof end, "Ends at %s", h2); }
    { TxtLine lh = txt_line(TXT_PG_CLOCK, hora, 255, 255, 255, 255);
      TxtLine lf = txt_line(TXT_PLR_ENDS, end, 255, 255, 255, 255);
      txt_draw_alpha(lh, right - lh.w, yRel, a);
      txt_draw_alpha(lf, right - lf.w, yRel + lh.h + PLR_META_GAP, a * 0.55f); }
  }

  // There used to be an age-rating badge here with the title's GENRE beside it, which
  // does not exist in the web app — a genre is not a content warning, and "Drama"
  // inside an orange badge reads as a warning. The web app shows up to five
  // "Category · Severity" lines from IMDb's parental guide, with a vertical 6px bar
  // in the accent colour flush left.
  //
  //   .player-parental-guide  left 64, top 48
  //   .player-parental-line   6 wide, radius 3, height = the list's
  //   .player-parental-list   padding-left 20, gap 4
  //   .player-parental-item   36 tall
  //   label 22/600 white 85% · separator 22/400 white 40% ·
  //   severity 22/400 white 50%
  //
  // A CLOCK OF ITS OWN, and not the OSD's alpha. This guide is an OPENING WARNING: it
  // says what the film contains before the scene starts to matter. Tied to the OSD it
  // reappeared every time the owner touched the remote, mid-film, when the
  // information is no longer any use — "it should only appear animated at the start of
  // the film and then never again".
  //
  // It counts from startImage (the first frame with a picture, not the screen's
  // opening): it comes in staggered line by line, stays PG_SEG_TOTAL and leaves. After
  // that it does not come back during this playback.
  {
    int np = parental_n();
    float tg = startImage ? (float)(now - startImage) / 1000.0f : -1.0f;
    if (np > 0 && tg >= 0.0f && tg < PG_SEG_TOTAL) {
      float output = anim_clamp((PG_SEG_TOTAL - tg) / PG_SEG_OUTPUT, 0.0f, 1.0f);
      float lin = PG_LINE_H, gap = PG_LINE_GAP;
      float height = np * lin + (np - 1) * gap;
      float y0 = PLR_PAD_Y;
      // The bar only grows once the first line has come in, otherwise it appears on
      // its own pointing at nothing.
      float eB = anim_clamp((tg - 0.10f) / 0.34f, 0.0f, 1.0f);
      eB = 1.0f - (1.0f - eB) * (1.0f - eB);
      { GfxRect bar = { PLR_PAD_X, y0, PG_BAR_W, height * eB };
        if (eB > 0.01f)
          gfx_color(bar, 0.5f * (PG_BAR_W / (height * eB)),
                  PLR_FILL_C, PLR_FILL_C, PLR_FILL_C, entry * output); }
      float xt = PLR_PAD_X + PG_BAR_W + PG_LIST_PADX;
      for (int i = 0; i < np; i++) {
        float yl = y0 + i * (lin + gap);
        float ts = anim_clamp((tg - 0.18f - i * 0.10f) / 0.30f, 0.0f, 1.0f);
        float ee = 1.0f - (1.0f - ts) * (1.0f - ts);   // desaceleracao
        float ag = entry * output * ee;
        float dx = (1.0f - ee) * 18.0f;                // it slides in from the left
        TxtLine lr, ls, lg;
        float cy, x;
        if (ag <= 0.004f) continue;
        lr = txt_line(TXT_PLR_PG_CAT, parental_label(i), 255, 255, 255, 255);
        ls = txt_line(TXT_PG_SEV, "\xc2\xb7", 255, 255, 255, 255);
        lg = txt_line(TXT_PG_SEV, parental_severity(i), 255, 255, 255, 255);
        cy = yl + (lin - lr.h) * 0.5f;
        x  = xt - dx;
        txt_draw_alpha(lr, x, cy, ag * 0.94f);  x += lr.w + PG_SEP_GAP;
        txt_draw_alpha(ls, x, yl + (lin - ls.h) * 0.5f, ag * 0.35f); x += ls.w + PG_SEP_GAP;
        txt_draw_alpha(lg, x, yl + (lin - lg.h) * 0.5f, ag * 0.62f);
      }
    }
  }
}
