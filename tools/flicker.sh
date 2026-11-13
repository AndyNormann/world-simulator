#!/usr/bin/env bash
# Sub-pixel, in-motion and streaming stability, per seed, on the ground and high
# up. `rebuild` forces every chunk to rebuild and reports the worst frame-to-
# frame change: it must be near zero, because a rebuild is allowed to be slow
# and is not allowed to be visible.
#
#   tools/flicker.sh 1 12 40
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
        awk -v a="$static" -v b="$walk" -v c="$rebuild" \
            'BEGIN{ if (a > 0.5 || b > 4.5 || c > 8.0) exit 1 }' || {
            echo "flicker: seed $s ($name) regressed (static=$static% walk=$walk% rebuild=$rebuild%)"
            fail=1
        }
    done
done
[ $fail -eq 0 ] && echo "flicker: ok" || exit 1
