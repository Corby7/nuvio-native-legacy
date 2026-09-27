#!/bin/bash
# timeshift.c against a local endless MPEG-TS upstream. No TV, no SDL window.
#
#   bash tests/timeshift.sh
set -eu
cd "$(dirname "$0")/.."
TMP=$(mktemp -d)
mkdir -p "$TMP/data"
export NUVIO_DATA="$TMP/data"   # never the developer's own ~/.nuvio
cat > "$TMP/upstream.py" <<'PY'
import http.server, struct, time
PAD = bytes(183)
class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def do_GET(self):
        if self.path == "/playlist":
            body = b"#EXTM3U\n#EXTINF:10,\nseg1.ts\n"
            self.send_response(200); self.send_header("Content-Type", "video/mp2t")
            self.send_header("Content-Length", str(len(body))); self.end_headers()
            self.wfile.write(body); return
        if self.headers.get("X-Auth") != "secret" or self.path.startswith("/404/"):
            self.send_response(404); self.send_header("Content-Length", "0"); self.end_headers(); return
        self.send_response(200); self.send_header("Content-Type", "video/mp2t")
        self.send_header("Connection", "close"); self.end_headers()
        limit = 2 << 20 if self.path.startswith("/flaky/") else 1 << 40
        seq, sent = int(time.time() * 5600) & 0xffffff, 0
        try:
            while sent < limit:
                chunk = b"".join(b"\x47" + struct.pack(">I", seq + i) + PAD for i in range(56))
                seq += 56
                self.wfile.write(chunk); sent += len(chunk)
                time.sleep(0.01)
        except (BrokenPipeError, ConnectionResetError):
            pass
s = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
s.daemon_threads = True
print(s.server_address[1], flush=True)
s.serve_forever()
PY
python3 "$TMP/upstream.py" > "$TMP/port" &
SERVER=$!
trap 'kill $SERVER 2>/dev/null; rm -rf "$TMP"' EXIT
for _ in $(seq 50); do [ -s "$TMP/port" ] && break; sleep 0.1; done
cc -std=gnu99 -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1 \
  tests/timeshift.c src/timeshift.c src/data.c src/net.c src/neturl.c -Isrc -I/usr/include/SDL2 \
  -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -o "$TMP/t" -lpthread -ldl -lSDL2
"$TMP/t" "$(cat "$TMP/port")" "$TMP/data" | grep -v '^\[net\]'
