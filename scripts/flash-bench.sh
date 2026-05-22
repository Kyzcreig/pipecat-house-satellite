#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
USER_HOME="${USER_HOME:-/Users/alexgierczyk}"
IDF_PATH="${IDF_PATH:-$USER_HOME/.platformio/packages/framework-espidf}"
PORT="${PORT:-/dev/cu.usbmodem4101}"

export HOME="${HOME_OVERRIDE:-$USER_HOME}"
export PATH="$USER_HOME/.platformio/tools/tool-cmake/bin:$USER_HOME/.platformio/packages/tool-cmake/bin:$USER_HOME/.platformio/tools/tool-ninja:$USER_HOME/.platformio/packages/tool-ninja:$PATH"
export WIFI_SSID="${WIFI_SSID:-Wi-Fight this Feeling}"
export PIPECAT_SMALLWEBRTC_URL="${PIPECAT_SMALLWEBRTC_URL:-http://skynet.local:7860/api/offer}"
export PIPECAT_BENCH_SEND_TONE="${PIPECAT_BENCH_SEND_TONE:-1}"

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
idf.py -p "$PORT" "$@"
