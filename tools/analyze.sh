#!/usr/bin/env bash
# Clang static analyzer over every game TU (~20s). Catches the class of bug the
# compiler cannot: a value used uninitialised, a division by a value that can be
# zero, a NULL handed to libc, a path that leaks.
#
#   tools/analyze.sh          report to stdout, exit 0 even with findings
#   tools/analyze.sh --strict exit 1 on any finding (for check --slow)
set -euo pipefail
cd "$(dirname "$0")/.."
total=0
for src in src/main.c src/world.c src/render.c src/replay.c src/state.c; do
    out=$(mktemp)
    clang --analyze -std=c99 -O1 -I libs -I libs/sokol -I libs/stb -I src \
          -fno-objc-arc -x objective-c "$src" 2>&1 > /dev/null | tee "$out" || true
    n=$(grep -c "warning:" "$out" || true)
    [ "$n" -gt 0 ] && echo "  $src: $n"
    total=$((total + n))
    rm -f "$out"
done
echo "analyze: $total finding(s)"
[ "${1:-}" = "--strict" ] && [ "$total" -gt 0 ] && exit 1
exit 0
