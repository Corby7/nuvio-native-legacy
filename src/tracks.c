#include "tracks.h"
#include "player.h"
#include "video.h"
#include "addons.h"
#include "gfx.h"
#include "text.h"
#include "anim.h"
#include "layout.h"
#include "subtitle.h"
#include <stdio.h>
#include <string.h>

// 1400 and not 1180: with the third column, "Very small" and "Dark 100%" did
// not fit in the value's space and came out cut. The AUDIO sheet, which has a
// single column, uses a fraction of this — see tracks_draw.
#define FX_WIDTH   1400.0f
#define FX_LINE   106.0f


// 3 and not 2: the subtitle sheet has the LIST and the STYLE, and FX_COL_STYLE
// is index 2. With two slots the style column wrote past the end of the array.
static int is_open, column, focus[3];
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

// Called when a new playback session starts: the external subtitle belongs to
// the session, not to the device. Without this the next title would open the
// sheet marking as active a subtitle that was not chosen for it.
void tracks_reset(void) { subExternal = -1; is_open = 0; subtitle_off(); }

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
static const char *const ST_ROT[FX_N_STYLE] = {
  "Size", "OpenSubtitles source", "Colour", "Opacity", "Background", "Position", "Border", "Delay",
  "Restore default"
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

static void cycleStyle(int line) {
  VideoSubtitleStyle *e = player_sub_style();
  switch (line) {
    case 0: e->size += 10; if (e->size > 200) e->size = 50; break;
    case 1: e->family = (e->family + 1) % TXT_FAMILY_N; break;
    case 2: e->color     = (e->color + 1) % VIDEO_SUB_NCOLORS; break;
    case 3: e->opacity = (e->opacity + 1) % 4; break;
    case 4: e->background   = (e->background + 1) % 5; break;
    case 5: e->position = (e->position + 1) % 8; break;
    case 6: e->border   = (e->border + 1) % 3; break;
    // -5 s to +5 s in 250 ms steps, wrapping round. A smaller step would take
    // dozens of presses to get anywhere on a remote control.
    case 7:
      e->delayMs += 250;
      if (e->delayMs > 5000) e->delayMs = -5000;
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
  if (k == SDLK_AC_BACK || k == SDLK_ESCAPE || k == SDLK_BACKSPACE) { is_open = 0; return; }
  // Left/right move between the LIST and the STYLE, and only on the subtitle
  // sheet. On the audio one there is nowhere to go — they used to jump to the
  // subtitle column, which is precisely what made the player's two buttons look
  // like the same button.
  if (k == SDLK_LEFT)  { if (mode && column == FX_COL_STYLE) column = 1; return; }
  if (k == SDLK_RIGHT) { if (mode && column == 1) column = FX_COL_STYLE; return; }
  if (k == SDLK_UP)    { if (focus[column] > 0) focus[column]--; adjustScroll(); return; }
  if (k == SDLK_DOWN)  { if (focus[column] < nLines(column) - 1) focus[column]++;
                         adjustScroll(); return; }
  if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
    // In the STYLE column OK CYCLES the value and applies it at once, without
    // closing: the owner needs to SEE the subtitle change in order to choose,
    // and closing the sheet on every press would hide the list from them.
    if (column == FX_COL_STYLE) { cycleStyle(focus[FX_COL_STYLE]); return; }
    apply(); is_open = 0; return;
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

static void column_draw(int col, float x, float width, float y0, float a) {
  const char *title=col==FX_COL_STYLE?"Style":col?"Subtitles":"Audio tracks";
  txt_draw_alpha(txt_line(TXT_PG_LABEL,title,188,190,196,255),x,y0,a);
  int n=nLines(col), r=scroll[col], end=r+visible;
  if(end>n) end=n;
  for(int i=r;i<end;i++) {
    float y=y0+64+(i-r)*FX_LINE;
    int sel=col==column && i==focus[col];
    const char *brand=NULL,*rot;
    char value[48];
    if(col==FX_COL_STYLE) {
      valueStyle(i,value,sizeof value); rot=ST_ROT[i]; brand=value;
    } else if(!col) {
      const VideoTrack *f=video_audio(i);
      rot=f?f->label:""; brand=f?f->language:NULL;
    } else {
      rot=i==0?"None":labelSubtitle(i-1,&brand);
      if(i && !brand) brand="Embedded";
    }
    if(sel) gfx_color((GfxRect){x-20,y-14,width+20,92},.18f,.95f,.95f,.96f,a);
    int c=sel?25:230, sub=sel?70:174;
    txt_draw_alpha(txt_line_trim(TXT_PANEL_ITEM,rot,c,c,c,255,width-72),x,y,a);
    if(brand && *brand)
      txt_draw_alpha(txt_line_trim(TXT_PG_END,brand,sub,sub,sub,255,width-72),x,y+34,a);
    int active=col==0?i==video_audio_current():
      col==1?(subExternal>=0?i-1==subExternal:i-1==video_subtitle_current()):0;
    if(active) txt_draw_alpha(txt_line(TXT_BODY,"✓",c,c,c,255),x+width-44,y+12,a);
  }
  if(!n) txt_block(TXT_PG_END,"No track available from this source.",178,180,186,x,y0+68,width,28,a,2);
  if(n>visible) {
    char num[48]; snprintf(num,sizeof num,"%d of %d",focus[col]+1,n);
    txt_draw_alpha(txt_line(TXT_MINI,num,174,176,182,255),x,y0+64+visible*FX_LINE,a);
  }
}

void tracks_draw(Uint32 now) {
  (void)now;
  if(anim<.01f) return;
  float a=anim;
  gfx_color((GfxRect){0,0,NV_SCREEN_W,NV_SCREEN_H},0,.025f,.025f,.03f,.88f*a);
  txt_draw_alpha(txt_line(TXT_PANEL_TITLE,mode?"Subtitles":"Audio",242,243,245,255),56,48,a);
  txt_draw_alpha(txt_line(TXT_PG_END,"Back to close",180,182,188,255),NV_SCREEN_W-250,60,a);
  visible=7;
  adjustScroll();
  if(!mode) column_draw(0,76,720,138,a);
  else {
    column_draw(1,76,990,138,a);
    column_draw(FX_COL_STYLE,1190,650,138,a);
  }
}
