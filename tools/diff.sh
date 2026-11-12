#!/usr/bin/env bash
# Compare two renders. "The hash changed" is not actionable; this says by how
# much and where.
#
#   tools/diff.sh tests/golden/default.png build/shots/golden-default.png [--tol 0.3]
#
# Exits 1 when the mean per-channel difference exceeds --tol (percent of 255).
# --heat FILE writes the per-pixel difference, amplified, as a PNG.
set -euo pipefail
cd "$(dirname "$0")/.."
exec python3 tools/diff.py "$@"
