#include "episodes.h"
#include "catalog.h"
#include "discover.h"
#include "extras.h"
#include "gfx.h"
#include "tex_cache.h"
#include "text.h"
#include "layout.h"
#include "anim.h"
#include "watchedep.h"
#include "player.h"
#include "video.h"
#include "detail.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The distance from one closed row's top to the next. The measurements are in
// layout.h under NV_EPL_*; the column and the header are the track menus'.
#define EP_PITCH    (NV_EPL_ROW_H + NV_EPL_ROW_GAP)
#define EP_VIEW_H   (NV_EPL_VIEW_BOTTOM - NV_EPL_VIEW_TOP)
// Where a row's name and detail line start, and so how wide its synopsis runs.
// On the focused row the thumb has grown and the text starts further in.
#define EP_TEXT_X(f) (NV_EPL_PADX + NV_EPL_THUMB_W * (1.0f + (NV_EPL_THUMB_GROW - 1.0f) * (f)) \
                      + NV_EPL_TEXT_GAP + NV_EPL_TEXT_GROW * (f))
#define EP_TEXT_W(f) (NV_EPL_W - NV_EPL_PADX - EP_TEXT_X(f))
// How many rows carry their own opening animation. A longer season still works:
// the rows past this one open and close without easing.
#define EP_MAX      512
static int is_open, title, currentT, currentE, season, focus;
static int requestT, requestE;
static float anim, scroll;
static int locateCurrent;
// Snap the list into place on the next update instead of easing into it: opening
// the panel, or changing season, is not a movement the eye should follow.
static int snap;
// How far open each row is, 0..1. Only the focused row heads for 1.
static float opened[EP_MAX];
// WHERE THE CURSOR IS: on the season pill, or in the list. The pill opens a menu
// of seasons; left/right change season from either place without opening it.
enum { Z_PILL, Z_LIST };
static int zone;
static int smOpen, smCursor, smScroll;

// THE PRESS THAT OPENS AN EPISODE is taken on its KEYUP, and only when its
// KEYDOWN happened here: a release without a press in this layer is not a click.
// The layer above may have closed on the KEYDOWN and let the KEYUP leak through,
// and acting on the KEYDOWN instead would leak this panel's KEYUP to the player.
static int okHeld;

static int nSeasons(void) {
  const CatItem *c = cat_item(title);
  return c && c->nSeasons > 0 ? c->nSeasons : 1;
}
static int numSeason(int i) {
  const CatItem *c = cat_item(title);
  return c && c->nSeasons > 0 ? c->seasons[i] : currentT;
}
static const CatEp *epLine(int line) {
  int n = cat_n_episodes(title);
  for (int i = 0, j = 0; i < n; i++) {
    const CatEp *ep = cat_episode(title, i);
    if (ep && ep->season == numSeason(season) && j++ == line) return ep;
  }
  return NULL;
}
static int nLines(void) {
  int n = 0;
  for (int i=0;i<cat_n_episodes(title);i++) {
    const CatEp *e=cat_episode(title,i);
    if(e && e->season==numSeason(season)) n++;
  }
  return n;
}
void episodes_open(int idx, int t, int e) {
  title = idx; currentT = t; currentE = e; is_open = 1;
  okHeld = 0;
  season = focus = 0; requestE = 0; scroll = 0;
  zone = Z_LIST; smOpen = 0;
  locateCurrent = 1; snap = 1;
  for (int i = 0; i < nSeasons(); i++) if (numSeason(i) == t) season = i;
  for (int i = 0; i < nLines(); i++) if (epLine(i)->episode == e) focus = i;
  disc_episodes(title, t);
}
int episodes_is_open(void) { return is_open; }
float episodes_shown(void) { return anim < 0.01f ? 0.0f : anim; }
void episodes_close(void) { is_open = 0; okHeld = 0; smOpen = 0; }
int episodes_chose(int *t, int *e) {
  if (!requestE) return 0;
  *t = requestT; *e = requestE; requestE = 0; return 1;
}
// Moves to another season. The list starts at its top, unless it is the season
// on screen, where it finds the episode playing — the same courtesy as opening.
static void pickSeason(int s) {
  if (s < 0 || s >= nSeasons() || s == season) return;
  season = s; focus = 0; scroll = 0; snap = 1;
  locateCurrent = numSeason(season) == currentT;
  disc_episodes(title, numSeason(season));
}

static void seasonName(int i, char *dst, size_t size) {
  int t = numSeason(i);
  if (t == 0) snprintf(dst, size, "Specials");
  else snprintf(dst, size, "Season %d", t);
}

// How many seasons the open menu shows at once, and keeping its cursor among them.
static int smVisible(void) {
  return nSeasons() < NV_EPL_SMENU_VIS ? nSeasons() : NV_EPL_SMENU_VIS;
}
static void smKeep(void) {
  int vis = smVisible();
  if (smCursor < smScroll) smScroll = smCursor;
  if (smCursor >= smScroll + vis) smScroll = smCursor - vis + 1;
  if (smScroll < 0) smScroll = 0;
}

static void smEvent(SDL_Keycode k) {
  if (k == SDLK_UP && smCursor > 0) smCursor--;
  else if (k == SDLK_DOWN && smCursor < nSeasons() - 1) smCursor++;
  else if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) {
    pickSeason(smCursor);
    smOpen = 0; zone = Z_LIST;
  } else if (k == SDLK_ESCAPE || k == SDLK_BACKSPACE || k == SDLK_DELETE ||
             k == SDLK_AC_BACK) smOpen = 0;
  smKeep();
}

void episodes_event(const SDL_Event *ev) {
  if (!is_open) return;

  // The season menu takes every key while it is up.
  if (smOpen) { if (ev->type == SDL_KEYDOWN) smEvent(ev->key.keysym.sym); return; }

  // OK ON AN EPISODE ROW opens it — on the KEYUP, see okHeld. Over an empty or
  // loading season OK means "try again", below.
  { SDL_Keycode ko = ev->key.keysym.sym;
    int isOk = (ko == SDLK_RETURN || ko == SDLK_KP_ENTER || ko == SDLK_SPACE);
    if (isOk && zone == Z_LIST && nLines() > 0) {
      if (ev->type == SDL_KEYDOWN && !ev->key.repeat) okHeld = 1;
      if (ev->type == SDL_KEYUP && okHeld) {
        const CatEp *ep = epLine(focus);
        okHeld = 0;
        if (ep) {
          if (ep->season != currentT || ep->episode != currentE) {
            requestT = ep->season; requestE = ep->episode;
          }
          is_open = 0;
        }
      }
      return;
    }
    // The KEYUP of an OK that acted on its KEYDOWN must not fall through.
    if (isOk && ev->type == SDL_KEYUP) { okHeld = 0; return; }
  }

  if (ev->type != SDL_KEYDOWN) return;
  SDL_Keycode k = ev->key.keysym.sym;
  if(k==SDLK_r) { disc_episodes(title,numSeason(season)); return; }
  if (k == SDLK_ESCAPE || k == SDLK_BACKSPACE || k == SDLK_DELETE || k == SDLK_AC_BACK) {
    is_open = 0; return;
  }
  // Up/down walk the episodes, and past the first one up onto the season pill.
  // Left/right change season from anywhere, without opening the pill's menu.
  { int n = nLines();
    if (k == SDLK_UP) {
      if (zone == Z_LIST && focus > 0) focus--;
      else zone = Z_PILL;
    }
    if (k == SDLK_DOWN) {
      if (zone == Z_PILL) zone = Z_LIST;
      else if (focus < n - 1) focus++;
    }
    if (k == SDLK_LEFT)  pickSeason(season - 1);
    if (k == SDLK_RIGHT) pickSeason(season + 1);
    if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) {
      if (zone == Z_PILL) { smOpen = 1; smCursor = season; smScroll = 0; smKeep(); }
      else if (!n) disc_episodes(title, numSeason(season));
    } }
}

// The text block's height with `lines` of synopsis under it. The last line's
// leading is air below the ink, and the padding already gives the row that.
static float synBlock(int lines) {
  return NV_EPL_SYN_DY + lines * NV_EPL_SYN_LD - (NV_EPL_SYN_LD - 22.0f);
}

static int synLines(const CatEp *ep) {
  int lines;
  if (!ep || !ep->synopsis[0]) return 0;
  lines = txt_block_lines(TXT_TRK_OPTSUB, ep->synopsis, EP_TEXT_W(1.0f));
  return lines > NV_EPL_SYN_LINES ? NV_EPL_SYN_LINES : lines;
}
// The open row's text block: the name, the detail line and the synopsis.
static float fullBlock(const CatEp *ep) {
  int lines = synLines(ep);
  return lines > 0 ? synBlock(lines) : NV_EPL_BLOCK_H;
}
// How much taller a row is when it is open: its synopsis, up to three lines, and
// NV_EPL_PADY above and below the block. A row with no synopsis does not grow.
static float openExtra(const CatEp *ep) {
  float h = fullBlock(ep) + NV_EPL_PADY * 2 - NV_EPL_ROW_H;
  return h > 0.0f ? h : 0.0f;
}
static float openedAt(int i) {
  return i < EP_MAX ? opened[i] : (i == focus && zone == Z_LIST ? 1.0f : 0.0f);
}

void episodes_update(float dt) {
  anim = anim_spring(anim, is_open ? 1 : 0, dt, NV_SPRING_SCREEN);
  if(!is_open && anim<.005f) return;
  disc_episodes_pending();
  int n = nLines();
  if(locateCurrent && n) {
    for(int i=0;i<n;i++) if(epLine(i)->episode==currentE) focus=i;
    locateCurrent=0; snap=1;
  }
  if (focus >= n) focus = n > 0 ? n - 1 : 0;
  // A row is open only while the cursor is on it: up on the pill, none is.
  for (int i = 0; i < n && i < EP_MAX; i++) {
    float want = i == focus && zone == Z_LIST;
    opened[i] = snap ? want : anim_spring(opened[i], want, dt, NV_SPRING_GRID);
  }
  // THE LIST'S OFFSET, as the sources sheet keeps its own: the focused row held
  // in the middle of the window, the list stopping at both ends, and Discover's
  // grid spring, so every list in the player moves the same way. It is measured
  // against where the rows are HEADING rather than where they are mid-animation:
  // every row above the focus closed, the focused one open.
  { float fh = NV_EPL_ROW_H + openExtra(n ? epLine(focus) : NULL);
    float total = n * EP_PITCH - NV_EPL_ROW_GAP + (fh - NV_EPL_ROW_H);
    float max = total - EP_VIEW_H;
    float target = focus * EP_PITCH - (EP_VIEW_H - fh) * 0.5f;
    // WHOLE ROWS ONLY at the top. Rows leave through the fade above the list, so
    // an offset between two rows fades the upper one out while it still has most
    // of its height on screen — a hole under the pill a row tall. Rounded to the
    // pitch, the top row always sits flush in the window.
    target = roundf(target / EP_PITCH) * EP_PITCH;
    if (target > max) target = max;
    if (target < 0) target = 0;
    scroll = snap ? target : anim_spring(scroll, target, dt, NV_SPRING_GRID); }
  if (n) snap = 0;
}

// THE EQUALISER, copied from the sources sheet so "playing" looks the same in
// both: three bars bottom-aligned on the text's baseline, on periods that are
// not multiples of one another so the eye reads movement, not a pattern.
static void equaliser(float x, float base, Uint32 now, float a) {
  static const float SPEED[3] = { 0.0091f, 0.0067f, 0.0113f };  // radians per ms
  static const float PHASE[3] = { 0.0f, 2.1f, 4.2f };
  int i;
  for (i = 0; i < 3; i++) {
    float s = 0.5f + 0.5f * sinf((float)now * SPEED[i] + PHASE[i]);
    float h = NV_SRC_EQ_H * (NV_SRC_EQ_MIN + (1.0f - NV_SRC_EQ_MIN) * s);
    gfx_color((GfxRect){ x + i * (NV_SRC_EQ_W + NV_SRC_EQ_GAP), base - h,
                         NV_SRC_EQ_W, h },
              NV_SRC_EQ_R / h, 1, 1, 1, a);
  }
}
#define EQ_TOTAL (3 * NV_SRC_EQ_W + 2 * NV_SRC_EQ_GAP)

// One item of the detail line, with the sources sheet's dot and spacing before
// it when it is not the first. Returns the new cursor.
static float metaItem(TxtStyle st, const char *text, float x, float y, int first,
                      int c, int dim, float a) {
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

// One row: the still on the left, the name and a detail line beside it, and — as
// the row opens — the synopsis under them. A row the cursor is not on, and that
// is not playing, drops back so the two that matter carry the column. `open` is
// the row's focus spring: it grows the thumb and opens the synopsis together.
static void rowDraw(const CatEp *ep, float cx, float y, float h, int sel, int next,
                    float open, float a, float vt, float vb) {
  const CatItem *ci = cat_item(title);
  float sc = 1.0f + (NV_EPL_THUMB_GROW - 1.0f) * open;
  // The thumb is centred on the row's WHOLE height, open or closed, and grows
  // about its left edge.
  GfxRect th = { cx + NV_EPL_PADX, y + (h - NV_EPL_THUMB_H * sc) * 0.5f,
                 NV_EPL_THUMB_W * sc, NV_EPL_THUMB_H * sc };
  float tx = cx + EP_TEXT_X(open), right = cx + NV_EPL_W - NV_EPL_PADX;
  float rt = NV_EPL_THUMB_R / th.h;
  // The text block, centred like the thumb: closed it is the name and the detail
  // line, and it grows toward the whole block as the row opens.
  float blockH = NV_EPL_BLOCK_H + (fullBlock(ep) - NV_EPL_BLOCK_H) * open;
  float bt = y + (h - blockH) * 0.5f;
  int current = ep->season == currentT && ep->episode == currentE;
  int lit = sel || current;
  int ink = lit ? 255 : NV_EPL_DIM, sub = lit ? 178 : NV_EPL_DIM_SUB;
  int dim = lit ? 104 : 80;
  const char *art;
  GLuint tex;
  char line[200];
  int watched, mapped;

  // THE BAND runs to the screen's edge, behind everything, and fades in from the
  // left (the right-hand menus' feather) so it has no edge of its own there.
  if (sel) {
    GfxRect band = { cx - NV_EPL_BAND_LEAD, y, NV_SCREEN_W - cx + NV_EPL_BAND_LEAD, h };
    gfx_rect(band, 0, GFX_MENU_FEATHER, 0, NV_EPL_BAND_FEATHER / band.w, 0, 0,
             1, 1, 1, NV_EPL_BAND * a);
  }

  // The ring rides on the thumbnail alone, outside its edge, and grows with it.
  if (sel) {
    float g = NV_RING_FOCUS;
    GfxRect r = { th.x - g, th.y - g, th.w + g * 2, th.h + g * 2 };
    gfx_rect(r, 0, GFX_RING_INSET, 0, g / r.h, 0, (NV_EPL_THUMB_R + g) / r.h,
             1, 1, 1, 0.96f * a * (open > 0.2f ? 1.0f : open * 5.0f));
  }

  gfx_color(th, rt, 1, 1, 1, 0.07f * a);
  art = ep->thumb[0] ? ep->thumb : (ci ? ci->backdrop : "");
  tex = art && art[0] ? tex_get_width(art, NV_EPL_THUMB_W * NV_EPL_THUMB_GROW) : 0;
  if (tex) {
    gfx_tex_aspect_current = tex_aspect(art);
    gfx_rect(th, tex, GFX_CARD, 0, 0, 0, rt, 0, 0, 0, a * (lit ? 1.0f : NV_EPL_DIM_THUMB));
    gfx_tex_aspect_current = 0;
  }

  // The playing episode's position, along the thumb's base.
  if (current && video_duration() > 0) {
    float f = (float)(video_pos() / video_duration());
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    gfx_color((GfxRect){ th.x, th.y + th.h - NV_EPL_PROG_H, th.w, NV_EPL_PROG_H },
              0, 0, 0, 0, 0.55f * a);
    gfx_color((GfxRect){ th.x, th.y + th.h - NV_EPL_PROG_H, th.w * f, NV_EPL_PROG_H },
              0, 1, 1, 1, a);
  }

  // THE WATCHED MARK: a dark disc on the thumb's top-right corner, the bare tick
  // white on it.
  mapped = ci ? watchedep_state(ci->imdb, ep->season, ep->episode) : -1;
  watched = mapped >= 0 ? mapped : extras_ep_watched(ep->season, ep->episode);
  if (watched) {
    float d = NV_EPL_CHECK, t = d * NV_EPL_CHECK_TICK, in = 8.0f;
    GfxRect st = { th.x + th.w - in - d, th.y + in, d, d };
    GfxRect tk = { st.x + (d - t) * 0.5f, st.y + (d - t) * 0.5f, t, t };
    gfx_color(st, 0.5f, 0.05f, 0.05f, 0.06f, 0.72f * a);
    gfx_icon_at(tk, "ep_watched", t, 1, 1, 1, 0.9f * a);
  }

  // UP NEXT, set right on the name and detail line's centre.
  if (next) {
    TxtLine l = txt_line(TXT_ERAIL_PILL, "UP NEXT", 17, 18, 20, 255);
    float pw = l.w + NV_EPL_NEXT_PADX * 2;
    GfxRect pr = { right - pw, bt + NV_EPL_BLOCK_H * 0.5f - NV_EPL_NEXT_H * 0.5f,
                   pw, NV_EPL_NEXT_H };
    gfx_color(pr, 8.0f / pr.h, 0.93f, 0.93f, 0.94f, a);
    txt_draw_alpha(l, pr.x + NV_EPL_NEXT_PADX, pr.y + (pr.h - l.h) * 0.5f, a);
    right = pr.x - 20.0f;
  }

  // "EP4 · What the River Keeps" — the season is on the pill already.
  snprintf(line, sizeof line, "EP%d%s%s", ep->episode, ep->name[0] ? " \xc2\xb7 " : "", ep->name);
  txt_draw_alpha(txt_line_trim(TXT_TRK_VALUE, line, ink, ink, ink, 255, right - tx), tx, bt, a);

  // The detail line, in the sources sheet's meta type and spacing: the runtime
  // and "watched", or, on the playing episode, the equaliser, "Playing" and how
  // long is left of it.
  { float my = bt + NV_EPL_SUB_DY, x = tx;
    if (current) {
      equaliser(x, my + txt_baseline(TXT_SRC_META), SDL_GetTicks(), a);
      x += EQ_TOTAL + 9.0f;
      x = metaItem(TXT_SRC_STATE, "Playing", x, my, 1, 244, dim, a);
      if (video_duration() > 0) {
        int left = (int)((video_duration() - video_pos()) / 60.0 + 0.5);
        snprintf(line, sizeof line, "%d min left", left < 1 ? 1 : left);
        metaItem(TXT_SRC_META, line, x, my, 0, sub, dim, a);
      }
    } else {
      x = metaItem(TXT_SRC_META, ep->duration, x, my, 1, sub, dim, a);
      if (watched) metaItem(TXT_SRC_META, "watched", x, my, !ep->duration[0], sub, dim, a);
    } }

  // THE SYNOPSIS, under the detail line, faded in as the row opens and cut to the
  // row's own height so it never spills over the row below while it grows.
  if (open > 0.01f && ep->synopsis[0]) {
    float cy0 = y > vt ? y : vt, cy1 = y + h < vb ? y + h : vb;
    if (cy1 > cy0) {
      gfx_crop(0, cy0, NV_SCREEN_W, cy1 - cy0);
      txt_block_trim(TXT_TRK_OPTSUB, ep->synopsis, 176, 178, 184,
                tx, bt + NV_EPL_SYN_DY, right - tx,
                NV_EPL_SYN_LD, a * open * open, NV_EPL_SYN_LINES);
      gfx_crop(0, vt, NV_SCREEN_W, vb - vt);
    }
  }
}

// THE SEASON PILL, drawn as the title page's season picker is (drawSeason in
// detail.c): #222 with a hair line at rest; focused, or open, the ground lifts to
// rgb(48,48,48) and the ring goes INSIDE the edge. Returns its rect, for the menu
// to hang under.
static GfxRect pillDraw(float x, float y, float a) {
  char s[32];
  TxtLine l;
  GfxRect r;
  float f = zone == Z_PILL || smOpen ? 1.0f : 0.0f;
  seasonName(season, s, sizeof s);
  l = txt_line_trim(TXT_TRK_VALUE, s, 255, 255, 255, 255,
                    NV_EPL_PILL_W - NV_EPL_PILL_PADX * 2 - 16.0f - NV_EPL_PILL_CHEV);
  r = (GfxRect){ x, y, NV_EPL_PILL_W, NV_EPL_PILL_H };
  { float luma = NV_DETWEB_REST + (NV_DETWEB_SEA_FOCUS_BG - NV_DETWEB_REST) * f;
    gfx_color(r, NV_RADIUS_PILL, luma, luma, luma, NV_PLR_DD_A * a); }
  if (f < 0.99f)
    gfx_rect(r, 0, GFX_RING, 0, NV_DETWEB_SEA_BORDER / r.h, 0, NV_RADIUS_PILL,
             1, 1, 1, 0.10f * a);
  else
    gfx_rect(r, 0, GFX_RING_INSET, 0, NV_DETWEB_SEA_RING / r.h, 0, NV_RADIUS_PILL,
             1, 1, 1, 0.96f * a);
  txt_draw_alpha(l, x + NV_EPL_PILL_PADX, y + (r.h - l.h) * 0.5f, a);
  gfx_icon((GfxRect){ r.x + r.w - NV_EPL_PILL_PADX - NV_EPL_PILL_CHEV,
                      y + (r.h - NV_EPL_PILL_CHEV) * 0.5f,
                      NV_EPL_PILL_CHEV, NV_EPL_PILL_CHEV }, "chevron_down",
           0.702f, 0.702f, 0.702f, a);
  return r;
}

// THE SEASON MENU, as the title page's (drawSeasonMenu in detail.c): a #222 plate
// with a drop shadow, hung under the pill, and pill-shaped options — the focused
// one inverted to #f5f5f5 with #111 ink. It opens with the focus on the season on
// screen, which is how it shows which one that is; no mark of its own.
static void seasonMenu(GfxRect pill, float a) {
  int vis = smVisible(), i;
  float w = pill.w;
  GfxRect box;
  float radius;
  char name[32];
  // As wide as the pill, or as the longest name needs.
  for (i = 0; i < nSeasons(); i++) {
    float need;
    seasonName(i, name, sizeof name);
    need = txt_line(TXT_TRK_OPT, name, 255, 255, 255, 255).w
           + (NV_DETWEB_SEA_MENU_PADX + NV_DETWEB_SEA_OPT_PADX) * 2;
    if (need > w) w = need;
  }
  box = (GfxRect){ pill.x, pill.y + pill.h + NV_DETWEB_SEA_MENU_GAP, w,
                   NV_DETWEB_SEA_MENU_PADY * 2 + vis * NV_EPL_SMENU_ROW };
  // The radius is normalised to the height; 32px keeps the corners the title
  // page's proportion on a plate this size rather than a lozenge.
  radius = 32.0f / box.h;
  // NO DROP SHADOW here, unlike the title page's: GFX_SHADOW is only soft
  // outside its quad, and a quad the plate's size left it a hard copy 8px lower
  // whose corners showed square under the plate's round ones. Over a see-through
  // plate it would also darken the plate itself.
  gfx_color(box, radius, NV_DETWEB_REST, NV_DETWEB_REST, NV_DETWEB_REST, NV_PLR_DD_A * a);
  gfx_rect(box, 0, GFX_RING, 0, 1.0f / box.h, 0, radius, 1, 1, 1, 0.08f * a);
  for (i = 0; i < vis; i++) {
    int s = smScroll + i, on = s == smCursor;
    GfxRect op = { box.x + NV_DETWEB_SEA_MENU_PADX,
                   box.y + NV_DETWEB_SEA_MENU_PADY + i * NV_EPL_SMENU_ROW,
                   box.w - NV_DETWEB_SEA_MENU_PADX * 2, NV_EPL_SMENU_ROW };
    int ink = on ? 17 : 255;
    if (on) gfx_color(op, NV_RADIUS_PILL, NV_DETWEB_FOCUS, NV_DETWEB_FOCUS, NV_DETWEB_FOCUS, a);
    seasonName(s, name, sizeof name);
    { TxtLine l = txt_line(TXT_TRK_OPT, name, ink, ink, ink, 255);
      txt_draw_alpha(l, op.x + NV_DETWEB_SEA_OPT_PADX, op.y + (op.h - l.h) * 0.5f, a); }
  }
}

void episodes_draw(void) {
  if (anim < .005f) return;
  int n = nLines(), i;
  // The panel slides in from the right and fades, as the track menus do.
  float slide = (1.0f - anim) * NV_EPL_VEIL_W * NV_TRK_SLIDE;
  float cx = NV_SCREEN_W - NV_EPL_PAD - NV_EPL_W + slide;
  float vt = NV_EPL_VIEW_TOP, y;
  const CatEp *next = player_next_episode();
  GfxRect pill;

  // The sources sheet's veil at full height, and wider than the track menus':
  // this column is wider, and the rows run to the bottom of the screen.
  gfx_rect((GfxRect){ NV_SCREEN_W - NV_EPL_VEIL_W + slide, 0, NV_EPL_VEIL_W, NV_SCREEN_H },
           0, GFX_SRC_VEIL, 0, 1, 0, 0,
           NV_SRC_INK_R, NV_SRC_INK_G, NV_SRC_INK_B, anim * NV_SRC_VEIL_A);

  // --- the heading, and the count centred on its capitals --------------------
  // The track menus' header, so the player's panels all open the same way.
  { TxtLine t = txt_line(TXT_PANEL_TITLE, "Episodes", 240, 241, 243, 255);
    txt_draw_alpha(t, cx + NV_EPL_PADX, NV_TRK_TITLE_Y, anim);
    if (n) {
      char c[32];
      float capT = txt_cap_inset(TXT_PANEL_TITLE), capC = txt_cap_inset(TXT_SRC_COUNT);
      float mid = NV_TRK_TITLE_Y + (capT + txt_baseline(TXT_PANEL_TITLE)) * 0.5f;
      snprintf(c, sizeof c, "%d episode%s", n, n == 1 ? "" : "s");
      txt_draw_alpha(txt_line(TXT_SRC_COUNT, c, 132, 135, 142, 255),
                     cx + NV_EPL_PADX + (float)t.w + 18.0f,
                     mid - (capC + txt_baseline(TXT_SRC_COUNT)) * 0.5f, anim);
    } }

  // --- the season pill, on the line the Subtitles sheet puts its tabs on -------
  pill = pillDraw(cx + NV_EPL_PADX, NV_TRK_TABS_Y, anim);

  // --- the list ---------------------------------------------------------------
  if (!n) {
    txt_block(TXT_TRK_OPTSUB, disc_episodes_loading(title)
                ? "Loading episodes\xe2\x80\xa6"
                : "Episodes unavailable. Press OK to try again.",
              133, 134, 136, cx + NV_EPL_PADX, vt + 16.0f, NV_EPL_W - NV_EPL_PADX * 2,
              28.0f, anim, 3);
  } else {
    // ROWS DISSOLVE AS THEY LEAVE THE TOP, the sources sheet's way (anim_edge):
    // across the air between the pill's ring and the first row a row going up
    // fades to nothing, so it is gone before the clip would cut it. Below, the
    // clip runs to the screen's edge and the list simply continues past it.
    float fadeTop = NV_TRK_TABS_Y + NV_EPL_PILL_H + NV_RING_FOCUS;
    gfx_crop(0, fadeTop, NV_SCREEN_W, NV_SCREEN_H - fadeTop);
    y = vt - scroll;
    for (i = 0; i < n && y < NV_SCREEN_H; i++) {
      float op = openedAt(i), h = NV_EPL_ROW_H, edge;
      const CatEp *ep = NULL;
      // A row that is opening needs its synopsis measured, and so its episode.
      if (op > 0.001f) { ep = epLine(i); h += openExtra(ep) * op; }
      edge = anim_edge(y, fadeTop, vt - fadeTop);
      if (y + h > fadeTop && edge > 0.004f) {
        if (!ep) ep = epLine(i);
        gfx_opacity_group = edge;
        if (ep) rowDraw(ep, cx, y, h, zone == Z_LIST && i == focus,
                        next && ep->season == next->season && ep->episode == next->episode,
                        op, anim, fadeTop, NV_SCREEN_H);
        // PUT IT BACK before anything else is drawn: a group opacity left set
        // bleeds onto every later draw call in the frame.
        gfx_opacity_group = 1.0f;
      }
      y += h + NV_EPL_ROW_GAP;
    }
    gfx_no_crop();
  }

  // The season menu over the list it replaces.
  if (smOpen) seasonMenu(pill, anim);
}
