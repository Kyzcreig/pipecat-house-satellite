#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC="$ROOT/xiao-esp32-s3/src"
BIN="$(mktemp /tmp/pipecat-xvf-beam-json.XXXXXX)"
trap 'rm -f "$BIN"' EXIT

CXX="${CXX:-c++}"
"$CXX" -std=c++17 -Wall -Wextra -Werror -I "$SRC" \
  "$ROOT/tests/host/test_xvf_beam_json.cpp" "$SRC/xvf_beam_json.cpp" \
  -o "$BIN"
"$BIN"
