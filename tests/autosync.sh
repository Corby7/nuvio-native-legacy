#!/bin/bash
# autosync.c's matcher in isolation, on synthetic films.
#
#   bash tests/autosync.sh
set -eu
cd "$(dirname "$0")/.."
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
cc -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1 \
  tests/autosync.c src/autosync.c src/lang.c -Isrc -Wall -Wextra -o "$TMP/t"
"$TMP/t"
