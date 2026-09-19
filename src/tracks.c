#include "tracks.h"
#include "player.h"
#include "video.h"
#include "addons.h"
#include "gfx.h"
#include "text.h"
#include "anim.h"
#include "layout.h"
#include "subtitle.h"
#include "settings.h"
#include <stdio.h>
#include <string.h>

// The panel's own measurements live in layout.h under NV_TRK_*, with the web
// app's selector beside each one. What is left here is the row PITCH, which is a
// height plus its gap and belongs to the drawing rather than to the stylesheet.
#define FX_OPT_PITCH   (NV_TRK_OPT_H + NV_TRK_OPT_GAP)
// The style row: the stepper's height with its padding, and the stack's gap.
#define FX_STYLE_H     72.0f
#define FX_STYLE_PITCH (FX_STYLE_H + 8.0f)
// .player-select-menu max-height: min(72vh, 820px) -> 777.6 at 1080.
#define FX_MENU_MAX    778.0f


// 3 and not 2: the subtitle sheet has the LIST and the STYLE, and FX_COL_STYLE
// is index 2. With two slots the style column wrote past the end of the array.
static int is_open, column, focus[3];
// WHICH ROW OF THE STACK HAS THE FOCUS while no list is expanded. The panel is
// one narrow column now, so the old left/right walk between two side-by-side
// columns is gone: 0 is the select, 1..FX_N_STYLE are the style rows beneath it.
static int row;
// Is the select's list expanded? It is what lets the panel be one narrow column
// instead of a pair of wide lists — a long subtitle list only takes screen while
// it is actually being read. BOTH panels open collapsed, on the select itself.
static int openSelect;
// WHICH OF A STYLE ROW'S TWO BUTTONS has the focus: 0 = minus, 1 = plus. The
// steppers are the targets, not the row that holds them — the row is a container,
// which is why it takes a quiet surface and never the inverted fill.
//
// It defaults to PLUS, and is put back to plus on every move between rows. Landing
// on minus would make the first press of OK undo rather than advance, and on a row
// like Delay or Size the direction you want first is nearly always up.
static int stepFocus = 1;
// SCROLL PER COLUMN, in ROWS (not in pixels): the sheet drew every track from
// the top and the panel has a limited height — with many subtitles the last
// ones fell outside the panel and off the screen. The focus reached them, the
// eyes did not. Storing how many rows have been scrolled is enough because the
// row height is fixed.
static int scroll[3];
// How many rows fit in the panel. Computed while drawing (it depends on the
// height chosen there) and read by the key handling, which runs before.
static int visible = 8;
static void adjustScroll(void);
static float anim;
// Which EXTERNAL subtitle (OpenSubtitles) is in force, as an index into the
// combined list — or -1 when the active one is embedded or there is none.
//
// This lives here and not in video.c because the pipeline does not return that
// information: video_subtitle_external sends setSubtitleSource with the URL and
// video.c's current-subtitle index stays untouched, still pointing at the
// EMBEDDED subtitle from before. Without this variable, choosing an
// OpenSubtitles subtitle put the "active" mark on another row (or on "Off") and
// the sheet reopened with the focus in the wrong place — the right subtitle
// played, only the sheet lied about which it was.
static int subExternal = -1;

// The automatic selection's state, declared here because tracks_reset (just
// below) clears it. See the block that follows for what it does.
#define TRK_AUTO_MS 8000
static int autoDone;
static Uint32 autoSince;

// Called when a new playback session starts: the external subtitle belongs to
// the session, not to the device. Without this the next title would open the
// sheet marking as active a subtitle that was not chosen for it.
void tracks_reset(void) {
  subExternal = -1; is_open = 0; subtitle_off();
  autoDone = 0; autoSince = 0;
}

// --- AUTOMATIC SELECTION -----------------------------------------------------
//
// Until this existed NOTHING ever selected a subtitle: apply(), below, was the
// only caller of video_choose_subtitle and subtitle_load in the whole app, and it
// only runs when someone walks into this sheet and presses OK. Every title on
// every start played with the subtitles off, which is what "subtitles are not
// really a thing on legacy" meant.
//
// TWO LISTS, ARRIVING AT DIFFERENT TIMES. The file's own tracks come with
// sourceInfo, and their CODEC — the only thing that says whether a track is text
// or a picture — comes later still, from the MKV header read. The addon's come
// from the network, seconds after that. So this runs every frame and decides on
// the first one where there is something worth deciding, or gives up at the
// deadline; deciding at a fixed instant would mean deciding with half the
// information on half the titles.
// A track that is a PICTURE cannot be turned into text on this TV: no style, no
// position, no size, and on a file whose only subtitles are PGS the choice shows
// nothing at all. An UNKNOWN codec (not an MKV, or the header read failed) is
// treated the same way — selecting it blind is how one ends up with a subtitle
// that silently does nothing, which is the defect this whole path is fixing. The
// addon's subtitle, which is text by definition, is the better answer in both
// cases.
static int embeddedText(const VideoTrack *t) {
  if (!t || !t->codec[0]) return 0;
  return !strstr(t->codec, "PGS") && !strstr(t->codec, "VOBSUB") &&
         !strstr(t->codec, "DVBSUB");
}

void tracks_auto(Uint32 now) {
  // 0 off, 1 automatic, 2 Portuguese, 3 English.
  int pref = settings_subtitle_pref();
  int want = pref == 2 ? 0 : pref == 3 ? 1 : -1;
  int embedded = video_n_subtitle(), pass, i;

  if (autoDone || pref == 0) return;
  if (!autoSince) autoSince = now ? now : 1;
  { int expired = now - autoSince >= TRK_AUTO_MS;
    // The addon search is still out: its result is the one that works on a file
    // with only PGS inside, so it is worth the wait.
    if (!expired && addons_subtitles_busy()) return;
    // The file HAS subtitles and no codec has been read yet — the MKV probe is
    // still running. Choosing now would either skip a perfectly good text track
    // or pick a PGS one; both are answered by waiting a moment.
    if (!expired && embedded > 0) {
      int known = 0;
      for (i = 0; i < embedded; i++)
        if (video_subtitle(i) && video_subtitle(i)->codec[0]) { known = 1; break; }
      if (!known) return;
    } }

  for (pass = 0; pass < 2; pass++) {
    int group = want >= 0 ? want : pass;
    // THE FILE'S OWN FIRST. It needs no download, and the pipeline keeps it in
    // sync with its own clock — our overlay syncs against a position the pipeline
    // reports, which is the same thing one step removed.
    for (i = 0; i < embedded; i++) {
      const VideoTrack *t = video_subtitle(i);
      if (!t || addons_language_group(t->language) != group) continue;
      if (!embeddedText(t)) continue;
      video_choose_subtitle(i); subtitle_off(); subExternal = -1;
      printf("[subtitle] auto: embedded %d (%s)\n", i, t->label);
      fflush(stdout);
      autoDone = 1; return;
    }
    for (i = 0; i < addons_n_subtitles(); i++) {
      const Subtitle *l = addons_subtitle(i);
      if (!l || addons_language_group(l->language) != group) continue;
      video_choose_subtitle(-1); subtitle_load(l->url);
      subExternal = embedded + i;
      printf("[subtitle] auto: addon %d (%s)\n", i, l->label);
      fflush(stdout);
      autoDone = 1; return;
    }
    // A named language does not fall back to the other one: being given
    // Portuguese after asking for English is an answer to a question nobody put.
    if (want >= 0) break;
  }

  // Nothing yet. Keep looking until the deadline — the addon list can still land
  // — and then stop, so the search does not run for the whole film.
  if (now - autoSince >= TRK_AUTO_MS) {
    autoDone = 1;
    printf("[subtitle] auto: nothing to select\n");
    fflush(stdout);
  }
}

// SEPARATE SHEETS: 0 = AUDIO only, 1 = SUBTITLE (list + style).
//
// They used to be ONE sheet with the two columns side by side, by my decision:
// "two screens would force you to leave and come back to check the pair". The
// owner asked for them separate, and the reference agrees — the TCL has a
// subtitle overlay of its own (SubtitleSelectionOverlay), with the style picker
// inside it. Comparing audio+subtitle at once was a case nobody asked for.
static int mode;
// Column inside the SUBTITLE sheet: 0 = list, 1 = style.
#define FX_COL_STYLE 2
#define FX_N_STYLE   9

static int nLines(int col);

void tracks_open(void) { tracks_open_at(0); }

// Opens ALREADY ON THE COLUMN the button asked for. The player has an audio
// icon and a subtitle icon, and both opened this sheet the same way, with the
// focus on audio: pressing "subtitles" and landing on audio makes the two
// buttons look like the same button — which is exactly what the owner reported.
// The panel is still ONE panel, with the two columns side by side (comparing
// the chosen pair is why it exists); what changes is where the focus starts.
void tracks_open_at(int col) {
  int n;
  is_open = 1;
  mode = (col == 1) ? 1 : 0;
  column = mode;                 // audio -> col 0; subtitle -> col 1
  // Both panels open COLLAPSED, on the select itself — the list is one press away
  // and the panel opens showing what is currently chosen rather than a wall of
  // options.
  openSelect = 0;
  row = 0;
  stepFocus = 1;
  focus[0] = video_audio_current();
  // The subtitle may be off (-1); the column's first row is always "Off", so
  // the list index is shifted by one.
  focus[1] = (subExternal >= 0 ? subExternal : video_subtitle_current()) + 1;
  // Clamp on both columns. The subtitle list GROWS during the session (the
  // OpenSubtitles ones arrive later) and the audio one only exists after
  // sourceInfo: storing an older index and reopening without checking puts the
  { int c; for (c = 0; c < 3; c++) {
      n = nLines(c);
      if (focus[c] >= n) focus[c] = n > 0 ? n - 1 : 0;
      if (focus[c] < 0)  focus[c] = 0;
    scroll[c] = 0;
    } }
}

int tracks_is_open(void) { return is_open; }

static int nSubtitles(void) {
  int n = video_n_subtitle() + addons_n_subtitles();
  return n;
}

static int nLines(int col) {
  if (col == FX_COL_STYLE) return FX_N_STYLE;
  if (col == 0) { int n = video_n_audio(); return n; }
  return nSubtitles() + 1;   // +1 for the "Off" row
}

// --- STYLE COLUMN ------------------------------------------------------------
//
// Eight "label: value" rows. OK cycles the value and applies it AT ONCE — the
// customisations below use only methods present in the firmware. The last row
// restores the whole set without needing dozens of presses on the remote.
// SHORT LABELS, because the panel is now one 340px column and the centre of a
// style row — what is left between the two steppers — is 168px of it. That is the
// same 168 the web app leaves itself, and its own labels ("Delay", "Bold",
// "Outline") are cut to fit it.
//
// Row 1 said "OpenSubtitles source" and always had: valueStyle draws
// TXT_FAMILIES_LABEL there, which is the subtitle FONT (Inter / LG / Droid) and
// has nothing to do with where the file came from. The row has been lying since it
// was written; it is called "Font" now.
static const char *const ST_ROT[FX_N_STYLE] = {
  "Size", "Font", "Colour", "Opacity", "Background", "Position", "Border", "Delay",
  "Reset"
};
static const char *const ST_BACKGROUND[5] = { "None", "Dark 25%", "Dark 50%",
                                          "Dark 75%", "Dark 100%" };
static const char *const ST_BORDER[3] = { "None", "Outline", "Shadow" };
static const char *const ST_OPACITY[4]  = { "100%", "75%", "50%", "25%" };

static void valueStyle(int line, char *dst, size_t size) {
  const VideoSubtitleStyle *e = player_sub_style();
  switch (line) {
    case 0: snprintf(dst, size, "%d%%", e->size); break;
    case 1: snprintf(dst, size, "%s", TXT_FAMILIES_LABEL[e->family >= 0 && e->family < TXT_FAMILY_N ? e->family : 0]); break;
    case 2: snprintf(dst, size, "%s", VIDEO_SUB_COLORS_LABEL[e->color % VIDEO_SUB_NCOLORS]); break;
    case 3: snprintf(dst, size, "%s", ST_OPACITY[e->opacity > 3 ? 3 : e->opacity]); break;
    case 4: snprintf(dst, size, "%s", ST_BACKGROUND[e->background > 4 ? 4 : e->background]); break;
    // The uMS accepts -3..4; the sheet shows 1..8 because "position -3" says
    // nothing to whoever is looking at the screen.
    case 5: snprintf(dst, size, "%d of 8", e->position + 1); break;
    case 6: snprintf(dst, size, "%s", ST_BORDER[e->border > 2 ? 2 : e->border]); break;
    case 7: {
      int a = e->delayMs;
      if (!a) snprintf(dst, size, "0 s");
      else    snprintf(dst, size, "%+.2f s", a / 1000.0f);
      break; }
    default: snprintf(dst, size, "Apply"); break;
  }
}

// A step of `dir` (+1 or -1) around a ring of `n`. C's % keeps the sign of the
// left operand, so (0 - 1) % 8 is 0 and not 7: stepping backwards off the start
// of any of these lists would stick rather than wrap without the addend.
static int ring(int v, int dir, int n) { return ((v + dir) % n + n) % n; }

// The steppers either side of the value, and OK, all arrive here. `dir` is +1 or
// -1; the rows that are not a ring (size, delay) clamp-and-wrap at their ends,
// and the last row is an action rather than a value, so both directions do the
// same thing to it.
static void stepStyle(int line, int dir) {
  VideoSubtitleStyle *e = player_sub_style();
  switch (line) {
    case 0:
      e->size += 10 * dir;
      if (e->size > 200) e->size = 50;
      if (e->size < 50)  e->size = 200;
      break;
    case 1: e->family     = ring(e->family,     dir, TXT_FAMILY_N); break;
    case 2: e->color      = ring(e->color,      dir, VIDEO_SUB_NCOLORS); break;
    case 3: e->opacity    = ring(e->opacity,    dir, 4); break;
    case 4: e->background = ring(e->background, dir, 5); break;
    case 5: e->position   = ring(e->position,   dir, 8); break;
    case 6: e->border     = ring(e->border,     dir, 3); break;
    // -5 s to +5 s in 250 ms steps, wrapping round. A smaller step would take
    // dozens of presses to get anywhere on a remote control.
    case 7:
      e->delayMs += 250 * dir;
      if (e->delayMs >  5000) e->delayMs = -5000;
      if (e->delayMs < -5000) e->delayMs =  5000;
      break;
    default:
      *e = (VideoSubtitleStyle){ 120, 0, 0, 3, 1, 0, 0, TXT_FAMILY_INTER };
      break;
  }
  player_sub_style_changed();
}


// The label of row `i` of the subtitle column. Up to video_n_subtitle() they
// are the embedded ones; after that come the OpenSubtitles ones.
static const char *labelSubtitle(int i, const char **brand) {
  int embedded = video_n_subtitle();
  *brand = NULL;
  if (i < embedded) {
    const VideoTrack *f = video_subtitle(i);
    return f ? f->label : "";
  }
  { const Subtitle *l = addons_subtitle(i - embedded);
    if (!l) return "";
    *brand = "OpenSubtitles";
    return l->label; }
}

static void apply(void) {
  // A choice made by hand ENDS the automatic one for this playback, "None"
  // included: turning the subtitle off and having it come back a frame later is
  // the app arguing with the person using it.
  autoDone = 1;
  if (column == 0) {
    video_choose_audio(focus[0]);
  } else {
    int i = focus[1] - 1;
    int embedded = video_n_subtitle();
    if (i < 0)        { video_choose_subtitle(-1); subtitle_off(); subExternal = -1; }
    else if (i < embedded) { video_choose_subtitle(i);  subtitle_off(); subExternal = -1; }
    else {
      const Subtitle *l = addons_subtitle(i - embedded);
      // Only mark as active if there was something to apply: without the URL
      // the uMS gets nothing, and the sheet would say "active" about nothing.
      if (l) {
        /* The font and the 16 sizes are ours now, not the webOS firmware's. */
        video_choose_subtitle(-1); subtitle_load(l->url); subExternal = i;
      }
    }
  }
}

void tracks_event(const SDL_Event *e) {
  SDL_Keycode k;
  if (!is_open || e->type != SDL_KEYDOWN) return;
  k = e->key.keysym.sym;

  // --- The expanded list owns every key until it is dismissed ----------------
  // Including BACK, which collapses the list rather than closing the panel: a
  // list you opened by pressing OK is a thing you should be able to back out of
  // without losing the panel behind it.
  if (openSelect) {
    column = mode;
    if (k == SDLK_AC_BACK || k == SDLK_ESCAPE || k == SDLK_BACKSPACE) {
      openSelect = 0;
      return;
    }
    if (k == SDLK_UP)   { if (focus[column] > 0) focus[column]--; adjustScroll(); return; }
    if (k == SDLK_DOWN) { if (focus[column] < nLines(column) - 1) focus[column]++;
                          adjustScroll(); return; }
    if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
      apply();
      // Choosing closes the panel outright, as it always has: the choice is the
      // whole errand, and dropping back to a collapsed select would make every
      // track change two presses instead of one.
      is_open = 0;
      return;
    }
    return;
  }

  // --- The collapsed stack --------------------------------------------------
  if (k == SDLK_AC_BACK || k == SDLK_ESCAPE || k == SDLK_BACKSPACE) { is_open = 0; return; }
  // Every move between rows puts the focus back on the PLUS. Carrying the side
  // across rows would mean the button under the cursor depends on where you came
  // from, which is not something you can see on screen.
  // The style rail belongs to the SUBTITLE panel. Audio's stack is the select and
  // nothing else, so there is nowhere below row 0 to go — without this the focus
  // walked off into rows that are never drawn, and the panel looked like it had
  // lost the cursor.
  if (k == SDLK_UP)   { if (row > 0) row--; stepFocus = 1; return; }
  if (k == SDLK_DOWN) { if (mode && row < FX_N_STYLE) row++; stepFocus = 1; return; }
  // LEFT/RIGHT WALK THE TWO BUTTONS. They are separate targets, each with its own
  // ring, so the keys move the focus between them rather than acting on the value
  // — OK is what acts. On the select row there is nothing either side to reach.
  if (k == SDLK_LEFT)  { if (row > 0) stepFocus = 0; return; }
  if (k == SDLK_RIGHT) { if (row > 0) stepFocus = 1; return; }
  if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
    if (!row) { openSelect = 1; column = mode; adjustScroll(); return; }
    // OK presses the button that is focused, and applies at once without closing:
    // the owner needs to SEE the subtitle change in order to choose it.
    stepStyle(row - 1, stepFocus ? 1 : -1);
    return;
  }
}

void tracks_update(float dt, Uint32 now) {
  (void)now;
  anim = anim_spring(anim, is_open ? 1.0f : 0.0f, dt, NV_SPRING_SCREEN);
}

// Brings the focused row inside the visible window, moving the MINIMUM: only
// when the focus passes one of the edges. Always scrolling to centre would make
// the whole list move on every keypress, which on a D-pad is disorienting.
static void adjustScroll(void) {
  int n = nLines(column), f = focus[column], *r = &scroll[column];
  if (visible < 1) return;
  if (f < *r) *r = f;
  else if (f >= *r + visible) *r = f - visible + 1;
  if (*r > n - visible) *r = n - visible;
  if (*r < 0) *r = 0;
}

// The label and sub-label of row `i` of the list currently in the select.
static void listRow(int i, const char **main, const char **sub) {
  *sub = NULL;
  if (!mode) {
    const VideoTrack *f = video_audio(i);
    *main = f ? f->label : "";
    *sub  = f ? f->language : NULL;
    return;
  }
  if (!i) { *main = "None"; return; }
  *main = labelSubtitle(i - 1, sub);
  if (!*sub) *sub = "Embedded";
}

// Which row of the list is the one actually playing.
static int listActive(int i) {
  if (!mode) return i == video_audio_current();
  return subExternal >= 0 ? i - 1 == subExternal : i - 1 == video_subtitle_current();
}

// What the COLLAPSED select shows: the active row, which is the only thing the
// row has space to say. Not the focused one — a select that reports whatever the
// cursor is resting on is describing the list, not the state.
static void selectValue(char *dst, size_t size) {
  int n = nLines(mode), i;
  for (i = 0; i < n; i++) {
    if (!listActive(i)) continue;
    { const char *main, *sub;
      listRow(i, &main, &sub);
      snprintf(dst, size, "%s", main && *main ? main : "Track"); }
    return;
  }
  snprintf(dst, size, "%s", mode ? "Off" : "Default");
}

// A row's ground. Focus is an INVERSION — the row fills with white and its type
// goes dark — rather than a ring around it. It is the whole reason the panel can
// drop every border and still read: an outline needs a box to sit on, and the
// boxes are what made the old sheet look like a form.
static void rowFill(GfxRect r, float radiusPx, int focused, float a) {
  float rad = radiusPx / r.h;
  if (focused) gfx_color(r, rad, NV_TRK_FOCUS_FILL, NV_TRK_FOCUS_FILL, NV_TRK_FOCUS_FILL, a);
  else         gfx_color(r, rad, 1.0f, 1.0f, 1.0f, 0.06f * a);
}

// The select row: label, value and the caret, on one line.
static void selectDraw(float x, float y, float w, int focused, float a) {
  char value[96];
  int lr = focused ? 121 : 145, lg = focused ? 122 : 146, lb = focused ? 124 : 148;
  int vr = focused ? NV_TRK_FOCUS_INK : 255;
  TxtLine label, val;
  float caretY;

  rowFill((GfxRect){ x, y, w, NV_TRK_ROW_H }, NV_TRK_ROW_R, focused, a);
  selectValue(value, sizeof value);
  label = txt_line(TXT_TRK_LABEL, mode ? "Subtitles" : "Track", lr, lg, lb, 255);
  txt_draw_alpha(label, x + NV_TRK_ROW_PAD,
                 y + (NV_TRK_ROW_H - label.h) * 0.5f, a);

  // The value is right-aligned and trimmed to what is left after the label, its
  // gap and the caret. Without the trim a long track name runs under the caret
  // and out through the panel's right padding.
  { float used = NV_TRK_ROW_PAD * 2 + label.w + 20.0f + NV_TRK_CARET + 12.0f;
    float room = w - used;
    if (room < 40.0f) room = 40.0f;
    val = txt_line_trim(TXT_TRK_VALUE, value, vr, vr, vr, 255, room);
    txt_draw_alpha(val, x + w - NV_TRK_ROW_PAD - NV_TRK_CARET - 12.0f - val.w,
                   y + (NV_TRK_ROW_H - val.h) * 0.5f, a); }

  // The caret. The web app rotates it 180 degrees when the list is open; this
  // one cannot — it is a PNG whose shape lives in its alpha, and nothing in the
  // draw path turns a quad. The open state is carried by the list sitting
  // directly under the row instead, which is the thing the rotation was pointing
  // at anyway.
  caretY = y + (NV_TRK_ROW_H - NV_TRK_CARET) * 0.5f;
  { float c = focused ? NV_TRK_FOCUS_INK / 255.0f : 0.5f;
    gfx_icon((GfxRect){ x + w - NV_TRK_ROW_PAD - NV_TRK_CARET, caretY,
                        NV_TRK_CARET, NV_TRK_CARET },
             "chevron_down", c, c, c, a); }
}

// The expanded list. `y` is where it starts and `limit` the panel's floor.
static void menuDraw(float x, float y, float w, float limit, float a) {
  float h = limit - y;
  int n = nLines(mode), i, r, end;

  if (h > FX_MENU_MAX) h = FX_MENU_MAX;
  if (h < FX_OPT_PITCH + 16.0f) return;
  gfx_color((GfxRect){ x, y, w, h }, NV_TRK_ROW_R / h, 1.0f, 1.0f, 1.0f, 0.04f * a);

  // adjustScroll works on `column`, which the key handler also sets — but it runs
  // before the first draw of a freshly opened list, so it is pinned here too.
  column = mode;
  visible = (int)((h - 16.0f + NV_TRK_OPT_GAP) / FX_OPT_PITCH);
  if (visible < 1) visible = 1;
  adjustScroll();

  if (!n) {
    txt_block(TXT_TRK_OPTSUB, "No track available from this source.",
              133, 134, 136, x + NV_TRK_OPT_PAD, y + 8.0f + 14.0f,
              w - NV_TRK_OPT_PAD * 2, 26.0f, a, 2);
    return;
  }

  r = scroll[mode]; end = r + visible;
  if (end > n) end = n;
  // The list is clipped to its own box: the bottom row of a list longer than the
  // window has to be CUT to read as "there is more below", not laid past the end
  // of the panel where it would sit on raw video.
  gfx_crop(x, y, w, h);
  for (i = r; i < end; i++) {
    float ry = y + 8.0f + (i - r) * FX_OPT_PITCH;
    int focused = i == focus[mode];
    const char *main, *sub;
    int mc = focused ? NV_TRK_FOCUS_INK : 255;
    int sc = focused ? NV_TRK_FOCUS_INK_SUB : 133;
    TxtLine lm;
    float textW = w - NV_TRK_OPT_PAD * 2 - NV_TRK_DOT - 12.0f;

    listRow(i, &main, &sub);
    if (focused)
      gfx_color((GfxRect){ x + 8.0f, ry, w - 16.0f, NV_TRK_OPT_H },
                NV_TRK_OPT_R / NV_TRK_OPT_H,
                NV_TRK_FOCUS_FILL, NV_TRK_FOCUS_FILL, NV_TRK_FOCUS_FILL, a);

    lm = txt_line_trim(TXT_TRK_OPT, main, mc, mc, mc, 255, textW);
    if (sub && *sub) {
      txt_draw_alpha(lm, x + 8.0f + NV_TRK_OPT_PAD, ry + 8.0f, a);
      txt_draw_alpha(txt_line_trim(TXT_TRK_OPTSUB, sub, sc, sc, sc, 255, textW),
                     x + 8.0f + NV_TRK_OPT_PAD, ry + 34.0f, a);
    } else {
      txt_draw_alpha(lm, x + 8.0f + NV_TRK_OPT_PAD,
                     ry + (NV_TRK_OPT_H - lm.h) * 0.5f, a);
    }

    // Selected is a DOT, not a tick: at 12px a glyph is a smudge, and the row
    // has already said what it is in words.
    if (listActive(i)) {
      float d = NV_TRK_DOT, dy = ry + (NV_TRK_OPT_H - d) * 0.5f;
      float c = focused ? NV_TRK_FOCUS_INK / 255.0f : 0.961f;
      gfx_color((GfxRect){ x + w - 8.0f - NV_TRK_OPT_PAD - d, dy, d, d },
                0.5f, c, c, c, a);
    }
  }
  gfx_no_crop();
}

// One style row: [-] label / value [+]. The steppers are drawn on every row, not
// only the focused one — they are what says the row is adjustable, and a control
// that only appears once you are on it cannot tell you that.
static void styleDraw(int line, float x, float y, float w, int focused, float a) {
  char value[64];
  float sw = NV_TRK_STEP_W, sh = NV_TRK_STEP_H;
  float sy = y + (FX_STYLE_H - sh) * 0.5f;
  float centreX = x + 16.0f + sw + 14.0f;
  float centreW = w - 32.0f - sw * 2 - 28.0f;
  TxtLine lm, lv;

  // No inversion here, and no colour change on the type either — see
  // NV_TRK_STYLE_FOCUS. The row only gains a quiet surface when it is the one
  // the D-pad is on.
  if (focused)
    gfx_color((GfxRect){ x, y, w, FX_STYLE_H }, NV_TRK_ROW_R / FX_STYLE_H,
              1.0f, 1.0f, 1.0f, NV_TRK_STYLE_FOCUS * a);
  valueStyle(line, value, sizeof value);

  // The two steppers are drawn on EVERY row and always the same: they are what
  // says the row is adjustable, and a control that only appears once you are on
  // it cannot tell you that.
  //
  // The focused one takes the app's 4px ring, OUTSIDE its box — the one number in
  // NV_RING_FOCUS that the home card, the episode card and the detail button all
  // share.
  //
  // It is a RING and not a white rounded rect behind the button: the button's own
  // fill is rgba(255,255,255,0.12), so a solid plate under it would show straight
  // through the translucent face and the whole 56px square would read as white
  // rather than as a ring around it. GFX_RING_INSET puts the band strictly inside
  // the enlarged quad's edge, which is exactly a 4px ring sitting outside the
  // button, and leaves the middle untouched.
  { TxtLine minus = txt_line(TXT_TRK_STEP, "-", 255, 255, 255, 255);
    TxtLine plus  = txt_line(TXT_TRK_STEP, "+", 255, 255, 255, 255);
    GfxRect a1 = { x + 16.0f, sy, sw, sh };
    GfxRect a2 = { x + w - 16.0f - sw, sy, sw, sh };
    int i;
    for (i = 0; i < 2; i++) {
      GfxRect b = i ? a2 : a1;
      gfx_color(b, NV_TRK_STEP_R / sh, 1.0f, 1.0f, 1.0f, NV_TRK_STEP_BG * a);
      if (focused && stepFocus == i) {
        float g = NV_RING_FOCUS;
        GfxRect ring = { b.x - g, b.y - g, b.w + g * 2, b.h + g * 2 };
        gfx_rect(ring, 0, GFX_RING_INSET, 0, g / ring.h, 0,
                 (NV_TRK_STEP_R + g) / ring.h, 1, 1, 1, 0.96f * a);
      }
    }
    txt_draw_alpha(minus, a1.x + (sw - minus.w) * 0.5f, a1.y + (sh - minus.h) * 0.5f, a);
    txt_draw_alpha(plus,  a2.x + (sw - plus.w)  * 0.5f, a2.y + (sh - plus.h)  * 0.5f, a); }

  if (centreW < 40.0f) centreW = 40.0f;
  lm = txt_line_trim(TXT_TRK_OPT, ST_ROT[line], 255, 255, 255, 255, centreW);
  lv = txt_line_trim(TXT_TRK_OPTSUB, value,
                     NV_TRK_STYLE_SUB, NV_TRK_STYLE_SUB + 1, NV_TRK_STYLE_SUB + 3,
                     255, centreW);
  txt_draw_alpha(lm, centreX + (centreW - lm.w) * 0.5f, y + 10.0f, a);
  txt_draw_alpha(lv, centreX + (centreW - lv.w) * 0.5f, y + 40.0f, a);
}

void tracks_draw(Uint32 now) {
  float a, px, cx, cw, y, limit;
  TxtLine title;
  (void)now;
  if (anim < .01f) return;
  a = anim;

  // The backdrop ramps the SAME WAY the panel's feather does — see GFX_MENU_SCRIM
  // in gfx.h for what happens when it does not.
  gfx_rect((GfxRect){ 0, 0, NV_SCREEN_W, NV_SCREEN_H }, 0, GFX_MENU_SCRIM,
           0, 0, 0, 0, 0, 0, 0, a);

  // The panel slides a SHORT way and fades, rather than flying in its own width.
  px = NV_SCREEN_W - NV_TRK_PANEL_W + (1.0f - a) * NV_TRK_PANEL_W * NV_TRK_SLIDE;
  gfx_rect((GfxRect){ px, 0, NV_TRK_PANEL_W, NV_SCREEN_H }, 0, GFX_MENU_FEATHER,
           0, NV_TRK_FEATHER / NV_TRK_PANEL_W, 0, 0,
           NV_TRK_INK_R, NV_TRK_INK_G, NV_TRK_INK_B, a);

  cx = px + NV_TRK_FEATHER;
  cw = NV_TRK_PANEL_W - NV_TRK_FEATHER - NV_TRK_PAD_RIGHT;
  limit = NV_SCREEN_H - 48.0f;

  title = txt_line(TXT_TRK_TITLE, mode ? "Subtitles" : "Audio", 255, 255, 255, 255);
  txt_draw_alpha(title, cx, NV_TRK_PAD_TOP, a);
  y = NV_TRK_PAD_TOP + NV_LD_TRK_TITLE + NV_TRK_TITLE_GAP;

  selectDraw(cx, y, cw, !openSelect && !row, a);
  y += NV_TRK_ROW_H + NV_TRK_STACK_GAP;

  if (openSelect) {
    menuDraw(cx, y, cw, limit, a);
    return;
  }

  // The style rail. Subtitles only: the audio panel's stack ends at the select.
  if (!mode) return;
  { int i;
    for (i = 0; i < FX_N_STYLE; i++) {
      float ry = y + i * FX_STYLE_PITCH;
      if (ry + FX_STYLE_H > limit) break;
      styleDraw(i, cx, ry, cw, row == i + 1, a);
    } }
}
