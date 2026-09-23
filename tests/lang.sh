#!/bin/bash
# lang.c in isolation.
#
#   bash tests/lang.sh
set -eu
cd "$(dirname "$0")/.."
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
cc -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1 \
  tests/lang.c src/lang.c -Isrc -Wall -Wextra -o "$TMP/t"
"$TMP/t"
