// The screen router.
//
// Before this, main.c decided between home and detail with an if. With the menu,
// search, library, settings and player, that if would have become a tangle where
// every screen has to know about the others — and the rule of "who eats the key"
// would be spread across six files. Here there is one CURRENT screen and a single
// priority order, written down in one place.
//
// The order of who receives the D-pad, from the top down:
//   1. player  — covers the whole screen
//   2. detail  — a layer over the current screen
//   3. menu    — a layer over the current screen
//   4. the current screen (home, search, library or settings)
#include "app.h"
#include "login.h"
#include "session.h"
#include "profiles.h"
#include "profile_select.h"
#include "sync.h"
#include "traktauth.h"
#include "simklauth.h"
#include "text.h"
#include "seeall.h"
#include "ctxmenu.h"
#include "mark.h"
#include <string.h>
#include "home.h"
#include "detail.h"
#include "menu.h"
#include "search.h"
#include "discoverui.h"
#include "library.h"
#include "profile.h"
#include "social.h"
#include "settings.h"
#include "player.h"
#include "streams.h"
#include "sourcepref.h"
#include "extras.h"
#include "video.h"
#include "addons.h"
#include "discover.h"
#include "trakt.h"
#include "tracks.h"
#include "episodes.h"
#include "proxy.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

// A debrid link expires in minutes; one minute is slack enough for the user to
// press Play right after opening the title without paying for another search.
#define NV_LINK_VALID_MS 60000

static int waitingSource;
static pthread_t threadSource;
// How long automatic Play waits on a slow addon before checking the links that
// have already landed (see the early start in app_update).
#define NV_SOURCE_EARLY_MS 2500u
// 1 while the check running was started on a partial list; `triedEarly` makes it
// once per Play, so a partial list that failed is not checked again row by row
// every frame until the search ends.
static int verifyEarly, triedEarly;
static _Atomic int sourceChosen = -2;   // release/acquire entre verificacao e UI

// Hands a chosen source to the player. A source that needs request headers goes
// through the loopback relay, because the pipeline cannot send them (proxy.h).
//
// And it tells the subtitle search which FILE is now playing, so OpenSubtitles
// can answer with subtitles timed for it rather than for the title in general.
static void playSource(const Stream *s) {
  static char local[4200];
  addons_subtitles_file(s->videoHash, s->videoSize, s->file);
  player_set_source(proxy_wrap(s->url, s->headers, local, sizeof local));
}

static ProfileData profilePending;
static int profileSuccess;
static _Atomic int profileLoad; // 0=idle, 1=network, 2=snapshot ready
static _Atomic unsigned profileGeneration = 1;
static pthread_mutex_t profileLock = PTHREAD_MUTEX_INITIALIZER;
typedef struct { unsigned generation; int profile; char account[96]; } ProfileRequest;
static void *loadProfile(void *u) {
  ProfileRequest *request=u;
  ProfileData new={0};
  int success=trakt_profile(&new);
  pthread_mutex_lock(&profileLock);
  // A change of account/profile invalidates the answer. The worker finishes, but
  // never publishes an old identity nor leaves a stale snapshot in the queue.
  if(request->generation==atomic_load_explicit(&profileGeneration,memory_order_acquire) &&
     atomic_load_explicit(&profileLoad,memory_order_relaxed)==1 && session_loggedin() &&
     profiles_active()==request->profile && !strcmp(session_user(),request->account)){
    profileSuccess=success;
    profilePending=new;
    atomic_store_explicit(&profileLoad,2,memory_order_release);
  }
  pthread_mutex_unlock(&profileLock);
  free(request);
  return NULL;
}
static void invalidateProfile(void) {
  atomic_fetch_add_explicit(&profileGeneration,1,memory_order_acq_rel);
  atomic_store_explicit(&profileLoad,0,memory_order_release);
  pthread_mutex_lock(&profileLock);memset(&profilePending,0,sizeof profilePending);profileSuccess=0;pthread_mutex_unlock(&profileLock);
  profile_set_data(NULL);
}
static void requestProfile(void) {
  pthread_t t;
  int expected = 0;
  ProfileRequest *request;
  profile_set_loading(1);
  if (!atomic_compare_exchange_strong(&profileLoad, &expected, 1)) return;
  request=calloc(1,sizeof *request);
  if(!request){atomic_store(&profileLoad,0);profile_set_error("Could not start the query. Try again.");return;}
  request->generation=atomic_load_explicit(&profileGeneration,memory_order_acquire);
  request->profile=profiles_active();
  snprintf(request->account,sizeof request->account,"%s",session_user());
  if (pthread_create(&t, NULL, loadProfile, request) == 0) pthread_detach(t);
  else { free(request); atomic_store(&profileLoad,0); profile_set_error("Could not start the query. Try again."); }
}

// Verification makes one request per candidate source and blocks; on a thread of
// its own the screen carries on at 60fps showing "Opening source".
// The row picked in the sheet when it still has to be resolved (a torrent),
// or -1 for the automatic walk.
static int sourcePicked = -1;
static void *chooseSource(void *u) {
  int t = 0, e = 0;
  (void)u;
  player_episode_current(&t, &e);
  if (sourcePicked >= 0) { sourceChosen = stream_verify_one(sourcePicked, t, e); return NULL; }
  // Up to 8: in a typical list of 12, the first ones are usually from the same
  // provider and fail together when the file is not cached. Testing only a few
  // returned "no source works" with good sources just further down.
  sourceChosen = stream_first_good(8, t, e);
  return NULL;
}
#include "catalog.h"
#include "gfx.h"
#include "catalog.h"
#include "layout.h"
#include <stdio.h>

static Screen screen = SCREEN_HOME;
static int wantsExit = 0;

// The detail screen needs the REAL rectangle the card came from so the flight can
// start there. Each screen that opens a title supplies its own; when none does
// (the menu's case, or an index coming from outside), it falls back to the whole
// screen.
static void openTitle(const HomeItem *it) {
  // THE SHARED-ELEMENT OPENING NEEDS SOMETHING ON SCREEN TO SHARE. It flies the
  // backdrop out of the home hero's rectangle and carries the hero's logo into the
  // title screen's layout, and both of those read state the HOME draws.
  //
  // openTitle is reached from the search and the Discover page too, and there the
  // home is not what is behind: its hero rect and logo rect are whatever the last
  // visit left, so the flight would start from a rectangle the viewer has never
  // seen and carry a logo that is not on screen. A "See all" grid covers the home
  // for the same reason, and with the hero switched off there is no rect at all.
  // Those get the plain full-bleed fade the closing already uses.
  int shared = screen == SCREEN_HOME && !seeall_is_open() && settings_hero_on();
  if (it && it->art) detail_open(it, shared);
}

static void openByIndex(int i) {
  const CatItem *c = cat_item(i);
  if (!c || (!c->backdrop[0] && !c->poster[0])) return;
  HomeItem it;
  GfxRect all = { 0, 0, NV_SCREEN_W, NV_SCREEN_H };
  // `it` is on the STACK and this function filled in every field EXCEPT the index
  // — which went in as rubbish. Since the library and the search open through
  // here, any title chosen in them led to the same film. The home did not suffer
  // because it hands over the whole HomeItem, index included.
  //
  // The field exists precisely because of this defect, and its comment in home.h
  // already warned: "it was missing, which is why the detail always opened item
  // 0". Zeroing the struct first guarantees the next new field is born defined
  // instead of repeating the history.
  memset(&it, 0, sizeof it);
  it.index_ = i;
  it.rect = all;
  it.art = c->backdrop[0] ? c->backdrop : c->poster;
  it.title = c->title;
  it.genre = c->genre;
  it.meta = c->meta;
  openTitle(&it);
}

// Builds the id the addons expect. For a series it is
// "tt1234567:season:episode"; without those two numbers the answer comes back
// EMPTY with HTTP 200, and that is why addons_fetch hard-coded ":1:1" — which
// made every series show episode 1's sources, whichever one was chosen.
static void idOfTarget(const CatItem *ci, char *dst, size_t n) {
  int t = 0, e = 0;
  if (!ci) { if (n) dst[0] = 0; return; }
  if (!strcmp(ci->kind, "series") && detail_ep_focus(&t, &e) && t > 0 && e > 0)
    snprintf(dst, n, "%.*s:%d:%d", (int)strcspn(ci->imdb,":"),ci->imdb, t, e);
  else
    snprintf(dst, n, "%s", ci->imdb);
}

static void swapScreen(Screen new) {
  if (new == screen) return;
  screen = new;
  // Each screen zeroes its own state when it is opened: coming back to the search
  // with the text from two navigations ago would be rubbish, not useful memory.
  switch (screen) {
    case SCREEN_SEARCH:      search_start();      break;
    case SCREEN_DISCOVER:   dui_start();         break;
    case SCREEN_LIBRARY: library_start(); break;
    case SCREEN_PROFILE:     profile_open(); requestProfile(); break;
    case SCREEN_SETTINGS:    settings_start();    break;
    default: break;
  }
}

static void targetPlayer(char *target, size_t size) {
  const CatItem *c = cat_item(player_index());
  int t, e;
  player_episode_current(&t, &e);
  if (!c) { target[0] = 0; return; }
  if (t > 0 && e > 0) snprintf(target,size,"%.*s:%d:%d",(int)strcspn(c->imdb,":"),c->imdb,t,e);
  else snprintf(target,size,"%s",c->imdb);
}
static void fetchForPlayer(void) {
  const CatItem *c = cat_item(player_index());
  char target[64]; targetPlayer(target,sizeof target);
  if (c && target[0]) {
    addons_fetch(target,c->kind);
    addons_fetch_subtitles(target,c->kind);
  }
}
static void episodeOfDetail(void) {
  int t=0,e=0;
  detail_ep_focus(&t,&e);
  player_set_episode(t,e);
}

// Has the home loaded? Without art in the package it does not, and until now
// that BROUGHT DOWN the app: app_start returned 0 and main exited with code 1.
// In an owner's package that never happened because the art shipped with it; in a
// distributable package, which can carry nobody's art or credentials, that was
// EVERYONE's first-run behaviour — the app opened and closed, before even the
// login screen.
static int homeReady;

int app_start(const char *dirArt) {
  homeReady = home_start(dirArt);
  if (!homeReady)
    printf("[app] no art in the package: home only appears after the first sync\n");
  menu_start();
  profile_start();
  // With no account, the app opens on the login screen. With a stored session it
  // does not even pass through it — asking for the code again on every start
  // would be the same as never having stored it.
  if (session_loggedin()) {
    screen = SCREEN_HOME;
    // With a stored session the cycle starts at boot: it is what brings in the
    // person's addons and Trakt, without which the home shows only what came in
    // the package.
    sync_start();
  } else {
    screen = SCREEN_LOGIN;
    login_start();
  }
  return 1;
}

void app_event(const SDL_Event *e) {
  if (e->type == SDL_QUIT) { wantsExit = 1; return; }

  // Login comes before everything, the player included: while there is no account
  // the rest of the app has no data to work with. Choosing a profile comes right
  // after, because that is what decides WHO the rest of the app will sync for.
  if (screen == SCREEN_LOGIN)          { login_event(e);     return; }
  if (screen == SCREEN_CHOICE_PROFILE) { profilesel_event(e); return; }

  if(e->type==SDL_KEYDOWN && !e->key.repeat &&
     (e->key.keysym.sym==SDLK_s || e->key.keysym.scancode==NV_SCANCODE_BLUE) &&
     screen==SCREEN_HOME && !player_is_open() && !detail_is_open() && !seeall_is_open() && !menu_is_open()) {
    if(profile_is_open() && profile_side())profile_close();
    else {profile_open_side();requestProfile();}
    return;
  }

  // The source sheet sits above everything: it is a question, and while it is
  // standing nothing else should answer the D-pad.
  if (tracks_is_open()) { tracks_event(e); return; }
  if (episodes_is_open()) { episodes_event(e); return; }
  if (stream_sheet_is_open()) { stream_sheet_event(e); return; }
  if (player_is_open()) { player_event(e); return; }
  if (detail_is_open()) { detail_event(e); return; }
  if (profile_is_open() && profile_side()) { profile_event(e); return; }
  if (menu_is_open())   { menu_event(e);   return; }
  // "See all" sits BETWEEN the home and the detail: it covers the home and the
  // detail covers it. That is why it comes after the detail and before the
  // per-screen routing.
  // The poster's menu sits ABOVE everything the home shows: it is modal.
  if (ctx_is_open())     { ctx_event(e);     return; }
  if (seeall_is_open()) { seeall_event(e); return; }

  switch (screen) {
    case SCREEN_SEARCH:      search_event(e);      break;
    case SCREEN_DISCOVER:   dui_event(e);         break;
    case SCREEN_LIBRARY: library_event(e); break;
    case SCREEN_PROFILE:     profile_event(e);     break;
    case SCREEN_SOCIAL:     social_event(e);     break;
    case SCREEN_SETTINGS:    settings_event(e);    break;
    default:              home_event(e);       break;
  }

  // The menu opens HERE, in the same event that asked for it, and not in the next
  // app_update. Deferred by one frame, the keys that come right after the LEFT —
  // and on a remote they do come — are delivered to the screen behind, which still
  // thinks it owns the focus. Every screen can ask, each with LEFT at its own
  // left edge.
  { int wants = 0;
    switch (screen) {
      case SCREEN_SEARCH:   wants = search_requested_menu();   break;
      case SCREEN_DISCOVER: wants = dui_requested_menu();      break;
      case SCREEN_LIBRARY:  wants = library_requested_menu();  break;
      case SCREEN_PROFILE:  wants = profile_requested_menu();  break;
      case SCREEN_SOCIAL:   wants = social_requested_menu();   break;
      case SCREEN_SETTINGS: wants = settings_requested_menu(); break;
      default:              wants = home_requested_menu();     break;
    }
    if (wants) menu_open(); }
}

// The detail screen may ask to open ANOTHER title (a credit from an actor's
// filmography, a "More like this" item). What swaps is here, and not it:
// reopening itself in the middle of its own drawing is the kind of thing that
// breaks silently, and the router is already the only place that knows how to
// open a title.
// The eye button: mark as WATCHED. It writes full progress into the app's file
// and tells Trakt, which is the source the owner uses on their other devices. It
// lives in the router for the same reason as everything else: it is the router
// that knows the catalogue and Trakt, and the detail screen need know neither.
static void markWatchedIfRequested(void) {
  const CatItem *c;
  int i;
  if (!detail_requested_watched()) return;
  i = detail_index();
  c = cat_item(i);
  if (!c) return;
  // It used to call trakt_mark (which is /scrobble/pause) with a duration of 1.0
  // — and that function starts with `durationSeg <= 1.0 -> return`. The button
  // changed only the local mirror and Trakt was NEVER told: it looked like it
  // worked and it did not. Now it goes through /sync/history, which is the
  // "I watched it" endpoint.
  //
  // And it toggles rather than only marking: the icon already shows both states,
  // so a button that only adds would have no way to undo a mistaken press.
  { int watched = (c->progress >= 90);
    cat_save_progress(i, watched ? 0.0 : 1.0, 1.0);
    if (c->imdb[0]) trakt_watched(c->imdb, !watched);
    printf("[app] watched %s: %s\n", watched ? "unmarked" : "marked",
           c->title); fflush(stdout); }
}

static void swapOfTitleIfRequested(void) {
  int target = detail_requested_open();
  if (target >= 0) { openByIndex(target); return; }
  // A title that came from OUTSIDE the catalogue: discovery fetched the meta on a
  // thread and says so here once it has gone in. Opening it on the network thread
  // would mean touching the screen from another thread; this is the only place
  // that opens a title.
  { int new = disc_title_ready();
    if (new >= 0) openByIndex(new); }
}

void app_update(float dt, Uint32 now) {
  if (screen == SCREEN_LOGIN) {
    login_update(dt, now);
    // The swap only happens HERE, when the session really exists — not at the
    // instant the server answered. That way the home never opens with a
    // half-finished session.
    if (login_done()) {
      // Right after signing in, the first sync cycle: it is what discovers how
      // many profiles the account has, and without it the picker screen would
      // have nothing to show.
      sync_reapply_settings();
      sync_start();
      screen = SCREEN_CHOICE_PROFILE;
      profilesel_start();
      menu_set_destination(MENU_START);
    }
    return;
  }

  // A session lost mid-use (a refused renewal): going back to login is the only
  // honest way out. Staying on the home would show the package's sample catalogue
  // as though it were the person's.
  if (!session_loggedin()) {
    invalidateProfile();
    screen = SCREEN_LOGIN;
    login_start();
    return;
  }

  // THE TRAKT AND SIMKL LINKS STEP ON EVERY SCREEN, and that is the whole point
  // of them being here rather than in a screen's own update. Both are device-code
  // flows: the code goes on the TV and the poll of /oauth/device/token is what
  // notices the person authorised it on their phone. But the flow is STARTED from
  // Settings, and these two calls used to sit inside the SCREEN_CHOICE_PROFILE
  // branch below — which returns — so from Settings nothing ever polled. The
  // phone said "device approved" and the TV sat on the code forever.
  //
  // Applying the token lives in the same step (traktauth.c only touches trakt.c
  // from the main loop), so a poll that did succeed still would not have landed.
  traktauth_step((unsigned)now);
  simklauth_step((unsigned)now);

  if (screen == SCREEN_CHOICE_PROFILE) {
    sync_step((unsigned)now);
    profilesel_update(dt, now);
    if (profilesel_requested_retry()) { sync_start(); return; }
    if (profilesel_wants_exit()) {
      // Without a confirmed choice, going back must not pick profile 1 by
      // accident. The screen stays visible and waits for an explicit choice.
      if (!profiles_needs_choose()) { screen = SCREEN_HOME; menu_set_destination(MENU_START); }
      return;
    }
    if (profilesel_done()) {
      // The profile has changed the sync's destination: running again brings THIS
      // profile's addons and progress, and not profile 1's, which the first cycle
      // picked up for want of a choice.
      invalidateProfile();
      sync_reapply_settings();
      sync_start();
      screen = SCREEN_HOME;
    }
    return;
  }

  // One cycle at a time, and only when the account exists. The step is cheap:
  // with no finished thread it does nothing.
  sync_step((unsigned)now);

  // An account with more than one profile and none chosen ON THIS INSTALLATION:
  // ask. This holds for someone who opened the app with a session already stored
  // too — the common path after the first day. Without this the app assumed
  // profile 1 forever, and `profiles_needs_choose()` was dead code.
  if (screen == SCREEN_HOME && !player_is_open() && !detail_is_open() &&
      profiles_needs_choose()) {
    screen = SCREEN_CHOICE_PROFILE;
    profilesel_start();
    return;
  }
  // And the automatic cycle — never with the player open: a burst of HTTP in the
  // middle of the video competes for CPU and network with the decoder.
  if (!player_is_open()) sync_periodic((unsigned)now);

  // During verification, do not replace the list the workers are reading.
  if (waitingSource != 2) addons_state();
  swapOfTitleIfRequested();
  markWatchedIfRequested();
  if (atomic_load_explicit(&profileLoad, memory_order_acquire) == 2) {
    ProfileData snapshot; int success;
    pthread_mutex_lock(&profileLock); snapshot=profilePending; success=profileSuccess; pthread_mutex_unlock(&profileLock);
    if (success) profile_set_data(&snapshot);
    else if (!trakt_active()) profile_set_state(PROFILE_STATE_DISCONNECTED,
                                                    "Trakt disconnected. Link the account to see your profile.");
    else profile_set_state(PROFILE_STATE_UNAVAILABLE,
                               "Profile unavailable. Your last summary is still safe, if there is one.");
    atomic_store_explicit(&profileLoad, 0, memory_order_release);
  }
  if ((screen == SCREEN_PROFILE || profile_side()) && profile_requested_update()) requestProfile();
  if (profile_requested_complete()) swapScreen(SCREEN_PROFILE);
  if (screen==SCREEN_HOME) {
    CatItem person;
    if(home_requested_person_social(&person)) {
      social_open(&person);swapScreen(SCREEN_SOCIAL);
    }
  }
  if (screen==SCREEN_HOME && home_requested_social()) {
    swapScreen(SCREEN_SETTINGS);menu_set_destination(MENU_SETTINGS);
  }
  // The compass in the search header. It is the only way into Discover, so the
  // request is read here and nowhere else.
  //
  // dui_start() ANSWERS, and the answer is obeyed: with no addon catalogues
  // loaded there is nothing to browse, and swapping to a screen that can only
  // show an empty grid and a picker with no options would be worse than the
  // press doing nothing. swapScreen would call dui_start a second time, so the
  // check is made here and the screen set directly.
  if (screen == SCREEN_SEARCH && search_requested_discover()) {
    if (dui_start()) screen = SCREEN_DISCOVER;
  }

  // Back out of Discover returns to the SEARCH screen, not to the home: it is
  // the reverse of the press that opened it, and the home is two steps away.
  // Hence a line of its own, before the generic close below.
  if (screen == SCREEN_DISCOVER && dui_wants_exit()) {
    swapScreen(SCREEN_SEARCH);
    menu_set_destination(MENU_FETCH);
  }

  // Outside the home, Back has somewhere to go: the home. Only there does it close the app.
  if (screen != SCREEN_HOME) {
    int shouldClose = (screen == SCREEN_SEARCH      && search_wants_exit())
              || (screen == SCREEN_LIBRARY && library_wants_exit())
              || (screen == SCREEN_PROFILE      && profile_wants_exit())
              || (screen == SCREEN_SOCIAL      && social_wants_exit())
              || (screen == SCREEN_SETTINGS    && settings_wants_exit());
    if (shouldClose) { swapScreen(SCREEN_HOME); menu_set_destination(MENU_START); }
  } else if (home_wants_exit()) {
    wantsExit = 1;
  }

  // Switching user, asked for by the side bar's footer. It comes BEFORE the
  // destination: the two come out of the same menu, and whoever asked to switch
  // does not want to change tab.
  if (menu_requested_swap()) {
    invalidateProfile();
    screen = SCREEN_CHOICE_PROFILE;
    profilesel_start();
    return;
  }

  // Choosing the screen already showing is a no-op: swapScreen ignores it.
  if (menu_changed_destination()) {
    switch (menu_destination()) {
      case MENU_FETCH:     swapScreen(SCREEN_SEARCH);      break;
      case MENU_LIBRARY: swapScreen(SCREEN_LIBRARY); break;
      // Already on the full profile: the side panel would only fold it back up.
      case MENU_PROFILE:
        if (screen != SCREEN_PROFILE) { profile_open_side(); requestProfile(); }
        break;
      case MENU_SETTINGS:    swapScreen(SCREEN_SETTINGS);    break;
      default:              swapScreen(SCREEN_HOME);       break;
    }
  }
  // Requests to open a title, coming from any screen.
  if (!detail_is_open() && !player_is_open()) {
    int idx = -1;
    HomeItem it;
    if (screen == SCREEN_HOME && home_requested_open()) {
      if (home_item_focused(&it)) openTitle(&it);
    } else if (screen == SCREEN_SEARCH && search_requested_open(&idx)) {
      if (search_item_focused(&it)) openTitle(&it); else openByIndex(idx);
    } else if (screen == SCREEN_DISCOVER && dui_requested_open(&idx)) {
      if (dui_item_focused(&it)) openTitle(&it); else openByIndex(idx);
    } else if (screen == SCREEN_LIBRARY && library_requested_open(&idx)) {
      if (library_item_focused(&it)) openTitle(&it); else openByIndex(idx);
    } else if (screen == SCREEN_PROFILE) {
      ProfileHighlight p;
      if (profile_item_selected(&p) && p.id[0]) {
        idx = cat_index_by_imdb(p.id);
        if (idx >= 0) openByIndex(idx); else disc_request_title(p.id);
      }
    } else if (screen == SCREEN_SOCIAL) {
      SocialItemSelected s;
      if (social_item_selected(&s) && s.imdb[0]) {
        idx=cat_index_by_imdb(s.imdb);
        if(idx>=0)openByIndex(idx); else disc_request_title(s.imdb);
      }
    }
  }

  // The detail screen's buttons: what knows there is a player and a library is the
  // router, not the detail screen.
  if (detail_is_open()) {
    // Play without choosing = automatic mode: the stream_automatic rule (MP4 4K
    // Dolby Vision first, otherwise the first in the list) decides on its own.
    // With no list, the player opens with no video rather than not opening — a
    // screen saying there is no source beats a button that appears not to respond.
    // On opening a title, ask for the sources NOW — the search takes seconds and
    // waiting for the user to press Play before starting would make the first
    // playback look frozen.
    // The trigger is the ID, not the title's index. With the index, changing
    // EPISODE did not repeat the search and the list stayed the previous
    // episode's — half a fix would be worse than none, because the screen would
    // show one episode's sources under another's name.
    { static char lastTarget[32] = "";
      int i = detail_index();
      const CatItem *ci = cat_item(i);
      char target[32];
      idOfTarget(ci, target, sizeof target);
      if (!player_is_open() && waitingSource != 2 && ci && ci->imdb[0] && strcmp(target, lastTarget)) {
        snprintf(lastTarget, sizeof lastTarget, "%s", target);
        { addons_fetch(target, ci->kind); }
        // Episodes of the open title, in the season where the owner stopped. It
        // comes from the network on the spot: keeping 40 titles' episode lists in
        // the package went stale with every new season.
        // On a FILM the same thread fetches /meta/movie when the catalogue does
        // not have the cast yet: that is where the page's actors, directing and
        // genres come from.
        if (!strcmp(ci->kind, "series") || ci->nCast == 0) disc_episodes(i, 0);
        // OpenSubtitles subtitles alongside: there are dozens per title and the
        // search takes seconds. Asking only when the owner opens the tracks sheet
        // would leave them waiting in front of an empty list.
        addons_fetch_subtitles(target, ci->kind);
      } }
    if (detail_requested_play() && waitingSource != 2) {
      // The screen opens NOW, in the "opening source" state, and the choice
      // happens afterwards. Choosing first would leave the button unresponsive
      // for seconds, and choosing without verifying delivered the debrid's notice
      // video — which plays normally and so passes for success.
      const CatItem *ci = cat_item(detail_index());
      player_open(detail_index(), NULL);
      // The entrance crossfades over the page and flies the logo out of it, rather
      // than cutting to black and fading the loading screen up from nothing.
      { GfxRect from = { 0, 0, 0, 0 };
        detail_logo_rect(&from);
        player_open_from_detail(from); }
      episodeOfDetail();
      // The episode is only final AFTER the player opens. Always redo the
      // subtitle request at that point; the prefetch search may have started on
      // the previously focused episode, and the worker now switches to the most
      // recent request without publishing stale results.
      if (ci && ci->imdb[0]) {
        char targetSub[64]; targetPlayer(targetSub, sizeof targetSub);
        addons_fetch_subtitles(targetSub, ci->kind);
      }
      if (stream_age_ms() > NV_LINK_VALID_MS && ci && ci->imdb[0]) {
        char target[32]; idOfTarget(ci, target, sizeof target);
        printf("source: list %ums old, refreshing (%s)\n",
               (unsigned)stream_age_ms(), target);
        addons_fetch(target, ci->kind);
      }
      waitingSource = 1;
    }
    if (detail_requested_mark()) {
      // Toggles on Trakt AND in the local mirror. The starting state comes from
      // ci->inList, which discovery filled from the real watchlist; without it
      // the button added a title that was already there all over again.
      int i = detail_index();
      const CatItem *c = cat_item(i);
      library_toggle_list(i);
      if (c && c->imdb[0]) trakt_watchlist(c->imdb, !c->inList);
      if (c) cat_set_in_list(i, !c->inList);
    }
    if (detail_requested_sources()) {
      // TMDB's runtime, so the sheet can turn a file size into a bitrate. It is
      // 0 for a series — extras_profile_duration is the FILM fact sheet — and
      // the sheet simply shows no bitrate on those rows rather than one derived
      // from a length it does not have.
      stream_sheet_runtime(extras_profile_duration() * 60);
      stream_sheet_open();
    }
  }
  // Choosing a source in the sheet starts playing THAT one. Switching source with
  // the player already open counts too: it closes the current session and opens on
  // the new one, otherwise two would be stuck in the same pipeline.
  // The search fired by Play has finished: now VERIFY the sources, in order, until
  // one leads to the file — and only then switch the video on.
  // START ON WHAT HAS LANDED when an addon is slow. The list grows one addon at a
  // time (stream_insert), and waiting for the last one made automatic Play as
  // slow as the slowest addon — a dead one cost its whole 12 s timeout every
  // time. After NV_SOURCE_EARLY_MS the links already in are checked; a winner
  // plays at once, and if none of them resolves the router goes back to waiting
  // and checks the whole list once it is complete, exactly as before. The pick
  // can differ from the all-addons one only when an addon is this slow.
  //
  // NOT WITH A REMEMBERED SOURCE: the pick the person made by hand may belong to
  // the addon still out, and starting without it would skip it.
  { int early = 0;
    if (waitingSource == 1 && !triedEarly && addons_state() == ADD_SEARCHING &&
        stream_n() > 0 && addons_search_ms() >= NV_SOURCE_EARLY_MS) {
      const CatItem *ci = cat_item(player_index());
      early = !(ci && ci->imdb[0] && sourcepref_has(ci->imdb));
    }
    if (early) {
      printf("source: %u ms in, checking the %d sources already in\n",
             addons_search_ms(), stream_n());
      mark("source: checking early");
      triedEarly = 1;
      verifyEarly = 1;
      stream_prefer(-1);
      sourcePicked = -1;
      waitingSource = 2;
      sourceChosen = -2;
      if (pthread_create(&threadSource, NULL, chooseSource, NULL) != 0) {
        waitingSource = 1; verifyEarly = 0;
      }
    } }
  if (waitingSource == 1 && addons_state() != ADD_SEARCHING) {
    verifyEarly = 0;
    // THE SOURCE REMEMBERED FOR THIS TITLE GOES TO THE FRONT OF THE QUEUE.
    // Here and not on the button: the list only exists once the addons have
    // answered, and every path that asks for a source passes through here —
    // Play/Resume on the title page, a new episode from the sheet, and the next
    // episode. Nothing remembered gives -1, and the walk runs as it always did.
    { const CatItem *ci = cat_item(player_index());
      stream_prefer(ci && ci->imdb[0] ? sourcepref_pick(ci->imdb) : -1); }
    sourcePicked = -1;
    waitingSource = 2;
    sourceChosen = -2;
    if (pthread_create(&threadSource, NULL, chooseSource, NULL) != 0) {
      waitingSource = 0; player_error_source();
    }
  }
  // An early check that found nothing is not a failure: the addons still out may
  // have what the first ones did not. Back to waiting for the complete list.
  if (waitingSource == 2 && sourceChosen == -1 && verifyEarly &&
      player_is_open() && !player_wants_exit()) {
    pthread_join(threadSource, NULL);
    verifyEarly = 0;
    waitingSource = 1;
    mark("source: early check found nothing, waiting for every addon");
  }
  if (waitingSource == 2 && sourceChosen != -2) {
    pthread_join(threadSource,NULL);
    const Stream *s = sourceChosen >= 0 ? stream_item(sourceChosen) : NULL;
    waitingSource = 0;
    verifyEarly = 0; triedEarly = 0;
    printf("automatic (checked): %s\n", s ? s->label : "(no usable source)");
    // The HDR/DV claim goes BEFORE playing: it is what the ACB bind describes to
    // tv.display. Without it the C9 shows everything mapped to SDR.
    if (s) video_set_dv(s->dolbyVision);
    // It announces the CONTAINER by the same route: it is what saves the Matroska
    // probe on a file that would never have such a header.
    if (s) video_set_mp4(s->mp4 || strstr(s->url, ".mp4") != NULL);
    mark(s ? "source chosen" : "no usable source");
    if (player_is_open() && !player_wants_exit()) {
      stream_set_current(sourceChosen);
      if (s) playSource(s);
      else {
        char why[120];
        if (stream_n() == 0) snprintf(why, sizeof why, "no addon returned a source");
        else snprintf(why, sizeof why, "none of the best %d of %d sources resolved "
                      "(dead links or debrid notices)", stream_n() < 8 ? stream_n() : 8,
                      stream_n());
        player_report_failure("source", why);
        player_error_source();
      }
    }
  }

  int source;
  if (waitingSource != 2 && stream_sheet_chose(&source)) {
    const Stream *s = stream_item(source);
    printf("source chosen: %s\n", s ? s->label : "?");
    if (s) video_set_dv(s->dolbyVision);
    if (s) {
      waitingSource=0;
      int title=player_is_open()?player_index():detail_index(), t=0,e=0;
      // REMEMBER THE PICK, and only a manual one: what the automatic rule
      // chooses never becomes a preference (sourcepref.h).
      { const CatItem *ci = cat_item(title);
        if (ci && ci->imdb[0]) sourcepref_store(ci->imdb, s); }
      if (player_is_open()) { player_episode_current(&t,&e); player_shutdown(); }
      else detail_ep_focus(&t,&e);
      player_open(title,NULL);
      player_set_episode(t,e);
      if (s->url[0]) {
        stream_set_current(source);
        playSource(s);
      } else {
        // A torrent: its url only exists once the debrid has resolved it, and
        // that blocks, so it goes through the same thread as the automatic walk
        // and the same hand-over below.
        sourcePicked = source;
        waitingSource = 2;
        sourceChosen = -2;
        if (pthread_create(&threadSource, NULL, chooseSource, NULL) != 0) {
          waitingSource = 0; player_error_source();
        }
      }
    }
  }
  if (waitingSource != 2 && player_requested_sources()) {
    // The one place the runtime is known exactly, episode or film: it is the
    // file that is open.
    stream_sheet_runtime((int)video_duration());
    stream_sheet_open();
  }
  if (waitingSource != 2 && stream_sheet_reload()) {
    if (player_is_open()) fetchForPlayer();
    else {
      const CatItem *ci=cat_item(detail_index()); char id[64];
      idOfTarget(ci,id,sizeof id);
      if (ci) addons_fetch(id,ci->kind);
    }
  }
  { int t,e;
    if (waitingSource != 2 && episodes_chose(&t,&e) && player_is_open()) {
      int title=player_index();
      player_shutdown(); player_open(title,NULL);
      player_set_episode(t,e);
      mark("fetching sources"); fetchForPlayer(); waitingSource=1;
    }
  }
  { int t,e;
    if (waitingSource != 2 && player_requested_next(&t,&e) && player_is_open()) {
      int title=player_index();
      player_shutdown(); player_open(title,NULL); player_set_episode(t,e);
      mark("next episode: fetching sources"); fetchForPlayer(); waitingSource=1;
    }
  }
  // Near the end of an episode, the next one's sources are fetched ahead of time,
  // so Next (or the automatic advance) does not open on the full addon wait. The
  // id is built exactly as targetPlayer builds it, or the cache would never match.
  if (waitingSource == 0 && player_is_open()) {
    const CatEp *p = player_prefetch_next();
    const CatItem *c = cat_item(player_index());
    if (p && c && c->imdb[0]) {
      char id[64];
      snprintf(id, sizeof id, "%.*s:%d:%d", (int)strcspn(c->imdb, ":"), c->imdb,
               p->season, p->episode);
      addons_prefetch(id, c->kind);
    }
  }
  episodes_update(dt);
  // The Dolby Vision fallback deadline: if the claim does not produce a picture,
  // the video reloads itself without it. It has to tick every frame (see video.h).
  video_pump();
  // A rebuild requested when the account's addon list arrived. Here, and not in
  // sync_step, because the first build may still be running at that moment and the
  // request has to survive until it finishes.
  disc_step();
  // The player returns 1 for the audio column and 2 for the subtitle one.
  { int q = player_requested_tracks();
    if (q) tracks_open_at(q == 2 ? 1 : 0); }
  tracks_update(dt, now);
  stream_sheet_update(dt, now);

  if (player_wants_exit() && !player_is_open()) player_shutdown();

  player_update(dt, now);
  detail_update(dt, now);
  menu_update(dt, now);
  seeall_update(dt, now);
  ctx_update(dt, now);
  { int i = ctx_requested_details();
    HomeItem it;
    // THE SAME ROAD AS OK ON THE CARD. The hold menu is opened from a home card,
    // and that card is still the home's focused item underneath it — so the
    // details go through openTitle, which picks the zoom out of the card on the
    // home and the plain fade where a collection's grid covers it. Opening from a
    // made-up centred rect, as this did, gave every title the same fade whatever
    // it was opened from.
    if (i >= 0 && screen == SCREEN_HOME && home_item_focused(&it) &&
        it.index_ == i && it.art) {
      openTitle(&it);
    } else if (i >= 0) {
      const CatItem *ci = cat_item(i);
      memset(&it, 0, sizeof it);
      it.index_ = i;
      it.rect = (GfxRect){ NV_SCREEN_W * 0.5f - 124.0f, NV_SCREEN_H * 0.5f - 186.0f,
                           248.0f, 372.0f };
      it.art   = ci ? (ci->poster[0] ? ci->poster : ci->backdrop) : NULL;
      it.title = ci ? ci->title : NULL;
      it.genre = ci ? ci->genre : NULL;
      it.meta   = ci ? ci->meta : NULL;
      detail_open(&it, 0);
    } }
  // RESUME / START FROM THE BEGINNING, from the hold menu on a card with progress.
  // The same road Play takes on the title screen, without the title screen: the
  // player opens in its "opening source" state and the source walk follows. The
  // episode is the item's own — a Continue watching card carries the one being
  // resumed.
  { int fromStart = 0, i = ctx_requested_play(&fromStart);
    // "Play on select" takes the same road: OK on a Continue watching card, with
    // the setting on, is the menu's Resume without the menu.
    if (i < 0 && screen == SCREEN_HOME && !detail_is_open() && !player_is_open())
      i = home_requested_play();
    if (i >= 0 && waitingSource != 2) {
      const CatItem *ci = cat_item(i);
      player_open(i, NULL);
      player_set_episode(ci ? ci->season : 0, ci ? ci->episode : 0);
      if (fromStart) player_start_over();
      // The series' episode list, for the next-episode card and the episode
      // sheet; the title screen fetches it on open, and this path never opened it.
      if (ci && !strcmp(ci->kind, "series")) disc_episodes(i, 0);
      mark(fromStart ? "hold menu: start over" : "hold menu: resume");
      fetchForPlayer();
      waitingSource = 1;
    } }
  // THE WHOLE ROW, asked for from the poster's menu. It is the one path left to
  // the grid from a catalogue row now that the "See all" card at the end of the
  // row is gone: the row itself pages as the owner walks it, and this is for
  // wanting the list at a glance rather than one poster at a time.
  { CtxCatalog c;
    if (ctx_requested_seeall(&c) && c.base[0] && c.catId[0])
      seeall_open(c.base, c.kind, c.catId, c.title); }
  // A title chosen in the grid: opens the detail, as though it had come from the home.
  { int idx = seeall_requested_open();
    if (idx >= 0) {
      const CatItem *ci = cat_item(idx);
      // The grid has no source rectangle for the transition to grow out of: the
      // card is on the screen that is leaving. It comes in centred, the size of a
      // poster — the detail covers the screen straight afterwards anyway.
      HomeItem it;
      memset(&it, 0, sizeof it);
      it.index_ = idx;
      it.rect = (GfxRect){ NV_SCREEN_W * 0.5f - 124.0f, NV_SCREEN_H * 0.5f - 186.0f,
                           248.0f, 372.0f };
      it.art   = ci ? (ci->poster[0] ? ci->poster : ci->backdrop) : NULL;
      it.title = ci ? ci->title : NULL;
      it.genre = ci ? ci->genre : NULL;
      it.meta   = ci ? ci->meta : NULL;
      detail_open(&it, 0);
    } }
  switch (screen) {
    case SCREEN_SEARCH:      search_update(dt, now);      break;
    case SCREEN_DISCOVER:   dui_update(dt, now);         break;
    case SCREEN_LIBRARY: library_update(dt, now); break;
    case SCREEN_PROFILE:     break;
    case SCREEN_SETTINGS:    settings_update(dt, now);    break;
    default:              home_update(dt, now);       break;
  }
  profile_update(dt, now);
  if(screen==SCREEN_SOCIAL) social_update(dt, now);
}

void app_draw(Uint32 now) {
  if (screen == SCREEN_LOGIN)          { login_draw(now);     return; }
  if (screen == SCREEN_CHOICE_PROFILE) { profilesel_draw(now); return; }

  // A real empty state, instead of a black screen that looks like a hang.
  if (!homeReady && screen == SCREEN_HOME && !player_is_open() && !detail_is_open()) {
    GfxRect background = { 0, 0, NV_SCREEN_W, NV_SCREEN_H };
    TxtLine t, sb;
    gfx_color(background, 0.0f, NV_COLOR_BACKGROUND_R, NV_COLOR_BACKGROUND_G, NV_COLOR_BACKGROUND_B, 1.0f);
    t = txt_line(TXT_TITLE2, "Preparing your catalogue…", 255, 255, 255, 255);
    txt_draw(t, (NV_SCREEN_W - t.w) * 0.5f, 460.0f);
    sb = txt_line(TXT_BODY,
                   sync_state() == SYNC_RUNNING
                     ? "Fetching your addons and what you were watching."
                     : "If this does not move on, check your addons in the account.",
                   160, 162, 170, 255);
    txt_draw(sb, (NV_SCREEN_W - sb.w) * 0.5f, 546.0f);
    if (menu_visible()) menu_draw(now);
    return;
  }

  // The player covers everything; drawing what is behind it is work thrown away —
  // the same arithmetic that already held for the stretched detail card.
  //
  // EXCEPT WHILE IT IS STILL ARRIVING FROM THE TITLE SCREEN. For that half second the
  // page is the background the player's art crossfades over, and its logo is the one
  // in flight — so the page keeps drawing, minus its own copy of the logo.
  detail_hide_logo(player_logo_in_flight());
  if (!player_is_open() || player_handing_off()) {
    // "See all" covers the screen behind it completely (an opaque background), so
    // the home need not be drawn underneath — the same arithmetic as
    // detail_covers_screen.
    //
    // COVERS, not IS_OPEN. A collection opens by growing a window out of the card
    // that was pressed, and while that window is opening the home is the
    // BACKGROUND the grid is arriving over — its hero is the same folder's art,
    // standing still while the treatment changes around it. Cutting it at the
    // first frame, as is_open did, left the window opening over black.
    // THE TITLE SCREEN'S BACKDROP FIRST, under the screen it is replacing. It is
    // the background of the transition, not a layer over it: drawn afterwards its
    // left boundary sweeps through the home's shelves and copy as a hard line.
    detail_draw_bg(now);
    if (!detail_covers_screen() && !seeall_covers_screen()) {
      switch (screen) {
        case SCREEN_SEARCH:      search_draw(now);      break;
        case SCREEN_DISCOVER:   dui_draw(now);         break;
        case SCREEN_LIBRARY: library_draw(now); break;
        case SCREEN_PROFILE:     profile_draw(now);     break;
        case SCREEN_SOCIAL:     social_draw(now);     break;
        case SCREEN_SETTINGS:    settings_draw(now);    break;
        default:              home_draw(now);       break;
      }
    }
    if (!detail_covers_screen()) seeall_draw(now);
    ctx_draw(now);
    detail_draw(now);
    // The rail does NOT exist on the web app's detail screen: it is full-bleed and
    // the content column starts at x=72, that is, INSIDE what the rail would
    // occupy. With the rail on top, the logo, the "Play" button and the duration
    // line were clipped by the 144px black band — it was the first defect to show
    // up in the device capture after the port.
    // The rail disappears with the detail open (the web app does not have it on
    // that screen) and disappears too when `collapseSidebar` is on, which is the
    // state of the owner's profile. Collapsed, it takes no width at all: the
    // content starts at 104, and what returns that x is settings_content_x().
    // The `collapseSidebar` guard does NOT go here. It already exists INSIDE
    // menu_draw, and there it skips only the FIXED RAIL — which is right:
    // collapsed, the bar takes no width but still opens as a LAYER when it takes
    // focus, exactly as the web app does.
    //
    // With the guard at this point too, menu_draw was never called on the owner's
    // profile (collapseSidebar on): the menu opened, swallowed the keys and drew
    // nothing. There was no menu and no route to Settings — it was the defect
    // reported as "the menu doesn't show and there are no settings".
    // The same rule guarded in two places: on the inside it means "do not paint
    // the band", on the outside it meant "do not exist".
    if (menu_visible() && !detail_is_open())
      menu_draw(now);
    if(profile_side() && !detail_is_open()) profile_draw(now);
  }
  player_draw(now);
  episodes_draw();
  stream_sheet_draw(now);
  tracks_draw(now);
  player_draw_subtitle_over();
}

// --- THE DEV CHANNEL, see app.h for why -------------------------------------

// Opens a title's detail by IMDb id, as though a card had been chosen. The source
// rect is the centred poster the "see all" grid already uses when it has no card
// on screen to grow out of — the same situation, and the detail covers the screen
// a moment later anyway.
int app_goto_detail(const char *imdb) {
  int i;
  if (!imdb || !imdb[0]) return 0;
  i = cat_index_by_imdb(imdb);
  if (i < 0) return 0;
  { const CatItem *ci = cat_item(i);
    HomeItem it;
    memset(&it, 0, sizeof it);
    it.index_ = i;
    it.rect = (GfxRect){ NV_SCREEN_W * 0.5f - 124.0f, NV_SCREEN_H * 0.5f - 186.0f,
                         248.0f, 372.0f };
    it.art   = ci ? (ci->poster[0] ? ci->poster : ci->backdrop) : NULL;
    it.title = ci ? ci->title : NULL;
    it.genre = ci ? ci->genre : NULL;
    it.meta  = ci ? ci->meta : NULL;
    // The detail draws OVER whatever is behind it, and the home is what it expects
    // to find there — opening it from the settings screen would leave that drawing
    // underneath and the Back key returning to it.
    if (screen != SCREEN_HOME) swapScreen(SCREEN_HOME);
    // The home may have just been swapped in underneath and has not drawn a frame,
    // let alone a hero for THIS title: there is nothing to continue from.
    detail_open(&it, 0);
    printf("[goto] %s -> catalogue index %d (%s)\n",
           imdb, i, ci && ci->title[0] ? ci->title : "?");
    fflush(stdout); }
  return 1;
}

void app_where(char *out, size_t n) {
  static const char *NAME[] = { "login", "profile-picker", "home", "search",
                                "library", "profile", "settings", "player",
                                "social", "discover" };
  // sizeof, not a literal. The bound was written as `< 9` and the tenth screen
  // arrived: every capture taken from the Discover screen was labelled "?",
  // which is the one thing a position log must never say. Counting the array
  // cannot drift from the array.
  const char *base = ((size_t)screen < sizeof NAME / sizeof *NAME)
                   ? NAME[screen] : "?";
  // The OVERLAYS are what the arrow keys actually reach, and they are the part a
  // blind sequence gets wrong: the player and the detail both sit over the home.
  if (player_is_open()) { snprintf(out, n, "player"); return; }
  if (detail_is_open()) {
    int t = 0, e = 0;
    if (detail_ep_focus(&t, &e) && t > 0)
      snprintf(out, n, "detail idx=%d S%dE%d", detail_index(), t, e);
    else
      snprintf(out, n, "detail idx=%d", detail_index());
    return;
  }
  if (seeall_is_open()) { snprintf(out, n, "seeall"); return; }
  if (menu_is_open())   { snprintf(out, n, "%s+menu", base); return; }
  snprintf(out, n, "%s", base);
}

void app_regions(void) {
  if (detail_is_open()) { detail_regions(); return; }
  printf("[rect] no regions for this screen yet\n");
  fflush(stdout);
}

int app_wants_exit(void) { return wantsExit; }

void app_shutdown(void) {
  if (waitingSource == 2) pthread_join(threadSource, NULL);
  waitingSource = 0;
  player_shutdown();
  settings_shutdown();
  library_shutdown();
  profile_shutdown();
  search_shutdown();
  dui_shutdown();
  home_shutdown();
}
