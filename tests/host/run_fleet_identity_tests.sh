#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"
CXX="${CXX:-c++}"
BIN="$(mktemp /tmp/pipecat-fleet-identity.XXXXXX)"
trap 'rm -f "$BIN"' EXIT

"$CXX" -std=c++17 -Wall -Wextra -Werror test_fleet_identity.cpp -o "$BIN"
"$BIN"
python3 test_fleet_identity_source.py
