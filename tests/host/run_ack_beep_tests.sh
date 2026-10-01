#!/usr/bin/env bash
# Host-side ack_beep tests (t_69ffa409): the device counts the hub's wake ACK
# beep as PLAYED only when I2S-written audio matches the marker fingerprint.
# Pure C, no ESP-IDF. Run from anywhere:
#   ./tests/host/run_ack_beep_tests.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC="$ROOT/xiao-esp32-s3/src"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

CC="${CC:-cc}"
"$CC" -std=c99 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -I "$SRC" \
  "$ROOT/tests/host/test_ack_beep.c" "$SRC/ack_beep.c" -lm \
  -o "$OUT/test_ack_beep"
"$OUT/test_ack_beep" "$ROOT/tests/host/fixtures/ack_chime_200ms_m10db.pcm" \
  "$SRC/selftest_clip.pcm"
