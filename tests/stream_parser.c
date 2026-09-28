// The parser in isolation: allows ASan without loading macOS's SDL initialiser.
#include "streams.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// One source, from the JSON an addon would really send. `name` and
// `description` are the two fields every aggregator fills; the tokens are read
// out of both together, which is why the tests below split the facts across
// them the way the addons do.
static Stream one(const char *name, const char *description, const char *url) {
  char json[4000];
  Stream *v = NULL, s;
  int count;
  snprintf(json, sizeof json,
           "{\"streams\":[{\"url\":\"%s\",\"name\":\"%s\",\"description\":\"%s\"}]}",
           url, name, description);
  count = stream_parse(json, "Torrentio", &v);
  assert(count == 1);
  s = v[0];
  free(v);
  return s;
}

int main(void) {
  char json[32000];size_t n=0;
  n+=snprintf(json+n,sizeof json-n,"{\"streams\":[");
  for(int i=0;i<100;i++) n+=snprintf(json+n,sizeof json-n,
    "%s{\"url\":\"https://example.invalid/%d\",\"behaviorHints\":{\"filename\":\"title.%s\"}}",
    i?",":"",i,i==99?"2160p.DV.Atmos.mp4":"1080p.DVDRip.mkv");
  snprintf(json+n,sizeof json-n,"]}");
  Stream *v=NULL;int count=stream_parse(json,"fixture",&v);
  assert(count==100 && v[99].mp4 && v[99].dolbyVision && v[99].height==2160);
  assert(!v[0].dolbyVision);free(v);
  count=stream_parse("{\"streams\":[]}","fixture",&v);assert(count==0);free(v);

  // --- THE ROW'S TOKENS ------------------------------------------------------
  // Torrentio's shape: the release name in `name`, the swarm and the size in
  // `description`, both marked with EMOJI the app's font cannot draw. The number
  // has to survive the parse or the seed count reaches the screen as a box.
  { Stream s = one("Torrentio 4k",
                   "Film.2024.2160p.UHD.BluRay.REMUX.DV.HDR.TrueHD.Atmos.7.1.x265-GRP "
                   "\\n\xF0\x9F\x91\xA4 48 \xF0\x9F\x92\xBE 54.3 GB",
                   "https://example.invalid/a.mkv");
    assert(!strcmp(s.res, "4K"));
    assert(!strcmp(s.range, "DV"));
    assert(!strcmp(s.source, "REMUX"));
    assert(!strcmp(s.audio, "ATMOS 7.1"));
    assert(!strcmp(s.codec, "HEVC"));
    assert(s.seeders == 48);
    // NOT cached, and so P2P — but the word "Torrentio" is what used to decide
    // that, which made every cached row from the addon a torrent too.
    assert(s.p2p == 1 && s.cached == 0);
    assert(s.tier == 3); }

  // The same file through a debrid cache: instant, no swarm worth reading, and a
  // bitrate the aggregator states itself.
  { Stream s = one("[RD+] AIOStreams",
                   "Film.2024.1080p.WEB-DL.DDP5.1.H.264-GRP \\n8.4 GB \\n12.4 Mbps",
                   "https://example.invalid/b.mkv");
    assert(!strcmp(s.res, "1080p"));
    assert(!s.range[0]);              // SDR shows nothing
    assert(!strcmp(s.source, "WEB-DL"));
    assert(!strcmp(s.audio, "EAC3 5.1"));
    assert(!strcmp(s.codec, "H.264"));
    assert(s.cached == 1 && s.p2p == 0);
    assert(s.mbps > 12.3f && s.mbps < 12.5f); }

  // "Not cached" is a torrent, whatever else the blob says.
  { Stream s = one("AIOStreams", "Not Cached \\nFilm.2024.720p.WEBRip.AAC-GRP \\nSeeders: 3",
                   "https://example.invalid/c.mkv");
    assert(s.cached == 0 && s.p2p == 1 && s.seeders == 3);
    // Three seeders on a webrip is the bottom of the scale, and the bar has to
    // say so — that is the whole reason POOR exists.
    assert(s.tier == 0); }

  // THE BITRATE THE ADDON DID NOT STATE. Torrentio sends a size and nothing
  // else; it only becomes a number once the sheet says what the size is a size
  // of. 8000 MB over 100 minutes is 10.7 Mbps.
  { Stream s = one("Torrentio", "Film.2024.1080p.BluRay.x264-GRP \\n\xF0\x9F\x92\xBE 8 GB",
                   "https://example.invalid/d.mkv");
    assert(s.sizeMB == 8192 && s.mbps == 0);
    stream_rank(&s, 100 * 60);
    assert(s.mbps > 10.5f && s.mbps < 11.0f);
    // And calling again never overwrites a bitrate already worked out.
    stream_rank(&s, 30 * 60);
    assert(s.mbps > 10.5f && s.mbps < 11.0f); }

  // A CAM is the one source that can reach POOR on its own.
  { Stream s = one("[RD+] AIOStreams", "Film.2024.720p.HDCAM.AAC-GRP \\n1.2 GB",
                   "https://example.invalid/e.mkv");
    assert(!strcmp(s.source, "CAM") && s.tier == 0); }

  // --- THE UPSTREAM SERVICE --------------------------------------------------
  // The exact shape MEASURED on the device: state, service, stars, held apart by
  // typographic spaces and a zero-width joiner.
  { Stream s = one("Cached\xE2\x80\x82\xE2\x80\x82" "Comet" "\xE2\x80\x8D\xE2\x80\x82\xE2\x80\x82"
                   "\xE2\x98\x85\xE2\x98\x85\xE2\x98\x85\xE2\x98\x85\xE2\x98\x85",
                   "49 \xE1\xB4\xB9\xE1\xB5\x87\xE1\xB5\x96\xCB\xA2",
                   "https://example.invalid/f.mkv");
    assert(!strcmp(s.service, "Comet"));
    assert(s.cached == 1 && s.p2p == 0);
    assert(s.mbps > 48.9f && s.mbps < 49.1f); }

  // An addon that IS the source keeps its name, less the words the chips say.
  { Stream s = one("Torrentio 4k", "Film.2024.2160p.WEB-DL.mkv \\n\xF0\x9F\x91\xA4 9",
                   "https://example.invalid/g.mkv");
    assert(!strcmp(s.service, "Torrentio")); }

  // OTHER PEOPLE'S TEMPLATES: bracketed cache tags, emoji, the chips' words and
  // the separators between them all go; what is left is the name.
  { Stream s = one("[RD\xE2\x9A\xA1] AIOStreams 4K", "Film.2024.2160p.mkv",
                   "https://example.invalid/h.mkv");
    assert(!strcmp(s.service, "AIOStreams")); }
  { Stream s = one("[RD+] Torrentio\\n4k DV | HDR", "Film.2024.2160p.mkv",
                   "https://example.invalid/i.mkv");
    assert(!strcmp(s.service, "Torrentio")); }
  { Stream s = one("\xF0\x9F\xA7\xB2 Comet \xC2\xB7 2160p", "Film.2024.2160p.mkv",
                   "https://example.invalid/j.mkv");
    assert(!strcmp(s.service, "Comet")); }
  // Nothing but a state and the chips' words: empty, so the row says the addon.
  { Stream s = one("\xE2\x9A\xA1 1080p | HDR", "Film.2024.1080p.mkv",
                   "https://example.invalid/k.mkv");
    assert(!s.service[0]); }

  // The two spellings of the same file, from the same list: both have to reach
  // the same codec and the same channel count.
  {
    { Stream s = one("Cached", "Last Seen S01E01 2160p ATVP WEB-DL DDP5 1 Atmos DV "
                     "HDR10Plus H 265-Kitsune.mkv", "https://example.invalid/i.mkv");
      assert(!strcmp(s.codec, "HEVC") && !strcmp(s.audio, "ATMOS 5.1")); }
    { Stream s = one("Cached", "Last.Seen.S01E01.2160p.Apple.TV+.WEB-DL.DDP.5.1.Atmos."
                     "HDR10+.H.265-BlackTV.mkv", "https://example.invalid/j.mkv");
      assert(!strcmp(s.codec, "HEVC") && !strcmp(s.audio, "ATMOS 5.1")); } }

  // --- TORRENTS AND bingeGroup -------------------------------------------------
  // A bare torrent survives the parser with no url (streams.c drops it when
  // there is no debrid key); externalUrl still never becomes a link.
  { Stream *v = NULL;
    int c = stream_parse("{\"streams\":[{\"infoHash\":\"abc\",\"fileIdx\":2},"
                         "{\"externalUrl\":\"https://example.invalid\"},"
                         "{\"name\":\"x\",\"clientResolve\":{\"infoHash\":\"def\"}},"
                         "{\"url\":\"https://example.invalid/a.mp4\"}]}", "Fixture", &v);
    assert(c == 3);
    assert(!v[0].url[0] && !strcmp(v[0].infoHash, "abc") && v[0].fileIdx == 2);
    assert(!v[1].url[0] && !strcmp(v[1].infoHash, "def") && v[1].fileIdx == -1);
    assert(v[2].url[0] && !v[2].infoHash[0]);
    free(v); }
  // bingeGroup comes from THIS stream's behaviorHints, never the next stream's,
  // and not from a nested object inside it.
  { Stream *v = NULL;
    int c = stream_parse("{\"streams\":[{\"url\":\"https://e.invalid/1\"},"
                         "{\"url\":\"https://e.invalid/2\",\"behaviorHints\":{\"proxyHeaders\":"
                         "{\"request\":{\"a\":\"b\"}},\"bingeGroup\":\"torrentio|1080p\"}}]}",
                         "Fixture", &v);
    assert(c == 2 && !v[0].bingeGroup[0] && !strcmp(v[1].bingeGroup, "torrentio|1080p"));
    assert(!v[0].headers[0] && !strcmp(v[1].headers, "a: b"));
    free(v); }
  // proxyHeaders.request becomes "Name: Value" lines for proxy.c. Connection
  // headers, empty values, non-strings and anything with a line break are
  // dropped — the web's normalizeHeaderEntries — and escapes are undone.
  { Stream *v = NULL;
    int c = stream_parse("{\"streams\":[{\"url\":\"https://e.invalid/1\",\"behaviorHints\":"
                         "{\"proxyHeaders\":{\"request\":{\"Authorization\":\"Bearer x\\\"y\","
                         "\"Range\":\"bytes=0-\",\"Host\":\"h\",\"X-Empty\":\"\",\"X-Num\":5,"
                         "\"X-Obj\":{\"a\":\"b\"},\"X-Bad\":\"a\\r\\nInjected: 1\","
                         "\"Referer\":\"https://site/\"},\"response\":{\"X-Resp\":\"no\"}}}},"
                         "{\"url\":\"https://e.invalid/2\"}]}",
                         "Fixture", &v);
    assert(c == 2);
    assert(!strcmp(v[0].headers, "Authorization: Bearer x\"y\nReferer: https://site/"));
    assert(!v[1].headers[0]);
    free(v); }

  // videoHash and videoSize, for the subtitle search (addons_subtitles_file).
  { Stream *v = NULL;
    int c = stream_parse("{\"streams\":[{\"url\":\"https://e.invalid/1\",\"behaviorHints\":"
                         "{\"videoHash\":\"8e245d9679d31e12\",\"videoSize\":57812312345,"
                         "\"filename\":\"The.Dark.Knight.2008.REMUX-FraMeSToR.mkv\"}},"
                         "{\"url\":\"https://e.invalid/2\"}]}", "Fixture", &v);
    assert(c == 2);
    assert(!strcmp(v[0].videoHash, "8e245d9679d31e12") && v[0].videoSize == 57812312345LL);
    assert(!strcmp(v[0].file, "The.Dark.Knight.2008.REMUX-FraMeSToR.mkv"));
    assert(!v[1].videoHash[0] && v[1].videoSize == 0);
    free(v); }

  puts("PASS ASan/UBSan: parser in isolation, 100 sources, tokens, service, cache state and the tier.");
}
