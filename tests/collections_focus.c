// The collection tile's focus animation, from the account's JSON to the video
// URL the tile plays. What has to be pinned down is mostly what it must REFUSE:
// a URL that is not a video would be loaded into the pipeline, fail, and leave
// the tile with nothing.
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

  // The case that exists: an MP4 in the repo's heroes/ folder is kept as it is,
  // and home.c plays it on the video plane.
  f = folderTitled("HBO");
  assert(f && !strcmp(f->focusVideo, CDN "/heroes/hbo-ident.mp4"));
  // The packaged flipbook's fields stay empty. An account collection has no
  // folder of numbered frames inside the .ipk, and home.c picks its branch on
  // exactly these.
  assert(f->frames == 0 && f->frameDir[0] == 0);

  // focusGifEnabled is the owner's switch and the web app honours it
  // (`focusGifEnabled !== false`). Ignoring it here would animate a tile the
  // owner turned off in the app.
  assert(folderTitled("Off")->focusVideo[0] == 0);

  // A .gif is NOT a video: the web shows it in an <img>, and this app has no GIF
  // player. The tile keeps its cover.
  assert(folderTitled("Gif")->focusVideo[0] == 0);

  // Any folder is fine for a video — the web's test is the extension alone.
  assert(!strcmp(folderTitled("Elsewhere")->focusVideo, CDN "/clips/hbo-ident.mp4"));

  // No focusGifUrl at all: the ordinary case for every collection that is not
  // the owner's streaming one.
  assert(folderTitled("None")->focusVideo[0] == 0);

  assert(!strcmp(folderTitled("Webm")->focusVideo, CDN "/heroes/prime-ident.webm"));

  // A query after the extension is still a video, as isVideoCollectionAssetUrl
  // reads it, and the URL goes to the pipeline whole.
  assert(!strcmp(folderTitled("Query")->focusVideo, CDN "/heroes/hbo-ident.mp4?v=abc"));

  // LONGER THAN THE FIELD. focusVideo is 512 bytes and the account's URLs are not
  // bounded by anything (collections.h records addon URLs past 4 KB), so it has
  // to refuse rather than keep a TRUNCATED URL — which would be a valid-looking
  // string pointing at nothing.
  { char pad[600]; memset(pad, 'x', sizeof pad - 1); pad[sizeof pad - 1] = 0;
    snprintf(longUrl, sizeof longUrl, CDN "/heroes/%s-ident.mp4", pad); }
  { char one[4096];
    snprintf(one, sizeof one,
      "{\"id\":\"9\",\"title\":\"Long\",\"focusGifUrl\":\"%s\"," SRC "}", longUrl);
    assert(col_load_account(body(one)) == 1); }
  assert(folderTitled("Long")->focusVideo[0] == 0);

  puts("collections focus: PASS (mp4, webm, disabled, gif, any folder, "
       "absent, query string, oversized URL)");
  return 0;
}
