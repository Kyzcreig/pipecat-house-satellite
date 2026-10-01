#!/usr/bin/env bash
# Merged-but-never-flashed features stay default-off (t_4f8fe707), plus a
# mutation arm per property so the contract is shown to gate. No ESP-IDF needed.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
T=tests/host/test_unflashed_default_off_source.py
python3 "$T" || exit 1

CFG=xiao-esp32-s3/src/pipecat_build_config.h.in
SRCCM=xiao-esp32-s3/src/CMakeLists.txt
MEDIA=xiao-esp32-s3/src/media.cpp
OTA=xiao-esp32-s3/src/ota.cpp
BK="$(mktemp -d)"
for f in "$CFG" "$SRCCM" "$MEDIA" "$OTA"; do cp "$f" "$BK/$(basename "$f")"; done
restore() { for f in "$CFG" "$SRCCM" "$MEDIA" "$OTA"; do cp "$BK/$(basename "$f")" "$f"; done; }
trap 'restore; rm -rf "$BK"' EXIT

fails=0
check() { # name
  if python3 "$T" >/dev/null 2>&1; then
    echo "MUTATION SURVIVED (contract is blind): $1"; fails=$((fails + 1))
  else
    echo "ok - contract goes RED on: $1"
  fi
  restore
}

perl -0pi -e 's/#define PIPECAT_UPLINK_64K 0/#define PIPECAT_UPLINK_64K 1/' "$CFG"
check "uplink 64k default flipped on"
perl -0pi -e 's/#define PIPECAT_DECIM_COMP 0  \/\/ #4 decimator droop comp: built, never flashed, needs its own card/#define PIPECAT_DECIM_COMP 0  \/\/ #4/' "$CFG"
check "decim_comp default lost its note"
perl -0pi -e 's/set\(TARGET_SRC "wifi.cpp"/set(TARGET_SRC "opus_gapfill.c" "wifi.cpp"/' "$SRCCM"
check "opus_gapfill.c compiled unconditionally"
perl -0pi -e 's/#define OPUS_ENCODER_BITRATE 30000/#define OPUS_ENCODER_BITRATE 64000/' "$MEDIA"
check "default uplink bitrate no longer the live 30k"
perl -0pi -e 's/  config.max_uri_handlers = 9;/  config.max_uri_handlers = 10;/' "$OTA"
check "default httpd slots no longer the live 9"
perl -0pi -e 's/(#else\n    int fec_size = opus_decode\(opus_decoder, data, size, decoder_buffer,\n\s+PCM_SAMPLES_PER_FRAME, 1 \/\* decode_fec \*\/\);\n)    s_pending_gap = 0;\n/    s_pending_gap = 0;\n$1/' "$MEDIA"
check "live FEC arm reordered (s_pending_gap before the decode)"

if [ "$fails" -eq 0 ]; then
  echo "all 6 mutations caught; unflashed default-off contract: PASS"
else
  echo "$fails mutation(s) survived"; exit 1
fi
