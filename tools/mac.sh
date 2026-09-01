#!/bin/bash
# Compila e roda no Mac, com a mesma pasta de arte do pacote da TV.
set -e
cd "$(dirname "$0")/.."
cc src/*.c -o /tmp/nuvio-native-legacy-mac -O1 -g \
  -I/opt/homebrew/include -I/opt/homebrew/include/SDL2 \
  -L/opt/homebrew/lib -lSDL2 -lSDL2_image -lSDL2_ttf \
  -framework OpenGL -Wno-deprecated-declarations
exec /tmp/nuvio-native-legacy-mac "$(pwd)/deploy/app/art" "$@"
