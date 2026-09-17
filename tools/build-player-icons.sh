#!/bin/bash
# Rasterises the PLAYER's transport glyphs from the web app's SVGs.
#
#   bash tools/build-player-icons.sh
#
# WHY THIS EXISTS. player.c has always claimed its glyphs were "the web app's
# .svg rasterised at 128px". Four of the seven were not: subtitles was a CC
# badge where the web app has a caption box, audio was a pair of circles where
# the web app has a speaker, episodes was stacked squares where the web app has
# a film strip, and sources was a cloud where the web app has a disc stack. They
# were hand-picked stand-ins from some other set, and nothing in the repository
# could tell you that — the PNGs are binary, the claim was a comment, and the
# two only disagree if you put them side by side.
#
# So the mapping lives here now, in the open, and re-running this is how you
# find out whether the app and the web app have drifted again.
#
# THE ALPHA IS THE GLYPH. gfx_icon draws with GFX_BRAND, which takes the shape
# from the texture's ALPHA and the colour from the caller (gfx.h:58) — that is
# what lets one file serve the white glyph on video and the black one on the
# focus puck. rsvg-convert with no --background gives exactly that: transparent
# ground, opaque mark. Do not "fix" these by flattening them onto a colour.
#
# 128px is the raster size the rest of art/icons uses. The transport draws them
# at 48 and the next-episode card at 40, so there is headroom for a 4K panel
# without a second set.
set -e
cd "$(dirname "$0")/.."

command -v rsvg-convert >/dev/null || { echo "rsvg-convert not found (brew install librsvg)" >&2; exit 1; }

# The sibling web checkout has been called both NuvioWeb-0.3.38-beta and
# NuvioWeb — the same two names tools/arm.sh tries, and for the same reason.
SRC=""
for c in ../NuvioWeb-0.3.38-beta ../NuvioWeb; do
  [ -d "$c/assets/icons" ] && SRC="$c/assets/icons"
done
[ -n "$SRC" ] || { echo "web icons not found in ../NuvioWeb*/assets/icons" >&2; exit 1; }

OUT=deploy/app/art/icons
SIZE="${NUVIO_ICON_PX:-128}"

# local name <- the web app's file, from getControlDefinitions() in
# js/ui/screens/player/playerScreen.js.
#
# audio takes the OUTLINE variant: the web app swaps to ic_player_audio_filled
# once a track has been picked BY HAND, and this app has no equivalent of that
# state to read (tracks.h exposes no "explicitly selected"). Outline is the
# state a fresh playback is in, so it is the honest one of the two.
MAP="
play:ic_player_play
pause:ic_player_pause
subtitles:ic_player_subtitles
audio:ic_player_audio_outline
episodes:ic_player_episodes
sources:ic_player_source
aspect:ic_player_aspect_ratio
stats:ic_player_stats
"

for pair in $MAP; do
  name="${pair%%:*}"; svg="${pair##*:}"
  [ -f "$SRC/$svg.svg" ] || { echo "    MISSING: $SRC/$svg.svg" >&2; exit 1; }
  rsvg-convert -w "$SIZE" -h "$SIZE" "$SRC/$svg.svg" -o "$OUT/$name.png"
  echo "    $name.png <- $svg.svg"
done

echo "==> $OUT, ${SIZE}px, from $SRC"
echo "    the TV only sees these after a new .ipk — bash tools/arm.sh --ipk"
