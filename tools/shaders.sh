#!/usr/bin/env bash
# Regenerate src/shaders.h. Human-triggered: the header is checked in so builds
# never need shdc.
#
# The header carries BOTH backends (metal_macos and glsl410 source) and either
# platform can generate it: Metal bytecode (`-b`) needs the Metal toolchain, so
# that flag is used when the toolchain is there and dropped when it is not. A
# header generated on Linux (MSL source instead of bytecode) builds on macOS and
# vice versa; the only thing lost is the generation-time Metal compile, which
# `tools/check.sh` catches on the first frame anyway.
#
# The tool is per platform: tools/sokol-shdc (macOS) and tools/sokol-shdc-linux,
# both built from the same sokol-tools commit as the vendored sokol headers (see
# libs/VENDORED.md) -- a newer shdc can emit code for an API the headers do not
# have. glsl410 is validated here if glslangValidator is installed, because the
# GL backend is compiled at runtime and a GLSL error would otherwise only show up
# as a black frame.
set -euo pipefail
cd "$(dirname "$0")/.."

case "$(uname -s)" in
    Darwin) SHDC=tools/sokol-shdc ;;
    *)      SHDC=tools/sokol-shdc-linux ;;
esac
if [ ! -x "$SHDC" ]; then
    echo "shaders: $SHDC is missing." >&2
    echo "  Build sokol-tools at the commit pinned in libs/VENDORED.md and put the" >&2
    echo "  binary here. src/shaders.h is checked in, so only editing shaders.glsl" >&2
    echo "  needs it." >&2
    exit 1
fi

gen() { "$SHDC" -i src/shaders.glsl -o src/shaders.h -l metal_macos:glsl410 -f sokol "$@"; }

if gen -b 2>/dev/null; then
    echo "regenerated src/shaders.h (metal bytecode + glsl410)"
elif gen; then
    echo "regenerated src/shaders.h (metal source + glsl410) -- no Metal toolchain here"
else
    echo "shaders: generation failed" >&2
    exit 1
fi

if command -v glslangValidator >/dev/null 2>&1; then
    mkdir -p build/shaders
    python3 - <<'PY'
# the generated header stores each stage's GLSL as a byte array; pull them out and
# write real .glsl files so the validator can see them
import re, pathlib
src = pathlib.Path('src/shaders.h').read_text()
out = pathlib.Path('build/shaders')
for m in re.finditer(r'static const uint8_t (\w+_source_glsl410)\[(\d+)\] = \{(.*?)\};',
                     src, re.S):
    name, body = m.group(1), m.group(3)
    data = bytes(int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{2})', body))
    ext = '.vert' if name.endswith('_vs_source_glsl410') else '.frag'
    (out / (name.replace('_source_glsl410', '') + ext)).write_bytes(data)
PY
    fail=0
    for f in build/shaders/*.vert build/shaders/*.frag; do
        [ -e "$f" ] || continue
        glslangValidator -S "${f##*.}" "$f" >/dev/null || { echo "shaders: GLSL validation failed in $f" >&2; fail=1; }
    done
    [ $fail -eq 0 ] && echo "shaders: glsl410 validated"
fi
