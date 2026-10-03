#!/usr/bin/env bash
# Fixed-beam tune surface contract (t_a6061357) + mutation arms proving it gates.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
T=tests/host/test_xvf_fixed_beam_source.py
python3 "$T" || exit 1

MEDIA=xiao-esp32-s3/src/media.cpp
BK="$(mktemp)"
cp "$MEDIA" "$BK"
restore() { cp "$BK" "$MEDIA"; }
trap 'restore; rm -f "$BK"' EXIT

fails=0
check() {
  if python3 "$T" >/dev/null 2>&1; then
    echo "MUTATION SURVIVED (contract is blind): $1"; fails=$((fails + 1))
  else
    echo "ok - contract goes RED on: $1"
  fi
  restore
}

perl -0pi -e 's/\{"fixed_beams_onoff", XVF_RESID_AEC, XVF_CMD_AEC_FIXEDBEAMSONOFF,\n     TuneTarget::XVF_INT32, false,/{"fixed_beams_onoff", XVF_RESID_AEC, XVF_CMD_AEC_FIXEDBEAMSONOFF,\n     TuneTarget::XVF_INT32, true,/' "$MEDIA"
check "fixed_beams_onoff made NVS-persistent (reboot no longer fail-safe)"
perl -0pi -e 's/record\(xvf_write_int32\(XVF_RESID_AEC, XVF_CMD_AEC_FIXEDBEAMSONOFF, 0\)\);/record(xvf_write_int32(XVF_RESID_AEC, XVF_CMD_AEC_FIXEDBEAMSONOFF, 1));/' "$MEDIA"
check "boot profile bakes fixed beams ON"
perl -0pi -e 's/  pair\[slot\] = value;\n/  pair[0] = value;\n  pair[1] = value;\n/' "$MEDIA"
check "pair write clobbers the other beam slot"
perl -0pi -e 's/entry\.target != TuneTarget::XVF_FLOAT &&\n      entry\.target != TuneTarget::XVF_FLOAT_PAIR_SLOT/entry.target != TuneTarget::XVF_FLOAT/' "$MEDIA"
check "azimuth radians int-truncated by normalize"

if [ "$fails" -eq 0 ]; then
  echo "all 4 mutations caught; fixed-beam contract: PASS"
else
  echo "$fails mutation(s) survived"; exit 1
fi
