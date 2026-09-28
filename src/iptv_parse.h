// IPTV DATA, PARSED: an M3U playlist's channels and an XMLTV guide's programmes.
//
// Pure C with no SDL, no GL and no network, on purpose: it is the part of Live
// TV that can be wrong in a thousand provider-specific ways, and every one of
// them should be caught by tests/iptv.sh on a laptop rather than on the TV.
// iptv.c does the fetching, the threads and the persistence around it.
//
// WHAT A PLAYLIST LOOKS LIKE IN THE WILD, and what this reads of it:
//
//   #EXTM3U url-tvg="http://…/epg.xml.gz"            <- the guide's address
//   #EXTINF:-1 tvg-id="bbc1.uk" tvg-name="BBC One" tvg-logo="http://…"
//              group-title="UK | General" tvg-chno="101",BBC One HD
//   #EXTVLCOPT:http-user-agent=Mozilla/5.0            <- headers for the stream
//   #EXTGRP:UK                                        <- a group, the old way
//   http://provider/live/user/pass/1234.m3u8
//
// Attributes are matched case-insensitively and in any order; the display name
// is whatever follows the LAST comma outside quotes (some providers put commas
// inside tvg-name, never outside it). `x-tvg-url` and `tvg-url` are accepted for
// the guide's address as well; a comma-separated list is kept whole (the loader
// reads each guide in turn).
//
// THE GUIDE is only kept for a window of time (`from`..`to`): a full XMLTV feed
// is a week of every channel the provider carries, a hundred megabytes of which
// the screen will show two hours. Programmes for channels that are not in the
// playlist are dropped at parse time for the same reason.
#ifndef NV_IPTV_PARSE_H
#define NV_IPTV_PARSE_H
#include <stddef.h>

typedef struct {
  const char *name;      // what the viewer reads: "BBC One HD"
  const char *url;       // the stream
  const char *logo;      // tvg-logo, "" when none
  const char *group;     // group-title / #EXTGRP, "" when none
  const char *tvgId;     // tvg-id, the key into the guide; "" when none
  const char *tvgName;   // tvg-name, the fallback key; "" when none
  const char *headers;   // "Name: Value\n…" from #EXTVLCOPT / #EXTHTTP, "" when none
  int number;            // tvg-chno when given, else its 1-based position
  int catchupDays;       // catchup-days / tvg-rec, 0 when none
  int catchup;           // IptvCatchup: how to ask for the past, NONE when it cannot be
  const char *catchupSource;   // catchup-source, the template; "" when none
  int groupIndex;        // into IptvList.groups, -1 when ungrouped
  // The channel's programmes, sorted by start: pg[firstPg .. firstPg+nPg-1].
  int firstPg, nPg;
} IptvChannel;

// CATCH-UP, the provider's archive: how a channel's past is asked for. These are
// the conventions Kodi's IPTV Simple client reads, which is what providers write
// playlists for:
//   catchup="default"    catchup-source is the whole archive URL, a template
//   catchup="append"     catchup-source is appended to the stream URL
//   catchup="shift"      the stream URL + ?utc={utc}&lutc={lutc}
//   catchup="flussonic"  (or "fs") …/index.m3u8 -> …/index-{utc}-{duration}.m3u8,
//                        …/mpegts -> …/timeshift_abs-{utc}.ts
//   catchup="xc"         Xtream Codes: …/live/u/p/1.ts -> …/timeshift/u/p/{minutes}/
//                        {Y}-{m}-{d}:{H}-{M}/1.ts, in the SERVER's local time
// The #EXTM3U line may carry catchup, catchup-source and catchup-days as
// defaults for every channel. A channel with catchup-days and no mode gets
// "default" when it has a source, "xc" when its URL is Xtream-shaped.
typedef enum {
  IPTV_CATCHUP_NONE, IPTV_CATCHUP_DEFAULT, IPTV_CATCHUP_APPEND, IPTV_CATCHUP_SHIFT,
  IPTV_CATCHUP_FLUSSONIC, IPTV_CATCHUP_XC
} IptvCatchup;

typedef struct {
  int channel;                 // index into IptvList.ch
  int seq;                     // position before sorting: ties keep feed order
  long long start, stop;       // unix seconds, UTC
  const char *title;
  const char *desc;            // "" when none
  const char *category;        // the first <category>, "" when none
} IptvProgramme;

// Strings live in arena blocks owned by the list: one free releases them all.
typedef struct IptvBlock IptvBlock;

typedef struct {
  IptvChannel *ch;   int nCh, capCh;
  IptvProgramme *pg; int nPg, capPg;
  const char **groups; int nGroups, capGroups;
  char epgUrl[1024];           // from the #EXTM3U header, "" when none
  // The #EXTM3U line's catch-up defaults.
  int catchupMode, catchupDays;
  char catchupSource[1024];
  // Seconds the Xtream server's clock is ahead of UTC: its timeshift URLs are
  // written in its own local time. iptv.c measures it; 0 otherwise.
  int serverOffset;
  IptvBlock *arena;
} IptvList;

void iptv_list_init(IptvList *l);
void iptv_list_free(IptvList *l);

// Appends the channels in `text` to `l`. Returns how many were added; 0 means
// `text` is not a playlist (an HTML error page, an expired-account JSON…).
int iptv_parse_m3u(IptvList *l, const char *text);

// Reads the programmes in `xml` that overlap [from, to) into `l`, matched to
// its channels by tvg-id, then by name (also with "(720p)" / "[…]" tags cut).
// Replaces any guide already attached. A matched channel with no tvg-logo takes
// the guide's <icon>. Returns how many programmes were kept.
int iptv_parse_xmltv(IptvList *l, const char *xml, long long from, long long to);

// A FURTHER GUIDE, for the channels the ones before it left empty: providers
// cover their headline channels and skip the rest, and a second XMLTV (the
// viewer's own, or the next one in a url-tvg list) fills the gaps. Channels that
// already have programmes are not matched at all, so a guide never mixes two
// schedules on one channel and the first guide read keeps priority. Returns how
// many programmes it added.
int iptv_parse_xmltv_more(IptvList *l, const char *xml, long long from, long long to);

// XMLTV's timestamp, "20260927143000 +0200" (the offset is optional and means
// UTC when absent), as unix seconds. 0 when it does not parse.
long long iptv_xmltv_time(const char *s);

// The programme on `ch` airing at `t`, or -1. Binary search: the guide draws
// this for every visible row, every frame.
int iptv_programme_at(const IptvList *l, int ch, long long t);
// The first programme on `ch` starting at or after `t`, or -1.
int iptv_programme_after(const IptvList *l, int ch, long long t);

// Gunzips `in` (a .gz file or a zlib stream) into a fresh NUL-terminated
// buffer, through the device's own libz loaded on demand, the SDK ships none to
// link against, the TV has one. NULL when `in` is not compressed or there is no
// libz; *outN gets the size.
char *iptv_gunzip(const char *in, long n, long *outN);

// A guide as the loader keeps it: inflated when gzipped, and cut to the
// programmes overlapping [from, to) WHILE inflating, so a week-long country
// guide never sits in memory whole. The header and <channel> blocks pass as
// they are. NULL when nothing came through.
char *iptv_guide_window(const char *in, long n, long long from, long long to, long *outN);

// The archive URL for channel `c` from `start` (unix seconds) until `stop`, with
// `now` for the templates that want it and `serverOffset` for Xtream's local
// time. 1 when written; 0 when the channel has no catch-up or it does not fit.
int iptv_catchup_url(const IptvChannel *c, long long start, long long stop,
                     long long now, int serverOffset, char *dst, size_t size);
// Expands a catch-up template's placeholders: {utc} {start} ${start} {utcend}
// {end} ${end} {lutc} {now} ${now} ${timestamp} {timestamp} {duration}
// {duration:N} {offset:N} ${offset} {Y} {m} {d} {H} {M} {S}, and {utc:FORMAT}
// {utcend:FORMAT} {lutc:FORMAT} with Y m d H M S in FORMAT. The broken-down
// fields are in UTC plus `offset` seconds. 0 when `dst` is too small.
int iptv_catchup_expand(const char *tpl, long long start, long long stop,
                        long long now, int offset, char *dst, size_t size);
// "xc", "shift", … as IptvCatchup; NONE for anything unknown.
int iptv_catchup_mode(const char *s, size_t n);
// An Xtream stream URL's parts: http://host:port[/live]/user/pass/id[.ext]. 1 when
// it has that shape. Any pointer may be NULL.
int iptv_xtream_parts(const char *url, char *origin, size_t no, char *user, size_t nu,
                      char *pass, size_t np, long *id, char *ext, size_t ne);

// XTREAM CODES' JSON API AS A PLAYLIST. Many panels refuse get.php (the M3U
// download) and answer only player_api.php, the API every IPTV app uses: the
// live streams (`action=get_live_streams`, a root array of objects) and their
// categories (`action=get_live_categories`). This writes the M3U text get.php
// would have given, so everything after the download, the cache, the guide,
// the channel indices, is the same code either way.
//
// Each stream becomes base/live/user/pass/<stream_id>.<ext>, with its name,
// stream_icon, epg_channel_id (as tvg-id), category name (as group-title), num
// (as tvg-chno) and tv_archive_duration (as catchup-days). JSON strings are
// fully decoded, \uXXXX and surrogate pairs included: these names are
// Arabic, Greek, Turkish as often as English. `categories` may be NULL.
// Returns a malloc'd playlist, or NULL when `streams` is not an array.
char *iptv_xtream_m3u(const char *streams, const char *categories, const char *base,
                      const char *user, const char *pass, const char *ext);

// STYLISED LATIN AS PLAIN LETTERS, in place. Channel names are decorated with
// Unicode lookalikes, "BBC One ᴴᴰ", "ʀᴀᴡ", "𝐒𝐤𝐲 𝐒𝐩𝐨𝐫𝐭𝐬", "Ⓢⓟⓞⓡⓣ",
// "ＵＫ", that no font on the TV draws: superscript and small-capital letters,
// the mathematical alphabets, enclosed and fullwidth letters. Each becomes the
// ASCII letter or digit it imitates. Real scripts (accents, Greek, Cyrillic,
// Arabic, CJK) are left alone; text.c finds a font for those. The text never
// grows, so this works in place.
void iptv_plain_text(char *s);

// Decodes XML entities (&amp; &#233; &#x2019; …) in place, into UTF-8.
void iptv_xml_unescape(char *s);

#endif
