#!/bin/bash
# The Live TV loader against a fake Xtream panel on loopback: get.php answers
# 884, player_api.php answers as a real panel does (json_encode's escapes).
#
#   bash tests/iptv_xtream.sh
set -eu
cd "$(dirname "$0")/.."
TMP=$(mktemp -d)
mkdir -p "$TMP/data"
export NUVIO_DATA="$TMP/data"

cat > "$TMP/panel.py" <<'PY'
import http.server, json, sys, time, urllib.parse
now = int(time.time())
fmt = lambda t: time.strftime("%Y%m%d%H%M%S +0000", time.gmtime(t))
autoHits = 0
class H(http.server.BaseHTTPRequestHandler):
  def log_message(self, *a): pass
  def send(self, code, body, kind="application/json"):
    b = body.encode()
    self.send_response(code); self.send_header("Content-Type", kind)
    self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
  def do_GET(self):
    u = urllib.parse.urlparse(self.path); q = dict(urllib.parse.parse_qsl(u.query))
    if u.path == "/get.php": return self.send(884, "", "text/html")
    if u.path == "/xmltv.php":
      return self.send(200, '<tv><channel id="cocuk.tr"/><programme start="%s" stop="%s" channel="cocuk.tr">'
                       '<title>Cartoons</title></programme></tv>' % (fmt(now - 600), fmt(now + 600)), "text/xml")
    if u.path == "/auto/epg_ripper_NL1.xml.gz":
      # A country's public guide: ids of its own, names as the channels are called.
      import gzip
      x = "<tv>" + "".join('<channel id="%s.nl"><display-name>%s</display-name></channel>' % (i, n)
                           for i, n in [("RTL4", "RTL 4"), ("SBS6", "SBS 6"), ("NPO2", "NPO 2")])
      x += "".join('<programme start="%s" stop="%s" channel="%s.nl"><title>%s now</title></programme>'
                   % (fmt(now - 600), fmt(now + 600), i, i) for i in ["RTL4", "SBS6", "NPO2"]) + "</tv>"
      b = gzip.compress(x.encode())
      self.send_response(200); self.send_header("Content-Type", "application/gzip")
      self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
      global autoHits; autoHits += 1
      return
    if u.path == "/auto-hits": return self.send(200, str(autoHits), "text/plain")
    if u.path.startswith("/auto/"): return self.send(404, "")
    if u.path == "/extra.xml.gz":
      # The viewer's own guide: the channel the provider's leaves out.
      import gzip
      b = gzip.compress(('<tv><channel id="News.x"><display-name>News</display-name></channel>'
                         '<programme start="%s" stop="%s" channel="News.x"><title>Headlines</title></programme></tv>'
                         % (fmt(now - 600), fmt(now + 600))).encode())
      self.send_response(200); self.send_header("Content-Type", "application/gzip")
      self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
      return
    if u.path != "/player_api.php": return self.send(404, "")
    user, pw = q.get("username"), q.get("password")
    if pw != "fa10": return self.send(200, json.dumps({"user_info": {"auth": 0}}))
    if "action" not in q:
      return self.send(200, json.dumps({"user_info": {"auth": 1, "status": "Expired" if user == "old" else "Active",
        "allowed_output_formats": ["m3u8", "ts"]}, "server_info": {"url": "127.0.0.1"}}))
    if q["action"] == "get_live_categories":
      return self.send(200, json.dumps([{"category_id": "5", "category_name": "Çocuk", "parent_id": 0}]))
    if q["action"] == "get_live_streams":
      return self.send(200, json.dumps([
        {"num": 1, "name": "Çocuk TV", "stream_id": 101, "stream_icon": "http://l.example/c.png",
         "epg_channel_id": "cocuk.tr", "category_id": "5", "tv_archive": 0},
        {"num": 2, "name": "News", "stream_id": "102", "stream_icon": "", "epg_channel_id": None,
         "category_id": "5"},
        {"num": 3, "name": "NL: RTL 4 FHD", "stream_id": 103, "category_id": "5"},
        {"num": 4, "name": "NL: SBS 6", "stream_id": 104, "category_id": "5"},
        {"num": 5, "name": "|NL| NPO 2 HD", "stream_id": 105, "category_id": "5"}]))
    self.send(404, "")
srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
print(srv.server_address[1], flush=True)
srv.serve_forever()
PY
python3 "$TMP/panel.py" > "$TMP/port" &
PANEL=$!
trap 'kill $PANEL 2>/dev/null; rm -rf "$TMP"' EXIT
until [ -s "$TMP/port" ]; do sleep 0.1; done

cc -std=gnu99 -g -O1 tests/iptv_xtream.c src/iptv.c src/iptv_parse.c src/net.c src/neturl.c src/proxy.c src/js.c src/data.c \
  -Isrc -I/usr/include/SDL2 -w -lSDL2 -ldl -lpthread -lm -o "$TMP/t"
# The fake panel is on loopback: no proxy in between.
NUVIO_AUTO_EPG_BASE="http://127.0.0.1:$(cat "$TMP/port")/auto/epg_ripper_" \
NO_PROXY=127.0.0.1 no_proxy=127.0.0.1 "$TMP/t" "$(cat "$TMP/port")"
