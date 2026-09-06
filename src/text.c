#include "text.h"
#include "gfx.h"
#include "layout.h"
#include "mark.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// How many lines of text are kept at once.
//
// 256 hit its limit when the title page gained the comments section: each card is
// ~7 lines (the name, five of text and the footer) and they live alongside
// episodes, cast, tabs, synopsis and the two meta lines. Past the ceiling, the
// LRU evicts lines THE SAME SCREEN is still going to draw in the same frame; they
// come back through the TXT_PER_FRAME queue, two at a time, and are evicted
// again. It is that loop that shows up as text "flickering".
//
// 512 changes neither the lookup cost (the probe starts at the hash and stops at
// the first hole) nor the rasterisation cost. It costs texture memory for lines
// that are not on screen — the price of not re-rasterising the ones that are.
#define MAX_LINES 512

typedef struct {
  char key[288];
  unsigned long hash;   // FNV-1a of the key, to skip the strcmp
  TxtLine line;
  unsigned long usage;
  unsigned long frameUsage;
  int busy;
} Entry;

// The factor between a BUFFER pixel and a layout pixel. The fonts are opened at
// `body * scale` and the cached line stores the measurement DIVIDED by it, so all
// the rest of the app goes on measuring in 1920x1080 while the glyph has the
// screen's real resolution.
//
// Without this the text was rasterised at 1080p and enlarged twofold on the 4K TV
// — which is exactly the blur the owner saw comparing it with the web app, where
// the browser rasterises at the devicePixelRatio.
static float scaleTxt = 1.0f;
static TTF_Font *fonts[TXT_NFONTS];

// The TTF files of the three weights, READ ONCE and kept alive as long as the app
// lives: FreeType's faces read from them on demand, so freeing here means reading
// freed memory on the first new glyph. They are ~900 KB in total.
// `ownerWeight` marks which pointers are owned: weights pointing at the same file
// share the buffer and only one of them frees it.
static unsigned char *bytesWeight[3];
static size_t         sizeWeight[3];
static int            ownerWeight[3];
// The RWops of each style. Kept because we open with freesrc=0 (the buffer is
// shared, the font must not close it) and somebody has to close it in
// txt_shutdown.
static SDL_RWops     *rwSource[TXT_NFONTS];

// Reads the whole file into a new buffer. NULL if it will not open.
static unsigned char *readAll(const char *path, size_t *size) {
  FILE *f = fopen(path, "rb");
  unsigned char *b;
  long n;
  *size = 0;
  if (!f) return NULL;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
  n = ftell(f);
  if (n <= 0) { fclose(f); return NULL; }
  rewind(f);
  b = malloc((size_t)n);
  if (!b) { fclose(f); return NULL; }
  if (fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
  fclose(f);
  *size = (size_t)n;
  return b;
}
// The alternatives are only opened for the 16 subtitle styles, and on demand.
// Opening the whole matrix (every family x every style in the app) would spend
// memory on a weak TV for a preference that affects at most four lines.
#define TXT_SUB_N (TXT_SUB_200 - TXT_SUB_50 + 1)
static TTF_Font *subFontsHeight[TXT_FAMILY_N][TXT_SUB_N];
static unsigned char subFontTried[TXT_FAMILY_N][TXT_SUB_N];
static int warningFallback[TXT_FAMILY_N];
const char *const TXT_FAMILIES_LABEL[TXT_FAMILY_N] = {
  "Inter", "LG Display", "Droid Sans"
};

static Entry cache[MAX_LINES];

// How many NEW lines may be rasterised per frame.
//
// Measured on the device: entering the detail page rasterises 12 lines at once and
// costs 12 ms — a whole frame, and the jolt lands exactly on the transition that
// is meant to be smooth. Rasterising a drop at a time makes the text settle a
// frame or two later, which nobody sees; the jolt, everybody sees.
// 2 and not 4: with 4 the worst frame averaged 6 ms on text alone, and the aim
// here is that NO single part eats more than a third of the frame.
#define TXT_PER_FRAME 2
static int traceThisFrame;
static unsigned long frameTxt = 1;

void txt_new_frame(void) { traceThisFrame = 0; frameTxt++; }
static unsigned long lruClock = 1;
int    txt_rasterized = 0;
// How many lines have been EVICTED to make room for others. Zero is the healthy
// state. If it starts rising again with the screen still, the table has filled up
// again and the text will flicker — better to read that off a counter than to
// find out from the complaint of whoever is looking at the screen.
int    txt_evictions = 0;
double txt_ms = 0.0;

// The weight per style. Each weight is a real Inter Display FILE (Regular 400,
// Medium 500, Bold 700) — there is no repeated pass and no sub-pixel offset to
// fake a weight. Synthetic bold (TTF_SetFontStyle) only comes in on the FALLBACK
// families (LG, Droid), which have no Bold file of their own; see the
// `if (c > 0 && ...)` in txt_start. That matters because synthetic bold thickens
// the strokes without redrawing anything, and next to a real Bold the difference
// shows immediately in large titles.
//
// HOW THE WEB'S WEIGHT 600 IS RESOLVED, and why there is no single value.
// The embedded Inter has no SemiBold, and adding the file is out of the question
// (the ipk is already 166 MB). That leaves a choice between Medium (100 too
// light) and Bold (100 too heavy), and the choice is OPTICAL, not arithmetic:
//
//   LIGHT text on a dark background looks thinner than it is  -> Bold
//   DARK text on a light pill looks thicker than it is        -> Medium
//
// So `.home-row-title` (600, white on dark) goes Bold and `.series-primary-btn`
// (600, black on a 96px white pill) goes Medium. They are two different
// destinations for the same 600 on purpose, and not an oversight — without the
// rule written here, the next person "fixes" one of the two and misaligns the screen.
enum { WEIGHT_REGULAR, WEIGHT_MEDIUM, WEIGHT_BOLD };
static const struct { int body, weight; } STYLES[TXT_NFONTS] = {
  { NV_FT_TITLE1,  WEIGHT_BOLD   },   // the film's title on the detail screen
  // It WAS WEIGHT_REGULAR, after the tracked-out header of the Apple app's title
  // page. That header NO LONGER EXISTS: the web app's detail screen is a
  // scrollable document with no fixed header, and the Apple app's has left the
  // port. Today TXT_TITLE2 is used only by "Library" (.library-page-title 56/600)
  // and by the empty-state titles of search and library — all THREE at weight 600
  // in the web app. Regular was 200 too light on all of them.
  { NV_FT_TITLE2,  WEIGHT_BOLD    },  // .library-page-title and empty states
  { NV_FT_TITLE3,  WEIGHT_BOLD   },   // the name inside the highlight card
  { NV_FT_HEADLINE, WEIGHT_MEDIUM },   // row header
  { NV_FT_BODY,     WEIGHT_MEDIUM },   // button label, episode title
  { NV_FT_CALLOUT,  WEIGHT_MEDIUM },   // genre line
  { NV_FT_CAPTION,  WEIGHT_REGULAR },  // synopsis, running text
  { NV_FT_CAPTION2, WEIGHT_REGULAR },  // credits, dates, labels
  // Below the 23px minimum tvOS sets for TEXT — but this is not text to read, it
  // is an age-rating badge, which on the device really is the size of an icon.
  { NV_FT_MINI,     WEIGHT_BOLD    },  // age-rating badge
  // The player, from the web app: the title at 700 and the body at 400
  // (.player-title has font-weight 700; .player-subtitle and .player-time-label
  // declare no weight and inherit normal).
  { NV_FT_PLR_TITLE, WEIGHT_BOLD    },
  { NV_FT_PLR_BODY,  WEIGHT_REGULAR },
  { NV_FT_ROW_TITLE, WEIGHT_BOLD    },  // .home-row-title (600)
  // The full-screen hero's secondary line. 600 on a dark background: Bold, by the
  // same optical rule written above.
  { NV_FT_HERO_SEC,   WEIGHT_BOLD    },
  // The detail screen, measured in the web app. The button label's weight 600 does
  // not exist in the embedded Inter package (only Regular, Medium and Bold): it
  // goes MEDIUM, which is 100 too light, and not Bold, which would be 100 too
  // heavy and visibly thickens on a light pill 96px tall.
  { NV_FT_DET_BUTTON, WEIGHT_MEDIUM  },
  { NV_FT_DET_META,  WEIGHT_REGULAR },
  { NV_FT_DET_SIN,   WEIGHT_REGULAR },
  { NV_FT_DET_META2, WEIGHT_REGULAR },
  { NV_FT_HERO_META, WEIGHT_MEDIUM  },   // .home-modern-hero-meta-line (21/500)
  { NV_FT_HERO_SIN,  WEIGHT_REGULAR },   // .home-hero-description (22/400)
  { NV_FT_PG_CLOCK, WEIGHT_MEDIUM  },  // .player-clock (26/600)
  { NV_FT_PG_END,     WEIGHT_REGULAR },  // .player-ends-at (20/400)
  { NV_FT_PG_LABEL,  WEIGHT_MEDIUM  },  // .player-parental-label (22/600)
  { NV_FT_PG_SEV,    WEIGHT_REGULAR },  // .player-parental-severity (22/400)
  { 36, WEIGHT_REGULAR },             // headers of the official player's panels
  { 24, WEIGHT_BOLD },                // episode/source inside the list
  { 28, WEIGHT_MEDIUM },              // title on the Continue Watching card
  { 23, WEIGHT_REGULAR },             // season and episode name
  { 20, WEIGHT_MEDIUM },              // time remaining in the card's badge
  { 110, WEIGHT_BOLD },               // the real position in the ranking
  { 20, WEIGHT_REGULAR }, { 24, WEIGHT_REGULAR }, { 28, WEIGHT_REGULAR },
  { 32, WEIGHT_REGULAR }, { 36, WEIGHT_REGULAR }, { 40, WEIGHT_REGULAR },
  { 44, WEIGHT_REGULAR }, { 48, WEIGHT_REGULAR }, { 52, WEIGHT_REGULAR },
  { 56, WEIGHT_REGULAR }, { 60, WEIGHT_REGULAR }, { 64, WEIGHT_REGULAR },
  { 68, WEIGHT_REGULAR }, { 72, WEIGHT_REGULAR }, { 76, WEIGHT_REGULAR },
  { 80, WEIGHT_REGULAR },
};

// A FALLBACK FOR WHAT INTER DOES NOT HAVE.
//
// Inter covers Latin, and that is all. A Japanese title from an actor's
// filmography (the new person screen shows several) came out as a row of little
// squares — the font has no glyph and SDL_ttf draws .notdef without complaining.
// The TV ships /usr/share/fonts/DroidSansFallback.ttf, which covers CJK; we open
// it ON DEMAND, at the same body size as the style, and only for the lines that
// need it.
//
// It is not a per-glyph fallback (that would mean composing the line character by
// character and losing the kerning): the WHOLE line goes to the fallback when the
// first non-ASCII character does not exist in the main font. A mixed title
// "Deadpool & ウルヴァリン" would come out entirely in the fallback, which is
// ugly but legible — and it is the rare case; the common one is a line written
// entirely in one script.
// ONE FALLBACK PER SCRIPT. The first version had a single file, chosen as "the
// CJK one", and an Iranian actress's name was still little squares: the
// DroidSansFallback has no Arabic. The TV ships a separate file for each script
// family, and that is why the choice is by codepoint range.
typedef enum { SCRIPT_CJK, SCRIPT_ARABIC, SCRIPT_CYRILLIC_ETC, SCRIPT_N } Script;
static TTF_Font *fallbacks[SCRIPT_N][TXT_NFONTS];
static char pathFallback[SCRIPT_N][512];

// The first codepoint OUTSIDE ASCII, or 0. It decodes UTF-8 by hand because it is
// the only point in the app that needs it and pulling in a library for three lines
// does not pay.
static Uint32 firstNotAscii(const char *s) {
  const unsigned char *p = (const unsigned char *)s;
  for (; *p; p++) {
    if (*p < 0x80) continue;
    if ((*p & 0xE0) == 0xC0 && p[1])
      return (Uint32)((*p & 0x1F) << 6 | (p[1] & 0x3F));
    if ((*p & 0xF0) == 0xE0 && p[1] && p[2])
      return (Uint32)((*p & 0x0F) << 12 | (p[1] & 0x3F) << 6 | (p[2] & 0x3F));
    if ((*p & 0xF8) == 0xF0) return 0x10000;   // outside the BMP: we do not handle it
    return 0;
  }
  return 0;
}

// Which fallback covers this codepoint. The ranges are the usual Unicode ones;
// anything that is not Arabic/Hebrew or CJK falls into the third, which is
// DroidSansFallback (it covers Cyrillic, Greek, Thai and more).
static Script scriptOf(Uint32 cp) {
  if (cp >= 0x0590 && cp <= 0x07FF) return SCRIPT_ARABIC;       // Hebrew + Arabic
  if (cp >= 0xFB50 && cp <= 0xFEFF) return SCRIPT_ARABIC;       // presentation forms
  if (cp >= 0x2E80 && cp <= 0x9FFF) return SCRIPT_CJK;
  if (cp >= 0xAC00 && cp <= 0xD7AF) return SCRIPT_CJK;         // hangul
  if (cp >= 0xF900 && cp <= 0xFAFF) return SCRIPT_CJK;
  return SCRIPT_CYRILLIC_ETC;
}

// The font line `s` should be drawn with. It returns the main one when that
// copes — which is the case for the overwhelming majority of lines.
static TTF_Font *fontOf(TxtStyle style, const char *s) {
  Uint32 cp = firstNotAscii(s);
  Script e;
  if (!cp || cp >= 0x10000) return fonts[style];
  // Portuguese and Spanish accents are in Inter; only what it really lacks falls
  // through to the fallback.
  if (TTF_GlyphIsProvided(fonts[style], (Uint16)cp)) return fonts[style];
  e = scriptOf(cp);
  if (!pathFallback[e][0]) return fonts[style];
  if (!fallbacks[e][style])
    fallbacks[e][style] = TTF_OpenFont(pathFallback[e],
                                       (int)(STYLES[style].body * scaleTxt + 0.5f));
  return fallbacks[e][style] ? fallbacks[e][style] : fonts[style];
}

static TTF_Font *subtitleFontOf(TxtStyle style, const char *s,
                                TxtFamily family) {
  int i;
  const char *path;
  if (family <= TXT_FAMILY_INTER || family >= TXT_FAMILY_N ||
      style < TXT_SUB_50 || style > TXT_SUB_200)
    return fontOf(style, s);
  i = style - TXT_SUB_50;
  if (subFontsHeight[family][i]) return subFontsHeight[family][i];
  if (subFontTried[family][i]) return fontOf(style, s);
  subFontTried[family][i] = 1;
  path = family == TXT_FAMILY_LG
          ? "/usr/share/fonts/LG_Display-Regular.ttf"
          : "/usr/share/fonts/DroidSans.ttf";
  subFontsHeight[family][i] = TTF_OpenFont(
      path, (int)(STYLES[style].body * scaleTxt + 0.5f));
  if (!subFontsHeight[family][i]) {
    if (!warningFallback[family]) {
      printf("subtitle font %s unavailable; using Inter\n",
             TXT_FAMILIES_LABEL[family]);
      warningFallback[family] = 1;
    }
    return fontOf(style, s);
  }
  return subFontsHeight[family][i];
}

int txt_start(const char *dirAssets, float scale) {
  if (scale < 0.5f) scale = 1.0f;
  scaleTxt = scale;
  if (TTF_Init() != 0) { printf("TTF_Init: %s\n", TTF_GetError()); return 0; }
  // LG's own font is the one the TV's interface uses; DroidSans is the fallback.
  // Inter is EMBEDDED in the package. The TV has only LG's fonts and Netflix's
  // app's — nothing close to tvOS's SF Pro. Inter was designed as a free
  // alternative with similar metrics, and it is what brings the letterforms
  // closest to the original. LG's fonts stay as a fallback: if the package is
  // installed without the fonts/ folder, the app stays legible instead of dying.
  char base[512] = "";
  if (dirAssets && *dirAssets) {
    snprintf(base, sizeof base, "%s/", dirAssets);
  } else {
    char *bp = SDL_GetBasePath();
    if (bp) { snprintf(base, sizeof base, "%s", bp); SDL_free(bp); }
  }

  char inter[3][512];
  snprintf(inter[WEIGHT_REGULAR], 512, "%sfonts/InterDisplay-Regular.ttf", base);
  snprintf(inter[WEIGHT_MEDIUM],  512, "%sfonts/InterDisplay-Medium.ttf",  base);
  snprintf(inter[WEIGHT_BOLD],    512, "%sfonts/InterDisplay-Bold.ttf",    base);

  const char *lg[3] = { "/usr/share/fonts/LG_Display-Light.ttf",
                        "/usr/share/fonts/LG_Display-Regular.ttf",
                        "/usr/share/fonts/LG_Display-Regular.ttf" };
  const char *droid[3] = { "/usr/share/fonts/DroidSans.ttf",
                           "/usr/share/fonts/DroidSans.ttf",
                           "/usr/share/fonts/DroidSans.ttf" };

  const char *families[3][3] = {
    { inter[0], inter[1], inter[2] },
    { lg[0], lg[1], lg[2] },
    { droid[0], droid[1], droid[2] },
  };
  const char *names[3] = { "Inter (embedded)", "LG Display", "DroidSans" };

  // The path of the CJK fallback. On the TV it is DroidSansFallback; on the Mac,
  // the system font that covers CJK — there this is only so the preview does not lie.
  // A width of 5, not 4: the Arabic row has four candidates plus the NULL, and the
  // loop below stops at the NULL. With [4] the terminator was silently discarded
  // and the Arabic search carried on reading into the Cyrillic row.
  { const char *cand[SCRIPT_N][5] = {
      /* SCRIPT_CJK          */ { "/usr/share/fonts/LG_Display_JP.ttf",
                               "/usr/share/fonts/DroidSansFallback.ttf",
                               "/System/Library/Fonts/Hiragino Sans GB.ttc", NULL },
      /* SCRIPT_ARABIC        */ { "/usr/share/fonts/DroidNaskh-Regular.ttf",
                               "/usr/share/fonts/LG_Display_Urdu.ttf",
                               "/System/Library/Fonts/Supplemental/GeezaPro.ttc",
                               "/System/Library/Fonts/Supplemental/Arial Unicode.ttf", NULL },
      /* SCRIPT_CYRILLIC_ETC */ { "/usr/share/fonts/DroidSansFallback.ttf",
                               "/usr/share/fonts/DroidSans.ttf",
                               "/System/Library/Fonts/Supplemental/Arial Unicode.ttf", NULL },
    };
    const char *nameScript[SCRIPT_N] = { "CJK", "arabic", "rest" };
    for (int e = 0; e < SCRIPT_N; e++) {
      for (int i = 0; cand[e][i]; i++) {
        FILE *fr = fopen(cand[e][i], "rb");
        if (fr) { fclose(fr);
                  snprintf(pathFallback[e], sizeof pathFallback[e], "%s", cand[e][i]);
                  break; }
      }
      printf("fallback %s: %s\n", nameScript[e],
             pathFallback[e][0] ? pathFallback[e] : "none");
    } }

  mark("fonts: start");
  for (int c = 0; c < 3; c++) {
    int all = 1;
    // ONE FILE, ONE READ.
    //
    // MEASURED: 1035 ms on the TV against 12 ms on the Mac for the SAME txt_start.
    // It is not FreeType that costs — it is the device's storage. TTF_OpenFont
    // opens and READS THE WHOLE FILE on every call, and there are TXT_NFONTS calls
    // over only THREE distinct files (Regular, Medium, Bold): the same dozen reads
    // of the same dozen megabytes, on a disk that delivers ~1 MB/s of small file.
    //
    // Here the three files are read ONCE into memory and each style opens over
    // those bytes with TTF_OpenFontRW. There is deliberately no thread: the
    // bottleneck was REPEATED I/O, and parallelising redundant reads on the same
    // slow storage does not make them less redundant — not doing the reads does.
    // Serial is more predictable, and none of this goes near FreeType's dubious
    // thread-safety.
    for (int p = 0; p < 3; p++) {
      int j;
      // LG repeats Regular across two weights and Droid across all three: do not read again.
      for (j = 0; j < p; j++)
        if (!strcmp(families[c][p], families[c][j])) break;
      if (j < p) { bytesWeight[p] = bytesWeight[j]; sizeWeight[p] = sizeWeight[j]; ownerWeight[p] = 0; continue; }
      bytesWeight[p] = readAll(families[c][p], &sizeWeight[p]);
      ownerWeight[p] = bytesWeight[p] ? 1 : 0;
      if (!bytesWeight[p]) { all = 0; break; }
    }
    if (all)
      for (int i = 0; i < TXT_NFONTS; i++) {
        int weight = STYLES[i].weight;
        // One RWops PER font: FreeType reads through the stream for the whole life
        // of the face, so two styles cannot share the same read position. They are
        // bytes in memory — creating the RWops costs no I/O.
        SDL_RWops *rw = SDL_RWFromConstMem(bytesWeight[weight], (int)sizeWeight[weight]);
        // freesrc=0: is it TTF_CloseFont in txt_shutdown that frees the RWops? No
        // — we pass 0 and keep the pointer, because the buffer is shared between
        // styles and must not be freed by the first font to close.
        fonts[i] = rw ? TTF_OpenFontRW(rw, 0, (int)(STYLES[i].body * scaleTxt + 0.5f)) : NULL;
        rwSource[i] = rw;
        // LG uses SDL 2.0.4: SDL_RWclose only exists in newer SDL versions.
        // SDL_FreeRW is the ABI available on webOS 4 and correctly frees the
        // stream created by SDL_RWFromConstMem.
        if (!fonts[i]) { if (rw) SDL_FreeRW(rw); rwSource[i] = NULL; all = 0; break; }
        // synthetic bold only on the fallback, which has no Bold file of its own
        if (c > 0 && STYLES[i].weight == WEIGHT_BOLD) TTF_SetFontStyle(fonts[i], TTF_STYLE_BOLD);
      }
    if (all) {
      printf("font: %s (%d styles, 3 reads)\n", names[c], TXT_NFONTS);
      mark("fonts: ready");
      return 1;
    }
    for (int i = 0; i < TXT_NFONTS; i++) {
      if (fonts[i]) TTF_CloseFont(fonts[i]);
      fonts[i] = NULL;
      if (rwSource[i]) SDL_FreeRW(rwSource[i]);
      rwSource[i] = NULL;
    }
    for (int p = 0; p < 3; p++) {
      if (ownerWeight[p]) free(bytesWeight[p]);
      bytesWeight[p] = NULL; sizeWeight[p] = 0; ownerWeight[p] = 0;
    }
  }
  printf("txt: no font loaded\n");
  mark("fonts: none loaded");
  return 0;
}

void txt_shutdown(void) {
  for (int i = 0; i < MAX_LINES; i++)
    if (cache[i].busy && cache[i].line.tex) glDeleteTextures(1, &cache[i].line.tex);
  // ORDER: the font first, the RWops after, the buffer last. FreeType's face still
  // references the stream, and the stream, the bytes.
  for (int i = 0; i < TXT_NFONTS; i++) {
    if (fonts[i]) TTF_CloseFont(fonts[i]);
    fonts[i] = NULL;
    if (rwSource[i]) SDL_FreeRW(rwSource[i]);
    rwSource[i] = NULL;
    for (int e = 0; e < SCRIPT_N; e++)
      if (fallbacks[e][i]) TTF_CloseFont(fallbacks[e][i]);
  }
  for (int p = 0; p < 3; p++) {
    if (ownerWeight[p]) free(bytesWeight[p]);
    bytesWeight[p] = NULL; sizeWeight[p] = 0; ownerWeight[p] = 0;
  }
  for (int f = 1; f < TXT_FAMILY_N; f++)
    for (int i = 0; i < TXT_SUB_N; i++)
      if (subFontsHeight[f][i]) TTF_CloseFont(subFontsHeight[f][i]);
  TTF_Quit();
}

static TxtLine lineFamily(TxtStyle style, const char *s, int r, int g,
                             int b, int a, TxtFamily family) {
  TxtLine empty = {0, 0, 0};
  if (!s || !*s || style < 0 || style >= TXT_NFONTS || !fonts[style]) return empty;

  if (family < TXT_FAMILY_INTER || family >= TXT_FAMILY_N)
    family = TXT_FAMILY_INTER;

  char key[288];
  snprintf(key, sizeof key, "%d:%d|%02x%02x%02x|%.236s", (int)family,
           (int)style, r & 255, g & 255, b & 255, s);

  // A hash of the key to avoid the strcmp on almost every entry: the lookup runs
  // for EVERY line of EVERY frame, and comparing 288 bytes hundreds of times per
  // frame costs more than the drawing.
  unsigned long h = 2166136261UL;
  { const char *p = key;
    for (; *p; p++) { h ^= (unsigned char)*p; h *= 16777619UL; } }

  // Probing from h % MAX_LINES, and not a sweep of all 256 entries. This lookup
  // runs for EVERY line of EVERY frame; the full sweep cost an average of 128
  // comparisons per hit. Probing from the hash's position the hit comes out in the
  // first few slots, and the search STOPS at the first empty slot: anything
  // inserted by this same rule is never after a hole.
  //
  // LRU eviction can open a hole in the middle of an old chain; the effect is at
  // most one re-rasterisation of that line (which goes back in nearer its hash),
  // never a wrong result — the key is checked by strcmp either way.
  int free_ = -1;
  for (int k = 0; k < MAX_LINES; k++) {
    int i = (int)((h + (unsigned long)k) % MAX_LINES);
    if (!cache[i].busy) { free_ = i; break; }
    if (cache[i].hash == h && strcmp(cache[i].key, key) == 0) {
      cache[i].usage = ++lruClock;
      cache[i].frameUsage = frameTxt;
      return cache[i].line;
    }
  }

  // Budget blown: return empty and try again next frame. The line appears one
  // frame late instead of freezing the current one.
  if (traceThisFrame >= TXT_PER_FRAME) return empty;
  traceThisFrame++;
  int slot = free_;
  if (slot < 0) {
    // The table is full: only now is a full sweep for the LRU worth it. That
    // happens at most TXT_PER_FRAME times per frame, not per line.
    unsigned long smaller = ~0UL;
    for (int i = 0; i < MAX_LINES; i++)
      if (cache[i].busy && cache[i].frameUsage != frameTxt &&
          cache[i].usage < smaller) {
        smaller = cache[i].usage;
        slot = i;
      }
  }
  if (slot < 0) return empty;
  if (cache[slot].busy) txt_evictions++;
  if (cache[slot].busy && cache[slot].line.tex) {
    // tell gfx: the name may be reused by the glGenTextures just below
    gfx_tex_forget(cache[slot].line.tex);
    glDeleteTextures(1, &cache[slot].line.tex);
  }

  Uint64 t0 = SDL_GetPerformanceCounter();
  SDL_Color color = { (Uint8)r, (Uint8)g, (Uint8)b, (Uint8)a };
  SDL_Surface *sf = TTF_RenderUTF8_Blended(
      subtitleFontOf(style, s, family), s, color);
  if (!sf) return empty;
  SDL_Surface *cv = SDL_ConvertSurfaceFormat(sf, SDL_PIXELFORMAT_ABGR8888, 0);
  SDL_FreeSurface(sf);
  if (!cv) return empty;

  GLuint t; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, cv->w, cv->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, cv->pixels);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  gfx_tex_forget(0);  // the upload's bind went around gfx_rect

  cache[slot].busy = 1;
  cache[slot].hash = h;
  strncpy(cache[slot].key, key, sizeof cache[slot].key - 1);
  // Measured in LAYOUT units, not in buffer pixels.
  cache[slot].line.tex = t;
  cache[slot].line.w = (int)(cv->w / scaleTxt + 0.5f);
  cache[slot].line.h = (int)(cv->h / scaleTxt + 0.5f);
  txt_rasterized++;
  txt_ms += (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
  cache[slot].usage = ++lruClock;
  cache[slot].frameUsage = frameTxt;
  SDL_FreeSurface(cv);
  return cache[slot].line;
}

TxtLine txt_line(TxtStyle style, const char *s, int r, int g, int b, int a) {
  return lineFamily(style, s, r, g, b, a, TXT_FAMILY_INTER);
}

TxtLine txt_line_family(TxtStyle style, const char *s, int r, int g,
                           int b, int a, TxtFamily family) {
  return lineFamily(style, s, r, g, b, a, family);
}

void txt_draw(TxtLine l, float x, float y) { txt_draw_alpha(l, x, y, 1.0f); }

// SNAPPING TO THE SCREEN'S PIXEL.
//
// The glyph's texture has exactly the resolution it will be drawn at, but the
// CORNER kept landing on a fractional coordinate: centring (`(r.h - l.h) * 0.5f`),
// stacks anchored to the bottom, scrolling springs. With the corner at 478.4,
// GL_LINEAR samples BETWEEN two texels and every letter comes out spread across
// two pixel columns — the whole text is half a pixel out of focus, all over the
// screen, all the time.
//
// That was what remained of the "blur" after 4K proved impossible: it is not
// resolution that is missing, it is the text landing on the pixel. The web app
// does not have this problem because the browser already positions glyphs on the
// device's grid.
//
// The rounding is done on the DRAWABLE's grid and not the layout's: on a retina
// Mac half a layout pixel is a whole screen pixel, and rounding on the wrong grid
// would throw the text out of place instead of settling it.
//
// Only TEXT snaps. Snapping cards and art would turn the springs into visible
// steps; the glyph does not suffer from that because the letter itself does not
// deform, it just moves from one pixel to the next.
static float fits(float v) {
  float e = scaleTxt;
  return (float)((int)(v * e + (v < 0.0f ? -0.5f : 0.5f))) / e;
}

void txt_draw_alpha(TxtLine l, float x, float y, float alpha) {
  if (!l.tex) return;
  GfxRect r = { fits(x), fits(y), (float)l.w, (float)l.h };
  gfx_rect(r, l.tex, GFX_TEXT, 0, 0, 0, 0.0f, 1, 1, 1, alpha);
}

float txt_tracking(TxtStyle style, const char *s, int r, int g, int b,
                   float x, float y, float alpha, float tracking) {
  if (!s || !*s) return 0.0f;
  float width = 0.0f;
  // It walks by UTF-8 CHARACTER, not by byte: cutting in the middle of an accent
  // produces an invalid glyph, and LG's font returns an empty rectangle.
  for (const unsigned char *p = (const unsigned char *)s; *p; ) {
    int n = 1;
    if      ((*p & 0xF8) == 0xF0) n = 4;
    else if ((*p & 0xF0) == 0xE0) n = 3;
    else if ((*p & 0xE0) == 0xC0) n = 2;
    char c[5]; int k = 0;
    while (k < n && p[k]) { c[k] = (char)p[k]; k++; }
    c[k] = 0; p += k ? k : 1;

    TxtLine l = txt_line(style, c, r, g, b, 255);
    if (x >= 0.0f && l.w) txt_draw_alpha(l, x + width, y, alpha);
    width += l.w + tracking;
  }
  return width > 0.0f ? width - tracking : 0.0f;
}

// Declared in text.h from the start and NEVER implemented. Nobody called it, so
// the link passed; the first call brought down the ARM build with "undefined
// reference". On the Mac that does NOT show up: `cc -fsyntax-only` on a loose file
// links nothing.
TxtLine txt_line_trim(TxtStyle style, const char *s, int r, int g, int b,
                         int a, float maxW) {
  return txt_line_trim_family(style, s, r, g, b, a, maxW,
                                 TXT_FAMILY_INTER);
}

TxtLine txt_line_trim_family(TxtStyle style, const char *s, int r, int g,
                                 int b, int a, float maxW,
                                 TxtFamily family) {
  TxtLine l = txt_line_family(style, s, r, g, b, a, family);
  if (!s || !*s || (float)l.w <= maxW) return l;
  char buf[512];
  size_t n = strlen(s);
  if (n >= sizeof buf - 4) n = sizeof buf - 4;
  memcpy(buf, s, n); buf[n] = 0;
  // It cuts by WORD while there is a space; only when a single word is left does
  // it cut in the middle of it. Always cutting by character leaves half a word
  // before the ellipsis, and that reads as corrupted text, not as truncation.
  while (n > 0) {
    size_t cut = n;
    while (cut > 0 && buf[cut - 1] != ' ') cut--;
    if (cut > 1) n = cut - 1; else n--;
    // never stop in the middle of a UTF-8 character: half a character becomes tofu
    while (n > 0 && ((unsigned char)buf[n] & 0xC0) == 0x80) n--;
    buf[n] = 0;
    if (!n) break;
    char t[520];
    snprintf(t, sizeof t, "%s\xe2\x80\xa6", buf);
    l = txt_line_family(style, t, r, g, b, a, family);
    if ((float)l.w <= maxW) return l;
  }
  return txt_line_family(style, "\xe2\x80\xa6", r, g, b, a, family);
}

float txt_block(TxtStyle style, const char *s, int r, int g, int b,
                float x, float y, float width, float leading, float alpha, int maxLines) {
  if (!s || !*s) return 0.0f;
  char line[512]; line[0] = 0;
  float used = 0.0f;
  int nLines = 0;
  const char *p = s;
  while (*p && (maxLines <= 0 || nLines < maxLines)) {
    // pega a proxima palavra
    const char *start = p;
    while (*p && *p != ' ') p++;
    size_t np = (size_t)(p - start);
    while (*p == ' ') p++;

    char attempt[512];
    size_t nl = strlen(line);
    if (nl + np + 2 >= sizeof attempt) break;
    memcpy(attempt, line, nl);
    if (nl) attempt[nl++] = ' ';
    memcpy(attempt + nl, start, np);
    attempt[nl + np] = 0;

    TxtLine m = txt_line(style, attempt, r, g, b, 255);
    if (m.w > width && line[0]) {
      // it did not fit: close the current line and start again with the word
      TxtLine l = txt_line(style, line, r, g, b, 255);
      txt_draw_alpha(l, x, y + used, alpha);
      used += leading; nLines++;
      if (maxLines > 0 && nLines >= maxLines) return used;
      memcpy(line, start, np); line[np] = 0;
    } else {
      memcpy(line, attempt, nl + np + 1);
    }
  }
  if (line[0] && (maxLines <= 0 || nLines < maxLines)) {
    TxtLine l = txt_line(style, line, r, g, b, 255);
    txt_draw_alpha(l, x, y + used, alpha);
    used += leading;
  }
  return used;
}

// It wraps like txt_block, but positions each line by the RIGHT EDGE. The
// duplication with txt_block is small and deliberate: unifying the two would need
// an alignment parameter on every call, and only this case needs it.
float txt_block_dir(TxtStyle style, const char *s, int r, int g, int b,
                    float xDir, float y, float width, float leading,
                    float alpha, int maxLines) {
  if (!s || !*s) return 0.0f;
  char line[512]; line[0] = 0;
  float used = 0.0f;
  int nLines = 0;
  const char *p = s;
  while (*p && (maxLines <= 0 || nLines < maxLines)) {
    const char *start = p;
    while (*p && *p != ' ') p++;
    size_t np = (size_t)(p - start);
    while (*p == ' ') p++;

    char attempt[512];
    size_t nl = strlen(line);
    if (nl + np + 2 >= sizeof attempt) break;
    memcpy(attempt, line, nl);
    if (nl) attempt[nl++] = ' ';
    memcpy(attempt + nl, start, np);
    attempt[nl + np] = 0;

    TxtLine m = txt_line(style, attempt, r, g, b, 255);
    if (m.w > width && line[0]) {
      TxtLine l = txt_line(style, line, r, g, b, 255);
      if (xDir >= 0.0f) txt_draw_alpha(l, xDir - l.w, y + used, alpha);
      used += leading; nLines++;
      if (maxLines > 0 && nLines >= maxLines) return used;
      memcpy(line, start, np); line[np] = 0;
    } else {
      memcpy(line, attempt, nl + np + 1);
    }
  }
  if (line[0] && (maxLines <= 0 || nLines < maxLines)) {
    TxtLine l = txt_line(style, line, r, g, b, 255);
    if (xDir >= 0.0f) txt_draw_alpha(l, xDir - l.w, y + used, alpha);
    used += leading;
  }
  return used;
}
