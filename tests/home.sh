#!/bin/bash
set -eu
cd "$(dirname "$0")/.."
flags=()
if [ "${SANITIZE:-0}" = 1 ]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
# Linka somente a lógica exercitada, sem inicializar SDL/AppKit nem vídeo.
# Além de ser rápido, permite ASAN no macOS beta sem o diálogo do loader SDL.
cc "${flags[@]}" src/catalog.c src/focus.c src/settings.c src/collections.c src/js.c tests/home_layout.c \
  -Isrc -o /tmp/nuvio-home-tests -O1 -g -ffunction-sections -fdata-sections \
  -Wl,-dead_strip -I/opt/homebrew/include -I/opt/homebrew/include/SDL2 \
  -Wno-deprecated-declarations -Wno-macro-redefined
/tmp/nuvio-home-tests
