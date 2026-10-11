#!/usr/bin/env bash
# tune-knob effect lint (t_029fcce2): every /xvf/tune knob must have a consumer
# that reaches hardware, not only store/load/readback/log. Runs the lint on the
# real tree (must be GREEN), then on mutated COPIES of the firmware source (the
# tree itself is never edited) that each reproduce one inert-knob shape; every
# mutation must turn the lint RED and name the knob. A guard that cannot fail
# is decoration.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
LINT=scripts/tune_knob_effect_lint.py
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

fails=0
python3 "$LINT" --repo . || { echo "FAIL: lint is red on the real tree"; exit 1; }

mutant() { # name knob perl-expr [file]
  local name="$1" knob="$2" expr="$3" file="${4:-media.cpp}"
  rm -rf "$WORK/m" && mkdir -p "$WORK/m/xiao-esp32-s3" && cp -R xiao-esp32-s3/src "$WORK/m/xiao-esp32-s3/src"
  perl -0pi -e "$expr" "$WORK/m/xiao-esp32-s3/src/$file"
  if cmp -s "xiao-esp32-s3/src/$file" "$WORK/m/xiao-esp32-s3/src/$file"; then
    echo "MUTATION DID NOT APPLY (source moved; update the runner): $name"; fails=$((fails+1)); return
  fi
  local out rc
  out="$(python3 "$LINT" --repo "$WORK/m" 2>&1)"; rc=$?
  if [ "$rc" -eq 1 ] && grep -q "FAIL tune knob '$knob'" <<<"$out"; then
    echo "ok - lint goes RED on: $name"
    grep "FAIL tune knob '$knob'" <<<"$out" | sed 's/^/     /'
  else
    echo "MUTATION SURVIVED (lint is blind, rc=$rc): $name"; echo "$out" | sed 's/^/     /'; fails=$((fails+1))
  fi
}

# 1. The 2026-10-11 claim, made true: led_brightness is stored, persisted, read
#    back and printed in /playback/stats, but no render path scales by it.
mutant "led_brightness stored+readback+status-print, no render path reads it" led_brightness \
  's{s_led_brightness\.load\(\) / 255\.0f}{1.0f}g; s{const uint8_t b = s_led_brightness\.load\(\);}{const uint8_t b = 255;}'

# 2. A new knob whose tune branch only records the value in a global and logs it.
mutant "new knob with a store-and-log-only tune branch" ring_gamma \
  's{(\{"led_brightness", 0, 0, TuneTarget::LED_BRIGHTNESS)}{\{"ring_gamma", 0, 0, TuneTarget::RING_GAMMA, false, true, 0.0f, 4.0f, 1.0f, false\},\n    $1}; s{(  LED_BRIGHTNESS,  //)}{  RING_GAMMA,\n$1}; s{(static std::atomic<uint8_t> s_led_brightness)}{static std::atomic<float> s_ring_gamma\{1.0f\};\n$1}; s{(  if \(entry->target == TuneTarget::LED_BRIGHTNESS\) \{\n    // Range-clamped)}{  if (entry->target == TuneTarget::RING_GAMMA) \{\n    s_ring_gamma.store(applied_value);\n    ESP_LOGI(LOG_TAG, "ring_gamma=%.2f", (double)s_ring_gamma.load());\n    result->readback = s_ring_gamma.load();\n    result->readback_valid = true;\n    result->applied = true;\n    return ESP_OK;\n  \}\n$1}'

# 3. Same new knob, read back by /xvf/read and printed via a one-line accessor in
#    ota.cpp status JSON: the accessor's callers are bookkeeping, still RED.
mutant "new knob read only by readback + an accessor used in a status print" ring_gamma \
  's{(\{"led_brightness", 0, 0, TuneTarget::LED_BRIGHTNESS)}{\{"ring_gamma", 0, 0, TuneTarget::RING_GAMMA, false, true, 0.0f, 4.0f, 1.0f, false\},\n    $1}; s{(  LED_BRIGHTNESS,  //)}{  RING_GAMMA,\n$1}; s{(static std::atomic<uint8_t> s_led_brightness)}{static std::atomic<float> s_ring_gamma\{1.0f\};\nfloat pipecat_ring_gamma() \{\n  return s_ring_gamma.load();\n\}\n$1}; s{(  if \(entry->target == TuneTarget::LED_BRIGHTNESS\) \{\n    \*readback)}{  if (entry->target == TuneTarget::RING_GAMMA) \{\n    *readback = s_ring_gamma.load();\n    *readback_valid = true;\n    return ESP_OK;\n  \}\n$1}; s{(  if \(entry->target == TuneTarget::LED_BRIGHTNESS\) \{\n    // Range-clamped)}{  if (entry->target == TuneTarget::RING_GAMMA) \{\n    s_ring_gamma.store(applied_value);\n    printf("ring_gamma %f\\n", (double)pipecat_ring_gamma());\n    result->applied = true;\n    return ESP_OK;\n  \}\n$1}'

# 4. An XVF register knob whose write was dropped from the generic branch (the
#    tune path reads back but never writes): every generic knob loses its sink.
mutant "generic XVF branch writes nothing (readback-only tune)" agc_maxgain \
  's{    ret = xvf_write_float\(entry->resid, entry->cmd, applied_value\);}{    ret = ESP_OK;}; s{    ret = xvf_write_int32\(entry->resid, entry->cmd,\n\s+static_cast<int32_t>\(applied_value\)\);}{    ret = ESP_OK;}; s{    ret = xvf_write_float_pair_slot\(entry->resid, entry->cmd, entry->pair_slot,\n\s+applied_value\);}{    ret = ESP_OK;}; s{    ret = xvf_write_uint8\(entry->resid, entry->cmd,\n\s+static_cast<uint8_t>\(applied_value\)\);}{    ret = ESP_OK;}'

echo
if [ "$fails" -eq 0 ]; then
  echo "all 4 mutations caught; the tune-knob effect lint actually gates"
  exit 0
fi
echo "$fails mutation(s) survived or failed to apply"
exit 1
