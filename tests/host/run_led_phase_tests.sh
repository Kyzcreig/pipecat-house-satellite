#!/usr/bin/env bash
# LED 'conversation' phase (t_54a2a223): pure decision tests (C), source
# contract, and mutation arms that prove both gate (each must turn RED).
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
SRC=xiao-esp32-s3/src
OUT="$(mktemp -d)"
CC="${CC:-cc}"
T=tests/host/test_led_phase_source.py

run_c() {
  "$CC" -std=c99 -Wall -Wextra -Werror -I "$SRC" tests/host/test_led_phase.c \
    -lm -o "$OUT/test_led_phase" && "$OUT/test_led_phase"
}
run_c || exit 1
python3 "$T" || exit 1

BK="$(mktemp -d)"
cp "$SRC/led_phase.h" "$SRC/media.cpp" "$SRC/rtvi.cpp" "$BK/"
restore() { cp "$BK/led_phase.h" "$BK/media.cpp" "$BK/rtvi.cpp" "$SRC/"; }
trap 'restore; rm -rf "$BK" "$OUT"' EXIT

fails=0
check() { # name, command
  if eval "$2" >/dev/null 2>&1; then
    echo "MUTATION SURVIVED: $1"; fails=$((fails + 1))
  else
    echo "ok - goes RED on: $1"
  fi
  restore
}

perl -0pi -e 's/return playing \? LED_PH_SPEAKING : LED_PH_CONVERSATION;/return LED_PH_CONVERSATION;/' "$SRC/led_phase.h"
check "speech does not win during conversation" run_c
perl -0pi -e 's/  if \(phase == LED_PH_CONVERSATION\)\n    return playing \? LED_PH_SPEAKING : LED_PH_CONVERSATION;\n//' "$SRC/led_phase.h"
check "conversation subject to the 30 s TTL" run_c
perl -0pi -e 's/float wave = 0\.5f - 0\.5f \* cosf\(two_pi \* t\);/float wave = 1.0f + 0.0f * t;/' "$SRC/led_phase.h"
check "no breath (flat level)" run_c
perl -0pi -e 's/          case hash\("conversation"\):\n            pipecat_led_set_phase\(PIPECAT_LED_PHASE_CONVERSATION\);\n            break;\n//' "$SRC/rtvi.cpp"
check "rtvi drops 'conversation'" "python3 $T"
perl -0pi -e 's/g_server_phase.compare_exchange_strong\(conv, PHASE_NONE\);//' "$SRC/media.cpp"
check "conversation survives peer loss" "python3 $T"
perl -0pi -e 's/led_hsv\(0\.0f, 0\.0f, bmax \*/led_hsv(0.0f, 0.0f, 1.0f */' "$SRC/media.cpp"
check "conversation ring ignores the brightness scalar" "python3 $T"

exit "$fails"
