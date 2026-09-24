// THE SOURCE THE PERSON PICKED, remembered per title and per profile.
// Ported from upstream (iqui27/nuvio-native-legacy, fontepref.c, 1.0.55-1.3.4).
//
// The app never kept which source was chosen in the sheet. The automatic rule
// scores the list (MP4 4K DV first) and does NOT KNOW that the source picked by
// hand was the only one with the right audio. So on the next episode the score
// picked another one, and Resume never resumed on the same source.
//
// --- WHAT "THE SAME SOURCE" MEANS ON THE NEXT EPISODE ----------------------
//
// Not the url: it changes every episode, and a debrid link is signed and
// expires in minutes. Not the file or the size, for the same reason. Not the
// index: the list arrives from addons in parallel and its order does not repeat.
//
// The match, in order:
//
//   1. The same bingeGroup. It is the addon's own statement that two streams
//      are the same source across episodes (Stremio's behaviorHints), and the
//      web app matches on it alone.
//   2. The same provider (the addon) AND the same AUDIO SIGNATURE — the
//      language/dub marks read from the label, description and file name
//      ("DUAL+ENG", a flag, "Dubbed"). Without the signature, "same provider"
//      returns the first Torrentio row, which is the subtitled one the person
//      did not want. A second pass accepts today's marks CONTAINING the
//      remembered ones ("Dual Audio" today, "Dual Audio BR DUB" tomorrow), but
//      a remembered mark is never dropped: a DUB choice never falls back to
//      the original audio.
//   3. Nothing: -1, and the automatic rule decides, as it always did.
//
// ONLY A MANUAL PICK IS STORED. What the automatic rule chooses never becomes a
// preference: someone who never opens the sheet keeps the score rule forever.
//
// It does not touch the network and verifies nothing. stream_first_good still
// checks the link; this module only says WHICH to try first.
#ifndef NV_SOURCEPREF_H
#define NV_SOURCEPREF_H

#include "streams.h"

// 200 titles x ~500 bytes. Full, the OLDEST preference goes.
#define SOURCEPREF_MAX 200
#define SOURCEPREF_AUDIO 64
#define SOURCEPREF_BINGE 128

// 180 days. What is kept is IDENTITY, not a link, so the link's short life has
// nothing to do with it. What expiry protects against is a provider that is
// gone — and a weekly season of 24 episodes takes ~168 days, which a shorter
// limit would cut in the middle of.
#define SOURCEPREF_VALID_S (180LL * 24LL * 3600LL)

typedef struct {
  char id[24];                       // the TITLE's IMDb id, no ":season:episode"
  char provider[96];
  char audio[SOURCEPREF_AUDIO];
  char bingeGroup[SOURCEPREF_BINGE];
  char label[192];                   // tie-break and log
  int  height;
  long long whenS;                   // time(NULL) of the pick
} SourcePref;

// Stores the pick and WRITES the file. `id` may carry an episode; it is cut
// here. Returns 1 when the table changed. A source with no provider is ignored.
int sourcepref_store(const char *id, const Stream *s);

// Index, IN THE CURRENT STREAM LIST, of the source that matches this title's
// preference. -1 when there is none or nothing today matches — and -1 means
// "go automatic", never "do not play".
int sourcepref_pick(const char *id);
// 1 when this title HAS a remembered pick, whether or not today's list holds it.
// The router asks before starting on a partial list: the pick may belong to the
// addon that has not answered yet, and starting without it would skip the
// source the person chose by hand.
int sourcepref_has(const char *id);

// The audio signature of a source: the marks found, sorted, joined by "+".
// Empty when the source declares nothing. Exposed for the tests.
void sourcepref_audio(const Stream *s, char *dst, unsigned size);

// Sign-out: deletes every profile's file.
void sourcepref_forget(void);

#endif
