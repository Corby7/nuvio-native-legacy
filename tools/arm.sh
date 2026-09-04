#!/bin/bash
# Builds for ARM, installs on the TV through Developer Mode and verifies it.
#
#   bash tools/arm.sh            # build, package, install, launch and verify
#   bash tools/arm.sh --build    # build only, no package and no TV
#   bash tools/arm.sh --ipk      # same as the default, but keeps the .ipk
#
#   NUVIO_TV_DEVICE=lgc3         # ares device name (see ares-setup-device --list)
#   NUVIO_NO_INSTALL=1           # build and package, stop before the TV
#
# ONE-TIME SETUP: the Developer Mode ssh key has a passphrase, so load it into
# the agent once per session or the verification step cannot read the TV:
#
#   ssh-add ~/.ssh/lgc3_webos
#
# It DOES go through the installer now, and that is a downgrade forced by the
# target, not a preference — see the long note at the install step.
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

# The TV is addressed by the DEVICE NAME registered with ares-setup-device, not
# by an IP: the Developer Mode ssh needs a key and a passphrase, and both live in
# ~/.webos/tv/novacom-devices.json under that name. `ares-setup-device --list`
# shows what is configured; this one is prisoner@192.168.1.100:9922.
#
# The address moved once already (the old default, 192.168.1.32, is a lease the
# TV no longer holds) and cost a deploy each time it was hardcoded. A name that
# resolves through the ares config cannot go stale that way.
TV_DEV="${NUVIO_TV_DEVICE:-lgc3}"
APP_ID="space.nuvio.native.legacy"
# The sibling web checkout supplies the whole ares toolchain, and has been called
# both NuvioWeb-0.3.38-beta and NuvioWeb. Try each rather than hard-coding one.
ARESDIR=""
for c in ../NuvioWeb-0.3.38-beta ../NuvioWeb; do
  [ -x "$c/node_modules/.bin/ares-package" ] && ARESDIR="$c/node_modules/.bin"
done
[ -n "$ARESDIR" ] || { echo "ares tools not found in ../NuvioWeb*/node_modules/.bin" >&2; exit 1; }
ARES="$ARESDIR/ares-package"

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

# BUILD ID COMPILED INTO THE BINARY, printed by main.c at startup.
#
# The md5 further down proves what is ON DISK. It does NOT prove what is
# RUNNING, and this deploy now goes through appInstallService — the very service
# this file already records as answering success without replacing the binary
# (three deploys lost that way, the app running an old build for 2h30 while the
# log said success). Only the process saying its own build id closes that.
#
# It goes in the env-file, so the "did every -D land" check below covers it for
# free.
NV_BUILD=$(date +%Y%m%d-%H%M%S)
echo "NV_BUILD=$NV_BUILD" >> "$ENVF"
echo "==> build $NV_BUILD"
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
    -DNV_BUILD="\"$NV_BUILD\"" \
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

# BUILD STAMP IN THE TITLE, applied BEFORE packaging because the .ipk carries
# appinfo.json — stamping afterwards would ship the previous build's number.
#
# "is it still the old build" cannot be answered by looking at the screen: the
# binary's md5 proves what is on DISK, not what was LAUNCHED, and the TV has two
# Nuvio apps (this one and the web "Nuvio TV") — opening the wrong tile gives
# exactly the same symptom. With the first 8 digits of the md5 in the title, the
# launcher answers for itself which build is there.
echo "==> stamping the title with the build"
STAMP=$(md5 -q nuvio-proto.arm 2>/dev/null || md5sum nuvio-proto.arm | cut -d' ' -f1)
STAMP=${STAMP:0:8}
# The source keeps the (BUILD) placeholder; only the packaged copy is stamped.
# Rewriting deploy/app/appinfo.json in place would make the placeholder survive
# exactly one build and then be gone from the tree.
sed "s/(BUILD)/($STAMP)/" deploy/app/appinfo.json > /tmp/appinfo.stamped.json
echo "    $STAMP"

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

# --build stops here: it is the "does it compile" path and wants nothing to do
# with packaging or with the TV.
[ "$1" = "--build" ] && exit 0

# THE PACKAGE IS NO LONGER OPTIONAL. Under Developer Mode there is no direct
# copy into the app directory, so the .ipk IS the deploy — `--ipk` now only
# means "leave it in the tree afterwards".
if true; then
  echo "==> packaging (without credentials)"
  STAGE=$(mktemp -d); TRASH="$TRASH $STAGE"
  cp -R deploy/app "$STAGE/app"
  # The stamped title goes into the STAGED copy, never into the tree: rewriting
  # deploy/app/appinfo.json in place would consume the (BUILD) placeholder on the
  # first build and it would never come back.
  cp /tmp/appinfo.stamped.json "$STAGE/app/appinfo.json"
  # cache/ is a RUNTIME cache, not package art: megabytes of downloaded images
  # that the app fetches again on its own.
  rm -rf "$STAGE/app/art/cache"
  for f in $PERSONAL_FILES; do rm -f "$STAGE/app/art/$f"; done

  # THE EXIT CODE OF ares-package IS NOT EVIDENCE. Version 3.2.4 writes the
  # package correctly and THEN throws in its own cleanup:
  #   ares-package ERR! uncaughtException TypeError: rimraf is not a function
  # It exits non-zero having done the job. Under `set -e` that aborts a deploy
  # that actually succeeded, so the check below asks the FILE, not the tool —
  # the same rule the rest of this script already follows.
  "$ARES" "$STAGE/app" -o . || echo "    (ares-package exited non-zero; checking the file instead)"
  IPK=$(ls -t ./*.ipk 2>/dev/null | head -1)
  if [ -z "$IPK" ] || [ ! -s "$IPK" ]; then
    echo "    ABORTED: ares-package produced no .ipk"
    exit 1
  fi

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

[ "$NUVIO_NO_INSTALL" = "1" ] && { echo "==> NUVIO_NO_INSTALL=1: stopping before the TV"; exit 0; }

# THE DIRECT COPY IS GONE, and not by preference.
#
# The old path scp'd the binary straight into the app directory as root over
# port 22, and it was right to: appInstallService answers `"returnValue": true`
# and `statusValue: 264` (installed) and DOES NOT REPLACE THE BINARY. Three
# deploys were lost to that — the app ran an old version for 2h30 while the log
# said success.
#
# That TV (a rooted C9) is gone. This one is an OLED55C32LA on webOS 23, running
# Developer Mode only: port 22 is closed, ssh is prisoner@...:9922, and the app
# directory is NOT writable by prisoner (measured — `test -w` says no). So the
# installer is the only way in, which means the verification below is not a
# nicety, it is the thing that stands between a deploy and a hope.
echo "==> installing on $TV_DEV"
"$ARESDIR/ares-install" -d "$TV_DEV" "$IPK"

# LAUNCH ONCE. Never in a loop: every start consumes backend quota, and when it
# runs out the login fails, the TV falls back to an anonymous session and syncs
# the wrong account — which looks like a bug in the app.
#
# Launching does NOT restart an app that is already running, it only brings it
# to the front. Close first so what starts is actually this build.
echo "==> launching"
"$ARESDIR/ares-launch" -d "$TV_DEV" --close "$APP_ID" >/dev/null 2>&1 || true
"$ARESDIR/ares-launch" -d "$TV_DEV" "$APP_ID"

# ---------------------------------------------------------------- verifying
#
# Two different questions, and the old script only ever answered the first:
#   1. is the right binary ON DISK?   -> md5
#   2. is that binary what is RUNNING? -> the build id it prints at startup
# Question 2 is the one appInstallService can lie about.
#
# The ssh details come from the SAME ares config the install used, so the check
# can never end up pointing at a different TV than the deploy did.
eval "$(python3 - "$TV_DEV" <<'PY'
import json, os, sys
name = sys.argv[1]
p = os.path.expanduser("~/.webos/tv/novacom-devices.json")
try:
    devices = json.load(open(p))
except Exception:
    sys.exit(0)
for e in devices:
    if e.get("name") == name:
        key = e.get("privateKey", {}).get("openSsh", "")
        print("TV_HOST=%s" % e.get("host", ""))
        print("TV_PORT=%s" % e.get("port", "9922"))
        print("TV_USER=%s" % e.get("username", "prisoner"))
        print("TV_KEY=%s" % (os.path.expanduser("~/.ssh/" + key) if key else ""))
        break
PY
)"

APPDIR=/media/developer/apps/usr/palm/applications/$APP_ID
if [ -z "$TV_HOST" ] || [ ! -f "$TV_KEY" ]; then
  echo "    NOT VERIFIED: no ssh details for '$TV_DEV' in ~/.webos/tv/novacom-devices.json"
  exit 1
fi
# BatchMode: the Developer Mode key has a passphrase, and without this ssh would
# PROMPT — a prompt inside a deploy script is a hang, which is the worst way for
# this to fail. Load it once with `ssh-add "$TV_KEY"` and it stays in the agent.
# HostKeyAlgorithms: the TV runs OpenSSH 6.1 and offers only ssh-rsa, which
# modern clients refuse by default ("no matching host key type found").
SSH="ssh -p ${TV_PORT:-9922} -i $TV_KEY -o StrictHostKeyChecking=no \
     -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedKeyTypes=+ssh-rsa \
     -o BatchMode=yes -o ConnectTimeout=10 ${TV_USER:-prisoner}@$TV_HOST"

echo "==> verifying"
LOCAL=$(md5 -q nuvio-proto.arm 2>/dev/null || md5sum nuvio-proto.arm | cut -d' ' -f1)
REMOTE=$($SSH "md5sum $APPDIR/nuvio-proto 2>/dev/null | cut -d' ' -f1" 2>/dev/null | tr -d '\r')
if [ -z "$REMOTE" ]; then
  echo "    NOT VERIFIED: could not read the TV over ssh."
  echo "    If it asked for a passphrase, run:  ssh-add $TV_KEY"
  exit 1
fi
if [ "$LOCAL" != "$REMOTE" ]; then
  echo "    FAILED: on disk $REMOTE, built $LOCAL — the installer did not swap the binary"
  exit 1
fi
echo "    on disk ok ($LOCAL)"

# The app has to boot far enough to write its first log line.
RUNNING=""
for _ in 1 2 3 4 5 6 7 8; do
  sleep 2
  RUNNING=$($SSH "sed -n 's/^\[main\] build //p' /tmp/nuvio.log 2>/dev/null | head -1" 2>/dev/null | tr -d '\r')
  [ -n "$RUNNING" ] && break
done
if [ -z "$RUNNING" ]; then
  echo "    NOT VERIFIED: the app never wrote a build line to /tmp/nuvio.log"
  exit 1
fi
if [ "$RUNNING" != "$NV_BUILD" ]; then
  echo "    FAILED: running build $RUNNING, expected $NV_BUILD"
  echo "    (installed but not swapped, or an older instance is still up)"
  exit 1
fi
echo "    running ok ($RUNNING)"
