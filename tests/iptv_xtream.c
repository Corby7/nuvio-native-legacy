// The loader against a fake Xtream panel (tests/iptv_xtream.sh serves it): a
// panel that answers 884 to get.php and serves player_api.php, the case that
// had Live TV fail on a real provider.
#include "iptv.h"
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
  snprintf(base, sizeof base, "http://127.0.0.1:%s", argv[1]);
  iptv_start();

  // The link a provider hands out as "your M3U", pasted as a playlist.
  snprintf(url, sizeof url, "%s/get.php?username=d5fe&password=fa10&type=m3u_plus&output=ts", base);
  load(IPTV_SRC_M3U, url, "", "", "");
  printf("status: '%s'\n", iptv_status());
  assert(iptv_state() == IPTV_READY && iptv_list());
  assert(iptv_list()->nCh == 2);
  assert(!strcmp(iptv_list()->ch[0].name, "\xC3\x87ocuk TV"));
  { char relay[4200];
    const char *u = iptv_play_url(0, relay, sizeof relay);
    printf("stream: %s\n", u);
    // TS: the link asked for output=ts, and the account allows it.
    assert(strstr(u, "/live/d5fe/fa10/101.ts")); }
  // The guide came from xmltv.php, the Xtream one, not from the playlist.
  assert(iptv_guide_state() == IPTV_READY && iptv_list()->ch[0].nPg == 1);

  // The same as an Xtream login: the same channels.
  load(IPTV_SRC_XTREAM, "", base, "d5fe", "fa10");
  assert(iptv_state() == IPTV_READY && iptv_list()->nCh == 2);
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

  // A wrong password: said as such, not as an 884.
  load(IPTV_SRC_XTREAM, "", base, "d5fe", "wrong");
  printf("status: '%s'\n", iptv_status());
  assert(strstr(iptv_status(), "refused this login"));

  // An expired account.
  load(IPTV_SRC_XTREAM, "", base, "old", "fa10");
  printf("status: '%s'\n", iptv_status());
  assert(strstr(iptv_status(), "expired"));

  iptv_shutdown();
  puts("PASS iptv_xtream: get.php refused (884), channels through player_api.php; pasted get.php link and its output=ts; extra guides fill the gaps; login and expiry said.");
  return 0;
}
