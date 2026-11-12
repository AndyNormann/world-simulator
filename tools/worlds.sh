#!/usr/bin/env bash
# Generate a batch of worlds: one top-down map + one stats line each, then a
# contact sheet to eyeball them all at once.
#
#   tools/worlds.sh [first] [last] [size]     default 1 40 384
set -euo pipefail
cd "$(dirname "$0")/.."
first=${1:-1}; last=${2:-40}; size=${3:-384}
./build.sh >/dev/null
mkdir -p build/maps
: > build/maps/stats.txt
for s in $(seq "$first" "$last"); do
    ./build/game worldmap --seed "$s" --size "${size}x${size}" \
        -o "build/maps/world-$s.png" | sed -n 's/^world /seed /p' >> build/maps/stats.txt
done
column -t build/maps/stats.txt 2>/dev/null || cat build/maps/stats.txt
python3 tools/mapsheet.py build/maps/sheet.png build/maps/world-*.png
