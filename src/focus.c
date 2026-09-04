#include "focus.h"
#include <string.h>

void focus_start(Focus *f, int nRows, const int *nColumns) {
  memset(f, 0, sizeof *f);
  if (nRows > FOCUS_MAX_ROWS) nRows = FOCUS_MAX_ROWS;
  f->nRows = nRows;
  for (int i = 0; i < nRows; i++) f->nColumns[i] = nColumns[i];
}

int focus_mover(Focus *f, int dx, int dy) {
  int fBefore = f->row, cBefore = f->column;

  if (dx) {
    int new = f->column + dx;
    if (new >= 0 && new < f->nColumns[f->row]) f->column = new;
  }
  if (dy) {
    // SKIPS an empty row. On a film the season and episode rows have zero
    // columns, and landing on them put the focus on something the screen does
    // not even draw: the D-pad felt stuck and the scroll still aimed at the
    // empty group. A row with no item must never take focus, so the search
    // carries on in the same direction until it finds one that has — or gives
    // up at the edge, returning 0.
    int new = f->row + dy;
    while (new >= 0 && new < f->nRows && f->nColumns[new] <= 0) new += dy;
    if (new >= 0 && new < f->nRows) {
      // remember where we were in this row before leaving it
      f->columnRemembered[f->row] = f->column;
      f->row = new;
      int target = f->columnRemembered[new];
      if (target >= f->nColumns[new]) target = f->nColumns[new] - 1;
      if (target < 0) target = 0;
      f->column = target;
    }
  }
  return (f->row != fBefore || f->column != cBefore);
}

int focus_index(const Focus *f, int row, int column) {
  return (f->row == row && f->column == column);
}
