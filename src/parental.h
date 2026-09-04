// Parental guide for the title being played — the panel the web app shows in
// the top-left corner of the player when the controls appear
// (.player-parental-guide). Up to five "Category · Severity" lines with a
// vertical accent-coloured bar on the left.
//
// The port drew something else there: a certification badge with the title's
// GENRE beside it, which does not exist on the web. A genre is not a content
// warning.
#ifndef NV_PARENTAL_H
#define NV_PARENTAL_H

#define PG_MAX 5

// Requests the guide for `imdb` (in "tt1234567" form). Does not block: it
// starts a thread and returns immediately. Calling again with the SAME id does
// not repeat the request.
void parental_request(const char *imdb);

// How many lines have arrived (0 while fetching, or when the title has no
// data). `label` is the translated category, `severity` the level.
int  parental_n(void);
const char *parental_label(int i);
const char *parental_severity(int i);

#endif
