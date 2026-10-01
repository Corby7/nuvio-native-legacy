// Updating the app from inside the app: the releases published on GitHub, their
// notes, and installing any of them — newer or older — over the running app.
//
// MEASURED ON THE C3 (webOS 26, 2026-10-01) with a spike that installed one
// build over another from inside the app:
// - luna-send-pub (devmode role, outbound "*", see video.c) may call
//   com.webos.appInstallService/dev/install; the install took 2 s;
// - the installer closes the running app itself ("app closing"), so what waits
//   for the end and opens the new version is a detached shell helper, which
//   survived it (adopted by init) and relaunched through applicationManager;
// - the data folder (.nuvio, inside the app's folder) came through untouched.
#ifndef NV_UPDATE_H
#define NV_UPDATE_H

typedef struct {
  char tag[32];        // "v1.5.1"
  char version[32];    // "1.5.1"
  char name[96];       // the release's title
  char date[24];       // "30 Sep 2026"
  char url[512];       // the .ipk's download URL, "" when the release has none
  char sha256[65];     // its digest as GitHub publishes it, "" when absent
  char *notes;         // plain text: one paragraph per line, see update_notes_*
  int prerelease;
  int cmp;             // against the running version: >0 newer, 0 same, <0 older
} UpdateRelease;

typedef enum { UPD_IDLE, UPD_LOADING, UPD_READY, UPD_FAILED } UpdateListState;

// Asks GitHub for the releases, on a thread. The list already shown stays until
// the answer is adopted by update_poll.
void update_fetch(void);
// Call every frame from the screen that shows the list: adopts a finished fetch
// on the main thread, so a list being drawn is never freed under it.
void update_poll(void);
UpdateListState update_list_state(void);
// Changes each time update_poll adopts a new list, so a screen knows to place
// its focus again.
int update_generation(void);
int update_n(void);
const UpdateRelease *update_item(int i);
// The newest release that is not a pre-release, or -1.
int update_latest(void);

// Release notes are stored one paragraph per line. A line starting with this
// byte is a heading; a line starting with "• " is a bullet.
#define NV_UPDATE_HEADING '\x01'

typedef enum {
  UPI_IDLE, UPI_DOWNLOADING, UPI_VERIFYING, UPI_INSTALLING, UPI_FAILED
} UpdateInstallState;

// Downloads release i, checks its SHA-256 and hands it to the installer through
// the detached helper. The app is closed by the installer once it starts and
// the helper reopens it. 0 if refused (busy, no package, not on a TV).
int update_install(int i);
UpdateInstallState update_install_state(void);
const char *update_install_error(void);
// Back to UPI_IDLE after a failure has been read.
void update_install_dismiss(void);

// The first version that can update itself. Installing an older one leaves the
// TV without this screen: getting back means Developer Mode or the Homebrew
// Channel.
#define NV_UPDATE_SINCE "1.6.0"
int update_version_cmp(const char *a, const char *b);

#endif
