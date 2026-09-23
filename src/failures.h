// THE PLAYBACK FAILURE LOG: one line per file that would not open or would not
// keep playing, kept in the user's data folder so it survives restarts and
// reinstalls.
//
// It exists to answer "which files don't work, and why" after the fact. A
// failure on the sofa is a black screen and a shrug; the line here carries the
// title, the episode, the stage it died at, the pipeline's own error text, and
// what the SOURCE was — addon, service, resolution, HDR, container, codec, size
// and the file name — which is what decides whether it is this app's bug, the
// TV's decoder, or a dead link.
//
// Read it with `bash tools/failures.sh` (from the TV) or at
// ~/.nuvio/playback-failures.log (on the Mac).
#ifndef NV_FAILURES_H
#define NV_FAILURES_H

// Appends `line` with a local timestamp in front. The file is rotated to
// playback-failures.old.log past 256 KB, so it cannot grow without end.
void failure_log(const char *line);

#endif
