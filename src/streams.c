#include "pointer.h"
#include "streams.h"
#include "tabs.h"
#include <pthread.h>
#include "net.h"
#include "gfx.h"
#include "text.h"
#include "anim.h"
#include "settings.h"
#include "layout.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "addons.h"
#include "mark.h"
#include "debrid.h"

static Stream *list;
static int n = 0;
static int current = -1, reload;
void stream_set_current(int i) { current = i >= 0 && i < n ? i : -1; }
int stream_current(void) { return current; }
int stream_sheet_reload(void) { int r = reload; reload = 0; return r; }

static int is_open = 0, focus = 0, choice = -1;
static float anim = 0.0f, scroll = 0.0f;

// EACH ROW'S ENTRANCE, parallel to `list` and moved with it — see NV_SRC_IN_MS
// in layout.h. `in` climbs from <= 0 (still waiting its turn) to 1 (settled);
// `dy` is how far above its slot a displaced row still sits. NULL when an
// allocation failed, and then every row is simply drawn settled.
typedef struct { float in, dy; } RowFx;
static RowFx *fx;

// Queues row `i`'s entrance, `order` rows into its answer and `wait` ms late.
// With the sheet down there is nobody to show it to, so it lands settled.
static void fxArrive(int i, int order, float wait) {
  if (!fx) return;
  fx[i].dy = 0.0f;
  if (!is_open) { fx[i].in = 1.0f; return; }
  if (order > NV_SRC_IN_CAP) order = NV_SRC_IN_CAP;
  fx[i].in = -(wait + (float)order * NV_SRC_IN_STAGGER) / NV_SRC_IN_MS;
}
static float fxIn(int i) { return fx && i >= 0 && i < n ? anim_smooth(fx[i].in) : 1.0f; }
static float tipA[2];   // the header tooltips' fades: Reload, Close
// Whether the list's scroll follows the cursor. A row the Magic Remote's pointer
// focused leaves it where it is (goalScroll): centring that row would slide
// another under a pointer that had not moved. Any arrow, the wheel's included,
// hands it back.
static int follow = 1;
static float goalScroll;
// The highlight's row, in ITEM units (2.4 = between the third and the fourth).
// The highlight slides between rows instead of jumping: with a hard jump the
// sheet looked like it swapped its contents on every keypress, and on a D-pad
// it is the continuity of the highlight that says "this is still the same list,
// you only moved".

// "" and not "FILE" when nothing says. The row prints the container as one word
// in a list of facts, and a fact nobody knows is left out — "FILE" was a label
// invented for the unknown case and it appeared on more rows than any real
// container did.
static const char *containerOf(const Stream *s) {
  if (s->mp4 || strstr(s->url, ".mp4") || strstr(s->label, ".mp4")) return "MP4";
  if (strstr(s->url, ".mkv") || strstr(s->file, ".mkv") || strstr(s->description, ".mkv")) return "MKV";
  if (strstr(s->url, ".m3u8") || strstr(s->label, "HLS")) return "HLS";
  return "";
}

// Re-scores every row against the runtime the sheet has been told about. Both
// are defined with the sheet's own state, below.
static void rank(void);
static int runtimeS;

static Uint32 receivedIn;

Uint32 stream_age_ms(void) {
  return receivedIn ? SDL_GetTicks() - receivedIn : 0xFFFFFFFFu;
}

static pthread_mutex_t seeLock = PTHREAD_MUTEX_INITIALIZER;
static unsigned listGen;
static int preferred = -1;

void stream_prefer(int i) { preferred = i >= 0 && i < n ? i : -1; }

void stream_set_list(const Stream *l, int count) {
  int i, k = 0, debrid = debrid_active();
  receivedIn = SDL_GetTicks();
  Stream *new = l && count > 0 ? malloc(sizeof(Stream) * (size_t)count) : NULL;
  if (l && count > 0 && !new) return;
  // A torrent with no url stays only when there is a debrid to resolve it;
  // otherwise it is a row that can never play.
  for (i = 0; i < count && new; i++)
    if (l[i].url[0] || debrid) new[k++] = l[i];
  if (new && count - k) printf("[source] %d torrents dropped: no debrid key\n", count - k);
  // Under the verification lock: a check in flight copies its row under it and
  // writes a resolved url back under it, only while the list is still this one.
  pthread_mutex_lock(&seeLock);
  free(list); list = new; n = new ? k : 0; current = -1;
  listGen++;
  pthread_mutex_unlock(&seeLock);
  // A Reload with the sheet up cascades the new list in from the top.
  free(fx); fx = n ? malloc(sizeof(RowFx) * (size_t)n) : NULL;
  for (i = 0; i < n; i++) fxArrive(i, i, 0.0f);
  preferred = -1;
  focus = 0;
  // A NEW LIST IS A NEW TITLE, so the runtime goes with the old one. Keeping it
  // would divide this title's file sizes by the last title's length and print a
  // bitrate that is wrong by whatever the two differ by — a 45-minute episode
  // measured against a 3-hour film reads as four times the quality it is.
  // Whoever opens the sheet supplies the runtime again (stream_sheet_runtime).
  runtimeS = 0;
  rank();
}

int stream_n(void) {
  return n;
}


const Stream *stream_item(int i) {
  return i >= 0 && i < n ? &list[i] : NULL;
}

// The owner's scoring rule, from strongest to weakest:
//   MP4 4K Dolby Vision  >  4K Dolby Vision (any container)
//   >  4K  >  Dolby Vision  >  resolution  >  arrival order
//
// Adding weights rather than comparing field by field keeps the rule in one
// place and readable: changing the preference means touching a number, not
// rewriting a chain of ifs where the order of comparisons becomes a hidden rule.
static long points(const Stream *s) {
  long p = 0;
  // DOLBY VISION ONLY SCORES ON MP4 — and this is measurement, not theory.
  //
  // Recorded on the owner's set (LG C9, webOS 4.10) playing an MKV the addon
  // advertised as DV:
  //   pipeline hdr: HDR10 (source DV=1)
  // The TV DOWNGRADED to HDR10. It is the behaviour already reported for
  // Matroska — webOS engages native DV on MP4 and falls back to HDR10 on MKV —
  // now confirmed here instead of merely cited.
  //
  // What that means in practice: in profile 5 the base layer is NOT compatible
  // with HDR10 (it is IPT-PQ), so decoding it as HDR10 produces exactly the
  // washed-out colours the owner reported. Preferring the DV version in MKV
  // meant deliberately choosing the file that looks WORSE on this TV.
  //
  // There is no way to fix the decoding through the URI: Kodi only solves it by
  // discarding the enhancement layer and rewriting the RPU, which requires
  // demuxing and feeding the pipeline by buffer — another project, already
  // recorded in video.c. What IS in reach is to stop rewarding a useless source.
  if (s->mp4 && s->height >= 2160 && s->dolbyVision) p += 100000;
  if (s->height >= 2160)                             p +=  20000;
  if (s->mp4 && s->dolbyVision)                      p +=  10000;
  if (s->dolbyAtmos)                                 p +=   2000;
  p += s->height;
  return p;
}

// A notice address and not a content one. These two were MEASURED on the
// device: AIOStreams redirects to slate.m3u8/slate.mp4 ("This playback link
// couldn't be verified") when the link has expired, and Debridio to
// downloading.mp4 when the file is not cached on Real-Debrid yet. Both are
// valid ~120s MP4s that PLAY NORMALLY — there is no error to detect, only the address.
static int addressOfWarning(const char *u) {
  return strstr(u, "downloading.mp4") || strstr(u, "/slate") ||
         strstr(u, "slate.mp4") || strstr(u, "slate.m3u8") ? 1 : 0;
}

// VERIFICACAO DAS CANDIDATAS EM PARALELO.
//
// It used to be up to 8 net_url_final calls IN SERIES, 20 s each — the second
// half of the 16.5 s measured between opening the title and having a source.
// It is doubly wasteful: most attempts DO resolve, so waiting for the 1st to
// finish before starting the 2nd only has value when the 1st fails.
//
// THE CHOICE RULE DOES NOT CHANGE: it is still "the highest-scoring one that
// resolves". The threads check the best N at once and the result is read IN
// SCORE ORDER, so the chosen source is exactly the one the serial version would
// have chosen — only without waiting for the earlier ones to fail one by one.
#define SEE_THREADS 4

// IT ANSWERS AS SOON AS THE ANSWER IS KNOWN, not when the last check is done.
// The rule is "the first one in order that passed", so the answer is settled the
// moment some row has passed and every row ahead of it has finished and failed —
// what the rows behind it do can no longer change it. Joining every thread
// meant the top candidate resolving in 300 ms still waited out a dead link
// further down, up to its full 10 s.
//
// The threads that are still running are left to finish on their own, which is
// why the batch lives on the heap with a reference count instead of in globals:
// the last one out frees it, whether that is verify or a straggler. A straggler
// never touches the list — only its own batch — so it cannot disturb the next
// title's check either.
typedef struct {
  int idx, ok, done;
  char url[4096], hash[48];
  int fileIdx;
} Check;
typedef struct {
  pthread_mutex_t lock;
  pthread_cond_t  changed;
  Check *checks;
  int nChecks, nextCheck, season, episode, refs;
} Batch;

static void batchRelease(Batch *b) {
  int last;
  pthread_mutex_lock(&b->lock);
  last = --b->refs == 0;
  pthread_mutex_unlock(&b->lock);
  if (!last) return;
  pthread_cond_destroy(&b->changed);
  pthread_mutex_destroy(&b->lock);
  free(b->checks);
  free(b);
}

static void *threadVerify(void *u) {
  Batch *b = (Batch *)u;
  for (;;) {
    int mine, i, ok = 0;
    char end[900];
    Check *c;
    pthread_mutex_lock(&b->lock);
    if (b->nextCheck >= b->nChecks) { pthread_mutex_unlock(&b->lock); break; }
    mine = b->nextCheck++;
    pthread_mutex_unlock(&b->lock);
    c = &b->checks[mine];
    i = c->idx;
    if (!c->url[0] && c->hash[0]) {
      // A link fresh out of the debrid needs no second trip below. It stays in
      // the check until the batch is over; stream_first_good writes it into the
      // list, and only if the list is still the one it was taken from.
      if (debrid_resolve(c->hash, c->fileIdx, b->season, b->episode,
                         c->url, sizeof c->url)) ok = 1;
      else printf("[source] %d torrent did not resolve on the debrid\n", i);
    } else if (c->url[0]) {
      // 10 s and not 20: in parallel the timeout stops adding up, but it is still
      // the time the owner waits for the slowest one ahead of the winner.
      if (!net_url_final(c->url, 10, end, sizeof end))
        printf("[source] %d did not resolve\n", i);
      else if (addressOfWarning(end))
        printf("[source] %d is a warning (%.60s)\n", i, end);
      else ok = 1;
    }
    pthread_mutex_lock(&b->lock);
    c->ok = ok; c->done = 1;
    pthread_cond_signal(&b->changed);
    pthread_mutex_unlock(&b->lock);
  }
  batchRelease(b);
  return NULL;
}

// The answer so far, under the batch lock: the position of the first row that
// passed with everything ahead of it failed, -1 when all failed, -2 while a row
// ahead of any winner is still being checked.
static int settled(const Batch *b) {
  int q;
  for (q = 0; q < b->nChecks; q++) {
    if (!b->checks[q].done) return -2;
    if (b->checks[q].ok) return q;
  }
  return -1;
}

// Checks the rows in `used`, in parallel, and returns the first that passed IN
// THE ORDER GIVEN. A torrent's resolved url is written back into the list.
static int verify(const int *used, int nu, int season, int episode) {
  int chosen = -1, q, at, created = 0;
  unsigned gen;
  Batch *b = calloc(1, sizeof *b);
  if (!b) return -1;
  b->checks = calloc((size_t)nu, sizeof(Check));
  if (!b->checks) { free(b); return -1; }
  pthread_mutex_init(&b->lock, NULL);
  pthread_cond_init(&b->changed, NULL);
  b->nChecks = nu; b->season = season; b->episode = episode;
  // Each check copies its row UNDER THE LOCK: out of it, stream_set_list may
  // swap the list at any moment.
  pthread_mutex_lock(&seeLock);
  gen = listGen;
  for (q = 0; q < nu; q++) {
    int i = used[q];
    b->checks[q].idx = i;
    if (i < 0 || i >= n) continue;
    snprintf(b->checks[q].url, sizeof b->checks[q].url, "%s", list[i].url);
    snprintf(b->checks[q].hash, sizeof b->checks[q].hash, "%s", list[i].infoHash);
    b->checks[q].fileIdx = list[i].fileIdx;
  }
  pthread_mutex_unlock(&seeLock);

  // One reference for verify, one per thread — taken BEFORE the thread starts,
  // so a thread that finishes at once cannot free the batch from under us.
  b->refs = 1;
  for (q = 0; q < SEE_THREADS && q < nu; q++) {
    pthread_t t;
    pthread_mutex_lock(&b->lock); b->refs++; pthread_mutex_unlock(&b->lock);
    if (pthread_create(&t, NULL, threadVerify, b) == 0) { pthread_detach(t); created++; }
    else { pthread_mutex_lock(&b->lock); b->refs--; pthread_mutex_unlock(&b->lock); }
  }
  if (!created) {                        // no threads: in series, same result
    pthread_mutex_lock(&b->lock); b->refs++; pthread_mutex_unlock(&b->lock);
    threadVerify(b);
  }

  pthread_mutex_lock(&b->lock);
  while ((at = settled(b)) == -2) pthread_cond_wait(&b->changed, &b->lock);
  // Rows nobody has started yet are not worth starting: the answer is in.
  b->nextCheck = b->nChecks;
  pthread_mutex_unlock(&b->lock);

  if (at >= 0) {
    // The winner's check is done, so no thread writes to it any more.
    chosen = b->checks[at].idx;
    pthread_mutex_lock(&seeLock);
    if (gen != listGen || chosen >= n) chosen = -1;   // the list moved on
    else if (!list[chosen].url[0])
      snprintf(list[chosen].url, sizeof list[chosen].url, "%s", b->checks[at].url);
    pthread_mutex_unlock(&seeLock);
  }
  batchRelease(b);
  return chosen;
}

// The rows the automatic walk checks, in the order it reads them: the
// remembered one first, then the best `attempts` by score. Returns how many went
// into `used`, which has room for attempts + 1.
static int candidates(int attempts, int *used) {
  int nu = 0;
  int total = stream_n();
  if (total < 1) return 0;
  if (attempts < 1) attempts = 1;
  if (attempts > total) attempts = total;

  // THE PREFERRED ONE GOES FIRST, ahead of the score: it is the source the
  // person picked by hand for this title, and it can sit anywhere in the list —
  // the best `attempts` by score might never reach it. It goes in as a
  // CANDIDATE, not a decision: it is checked like the others, and when it does
  // not resolve the score order carries on right behind it.
  if (preferred >= 0 && preferred < total) used[nu++] = preferred;

  // Then the best `attempts` ones, IN SCORE ORDER — the same order the serial
  // loop walked.
  while (nu < attempts + (preferred >= 0 && preferred < total)) {
    int best = -1, i, j;
    long largerP = 0;
    for (i = 0; i < total; i++) {
      int watched = 0;
      for (j = 0; j < nu; j++) if (used[j] == i) { watched = 1; break; }
      if (watched) continue;
      { long p = points(&list[i]);
        if (best < 0 || p > largerP) { best = i; largerP = p; } }
    }
    if (best < 0) break;
    used[nu++] = best;
  }
  return nu;
}

int stream_first_good(int attempts, int season, int episode) {
  int *used, nu;
  int chosen = -1;
  if (stream_n() < 1) return -1;
  used = calloc((size_t)(attempts < 1 ? 1 : attempts) + 1, sizeof *used);
  if (!used) return -1;
  nu = candidates(attempts, used);
  if (nu < 1) { free(used); return -1; }

  mark("source: check start");
  chosen = verify(used, nu, season, episode);
  mark(chosen >= 0 ? "source: check ok" : "source: check returned nothing");
  free(used);
  if (chosen >= 0) printf("[source] %d ok\n", chosen);
  return chosen;
}

int stream_first_good_direct(int attempts, int season, int episode, unsigned *gen) {
  int *used, nu, q, chosen;
  used = calloc((size_t)(attempts < 1 ? 1 : attempts) + 1, sizeof *used);
  if (!used) return -1;
  // The rows are picked UNDER THE LOCK, and the list's generation with them:
  // this runs while the title page is open, where a new episode's search can
  // replace the list at any moment. The caller trusts the answer only while the
  // list is still that generation.
  pthread_mutex_lock(&seeLock);
  if (gen) *gen = listGen;
  nu = candidates(attempts, used);
  // CUT AT THE FIRST TORRENT. Resolving one asks the debrid to fetch it, which
  // is real work on the person's account for a title they may only be reading
  // about. The rows ahead of it are checked; whatever passes among them is the
  // row the full walk would choose too, because the walk reads in this order.
  for (q = 0; q < nu; q++) if (!list[used[q]].url[0]) break;
  pthread_mutex_unlock(&seeLock);
  nu = q;
  if (nu < 1) { free(used); return -1; }
  mark("source: check ahead of Play");
  chosen = verify(used, nu, season, episode);
  free(used);
  if (chosen >= 0) printf("[source] %d ok ahead of Play\n", chosen);
  return chosen;
}

unsigned stream_list_gen(void) {
  unsigned g;
  pthread_mutex_lock(&seeLock);
  g = listGen;
  pthread_mutex_unlock(&seeLock);
  return g;
}

int stream_verify_one(int index, int season, int episode) {
  int r;
  if (index < 0 || index >= stream_n()) return -1;
  r = verify(&index, 1, season, episode);
  printf("[source] picked %d %s\n", index, r >= 0 ? "ok" : "did not resolve");
  return r;
}

int stream_automatic(void) {
  if (!stream_n()) return -1;
  int best = 0;
  long larger = points(&list[0]);
  for (int i = 1; i < n; i++) {
    long p = points(&list[i]);
    // `>` and not `>=`: on a tie the FIRST in the list wins, which is the order
    // the addon returned — and it usually knows something the score cannot see.
    if (p > larger) { larger = p; best = i; }
  }
  return best;
}


static int group, filter;
static char providers[13][96];
static int nProviders;

static void rank(void) {
  int i;
  for (i = 0; i < n; i++) stream_rank(&list[i], runtimeS);
}

void stream_sheet_runtime(int seconds) {
  // A guard and not an assignment: video_duration() answers 0 until the pipeline
  // has the file open, and the player asks every frame the sheet is up. Taking
  // the 0 would wipe a runtime the title screen had already supplied.
  if (seconds <= 0 || seconds == runtimeS) return;
  runtimeS = seconds;
  rank();
}

static void updateProviders(void) {
  nProviders = 1;
  snprintf(providers[0],sizeof providers[0],"All");
  for (int i=0;i<n;i++) {
    int j;
    for(j=1;j<nProviders;j++) if(!strcmp(providers[j],list[i].provider)) break;
    if(j==nProviders && nProviders<13)
      snprintf(providers[nProviders++],96,"%s",list[i].provider);
  }
  if(filter>=nProviders) filter=0;
}
static int filtered(int line) {
  for(int i=0,j=0;i<n;i++)
    if(!filter || !strcmp(list[i].provider,providers[filter]))
      if(j++==line) return i;
  return -1;
}
static int nFiltered(void) {
  int k=0;
  for(int i=0;i<n;i++) if(!filter || !strcmp(list[i].provider,providers[filter])) k++;
  return k;
}
// One addon's rows, into the list at `at`. See streams.h for why the list grows
// instead of being replaced.
int stream_insert(int at, const Stream *l, int count) {
  int i, k = 0, debrid = debrid_active(), line = -1;
  Stream *grown;
  if (!l || count <= 0) return 0;
  if (at < 0 || at > n) at = n;
  for (i = 0; i < count; i++) if (l[i].url[0] || debrid) k++;
  if (count - k) printf("[source] %d torrents dropped: no debrid key\n", count - k);
  if (!k) return 0;
  // The row the sheet's cursor is on, as a LIST index: the cursor counts lines
  // of the filtered view, and rows landing above it would otherwise slide a
  // different source under it while the viewer is reading.
  if (is_open) line = filtered(focus);
  pthread_mutex_lock(&seeLock);
  grown = realloc(list, sizeof(Stream) * (size_t)(n + k));
  if (!grown) { pthread_mutex_unlock(&seeLock); return 0; }
  list = grown;
  memmove(list + at + k, list + at, sizeof(Stream) * (size_t)(n - at));
  for (i = 0, k = 0; i < count; i++)
    if (l[i].url[0] || debrid) list[at + k++] = l[i];
  n += k;
  listGen++;
  pthread_mutex_unlock(&seeLock);
  // The entrances move with their rows. A table that was already lost stays
  // lost rather than being grown over rows it never described.
  if (fx || n == k) {
    RowFx *g = realloc(fx, sizeof(RowFx) * (size_t)n);
    if (!g) { free(fx); fx = NULL; }
    else {
      int shown = 0, displaced = at < n - k;
      fx = g;
      memmove(fx + at + k, fx + at, sizeof(RowFx) * (size_t)(n - k - at));
      for (i = at; i < at + k; i++)
        if (!filter || !strcmp(list[i].provider, providers[filter])) shown++;
      // Rows pushed down start where they were drawn and slide to their slot;
      // the newcomers wait for the gap to open before they fade into it.
      if (is_open)
        for (i = at + k; i < n; i++) fx[i].dy -= (float)shown * NV_SRC_ROW;
      for (i = at; i < at + k; i++)
        fxArrive(i, i - at, displaced ? NV_SRC_IN_GAP : 0.0f);
    }
  }
  // Every index held into the list moves with the rows it pointed at.
  if (current >= at) current += k;
  if (preferred >= at) preferred += k;
  if (choice >= at) choice += k;
  for (i = at; i < at + k; i++) stream_rank(&list[i], runtimeS);
  if (line >= 0) {
    int j, v = 0;
    if (line >= at) line += k;
    updateProviders();
    for (j = 0; j < line; j++)
      if (!filter || !strcmp(list[j].provider, providers[filter])) v++;
    focus = v;
  }
  return k;
}

void stream_sheet_open(void) {
  follow=1;
  is_open=1; choice=-1; focus=0; group=1; filter=0; reload=0;
  updateProviders();
  if(current>=0) focus=current;
  scroll=0;
}
int stream_sheet_is_open(void) { return is_open; }
float stream_sheet_shown(void) { return anim; }
// The pointer's setters: the header's two icons, a provider tab (on the click
// only — sweeping across the strip must not reshuffle the list), and a row.
static void pointHead(int i,int unused) {
  (void)unused;
  if(!is_open) return;
  group=-1; focus=i;
}
static void clickTab(int i,int unused) {
  (void)unused;
  if(!is_open || i<0 || i>=nProviders) return;
  group=0; filter=i; focus=0; scroll=0; goalScroll=0;
}
static void pointRow(int row,int unused) {
  (void)unused;
  if(!is_open || row<0 || row>=nFiltered()) return;
  group=1; focus=row; follow=0;
}
static void pointOff(int a,int b) { (void)a; (void)b; is_open=0; }
static void pointPanel(int a,int b) { (void)a; (void)b; }

void stream_sheet_event(const SDL_Event *e) {
  if(!is_open || e->type!=SDL_KEYDOWN) return;
  SDL_Keycode k=e->key.keysym.sym;
  if(k==SDLK_UP || k==SDLK_DOWN || k==SDLK_LEFT || k==SDLK_RIGHT) follow=1;
  if(k==SDLK_ESCAPE || k==SDLK_AC_BACK || k==SDLK_BACKSPACE || k==SDLK_DELETE) {is_open=0;return;}
  if(k==SDLK_r) {reload=1;return;}
  int nf=nFiltered();
  if(k==SDLK_UP) {if(group==1 && focus>0) focus--; else if(group>-1) group--;}
  if(k==SDLK_DOWN) {if(group<1) group++; else if(focus<nf-1) focus++;}
  if(group==0 && (k==SDLK_LEFT || k==SDLK_RIGHT)) {
    filter+=k==SDLK_RIGHT?1:-1;
    if(filter<0) filter=0;
    if(filter>=nProviders) filter=nProviders-1;
    focus=0;scroll=0;
  }
  if(group==-1 && (k==SDLK_LEFT || k==SDLK_RIGHT)) focus=k==SDLK_LEFT?0:1;
  if(k==SDLK_RETURN || k==SDLK_KP_ENTER) {
    if(group==-1) {if(focus==0) reload=1;else is_open=0;}
    else if(group==0) {group=1;focus=0;}
    else {choice=filtered(focus);if(choice>=0) is_open=0;}
  }
}
// The cursor on the provider tabs, 0..1 — see tab_draw.
static float tabsLit;
// The trailing placeholder's strength and the searching dots' beside the count:
// both 1 while the addons are still out.
static float skelA;
void stream_sheet_update(float dt, Uint32 now) {
  (void)now;
  // IT ARRIVES AS THE PLAYER'S OTHER PANELS DO — audio, subtitles, episodes: a
  // short slide from the right (NV_TRK_SLIDE of its width) and a fade, on the
  // screen spring. It used to haul its whole width in from the edge, which made
  // it the one panel in the player that moved differently from the rest.
  anim=anim_spring(anim,is_open?1:0,dt,NV_SPRING_SCREEN);
  { float t=group==0?1.0f:0.0f;
    tabsLit=anim_spring(tabsLit,t,dt,t>tabsLit?NV_SPRING_FOCUS:NV_SPRING_BLUR); }
  { int k;
    for(k=0;k<2;k++)
      tipA[k]=anim_ramp(tipA[k],is_open && group==-1 && focus==k?1.0f:0.0f,dt,NV_SRC_TIP_MS); }
  updateProviders();
  int nf=nFiltered();
  if(group==1 && focus>=nf) focus=nf>0?nf-1:0;
  float area=NV_SCREEN_H-NV_SRC_TOP-NV_SRC_FOOT;
  float max=nf*NV_SRC_ROW-area;
  float target=focus*NV_SRC_ROW-(area-NV_SRC_ROW)*.5f;
  if(target>max) target=max;
  if(target<0) target=0;
  if(!follow) target=goalScroll;
  goalScroll=target;
  // Discover's grid spring, so the three lists that scroll row by row — Discover,
  // the title's episodes and this one — all move the same way.
  scroll=anim_spring(scroll,target,dt,NV_SPRING_GRID);
  // The displaced rows ride the SAME spring as the scroll — see NV_SRC_IN_MS.
  if(fx) { int i;
    for(i=0;i<n;i++) {
      if(fx[i].in<1.0f) fx[i].in=anim_ramp(fx[i].in,1.0f,dt,NV_SRC_IN_MS);
      if(fx[i].dy!=0.0f) {
        fx[i].dy=anim_spring(fx[i].dy,0.0f,dt,NV_SPRING_GRID);
        if(fabsf(fx[i].dy)<0.25f) fx[i].dy=0.0f;
      }
    } }
  skelA=anim_spring(skelA,is_open && addons_busy()?1.0f:0.0f,dt,NV_SPRING_SCREEN);
}
int stream_sheet_chose(int *out) {
  if(choice<0) return 0;
  if(out) *out=choice;
  choice=-1;return 1;
}

// --- THE ROW ------------------------------------------------------------------
//
// Three chips, then quiet text; the size and the quality bar on the right. The
// rules and the reasoning are in layout.h under "THE SOURCES SHEET"; what
// follows is only their arithmetic.

// CAPITALS CENTRED, not the line's box. A TxtLine is as tall as the FONT, so a
// 20px line carries about 6px of air over a capital and 5 under the baseline —
// centring the box inside a 38px chip therefore sits the word visibly low. See
// txt_cap_inset in text.h, which exists for exactly this.
static float capCentre(TxtStyle st, float top, float h) {
  float inset = txt_cap_inset(st), cap = txt_baseline(st) - inset;
  return top + (h - cap) * 0.5f - inset;
}

// ONE CHIP, and it is one of two things and never a third: FILLED — a white
// ground with dark ink, which 4K and only 4K gets — or OUTLINED, a ring with
// nothing inside it.
//
// The ring is GFX_RING_INSET and not a smaller rectangle painted in the
// background colour: the row behind it is transparent when unfocused and a pale
// wash when focused, so a painted "middle" would read as a light smudge on one
// row and match on the other. Inset rather than GFX_RING because that one
// strokes ACROSS the quad's edge, which would make a chip 2px wider than the
// width this returns and walk the whole row along.
static float chip(const char *label, float x, float y, int filled,
                  float cr, float cg, float cb, int ir, int ig, int ib, float a) {
  float lw = txt_tracking(TXT_SRC_CHIP, label, ir, ig, ib, -1, 0, 0, NV_SRC_CHIP_TRACK);
  float w = lw + 2 * NV_SRC_CHIP_PADX;
  GfxRect r = { x, y, w, NV_SRC_CHIP_H };
  float rad = NV_SRC_CHIP_R / NV_SRC_CHIP_H;
  if (filled) gfx_color(r, rad, cr, cg, cb, a);
  else gfx_rect(r, 0, GFX_RING_INSET, 0, NV_SRC_CHIP_RING / NV_SRC_CHIP_H, 0,
                rad, cr, cg, cb, a);
  txt_tracking(TXT_SRC_CHIP, label, ir, ig, ib, x + NV_SRC_CHIP_PADX,
               capCentre(TXT_SRC_CHIP, y, NV_SRC_CHIP_H), a, NV_SRC_CHIP_TRACK);
  return w + NV_SRC_CHIP_GAP;
}

// THE QUALITY BAR. Four segments, filled up to the tier — ordinal by
// construction, the way a signal-strength meter reads. It replaces both the row
// of stars the aggregators send and the "Tier" word the sheet used to print:
// neither of those says which end is good.
static void segments(float x, float y, int tier, float a) {
  int i;
  for (i = 0; i < NV_SRC_SEG_N; i++)
    gfx_color((GfxRect){ x + i * (NV_SRC_SEG_W + NV_SRC_SEG_GAP), y,
                         NV_SRC_SEG_W, NV_SRC_SEG_H },
              0.5f, 1, 1, 1, (i <= tier ? 0.82f : 0.16f) * a);
}

// EVERY WORD IN A ROW GOES DOWN TWICE: a black copy NV_SRC_SHADOW lower, then
// the ink. Not the chips — those sit on a ground of their own, and a shadow
// under the filled 4K one reads as a smudge rather than a lift.
static void ink(TxtLine l, float x, float y, float a) {
  txt_draw_shadow(l, x, y + NV_SRC_SHADOW, a * NV_SRC_SHADOW_A);
  txt_draw_alpha(l, x, y, a);
}

// THE EQUALISER, for the row that is playing. Bottom-aligned on the text's own
// baseline, because the bars are read as sitting ON the line the words sit on —
// centred on the line box they float, and the shortest of them floats worst.
//
// The three periods are deliberately not multiples of one another: at any simple
// ratio the bars return to the same arrangement every cycle and the eye starts
// reading the pattern rather than the movement.
static void equaliser(float x, float base, Uint32 now, float a) {
  static const float SPEED[3] = { 0.0091f, 0.0067f, 0.0113f };  // radians per ms
  static const float PHASE[3] = { 0.0f, 2.1f, 4.2f };
  int i;
  for (i = 0; i < 3; i++) {
    float s = 0.5f + 0.5f * sinf((float)now * SPEED[i] + PHASE[i]);
    float h = NV_SRC_EQ_H * (NV_SRC_EQ_MIN + (1.0f - NV_SRC_EQ_MIN) * s);
    gfx_color((GfxRect){ x + i * (NV_SRC_EQ_W + NV_SRC_EQ_GAP), base - h,
                         NV_SRC_EQ_W, h },
              NV_SRC_EQ_R / h, 1, 1, 1, a);
  }
}
#define SRC_EQ_TOTAL (3 * NV_SRC_EQ_W + 2 * NV_SRC_EQ_GAP)

static const char *TIER_WORD[4] = { "POOR", "FAIR", "GOOD", "BEST" };

// THE TIER'S COLOUR. Sampled off the design's own render, and undimmed: three of
// its four rows are unfocused there, so the values on screen are the ink at
// NV_SRC_DIM over the sheet — BEST reads (126,208,158) at full strength and GOOD
// reads (89,104,67), which is (179,209,135) halved.
//
// A RAMP AND NOT FOUR LABELS: green to lime to amber to coral is the same
// direction the bar already runs in, so the two say one thing twice rather than
// two things. That redundancy is the point on a screen read from three metres —
// the word survives when the four little segments are too small to count, and the
// bar survives for anyone who cannot separate the amber from the coral.
//
// POOR is the one the design does not show; coral continues the ramp without
// becoming a warning. The row is still a source somebody may want, not an error.
static const int TIER_INK[4][3] = {
  { 224, 132, 120 },   // POOR
  { 225, 185, 115 },   // FAIR
  { 179, 209, 135 },   // GOOD
  { 126, 208, 158 },   // BEST
};

// "INSTANT" IS NOT GREEN. It was BEST's green, and next to the tier word it
// made two colour signals per row where the design has one. It is a neutral one
// step BRIGHTER than the rest of its line instead: the lead fact, not a badge.
// One item of the second line, with the dot that separates it from the one
// before. Returns the new cursor.
static float metaItemIn(TxtStyle st, const char *text, float x, float y, int first,
                        int r, int g, int b, int dim, float a) {
  if (!text || !text[0]) return x;
  if (!first) {
    TxtLine d = txt_line(TXT_SRC_META, "\xC2\xB7", 255, 255, 255, dim);
    ink(d, x, y, a);
    x += (float)d.w + 10.0f;
  }
  { TxtLine l = txt_line(st, text, r, g, b, 255);
    ink(l, x, y, a);
    return x + (float)l.w + 10.0f; }
}
static float metaItem(const char *text, float x, float y, int first,
                      int r, int g, int b, int dim, float a) {
  return metaItemIn(TXT_SRC_META, text, x, y, first, r, g, b, dim, a);
}

// THE FOCUSED ROW'S TYPE IS WHITE, not the grey the others use. The band alone
// was carrying the state, and at 0.12 over a veil that no longer hides the
// picture it could not carry it far enough — from the sofa the row you were on
// looked like the rows you were not. Brightening the ink costs nothing and is
// the half of the pair that survives at distance.
static void drawRow(const Stream *s, float left, float w, float top, int playing,
                    int sel, Uint32 now, float a) {
  // Only the LEAD fact is bright on the focused row ("Instant", "Playing"); the
  // rest of the line and the audio/codec run stay grey and the dots darker still,
  // as the design sets them. All-white read as one undifferentiated sentence.
  const int metaR = sel ? 176 : 146, metaG = sel ? 178 : 149, metaB = sel ? 184 : 156;
  const int sep = sel ? 104 : 80;   // the separating dots follow the type
  const int readyR = sel ? 244 : 178;
  float x = left, y = top + NV_SRC_CHIP_Y;
  char buf[64];
  const char *container = containerOf(s);
  int first = 1;

  // --- the chips, left to right: resolution, dynamic range, source.
  if (s->res[0]) {
    int uhd = !strcmp(s->res, "4K");
    x += uhd ? chip(s->res, x, y, 1, 0.97f, 0.97f, 0.98f, 18, 18, 20, a)
             : chip(s->res, x, y, 0, 1, 1, 1, 222, 224, 228, a);
  }
  // Dolby Vision used to carry a gold outline as the row's one tint. It is white
  // like every other chip now: the row already spends colour on the tier word and
  // on the Instant mark, and a third accent left nothing quiet enough for any of
  // them to read as a signal. The chip still says DV; it no longer shouts it.
  if (s->range[0]) x += chip(s->range, x, y, 0, 1, 1, 1, 220, 222, 228, a);
  if (s->source[0]) x += chip(s->source, x, y, 0, 1, 1, 1, 226, 228, 232, a);

  // --- audio and codec: BARE TEXT, never a chip. They are what you read after
  // the question the chips answered, and giving them a shape of their own is
  // what made the old row a wall of six logotypes at five sizes.
  // They are separated EXACTLY as the line below is — metaItem's dot and its 10px
  // either side — and not by "  ·  " in one string: two spaces are narrower than
  // those gaps, and the two lines then kept two different rhythms.
  { const char *part[2] = { s->audio, s->codec };
    const int tr = sel ? 184 : 152, tg = sel ? 186 : 155, tb = sel ? 192 : 162;
    int i, lead = 1;
    // `x` already carries one NV_SRC_CHIP_GAP past the last chip, which is the
    // same gap the chips keep between themselves. Nothing is added to it.
    for (i = 0; i < 2; i++) {
      if (!part[i][0]) continue;
      if (!lead) {
        TxtLine d = txt_line(TXT_SRC_TEXT, "\xC2\xB7", 255, 255, 255, sep);
        ink(d, x, capCentre(TXT_SRC_TEXT, y, NV_SRC_CHIP_H), a);
        x += (float)d.w + 10.0f;
      }
      { float ty = capCentre(TXT_SRC_TEXT, y, NV_SRC_CHIP_H);
        float tw = txt_tracking(TXT_SRC_TEXT, part[i], tr, tg, tb, -1, 0, 0,
                                NV_SRC_TEXT_TRACK);
        // Tracked text cannot be trimmed, so a run that would reach the size
        // column is left out whole rather than cut mid-word.
        if (x + tw > left + w - NV_SRC_SIZE_COL) break;
        txt_tracking(TXT_SRC_TEXT, part[i], 0, 0, 0, x, ty + NV_SRC_SHADOW,
                     a * NV_SRC_SHADOW_A, NV_SRC_TEXT_TRACK);
        txt_tracking(TXT_SRC_TEXT, part[i], tr, tg, tb, x, ty, a, NV_SRC_TEXT_TRACK);
        x += tw + 10.0f; }
      lead = 0;
    } }

  // --- the second line: how it plays, where it came from, and how fat it is.
  x = left; y = top + NV_SRC_META_Y;
  // PLAYING REPLACES THE AVAILABILITY MARK, it does not sit beside it. Whether
  // this source is instant stopped mattering the moment it became the one on
  // screen, and the row has no room to say both.
  if (playing) {
    equaliser(x, y + txt_baseline(TXT_SRC_META), now, a);
    x += SRC_EQ_TOTAL + 9.0f;
    x = metaItemIn(TXT_SRC_STATE, "Playing", x, y, 1, 244, 245, 248, sep, a);
    first = 0;
  } else if (s->cached) {
    // The bolt is a PNG, not a character: Inter has no U+26A1 and SDL_ttf draws
    // .notdef without complaining, which is the hollow box this sheet used to
    // show wherever an addon's own emoji reached the screen.
    gfx_icon((GfxRect){ x, y + 1.0f, NV_SRC_BOLT, NV_SRC_BOLT }, "instant",
             readyR / 255.0f, readyR / 255.0f, readyR / 255.0f, a);
    x += NV_SRC_BOLT + 7.0f;
    x = metaItem("Instant", x, y, 1, readyR, readyR, readyR + 4, sep, a);
    first = 0;
  } else if (s->p2p) {
    // P2P is the exception, so it gets a mark; "not cached" is not a state worth
    // a word of its own on every other row.
    TxtLine l = txt_line(TXT_SRC_TIER, "P2P", sel ? 218 : 156,
                         sel ? 220 : 158, sel ? 226 : 164, 255);
    float pw = (float)l.w + 20.0f, ph = 26.0f;
    if (!first) x += 10.0f;
    gfx_color((GfxRect){ x, y + 1.0f, pw, ph }, 7.0f / ph, 1, 1, 1, 0.10f * a);
    txt_draw_alpha(l, x + 10.0f, capCentre(TXT_SRC_TIER, y + 1.0f, ph), a);
    x += pw + 10.0f;
    first = 1;   // the chip carries its own separation
  }
  x = metaItem(s->service[0] ? s->service : s->provider, x, y, first,
                metaR, metaG, metaB, sep, a); first = 0;
  x = metaItem(container, x, y, 0, metaR, metaG, metaB, sep, a);
  // THE SEED COUNT ONLY ON A TORRENT. On an instant row it is a number the
  // aggregator scraped from somewhere and it changes nothing about whether the
  // file plays; printing it on every row is what taught the eye to skip it.
  if (s->p2p && s->seeders > 0) {
    snprintf(buf, sizeof buf, "\xE2\x86\x91 %d", s->seeders);
    x = metaItem(buf, x, y, 0, metaR, metaG, metaB, sep, a);
  }
  if (s->mbps > 0.05f) {
    snprintf(buf, sizeof buf, "%.1f Mbps", (double)s->mbps);
    x = metaItem(buf, x, y, 0, metaR, metaG, metaB, sep, a);
  }

  // --- the right column: size over the quality bar, both flush right.
  if (s->sizeMB > 0) {
    TxtLine l;
    if (s->sizeMB >= 1024) snprintf(buf, sizeof buf, "%.1f GB", s->sizeMB / 1024.0);
    else                   snprintf(buf, sizeof buf, "%ld MB", s->sizeMB);
    l = txt_line(TXT_SRC_SIZE, buf, 244, 245, 248, 255);
    ink(l, left + w - (float)l.w, top + NV_SRC_SIZE_Y, a);
  }
  { const char *word = TIER_WORD[s->tier & 3];
    const int *tint = TIER_INK[s->tier & 3];
    float tw = txt_tracking(TXT_SRC_TIER, word, tint[0], tint[1], tint[2],
                            -1, 0, 0, NV_SRC_TIER_TRACK);
    float bw = NV_SRC_SEG_N * NV_SRC_SEG_W + (NV_SRC_SEG_N - 1) * NV_SRC_SEG_GAP;
    float wordX = left + w - tw;
    float wordY = capCentre(TXT_SRC_TIER, top + NV_SRC_META_Y - 6.0f, 26.0f);
    // txt_tracking rasterises per COLOUR, so the black twin is a second cache
    // entry — but there are only four tier words, so that is four in total.
    txt_tracking(TXT_SRC_TIER, word, 0, 0, 0,
                 wordX, wordY + NV_SRC_SHADOW, a * NV_SRC_SHADOW_A,
                 NV_SRC_TIER_TRACK);
    txt_tracking(TXT_SRC_TIER, word, tint[0], tint[1], tint[2],
                 wordX, wordY, a, NV_SRC_TIER_TRACK);
    // THE BAR STAYS WHITE. It is the redundant half of the pair, and tinting it
    // too would make the row's loudest mark its quality rather than its
    // resolution — the 4K chip is the one thing that may shout here.
    segments(wordX - 14.0f - bw, top + NV_SRC_META_Y + 5.0f, s->tier, a); }
}

// --- THE TABS -----------------------------------------------------------------
//
// tabs.c's words and underline. The underlined word is the filter; it lights up
// white while the cursor is on the strip and greys back when it goes down to the
// list — see tab_draw.
static void tabs(float left, float w, float y, float lit, float a) {
  float widths[13], x;
  int i, from = 0;
  for (i = 0; i < nProviders; i++) widths[i] = tab_width(providers[i]);
  // Scroll only as far as it takes to bring the active tab into view: the strip
  // keeps its left edge whenever it fits, which is the common case.
  { float upTo = 0;
    for (i = 0; i <= filter && i < nProviders; i++) upTo += widths[i];
    while (upTo > w && from < filter) { upTo -= widths[from]; from++; } }
  x = left;
  for (i = from; i < nProviders; i++) {
    if (x + widths[i] - NV_TAB_GAP > left + w) break;
    pointer_zone_click(x, y, widths[i] - NV_TAB_GAP, NV_TAB_H, clickTab, i, 0);
    x += tab_draw(x, y, providers[i], i == filter, lit, a);
  }
}

// ONE ROW: its card (or band) and its content, with its top edge at `y`.
static void sheetRow(int row, float y, float cx, float cw, Uint32 now) {
  int i=filtered(row), sel=group==1 && focus==row;
  // The row's own box, softened over NV_SRC_BAND_LEAD at its left end — see the
  // note in layout.h. It stops next to the first chip and never reaches the
  // picture.
#if NV_SRC_CARDS
  { GfxRect card={cx,y,cw,NV_SRC_CARD_H};
    float rad=NV_SRC_CARD_R/NV_SRC_CARD_H;
    gfx_color(card,rad,NV_SRC_INK_R,NV_SRC_INK_G,NV_SRC_INK_B,NV_SRC_CARD_BASE*anim);
    gfx_color(card,rad,1,1,1,(i==current?NV_SRC_CARD_PLAYING:
                              sel?NV_SRC_CARD_FOCUS:NV_SRC_CARD_FILL)*anim);
    if(sel) gfx_rect(card,0,GFX_RING_INSET,0,NV_SRC_CARD_RING/NV_SRC_CARD_H,0,
                     rad,1,1,1,0.95f*anim); }
  drawRow(&list[i],cx+NV_SRC_CARD_PADX,cw-2*NV_SRC_CARD_PADX,
          y+(NV_SRC_CARD_H-NV_SRC_ROW_H)*0.5f,i==current,sel,now,
          anim*(sel||i==current?1.0f:NV_SRC_DIM));
#else
  if(sel) gfx_rect((GfxRect){cx-NV_SRC_BAND_LEAD,y,NV_SRC_BAND_W,NV_SRC_ROW_H},
                   0,GFX_MENU_FEATHER,0,NV_SRC_BAND_FADE/NV_SRC_BAND_W,0,0,
                   1,1,1,NV_SRC_FOCUS_FILL*anim);
  drawRow(&list[i],cx,cw,y,i==current,sel,now,anim*(sel?1.0f:NV_SRC_DIM));
#endif
}

// THE PLACEHOLDER: one card's ground with grey bars where the chips, the line
// under them and the size go, breathing slowly, one slot past the last row while
// the slow addons are still out. It says "more are coming" in the shape they will
// take, at the place they will land. It comes in with the last arrival rather
// than a frame ahead of it, and an empty list has the sentence instead.
static void placeholder(int nf, float cx, float cw, float fadeTop, Uint32 now) {
  static const float CHIP_W[3]={58.0f,50.0f,88.0f};
  float y=NV_SRC_TOP+nf*NV_SRC_ROW-scroll, a, x, p, ba;
  float left=cx+NV_SRC_CARD_PADX, right=cx+cw-NV_SRC_CARD_PADX;
  float top=y+(NV_SRC_CARD_H-NV_SRC_ROW_H)*0.5f;
  GfxRect card={cx,y,cw,NV_SRC_CARD_H};
  float rad=NV_SRC_CARD_R/NV_SRC_CARD_H;
  int k;
  if(!nf || skelA<0.004f || y>NV_SCREEN_H) return;
  a=skelA*anim*anim_edge(y,fadeTop,NV_SRC_TOP-fadeTop)*fxIn(filtered(nf-1));
  if(a<=0.004f) return;
  p=0.5f+0.5f*sinf((float)now*0.0042f);
  ba=a*(0.06f+0.05f*p);
  gfx_color(card,rad,NV_SRC_INK_R,NV_SRC_INK_G,NV_SRC_INK_B,NV_SRC_CARD_BASE*a);
  gfx_color(card,rad,1,1,1,NV_SRC_CARD_FILL*NV_SRC_DIM*a);
  for(x=left,k=0;k<3;k++) {
    gfx_color((GfxRect){x,top+NV_SRC_CHIP_Y,CHIP_W[k],NV_SRC_CHIP_H},
              NV_SRC_CHIP_R/NV_SRC_CHIP_H,1,1,1,ba);
    x+=CHIP_W[k]+NV_SRC_CHIP_GAP;
  }
  gfx_color((GfxRect){left,top+NV_SRC_META_Y+4.0f,240.0f,16.0f},0.5f,1,1,1,ba);
  gfx_color((GfxRect){right-96.0f,top+NV_SRC_SIZE_Y+4.0f,96.0f,24.0f},
            8.0f/24.0f,1,1,1,ba);
}

// Three dots after the count, lit in turn, while the addons are still answering:
// the rows in front of you are not the whole list yet. The empty sheet counts
// its own dots in the sentence, so these wait for the first row.
static void searchingDots(float x, float midY, Uint32 now) {
  int i;
  if(!n || skelA<0.004f) return;
  for(i=0;i<3;i++) {
    float p=0.5f+0.5f*sinf((float)now*0.0065f-i*0.9f);
    gfx_color((GfxRect){x+i*13.0f,midY-3.5f,7.0f,7.0f},0.5f,
              1,1,1,(0.22f+0.55f*p)*skelA*anim);
  }
}

void stream_sheet_draw(Uint32 now) {
  if(anim<.005f) return;
  // The short slide the track menus use — see the update.
  float slide=(1-anim)*NV_SRC_VEIL_W*NV_TRK_SLIDE;
  float x=NV_SCREEN_W-NV_SRC_VEIL_W+slide;
  float cx=NV_SCREEN_W-NV_SRC_PAD-NV_SRC_CONTENT_W+slide, cw=NV_SRC_CONTENT_W;
  int nf, row;
  char count[192];
  // ONE QUAD, ONE RAMP, AND NO FULL-SCREEN SCRIM BEHIND IT.
  //
  // There used to be a flat 0.35 black over the whole screen under this. A
  // uniform veil dims the backdrop equally everywhere, so it contributes no
  // gradient of its own and does nothing the ramp is not already doing — it only
  // costs a second full screen of fill and flattens the hero the design shows at
  // full strength on the left. The ramp is the entire treatment.
  gfx_rect((GfxRect){x,0,NV_SRC_VEIL_W,NV_SCREEN_H},0,GFX_SRC_VEIL,0,
           1,0,0,NV_SRC_INK_R,NV_SRC_INK_G,NV_SRC_INK_B,anim*NV_SRC_VEIL_A);
  // Off the sheet, a click closes it as Back does; the column itself is inert.
  if(is_open) {
    pointer_zone_click(0,0,NV_SCREEN_W,NV_SCREEN_H,pointOff,0,0);
    pointer_zone_hover(cx-NV_SRC_PAD,0,NV_SCREEN_W-cx+NV_SRC_PAD,NV_SCREEN_H,pointPanel,0,0);
  }

  // --- the heading, with the count on its baseline and the episode after it.
  txt_draw_alpha(txt_line(TXT_PANEL_TITLE,"Sources",240,241,243,255),cx,NV_SRC_TITLE_Y,anim);
  { TxtLine title=txt_line(TXT_PANEL_TITLE,"Sources",240,241,243,255);
    // CENTRED ON THE HEADING'S CAPITALS, not sat on its baseline. Sharing a
    // baseline is right for two runs of the same sentence; here the count is a
    // separate object beside a 40px word, and hung off the baseline it read as
    // having slipped down. Centring uses the CAP box of each and not the line
    // box, for the reason txt_cap_inset exists: a line is as tall as the font, so
    // two of different sizes centred box-to-box are not optically centred at all.
    float capT=txt_cap_inset(TXT_PANEL_TITLE), capC=txt_cap_inset(TXT_SRC_COUNT);
    float midT=NV_SRC_TITLE_Y+(capT+txt_baseline(TXT_PANEL_TITLE))*0.5f;
    float by=midT-(capC+txt_baseline(TXT_SRC_COUNT))*0.5f;
    // The count ONLY. The episode used to follow it when the sheet was opened
    // from the player, and it is already on screen in the player behind.
    snprintf(count,sizeof count,"%d found",n);
    { TxtLine c=txt_line_trim(TXT_SRC_COUNT,count,132,135,142,255,cw-(float)title.w-340.0f);
      txt_draw_alpha(c,cx+(float)title.w+18.0f,by,anim);
      searchingDots(cx+(float)title.w+18.0f+(float)c.w+12.0f,
                    by+(capC+txt_baseline(TXT_SRC_COUNT))*0.5f,now); } }

  // Reload and Close: TWO ICONS IN ONE PILL, as the design draws them. The pill
  // is a faint fill and a hairline, so at rest it reads as one quiet control;
  // the cursor on either icon puts a white disc behind it with the glyph in ink.
  { float h=NV_SRC_HEAD_H, w=2*NV_SRC_HEAD_BTN+2*NV_SRC_HEAD_INSET;
    float px=cx+cw-w, py=NV_SRC_TITLE_Y+2.0f+22.0f-h*0.5f;
    GfxRect pill={px,py,w,h};
    int i;
    gfx_color(pill,0.5f,1,1,1,0.05f*anim);
    gfx_rect(pill,0,GFX_RING_INSET,0,1.5f/h,0,0.5f,1,1,1,0.10f*anim);
    for(i=0;i<2;i++) {
      int sel=group==-1 && focus==i;
      float bx=px+NV_SRC_HEAD_INSET+i*NV_SRC_HEAD_BTN, by=py+(h-NV_SRC_HEAD_BTN)*0.5f;
      float ic=NV_SRC_HEAD_ICON, c=sel?0.09f:0.86f;
      pointer_zone(bx,by,NV_SRC_HEAD_BTN,NV_SRC_HEAD_BTN,pointHead,i,0);
      if(sel) gfx_color((GfxRect){bx,by,NV_SRC_HEAD_BTN,NV_SRC_HEAD_BTN},0.5f,
                        .94f,.94f,.95f,anim);
      gfx_icon((GfxRect){bx+(NV_SRC_HEAD_BTN-ic)*0.5f,by+(NV_SRC_HEAD_BTN-ic)*0.5f,ic,ic},
               i?"close":"reload",c,c,c+(sel?0.01f:0.02f),anim);
      // THE TOOLTIP, under the icon rather than over it: above there is only the
      // top margin. It is centred on the button, but never allowed past the
      // pill's right edge, or "Close" would run off towards the screen's.
      if(tipA[i]>0.01f) {
        const char *tip=i?"Close":"Reload";
        float al=anim*tipA[i];
        TxtLine t=txt_line(TXT_SRC_STATE,tip,244,245,248,255);
        TxtLine d=txt_line(TXT_SRC_STATE,tip,0,0,0,255);
        float tx=bx+(NV_SRC_HEAD_BTN-(float)t.w)*0.5f;
        float ty=py+h+NV_SRC_TIP_GAP-(1.0f-tipA[i])*NV_SRC_TIP_RISE;
        if(tx+(float)t.w>px+w) tx=px+w-(float)t.w;
        txt_draw_alpha(d,tx,ty+2.0f,al*0.80f);
        txt_draw_alpha(d,tx-1.0f,ty+3.0f,al*0.40f);
        txt_draw_alpha(d,tx+1.0f,ty+3.0f,al*0.40f);
        txt_draw_alpha(t,tx,ty,al);
      }
    } }

  gfx_crop(cx,NV_SRC_TABS_Y,cw,NV_TAB_H);
  tabs(cx,cw,NV_SRC_TABS_Y,tabsLit,anim);
  gfx_no_crop();

  // ROWS DISSOLVE AS THEY LEAVE THE TOP, the way Discover's grid and the episode
  // list do (anim_edge): across the air between the tabs' focus ring and the first
  // row, a row going up fades to nothing, so it is gone before the clip would cut it
  // against the tabs. The clip therefore starts at the tabs' base, not at the list.
  const float fadeTop=NV_SRC_TABS_Y+NV_TAB_H;
  // It runs to the SCREEN's bottom edge, not NV_SRC_FOOT above it. Clipped there,
  // the card under the last whole one was cut off 32px short of the edge with
  // nothing below it — a line drawn by the layout's padding, not by anything on
  // screen. NV_SRC_FOOT stays in the scroll's arithmetic, so the LAST card still
  // ends with that air under it.
  gfx_crop(x,fadeTop,NV_SRC_VEIL_W,NV_SCREEN_H-fadeTop);
  pointer_clip(x,fadeTop,NV_SRC_VEIL_W,NV_SCREEN_H-fadeTop);
  nf=nFiltered();
  // THE PLAYING CARD STICKS UNDER THE TABS. Once the list has scrolled it past the
  // first row's place it stays there, so what is on screen is always in view
  // while you look for something else; the rows going up dissolve into the air
  // under it instead of under the tabs.
  { int pin=-1;
    const float pinBottom=NV_SRC_TOP+NV_SRC_CARD_H;
#if NV_SRC_CARDS
    for(row=0;row<nf;row++)
      if(filtered(row)==current) { if(row*NV_SRC_ROW<scroll) pin=row; break; }
#endif
    for(row=0;row<nf;row++) {
      int i=filtered(row);
      float y=NV_SRC_TOP+row*NV_SRC_ROW-scroll+(fx?fx[i].dy:0.0f), in=fxIn(i);
      float edge=pin>=0 ? anim_edge(y,pinBottom,NV_SRC_TOP+NV_SRC_ROW-pinBottom)
                        : anim_edge(y,fadeTop,NV_SRC_TOP-fadeTop);
      if(row==pin) continue;
      if(y+NV_SRC_ROW<fadeTop || y>NV_SCREEN_H) continue;
      if(edge<=0.004f || in<=0.004f) continue;
      gfx_opacity_group=edge*in;
      // The rise is drawing only: the pointer's zone stays on the row's slot.
      sheetRow(row,y+(1.0f-in)*NV_SRC_IN_RISE,cx,cw,now);
      pointer_zone(cx,y,cw,NV_SRC_ROW,pointRow,row,0);
      // PUT IT BACK before anything else is drawn: a group opacity left set bleeds
      // onto every later draw call in the frame.
      gfx_opacity_group=1.0f;
    }
    if(pin>=0) {
      sheetRow(pin,NV_SRC_TOP,cx,cw,now);
      pointer_zone(cx,NV_SRC_TOP,cw,NV_SRC_ROW,pointRow,pin,0);
    } }
  placeholder(nf,cx,cw,fadeTop,now);
  if(!nf) {
    // While the addons answer, the dots count 1, 2, 3 and round again, so a wait
    // reads as work in progress and not as a frozen sheet. The text is
    // left-aligned, so only the dots move and the words stay put.
    static const char *const FETCHING[3]={
      "Fetching sources from the addons.",
      "Fetching sources from the addons..",
      "Fetching sources from the addons..."};
    const char *msg=addons_busy()?FETCHING[(now/450u)%3u]:"No direct source available. Use Reload to try again.";
    txt_block(TXT_SRCH_EMPTY,msg,166,169,176,cx,NV_SRC_TOP+28.0f,cw,34.0f,anim,3);
  }
  gfx_no_crop();
  pointer_no_clip();
}
