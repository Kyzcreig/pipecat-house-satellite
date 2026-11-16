#!/usr/bin/env bash
# ESPHome native API interop (t_6215e36e): the firmware's esphome_api.c +
# ble_hci.c behind a loopback socket, driven by the Home Assistant client
# library at the version HA 2026.10.2 pins (aioesphomeapi==46.6.0). Proves
# handshake, device info / feature flags, keepalive pings and raw
# advertisement delivery against the real decoder, not our own.
# Needs python3 with venv + network for the pinned wheels (first run only
# when PIPECAT_INTEROP_VENV points at a kept venv).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
CC="${CC:-cc}"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
VENV="${PIPECAT_INTEROP_VENV:-$OUT/venv}"
if [ ! -x "$VENV/bin/python" ]; then
  python3 -m venv "$VENV"
  "$VENV/bin/pip" install -q 'aioesphomeapi==46.6.0' 'bleak-esphome==4.0.0' \
    'bluetooth-data-tools==1.29.24'
fi
"$CC" -std=c99 -Wall -Wextra -Werror -I xiao-esp32-s3/src \
  tests/host/esphome_api_host_server.c xiao-esp32-s3/src/esphome_api.c \
  xiao-esp32-s3/src/ble_hci.c -o "$OUT/server"
timeout 90 "$VENV/bin/python" tests/host/test_esphome_api_interop.py "$OUT/server"
