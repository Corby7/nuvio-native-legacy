#include "streams.h"
#include "js.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int contains(const char *s, const char *term) {
  for (; *s; s++) if (!strncasecmp(s, term, strlen(term))) return 1;
  return 0;
}

// An isolated token matches .DV., DV/HDR and [DV], but never DVD/DVDRip.
static int token(const char *s, const char *t) {
  size_t n = strlen(t);
  const char *p;
  for (p = s; *p; p++)
    if ((p == s || !isalnum((unsigned char)p[-1])) &&
        !strncasecmp(p, t, n) && !isalnum((unsigned char)p[n])) return 1;
  return 0;
}

// --- THE ROW'S TOKENS --------------------------------------------------------
//
// Everything the sources sheet draws is decided HERE, once, out of the one blob
// the addons send: name + description + title + filename. The sheet then draws
// strings and never looks at the blob again.
//
// The vocabulary is fixed and short on purpose (see "THE SOURCES SHEET" in
// layout.h): four resolutions, five dynamic ranges, six sources, one audio
// format with its channel count, one codec. An addon that sends something else
// gets an EMPTY field, and the sheet omits that token — which is the whole point
// of a closed vocabulary. Inventing a label for the unrecognised case is what
// filled the old row with "FILE" and "Unknown source".

// The number that follows a marker, past whatever punctuation separates them.
static int numAfter(const char *p) {
  while (*p == ' ' || *p == ':' || *p == '-' || *p == '\t') p++;
  return isdigit((unsigned char)*p) ? atoi(p) : 0;
}

static int startsWord(const char *text, const char *p) {
  return p == text || !isalnum((unsigned char)p[-1]);
}

// SEEDERS, in the four spellings the aggregators use. The fourth is an EMOJI —
// Torrentio writes "\xF0\x9F\x91\xA4 48" — and it is matched as the three bytes it
// is: the app's Inter has no glyph for it (nor for the bolt, nor for the floppy
// disc), so every one of these marks has to be turned into a token here or it
// reaches the screen as .notdef and draws a hollow box.
static int seedersOf(const char *text) {
  const char *p;
  if ((p = strstr(text, "\xF0\x9F\x91\xA4"))) { int v = numAfter(p + 4); if (v) return v; }
  for (p = text; *p; p++) {
    if (!startsWord(text, p)) continue;
    if (!strncasecmp(p, "seeders", 7)) { int v = numAfter(p + 7); if (v) return v; }
    else if (!strncasecmp(p, "seeds", 5)) { int v = numAfter(p + 5); if (v) return v; }
    else if (!strncasecmp(p, "seed", 4))  { int v = numAfter(p + 4); if (v) return v; }
    else if (!strncasecmp(p, "peers", 5)) { int v = numAfter(p + 5); if (v) return v; }
  }
  // "48 seeders", the other way round.
  for (p = text; *p; p++) {
    const char *q, *r;
    if (!isdigit((unsigned char)*p) || !startsWord(text, p)) continue;
    q = p; while (isdigit((unsigned char)*q)) q++;
    r = q; while (*r == ' ') r++;
    if (!strncasecmp(r, "seed", 4)) return atoi(p);
  }
  return 0;
}

// The bitrate the addon states, in the three spellings AIOStreams emits — the
// third being the superscript "\xE1\xB4\xB9\xE1\xB5\x87\xE1\xB5\x96\xCB\xA2", which
// Inter does have but which reads as noise at this size.
static float mbpsOf(const char *text) {
  const char *p;
  for (p = text; *p; p++) {
    const char *q, *r;
    char buf[24];
    size_t k = 0;
    if (!isdigit((unsigned char)*p)) continue;
    if (p != text && (isalnum((unsigned char)p[-1]) || p[-1] == '.')) continue;
    q = p; while (isdigit((unsigned char)*q) || *q == '.' || *q == ',') q++;
    r = q; while (*r == ' ') r++;
    if (strncasecmp(r, "mbps", 4) && strncasecmp(r, "mb/s", 4) &&
        strncmp(r, "\xE1\xB4\xB9\xE1\xB5\x87\xE1\xB5\x96\xCB\xA2", 11)) continue;
    for (r = p; r < q && k < sizeof buf - 1; r++) buf[k++] = *r == ',' ? '.' : *r;
    buf[k] = 0;
    return (float)atof(buf);
  }
  return 0;
}

// INSTANT OR P2P, and nothing in between.
//
// "Instant" is the flag worth reading on a row: it says the file will start
// playing when you press OK rather than after a download nobody can see the
// progress of. The counterpart is not "cached: no" — it is a TORRENT, and that
// is the only state where the seed count decides whether it plays at all.
//
// "torrent" and "magnet" are NOT tested for. The provider's own name reaches
// this text, and "Torrentio" contains "torrent" — testing for it marked every
// row from the most common addon in the world as P2P, cached or not.
static void availability(Stream *s, const char *text) {
  int no = contains(text, "not cached") || contains(text, "uncached") ||
           contains(text, "non-cached");
  s->cached = !no && (contains(text, "cached") || token(text, "instant") ||
                      strstr(text, "\xE2\x9A\xA1") != NULL ||
                      contains(text, "[rd+]") || contains(text, "[ad+]") ||
                      contains(text, "[pm+]") || contains(text, "[tb+]") ||
                      contains(text, "[dl+]") || contains(text, "[oc+]"));
  s->p2p = !s->cached && (no || s->seeders > 0 || token(text, "p2p") ||
                          strstr(text, "\xF0\x9F\xA7\xB2") != NULL ||
                          contains(text, "[rd download]"));
}

// THE UPSTREAM SERVICE, out of the aggregator's own stream name.
//
// MEASURED on the device, AIOStreams writes it exactly like this:
//
//   "Cached  Comet‍  ★★★★★"
//
// — the cache state, the service, and a run of stars, held apart by typographic
// spaces and a zero-width joiner. Three of those four parts already have a home
// on the row: the state is the Instant mark, the stars are the segmented bar
// (which at least says which end is good), and the invisible characters are
// nothing at all. Inter has no glyph for any of them, so left in the string they
// reach the screen as .notdef boxes.
//
// What is left is the service, and it is the one part the row could not say
// before: every row of a twelve-source list read "AIOStreams".
//
// OTHER PEOPLE'S TEMPLATES. That shape is the owner's AIOStreams formatter; the
// default formatters, Torrentio and the rest name a stream "[RD⚡] AIOStreams
// 4K" or "Torrentio\n4k DV" instead. Printed whole, that ran under the row's
// right column with its emoji as hollow boxes. So the name is cleaned the same
// way whatever its shape: bracketed tags ([RD+], (AD)) and anything Inter
// cannot draw go, and so do the words the chips already say (4K, 1080p, DV,
// HDR) and the separators left between them. The owner's names never contain
// any of those, so they come out exactly as before.
static int chipWord(const char *w, size_t n) {
  static const char *WORDS[] = { "4k", "uhd", "fhd", "hd", "sd", "2160p", "1440p", "1080p",
                                 "720p", "576p", "480p", "360p", "dv", "dovi", "hdr", "hdr10",
                                 "hdr10+", "hlg", "sdr", "remux" };
  for (size_t i = 0; i < sizeof WORDS / sizeof *WORDS; i++)
    if (strlen(WORDS[i]) == n && !strncasecmp(w, WORDS[i], n)) return 1;
  return 0;
}
static int separatorWord(const char *w, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (isalnum((unsigned char)w[i]) || (unsigned char)w[i] >= 0x80) {
      // The middle dot and the bullet are separators too, letters are not.
      if (n == 2 && !strncmp(w, "\xC2\xB7", 2)) return 1;
      if (n == 3 && !strncmp(w, "\xE2\x80\xA2", 3)) return 1;
      return 0;
    }
  return 1;
}
static void serviceOf(const char *name, char *out, size_t cap) {
  static const char *DROP[] = { "\xE2\x80\x8B", "\xE2\x80\x8C", "\xE2\x80\x8D",
                                "\xE2\x81\xA0", "\xE2\x81\xA1", "\xE2\x81\xA2",
                                "\xE2\x81\xA3", "\xE2\x81\xA4", "\xEF\xBB\xBF",
                                "\xEF\xB8\x8F", "\xEF\xB8\x8E" };
  static const char *STOP[] = { "\xE2\x98\x85", "\xE2\x98\x86", "\xE2\xAD\x90" };
  static const char *LEAD[] = { "not", "uncached", "cached", "instant",
                                "download", "p2p", "debrid", "torrent" };
  char buf[192];
  size_t k = 0, i, o = 0;
  const char *p = name, *q;
  int kept = 0;

  out[0] = 0;
  for (; *p && k < sizeof buf - 1; ) {
    int skipped = 0;
    unsigned char c = (unsigned char)*p;
    for (i = 0; i < sizeof STOP / sizeof *STOP; i++)
      if (!strncmp(p, STOP[i], 3)) { p = ""; skipped = 1; break; }
    if (!*p) break;
    if (skipped) continue;
    for (i = 0; i < sizeof DROP / sizeof *DROP; i++)
      if (!strncmp(p, DROP[i], 3)) { p += 3; skipped = 1; break; }
    if (skipped) continue;
    // A bracketed tag, "[RD+]" or "(AD)": the cache state in shorthand.
    if (c == '[' || c == '(') {
      const char *close = strchr(p, c == '[' ? ']' : ')');
      if (close) { p = close + 1; buf[k++] = ' '; continue; }
    }
    // Emoji (four-byte UTF-8) and the arrows, symbols and dingbats blocks
    // (U+2190-U+2BFF: the bolt, the magnet's neighbours, the floppy): no glyph.
    if (c >= 0xF0) { p += 4; buf[k++] = ' '; continue; }
    if (c == 0xE2 && (unsigned char)p[1] >= 0x86 && (unsigned char)p[1] <= 0xAF) {
      p += 3; buf[k++] = ' '; continue;
    }
    // Every space-like character becomes one plain space, so the words below
    // have a single thing to split on.
    if (c < 32 || c == ' ') { buf[k++] = ' '; p++; continue; }
    if (!strncmp(p, "\xE2\x80", 2) && (unsigned char)p[2] >= 0x80 &&
        (unsigned char)p[2] <= 0x8A) { buf[k++] = ' '; p += 3; continue; }
    buf[k++] = *p++;
  }
  buf[k] = 0;

  // Word by word: the state words only at the front, as before; the chips'
  // words and bare separators anywhere.
  for (q = buf; *q; ) {
    const char *w;
    size_t n;
    while (*q == ' ') q++;
    if (!*q) break;
    w = q;
    while (*q && *q != ' ') q++;
    n = (size_t)(q - w);
    if (!kept) {
      for (i = 0; i < sizeof LEAD / sizeof *LEAD; i++)
        if (strlen(LEAD[i]) == n && !strncasecmp(w, LEAD[i], n)) break;
      if (i < sizeof LEAD / sizeof *LEAD) continue;
    }
    if (chipWord(w, n) || separatorWord(w, n)) continue;
    if (o + (o ? 1 : 0) + n >= cap) break;
    if (o) out[o++] = ' ';
    memcpy(out + o, w, n);
    o += n;
    kept = 1;
  }
  // A name that was ONLY a state, tags and some stars leaves nothing; the row
  // then falls back to the addon, which is the honest answer.
  out[o] = 0;
}
static void tokens(Stream *s, const char *text) {
  const char *fmt = "", *ch = "";

  if (s->height >= 2160)      snprintf(s->res, sizeof s->res, "4K");
  else if (s->height >= 1440) snprintf(s->res, sizeof s->res, "1440p");
  else if (s->height >= 1080) snprintf(s->res, sizeof s->res, "1080p");
  else if (s->height >= 720)  snprintf(s->res, sizeof s->res, "720p");
  else if (s->height > 0)     snprintf(s->res, sizeof s->res, "SD");

  // SDR IS THE ABSENCE OF A CHIP. Every file is one thing or another, so a chip
  // that appears on every row carries no information and costs the row its
  // scannability — the badge sheet's "SDR: show nothing".
  if (s->dolbyVision)                      snprintf(s->range, sizeof s->range, "DV");
  else if (contains(text, "hdr10+") ||
           contains(text, "hdr10plus"))    snprintf(s->range, sizeof s->range, "HDR10+");
  else if (contains(text, "hdr10"))        snprintf(s->range, sizeof s->range, "HDR10");
  else if (token(text, "hlg"))             snprintf(s->range, sizeof s->range, "HLG");
  else if (token(text, "hdr"))             snprintf(s->range, sizeof s->range, "HDR");

  // "BDRemux" is one word in the Russian trackers' titles, so `token` never saw it.
  if (token(text, "remux") || token(text, "bdremux"))
                                           snprintf(s->source, sizeof s->source, "REMUX");
  else if (contains(text, "bluray") || contains(text, "blu-ray") ||
           token(text, "bdrip") || token(text, "brrip"))
                                           snprintf(s->source, sizeof s->source, "BLURAY");
  else if (contains(text, "web-dl") || contains(text, "webdl") ||
           contains(text, "web.dl"))       snprintf(s->source, sizeof s->source, "WEB-DL");
  else if (contains(text, "webrip") || contains(text, "web-rip"))
                                           snprintf(s->source, sizeof s->source, "WEBRIP");
  else if (token(text, "hdtv"))            snprintf(s->source, sizeof s->source, "HDTV");
  // A SCREENER is ranked with a cam: both are pre-release copies, and DVDSCR has
  // to be caught before DVDRip below or it would read as the finished disc.
  else if (token(text, "cam") || token(text, "camrip") || token(text, "hdcam") ||
           token(text, "telesync") || token(text, "dvdscr") || token(text, "scr") ||
           token(text, "screener"))        snprintf(s->source, sizeof s->source, "CAM");
  // DVD and HDRip were outside the vocabulary, and a release that says nothing
  // but "DVDRip" came out with no chip at all — a row with no video line.
  else if (token(text, "dvdrip") || token(text, "dvd") || token(text, "dvd5") ||
           token(text, "dvd9"))            snprintf(s->source, sizeof s->source, "DVD");
  else if (token(text, "hdrip"))           snprintf(s->source, sizeof s->source, "HDRIP");

  if (s->dolbyAtmos)                             fmt = "ATMOS";
  else if (contains(text, "dts:x") || contains(text, "dts-x"))   fmt = "DTS-X";
  else if (contains(text, "truehd") || contains(text, "true-hd")) fmt = "TRUEHD";
  else if (contains(text, "dts-hd") || contains(text, "dtshd"))  fmt = "DTS-HD";
  else if (token(text, "dts"))                   fmt = "DTS";
  else if (token(text, "flac"))                  fmt = "FLAC";
  else if (contains(text, "eac3") || contains(text, "e-ac-3") ||
           contains(text, "ddp") || contains(text, "dd+"))       fmt = "EAC3";
  else if (contains(text, "ac3") || contains(text, "dd5.1"))     fmt = "DD";
  else if (token(text, "aac"))                   fmt = "AAC";
  // "DDP5 1" is the same thing as "DDP5.1" — the same trap as the codec above,
  // and from the same files.
  ch = contains(text, "7.1") || contains(text, "7 1") ? "7.1" :
       contains(text, "5.1") || contains(text, "5 1") ? "5.1" :
       contains(text, "2.0") || contains(text, "2 0") ? "2.0" : "";
  if (fmt[0] && ch[0]) snprintf(s->audio, sizeof s->audio, "%s %s", fmt, ch);
  else                 snprintf(s->audio, sizeof s->audio, "%s", fmt);

  // "H 265" AND "H.265": release names punctuate the codec every way there is,
  // and MEASURED on the device both spellings arrive from the same addon in the
  // same list — "...Atmos DV HDR10Plus H 265-Kitsune.mkv" beside
  // "...Atmos.HDR10+.H.265-BlackTV.mkv". Matching one of the two is how a row
  // came to show a codec while the row under it, of the same file, showed none.
  if (contains(text, "hevc") || token(text, "x265") || token(text, "h265") ||
      contains(text, "h.265") || contains(text, "h 265"))
                                          snprintf(s->codec, sizeof s->codec, "HEVC");
  else if (token(text, "av1"))            snprintf(s->codec, sizeof s->codec, "AV1");
  else if (token(text, "avc") || token(text, "x264") || token(text, "h264") ||
           contains(text, "h.264") || contains(text, "h 264"))
                                          snprintf(s->codec, sizeof s->codec, "H.264");

  s->seeders = seedersOf(text);
  s->mbps = mbpsOf(text);
  availability(s, text);
}

// QUALITY AS A DIRECTION, NOT A LABEL.
//
// The bar replaces both the row of stars the aggregators send and the "Tier"
// word the old sheet printed: neither tells you which end is good. A segmented
// bar is ordinal by construction — more filled is better, the way signal
// strength reads — and the word after it only removes the last doubt.
//
// FAIR is the starting point, because "watchable, compromises somewhere" is what
// an unremarkable file is. Everything below moves off it by one step at a time,
// and the bitrate is what separates two files that carry the same three chips —
// two 54 GB remuxes differ by nothing else a chip can show.
void stream_rank(Stream *s, int runtimeSeconds) {
  int score = 2;
  if (!s->mbps && s->sizeMB > 0 && runtimeSeconds > 0)
    s->mbps = (float)(s->sizeMB * 8.0 / runtimeSeconds);

  // Remux is the top of the scale by definition — an untouched stream off the
  // disc — so it alone can carry a row to BEST with nothing else going for it.
  if (!strcmp(s->source, "REMUX"))                                 score += 3;
  else if (!strcmp(s->source, "BLURAY") || !strcmp(s->source, "WEB-DL")) score += 1;
  else if (!strcmp(s->source, "WEBRIP") || !strcmp(s->source, "HDTV") ||
           !strcmp(s->source, "HDRIP"))                            score -= 1;
  else if (!strcmp(s->source, "DVD"))                              score -= 2;
  else if (!strcmp(s->source, "CAM"))                              score -= 4;

  if (strstr(s->audio, "ATMOS") || strstr(s->audio, "TRUEHD") ||
      strstr(s->audio, "DTS-HD") || strstr(s->audio, "DTS-X") ||
      strstr(s->audio, "FLAC"))                                    score += 1;
  else if (strstr(s->audio, "AAC"))                                score -= 1;

  // Against the resolution, because 8 Mbps is a good 1080p and a poor 4K.
  if (s->mbps > 0) {
    float good = s->height >= 2160 ? 25.0f : s->height >= 1080 ? 8.0f : 3.0f;
    if (s->mbps >= good * 2.0f)      score += 2;
    else if (s->mbps >= good)        score += 1;
    else if (s->mbps < good * 0.45f) score -= 2;
  }

  // The swarm, and ONLY on a torrent: on a cached file the number is whatever
  // the aggregator last scraped and has no bearing on whether it plays.
  if (s->p2p) {
    if (s->seeders < 5)        score -= 3;
    else if (s->seeders < 20)  score -= 1;
    else if (s->seeders >= 50) score += 1;
  }
  if (s->height && s->height < 720) score -= 1;

  s->tier = score >= 6 ? 3 : score >= 4 ? 2 : score >= 2 ? 1 : 0;
}

// The object named `key` inside [start,end): its '{' in *obj and its end in
// *objEnd. 1 if found. Bounded, because strstr runs to the end of the DOCUMENT
// and would otherwise find the next stream's object.
static int objectIn(const char *start, const char *end, const char *key,
                    const char **obj, const char **objEnd) {
  char quoted[40];
  const char *k, *o, *e;
  snprintf(quoted, sizeof quoted, "\"%s\"", key);
  k = strstr(start, quoted);
  if (!k || k >= end) return 0;
  o = k + strlen(quoted);
  while (o < end && (*o == ' ' || *o == ':' || *o == '\t' || *o == '\n' || *o == '\r')) o++;
  if (o >= end || *o != '{') return 0;
  e = js_end(o);
  if (!e || e > end) return 0;
  *obj = o; *objEnd = e;
  return 1;
}

// A JSON string starting at its opening quote. Copies it into dst (an escape
// keeps only the character after the backslash, and a control character makes
// the whole string unusable: a header value must never carry a line break) and
// returns what follows the closing quote, or NULL.
static const char *stringAt(const char *p, const char *end, char *dst, size_t size,
                            int *clean) {
  size_t n = 0;
  *clean = 1;
  if (p >= end || *p != '"') return NULL;
  for (p++; p < end && *p != '"'; p++) {
    char c = *p;
    if (c == '\\' && p + 1 < end) {
      c = *++p;
      if (c == 'n' || c == 'r' || c == 't' || c == 'u' || c == 'b' || c == 'f') *clean = 0;
    }
    if ((unsigned char)c < ' ') *clean = 0;
    if (n + 1 < size) dst[n++] = c; else *clean = 0;
  }
  dst[n] = 0;
  return p < end ? p + 1 : NULL;
}

// behaviorHints.proxyHeaders.request, as "Name: Value" lines joined by '\n'.
// The same filter as the web's normalizeHeaderEntries: no empty names or values,
// nothing that breaks a line, and none of the headers that belong to the
// connection itself — the relay sets those, and a second Range would break seeking.
static void requestHeaders(const char *bh, const char *bhEnd, char *dst, size_t size) {
  static const char *const HOP[] = { "connection", "content-length", "host",
                                     "range", "transfer-encoding", NULL };
  const char *ph, *phEnd, *rq, *rqEnd, *p;
  size_t used = 0;
  dst[0] = 0;
  if (!objectIn(bh, bhEnd, "proxyHeaders", &ph, &phEnd)) return;
  if (!objectIn(ph, phEnd, "request", &rq, &rqEnd)) return;
  p = rq + 1;
  for (;;) {
    char name[128], value[900];
    int cleanN, cleanV, k, hop = 0;
    while (p < rqEnd && *p != '"' && *p != '}') p++;
    if (p >= rqEnd || *p == '}') break;
    p = stringAt(p, rqEnd, name, sizeof name, &cleanN);
    if (!p) break;
    while (p < rqEnd && (*p == ' ' || *p == ':' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    if (p < rqEnd && *p == '"') {
      p = stringAt(p, rqEnd, value, sizeof value, &cleanV);
      if (!p) break;
    } else {
      // Not a string (a number, an object): skipped, as the web's String() of
      // it would not be a header anyone meant to send.
      while (p < rqEnd && *p != ',' && *p != '}') {
        if (*p == '{' || *p == '[') p = js_end(p); else p++;
      }
      continue;
    }
    for (k = 0; HOP[k]; k++) if (!strcasecmp(name, HOP[k])) hop = 1;
    if (!cleanN || !cleanV || !name[0] || !value[0] || hop || strchr(name, ':')) continue;
    if (used + strlen(name) + strlen(value) + 4 >= size) break;
    used += (size_t)snprintf(dst + used, size - used, "%s%s: %s",
                             used ? "\n" : "", name, value);
  }
}

int stream_parse(const char *json, const char *provider, Stream **output) {
  const char *p, *end;
  int n = 0, cap = 0;
  Stream *v = NULL;
  *output = NULL;
  if (!json) return 0;
  p = js_array(json, json + strlen(json), "streams");
  while (p && *p == '{') {
    Stream s = {0};
    char title[2048] = "", text[5000];
    end = js_end(p);
    if (!end || end <= p) break;
    js_text(p, end, "url", s.url, sizeof s.url);
    s.fileIdx = -1;
    // A bare torrent: no http url, an infoHash (at the top, or inside
    // clientResolve as AIOStreams sends it). Its url is made later, by the
    // debrid, and streams.c drops the row when there is no debrid to make it.
    // The bounds matter: strstr runs to the end of the DOCUMENT, and without
    // `< end` a stream would take the next one's hash.
    if (strncmp(s.url, "http://", 7) && strncmp(s.url, "https://", 8)) {
      const char *cr = strstr(p, "\"clientResolve\"");
      s.url[0] = 0;
      if (!js_text(p, end, "infoHash", s.infoHash, sizeof s.infoHash) && cr && cr < end)
        js_text(cr, end, "infoHash", s.infoHash, sizeof s.infoHash);
      s.fileIdx = (int)js_num(p, end, "fileIdx", -1);
    }
    // Do not expose externalUrl as a direct link, and never play a truncated URL.
    if ((s.infoHash[0] || !strncmp(s.url, "http://", 7) || !strncmp(s.url, "https://", 8)) &&
        strlen(s.url) < sizeof s.url - 1) {
      js_text(p, end, "name", s.label, sizeof s.label);
      js_text(p, end, "description", s.description, sizeof s.description);
      js_text(p, end, "title", title, sizeof title);
      js_text(p, end, "filename", s.file, sizeof s.file);
      // behaviorHints.bingeGroup, looked for INSIDE this stream's behaviorHints
      // and not loose: a stream without behaviorHints would otherwise inherit
      // the next stream's group, and the symptom would be the app remembering
      // the wrong source on the next episode.
      { const char *bh = strstr(p, "\"behaviorHints\"");
        if (bh && bh < end) {
          const char *obj = strchr(bh + 15, '{');
          const char *objEnd = obj && obj < end ? js_end(obj) : NULL;
          if (objEnd && objEnd <= end) {
            js_text(obj, objEnd, "bingeGroup", s.bingeGroup, sizeof s.bingeGroup);
            requestHeaders(obj, objEnd, s.headers, sizeof s.headers);
            js_text(obj, objEnd, "videoHash", s.videoHash, sizeof s.videoHash);
          }
        } }
      if (!s.description[0]) snprintf(s.description, sizeof s.description, "%s", title);
      if (!s.label[0]) snprintf(s.label, sizeof s.label, "%s", provider);
      snprintf(s.provider, sizeof s.provider, "%s", provider);
      snprintf(text, sizeof text, "%s %s %s %s", s.label, s.description, title, s.file);
      s.height = contains(text, "2160") || token(text, "4k") || token(text, "uhd") ? 2160 :
                 contains(text, "1440") ? 1440 : contains(text, "1080") ? 1080 :
                 contains(text, "720") ? 720 : contains(text, "480") ? 480 :
                 // Below 480 only as a whole "576p"/"360p" token: the bare
                 // numbers turn up in file sizes and episode titles.
                 token(text, "576p") || token(text, "576i") || token(text, "360p") ||
                 token(text, "240p") ? 480 :
                 // A DVD is standard definition whether or not it says so.
                 token(text, "dvdrip") || token(text, "dvd") ? 480 : 0;
      s.dolbyVision = token(text, "dv") || token(text, "dovi") ||
                      contains(text, "dolby vision") || contains(text, "dolbyvision");
      s.dolbyAtmos = token(text, "atmos");
      s.mp4 = token(text, "mp4") || contains(s.url, ".mp4");
      double bytes = js_num(p, end, "videoSize", 0);
      if (bytes > 0) s.videoSize = (long long)bytes;
      if (bytes > 0) s.sizeMB = (long)(bytes / (1024.0 * 1024.0));
      else {
        const char *u = strstr(text, " GB");
        double scale = 1024;
        if (!u) { u = strstr(text, " MB"); scale = 1; }
        if (u) {
          const char *start = u;
          while (start > text && (isdigit((unsigned char)start[-1]) || start[-1] == '.')) start--;
          if (start < u) s.sizeMB = (long)(atof(start) * scale);
        }
      }
      tokens(&s, text);
      serviceOf(s.label, s.service, sizeof s.service);
      // With no runtime yet: the parser has the size but not what it is a size
      // OF. The sheet calls this again as soon as it knows (stream_sheet_runtime).
      stream_rank(&s, 0);
      if (n == cap) {
        int new = cap ? cap * 2 : 32;
        Stream *tmp = realloc(v, (size_t)new * sizeof *tmp);
        if (!tmp) { free(v); return -1; }
        v = tmp; cap = new;
      }
      v[n++] = s;
    }
    p = js_next(end);
  }
  *output = v;
  return n;
}
