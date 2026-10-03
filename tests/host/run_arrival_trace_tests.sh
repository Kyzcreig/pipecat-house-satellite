#!/usr/bin/env bash
# Host tests for the downlink arrival trace (t_5faf78b4).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
cc -std=c11 -Wall -Wextra -Werror -I"$ROOT/xiao-esp32-s3/components/peer" \
  "$ROOT/tests/host/test_arrival_trace.c" \
  "$ROOT/xiao-esp32-s3/components/peer/arrival_trace.c" -o "$OUT/t"
"$OUT/t"
