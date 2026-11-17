#!/usr/bin/env bash
# Re-record this platform's goldens. Run this only when you *intend* the pixels
# to change, and say why in the commit message.
#
# There is one golden set per platform: Metal and GL cannot produce the same
# bytes (different rasterization at edges, different TAA history, different float
# rounding), so a single set would have to be diffed at a tolerance loose enough
# to catch nothing. `GOLDEN_PLATFORM=linux` overrides the default (uname).
set -euo pipefail
cd "$(dirname "$0")/.."
./build.sh >/dev/null
./build/game replay replays/golden.txt --seed 7 --size 1280x720 >/dev/null
case "$(uname -s)" in
    Darwin) PLATFORM=macos ;;
    Linux)  PLATFORM=linux ;;
    *)      PLATFORM=$(uname -s | tr 'A-Z' 'a-z') ;;
esac
PLATFORM=${GOLDEN_PLATFORM:-$PLATFORM}
mkdir -p "tests/golden/$PLATFORM"
for n in dusk high low; do
    cp "build/shots/golden-$n.png" "tests/golden/$PLATFORM/$n.png"
done
echo "goldens recorded ($PLATFORM): $(ls tests/golden/$PLATFORM/*.png | tr '\n' ' ')"
