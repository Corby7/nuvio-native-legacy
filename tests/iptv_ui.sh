#!/bin/bash
# The Live TV screen end to end, off the TV: a playlist and a gzipped XMLTV
# guide generated around the current time and served over file://, the screen
# driven by key events, and review captures written as BMPs.
#
#   bash tests/iptv_ui.sh            # captures into /tmp
#   NUVIO_SHOTS=dir bash tests/iptv_ui.sh
#
# Needs SDL2, SDL2_image, SDL2_ttf and a GL context. On Linux without a display
# it runs itself under xvfb-run. The video pipeline is tests/video_stub.c, which
# plays everything and supplies a generated picture, so the overlays over a
# playing channel can be captured.
set -eu
cd "$(dirname "$0")/.."
ROOT=$(pwd)
OUT="${NUVIO_SHOTS:-/tmp}"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/data" "$OUT"
# data.c takes the folder from here first; never the developer's own ~/.nuvio.
export NUVIO_DATA="$TMP/data"

# An endless MPEG-TS for the pause buffer's channel: numbered 188-byte packets
# at about 1 MB/s (tests/timeshift.sh has the same upstream).
cat > "$TMP/ts.py" <<'PY'
import http.server, struct, time
PAD = bytes(183)
class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def do_GET(self):
        self.send_response(200); self.send_header("Content-Type", "video/mp2t")
        self.send_header("Connection", "close"); self.end_headers()
        seq = 0
        try:
            while True:
                self.wfile.write(b"".join(b"\x47" + struct.pack(">I", seq + i) + PAD for i in range(56)))
                seq += 56
                time.sleep(0.01)
        except (BrokenPipeError, ConnectionResetError):
            pass
s = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
s.daemon_threads = True
print(s.server_address[1], flush=True)
s.serve_forever()
PY
python3 "$TMP/ts.py" > "$TMP/tsport" &
TS_SERVER=$!
trap 'kill $TS_SERVER 2>/dev/null; rm -rf "$TMP"' EXIT
for _ in $(seq 50); do [ -s "$TMP/tsport" ] && break; sleep 0.1; done

python3 - "$TMP" "$ROOT" "$(cat "$TMP/tsport")" <<'PY'
import gzip, sys, time, random
tmp, root, tsport = sys.argv[1], sys.argv[2], sys.argv[3]
random.seed(7)
groups = ["News", "Sport", "Movies", "Kids", "Local"]
names = {
  "News": ["World News", "News 24", "Business Today", "Politics Live", "Weather Now", "Morning Desk",
           "Global Report", "City News", "Tech Brief", "Evening Post", "Headlines", "Parliament"],
  "Sport": ["Sport One", "Sport Two", "Football Plus", "Tennis TV", "Motor Racing", "Golf Channel",
            "Cycling Live", "Olympic Stories", "Fight Night", "Basketball HD", "Cricket 1", "Rugby World"],
  "Movies": ["Cinema Classics", "Action Max", "Drama Box", "Comedy Central", "Thriller Zone", "Film Four",
             "Sci-Fi Station", "Horror Night", "Romance HD", "Documentary", "Indie Films", "Premiere"],
  "Kids": ["Cartoon Time", "Junior", "Toons HD", "Discovery Kids", "Nature Kids", "Tiny Pop",
           "Kids Club", "Animation+", "Story Time", "Learning TV", "Puppets", "Family"],
  "Local": ["Local TS"],
}
shows = ["Morning Briefing", "The Big Match", "Midday Movie: The Long Road Home", "Documentary: Oceans",
         "Live: Championship Final", "Evening News", "Quiz Night", "Weather", "Late Show", "Cooking with Ana",
         "Film: Northern Lights", "Cartoon Marathon", "Highlights", "Talk Back", "Science Hour", "Market Watch"]
logos = [f"{root}/deploy/app/art/brands/{b}" for b in ["tmdb.png", "trakt.png", "imdb.png", "letterboxd.png"]]
m3u = ['#EXTM3U url-tvg="file://%s/guide.xml.gz"' % tmp]
chans = []
n = 1
for g in groups:
  for nm in names[g]:
    cid = nm.lower().replace(" ", "").replace(":", "") + ".tv"
    logo = logos[n % 4] if n % 3 else ""
    # Sport keeps a day of catch-up, the Kodi "shift" way.
    extra = ' catchup="shift" catchup-days="1"' if g == "Sport" else ""
    m3u.append('#EXTINF:-1 tvg-id="%s" tvg-name="%s" tvg-logo="%s" group-title="%s" tvg-chno="%d"%s,%s'
               % (cid, nm, logo, g, 100 + n, extra, nm))
    m3u.append("http://127.0.0.1:%s/live.ts" % tsport if g == "Local" else "http://example.invalid/live/%d.m3u8" % n)
    chans.append(cid)
    n += 1
open(tmp + "/playlist.m3u", "w").write("\n".join(m3u) + "\n")
now = int(time.time())
t0 = now - now % 1800 - 3 * 3600
fmt = lambda t: time.strftime("%Y%m%d%H%M%S +0000", time.gmtime(t))
x = ['<?xml version="1.0" encoding="UTF-8"?>', "<tv>"]
for cid in chans:
  x.append('<channel id="%s"><display-name>%s</display-name></channel>' % (cid, cid))
for cid in chans:
  t = t0 - random.choice([0, 600, 900, 1200])
  if cid.startswith("weather"): continue          # a channel with no guide
  while t < now + 14 * 3600:
    d = random.choice([900, 1800, 1800, 3600, 3600, 5400, 7200])
    title = random.choice(shows)
    x.append('<programme start="%s" stop="%s" channel="%s"><title>%s</title>'
             '<desc>An episode of %s. Presenters follow the story from the studio and on location, '
             'with guests and reports from around the country.</desc><category>General</category></programme>'
             % (fmt(t), fmt(t + d), cid, title.replace("&", "&amp;"), title.replace("&", "&amp;")))
    t += d
x.append("</tv>")
gzip.open(tmp + "/guide.xml.gz", "wt").write("\n".join(x))
open(tmp + "/data/iptv.txt", "w").write("kind=m3u\nurl=file://%s/playlist.m3u\nepg=\nserver=\nuser=\npass=\nbuffer=15\n" % tmp)
PY

sources=()
for s in src/*.c; do
  case "$s" in src/main.c|src/video.c|src/video_mac.c) ;; *) sources+=("$s") ;; esac
done
sources+=(tests/video_stub.c)
if [ "$(uname)" = Darwin ]; then
  cc "${sources[@]}" tests/iptv_ui.c -Isrc -o "$TMP/t" -O1 -g \
    -I/opt/homebrew/include -I/opt/homebrew/include/SDL2 -L/opt/homebrew/lib \
    -lSDL2 -lSDL2_image -lSDL2_ttf -framework OpenGL -Wno-deprecated-declarations -Wno-macro-redefined
  "$TMP/t" "$TMP/data" "$OUT"
else
  cc -std=gnu99 "${sources[@]}" tests/iptv_ui.c -Isrc -I/usr/include/SDL2 -o "$TMP/t" -O1 -g -w \
    -lSDL2 -lSDL2_image -lSDL2_ttf -lGLESv2 -ldl -lpthread -lm
  if [ -z "${DISPLAY:-}" ]; then
    xvfb-run -a -s "-screen 0 1920x1080x24" "$TMP/t" "$TMP/data" "$OUT"
  else
    "$TMP/t" "$TMP/data" "$OUT"
  fi
fi
