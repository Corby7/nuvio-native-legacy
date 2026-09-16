#!/bin/bash
# Builds the app icon: the badged 1024px master, plus the two sizes webOS asks
# for in appinfo.json (80px `icon`, 130px `largeIcon`).
#
#   bash tools/build-icons.sh            # default badge, "ribbon"
#   NUVIO_ICON_BADGE=chip  bash tools/build-icons.sh
#   NUVIO_ICON_BADGE=tint  bash tools/build-icons.sh
#   NUVIO_ICON_BADGE=plain bash tools/build-icons.sh
#
# WHY A BADGE AT ALL: this app and the web build (space.nuvio.webos, "Nuvio TV")
# install side by side on the same TV and share one brand mark. On the C3 home
# row the title under the tile is small and truncated — "Nuvio Legacy Nati…"
# against "Nuvio TV" — so the picture is what you actually pick from the couch.
# The badge is the only thing that tells them apart at that distance.
#
# The 80px tile is too small for the word to be read: at that size the amber
# sash alone carries the meaning, and "DEV" only becomes legible on the 130px
# tile and in the Mac Dock. That is deliberate, not an oversight.
#
# The source of truth is assets/icon-1024.png, the SAME master the Electron
# desktop build uses (NuvioWeb/build/icon.png). It is never edited here; every
# output is derived, so the three packages cannot drift apart.
set -e
cd "$(dirname "$0")/.."

command -v rsvg-convert >/dev/null || { echo "rsvg-convert not found (brew install librsvg)" >&2; exit 1; }

SRC=assets/icon-1024.png
OUT=assets/icon-1024-legacy.png
BADGE="${NUVIO_ICON_BADGE:-ribbon}"
B64=$(base64 -i "$SRC")

case "$BADGE" in
  # Amber sash across the bottom-left corner. Loudest of the three, and the
  # only one still readable as "not the normal app" at 80px.
  ribbon) OVERLAY='<polygon points="0,464 560,1024 380,1024 0,644" fill="#f0a03c"/>
<text x="235" y="800" transform="rotate(45 235 800)" font-family="Helvetica,Arial,sans-serif" font-weight="bold" font-size="112" letter-spacing="6" text-anchor="middle" dominant-baseline="central" fill="#141416">DEV</text>' ;;
  # Corner chip. Quieter, keeps the mark whole; the N is legible from 130px up.
  chip)   OVERLAY='<circle cx="830" cy="830" r="150" fill="#f0a03c" stroke="#141416" stroke-width="22"/>
<text x="830" y="890" font-family="Helvetica,Arial,sans-serif" font-weight="bold" font-size="190" text-anchor="middle" fill="#141416">N</text>' ;;
  tint)   OVERLAY='' ;;
  plain)  OVERLAY='' ;;
  *) echo "unknown NUVIO_ICON_BADGE: $BADGE (ribbon|chip|tint|plain)" >&2; exit 1 ;;
esac

# The tint variant is a different colourway rather than an added shape: the mark
# is hue-rotated and the result masked back over the original, so the near-black
# ground stays exactly the brand ground. Rotating the whole image instead turns
# the background brown — the mask is what keeps it clean. The mask is a
# hard-contrast luminance key of the master: the mark is bright, the ground is
# not, and the steep feFuncA turns that gap into a clean 0/1 edge.
TINT=''
if [ "$BADGE" = tint ]; then
  TINT='<filter id="key" color-interpolation-filters="sRGB"><feColorMatrix type="luminanceToAlpha"/><feComponentTransfer><feFuncA type="linear" slope="9" intercept="-0.45"/></feComponentTransfer><feColorMatrix type="matrix" values="0 0 0 0 1  0 0 0 0 1  0 0 0 0 1  0 0 0 1 0"/></filter>
<filter id="hue" color-interpolation-filters="sRGB"><feColorMatrix type="hueRotate" values="300"/></filter>
<mask id="mark"><image x="0" y="0" width="1024" height="1024" filter="url(#key)" xlink:href="data:image/png;base64,'"$B64"'"/></mask>'
  OVERLAY='<image x="0" y="0" width="1024" height="1024" filter="url(#hue)" mask="url(#mark)" xlink:href="data:image/png;base64,'"$B64"'"/>'
fi

SVG=$(mktemp -t nuvio-icon).svg
trap 'rm -f "$SVG"' EXIT
cat > "$SVG" <<EOF
<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="1024" height="1024" viewBox="0 0 1024 1024">
$TINT
<image x="0" y="0" width="1024" height="1024" xlink:href="data:image/png;base64,$B64"/>
$OVERLAY
</svg>
EOF

rsvg-convert -w 1024 -h 1024 "$SVG" -o "$OUT"
# sips downsamples with a decent filter; the 80px tile is where the sash has to
# survive, so check that one by eye after changing anything above.
sips -z 80 80 "$OUT" --out deploy/app/icon.png >/dev/null
sips -z 130 130 "$OUT" --out deploy/app/largeIcon.png >/dev/null
echo "==> $BADGE: $OUT, deploy/app/icon.png (80), deploy/app/largeIcon.png (130)"
echo "    the TV only sees these after a new .ipk — bash tools/arm.sh --ipk"
