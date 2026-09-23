// A loopback HTTP relay that adds request headers the pipeline cannot send.
//
// WHY IT EXISTS. The `load` payload to com.webos.media carries a URI and nothing
// else; the pipeline fetches it with its own GStreamer HTTP client. A source that
// authenticates with a header — `behaviorHints.proxyHeaders.request` in the
// addon's answer — reached the server bare and got a 401 (measured: errorCode
// 40401, then 206 "Media Authorized Error"). The web app hits the same wall with
// <video> and solves it the same way: a server on 127.0.0.1 that the pipeline
// fetches from, which repeats each request upstream with the headers added
// (js/platform/webos/webosPlaybackProxy.js).
//
// THE URL KEEPS THE SOURCE'S PATH. The local address is
// http://127.0.0.1:<port>/p/<token><path>?<query>, where the token stands for the
// source's origin and headers. An HLS playlist's relative segment URLs then
// resolve against the proxy on their own and inherit the headers; the web's
// proxy is built on the same idea. Absolute segment URLs on another host do not
// pass through it — same limitation as the web.
//
// The headers never appear in the URL or in the log: the token is a random key
// into a table in this process, and only the header NAMES are printed.
#ifndef NV_PROXY_H
#define NV_PROXY_H

// The URL to hand the pipeline for `url`. `headers` is "Name: Value" lines
// separated by '\n' (Stream.headers). With no headers, or when the relay cannot
// start, returns `url` itself and the source plays as it did before. Otherwise
// writes the local address into `dst` and returns `dst`.
//
// Starts the listener on first use. Safe from any thread.
const char *proxy_wrap(const char *url, const char *headers, char *dst,
                       unsigned size);

#endif
