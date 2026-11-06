#!/bin/bash
# Builds every picture of the brand from the Facet Bands mark: three nested,
# rounded play triangles — a faint outer band, a middle one, a solid core — each
# split into the same three facets (cyan above, blue on the left, violet below).
#
#   bash tools/build-brand.sh
#
# Writes:
#   assets/icon-1024.png                 the app icon master (then tools/build-icons.sh
#                                        cuts the 80/130 webOS tiles and the Mac icns from it)
#   deploy/app/art/icons/brand_lockup.png  mark + wordmark, the splash and profile picker
#   deploy/app/art/icons/brand_mark.png    the mark alone, tight, for the side menu
#   deploy/app/art/icons/intro_bands.png   the three bands apart, one per cell, so the
#                                          opening can bring them in one by one
#   assets/android/                        the Android TV banner, 1280 master + drawable-*/banner.png
#
# The wordmark is assets/brand/wordmark.png, 705x129, the letters exactly as they
# were in the old lockup: splash.c's letter cuts and the "o" portal are measured
# off it, so it is placed at a whole pixel and never resampled.
#
# THE LOCKUP'S GEOMETRY IS splash.c's. Moving anything below means re-measuring
# LOCK_FILE_W/H, BAND_*, MARK_P*, letterCut, WORD_MID_Y and O_HOLE_* there.
set -e
cd "$(dirname "$0")/.."
command -v rsvg-convert >/dev/null || { echo "rsvg-convert not found (brew install librsvg)" >&2; exit 1; }

GROUND='#0d101e'
# The mark is drawn in its own units: viewBox 28 16 76 88, the bands concentric
# about (55, 60), where the three facets meet.
DEFS='<defs>
<clipPath id="outer"><path d="M30.00 28.00 L30.00 92.00 Q30.00 104.00 40.40 98.00 L95.60 66.00 Q106.00 60.00 95.60 54.00 L40.40 22.00 Q30.00 16.00 30.00 28.00 Z M34.50 33.76 Q34.50 23.92 43.03 28.84 L88.29 55.08 Q96.82 60.00 88.29 64.92 L43.03 91.16 Q34.50 96.08 34.50 86.24 Z" clip-rule="evenodd"/></clipPath>
<clipPath id="mid"><path d="M38.50 38.88 L38.50 81.12 Q38.50 89.04 45.36 85.08 L81.80 63.96 Q88.66 60.00 81.80 56.04 L45.36 34.92 Q38.50 30.96 38.50 38.88 Z M42.50 44.00 Q42.50 38.00 47.70 41.00 L75.30 57.00 Q80.50 60.00 75.30 63.00 L47.70 79.00 Q42.50 82.00 42.50 76.00 Z" clip-rule="evenodd"/></clipPath>
<clipPath id="core"><path d="M44.50 46.56 L44.50 73.44 Q44.50 78.48 48.87 75.96 L72.05 62.52 Q76.42 60.00 72.05 57.48 L48.87 44.04 Q44.50 41.52 44.50 46.56 Z"/></clipPath>
<linearGradient id="top" gradientUnits="userSpaceOnUse" x1="30" y1="16" x2="106" y2="60"><stop offset="0" stop-color="#6be6f7"/><stop offset="1" stop-color="#35b6ee"/></linearGradient>
<linearGradient id="low" gradientUnits="userSpaceOnUse" x1="30" y1="60" x2="106" y2="104"><stop offset="0" stop-color="#8c4cf0"/><stop offset="1" stop-color="#e85bd2"/></linearGradient>
</defs>'
FACETS='<polygon points="30,16 106,60 55,60" fill="url(#top)"/><polygon points="30,16 55,60 30,104" fill="#3a5fd8"/><polygon points="30,104 55,60 106,60" fill="url(#low)"/>'
band() { echo "<g clip-path=\"url(#$1)\" opacity=\"$2\">$FACETS</g>"; }
# The whole mark. Large sizes use 0.4/0.72; at menu and tile sizes the faint
# bands are lifted a little (0.45/0.75) so they do not vanish.
mark() { band outer "$1"; band mid "$2"; band core 1; }
# The mark's units placed at (x, y), `u` pixels to the unit.
at() { echo "translate($1 $2) scale($3) translate(-28 -16)"; }

svg() { # w h body -> out
  local f; f=$(mktemp "${TMPDIR:-/tmp}/nuvio-brand.XXXXXX")
  printf '<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="%s" height="%s" viewBox="0 0 %s %s">%s%s</svg>' \
    "$1" "$2" "$1" "$2" "$DEFS" "$3" > "$f.svg"
  rsvg-convert -w "$1" -h "$2" "$f.svg" -o "$4"
  rm -f "$f" "$f.svg"
}

# THE LOCKUP, 1149x378: the Facet Bands canvas's lockup at 2.304x (the wordmark
# there is 306x56 and here 705x129): the mark's 76x88 box at (16,16), 3.9271 px to
# the unit; the wordmark 113 px further right than the box's edge and 5 board px
# above its centre line.
U=3.927139
WORD=$(base64 < assets/brand/wordmark.png | tr -d '\n')
svg 1149 378 "<g transform=\"$(at 16 16 $U)\">$(mark 0.4 0.72)</g>
<image x=\"428\" y=\"113\" width=\"705\" height=\"129\" xlink:href=\"data:image/png;base64,$WORD\"/>" \
  deploy/app/art/icons/brand_lockup.png

# THE BANDS, 936x362: three 312x362 cells, core, middle, outer, each the lockup's
# region x 8..320, y 8..370 with only that band in it, at the opacity it has there.
B=""
i=0
for spec in "core 1" "mid 0.72" "outer 0.4"; do
  set -- $spec
  B="$B<g transform=\"translate($((i * 312)) 0) $(at 8 8 $U)\">$(band "$1" "$2")</g>"
  i=$((i + 1))
done
svg 936 362 "$B" deploy/app/art/icons/intro_bands.png

# THE MENU MARK, 96x108: the art's own bounds (x 30..100.8, y 20..100), drawn 54
# tall in the side menu, so twice that.
svg 96 108 "<g transform=\"scale(1.3333) translate(-29.5 -19.5)\">$(mark 0.45 0.75)</g>" \
  deploy/app/art/icons/brand_mark.png

# THE APP ICON, 1024. Not the canvas's small tile: at Dock and launcher size that
# mark (half the tile, outer band at 0.4) went dark on dark and could not be read.
# Here the art is three quarters of the tile tall, the outer bands lifted to
# 0.62/0.86, and the ground carries the splash's purple glow from below so the
# tile does not read as a black square.
#
# OPTICALLY CENTRED, NOT ON ITS BOUNDS. Centred on its box (x 172..852) the play
# triangle read as sitting LEFT in the Dock: a triangle's weight is a third of
# the way in from its flat side (centroid x 399, 113 px left of the middle).
# Moving the centroid all the way to the middle would push the tip off the
# tile, so it goes halfway: 48 px right (5 units), box 220..900.
U=9.6
svg 1024 1024 "<radialGradient id=\"glow\" gradientUnits=\"userSpaceOnUse\" cx=\"512\" cy=\"1100\" r=\"1100\" gradientTransform=\"translate(512 1100) scale(1 0.8) translate(-512 -1100)\"><stop offset=\"0\" stop-color=\"#4a2a6e\"/><stop offset=\"0.45\" stop-color=\"#1c1734\"/><stop offset=\"0.85\" stop-color=\"$GROUND\"/></radialGradient>
<rect width=\"1024\" height=\"1024\" fill=\"url(#glow)\"/>
<g transform=\"$(at 200.96 89.6 $U)\">$(mark 0.62 0.86)</g>" assets/icon-1024.png

# THE ANDROID TV BANNER (android:banner), the canvas's Facet Bands card: the
# lockup centred on the navy with the purple rising from below, 1280x720 at 2x the
# canvas (mark box 260x300, gap 98, wordmark 612x112 sitting 10 px above the
# middle). Android TV rounds the corners itself, so the banner is full bleed; the
# density buckets are cut from it (320x180 is the xhdpi size Android TV asks for).
mkdir -p assets/android
svg 1280 720 "<radialGradient id=\"glow\" gradientUnits=\"userSpaceOnUse\" cx=\"640\" cy=\"777.6\" r=\"1536\" gradientTransform=\"translate(640 777.6) scale(1 0.35156) translate(-640 -777.6)\"><stop offset=\"0\" stop-color=\"#36214f\"/><stop offset=\"0.35\" stop-color=\"#1a1530\"/><stop offset=\"0.7\" stop-color=\"$GROUND\"/></radialGradient>
<rect width=\"1280\" height=\"720\" fill=\"url(#glow)\"/>
<g transform=\"$(at 155.45 210 3.409091)\">$(mark 0.4 0.72)</g>
<image x=\"513\" y=\"294\" width=\"612\" height=\"112\" xlink:href=\"data:image/png;base64,$WORD\"/>" \
  assets/android/banner-1280.png
for d in "xhdpi 320 180" "xxhdpi 480 270" "xxxhdpi 640 360"; do
  set -- $d
  mkdir -p "assets/android/drawable-$1"
  sips -z "$3" "$2" assets/android/banner-1280.png --out "assets/android/drawable-$1/banner.png" >/dev/null
done

echo "==> brand_lockup.png, intro_bands.png, brand_mark.png, assets/icon-1024.png"
echo "    now: bash tools/build-icons.sh (the webOS tiles and the Mac icon)"
