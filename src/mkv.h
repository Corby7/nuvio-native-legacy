// Reading a Matroska HEADER, only to discover the tracks' languages.
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
// IT IS NOT A DEMUXER. It decodes nothing, follows no Cues, reads no Clusters.
// It walks the element tree as far as Segment > Tracks and stops. Anything that
// does not match what it expects makes the read give up silently — the caller
// carries on with "Subtitle N", which is what there was before.
#ifndef NV_MKV_H
#define NV_MKV_H

#define MKV_MAX_TRACKS 64

typedef struct {
  int  number;        // TrackNumber, o mesmo `trackNum` do sourceInfo da LG
  int  kind;          // 1 video, 2 audio, 17 legenda (TrackType do Matroska)
  char language[8];     // "por", "eng"... vazio quando o arquivo nao etiqueta
  char name[48];      // Name, quando existe ("Forced", "SDH", "Full")
  char codec[24];     // CodecID ("S_TEXT/UTF8", "S_HDMV/PGS")
} MkvTrack;

// Reads `url`'s header and fills `out`. Returns how many tracks it found, 0 when
// it did not work (not an MKV, a server without Range, a header larger than the
// chunk). BLOCKS: call from a thread of your own.
int mkv_tracks(const char *url, MkvTrack *output, int max);

#endif
