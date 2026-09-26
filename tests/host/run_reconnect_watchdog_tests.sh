#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"
CXX="${CXX:-c++}"
BIN="$(mktemp /tmp/pipecat-reconnect-watchdog.XXXXXX)"
trap 'rm -f "$BIN"' EXIT

"$CXX" -std=c++17 -Wall -Wextra -Werror test_reconnect_watchdog.cpp -o "$BIN"
"$BIN"
