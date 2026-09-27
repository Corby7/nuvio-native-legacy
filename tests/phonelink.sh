#!/bin/bash
# phonelink.c on its own, over a real socket on loopback.
#
#   bash tests/phonelink.sh
set -eu
cd "$(dirname "$0")/.."
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
cc -std=gnu99 -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1 \
  tests/phonelink.c src/phonelink.c -Isrc -Wall -Wextra -lpthread -o "$TMP/t"
"$TMP/t"
