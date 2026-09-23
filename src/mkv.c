#include "mkv.h"
#include "net.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// How much of the file to download. The Tracks element sits right after the
// SeekHead and the Info, before the first Cluster — in practice within the
// first 200 KB.
//
// 320 KB and not 2 MB. The 2 MB were "generous slack" for a remux with cover
// art embedded before Tracks, and they cost dearly at the one moment this read
// happens: WITH THE VIDEO ALREADY PLAYING, over the same connection and from
// the same server. MEASURED on the owner's TV — the read finished at 45.9 s
// and the buffer ran short 1.6 s later, dropping to 2.8 s and taking 9 s to
// recover. The case the 2 MB covered (cover art before Tracks) is rare; the
// cost was paid on EVERY playback. Losing the language on one such file beats
// stuttering the video on all of them.
#define MKV_CHUNK  (320L * 1024)

// --- EBML: variable-length integers ------------------------------------------
//
// The first byte says, by the position of the highest 1 bit, how many bytes the
// number occupies. In the ID that bit IS PART of the value (which is why IDs
// are written as 0x1A45DFA3); in the SIZE it is a marker and drops out.
// Swapping the two is the classic first-timer's mistake here, and the symptom
// is the whole tree coming out shifted.
static int widthOf(unsigned char b) {
  int i;
  for (i = 0; i < 8; i++) if (b & (0x80 >> i)) return i + 1;
  return 0;                       // byte 0x00: invalido em EBML
}

// Le um ID (mantendo o bit marcador). 0 e o fim ou dado invalido.
static unsigned long readId(const unsigned char *p, long remains, int *used) {
  int w, i;
  unsigned long v;
  if (remains < 1) return 0;
  w = widthOf(p[0]);
  if (w < 1 || w > 4 || remains < w) return 0;
  v = 0;
  for (i = 0; i < w; i++) v = (v << 8) | p[i];
  *used = w;
  return v;
}

// Reads a SIZE (stripping the marker bit). Returns -1 on invalid and -2 on the
// "unknown" size (all data bits 1), which Segment uses in a live-streamed file
// — there the read continues INSIDE the element instead of skipping over it.
// vez de pular por cima dele.
static long readSize(const unsigned char *p, long remains, int *used) {
  int w, i;
  unsigned long v;
  int allUm = 1;
  if (remains < 1) return -1;
  w = widthOf(p[0]);
  if (w < 1 || w > 8 || remains < w) return -1;
  v = p[0] & (0xFF >> w);
  if ((unsigned char)(p[0] & (0xFF >> w)) != (unsigned char)(0xFF >> w)) allUm = 0;
  for (i = 1; i < w; i++) {
    if (p[i] != 0xFF) allUm = 0;
    v = (v << 8) | p[i];
  }
  *used = w;
  if (allUm) return -2;
  return (long)v;
}

static unsigned long readUint(const unsigned char *p, long n) {
  unsigned long v = 0;
  long i;
  if (n < 1 || n > 8) return 0;
  for (i = 0; i < n; i++) v = (v << 8) | p[i];
  return v;
}

static void readText(const unsigned char *p, long n, char *dst, size_t size) {
  size_t k = (size_t)n;
  if (k > size - 1) k = size - 1;
  memcpy(dst, p, k);
  dst[k] = 0;
  // Matroska pads strings with NUL on the right; cutting here stops the rest of
  // the field turning into rubbish on screen.
  { size_t i; for (i = 0; i < k; i++) if (dst[i] == 0) { dst[i] = 0; break; } }
}

// --- ids que interessam ------------------------------------------------------
#define ID_SEGMENT     0x18538067UL
#define ID_TRACKS      0x1654AE6BUL
#define ID_TRACKENTRY  0xAEUL
#define ID_TRACKNUMBER 0xD7UL
#define ID_TRACKTYPE   0x83UL
#define ID_LANGUAGE    0x22B59CUL     // Language (ISO 639-2), the classic one
#define ID_LANG_BCP47  0x22B59DUL     // LanguageBCP47 ("pt-BR"), newer
#define ID_NAME        0x536EUL
#define ID_CODECID     0x86UL
#define ID_FLAGFORCED  0x55AAUL

// Reads the TrackEntry elements inside an already-located Tracks.
static int readTracks(const unsigned char *p, long n, MkvTrack *output, int max) {
  long o = 0;
  int found = 0;
  while (o < n && found < max) {
    int ui = 0, ut = 0;
    unsigned long id = readId(p + o, n - o, &ui);
    long size;
    if (!id) break;
    size = readSize(p + o + ui, n - o - ui, &ut);
    if (size < 0) break;
    o += ui + ut;
    if (o + size > n) break;
    if (id == ID_TRACKENTRY) {
      MkvTrack f;
      long q = 0;
      memset(&f, 0, sizeof f);
      while (q < size) {
        int vi = 0, vt = 0;
        unsigned long fid = readId(p + o + q, size - q, &vi);
        long fontSize;
        if (!fid) break;
        fontSize = readSize(p + o + q + vi, size - q - vi, &vt);
        if (fontSize < 0) break;
        q += vi + vt;
        if (q + fontSize > size) break;
        { const unsigned char *v = p + o + q;
          if (fid == ID_TRACKNUMBER) f.number = (int)readUint(v, fontSize);
          else if (fid == ID_TRACKTYPE) f.kind = (int)readUint(v, fontSize);
          else if (fid == ID_LANGUAGE || fid == ID_LANG_BCP47) {
            // BCP47 beats ISO 639-2 when both exist: "pt-BR" says more than
            // "por", and it is what the owner wants to see in the list.
            if (fid == ID_LANG_BCP47 || !f.language[0])
              readText(v, fontSize, f.language, sizeof f.language);
          }
          else if (fid == ID_NAME)    readText(v, fontSize, f.name,  sizeof f.name);
          else if (fid == ID_CODECID) readText(v, fontSize, f.codec, sizeof f.codec);
          else if (fid == ID_FLAGFORCED) f.forced = readUint(v, fontSize) != 0; }
        q += fontSize;
      }
      if (f.number > 0) output[found++] = f;
    }
    o += size;
  }
  return found;
}

// Walks the tree until it finds Tracks. Descends into Segment (which is a giant
// container) and SKIPS the rest — without the skip the search would sweep byte
// by byte and match any coincidence inside the video data.
static int findTracks(const unsigned char *p, long n, MkvTrack *output, int max) {
  long o = 0;
  while (o < n) {
    int ui = 0, ut = 0;
    unsigned long id = readId(p + o, n - o, &ui);
    long size;
    if (!id) return 0;
    size = readSize(p + o + ui, n - o - ui, &ut);
    if (size == -1) return 0;
    o += ui + ut;
    if (id == ID_SEGMENT || size == -2) {
      // Segment: descend into it. An unknown size likewise — there is nothing
      // to skip by.
      if (id == ID_SEGMENT) continue;
      return 0;
    }
    if (id == ID_TRACKS) {
      long disp = n - o;
      if (size > disp) size = disp;     // header larger than the downloaded chunk
      return readTracks(p + o, size, output, max);
    }
    if (o + size > n) return 0;        // the element runs past what we downloaded
    o += size;
  }
  return 0;
}

int mkv_tracks(const char *url, MkvTrack *output, int max) {
  char *buf;
  long n = 0;
  int found;
  if (!url || !url[0] || !output || max < 1) return 0;
  buf = net_download_chunk(url, 20, 0, MKV_CHUNK - 1, &n);
  if (!buf) return 0;
  // The EBML signature. Without it this is not Matroska (it could be MP4, or an
  // error HTML the server returned with a 200), and going on would read rubbish.
  if (n < 64 || (unsigned char)buf[0] != 0x1A || (unsigned char)buf[1] != 0x45 ||
      (unsigned char)buf[2] != 0xDF || (unsigned char)buf[3] != 0xA3) {
    free(buf);
    return 0;
  }
  found = findTracks((const unsigned char *)buf, n, output, max);
  free(buf);
  printf("[mkv] %d tracks read from the header (%ld bytes)\n", found, n);
  fflush(stdout);
  return found;
}
