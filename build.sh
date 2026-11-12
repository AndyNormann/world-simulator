#!/usr/bin/env bash
# macOS/Metal only. The GL, D3D11 and X11 paths are gone; the shaders are
# sokol-shdc generated (see tools/shaders.sh) and the checked-in src/shaders.h
# carries the MSL.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build/shots

CFLAGS_BASE="-std=c99 -g -Wall -Wextra -Werror -Wno-deprecated-declarations -I libs -I libs/sokol -I libs/stb -I src"
# -fno-objc-arc: sokol_app.h and the Metal readback in main.c are written for
# manual retain/release. Pinning it here means a future clang default change
# cannot silently break the build.
OBJCFLAGS="-fno-objc-arc"
SAN="-fsanitize=address,undefined"
FRAMEWORKS="-framework Cocoa -framework QuartzCore -framework Metal -framework AudioToolbox"

# ponytail: the game builds optimized by default -- at -O0 + ASan the terrain
# build blocks the main thread for seconds, which reads as a hung window.
# Sanitizers stay on for `test` and for the explicit `debug` target.
MODE="${1:-game}"
case "$MODE" in
    test|debug) CFLAGS="$CFLAGS_BASE -O0 $SAN"; LDFLAGS="$SAN" ;;
    *)          CFLAGS="$CFLAGS_BASE -O2";       LDFLAGS=""     ;;
esac

if [ "$MODE" = "test" ]; then
    cc $CFLAGS tests/math_test.c -o build/math_test -lm $LDFLAGS
    ./build/math_test
    cc $CFLAGS tests/terrain_test.c -o build/terrain_test -lm $LDFLAGS
    ./build/terrain_test
    exit 0
fi

clang -x objective-c $CFLAGS $OBJCFLAGS src/main.c -o build/game \
    $FRAMEWORKS -lm -lpthread $LDFLAGS
