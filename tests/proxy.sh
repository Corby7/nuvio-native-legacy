#!/bin/bash
# proxy.c against a local upstream that demands a header. No TV, no SDL.
#
#   bash tests/proxy.sh
set -eu
cd "$(dirname "$0")/.."
TMP=$(mktemp -d)
cat > "$TMP/upstream.py" <<'PY'
import http.server, sys
DATA = bytes(((i * 7 + 3) & 255) for i in range(1048576))
class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def do_GET(self):
        if self.path.startswith("/hop/"):
            self.send_response(302); self.send_header("Location", self.path[4:])
            self.send_header("Content-Length", "0"); self.end_headers(); return
        if self.headers.get("X-Auth") != "secret":
            self.send_response(401); self.send_header("Content-Length", "0"); self.end_headers(); return
        start, end = 0, len(DATA) - 1
        rng = self.headers.get("Range")
        if rng:
            a, b = rng.split("=")[1].split("-")
            start = int(a); end = int(b) if b else end
        body = DATA[start:end + 1]
        self.send_response(206 if rng else 200)
        self.send_header("Content-Type", "video/x-matroska")
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Set-Cookie", "must-not-pass=1")
        if rng: self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, len(DATA)))
        self.send_header("Content-Length", str(len(body)))
        self.end_headers(); self.wfile.write(body)
s = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
print(s.server_address[1], flush=True)
s.serve_forever()
PY
python3 "$TMP/upstream.py" > "$TMP/port" &
SERVER=$!
trap 'kill $SERVER 2>/dev/null; rm -rf "$TMP"' EXIT
for _ in $(seq 50); do [ -s "$TMP/port" ] && break; sleep 0.1; done
cc -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1 \
  tests/proxy.c src/proxy.c src/net.c src/neturl.c -Isrc -Wall -Wextra -Wno-unused-parameter \
  -o "$TMP/t"
"$TMP/t" "$(cat "$TMP/port")" | grep -v '^\[net\]'
