#!/bin/bash
# t_2a5f2312 (GADGET-3): build a KITCHEN image (Apollo 09:25 PT 10-04) from the golden-cd45607 worktree.
# Env copied from t_4431412c/build_room.sh (== golden-2026-10-03 bench build_environment).
#   B=<builddir> [PIPECAT_WIFI_FAST_REJOIN=1] bash build_arm.sh
# Arm "instr" (flag unset): live behaviour + /ota/status wifi_* counters.
# Arm "fast"  (flag=1):     Muse-pattern cached-BSSID fast re-join, RSSI floor -75.
set -eu
SAT=${SAT:-/Volumes/fleet-scratch/workspaces/house-voice/t_2a5f2312/sat-cd45607}
ROOM=${ROOM:-kitchen}; case $ROOM in kitchen) PORT=7861;; bench) echo "REFUSED: bench off limits (Apollo 09:25 PT 10-04)"; exit 98;; *) exit 2;; esac
B=${B:?build dir}
cd "$SAT"
export IDF_PATH="$HOME/.platformio/packages/framework-espidf"
export IDF_PYTHON_ENV_PATH="$(ls -d $HOME/.espressif/python_env/idf5.5_py3.14_env 2>/dev/null || ls -d $HOME/.espressif/python_env/idf5.5_* | head -1)"
export IDF_TOOLS_PYTHON_CMD="$IDF_PYTHON_ENV_PATH/bin/python"
unset VIRTUAL_ENV
. "$IDF_PATH/export.sh" >/dev/null 2>&1
export WIFI_SSID="Wi-Fight this Feeling"
WIFI_PASSWORD="$(op item get eend5ky2brz2uz6y3apfzn6rki --vault Engineering --fields "wireless network password" --reveal)"
export WIFI_PASSWORD
: "${WIFI_PASSWORD:?empty wifi password}"
export FLASH_ASSERT_ROOM=$ROOM PIPECAT_AEC_FAR_EXTGAIN_DB=12.0f PIPECAT_AGC_DESIRED_LEVEL=0.0045f
export PIPECAT_BENCH_SEND_TONE=0 PIPECAT_DUAL_STREAM=1
export PIPECAT_MDNS_HOSTNAME=${ROOM}-xvf3800 PIPECAT_MDNS_INSTANCE="$(/usr/bin/python3 -c "import sys;print(sys.argv[1].capitalize())" "$ROOM") XVF3800 Voice Satellite"
export PIPECAT_SATELLITE_ID=$ROOM PIPECAT_SMALLWEBRTC_URL="http://192.168.1.216:${PORT}/api/offer"
unset PIPECAT_ARRIVAL_TRACE PIPECAT_ADAPTIVE_PREBUFFER PIPECAT_NACK PIPECAT_UPLINK_64K PIPECAT_DECIM_COMP PIPECAT_FEC_LBRR PIPECAT_XVF_AEC_FILTER
cd xiao-esp32-s3
IDF_PY="$IDF_PYTHON_ENV_PATH/bin/python"
rm -rf "$B" sdkconfig  # stale project sdkconfig shadows sdkconfig.defaults
echo "FAST_REJOIN=${PIPECAT_WIFI_FAST_REJOIN:-unset} HEAD=$(git rev-parse --short HEAD)"
"$IDF_PY" "$IDF_PATH/tools/idf.py" -B "$B" -DPROJECT_VER=$(git rev-parse --short HEAD) reconfigure >$B.log 2>&1
"$IDF_PY" "$IDF_PATH/tools/idf.py" -B "$B" build >>$B.log 2>&1
echo BUILD_RC=$?
grep -E 'PIPECAT_SMALLWEBRTC_URL|PIPECAT_SATELLITE_ID|DUAL_STREAM|WIFI_FAST_REJOIN' "$B/generated/pipecat_build_config.h" | sed 's/WIFI_SSID.*//;s/WIFI_PASSWORD.*//'
echo "compile entries: $(grep -c '"command"\|"arguments"' "$B/compile_commands.json")  DUAL: $(grep -c PIPECAT_DUAL_STREAM "$B/compile_commands.json")  FAST_REJOIN defines: $(grep -c 'DPIPECAT_WIFI_FAST_REJOIN=1' "$B/compile_commands.json")"
shasum -a 256 "$B/src.bin"
"$IDF_PY" -m esptool image_info --version 2 "$B/src.bin" 2>/dev/null | grep -E 'App version|Validation hash' || true
