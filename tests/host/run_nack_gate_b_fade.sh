#!/usr/bin/env bash
# Deterministic host replay of the Gate-B one-second fade + 60s breaker re-probe.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PEER="$ROOT/xiao-esp32-s3/components/peer"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

CC="${CC:-cc}"
"$CC" -std=c99 -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer -I "$PEER" \
  "$ROOT/tests/host/test_nack_gate_b_fade.c" "$PEER/nack_client.c" \
  -o "$OUT/test_nack_gate_b_fade"
"$OUT/test_nack_gate_b_fade"
