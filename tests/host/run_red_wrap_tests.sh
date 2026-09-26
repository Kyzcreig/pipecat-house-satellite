#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CC="${CC:-cc}"
BIN="${TMPDIR:-/tmp}/test_red_wrap.$$"
trap 'rm -f "$BIN"' EXIT
"$CC" -std=c11 -Wall -Wextra -Werror \
  "$ROOT/tests/host/test_red_wrap.c" \
  "$ROOT/xiao-esp32-s3/components/peer/red_wrap.c" \
  -o "$BIN"
"$BIN"
