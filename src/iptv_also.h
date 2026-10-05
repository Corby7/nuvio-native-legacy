// THE SAME BROADCAST ELSEWHERE: every channel showing what the tuned one shows,
// the best picture first.
//
// A race is on Sky Sports F1 HD, on its 4K copy, on a Dutch and a German sports
// channel, each in the playlist two or three times over. Two ways to know they
// carry the same thing:
//   - the channel is one of a channel's copies (iptv_same_channel: the names
//     agree once the country tag and the quality words are gone), which needs
//     no guide at all;
//   - the guide has a programme on it now whose title names the same programme
//     (iptv_same_title). A title of a single word ("News") needs the times to
//     agree as well, or every news channel would be one.
//
// QUALITY IS WHAT WAS MEASURED, ELSE WHAT THE NAME CLAIMS. The stream itself is
// never opened to find out: IPTV accounts allow one connection, and a probe
// would cut off the channel being watched. Instead every channel played leaves
// its decoded height and frame rate behind (iptv_quality_note), kept by name in
// iptv_quality.txt; a "4K" that turned out to be 1080p ranks as 1080p from
// then on. A channel never played ranks by its name's tag, then its group's
// ("4K| SPORT").
#ifndef NV_IPTV_ALSO_H
#define NV_IPTV_ALSO_H

#define IPTV_ALSO_MAX 48

// The channels airing what `ch` airs at unix time `t`, `ch` among them, the
// best picture first (tier, then frame rate, then channel number). Returns how
// many were written to `out`, at most `max`; 1 means nowhere else.
int iptv_also(int ch, long long t, int *out, int max);

// Channel `ch`'s tier (IPTV_Q_*): measured when it has been played, else claimed.
int iptv_quality(int ch);
// 1 when iptv_quality(ch) comes from the decoder rather than the name.
int iptv_quality_measured(int ch);
// "4K", "1080p50", "720p", "SD"; "" when nothing is known.
const char *iptv_quality_label(int ch, char *dst, unsigned size);
// What the decoder made of channel `ch`: kept for the next ranking.
void iptv_quality_note(int ch, int width, int height, int fpsMilli);

#endif
