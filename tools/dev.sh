#!/bin/bash
# The dev channel, for driving the app without somebody holding the remote.
#
# IT LIVES IN THE REPO, and that is the point. Its two predecessors lived in
# /tmp (TOOLS.md still named them) and went with a reboot, leaving the file
# telling you to run scripts that did not exist. The protocol they spoke is in
# TOOLS.md; this is the implementation, kept where it cannot evaporate.
#
#   tools/dev.sh run                  build, relaunch, wait for ready
#   tools/dev.sh goto tt12637874      open that title's detail
#   tools/dev.sh where                which screen, from the log (no capture)
#   tools/dev.sh rects                the named regions of the current screen
#   tools/dev.sh key down down ok     inject keys
#   tools/dev.sh shot name            capture the whole frame
#   tools/dev.sh shot name episodes   capture, cropped to a named region
#
# NUVIO_TARGET=tv sends the same commands to the TV over ssh instead.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${NUVIO_SHOTS:-$ROOT/.macref}"
LOG="${NUVIO_LOG:-$ROOT/.macref/app.log}"
BIN=/tmp/nuvio-native-legacy-mac
mkdir -p "$OUT"

# requestNew() in main.c tests st_size > 0, so an EMPTY request file is never
# seen. Everything written here carries a payload for that reason.
say() { printf '%s' "$1" > "$2"; }

case "$1" in
  run)
    cd "$ROOT"
    pkill -f "$(basename $BIN)" 2>/dev/null || true
    sleep 0.5
    ENV_D=$(tools/env.sh)
    # Video through libmpv when it is installed (see tools/mac.sh).
    MPV_FLAGS=""
    [ -f /opt/homebrew/include/mpv/client.h ] && MPV_FLAGS="-DNV_MPV -lmpv"
    # QUIET, except for what is actually wrong. gl_compat.h redefines three GL
    # macros the macOS SDK already has, once per translation unit -- 150-odd lines
    # of noise per build that hides a real diagnostic in the middle of it.
    if ! eval cc src/*.c -o "$BIN" -O1 -g "$ENV_D" $MPV_FLAGS \
      -I/opt/homebrew/include -I/opt/homebrew/include/SDL2 \
      -L/opt/homebrew/lib -lSDL2 -lSDL2_image -lSDL2_ttf \
      -framework OpenGL -Wno-deprecated-declarations -Wno-macro-redefined \
      2> "$OUT/build.log"; then
      grep -E "error" "$OUT/build.log" | head -20 >&2
      exit 1
    fi
    grep -E "warning" "$OUT/build.log" | grep -v "macro redefined" | head -10 >&2 || true
    NUVIO_DATA="${NUVIO_DATA:-$HOME/.nuvio}" nohup "$BIN" "$ROOT/deploy/app/art" \
      > "$LOG" 2>&1 &
    # WAIT FOR READY, do not sleep a guess. Measured: the first [nav] lands about
    # a second after launch, and the 12s sleep this replaces cost three times the
    # build it followed.
    for _ in $(seq 1 80); do
      sleep 0.25
      grep -q '^\[nav\]' "$LOG" 2>/dev/null && { echo ready; exit 0; }
    done
    echo "did not become ready; see $LOG" >&2; exit 1 ;;

  goto)  say "$2" /tmp/nuvio-goto ;;
  where) grep '^\[nav\]' "$LOG" | tail -1 ;;
  rects)
    say regions /tmp/nuvio-key
    sleep 0.6
    # Only the lines from THIS request: the log keeps every earlier dump.
    awk '/^\[rect\]/{n++; a[n]=$0} END{for(i=1;i<=n;i++) print a[i]}' "$LOG" | tail -8 ;;
  key)
    shift
    for k in "$@"; do say "$k" /tmp/nuvio-key; sleep 0.45; done ;;

  shot)
    name="${2:-shot}"
    region="$3"
    # The .bmp belongs to the app and /tmp is sticky, so it cannot be deleted
    # here: wait for its MTIME to change, or the previous capture comes back.
    before=$(stat -f %m /tmp/nuvio-shot.bmp 2>/dev/null || echo 0)
    say shot /tmp/nuvio-shot-req
    for _ in $(seq 1 40); do
      sleep 0.25
      now=$(stat -f %m /tmp/nuvio-shot.bmp 2>/dev/null || echo 0)
      [ "$now" != "$before" ] && break
    done
    sips -s format png /tmp/nuvio-shot.bmp --out "$OUT/$name.png" >/dev/null
    if [ -n "$region" ]; then
      line=$("$0" rects | awk -v r="$region" '$2==r{print; exit}')
      [ -n "$line" ] || { echo "no region '$region' on this screen" >&2; exit 1; }
      # The app reports in 1920x1080 space; the Mac buffer is retina, so the
      # rect scales by whatever the capture's real width is.
      W=$(sips -g pixelWidth "$OUT/$name.png" | awk '/pixelWidth/{print $2}')
      echo "$line" | awk -v s="$W" -v f="$OUT/$name.png" '{
        split($3,p,","); split($4,d,"x"); k=s/1920
        pad=12
        x=(p[1]-pad)*k; y=(p[2]-pad)*k; w=(d[1]+pad*2)*k; h=(d[2]+pad*2)*k
        if (x<0) x=0; if (y<0) y=0
        printf "sips -c %d %d --cropOffset %d %d %s --out %s\n", h, w, y, x, f, f
      }' | sh >/dev/null
    fi
    echo "$OUT/$name.png" ;;

  *) sed -n '2,18p' "$0"; exit 1 ;;
esac
