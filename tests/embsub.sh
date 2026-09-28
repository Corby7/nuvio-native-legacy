#!/bin/bash
# The app's own embedded subtitles (mkv.c, embsub_text.c, spu.c) in isolation.
#
#   bash tests/embsub.sh
set -eu
cd "$(dirname "$0")/.."
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
cc -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1 \
  tests/embsub.c src/mkv.c src/embsub_text.c src/spu.c src/subtitle.c src/subcharset.c \
  -Isrc -Wall -Wextra -lpthread -o "$TMP/t"
"$TMP/t"
