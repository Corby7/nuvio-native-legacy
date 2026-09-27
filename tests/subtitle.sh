#!/bin/bash
# subtitle.c and subcharset.c in isolation, the network stubbed.
#
#   bash tests/subtitle.sh
set -eu
cd "$(dirname "$0")/.."
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
cc -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1 \
  tests/subtitle.c src/subtitle.c src/subcharset.c -Isrc -Wall -Wextra -lpthread -o "$TMP/t"
"$TMP/t"
