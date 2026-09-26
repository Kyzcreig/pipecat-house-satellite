#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"
CXX="${CXX:-c++}"
BIN="$(mktemp /tmp/pipecat-boot-guard.XXXXXX)"
trap 'rm -f "$BIN"' EXIT

"$CXX" -std=c++17 -Wall -Wextra -Werror test_boot_guard.cpp -o "$BIN"
"$BIN"
python3 test_power_recovery_source.py
