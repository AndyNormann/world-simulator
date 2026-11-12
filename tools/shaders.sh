#!/usr/bin/env bash
# Regenerate src/shaders.h. Human-triggered: the header is checked in so builds
# never need shdc. Bytecode (-b) needs the Metal toolchain
# (xcodebuild -downloadComponent MetalToolchain); source strings do not.
set -euo pipefail
cd "$(dirname "$0")/.."
tools/sokol-shdc -i src/shaders.glsl -o src/shaders.h -l metal_macos:glsl410 -f sokol
echo "regenerated src/shaders.h"
