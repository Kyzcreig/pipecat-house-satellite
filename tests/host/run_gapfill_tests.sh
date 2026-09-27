#!/usr/bin/env bash
# Host-side downlink gap-fill (in-band FEC vs PLC attribution) tests — links the
# VENDORED esp-libopus exactly as the firmware does (FIXED_POINT) and decodes a
# real live-config downlink stream (tests/host/fixtures/downlink/). No ESP-IDF.
# Run from anywhere: ./tests/host/run_gapfill_tests.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC="$ROOT/xiao-esp32-s3/src"
OPUS="$ROOT/esp32-s3-box-3/components/esp-libopus"
FIX="$ROOT/tests/host/fixtures/downlink"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

if [ ! -f "$OPUS/include/opus.h" ]; then
  # CI's actions/checkout does not fetch submodules; fetch just this one.
  git -C "$ROOT" submodule update --init esp32-s3-box-3/components/esp-libopus >&2 || true
fi
if [ ! -f "$OPUS/include/opus.h" ]; then
  echo "esp-libopus submodule is empty — run: git submodule update --init --recursive" >&2
  exit 2
fi

CC="${CC:-cc}"
# Same source list + defines as the component's CMakeLists.txt (fixed-point build).
OPUS_SRCS="$(grep -ho '"[a-zA-Z_/0-9]*\.c"' "$OPUS/celt_sources.mk" "$OPUS/opus_sources.mk" "$OPUS/silk_sources.mk" | tr -d '"' | sort -u | sed "s|^|$OPUS/|")"
i=0
for f in $OPUS_SRCS; do
  "$CC" -std=gnu99 -O1 -w -c "$f" -o "$OUT/opus_$i.o" -I "$OPUS/include" -I "$OPUS/celt" -I "$OPUS/silk" -I "$OPUS/silk/fixed" -I "$OPUS/src" \
    -DHAVE_LRINT -DHAVE_LRINTF -DFIXED_POINT -DDISABLE_FLOAT_API -DUSE_ALLOCA -DOPUS_BUILD
  i=$((i + 1))
done

"$CC" -std=c99 -Wall -Wextra -Werror -I "$SRC" -I "$OPUS/include" \
  "$ROOT/tests/host/test_opus_gapfill.c" "$SRC/opus_gapfill.c" "$OUT"/opus_*.o \
  -lm -o "$OUT/test_opus_gapfill"
"$OUT/test_opus_gapfill" "$FIX"
python3 "$ROOT/tests/host/test_opus_gapfill_source.py"
