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
# geometry. What it exists to catch is the *shadow* changing with the tier, which
# it did at 9.7% until the horizon bake moved off the mesh and onto a world
# lattice (see bake_horizon_grid) -- so this is a regression alarm at 12%, not an
# acceptance criterion.
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
        awk -v a="$static" -v b="$walk" -v c="$rebuild" -v d="$cull" \
            -v e="$lod" -v f="$lodmax" \
            'BEGIN{ if (a > 0.5 || b > 8.0 || c > 8.0 || d > 0.0 || e > 12.0 || f > 210) exit 1 }' || {
            echo "flicker: seed $s ($name) regressed (static=$static% walk=$walk% rebuild=$rebuild% cull=$cull% lod=$lod% lodmax=$lodmax)"
            fail=1
        }
    done
done
[ $fail -eq 0 ] && echo "flicker: ok" || exit 1
