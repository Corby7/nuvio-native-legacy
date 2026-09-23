#include "tracks.h"
#include "player.h"
#include "video.h"
#include "addons.h"
#include "gfx.h"
#include "text.h"
#include "anim.h"
#include "layout.h"
#include "subtitle.h"
#include "settings.h"
#include "streams.h"
#include "tabs.h"
#include "catalog.h"
#include "detail.h"
#include "data.h"
#include <sys/stat.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

// The panel's measurements live in layout.h under NV_TRK_*.

static int is_open;
static float anim;
// Which EXTERNAL subtitle (OpenSubtitles) is in force, as an index into the
// combined list — or -1 when the active one is embedded or there is none.
//
// This lives here and not in video.c because the pipeline does not return that
// information: video_subtitle_external sends setSubtitleSource with the URL and
// video.c's current-subtitle index stays untouched, still pointing at the
// EMBEDDED subtitle from before. Without this variable, choosing an
// OpenSubtitles subtitle put the "active" mark on another row (or on "Off") and
// the sheet reopened with the focus in the wrong place — the right subtitle
// played, only the sheet lied about which it was.
static int subExternal = -1;

// The automatic selection's state, declared here because tracks_reset (just
// below) clears it. See the block that follows for what it does.
#define TRK_AUTO_MS 8000
static int autoDone;
static Uint32 autoSince;
// Whether this playback has read its show's remembered language yet (below).
static int memRead;

// Called when a new playback session starts: the external subtitle belongs to
// the session, not to the device. Without this the next title would open the
// sheet marking as active a subtitle that was not chosen for it.
void tracks_reset(void) {
  subExternal = -1; is_open = 0; subtitle_off();
  autoDone = 0; autoSince = 0;
  memRead = 0;
}

// --- AUTOMATIC SELECTION -----------------------------------------------------
//
// Until this existed NOTHING ever selected a subtitle: apply(), below, was the
// only caller of video_choose_subtitle and subtitle_load in the whole app, and it
// only runs when someone walks into this sheet and presses OK. Every title on
// every start played with the subtitles off, which is what "subtitles are not
// really a thing on legacy" meant.
//
// TWO LISTS, ARRIVING AT DIFFERENT TIMES. The file's own tracks come with
// sourceInfo, and their CODEC — the only thing that says whether a track is text
// or a picture — comes later still, from the MKV header read. The addon's come
// from the network, seconds after that. So this runs every frame and decides on
// the first one where there is something worth deciding, or gives up at the
// deadline; deciding at a fixed instant would mean deciding with half the
// information on half the titles.
// THE DECISION LOG: one line per playback in subtitle-auto.log, in the data
// folder — the one place on the TV that ssh can read, since the app's stdout goes
// to a private /tmp. The automatic choice depends on what the file and the addon
// report, and neither can be seen from the sofa; "no subtitle came on" has several
// different causes, and this line says which one it was.
static void autoLog(const char *outcome) {
  char path[600], line[1600], tracksBuf[900] = "";
  const CatItem *c = cat_item(player_index());
  const Stream *st = stream_n() > 0 ? stream_item(stream_current()) : NULL;
  int t = 0, e = 0, i, n = video_n_subtitle();
  size_t k = 0;
  struct stat sb;
  time_t now = time(NULL);
  struct tm lt;
  FILE *f;
  player_episode_current(&t, &e);
  for (i = 0; i < n && k + 80 < sizeof tracksBuf; i++) {
    const VideoTrack *v = video_subtitle(i);
    k += (size_t)snprintf(tracksBuf + k, sizeof tracksBuf - k, "%s[%s/%s/%s]", i ? " " : "",
                          v && v->language[0] ? v->language : "-",
                          v && v->codec[0] ? v->codec : "-", v ? v->label : "?");
  }
  snprintf(line, sizeof line,
           "%s S%dE%d | pref %d | file %s | embedded %d: %s | addon %d%s | %s",
           c ? c->title : "?", t, e, settings_subtitle_pref(),
           st && st->file[0] ? st->file : "?", n, n ? tracksBuf : "none",
           addons_n_subtitles(), addons_subtitles_busy() ? " (still searching)" : "",
           outcome);
  printf("[subtitle] auto: %s\n", line);
  fflush(stdout);
  if (!data_path(path, sizeof path, "subtitle-auto.log")) return;
  // Kept small: it answers "what happened on the last few titles", not a history.
  if (stat(path, &sb) == 0 && sb.st_size > 64L * 1024L) remove(path);
  if (!(f = fopen(path, "a"))) return;
  localtime_r(&now, &lt);
  { char stamp[32]; strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &lt);
    fprintf(f, "%s | %s\n", stamp, line); }
  fclose(f);
}

// --- THE LANGUAGE REMEMBERED PER SHOW ------------------------------------------
//
// What the viewer picks in the sheet — a language, or Off — is kept per SHOW
// (per film for a film), in subtitle-shows.txt: "<imdb id> <language name>". The
// next episode then starts in the language they chose for this series, not in the
// device-wide default: English subtitles on one show and none on another is a
// perfectly ordinary household. The web app keeps the same per-title language.
//
// The LANGUAGE, not the track: episode files differ in their tracks, and the
// release that carried "English SDH" last week may carry only "English" now.
static const char LANG_OFF[] = "Off";
static const char LANG_UNKNOWN[] = "Unknown";
#define MEM_FILE "subtitle-shows.txt"
#define MEM_MAX  300
// This playback's show and what it remembers, read once per playback: the sheet's
// language order asks every frame, and the file does not change under us.
static char memShow[24], memLang[32];

static void showKey(char *dst, size_t size) {
  const CatItem *c = cat_item(player_index());
  const char *id = c ? c->imdb : "";
  snprintf(dst, size, "%.*s", (int)strcspn(id, ":"), id);
}

// The remembered language for this playback's show, or "" for none.
static const char *memLanguage(void) {
  char path[600], line[96];
  FILE *f;
  if (memRead) return memLang;
  memRead = 1;
  memLang[0] = 0;
  showKey(memShow, sizeof memShow);
  if (!memShow[0] || !data_path(path, sizeof path, MEM_FILE) || !(f = fopen(path, "r")))
    return memLang;
  while (fgets(line, sizeof line, f)) {
    char id[24], lang[32];
    line[strcspn(line, "\r\n")] = 0;
    // The name can hold a space ("Portuguese (BR)"), so it is the rest of the line.
    if (sscanf(line, "%23s %31[^\n]", id, lang) == 2 && !strcmp(id, memShow))
      snprintf(memLang, sizeof memLang, "%s", lang);   // the last line for a show wins
  }
  fclose(f);
  return memLang;
}

// Records `lang` for this playback's show: the show's old line goes, the new one
// is appended, and the oldest shows fall off past MEM_MAX.
static void memRemember(const char *lang) {
  char path[600], tmp[620], lines[MEM_MAX][96];
  int n = 0, i, from;
  FILE *f;
  memLanguage();
  if (!memShow[0] || !lang || !lang[0] || !strcmp(lang, LANG_UNKNOWN)) return;
  snprintf(memLang, sizeof memLang, "%s", lang);
  if (!data_path(path, sizeof path, MEM_FILE)) return;
  if ((f = fopen(path, "r"))) {
    char line[96];
    size_t k = strlen(memShow);
    while (fgets(line, sizeof line, f) && n < MEM_MAX) {
      line[strcspn(line, "\r\n")] = 0;
      if (!line[0] || (!strncmp(line, memShow, k) && line[k] == ' ')) continue;
      snprintf(lines[n++], sizeof lines[0], "%s", line);
    }
    fclose(f);
  }
  from = n >= MEM_MAX ? n - MEM_MAX + 1 : 0;
  snprintf(tmp, sizeof tmp, "%s.tmp", path);
  if (!(f = fopen(tmp, "w"))) return;
  for (i = from; i < n; i++) fprintf(f, "%s\n", lines[i]);
  fprintf(f, "%s %s\n", memShow, lang);
  fclose(f);
  rename(tmp, path);
}

// A track that is a PICTURE (PGS, VobSub, DVB) cannot be restyled on this TV, and
// on a file whose only subtitles are pictures the addon's text subtitle is the
// better answer. An UNKNOWN codec is NOT treated as a picture: the codec comes
// from the MKV header read, so every track of an MP4 has none — and MP4 carries
// text subtitles only. Skipping unknowns is what left the file's own English
// track off on every MP4.
static int embeddedBitmap(const VideoTrack *t) {
  return t && (strstr(t->codec, "PGS") || strstr(t->codec, "VOBSUB") ||
               strstr(t->codec, "DVBSUB"));
}

// Whether a track in language `code` is the one wanted: the show's remembered
// language by NAME when there is one, else the Settings row's group.
static int wanted(const char *code, const char *remembered, int group) {
  if (remembered[0]) return code && code[0] && !strcmp(video_language_name(code), remembered);
  return addons_language_group(code) == group;
}

void tracks_auto(Uint32 now) {
  // 0 off, 1 automatic, 2 Portuguese, 3 English. AUTOMATIC IS ENGLISH: it used to
  // mean Portuguese first, a leftover from the app's first owner that handed an
  // English viewer a Portuguese download whenever the addon had one.
  int pref = settings_subtitle_pref();
  int group = pref == 2 ? 0 : 1;
  int embedded = video_n_subtitle(), i, expired;
  // What the viewer chose for this show last time outranks the device default,
  // Off included. If that language is not on offer this time, the default takes
  // over at the deadline rather than leaving the episode without any.
  const char *memory = memLanguage();
  char remembered[32];

  if (autoDone) return;
  if (!strcmp(memory, LANG_OFF)) { autoDone = 1; autoLog("remembered Off for this show"); return; }
  if (!autoSince) autoSince = now ? now : 1;
  expired = now - autoSince >= TRK_AUTO_MS;
  if (memory[0] && !expired) snprintf(remembered, sizeof remembered, "%s", memory);
  else {
    remembered[0] = 0;
    if (pref == 0) { autoDone = 1; return; }
  }

  // THE FILE'S OWN FIRST, before any download. It needs no network, and the
  // pipeline keeps it in sync with its own clock.
  //
  // While the MKV header read is still out, a track's codec is not known yet and
  // it could turn out to be a picture — so hold off a moment, unless the deadline
  // has come, rather than pick one that silently shows nothing.
  if (!expired && embedded > 0) {
    int known = 0;
    for (i = 0; i < embedded; i++)
      if (video_subtitle(i) && video_subtitle(i)->codec[0]) { known = 1; break; }
    if (!known && stream_n() > 0 && strstr(stream_item(stream_current())->file, ".mkv"))
      return;
  }
  for (i = 0; i < embedded; i++) {
    const VideoTrack *t = video_subtitle(i);
    if (!t || !wanted(t->language, remembered, group) || embeddedBitmap(t)) continue;
    video_choose_subtitle(i); subtitle_off(); subExternal = -1;
    { char o[96]; snprintf(o, sizeof o, "chose embedded %d%s", i,
                           remembered[0] ? " (remembered for this show)" : ""); autoLog(o); }
    autoDone = 1; return;
  }

  // Then the addon's. Its search is still out on most starts, so wait for it.
  if (!expired && addons_subtitles_busy()) return;
  for (i = 0; i < addons_n_subtitles(); i++) {
    const Subtitle *l = addons_subtitle(i);
    if (!l || !wanted(l->language, remembered, group)) continue;
    video_choose_subtitle(-1); subtitle_load(l->url);
    subExternal = embedded + i;
    { char o[128]; snprintf(o, sizeof o, "chose addon %d (%s)%s", i, l->language,
                            remembered[0] ? " (remembered for this show)" : ""); autoLog(o); }
    autoDone = 1; return;
  }

  // Nothing yet. Keep looking until the deadline — the addon list can still land
  // — and then stop, so the search does not run for the whole film. The Settings
  // language does not fall back to another: asked for English, being given
  // Portuguese is an answer to a question nobody put.
  if (expired) {
    autoDone = 1;
    autoLog(memory[0] ? "nothing selected (remembered language not on offer either)"
                      : "nothing selected");
  }
}

// --- THE SHEET'S STATE ---------------------------------------------------------
//
// SEPARATE SHEETS: audio is one list; subtitles has the two tabs. The TCL's own
// SubtitleSelectionOverlay is the reference for keeping them apart — comparing
// audio and subtitle at once was a case nobody asked for.
enum { MODE_AUDIO, MODE_SUBTITLE };
enum { TAB_TRACKS, TAB_STYLE };
// Where the D-pad is. The tab row is shared; the rest belongs to one layout each.
// Audio has only its list, which is Z_SUBSEL's list held permanently open.
enum { Z_TABS, Z_LANGSEL, Z_SUBSEL, Z_TILE, Z_CHIP };
static int mode, tab, zone;
// Which select's list is unfolded: 0 none, 1 Language, 2 Subtitle. An open list
// owns every key until OK or BACK folds it.
enum { OPEN_NONE, OPEN_LANG, OPEN_SUB };
static int openSel;
// The CHOSEN language, held BY NAME, not by index. The list is rebuilt from what
// the pipeline and the addon have reported so far, and the addon's answer lands
// seconds after the sheet can be open: a new language sorted in above it would
// otherwise swap the choice for a different language under you.
static char langKey[32];
// The cursors inside the open lists, and how far each list is scrolled — in rows,
// not pixels: the pitch is fixed.
static int langCursor, optFocus, langScroll, optScroll;
static int tileFocus, chipFocus;
// The Style bar's own curve, so the switch between the two layouts is a crossfade
// rather than a cut.
static float styleAnim;

// --- LANGUAGES -----------------------------------------------------------------
//
// The Language select's list. "Off" leads, then the languages the Settings row prefers,
// then the rest alphabetically, and "Unknown" — tracks the file never tagged —
// sinks to the bottom rather than landing in the middle under U.
#define LANG_MAX 32
#define OPT_MAX  (NV_TRACK_MAX + SUB_MAX)
typedef struct { char name[32]; int count, active, rank; } Lang;
static Lang langs[LANG_MAX];
static int nLangs;

static int nSubtitles(void) { return video_n_subtitle() + addons_n_subtitles(); }

// The playing row as an index into the combined list, -1 when off.
static int subActive(void) {
  return subExternal >= 0 ? subExternal : video_subtitle_current();
}

// The language of row `i` of the combined list — embedded first, then addon — by
// the same name table the track labels are written with, so the column and the
// labels cannot disagree about what a code is called.
static void subLanguage(int i, char *dst, size_t size) {
  int embedded = video_n_subtitle();
  const char *code = NULL;
  if (i < embedded) {
    const VideoTrack *t = video_subtitle(i);
    code = t ? t->language : NULL;
  } else {
    const Subtitle *l = addons_subtitle(i - embedded);
    code = l ? l->language : NULL;
  }
  snprintf(dst, size, "%s", code && *code ? video_language_name(code) : LANG_UNKNOWN);
}

// 0 and 1 are the Settings preference ("Automatic" is Portuguese then English, the
// order the addon search already uses); 2 is everything else; 3 is Unknown.
static int langRank(const char *name) {
  int pref = settings_subtitle_pref();
  int pt = !strncmp(name, "Portuguese", 10), en = !strcmp(name, "English");
  if (!strcmp(name, LANG_UNKNOWN)) return 3;
  if (memRead && !strcmp(name, memLang)) return 0;
  if (pref == 2 && pt) return 0;
  if (pref == 3 && en) return 0;
  if (pref == 1 && en) return 0;
  return 2;
}

static int langBefore(const Lang *l, const Lang *r) {
  if (l->rank != r->rank) return l->rank < r->rank;
  return strcasecmp(l->name, r->name) < 0;
}

// Rebuilt on every key and every frame: it is at most a couple of dozen rows,
// and a cached copy is one more thing that can go stale when the addon lands.
static void buildLangs(void) {
  int n = nSubtitles(), act = subActive(), i, j;
  snprintf(langs[0].name, sizeof langs[0].name, "%s", LANG_OFF);
  langs[0].count = 0; langs[0].active = act < 0; langs[0].rank = -1;
  nLangs = 1;
  for (i = 0; i < n; i++) {
    char name[32];
    subLanguage(i, name, sizeof name);
    for (j = 1; j < nLangs && strcmp(langs[j].name, name); j++) {}
    if (j == nLangs) {
      if (nLangs == LANG_MAX) continue;
      snprintf(langs[j].name, sizeof langs[j].name, "%s", name);
      langs[j].count = 0; langs[j].active = 0; langs[j].rank = langRank(name);
      nLangs++;
    }
    langs[j].count++;
    if (i == act) langs[j].active = 1;
  }
  for (i = 2; i < nLangs; i++) {
    Lang v = langs[i];
    for (j = i; j > 1 && langBefore(&v, &langs[j - 1]); j--) langs[j] = langs[j - 1];
    langs[j] = v;
  }
}

static int langChosen(void) {
  int i;
  for (i = 0; i < nLangs; i++) if (!strcmp(langs[i].name, langKey)) return i;
  return 0;
}

static void setLang(int i) {
  if (i < 0) i = 0;
  if (i >= nLangs) i = nLangs - 1;
  snprintf(langKey, sizeof langKey, "%s", langs[i].name);
}

// The Subtitle select's list: the rows of the combined list in language `li`, in
// list order — the file's own first, then the addon's.
static int langOptions(int li, int *out) {
  int n = nSubtitles(), i, k = 0;
  if (li <= 0 || li >= nLangs) return 0;
  for (i = 0; i < n && k < OPT_MAX; i++) {
    char name[32];
    subLanguage(i, name, sizeof name);
    if (!strcmp(name, langs[li].name)) out[k++] = i;
  }
  return k;
}

// The option in language `li` that is playing, or -1.
static int activeOption(int li) {
  int opts[OPT_MAX], n = langOptions(li, opts), act = subActive(), k;
  for (k = 0; k < n; k++) if (opts[k] == act) return k;
  return -1;
}

// --- A TRACK ROW'S WORDING -----------------------------------------------------
//
// The language select has already said the language, so a subtitle row never
// repeats it. What is left to say is what actually tells two rows apart: for the
// file's own tracks, the name the file gives them and whether they are text or a
// picture; for the addon's, the release they were cut for and whether that is
// the release playing.

// The CodecID, shortened for reading. `bitmap` comes back 1 for the formats that
// are PICTURES: those cannot be restyled, moved or resized, so every Style setting
// silently does nothing on one — the row says so before it is chosen.
static const char *codecShort(const char *codec, int *bitmap) {
  *bitmap = 0;
  if (!codec || !codec[0]) return NULL;
  if (strstr(codec, "PGS"))    { *bitmap = 1; return "PGS"; }
  if (strstr(codec, "VOBSUB")) { *bitmap = 1; return "VobSub"; }
  if (strstr(codec, "DVBSUB")) { *bitmap = 1; return "DVB"; }
  if (strstr(codec, "WEBVTT")) return "WebVTT";
  if (strstr(codec, "ASS"))    return "ASS";
  if (strstr(codec, "SSA"))    return "SSA";
  if (strstr(codec, "UTF8"))   return "SRT";
  return NULL;
}

// What an embedded label says BESIDES its language. The labels were written for
// one mixed list, so they lead with it ("English  ·  SDH").
static const char *labelRest(const char *label, const char *lang) {
  static const char SEP[] = "  \xc2\xb7  ";
  size_t n = strlen(lang);
  if (strncmp(label, lang, n)) return label;
  if (!label[n]) return "";
  if (!strncmp(label + n, SEP, sizeof SEP - 1)) return label + n + sizeof SEP - 1;
  return label;
}

// Case-blind substring test. strcasestr is a GNU extension the device's glibc
// only declares under _GNU_SOURCE.
static int hasWord(const char *s, const char *w) {
  size_t n = strlen(w);
  for (; *s; s++) if (!strncasecmp(s, w, n)) return 1;
  return 0;
}

// The release GROUP: the tag after the last '-' of the name, before the
// extension — "ETHEL" in "Silo.S02E04.2160p.WEB.h265-ETHEL.mkv". It is the one
// token that identifies the encode. Two files from the same group share cuts and
// frame rate, which is what decides whether a subtitle stays in sync; resolution
// and source do not, since a group's 1080p and 2160p come from the same master.
static int releaseGroup(const char *name, char *dst, size_t size) {
  const char *slash = strrchr(name, '/'), *dash, *end;
  size_t k = 0;
  if (slash) name = slash + 1;
  end = strrchr(name, '.');
  if (!end) end = name + strlen(name);
  for (dash = end; dash > name && dash[-1] != '-'; dash--) {}
  if (dash == name) return 0;
  for (; dash < end && k + 1 < size; dash++) {
    char c = *dash;
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) break;
    dst[k++] = c;
  }
  dst[k] = 0;
  return k >= 2;
}

static int matchesPlaying(const char *release) {
  const Stream *st = stream_n() > 0 ? stream_item(stream_current()) : NULL;
  char a[32], b[32];
  if (!st || !release || !release[0] || !st->file[0]) return 0;
  if (!releaseGroup(release, a, sizeof a) || !releaseGroup(st->file, b, sizeof b)) return 0;
  return !strcasecmp(a, b);
}

// The main line and the detail line for row `i` of the combined list. `ordinal`
// is its place among its language's rows, for a track that has no name at all.
static void optionText(int i, int ordinal, char *main, size_t mSize,
                       char *sub, size_t sSize) {
  int embedded = video_n_subtitle();
  char lang[32];
  subLanguage(i, lang, sizeof lang);
  if (i < embedded) {
    const VideoTrack *t = video_subtitle(i);
    const char *rest = t ? labelRest(t->label, lang) : "";
    int bitmap;
    const char *fmt = codecShort(t ? t->codec : NULL, &bitmap);
    // The names that are a KIND of subtitle are worth a word of explanation; the
    // rest ("Full", "Commentary", a translator's name) say what they say.
    if (!strcasecmp(rest, "sdh") || hasWord(rest, "sdh"))
      snprintf(main, mSize, "SDH  \xc2\xb7  with sound descriptions");
    else if (!strcasecmp(rest, "forced") || hasWord(rest, "forced"))
      snprintf(main, mSize, "Forced  \xc2\xb7  foreign dialogue only");
    else if (rest[0]) snprintf(main, mSize, "%s", rest);
    else snprintf(main, mSize, "Track %d", ordinal + 1);
    snprintf(sub, sSize, "Embedded%s%s%s", fmt ? "  \xc2\xb7  " : "", fmt ? fmt : "",
             bitmap ? "  \xc2\xb7  image, can't be restyled" : "");
  } else {
    const Subtitle *l = addons_subtitle(i - embedded);
    if (l && l->release[0]) snprintf(main, mSize, "%s", l->release);
    else snprintf(main, mSize, "Download %d", ordinal + 1);
    snprintf(sub, sSize, "OpenSubtitles%s",
             l && matchesPlaying(l->release) ? "  \xc2\xb7  matches your file" : "");
  }
}

// --- STYLE ---------------------------------------------------------------------
//
// Eight tiles, one per setting. Every change applies AT ONCE — the viewer has to
// SEE the subtitle change in order to choose it.
#define FX_N_TILE 8
enum { ST_SIZE, ST_FONT, ST_COLOUR, ST_OPACITY, ST_BACKGROUND, ST_POSITION,
       ST_EDGE, ST_DELAY };
static const char *const ST_LABEL[FX_N_TILE] = {
  "SIZE", "FONT", "COLOUR", "OPACITY", "BACKGROUND", "HEIGHT", "EDGE", "DELAY"
};
static const char *const ST_BACKGROUND_LABEL[5] = { "None", "Dark 25%", "Dark 50%",
                                                    "Dark 75%", "Solid" };
static const char *const ST_EDGE_LABEL[3] = { "None", "Outline", "Shadow" };
static const char *const ST_OPACITY_LABEL[4] = { "100%", "75%", "50%", "25%" };
// The three settings with too many values to lay out as chips get a pair of
// steppers instead, worded as what they do rather than as "-" and "+".
static const char *const ST_STEP_LABEL[FX_N_TILE][2] = {
  [ST_SIZE]     = { "Smaller", "Larger" },
  [ST_POSITION] = { "Lower",   "Higher" },
  [ST_DELAY]    = { "Earlier", "Later"  },
};
#define ST_DEFAULT ((VideoSubtitleStyle){ 120, 0, 0, 3, 1, 0, 0, TXT_FAMILY_INTER })

static int tileStepper(int t) { return t == ST_SIZE || t == ST_POSITION || t == ST_DELAY; }

// How many choices a tile has, the RESET chip not included.
static int tileChoices(int t) {
  switch (t) {
    case ST_FONT:       return TXT_FAMILY_N;
    case ST_COLOUR:     return VIDEO_SUB_NCOLORS;
    case ST_OPACITY:    return 4;
    case ST_BACKGROUND: return 5;
    case ST_EDGE:       return 3;
    default:            return 2;
  }
}

static const char *choiceLabel(int t, int c) {
  switch (t) {
    case ST_FONT:       return TXT_FAMILIES_LABEL[c];
    case ST_COLOUR:     return VIDEO_SUB_COLORS_LABEL[c];
    case ST_OPACITY:    return ST_OPACITY_LABEL[c];
    case ST_BACKGROUND: return ST_BACKGROUND_LABEL[c];
    case ST_EDGE:       return ST_EDGE_LABEL[c];
    default:            return ST_STEP_LABEL[t][c];
  }
}

// The value a chip tile holds now, or -1 for a stepper tile.
static int tileCurrent(int t) {
  const VideoSubtitleStyle *e = player_sub_style();
  switch (t) {
    case ST_FONT:       return e->family >= 0 && e->family < TXT_FAMILY_N ? e->family : 0;
    case ST_COLOUR:     return e->color % VIDEO_SUB_NCOLORS;
    case ST_OPACITY:    return e->opacity > 3 ? 3 : e->opacity;
    case ST_BACKGROUND: return e->background > 4 ? 4 : e->background;
    case ST_EDGE:       return e->border > 2 ? 2 : e->border;
    default:            return -1;
  }
}

static void tileValue(int t, char *dst, size_t size) {
  const VideoSubtitleStyle *e = player_sub_style();
  switch (t) {
    case ST_SIZE: snprintf(dst, size, "%d%%", e->size); break;
    // The uMS takes -3..4 and the overlay moves 48px a step. Said in pixels from
    // the default, because "position 5 of 8" does not tell you which way is up.
    case ST_POSITION:
      if (e->position == 3) snprintf(dst, size, "Default");
      else snprintf(dst, size, "%s%d px", e->position > 3 ? "+" : "\xe2\x88\x92",
                    (e->position > 3 ? e->position - 3 : 3 - e->position) * 48);
      break;
    case ST_DELAY:
      if (!e->delayMs) snprintf(dst, size, "0.0 s");
      else snprintf(dst, size, "%s%.2f s", e->delayMs > 0 ? "+" : "\xe2\x88\x92",
                    (e->delayMs > 0 ? e->delayMs : -e->delayMs) / 1000.0f);
      break;
    default: snprintf(dst, size, "%s", choiceLabel(t, tileCurrent(t))); break;
  }
}

// Size, Position and Delay STOP at their ends rather than wrapping: on a stepper
// worded "Larger", one more press landing on the smallest size is a surprise.
static void stepTile(int t, int dir) {
  VideoSubtitleStyle *e = player_sub_style();
  switch (t) {
    case ST_SIZE:
      e->size += 10 * dir;
      if (e->size > 200) e->size = 200;
      if (e->size < 50)  e->size = 50;
      break;
    case ST_POSITION:
      e->position += dir;
      if (e->position > 7) e->position = 7;
      if (e->position < 0) e->position = 0;
      break;
    // -5 s to +5 s in 250 ms steps. A smaller step would take dozens of presses
    // to get anywhere on a remote control.
    case ST_DELAY:
      e->delayMs += 250 * dir;
      if (e->delayMs >  5000) e->delayMs =  5000;
      if (e->delayMs < -5000) e->delayMs = -5000;
      break;
    default: break;
  }
  player_sub_style_changed();
}

static void setTile(int t, int v) {
  VideoSubtitleStyle *e = player_sub_style();
  switch (t) {
    case ST_FONT:       e->family = v; break;
    case ST_COLOUR:     e->color = v; break;
    case ST_OPACITY:    e->opacity = v; break;
    case ST_BACKGROUND: e->background = v; break;
    case ST_EDGE:       e->border = v; break;
    default: return;
  }
  player_sub_style_changed();
}

static void resetStyle(void) {
  *player_sub_style() = ST_DEFAULT;
  player_sub_style_changed();
}

// Where the chip row's focus lands on the way down: the value in force, so OK
// there is a no-op rather than a change; on a stepper, the forward one — the
// direction you want first is nearly always up.
static void enterChips(void) {
  int c = tileCurrent(tileFocus);
  chipFocus = c >= 0 ? c : 1;
  zone = Z_CHIP;
}

// --- OPENING AND CHOOSING ------------------------------------------------------

void tracks_open(void) { tracks_open_at(0); }

// Opens ALREADY ON THE SHEET the button asked for: pressing "subtitles" and
// landing on audio made the two buttons look like the same button.
//
// The subtitle sheet opens on Tracks with both selects folded, the cursor on the
// Subtitle select when a subtitle is on — switching to another version of the
// same language is the commonest errand — and on Language when none is.
void tracks_open_at(int col) {
  is_open = 1;
  mode = col == 1 ? MODE_SUBTITLE : MODE_AUDIO;
  tab = TAB_TRACKS;
  styleAnim = 0.0f;
  openSel = OPEN_NONE;
  langScroll = optScroll = 0;
  tileFocus = 0; chipFocus = 0;
  if (mode == MODE_AUDIO) {
    int n = video_n_audio();
    optFocus = video_audio_current();
    if (optFocus >= n) optFocus = n - 1;
    if (optFocus < 0) optFocus = 0;
    zone = Z_SUBSEL;
    return;
  }
  buildLangs();
  { int i, li = 0;
    for (i = 0; i < nLangs; i++) if (langs[i].active) { li = i; break; }
    setLang(li);
    zone = li > 0 ? Z_SUBSEL : Z_LANGSEL; }
}

int tracks_is_open(void) { return is_open; }

static void applySubtitle(int i) {
  int embedded = video_n_subtitle();
  // A choice made by hand ENDS the automatic one for this playback, "Off"
  // included: turning the subtitle off and having it come back a frame later is
  // the app arguing with the person using it.
  autoDone = 1;
  // ...and it is what this show will start with next time.
  { char lang[32];
    if (i < 0) snprintf(lang, sizeof lang, "%s", LANG_OFF);
    else subLanguage(i, lang, sizeof lang);
    memRemember(lang); }
  if (i < 0)             { video_choose_subtitle(-1); subtitle_off(); subExternal = -1; }
  else if (i < embedded) { video_choose_subtitle(i);  subtitle_off(); subExternal = -1; }
  else {
    const Subtitle *l = addons_subtitle(i - embedded);
    // Only mark as active if there was something to apply: without the URL the
    // uMS gets nothing, and the sheet would say "active" about nothing.
    if (l) { video_choose_subtitle(-1); subtitle_load(l->url); subExternal = i; }
  }
}

static int isBack(SDL_Keycode k) {
  return k == SDLK_AC_BACK || k == SDLK_ESCAPE || k == SDLK_BACKSPACE;
}
static int isOk(SDL_Keycode k) { return k == SDLK_RETURN || k == SDLK_KP_ENTER; }

// The Subtitle select exists only once a language is chosen that has tracks.
static int subSelShown(void) {
  int opts[OPT_MAX];
  return langChosen() > 0 && langOptions(langChosen(), opts) > 0;
}

static void eventAudio(SDL_Keycode k) {
  int n = video_n_audio();
  if (isBack(k)) { is_open = 0; return; }
  if (k == SDLK_UP   && optFocus > 0)     optFocus--;
  if (k == SDLK_DOWN && optFocus < n - 1) optFocus++;
  // Choosing closes the sheet: the choice is the whole errand.
  if (isOk(k) && n > 0) { autoDone = 1; video_choose_audio(optFocus); is_open = 0; }
}

static void eventLangList(SDL_Keycode k) {
  if (isBack(k)) { openSel = OPEN_NONE; return; }
  if (k == SDLK_UP   && langCursor > 0)          langCursor--;
  if (k == SDLK_DOWN && langCursor < nLangs - 1) langCursor++;
  if (!isOk(k)) return;
  openSel = OPEN_NONE;
  setLang(langCursor);
  // Off is the whole answer. A language turns on its track — the one already
  // playing if it is this language, else its first — and hands the cursor to the
  // Subtitle select just below, where its other versions are.
  if (langCursor == 0) { applySubtitle(-1); zone = Z_LANGSEL; return; }
  { int opts[OPT_MAX], n = langOptions(langCursor, opts);
    if (n > 0 && activeOption(langCursor) < 0) applySubtitle(opts[0]);
    zone = n > 0 ? Z_SUBSEL : Z_LANGSEL; }
}

static void eventSubList(SDL_Keycode k) {
  int opts[OPT_MAX], n = langOptions(langChosen(), opts);
  if (isBack(k)) { openSel = OPEN_NONE; return; }
  if (k == SDLK_UP   && optFocus > 0)     optFocus--;
  if (k == SDLK_DOWN && optFocus < n - 1) optFocus++;
  // Choosing the subtitle closes the sheet: it is the end of the errand.
  if (isOk(k) && optFocus < n) { applySubtitle(opts[optFocus]); openSel = OPEN_NONE; is_open = 0; }
}

static void eventTracks(SDL_Keycode k) {
  if (openSel == OPEN_LANG) { eventLangList(k); return; }
  if (openSel == OPEN_SUB)  { eventSubList(k);  return; }
  if (isBack(k)) { is_open = 0; return; }
  if (zone == Z_TABS) {
    if (k == SDLK_RIGHT) tab = TAB_STYLE;
    else if (k == SDLK_DOWN) zone = Z_LANGSEL;
    return;
  }
  if (k == SDLK_UP) { zone = zone == Z_SUBSEL ? Z_LANGSEL : Z_TABS; return; }
  if (k == SDLK_DOWN) { if (zone == Z_LANGSEL && subSelShown()) zone = Z_SUBSEL; return; }
  if (!isOk(k)) return;
  // OK unfolds the select with the cursor on what is in force, so a second OK
  // changes nothing.
  if (zone == Z_LANGSEL) {
    langCursor = langChosen(); langScroll = 0; openSel = OPEN_LANG;
  } else {
    int a = activeOption(langChosen());
    optFocus = a >= 0 ? a : 0; optScroll = 0; openSel = OPEN_SUB;
  }
}

static void eventStyle(SDL_Keycode k) {
  int nChips = tileChoices(tileFocus) + 1;    // + Reset to defaults
  if (zone == Z_TABS) {
    if (isBack(k)) { is_open = 0; return; }
    if (k == SDLK_LEFT) tab = TAB_TRACKS;
    else if (k == SDLK_DOWN || isOk(k)) zone = Z_TILE;
    return;
  }
  if (zone == Z_TILE) {
    if (isBack(k)) { is_open = 0; return; }
    if (k == SDLK_UP) zone = Z_TABS;
    else if (k == SDLK_LEFT  && tileFocus > 0)             tileFocus--;
    else if (k == SDLK_RIGHT && tileFocus < FX_N_TILE - 1) tileFocus++;
    else if (k == SDLK_DOWN || isOk(k)) enterChips();
    return;
  }
  // Z_CHIP
  if (isBack(k) || k == SDLK_UP) { zone = Z_TILE; return; }
  if (k == SDLK_LEFT  && chipFocus > 0)          chipFocus--;
  if (k == SDLK_RIGHT && chipFocus < nChips - 1) chipFocus++;
  if (isOk(k)) {
    if (chipFocus == nChips - 1) resetStyle();
    else if (tileStepper(tileFocus)) stepTile(tileFocus, chipFocus ? 1 : -1);
    else setTile(tileFocus, chipFocus);
  }
}

void tracks_event(const SDL_Event *e) {
  SDL_Keycode k;
  if (!is_open || e->type != SDL_KEYDOWN) return;
  k = e->key.keysym.sym;
  if (mode == MODE_AUDIO) { eventAudio(k); return; }
  buildLangs();
  if (tab == TAB_TRACKS) eventTracks(k); else eventStyle(k);
}

// The cursor on the Tracks / Style tabs, 0..1 — see tab_draw.
static float tabsLit;
void tracks_update(float dt, Uint32 now) {
  (void)now;
  { float t = zone == Z_TABS ? 1.0f : 0.0f;
    tabsLit = anim_spring(tabsLit, t, dt, t > tabsLit ? NV_SPRING_FOCUS : NV_SPRING_BLUR); }
  anim = anim_spring(anim, is_open ? 1.0f : 0.0f, dt, NV_SPRING_SCREEN);
  styleAnim = anim_spring(styleAnim, is_open && mode == MODE_SUBTITLE && tab == TAB_STYLE
                          ? 1.0f : 0.0f, dt, NV_SPRING_SCREEN);
}

float tracks_shown(void) { return anim < 0.01f ? 0.0f : anim; }

float tracks_style_shown(void) {
  float s = styleAnim * anim;
  return s < 0.01f ? 0.0f : s;
}


// --- DRAWING -------------------------------------------------------------------

// Keeps the cursor inside the window, moving the MINIMUM: only when it passes one
// of the edges. Always scrolling to centre would make the whole list move on
// every keypress, which on a D-pad is disorienting.
static void keepInView(int f, int n, int visible, int *scroll) {
  if (visible < 1) visible = 1;
  if (f < *scroll) *scroll = f;
  else if (f >= *scroll + visible) *scroll = f - visible + 1;
  if (*scroll > n - visible) *scroll = n - visible;
  if (*scroll < 0) *scroll = 0;
}

static void ringAround(GfxRect b, float radiusPx, float a) {
  float g = NV_RING_FOCUS;
  GfxRect r = { b.x - g, b.y - g, b.w + g * 2, b.h + g * 2 };
  gfx_rect(r, 0, GFX_RING_INSET, 0, g / r.h, 0, (radiusPx + g) / r.h,
           1, 1, 1, 0.96f * a);
}

// The playing mark: a dot, and ON beside it when there is room to say it.
// Returns the x it starts at, so what sits to its left can stop short of it.
static float onMark(float right, float cy, int focused, int word, float a) {
  int c = focused ? NV_TRK_FOCUS_INK : 245;
  float x = right, d = NV_TRK_DOT;
  if (word) {
    TxtLine on = txt_line(TXT_ERAIL_PILL, "ON", c, c, c, 255);
    x -= on.w;
    txt_draw_alpha(on, x, cy - on.h * 0.5f, a);
    x -= 10.0f;
  }
  x -= d;
  gfx_color((GfxRect){ x, cy - d * 0.5f, d, d }, 0.5f, c / 255.0f, c / 255.0f, c / 255.0f, a);
  return x;
}

// THE HEADER, measured off the sources sheet: the heading at NV_TRK_TITLE_Y with
// its count centred on the heading's capitals, and — on the subtitle sheet — the
// tabs on NV_TRK_TABS_Y. `x` is the column's left edge.
//
// `right` (0..1) is the Style tab's pull: there the panel has gone and the header
// stands alone over the picture, so it goes flush to the screen's right margin —
// heading and tabs each set right — and the count fades out: it counts tracks,
// and there are no tracks on that tab.
static void drawHeader(float x, int count, float right, float a) {
  const char *name = mode == MODE_SUBTITLE ? "Subtitles" : "Audio";
  TxtLine title = txt_line(TXT_PANEL_TITLE, name, 240, 241, 243, 255);
  float edge = NV_SCREEN_W - NV_TRK_PAD, ty = NV_TRK_TITLE_Y;
  float tx = x + (edge - title.w - x) * right;
  txt_draw_alpha(title, tx, ty, a);
  if (count >= 0 && right < 0.99f) {
    char n[48];
    float capT = txt_cap_inset(TXT_PANEL_TITLE), capC = txt_cap_inset(TXT_SRC_COUNT);
    float mid = ty + (capT + txt_baseline(TXT_PANEL_TITLE)) * 0.5f;
    float by = mid - (capC + txt_baseline(TXT_SRC_COUNT)) * 0.5f;
    if (mode == MODE_SUBTITLE)
      snprintf(n, sizeof n, "%d available%s", count,
               addons_subtitles_busy() ? "  \xc2\xb7  searching" : "");
    else
      snprintf(n, sizeof n, "%d track%s", count, count == 1 ? "" : "s");
    txt_draw_alpha(txt_line(TXT_SRC_COUNT, n, 132, 135, 142, 255),
                   tx + (float)title.w + 18.0f, by, a * (1.0f - right));
  }
  if (mode != MODE_SUBTITLE) return;

  { float tabsW = tab_width("Tracks") + tab_width("Style") - NV_TAB_GAP;
    float px = x + (edge - tabsW - x) * right;
    px += tab_draw(px, NV_TRK_TABS_Y, "Tracks", tab == TAB_TRACKS, tabsLit, a);
    tab_draw(px, NV_TRK_TABS_Y, "Style", tab == TAB_STYLE, tabsLit, a); }
}

static void quiet(const char *s, float x, float y, float w, float a) {
  txt_block(TXT_TRK_OPTSUB, s, 133, 134, 136, x + NV_TRK_SEL_PADX, y + 16.0f,
            w - NV_TRK_SEL_PADX * 2, 26.0f, a, 3);
}

// "value · tail" on one line, the tail in grey, the pair centred on `yc`. The
// value gives way first: a long release name is trimmed so the tail — the part
// that says what KIND of thing it is — always survives.
static void valueTail(TxtStyle vs, TxtStyle ts, const char *value, const char *tail,
                      float x, float yc, float room, int ink, int grey, float a) {
  TxtLine dot = txt_line(ts, "\xc2\xb7", grey, grey, grey, 255);
  TxtLine lt = txt_line(ts, tail && *tail ? tail : "", grey, grey, grey, 255);
  float tw = tail && *tail ? 12.0f * 2 + dot.w + lt.w : 0.0f;
  TxtLine lv = txt_line_trim(vs, value, ink, ink, ink, 255, room - tw > 60.0f ? room - tw : 60.0f);
  txt_draw_alpha(lv, x, yc - lv.h * 0.5f, a);
  if (tw > 0.0f) {
    txt_draw_alpha(dot, x + lv.w + 12.0f, yc - dot.h * 0.5f, a);
    txt_draw_alpha(lt, x + lv.w + 24.0f + dot.w, yc - lt.h * 0.5f, a);
  }
}

// THE DROPDOWNS ARE THE TITLE PAGE'S season picker (drawSeason and
// drawSeasonMenu in detail.c), so every dropdown in the app looks like one thing.
//
// The anchor: a full pill in #222 with a hair line at rest; focused, or with its
// menu down, the ground lifts to rgb(48,48,48) and the ring goes INSIDE the edge.
static void selectGround(GfxRect r, int focused, int open, float a) {
  float f = focused || open ? 1.0f : 0.0f;
  float luma = NV_DETWEB_REST + (NV_DETWEB_SEA_FOCUS_BG - NV_DETWEB_REST) * f;
  gfx_color(r, NV_RADIUS_PILL, luma, luma, luma, NV_PLR_DD_A * a);
  if (f < 0.99f)
    gfx_rect(r, 0, GFX_RING, 0, NV_DETWEB_SEA_BORDER / r.h, 0, NV_RADIUS_PILL,
             1, 1, 1, 0.10f * a);
  else
    gfx_rect(r, 0, GFX_RING_INSET, 0, NV_DETWEB_SEA_RING / r.h, 0, NV_RADIUS_PILL,
             1, 1, 1, 0.96f * a);
}

// An option under the cursor: a pill inverted to #f5f5f5; its type goes to #111.
static void optionGround(GfxRect r, float a) {
  gfx_color(r, NV_RADIUS_PILL, NV_DETWEB_FOCUS, NV_DETWEB_FOCUS, NV_DETWEB_FOCUS, a);
}

// A SELECT: what is in force, and the chevron that says OK opens it. No label —
// "English · 3 subtitles" says what the picker is for.
static void selectRow(GfxRect r, const char *value, const char *tail, int focused,
                      int open, float a) {
  selectGround(r, focused, open, a);
  valueTail(TXT_TRK_VALUE, TXT_TRK_LABEL, value, tail, r.x + NV_TRK_SEL_PADX,
            r.y + r.h * 0.5f, r.w - NV_TRK_SEL_PADX * 2 - 16.0f - NV_TRK_CHEV,
            255, 179, a);
  gfx_icon((GfxRect){ r.x + r.w - NV_TRK_SEL_PADX - NV_TRK_CHEV,
                      r.y + (r.h - NV_TRK_CHEV) * 0.5f, NV_TRK_CHEV, NV_TRK_CHEV },
           "chevron_down", 0.702f, 0.702f, 0.702f, a);
}

// The OPEN menu: a #222 plate with the title page's drop shadow, hung under the
// anchor at its width, in front of whatever is below. Returns the first option's
// rect; *vis is how many fit.
static GfxRect menuPlate(GfxRect anchor, int n, float rowH, float limit, int cursor,
                         int *scroll, int *vis, float a) {
  float top = anchor.y + anchor.h + NV_DETWEB_SEA_MENU_GAP;
  int fit = (int)((limit - top - NV_DETWEB_SEA_MENU_PADY * 2) / rowH);
  GfxRect box;
  float radius;
  if (fit < 1) fit = 1;
  *vis = n < fit ? n : fit;
  keepInView(cursor, n, *vis, scroll);
  box = (GfxRect){ anchor.x, top, anchor.w, NV_DETWEB_SEA_MENU_PADY * 2 + *vis * rowH };
  // Normalised to the height: 32px keeps the title page's proportion on a plate
  // this size rather than rounding it into a lozenge.
  radius = 32.0f / box.h;
  // NO DROP SHADOW here, unlike the title page's: GFX_SHADOW is only soft
  // outside its quad, and a quad the plate's size left it a hard copy 8px lower
  // whose corners showed square under the plate's round ones. Over a see-through
  // plate it would also darken the plate itself.
  gfx_color(box, radius, NV_DETWEB_REST, NV_DETWEB_REST, NV_DETWEB_REST, NV_PLR_DD_A * a);
  gfx_rect(box, 0, GFX_RING, 0, 1.0f / box.h, 0, radius, 1, 1, 1, 0.08f * a);
  return (GfxRect){ box.x + NV_DETWEB_SEA_MENU_PADX, box.y + NV_DETWEB_SEA_MENU_PADY,
                    box.w - NV_DETWEB_SEA_MENU_PADX * 2, rowH };
}

#define FX_ROW(op, i) ((GfxRect){ (op).x, (op).y + (i) * (op).h, (op).w, (op).h })

// The Language select's menu.
static void langList(GfxRect anchor, float limit, float a) {
  int i, vis;
  GfxRect op = menuPlate(anchor, nLangs, NV_TRK_LANG_H, limit, langCursor,
                         &langScroll, &vis, a);
  for (i = 0; i < vis; i++) {
    int c = langScroll + i, focused = c == langCursor;
    GfxRect r = FX_ROW(op, i);
    float right = r.x + r.w - NV_TRK_SEL_PADX, yc = r.y + r.h * 0.5f;
    char tail[24] = "";
    if (focused) optionGround(r, a);
    if (langs[c].active) right = onMark(right, yc, focused, 1, a) - 16.0f;
    if (c > 0) snprintf(tail, sizeof tail, "%d", langs[c].count);
    valueTail(TXT_TRK_OPT, TXT_TRK_OPTSUB, langs[c].name, tail,
              r.x + NV_TRK_SEL_PADX, yc, right - r.x - NV_TRK_SEL_PADX,
              focused ? 17 : 255, focused ? 90 : 133, a);
  }
}

// The Subtitle select's menu — and, with `audio`, the Audio sheet's list, which
// is the same menu held open under its heading.
static void optList(GfxRect anchor, float limit, int audio, float a) {
  int opts[OPT_MAX], n, act, i, vis;
  float rowH = audio ? NV_TRK_LANG_H : NV_TRK_OPT_H;
  GfxRect op;
  if (audio) { n = video_n_audio(); act = video_audio_current(); }
  else { n = langOptions(langChosen(), opts); act = subActive(); }
  if (!n) { quiet("No track available from this source.", anchor.x,
                  anchor.y + anchor.h, anchor.w, a); return; }
  op = menuPlate(anchor, n, rowH, limit, optFocus, &optScroll, &vis, a);
  for (i = 0; i < vis; i++) {
    int c = optScroll + i, focused = c == optFocus;
    int on = audio ? c == act : opts[c] == act;
    GfxRect r = FX_ROW(op, i);
    float right = r.x + r.w - NV_TRK_SEL_PADX, tx = r.x + NV_TRK_SEL_PADX;
    int ink = focused ? 17 : 255, grey = focused ? 90 : 133;
    if (focused) optionGround(r, a);
    if (on) right = onMark(right, r.y + r.h * 0.5f, focused, 1, a) - 16.0f;
    if (audio) {
      const VideoTrack *t = video_audio(c);
      TxtLine l = txt_line_trim(TXT_TRK_OPT, t && t->label[0] ? t->label : "Track",
                                ink, ink, ink, 255, right - tx);
      txt_draw_alpha(l, tx, r.y + (r.h - l.h) * 0.5f, a);
    } else {
      char main[128], sub[128];
      optionText(opts[c], c, main, sizeof main, sub, sizeof sub);
      txt_draw_alpha(txt_line_trim(TXT_TRK_OPT, main, ink, ink, ink, 255, right - tx),
                     tx, r.y + 14.0f, a);
      txt_draw_alpha(txt_line_trim(TXT_TRK_OPTSUB, sub, grey, grey, grey, 255, right - tx),
                     tx, r.y + 46.0f, a);
    }
  }
}

// THE SIDE PANEL: audio, and the subtitle sheet's Tracks tab, in the sources
// sheet's shell. It leaves the way it came — sliding right and fading — as the
// Style bar comes up.
static void drawPanel(float a, float away) {
  float slide = (1.0f - anim + away) * NV_TRK_VEIL_W * NV_TRK_SLIDE;
  float cx = NV_SCREEN_W - NV_TRK_PAD - NV_TRK_CONTENT_W + slide, cw = NV_TRK_CONTENT_W;
  float limit = NV_SCREEN_H - NV_TRK_FOOT;
  if (a < 0.01f) return;
  gfx_rect((GfxRect){ NV_SCREEN_W - NV_TRK_VEIL_W + slide, 0, NV_TRK_VEIL_W, NV_SCREEN_H },
           0, GFX_SRC_VEIL, 0, 1, NV_TRK_VEIL_CLEAR, 0,
           NV_SRC_INK_R, NV_SRC_INK_G, NV_SRC_INK_B, a * NV_SRC_VEIL_A);

  if (mode == MODE_AUDIO) {
    drawHeader(cx, video_n_audio(), 0.0f, a);
    // The audio sheet has no tabs; its list hangs where they would be.
    optList((GfxRect){ cx, NV_TRK_TABS_Y - NV_TRK_MENU_GAP, cw, 0 }, limit, 1, a);
    return;
  }
  { int li = langChosen(), ai = activeOption(li), opts[OPT_MAX], shown = subSelShown();
    GfxRect lang = { cx, NV_TRK_TOP, cw, NV_TRK_SEL_H };
    GfxRect sub = { cx, NV_TRK_TOP + NV_TRK_SEL_H + NV_TRK_SEL_GAP, cw, NV_TRK_SEL_H };
    char count[24] = "", value[128], detail[128];
    if (li > 0) snprintf(count, sizeof count, "%d subtitle%s", langs[li].count,
                         langs[li].count == 1 ? "" : "s");
    selectRow(lang, li > 0 ? langs[li].name : "Subtitles off", count,
              zone == Z_LANGSEL && openSel == OPEN_NONE, openSel == OPEN_LANG, a);

    if (shown) {
      langOptions(li, opts);
      detail[0] = '\0';
      if (ai >= 0) {
        char *cut;
        optionText(opts[ai], ai, value, sizeof value, detail, sizeof detail);
        // Folded, the select says where the track comes from as its tail, the way
        // the Language select carries its count: "Track 1 · Embedded". The format
        // and the rest of the detail line stay in the open menu.
        if ((cut = strstr(detail, "  \xc2\xb7  "))) *cut = '\0';
      } else snprintf(value, sizeof value, "Choose a subtitle");
      selectRow(sub, value, detail, zone == Z_SUBSEL && openSel == OPEN_NONE,
                openSel == OPEN_SUB, a);
    } else if (!nSubtitles()) {
      quiet(addons_subtitles_busy() ? "Searching OpenSubtitles\xe2\x80\xa6"
                                    : "No subtitles for this title.", cx, sub.y, cw, a);
    }

    // The open menu LAST, in front of the select below it.
    if (openSel == OPEN_LANG) langList(lang, limit, a);
    if (openSel == OPEN_SUB)  optList(sub, limit, 0, a); }
}

// THE STYLE BAR, drawn to the design: the picture keeps the screen, and the
// controls sit low along the bottom on the episode rail's ramp — a row of tiles,
// one per setting, and under it the choices for the focused tile, with Reset at
// the far end. The preview subtitle lands above them.

// A hairline-edged box: the design's resting tile and chip.
static void quietBox(GfxRect r, float fill, float line, float a) {
  float rad = NV_TRK_BOX_R / r.h;
  gfx_color(r, rad, 1, 1, 1, fill * a);
  gfx_rect(r, 0, GFX_RING_INSET, 0, 1.5f / r.h, 0, rad, 1, 1, 1, line * a);
}

// The focused tile and the chosen chip: a light face with dark ink.
static void lightBox(GfxRect r, float a) {
  float rad = NV_TRK_BOX_R / r.h;
  gfx_color(r, rad, NV_TRK_LIGHT, NV_TRK_LIGHT, NV_TRK_LIGHT, a);
}

static void drawBar(float a) {
  float drop = (1.0f - styleAnim) * 40.0f, x = NV_TRK_BAR_X;
  float w = NV_SCREEN_W - NV_TRK_BAR_X * 2;
  float tw = (w - NV_TRK_TILE_GAP * (FX_N_TILE - 1)) / FX_N_TILE;
  float chipY = NV_SCREEN_H - NV_TRK_BAR_BOTTOM - NV_TRK_CHIP_H + drop;
  float tileY = chipY - NV_TRK_ROWS_GAP - NV_TRK_TILE_H;
  int i;
  if (a < 0.01f) return;
  // THE TRANSPORT'S OWN SCRIM, not the episode rail's. The rail's ramp climbs to
  // 0.72 in its first fifth, which over a short run is a dark band with a visible
  // start; this one is shallow segments from a true zero to 0.88, so there is no
  // line to find, and it is the shading the player already puts under its bar.
  // No flat dim over the rest: the picture is what the preview is judged against.
  gfx_rect((GfxRect){ 0, NV_TRK_SCRIM_Y, NV_SCREEN_W, NV_SCREEN_H - NV_TRK_SCRIM_Y }, 0,
           GFX_VEIL_PLAYER, 0, 0, 0, 0.0f, 0, 0, 0, a);

  // The tiles: a spaced-capitals label over the value. The focused one turns
  // light, with the ‹ › that say it has a set of values under it.
  for (i = 0; i < FX_N_TILE; i++) {
    GfxRect r = { x + i * (tw + NV_TRK_TILE_GAP), tileY, tw, NV_TRK_TILE_H };
    int focused = zone == Z_TILE && i == tileFocus;
    int owner = zone == Z_CHIP && i == tileFocus;
    int ink = focused ? NV_TRK_FOCUS_INK : 255, lab = focused ? 92 : 128;
    float vx = r.x + NV_TRK_TILE_PADX, room = tw - NV_TRK_TILE_PADX * 2;
    char v[32];
    if (focused) {
      lightBox(r, a);
      gfx_rect(r, 0, GFX_RING_INSET, 0, NV_TRK_TILE_RING / r.h, 0, NV_TRK_BOX_R / r.h,
               0.55f, 0.56f, 0.58f, a);
    } else quietBox(r, owner ? NV_TRK_TILE_OWNER : NV_TRK_TILE_FILL,
                    owner ? NV_TRK_TILE_OWNER_LINE : NV_TRK_TILE_LINE, a);
    txt_tracking(TXT_CWC_KICKER, ST_LABEL[i], lab, lab, lab + 2, r.x + NV_TRK_TILE_PADX,
                 r.y + 14.0f, a, 1.5f);
    tileValue(i, v, sizeof v);
    if (focused) {
      TxtLine lt = txt_line(TXT_TRK_OPT, "\xe2\x80\xb9", ink, ink, ink, 255);
      TxtLine gt = txt_line(TXT_TRK_OPT, "\xe2\x80\xba", ink, ink, ink, 255);
      float vy = r.y + 44.0f;
      txt_draw_alpha(lt, vx, vy, a);
      txt_draw_alpha(gt, r.x + r.w - NV_TRK_TILE_PADX - gt.w, vy, a);
      vx += lt.w + 12.0f; room -= lt.w + gt.w + 24.0f;
    }
    txt_draw_alpha(txt_line_trim(TXT_TRK_OPT, v, ink, ink, ink, 255, room),
                   vx, r.y + 44.0f, a);
  }

  // The choices for the focused tile. The value in force is LIGHT; the cursor
  // is the ring — the two can differ, and both need showing. Reset sits at the
  // far end with its icon, quieter than the choices.
  { int n = tileChoices(tileFocus), cur = tileCurrent(tileFocus);
    float cx = x;
    for (i = 0; i <= n; i++) {
      int reset = i == n, sel = !reset && i == cur;
      int focused = zone == Z_CHIP && i == chipFocus;
      int ink = sel ? NV_TRK_FOCUS_INK : reset ? 176 : 235;
      const char *s = reset ? "Reset to defaults" : choiceLabel(tileFocus, i);
      // The value in force is set BOLD as well as light: the fill says which, the
      // weight keeps saying it where the fill is hard to see against a bright frame.
      TxtLine l = txt_line(sel ? TXT_ERAIL_PILL : TXT_TRK_OPTSUB, s, ink, ink,
                           ink + (reset ? 2 : 0), 255);
      float icon = reset ? NV_TRK_RESET_ICON + 10.0f : 0.0f;
      GfxRect r = { cx, chipY, l.w + icon + NV_TRK_CHIP_PAD * 2, NV_TRK_CHIP_H };
      if (reset) r.x = x + w - r.w;
      if (sel) lightBox(r, a); else quietBox(r, NV_TRK_CHIP_FILL, NV_TRK_CHIP_LINE, a);
      if (focused) ringAround(r, NV_TRK_BOX_R, a);
      if (reset) {
        float c = 176 / 255.0f;
        gfx_icon((GfxRect){ r.x + NV_TRK_CHIP_PAD, r.y + (r.h - NV_TRK_RESET_ICON) * 0.5f,
                            NV_TRK_RESET_ICON, NV_TRK_RESET_ICON }, "reset", c, c, c, a);
      }
      txt_draw_alpha(l, r.x + NV_TRK_CHIP_PAD + icon, r.y + (r.h - l.h) * 0.5f, a);
      cx += r.w + NV_TRK_CHIP_GAP;
    } }
}

void tracks_draw(Uint32 now) {
  (void)now;
  if (anim < .01f) return;
  if (mode == MODE_SUBTITLE) buildLangs();
  drawPanel(anim * (1.0f - styleAnim), styleAnim);
  if (mode != MODE_SUBTITLE) return;
  drawBar(anim * styleAnim);
  // THE HEADER STAYS PUT across the two tabs. The panel under it leaves for the
  // Style bar, but the heading and the tabs are how you get back — moving them to
  // the other corner of the screen made the switch look like a different sheet.
  // With the veil gone, the player's own top-right pool keeps them readable.
  { float slide = (1.0f - anim) * NV_TRK_VEIL_W * NV_TRK_SLIDE;
    float cx = NV_SCREEN_W - NV_TRK_PAD - NV_TRK_CONTENT_W + slide;
    if (styleAnim > 0.01f)
      gfx_rect((GfxRect){ NV_SCREEN_W - NV_TRK_POOL_W, 0, NV_TRK_POOL_W, NV_TRK_POOL_H },
               0, GFX_VEIL_POOL, 0, 0, 0, 0.0f, 0, 0, 0, anim * styleAnim);
    drawHeader(cx, nSubtitles(), styleAnim, anim); }
}
