#!/bin/bash
# Prints the playback failure log — every file that would not open or would not
# keep playing, one line each (see src/failures.h for what the fields mean).
#
#   bash tools/failures.sh           # from the TV
#   bash tools/failures.sh --mac     # from this Mac's ~/.nuvio
#   bash tools/failures.sh --clear   # empty the TV's log once it has been read
#
# Line format:
#   when | stage | title SxEy (imdb) | reason | addon / service | res range
#   source codec audio container size | file name | host
set -e
cd "$(dirname "$0")/.."

LOG=playback-failures.log
if [ "$1" = "--mac" ]; then
  cat "${NUVIO_DATA:-$HOME/.nuvio}/$LOG" 2>/dev/null || echo "(no failures logged on this Mac)"
  exit 0
fi

TV_DEV="${NUVIO_TV_DEVICE:-lgc3}"
APP_ID=space.nuvio.native.legacy
# The same lookup arm.sh does, so this reads the TV that arm.sh deploys to.
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
if [ -z "$TV_HOST" ] || [ ! -f "$TV_KEY" ]; then
  echo "no ssh details for '$TV_DEV' in ~/.webos/tv/novacom-devices.json" >&2
  exit 1
fi
SSH="ssh -p ${TV_PORT:-9922} -i $TV_KEY -o StrictHostKeyChecking=no \
     -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedKeyTypes=+ssh-rsa \
     -o BatchMode=yes -o ConnectTimeout=10 ${TV_USER:-prisoner}@$TV_HOST"

# The app's data folder on the TV is its own .nuvio (HOME inside the jail);
# /media/developer/temp/nuvio is data.c's fallback candidate.
DIRS="/media/developer/apps/usr/palm/applications/$APP_ID/.nuvio /media/developer/temp/nuvio"
if [ "$1" = "--clear" ]; then
  $SSH "for d in $DIRS; do [ -f \$d/$LOG ] && : > \$d/$LOG; done; true"
  echo "cleared"
  exit 0
fi
$SSH "for d in $DIRS; do [ -f \$d/$LOG ] && cat \$d/$LOG && exit 0; done; echo '(no failures logged on the TV)'" \
  || { echo "could not reach the TV — if it asked for a passphrase: ssh-add $TV_KEY" >&2; exit 1; }
