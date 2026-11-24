#!/usr/bin/env bash
# /xvf/route calibration routing (t_3565803e): policy unit tests + source contract.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC="$ROOT/xiao-esp32-s3/src"
BIN="$(mktemp /tmp/pipecat-xvf-route-cal.XXXXXX)"
trap 'rm -f "$BIN"' EXIT

CXX="${CXX:-c++}"
"$CXX" -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-sanitize-recover=all -I "$SRC" \
  "$ROOT/tests/host/test_xvf_route_cal.cpp" -o "$BIN"
"$BIN"
python3 "$ROOT/tests/host/test_xvf_route_cal_source.py"
