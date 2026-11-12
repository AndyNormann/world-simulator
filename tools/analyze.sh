#!/usr/bin/env bash
# Clang static analyzer over the game TU (~6s). Catches the class of bug the
# compiler cannot: paths where a value is used uninitialised, a division by a
# value that can be zero, a NULL handed to libc.
#
#   tools/analyze.sh          report to stdout, exit 0 even with findings
#   tools/analyze.sh --strict exit 1 on any finding (for check --slow)
set -euo pipefail
cd "$(dirname "$0")/.."
out=$(mktemp)
clang --analyze -std=c99 -O1 -I libs -I libs/sokol -I libs/stb -I src \
      -fno-objc-arc -x objective-c src/main.c 2>&1 > /dev/null | tee "$out"
n=$(grep -c "warning:" "$out" || true)
rm -f "$out"
echo "analyze: $n finding(s)"
[ "${1:-}" = "--strict" ] && [ "$n" -gt 0 ] && exit 1
exit 0
