#include "notice.h"
#include "layout.h"
#include "text.h"
#include "anim.h"
#include "gfx.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

// THE NOTICE (see notice.h): a compact pill at the top centre, led by a round badge drawn as the
// hold ring — the logo's loop of colours, lifted towards white (GFX_RING_INSET_FILL
// with its brand flag) — with a glyph inside. It replaces a navy pill with a
// traffic-light dot, a tint and three colours that appeared nowhere else in the app.
//   NOTE_GOOD, NOTE_MODE  the full brand ring
//   NOTE_BUSY             a dim track with a brand-coloured arc running round it,
//                         the seek spinner's run of dots at badge size
//   NOTE_WARN, NOTE_ERROR the ring and glyph solid amber / red: the only places
//                         a notice uses a colour that is not the logo's
// The text is split at its first " · ": what happened in white, the detail after it
// in grey. NOTE_MODE (the aspect key) is the same shape a size up, lower down.
// `a` is the caller's fade; `from` is when it came up, for the drop-in.
#define NV_NOTE_TOP       52.0f
#define NV_NOTE_TOP_MODE 150.0f
#define NV_NOTE_DROP      14.0f    // px it drops in from
#define NV_NOTE_IN_MS    320.0f
#define NV_NOTE_SPIN_MS  1000.0f   // ms per turn of the busy arc
#define NV_NOTE_ARC       4.2f     // radians, the busy arc's length
void notice_draw(const char *text, const char *icon, int kind, float a,
                       Uint32 from, Uint32 now) {
  char head[160];
  const char *tail = NULL, *sep;
  int mode = kind == NOTE_MODE;
  float d = mode ? 88.0f : 64.0f, pad = mode ? 16.0f : 12.0f, padR = mode ? 56.0f : 40.0f;
  float gap = 20.0f, tgap = 14.0f, ring = mode ? 5.0f : 4.0f;
  float el = (float)(Uint32)(now - from), u, in, pw, ph, tx;
  float ir = 0.96f, ig = 0.96f, ib = 0.96f;
  TxtLine t, s = { 0, 0, 0 };
  GfxRect pill, badge;
  if (!text || !*text) return;
  u = anim_clamp(el / NV_NOTE_IN_MS, 0.0f, 1.0f);
  in = 1.0f - (1.0f - u) * (1.0f - u) * (1.0f - u);
  a *= anim_clamp(el / 200.0f, 0.0f, 1.0f);
  if (a <= 0.005f) return;

  sep = mode ? NULL : strstr(text, "\xC2\xB7");
  if (sep) {
    size_t n = (size_t)(sep - text);
    while (n && text[n - 1] == ' ') n--;
    if (n >= sizeof head) n = sizeof head - 1;
    memcpy(head, text, n); head[n] = 0;
    tail = sep + 2;
    while (*tail == ' ') tail++;
    if (!*tail) tail = NULL;
  } else snprintf(head, sizeof head, "%s", text);

  t = txt_line_trim(mode ? TXT_PLR_TITLE : TXT_TRK_OPT, head, 245, 245, 245, 255, 1100);
  if (tail) s = txt_line_trim(TXT_PLR_ENDS, tail, 156, 156, 163, 255, 1400.0f - (float)t.w);
  pw = pad + d + gap + (float)t.w + (tail ? tgap + (float)s.w : 0.0f) + padR;
  ph = d + pad * 2.0f;
  pill = (GfxRect){ (NV_SCREEN_W - pw) * 0.5f,
                    (mode ? NV_NOTE_TOP_MODE : NV_NOTE_TOP) - NV_NOTE_DROP * (1.0f - in),
                    pw, ph };
  gfx_drop_shadow(pill, ph * 0.5f, 28.0f, 10.0f, 0.45f * a);
  gfx_color(pill, 0.5f, 20 / 255.0f, 20 / 255.0f, 22 / 255.0f, 0.94f * a);
  gfx_rect(pill, 0, GFX_RING_INSET, 0, 1.5f / ph, 0, 0.5f, 1, 1, 1, 0.07f * a);

  badge = (GfxRect){ pill.x + pad, pill.y + pad, d, d };
  gfx_color(badge, 0.5f, 0.11f, 0.11f, 0.12f, a);
  if (kind == NOTE_WARN || kind == NOTE_ERROR) {
    if (kind == NOTE_WARN) { ir = 1.0f; ig = 0.71f; ib = 0.28f; }
    else                   { ir = 1.0f; ig = 0.48f; ib = 0.42f; }
    gfx_rect(badge, 0, GFX_RING_INSET, 0, ring / d, 0, 0.5f, ir, ig, ib, a);
  } else if (kind == NOTE_BUSY) {
    float cx = badge.x + d * 0.5f, cy = badge.y + d * 0.5f, rr = d * 0.5f - ring * 0.5f;
    float head0 = (float)(now % (Uint32)NV_NOTE_SPIN_MS) * (6.2831853f / NV_NOTE_SPIN_MS);
    int k, n = (int)(NV_NOTE_ARC * rr / 1.4f);
    gfx_rect(badge, 0, GFX_RING_INSET, 0, ring / d, 0, 0.5f, 1, 1, 1, 0.12f * a);
    for (k = 0; k <= n; k++) {
      float f = (float)k / (float)n, ang = head0 - NV_NOTE_ARC * f, cr, cg, cb;
      // The ramp from the head back, lifted 40% towards white like the hold ring's
      // loop, and the tail thinning out so the arc has a direction.
      gfx_seek_brand_at(1.0f - f, &cr, &cg, &cb);
      cr += (1.0f - cr) * 0.4f; cg += (1.0f - cg) * 0.4f; cb += (1.0f - cb) * 0.4f;
      gfx_color((GfxRect){ cx + cosf(ang) * rr - ring * 0.5f, cy + sinf(ang) * rr - ring * 0.5f,
                           ring, ring }, 0.5f, cr, cg, cb, a * (1.0f - 0.75f * f));
    }
  } else {
    gfx_rect(badge, 0, GFX_RING_INSET_FILL, 1.0f, 1.0f, ring / d, 0.5f, 1, 1, 1, a);
  }
  { float is = d * 0.44f;
    gfx_icon((GfxRect){ badge.x + (d - is) * 0.5f, badge.y + (d - is) * 0.5f, is, is },
             icon, ir, ig, ib, a); }

  tx = badge.x + d + gap;
  txt_draw_alpha(t, tx, pill.y + (ph - (float)t.h) * 0.5f, a);
  if (tail)
    txt_draw_alpha(s, tx + (float)t.w + tgap, pill.y + (ph - (float)s.h) * 0.5f, a);
}
