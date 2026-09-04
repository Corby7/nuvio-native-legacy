#!/bin/bash
# Builds for ARM and installs on the TV by DIRECT COPY. Full cycle (~30s).
#
#   bash tools/arm.sh            # build, copy, check and launch
#   bash tools/arm.sh --build    # build only
#   bash tools/arm.sh --ipk      # also produce the .ipk (for distribution)
#
# It does NOT use appInstallService, and the reason is written at the send step:
# that service answers success and does not swap the binary.
#
# The image comes from tools/Dockerfile:
#   docker build --platform linux/arm64 -t nuvio-webos-sdk tools/
#
# -ldl is mandatory: video.c opens libluna-service2 with dlopen. The old
# runbook's compile line lacked it, and the link failed on 'dlsym@@GLIBC_2.4'.
#
# Do NOT add -lcurl. net.c opens libcurl with dlopen at runtime, trying so.5 and
# then so.4. Linking curl creates a hard dependency on the SDK's libcurl.so.4,
# and the TV only has /usr/lib/libcurl.so.5 — the binary does not even start,
# with "libcurl.so.4 => not found" and no message visible on screen.
set -e
cd "$(dirname "$0")/.."

TV_IP="${NUVIO_TV_IP:-192.168.1.32}"
TV_PASS="${NUVIO_TV_PASS:-alpine}"
APP_ID="space.nuvio.native.legacy"
# The sibling web checkout supplies ares-package, and has been called both
# NuvioWeb-0.3.38-beta and NuvioWeb. Try each rather than hard-coding one.
ARES=""
for c in ../NuvioWeb-0.3.38-beta ../NuvioWeb; do
  [ -x "$c/node_modules/.bin/ares-package" ] && ARES="$c/node_modules/.bin/ares-package"
done
[ -n "$ARES" ] || { echo "ares-package not found in ../NuvioWeb*/node_modules/.bin" >&2; exit 1; }

echo "==> building for ARM"
# The server configuration enters through an ENVIRONMENT VARIABLE and the -D
# flags are assembled inside the container. Passing them on the `sh -c` line
# would need quotes inside quotes and the failure would be SILENT: the macro
# arrives empty, the binary compiles, installs, opens — and the login screen
# says "package built without a server". That is what happened on this
# feature's first deploy.
# ONE cleanup, with everything in it: `trap` does not accumulate — a second
# `trap ... EXIT` replaces the first, and the file holding the anon key would be
# left behind in /tmp every time --ipk was used.
TRASH=""
cleanup() { [ -n "$TRASH" ] && rm -rf $TRASH; }
trap cleanup EXIT
ENVF=$(mktemp); TRASH="$TRASH $ENVF"
tools/env.sh --env-file "$ENVF"
docker run --rm --platform linux/arm64 --env-file "$ENVF" \
  -v "$PWD":/work nuvio-webos-sdk sh -c '
  SR=$NUVIO_SYSROOT
  arm-webos-linux-gnueabi-gcc src/*.c -o nuvio-proto.arm -O2 \
    -DNV_SUPABASE_URL="\"$NV_SUPABASE_URL\"" \
    -DNV_SUPABASE_ANON_KEY="\"$NV_SUPABASE_ANON_KEY\"" \
    -DNV_TV_LOGIN_BASE="\"$NV_TV_LOGIN_BASE\"" \
    -DNV_TRAKT_CLIENT_ID="\"$NV_TRAKT_CLIENT_ID\"" \
    -DNV_TRAKT_CLIENT_SECRET="\"$NV_TRAKT_CLIENT_SECRET\"" \
    -DNV_SIMKL_CLIENT_ID="\"$NV_SIMKL_CLIENT_ID\"" \
    -DNV_SIMKL_APP="\"$NV_SIMKL_APP\"" \
    -I$SR/usr/include -I$SR/usr/include/SDL2 \
    -lSDL2 -lSDL2_image -lSDL2_ttf -lGLESv2 -lEGL -ldl -lpthread -lm'

# CHECKS that the configuration really made it into the binary. Without this the
# only symptom is the login screen saying the package shipped with no server,
# already on the TV. The check tests EVERY key, reading the VALUES FROM THE
# ENV-FILE.
#
# It used to read from the SHELL's environment, and that made it useless without
# looking useless: the variables only exist INSIDE the container (--env-file
# injects them there), so on the host they were all empty, each one fell into
# the "warning: empty" branch and the check was SKIPPED. A guard that switches
# itself off is worse than no guard, because it reassures.
while IFS='=' read -r NAME VALUE; do
  [ -z "$NAME" ] && continue
  if [ -z "$VALUE" ]; then
    echo "    warning: $NAME empty in local.properties"
    continue
  fi
  if ! strings nuvio-proto.arm 2>/dev/null | grep -qF "$VALUE"; then
    echo "    ABORTED: $NAME did not make it into the ARM binary"
    exit 1
  fi
done < "$ENVF"

cp nuvio-proto.arm deploy/app/nuvio-proto
rm -f ./*.ipk

# The .ipk only matters for DISTRIBUTION (installing on another TV, publishing).
# The development cycle does not go through it — see the note below.
#
# IT PACKAGES FROM A CLEAN COPY, never from deploy/app directly. A concrete
# reason: `ares-package deploy/app` takes the WHOLE folder, and art/ holds a
# PERSON's credentials — the Trakt token, the addon URLs with the debrid key
# embedded, the TMDB key and the mdblist one (that last at mode 0600, it is so
# secret). An .ipk built that way hands all of it to whoever installs it. Until
# the version with login this could not be otherwise, because the app depended
# on the files; now it does not, and shipping them anyway would just be
# carelessness.
#
# settings.txt goes for the same reason, with smaller damage: it is the LAYOUT
# preference of whoever built the package, and it would arrive looking like the
# preference of whoever installed it.
#
# The list carries BOTH spellings, English and the pre-1.0.1 Portuguese ones: an
# older art/ folder can still be sitting on the machine that builds the package.
PERSONAL_FILES="trakt.txt addons.txt tmdb.txt mdblist.txt
                settings.txt progress.txt cloud.txt session.txt profile.txt
                client.txt
                ajustes.txt progresso.txt nuvem.txt sessao.txt perfil.txt
                cliente.txt"

if [ "$1" = "--ipk" ]; then
  echo "==> packaging (without credentials)"
  STAGE=$(mktemp -d); TRASH="$TRASH $STAGE"
  cp -R deploy/app "$STAGE/app"
  # cache/ is a RUNTIME cache, not package art: megabytes of downloaded images
  # that the app fetches again on its own.
  rm -rf "$STAGE/app/art/cache"
  for f in $PERSONAL_FILES; do rm -f "$STAGE/app/art/$f"; done

  "$ARES" "$STAGE/app" -o .
  IPK=$(ls -t ./*.ipk | head -1)

  # CHECKS the FINISHED package, not the folder it came from. The exclusion list
  # above is an intent; the test below is the fact.
  #
  # A MEASURED TRAP: the .ipk is a Debian package (an `ar` archive holding
  # debian-binary + control.tar.gz + data.tar.gz). `tar tzf package.ipk` LISTS,
  # with no error at all, only those three names — never the app's files. A check
  # written that way always passes, secret included. You have to unpack the `ar`
  # and list data.tar.gz.
  LISTING=$(cd "$STAGE" && ar x "$OLDPWD/$IPK" 2>/dev/null && tar tzf data.tar.gz 2>/dev/null)
  if [ -z "$LISTING" ]; then
    echo "    ABORTED: could not read the package to check it; I will not claim it is clean"
    rm -f "$IPK"
    exit 1
  fi
  LEAKED=""
  for f in $PERSONAL_FILES; do
    printf '%s\n' "$LISTING" | grep -q "art/$f$" && LEAKED="$LEAKED $f"
  done
  if [ -n "$LEAKED" ]; then
    echo "    ABORTED: the package carries credentials ->$LEAKED"
    rm -f "$IPK"
    exit 1
  fi
  echo "    $IPK ($(du -h "$IPK" | cut -f1)) — without art/{$(echo $PERSONAL_FILES | tr ' ' ',')}"
fi

[ "$1" = "--build" ] && exit 0

# THE TV IS ROOTED: direct copy, without going through the installer.
#
# appInstallService answers `"returnValue": true` and `statusValue: 264`
# (installed) and DOES NOT REPLACE THE BINARY — it writes art/ and leaves
# nuvio-proto and appinfo.json untouched. I lost three deploys believing they
# had landed: the app on the TV ran an old version for 2h30 while the log said
# success.
#
# With root there is no reason for the middleman. scp + mv + chmod does the same
# in two seconds, and the md5 at the end PROVES it went up — that is the real
# lesson: a deploy without verification is hope, not delivery.
#
# ares-install is no use here either: it expects Developer Mode's
# prisoner@<ip>:9922, and this TV does not run Developer Mode — it is root on 22
# with the password alpine.
APPDIR=/media/developer/apps/usr/palm/applications/$APP_ID
SSH="sshpass -p $TV_PASS ssh -o StrictHostKeyChecking=no"
SCP="sshpass -p $TV_PASS scp -o StrictHostKeyChecking=no -q"

# THE DIRECTORY MAY NOT EXIST: the owner may have uninstalled the app from the
# TV, and then every scp below fails with "No such file or directory" — which is
# exactly what happened. Creating it first makes the deploy able to REINSTALL,
# not just update.
$SSH "root@$TV_IP" "mkdir -p $APPDIR"

echo "==> sending the binary to $TV_IP"
$SCP nuvio-proto.arm "root@$TV_IP:$APPDIR/nuvio-proto.new"
# Rename instead of overwrite: if the app is running, the executable is mapped
# and a direct write fails with ETXTBSY. The rename swaps the inode.
$SSH "root@$TV_IP" "cd $APPDIR && mv -f nuvio-proto.new nuvio-proto && chmod 755 nuvio-proto"

# THE ART changes rarely, but when it does (a new icon, a new source) it has to
# go along. BUILD STAMP IN THE TITLE.
#
# "is it still the old build" cannot be answered by looking at the screen: the
# binary's md5 proves what is on DISK, not what was LAUNCHED, and the TV has two
# Nuvio apps (this one and the web "Nuvio TV") — opening the wrong tile gives
# exactly the same symptom. With the first 8 digits of the md5 in the title, the
# launcher answers for itself which build is there.
echo "==> stamping the title with the build"
STAMP=$(md5 -q nuvio-proto.arm 2>/dev/null || md5sum nuvio-proto.arm | cut -d' ' -f1)
STAMP=${STAMP:0:8}
sed "s/(BUILD)/($STAMP)/" deploy/app/appinfo.json > /tmp/appinfo.stamped.json
cp /tmp/appinfo.stamped.json deploy/app/appinfo.json.stamped

$SCP /tmp/appinfo.stamped.json "root@$TV_IP:$APPDIR/appinfo.json"

echo "==> syncing art"
# --exclude art/cache: it is a RUNTIME cache, not package art. With it the tar
# went past 49 MB (27 MB of posters downloaded on the Mac alone) and extraction
# in the TV's busybox died halfway — and whatever came AFTER "cache/" in
# alphabetical order, "brands/" included, vanished silently. That is how the
# Trakt wordmark "went up" three times without ever arriving.
#
# Without the cache the tar drops to a few MB. The TV rebuilds its own, and no
# longer inherits the Mac's uid — the same trap the chown below remedies.
#
# 2>&1 and not 2>/dev/null: a hidden tar error is exactly what made the deploy
# lie. The lesson was already written here for the binary (the md5 at the end)
# and the art had been left out of it.
# The xattr noise filter stays on the MAC: the TV's sh is busybox ash and has no
# PIPESTATUS — trying to use it there gave "bad substitution" and broke the
# deploy.
#
# And the status comes from a local `set -o pipefail` around the pipe, not from
# PIPESTATUS indices, which turn ambiguous the moment you add a `|| true`.
if ! ( set -o pipefail
       tar czf - -C deploy/app --exclude 'art/cache' \
           --exclude 'appinfo.json.stamped' \
           art fonts icon.png \
         | $SSH "root@$TV_IP" "tar xzf - -C $APPDIR" ) 2>&1 \
     | grep -v 'unknown extended header keyword'; then
  :
fi
if ! $SSH "root@$TV_IP" "test -f $APPDIR/art/brands/trakt.png"; then
  echo "    FAILED: the art did not reach the TV"; exit 1
fi
rm -f deploy/app/appinfo.json.stamped
# The `core` from an old crash sits in the app's directory and weighs 118 MB on
# a 4.2 G partition. It is no use once the report has been generated.
$SSH "root@$TV_IP" "rm -f $APPDIR/core; rm -f /tmp/nuvio-shot-req /tmp/nuvio-shot.bmp" || true

# OWNER OF THE CACHE FOLDER. The tar is made on the Mac and extracted as ROOT on
# the TV, so art/cache inherits the Mac's uid (13888160) at mode 755. The app
# runs as uid 5152 and CANNOT WRITE there: every downloaded image was discarded
# silently, and the two decode threads kept re-fetching what could never be
# stored.
#
# MEASURED: 91 "decode failed" in the log with ZERO network errors. After the
# chown, 15 — and textures went from 87 to 99. Without this line the defect
# comes back on every deploy, and the symptom is "a poster that does not
# appear", which has already cost half a session.
$SSH "root@$TV_IP" "mkdir -p $APPDIR/art/cache && chown -R 5152:5000 $APPDIR/art && chmod -R u+rwX $APPDIR/art"

echo "==> verifying"
LOCAL=$(md5 -q nuvio-proto.arm 2>/dev/null || md5sum nuvio-proto.arm | cut -d' ' -f1)
REMOTO=$($SSH "root@$TV_IP" "md5sum $APPDIR/nuvio-proto | cut -d' ' -f1" 2>/dev/null | tr -d '\r')
if [ "$LOCAL" != "$REMOTO" ]; then
  echo "    FAILED: local $LOCAL != TV $REMOTO"
  exit 1
fi
echo "    ok ($LOCAL)"

echo "==> launching"
( sleep 2
  printf 'luna-send -n 1 -f luna://com.webos.applicationManager/launch '"'"'{"id":"%s"}'"'"'\n' "$APP_ID"
  sleep 3
  printf 'exit\n'
) | nc -w20 "$TV_IP" 23 | tr -d '\0' | grep returnValue
