#include "update.h"
#include "appid.h"
#include "data.h"
#include "js.h"
#include "net.h"
#include <ctype.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define NV_UPDATE_RELEASES \
  "https://api.github.com/repos/Corby7/nuvio-native-legacy/releases?per_page=30"
#define NV_UPDATE_MAX 30

// WHERE THE PACKAGE AND THE HELPER GO. /media/developer/temp is world-writable
// (drwxrwxrwx root) and is where ares-install itself uploads a package before
// asking appInstallService for it, so the installer can read from it. The app's
// own /tmp is private to it since webOS 11 — nothing else could read a package
// left there.
#define NV_UPDATE_DIR    "/media/developer/temp"
#define NV_UPDATE_IPK    NV_UPDATE_DIR "/nuvio-update.ipk"
#define NV_UPDATE_SCRIPT NV_UPDATE_DIR "/nuvio-update.sh"

// --- THE LIST -----------------------------------------------------------------

static UpdateRelease shown[NV_UPDATE_MAX], fetched[NV_UPDATE_MAX];
static int nShown, nFetched;
static UpdateListState listState = UPD_IDLE;
// Set by the fetch thread under `lock`; update_poll adopts it.
static int fetchDone, fetchOk, fetching;
static int generation;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

int update_version_cmp(const char *a, const char *b) {
  for (int k = 0; k < 4; k++) {
    long x, y;
    while (*a && !isdigit((unsigned char)*a)) a++;
    while (*b && !isdigit((unsigned char)*b)) b++;
    x = strtol(a, (char **)&a, 10);
    y = strtol(b, (char **)&b, 10);
    if (x != y) return x < y ? -1 : 1;
  }
  return 0;
}

// The string value of `key` with its escapes decoded — newlines kept, which
// js_text flattens to spaces, and \uXXXX as UTF-8. NULL when absent.
static char *jsonString(const char *start, const char *end, const char *key) {
  size_t cap = (size_t)(end - start) + 1, k = 0;
  char *raw = malloc(cap), *out;
  const char *p;
  if (!raw) return NULL;
  if (!js_raw(start, end, key, raw, cap) || raw[0] != '"') { free(raw); return NULL; }
  out = malloc(strlen(raw) + 1);
  if (!out) { free(raw); return NULL; }
  for (p = raw + 1; *p && *p != '"'; p++) {
    if (*p != '\\' || !p[1]) { out[k++] = *p; continue; }
    p++;
    switch (*p) {
      case 'n': out[k++] = '\n'; break;
      case 'r': break;
      case 't': out[k++] = ' '; break;
      case 'u': {
        unsigned v = 0; int i;
        for (i = 1; i <= 4 && isxdigit((unsigned char)p[i]); i++)
          v = v * 16 + (unsigned)(isdigit((unsigned char)p[i]) ? p[i] - '0'
                                  : (tolower((unsigned char)p[i]) - 'a' + 10));
        p += i - 1;
        // Surrogate halves (emoji) are dropped: Inter has no glyph for them.
        if (v >= 0xD800 && v <= 0xDFFF) break;
        if (v < 0x80) out[k++] = (char)v;
        else if (v < 0x800) { out[k++] = (char)(0xC0 | v >> 6); out[k++] = (char)(0x80 | (v & 63)); }
        else { out[k++] = (char)(0xE0 | v >> 12); out[k++] = (char)(0x80 | (v >> 6 & 63));
               out[k++] = (char)(0x80 | (v & 63)); }
        break;
      }
      default: out[k++] = *p;
    }
  }
  out[k] = 0;
  free(raw);
  return out;
}

// Removes every `pat` from s, in place.
static void strip(char *s, const char *pat) {
  size_t n = strlen(pat);
  char *p;
  while ((p = strstr(s, pat)) != NULL) memmove(p, p + n, strlen(p + n) + 1);
}

// [text](url) becomes text.
static void unlink_md(char *s) {
  char *o, *c, *e;
  while ((o = strchr(s, '[')) && (c = strstr(o, "](")) && (e = strchr(c, ')'))) {
    memmove(c, e + 1, strlen(e + 1) + 1);
    memmove(o, o + 1, strlen(o + 1) + 1);
    s = c - 1;
  }
}

// THE NOTES AS THE TV SHOWS THEM. The releases are written in Markdown for
// GitHub; the TV has no Markdown renderer and no newline in its text blocks, so
// this keeps one paragraph per line, marks headings and bullets, and drops the
// "Install" section and checksum line — instructions for someone at a computer,
// and exactly what this screen replaces.
static char *cleanNotes(const char *md) {
  char *out = malloc(strlen(md) * 3 + 8), *line, *copy, *save = NULL;
  size_t k = 0;
  int skipping = 0;
  if (!out) return NULL;
  copy = strdup(md);
  if (!copy) { free(out); return NULL; }
  for (line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
    int heading = 0, bullet = 0;
    char *t = line;
    while (*t == ' ') t++;
    if (*t == '#') {
      while (*t == '#') t++;
      while (*t == ' ') t++;
      heading = 1;
      skipping = !strncasecmp(t, "install", 7);
    }
    if (skipping) continue;
    if (!strncasecmp(t, "SHA-256", 7) || !strncasecmp(t, "SHA256", 6)) continue;
    if ((t[0] == '-' || t[0] == '*') && t[1] == ' ') { t += 2; bullet = 1; }
    strip(t, "**"); strip(t, "__"); strip(t, "`");
    unlink_md(t);
    { size_t n = strlen(t);
      while (n && (t[n - 1] == ' ')) t[--n] = 0; }
    if (!*t) continue;
    if (heading) out[k++] = NV_UPDATE_HEADING;
    else if (bullet) { memcpy(out + k, "\xe2\x80\xa2 ", 4); k += 4; }
    memcpy(out + k, t, strlen(t)); k += strlen(t);
    out[k++] = '\n';
  }
  while (k && out[k - 1] == '\n') k--;
  out[k] = 0;
  free(copy);
  return out;
}

static void dateOf(const char *iso, char *dst, size_t n) {
  static const char *M[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                             "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
  int y = 0, m = 0, d = 0;
  if (sscanf(iso, "%d-%d-%d", &y, &m, &d) == 3 && m >= 1 && m <= 12)
    snprintf(dst, n, "%d %s %d", d, M[m - 1], y);
  else dst[0] = 0;
}

static void freeList(UpdateRelease *l, int n) {
  for (int i = 0; i < n; i++) { free(l[i].notes); l[i].notes = NULL; }
}

// One release object, [r, e).
static int parseRelease(const char *r, const char *e, UpdateRelease *out) {
  const char *a;
  char *body;
  memset(out, 0, sizeof *out);
  if (js_flag(r, e, "draft", 0)) return 0;
  if (!js_text(r, e, "tag_name", out->tag, sizeof out->tag)) return 0;
  snprintf(out->version, sizeof out->version, "%s",
           out->tag[0] == 'v' || out->tag[0] == 'V' ? out->tag + 1 : out->tag);
  if (!js_text(r, e, "name", out->name, sizeof out->name))
    snprintf(out->name, sizeof out->name, "%s", out->tag);
  { char iso[40] = "";
    js_text(r, e, "published_at", iso, sizeof iso);
    dateOf(iso, out->date, sizeof out->date); }
  out->prerelease = js_flag(r, e, "prerelease", 0);
  // The package is the asset whose name ends in _arm.ipk.
  for (a = js_array(r, e, "assets"); a; a = js_next(js_end(a))) {
    const char *ae = js_end(a);
    char name[160], digest[80];
    size_t n;
    if (!ae || *a != '{') break;
    if (!js_text(a, ae, "name", name, sizeof name)) continue;
    n = strlen(name);
    if (n < 8 || strcmp(name + n - 8, "_arm.ipk")) continue;
    js_text(a, ae, "browser_download_url", out->url, sizeof out->url);
    if (js_text(a, ae, "digest", digest, sizeof digest) && !strncmp(digest, "sha256:", 7))
      snprintf(out->sha256, sizeof out->sha256, "%s", digest + 7);
    break;
  }
  body = jsonString(r, e, "body");
  out->notes = body ? cleanNotes(body) : NULL;
  free(body);
  out->cmp = update_version_cmp(out->version, NV_APP_VERSION);
  return 1;
}

static void *fetchWork(void *unused) {
  char *body = net_download(NV_UPDATE_RELEASES, 20);
  UpdateRelease list[NV_UPDATE_MAX];
  int n = 0, ok = 0;
  const char *r;
  (void)unused;
  if (body && (r = js_root_array(body)) != NULL) {
    ok = 1;
    for (; r && n < NV_UPDATE_MAX; r = js_next(js_end(r))) {
      const char *e = js_end(r);
      if (!e || *r != '{') break;
      if (parseRelease(r, e, &list[n])) n++;
    }
  }
  free(body);
  printf("[update] releases: %s, %d\n", ok ? "ok" : "failed", n);
  fflush(stdout);
  pthread_mutex_lock(&lock);
  freeList(fetched, nFetched);
  memcpy(fetched, list, sizeof(UpdateRelease) * (size_t)n);
  nFetched = n;
  fetchOk = ok;
  fetchDone = 1;
  fetching = 0;
  pthread_mutex_unlock(&lock);
  return NULL;
}

void update_fetch(void) {
  pthread_t t;
  pthread_mutex_lock(&lock);
  if (fetching) { pthread_mutex_unlock(&lock); return; }
  fetching = 1;
  pthread_mutex_unlock(&lock);
  if (listState != UPD_READY) listState = UPD_LOADING;
  if (pthread_create(&t, NULL, fetchWork, NULL) != 0) {
    pthread_mutex_lock(&lock); fetching = 0; pthread_mutex_unlock(&lock);
    if (listState == UPD_LOADING) listState = UPD_FAILED;
    return;
  }
  pthread_detach(t);
}

void update_poll(void) {
  pthread_mutex_lock(&lock);
  if (fetchDone) {
    fetchDone = 0;
    if (fetchOk) {
      freeList(shown, nShown);
      memcpy(shown, fetched, sizeof(UpdateRelease) * (size_t)nFetched);
      nShown = nFetched;
      nFetched = 0;
      listState = UPD_READY;
      generation++;
    } else if (listState != UPD_READY) {
      listState = UPD_FAILED;
    }
  }
  pthread_mutex_unlock(&lock);
}

UpdateListState update_list_state(void) { return listState; }
int update_generation(void) { return generation; }
int update_n(void) { return nShown; }
const UpdateRelease *update_item(int i) { return i >= 0 && i < nShown ? &shown[i] : NULL; }
int update_latest(void) {
  for (int i = 0; i < nShown; i++) if (!shown[i].prerelease) return i;
  return -1;
}

// --- SHA-256 ------------------------------------------------------------------
//
// The package is checked against the digest GitHub computes for every release
// asset: a download cut short or altered is never handed to the installer.

static const uint32_t K256[64] = {
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
  0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
  0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
  0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
  0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
  0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
  0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256Block(uint32_t h[8], const unsigned char *p) {
  uint32_t w[64], a, b, c, d, e, f, g, hh;
  int i;
  for (i = 0; i < 16; i++)
    w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 |
           (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
  for (i = 16; i < 64; i++) {
    uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4]; f = h[5]; g = h[6]; hh = h[7];
  for (i = 0; i < 64; i++) {
    uint32_t t1 = hh + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
    uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

static void sha256Hex(const unsigned char *data, size_t n, char out[65]) {
  uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
  unsigned char tail[128];
  size_t full = n & ~(size_t)63, rest = n - full, t;
  uint64_t bits = (uint64_t)n * 8;
  for (size_t i = 0; i < full; i += 64) sha256Block(h, data + i);
  memset(tail, 0, sizeof tail);
  memcpy(tail, data + full, rest);
  tail[rest] = 0x80;
  t = rest < 56 ? 64 : 128;
  for (int i = 0; i < 8; i++) tail[t - 1 - i] = (unsigned char)(bits >> (i * 8));
  sha256Block(h, tail);
  if (t == 128) sha256Block(h, tail + 64);
  for (int i = 0; i < 8; i++) snprintf(out + i * 8, 9, "%08x", h[i]);
}

// --- INSTALLING ---------------------------------------------------------------

// THE HELPER. It must outlive the app: the installer closes the running app
// before it swaps the files, so whatever waits for the end of the install and
// opens the new version cannot be the app. luna-send-pub has a devmode role
// with outbound "*" (see video.c), which is what lets it reach the installer.
// The subscription is read from a file rather than a pipe so that the helper
// can stop on the final state and kill the call, which otherwise stays open.
// Whatever the outcome it opens the app again: after a failure that is the old
// version, which says so on its next look at this screen.
static const char SCRIPT[] =
  "#!/bin/sh\n"
  "L=\"$1\"; IPK=\"$2\"; ID=\"$3\"\n"
  "R=" NV_UPDATE_DIR "/nuvio-update.out\n"
  "log() { echo \"$(date +%H:%M:%S) $*\" >> \"$L\"; }\n"
  "log \"helper $$ installing $IPK\"\n"
  ": > \"$R\"\n"
  "/usr/bin/luna-send-pub -i luna://com.webos.appInstallService/dev/install "
    "\"{\\\"id\\\":\\\"$ID\\\",\\\"ipkUrl\\\":\\\"$IPK\\\",\\\"subscribe\\\":true}\" "
    "> \"$R\" 2>&1 &\n"
  "P=$!\n"
  "i=0; state=timeout\n"
  "while [ $i -lt 180 ]; do\n"
  "  sleep 1; i=$((i+1))\n"
  "  if grep -qE '\"state\" *: *\"installed\"' \"$R\"; then state=installed; break; fi\n"
  "  if grep -qiE 'fail|\"returnValue\" *: *false|errorCode' \"$R\"; then state=failed; break; fi\n"
  "done\n"
  "kill $P 2>/dev/null\n"
  "grep -iE 'fail|error|\"state\" *: *\"installed\"' \"$R\" | tail -3 >> \"$L\"\n"
  "log \"install: $state after ${i}s\"\n"
  "rm -f \"$IPK\" \"$R\"\n"
  "sleep 1\n"
  "/usr/bin/luna-send-pub -w 5000 -n 1 luna://com.webos.applicationManager/launch "
    "\"{\\\"id\\\":\\\"$ID\\\"}\" >> \"$L\" 2>&1\n"
  "log \"relaunch asked\"\n";

static UpdateInstallState installState = UPI_IDLE;
static char installError[160];

static void ulog(const char *fmt, ...) {
  char path[600], when[16];
  time_t t = time(NULL);
  va_list ap;
  FILE *f;
  printf("[update] ");
  va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
  printf("\n"); fflush(stdout);
  if (!data_path(path, sizeof path, "update.log")) return;
  if (!(f = fopen(path, "a"))) return;
  strftime(when, sizeof when, "%H:%M:%S", localtime(&t));
  fprintf(f, "%s app: ", when);
  va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
  fprintf(f, "\n");
  fclose(f);
}

static void fail(const char *why) {
  ulog("failed: %s", why);
  pthread_mutex_lock(&lock);
  snprintf(installError, sizeof installError, "%s", why);
  installState = UPI_FAILED;
  pthread_mutex_unlock(&lock);
}

static void setState(UpdateInstallState s) {
  pthread_mutex_lock(&lock); installState = s; pthread_mutex_unlock(&lock);
}

static int writeFile(const char *path, const void *data, long n) {
  FILE *f = fopen(path, "wb");
  if (!f) return 0;
  if (fwrite(data, 1, (size_t)n, f) != (size_t)n) { fclose(f); return 0; }
  return fclose(f) == 0;
}

// The log path is handed to the helper as an argument and the package path is
// spliced into the installer's JSON there, so both are held to characters that
// need no escaping in JSON or in the shell.
static int safePath(const char *p) {
  if (!p || p[0] != '/') return 0;
  for (; *p; p++)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
          (*p >= '0' && *p <= '9') || strchr("/._-", *p))) return 0;
  return 1;
}

// Double fork and setsid: the helper ends up in a session of its own, adopted by
// init, so the installer closing the app leaves it running.
static int spawnDetached(const char *logPath) {
  pid_t pid = fork();
  if (pid < 0) return 0;
  if (pid == 0) {
    if (setsid() < 0) _exit(1);
    pid = fork();
    if (pid != 0) _exit(pid < 0 ? 1 : 0);
    freopen("/dev/null", "w", stdout);
    freopen("/dev/null", "w", stderr);
    execl("/bin/sh", "sh", NV_UPDATE_SCRIPT, logPath, NV_UPDATE_IPK, NV_APP_ID, (char *)NULL);
    _exit(127);
  }
  { int st = 0; waitpid(pid, &st, 0); return WIFEXITED(st) && WEXITSTATUS(st) == 0; }
}

typedef struct { char url[512], sha256[65], version[32]; } Job;

static void *installWork(void *arg) {
  Job *job = arg;
  char logPath[600], sum[65];
  long n = 0;
  char *body;
  if (!data_path(logPath, sizeof logPath, "update.log"))
    snprintf(logPath, sizeof logPath, NV_UPDATE_DIR "/nuvio-update.log");
  ulog("installing %s from %s", job->version, job->url);
  body = net_download_bin(job->url, 180, &n);
  if (!body || n < 1024) { free(body); fail("The download failed. Check the connection and try again."); goto done; }
  setState(UPI_VERIFYING);
  sha256Hex((const unsigned char *)body, (size_t)n, sum);
  if (job->sha256[0] && strcasecmp(sum, job->sha256)) {
    ulog("sha256 %s, expected %s", sum, job->sha256);
    free(body);
    fail("The download did not match the release's checksum. Nothing was installed.");
    goto done;
  }
  if (!writeFile(NV_UPDATE_IPK, body, n)) {
    free(body); fail("The package could not be saved on the TV."); goto done;
  }
  free(body);
  if (!safePath(logPath) ||
      !writeFile(NV_UPDATE_SCRIPT, SCRIPT, (long)strlen(SCRIPT))) {
    fail("The installer could not be prepared."); goto done;
  }
  setState(UPI_INSTALLING);
  if (!spawnDetached(logPath)) { fail("The installer could not be started."); goto done; }
  ulog("helper started; %ld bytes, sha256 %s", n, sum);
done:
  free(job);
  return NULL;
}

int update_install(int i) {
  const UpdateRelease *r = update_item(i);
  pthread_t t;
  Job *job;
#ifdef __APPLE__
  if (r) {
    snprintf(installError, sizeof installError, "Updates install on the TV only.");
    installState = UPI_FAILED;
  }
  return 0;
#endif
  if (!r || !r->url[0]) return 0;
  pthread_mutex_lock(&lock);
  if (installState == UPI_DOWNLOADING || installState == UPI_VERIFYING ||
      installState == UPI_INSTALLING) { pthread_mutex_unlock(&lock); return 0; }
  installState = UPI_DOWNLOADING;
  installError[0] = 0;
  pthread_mutex_unlock(&lock);
  job = calloc(1, sizeof *job);
  if (!job) { setState(UPI_IDLE); return 0; }
  snprintf(job->url, sizeof job->url, "%s", r->url);
  snprintf(job->sha256, sizeof job->sha256, "%s", r->sha256);
  snprintf(job->version, sizeof job->version, "%s", r->version);
  if (pthread_create(&t, NULL, installWork, job) != 0) {
    free(job); setState(UPI_IDLE); return 0;
  }
  pthread_detach(t);
  return 1;
}

UpdateInstallState update_install_state(void) {
  UpdateInstallState s;
  pthread_mutex_lock(&lock); s = installState; pthread_mutex_unlock(&lock);
  return s;
}

const char *update_install_error(void) { return installError; }

void update_install_dismiss(void) {
  pthread_mutex_lock(&lock);
  if (installState == UPI_FAILED) installState = UPI_IDLE;
  pthread_mutex_unlock(&lock);
}
