// QR Code generator, only as much as the login screen needs.
//
// WHY IT EXISTS — and it was not the plan. The plan said "show the code in big
// letters, QR later". MEASURED against the real server, the response from
// start_tv_login_session is:
//   {"code":"fa0010cad8b5d2f512e58646ab82ca6b",
//    "web_url":"https://nuvio.tv/tv-login?code=fa0010cad8b5d2f512e58646ab82ca6b"}
// Thirty-two hexadecimal digits. Nobody reads that off a TV and types it into a
// phone without a mistake. The QR stopped being decoration and became the only
// way in — which is why it moved into Phase A.
//
// DELIBERATELY NARROW SCOPE: BYTE mode, L correction, versions 1 to 6 (up to
// 41x41 modules, 134 bytes of data). From version 7 the symbol starts requiring
// a version information block with its own BCH, and nothing here needs that:
// the login URL is ~55 characters and fits in version 4. Less code, fewer
// places to get it wrong.
//
// CHECKED against the `segno` implementation (Python), module by module, across
// several inputs — including the real login URL. See tools/qr_check.py.
#ifndef NV_QR_H
#define NV_QR_H

#define QR_MAX_SIDE 41   // versao 6: 17 + 4*6

typedef struct {
  int side;                                  // 0 quando nao coube
  unsigned char m[QR_MAX_SIDE * QR_MAX_SIDE];// 1 = modulo escuro
} Qr;

// Encodes `text`. Returns 1 on success; 0 when the text does not fit in version
// 6 (more than 134 bytes) or is empty.
int qr_generate(Qr *q, const char *text);

static inline int qr_modulo(const Qr *q, int x, int y) {
  if (!q || x < 0 || y < 0 || x >= q->side || y >= q->side) return 0;
  return q->m[y * q->side + x];
}

#endif
