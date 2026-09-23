// AUDIO and SUBTITLE sheets, opened from the player icons.
//
// Audio is a side panel with one list. Subtitles is the same panel with two tabs:
//
//   TRACKS  two selects, stacked — the LANGUAGE, then the SUBTITLE in just that
//           language. Picking a language first is what keeps a file with four
//           English tracks and three Dutch ones from being one list of seven
//           where the row you want says the same thing as its neighbour.
//   STYLE   not a panel at all: a bar along the bottom, so the subtitle is
//           previewed where it will really sit, at its real width.
//
// The track list merges the ones EMBEDDED in the file (the pipeline sees them)
// with the OpenSubtitles ones (downloaded by the addon). Different things at the
// source, the same thing to the viewer, so they appear together, labelled.
#ifndef NV_TRACKS_H
#define NV_TRACKS_H
#include <SDL2/SDL.h>

// Resets what belongs to the SESSION rather than the device — today, which
// external subtitle is in force. Called by the player when a new playback
// starts.
void tracks_reset(void);

// AUTOMATIC SELECTION, once per playback, from the Settings row "Subtitles".
// Called every frame by the player while a title is on screen: the two lists it
// chooses from arrive at different moments (the file's tracks with the pipeline,
// the addon's seconds later over the network), so there is no single instant
// that could be "the" moment to decide. It settles on the first frame where
// there is something to choose, or gives up at the deadline.
void tracks_auto(Uint32 now);

void tracks_open(void);
// Opens with focus ALREADY on the requested column: 0 = audio, 1 = subtitles.
// The player has an icon for each, and always opening on audio made the two
// look like the same button.
void tracks_open_at(int col);
int  tracks_is_open(void);
void tracks_event(const SDL_Event *e);
void tracks_update(float dt, Uint32 now);
void tracks_draw(Uint32 now);

// How far the sheet is in, 0..1. The player fades ALL of its chrome by it: the
// sheet is the one thing to read while it is up, and the picture and the
// subtitles are what stays.
float tracks_shown(void);
// How far the STYLE BAR is in, 0..1. The player keeps the subtitle above it.
float tracks_style_shown(void);

#endif
