// Spatial focus manager with a PER-ROW COLUMN MEMORY.
//
// Column memory is the detail that separates good navigation from irritating
// navigation: going down from row 1 (column 5) to row 2 and back, the focus has
// to return to column 5, not column 0. tvOS does this; without it the user
// loses their place every time they change row.
#ifndef NV_FOCUS_H
#define NV_FOCUS_H

// 48 AND NOT 32, which is MAX_FILTER in home.c — the home builds up to that many
// rows (24 catalogues plus the owner's collections) and focus_start CLAMPS what it
// is given. At 32 the rows past the thirty-second existed, were drawn and were
// simply unreachable: the down arrow stopped, with nothing logged. tests/
// home_layout.c asserts the two agree, and that assertion has been failing since
// MAX_FILTER was raised.
#define FOCUS_MAX_ROWS 48

typedef struct {
  int row;
  int column;
  int columnRemembered[FOCUS_MAX_ROWS];
  int nRows;
  int nColumns[FOCUS_MAX_ROWS];
} Focus;

void focus_start(Focus *f, int nRows, const int *nColumns);
int  focus_move(Focus *f, int dx, int dy);   // 1 if it moved

// A GRID: up and down KEEP the column, with no per-row memory.
//
// The column memory above is right for rows of CONTENT, where each row has a
// length of its own and the viewer holds their place in each. In a GRID it is a
// visible defect: the search keyboard has 6 columns per row, and going down from
// "f" (column 5) landed on column 0 of the next row, because that is where the
// cursor had last been IN THAT ROW — on "g" instead of "l". Going down again:
// "m". Coming back, the cursor reappears on "f". From the sofa that reads
// exactly as the report says, "it jumps to a random letter". The library's
// poster grid has the same defect for the same reason.
//
// Here the column is PRESERVED and only clamped to the end of the destination
// row when that row is shorter (the keyboard's last row has 3 keys, not 6).
int  focus_move_grid(Focus *f, int dx, int dy);

int  focus_index(const Focus *f, int row, int column);

#endif
