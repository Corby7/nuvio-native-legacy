#!/bin/bash
# Build and run on the Mac, against the same art folder the TV package uses.
#
# IT STAYS SIGNED IN BETWEEN RUNS. The session lives in $NUVIO_DATA, which here
# points at ~/.nuvio: sign in ONCE (scanning the QR on screen) and every
# following start opens with the account already in, because the refresh token
# renews itself.
# CONFIRMED: first start "[session] signed in as ..."; restarting without
# scanning anything, "[session] session restored ..." and
# "[addons] N from the account".
#
# To test as a NEW user (the first run of whoever installs it), point the
# variable at an empty folder:
#     NUVIO_DATA=/tmp/nuvio-new bash tools/mac.sh
#
# "Sign out", in Settings, erases the whole of ~/.nuvio (session, settings and
# progress) — after that you have to scan again.
set -e
cd "$(dirname "$0")/.."
# The server -D flags come from the SAME local.properties as the web app
# (tools/env.sh). Without them the app compiles, installs and opens — and the
# only symptom is the login screen saying "This package was built without a
# server." None of those values goes into a versioned source file.
ENV_D=$(tools/env.sh)
# Outside the package folder: the Mac must not write the session into deploy/.
export NUVIO_DATA="${NUVIO_DATA:-$HOME/.nuvio}"

# IT BUILDS INTO AN .app BUNDLE, and that is the only way to get the logo.
# macOS takes the Dock icon and the menu-bar name from the bundle the running
# executable sits in — SDL_SetWindowIcon is ignored here, and a bare binary in
# /tmp always draws the generic rocket. The bundle is still launched directly
# (not through `open`), so stdout stays in this terminal like before.
APP=/tmp/Nuvio-Legacy.app
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"

# The icns comes from the BADGED master, the same file the webOS tiles are cut
# from (tools/build-icons.sh, derived from assets/icon-1024.png — itself the
# master the Electron desktop build uses). The badge matters here too: the
# desktop app is also called Nuvio and also lives in the Dock, so without it
# cmd-tab shows the same mark twice. Rebuilt only when the master is newer,
# because iconutil takes a second.
ICON_SRC=assets/icon-1024-legacy.png
ICNS="$APP/Contents/Resources/nuvio.icns"
if [ "$ICON_SRC" -nt "$ICNS" ]; then
  SET=$(mktemp -d)/nuvio.iconset; mkdir -p "$SET"
  for s in 16 32 128 256 512; do
    sips -z $s $s "$ICON_SRC" --out "$SET/icon_${s}x${s}.png" >/dev/null
    sips -z $((s*2)) $((s*2)) "$ICON_SRC" --out "$SET/icon_${s}x${s}@2x.png" >/dev/null
  done
  iconutil -c icns "$SET" -o "$ICNS"
  rm -rf "$(dirname "$SET")"
fi

# CFBundleIconFile names the icns WITHOUT the extension; with it, the Dock
# silently falls back to the generic icon and nothing is logged.
cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>Nuvio Legacy</string>
  <key>CFBundleDisplayName</key><string>Nuvio Legacy</string>
  <key>CFBundleIdentifier</key><string>space.nuvio.native.legacy</string>
  <key>CFBundleExecutable</key><string>nuvio-legacy</string>
  <key>CFBundleIconFile</key><string>nuvio</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>1.0.1</string>
  <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
PLIST

eval cc src/*.c -o "$APP/Contents/MacOS/nuvio-legacy" -O1 -g "$ENV_D" \
  -I/opt/homebrew/include -I/opt/homebrew/include/SDL2 \
  -L/opt/homebrew/lib -lSDL2 -lSDL2_image -lSDL2_ttf \
  -framework OpenGL -Wno-deprecated-declarations

# Rewriting the executable leaves the old ad-hoc signature stale, and the Dock
# caches the icon per bundle: re-sign and touch, or the first run after a
# rebuild can still show the previous icon.
codesign -f -s - "$APP" >/dev/null 2>&1 || true
touch "$APP"

exec "$APP/Contents/MacOS/nuvio-legacy" "$(pwd)/deploy/app/art" "$@"
