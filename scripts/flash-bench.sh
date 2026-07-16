#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
USER_HOME="${USER_HOME:-/Users/alexgierczyk}"
IDF_PATH="${IDF_PATH:-$USER_HOME/.platformio/packages/framework-espidf}"
PORT="${PORT:-/dev/cu.usbmodem4101}"
REQUIRED_FIRMWARE_BASE_COMMIT="${REQUIRED_FIRMWARE_BASE_COMMIT:-fc3edc0}"

# Refuse to build from a branch that omits the currently deployed production
# lineage. This is the code-provenance equivalent of the MAC identity gate below.
if ! git -C "$REPO_ROOT" merge-base --is-ancestor \
  "$REQUIRED_FIRMWARE_BASE_COMMIT" HEAD; then
  echo "flash-bench: ABORT — HEAD does not descend from required production base $REQUIRED_FIRMWARE_BASE_COMMIT." >&2
  exit 4
fi
echo "flash-bench: provenance preflight PASS — HEAD descends from $REQUIRED_FIRMWARE_BASE_COMMIT." >&2

export HOME="${HOME_OVERRIDE:-$USER_HOME}"
export PATH="$USER_HOME/.platformio/tools/tool-cmake/bin:$USER_HOME/.platformio/packages/tool-cmake/bin:$USER_HOME/.platformio/tools/tool-ninja:$USER_HOME/.platformio/packages/tool-ninja:$PATH"
export WIFI_SSID="${WIFI_SSID:-Wi-Fight this Feeling}"
export PIPECAT_SMALLWEBRTC_URL="${PIPECAT_SMALLWEBRTC_URL:-http://192.168.1.78:7860/api/offer}"
export PIPECAT_BENCH_SEND_TONE="${PIPECAT_BENCH_SEND_TONE:-1}"
export PIPECAT_SATELLITE_ID="${PIPECAT_SATELLITE_ID:-bench}"
export PIPECAT_MDNS_HOSTNAME="${PIPECAT_MDNS_HOSTNAME:-${PIPECAT_SATELLITE_ID}-xvf3800}"
export PIPECAT_MDNS_INSTANCE="${PIPECAT_MDNS_INSTANCE:-${PIPECAT_SATELLITE_ID} XVF3800 Voice Satellite}"
export PIPECAT_AEC_FAR_EXTGAIN_DB="${PIPECAT_AEC_FAR_EXTGAIN_DB:-0.0f}"

if [[ -z "${WIFI_PASSWORD:-}" ]]; then
  if [[ -f "$USER_HOME/.openclaw/.op-service-token" ]] && command -v op >/dev/null 2>&1; then
    export OP_SERVICE_ACCOUNT_TOKEN="$(<"$USER_HOME/.openclaw/.op-service-token")"
    export OP_CACHE=false
    WIFI_PASSWORD="$(
      op item get --cache=false \
        "${WIFI_1P_ITEM:-Wi-Fi — Wi-Fight this Feeling (2.4GHz)}" \
        --vault "${WIFI_1P_VAULT:-Engineering}" \
        --reveal \
        --field "${WIFI_1P_FIELD:-wireless network password}"
    )"
    export WIFI_PASSWORD
  else
    echo "WIFI_PASSWORD is required, or install/configure 1Password CLI service-token access." >&2
    exit 2
  fi
fi

if [[ ! -f "$IDF_PATH/export.sh" ]]; then
  echo "ESP-IDF export.sh not found at $IDF_PATH/export.sh" >&2
  exit 2
fi

# shellcheck source=/dev/null
. "$IDF_PATH/export.sh" >/dev/null

cd "$REPO_ROOT/xiao-esp32-s3"
if [[ ! -f sdkconfig ]] || ! grep -q '^CONFIG_IDF_TARGET="esp32s3"' sdkconfig; then
  idf.py --preview set-target esp32s3
fi
idf.py reconfigure
if [[ "$#" -eq 0 ]]; then
  set -- flash
fi

# --- Device-identity preflight (guards the 2026-07-05 mix-up) -----------------
# If this invocation FLASHES, resolve the connected board's MAC->room and refuse
# to proceed if it isn't the room we mean. Set FLASH_ASSERT_ROOM=theater|kitchen
# (or it auto-derives from PIPECAT_SATELLITE_ID when that names a known room).
# Bypass for a genuinely new/unknown board with FLASH_ASSERT_ROOM=skip.
_want_room="${FLASH_ASSERT_ROOM:-}"
if [[ -z "$_want_room" ]]; then
  case "${PIPECAT_SATELLITE_ID:-}" in
    theater|kitchen) _want_room="$PIPECAT_SATELLITE_ID" ;;
  esac
fi
if [[ "$*" == *flash* && -n "$_want_room" && "$_want_room" != "skip" ]]; then
  echo "flash-bench: device-identity preflight — asserting connected board is '$_want_room'..." >&2
  if ! python3 "$REPO_ROOT/scripts/device_identity.py" --port "$PORT" --assert "$_want_room"; then
    echo "flash-bench: ABORT — connected board is NOT '$_want_room'. Refusing to flash the wrong satellite." >&2
    echo "flash-bench: (bypass for a new/unknown board with FLASH_ASSERT_ROOM=skip)" >&2
    exit 3
  fi
fi
# -----------------------------------------------------------------------------

idf.py -p "$PORT" "$@"
