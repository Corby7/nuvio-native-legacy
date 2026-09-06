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
#include "video.h"
#include "tracks.h"
#include "gfx.h"
#include "text.h"
#include "tex_cache.h"
#include "anim.h"
#include "layout.h"
#include "catalog.h"
#include "trakt.h"
#include "sync.h"
#include "parental.h"
#include "episodes.h"
#include "streams.h"
#include "subtitle.h"
#include "intro.h"
#include "home.h"
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
// THE WEB APP'S PLAYER MEASUREMENTS
//
// This screen no longer follows the Apple app's player: it follows our web app,
// which is this legacy variant's reference. The values are the CSS's resolved at
// 1920x1080, which is where the app runs — in the file they are min(Xvw, Ypx) and
// the TV always hits the ceiling. Each one's origin is recorded so it can be checked.
//
//   #playerUiRoot        --player-controls-x/y      64 / 48
//   .player-control-btn  --player-control-size      96   (gap 4px)
//   .player-progress-track  height 6 -> 10 with focus, radius 3
//   .player-progress-shell  margin-top 12
//   .player-controls-row    margin-top 16
//   .player-controls-gradient-top/bottom   150 / 200
//
// BUT THOSE ARE THE BASE VALUES, AND NOT THIS SCREEN'S. The `#playerUiRoot` block
// (components.css:15251) is the port of the Android TV player and redoes almost all
// of them with the x2 conversion the repository uses for the 1920 canvas ("ATV 6dp
// -> 12px"). What was here was half the right size on almost everything — the bar,
// the buttons' gap, the row's breathing room and both gradients. The ones the ATV
// block does NOT redo (padding 64/48, the bar's margin-top 12) stay as they are.
//
//   .player-progress-track  12 -> 20 with focus, radius 6
//   .player-control-buttons gap 8
//   .player-controls-row    margin-top 32
//   .player-control-icon    48
//   gradients               300 (top) / 400 (base)
#define PLR_PAD_X         64.0f
#define PLR_PAD_Y         48.0f
// The side margin of the footer's CONTENT (title, buttons, clock). The bar's track
// still runs 0..width; only the content is inset, so as not to fall in the zone the
// TV cuts by overscan. The same value as the title page's gutter.
#define PLR_MARGIN        96.0f
#define PLR_BTN_D         76.0f
#define PLR_BTN_GAP        8.0f
// 12px at rest, 20px with focus — both from the ATV block. The bar HAS STARTED
// taking focus (UP from the button row); before, only the buttons did, and that is
// why there was no way to seek through the film with the bar.
// A MINIMAL BAR, EDGE TO EDGE. It was 12px tall with a 64px margin on each side and
// radius 6 — and the radius was the defect: in this API it is a FRACTION of the
// smaller side (see gfx.h), at most 0.5, so 6.0 degenerated the SDF. The effect was
// the initial fill becoming a bubble instead of a bar growing, and only "appearing"
// after many minutes of film, once it was wide enough for the shape to resolve. It
// is what the owner described: "it takes ages to show it filling, it isn't well
// calibrated".
//
// Now it is a straight hairline with square corners (radius 0), flush with the
// screen's edges. With no radius there is no SDF to degenerate and the first pixel
// of progress appears at once.
#define PLR_RAIL_H       4.0f
// 20px with focus (`min(1.04vw, 20px)` in .player-progress-shell.focused).
#define PLR_RAIL_H_FOCUS  8.0f
#define PLR_RAIL_R       0.0f   // a square corner: see the note above
#define PLR_GAP_BAR     12.0f   // meta -> bar
#define PLR_GAP_ROW       32.0f   // bar -> button row
#define PLR_GRADIENT_BOTTOM   400.0f
#define PLR_GRADIENT_TOP    300.0f
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
#define PG_LINE_H        36.0f
#define PG_LINE_GAP       4.0f
// The veil became the web app's two gradients (PLR_GRADIENT_TOP/BOTTOM). It exists
// so the text reads over the image — without it, a bright scene wipes out the
// title's name.

// Compact transport. The jumps are still reachable through the arrows on the bar.
enum { PLR_PLAY, PLR_ASPECT, PLR_CC, PLR_AUDIO,
       PLR_SOURCES, PLR_EPISODES, PLR_NBTNS };

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
static int   visible = 0;          // the controls' target (1 = up)
static float anim = 0.0f;          // 0..1 following `visible`, by spring
static float focusB[PLR_NBTNS];     // each button's focus spring
static float entry = 0.0f;       // 0..1 the screen's opening/closing fade
static Uint32 lastInput = 0;
// The instant the PICTURE started (not the screen's opening: between the two there
// is the source search, which can take seconds). Zero while there has been none.
// The parental guide relies on this to appear ONCE, at the start, and disappear.
static Uint32 startImage = 0;
// THE TWO MEDIA VARIABLES. All the rest of the file reads only from here — when the
// real video comes in, they are the ones the decoder starts filling.
static int   hasVideo = 0;
static int   reqTracks = 0;
static int   waitingSource = 0;   // opened with no URL, waiting for the addon to answer
static float posSeg = 0.0f;
static float durationSeg = PLR_DURATION_DEFAULT;

static char lineEp[220];          // "T1, E1 · <sinopse curta>", montada na abertura

static const CatItem *item(void) { return cat_item(idx); }
static int epT, epE, reqSources, errorSource, reqNextT, reqNextE;
static int introIdx=-1, introT=-1, introE=-1;
static int resumeApplied, resumePct;
int player_index(void) { return idx; }
const char *player_line_episode(void) { return lineEp; }
void player_episode_current(int *t, int *e) { *t = epT; *e = epE; }
int player_requested_sources(void) { int p = reqSources; reqSources = 0; return p; }
int player_requested_next(int *t,int *e) {
  if(!reqNextT||!reqNextE)return 0;
  if(t)*t=reqNextT;if(e)*e=reqNextE;reqNextT=reqNextE=0;return 1;
}
const CatEp *player_next_episode(void) {
  const CatEp *best=NULL;
  for(int i=0;i<cat_n_episodes(idx);i++) {
    const CatEp *p=cat_episode(idx,i);if(!p)continue;
    if(p->season<epT||(p->season==epT&&p->episode<=epE))continue;
    if(!best||p->season<best->season||
       (p->season==best->season&&p->episode<best->episode))best=p;
  }
  return best;
}
void player_error_source(void) { waitingSource = 0; errorSource = 1; visible = 1; playing = 0; }
void player_set_episode(int t, int e) {
  const CatItem *c = item();
  epT = t; epE = e; lineEp[0] = 0;
  resumePct = 0;
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
  toastAte = SDL_GetTicks() + PLR_TOAST_MS;
}

void player_open(int indexCatalog, const char *url) {
  int n = cat_n(); if (n < 1) n = 1;
  idx = ((indexCatalog % n) + n) % n;
  is_open = 1; exiting = 0; requestedExit = 0; barFocus = 0;
  // The title's parental guide: requested HERE and not while drawing, so the answer
  // has already arrived when the controls appear for the first time.
  { const CatItem *ci = cat_item(idx);
    if (ci && ci->imdb[0]) parental_request(ci->imdb); }
  playing = 1; visible = 1; anim = 0.0f; entry = 0.0f;
  reqSources = errorSource = reqTracks = reqNextT = reqNextE = 0; startImage = 0;
  resumeApplied=0;
  button = PLR_PLAY;
  memset(focusB, 0, sizeof focusB);
  posSeg = 0.0f;
  lastInput = SDL_GetTicks();
  waitingSource = (url == NULL);
  // An external subtitle belongs to the session that has just ended, not to this one.
  tracks_reset();
  // The aspect mode belongs to the DEVICE, not to the session: rereading here is
  // what makes "Cinema zoom" still apply on the next film, as in the web app.
  prefsRead();
  toastAte = 0;
  hasVideo = (url && *url && video_play(url));
  applyAspect();

  const CatItem *c = item();
  float d = c ? durationOfMeta(c->meta) : 0.0f;
  durationSeg = d > 1.0f ? d : PLR_DURATION_DEFAULT;

  // The episode's identity is independent of the focus in the navigation panel.
  player_set_episode(c ? c->season : 0, c ? c->episode : 0);
  if (url && *url && !hasVideo) player_error_source();
}

int player_is_open(void)    { return is_open; }
int player_wants_exit(void) { return requestedExit; }
// Only after loadCompleted. Before that the pipeline has put nothing on the hardware
// plane, and punching the surface early swapped the art for a BLACK rectangle while
// the stream opened — which was the "you press play and it goes black".
void player_set_source(const char *url) {
  if (!is_open || !url || !*url) return;
  waitingSource = 0;
  errorSource = 0;
  hasVideo = video_play(url);
  if (!hasVideo) player_error_source();
  applyAspect();
}

// Consumes the request to open the tracks sheet: whoever reads it, clears it.
int  player_requested_tracks(void) { int v = reqTracks; reqTracks = 0; return v; }

int  player_has_video(void) { return hasVideo && video_ready(); }

// The stream is opening: video has been requested, but there is no picture yet.
int  player_loading(void) { return waitingSource || (hasVideo && !video_ready()); }
int  player_controls_visible(void) { return visible; }

void player_shutdown(void) {
  // Save BEFORE stopping: video_stop unloads the pipeline and the position goes with
  // it. A title almost at the end counts as watched in full — going back to a card
  // saying "2 min left" when it has actually finished is worse than rounding.
  if (hasVideo && video_ready() && durationSeg > 1.0f) {
    float pos = posSeg >= durationSeg - 60.0f ? durationSeg : posSeg;
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
      // And to the ACCOUNT. Trakt and the account are two different destinations: not
      // every user turns Trakt on, and the official app's progress comes from the account.
      sync_dirty_progress();
    }
  }
  if (hasVideo) video_stop();
  hasVideo = 0; waitingSource = 0; is_open = 0; exiting = 0; requestedExit = 0;
  startImage = 0;
  episodes_close();
  intro_off(); introIdx=introT=introE=-1;
  subtitle_off();
}

static int offerNext(void) {
  const CatEp *p=player_next_episode();double end;int kind;
  if(!p||durationSeg<=1)return 0;
  if(intro_active(posSeg,&end,&kind)&&kind==INTRO_CREDITS)return 1;
  return durationSeg-posSeg<=120.0f;
}

// Every key wakes the controls, including one that has already carried out an
// action: on the device there is no command that happens with the bar hidden without
// bringing the bar along — the user needs to see the effect of what they pressed.
static void wake(void) { visible = 1; lastInput = SDL_GetTicks(); }

static void togglePlaying(void) {
  playing = !playing;
  if (hasVideo) video_pause(!playing);
}

// A 10s jump with a limit. It only counts with the controls up: blind, an arrow
// would be an invisible jump — with the buttons, whoever presses is looking at a
// button that says «10 / 10».
static void jump(int dir) {
  posSeg += dir * PLR_JUMP_SEG;
  posSeg = anim_clamp(posSeg, 0.0f, durationSeg);
  if (hasVideo) video_fetch(posSeg);
}

void player_event(const SDL_Event *e) {
  if (!is_open || exiting || e->type != SDL_KEYDOWN) return;
  SDL_Keycode k = e->key.keysym.sym;

  if (k == SDLK_ESCAPE || k == SDLK_AC_BACK || k == SDLK_BACKSPACE ||
      k == SDLK_DELETE) {
    exiting = 1; requestedExit = 1;
    return;
  }

  // CONTROLS HIDDEN: any direction only wakes the interface. OK straight away
  // pauses/resumes without navigating anything — it is the device's gesture: one
  // press in the centre and the video obeys, with no steps in between.
  // THE ASPECT KEY works always, with the controls up or hidden. In the web app the
  // mode is only changed through a button inside "More Actions" — two steps with a
  // cursor that does not exist here. On a TV the gesture has to be one press, and the
  // notice that rises on the change already says which mode you have entered, so the
  // key does not even need the interface open. 0 is the free key on the LG remote.
  if (k == SDLK_0 || k == SDLK_KP_0) { player_aspect_cycle(); return; }

  if (!visible) {
    if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) {
      if(offerNext()) { const CatEp*p=player_next_episode();reqNextT=p->season;reqNextE=p->episode;return; }
      { double end;int kind;if(intro_active(posSeg,&end,&kind)&&kind!=INTRO_CREDITS){
          posSeg=(float)end+.25f;if(hasVideo)video_fetch(posSeg);return; } }
      togglePlaying(); wake(); return;
    }
    if (k == SDLK_UP || k == SDLK_DOWN || k == SDLK_LEFT || k == SDLK_RIGHT)
      wake();
    return;
  }

  // CONTROLS UP: the focus moves along the buttons and OK presses the focused one.
  if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) {
    // On the bar, OK pauses/resumes: it is what is left that is useful, since the bar
    // has no action of its own in the web app.
    if (barFocus) { togglePlaying(); wake(); return; }
    switch (button) {
      case PLR_PLAY:    togglePlaying(); break;
      case PLR_ASPECT: player_aspect_cycle(); break;
      // CC and AUDIO open the SAME sheet, but on different columns: pressing
      // "subtitles" and landing on audio made the two buttons look like one.
      case PLR_CC:      reqTracks = 2;     break;   // 2 = the subtitle column
      case PLR_SOURCES:  reqSources = 1; break;
      case PLR_EPISODES: if (epT > 0) episodes_open(idx, epT, epE); break;
      default:          reqTracks = 1;     break;   // 1 = the audio column
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
    // Through the UP gesture the sheet opens on AUDIO, which is the column the hand
    // looks for most.
    if (!barFocus) barFocus = 1; else reqTracks = 1;
    wake();
    return;
  }
  if (barFocus) {
    // On the bar, LEFT and RIGHT seek through the film instead of changing button.
    if (k == SDLK_LEFT)       jump(-1);
    else if (k == SDLK_RIGHT) jump(1);
    else if (k == SDLK_DOWN)  barFocus = 0;
    wake();
    return;
  }
  if (k == SDLK_DOWN) {
    // DOWN from the row means "get the controls out of the way".
    // It does not call wake(): that would put the bar back in the same event and make
    // the command look broken. The next directional press reveals it again.
    barFocus = 0;
    visible = 0;
    lastInput = SDL_GetTicks();
    return;
  }
  // No wrap-around at the ends: the row is short and fits in a single glance; going
  // round at the end reads as an error, not as a shortcut.
  if (k == SDLK_LEFT  && button > 0)          button--;
  else if (k == SDLK_RIGHT && button < PLR_NBTNS - (epT > 0 ? 1 : 2)) button++;
  wake();
}

void player_update(float dt, Uint32 now) {
  if (!is_open) return;

  entry = anim_spring(entry, exiting ? 0.0f : 1.0f, dt, NV_SPRING_SCREEN);
  // Marks the first frame WITH A PICTURE. It is from here that the parental guide
  // counts its time — counting from the screen's opening would make the guide spend
  // its allowance while the app was still looking for a source, and it would
  // disappear before the film appeared.
  if (!startImage && hasVideo && video_ready()) { startImage = now; wake(); }
  if (exiting && entry < 0.02f) { is_open = 0; exiting = 0; entry = 0.0f; return; }

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

  if (hasVideo && video_active()) {
    double d = video_duration();
    posSeg = (float)video_pos();
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

  // Paused, the controls stay. Making them disappear would leave the user in front of
  // a still frame with no clue that it was they who paused.
  if (visible && playing && !player_loading() && !episodes_is_open() &&
      !stream_sheet_is_open() && !tracks_is_open() && now - lastInput > PLR_HIDES_MS) visible = 0;
  if (epT > 0 && !strstr(lineEp, " · ")) player_set_episode(epT, epE);

  anim = anim_spring(anim, visible ? 1.0f : 0.0f, dt,
                   visible ? NV_SPRING_FOCUS : NV_SPRING_BLUR);
  for (int i = 0; i < PLR_NBTNS; i++) {
    float target = (visible && button == i) ? 1.0f : 0.0f;
    focusB[i] = anim_spring(focusB[i], target, dt,
                         target > focusB[i] ? NV_SPRING_FOCUS : NV_SPRING_BLUR);
  }
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

static void iconPlayPause(float cx, float cy, float a, int pause, float luma) {
  iconFile(cx, cy, a, luma, pause ? "pause" : "play", PLR_ICON_H * 1.15f);
}

static void iconSubtitles(float cx, float cy, float a, float luma) {
  iconFile(cx, cy, a, luma, "subtitles", PLR_ICON_H * 1.15f);
}

static void iconAudio(float cx, float cy, float a, float luma) {
  iconFile(cx, cy, a, luma, "audio", PLR_ICON_H * 1.15f);
}

static void iconAspect(float cx, float cy, float a, float luma) {
  iconFile(cx, cy, a, luma, "aspect", PLR_ICON_H * 1.15f);
}

// A round transport button: translucent when idle, white when focused, and the glyph
// always with the right contrast against its background.
static void buttonCircle(float cx, float cy, float f, float a, int sel) {
  float d = PLR_BTN_D * (1.0f + 0.09f * f);
  GfxRect r = { cx - d * 0.5f, cy - d * 0.5f, d, d };
  if (sel) gfx_color(r, 0.5f, 0.97f, 0.97f, 0.98f, 0.96f * a);
  else     gfx_color(r, 0.5f, 0.05f, 0.05f, 0.06f, 0.42f * a);
}

static void colorSubtitle(int i,int *r,int *g,int *b){
  static const unsigned char c[VIDEO_SUB_NCOLORS][3]={
    {255,255,255},{255,222,48},{64,224,112},{78,156,255},{255,80,80},{18,18,18}};
  if(i<0||i>=VIDEO_SUB_NCOLORS)i=0;*r=c[i][0];*g=c[i][1];*b=c[i][2];
}

/* The C9's uMS limits the font and the scale. OpenSubtitles goes through this
 * SDL/GLES overlay, exactly like the web app's HTML overlay. */
static void drawSubtitleExternal(void){
  char text[768],*line,*salva;TxtLine color[4],border[4];int n=0,r,g,b;
  if(!subtitle_text(posSeg,subStyle.delayMs,text,sizeof text))return;
  int pct=subStyle.size;if(pct<50)pct=50;if(pct>200)pct=200;pct=(pct/10)*10;
  TxtStyle st=(TxtStyle)(TXT_SUB_50+(pct-50)/10);colorSubtitle(subStyle.color,&r,&g,&b);
  float alpha=(subStyle.opacity==3?.25f:subStyle.opacity==2?.5f:subStyle.opacity==1?.75f:1.f)*entry;
  line=strtok_r(text,"\n",&salva);
  while(line&&n<4){
    TxtFamily fam=(TxtFamily)subStyle.family;
    color[n]=txt_line_trim_family(st,line,r,g,b,255,1660,fam);
    border[n]=subStyle.border?txt_line_trim_family(st,line,0,0,0,255,1660,fam):(TxtLine){0};
    n++;line=strtok_r(NULL,"\n",&salva);
  }
  if(!n)return;
  float total=0;for(int i=0;i<n;i++)total+=color[i].h+(i?5:0);
  float base=visible?760.f:1000.f;
  if(offerNext())base=690.f;
  base-=(subStyle.position-3)*48.f;
  float y=base-total;
  for(int i=0;i<n;i++){
    TxtLine l=color[i];float x=(NV_SCREEN_W-l.w)*.5f;
    if(subStyle.background){float fa=subStyle.background*.16f*alpha;gfx_color((GfxRect){x-18,y-6,l.w+36,l.h+12},.16f,0,0,0,fa);}
    if(border[i].tex){float d=subStyle.border==2?4.f:2.f;
      txt_draw_alpha(border[i],x+d,y+d,.82f*alpha);
      if(subStyle.border==1){txt_draw_alpha(border[i],x-d,y,.82f*alpha);txt_draw_alpha(border[i],x,y-d,.82f*alpha);}
    }
    txt_draw_alpha(l,x,y,alpha);y+=l.h+5;
  }
}

static void drawActionsEpisode(void){
  const CatEp *next=player_next_episode();double end;int kind=0;
  int chunk=intro_active(posSeg,&end,&kind);
  if(offerNext()&&next){
    GfxRect p={420,720,1080,194};gfx_color(p,.10f,.045f,.045f,.05f,.94f*entry);
    gfx_rect(p,0,GFX_RING,0,.008f,0,.10f,1,1,1,.20f*entry);
    const char *art=next->thumb[0]?next->thumb:(item()&&item()->backdrop[0]?item()->backdrop:NULL);
    if(art){GLuint tx=tex_get_width(art,288);if(tx){gfx_tex_aspect_current=tex_aspect(art);gfx_rect((GfxRect){450,738,288,158},tx,GFX_CARD,0,0,0,.08f,1,1,1,entry);gfx_tex_aspect_current=0;}}
    TxtLine l=txt_line(TXT_PLR_BODY,"Next episode",205,207,213,255);txt_draw_alpha(l,782,752,entry);
    char name[220];snprintf(name,sizeof name,"S%dE%d · %s",next->season,next->episode,next->name);
    TxtLine t=txt_line_trim(TXT_PLR_TITLE,name,250,250,252,255,430);txt_draw_alpha(t,782,794,entry);
    GfxRect bot={1240,775,220,76};gfx_color(bot,.5f,.08f,.08f,.09f,.96f*entry);gfx_rect(bot,0,GFX_RING,0,.018f,0,.5f,1,1,1,.35f*entry);
    gfx_icon((GfxRect){1264,793,40,40},"play",1,1,1,entry);TxtLine rt=txt_line(TXT_BODY,"Play",246,246,248,255);txt_draw_alpha(rt,1310,797,entry);
  } else if(chunk&&kind!=INTRO_CREDITS){
    const char *rot=kind==INTRO_SUMMARY?"Skip recap":"Skip intro";
    TxtLine t=txt_line(TXT_BODY,rot,250,250,252,255);float w=t.w+116;
    GfxRect p={64,730,w,88};gfx_color(p,.5f,.075f,.075f,.085f,.94f*entry);
    gfx_icon((GfxRect){88,752,44,44},"forward",1,1,1,entry);txt_draw_alpha(t,148,752,entry);
  }
}

void player_draw(Uint32 now) {
  (void)now;
  if (!is_open) return;
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
    if (hole.w > 0.0f && hole.h > 0.0f) gfx_hole(hole);
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

  // An opening indicator: dots pulsing in the centre, over the darkened art.
  // A spinner would need rotation in the shader; three dots in counterphase say the
  // same thing with what already exists, and they read well from a distance.
  if (player_loading()) {
    GfxRect dark = { 0, 0, NV_SCREEN_W, NV_SCREEN_H };
    int k;
    gfx_color(dark, 0.0f, 0, 0, 0, 0.55f * entry);
    GLuint logo = c && c->logo[0] ? tex_get_width(c->logo, 520) : 0;
    if (logo) {
      float ar = tex_aspect(c->logo), w = 520, h = ar > 0 ? w / ar : 120;
      if (h > 160) { h = 160; w = h * ar; }
      gfx_rect((GfxRect){(NV_SCREEN_W-w)*.5f,NV_SCREEN_H*.5f-h-60,w,h},logo,
               tex_brand_dark(c->logo)?GFX_BRAND:GFX_TEXT,0,0,0,0,.95f,.95f,.97f,entry);
    } else {
      TxtLine t = txt_line_trim(TXT_PLR_TITLE,c?c->title:"Playing",240,241,244,255,680);
      txt_draw_alpha(t,(NV_SCREEN_W-t.w)*.5f,NV_SCREEN_H*.5f-150,entry);
    }
    // A ring with a luminous tail, animated with no new textures per frame.
    for (k = 0; k < 12; k++) {
      float ang = k * 6.2831853f / 12.0f + now * .006f;
      float br = .18f + .82f * k / 11.0f;
      GfxRect pt = {NV_SCREEN_W*.5f + cosf(ang)*24 - 4,
                    NV_SCREEN_H*.5f + sinf(ang)*24 - 4,8,8};
      gfx_color(pt,.5f,.95f,.95f,.97f,br*entry);
    }
    { TxtLine lc = txt_line(TXT_CALLOUT, "Opening source", 236, 237, 242, 255);
      txt_draw_alpha(lc, NV_SCREEN_W * 0.5f - lc.w * 0.5f,
                         NV_SCREEN_H * 0.5f + 50, 0.85f * entry); }
    if (lineEp[0]) {
      TxtLine le = txt_line_trim(TXT_PG_END,lineEp,196,198,204,255,680);
      txt_draw_alpha(le,(NV_SCREEN_W-le.w)*.5f,NV_SCREEN_H*.5f+94,entry);
    }
  }
  if (errorSource) {
    gfx_color(screen,0,.02f,.02f,.025f,.65f);
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
    TxtLine l = txt_line(TXT_PLR_TITLE, player_aspect_label(aspect),
                           243, 248, 255, 242);
    float pw = (float)l.w + 128.0f, ph = 128.0f;
    GfxRect pil = { (NV_SCREEN_W - pw) * 0.5f, 160.0f, pw, ph };
    // The radius is a FRACTION of the smaller side (see gfx.h): 0.5 is the full pill.
    gfx_color(pil, 0.5f, 9.0f / 255.0f, 13.0f / 255.0f, 20.0f / 255.0f, 0.88f * at);
    txt_draw_alpha(l, pil.x + (pw - l.w) * 0.5f,
                       pil.y + (ph - (float)l.h) * 0.5f, at);
  }

  /* They stay when the controls disappear: they are content, not player chrome. */
  drawSubtitleExternal();
  drawActionsEpisode();

  float a = anim * entry;
  if (a <= 0.005f) return;   // playing clean: nothing over the image

  // Two gradients, as in the web app: .player-controls-gradient-top (150px, 0.7 -> 0)
  // and .player-controls-gradient-bottom (200px, 0 -> 0.8). The bottom one supports
  // the title and the bar; the top one exists because the badges and the age rating
  // sit up there and without it they would disappear over a bright scene. Both follow
  // the controls' animation: fixed, they would leave a permanent shadow over every scene.
  GfxRect veil = { 0, NV_SCREEN_H - PLR_GRADIENT_BOTTOM, NV_SCREEN_W, PLR_GRADIENT_BOTTOM };
  // GFX_VEIL_BOTTOM and not GFX_VEIL: that one darkens the LEFT too (made for the
  // home's hero) and left this rectangle's top-left corner dark with its right
  // transparent — the boundary between the two read as a plate.
  gfx_rect(veil, 0, GFX_VEIL_BOTTOM, 0, 0, 0, 0.0f, 0, 0, 0, 0.86f * a);
  { GfxRect top = { 0, 0, NV_SCREEN_W, PLR_GRADIENT_TOP };
    gfx_rect(top, 0, GFX_VEIL_TOP, 0, 0, 0, 0.0f, 0, 0, 0, 0.70f * a); }

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
  float slideDown = (1.0f - anim) * PLR_SLIDE
              + (1.0f - eEnt) * PLR_SLIDE * 1.8f;

  // Anchored from the bottom up, in the order of the web app's
  // .player-controls-bottom column read backwards: the button row rests against the
  // bottom margin, the bar sits 16px above it and the meta 12px above the bar. The
  // margin is --player-controls-y (48), not the app's general margin.
  float yRowTop = NV_SCREEN_H - PLR_PAD_Y - PLR_BTN_D + slideDown;
  float cyButtons = yRowTop + PLR_BTN_D * 0.5f;
  float yBar   = yRowTop - PLR_GAP_ROW - PLR_RAIL_H;

  // --- the progress bar ---
  // The bar occupies the whole usable width, between the player's margins. With no
  // head marker: the web app has none — the bar thickens from 6 to 10px when it takes
  // focus, and that is what says it is operable. Here the focus moves only along the
  // buttons, so it stays at 6.
  // EDGE TO EDGE: it touches both edges of the screen. With a margin it read as a
  // component floating in the middle of the footer; flush, it is the video's edge.
  float bx = 0.0f, bw = NV_SCREEN_W;
  // A SAFE MARGIN for the CONTENT (title, meta, buttons, clock).
  //
  // The track still runs edge to edge on purpose — flush, it reads as the video's
  // edge. What must not touch the edge is the TEXT: at x=0 it falls in the zone the
  // TV cuts by overscan, and the owner saw the title and the time cut off at both
  // edges. They are two different roles that were sharing the same x only because
  // they were born together.
  //
  // 96 is the same side margin as the title page's (NV_DETP_X, the web app's
  // --tv-safe-gutter-width), so the player stops being the only place in the app with
  // an edge rule of its own. It stays a local constant because player.c does not
  // include detail.h — and should not, just for one number.
  float cx = bx + PLR_MARGIN;
  float cw = bw - PLR_MARGIN * 2.0f;
  float frac = durationSeg > 0.0f ? anim_clamp(posSeg / durationSeg, 0.0f, 1.0f) : 0.0f;
  // With focus the track thickens from 6 to 10 and lightens from 0.30 to 0.45, and it
  // grows DOWNWARDS from the same baseline — growing upwards would move the meta and
  // the title too, which are anchored to it.
  // The track grows DOWNWARDS from the same baseline — growing upwards would move the
  // title too, which is anchored to it.
  float hRail = barFocus ? PLR_RAIL_H_FOCUS : PLR_RAIL_H;
  GfxRect rail = { bx, yBar, bw, hRail };
  GfxRect traveled = { bx, yBar, bw * frac, hRail };
  gfx_color(rail, PLR_RAIL_R, 1, 1, 1, (barFocus ? 0.34f : 0.22f) * a);
  // The pipeline's buffer, between what has been played and the end: it is what shows
  // the video is ahead of the clock. With no data from the pipeline the segment does
  // not exist — inventing "almost all loaded" would be worse than the plain bar. In
  // the web app it is the SAME colour as the fill at 0.35 (.player-progress-buffered).
  { float bufFrac = durationSeg > 0.0f ? anim_clamp(video_buffer_end() / durationSeg, 0.0f, 1.0f) : 0.0f;
    if (bufFrac > frac + 0.004f) {
      GfxRect buf = { bx + bw * frac, yBar, bw * (bufFrac - frac), hRail };
      gfx_color(buf, PLR_RAIL_R, PLR_FILL_C, PLR_FILL_C, PLR_FILL_C, 0.35f * a);
    } }
  // Half a pixel already counts: with the test at 1.0 the start of the film drew
  // nothing, and the bar seemed only to start moving after a while.
  if (traveled.w > 0.5f)
    gfx_color(traveled, PLR_RAIL_R, PLR_FILL_C, PLR_FILL_C, PLR_FILL_C, a);

  // A film: the name only. A series: the name followed by S/E and the episode's title.
  // The file and the provider belong to the sources sheet, not to the transport.
  float yMetaBase = yBar - PLR_GAP_BAR;
  if (lineEp[0]) {
    TxtLine le=txt_line_trim(TXT_PLR_BODY,lineEp,218,220,224,255,cw*.67f);
    yMetaBase-=le.h;
    txt_draw_alpha(le,cx,yMetaBase,a);
    yMetaBase-=6;
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
  // No redundant jump buttons. The focus runs only through the visible actions.
  {
    // .player-controls-row is space-between: the group of buttons on the LEFT, with a
    // 4px gap between them, and the time label pushed to the right by
    // margin-left:auto. It is not the Apple app's centred transport.
    float step = PLR_BTN_D + PLR_BTN_GAP;
    float x0    = cx + PLR_BTN_D * 0.5f;
    float cxs[PLR_NBTNS];
    for (int i=0;i<PLR_NBTNS;i++) cxs[i]=x0+i*step;
    for (int i = 0; i < PLR_NBTNS - (epT > 0 ? 0 : 1); i++) {
      float f = focusB[i];
      int sel = (button == i && !barFocus);
      buttonCircle(cxs[i], cyButtons, f, a, sel);
      float luma = sel ? 0.13f : 0.94f;
      switch (i) {
        case PLR_PLAY:    iconPlayPause(cxs[i], cyButtons, a, playing, luma); break;
        case PLR_CC:      iconSubtitles(cxs[i], cyButtons, a, luma); break;
        case PLR_ASPECT: iconAspect(cxs[i], cyButtons, a, luma); break;
        case PLR_SOURCES: iconFile(cxs[i],cyButtons,a,luma,"sources",44); break;
        case PLR_EPISODES: iconFile(cxs[i],cyButtons,a,luma,"episodes",44); break;
        default:          iconAudio(cxs[i], cyButtons, a, luma); break;
      }
    }
    if (!barFocus) {
      const char *labels[]={"Play / pause","Aspect ratio","Subtitles","Audio","Sources","Episodes"};
      TxtLine label=txt_line(TXT_PG_END,labels[button],210,212,218,255);
      txt_draw_alpha(label,cxs[button]-label.w*.5f,cyButtons+PLR_BTN_D*.5f+10,a);
    }
  }

  // --- the time label, at the right-hand end of the same row -----------------
  // A single label, "elapsed / total", like the web app's #playerTimeLabel. Here there
  // were TWO — elapsed to the left of the bar and a NEGATIVE remainder to the right —
  // which is the Apple app's convention, not ours. Vertically centred with the circles
  // because in the web app it is an item of a flex row with align-items:center.
  {
    char t1[24], t2[24], all[52];
    fmtTime(t1, sizeof t1, posSeg, 0);
    fmtTime(t2, sizeof t2, durationSeg, 0);
    snprintf(all, sizeof all, "%s / %s", t1, t2);
    { TxtLine l = txt_line(TXT_PLR_BODY, all, 255, 255, 255, 230);
      txt_draw_alpha(l, cx + cw - l.w,
                         cyButtons - (float)l.h * 0.5f, a * 0.9f); }
  }

  // Format badges at the top right. They come from the STREAM, not from a constant:
  // the two used to be hard-coded and announced Dolby Vision on an HDR10 file and
  // Atmos on a stereo track. A badge that lies is worse than no badge, because it is
  // what the owner trusts to know whether they got the good version.
  {
    const char *badges[3];
    int nBadges = 0;
    char res[16] = "";
    if (video_width() >= 3840)      snprintf(res, sizeof res, "4K");
    else if (video_width() >= 1920) snprintf(res, sizeof res, "HD");
    if (res[0]) badges[nBadges++] = res;
    // MEASURED on this TV, a line from the log itself while playing an MKV the addon
    // advertised as Dolby Vision:
    //   [video] pipeline HDR: HDR10 (source claimed DV=1)
    // That was exactly the case where the badge lied.
    //
    // "Dolby Vision" only when the PIPELINE returned DolbyVision in the videoInfo —
    // video_has_dolby_vision no longer reads the addon's claim. It is MEASURED that on
    // this TV an MKV advertised as DV comes back HDR10; the badge said Dolby Vision
    // over an HDR10 stream, and the owner trusts it precisely to know whether they got
    // the good version. When the pipeline says HDR10, the badge says HDR10 — staying
    // quiet would hide half the answer.
    if (video_has_dolby_vision())                  badges[nBadges++] = "Dolby Vision";
    else if (!strcasecmp(video_hdr(), "HDR10"))    badges[nBadges++] = "HDR10";
    if (video_has_atmos())        badges[nBadges++] = "Dolby Atmos";

    // THE CLOCK and "Ends at", which are what the web app puts in this corner
    // (.player-controls-top, playerScreen.js:5846). The quality badges are the port's
    // addition and now sit BELOW them, not in their place.
    //
    //   .player-clock    26/600 white 96%
    //   .player-ends-at  20/400 white 78%, just below
    float yRel = PLR_PAD_Y + slideDown;
    {
      time_t nowT = time(NULL);
      struct tm lt;
      char hora[8], end[32];
      localtime_r(&nowT, &lt);
      strftime(hora, sizeof hora, "%H:%M", &lt);
      { double missing = durationSeg - posSeg;
        time_t t2 = nowT + (time_t)(missing > 0.0 ? missing : 0.0);
        struct tm lf; char h2[8];
        localtime_r(&t2, &lf);
        strftime(h2, sizeof h2, "%H:%M", &lf);
        snprintf(end, sizeof end, "Ends at %s", h2); }
      TxtLine lh = txt_line(TXT_PG_CLOCK, hora, 255, 255, 255, 255);
      TxtLine lf = txt_line(TXT_PG_END, end, 255, 255, 255, 255);
      txt_draw_alpha(lh, NV_SCREEN_W - PLR_PAD_X - lh.w, yRel, a * 0.96f);
      txt_draw_alpha(lf, NV_SCREEN_W - PLR_PAD_X - lf.w, yRel + lh.h + 2.0f,
                         a * 0.78f);
      yRel += lh.h + 2.0f + lf.h;
    }

    { float sy = yRel + 16.0f;
      int i;
      // A STAGGERED ENTRANCE. These badges already appeared one by one, but by
      // accident: the text rasteriser does at most TXT_PER_FRAME lines per frame
      // (text.c:40, and there is a measured reason for that), so the third badge
      // arrived two frames after the first. Read on a TV that is a defect — "they come
      // in showing one at a time", in the owner's words.
      //
      // The fix is not to hurry the rasteriser: it is to OWN the staggering and give
      // it a curve. Each badge comes in 90 ms after the previous one, rising 10px and
      // gaining opacity. What was an artefact becomes a cadence, and the raster's delay
      // hides inside the animation itself.
      float t0 = (float)(now - lastInput) / 1000.0f;
      for (i = 0; i < nBadges; i++) {
        float ts = anim_clamp((t0 - i * 0.09f) / 0.26f, 0.0f, 1.0f);
        float e  = 1.0f - (1.0f - ts) * (1.0f - ts);   // desaceleracao
        TxtLine l = txt_line(TXT_MINI, badges[i], 236, 237, 242, 255);
        if (e > 0.004f)
          txt_draw_alpha(l, NV_SCREEN_W - PLR_PAD_X - l.w,
                             sy + (1.0f - e) * 10.0f, a * 0.85f * e);
        sy += l.h + 6.0f;
      } }
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
        lr = txt_line(TXT_PG_LABEL, parental_label(i), 255, 255, 255, 255);
        ls = txt_line(TXT_PG_SEV, "\xc2\xb7", 255, 255, 255, 255);
        lg = txt_line(TXT_PG_SEV, parental_severity(i), 255, 255, 255, 255);
        cy = yl + (lin - lr.h) * 0.5f;
        x  = xt - dx;
        txt_draw_alpha(lr, x, cy, ag * 0.85f);  x += lr.w;
        txt_draw_alpha(ls, x, yl + (lin - ls.h) * 0.5f, ag * 0.40f); x += ls.w;
        txt_draw_alpha(lg, x, yl + (lin - lg.h) * 0.5f, ag * 0.50f);
      }
    }
  }
}
