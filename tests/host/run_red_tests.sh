#!/usr/bin/env bash
# Host-side RED (RFC 2198) unwrap tests — pure byte logic, no ESP-IDF needed.
# Run from anywhere: ./tests/host/run_red_tests.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PEER="$ROOT/xiao-esp32-s3/components/peer"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

CC="${CC:-cc}"
"$CC" -std=c99 -Wall -Wextra -Werror -I "$PEER" \
  "$ROOT/tests/host/test_red_unwrap.c" "$PEER/red_unwrap.c" \
  -o "$OUT/test_red_unwrap"
"$OUT/test_red_unwrap"
