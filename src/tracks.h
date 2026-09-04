// AUDIO AND SUBTITLE sheet, opened from the player icons.
//
// Two columns in a single panel, as on the device: audio on the left, subtitles
// on the right. Splitting them into two screens would force you to leave and
// come back to check the chosen pair, which is exactly what you want to
// compare.
//
// The subtitle list merges the ones EMBEDDED in the file (the pipeline sees
// them) with the OpenSubtitles ones (downloaded by the addon). They are
// different things at the source and the same thing to the viewer, so they
// appear together, labelled.
#ifndef NV_TRACKS_H
#define NV_TRACKS_H
#include <SDL2/SDL.h>

// Resets what belongs to the SESSION rather than the device — today, which
// external subtitle is in force. Called by the player when a new playback
// starts.
void tracks_reset(void);

void tracks_open(void);
// Opens with focus ALREADY on the requested column: 0 = audio, 1 = subtitles.
// The player has an icon for each, and always opening on audio made the two
// look like the same button.
void tracks_open_em(int col);
int  tracks_is_open(void);
void tracks_event(const SDL_Event *e);
void tracks_update(float dt, Uint32 now);
void tracks_draw(Uint32 now);

#endif
