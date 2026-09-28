// The loader against a fake Xtream panel (tests/iptv_xtream.sh serves it): a
// panel that answers 884 to get.php and serves player_api.php, the case that
// had Live TV fail on a real provider.
#include "iptv.h"
#include "net.h"
#include "data.h"
#include <SDL2/SDL.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// iptv_step until the load settles, ~15 s at most.
static void settle(void) {
  for (int i = 0; i < 1500; i++) {
    iptv_step();
    if (iptv_state() != IPTV_LOADING && iptv_guide_state() != IPTV_LOADING) { iptv_step(); return; }
    SDL_Delay(10);
  }
  assert(!"the load never settled");
}

static char epg[1024];   // the viewer's extra guides for the next load

static void load(int kind, const char *url, const char *server, const char *user, const char *pass) {
  IptvSource s;
  memset(&s, 0, sizeof s);
  s.kind = kind;
  snprintf(s.epg, sizeof s.epg, "%s", epg);
  snprintf(s.url, sizeof s.url, "%s", url);
  snprintf(s.server, sizeof s.server, "%s", server);
  snprintf(s.user, sizeof s.user, "%s", user);
  snprintf(s.pass, sizeof s.pass, "%s", pass);
  iptv_set_source(&s);
  settle();
}

int main(int argc, char **argv) {
  char base[128], url[256];
  assert(argc > 1);
  SDL_Init(SDL_INIT_TIMER);
  data_start(NULL);   // NUVIO_DATA, the script's own folder: where the country guide is kept
  snprintf(base, sizeof base, "http://127.0.0.1:%s", argv[1]);
  iptv_start();

  // The link a provider hands out as "your M3U", pasted as a playlist.
  snprintf(url, sizeof url, "%s/get.php?username=d5fe&password=fa10&type=m3u_plus&output=ts", base);
  load(IPTV_SRC_M3U, url, "", "", "");
  printf("status: '%s'\n", iptv_status());
  assert(iptv_state() == IPTV_READY && iptv_list());
  assert(iptv_list()->nCh == 5);
  assert(!strcmp(iptv_list()->ch[0].name, "\xC3\x87ocuk TV"));
  { char relay[4200];
    const char *u = iptv_play_url(0, relay, sizeof relay);
    printf("stream: %s\n", u);
    // TS: the link asked for output=ts, and the account allows it.
    assert(strstr(u, "/live/d5fe/fa10/101.ts")); }
  // The guide came from xmltv.php, the Xtream one, not from the playlist.
  assert(iptv_guide_state() == IPTV_READY && iptv_list()->ch[0].nPg == 1);
  // The three NL: channels it leaves out came from the Dutch public guide,
  // found by their tags; "News" has no country and stays without. They follow
  // the provider's guide, which is shown first: wait for them.
  for (int i = 0; i < 1000 && iptv_list()->ch[2].nPg == 0; i++) { iptv_step(); SDL_Delay(10); }
  { const IptvList *l = iptv_list();
    assert(l->ch[1].nPg == 0);
    for (int i = 2; i < 5; i++) assert(l->ch[i].nPg == 1);
    assert(!strcmp(l->pg[l->ch[2].firstPg].title, "RTL4 now")); }

  // The same as an Xtream login: the same channels.
  load(IPTV_SRC_XTREAM, "", base, "d5fe", "fa10");
  assert(iptv_state() == IPTV_READY && iptv_list()->nCh == 5);
  // No link to ask for a container: HLS, which the account allows; the other
  // one is there for a stream that will not play.
  { char relay[4200];
    assert(strstr(iptv_play_url(0, relay, sizeof relay), "/live/d5fe/fa10/101.m3u8"));
    assert(strstr(iptv_play_url_alt(0, relay, sizeof relay), "/live/d5fe/fa10/101.ts")); }

  // Extra guides: a dead address is passed over, the viewer's guide fills the
  // channel the provider's leaves out, and the provider's still covers its own.
  assert(iptv_list()->ch[1].nPg == 0);
  snprintf(epg, sizeof epg, "%s/nothing.xml %s/extra.xml.gz", base, base);
  load(IPTV_SRC_XTREAM, "", base, "d5fe", "fa10");
  assert(iptv_guide_state() == IPTV_READY);
  { const IptvList *l = iptv_list();
    assert(l->ch[0].nPg == 1 && !strcmp(l->pg[l->ch[0].firstPg].title, "Cartoons"));
    assert(l->ch[1].nPg == 1 && !strcmp(l->pg[l->ch[1].firstPg].title, "Headlines")); }
  epg[0] = 0;
  // The country guide was kept on disk: the reloads did not fetch it again.
  { char hits[64];
    char *h;
    snprintf(hits, sizeof hits, "%s/auto-hits", base);
    h = net_download(hits, 5);
    assert(h && atoi(h) == 1);
    free(h); }

  // TRYING a source (the Source screen's Save and load): a wrong password
  // fails as a sign-in, and the working source and its channels stay; the
  // right one takes over.
  { IptvSource t, before = *iptv_source();
    int st, n0 = iptv_list()->nCh;
    char why[160];
    memset(&t, 0, sizeof t);
    t.kind = IPTV_SRC_XTREAM;
    snprintf(t.server, sizeof t.server, "%s", base);
    snprintf(t.user, sizeof t.user, "d5fe");
    snprintf(t.pass, sizeof t.pass, "wrong");
    iptv_try_source(&t, 15);
    for (int i = 0; i < 1500 && iptv_try_state() == IPTV_LOADING; i++) { iptv_step(); SDL_Delay(10); }
    assert(iptv_try_state() == IPTV_FAILED);
    assert(iptv_try_failure(&st, why, sizeof why) == IPTV_FAIL_LOGIN);
    assert(!strcmp(iptv_source()->pass, before.pass) && iptv_list() && iptv_list()->nCh == n0);
    settle();                                  // the interrupted load of the current one finishes
    snprintf(t.pass, sizeof t.pass, "fa10");
    snprintf(t.user, sizeof t.user, "d5fe");
    iptv_try_source(&t, 15);
    for (int i = 0; i < 1500 && iptv_try_state() == IPTV_LOADING; i++) { iptv_step(); SDL_Delay(10); }
    assert(iptv_try_state() == IPTV_READY);
    assert(!strcmp(iptv_source()->pass, "fa10"));
    settle();
    assert(iptv_list() && iptv_list()->nCh == 5); }

  // A wrong password: said as such, not as an 884.
  load(IPTV_SRC_XTREAM, "", base, "d5fe", "wrong");
  printf("status: '%s'\n", iptv_status());
  assert(strstr(iptv_status(), "refused this login"));

  // An expired account.
  load(IPTV_SRC_XTREAM, "", base, "old", "fa10");
  printf("status: '%s'\n", iptv_status());
  assert(strstr(iptv_status(), "expired"));

  // A one-off server error on the API: the second try loads the channels.
  load(IPTV_SRC_XTREAM, "", base, "flaky", "fa10");
  printf("status: '%s'\n", iptv_status());
  assert(iptv_state() == IPTV_READY && iptv_list() && iptv_list()->nCh == 5);

  // An API that keeps failing, with get.php refused as well: the API's error is
  // the one said, not get.php's 884.
  load(IPTV_SRC_XTREAM, "", base, "down", "fa10");
  printf("status: '%s'\n", iptv_status());
  assert(strstr(iptv_status(), "(513)") && !strstr(iptv_status(), "884"));

  iptv_shutdown();
  puts("PASS iptv_xtream: get.php refused (884), channels through player_api.php; pasted get.php link and its output=ts; extra guides fill the gaps; country guides found by tag and kept; login and expiry said; a one-off API error retried, a lasting one said as itself.");
  return 0;
}
