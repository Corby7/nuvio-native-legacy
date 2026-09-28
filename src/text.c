#include "text.h"
#include "gfx.h"
#include "layout.h"
#include "mark.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <ctype.h>
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

// The TTF files, READ ONCE and kept alive as long as the app lives: FreeType's
// faces read from them on demand, so freeing here means reading freed memory on
// the first new glyph. Indexed by fileOf: the three Display weights, then the
// three Text weights; only the ones a style uses are read (~400 KB each).
// `ownerWeight` marks which pointers are owned: weights pointing at the same file
// share the buffer and only one of them frees it.
#define TXT_FILES 6
static unsigned char *bytesWeight[TXT_FILES];
static size_t         sizeWeight[TXT_FILES];
static int            ownerWeight[TXT_FILES];
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

void txt_new_frame(void) { traceThisFrame = 0; txt_misses = 0; frameTxt++; }
static unsigned long lruClock = 1;
int    txt_rasterized = 0;
// How many lines have been EVICTED to make room for others. Zero is the healthy
// state. If it starts rising again with the screen still, the table has filled up
// again and the text will flicker — better to read that off a counter than to
// find out from the complaint of whoever is looking at the screen.
int    txt_evictions = 0;
int    txt_misses = 0;
double txt_ms = 0.0;

// The weight per style. Each weight is a real Inter FILE (Regular 400, Medium
// 500, Bold 700), in the Display or the text cut by size (NV_FT_DISPLAY_MIN).
// There is no repeated pass and no sub-pixel offset to fake a weight. Synthetic bold (TTF_SetFontStyle) only comes in on the FALLBACK
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

// WHICH CUT OF INTER. Inter Display is drawn for headings: tighter spacing and
// closed-up apertures that look right at 48 px and start to run together at 17
// from the couch. Inter (the text cut) is drawn for exactly those sizes. So a
// style under NV_FT_DISPLAY_MIN gets the text cut and the rest keep Display —
// the owner compared the two side by side and chose the split.
#define NV_FT_DISPLAY_MIN 28
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
  // The primary button's 600 stays MEDIUM by the optical rule written above. The
  // pill now has TWO grounds — #222 with white ink at rest, #f5f5f5 with #111 ink
  // focused — and Medium is the side of the choice that serves the focused state,
  // which is the one being read.
  { NV_FT_DETWEB_BTN, WEIGHT_MEDIUM },
  // The tooltip is the exception: the sheet says `font-weight: bold`, so this 700
  // is not a 600 being resolved and goes straight to Bold.
  { NV_FT_DETWEB_TIP, WEIGHT_BOLD   },
  // The picker's value is 600 on a DARK pill, so it goes Bold by the optical rule
  // above; the " · N Eps" tail beside it really is 400 and stays Regular.
  { NV_FT_DETWEB_SEA, WEIGHT_BOLD    },
  { NV_FT_DETWEB_SEA, WEIGHT_REGULAR },
  // An option is 500, and it is read on a LIGHT row as often as a dark one (the
  // focused one inverts). Medium is the value 500 itself, with nothing to resolve.
  { NV_FT_DETWEB_OPT, WEIGHT_MEDIUM  },
  { NV_FT_DETWEB_EPB, WEIGHT_BOLD    },   // 600, the tracked-out "EP 3"
  { NV_FT_DETWEB_EPM, WEIGHT_REGULAR },
  // The episode row's title is 600 on a dark ground: Bold, by the optical rule.
  { NV_FT_DETWEB_EPT, WEIGHT_BOLD    },
  { NV_FT_DETWEB_EPD, WEIGHT_REGULAR },
  { NV_FT_HERO_META, WEIGHT_MEDIUM  },   // .home-modern-hero-meta-line (21/500)
  { NV_FT_HERO_SIN,  WEIGHT_REGULAR },   // .home-hero-description (24/400)
  // BOLD, not Medium. The sheet says 600, and the optical rule at the top of this
  // table resolves a 600 on a DARK ground upwards: this is white type sitting on
  // whatever frame the film happens to be showing. It was Medium at 26px, which is
  // the one place in the table the rule was written and then not applied.
  { NV_FT_PG_CLOCK, WEIGHT_BOLD    },  // .player-clock (36/600)
  { NV_FT_PG_END,     WEIGHT_REGULAR },  // .player-ends-at (20/400)
  { NV_FT_PG_LABEL,  WEIGHT_MEDIUM  },  // .player-parental-label (22/600)
  // MEDIUM now, not Regular. It grew to 24 with the category beside it, and at
  // that size Regular white-on-video is the weight that disappears first — the same
  // optical rule the top of this table states, applied to the quieter half of the
  // pair rather than the loud one.
  { NV_FT_PG_SEV,    WEIGHT_MEDIUM  },  // .player-parental-severity / separator
  // The sources sheet is its ONLY caller (episodes.c takes TXT_PANEL_ITEM, not
  // this one), so the weight moved to Bold with that sheet's rebuild and no other
  // screen saw it. 36 -> 40 for the same reason: "Sources" is now the heading of
  // a table, with a count beside it, not a label over a list.
  { NV_FT_SRC_TITLE, WEIGHT_BOLD },   // "Sources", the sheet's heading
  { 24, WEIGHT_BOLD },                // episode/source inside the list
  { 28, WEIGHT_MEDIUM },              // title on the Continue Watching card
  { 23, WEIGHT_REGULAR },             // season and episode name
  // The remaining time on the Continue Watching card. It goes BOLD now that it has
  // no container under it (see resume.c): Medium was right on a plate, where the
  // plate carried the contrast; floating on shaded artwork the same weight reads
  // as thin, and thin white type on a frame is the first thing to disappear at the
  // distance a TV is watched from.
  { 20, WEIGHT_BOLD },                // time remaining on the card
  { 110, WEIGHT_BOLD },               // the real position in the ranking
  { 20, WEIGHT_REGULAR }, { 24, WEIGHT_REGULAR }, { 28, WEIGHT_REGULAR },
  { 32, WEIGHT_REGULAR }, { 36, WEIGHT_REGULAR }, { 40, WEIGHT_REGULAR },
  { 44, WEIGHT_REGULAR }, { 48, WEIGHT_REGULAR }, { 52, WEIGHT_REGULAR },
  { 56, WEIGHT_REGULAR }, { 60, WEIGHT_REGULAR }, { 64, WEIGHT_REGULAR },
  { 68, WEIGHT_REGULAR }, { 72, WEIGHT_REGULAR }, { 76, WEIGHT_REGULAR },
  { 80, WEIGHT_REGULAR },
  // The profile picker. 500 goes MEDIUM; 600 goes MEDIUM too, by the rule already
  // written above for the detail screen's button — the embedded Inter has only
  // Regular, Medium and Bold, and Bold at these sizes is 100 too heavy.
  //
  // TXT_PSEL_NAME_F is the exception and IS Bold. The web app changes the name's
  // weight from 500 to 600 on focus, and font-weight is not in that rule's
  // `transition` list, so it pops — with Medium on both sides the focused name
  // would differ only in colour, and the weight step is half the cue.
  { 48, WEIGHT_MEDIUM  },   // TXT_PSEL_TITLE
  { 36, WEIGHT_MEDIUM  },   // TXT_PSEL_SUB
  { 34, WEIGHT_MEDIUM  },   // TXT_PSEL_NAME
  { 34, WEIGHT_BOLD    },   // TXT_PSEL_NAME_F
  { 22, WEIGHT_MEDIUM  },   // TXT_PSEL_BADGE
  { 28, WEIGHT_MEDIUM  },   // TXT_PSEL_HINT
  // Rasterised at the RESTING size and scaled up by the focus, not the other way
  // round: three of the four cards on screen are at rest, and that is where the
  // glyph has to land on the pixel.
  { 77, WEIGHT_BOLD    },   // TXT_PSEL_INITIAL
  { 28, WEIGHT_BOLD    },   // TXT_PSEL_STAR
  // The Continue Watching card's copy, in the MODERN layout's sizes. The three
  // that were already here (TXT_CW_TITLE 28/500 and TXT_CW_META 23/400 doing
  // double duty) came from the CLASSIC block, which is roughly half the scale,
  // and they are still in use by the "Among friends" card — hence three new
  // entries rather than three edits.
  //
  // Both 600s go BOLD by the optical rule written at the top of this table:
  // light text over dark artwork, where Medium reads a weight too thin.
  { 30, WEIGHT_BOLD    },   // TXT_CWC_TITLE  .home-continue-title    30 / 600
  { 17, WEIGHT_BOLD    },   // TXT_CWC_KICKER .home-continue-kicker   17 / 600
  { 21, WEIGHT_REGULAR },   // TXT_CWC_SUB    .home-continue-subtitle 21 / 400
  // The transport row. The tooltip's sheet says `font-weight: bold` outright, so
  // this 700 is not a 600 being resolved and goes straight to Bold - the same
  // exception already recorded above for TXT_DETWEB_TIP, which is the same rule in
  // the detail screen.
  { 20, WEIGHT_BOLD    },   // TXT_PLR_TIP   .player-control-btn::after   20 / bold
  // The time readout's 600 goes BOLD by the optical rule at the top of this table:
  // it is light type floating on shaded video, where Medium reads a weight thin.
  { NV_FT_PLR_TIME, WEIGHT_BOLD },  // TXT_PLR_TIME  .player-time-label, elapsed
  // Both 500s are Medium outright — 500 IS Medium, there is nothing to resolve.
  { NV_FT_PLR_ENDS,  WEIGHT_MEDIUM },  // TXT_PLR_ENDS   .player-ends-at
  { NV_FT_PLR_META3, WEIGHT_MEDIUM },  // TXT_PLR_META3  .player-meta-tertiary
  // The readout's tail. Medium against the elapsed time's Bold: one step of weight
  // is the whole separation, so it has to be a real step and not 600 against 700.
  { NV_FT_PLR_TIME,  WEIGHT_MEDIUM },  // TXT_PLR_TIME_T .player-time-label tail
  // 700 outright, like the tooltip: the sheet says `font-weight: 700` for the pill,
  // it is not a 600 being resolved by the optical rule.
  { NV_FT_PLR_DELTA, WEIGHT_BOLD   },  // TXT_PLR_DELTA  .player-seek-delta
  // The stats panel. The value is Medium and the label a size down from it — the
  // sheet separates them by colour alone (60% against 92%), which on a plate over
  // moving picture is not enough on its own to tell a heading from a reading.
  { NV_FT_PLR_STAT,  WEIGHT_MEDIUM },  // TXT_PLR_STAT   .player-stats-value
  { NV_FT_PLR_STATL, WEIGHT_MEDIUM },  // TXT_PLR_STATL  .player-stats-label
  { NV_FT_PLR_BADGE, WEIGHT_BOLD   },  // TXT_PLR_BADGE  .player-stats-quality
  // The guide's category. 600 on a dark ground goes BOLD by the optical rule.
  { NV_FT_PLR_PG_CAT, WEIGHT_BOLD  },  // TXT_PLR_PG_CAT .player-parental-label
  // The track menus. 800 is Bold outright — there is no heavier face here, and the
  // panel title is the one line in it meant to be read from across the room.
  { NV_FT_TRK_TITLE,  WEIGHT_BOLD   },  // TXT_TRK_TITLE  .player-dialog-title
  // THE ONE STYLE IN THIS BLOCK THAT DOES NOT TAKE THE OPTICAL RULE. Every other
  // 600 in this table goes Bold because light type on a dark ground reads a weight
  // thin — but this label is the deliberately recessive half of a pair, set at 55%
  // white against a value at 100%, and Bold would undo in weight exactly what the
  // colour is doing. Medium against the value's Bold is one full step, which is
  // the same separation TXT_PLR_TIME_T buys for the same reason.
  { NV_FT_TRK_LABEL,  WEIGHT_MEDIUM },  // TXT_TRK_LABEL  .player-select-label
  { NV_FT_TRK_VALUE,  WEIGHT_BOLD   },  // TXT_TRK_VALUE  .player-select-value
  { NV_FT_TRK_OPT,    WEIGHT_BOLD   },  // TXT_TRK_OPT    .player-select-option-main
  { NV_FT_TRK_OPTSUB, WEIGHT_MEDIUM },  // TXT_TRK_OPTSUB .player-select-option-sub
  { NV_FT_TRK_STEP,   WEIGHT_BOLD   },  // TXT_TRK_STEP   .player-dialog-step
  // The jump-ahead prompts. The skip label is a 500 and stays Medium: it sits on
  // an opaque pill, not on shaded video, so the optical rule that lifts 600s to
  // Bold elsewhere in this table has nothing to correct for here.
  { NV_FT_SKIP,       WEIGHT_MEDIUM },  // TXT_SKIP        .player-skip-intro-label
  { NV_FT_NEXT_KICK,  WEIGHT_BOLD   },  // TXT_NEXT_KICK   .player-next-episode-kicker
  { NV_FT_NEXT_TITLE, WEIGHT_BOLD   },  // TXT_NEXT_TITLE  .player-next-episode-title
  // Bold on both pills, against the Medium rule for dark ink on a light pill:
  // the pair sit side by side, one inverted, and Medium on the white one read as
  // a lighter label than its neighbour rather than the same weight.
  { NV_FT_NEXT_PILL,  WEIGHT_BOLD   },  // TXT_NEXT_PILL   the card's action pills
  { NV_FT_NEXT_PILL,  WEIGHT_MEDIUM },  // TXT_NEXT_PILL_MED the resting Not now
  { NV_FT_NEXT_COUNT, WEIGHT_BOLD   },  // TXT_NEXT_COUNT  the card's countdown
  // The episode rail.
  { NV_FT_ERAIL_META,  WEIGHT_BOLD   }, // TXT_ERAIL_META   .player-episode-detail-meta
  { NV_FT_ERAIL_TITLE, WEIGHT_BOLD   }, // TXT_ERAIL_TITLE  .player-episode-detail-title
  { NV_FT_ERAIL_OVER,  WEIGHT_MEDIUM }, // TXT_ERAIL_OVER   .player-episode-detail-overview
  { NV_FT_ERAIL_CODE,  WEIGHT_BOLD   }, // TXT_ERAIL_CODE   .player-episode-code
  { NV_FT_ERAIL_PILL,  WEIGHT_BOLD   }, // TXT_ERAIL_PILL   .player-episode-current
  { NV_FT_ERAIL_CTITLE,WEIGHT_BOLD   }, // TXT_ERAIL_CTITLE .player-episode-card-title
  // The search screen. Both 500s are MEDIUM outright — 500 IS Medium, there is
  // nothing for the optical rule at the top of this table to resolve — and the
  // two 400s are Regular for the same reason. Nothing here is a 600.
  { NV_FT_SRCH_NAME,  WEIGHT_MEDIUM  },  // TXT_SRCH_NAME  .search-result-name
  { NV_FT_SRCH_META,  WEIGHT_REGULAR },  // TXT_SRCH_META  .search-results-subtitle
  { NV_FT_SRCH_EMPTY, WEIGHT_REGULAR },  // TXT_SRCH_EMPTY .search-empty-state p
  // The sources sheet. The chip label and the tier word are BOLD because they are
  // small caps on a dark ground, which is the optical rule at the top of this
  // table; the size is bold because it is the number the row is scanned for.
  { NV_FT_SRC_COUNT, WEIGHT_REGULAR },   // TXT_SRC_COUNT
  { NV_FT_SRC_TAB,   WEIGHT_BOLD    },   // TXT_SRC_TAB
  { NV_FT_SRC_CHIP,  WEIGHT_BOLD    },   // TXT_SRC_CHIP
  { NV_FT_SRC_TEXT,  WEIGHT_MEDIUM  },   // TXT_SRC_TEXT
  { NV_FT_SRC_META,  WEIGHT_MEDIUM  },   // TXT_SRC_META
  { NV_FT_SRC_SIZE,  WEIGHT_BOLD    },   // TXT_SRC_SIZE
  { NV_FT_SRC_TIER,  WEIGHT_BOLD    },   // TXT_SRC_TIER
  { NV_FT_SRC_META,  WEIGHT_BOLD    },   // TXT_SRC_STATE
  // The player's title block.
  { NV_FT_PLR_EP,    WEIGHT_BOLD    },   // TXT_PLR_EPCODE
  { NV_FT_PLR_EP,    WEIGHT_MEDIUM  },   // TXT_PLR_EPNAME
  { NV_FT_PLR_QUICK, WEIGHT_BOLD    },   // TXT_PLR_QUICK
  { NV_FT_LIB_TAB,   WEIGHT_BOLD    },   // TXT_LIB_TAB
  // MEDIUM, not the season picker's Bold: set 30 on an #222 pill, Bold read as
  // shouting next to the page's other controls.
  { NV_FT_DD_SEL,    WEIGHT_MEDIUM  },   // TXT_DD_SEL
  { NV_FT_MENU,      WEIGHT_REGULAR },   // TXT_MENU_ITEM
  { NV_FT_MENU,      WEIGHT_BOLD    },   // TXT_MENU_SEL
  // The pause overlay. Light type over a darkened blur, so the optical rule at the
  // top of this table applies throughout; only the synopsis stays Regular, being
  // the one block meant to be read rather than glanced at.
  { NV_FT_PAUSE_TITLE, WEIGHT_BOLD    }, // TXT_PAUSE_TITLE
  { NV_FT_PAUSE_META,  WEIGHT_MEDIUM  }, // TXT_PAUSE_META
  { NV_FT_PAUSE_EP,    WEIGHT_BOLD    }, // TXT_PAUSE_EP
  { NV_FT_PAUSE_SIN,   WEIGHT_REGULAR }, // TXT_PAUSE_SIN
  { NV_FT_PAUSE_CLOCK, WEIGHT_MEDIUM  }, // TXT_PAUSE_CLOCK
  { 42, WEIGHT_BOLD    },   // TXT_LIVE_TITLE
  { 48, WEIGHT_BOLD    },   // TXT_LIVE_TITLE_F
  { 19, WEIGHT_MEDIUM  },   // TXT_LIVE_META
  { 19, WEIGHT_BOLD    },   // TXT_LIVE_META_B
  { 21, WEIGHT_MEDIUM  },   // TXT_LIVE_NAME
  { 17, WEIGHT_MEDIUM  },   // TXT_LIVE_NOTE
  { 16, WEIGHT_MEDIUM  },   // TXT_LIVE_TIME
  { 13, WEIGHT_BOLD    },   // TXT_LIVE_TAG
  { 25, WEIGHT_BOLD    },   // TXT_SRCP_NAME
  { 12, WEIGHT_BOLD    },   // TXT_SRCP_BADGE
  { 28, WEIGHT_BOLD    },   // TXT_SRCP_NUM
  { 16, WEIGHT_BOLD    },   // TXT_SRCP_SECTION
  { 24, WEIGHT_BOLD    },   // TXT_SRCP_STEP
  { 22, WEIGHT_BOLD    },   // TXT_SRCP_ADDR
  { 30, WEIGHT_BOLD    },   // TXT_SRCP_CODE
  { 17, WEIGHT_REGULAR },   // TXT_SRCP_DESC
  { 23, WEIGHT_BOLD    },   // TXT_SRCP_VALUE
  { 18, WEIGHT_REGULAR },   // TXT_SRCP_PATH
  { 16, WEIGHT_REGULAR },   // TXT_SRCP_NOTE
  { 21, WEIGHT_BOLD    },   // TXT_SRCP_PRIMARY
  { 21, WEIGHT_MEDIUM  },   // TXT_SRCP_GHOST
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

// A character that DRAWS NOTHING: the bidi marks, the zero-width joiners and the
// soft hyphen. They are invisible by definition, so they must never be what
// decides which font a line is drawn in.
//
// This is not hypothetical. A collection whose title is "\u200EDiscover" — a
// LEFT-TO-RIGHT MARK before the D, which the web app keeps in the name and no
// screen has ever shown — had a first non-ASCII codepoint that Inter does not
// provide, so fontOf sent the WHOLE row header to the script fallback: a
// regular-weight face with none of Inter Bold's shapes. On the home the row read
// as if somebody had un-bolded that one title.
static int invisibleFormat(Uint32 cp) {
  return cp == 0x00AD                      // soft hyphen
      || (cp >= 0x200B && cp <= 0x200F)    // ZWSP, ZWNJ, ZWJ, LRM, RLM
      || (cp >= 0x202A && cp <= 0x202E)    // bidi embedding and override
      || (cp >= 0x2060 && cp <= 0x2064)    // word joiner and the invisible operators
      || (cp >= 0x2066 && cp <= 0x2069)    // bidi isolates
      || cp == 0xFEFF;                     // zero-width no-break space (BOM)
}

// Copies `s` into `out` WITHOUT the characters that draw nothing, and returns the
// copy. When there is nothing to remove — nearly every line the app ever draws —
// it returns `s` ITSELF and copies not one byte: this runs for every line of every
// frame and must not turn into a memcpy of the whole screen's text.
//
// SKIPPING THEM IN firstNotAscii WAS NOT ENOUGH. That put "\u200EDiscover" back
// into Inter Display Bold, which is right, and Inter has NO GLYPH for U+200E — so
// the mark that had been invisible in the fallback face came out as .notdef, a
// box in front of the D. A character that is defined to draw nothing must not
// reach the rasteriser at all; picking the right font for it was only half the
// job.
//
// A string that does not fit `out` is returned UNTOUCHED: truncating it would eat
// the end of a synopsis to remove a mark, which is a far worse trade. At 1024 the
// only callers near the limit are the subtitle lines, and those come from files
// that have no reason to carry bidi marks.
static const char *withoutInvisible(const char *s, char *out, size_t n) {
  const unsigned char *p = (const unsigned char *)s;
  size_t k = 0;
  int found = 0;
  for (; *p; p++) {
    Uint32 cp;
    int len;
    if (*p < 0x80) continue;
    if      ((*p & 0xE0) == 0xC0 && p[1]) { cp = (Uint32)((*p & 0x1F) << 6 | (p[1] & 0x3F)); len = 2; }
    else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) { cp = (Uint32)((*p & 0x0F) << 12 | (p[1] & 0x3F) << 6 | (p[2] & 0x3F)); len = 3; }
    else continue;   // 4-byte sequences are emoji; nothing invisible lives there
    if (invisibleFormat(cp)) { found = 1; break; }
    p += len - 1;
  }
  if (!found) return s;
  if (strlen(s) >= n) return s;
  for (p = (const unsigned char *)s; *p; ) {
    Uint32 cp;
    int len = 1;
    if      ((*p & 0xF8) == 0xF0) len = 4;
    else if ((*p & 0xF0) == 0xE0) len = 3;
    else if ((*p & 0xE0) == 0xC0) len = 2;
    // A truncated sequence at the end of the buffer: copy the bytes as they are
    // and let TTF answer for them, exactly as it did before this function existed.
    { int i = 0;
      while (i < len && p[i]) i++;
      if (i < len) len = i ? i : 1; }
    cp = len == 2 ? (Uint32)((p[0] & 0x1F) << 6 | (p[1] & 0x3F))
       : len == 3 ? (Uint32)((p[0] & 0x0F) << 12 | (p[1] & 0x3F) << 6 | (p[2] & 0x3F))
       : 0;
    if (!(len == 2 || len == 3) || !invisibleFormat(cp)) {
      int i;
      for (i = 0; i < len; i++) out[k++] = (char)p[i];
    }
    p += len;
  }
  out[k] = 0;
  return out;
}

// A DECORATIVE codepoint: symbol, arrow, pictogram, emoji, variation selector.
// It belongs to no script, so it must never be what decides which font the WHOLE
// LINE is drawn in.
//
// WHY THIS EXISTS, with Inter's coverage MEASURED rather than assumed.
//
// fontOf sends the entire line to the fallback face when the first non-ASCII
// character is not in Inter. That rule is right for SCRIPT — "Deadpool &
// ウルヴァリン" comes out legible in DroidSansFallback — and wrong for SYMBOL.
// The stream names addons return are full of decoration, and a single symbol
// Inter lacks was enough to flip the whole Sources sheet to DroidSansFallback,
// which is a CJK face: its Latin is heavier and hinted differently, and on
// screen that reads exactly as the report did — "all the text appears in bold,
// the information looks pixelated". It was also extra work (opening a second
// font file per style and rasterising with a much larger face) on the very
// screen the same report calls slow.
//
// CHECKED across the three embedded InterDisplay weights with TTF_GlyphIsProvided:
//   HAS      arrows, bullet, ellipsis, en/em dash, curly quotes, star, play, tick, middot
//   HAS NOT  high voltage, gear, glowing star
// So what brought the line down were the ones Inter does not have, and the high
// voltage sign is the most common of those in Torrentio and AIOStreams names.
// The arrows and the play triangle the INTERFACE itself uses in its own labels
// always passed, and still pass: the decision below is per available glyph, not
// per codepoint range.
//
// Emoji had the OTHER symptom, not this one: fontOf returns the main font for a
// codepoint outside the BMP (TTF_GlyphIsProvided takes a Uint16 and cannot reach
// them), so those never changed the line's font — they came out as the .notdef
// box. Both cases die here.
static int decorative(Uint32 cp) {
  if (cp >= 0x2000  && cp <= 0x2BFF)  return 1;  // punctuation, arrows, symbols, dingbats
  if (cp >= 0x2E00  && cp <= 0x2E7F)  return 1;  // supplemental punctuation
  if (cp >= 0xFE00  && cp <= 0xFE0F)  return 1;  // variation selectors
  if (cp >= 0x1F000 && cp <= 0x1FAFF) return 1;  // emoji and pictographs
  return 0;
}

// Decodes one UTF-8 codepoint and says how many bytes it took. An invalid
// sequence returns the raw byte with a length of 1, so the loop can never stall.
static Uint32 decodeCp(const unsigned char *p, int *n) {
  if (*p < 0x80) { *n = 1; return *p; }
  if ((*p & 0xE0) == 0xC0 && p[1]) { *n = 2; return (Uint32)((*p & 0x1F) << 6 | (p[1] & 0x3F)); }
  if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) { *n = 3;
    return (Uint32)((*p & 0x0F) << 12 | (p[1] & 0x3F) << 6 | (p[2] & 0x3F)); }
  if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) { *n = 4;
    return (Uint32)((*p & 0x07) << 18 | (p[1] & 0x3F) << 12 |
                    (p[2] & 0x3F) << 6 | (p[3] & 0x3F)); }
  *n = 1; return *p;
}

// Takes out of the line the decorative codepoints the main font DOES NOT HAVE,
// and only those.
//
// Without this there were two bad outcomes and no good one: keep the character
// and draw the .notdef box (which is what happened to emoji), or swap the font
// for the entire line (which is what happened to BMP symbols). Removing is the
// third: "<bolt> 1080p · 4.2 GB" stays "1080p · 4.2 GB", in Inter, with no box.
//
// What the font HAS stays: en dash, ellipsis, curly quotes and the middot itself
// are in Inter and pass through untouched — the decision is per available glyph,
// not per range. Emoji outside the BMP always go: TTF_GlyphIsProvided takes a
// Uint16 and cannot reach those codepoints, and none of this app's faces has them.
//
// COST: one pass per line, and only when the line has a non-ASCII byte — the
// common path leaves on the first comparison. The rasterised line is cached, but
// the cache KEY is built from the already-cleaned text, so this pass runs every
// frame; that is why the early exit matters.
static const char *withoutDecorativeNoGlyph(TxtStyle style, const char *s,
                                            char *dst, size_t n) {
  const unsigned char *p = (const unsigned char *)s;
  size_t k = 0;
  int any = 0;
  // No face, no glyph question to ask — and TTF_GlyphIsProvided would be handed
  // a NULL font. txt_tracking reaches here without the style check its callers
  // in lineFamily and widthOf already made.
  if (style < 0 || style >= TXT_NFONTS || !fonts[style]) return s;
  for (; *p; p++) if (*p >= 0x80) { any = 1; break; }
  if (!any) return s;
  // A line longer than the buffer is left exactly as it is. Cutting visible text
  // to fit a buffer of mine would trade a cosmetic defect for lost information.
  if (strlen(s) + 1 > n) return s;
  p = (const unsigned char *)s;
  while (*p) {
    int len = 1;
    Uint32 cp = decodeCp(p, &len);
    int drop = 0;
    if (decorative(cp))
      drop = cp >= 0x10000 || !TTF_GlyphIsProvided(fonts[style], (Uint16)cp);
    if (!drop) { int i; for (i = 0; i < len; i++) dst[k++] = (char)p[i]; }
    p += len;
  }
  dst[k] = 0;
  // A double space, or one at either edge, is a leftover of what was removed —
  // not of the text.
  { size_t r = 0, w = 0;
    while (dst[r] == ' ') r++;
    for (; dst[r]; r++) {
      if (dst[r] == ' ' && w && dst[w - 1] == ' ') continue;
      dst[w++] = dst[r];
    }
    while (w && dst[w - 1] == ' ') w--;
    dst[w] = 0; }
  return dst;
}

// The first codepoint OUTSIDE ASCII that could need another font, or 0. It decodes
// UTF-8 by hand because it is the only point in the app that needs it and pulling
// in a library for three lines does not pay.
//
// Skipping the invisible characters means the loop can no longer return on the
// first sequence it meets, so it has to ADVANCE past the continuation bytes: read
// as single bytes they look like the start of nothing and the scan would answer
// with rubbish.
static Uint32 firstNotAscii(const char *s) {
  const unsigned char *p = (const unsigned char *)s;
  for (; *p; p++) {
    Uint32 cp;
    if (*p < 0x80) continue;
    if ((*p & 0xE0) == 0xC0 && p[1]) {
      cp = (Uint32)((*p & 0x1F) << 6 | (p[1] & 0x3F));
      p += 1;
    } else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
      cp = (Uint32)((*p & 0x0F) << 12 | (p[1] & 0x3F) << 6 | (p[2] & 0x3F));
      p += 2;
    } else if ((*p & 0xF8) == 0xF0) {
      return 0x10000;   // outside the BMP: we do not handle it
    } else {
      return 0;
    }
    // An emoji joiner (U+200D) inside a 4-byte sequence never reaches here: the
    // branch above answers for the whole thing. What is skipped here is a mark
    // standing on its own, and a line made ONLY of those needs no fallback at all.
    if (!invisibleFormat(cp)) return cp;
  }
  return 0;
}

// Past the first non-ASCII character of `s` that is `cp` (as firstNotAscii
// found it): where the next look starts.
static const char *nextChar(const char *s, Uint32 cp) {
  const unsigned char *p = (const unsigned char *)s;
  for (; *p; p++) {
    Uint32 c;
    int len;
    if (*p < 0x80) continue;
    if      ((*p & 0xE0) == 0xC0 && p[1]) { c = (Uint32)((*p & 0x1F) << 6 | (p[1] & 0x3F)); len = 2; }
    else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) { c = (Uint32)((*p & 0x0F) << 12 | (p[1] & 0x3F) << 6 | (p[2] & 0x3F)); len = 3; }
    else return s + strlen(s);
    if (c == cp) return (const char *)p + len;
    p += len - 1;
  }
  return (const char *)p;
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

// RIGHT-TO-LEFT LINES IN VISUAL ORDER. SDL_ttf lays every string out left to
// right, so a Hebrew or Arabic title came out mirrored: "חדשות הספורט" read as
// "טרופסה תושדח". This is the part of the Unicode bidi algorithm a single line
// of a title needs: letters are strong right-to-left (Hebrew, Arabic) or strong
// left-to-right (Latin and the rest, and digits, which keep their own order);
// punctuation and spaces take the direction of the letters either side of them,
// or the line's when those disagree; the line's direction is its first strong
// letter's. Digits are the one wrinkle: they read left to right inside a
// right-to-left run, and count as right-to-left for the punctuation around them
// unless a left-to-right letter came before (rules W7, N1, I1). Then each run is reversed per the levels (rule L2), and brackets in
// a right-to-left run are mirrored. Arabic letters are not joined here: that
// needs a shaper this build does not have.
//
// A line with nothing right-to-left in it comes back untouched, and so does one
// too long for `out`.
static int rtlLetter(Uint32 cp) {
  return (cp >= 0x0590 && cp <= 0x08FF) || (cp >= 0xFB1D && cp <= 0xFDFF) || (cp >= 0xFE70 && cp <= 0xFEFC);
}
static int neutralChar(Uint32 cp) {
  if (cp < 0x80) return !isalnum((int)cp);
  return (cp >= 0x0080 && cp <= 0x00BF) || cp == 0x00D7 || cp == 0x00F7 ||
         (cp >= 0x2000 && cp <= 0x2BFF) || (cp >= 0x3000 && cp <= 0x303F) || cp == 0xFEFF;
}
static Uint32 mirrored(Uint32 cp) {
  switch (cp) {
    case '(': return ')'; case ')': return '(';
    case '[': return ']'; case ']': return '[';
    case '{': return '}'; case '}': return '{';
    case '<': return '>'; case '>': return '<';
    case 0x00AB: return 0x00BB; case 0x00BB: return 0x00AB;
    default: return cp;
  }
}
#define BIDI_MAX 512
static const char *visualOrder(const char *s, char *out, size_t n) {
  Uint32 cp[BIDI_MAX];
  unsigned char cls[BIDI_MAX], lev[BIDI_MAX];   // cls: 0 L, 1 R, 2 neutral, 3 digit
  const unsigned char *p = (const unsigned char *)s;
  int m = 0, any = 0, para = 0, maxLev = 0;
  size_t k = 0;
  while (*p) {
    Uint32 c;
    int len;
    if (m == BIDI_MAX) return s;
    if      (*p < 0x80) { c = *p; len = 1; }
    else if ((*p & 0xE0) == 0xC0 && p[1]) { c = (Uint32)((*p & 0x1F) << 6 | (p[1] & 0x3F)); len = 2; }
    else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) { c = (Uint32)((*p & 0x0F) << 12 | (p[1] & 0x3F) << 6 | (p[2] & 0x3F)); len = 3; }
    else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
      c = (Uint32)((*p & 0x07) << 18 | (p[1] & 0x3F) << 12 | (p[2] & 0x3F) << 6 | (p[3] & 0x3F)); len = 4;
    }
    else return s;
    cp[m] = c;
    cls[m] = rtlLetter(c) ? 1 : c >= '0' && c <= '9' ? 3 : neutralChar(c) ? 2 : 0;
    if (cls[m] == 1) any = 1;
    m++;
    p += len;
  }
  if (!any) return s;
  for (int i = 0; i < m; i++) if (cls[i] < 2) { para = cls[i]; break; }
  // W7: digits after a left-to-right letter are simply left-to-right.
  { int strong = para;
    for (int i = 0; i < m; i++) {
      if (cls[i] < 2) strong = cls[i];
      else if (cls[i] == 3 && strong == 0) cls[i] = 0;
    } }
  // Neutrals: the class either side when they agree (a digit counting as
  // right-to-left), else the line's.
  for (int i = 0; i < m; ) {
    int j = i, before, after;
    if (cls[i] != 2) { i++; continue; }
    while (j < m && cls[j] == 2) j++;
    before = i > 0 ? (cls[i - 1] == 3 ? 1 : cls[i - 1]) : para;
    after = j < m ? (cls[j] == 3 ? 1 : cls[j]) : para;
    for (int t = i; t < j; t++) cls[t] = (unsigned char)(before == after ? before : para);
    i = j;
  }
  for (int i = 0; i < m; i++) {
    lev[i] = (unsigned char)(cls[i] == 3 ? 2 : para ? (cls[i] ? 1 : 2) : (cls[i] ? 1 : 0));
    if (lev[i] > maxLev) maxLev = lev[i];
  }
  for (int L = maxLev; L >= 1; L--)
    for (int i = 0; i < m; ) {
      int j = i;
      if (lev[i] < L) { i++; continue; }
      while (j < m && lev[j] >= L) j++;
      for (int a = i, b = j - 1; a < b; a++, b--) {
        Uint32 tc = cp[a]; unsigned char tl = lev[a];
        cp[a] = cp[b]; lev[a] = lev[b];
        cp[b] = tc; lev[b] = tl;
      }
      i = j;
    }
  for (int i = 0; i < m; i++) {
    Uint32 c = lev[i] & 1 ? mirrored(cp[i]) : cp[i];
    if (k + 5 > n) return s;
    if (c < 0x80) out[k++] = (char)c;
    else if (c < 0x800) { out[k++] = (char)(0xC0 | c >> 6); out[k++] = (char)(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { out[k++] = (char)(0xE0 | c >> 12); out[k++] = (char)(0x80 | (c >> 6 & 0x3F)); out[k++] = (char)(0x80 | (c & 0x3F)); }
    else { out[k++] = (char)(0xF0 | c >> 18); out[k++] = (char)(0x80 | (c >> 12 & 0x3F));
           out[k++] = (char)(0x80 | (c >> 6 & 0x3F)); out[k++] = (char)(0x80 | (c & 0x3F)); }
  }
  out[k] = 0;
  return out;
}

const char *txt_visual_order(const char *s, char *out, size_t n) { return visualOrder(s, out, n); }

// The font line `s` should be drawn with. It returns the main one when that
// copes — which is the case for the overwhelming majority of lines.
static TTF_Font *fontOf(TxtStyle style, const char *s) {
  Uint32 cp = 0;
  Script e;
  // The first character Inter LACKS decides, not merely the first that is not
  // ASCII: "Next · <a Hebrew title>" met the middle dot first, which Inter has,
  // and the Hebrew after it came out as a row of boxes. Portuguese and Spanish
  // accents, the dot and the dashes are all Inter's; only what it really lacks
  // falls through to the fallback.
  for (const char *p = s; (cp = firstNotAscii(p)) != 0; ) {
    if (cp >= 0x10000) return fonts[style];
    if (!TTF_GlyphIsProvided(fonts[style], (Uint16)cp)) break;
    p = nextChar(p, cp);
  }
  if (!cp) return fonts[style];
  e = scriptOf(cp);
  if (!pathFallback[e][0]) return fonts[style];
  if (!fallbacks[e][style]) {
    fallbacks[e][style] = TTF_OpenFont(pathFallback[e],
                                       (int)(STYLES[style].body * scaleTxt + 0.5f));
    // THE FALLBACK FILES COME IN ONE WEIGHT — DroidSansFallback has no Bold, and
    // neither has the Mac's Arial Unicode. Without this a Cyrillic or CJK title in
    // a Bold style came out regular beside its Latin neighbours: the same mismatch
    // the invisible mark above produced, except here for text that really does need
    // the fallback. Synthetic bold is what txt_start already does for the LG and
    // Droid families, for the same reason and with the same reservations.
    if (fallbacks[e][style] && STYLES[style].weight == WEIGHT_BOLD)
      TTF_SetFontStyle(fallbacks[e][style], TTF_STYLE_BOLD);
  }
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

// The file style `i` opens: its weight, in the Display cut or, under
// NV_FT_DISPLAY_MIN, the text cut three slots on.
static int fileOf(int i) {
  return STYLES[i].weight + (STYLES[i].body < NV_FT_DISPLAY_MIN ? 3 : 0);
}

// FACES AT ANY SIZE, for txt_line_px. A style's face is opened once at the
// style's own size; a miniature (the settings previews) needs the same weights
// at a fraction of it. Opened on demand from the bytes already in memory and kept:
// a preview asks for a handful of sizes and asks for them every frame.
#define TXT_SIZED_N 16
static struct { int file, px; TTF_Font *font; SDL_RWops *rw; } sized[TXT_SIZED_N];
static int fontFallback;

static TTF_Font *sizedFont(TxtStyle style, int px) {
  // Under the Display cut's floor the Text cut, as fileOf does for a style.
  int file = STYLES[style].weight + (px < NV_FT_DISPLAY_MIN ? 3 : 0), i;
  for (i = 0; i < TXT_SIZED_N && sized[i].font; i++)
    if (sized[i].file == file && sized[i].px == px) return sized[i].font;
  if (i == TXT_SIZED_N || !bytesWeight[file]) return NULL;
  sized[i].rw = SDL_RWFromConstMem(bytesWeight[file], (int)sizeWeight[file]);
  if (!sized[i].rw) return NULL;
  sized[i].font = TTF_OpenFontRW(sized[i].rw, 0, (int)(px * scaleTxt + 0.5f));
  if (!sized[i].font) { SDL_FreeRW(sized[i].rw); sized[i].rw = NULL; return NULL; }
  if (fontFallback && STYLES[style].weight == WEIGHT_BOLD)
    TTF_SetFontStyle(sized[i].font, TTF_STYLE_BOLD);
  sized[i].file = file;
  sized[i].px = px;
  return sized[i].font;
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

  char inter[TXT_FILES][512];
  snprintf(inter[WEIGHT_REGULAR], 512, "%sfonts/InterDisplay-Regular.ttf", base);
  snprintf(inter[WEIGHT_MEDIUM],  512, "%sfonts/InterDisplay-Medium.ttf",  base);
  snprintf(inter[WEIGHT_BOLD],    512, "%sfonts/InterDisplay-Bold.ttf",    base);
  snprintf(inter[3 + WEIGHT_REGULAR], 512, "%sfonts/Inter-Regular.ttf", base);
  snprintf(inter[3 + WEIGHT_MEDIUM],  512, "%sfonts/Inter-Medium.ttf",  base);
  snprintf(inter[3 + WEIGHT_BOLD],    512, "%sfonts/Inter-Bold.ttf",    base);

  const char *lg[3] = { "/usr/share/fonts/LG_Display-Light.ttf",
                        "/usr/share/fonts/LG_Display-Regular.ttf",
                        "/usr/share/fonts/LG_Display-Regular.ttf" };
  const char *droid[3] = { "/usr/share/fonts/DroidSans.ttf",
                           "/usr/share/fonts/DroidSans.ttf",
                           "/usr/share/fonts/DroidSans.ttf" };

  // LG and Droid have one cut: their "text" files are the same three again, and
  // the read below notices the repeat and shares the buffer.
  const char *families[3][TXT_FILES] = {
    { inter[0], inter[1], inter[2], inter[3], inter[4], inter[5] },
    { lg[0], lg[1], lg[2], lg[0], lg[1], lg[2] },
    { droid[0], droid[1], droid[2], droid[0], droid[1], droid[2] },
  };
  int need[TXT_FILES] = {0};
  for (int i = 0; i < TXT_NFONTS; i++) need[fileOf(i)] = 1;
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
    for (int p = 0; p < TXT_FILES; p++) {
      int j;
      if (!need[p]) continue;
      // LG repeats Regular across two weights and Droid across all three: do not read again.
      for (j = 0; j < p; j++)
        if (need[j] && !strcmp(families[c][p], families[c][j])) break;
      if (j < p) { bytesWeight[p] = bytesWeight[j]; sizeWeight[p] = sizeWeight[j]; ownerWeight[p] = 0; continue; }
      bytesWeight[p] = readAll(families[c][p], &sizeWeight[p]);
      ownerWeight[p] = bytesWeight[p] ? 1 : 0;
      if (!bytesWeight[p]) { all = 0; break; }
    }
    if (all)
      for (int i = 0; i < TXT_NFONTS; i++) {
        int weight = fileOf(i);
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
      { int reads = 0;
        for (int p = 0; p < TXT_FILES; p++) reads += ownerWeight[p];
        printf("font: %s (%d styles, %d reads)\n", names[c], TXT_NFONTS, reads); }
      fontFallback = c > 0;
      mark("fonts: ready");
      return 1;
    }
    for (int i = 0; i < TXT_NFONTS; i++) {
      if (fonts[i]) TTF_CloseFont(fonts[i]);
      fonts[i] = NULL;
      if (rwSource[i]) SDL_FreeRW(rwSource[i]);
      rwSource[i] = NULL;
    }
    for (int p = 0; p < TXT_FILES; p++) {
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
  for (int i = 0; i < TXT_SIZED_N; i++) {
    if (sized[i].font) TTF_CloseFont(sized[i].font);
    if (sized[i].rw) SDL_FreeRW(sized[i].rw);
    sized[i].font = NULL; sized[i].rw = NULL;
  }
  for (int i = 0; i < TXT_NFONTS; i++) {
    if (fonts[i]) TTF_CloseFont(fonts[i]);
    fonts[i] = NULL;
    if (rwSource[i]) SDL_FreeRW(rwSource[i]);
    rwSource[i] = NULL;
    for (int e = 0; e < SCRIPT_N; e++)
      if (fallbacks[e][i]) TTF_CloseFont(fallbacks[e][i]);
  }
  for (int p = 0; p < TXT_FILES; p++) {
    if (ownerWeight[p]) free(bytesWeight[p]);
    bytesWeight[p] = NULL; sizeWeight[p] = 0; ownerWeight[p] = 0;
  }
  for (int f = 1; f < TXT_FAMILY_N; f++)
    for (int i = 0; i < TXT_SUB_N; i++)
      if (subFontsHeight[f][i]) TTF_CloseFont(subFontsHeight[f][i]);
  TTF_Quit();
}

// `px` > 0 sets the style's weight at that size instead of its own (txt_line_px).
static TxtLine lineFamily(TxtStyle style, const char *s, int r, int g,
                             int b, int a, TxtFamily family, int px) {
  TxtLine empty = {0, 0, 0};
  char clean[1024], clean2[1024];
  if (!s || !*s || style < 0 || style >= TXT_NFONTS || !fonts[style]) return empty;

  // HERE AND IN widthOf, and nowhere else: those two are what every public entry
  // point funnels through, so cleaning at both keeps the measurement and the
  // rasterisation looking at the SAME string — a line stripped for drawing but
  // measured raw would wrap at the wrong word. It also means "\u200EDiscover" and
  // "Discover" share one cache entry instead of two.
  s = withoutInvisible(s, clean, sizeof clean);
  if (!*s) return empty;   // nothing but invisible marks: nothing to draw

  if (family < TXT_FAMILY_INTER || family >= TXT_FAMILY_N)
    family = TXT_FAMILY_INTER;

  // AND THE DECORATIVE CHARACTERS THE FACE HAS NO GLYPH FOR — before the cache
  // key, so the line that is stored is the line that is drawn: two entries
  // differing only by an emoji nobody renders become one, which also relieves
  // the table on the Sources screen.
  //
  // ONLY ON THE MAIN FAMILY. The test asks fonts[style] whether the glyph
  // exists, and the subtitle families draw from subFontsHeight instead: applying
  // it there would strip a subtitle of a symbol ITS OWN face has. The screen
  // that motivated this — the source list — is all Inter.
  if (family == TXT_FAMILY_INTER) {
    s = withoutDecorativeNoGlyph(style, s, clean2, sizeof clean2);
    if (!*s) return empty;
  }

  char key[288];
  snprintf(key, sizeof key, "%d:%d:%d|%02x%02x%02x|%.230s", (int)family,
           (int)style, px, r & 255, g & 255, b & 255, s);

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

  // NOT IN THE CACHE. Counted BEFORE the budget check, because a line the budget
  // refused is just as absent from the screen as one that has not been asked for
  // — and a caller waiting for its block to settle has to see both. See
  // txt_misses.
  txt_misses++;

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
  TTF_Font *face = px > 0 ? sizedFont(style, px) : subtitleFontOf(style, s, family);
  if (!face) return empty;
  // Drawn in visual order; cached, measured and trimmed in logical order.
  char vis[1024];
  SDL_Surface *sf = TTF_RenderUTF8_Blended(face, visualOrder(s, vis, sizeof vis), color);
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

// --- WHERE THE INK SITS INSIDE A LINE ----------------------------------------
//
// See the note in text.h. Both answers are pure face metrics, so they are asked
// for once per style and kept: TTF_GlyphMetrics walks the outline.
static float capInset[TXT_NFONTS], baseline[TXT_NFONTS];
static char  inkKnown[TXT_NFONTS];
static void inkOf(TxtStyle style) {
  TTF_Font *f;
  int minx, maxx, miny, maxy, adv;
  if (style < 0 || style >= TXT_NFONTS || inkKnown[style]) return;
  f = fontOf(style, "H");
  if (!f) return;               // not open yet: ask again next frame
  inkKnown[style] = 1;
  baseline[style] = (float)TTF_FontAscent(f) / scaleTxt;
  if (TTF_GlyphMetrics(f, 'H', &minx, &maxx, &miny, &maxy, &adv) == 0)
    capInset[style] = (float)(TTF_FontAscent(f) - maxy) / scaleTxt;
}
float txt_cap_inset(TxtStyle style) {
  if (style < 0 || style >= TXT_NFONTS) return 0.0f;
  inkOf(style); return capInset[style];
}
float txt_baseline(TxtStyle style) {
  if (style < 0 || style >= TXT_NFONTS) return 0.0f;
  inkOf(style); return baseline[style];
}

// --- MEASURING WITHOUT RASTERISING -------------------------------------------
//
// txt_block looked for its wrap point by calling txt_line on EVERY cumulative
// prefix of every line: ~30 GL textures per synopsis, created and thrown away
// after comparing a single number. With TXT_PER_FRAME at 2 the block needed
// fifteen frames to settle, and that was visible — the hero's description typed
// itself in, two lines at a time. Worse, a prefix the budget refused came back
// with w = 0, so the fit test passed and the wrap was WRONG on those frames: the
// text also reflowed as it settled.
//
// TTF_SizeUTF8 answers the same question with no surface, no GL object and no
// budget. Only the lines that are actually DRAWN go through lineFamily now, and
// those are four, not thirty-four.
//
// The cache is here because the same prefix is asked for every frame — the hero
// lays its copy out twice per frame, once to measure the block's height and once
// to draw it — and walking the glyph metrics of a whole line is not free on this
// CPU. An entry costs no GL object, so it is NOT budgeted: refusing a
// measurement is what caused the defect in the first place.
#define MAX_MEASURES 512
typedef struct {
  char key[288];
  unsigned long hash;
  float width;
  unsigned long usage;
  int busy;
} Measure;
static Measure measures[MAX_MEASURES];

// The width in LAYOUT units — the same units lineFamily stores, so the two are
// interchangeable in a comparison.
static float widthOf(TxtStyle style, const char *s, TxtFamily family) {
  char clean[1024], clean2[1024];
  if (!s || !*s || style < 0 || style >= TXT_NFONTS || !fonts[style]) return 0.0f;
  // The other half of the pair described in lineFamily: measure what will be
  // drawn, never what was passed in.
  s = withoutInvisible(s, clean, sizeof clean);
  if (!*s) return 0.0f;
  if (family < TXT_FAMILY_INTER || family >= TXT_FAMILY_N)
    family = TXT_FAMILY_INTER;
  // The other half of the pair, for the decorative strip too: measuring the raw
  // string while drawing the cleaned one would wrap at the wrong word.
  if (family == TXT_FAMILY_INTER) {
    s = withoutDecorativeNoGlyph(style, s, clean2, sizeof clean2);
    if (!*s) return 0.0f;
  }

  // NO COLOUR in the key: the width of a string does not depend on it, so the
  // same line measured in two colours is measured once. The LENGTH is in the key
  // because the string is truncated into it: txt_line_trim asks for variants
  // that differ only past the truncation point, and without the length they
  // would all share an entry and give the same width.
  char key[288];
  snprintf(key, sizeof key, "%d:%d:%u|%.240s", (int)family, (int)style,
           (unsigned)strlen(s), s);
  unsigned long h = 2166136261UL;
  { const char *p = key;
    for (; *p; p++) { h ^= (unsigned char)*p; h *= 16777619UL; } }

  int free_ = -1;
  for (int k = 0; k < MAX_MEASURES; k++) {
    int i = (int)((h + (unsigned long)k) % MAX_MEASURES);
    if (!measures[i].busy) { free_ = i; break; }
    if (measures[i].hash == h && strcmp(measures[i].key, key) == 0) {
      measures[i].usage = ++lruClock;
      return measures[i].width;
    }
  }

  int w = 0, hGlyph = 0;
  if (TTF_SizeUTF8(subtitleFontOf(style, s, family), s, &w, &hGlyph) != 0)
    return 0.0f;
  float width = (float)w / scaleTxt;

  int slot = free_;
  if (slot < 0) {
    unsigned long smaller = ~0UL;
    for (int i = 0; i < MAX_MEASURES; i++)
      if (measures[i].usage < smaller) { smaller = measures[i].usage; slot = i; }
  }
  if (slot < 0) return width;   // cannot happen; the measurement is still right
  measures[slot].busy = 1;
  measures[slot].hash = h;
  strncpy(measures[slot].key, key, sizeof measures[slot].key - 1);
  measures[slot].key[sizeof measures[slot].key - 1] = 0;
  measures[slot].width = width;
  measures[slot].usage = ++lruClock;
  return width;
}

TxtLine txt_line(TxtStyle style, const char *s, int r, int g, int b, int a) {
  return lineFamily(style, s, r, g, b, a, TXT_FAMILY_INTER, 0);
}

TxtLine txt_line_px(TxtStyle style, const char *s, float px, int r, int g, int b, int a) {
  int p = (int)(px + 0.5f);
  return lineFamily(style, s, r, g, b, a, TXT_FAMILY_INTER, p < 1 ? 1 : p);
}

TxtLine txt_line_family(TxtStyle style, const char *s, int r, int g,
                           int b, int a, TxtFamily family) {
  return lineFamily(style, s, r, g, b, a, family, 0);
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
  // Fully transparent is not drawn. The callers that MEASURE a block (txt_block
  // with a negative x, the hero laying its copy out during the gap) run the
  // whole layout with alpha 0, and every one of those lines was still being
  // submitted to the batch.
  if (alpha <= 0.004f) return;
  GfxRect r = { fits(x), fits(y), (float)l.w, (float)l.h };
  gfx_rect(r, l.tex, GFX_TEXT, 0, 0, 0, 0.0f, 1, 1, 1, alpha);
}

// GFX_BRAND and not GFX_TEXT: that one passes the texture's RGB straight through
// and would draw the line in its own colour again. See the note in text.h.
void txt_draw_shadow(TxtLine l, float x, float y, float alpha) {
  if (!l.tex) return;
  if (alpha <= 0.004f) return;
  { GfxRect r = { fits(x), fits(y), (float)l.w, (float)l.h };
    gfx_rect(r, l.tex, GFX_BRAND, 0, 0, 0, 0.0f, 0, 0, 0, alpha); }
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

    // A character that draws nothing takes no space and no tracking either: the
    // stripping happens inside txt_line, so without this test an invisible mark
    // would still open a gap the width of the tracking in the middle of a word.
    // The same holds for a decorative character the face has no glyph for, which
    // txt_line now removes as well — it would otherwise be an invisible
    // character that still cost a letter's spacing.
    char one[8], one2[8];
    if (!*withoutInvisible(c, one, sizeof one)) continue;
    if (!*withoutDecorativeNoGlyph(style, c, one2, sizeof one2)) continue;

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
float txt_width(TxtStyle style, const char *s) {
  return (s && *s) ? widthOf(style, s, TXT_FAMILY_INTER) : 0.0f;
}

TxtLine txt_line_trim(TxtStyle style, const char *s, int r, int g, int b,
                         int a, float maxW) {
  return txt_line_trim_family(style, s, r, g, b, a, maxW,
                                 TXT_FAMILY_INTER);
}

TxtLine txt_line_trim_family(TxtStyle style, const char *s, int r, int g,
                                 int b, int a, float maxW,
                                 TxtFamily family) {
  if (!s || !*s) return txt_line_family(style, s, r, g, b, a, family);
  // MEASURED before it is rasterised. This used to rasterise the WHOLE string
  // just to discover it did not fit, and then one texture per candidate on the
  // way down: a long meta line spent the frame's entire TXT_PER_FRAME budget on
  // strings nobody would ever see, and the line that did fit arrived frames
  // later. Only the winner is rasterised now.
  if (widthOf(style, s, family) <= maxW)
    return txt_line_family(style, s, r, g, b, a, family);
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
    if (widthOf(style, t, family) <= maxW)
      return txt_line_family(style, t, r, g, b, a, family);
  }
  return txt_line_family(style, "\xe2\x80\xa6", r, g, b, a, family);
}

// The one wrapper behind txt_block and txt_block_trim. With `trim`, a block that
// runs out of lines before it runs out of text ends its last line on an ellipsis.
static float blockDraw(TxtStyle style, const char *s, int r, int g, int b,
                       float x, float y, float width, float leading, float alpha,
                       int maxLines, int trim) {
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

    // The FIT TEST, and nothing more: no texture is made for a prefix that is
    // only being compared against a number. See widthOf.
    if (widthOf(style, attempt, TXT_FAMILY_INTER) > width && line[0]) {
      // it did not fit: close the current line and start again with the word
      // THE LAST LINE, with text still to come: it takes the rest and is trimmed
      // to the width, which is what puts the ellipsis on it.
      if (trim && maxLines > 0 && nLines == maxLines - 1) {
        char rest[1024];
        snprintf(rest, sizeof rest, "%s %s", line, start);
        txt_draw_alpha(txt_line_trim(style, rest, r, g, b, 255, width), x, y + used, alpha);
        return used + leading;
      }
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

float txt_block(TxtStyle style, const char *s, int r, int g, int b,
                float x, float y, float width, float leading, float alpha, int maxLines) {
  return blockDraw(style, s, r, g, b, x, y, width, leading, alpha, maxLines, 0);
}
float txt_block_trim(TxtStyle style, const char *s, int r, int g, int b,
                     float x, float y, float width, float leading, float alpha, int maxLines) {
  return blockDraw(style, s, r, g, b, x, y, width, leading, alpha, maxLines, 1);
}

int txt_block_lines(TxtStyle style, const char *s, float width) {
  if (!s || !*s) return 0;
  char line[512]; line[0] = 0;
  int nLines = 0;
  const char *p = s;
  while (*p) {
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
    if (widthOf(style, attempt, TXT_FAMILY_INTER) > width && line[0]) {
      nLines++;
      memcpy(line, start, np); line[np] = 0;
    } else {
      memcpy(line, attempt, nl + np + 1);
    }
  }
  return nLines + (line[0] ? 1 : 0);
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

    // The FIT TEST, and nothing more: no texture is made for a prefix that is
    // only being compared against a number. See widthOf.
    if (widthOf(style, attempt, TXT_FAMILY_INTER) > width && line[0]) {
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
