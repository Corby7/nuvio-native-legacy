// The same broadcast elsewhere: see iptv_also.h.
#include "iptv_also.h"
#include "iptv.h"
#include "data.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QUALITY_FILE "iptv_quality.txt"
#define QUALITY_KEEP 1500   // channels remembered; the oldest go first

// --- What was measured -----------------------------------------------------------------
// One line per channel, "height fpsMilli name", the most recent last.
typedef struct { char *name; int height, fps; } Measured;
static Measured *seen;
static int nSeen, capSeen, seenLoaded;

static void loadSeen(void) {
  char *text, *line, *save = NULL;
  seenLoaded = 1;
  if (!(text = data_read(QUALITY_FILE))) return;
  for (line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
    int h, fps, at = 0;
    if (sscanf(line, "%d %d %n", &h, &fps, &at) < 2 || !at || !line[at]) continue;
    if (nSeen == capSeen) {
      int cap = capSeen ? capSeen * 2 : 128;
      Measured *m = realloc(seen, (size_t)cap * sizeof *m);
      if (!m) break;
      seen = m; capSeen = cap;
    }
    if (!(seen[nSeen].name = strdup(line + at))) break;
    seen[nSeen].height = h; seen[nSeen].fps = fps;
    nSeen++;
  }
  free(text);
}

static const Measured *measured(const char *name) {
  if (!seenLoaded) loadSeen();
  // From the end: a name noted twice (an older copy of the file) counts as its last.
  for (int i = nSeen - 1; i >= 0; i--) if (!strcmp(seen[i].name, name)) return &seen[i];
  return NULL;
}

static void saveSeen(void) {
  size_t cap = 1, len = 0;
  char *out;
  for (int i = 0; i < nSeen; i++) cap += strlen(seen[i].name) + 24;
  if (!(out = malloc(cap))) return;
  out[0] = 0;
  for (int i = 0; i < nSeen; i++)
    len += (size_t)snprintf(out + len, cap - len, "%d %d %s\n", seen[i].height, seen[i].fps, seen[i].name);
  data_write(QUALITY_FILE, out);
  free(out);
}

void iptv_quality_note(int ch, int width, int height, int fpsMilli) {
  const IptvList *l = iptv_list();
  const char *name;
  Measured m;
  int at = -1;
  (void)width;
  if (!l || ch < 0 || ch >= l->nCh || height <= 0) return;
  name = l->ch[ch].name;
  if (!seenLoaded) loadSeen();
  for (int i = nSeen - 1; i >= 0; i--) if (!strcmp(seen[i].name, name)) { at = i; break; }
  if (at >= 0 && seen[at].height == height && (seen[at].fps == fpsMilli || !fpsMilli)) return;
  if (at >= 0) {
    // To the end, as the most recent.
    m = seen[at];
    memmove(seen + at, seen + at + 1, (size_t)(nSeen - at - 1) * sizeof *seen);
    nSeen--;
  } else {
    if (!(m.name = strdup(name))) return;
    m.fps = 0;
  }
  m.height = height;
  if (fpsMilli > 0) m.fps = fpsMilli;
  if (nSeen == QUALITY_KEEP) {
    free(seen[0].name);
    memmove(seen, seen + 1, (size_t)(nSeen - 1) * sizeof *seen);
    nSeen--;
  }
  if (nSeen == capSeen) {
    int cap = capSeen ? capSeen * 2 : 128;
    Measured *g = realloc(seen, (size_t)cap * sizeof *g);
    if (!g) { free(m.name); return; }
    seen = g; capSeen = cap;
  }
  seen[nSeen++] = m;
  saveSeen();
}

// --- Quality ---------------------------------------------------------------------------
static const IptvChannel *channel(int ch) {
  const IptvList *l = iptv_list();
  return l && ch >= 0 && ch < l->nCh ? &l->ch[ch] : NULL;
}

static int claimed(int ch) {
  const IptvList *l = iptv_list();
  const IptvChannel *c = channel(ch);
  int q;
  if (!c) return IPTV_Q_UNKNOWN;
  if ((q = iptv_name_quality(c->name)) != IPTV_Q_UNKNOWN) return q;
  if (c->groupIndex >= 0 && c->groupIndex < l->nGroups) return iptv_name_quality(l->groups[c->groupIndex]);
  return IPTV_Q_UNKNOWN;
}

int iptv_quality_measured(int ch) {
  const IptvChannel *c = channel(ch);
  return c && measured(c->name);
}

int iptv_quality(int ch) {
  const IptvChannel *c = channel(ch);
  const Measured *m = c ? measured(c->name) : NULL;
  return m ? iptv_height_quality(m->height) : claimed(ch);
}

static int fpsOf(int ch) {
  const IptvChannel *c = channel(ch);
  const Measured *m = c ? measured(c->name) : NULL;
  return m ? m->fps : 0;
}

// Measured, the lines it has ("1080p50": a smooth picture says so, sport at 50
// frames a second is half the point); claimed, only the tag's own word ("HD"
// is as often 1080i as 720p).
const char *iptv_quality_label(int ch, char *dst, unsigned size) {
  static const char *CLAIM[] = { "", "SD", "HD", "FHD", "4K" };
  const IptvChannel *c = channel(ch);
  const Measured *m = c ? measured(c->name) : NULL;
  char lines[16];
  if (!size) return dst;
  if (!m) { snprintf(dst, size, "%s", CLAIM[claimed(ch)]); return dst; }
  if (m->height >= 1800) snprintf(lines, sizeof lines, "4K");
  else snprintf(lines, sizeof lines, "%dp", m->height >= 1000 && m->height <= 1088 ? 1080 : m->height);
  if (m->fps >= 45000) snprintf(dst, size, "%s%d", lines, (m->fps + 500) / 1000);
  else snprintf(dst, size, "%s", lines);
  return dst;
}

// --- Elsewhere -------------------------------------------------------------------------
static int isDivider(const char *n) {
  while (*n == ' ') n++;
  return n[0] == '#' && n[1] == '#' && n[2] == '#';
}

static long long gap(long long a, long long b) { return a > b ? a - b : b - a; }

// The same broadcast: a copy of the channel, or the same programme on now.
static int airsTheSame(const IptvList *l, int ch, const IptvProgramme *pg, int c, long long t) {
  int p, s;
  if (iptv_same_channel(l->ch[ch].name, l->ch[c].name)) return 1;
  if (!pg || (p = iptv_programme_at(l, c, t)) < 0) return 0;
  s = iptv_same_title(pg->title, l->pg[p].title);
  // One word in common is "News" on every news channel: only a simulcast,
  // starting and ending with this one, counts.
  return s == 2 || (s == 1 && gap(pg->start, l->pg[p].start) <= 600 && gap(pg->stop, l->pg[p].stop) <= 600);
}

static int score(int ch) {
  int fps = fpsOf(ch);
  return iptv_quality(ch) * 4 + (iptv_quality_measured(ch) ? 2 : 0) + (fps >= 45000 ? 1 : 0);
}

int iptv_also(int ch, long long t, int *out, int max) {
  const IptvList *l = iptv_list();
  const IptvProgramme *pg;
  int n = 0, p, *rank;
  if (!l || ch < 0 || ch >= l->nCh || max < 1) return 0;
  p = iptv_programme_at(l, ch, t);
  pg = p >= 0 ? &l->pg[p] : NULL;
  out[n++] = ch;
  for (int c = 0; c < l->nCh && n < max; c++)
    if (c != ch && !isDivider(l->ch[c].name) && airsTheSame(l, ch, pg, c, t)) out[n++] = c;
  // Best first: a stable insertion sort on the score, the channel number breaking ties.
  if (!(rank = malloc((size_t)n * sizeof *rank))) return n;
  for (int i = 0; i < n; i++) rank[i] = score(out[i]);
  for (int i = 1; i < n; i++) {
    int c = out[i], r = rank[i], j = i - 1;
    while (j >= 0 && (rank[j] < r || (rank[j] == r && l->ch[out[j]].number > l->ch[c].number))) {
      out[j + 1] = out[j]; rank[j + 1] = rank[j]; j--;
    }
    out[j + 1] = c; rank[j + 1] = r;
  }
  free(rank);
  return n;
}
