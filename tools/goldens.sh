#!/usr/bin/env bash
# Re-record tests/golden/. Run this only when you *intend* the pixels to change,
# and say why in the commit message.
set -euo pipefail
cd "$(dirname "$0")/.."
./build.sh >/dev/null
./build/game replay replays/golden.txt --seed 7 --size 1280x720 >/dev/null
mkdir -p tests/golden
for n in dusk high low; do
    cp "build/shots/golden-$n.png" "tests/golden/$n.png"
done
echo "goldens recorded: $(ls tests/golden/*.png | tr '\n' ' ')"
