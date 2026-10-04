#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"
CXX="${CXX:-c++}"
BIN="$(mktemp /tmp/pipecat-webrtc-redial.XXXXXX)"
trap 'rm -f "$BIN"' EXIT

"$CXX" -std=c++17 -Wall -Wextra -Werror test_webrtc_redial.cpp -o "$BIN"
"$BIN"
python3 test_webrtc_redial_source.py
