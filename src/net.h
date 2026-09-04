// Simple HTTP(S) fetching, into memory.
//
// Uses the DEVICE's libcurl via dlopen. Writing TLS by hand was out of the
// question and the SDK ships no libcurl to link against — but
// /usr/lib/libcurl.so.5 exists on the TV, and the addons only speak https. On
// the Mac it uses the system libcurl.
#ifndef NV_NET_H
#define NV_NET_H

// Downloads all of `url` into a fresh NUL-terminated buffer and returns it; the
// caller frees it. NULL on any failure. BLOCKS — call from a thread of your
// own, never from the draw loop.
char *net_download(const char *url, int seconds);

// The same, but for BINARY content: returns the size in *n. The version above
// is NUL-terminated and suits JSON; an image has zeros in the middle and strlen
// would lie.
char *net_download_bin(const char *url, int seconds, long *n);

// With headers. `headers` is a NULL-terminated array of ready-made lines
// ("Authorization: Bearer x"). It exists because of Trakt, which requires the
// token and the application key in headers — there is no way to pass them in
// the URL.
char *net_download_com(const char *url, int seconds, const char *const *headers);

// Downloads ONLY A CHUNK, via the Range header. Returns the size in *size.
//
// It exists to read an MKV header without pulling the whole file: the LG
// pipeline returns "language":"(null)" on EVERY subtitle track (measured on a
// file with 43 subtitles — the audio carries a language, the subtitles do not),
// and the only way to know the language is to read the container itself, which
// is what the browser does on its own in the web app.
//
// A server that ignores Range returns the whole file; so the caller has to be
// ready to receive MORE than it asked for, and to stop reading once it has
// found what it wanted.
char *net_download_chunk(const char *url, int seconds, long start, long end,
                         long *size);

// Byte cap for the current transfer (0 = no cap). Internal to the module; it is
// exposed only because net_download_chunk uses it. Do not touch from outside.
extern long net_cap;

// Follows the redirects and returns the FINAL address, without downloading the
// body. It tells you whether a debrid link leads to the file or to a notice
// video ("downloading.mp4", "slate.mp4") — which PLAYS PERFECTLY WELL and so
// raises no error at all. 1 if it resolved.
int net_url_final(const char *url, int seconds, char *dst, unsigned size);

// JSON POST. It exists for Trakt, which only accepts writes over POST.
char *net_post(const char *url, int seconds, const char *const *headers,
                  const char *body);

// The same, but returns the HTTP CODE in *status (0 when the request never left).
// It exists because of Supabase: there a 401 is not a failure, it is the
// instruction to refresh the token and retry. Without the code in hand,
// "session expired" and "server down" arrive identically — as NULL — and the
// app either drops the session for nothing or spins in a retry loop against an
// error that will not pass.
// The error body comes back too: PostgREST explains in the body which function
// or table does not exist, and that string is what separates "old server" from
// "wrong parameter".
char *net_post_st(const char *url, int seconds, const char *const *headers,
                     const char *body, int *status);

// GET with headers AND the HTTP code. For the same reason as the POST above:
// reading a Supabase table has to tell "table does not exist" (404 with
// PGRST205 in the body) from "no network", because the first means falling
// through to the next table and the second means touching nothing.
//
// NOTE: unlike net_download_with, this does NOT turn 4xx into NULL. The error
// body is exactly what the caller wants to read.
char *net_download_st(const char *url, int seconds, const char *const *headers,
                     int *status);

// Loads libcurl NOW, on the calling thread. It exists so startup can do this on
// the main thread, before any network thread is born: `curl_global_init` is not
// thread safe, and the internal lock is the second line of defence, not the
// first. Calling it more than once costs nothing.
void net_prepare(void);

#endif
