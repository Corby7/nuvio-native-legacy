// THE PHONE FORM: the Live TV source typed on a phone instead of the remote.
//
// A playlist address is 60 to 200 characters of provider noise, and an Xtream
// password is random on purpose; both are miserable on a TV keyboard and a
// paste away on a phone. So while the setup form is on screen, the TV serves a
// small page on the home network and shows its address as a QR code. The phone
// fills the same fields in and presses Save; the TV takes them as if typed.
//
// NOTHING LEAVES THE HOUSE: the phone talks to the TV directly, with no server
// of ours in between. What guards it:
//   - it listens only while the setup form is open (phonelink_open/close);
//   - every request must carry the random code in the QR, 128 bits, new each
//     time the form opens;
//   - one save closes it.
// It is plain HTTP on the local network; the code and the short window are the
// protection, not encryption.
//
// THREADING: the listener runs on a thread of its own and hands a saved form
// over under a lock. Everything below is called from the main thread.
#ifndef NV_PHONELINK_H
#define NV_PHONELINK_H
#include "iptv.h"
#include <stddef.h>

typedef enum {
  PL_OFF,       // not listening, or no network address to offer
  PL_WAITING,   // listening; nobody has opened the page yet
  PL_OPENED,    // a phone opened the page: the network lets it through
} PhoneLinkState;

// Starts listening (idempotent while open) and prefills the page with `current`
// — never its password. Returns 1 when there is an address to show.
int  phonelink_open(const IptvSource *current);
// Stops listening; the code in the QR stops working.
void phonelink_close(void);
int  phonelink_state(void);
// "http://192.168.1.23:8787/?k=…", or "" when closed.
const char *phonelink_url(void);
// The short way in, for when the camera will not read the QR across a room:
// the address to type ("192.168.1.23:8787") and the four digits it asks for.
// Both "" when closed. The digits change after too many wrong tries.
void phonelink_short(char *addr, size_t na, char *digits, size_t nd);
// 1 once, on the frame after the phone saved: `out` is the source it sent.
int  phonelink_take(IptvSource *out);

// Exposed for tests: the listener's port, 0 when closed.
int  phonelink_port(void);

#endif
