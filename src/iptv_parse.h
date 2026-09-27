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
// the guide's address as well, and a comma-separated list keeps its first entry.
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
  int groupIndex;        // into IptvList.groups, -1 when ungrouped
  // The channel's programmes, sorted by start: pg[firstPg .. firstPg+nPg-1].
  int firstPg, nPg;
} IptvChannel;

typedef struct {
  int channel;                 // index into IptvList.ch
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
  IptvBlock *arena;
} IptvList;

void iptv_list_init(IptvList *l);
void iptv_list_free(IptvList *l);

// Appends the channels in `text` to `l`. Returns how many were added; 0 means
// `text` is not a playlist (an HTML error page, an expired-account JSON…).
int iptv_parse_m3u(IptvList *l, const char *text);

// Reads the programmes in `xml` that overlap [from, to) into `l`, matched to
// its channels by tvg-id, then by name. Replaces any guide already attached.
// Returns how many programmes were kept.
int iptv_parse_xmltv(IptvList *l, const char *xml, long long from, long long to);

// XMLTV's timestamp, "20260927143000 +0200" (the offset is optional and means
// UTC when absent), as unix seconds. 0 when it does not parse.
long long iptv_xmltv_time(const char *s);

// The programme on `ch` airing at `t`, or -1. Binary search: the guide draws
// this for every visible row, every frame.
int iptv_programme_at(const IptvList *l, int ch, long long t);
// The first programme on `ch` starting at or after `t`, or -1.
int iptv_programme_after(const IptvList *l, int ch, long long t);

// Gunzips `in` (a .gz file or a zlib stream) into a fresh NUL-terminated
// buffer, through the device's own libz loaded on demand — the SDK ships none to
// link against, the TV has one. NULL when `in` is not compressed or there is no
// libz; *outN gets the size.
char *iptv_gunzip(const char *in, long n, long *outN);

// Decodes XML entities (&amp; &#233; &#x2019; …) in place, into UTF-8.
void iptv_xml_unescape(char *s);

#endif
