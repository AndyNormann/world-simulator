#!/usr/bin/env bash
# Thin wrapper over the Makefile, because `./build.sh [game|test|debug]` is what
# every note and agent remembers.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build/shots
# BACKEND=gl forces the GL backend on any platform (the Makefile defaults to
# Metal on macOS and GL elsewhere). Passed through so `BACKEND=gl tools/check.sh`
# runs the whole gate against the GL path.
MKARGS="BACKEND=${BACKEND:-auto}"
if [ "${BACKEND:-auto}" = "auto" ]; then MKARGS=""; fi
case "${1:-game}" in
    test)  make --no-print-directory -j4 $MKARGS test ;;
    debug) make --no-print-directory -j4 $MKARGS MODE=debug game ;;
    tsan)  make --no-print-directory -j4 $MKARGS MODE=tsan game ;;
    *)     make --no-print-directory -j4 $MKARGS game ;;
esac
