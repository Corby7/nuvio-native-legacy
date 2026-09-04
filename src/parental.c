#include "parental.h"
#include "net.h"
#include "js.h"
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// Endereco em js/config.js do app web: PARENTAL_GUIDE_API_URL.
#define PG_URL "https://api.tiffara.com/titles/%s/parentsGuide"

// THE ORDER matters: it is the same one the web lists the categories in
// (parentalGuideRepository.js:86), and therefore the same one they appear in on
// screen.
static const struct { const char *key, *label; } CATS[PG_MAX] = {
  { "SEXUAL_CONTENT",              "Nudez" },
  { "VIOLENCE",                    "Viol\xc3\xaa" "ncia" },
  { "PROFANITY",                   "Linguagem Impr\xc3\xb3" "pria" },
  { "ALCOHOL_DRUGS",               "Drogas/\xc3\x81" "lcool" },
  { "FRIGHTENING_INTENSE_SCENES",  "Conte\xc3\xba" "do Assustador" },
};

static const struct { const char *level, *label; } LEVELS[] = {
  { "mild",     "Mild" },
  { "moderate", "Moderado" },
  { "severe",   "Severo" },
  { NULL, NULL }
};

static struct { const char *label, *severity; } lines[PG_MAX];
static int  nLines;
static char idRequest[16];
static char idInProgress[16];
static int  threadAlive;
static pthread_t thread;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static const char *labelLevel(const char *level) {
  int i;
  for (i = 0; LEVELS[i].level; i++)
    if (!strcmp(LEVELS[i].level, level)) return LEVELS[i].label;
  return NULL;
}

// The DOMINANT severity of a category, by the web's rule (resolveSeverity): the
// most-voted level among those other than "none"; discarded when "none" has as
// many votes as it or more. Without that second half, every film would show all
// five lines, because there is always some vote on everything.
static const char *dominant(const char *start, const char *end) {
  const char *bq = js_array(start, end, "severityBreakdowns");
  char best[16] = "";
  double vBest = 0.0, vNone = 0.0;
  while (bq) {
    const char *bf = js_end(bq);
    char level[16] = "";
    double votes;
    js_text(bq, bf, "severityLevel", level, sizeof level);
    votes = js_num(bq, bf, "voteCount", 0.0);
    if (!strcmp(level, "none")) {
      vNone = votes;
    } else if (level[0] && votes > vBest) {
      vBest = votes;
      snprintf(best, sizeof best, "%s", level);
    }
    bq = js_next(bf);
  }
  if (!best[0] || vBest <= vNone) return NULL;
  return labelLevel(best);
}

static void *fetch(void *arg) {
  char url[160], id[16];
  char *body;
  (void)arg;
  pthread_mutex_lock(&lock);
  snprintf(id, sizeof id, "%s", idInProgress);
  pthread_mutex_unlock(&lock);

  snprintf(url, sizeof url, PG_URL, id);
  body = net_download(url, 8);
  if (body) {
    struct { const char *r, *g; } found[PG_MAX];
    int n = 0, k;
    // One pass per category rather than one over the array: that way the
    // screen's order is CATS's, not the order the API returned.
    for (k = 0; k < PG_MAX; k++) {
      const char *q = js_array(body, NULL, "parentsGuide");
      while (q) {
        const char *f = js_end(q);
        char cat[48] = "";
        js_text(q, f, "category", cat, sizeof cat);
        if (!strcmp(cat, CATS[k].key)) {
          const char *g = dominant(q, f);
          if (g) { found[n].r = CATS[k].label; found[n].g = g; n++; }
          break;
        }
        q = js_next(f);
      }
    }
    free(body);
    pthread_mutex_lock(&lock);
    // Only publishes if the title is still this one: changing film during the
    // fetch would leave the previous one's warning on the next one's screen.
    if (!strcmp(id, idRequest)) {
      for (k = 0; k < n; k++) {
        lines[k].label = found[k].r;
        lines[k].severity = found[k].g;
      }
      nLines = n;
    }
    pthread_mutex_unlock(&lock);
    printf("[parental] %s -> %d lines\n", id, n); fflush(stdout);
  }
  pthread_mutex_lock(&lock);
  threadAlive = 0;
  pthread_mutex_unlock(&lock);
  return NULL;
}

void parental_request(const char *imdb) {
  if (!imdb || imdb[0] != 't') return;
  pthread_mutex_lock(&lock);
  if (!strcmp(idRequest, imdb)) { pthread_mutex_unlock(&lock); return; }
  snprintf(idRequest, sizeof idRequest, "%s", imdb);
  nLines = 0;
  if (threadAlive) { pthread_mutex_unlock(&lock); return; }
  snprintf(idInProgress, sizeof idInProgress, "%s", imdb);
  threadAlive = 1;
  pthread_mutex_unlock(&lock);
  if (pthread_create(&thread, NULL, fetch, NULL) != 0) threadAlive = 0;
  else pthread_detach(thread);
}

int parental_n(void) {
  int n;
  pthread_mutex_lock(&lock);
  n = nLines;
  pthread_mutex_unlock(&lock);
  return n;
}
const char *parental_label(int i) {
  return (i >= 0 && i < nLines) ? lines[i].label : "";
}
const char *parental_severity(int i) {
  return (i >= 0 && i < nLines) ? lines[i].severity : "";
}
