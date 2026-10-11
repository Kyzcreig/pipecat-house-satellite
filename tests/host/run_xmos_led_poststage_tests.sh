#!/usr/bin/env bash
# XMOS LED post-stage tune params (t_61530bca): source contract + mutation arms
# that prove the contract gates (each mutation must turn it RED).
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
T=tests/host/test_xmos_led_poststage_source.py
MEDIA=xiao-esp32-s3/src/media.cpp
python3 "$T" || exit 1

BK="$(mktemp)"
cp "$MEDIA" "$BK"
restore() { cp "$BK" "$MEDIA"; }
trap 'restore; rm -f "$BK"' EXIT

fails=0
check() { # name
  if python3 "$T" >/dev/null 2>&1; then
    echo "MUTATION SURVIVED (contract is blind): $1"; fails=$((fails + 1))
  else
    echo "ok - contract goes RED on: $1"
  fi
  restore
}

perl -0pi -e 's/\{"xmos_led_brightness", XVF_RESID_GPO, XVF_CMD_GPO_LED_BRIGHTNESS,\n\s*TuneTarget::XVF_UINT8, false, true, 0\.0f, 255\.0f, 127\.0f, false\},\n//' "$MEDIA"
check "xmos_led_brightness row removed"
perl -0pi -e 's/(\{"xmos_led_gammify", XVF_RESID_GPO, XVF_CMD_GPO_LED_GAMMIFY,\n\s*TuneTarget::XVF_UINT8, )false/${1}true/' "$MEDIA"
check "xmos_led_gammify made NVS-persistent (would enter the golden key-set)"
perl -0pi -e 's/xvf_write_bytes\(resid, cmd, &value, 1\)/xvf_write_bytes(resid, cmd, &value, 4)/' "$MEDIA"
check "uint8 write widened to 4 bytes"
perl -0pi -e 's/  \} else if \(is_uint8\) \{\n    ret = xvf_write_uint8\(entry->resid, entry->cmd,\n\s*static_cast<uint8_t>\(applied_value\)\);\n//' "$MEDIA"
check "tune dispatch falls through to int32 for uint8 params"
perl -0pi -e 's/(static esp_err_t xvf_write_led_ring\(const uint32_t rgb\[XVF_LED_COUNT\]\) \{\n)/$1  const float k = s_led_brightness.load() \/ 255.0f; (void)k;\n/' "$MEDIA"
check "second scaler added at xvf_write_led_ring"

exit "$fails"
