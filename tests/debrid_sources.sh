#!/bin/bash
# debrid.c and sourcepref.c against canned answers. No network, no SDL runtime.
#
#   bash tests/debrid_sources.sh
set -eu
cd "$(dirname "$0")/.."
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
cc -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1 \
  tests/debrid_sources.c src/debrid.c src/sourcepref.c src/js.c \
  -Isrc -I/opt/homebrew/include -I/opt/homebrew/include/SDL2 \
  -Wall -Wextra -Wno-unused-parameter -o "$TMP/t"
"$TMP/t"
