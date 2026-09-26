#!/bin/bash
# mkv.c in isolation, the network stubbed over a fixture file.
#
#   bash tests/mkv.sh
set -eu
cd "$(dirname "$0")/.."
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
cc -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1 \
  tests/mkv.c src/mkv.c -Isrc -Wall -Wextra -o "$TMP/t"
"$TMP/t"
