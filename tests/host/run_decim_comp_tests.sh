#!/usr/bin/env bash
# Host-side decim_comp tests (48->16 kHz decimator droop compensator) —
# pure C logic, no ESP-IDF needed. Run from anywhere:
#   ./tests/host/run_decim_comp_tests.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC="$ROOT/xiao-esp32-s3/src"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

CC="${CC:-cc}"
"$CC" -std=c99 -Wall -Wextra -Werror -I "$SRC" \
  "$ROOT/tests/host/test_decim_comp.c" "$SRC/decim_comp.c" \
  -lm -o "$OUT/test_decim_comp"
"$OUT/test_decim_comp"
