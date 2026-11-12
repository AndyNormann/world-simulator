#!/usr/bin/env bash
# Emit compile_commands.json so clangd (and anything else that speaks the
# language server protocol) sees the real flags instead of guessing. Regenerate
# after changing Makefile flags; the file itself is gitignored.
set -euo pipefail
cd "$(dirname "$0")/.."
root="$PWD"
python3 - "$root" > compile_commands.json <<'PY'
import json, sys
root = sys.argv[1]
common = ("-std=c99 -g -Wall -Wextra -Wno-deprecated-declarations -O2 "
          "-I libs -I libs/sokol -I libs/stb -I src").split()
objc = ["-fno-objc-arc"]
def entry(src, args):
    return {"directory": root, "file": "%s/%s" % (root, src), "arguments": args}
out = [
    entry("src/main.c", ["clang", "-x", "objective-c"] + common + objc + ["-c", "src/main.c"]),
    entry("src/sokol_impl.c", ["clang", "-x", "objective-c"] + common + objc + ["-c", "src/sokol_impl.c"]),
    entry("src/world.c", ["clang", "-x", "objective-c"] + common + objc + ["-c", "src/world.c"]),
    entry("src/render.c", ["clang", "-x", "objective-c"] + common + objc + ["-c", "src/render.c"]),
    entry("src/replay.c", ["clang", "-x", "objective-c"] + common + objc + ["-c", "src/replay.c"]),
    entry("src/state.c", ["clang", "-x", "objective-c"] + common + objc + ["-c", "src/state.c"]),
    entry("tests/math_test.c", ["clang", "-std=c99", "-g", "-O0", "-I", "libs", "-I", "src", "-c", "tests/math_test.c"]),
    entry("tests/terrain_test.c", ["clang", "-std=c99", "-g", "-O0", "-I", "libs", "-I", "src", "-c", "tests/terrain_test.c"]),
]
print(json.dumps(out, indent=2))
PY
echo "wrote compile_commands.json"
