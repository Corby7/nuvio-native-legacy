#include "streams.h"
#include "badges.h"
#include "js.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int contains(const char *s, const char *term) {
  for (; *s; s++) if (!strncasecmp(s, term, strlen(term))) return 1;
  return 0;
}

// An isolated token matches .DV., DV/HDR and [DV], but never DVD/DVDRip.
static int token(const char *s, const char *t) {
  size_t n = strlen(t);
  const char *p;
  for (p = s; *p; p++)
    if ((p == s || !isalnum((unsigned char)p[-1])) &&
        !strncasecmp(p, t, n) && !isalnum((unsigned char)p[n])) return 1;
  return 0;
}

int stream_parse(const char *json, const char *provider, Stream **output) {
  const char *p, *end;
  int n = 0, cap = 0;
  Stream *v = NULL;
  *output = NULL;
  if (!json) return 0;
  p = js_array(json, json + strlen(json), "streams");
  while (p && *p == '{') {
    Stream s = {0};
    char title[2048] = "", text[5000];
    end = js_end(p);
    if (!end || end <= p) break;
    js_text(p, end, "url", s.url, sizeof s.url);
    // Do not expose torrents/externalUrl as direct links, and never play a truncated URL.
    if ((!strncmp(s.url, "http://", 7) || !strncmp(s.url, "https://", 8)) &&
        strlen(s.url) < sizeof s.url - 1) {
      js_text(p, end, "name", s.label, sizeof s.label);
      js_text(p, end, "description", s.description, sizeof s.description);
      js_text(p, end, "title", title, sizeof title);
      js_text(p, end, "filename", s.file, sizeof s.file);
      if (!s.description[0]) snprintf(s.description, sizeof s.description, "%s", title);
      if (!s.label[0]) snprintf(s.label, sizeof s.label, "%s", provider);
      snprintf(s.provider, sizeof s.provider, "%s", provider);
      snprintf(text, sizeof text, "%s %s %s %s", s.label, s.description, title, s.file);
      s.height = contains(text, "2160") || token(text, "4k") || token(text, "uhd") ? 2160 :
                 contains(text, "1440") ? 1440 : contains(text, "1080") ? 1080 :
                 contains(text, "720") ? 720 : contains(text, "480") ? 480 : 0;
      s.dolbyVision = token(text, "dv") || token(text, "dovi") ||
                      contains(text, "dolby vision") || contains(text, "dolbyvision");
      s.dolbyAtmos = token(text, "atmos");
      s.badges = badges_detect(text);
      s.mp4 = token(text, "mp4") || contains(s.url, ".mp4");
      double bytes = js_num(p, end, "videoSize", 0);
      if (bytes > 0) s.sizeMB = (long)(bytes / (1024.0 * 1024.0));
      else {
        const char *u = strstr(text, " GB");
        double scale = 1024;
        if (!u) { u = strstr(text, " MB"); scale = 1; }
        if (u) {
          const char *start = u;
          while (start > text && (isdigit((unsigned char)start[-1]) || start[-1] == '.')) start--;
          if (start < u) s.sizeMB = (long)(atof(start) * scale);
        }
      }
      if (n == cap) {
        int new = cap ? cap * 2 : 32;
        Stream *tmp = realloc(v, (size_t)new * sizeof *tmp);
        if (!tmp) { free(v); return -1; }
        v = tmp; cap = new;
      }
      v[n++] = s;
    }
    p = js_next(end);
  }
  *output = v;
  return n;
}
