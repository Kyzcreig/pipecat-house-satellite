#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"
CXX="${CXX:-c++}"
BIN="$(mktemp /tmp/pipecat-server-liveness.XXXXXX)"
trap 'rm -f "$BIN"' EXIT

"$CXX" -std=c++17 -Wall -Wextra -Werror test_server_liveness.cpp -o "$BIN"
"$BIN"
python3 test_server_liveness_source.py
