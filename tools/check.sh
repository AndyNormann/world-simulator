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
    # rendering: sub-pixel stability is the flicker the player reports, and it
    # regresses silently -- a wrong blend weight looks fine in a still
    tools/flicker.sh 1 12 40
    # the sim is data: it is reviewed by reading it, so determinism is the only
    # thing worth asserting mechanically -- a second run of the same seed must
    # produce the same world, or "come back to a changed place" is not buildable
    for s in 1 7 12; do
        ./build/game history --seed "$s" --years 500 --check | tail -1
    done
fi

if [ $quick -eq 0 ]; then
    ./build/game replay replays/water.txt    --seed 7 --size 960x540 >/dev/null
    ./build/game replay replays/trees.txt    --seed 7 --size 960x540 --trees-only >/dev/null
    ./build/game replay replays/streaming.txt --seed 7 --size 960x540 >/dev/null
    ./build/game replay replays/golden.txt   --seed 7 --size 1280x720 >/dev/null
    # the reported artifact cameras, for their structural counters: long facets,
    # slivers, unused points, a seabed colour on dry ground, a seam chord off the
    # ground. P1..P4 of PLAN.md drive these down; today they are the "before"
    ./build/game replay replays/artifacts.txt --seed 1 --world alpine --size 960x540 >/dev/null
    # a world built in place must equal a world built by a fresh process: this
    # is the only test that can see a static that world_rebuild forgot, and the
    # overlay's rebuild button goes through the same request path
    ./build/game shot --seed 7 --size 320x180 --ssaa 1 -o build/shots/rb-fresh.png >/dev/null
    ./build/game shot --seed 7 --rebuild 7 --size 320x180 --ssaa 1 -o build/shots/rb-inplace.png >/dev/null
    tools/diff.sh build/shots/rb-fresh.png build/shots/rb-inplace.png --tol 0.0
    # M2: the town is geometry with walls in it, so it is walked, not just drawn
    ./build/game dump replays/town.txt --seed 12 --settlement 3 >/dev/null
    # goldens are per platform (see tools/goldens.sh): Metal and GL do not
    # produce the same bytes, and a shared set would have to be diffed at a
    # tolerance that catches nothing
case "$(uname -s)" in
    Darwin) PLATFORM=macos ;;
    Linux)  PLATFORM=linux ;;
    *)      PLATFORM=$(uname -s | tr 'A-Z' 'a-z') ;;
esac
PLATFORM=${GOLDEN_PLATFORM:-$PLATFORM}
    if [ ! -d "tests/golden/$PLATFORM" ]; then
        echo "check: no goldens for $PLATFORM -- run tools/goldens.sh first" >&2
        exit 1
    fi
    for n in dusk high low; do
        tools/diff.sh "tests/golden/$PLATFORM/$n.png" "build/shots/golden-$n.png" --tol 0.3
    done
fi

echo "check: ok"
