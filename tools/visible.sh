#!/bin/sh
# How much of the frame does each artifact class actually cover?
#
#   tools/visible.sh              # seeds 1 and 7, three cameras
#   tools/visible.sh 1 12         # other seeds
#
# A counter says how many facets of a class exist. It does not say whether a
# player can see one, and the answer has been surprising both ways: the dark
# chord class covered 1.9% of a 960x540 frame (obvious), while others are below
# a hundred pixels on the same views (invisible, and no amount of work on them
# can show up on screen).
#
# The game paints one class magenta with V_CLASS = tiny|seam|dark|fall
# (src/world.c, vis_match) and this counts the pixels that survive shading,
# TAA and fog: r-g > 60 and b-g > 60, which nothing else in the palette is.
set -euo pipefail
cd "$(dirname "$0")/.."

seeds=${*:-1 7}
out=build/shots
mkdir -p "$out"

cams="shelf:-3028.6,235.0,-3592.7:74.66:-16.37:10.41
lake:-7149.1,511.6,-113.6:60.31:-35.48:9.77
high:-300,2500,600:160:-35:12.0
fall:-6267.2,661.6,-2415.1:-609.87:-7.59:13.44
tongue:-4747.5,55.4,310.9:-7.43:-0.36:11.91"

printf '%-6s %-6s %-6s %8s %8s\n' class seed view pixels pct
for cls in tiny seam dark fall; do
    for s in $seeds; do
        for c in $cams; do
            name=$(echo "$c" | cut -d: -f1)
            IFS=: read -r _ xyz yaw pitch hour <<EOF
$c
EOF
            png="$out/vis-$cls-$s-$name.png"
            V_CLASS=$cls ./build/game shot --seed "$s" --world alpine \
                --size 960x540 --ssaa 1 --cam "$xyz" --yaw "$yaw" --pitch "$pitch" \
                --time "$hour" -o "$png" >/dev/null 2>&1
            read -r px pct <<EOF
$(python3 - "$png" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB')
w, h = im.size
d = im.tobytes()
n = sum(1 for i in range(0, len(d), 3) if d[i] - d[i+1] > 60 and d[i+2] - d[i+1] > 60)
print(n, "%.3f" % (100.0 * n / (w * h)))
PY
)
EOF
            printf '%-6s %-6s %-6s %8s %8s\n' "$cls" "$s" "$name" "$px" "$pct"
        done
    done
done
