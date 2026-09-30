#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC="$ROOT/xiao-esp32-s3/src"
BIN="$(mktemp /tmp/pipecat-xvf-aec-filter.XXXXXX)"
trap 'rm -f "$BIN"' EXIT

CXX="${CXX:-c++}"
"$CXX" -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-sanitize-recover=all -I "$SRC" \
  "$ROOT/tests/host/test_xvf_aec_filter.cpp" "$SRC/xvf_aec_filter.cpp" \
  -o "$BIN"
"$BIN"
python3 "$ROOT/tests/host/test_xvf_aec_filter_source.py"
