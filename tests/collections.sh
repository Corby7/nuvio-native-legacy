#!/bin/bash
set -eu
cd "$(dirname "$0")/.."
flags=(-O1 -g -ffunction-sections -fdata-sections -Wl,-dead_strip -Isrc -I/opt/homebrew/include -I/opt/homebrew/include/SDL2 -Wno-macro-redefined -Wno-deprecated-declarations)
if [ "${SANITIZE:-0}" = 1 ]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
cc "${flags[@]}" tests/collections_data.c src/js.c -o /tmp/nuvio-collections-data-tests
/tmp/nuvio-collections-data-tests
cc "${flags[@]}" tests/collections_focus.c src/js.c -o /tmp/nuvio-collections-focus-tests
/tmp/nuvio-collections-focus-tests
# The sprite-sheet cell, with a window and a GL context: it is the one part of
# the focus animation that only a real draw can check.
cc "${flags[@]}" tests/focus_sheet.c src/gfx.c src/tex_cache.c src/webp.c src/net.c \
  -L/opt/homebrew/lib -lSDL2 -lSDL2_image -framework OpenGL -o /tmp/nuvio-focus-sheet-tests
/tmp/nuvio-focus-sheet-tests
cc "${flags[@]}" tests/badges.c -o /tmp/nuvio-badges-tests
/tmp/nuvio-badges-tests
