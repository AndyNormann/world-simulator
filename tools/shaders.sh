#!/usr/bin/env bash
# Regenerate src/shaders.h. Human-triggered: the header is checked in so builds
# never need shdc.
#
# -b emits precompiled Metal bytecode, so a shader syntax error fails *here*
# instead of surfacing as a black frame at runtime. It needs the Metal
# toolchain: `xcodebuild -downloadComponent metaltoolchain` and an
# xcode-select pointing at Xcode. glsl410 stays source, which is what a future
# non-Metal backend needs.
set -euo pipefail
cd "$(dirname "$0")/.."
tools/sokol-shdc -i src/shaders.glsl -o src/shaders.h -l metal_macos:glsl410 -f sokol -b
echo "regenerated src/shaders.h"
