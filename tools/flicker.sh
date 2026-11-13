#!/usr/bin/env bash
# Sub-pixel, in-motion and streaming stability, per seed, on the ground and high
# up. `rebuild` forces every chunk to rebuild and reports the worst frame-to-
# frame change: it must be near zero, because a rebuild is allowed to be slow
# and is not allowed to be visible.
#
#   tools/flicker.sh 1 12 40
#
# `cull` renders the same frame with the frustum test switched off and counts
# the pixels that differ: it must be exactly 0, because a chunk that is still on
# screen may not be thrown away. That is the "terrain disappears when it reaches
# the edge of the frame" report, and no frame-to-frame number can see it -- the
# hole holds still. It is a fixed-view spot check, not proof: the invariant it
# guards is "the cull box is the chunk's own box, not a guess".
#
# `static` is the accumulator converging: it must be ~0 or the temporal filter
# is rippling. `walk` is the average frame-to-frame change while moving 0.6
# units per frame -- that is the number the player calls flicker, and it is the
# reason the altitude case exists: the first TAA pass reset its history on any
# camera movement, so it did nothing exactly when it was needed, and that was
# only visible from height.
#
# The pixel counts are contrast-weighted, so they are a regression alarm rather
# than a perceptual score: a sharper image legitimately scores worse.
set -euo pipefail
cd "$(dirname "$0")/.."
./build.sh >/dev/null
fail=0
for s in "$@"; do
    for view in "ground:" "high:--cam -300,2500,600 --yaw 160 --pitch -35"; do
        name=${view%%:*}
        args=${view#*:}
        line=$(./build/game shot --seed "$s" --size 800x450 $args --flicker 2>/dev/null | grep '^flicker')
        echo "$name $line"
        static=$(echo "$line" | sed -n 's/.*static=\([0-9.]*\)%.*/\1/p')
        walk=$(echo "$line"   | sed -n 's/.* walk=\([0-9.]*\)%.*/\1/p')
        rebuild=$(echo "$line" | sed -n 's/.* rebuild=\([0-9.]*\)%.*/\1/p')
        cull=$(echo "$line"    | sed -n 's/.* cull=\([0-9.]*\)%.*/\1/p')
        awk -v a="$static" -v b="$walk" -v c="$rebuild" -v d="$cull" \
            'BEGIN{ if (a > 0.5 || b > 4.5 || c > 8.0 || d > 0.0) exit 1 }' || {
            echo "flicker: seed $s ($name) regressed (static=$static% walk=$walk% rebuild=$rebuild% cull=$cull%)"
            fail=1
        }
    done
done
[ $fail -eq 0 ] && echo "flicker: ok" || exit 1
