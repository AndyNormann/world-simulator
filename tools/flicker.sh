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
# than a perceptual score: a sharper image legitimately scores worse. They are
# lod/lodmax are the tier swap: the same view rendered with every chunk at its
# finest mesh, rebuilt first (setting the clamp does nothing until the streamer
# re-queues). It reads 0.8% ground / 9.0% high today, and *by design*: the facet
# shade is ±18% per facet and a coarser mesh picks different facets, and the
# ground itself moves by the chord error. Measured: 9.0% -> 6.4% with the facet
# shade forced off, so two thirds of it is the low-poly look and the rest is LOD
# geometry. The stitch-ring experiment (P4: every chunk's border at step 1) costs
# 68% more points and 68% more builder time and moves this from 7.4% to 7.1%, so
# the seam is in the interior facet pattern and not in the border chain: a step-4
# chunk is a *different triangulation*, not a coarse copy of the fine one. What it exists to catch is the *shadow* changing with the tier, which
# it did at 9.7% until the horizon bake moved off the mesh and onto a world
# lattice (see bake_horizon_grid) -- so this is a regression alarm at 12%, not an
# acceptance criterion.
#
# `sprint` is 1.5 s at shift speed (330 units, far enough to cross a LOD band
# boundary) and `smax` the worst single frame of it: the only column here that
# can see a tier swap from the player's seat. Measured with NOLOD=1, which
# removes tier changes entirely, seed 1 reads 18.97% mean / 24.86% worst either
# way -- a swap is invisible in motion because both boundaries are 4000 and 8000
# units away, where a 24-cell chunk is a handful of pixels. What it does catch is
# streaming failing loudly (a hole, a flash): seed 40's auto camera hits a coast
# and reads smax 42%, so this is an alarm at 55%, not a quality score.
#
# `pop` is the same 0.6-unit step as `walk` but with the temporal history
# cleared before every frame: every other column measures a *converged*
# accumulator, which is what the player sees but also what averages a one-frame
# change over ten. This is the un-smoothed figure, and it is the one an "it
# popped" report is about. `fly` is the flying analogue of `walk` -- a 8 degree
# pan, a 5 degree look-up and a 29-unit climb, 26 frames each at ~2 px/frame --
# because a number only ever measured while walking cannot say whether flying is
# steady; a chunk arriving mid-flight adds its own area to it. Both are
# regression alarms at 20% / 45%, not quality scores: a pan moves every pixel.
#
# THE GATES ARE TODAY'S NUMBERS PLUS HEADROOM, not the plan's targets: they fail
# on a *change*, and a number that is a settled look difference (lod) is not a
# target at all. Worst of six views (seeds 1, 12, 40; ground and high) on the
# machine that wrote this: static 0.00, walk 3.8, wmax 4.4, rebuild 0.03, cull
# 0.00, lod 7.8, lodmax 223, sprint 16.6, smax 42.1, pop 3.7, pmax 3.8, fly 31.3,
# p99 11.5 ms, max 15.8 ms.
#
# frame_ms is the cost side, which no pixel diff can see: p50/p99/max of every
# rendered frame during the probe, including the streaming bursts. A renderer
# that is stable and correct but stalls for 200 ms has failed the player just as
# much, and "it stutters" is a report with no image to compare. Gated as an
# alarm at p99 40 ms / max 120 ms on the same machine that reads ~7/11 today.
#
# also *camera* dependent, and the ground camera is auto-framed -- it moves when
# the terrain or the tree placement does. Seed 1's ground view went from a
# meadow (walk 3.8%) to a shoreline (6.9%) when trees stopped standing in water,
# with no rendering change at all. So the walk gate is a gross-regression alarm
# at 8%: the failures it exists for were 6-12% before temporal AA.
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
        lod=$(echo "$line"     | sed -n 's/.* lod=\([0-9.]*\)%.*/\1/p')
        lodmax=$(echo "$line"  | sed -n 's/.* lodmax=\([0-9]*\).*/\1/p')
        pop=$(echo "$line"     | sed -n 's/.* pop=\([0-9.]*\)%.*/\1/p')
        pmax=$(echo "$line"    | sed -n 's/.* pmax=\([0-9.]*\)%.*/\1/p')
        fly=$(echo "$line"     | sed -n 's/.* fly=\([0-9.]*\)%.*/\1/p')
        sprint=$(echo "$line"  | sed -n 's/.* sprint=\([0-9.]*\)%.*/\1/p')
        smax=$(echo "$line"    | sed -n 's/.* smax=\([0-9.]*\)%.*/\1/p')
        p99=$(echo "$line"     | sed -n 's/.* p99=\([0-9.]*\).*/\1/p')
        fmax=$(echo "$line"    | sed -n 's/.* max=\([0-9.]*\).*/\1/p')
        awk -v a="$static" -v b="$walk" -v c="$rebuild" -v d="$cull" \
            -v e="$lod" -v f="$lodmax" -v g="$p99" -v h="$fmax" -v i="$smax" \
            -v j="$pop" -v k="$pmax" -v l="$fly" \
            'BEGIN{ if (a > 0.5 || b > 6.0 || c > 1.0 || d > 0.0 || e > 12.0 || f > 300 || g > 25.0 || h > 60.0 || i > 55.0 || j > 6.0 || k > 7.0 || l > 45.0) exit 1 }' || {
            echo "flicker: seed $s ($name) regressed (static=$static% walk=$walk% rebuild=$rebuild% cull=$cull% lod=$lod% lodmax=$lodmax sprint=$sprint% smax=$smax% pop=$pop% pmax=$pmax% fly=$fly% p99=${p99}ms max=${fmax}ms)"
            fail=1
        }
    done
done
[ $fail -eq 0 ] && echo "flicker: ok" || exit 1
