#!/usr/bin/env bash
# Definition of done. One exit code: build, unit tests, the sim-only smoke
# replay, the per-area replays, and the three golden images.
#
#   tools/check.sh          everything (~6s)
#   tools/check.sh --quick  build + unit tests + smoke replay (~3s)
set -euo pipefail
cd "$(dirname "$0")/.."

quick=0
[ "${1:-}" = "--quick" ] && quick=1

./build.sh                                          # -Werror: warnings are failures
./build.sh test | tail -2                           # math_test, terrain_test
./build/game dump replays/smoke.txt | tail -1        # sim + asserts, no GPU

if [ $quick -eq 0 ]; then
    ./build/game replay replays/water.txt    --seed 7 --size 960x540 >/dev/null
    ./build/game replay replays/trees.txt    --seed 7 --size 960x540 --trees-only >/dev/null
    ./build/game replay replays/streaming.txt --seed 7 --size 960x540 >/dev/null
    ./build/game replay replays/golden.txt   --seed 7 --size 1280x720 >/dev/null
    for n in dusk high low; do
        tools/diff.sh "tests/golden/$n.png" "build/shots/golden-$n.png" --tol 0.3
    done
fi

echo "check: ok"
