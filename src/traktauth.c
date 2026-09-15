#include "traktauth.h"
#include "cloud.h"
#include "data.h"
#include "net.h"
#include "trakt.h"
#include "sync.h"
#include "discover.h"
#include "js.h"
#include "jsw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#define TRA_FILE  "trakt.txt"
#define TRA_STREAM "trakt-flow.txt"
#define TRA_BASE "https://api.trakt.tv"
// When Trakt does not send `interval`, 5s is what its documentation suggests.
#define TRA_POLL_DEFAULT 5000u

static TraState state = TRA_STOPPED;
static char deviceCode[128];
static char userCode[32];
static char url[160];
static char error[200];
static char token[300], refresh[300];
static unsigned pollMs = TRA_POLL_DEFAULT;
static unsigned nextPoll, beganMs, limitMs;
// A deadline in WALL CLOCK, not in ticks: the request has to survive an app
// restart, and SDL_GetTicks resets along with the process.
static long expiresIn;
// The ACCESS TOKEN's lifetime, which is a different clock from expiresIn above —
// that one is the pairing code's deadline. Trakt sends 90 days here.
static long tokenLifetime;
// WHEN the access token was issued (Trakt's `created_at`, in seconds). Together
// with tokenLifetime it is the only way to know, without asking, that the token
// on disk is already dead.
static long createdAt;
// 1 when the load found an access token expired ON PAPER but a refresh token
// still in hand: the renewal goes out on the first step, rather than spending
// the whole first discovery cycle collecting 401s with a dead credential.
static int renewPending;

static pthread_t thread;
static int threadAlive, threadReady;
// 1 when the thread has just obtained the token and the main loop has not
// applied it yet. Applying it inside the thread would touch trakt.c while the
// UI is reading from it.
static int tokenNew;

static char *post(const char *path, const char *body, int *status) {
  char complete[300];
  const char *header[2];
  snprintf(complete, sizeof complete, "%s%s", TRA_BASE, path);
  // Trakt requires the API version header; without it it answers 412.
  header[0] = "trakt-api-version: 2";
  header[1] = NULL;
  return net_post_st(complete, 20, header, body, status);
}

// ---------------------------------------------------------------- disk

// THE PENDING REQUEST goes to disk. Without this the device code lived only in
// memory: the app restarting — or the deploy itself — was enough for the
// authorisation done on the phone to have nobody left asking about it. That is
// exactly what happened: the owner authorised and the app "did not update",
// because the instance that had asked for the code no longer existed. The web
// app stores the same state (TraktAuthStore.saveDeviceFlow).
static void writeStream(void) {
  char buf[600];
  snprintf(buf, sizeof buf, "%s\t%s\t%s\t%ld\n", deviceCode, userCode, url, expiresIn);
  data_write(TRA_STREAM, buf);
}

static void forgetStream(void) {
  deviceCode[0] = userCode[0] = 0;
  expiresIn = 0;
  data_erase(TRA_STREAM);
}

static void save(void) {
  char buf[800];
  // Starts with the old art/trakt.txt shape ("token<TAB>clientId"), so the file
  // stays readable to anyone who knew that one; the difference is the PLACE,
  // this being the installation folder rather than the package.
  //
  // THE THREE COLUMNS AFTER IT ARE WHAT MAKES RENEWAL POSSIBLE AT ALL. The
  // refresh token was being fetched and pushed to the account and then dropped
  // on the floor here, so a restart lost it: this installation had no way to
  // renew, and when the access token expired the screen went on saying
  // "connected" while every call came back 401. `createdAt` + `expiresIn` are
  // what let the load tell a live token from a dead one without asking.
  snprintf(buf, sizeof buf, "%s\t%s\t%s\t%ld\t%ld\n", token,
           cloud_trakt_client(), refresh, createdAt, tokenLifetime);
  data_write(TRA_FILE, buf);
}

int traktauth_load(void) {
  char *b = data_read(TRA_FILE);
  if (!b) return 0;
  // Up to five tab-separated columns: token, clientId, refresh, createdAt,
  // expiresIn. A TWO-COLUMN FILE STILL LOADS — that is what every install
  // written before the renewal existed has — and simply reports no refresh and
  // no deadline, which the guard below reads as "cannot tell, use it".
  { char *col[5] = {0}, *p = b;
    int k = 0;
    col[k++] = p;
    while (k < 5 && (p = strchr(p, '\t')) != NULL) { *p++ = 0; col[k++] = p; }
    { char *end = col[k - 1] + strlen(col[k - 1]);
      while (end > col[k - 1] && (end[-1] == '\n' || end[-1] == '\r')) *--end = 0; }
    if (col[0] && col[0][0]) snprintf(token, sizeof token, "%s", col[0]);
    if (col[2] && col[2][0]) snprintf(refresh, sizeof refresh, "%s", col[2]);
    if (col[3]) createdAt = atol(col[3]);
    if (col[4]) tokenLifetime = atol(col[4]);
  }
  if (token[0]) {
    // EXPIRED ON PAPER: createdAt + expiresIn has already passed. Handing this
    // token to trakt.c would send every call of the first discovery cycle out to
    // the network to come back 401 — seconds each, in the same pool the rest of
    // the home is waiting on. With a refresh in hand the renewal resolves it
    // first; the step applies the new token and asks for the rows again.
    //
    // Five minutes of margin, because a token that expires during the cycle is
    // the same problem arriving slightly later.
    if (refresh[0] && createdAt > 0 && tokenLifetime > 0 &&
        (long)time(NULL) >= createdAt + tokenLifetime - 300) {
      renewPending = 1;
      printf("[trakt] token expired in the file — renewing before the 401\n");
    } else {
      trakt_set(token, cloud_trakt_client());
    }
    state = TRA_ON;
  }
  free(b);
  if (token[0]) return 1;

  // No token, but there may be a request in flight from before the restart.
  { char *f = data_read(TRA_STREAM);
    if (f) {
      char *c[4] = { f, NULL, NULL, NULL };
      int i;
      for (i = 1; i < 4 && c[i - 1]; i++) {
        c[i] = strchr(c[i - 1], '\t');
        if (c[i]) *c[i]++ = 0;
      }
      { char *end = f + strlen(f);
        while (end > f && (end[-1] == '\n' || end[-1] == '\r')) *--end = 0; }
      if (c[3]) {
        long now = (long)time(NULL);
        long ate = atol(c[3]);
        if (ate > now + 5) {
          snprintf(deviceCode, sizeof deviceCode, "%s", c[0]);
          snprintf(userCode, sizeof userCode, "%s", c[1]);
          snprintf(url, sizeof url, "%s", c[2]);
          expiresIn = ate;
          state = TRA_WAITING;
          printf("[trakt] resuming the pending request (%lds left)\n", ate - now);
        } else {
          data_erase(TRA_STREAM);
        }
      }
      free(f);
    } }
  return 0;
}

void traktauth_forget(void) {
  renewPending = 0;
  createdAt = 0;
  token[0] = refresh[0] = url[0] = error[0] = 0;
  tokenLifetime = 0;
  state = TRA_STOPPED;
  data_erase(TRA_FILE);
  forgetStream();
}

// ---------------------------------------------------------------- fluxo

static void *threadRequest(void *u) {
  Jsw w;
  char *r;
  int st = 0;
  (void)u;
  error[0] = userCode[0] = deviceCode[0] = 0;

  if (!cloud_trakt_client()[0] || !cloud_trakt_secret()[0]) {
    // A BUILD case, not a user one: the package shipped without the
    // application's keys. Saying so stops the person retrying forever.
    snprintf(error, sizeof error, "package has no Trakt keys");
    state = TRA_ERROR;
    threadReady = 1;
    return NULL;
  }

  jsw_start(&w);
  jsw_obj_start(&w);
  jsw_cs(&w, "client_id", cloud_trakt_client());
  jsw_obj_end(&w);
  r = post("/oauth/device/code", jsw_text_final(&w), &st);
  jsw_free(&w);

  if (r && st >= 200 && st < 300) {
    const char *end = r + strlen(r);
    double interval, expires;
    js_text(r, end, "device_code", deviceCode, sizeof deviceCode);
    js_text(r, end, "user_code", userCode, sizeof userCode);
    js_text(r, end, "verification_url", url, sizeof url);
    interval = js_num(r, end, "interval", 0);
    expires = js_num(r, end, "expires_in", 0);
    if (interval >= 1.0 && interval <= 60.0) pollMs = (unsigned)(interval * 1000.0);
    limitMs = (expires > 30.0 && expires < 3600.0) ? (unsigned)(expires * 1000.0) : 600000u;
  }
  if (!deviceCode[0] || !userCode[0]) {
    if (st == 429) snprintf(error, sizeof error, "Trakt asked us to wait; try again shortly");
    else snprintf(error, sizeof error, "could not request the code from Trakt (HTTP %d)", st);
    state = TRA_ERROR;
  } else {
    if (!url[0]) snprintf(url, sizeof url, "https://trakt.tv/activate");
    expiresIn = (long)time(NULL) + (long)(limitMs / 1000u);
    writeStream();
    state = TRA_WAITING;
  }
  free(r);
  threadReady = 1;
  return NULL;
}

static void *threadPoll(void *u) {
  Jsw w;
  char *r;
  int st = 0;
  (void)u;

  jsw_start(&w);
  jsw_obj_start(&w);
  jsw_cs(&w, "code", deviceCode);
  jsw_cs(&w, "client_id", cloud_trakt_client());
  jsw_cs(&w, "client_secret", cloud_trakt_secret());
  jsw_obj_end(&w);
  r = post("/oauth/device/token", jsw_text_final(&w), &st);
  jsw_free(&w);

  if (r && st >= 200 && st < 300) {
    char t[300];
    if (js_text(r, r + strlen(r), "access_token", t, sizeof t)) {
      snprintf(token, sizeof token, "%s", t);
      // The refresh token goes to the account alongside it: without it the link
      // dies the day the access token expires and the web app would have no way
      // to renew it.
      if (!js_text(r, r + strlen(r), "refresh_token", refresh, sizeof refresh))
        refresh[0] = 0;
      // The account stores a lifetime, so it has to come from the answer. It was
      // never read before because the old push sent an opaque credential_json
      // that had no room for it.
      tokenLifetime = (long)js_num(r, r + strlen(r), "expires_in", 0.0);
      // `created_at` comes from Trakt; falling back to now is right, because
      // "now" is when we received it. Without this the deadline in the file is
      // unknowable and the load cannot tell a live token from a dead one.
      createdAt = (long)js_num(r, r + strlen(r), "created_at", (double)time(NULL));
      tokenNew = 1;
      forgetStream();
      state = TRA_ON;
    } else {
      snprintf(error, sizeof error, "Trakt answered without a token");
      state = TRA_ERROR;
    }
  } else if (st == 400) {
    /* not authorised yet: keep asking */
  } else if (st == 429) {
    // Trakt told us to slow down. Raise the interval, with a ceiling.
    pollMs += 5000u;
    if (pollMs > 60000u) pollMs = 60000u;
  } else if (st == 409) {
    snprintf(error, sizeof error, "this code has already been used");
    forgetStream();
    state = TRA_ERROR;
  } else if (st == 410) {
    snprintf(error, sizeof error, "the code expired");
    forgetStream();
    state = TRA_ERROR;
  } else if (st == 418) {
    snprintf(error, sizeof error, "authorisation denied by Trakt");
    forgetStream();
    state = TRA_ERROR;
  } else if (st) {
    snprintf(error, sizeof error, "failed to exchange the code (HTTP %d)", st);
    state = TRA_ERROR;
  }
  free(r);
  threadReady = 1;
  return NULL;
}

// RENEW FROM THE REFRESH TOKEN.
//
// The refresh has always been FETCHED (it is read out of the pairing answer and
// pushed to the account) and was never USED here: an expired access token stayed
// expired for ever, and the screen said "connected" while everything came back
// 401. Standard OAuth grant; Trakt answers with a NEW refresh token — it
// rotates — which travels on to the account with the rest.
static void *threadRenew(void *u) {
  Jsw w;
  char *r;
  int st = 0;
  (void)u;
  jsw_start(&w);
  jsw_obj_start(&w);
  jsw_cs(&w, "refresh_token", refresh);
  jsw_cs(&w, "client_id", cloud_trakt_client());
  jsw_cs(&w, "client_secret", cloud_trakt_secret());
  jsw_cs(&w, "redirect_uri", "urn:ietf:wg:oauth:2.0:oob");
  jsw_cs(&w, "grant_type", "refresh_token");
  jsw_obj_end(&w);
  r = post("/oauth/token", jsw_text_final(&w), &st);
  jsw_free(&w);

  if (r && st >= 200 && st < 300) {
    char t[300];
    const char *end = r + strlen(r);
    if (js_text(r, end, "access_token", t, sizeof t)) {
      snprintf(token, sizeof token, "%s", t);
      // THE REFRESH ROTATES: the old one dies with this answer, so failing to
      // read the new one has to clear the field rather than keep a dead value
      // that would be retried for ever.
      if (!js_text(r, end, "refresh_token", refresh, sizeof refresh))
        refresh[0] = 0;
      createdAt = (long)js_num(r, end, "created_at", (double)time(NULL));
      tokenLifetime = (long)js_num(r, end, "expires_in", 86400.0);
      tokenNew = 1;
      state = TRA_ON;
      printf("[trakt] credential renewed from the refresh token\n");
    } else {
      snprintf(error, sizeof error, "the renewal came back without a token");
      state = TRA_ERROR;
    }
  } else if (st == 400 || st == 401 || st == 403 || st == 404) {
    // invalid_grant: the refresh has died too. Only a fresh pairing fixes this,
    // and the screen has to say so instead of showing "connected".
    snprintf(error, sizeof error, "the Trakt session expired \xe2\x80\x94 connect again");
    state = TRA_ERROR;
  }
  // A TRANSPORT failure (st == 0) leaves the state alone on purpose: the step
  // will try again. A TV with no network for a minute is not a dead session.
  free(r);
  threadReady = 1;
  return NULL;
}

static void release(void *(*routine)(void *));

static void release(void *(*routine)(void *)) {
  if (threadAlive) return;
  threadReady = 0;
  if (pthread_create(&thread, NULL, routine, NULL) == 0) { pthread_detach(thread); threadAlive = 1; }
  else { snprintf(error, sizeof error, "no thread to talk to Trakt"); state = TRA_ERROR; }
}

void traktauth_begin(void) {
  if (state == TRA_REQUESTING || state == TRA_WAITING) return;
  error[0] = 0;
  beganMs = 0;
  pollMs = TRA_POLL_DEFAULT;
  state = TRA_REQUESTING;
  release(threadRequest);
}

void traktauth_step(unsigned nowMs) {
  if (threadAlive && threadReady) { threadAlive = 0; threadReady = 0; }
  if (threadAlive) return;

  // A 401 IN THIS SESSION, or a token already dead when it was loaded. With a
  // refresh stored, renew — one attempt a minute, because a transport failure
  // must not burn the session. With no refresh there is no route at all (a
  // credential that came from the package, or from an account row without one),
  // so the state drops to ERROR and the screen finally tells the truth instead
  // of showing "connected".
  //
  // The guard is not `state == TRA_ON`: a token loaded from the package leaves
  // the state STOPPED with trakt_active() true, and that was exactly the case
  // that displayed "connected" with everything failing.
  if ((renewPending || (trakt_refused() && trakt_active())) &&
      state != TRA_REQUESTING && state != TRA_WAITING) {
    // lastRenewal == 0 means "never tried", not "tried at instant zero".
    // Without that case a 401 in the app's first minute waited for 60 s of
    // uptime before renewing — which is the "Trakt takes a while to show up".
    static unsigned lastRenewal;
    if (refresh[0]) {
      if (!lastRenewal || nowMs - lastRenewal > 60000u) {
        lastRenewal = nowMs;
        release(threadRenew);
      }
    } else if (state != TRA_ERROR) {
      snprintf(error, sizeof error, "the Trakt session expired \xe2\x80\x94 connect again");
      state = TRA_ERROR;
    }
  }

  // Apply the token in the MAIN LOOP, never in the thread: trakt.c is read by the UI.
  if (tokenNew) {
    tokenNew = 0;
    renewPending = 0;
    trakt_set(token, cloud_trakt_client());
    save();
    // And send it to the ACCOUNT, so the person's other devices inherit the link.
    // Trakt's own RPC, not the provider-credential table: that table rejects
    // trackers outright (400, PG 22023 "Unsupported provider credential: trakt")
    // and every link made on this TV was being dropped on the floor.
    //
    // The user id and the username go empty: linking finishes before the app has
    // asked Trakt who this is, and the RPC takes them as required arguments.
    sync_push_tracker("trakt", token, refresh, tokenLifetime, "", "");
    // THE HOME HAS TO BE BUILT AGAIN. "Continue watching" is assembled inside
    // discover's build(), which calls trakt_resume() — and that returns 0 the
    // moment the credential is not there yet. Linking on this TV happens LONG
    // after that build, so without this the row only appeared on the next
    // launch: the link said it had worked and the home showed nothing.
    disc_rebuild();
    printf("[trakt] linked on this TV\n");
    fflush(stdout);
  }

  if (state != TRA_WAITING) return;
  if (!beganMs) beganMs = nowMs;
  if (expiresIn && (long)time(NULL) >= expiresIn) {
    snprintf(error, sizeof error, "the code expired");
    forgetStream();
    state = TRA_ERROR;
    return;
  }
  if (nowMs >= nextPoll) {
    nextPoll = nowMs + pollMs;
    release(threadPoll);
  }
}

void traktauth_cancel(void) {
  if (state == TRA_REQUESTING || state == TRA_WAITING || state == TRA_ERROR)
    state = token[0] ? TRA_ON : TRA_STOPPED;
}

TraState   traktauth_state(void) { return state; }
const char *traktauth_code(void) { return userCode; }
const char *traktauth_url(void)    { return url; }
const char *traktauth_error(void)   { return error; }
