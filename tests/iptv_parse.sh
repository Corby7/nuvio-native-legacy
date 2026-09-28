#!/bin/bash
# iptv_parse.c in isolation: M3U playlists, XMLTV guides, and a gzipped guide
# made here with the system gzip (two members, the way some panels serve it).
#
#   bash tests/iptv_parse.sh
set -eu
cd "$(dirname "$0")/.."
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
cc -std=c99 -D_DEFAULT_SOURCE -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1 \
  tests/iptv_parse.c src/iptv_parse.c -Isrc -Wall -Wextra -ldl -o "$TMP/t"
"$TMP/t" --dump-xml "$TMP/a.xml" "$TMP/b.xml"
{ gzip -c "$TMP/a.xml"; gzip -c "$TMP/b.xml"; } > "$TMP/guide.xml.gz"
"$TMP/t" --dump-big "$TMP/big.xml"
gzip -c "$TMP/big.xml" > "$TMP/big.xml.gz"
"$TMP/t" "$TMP/guide.xml.gz" "$TMP/big.xml.gz"
