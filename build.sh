#!/usr/bin/env bash
# Thin wrapper over the Makefile, because `./build.sh [game|test|debug]` is what
# every note and agent remembers.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build/shots
case "${1:-game}" in
    test)  make --no-print-directory -j4 test ;;
    debug) make --no-print-directory -j4 MODE=debug game ;;
    tsan)  make --no-print-directory -j4 MODE=tsan game ;;
    *)     make --no-print-directory -j4 game ;;
esac
