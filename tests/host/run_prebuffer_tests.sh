#!/usr/bin/env bash
# Host-side prebuffer_ctl tests (gap_resumes + Phase 6 adaptive prebuffer) —
# pure C logic, no ESP-IDF needed. Run from anywhere:
#   ./tests/host/run_prebuffer_tests.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC="$ROOT/xiao-esp32-s3/src"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

CC="${CC:-cc}"
"$CC" -std=c99 -Wall -Wextra -Werror -I "$SRC" \
  "$ROOT/tests/host/test_prebuffer_ctl.c" "$SRC/prebuffer_ctl.c" \
  -o "$OUT/test_prebuffer_ctl"
"$OUT/test_prebuffer_ctl"
