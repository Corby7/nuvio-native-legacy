#!/bin/bash
set -eu
cd "$(dirname "$0")/.."
flags=()
if [ "${SANITIZE:-0}" = 1 ]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
cc "${flags[@]}" src/catalog.c tests/cw_progress.c -Isrc -o /tmp/nuvio-cw-progress-tests -O1 -g \
  -ffunction-sections -fdata-sections -Wl,-dead_strip \
  -I/opt/homebrew/include -I/opt/homebrew/include/SDL2 \
  -Wno-deprecated-declarations -Wno-macro-redefined
/tmp/nuvio-cw-progress-tests
