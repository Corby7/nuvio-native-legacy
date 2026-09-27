#include "subtitle.h"
#include "subcharset.h"
#include "net.h"
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static SubtitleCue *cues;
// reach[i] is the latest end among cues[0..i]. The cues are sorted by start but
// may overlap — an ASS sign held on screen while the dialogue runs under it —
// so the end times are not sorted, and this is what tells subtitle_text where
// to stop looking back.
static double *reach;
static int nCues, on;
static unsigned generation, retimed;

static double parseTime(const char *s) {
  int h=0,m=0; double seg=0;
  if (sscanf(s,"%d:%d:%lf",&h,&m,&seg)==3) return h*3600.0+m*60.0+seg;
  if (sscanf(s,"%d:%lf",&m,&seg)==2) return m*60.0+seg;
  return -1;
}

static void entity(char *s) {
  char *r=s,*w=s;
  while (*r) {
    if (*r=='<' ) {
      if (!strncasecmp(r,"<br",3)) { *w++='\n'; }
      while (*r && *r!='>') r++;
      if (*r) r++;
    } else if (!strncmp(r,"&amp;",5))  { *w++='&'; r+=5; }
    else if (!strncmp(r,"&lt;",4))   { *w++='<'; r+=4; }
    else if (!strncmp(r,"&gt;",4))   { *w++='>'; r+=4; }
    else if (!strncmp(r,"&quot;",6)) { *w++='"'; r+=6; }
    else if (!strncmp(r,"&#39;",5))  { *w++='\''; r+=5; }
    else *w++=*r++;
  }
  *w=0;
}

static int srtTags(char *s);

static int parseSrt(const char *body, SubtitleCue **output) {
  char *buf,*p,*line; int n=0,cap=128;
  SubtitleCue *v;
  if (output) *output=NULL;
  if (!body || !output) return 0;
  buf=strdup(body); if(!buf)return 0;
  v=calloc((size_t)cap,sizeof *v); if(!v){free(buf);return 0;}
  p=buf;
  if ((unsigned char)p[0]==0xef && (unsigned char)p[1]==0xbb && (unsigned char)p[2]==0xbf) p+=3;
  while (*p) {
    char *next=strchr(p,'\n');
    if(next)*next++=0;
    { char *q=strchr(p,'\r'); if(q)*q=0; }
    line=p; p=next?next:p+strlen(p);
    if (!strstr(line,"-->")) continue;
    char *seta=strstr(line,"-->"); *seta=0; seta+=3;
    while(isspace((unsigned char)*seta))seta++;
    for(char *q=line;*q;q++)if(*q==',')*q='.';
    for(char *q=seta;*q;q++)if(*q==',')*q='.';
    double start=parseTime(line),end=parseTime(seta);
    if(start<0||end<=start)continue;
    char text[768]={0}; size_t used=0;
    while(*p) {
      char *nl=strchr(p,'\n'); if(nl)*nl++=0;
      { char *q=strchr(p,'\r');if(q)*q=0; }
      if(!*p){p=nl?nl:p;break;}
      size_t l=strlen(p),remains=sizeof text-used-1;
      if(used&&remains){text[used++]='\n';remains--;}
      if(l>remains)l=remains;memcpy(text+used,p,l);used+=l;text[used]=0;
      p=nl?nl:p+strlen(p);
    }
    entity(text);
    int align=srtTags(text);
    if(!text[0])continue;
    if(n==cap){cap*=2;SubtitleCue *nv=realloc(v,(size_t)cap*sizeof *v);if(!nv)break;v=nv;}
    // Zeroed: realloc's new half is not, and a stray `positioned` sent every cue
    // past the 128th to a random corner.
    memset(&v[n],0,sizeof v[n]);
    v[n].start=start;v[n].end=end;v[n].align=align;snprintf(v[n].text,sizeof v[n].text,"%s",text);n++;
  }
  free(buf);
  if(!n){free(v);return 0;}
  *output=v;return n;
}

// --- ASS / SSA ----------------------------------------------------------------
//
// The text, and WHERE it goes: the alignment (\an, the older \a, or the style's
// own Alignment) and a \pos point, which is the subset the web app keeps. A
// \move is placed at its end point, still, as the web's fallback does. Fonts,
// colours, fades and karaoke are dropped; the viewer's own subtitle style is
// used for every line. A vector drawing ({\p1}...) has no text at all and is
// skipped, or its path commands would be shown as words.

// "0:01:02.50" -> 62.5, the fraction optional; -1 when it is not an ASS time.
// Only '.' starts a fraction: the time is read in place, and the ',' after it
// is the next column.
static double assTime(const char *s) {
  int h, m, sec, frac = 0, digits = 0, used = 0;
  while (*s == ' ') s++;
  if (sscanf(s, "%d:%d:%d%n", &h, &m, &sec, &used) != 3 || h < 0 || m < 0 || sec < 0) return -1;
  s += used;
  if (*s == '.') {
    s++;
    while (isdigit((unsigned char)*s)) { if (digits < 3) { frac = frac * 10 + (*s - '0'); digits++; } s++; }
    while (digits++ < 3) frac *= 10;
  }
  while (*s == ' ') s++;
  if (*s && *s != ',') return -1;
  return h * 3600.0 + m * 60.0 + sec + frac / 1000.0;
}

// Override blocks out, \N and \n to line breaks, \h to a space. 0 when the
// line is a drawing.
static int assText(const char *in, char *out, size_t size) {
  size_t w = 0;
  while (*in && w + 1 < size) {
    if (*in == '{') {
      const char *close = strchr(in, '}'), *q;
      if (!close) break;
      for (q = in; q < close; q++)
        if (q[0] == '\\' && q[1] == 'p' && q[2] >= '1' && q[2] <= '9') return 0;
      in = close + 1;
    } else if (in[0] == '\\' && (in[1] == 'N' || in[1] == 'n')) { out[w++] = '\n'; in += 2; }
    else if (in[0] == '\\' && in[1] == 'h') { out[w++] = ' '; in += 2; }
    else out[w++] = *in++;
  }
  out[w] = 0;
  // Trim, including the empty lines a leading or trailing \N leaves behind.
  { char *a = out, *z = out + strlen(out);
    while (*a && isspace((unsigned char)*a)) a++;
    while (z > a && isspace((unsigned char)z[-1])) z--;
    *z = 0; memmove(out, a, (size_t)(z - a) + 1); }
  return out[0] != 0;
}

// The columns of a Dialogue line. `nFields` counts them, Text always last.
typedef struct { int start, end, style, nFields; } AssFormat;

static int assFormat(const char *list, AssFormat *f) {
  int i = 0, text = -1; AssFormat r = { -1, -1, -1, 0 };
  while (*list) {
    char name[16]; size_t k = 0;
    while (*list == ' ') list++;
    while (*list && *list != ',' && k + 1 < sizeof name) name[k++] = (char)tolower((unsigned char)*list++);
    while (k && name[k - 1] == ' ') k--;
    name[k] = 0;
    if (!strcmp(name, "start")) r.start = i;
    else if (!strcmp(name, "end")) r.end = i;
    else if (!strcmp(name, "text")) text = i;
    else if (!strcmp(name, "style")) r.style = i;
    i++;
    if (*list == ',') list++;
    else if (*list) while (*list && *list != ',') list++;
  }
  // A Styles section has a Format line too; only an Events one names all three.
  if (r.start < 0 || r.end < 0 || text != i - 1) return 0;
  r.nFields = i; *f = r;
  return 1;
}

// A headerless body (some proxies strip the sections) still carries its timing:
// either "Start,End,Text" or the standard ten columns behind a Layer or Marked.
static int assInferFormat(const char *rest, AssFormat *f) {
  const char *c1 = strchr(rest, ','), *c2 = c1 ? strchr(c1 + 1, ',') : NULL;
  if (c1 && assTime(rest) >= 0 && assTime(c1 + 1) >= 0) {
    f->start = 0; f->end = 1; f->style = -1; f->nFields = 3; return 1;
  }
  if (c2 && assTime(c1 + 1) >= 0 && assTime(c2 + 1) >= 0) {
    f->start = 1; f->end = 2; f->style = 3; f->nFields = 10; return 1;
  }
  return 0;
}

// SSA's \a numbering (1-3 bottom, +4 top, +8 middle) as the numpad one \an uses.
static int ssaToNumpad(int a) {
  if (a >= 1 && a <= 3) return a;
  if (a >= 5 && a <= 7) return a + 2;
  if (a >= 9 && a <= 11) return a - 5;
  return 0;
}

// The first \anN or \aN inside the override blocks, as numpad; 0 when none.
static int assInlineAlign(const char *s) {
  for (const char *o = strchr(s, '{'); o; o = strchr(o + 1, '{')) {
    const char *close = strchr(o, '}');
    if (!close) break;
    for (const char *q = o; q < close; q++) {
      if (q[0] != '\\' || q[1] != 'a') continue;
      if (q[2] == 'n' && q[3] >= '1' && q[3] <= '9') return q[3] - '0';
      if (isdigit((unsigned char)q[2])) return ssaToNumpad(atoi(q + 2));
    }
  }
  return 0;
}

// The point of a \pos(x,y), or the end point of a \move(x1,y1,x2,y2[,t1,t2]).
static int assPoint(const char *s, float *x, float *y) {
  const char *q;
  double v[4];
  if ((q = strstr(s, "\\pos(")) && sscanf(q + 5, "%lf , %lf", &v[0], &v[1]) == 2) {
    *x = (float)v[0]; *y = (float)v[1]; return 1;
  }
  if ((q = strstr(s, "\\move(")) &&
      sscanf(q + 6, "%lf , %lf , %lf , %lf", &v[0], &v[1], &v[2], &v[3]) == 4) {
    *x = (float)v[2]; *y = (float)v[3]; return 1;
  }
  return 0;
}

// ASS tags left inside an SRT by converters and fansub tools: {\an8}, {\i1},
// \N. A block that starts "{\" goes, its \an kept as the cue's alignment
// (returned, 0 when none); \N and \n become line breaks, \h a space. A '{' with
// no backslash after it is dialogue and stays. Trimmed after, so a cue that was
// only tags or a lone space comes out empty.
static int srtTags(char *s) {
  int align = assInlineAlign(s);
  char *r = s, *w = s, *close;
  while (*r) {
    if (r[0] == '{' && r[1] == '\\' && (close = strchr(r, '}'))) r = close + 1;
    else if (r[0] == '\\' && (r[1] == 'N' || r[1] == 'n')) { *w++ = '\n'; r += 2; }
    else if (r[0] == '\\' && r[1] == 'h') { *w++ = ' '; r += 2; }
    else *w++ = *r++;
  }
  *w = 0;
  { char *a = s, *z = s + strlen(s);
    while (*a && isspace((unsigned char)*a)) a++;
    while (z > a && isspace((unsigned char)z[-1])) z--;
    *z = 0; memmove(s, a, (size_t)(z - a) + 1); }
  return align;
}

// A style's name and alignment, from [V4+ Styles] (numpad) or [V4 Styles] (SSA).
typedef struct { char name[48]; int align; } AssStyle;
#define ASS_STYLES 128

static int parseAss(const char *body, SubtitleCue **output) {
  const char *p = body;
  int n = 0, cap = 128, haveFormat = 0, nStyles = 0, styleName = -1, styleAlign = -1, ssa = 0;
  double resX = 0, resY = 0;
  AssFormat format = { 0 };
  AssStyle *styles = calloc(ASS_STYLES, sizeof *styles);
  SubtitleCue *v = calloc((size_t)cap, sizeof *v);
  if (!v || !styles) { free(v); free(styles); return 0; }
  while (*p) {
    const char *eol = strchr(p, '\n'), *next = eol ? eol + 1 : p + strlen(p);
    size_t len = (size_t)(next - p);
    char line[4096], *rest;
    if (len >= sizeof line) { p = next; continue; }
    memcpy(line, p, len); line[len] = 0; p = next;
    { char *q = line + strlen(line); while (q > line && (q[-1] == '\n' || q[-1] == '\r')) *--q = 0; }
    rest = line; while (*rest == ' ' || *rest == '\t') rest++;
    if (!strncasecmp(rest, "[V4 Styles", 10)) { ssa = 1; continue; }
    if (!strncasecmp(rest, "[V4+ Styles", 11)) { ssa = 0; continue; }
    if (!strncasecmp(rest, "PlayResX:", 9)) { resX = atof(rest + 9); continue; }
    if (!strncasecmp(rest, "PlayResY:", 9)) { resY = atof(rest + 9); continue; }
    if (!strncasecmp(rest, "Format:", 7)) {
      if (assFormat(rest + 7, &format)) { haveFormat = 1; continue; }
      // The Styles section's Format: where Name and Alignment sit.
      int i = 0; styleName = styleAlign = -1;
      for (const char *q = rest + 7; *q; i++) {
        while (*q == ' ') q++;
        if (!strncasecmp(q, "Name", 4) && (q[4] == ',' || q[4] == ' ' || !q[4])) styleName = i;
        if (!strncasecmp(q, "Alignment", 9)) styleAlign = i;
        q = strchr(q, ','); if (!q) break; q++;
      }
      continue;
    }
    if (!strncasecmp(rest, "Style:", 6)) {
      const char *q = rest + 6; int i = 0;
      if (styleName < 0 || styleAlign < 0 || nStyles == ASS_STYLES) continue;
      AssStyle st = { "", 0 };
      for (; q; i++) {
        while (*q == ' ') q++;
        if (i == styleName) {
          size_t k = strcspn(q, ",");
          if (k >= sizeof st.name) k = sizeof st.name - 1;
          memcpy(st.name, q, k); st.name[k] = 0;
          while (k && st.name[k - 1] == ' ') st.name[--k] = 0;
        }
        if (i == styleAlign) st.align = ssa ? ssaToNumpad(atoi(q)) : atoi(q);
        q = strchr(q, ','); if (q) q++;
      }
      if (st.align < 1 || st.align > 9) st.align = 0;
      if (st.name[0]) styles[nStyles++] = st;
      continue;
    }
    if (strncasecmp(rest, "Dialogue:", 9)) continue;
    rest += 9; while (*rest == ' ') rest++;
    AssFormat f = format;
    if (!haveFormat && !assInferFormat(rest, &f)) continue;
    // Every column but the last ends at a comma; Text keeps its own commas.
    const char *col[32]; int i;
    if (f.nFields > 32) continue;
    col[0] = rest;
    for (i = 1; i < f.nFields; i++) {
      const char *c = strchr(col[i - 1], ',');
      if (!c) break;
      col[i] = c + 1;
    }
    if (i < f.nFields) continue;
    double start = assTime(col[f.start]), end = assTime(col[f.end]);
    const char *raw = col[f.nFields - 1];
    char text[768];
    if (start < 0 || end <= start || !assText(raw, text, sizeof text)) continue;
    if (n == cap) { SubtitleCue *nv = realloc(v, (size_t)cap * 2 * sizeof *v); if (!nv) break; v = nv; cap *= 2; }
    SubtitleCue *c = &v[n];
    memset(c, 0, sizeof *c);
    c->start = start; c->end = end;
    snprintf(c->text, sizeof c->text, "%s", text);
    c->align = assInlineAlign(raw);
    if (!c->align && f.style >= 0) {
      char name[48]; size_t k;
      const char *q = col[f.style];
      while (*q == ' ') q++;
      k = strcspn(q, ",");
      if (k >= sizeof name) k = sizeof name - 1;
      memcpy(name, q, k); name[k] = 0;
      while (k && name[k - 1] == ' ') name[--k] = 0;
      // "*Default" is how some writers mark the default style.
      for (int s = 0; s < nStyles; s++)
        if (!strcasecmp(styles[s].name, name) ||
            (name[0] == '*' && !strcasecmp(styles[s].name, name + 1))) { c->align = styles[s].align; break; }
    }
    c->positioned = assPoint(raw, &c->x, &c->y);
    n++;
  }
  free(styles);
  if (!n) { free(v); return 0; }
  // A point is in script pixels: the script's PlayRes, with the defaults libass
  // uses when it is missing (384x288, or one side derived from the other).
  if (resX <= 0 && resY <= 0) { resX = 384; resY = 288; }
  else if (resX <= 0) resX = resY == 1024 ? 1280 : resY * 4 / 3;
  else if (resY <= 0) resY = resX == 1280 ? 1024 : resX * 3 / 4;
  for (int i = 0; i < n; i++) {
    if (!v[i].positioned) continue;
    v[i].x = (float)(v[i].x / resX); v[i].y = (float)(v[i].y / resY);
    if (v[i].x < 0) v[i].x = 0; if (v[i].x > 1) v[i].x = 1;
    if (v[i].y < 0) v[i].y = 0; if (v[i].y > 1) v[i].y = 1;
  }
  *output = v;
  return n;
}

// Whether a line of the body starts with "Dialogue:" — an ASS event. An SRT
// whose dialogue merely mentions the word does not start a line with it.
static int looksLikeAss(const char *body) {
  const char *p = body;
  while (p && *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r') p++;
    if (!strncasecmp(p, "Dialogue:", 9)) return 1;
    p = strchr(p, '\n'); if (p) p++;
  }
  return 0;
}

static int byStart(const void *a, const void *b) {
  const SubtitleCue *x = a, *y = b;
  if (x->start != y->start) return x->start < y->start ? -1 : 1;
  return (x->end > y->end) - (x->end < y->end);
}

// SRT/VTT or ASS/SSA, told apart by the body: addons mislabel the extension. The
// cues come back sorted by start, which neither format promises and
// subtitle_text's search needs.
int subtitle_parse(const char *body, SubtitleCue **output) {
  int n = 0;
  if (output) *output = NULL;
  if (!body || !output) return 0;
  if (looksLikeAss(body)) n = parseAss(body, output);
  if (!n) n = parseSrt(body, output);
  if (n > 1) qsort(*output, (size_t)n, sizeof **output, byStart);
  return n;
}

// --- THE FORMAT, for the sheet -------------------------------------------------
//
// "ASS", "SSA", "SRT" or "VTT", as the embedded rows already say it. An addon
// names none, and most of its URLs carry no extension (OpenSubtitles serves
// /file/1954191548), so the body is what says. The kinds are kept per URL for the
// session: every subtitle loaded records its own, and subtitle_kind can fetch the
// ones the sheet is showing, one at a time on a thread of its own. The host does
// not honour Range, so a probe is the whole file — tens of KB, and only for the
// rows on screen.

static const char *bodyKind(const char *b) {
  if (!strncmp(b, "\xef\xbb\xbf", 3)) b += 3;
  while (isspace((unsigned char)*b)) b++;
  if (!strncmp(b, "WEBVTT", 6)) return "VTT";
  if (!looksLikeAss(b) && !strstr(b, "[Script Info]")) return "SRT";
  // SSA is v4.00 with [V4 Styles]; ASS is v4.00+ with [V4+ Styles].
  if (strstr(b, "[V4 Styles]") || strstr(b, "[v4 Styles]")) return "SSA";
  { const char *t = strstr(b, "ScriptType:");
    if (t) { t += 11; while (*t == ' ') t++;
             if (!strncasecmp(t, "v4.00", 5) && t[5] != '+') return "SSA"; } }
  return "ASS";
}

// The extension of a URL's path, when it is one of the four.
static const char *urlKind(const char *url) {
  size_t n = strcspn(url, "?#");
  const char *dot;
  char ext[5] = "";
  for (dot = url + n; dot > url && dot[-1] != '.' && dot[-1] != '/'; dot--) {}
  if (dot == url || dot[-1] != '.' || url + n - dot > 3) return NULL;
  memcpy(ext, dot, (size_t)(url + n - dot)); ext[url + n - dot] = 0;
  if (!strcasecmp(ext, "ass")) return "ASS";
  if (!strcasecmp(ext, "ssa")) return "SSA";
  if (!strcasecmp(ext, "srt")) return "SRT";
  if (!strcasecmp(ext, "vtt")) return "VTT";
  return NULL;
}

#define KIND_MAX 256
// kind "" with `queued` = waiting for the probe; "?" = the probe failed.
static struct { unsigned hash; char kind[4]; int queued; char *url; } kinds[KIND_MAX];
static int nKinds, prober;

static unsigned urlHash(const char *s) {
  unsigned h = 2166136261u;
  for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
  return h ? h : 1;
}

// The entry for `url`, made when missing (the oldest goes when full); the caller
// holds the lock.
static int kindSlot(const char *url, int make) {
  unsigned h = urlHash(url);
  int i;
  for (i = 0; i < nKinds; i++) if (kinds[i].hash == h) return i;
  if (!make) return -1;
  if (nKinds == KIND_MAX) {
    free(kinds[0].url);
    memmove(kinds, kinds + 1, (KIND_MAX - 1) * sizeof *kinds);
    nKinds--;
  }
  memset(&kinds[nKinds], 0, sizeof *kinds);
  kinds[nKinds].hash = h;
  return nKinds++;
}

static void kindSet(const char *url, const char *kind) {
  int i;
  pthread_mutex_lock(&lock);
  i = kindSlot(url, 1);
  snprintf(kinds[i].kind, sizeof kinds[i].kind, "%s", kind);
  kinds[i].queued = 0; free(kinds[i].url); kinds[i].url = NULL;
  pthread_mutex_unlock(&lock);
}

static void *probe(void *unused) {
  (void)unused;
  for (;;) {
    char *url = NULL;
    int i;
    pthread_mutex_lock(&lock);
    // Newest first: the rows on screen now, not the ones scrolled past.
    for (i = nKinds - 1; i >= 0; i--)
      if (kinds[i].queued && kinds[i].url) { url = kinds[i].url; kinds[i].url = NULL; break; }
    if (!url) { prober = 0; pthread_mutex_unlock(&lock); return NULL; }
    pthread_mutex_unlock(&lock);
    { long size = 0; const char *charset = "";
      char *raw = net_download_bin(url, 15, &size);
      char *body = raw ? subcharset_utf8(raw, size, "", &charset) : NULL;
      kindSet(url, body ? bodyKind(body) : "?");
      free(raw); free(body); }
    free(url);
  }
}

const char *subtitle_kind(const char *url, int fetch) {
  static char out[4];
  const char *k;
  int i;
  if (!url || !*url) return NULL;
  if ((k = urlKind(url))) return k;
  pthread_mutex_lock(&lock);
  i = kindSlot(url, fetch);
  out[0] = 0;
  if (i >= 0 && kinds[i].kind[0] && strcmp(kinds[i].kind, "?")) snprintf(out, sizeof out, "%s", kinds[i].kind);
  else if (i >= 0 && fetch && !kinds[i].kind[0] && !kinds[i].queued) {
    kinds[i].queued = 1; kinds[i].url = strdup(url);
    if (!prober) {
      pthread_t t;
      prober = 1;
      if (pthread_create(&t, NULL, probe, NULL) == 0) pthread_detach(t); else prober = 0;
    }
  }
  pthread_mutex_unlock(&lock);
  return out[0] ? out : NULL;
}

typedef struct { char url[1400]; char language[8]; unsigned g; } Request;

// Frees the loaded cues; the caller holds the lock.
static void dropCues(void) { free(cues); free(reach); cues=NULL; reach=NULL; nCues=0; }

static void *download(void *u) {
  Request *p=u; long size=0; const char *charset="";
  char *raw=net_download_bin(p->url,20,&size),*body=raw?subcharset_utf8(raw,size,p->language,&charset):NULL;
  SubtitleCue *v=NULL; double *r=NULL;
  int n=body?subtitle_parse(body,&v):0,i;
  if(body)kindSet(p->url,bodyKind(body));
  free(raw);free(body);
  if(n&&(r=malloc((size_t)n*sizeof *r))!=NULL)
    for(i=0;i<n;i++)r[i]=i&&r[i-1]>v[i].end?r[i-1]:v[i].end;
  else if(n){free(v);v=NULL;n=0;}
  pthread_mutex_lock(&lock);
  if(p->g==generation&&on){dropCues();cues=v;reach=r;nCues=n;v=NULL;r=NULL;}
  pthread_mutex_unlock(&lock);
  free(v);free(r);
  printf("[subtitle] OpenSubtitles: %d blocks, %s%s\n",n,charset[0]?charset:"no body",n?"":" (failed)");
  fflush(stdout);
  free(p);return NULL;
}

void subtitle_load(const char *url,const char *language) {
  Request *p; pthread_t thread;
  if(!url||!*url)return;
  p=calloc(1,sizeof *p);if(!p)return;
  pthread_mutex_lock(&lock);on=1;p->g=++generation;dropCues();pthread_mutex_unlock(&lock);
  snprintf(p->url,sizeof p->url,"%s",url);
  snprintf(p->language,sizeof p->language,"%s",language?language:"");
  if(pthread_create(&thread,NULL,download,p)==0)pthread_detach(thread);else free(p);
}

void subtitle_off(void) {
  pthread_mutex_lock(&lock);on=0;generation++;dropCues();pthread_mutex_unlock(&lock);
}

unsigned subtitle_ready(void) {
  unsigned g;
  pthread_mutex_lock(&lock);g=on&&nCues>0?generation:0;pthread_mutex_unlock(&lock);
  return g;
}

int subtitle_times(unsigned g,double **starts,double **ends) {
  int i,n=0;
  *starts=*ends=NULL;
  pthread_mutex_lock(&lock);
  if(g&&g==generation&&on&&nCues>0){
    *starts=malloc((size_t)nCues*sizeof **starts);*ends=malloc((size_t)nCues*sizeof **ends);
    if(*starts&&*ends){for(i=0;i<nCues;i++){(*starts)[i]=cues[i].start;(*ends)[i]=cues[i].end;}n=nCues;}
  }
  pthread_mutex_unlock(&lock);
  if(!n){free(*starts);free(*ends);*starts=*ends=NULL;}
  return n;
}

// The lines stay sorted: a positive scale and one offset keep their order, and
// move reach[] exactly as they move the ends it was taken from.
int subtitle_retime(unsigned g,double scale,double offset) {
  int i,done=0;
  if(scale<=0)return 0;
  pthread_mutex_lock(&lock);
  if(g&&g==generation&&on&&retimed!=g){
    for(i=0;i<nCues;i++){
      cues[i].start=cues[i].start*scale+offset;cues[i].end=cues[i].end*scale+offset;
      reach[i]=reach[i]*scale+offset;
    }
    retimed=g;done=1;
  }
  pthread_mutex_unlock(&lock);
  return done;
}

// Every cue on screen at `t`, oldest first. ASS files overlap cues all the
// time (a sign over the dialogue) and repeat the same line on several layers
// for its outline or shadow; a repeat in the same place is kept once.
// A POSITIVE delay shows the subtitle LATER, as the sheet's "Later" chip and mpv's
// sub-delay say: the cue for t plays at t + delay, so the one due now is t - delay.
int subtitle_shown(double posSeg,int delayMs,SubtitleCue *out,int max) {
  int lo=0,hi,last,i,k,hits[16],nHits=0; double t=posSeg-(double)delayMs/1000.0;
  if(!out||max<=0)return 0;
  if(max>16)max=16;
  pthread_mutex_lock(&lock);
  // The last cue that has started by t.
  hi=nCues-1;last=-1;
  while(lo<=hi){int m=(lo+hi)/2;if(cues[m].start<=t){last=m;lo=m+1;}else hi=m-1;}
  for(i=last;i>=0&&reach[i]>=t&&nHits<max;i--){
    const SubtitleCue *c=&cues[i];
    if(c->end<t)continue;
    for(k=0;k<nHits;k++){
      const SubtitleCue *h=&cues[hits[k]];
      if(!strcmp(h->text,c->text)&&h->align==c->align&&h->positioned==c->positioned&&
         h->x==c->x&&h->y==c->y)break;
    }
    if(k==nHits)hits[nHits++]=i;
  }
  for(k=0;k<nHits;k++)out[k]=cues[hits[nHits-1-k]];
  pthread_mutex_unlock(&lock);return nHits;
}

// The same cues as one block of text, one per line.
int subtitle_text(double posSeg,int delayMs,char *dst,size_t size) {
  SubtitleCue shown[8]; size_t used=0;
  int n,k;
  if(!dst||!size)return 0;dst[0]=0;
  n=subtitle_shown(posSeg,delayMs,shown,8);
  for(k=0;k<n;k++){
    int w=snprintf(dst+used,size-used,"%s%s",used?"\n":"",shown[k].text);
    if(w<0||(size_t)w>=size-used)break;
    used+=(size_t)w;
  }
  return n>0;
}
