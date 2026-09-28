// VOBSUB: the DVD's subtitle pictures, as a Matroska S_VOBSUB track carries them.
//
// WHY THIS EXISTS. The LG pipeline does not offer a VobSub track at all — it
// lists only the text tracks of a file, so a DVD rip whose English subtitles
// are pictures showed one track, or none. embsub.c fetches the frames out of the
// file; this turns one frame into a picture.
//
// A frame is one SPU packet: a 4-colour run-length bitmap, interlaced (even
// lines, then odd), followed by a chain of control sequences that say where it
// goes, which 4 of the 16 palette colours it uses, how opaque each is, and when
// it comes on and off. The palette itself is not in the packet: it is in the
// track's CodecPrivate, which is the .idx file's text ("palette: 000000, ...").
//
// PURE: no network, no GL. The regression test drives it over a packet built by
// hand.
#ifndef NV_SPU_H
#define NV_SPU_H

typedef struct {
  int x, y, w, h;            // where, in the idx "size:" frame
  int color[4], alpha[4];    // palette index 0..15 and opacity 0..15, per pixel value
  int top, bottom;           // offsets of the two fields' RLE data in the packet
  double on, off;            // seconds after the frame's timestamp; off -1 = not said
  int forced;
} SpuFrame;

typedef struct {
  int w, h;                  // the "size:" frame, 720x576 when missing
  unsigned palette[16];      // 0xRRGGBB
} SpuIdx;

// Reads the .idx text. Missing fields keep their defaults: PAL size and a grey
// ramp, which at least shows a readable picture on a file that names none.
void spu_idx(const char *text, SpuIdx *idx);

// Reads the control sequences. 1 when the packet names a display area and both
// fields; 0 on anything malformed.
int spu_parse(const unsigned char *p, int n, SpuFrame *f);

// Decodes the bitmap into `rgba` (f->w * f->h * 4 bytes, straight alpha).
// 1 on success.
int spu_render(const unsigned char *p, int n, const SpuFrame *f, const SpuIdx *idx,
               unsigned char *rgba);

#endif
