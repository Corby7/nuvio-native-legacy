// THE PLAYER SHEETS' TABS: words with an underline, not pills.
//
// Sources and Subtitles both carry a strip of them under their heading. Pills had
// the same rounded shape as the rows beneath and read as one more option; a bare
// word with a rule under the chosen one reads as navigation above the list. One
// implementation, so the two sheets cannot drift.
#ifndef NV_TABS_H
#define NV_TABS_H

// How wide tab `name` is, the gap after it included — for a caller that has to
// work out which tabs fit before drawing any.
float tab_width(const char *name);
// One tab at (x, y), top of its NV_TAB_H line. `on` is the chosen tab, `cursor`
// that the D-pad is on the strip (only meaningful on the chosen one). Returns the
// width it took, the gap included.
float tab_draw(float x, float y, const char *name, int on, int cursor, float a);

#endif
