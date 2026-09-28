#!/bin/bash
# Rasterises the SOURCES sheet's own glyphs.
#
#   bash tools/build-source-icons.sh
#
# The "Instant" bolt, and the header's Reload and Close. Also the subtitle Style
# bar's Reset, which is Reload mirrored. And the player's Sources button, the
# stack, and the title screen's, detail_stack{,_filled}. The bolt cannot be a
# character: the sheet draws in Inter, Inter has no U+26A1, and SDL_ttf renders .notdef without
# complaining — which is the hollow box the addons' own emoji made whenever one
# reached the screen intact. See the note in src/stream_parse.c.
#
# Unlike tools/build-player-icons.sh this one reads from THIS repository:
# assets/icons/ holds the source, because the web app carries the same glyph
# inline in a JavaScript string and has no file to point at.
#
# The ctx_* set is the home hold menu's row glyphs (src/ctxmenu.c).
#
# The set_* set is the Settings screen's group-header glyphs (src/settings.c).
#
# The src_* set is Live TV's Source screen (src/iptvui.c): Refresh, and the
# result line's tick, ! and cross. live_caret_{up,down} are Phosphor's carets (Bold),
# the full-screen channel carousel's chevrons.
#
# 128px is the raster size the rest of art/icons uses; the row draws it at 21.
set -e
cd "$(dirname "$0")/.."

command -v rsvg-convert >/dev/null || { echo "rsvg-convert not found (brew install librsvg)" >&2; exit 1; }

OUT=deploy/app/art/icons
SIZE="${NUVIO_ICON_PX:-128}"

for name in instant reload reset close ep_watched stack detail_stack detail_stack_filled \
            ctx_info ctx_plus ctx_minus ctx_check ctx_x ctx_grid \
            ctx_play ctx_restart ctx_hide \
            set_playback set_home set_appearance set_account set_live \
            src_refresh src_ok src_warn src_fail live_caret_up live_caret_down; do
  rsvg-convert -w "$SIZE" -h "$SIZE" "assets/icons/$name.svg" -o "$OUT/$name.png"
  echo "    $name.png <- assets/icons/$name.svg"
done

echo "==> $OUT, ${SIZE}px"
echo "    the TV only sees these after a new .ipk — bash tools/arm.sh --ipk"
