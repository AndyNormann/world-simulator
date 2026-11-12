#!/usr/bin/env bash
# Fast, GPU-less checks. Replays run in `dump` mode: sim only, and the asserts
# that need a rendered frame are skipped rather than failing.
set -euo pipefail
cd "$(dirname "$0")/.."
./build.sh test
./build.sh
./build/game dump replays/smoke.txt
