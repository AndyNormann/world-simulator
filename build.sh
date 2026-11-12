#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build

CFLAGS_BASE="-std=c99 -g -Wall -Wextra -Werror -Wno-deprecated-declarations -I libs -I libs/sokol -I libs/stb -I src"
SAN="-fsanitize=address,undefined"

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

if [ "$(uname)" = "Darwin" ]; then
    clang -x objective-c $CFLAGS src/main.c -o build/game \
        -framework Cocoa -framework QuartzCore -framework OpenGL -framework AudioToolbox $LDFLAGS

    # Wrap in an .app bundle so `open -g build/game.app` can launch it without
    # stealing focus. `open` cannot launch a bare executable, and a shell-launched
    # GUI app always activates itself.
    APP=build/game.app
    mkdir -p $APP/Contents/MacOS
    cat > $APP/Contents/Info.plist <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleExecutable</key><string>game</string>
  <key>CFBundleIdentifier</key><string>local.terrain</string>
  <key>CFBundleName</key><string>game</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>LSMinimumSystemVersion</key><string>10.15</string>
</dict></plist>
PLIST
    cp build/game $APP/Contents/MacOS/game
else
    cc $CFLAGS src/main.c -o build/game \
        -lX11 -lGL -lpthread -ldl -lm $LDFLAGS
fi