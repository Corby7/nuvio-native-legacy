// THE SCROLLBAR: a readout, not a control — nothing on a remote can drag it.
// It answers "how far in am I", and it answers with where the scroll is GOING,
// on the frame the press commits it, rather than tweening a thumb alongside the
// spring; a bar that animates on its own drifts out of step with what it reports.
//
// Two shapes, one look:
//   - SEGMENTED, for a grid, which moves a whole row per press: one segment per
//     row, the ones behind dimly lit as progress, the live one marked by WIDTH
//     (6 -> 10px) and not by colour, which the rest of the app has spoken for.
//   - CONTINUOUS, the same bar without the gaps: a track lit behind the thumb,
//     the thumb widening the same way. For lists, and for grids too long for
//     their segments to stay 24px tall.
//
// At rest the whole bar sits at 30%. A move brings it to full, widens the live
// piece, lights it softly and — where there is a count to give — slides a
// "2 / 9" chip in beside it; 1.2s after the last move the chip goes, then the
// bar settles back over 400ms.
//
// Both draw centred on the vertical line `axis`, from `top` to `bottom`.
#ifndef NV_SCROLLBAR_H
#define NV_SCROLLBAR_H
#include <SDL2/SDL.h>

typedef struct {
  int    at;      // the row/item last reported
  float  pos;     // the thumb's place last reported
  int    seen;    // 0 until the first frame, which is never a move
  Uint32 moved;   // when it last moved; 0 = never, at rest
} ScrollBar;

// A grid's: `row` of `rows`. Falls back to the continuous bar (with the chip)
// when the segments would come out under 24px; `onScreen` is how many rows
// the screen shows, for that thumb's length.
void scrollbar_rows(ScrollBar *b, float axis, float top, float bottom,
                    int row, int rows, float onScreen, float alpha, Uint32 now);

// A list's: the thumb at `pos` 0..1 along the track and `size` 0..1 of it.
// `at` of `n` is the chip's count; n <= 0 draws no chip, and then only `pos`
// moving counts as a move.
void scrollbar_draw(ScrollBar *b, float axis, float top, float bottom,
                    float pos, float size, int at, int n, float alpha, Uint32 now);
#endif
