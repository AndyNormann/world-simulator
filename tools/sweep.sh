#!/usr/bin/env bash
# Run the generated-world invariants over a range of seeds. Each seed is a fresh
# process, so a crash, an internal assert, a NaN or an assert failure in
# replays/sweep.txt fails the run and names the seed.
#
#   tools/sweep.sh [first] [last]        default 1..40 (~25s)
set -euo pipefail
cd "$(dirname "$0")/.."
first="${1:-1}"; last="${2:-40}"
./build.sh >/dev/null
fails=0
for seed in $(seq "$first" "$last"); do
    if ! ./build/game dump replays/sweep.txt --seed "$seed" >/dev/null 2>/tmp/sweep.err; then
        echo "seed $seed FAILED: $(tail -1 /tmp/sweep.err)"
        fails=$((fails + 1))
    fi
done
rm -f /tmp/sweep.err
[ "$fails" -eq 0 ] || { echo "sweep: $fails of $((last - first + 1)) seeds failed"; exit 1; }
echo "sweep: seeds $first..$last ok"
