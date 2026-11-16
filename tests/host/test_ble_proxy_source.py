#!/usr/bin/env python3
"""Source contract: the BLE scanner costs nothing unless it is switched on.

t_6215e36e. With PIPECAT_BLE_PROXY=1 compiled in, the toggle is NVS
ble_proxy/enabled, default off. OFF must mean: the Bluetooth controller is
never initialised (it shares the one 2.4 GHz radio with the voice Wi-Fi link),
no task, no socket, and its static RAM is released. ON scans passively.
Run by tests/host/run_ble_proxy_tests.sh (bare python3, no pytest).
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "xiao-esp32-s3" / "src"
PROXY = (SRC / "ble_proxy.cpp").read_text()
MAIN = (SRC / "main.cpp").read_text()
OTA = (SRC / "ota.cpp").read_text()


def start_body() -> str:
    i = PROXY.index("void pipecat_ble_proxy_start() {")
    return PROXY[i : PROXY.index("\n}\n", i)]


def test_off_returns_before_any_radio_work() -> None:
    body = start_body()
    off = re.search(r"if \(!s_enabled\) \{(.*?)\n    return;\n  \}", body, re.S)
    assert off, "start: no early return on !s_enabled"
    assert "esp_bt_controller_mem_release(ESP_BT_MODE_BLE)" in off.group(1)
    gate = off.end()
    for call in ("esp_bt_controller_init", "esp_bt_controller_enable",
                 "xTaskCreatePinnedToCore", "advertise_mdns", "xQueueCreate"):
        pos = body.find(call)
        assert pos > gate, f"{call} reachable before the enabled gate"
    assert PROXY.count("esp_bt_controller_init(") == 1


def test_default_is_off_and_only_1_turns_it_on() -> None:
    assert "static bool s_enabled = false;" in PROXY
    assert "s_enabled = en == 1;" in PROXY


def test_scan_is_passive_at_the_esphome_default_timing() -> None:
    assert "ble_hci_cmd_le_set_scan_params(buf, 1, iv, wn)" in PROXY
    assert "kDefaultIntervalMs = 320;" in PROXY
    assert "kDefaultWindowMs = 30;" in PROXY


def test_wiring_is_behind_the_build_flag() -> None:
    m = re.search(r"#if PIPECAT_BLE_PROXY\n(.*?)#endif", MAIN[MAIN.index("pipecat_init_webrtc();"):], re.S)
    assert m and "pipecat_ble_proxy_start();" in m.group(1)
    assert MAIN.count("pipecat_ble_proxy_start") == 1
    reg = OTA.index("pipecat_ble_proxy_register_http(g_ota_server)")
    assert OTA.rfind("#if PIPECAT_BLE_PROXY", 0, reg) > OTA.rfind("#endif", 0, reg)
    assert re.search(r"#if PIPECAT_BLE_PROXY\n  config.max_uri_handlers \+= 2;", OTA)


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
    print("ble_proxy default-off source contract: PASS")
