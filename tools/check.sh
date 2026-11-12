#!/usr/bin/env bash
# Definition of done. One exit code: build, unit tests, the sim-only smoke
# replay, the per-area replays, and the three golden images.
#
#   tools/check.sh          everything (~7s)
#   tools/check.sh --quick  build + unit tests + smoke replay (~3s)
#   tools/check.sh --slow   + static analyzer + 40-seed sweep (~40s)
set -euo pipefail
cd "$(dirname "$0")/.."

quick=0 slow=0
[ "${1:-}" = "--quick" ] && quick=1
[ "${1:-}" = "--slow" ] && slow=1

./build.sh                                          # -Werror: warnings are failures
./build.sh test | tail -2                           # math_test, terrain_test
./build/game dump replays/smoke.txt | tail -1        # sim + asserts, no GPU

if [ $slow -eq 1 ]; then
    tools/analyze.sh --strict
    tools/sweep.sh 1 40
fi

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
