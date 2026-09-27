// subtitle.c and subcharset.c in isolation: ASS/SSA parsing, overlapping cues,
// and the decoding of subtitles that are not UTF-8.
#include "subtitle.h"
#include "subcharset.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// The network: subtitle_load downloads whatever `served` holds.
static const char *served;
static long servedSize;
char *net_download_bin(const char *url, int seconds, long *n) {
  char *b = malloc((size_t)servedSize + 1);
  (void)url; (void)seconds;
  memcpy(b, served, (size_t)servedSize); b[servedSize] = 0;
  *n = servedSize;
  return b;
}

static void load(const char *body, long n, const char *language) {
  served = body; servedSize = n;
  subtitle_load("https://subs.example/x", language);
  for (int i = 0; i < 200 && !subtitle_ready(); i++) usleep(5000);
  assert(subtitle_ready());
}

static char *decode(const char *bytes, const char *language, const char **charset) {
  return subcharset_utf8(bytes, (long)strlen(bytes), language, charset);
}

static void testAss(void) {
  SubtitleCue *v = NULL;
  const char *ass =
    "\xef\xbb\xbf[Script Info]\r\nScriptType: v4.00+\r\n\r\n"
    "[V4+ Styles]\r\nFormat: Name, Fontname, Fontsize, PrimaryColour\r\n"
    "Style: Default,Arial,20,&H00FFFFFF\r\n\r\n"
    "[Events]\r\n"
    "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\r\n"
    "Dialogue: 0,0:00:05.00,0:00:07.50,Default,,0,0,0,,Later, with a comma\r\n"
    "Comment: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,not shown\r\n"
    "Dialogue: 0,0:00:01.00,0:00:03.25,Default,,0,0,0,,{\\an8\\i1}Hello{\\i0}\\Nworld\\hagain\r\n"
    "Dialogue: 0,0:00:02.00,0:00:04.00,Sign,,0,0,0,,{\\p1}m 0 0 l 100 0 100 100{\\p0}\r\n"
    "Dialogue: 0,1:02:03.4,1:02:04,Default,,0,0,0,,Short fraction\r\n";
  int n = subtitle_parse(ass, &v);
  assert(n == 3);
  // Sorted by start, the drawing and the Comment gone.
  assert(v[0].start == 1.0 && v[0].end == 3.25 && !strcmp(v[0].text, "Hello\nworld again"));
  assert(v[1].start == 5.0 && !strcmp(v[1].text, "Later, with a comma"));
  assert(v[2].start == 3723.4 && !strcmp(v[2].text, "Short fraction"));
  free(v);

  // Headerless, both shapes.
  n = subtitle_parse("Dialogue: 0:00:01.00,0:00:02.00,Bare\n"
                     "Dialogue: Marked=0,0:00:03.00,0:00:04.00,Default,,0,0,0,,Marked\n", &v);
  assert(n == 2 && !strcmp(v[0].text, "Bare") && !strcmp(v[1].text, "Marked"));
  free(v);

  // An SRT that talks about dialogue is still an SRT.
  n = subtitle_parse("1\n00:00:01,000 --> 00:00:02,000\nDialogue: is what this is\n", &v);
  assert(n == 1 && !strcmp(v[0].text, "Dialogue: is what this is"));
  free(v);

  // The SRT path, unchanged, and now sorted.
  n = subtitle_parse("2\n00:00:04,000 --> 00:00:06,000\nSecond\n\n"
                     "1\n00:00:01,000 --> 00:00:03,000\n<i>First &amp; one</i>\n", &v);
  assert(n == 2 && !strcmp(v[0].text, "First & one") && !strcmp(v[1].text, "Second"));
  free(v);
  puts("ASS/SSA parsing: ok");
}

static void testLayout(void) {
  SubtitleCue *v = NULL;
  const char *ass =
    "[Script Info]\nPlayResX: 1280\nPlayResY: 720\n\n"
    "[V4+ Styles]\n"
    "Format: Name, Fontname, Fontsize, PrimaryColour, Alignment, MarginV\n"
    "Style: Default,Arial,48,&H00FFFFFF,2,20\n"
    "Style: Top Sign ,Arial,40,&H00FFFFFF,8,20\n\n"
    "[Events]\n"
    "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
    "Dialogue: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,Plain\n"
    "Dialogue: 0,0:00:02.00,0:00:03.00,Top Sign,,0,0,0,,From the style\n"
    "Dialogue: 0,0:00:03.00,0:00:04.00,Top Sign,,0,0,0,,{\\an1}Inline wins\n"
    "Dialogue: 0,0:00:04.00,0:00:05.00,Default,,0,0,0,,{\\an7\\pos(640,360)}Placed\n"
    "Dialogue: 0,0:00:05.00,0:00:06.00,Default,,0,0,0,,{\\move(0,0,1280,720,0,500)}Moved\n"
    "Dialogue: 0,0:00:06.00,0:00:07.00,*Default,,0,0,0,,{\\a6}Old SSA top\n"
    "Dialogue: 0,0:00:07.00,0:00:08.00,Default,,0,0,0,,{\\pos(-50,2000)}Off screen\n";
  int n = subtitle_parse(ass, &v);
  assert(n == 7);
  assert(v[0].align == 2 && !v[0].positioned);
  assert(v[1].align == 8 && !v[1].positioned);             // style name trimmed
  assert(v[2].align == 1);
  assert(v[3].align == 7 && v[3].positioned && v[3].x == 0.5f && v[3].y == 0.5f);
  assert(v[4].positioned && v[4].x == 1.0f && v[4].y == 1.0f);   // \move's end point
  assert(v[5].align == 8);                                  // SSA \a6 = top centre
  assert(v[6].positioned && v[6].x == 0.0f && v[6].y == 1.0f);   // clamped to the frame
  free(v);

  // No PlayRes: libass's 384x288.
  n = subtitle_parse("Dialogue: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,{\\pos(192,72)}Old script\n", &v);
  assert(n == 1 && v[0].positioned && v[0].x == 0.5f && v[0].y == 0.25f);
  free(v);

  // SRT carries no layout.
  n = subtitle_parse("1\n00:00:01,000 --> 00:00:02,000\nx\n", &v);
  assert(n == 1 && !v[0].align && !v[0].positioned);
  free(v);
  puts("ASS layout: ok");
}

static void testOverlap(void) {
  char text[768];
  const char *ass =
    "[Events]\n"
    "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
    "Dialogue: 0,0:00:00.00,0:01:40.00,Sign,,0,0,0,,{\\pos(100,100)}THE SIGN\n"
    "Dialogue: 1,0:00:10.00,0:00:12.00,Default,,0,0,0,,Line A\n"
    "Dialogue: 0,0:00:10.00,0:00:12.00,Default,,0,0,0,,Line A\n"
    "Dialogue: 0,0:00:20.00,0:00:22.00,Default,,0,0,0,,Line B\n";
  load(ass, (long)strlen(ass), "eng");
  assert(subtitle_text(11, 0, text, sizeof text) && !strcmp(text, "THE SIGN\nLine A"));
  // The sign reaches past many later cues: reach[] finds it.
  assert(subtitle_text(21, 0, text, sizeof text) && !strcmp(text, "THE SIGN\nLine B"));
  assert(subtitle_text(50, 0, text, sizeof text) && !strcmp(text, "THE SIGN"));
  assert(!subtitle_text(101, 0, text, sizeof text) && !text[0]);
  // Retiming moves the reach with the cues.
  assert(subtitle_retime(subtitle_ready(), 1.0, 100.0));
  assert(!subtitle_text(50, 0, text, sizeof text));
  assert(subtitle_text(150, 0, text, sizeof text) && !strcmp(text, "THE SIGN"));
  subtitle_off();
  puts("Overlapping cues: ok");
}

static void testCharset(void) {
  const char *cs;
  char *s;

  // Windows-1252 Portuguese, by its language.
  s = decode("Ol\xe1, voc\xea est\xe1 a\xed?", "por", &cs);
  assert(!strcmp(cs, "windows-1252") && !strcmp(s, "Olá, você está aí?")); free(s);

  // Windows-1250 Czech, with no language: the frequency guess.
  s = decode("P\xf8\xedli\x9a \x9elu\x9dou\xe8k\xfd k\xf9\xf2 \xfap\xecl \xef\xe1" "belsk\xe9 \xf3" "dy", "", &cs);
  assert(!strcmp(cs, "windows-1250") && !strcmp(s, "Příliš žluťoučký kůň úpěl ďábelské ódy")); free(s);

  // Windows-1251 Russian, by its language.
  s = decode("\xcf\xf0\xe8\xe2\xe5\xf2, \xec\xe8\xf0", "rus", &cs);
  assert(!strcmp(cs, "windows-1251") && !strcmp(s, "Привет, мир")); free(s);

  // Windows-1256 Arabic, guessed by its article.
  s = decode("\xc7\xe1\xd3\xe1\xc7\xe3 \xda\xe1\xed\xdf\xe3 \xc7\xe1\xd3\xe1\xc7\xe3", "", &cs);
  assert(!strcmp(cs, "windows-1256") && !strcmp(s, "السلام عليكم السلام")); free(s);

  // UTF-8 stays as it is, BOM dropped.
  s = decode("\xef\xbb\xbf" "Café ☕", "fre", &cs);
  assert(!strcmp(cs, "utf-8") && !strcmp(s, "Café ☕")); free(s);

  // UTF-16LE with its BOM, a surrogate pair included; NULs inside, so the
  // length is passed by hand.
  { static const char u16[] = "\xff\xfe" "O\0l\0\xe1\0\x3d\xd8\x00\xde";
    s = subcharset_utf8(u16, sizeof u16 - 1, "", &cs);
    assert(!strcmp(cs, "utf-16le") && !strcmp(s, "Olá😀")); free(s); }

  // Double-encoded Hebrew: Windows-1255 bytes read as Latin-1, saved as UTF-8.
  s = decode("\xc3\xa9\xc3\xa5\xc3\xad \xc3\xa8\xc3\xa5\xc3\xa1", "heb", &cs);
  assert(!strcmp(s, "יום טוב")); free(s);

  // A Japanese file that is not UTF-8 is refused, not garbled.
  s = decode("\x82\xb1\x82\xf1\x82\xc9\x82\xbf\x82\xcd", "jpn", &cs);
  assert(!s && !strcmp(cs, "unsupported"));

  // And through the loader, end to end.
  { char text[768];
    const char *srt = "1\n00:00:01,000 --> 00:00:02,000\nJ\xe1 est\xe1\n";
    load(srt, (long)strlen(srt), "pob");
    assert(subtitle_text(1.5, 0, text, sizeof text) && !strcmp(text, "Já está"));
    subtitle_off(); }
  puts("Charset decoding: ok");
}

int main(void) {
  testAss();
  testLayout();
  testOverlap();
  testCharset();
  return 0;
}
