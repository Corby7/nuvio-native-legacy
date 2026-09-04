// Text: SDL_ttf rasterises to a texture, cached by (font, size, string). Without
// the cache, every frame would rasterise the same row titles again — text
// rasterisation is expensive and the content here changes little.
#ifndef NV_TEXT_H
#define NV_TEXT_H
#include "gl_compat.h"

// The tvOS scale. Each style carries a size AND a weight: on the device the
// difference between a title and a subtitle comes as much from the weight as
// from the size, and using a single weight flattens the whole hierarchy — that
// is what made the screen look like "everything the same size, some bigger".
typedef enum {
  TXT_TITLE1, TXT_TITLE2, TXT_TITLE3, TXT_HEADLINE,
  TXT_BODY, TXT_CALLOUT, TXT_CAPTION, TXT_CAPTION2, TXT_MINI,
  // The player's two come from the web app, not from the tvOS scale. They sit at
  // the END of the enum on purpose: the STYLES table in text.c is indexed by this
  // order, and inserting in the middle shifts every following style silently.
  TXT_PLR_TITLE, TXT_PLR_BODY, TXT_ROW_TITLE, TXT_HERO_SEC,
  // The DETAIL screen, measured in the web app. They reuse no tvOS style because
  // none matches: the synopsis there is 26/400 and TXT_CAPTION here is 22/400 —
  // four pixels that change how many lines fit in the block.
  TXT_DET_BUTTON,   // .series-primary-btn      25 / 600
  TXT_DET_META,    // .series-detail-support   25 / 400
  TXT_DET_SIN,     // .series-detail-description 26 / 400
  TXT_DET_META2,   // .detail-meta-row.secondary 23 / 400
  // The HERO's meta line: 21 / 500, rgb(179,179,179). It is neither TXT_CAPTION
  // (22/400) nor TXT_CALLOUT (28/500) — one gets the weight wrong, the other the
  // size, and the line came out either too faint or too heavy against the art.
  TXT_HERO_META,
  // The hero's synopsis: .home-hero-description, 22/400 full white. TXT_CAPTION
  // has the same size but is grey — the colour comes from whoever draws, not
  // from the style.
  TXT_HERO_SIN,
  // Canto superior do PLAYER, do bloco #playerUiRoot do web:
  //   .player-clock          26 / 600
  //   .player-ends-at        20 / 400
  //   .player-parental-label 22 / 600
  //   .player-parental-severity e .player-parental-separator 22 / 400
  TXT_PG_CLOCK, TXT_PG_END, TXT_PG_LABEL, TXT_PG_SEV,
  TXT_PANEL_TITLE, TXT_PANEL_ITEM,
  TXT_CW_TITLE, TXT_CW_META, TXT_CW_BADGE,
  TXT_RANK,
  // Legenda externa: 50%..200%, em passos de 10. O firmware da C9 oferece
  // apenas cinco degraus; estas fontes pertencem ao overlay do proprio app.
  TXT_SUB_50, TXT_SUB_60, TXT_SUB_70, TXT_SUB_80,
  TXT_SUB_90, TXT_SUB_100, TXT_SUB_110, TXT_SUB_120,
  TXT_SUB_130, TXT_SUB_140, TXT_SUB_150, TXT_SUB_160,
  TXT_SUB_170, TXT_SUB_180, TXT_SUB_190, TXT_SUB_200,
  TXT_NFONTS
} TxtStyle;

typedef struct { GLuint tex; int w, h; } TxtLine;

// Familia alternativa usada SOMENTE pelo renderer de legenda externa. A
// interface continua em Inter; misturar a familia da legenda com menus faria
// a preferencia de reproducao redesenhar o app inteiro.
typedef enum {
  TXT_FAMILY_INTER = 0,
  TXT_FAMILY_LG,
  TXT_FAMILY_DROID,
  TXT_FAMILY_N
} TxtFamily;

extern const char *const TXT_FAMILIES_PT[TXT_FAMILY_N];

// Instrumentation: how many lines were RASTERISED (rather than coming from the
// cache) in the frame, and what that cost. Rasterising text is the most
// expensive operation inside a frame, and without a counter there is no telling
// whether a jank came from there or from a texture upload.
extern int    txt_rasterized;
// Evictions from the line cache. Anything other than zero with the screen idle
// means the table does not fit what the screen draws, and the text flickers.
extern int    txt_evictions;
extern double txt_ms;

// `dirAssets` is the folder containing fonts/. On the device it is the app's
// folder; on the Mac, the package's — without this parameter the font was only
// looked for next to the executable, and running locally fell straight to the
// fallback.
// `scale` is the ratio between the buffer and the layout canvas (2 on a 4K TV, 1
// at 1080p). The fonts are opened at that size and the returned line still
// measures in layout units — see text.c.
int  txt_start(const char *dirAssets, float scale);
void txt_shutdown(void);

// Returns a cached line. Colour in 0..255. Never returns NULL; on failure,
// w/h = 0.
// Resets the frame's rasterisation budget. Call once per frame, before drawing;
// without it the budget runs out and the text disappears.
void txt_new_frame(void);

TxtLine txt_line(TxtStyle style, const char *s, int r, int g, int b, int a);

// Like txt_line, but picks one of the families that are safe for subtitles. If
// the system font does not exist (in the Mac preview, for instance), it falls
// back to the embedded Inter and logs the fallback once.
TxtLine txt_line_family(TxtStyle style, const char *s, int r, int g,
                           int b, int a, TxtFamily family);

// A line that NEVER exceeds `maxW`: it trims by word (or by character, if a
// single word already overflows) and closes with "…". Content that comes from
// outside (an addon's name, a TMDB genre) has no guaranteed length, and without
// trimming it invades the neighbouring column — which is what showed up in the
// Top 10 and on the tracks sheet.
TxtLine txt_line_trim(TxtStyle style, const char *s, int r, int g, int b,
                         int a, float maxW);
TxtLine txt_line_trim_family(TxtStyle style, const char *s, int r, int g,
                                 int b, int a, float maxW, TxtFamily family);

// Desenha no canto superior esquerdo (x,y).
void txt_draw(TxtLine l, float x, float y);
void txt_draw_alpha(TxtLine l, float x, float y, float alpha);

// Draws with letter SPACING (tracking) and returns the total width. SDL_ttf has
// no tracking, and the tvOS page title depends on it: without the wide spacing
// the same text in capitals reads as shouting, not as a heading. Pass x = -1 to
// measure only, without drawing.
float txt_tracking(TxtStyle style, const char *s, int r, int g, int b,
                   float x, float y, float alpha, float tracking);

// Draws text WRAPPED into lines that fit `width`, returning the height used.
// Without this, any variable-length text (an episode synopsis, a title's name)
// spills into the neighbouring column — there is no "writing short enough" when
// the content comes from outside.
float txt_block(TxtStyle style, const char *s, int r, int g, int b,
                float x, float y, float width, float leading, float alpha, int maxLines);

// The same block, but RIGHT-ALIGNED: every line ends at `xRight`. The credits in
// the bottom-right corner need this — left-aligned, their edge comes out ragged
// against the card's margin.
float txt_block_dir(TxtStyle style, const char *s, int r, int g, int b,
                    float xDir, float y, float width, float leading,
                    float alpha, int maxLines);

#endif
