// Reading a Matroska HEADER, for the tracks' languages, and the CUES INDEX, for
// the timing of every embedded subtitle line.
//
// WHY THIS EXISTS. The LG pipeline returns, in sourceInfo, the language of every
// AUDIO track ("en", "es", "fr", "it") and NO subtitle language: measured on a
// file of the owner's with 43 subtitles, all of them "language":"(null)", and
// the only fields in subtitleTrackInfo are trackNum, language, type and
// periodStart. There is no other field to read — the information simply does not
// come out of the pipeline.
//
// The web app shows the languages because the BROWSER demuxes the file itself
// and exposes textTracks. This module does the same thing in miniature: it
// downloads the first few megabytes by Range and reads the EBML Tracks element.
//
// THE CUES are the second reason. A muxer indexes every subtitle frame there
// (ffmpeg does, with its duration), so the index alone gives the start and end
// of every line of every embedded subtitle — timed to THIS video, which is what
// an addon subtitle is lined up against to sync it. Reading it costs one Range
// request at the end of the file and never touches the Clusters.
//
// IT IS NOT A DEMUXER. It decodes nothing and reads no Clusters. Anything that
// does not match what it expects makes the read give up silently — the caller
// carries on with what there was before.
//
// OFFSETS ARE 64-BIT. The TV is 32-bit ARM, where long is 32 bits, and the Cues
// of a film sit past the 2 GB mark: a long offset would wrap on exactly the files
// this is for.
#ifndef NV_MKV_H
#define NV_MKV_H

#define MKV_MAX_TRACKS 64

typedef struct {
  int  number;        // TrackNumber, the same `trackNum` as LG's sourceInfo
  int  kind;          // 1 video, 2 audio, 17 subtitle (Matroska TrackType)
  char language[8];     // "por", "eng"... empty when the file does not tag it
  char name[48];      // Name, when present ("Forced", "SDH", "Full")
  char codec[24];     // CodecID ("S_TEXT/UTF8", "S_HDMV/PGS")
  int  forced;        // FlagForced: signs and foreign dialogue only
} MkvTrack;

typedef struct {
  MkvTrack tracks[MKV_MAX_TRACKS];
  int nTracks;
  long long cuesAt;              // file offset of the Cues element; -1 unknown
  unsigned long long scale;      // TimestampScale: nanoseconds per tick
} MkvHead;

// One subtitle frame as the Cues index lists it. `end` equals `start` when the
// muxer wrote no CueDuration.
typedef struct {
  int track;          // TrackNumber
  double start, end;  // seconds
} MkvCue;

// Downloads `url`'s header and fills `h`. Returns how many tracks it found, 0
// when it did not work (not an MKV, a server without Range, a header larger
// than the chunk). BLOCKS: call from a thread of your own.
int mkv_head(const char *url, MkvHead *h);

// Downloads the Cues that `h` points at and returns the SUBTITLE entries, in
// the index's order (by time), in a fresh *out the caller frees. -1 when there
// is no index to read or the read failed; `bytes` gets what was downloaded and
// `why` (optional) says what failed, for the log. BLOCKS.
int mkv_cues(const char *url, const MkvHead *h, MkvCue **out, long *bytes,
             char *why, unsigned whySize);

// The pure halves of the two calls above, also used by the regression test.
// `p` is the file from offset 0 for the head, and starts AT the Cues element
// for the cues.
int mkv_head_parse(const unsigned char *p, long n, MkvHead *h);
int mkv_cues_parse(const unsigned char *p, long n, const MkvHead *h, MkvCue **out);

#endif
