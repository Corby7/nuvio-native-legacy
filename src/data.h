// Where the app writes what belongs to the USER — session, active profile,
// sync cache.
//
// Until now settings.c and catalog.c wrote inside dirArt, the PACKAGE folder.
// In developer mode that works, which is why it went unnoticed; in a properly
// installed app the package folder is the wrong place, and it is the same
// folder for everyone who uses the device. With login, writing there would mean
// one person's session inside another person's app.
//
// The folder is DISCOVERED, not guessed: there is no reliable public
// documentation of where a NATIVE webOS app may write, and a wrong fixed path
// turns "nothing was saved" into a silent defect — the app opens, looks signed
// in, and on the next start has forgotten everything with no message at all.
// The probe genuinely tries to write in each candidate and logs which one
// won.
#ifndef NV_DATA_H
#define NV_DATA_H

// Chooses the folder. `dirArt` comes in as a LAST resort (it is today's
// behaviour, and it beats writing nothing). Call once, at startup.
void data_start(const char *dirArt);

// The chosen folder, with no trailing slash. Never NULL after data_start; it
// may be "" if no candidate accepted a write — in that case writing is a no-op
// and the log has already said why.
const char *data_dir(void);

// Builds `data_dir()/name` into `dst`. Returns dst, or NULL if there is no folder.
char *data_path(char *dst, unsigned size, const char *name);

// Writes `content` to `name` atomically (temporary + rename). 1 on success.
// Atomic because losing the session to a half-written file is exactly the kind
// of defect that only shows up on somebody else's device.
int data_write(const char *name, const char *content);

// Reads all of `name` into a fresh NUL-terminated buffer (caller frees).
char *data_read(const char *name);

int data_erase(const char *name);

// A STABLE identifier for this installation, generated on the first run and
// saved. The web sync sends this in `p_origin_client_id` so the server does not
// hand the device back the write it just made itself — without a stable id
// every start looks like a new device and the echo comes back.
const char *data_client_id(void);

// A fresh UUID v4 on every call. MEASURED against the server: the
// start_tv_login_session RPC refuses any nonce that is not a UUID with "Invalid
// device nonce" — an identifier of our own, however unique, does not pass.
void data_uuid(char *dst, unsigned size);

#endif
