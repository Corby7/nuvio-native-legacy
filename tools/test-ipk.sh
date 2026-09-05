#!/bin/bash
# Proves the .ipk carries NO personal credential — without needing docker.
#
# Runs only arm.sh's packaging step (the same staging area, the same exclusion
# list, the same check) using the nuvio-proto already in deploy/app. It exists
# because the normal cycle requires building for ARM, and nobody is going to run
# a 30s build just to check a list of files.
#
# TWO TRAPS this test exists to keep from coming back:
#   1. `ares-package deploy/app` takes the WHOLE folder, and art/ holds the
#      Trakt token, the addon URLs with a debrid key in them, the TMDB key and
#      the mdblist one (mode 0600).
#   2. the .ipk is a Debian package: `tar tzf package.ipk` lists, WITH NO ERROR,
#      only debian-binary / control.tar.gz / data.tar.gz. A check written that
#      way always passes, secret included.
#
# Verified both ways: with the exclusion it comes out clean; letting trakt.txt
# in on purpose, the test CATCHES it and returns 1.
set -e
cd "$(dirname "$0")/.."
# The sibling web checkout supplies ares-package, and has been called both
# NuvioWeb-0.3.38-beta and NuvioWeb. Try each rather than hard-coding one.
ARES=""
for c in ../NuvioWeb-0.3.38-beta ../NuvioWeb; do
  [ -x "$c/node_modules/.bin/ares-package" ] && ARES="$c/node_modules/.bin/ares-package"
done
[ -n "$ARES" ] || { echo "ares-package not found in ../NuvioWeb*/node_modules/.bin" >&2; exit 1; }
PERSONAL_FILES="trakt.txt addons.txt tmdb.txt mdblist.txt
                settings.txt progress.txt cloud.txt session.txt profile.txt
                client.txt
                ajustes.txt progresso.txt nuvem.txt sessao.txt perfil.txt
                cliente.txt"
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
cp -R deploy/app "$STAGE/app"
rm -rf "$STAGE/app/art/cache"
for f in $PERSONAL_FILES; do rm -f "$STAGE/app/art/$f"; done

OUTDIR=$(mktemp -d)
"$ARES" "$STAGE/app" -o "$OUTDIR" >/dev/null
IPK=$(ls -t "$OUTDIR"/*.ipk | head -1)

LISTING=$(cd "$STAGE" && ar x "$IPK" 2>/dev/null && tar tzf data.tar.gz 2>/dev/null)
[ -z "$LISTING" ] && { echo "ABORTED: could not read the package to check it"; exit 1; }

echo "test package: $(du -h "$IPK" | cut -f1)"
echo "art/*.txt files inside the package:"
printf '%s\n' "$LISTING" | grep -E "art/.*\.txt$" | sed 's|.*/art/|  |' | sort
LEAKED=""
for f in $PERSONAL_FILES; do
  printf '%s\n' "$LISTING" | grep -q "art/$f$" && LEAKED="$LEAKED $f"
done
rm -rf "$OUTDIR"
if [ -n "$LEAKED" ]; then echo "FAILED: the package carries a credential ->$LEAKED"; exit 1; fi
echo "OK: no credential in the package"
