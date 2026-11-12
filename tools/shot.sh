#!/usr/bin/env bash
# Render one frame without stealing focus, then downscale it.
set -euo pipefail

out="build/shot.png"
rm -f "$out" "$out.txt" "$out.small.png" "$out.small.jpg"
mkdir -p build

open -g build/game.app --args --shot --out "$PWD/$out" "$@"

for _ in $(seq 1 60); do
    [ -f "$out.txt" ] && break
    sleep 0.5
done
[ -f "$out.txt" ] || { echo "shot.sh: no capture after 30s" >&2; exit 1; }
cat "$out.txt"
python3 tools/shrink.py "$out" 900
