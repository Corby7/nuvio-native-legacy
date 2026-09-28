// LIVE PAUSE AND REWIND for a channel whose provider keeps no archive.
//
// A live stream has no past to seek in: what the pipeline has not read is gone.
// So while a channel plays, this module is the one that reads it, a thread
// downloads the stream into a RING FILE in the data folder, and the pipeline
// plays from a loopback server that reads the ring. Pausing the pipeline then
// only stops the reading side; the recording carries on, and resuming picks up
// exactly where the picture froze. Rewinding is a new load of the loopback URL
// at an earlier byte, found through an index of when each byte arrived.
//
// ONLY MPEG-TS. A TS stream is a flat run of 188-byte packets that a demuxer
// can join anywhere, so a byte offset IS a place to resume. HLS (.m3u8) is a
// playlist of segments on the provider's side; recording it would mean
// fetching and rewriting segments, which this does not do: timeshift_begin
// refuses it, and the screen falls back to catch-up, or to "resume live".
//
// ONE UPSTREAM CONNECTION. The pipeline never talks to the provider while this
// runs, so an account limited to one stream is not charged a second.
//
// The ring's size is a byte budget (bitrate × minutes, capped by the free space
// of the data folder); what it holds in TIME is whatever the channel's bitrate
// makes of it, which timeshift_range reports.
#ifndef NV_TIMESHIFT_H
#define NV_TIMESHIFT_H

// Starts recording `url` (with "Name: Value\n" `headers`, may be NULL) into a
// ring of `capBytes`. Stops any recording before it. 1 when started; the
// recording may still fail on its first bytes, see timeshift_state.
int  timeshift_begin(const char *url, const char *headers, long long capBytes);
void timeshift_end(void);

typedef enum {
  TS_OFF,         // nothing recording
  TS_STARTING,    // connected, no stream bytes yet
  TS_RECORDING,   // bytes arriving; the loopback URL plays
  TS_REFUSED,     // the upstream answered with something that is not MPEG-TS
  TS_FAILED       // the upstream failed and reconnecting did not help
} TimeshiftState;
int  timeshift_state(void);

// What the ring holds, as wall-clock seconds (fractional, unix): the oldest
// byte still in it and the newest. 0 when nothing is recorded yet.
int  timeshift_range(double *oldest, double *newest);

// The loopback URL that plays the recording from wall time `t` (clamped into
// the range). NULL while nothing is recorded. The response never ends while
// the recording continues: it is a live stream that happens to start earlier.
const char *timeshift_url(double t, char *dst, unsigned size);

// The byte budget for `minutes` of a channel, capped by the data folder's free
// space (half of it at most). 0 when there is no room worth using.
long long timeshift_budget(int minutes);

#endif
