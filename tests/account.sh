#!/bin/bash
# Account tests. THEY NEED THE NETWORK: they talk to the real server.
#
#   bash tests/account.sh
#
# The settings one needs no network. The sign-out one opens an ANONYMOUS session
# and writes it as if it were a user's. That is not a testing hack: the
# anonymous user has a real account on the server (the web app itself does this
# before asking for the login code), with addons seeded. It is the only way to
# exercise the whole cycle without somebody authorising on a phone.
set -e
cd "$(dirname "$0")/.."
ENV_D=$(tools/env.sh)
FONTES=$(ls src/*.c | grep -v 'src/main.c' | tr '\n' ' ')
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

echo "==> logout apaga o user previous"
eval cc tests/account_logout.c $FONTES -o "$TMP/logout" "$ENV_D" -Isrc \
  -I/opt/homebrew/include -I/opt/homebrew/include/SDL2 \
  -L/opt/homebrew/lib -lSDL2 -lSDL2_image -lSDL2_ttf \
  -framework OpenGL -Wno-deprecated-declarations -Wno-macro-redefined
NUVIO_DATA="$TMP/data" "$TMP/logout"

echo
echo "==> ajustes da account chegam com o sentido certo"
eval cc tests/account_settings.c $FONTES -o "$TMP/ajustes" "$ENV_D" -Isrc \
  -I/opt/homebrew/include -I/opt/homebrew/include/SDL2 \
  -L/opt/homebrew/lib -lSDL2 -lSDL2_image -lSDL2_ttf \
  -framework OpenGL -Wno-deprecated-declarations -Wno-macro-redefined
"$TMP/ajustes"

echo
echo "==> QR legivel por leitor de verdade"
cc tools/qr_dump.c src/qr.c -o "$TMP/qr" -Isrc
PY=${NUVIO_PY:-/tmp/qrvenv/bin/python}
if [ -x "$PY" ]; then "$PY" tools/qr_check.py "$TMP/qr"
else echo "   PULADO: sem venv com opencv (see o cabecalho de tools/qr_check.py)"; fi
