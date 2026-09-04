// Spatial focus manager with a PER-ROW COLUMN MEMORY.
//
// Column memory is the detail that separates good navigation from irritating
// navigation: going down from row 1 (column 5) to row 2 and back, the focus has
// to return to column 5, not column 0. tvOS does this; without it the user
// loses their place every time they change row.
#ifndef NV_FOCUS_H
#define NV_FOCUS_H

#define FOCUS_MAX_ROWS 32

typedef struct {
  int row;
  int column;
  int columnRemembered[FOCUS_MAX_ROWS];
  int nRows;
  int nColumns[FOCUS_MAX_ROWS];
} Focus;

void focus_start(Focus *f, int nRows, const int *nColumns);
int  focus_mover(Focus *f, int dx, int dy);   // 1 se moveu
int  focus_index(const Focus *f, int row, int column);

#endif
