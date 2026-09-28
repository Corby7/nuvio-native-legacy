// iptv_parse.c: playlists and guides as providers actually write them.
#include "iptv_parse.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// 2026-09-27 12:00:00 UTC.
#define NOON 1790510400LL

static const char *M3U =
  "\xEF\xBB\xBF#EXTM3U url-tvg=\"http://epg.example/guide.xml.gz,http://backup/epg.xml\"\r\n"
  "#EXTINF:-1 tvg-id=\"bbc1.uk\" tvg-name=\"BBC One\" tvg-logo=\"http://img/bbc1.png\" "
    "group-title=\"UK | General\" tvg-chno=\"101\",BBC One HD\r\n"
  "#EXTVLCOPT:http-user-agent=Mozilla/5.0 (SMART-TV)\r\n"
  "#EXTVLCOPT:http-referrer=http://portal.example/\r\n"
  "http://provider/live/u/p/1.m3u8\r\n"
  "\r\n"
  // A comma inside a quoted attribute, and the same tvg-id as the HD copy.
  "#EXTINF:-1 tvg-id=\"bbc1.uk\" tvg-name=\"BBC One, SD\" group-title=\"UK | General\",BBC One SD\n"
  "http://provider/live/u/p/2.ts\n"
  // No tvg-id: matched to the guide by name. The group the old way.
  "#EXTINF:-1 tvg-logo=\"http://img/news.png\",News 24\n"
  "#EXTGRP:News\n"
  "#EXTHTTP:{\"User-Agent\":\"VLC/3.0\",\"Referer\":\"http://r/\"}\n"
  "http://provider/live/u/p/3.m3u8\n"
  // Unquoted attributes, upper-case keys, catch-up.
  "#EXTINF:-1 TVG-ID=sport.one GROUP-TITLE=Sport catchup-days=7,Sport One\n"
  "#KODIPROP:inputstream.adaptive.manifest_type=hls\n"
  "http://provider/live/u/p/4.m3u8\n"
  // A bare URL with no #EXTINF at all.
  "http://provider/movies/Some%20Film.mp4?token=abc\n"
  // A stray word that is not a stream.
  "garbage\n";

static const char *XML =
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
  "<tv generator-info-name=\"x\">\n"
  "  <channel id=\"bbc1.uk\"><display-name>BBC One</display-name></channel>\n"
  "  <channel id=\"n24\"><display-name lang=\"en\">Other</display-name>"
      "<display-name>News 24</display-name></channel>\n"
  "  <channel id=\"unused\"><display-name>Nobody</display-name></channel>\n"
  // Airing at noon (UTC+2 offset: 13:00+0200 is 11:00Z).
  "  <programme start=\"20260927130000 +0200\" stop=\"20260927143000 +0200\" channel=\"bbc1.uk\">\n"
  "    <title lang=\"en\">Tom &amp; Jerry&#8217;s &#x2764; Day</title>\n"
  "    <desc lang=\"en\"><![CDATA[Cats <b>and</b> mice.]]></desc>\n"
  "    <category lang=\"en\">Kids</category><category>Animation</category>\n"
  "  </programme>\n"
  // Next, then a duplicate of it from a merged feed.
  "  <programme start=\"20260927123000 +0000\" stop=\"20260927130000 +0000\" channel=\"bbc1.uk\">"
      "<title>News</title></programme>\n"
  "  <programme start=\"20260927123000 +0000\" stop=\"20260927130000 +0000\" channel=\"bbc1.uk\">"
      "<title>News (dup)</title></programme>\n"
  // No stop: ends where the next begins.
  "  <programme start=\"20260927130000\" channel=\"bbc1.uk\"><title>Film</title></programme>\n"
  "  <programme start=\"20260927150000\" stop=\"20260927160000\" channel=\"bbc1.uk\"><title>Late</title></programme>\n"
  // Outside the window: gone.
  "  <programme start=\"20260920120000 +0000\" stop=\"20260920130000 +0000\" channel=\"bbc1.uk\"><title>Old</title></programme>\n"
  "  <programme start=\"20261020120000 +0000\" stop=\"20261020130000 +0000\" channel=\"bbc1.uk\"><title>Far</title></programme>\n"
  // Matched through the <channel> display name.
  "  <programme start=\"20260927110000 +0000\" stop=\"20260927140000 +0000\" channel=\"n24\"><title>Headlines</title></programme>\n"
  // Matched by tvg-id directly, no <channel> block.
  "  <programme start=\"20260927114500 +0000\" stop=\"20260927124500 +0000\" channel=\"sport.one\"><title>Match</title></programme>\n"
  // Unknown channel.
  "  <programme start=\"20260927110000 +0000\" stop=\"20260927140000 +0000\" channel=\"unused\"><title>X</title></programme>\n"
  "</tv>\n";

static void testTime(void) {
  assert(iptv_xmltv_time("20260927120000 +0000") == NOON);
  assert(iptv_xmltv_time("20260927120000") == NOON);
  assert(iptv_xmltv_time("20260927140000 +0200") == NOON);
  assert(iptv_xmltv_time("20260927063000 -0530") == NOON);
  assert(iptv_xmltv_time("202609271200 +0000") == NOON);   // no seconds
  assert(iptv_xmltv_time("1970010100000 0") == 0);
  assert(iptv_xmltv_time("garbage") == 0 && iptv_xmltv_time(NULL) == 0);
  assert(iptv_xmltv_time("20260231250000") == 0);          // hour 25
  // A leap day, and across a year end.
  assert(iptv_xmltv_time("20240229000000 +0000") == 1709164800LL);
  assert(iptv_xmltv_time("20270101000000 +0100") == 1798758000LL);
}

static void testEntities(void) {
  char s[] = "a &amp; b &lt;c&gt; &quot;d&quot; &apos;e&apos; &#233; &#x1F600; &bogus; &";
  iptv_xml_unescape(s);
  assert(!strcmp(s, "a & b <c> \"d\" 'e' \xC3\xA9 \xF0\x9F\x98\x80 &bogus; &"));
}

static void testM3u(IptvList *l) {
  int n = iptv_parse_m3u(l, M3U);
  assert(n == 5 && l->nCh == 5);
  assert(!strcmp(l->epgUrl, "http://epg.example/guide.xml.gz,http://backup/epg.xml"));

  assert(!strcmp(l->ch[0].name, "BBC One HD"));
  assert(!strcmp(l->ch[0].tvgId, "bbc1.uk"));
  assert(!strcmp(l->ch[0].tvgName, "BBC One"));
  assert(!strcmp(l->ch[0].logo, "http://img/bbc1.png"));
  assert(!strcmp(l->ch[0].group, "UK | General"));
  assert(l->ch[0].number == 101);
  assert(!strcmp(l->ch[0].url, "http://provider/live/u/p/1.m3u8"));
  assert(!strcmp(l->ch[0].headers,
                 "User-Agent: Mozilla/5.0 (SMART-TV)\nReferer: http://portal.example/\n"));

  assert(!strcmp(l->ch[1].name, "BBC One SD"));
  assert(!strcmp(l->ch[1].tvgName, "BBC One, SD"));
  assert(l->ch[1].number == 2 && !l->ch[1].headers[0]);
  assert(l->ch[1].groupIndex == l->ch[0].groupIndex);

  assert(!strcmp(l->ch[2].name, "News 24") && !strcmp(l->ch[2].group, "News"));
  assert(!strcmp(l->ch[2].headers, "User-Agent: VLC/3.0\nReferer: http://r/\n"));

  assert(!strcmp(l->ch[3].tvgId, "sport.one") && !strcmp(l->ch[3].group, "Sport"));
  assert(l->ch[3].catchupDays == 7);

  assert(!strcmp(l->ch[4].name, "Some%20Film.mp4") && l->ch[4].groupIndex == -1);

  assert(l->nGroups == 3);
  assert(!strcmp(l->groups[0], "UK | General") && !strcmp(l->groups[1], "News") &&
         !strcmp(l->groups[2], "Sport"));

  // Not a playlist.
  { IptvList bad; iptv_list_init(&bad);
    assert(iptv_parse_m3u(&bad, "<html><body>401</body></html>") == 0);
    assert(iptv_parse_m3u(&bad, "{\"user_info\":{\"auth\":0}}") == 0);
    assert(iptv_parse_m3u(&bad, "") == 0 && iptv_parse_m3u(&bad, NULL) == 0);
    iptv_list_free(&bad); }
}

static void testXmltv(IptvList *l) {
  int n = iptv_parse_xmltv(l, XML, NOON - 6 * 3600, NOON + 24 * 3600);
  int p;
  // bbc1 (HD + SD share it): Tom&Jerry, News, Film, Late = 4 each; n24: 1; sport: 1.
  assert(n == 10 && l->nPg == 10);
  assert(l->ch[0].nPg == 4 && l->ch[1].nPg == 4);
  assert(l->ch[2].nPg == 1 && l->ch[3].nPg == 1 && l->ch[4].nPg == 0);

  p = iptv_programme_at(l, 0, NOON);
  assert(p >= 0);
  assert(!strcmp(l->pg[p].title, "Tom & Jerry\xE2\x80\x99s \xE2\x9D\xA4 Day"));
  assert(!strcmp(l->pg[p].desc, "Cats <b>and</b> mice."));
  assert(!strcmp(l->pg[p].category, "Kids"));
  assert(l->pg[p].start == NOON - 3600 && l->pg[p].stop == NOON + 1800);

  p = iptv_programme_after(l, 0, NOON + 1);
  assert(p >= 0 && !strcmp(l->pg[p].title, "News"));
  p = iptv_programme_at(l, 0, NOON + 3600);
  assert(p >= 0 && !strcmp(l->pg[p].title, "Film"));
  assert(l->pg[p].stop == NOON + 3 * 3600);   // filled from "Late"
  p = iptv_programme_at(l, 0, NOON + 3 * 3600 + 60);
  assert(p >= 0 && !strcmp(l->pg[p].title, "Late") && l->pg[p].desc[0] == 0);
  assert(iptv_programme_at(l, 0, NOON + 5 * 3600) == -1);
  assert(iptv_programme_after(l, 0, NOON + 5 * 3600) == -1);

  p = iptv_programme_at(l, 1, NOON);
  assert(p >= 0 && l->pg[p].channel == 1 && !strcmp(l->pg[p].title, "Tom & Jerry\xE2\x80\x99s \xE2\x9D\xA4 Day"));
  p = iptv_programme_at(l, 2, NOON);
  assert(p >= 0 && !strcmp(l->pg[p].title, "Headlines"));
  p = iptv_programme_at(l, 3, NOON);
  assert(p >= 0 && !strcmp(l->pg[p].title, "Match"));
  assert(iptv_programme_at(l, 4, NOON) == -1 && iptv_programme_at(l, 99, NOON) == -1);

  // Re-attaching replaces the guide instead of appending to it.
  assert(iptv_parse_xmltv(l, XML, NOON - 6 * 3600, NOON + 24 * 3600) == 10);
  assert(l->ch[0].nPg == 4);
  assert(iptv_parse_xmltv(l, "not xml", 0, NOON * 2) == 0 && l->ch[0].nPg == 0);
}

// iptv-org's way of writing a playlist: no logos, tags after the names, and ids
// the guide does not use. The guide's names and icons are what match.
static void testTagsAndIcons(void) {
  IptvList l;
  iptv_list_init(&l);
  assert(iptv_parse_m3u(&l,
    "#EXTM3U\n"
    "#EXTINF:-1 tvg-id=\"00sReplay.us@SD\",00s Replay (720p) [Geo-blocked]\n"
    "http://s/1.m3u8\n"
    "#EXTINF:-1 tvg-id=\"x.us@SD\",Cops [Not 24/7]\n"
    "http://s/2.m3u8\n"
    // A parenthesis that is part of the name is not cut: "Local" must not match.
    "#EXTINF:-1,Local (US)\n"
    "http://s/3.m3u8\n"
    // Its own logo wins over the guide's.
    "#EXTINF:-1 tvg-logo=\"http://own.png\",Kept (1080p)\n"
    "http://s/4.m3u8\n"
    // Matched by id, logo from the guide.
    "#EXTINF:-1 tvg-id=\"byid\",Whatever\n"
    "http://s/5.m3u8\n") == 5);
  assert(iptv_parse_xmltv(&l,
    "<tv>\n"
    "<channel id=\"a1\"><display-name>00s Replay</display-name><icon src=\"http://i/a1.png\"/></channel>\n"
    "<channel id=\"a2\"><display-name>Cops</display-name><icon src=\"http://i/a2.png\" width=\"1\"/></channel>\n"
    "<channel id=\"a3\"><display-name>Local</display-name><icon src=\"http://i/a3.png\"/></channel>\n"
    "<channel id=\"a4\"><display-name>Kept</display-name><icon src=\"http://i/a4.png\"/></channel>\n"
    "<channel id=\"byid\"><display-name>Other</display-name><icon src=\"http://i/b&amp;c.png\"/></channel>\n"
    "<programme start=\"20260927110000 +0000\" stop=\"20260927130000 +0000\" channel=\"a1\"><title>A</title></programme>\n"
    "<programme start=\"20260927110000 +0000\" stop=\"20260927130000 +0000\" channel=\"a2\"><title>B</title></programme>\n"
    "<programme start=\"20260927110000 +0000\" stop=\"20260927130000 +0000\" channel=\"a3\"><title>C</title></programme>\n"
    "<programme start=\"20260927110000 +0000\" stop=\"20260927130000 +0000\" channel=\"a4\"><title>D</title></programme>\n"
    "</tv>\n", NOON - 3600, NOON + 3600) == 3);
  assert(l.ch[0].nPg == 1 && !strcmp(l.ch[0].logo, "http://i/a1.png"));
  assert(l.ch[1].nPg == 1 && !strcmp(l.ch[1].logo, "http://i/a2.png"));
  assert(l.ch[2].nPg == 0 && !l.ch[2].logo[0]);
  assert(l.ch[3].nPg == 1 && !strcmp(l.ch[3].logo, "http://own.png"));
  assert(l.ch[4].nPg == 0 && !strcmp(l.ch[4].logo, "http://i/b&c.png"));
  iptv_list_free(&l);
}

// A large synthetic guide: nothing quadratic, nothing leaked (ASan).
static void testScale(void) {
  IptvList l;
  size_t cap = 64u << 20, n = 0;
  char *m3u = malloc(cap), *xml = malloc(cap);
  const int CH = 3000, PER = 40;
  assert(m3u && xml);
  iptv_list_init(&l);
  n += (size_t)snprintf(m3u + n, cap - n, "#EXTM3U\n");
  for (int i = 0; i < CH; i++)
    n += (size_t)snprintf(m3u + n, cap - n,
                          "#EXTINF:-1 tvg-id=\"c%d\" group-title=\"G%d\",Channel %d\nhttp://x/%d.ts\n",
                          i, i % 50, i, i);
  assert(iptv_parse_m3u(&l, m3u) == CH && l.nGroups == 50);
  n = 0;
  n += (size_t)snprintf(xml + n, cap - n, "<tv>\n");
  for (int i = 0; i < CH; i++)
    for (int k = 0; k < PER; k++) {
      long long s = NOON - 12 * 3600 + k * 1800LL;
      int y, mo, d, h, mi;
      // Back to a timestamp through the parser's own inverse would be circular;
      // build it from the known noon instead: 2026-09-27 00:00Z plus k halves.
      long long off = s - (NOON - 12 * 3600);
      h = (int)(off / 3600); mi = (int)(off % 3600 / 60);
      y = 2026; mo = 9; d = 27 + h / 24; h %= 24;
      n += (size_t)snprintf(xml + n, cap - n,
          "<programme start=\"%04d%02d%02d%02d%02d00 +0000\" stop=\"%04d%02d%02d%02d%02d00 +0000\" "
          "channel=\"c%d\"><title>Show %d</title><desc>About &amp; more</desc></programme>\n",
          y, mo, d, h, mi, y, mo, d + (mi == 30 && h == 23), mi == 30 ? (h + 1) % 24 : h,
          mi == 30 ? 0 : 30, i, k);
    }
  n += (size_t)snprintf(xml + n, cap - n, "</tv>\n");
  { int kept = iptv_parse_xmltv(&l, xml, NOON - 3 * 3600, NOON + 3 * 3600);
    // Twelve half-hours inside a six-hour window, per channel.
    assert(kept == CH * 12); }
  for (int i = 0; i < CH; i += 97) {
    int p = iptv_programme_at(&l, i, NOON + 60);
    assert(p >= 0 && l.pg[p].channel == i && !strcmp(l.pg[p].title, "Show 24"));
  }
  iptv_list_free(&l);
  free(m3u); free(xml);
}

// The guide as it usually arrives: guide.xml.gz. `path` is written by
// tests/iptv_parse.sh with the system gzip, two members concatenated.
static void testGunzip(const char *path) {
  FILE *f = fopen(path, "rb");
  long n, m = 0;
  char *raw, *xml;
  IptvList l;
  assert(f);
  fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
  raw = malloc((size_t)n);
  assert(raw && fread(raw, 1, (size_t)n, f) == (size_t)n);
  fclose(f);
  xml = iptv_gunzip(raw, n, &m);
  assert(xml && m == (long)strlen(xml));
  assert(strstr(xml, "<tv ") && strstr(xml, "</tv>"));   // both members
  iptv_list_init(&l);
  assert(iptv_parse_m3u(&l, M3U) == 5);
  assert(iptv_parse_xmltv(&l, xml, NOON - 6 * 3600, NOON + 24 * 3600) == 10);
  iptv_list_free(&l);
  free(xml);
  // Cut while inflating: the whole window keeps all ten; a narrow one keeps
  // exactly what a parse over that window would.
  { char *w = iptv_guide_window(raw, n, NOON - 6 * 3600, NOON + 24 * 3600, &m);
    assert(w && m == (long)strlen(w) && strstr(w, "<channel"));
    iptv_list_init(&l);
    assert(iptv_parse_m3u(&l, M3U) == 5);
    assert(iptv_parse_xmltv(&l, w, NOON - 6 * 3600, NOON + 24 * 3600) == 10);
    iptv_list_free(&l);
    free(w); }
  // Not compressed: NULL, and the caller keeps the plain text.
  assert(iptv_gunzip("<tv></tv>", 9, &m) == NULL && m == 0);
  // A corrupt stream after a valid magic.
  { char bad[64] = { 0x1f, (char)0x8b, 8, 0, 0, 0, 0, 0, 0, 3, 'x', 'y', 'z' };
    assert(iptv_gunzip(bad, sizeof bad, &m) == NULL); }
  free(raw);
}

// Xtream's JSON as panels send it: PHP's json_encode escapes every non-ASCII
// character and every slash, ids arrive as numbers or as strings, nulls where a
// field is missing, and objects nested where nothing here looks.
static void testXtream(void) {
  static const char *CATS =
    "[{\"category_id\":\"1\",\"category_name\":\"T\\u00fcrkiye | Ulusal\",\"parent_id\":0},"
    " {\"category_id\":\"2\",\"category_name\":\"Sport \\\"HD\\\"\",\"parent_id\":0}]";
  static const char *STREAMS =
    "[{\"num\":1,\"name\":\"TRT 1 \\ud83d\\udcfa\",\"stream_type\":\"live\",\"stream_id\":1234,"
    "\"stream_icon\":\"http:\\/\\/logos.example\\/trt1.png\",\"epg_channel_id\":\"trt1.tr\","
    "\"added\":\"1700000000\",\"category_id\":\"1\",\"tv_archive\":1,\"tv_archive_duration\":\"3\","
    "\"extra\":{\"name\":\"not this\",\"list\":[1,{\"a\":\"]\"}]}},\n"
    " {\"num\":\"2\",\"name\":\"Sky Sports News, HD\",\"stream_id\":\"77\",\"stream_icon\":\"\","
    "\"epg_channel_id\":null,\"category_id\":\"2\",\"tv_archive\":0,\"tv_archive_duration\":0},"
    " {\"num\":3,\"name\":\"\",\"stream_id\":5},"
    " {\"num\":4,\"name\":\"No group\",\"stream_id\":9,\"category_id\":\"99\"}]";
  IptvList l;
  char *m3u = iptv_xtream_m3u(STREAMS, CATS, "http://p.example:8080", "user", "pass", "m3u8");
  assert(m3u);
  iptv_list_init(&l);
  assert(iptv_parse_m3u(&l, m3u) == 3);             // the nameless one is dropped
  assert(!strcmp(l.ch[0].name, "TRT 1 \xF0\x9F\x93\xBA"));   // a surrogate pair, whole
  assert(!strcmp(l.ch[0].url, "http://p.example:8080/live/user/pass/1234.m3u8"));
  assert(!strcmp(l.ch[0].logo, "http://logos.example/trt1.png"));
  assert(!strcmp(l.ch[0].tvgId, "trt1.tr"));
  assert(!strcmp(l.ch[0].group, "T\xC3\xBCrkiye | Ulusal"));
  assert(l.ch[0].number == 1 && l.ch[0].catchupDays == 3);
  // A comma in the name survives as a low comma; a quote in a group as '.
  assert(!strcmp(l.ch[1].name, "Sky Sports News\xE2\x80\x9A HD"));
  assert(!strcmp(l.ch[1].group, "Sport 'HD'"));
  assert(!strcmp(l.ch[1].url, "http://p.example:8080/live/user/pass/77.m3u8"));
  assert(l.ch[1].number == 2 && l.ch[1].catchupDays == 0 && !l.ch[1].tvgId[0]);
  assert(!strcmp(l.ch[2].name, "No group") && !l.ch[2].group[0]);
  iptv_list_free(&l);
  free(m3u);
  // Raw TS when that is what the account allows; no categories at all.
  m3u = iptv_xtream_m3u(STREAMS, NULL, "http://p.example", "u", "p", "ts");
  assert(m3u && strstr(m3u, "http://p.example/live/u/p/1234.ts\n"));
  free(m3u);
  // Not an array: an expired account's object, an HTML page.
  assert(!iptv_xtream_m3u("{\"user_info\":{\"auth\":0}}", NULL, "h", "u", "p", "m3u8"));
  assert(!iptv_xtream_m3u("<html>884</html>", NULL, "h", "u", "p", "m3u8"));
  // An empty array is a playlist with no channels, which the loader reports.
  m3u = iptv_xtream_m3u("[]", "[]", "h", "u", "p", "m3u8");
  iptv_list_init(&l);
  assert(m3u && iptv_parse_m3u(&l, m3u) == 0);
  iptv_list_free(&l);
  free(m3u);
}

// Decorated names as they come in real playlists, and real scripts left alone.
static void testPlain(void) {
  static const struct { const char *in, *out; } CASES[] = {
    { "BBC One \xE1\xB4\xB4\xE1\xB4\xB0", "BBC One HD" },                          // ᴴᴰ
    { "\xE1\xB5\x81\xE1\xB4\xB4\xE1\xB4\xB0 \xE2\x81\xB4\xE1\xB4\xB7", "UHD 4K" },   // ᵁᴴᴰ ⁴ᴷ
    { "\xCA\x80\xE1\xB4\x80\xE1\xB4\xA1", "RAW" },                                 // ʀᴀᴡ
    { "\xF0\x9D\x90\x92\xF0\x9D\x90\xA4\xF0\x9D\x90\xB2 1", "Sky 1" },             // 𝐒𝐤𝐲 (bold)
    { "\xF0\x9D\x98\x9A\xF0\x9D\x98\xB1\xF0\x9D\x98\xB0", "Spo" },                // 𝘚𝘱𝘰 (sans italic)
    { "\xF0\x9D\x9F\x8F\xF0\x9D\x9F\x8E", "10" },                                  // 𝟏𝟎
    { "\xE2\x93\x88\xE2\x93\x9F\xE2\x93\x9E", "Spo" },                            // Ⓢⓟⓞ
    { "\xEF\xBC\xB5\xEF\xBC\xAB\xEF\xBC\x9A News", "UK: News" },                     // ＵＫ：
    { "\xF0\x9F\x85\xB7\xF0\x9F\x85\xB3 Cinema", "HD Cinema" },                      // 🅷🅳
    { "Channel \xE2\x91\xB3", "Channel 20" },                                          // ⑳
    { "\xE2\x84\x8D\xE2\x84\x99", "HP" },                                            // ℍℙ
    // Left alone: accents, Greek, Cyrillic, Arabic, CJK, and the symbols text.c drops.
    { "T\xC3\xBCrkiye \xCE\x95\xCE\xA1\xCE\xA4 \xD0\x9F\xD0\xB5\xD1\x80\xD0\xB2\xD1\x8B\xD0\xB9",
      "T\xC3\xBCrkiye \xCE\x95\xCE\xA1\xCE\xA4 \xD0\x9F\xD0\xB5\xD1\x80\xD0\xB2\xD1\x8B\xD0\xB9" },
    { "\xD8\xA7\xD9\x84\xD8\xAC\xD8\xB2\xD9\x8A\xD8\xB1\xD8\xA9 \xE6\x97\xA5\xE6\x9C\xAC",
      "\xD8\xA7\xD9\x84\xD8\xAC\xD8\xB2\xD9\x8A\xD8\xB1\xD8\xA9 \xE6\x97\xA5\xE6\x9C\xAC" },
    { "\xE2\x9A\xA1 Live \xF0\x9F\x87\xAC\xF0\x9F\x87\xA7", "\xE2\x9A\xA1 Live \xF0\x9F\x87\xAC\xF0\x9F\x87\xA7" },
    { "bad \xFF\xC3 end", "bad \xFF\xC3 end" },                                        // not UTF-8
  };
  for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
    char b[128];
    snprintf(b, sizeof b, "%s", CASES[i].in);
    iptv_plain_text(b);
    if (strcmp(b, CASES[i].out)) { printf("plain %zu: '%s' -> '%s'\n", i, CASES[i].in, b); assert(0); }
  }
  // Through the parser: the name and the group come out plain, tvg-name as
  // written (the guide matches on it).
  { IptvList l;
    iptv_list_init(&l);
    assert(iptv_parse_m3u(&l, "#EXTM3U\n#EXTINF:-1 tvg-name=\"BBC \xE1\xB4\xB4\xE1\xB4\xB0\" "
                              "group-title=\"\xEF\xBC\xB5\xEF\xBC\xAB\",BBC \xE1\xB4\xB4\xE1\xB4\xB0\n"
                              "http://x/1\n"
                              "#EXTINF:-1 tvg-name=\"\xCA\x80\xE1\xB4\x80\xE1\xB4\xA1\",\n"
                              "http://x/2\n") == 2);
    assert(!strcmp(l.ch[0].name, "BBC HD") && !strcmp(l.ch[0].group, "UK"));
    assert(!strcmp(l.ch[0].tvgName, "BBC \xE1\xB4\xB4\xE1\xB4\xB0"));
    assert(!strcmp(l.ch[1].name, "RAW"));            // named from tvg-name, plain
    assert(!strcmp(l.ch[1].tvgName, "\xCA\x80\xE1\xB4\x80\xE1\xB4\xA1"));
    assert(l.nGroups == 1 && !strcmp(l.groups[0], "UK"));
    iptv_list_free(&l); }
}

// A channel in several copies, only one with the guide's id, and names dressed
// as IPTV playlists dress them: every copy gets the guide; a +1 does not.
static void testLooseMatch(void) {
  IptvList l;
  long long st = iptv_xmltv_time("20260927180000 +0000");
  const char *m3u =
    "#EXTM3U\n"
    "#EXTINF:-1 tvg-id=\"ssf1.uk\",Sky Sports F1 HD\n"                 "http://x/1\n"
    "#EXTINF:-1,Sky Sports F1 4K\n"                                      "http://x/2\n"
    "#EXTINF:-1,UK: SKY SPORTS F1 \xE1\xB5\x81\xE1\xB4\xB4\xE1\xB4\xB0 HEVC\n" "http://x/3\n"
    "#EXTINF:-1,|NL| NPO 1 FHD [50fps]\n"                                "http://x/4\n"
    "#EXTINF:-1,Discovery +1\n"                                          "http://x/5\n"
    "#EXTINF:-1 tvg-id=\"disc.uk\",Discovery\n"                         "http://x/6\n";
  const char *xml =
    "<tv><channel id=\"ssf1.uk\"><display-name>Sky Sports F1</display-name></channel>"
    "<channel id=\"npo1.nl\"><display-name>NPO 1</display-name></channel>"
    "<channel id=\"disc.uk\"><display-name>Discovery</display-name></channel>"
    "<programme start=\"20260927180000 +0000\" stop=\"20260927190000 +0000\" channel=\"ssf1.uk\"><title>Qualifying</title></programme>"
    "<programme start=\"20260927190000 +0000\" stop=\"20260927200000 +0000\" channel=\"ssf1.uk\"><title>Race</title></programme>"
    "<programme start=\"20260927180000 +0000\" stop=\"20260927190000 +0000\" channel=\"npo1.nl\"><title>Journaal</title></programme>"
    "<programme start=\"20260927180000 +0000\" stop=\"20260927190000 +0000\" channel=\"disc.uk\"><title>Gold Rush</title></programme>"
    "</tv>";
  iptv_list_init(&l);
  assert(iptv_parse_m3u(&l, m3u) == 6);
  iptv_parse_xmltv(&l, xml, st - 3600, st + 86400);
  for (int c = 0; c < 3; c++) {
    int p = iptv_programme_at(&l, c, st + 60);
    assert(l.ch[c].nPg == 2 && p >= 0 && !strcmp(l.pg[p].title, "Qualifying"));
  }
  assert(l.ch[3].nPg == 1 && !strcmp(l.pg[l.ch[3].firstPg].title, "Journaal"));
  assert(l.ch[4].nPg == 0);                         // +1 is another schedule
  assert(l.ch[5].nPg == 1);
  // Sorted, and each channel's range is its own.
  for (int i = 1; i < l.nPg; i++)
    assert(l.pg[i - 1].channel < l.pg[i].channel ||
           (l.pg[i - 1].channel == l.pg[i].channel && l.pg[i - 1].start < l.pg[i].start));
  iptv_list_free(&l);
}

// One guide reaching one channel twice, by its tvg-id and by another guide
// channel's display name, feeds it ONE schedule: the id's. Two interleaved
// schedules drew as overlapping blocks. With no id, the first name wins.
static void testOneFeed(void) {
  IptvList l;
  long long st = iptv_xmltv_time("20260927180000 +0000");
  const char *m3u =
    "#EXTINF:-1 tvg-id=\"zs4.nl\",NL: ZIGGO SPORT 4\n"  "http://x/1\n"
    "#EXTINF:-1,NL: ZIGGO SPORT 5\n"                     "http://x/2\n";
  const char *xml =
    "<tv><channel id=\"other4\"><display-name>Ziggo Sport 4 HD</display-name></channel>"
    "<channel id=\"zs4.nl\"><display-name>Ziggo Sport 4</display-name></channel>"
    "<channel id=\"a5\"><display-name>Ziggo Sport 5</display-name></channel>"
    "<channel id=\"b5\"><display-name>Ziggo Sport 5 HD</display-name></channel>"
    "<programme start=\"20260927180000 +0000\" stop=\"20260927190000 +0000\" channel=\"other4\"><title>Wrong</title></programme>"
    "<programme start=\"20260927181500 +0000\" stop=\"20260927191500 +0000\" channel=\"zs4.nl\"><title>Tennis</title></programme>"
    "<programme start=\"20260927180000 +0000\" stop=\"20260927190000 +0000\" channel=\"a5\"><title>Golf</title></programme>"
    "<programme start=\"20260927183000 +0000\" stop=\"20260927193000 +0000\" channel=\"b5\"><title>Darts</title></programme>"
    "</tv>";
  iptv_list_init(&l);
  assert(iptv_parse_m3u(&l, m3u) == 2);
  iptv_parse_xmltv(&l, xml, st - 3600, st + 86400);
  assert(l.ch[0].nPg == 1 && !strcmp(l.pg[l.ch[0].firstPg].title, "Tennis"));
  assert(l.ch[1].nPg == 1 && !strcmp(l.pg[l.ch[1].firstPg].title, "Golf"));
  iptv_list_free(&l);
}

// One guide id carrying two schedules ten minutes apart, as a provider's
// merged feed does: the first timeline stays, the programmes starting inside
// it go, and a two-minute overlap is trimmed rather than dropped.
static void testOneTimeline(void) {
  IptvList l;
  long long st = iptv_xmltv_time("20260928130000 +0000");
  const char *m3u = "#EXTINF:-1 tvg-id=\"e1\",Eleven 1\n" "http://x/1\n";
  const char *xml =
    "<tv><programme start=\"20260928130000 +0000\" stop=\"20260928132500 +0000\" channel=\"e1\"><title>Show A</title></programme>"
    "<programme start=\"20260928131000 +0000\" stop=\"20260928133500 +0000\" channel=\"e1\"><title>Show B</title></programme>"
    "<programme start=\"20260928132500 +0000\" stop=\"20260928152700 +0000\" channel=\"e1\"><title>Ligue 1</title></programme>"
    "<programme start=\"20260928133500 +0000\" stop=\"20260928140000 +0000\" channel=\"e1\"><title>Show B2</title></programme>"
    "<programme start=\"20260928152500 +0000\" stop=\"20260928170000 +0000\" channel=\"e1\"><title>Late</title></programme>"
    "</tv>";
  iptv_list_init(&l);
  assert(iptv_parse_m3u(&l, m3u) == 1);
  iptv_parse_xmltv(&l, xml, st - 3600, st + 86400);
  assert(l.ch[0].nPg == 3);
  { const IptvProgramme *p = &l.pg[l.ch[0].firstPg];
    assert(!strcmp(p[0].title, "Show A") && !strcmp(p[1].title, "Ligue 1") && !strcmp(p[2].title, "Late"));
    assert(p[1].stop == p[2].start); }
  iptv_list_free(&l);
}

// A second guide fills the channels the first left empty, and only those: a
// channel the first guide covers keeps that schedule even when the second one
// also knows it.
static void testMoreGuides(void) {
  IptvList l;
  long long st = iptv_xmltv_time("20260927180000 +0000");
  const char *m3u =
    "#EXTM3U\n"
    "#EXTINF:-1 tvg-id=\"a.uk\",Alpha\n"         "http://x/1\n"
    "#EXTINF:-1 tvg-id=\"b.uk\",UK - Beta HD\n"  "http://x/2\n"
    "#EXTINF:-1,Beta 4K\n"                       "http://x/3\n"
    "#EXTINF:-1,Gamma\n"                         "http://x/4\n";
  const char *first =
    "<tv><programme start=\"20260927180000 +0000\" stop=\"20260927190000 +0000\" channel=\"a.uk\">"
    "<title>From the provider</title></programme></tv>";
  const char *second =
    "<tv><channel id=\"Alpha.x\"><display-name>Alpha</display-name></channel>"
    "<channel id=\"Beta.x\"><display-name>Beta</display-name></channel>"
    "<programme start=\"20260927180000 +0000\" stop=\"20260927190000 +0000\" channel=\"Alpha.x\"><title>Other</title></programme>"
    "<programme start=\"20260927183000 +0000\" stop=\"20260927193000 +0000\" channel=\"Alpha.x\"><title>Other 2</title></programme>"
    "<programme start=\"20260927180000 +0000\" stop=\"20260927190000 +0000\" channel=\"Beta.x\"><title>Beta show</title></programme>"
    "</tv>";
  iptv_list_init(&l);
  assert(iptv_parse_m3u(&l, m3u) == 4);
  assert(iptv_parse_xmltv(&l, first, st - 3600, st + 86400) == 1);
  assert(l.ch[1].nPg == 0);
  assert(iptv_parse_xmltv_more(&l, second, st - 3600, st + 86400) == 2);
  assert(l.ch[0].nPg == 1 && !strcmp(l.pg[l.ch[0].firstPg].title, "From the provider"));
  assert(l.ch[1].nPg == 1 && !strcmp(l.pg[l.ch[1].firstPg].title, "Beta show"));
  assert(l.ch[2].nPg == 1 && !strcmp(l.pg[l.ch[2].firstPg].title, "Beta show"));   // sibling
  assert(l.ch[3].nPg == 0);
  assert(iptv_parse_xmltv_more(&l, second, st - 3600, st + 86400) == 0);        // nothing left it knows
  for (int i = 1; i < l.nPg; i++)
    assert(l.pg[i - 1].channel < l.pg[i].channel ||
           (l.pg[i - 1].channel == l.pg[i].channel && l.pg[i - 1].start < l.pg[i].start));
  iptv_list_free(&l);
}

// A guide large enough to cross the trim's chunks many times, with long
// descriptions straddling them: the window cut keeps exactly the programmes a
// parse of the whole text keeps, plain and gzipped (the script gzips it).
static char *bigGuide(void) {
  size_t cap = 12u << 20, k = 0;
  char *x = malloc(cap), desc[3001];
  long long t0 = iptv_xmltv_time("20260920000000 +0000");
  assert(x);
  memset(desc, 'd', 3000); desc[3000] = 0;
  k += (size_t)snprintf(x + k, cap - k, "<?xml version=\"1.0\"?>\n<tv>\n");
  for (int c = 0; c < 20; c++)
    k += (size_t)snprintf(x + k, cap - k, "<channel id=\"c%d\"><display-name>Chan %d</display-name></channel>\n", c, c);
  for (int c = 0; c < 20; c++)
    for (long long t = t0; t < t0 + 7 * 86400; t += 3600) {
      struct tm tm; time_t a = (time_t)t, b = (time_t)(t + 3600);
      char s1[32], s2[32];
      gmtime_r(&a, &tm); strftime(s1, sizeof s1, "%Y%m%d%H%M%S +0000", &tm);
      gmtime_r(&b, &tm); strftime(s2, sizeof s2, "%Y%m%d%H%M%S +0000", &tm);
      k += (size_t)snprintf(x + k, cap - k, "<programme start=\"%s\" stop=\"%s\" channel=\"c%d\">"
                            "<title>T%lld</title><desc>%s</desc></programme>\n",
                            s1, s2, c, t, (t / 3600) % 5 ? "short" : desc);
      assert(k < cap - 8192);
    }
  snprintf(x + k, cap - k, "</tv>\n");
  return x;
}

static void testWindow(const char *gzPath) {
  char *x = bigGuide(), *w;
  long m = 0, n;
  long long t0 = iptv_xmltv_time("20260923120000 +0000"), from = t0 - 3 * 3600, to = t0 + 30 * 3600;
  IptvList a, b;
  char m3u[4096];
  size_t k = (size_t)snprintf(m3u, sizeof m3u, "#EXTM3U\n");
  for (int c = 0; c < 20; c++)
    k += (size_t)snprintf(m3u + k, sizeof m3u - k, "#EXTINF:-1 tvg-id=\"c%d\",Chan %d\nhttp://x/%d\n", c, c, c);
  iptv_list_init(&a); iptv_list_init(&b);
  assert(iptv_parse_m3u(&a, m3u) == 20 && iptv_parse_m3u(&b, m3u) == 20);
  n = iptv_parse_xmltv(&a, x, from, to);
  assert(n == 20 * 33);                       // 33 hours, on the hour
  w = iptv_guide_window(x, (long)strlen(x), from, to, &m);
  assert(w && m < (long)strlen(x) / 4);
  assert(iptv_parse_xmltv(&b, w, from, to) == n);
  for (int i = 0; i < n; i++) assert(a.pg[i].start == b.pg[i].start && !strcmp(a.pg[i].desc, b.pg[i].desc));
  free(w);
  if (gzPath) {
    FILE *f = fopen(gzPath, "rb");
    char *raw;
    long rn;
    assert(f);
    fseek(f, 0, SEEK_END); rn = ftell(f); fseek(f, 0, SEEK_SET);
    raw = malloc((size_t)rn);
    assert(raw && fread(raw, 1, (size_t)rn, f) == (size_t)rn);
    fclose(f);
    w = iptv_guide_window(raw, rn, from, to, &m);
    assert(w);
    iptv_list_free(&b); iptv_list_init(&b);
    assert(iptv_parse_m3u(&b, m3u) == 20);
    assert(iptv_parse_xmltv(&b, w, from, to) == n);
    free(w); free(raw);
  }
  iptv_list_free(&a); iptv_list_free(&b);
  free(x);
}

static void testCatchup(void) {
  IptvList l;
  char u[1024];
  long long st = iptv_xmltv_time("20260927180000 +0000"), en = st + 1800, now = st + 3600;
  const char *m3u =
    "#EXTM3U catchup=\"shift\" catchup-days=\"5\"\n"
    "#EXTINF:-1 tvg-id=\"a\",Shifted\n"
    "http://h/a.m3u8?t=1\n"
    "#EXTINF:-1 catchup=\"default\" catchup-source=\"http://arc/{utc}/{utcend}/{duration:60}/${offset}/{Y}{m}{d}-{H}{M}{S}/{utc:Y-m-d H:M}/{lutc}\" catchup-days=\"2\",Templated\n"
    "http://h/b.ts\n"
    "#EXTINF:-1 catchup=\"append\" catchup-source=\"?start={utc}&end={utcend}\",Appended\n"
    "http://h/c.m3u8\n"
    "#EXTINF:-1 catchup=\"fs\",Fluss\n"
    "http://f/ch/index.m3u8?token=x\n"
    "#EXTINF:-1 catchup=\"flussonic\",FlussTs\n"
    "http://f/ch/mpegts\n"
    "#EXTINF:-1 catchup=\"xc\",Xtream\n"
    "http://x:8080/live/u1/p1/1234.ts\n"
    "#EXTINF:-1 catchup=\"xc\",XtreamBare\n"
    "http://x:8080/u1/p1/99\n";
  iptv_list_init(&l);
  assert(iptv_parse_m3u(&l, m3u) == 7);
  // The header's defaults reach a channel with none of its own.
  assert(l.ch[0].catchup == IPTV_CATCHUP_SHIFT && l.ch[0].catchupDays == 5);
  assert(iptv_catchup_url(&l.ch[0], st, en, now, 0, u, sizeof u));
  assert(!strcmp(u, "http://h/a.m3u8?t=1&utc=1790532000&lutc=1790535600"));
  assert(l.ch[1].catchup == IPTV_CATCHUP_DEFAULT && l.ch[1].catchupDays == 2);
  assert(iptv_catchup_url(&l.ch[1], st, en, now, 0, u, sizeof u));
  assert(!strcmp(u, "http://arc/1790532000/1790533800/30/3600/20260927-180000/2026-09-27 18:00/1790535600"));
  assert(iptv_catchup_url(&l.ch[2], st, en, now, 0, u, sizeof u));
  assert(!strcmp(u, "http://h/c.m3u8?start=1790532000&end=1790533800"));
  assert(iptv_catchup_url(&l.ch[3], st, en, now, 0, u, sizeof u));
  assert(!strcmp(u, "http://f/ch/index-1790532000-1800.m3u8?token=x"));
  assert(iptv_catchup_url(&l.ch[4], st, en, now, 0, u, sizeof u));
  assert(!strcmp(u, "http://f/ch/timeshift_abs-1790532000.ts"));
  // Xtream: whole minutes, rounded up, in the server's local time (+2 h here).
  assert(iptv_catchup_url(&l.ch[5], st, en + 10, now, 7200, u, sizeof u));
  assert(!strcmp(u, "http://x:8080/timeshift/u1/p1/31/2026-09-27:20-00/1234.ts"));
  assert(iptv_catchup_url(&l.ch[6], st, en, now, 0, u, sizeof u));
  assert(!strcmp(u, "http://x:8080/timeshift/u1/p1/30/2026-09-27:18-00/99.ts"));
  // Too small a buffer is a refusal, not a truncation.
  assert(!iptv_catchup_url(&l.ch[1], st, en, now, 0, u, 20));
  iptv_list_free(&l);
  // Days and an Xtream-shaped URL, no mode anywhere: Xtream's own archive.
  iptv_list_init(&l);
  assert(iptv_parse_m3u(&l, "#EXTM3U\n#EXTINF:-1 tvg-rec=\"3\",RecOnly\nhttp://x:8080/live/u1/p1/77.m3u8\n") == 1);
  assert(l.ch[0].catchup == IPTV_CATCHUP_XC && l.ch[0].catchupDays == 3);
  assert(iptv_catchup_url(&l.ch[0], st, en, now, 0, u, sizeof u));
  assert(!strcmp(u, "http://x:8080/timeshift/u1/p1/30/2026-09-27:18-00/77.m3u8"));
  iptv_list_free(&l);
  // A playlist without any of it: no catch-up anywhere.
  iptv_list_init(&l);
  assert(iptv_parse_m3u(&l, "#EXTM3U\n#EXTINF:-1,Plain\nhttp://h/p.m3u8\n") == 1);
  assert(l.ch[0].catchup == IPTV_CATCHUP_NONE && !l.ch[0].catchupDays);
  assert(!iptv_catchup_url(&l.ch[0], st, en, now, 0, u, sizeof u));
  iptv_list_free(&l);
  { long id; char o[64], us[16], pw[16], ex[8];
    assert(iptv_xtream_parts("https://s.tv:443/live/a/b/12.m3u8?x", o, sizeof o, us, sizeof us, pw, sizeof pw, &id, ex, sizeof ex));
    assert(!strcmp(o, "https://s.tv:443") && !strcmp(us, "a") && !strcmp(pw, "b") && id == 12 && !strcmp(ex, "m3u8"));
    assert(!iptv_xtream_parts("http://h/some/deep/path/a/b/12.ts", 0, 0, 0, 0, 0, 0, 0, 0, 0));
    assert(!iptv_xtream_parts("http://h/live/a/b/name.ts", 0, 0, 0, 0, 0, 0, 0, 0, 0)); }
}

int main(int argc, char **argv) {
  IptvList l;
  if (argc > 2 && !strcmp(argv[1], "--dump-big")) {
    char *x = bigGuide();
    FILE *f = fopen(argv[2], "wb");
    assert(f);
    fwrite(x, 1, strlen(x), f);
    fclose(f); free(x);
    return 0;
  }
  if (argc > 3 && !strcmp(argv[1], "--dump-xml")) {
    // The script asks for the guide text to gzip it, in two halves.
    size_t half = strlen(XML) / 2;
    FILE *a = fopen(argv[2], "wb"), *b = fopen(argv[3], "wb");
    assert(a && b);
    fwrite(XML, 1, half, a); fwrite(XML + half, 1, strlen(XML) - half, b);
    fclose(a); fclose(b);
    return 0;
  }
  testTime();
  testEntities();
  testOneFeed();
  testOneTimeline();
  iptv_list_init(&l);
  testM3u(&l);
  testXmltv(&l);
  iptv_list_free(&l);
  testTagsAndIcons();
  testScale();
  testXtream();
  testPlain();
  testCatchup();
  testLooseMatch();
  testMoreGuides();
  if (argc > 1) testGunzip(argv[1]);
  testWindow(argc > 2 ? argv[2] : NULL);
  puts("PASS iptv_parse: M3U attributes, headers, groups; XMLTV times, entities, matching, tags, icons, window; Xtream API; stylised Latin; catch-up URLs; loose guide matching; further guides; gzip; guides cut to the window while inflating.");
  return 0;
}
