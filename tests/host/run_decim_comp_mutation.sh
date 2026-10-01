#!/usr/bin/env bash
# Mutation check for the decim_comp source contract: each mutation below
# removes ONE property the contract claims to pin. The contract must go RED for
# every one of them. A source guard that cannot fail is decoration.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
MEDIA=xiao-esp32-s3/src/media.cpp
CMAKE=xiao-esp32-s3/src/CMakeLists.txt
COMP=xiao-esp32-s3/src/decim_comp.c
OTA=xiao-esp32-s3/src/ota.cpp
CFG=xiao-esp32-s3/src/pipecat_build_config.h.in
BK="$(mktemp -d)"
cp "$MEDIA" "$BK/media" ; cp "$CMAKE" "$BK/cmake" ; cp "$COMP" "$BK/comp" ; cp "$OTA" "$BK/ota" ; cp "$CFG" "$BK/cfg"
restore() { cp "$BK/media" "$MEDIA"; cp "$BK/cmake" "$CMAKE"; cp "$BK/comp" "$COMP"; cp "$BK/ota" "$OTA"; cp "$BK/cfg" "$CFG"; }
trap 'restore; rm -rf "$BK"' EXIT

fails=0
check() { # name
  if python3 tests/host/test_decim_comp_source.py >/dev/null 2>&1; then
    echo "MUTATION SURVIVED (contract is blind): $1"; fails=$((fails+1))
  else
    echo "ok - contract goes RED on: $1"
  fi
  restore
}

# 1. the fix is never called on the dual-stream path (the inert-fix class)
perl -0pi -e 's/decim_comp_run_stereo\(&s_decim_comp_l/decim_comp_noop_stereo(&s_decim_comp_l/' "$MEDIA"
check "dual-stream capture path no longer calls the compensator"

# 2. the fix is never called on the mono path (silent divergence between paths)
perl -0pi -e 's/      decim_comp_run\(&s_decim_comp_l/      \/\/ decim_comp_run(&s_decim_comp_l/' "$MEDIA"
check "mono capture path no longer calls the compensator"

# 3. not compiled in at all
perl -0pi -e 's/"decim_comp\.c"//' "$CMAKE"
check "decim_comp.c dropped from the component SRCS"

# 4. state never initialised (stale history across a reconnect)
perl -0pi -e 's/  decim_comp_init\(&s_decim_comp_r\);\n//' "$MEDIA"
check "right-channel state left uninitialised"

# 5. taps no longer unity gain (a silent level shift that confounds the A/B)
perl -0pi -e 's/18892/18992/' "$COMP"
check "taps no longer sum to Q14 unity"

# 6. accumulator shift no longer matches the tap format (6 dB error)
perl -0pi -e 's/acc >> 14/acc >> 15/' "$COMP"
check "accumulator shift desynced from the Q14 taps"

# 7. cost instrumentation dropped from /playback/stats
perl -0pi -e 's/\\"decim_comp_max_us\\":%lu,//' "$OTA"
check "per-frame cost no longer reported on /playback/stats"

# 8. build gate removed (no one-rebuild revert)
perl -0pi -e 's/#define PIPECAT_DECIM_COMP 0/#define PIPECAT_DECIM_COMP_X 0/' "$CFG"
check "build gate renamed away"

echo
if [ "$fails" -eq 0 ]; then
  echo "all 8 mutations caught — the source contract actually gates"
else
  echo "$fails mutation(s) survived"; exit 1
fi
