#!/usr/bin/env bash
# Host-side reproduction of the kitchen NACK-v2 arm reset (t_5c931cb9).
# Pure pthreads model of the transport-loop-vs-RTVI-queue backpressure race —
# no ESP-IDF needed. Run from anywhere: ./tests/host/run_rtvi_backpressure_tests.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

CC="${CC:-cc}"
"$CC" -std=c11 -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  "$ROOT/tests/host/test_rtvi_backpressure.c" \
  -o "$OUT/test_rtvi_backpressure"
"$OUT/test_rtvi_backpressure"
