#!/usr/bin/env bash
# Host-side NACK retransmit client tests — pure logic, no ESP-IDF needed.
# Run from anywhere: ./tests/host/run_nack_tests.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PEER="$ROOT/xiao-esp32-s3/components/peer"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

CC="${CC:-cc}"
"$CC" -std=c99 -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer -I "$PEER" \
  "$ROOT/tests/host/test_nack_client.c" "$PEER/nack_client.c" \
  -o "$OUT/test_nack_client"
"$OUT/test_nack_client"
