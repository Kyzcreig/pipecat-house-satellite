#!/usr/bin/env bash
# BLE passive scanner (t_6215e36e): the HCI command/event codec and the
# minimal ESPHome native API codec under ASan/UBSan, the default-off source
# contract of ble_proxy.cpp, then named mutants that must each turn a test
# RED. No ESP-IDF needed.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
CC="${CC:-cc}"
SRC_DIR=xiao-esp32-s3/src
HCI=$SRC_DIR/ble_hci.c
API=$SRC_DIR/esphome_api.c
PROXY=$SRC_DIR/ble_proxy.cpp
SET=$SRC_DIR/ble_proxy_settings.c
OUT="$(mktemp -d)"
BK="$(mktemp -d)"
cp "$HCI" "$API" "$PROXY" "$SET" "$BK/"
restore() {
  cp "$BK/ble_hci.c" "$HCI"; cp "$BK/esphome_api.c" "$API"
  cp "$BK/ble_proxy.cpp" "$PROXY"; cp "$BK/ble_proxy_settings.c" "$SET"
}
trap 'restore; rm -rf "$BK" "$OUT"' EXIT

build() {
  "$CC" -std=c99 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -fno-sanitize-recover=all -I "$SRC_DIR" tests/host/test_ble_hci.c "$HCI" \
    -o "$OUT/hci" &&
  "$CC" -std=c99 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -fno-sanitize-recover=all -I "$SRC_DIR" tests/host/test_esphome_api.c \
    "$API" "$HCI" -o "$OUT/api" &&
  "$CC" -std=c99 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -fno-sanitize-recover=all -I "$SRC_DIR" tests/host/test_ble_proxy_settings.c \
    "$SET" -o "$OUT/set"
}
run() {
  "$OUT/hci" && "$OUT/api" && "$OUT/set" &&
    python3 tests/host/test_ble_proxy_source.py
}
build || exit 1
run || exit 1

fails=0
check() { # name
  if build 2>/dev/null && run >/dev/null 2>&1; then
    echo "MUTATION SURVIVED (test is blind): $1"; fails=$((fails + 1))
  else
    echo "ok - test goes RED on: $1"
  fi
  restore
}
mutate() { # file perl-expr name
  perl -0pi -e "$2" "$1"
  cmp -s "$1" "$BK/$(basename "$1")" && { echo "mutant did not apply: $3"; exit 1; }
  check "$3"
}

mutate "$HCI" 's/for \(int b = 5; b >= 0; --b\)/for (int b = 0; b <= 5; ++b)/' \
  "address byte order reversed"
mutate "$HCI" 's/r->rssi = \(int8_t\)p\[dlen\];/r->rssi = (int8_t)(p[dlen] - 1);/' \
  "rssi off by one (the Espressif example's -(0xFF-x))"
mutate "$HCI" 's/if \(dlen > BLE_ADV_DATA_MAX \|\| /if (/' \
  "31-byte data limit dropped"
mutate "$HCI" 's/buf\[n\+\+\] = passive \? 0x00 : 0x01;/buf[n++] = 0x01;/' \
  "passive scan sends active"
mutate "$API" 's/\(\(uint32_t\)v << 1\) \^ \(uint32_t\)\(v >> 31\)/(uint32_t)v/' \
  "rssi not zigzag-encoded (sint32)"
mutate "$API" 's/pb_field_varint\(&b, 15, ESPAPI_BLE_FEATURES\);/pb_field_varint(&b, 15, 0x23);/' \
  "feature flags claim ACTIVE_CONNECTIONS"
mutate "$API" 's/    if \(len > ESPAPI_RX_MAX - hdr\)\n      return ESPAPI_ERROR;\n//' \
  "declared frame length not bounded"
mutate "$API" 's/        if \(fn == field\)\n          \*out = v;\n//' \
  "subscribe flags never read"
mutate "$PROXY" 's/static bool s_enabled = false;/static bool s_enabled = true;/' \
  "toggle defaults on"
mutate "$PROXY" 's/(void pipecat_ble_proxy_start\(\) \{\n  load_nvs\(\);\n)/$1  esp_bt_controller_init(nullptr);\n/' \
  "controller started before the enabled gate"
mutate "$PROXY" 's/    esp_bt_controller_mem_release\(ESP_BT_MODE_BLE\);\n//' \
  "OFF keeps the controller RAM"
# Prism round 1 on #44 (P1 f9a136d7f284, 977995c2c2b2; P2 09effbd59d49,
# ef8a8c874df9, 28f87717c228).
mutate "$PROXY" 's/(  esp_err_t ret = esp_bt_controller_init\(&cfg\);\n)/  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);\n$1/' \
  "scanner ON switches voice Wi-Fi to modem sleep"
mutate "$PROXY" 's/mdns_service_add\(s_info.name, "_esphomelib"/mdns_service_add(nullptr, "_esphomelib"/' \
  "_esphomelib instance not the API device name"
mutate "$SET" 's/enabled\[1\] == .\\0./1/' \
  "enabled=1x accepted"
mutate "$SET" 's/v\[n\] < .0. \|\| v\[n\] > .9./0/' \
  "non-digit timing accepted"
mutate "$SET" 's/BleProxySettings next = \*stored;/BleProxySettings next = *running;/' \
  "POST compares against the boot copy, not NVS"
mutate "$SET" 's/  if \(running->enabled != stored->enabled\)\n    return true;\n//' \
  "pending on/off flip not reported"

if [ "$fails" -eq 0 ]; then
  echo "all 17 mutations caught; ble_hci + esphome_api + ble_proxy: PASS"
else
  echo "$fails mutation(s) survived"; exit 1
fi
