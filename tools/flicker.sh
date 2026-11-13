#!/usr/bin/env bash
# Sub-pixel stability, per seed. A renderer that shimmers reports a large number
# here and a stable one reports a small one; the thresholds are what "no visible
# flicker while walking" is asserted as.
#
#   tools/flicker.sh 1 12 40
set -euo pipefail
cd "$(dirname "$0")/.."
./build.sh >/dev/null
fail=0
for s in "$@"; do
    line=$(./build/game shot --seed "$s" --size 800x450 --flicker 2>/dev/null | grep '^flicker')
    echo "$line"
    # A regression alarm, not a perceptual score: the pixel count is weighted by
    # contrast, so a sharper image legitimately scores worse. What it catches is
    # the renderer going wrong -- a blend weight stuck at zero, a broken
    # reprojection, unstable streaming -- not small quality changes.
    static=$(echo "$line" | sed -n 's/.*static=\([0-9.]*\)%.*/\1/p')
    yaw=$(echo "$line" | sed -n 's/.* yaw=\([0-9.]*\)%.*/\1/p')
    walk=$(echo "$line" | sed -n 's/.* walk=\([0-9.]*\)%.*/\1/p')
    awk -v a="$static" -v b="$yaw" -v c="$walk" 'BEGIN{ if (a > 0.5 || b > 20.0 || c > 6.0) exit 1 }' || {
        echo "flicker: seed $s regressed (static=$static% yaw=$yaw% walk=$walk%)"
        fail=1
    }
done
[ $fail -eq 0 ] && echo "flicker: ok" || exit 1
