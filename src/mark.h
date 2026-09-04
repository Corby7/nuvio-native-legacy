// TIME MARKS: a named stamp, with the millisecond it happened at.
//
// It exists because "it feels really sluggish" cannot be answered by looking at
// the screen. The FPS report gives the cost of a FRAME; what was missing was
// the cost of a JOURNEY — from launch until home has content, from the key
// press until the text settles, from "Play" until the video starts. Those are
// tens of seconds spread across different threads, and none of them show up in
// a frame number.
//
// The output goes to /tmp/nuvio-marks.txt because on the device the standard
// output of an app launched by applicationManager reaches nowhere you can read
// — the same reason /tmp/nuvio-fps.txt already exists.
//
// Cheap on purpose: one fprintf per event, and there are only a few dozen per
// session. Do not instrument per frame with this.
#ifndef NV_MARK_H
#define NV_MARK_H

// Stamps `name` with the ms elapsed since mark_start(). Safe to call from any
// thread.
void mark(const char *name);

// Resets the clock and restarts the file. Called once, at the start of main.
void mark_start(void);

#endif
