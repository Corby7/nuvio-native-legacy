#!/bin/bash
# Rasterises the SEARCH screen's glyphs from the web app.
#
#   bash tools/build-search-icons.sh
#
# WHY IT EXTRACTS RATHER THAN COPIES. Two of the six live as .svg FILES in the
# web checkout and four do not: the magnifier, the clear cross, the history
# clock and the microphone are Phosphor paths written INLINE in the markup of
# js/ui/screens/search/searchScreen.js. Re-typing those four here would create
# a second copy that nobody would ever diff against the first — which is the
# exact failure tools/build-player-icons.sh was written to end, where four of
# seven transport glyphs had quietly become stand-ins from another set while a
# comment still claimed they came from the web app.
#
# So the four are pulled out of the markup by the class of the element that
# carries them. If the web app restyles one, the next run picks up the change;
# if it removes one, this script fails loudly instead of shipping a stale PNG.
#
# THE ALPHA IS THE GLYPH, as everywhere else in art/icons: gfx_icon draws with
# GFX_BRAND, which takes the shape from the texture's alpha and the colour from
# the caller. That is what lets one file serve the grey magnifier at rest, the
# brighter one on focus, and the dark arrow on the focused white "See All"
# circle. rsvg-convert with no --background gives exactly that. Do not "fix"
# these by flattening them onto a colour.
#
# 128px, the raster size the rest of art/icons uses. The screen draws them at
# 28..48, so there is headroom for a 4K panel without a second set.
set -e
cd "$(dirname "$0")/.."

command -v rsvg-convert >/dev/null || { echo "rsvg-convert not found (brew install librsvg)" >&2; exit 1; }

# The sibling web checkout has been called both NuvioWeb-0.3.38-beta and
# NuvioWeb — the same two names tools/arm.sh and tools/build-player-icons.sh
# try, and for the same reason.
WEB=""
for c in ../NuvioWeb-0.3.38-beta ../NuvioWeb; do
  [ -d "$c/assets/icons" ] && WEB="$c"
done
[ -n "$WEB" ] || { echo "web checkout not found at ../NuvioWeb*" >&2; exit 1; }

SCREEN="$WEB/js/ui/screens/search/searchScreen.js"
[ -f "$SCREEN" ] || { echo "MISSING: $SCREEN" >&2; exit 1; }

OUT=deploy/app/art/icons
SIZE="${NUVIO_ICON_PX:-128}"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# local name <- the class whose element carries the inline <svg>.
#
# search_clock comes off .search-history-chip, the "Recent searches" pill. It is
# Phosphor's ClockCounterClockwise and it is the only one of the four that is
# repeated per chip — the first match is the one taken, and they are identical.
#
# search_discover is Phosphor's Compass, off .search-discover-btn. There is no
# search_voice: the web app's microphone drives window.SpeechRecognition, and
# this TV refuses that to every client but LG's own assistant — the measurement
# and the refusal text are recorded at the top of search.c.
INLINE="
search_glass:search-input-icon
search_clear:search-clear-btn
search_clock:search-history-chip
search_discover:search-discover-btn
"

for pair in $INLINE; do
  name="${pair%%:*}"; cls="${pair##*:}"
  # Everything from the class name to the first </svg> after it, then back to
  # the <svg that opens it. Node does the work rather than sed: the markup is a
  # template literal spread over several lines and with ${...} holes in it, and
  # a line-oriented tool gets that wrong in a way that only shows up as a
  # missing glyph on the TV.
  node -e '
    const fs = require("fs");
    const src = fs.readFileSync(process.argv[1], "utf8");
    const at = src.indexOf(process.argv[2]);
    if (at < 0) { console.error("class not found: " + process.argv[2]); process.exit(1); }
    const close = src.indexOf("</svg>", at);
    if (close < 0) { console.error("no </svg> after " + process.argv[2]); process.exit(1); }
    const open = src.lastIndexOf("<svg", close);
    if (open < at - 400 || open > close) { console.error("no <svg> near " + process.argv[2]); process.exit(1); }
    let svg = src.slice(open, close + 6);
    // The markup sets width/height in CSS pixels for the browser. Strip them so
    // rsvg scales the viewBox to the raster size asked for here instead of
    // drawing a 24px glyph in the corner of a 128px canvas.
    svg = svg.replace(/\s(width|height)="[^"]*"/g, "");
    // currentColor has no meaning outside a document. White keeps the shape in
    // the alpha, which is all gfx_icon reads.
    svg = svg.replace(/currentColor/g, "#ffffff");
    process.stdout.write(svg + "\n");
  ' "$SCREEN" "$cls" > "$TMP/$name.svg"
  rsvg-convert -w "$SIZE" -h "$SIZE" "$TMP/$name.svg" -o "$OUT/$name.png"
  echo "    $name.png <- inline <svg> on .$cls"
done

# The two that really are files. The focused variant is a SEPARATE file and not
# the same shape recoloured: the web app swaps the mask image outright
# (.search-seeall-card.focused .search-seeall-arrow), outline to filled.
FILES="
search_seeall:ic_search_see_all
search_seeall_fill:ic_home_see_all_filled
"

for pair in $FILES; do
  name="${pair%%:*}"; svg="${pair##*:}"
  [ -f "$WEB/assets/icons/$svg.svg" ] || { echo "    MISSING: $WEB/assets/icons/$svg.svg" >&2; exit 1; }
  rsvg-convert -w "$SIZE" -h "$SIZE" "$WEB/assets/icons/$svg.svg" -o "$OUT/$name.png"
  echo "    $name.png <- $svg.svg"
done

echo "==> $OUT, ${SIZE}px, from $WEB"
echo "    the TV only sees these after a new .ipk — bash tools/arm.sh --ipk"
