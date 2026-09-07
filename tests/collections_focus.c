// The collection tile's focus animation, from the account's JSON to the sprite
// sheet URL the tile draws.
//
// The derivation is a CONVENTION over the asset repo's layout (heroes/<x>.mp4
// next to focus/<x>.jpg), so what has to be pinned down is not that it rewrites
// a URL — it is everything it must REFUSE to rewrite. A wrong guess here does
// not fail loudly: it puts a 404 where the animation was meant to be.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/collections.c"

// collections.c resolves a source's address through the installed addons. No
// addon is installed in this test and none is needed: every folder below carries
// a complete source only so that col_load_account does not skip it before the
// art is ever read.
const char *addons_base_for_id(const char *id) { (void)id; return ""; }

#define CDN "https://cdn.jsdelivr.net/gh/Corby7/nuvio-assets@main"

static const ColFolder *folderTitled(const char *title) {
  int i;
  for (i = 0; i < col_n(); i++)
    if (!strcmp(col_folder(i)->title, title)) return col_folder(i);
  return NULL;
}

// One collection, one folder per case. The source is the same everywhere and is
// only there to keep the folder.
static char *body(const char *folders) {
  static char buf[8192];
  snprintf(buf, sizeof buf,
    "[{\"collections_json\":[{\"id\":\"c1\",\"title\":\"Streaming\",\"folders\":[%s]}]}]",
    folders);
  return buf;
}
#define SRC "\"catalogSources\":[{\"addonId\":\"a\",\"type\":\"movie\",\"catalogId\":\"top\"}]"

int main(void) {
  const ColFolder *f;
  char longUrl[900];

  assert(col_load_account(body(
    "{\"id\":\"1\",\"title\":\"HBO\","
      "\"focusGifUrl\":\"" CDN "/heroes/hbo-ident.mp4\"," SRC "},"
    "{\"id\":\"2\",\"title\":\"Off\",\"focusGifEnabled\":false,"
      "\"focusGifUrl\":\"" CDN "/heroes/hbo-ident.mp4\"," SRC "},"
    "{\"id\":\"3\",\"title\":\"Gif\","
      "\"focusGifUrl\":\"" CDN "/heroes/hbo-ident.gif\"," SRC "},"
    "{\"id\":\"4\",\"title\":\"Elsewhere\","
      "\"focusGifUrl\":\"" CDN "/clips/hbo-ident.mp4\"," SRC "},"
    "{\"id\":\"5\",\"title\":\"None\"," SRC "},"
    "{\"id\":\"6\",\"title\":\"Webm\","
      "\"focusGifUrl\":\"" CDN "/heroes/prime-ident.webm\"," SRC "},"
    "{\"id\":\"7\",\"title\":\"Query\","
      "\"focusGifUrl\":\"" CDN "/heroes/hbo-ident.mp4?v=abc\"," SRC "}"
  )) == 7);

  // The case that exists: an MP4 in the repo's heroes/ folder becomes the sheet
  // baked beside it by make-focus-sheet.sh.
  f = folderTitled("HBO");
  assert(f && !strcmp(f->focusSheet, CDN "/focus/hbo-ident.jpg"));
  // The packaged flipbook's fields stay empty. An account collection has no
  // folder of numbered frames inside the .ipk, and home.c picks its branch on
  // exactly these.
  assert(f->frames == 0 && f->frameDir[0] == 0);

  // focusGifEnabled is the owner's switch and the web app honours it
  // (`focusGifEnabled !== false`). Ignoring it here would animate a tile the
  // owner turned off in the app.
  assert(folderTitled("Off")->focusSheet[0] == 0);

  // A .gif is NOT rewritten. It is a real animation the web can play, so the
  // temptation is to treat it like the MP4 — but make-focus-sheet.sh only runs
  // over heroes/*.mp4, so the .jpg this would name does not exist.
  assert(folderTitled("Gif")->focusSheet[0] == 0);

  // Outside heroes/ the convention says nothing, so neither does this.
  assert(folderTitled("Elsewhere")->focusSheet[0] == 0);

  // No focusGifUrl at all: the ordinary case for every collection that is not
  // the owner's streaming one.
  assert(folderTitled("None")->focusSheet[0] == 0);

  assert(!strcmp(folderTitled("Webm")->focusSheet, CDN "/focus/prime-ident.jpg"));

  // A cache-busting query is left alone rather than half-parsed. Rewriting it
  // would have to decide where the name ends, and guessing wrong builds a URL
  // that 404s; refusing keeps the tile static, which is the safe answer.
  assert(folderTitled("Query")->focusSheet[0] == 0);

  // LONGER THAN THE FIELD. focusSheet is 512 bytes and the account's URLs are not
  // bounded by anything (collections.h records addon URLs past 4 KB), so the
  // derivation has to refuse rather than emit a TRUNCATED URL — which would be a
  // valid-looking string pointing at nothing.
  { char pad[600]; memset(pad, 'x', sizeof pad - 1); pad[sizeof pad - 1] = 0;
    snprintf(longUrl, sizeof longUrl, CDN "/heroes/%s-ident.mp4", pad); }
  { char one[4096];
    snprintf(one, sizeof one,
      "{\"id\":\"9\",\"title\":\"Long\",\"focusGifUrl\":\"%s\"," SRC "}", longUrl);
    assert(col_load_account(body(one)) == 1); }
  assert(folderTitled("Long")->focusSheet[0] == 0);

  puts("collections focus: PASS (mp4 sheet, webm, disabled, gif, off-convention, "
       "absent, query string, oversized URL)");
  return 0;
}
