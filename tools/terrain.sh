#!/usr/bin/env bash
# Terrain quality table for a seed range: per archetype, the numbers that move
# when the generator changes. Regenerate the maps first if they are stale:
#   tools/worlds.sh 1 40 384 && tools/terrain.sh
set -euo pipefail
cd "$(dirname "$0")/.."

[ -f build/maps/stats.txt ] || { echo "terrain: run tools/worlds.sh first" >&2; exit 1; }

awk '
/^seed/ {
    name = $3
    if (!(name in n)) order[++m] = name
    n[name]++
    for (i = 4; i <= NF; i++) {
        split($i, kv, "=")
        gsub(/%/, "", kv[2])
        key = name SUBSEP kv[1]
        val = kv[2] + 0
        if (!(key in seen)) { seen[key] = 1; lo[key] = val; hi[key] = val }
        if (val < lo[key]) lo[key] = val
        if (val > hi[key]) hi[key] = val
    }
}
END {
    fmt = "%-12s %4s  %-15s %-13s %-12s %-12s\n"
    printf fmt, "archetype", "n", "relief", "land %", "town-masses", "viable"
    for (k = 1; k <= m; k++) {
        a = order[k]
        printf "%-12s %4d  ", a, n[a]
        printf "%-15s ", sprintf("%.0f..%.0f", lo[a, "relief"], hi[a, "relief"])
        printf "%-13s ", sprintf("%.1f..%.1f", lo[a, "land"], hi[a, "land"])
        printf "%-12s ", sprintf("%.0f..%.0f", lo[a, "towns"], hi[a, "towns"])
        printf "%-12s\n", sprintf("%.0f..%.0f", lo[a, "viable"], hi[a, "viable"])
    }
}' build/maps/stats.txt
