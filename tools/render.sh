#!/usr/bin/env bash
# The one command an agent needs. Always rebuilds first: rendering a binary from
# before the edit is the classic background-agent failure.
#
#   tools/render.sh shot   [--seed N --cam x,y,z --yaw DEG --pitch DEG --time H --out FILE]
#   tools/render.sh replay replays/smoke.txt [--seed N --size WxH]
#
# Metrics go to stdout; the shot also writes <out>.small.png (900px) so a model
# never has to read a full-size image.
set -euo pipefail
cd "$(dirname "$0")/.."

mode="${1:-shot}"
[ $# -gt 0 ] && shift
mkdir -p build/shots
./build.sh >/dev/null
exec ./build/game "$mode" "$@"
