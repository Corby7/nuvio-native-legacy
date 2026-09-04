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
eval cc src/*.c -o /tmp/nuvio-native-legacy-mac -O1 -g "$ENV_D" \
  -I/opt/homebrew/include -I/opt/homebrew/include/SDL2 \
  -L/opt/homebrew/lib -lSDL2 -lSDL2_image -lSDL2_ttf \
  -framework OpenGL -Wno-deprecated-declarations
exec /tmp/nuvio-native-legacy-mac "$(pwd)/deploy/app/art" "$@"
