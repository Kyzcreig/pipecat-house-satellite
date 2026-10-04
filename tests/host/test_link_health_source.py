#!/usr/bin/env python3
"""Source contract: link health telemetry (GADGET-1, t_1910d632).

rssi_dbm + heap levels must be on BOTH surfaces the hub polls (/playback/stats
and /ota/status), the serial LINK_HEALTH line runs every 5 s on the esp_timer
task (no new FreeRTOS task, no audio-path touch), and the feature is
unconditional (telemetry only, no build flag to forget).
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "xiao-esp32-s3" / "src"
CMAKE = (SRC / "CMakeLists.txt").read_text()
OTA = (SRC / "ota.cpp").read_text()
MAIN = (SRC / "main.cpp").read_text()
LH = (SRC / "link_health.cpp").read_text()
LHH = (SRC / "link_health.h").read_text()

# Always built: it is on the unconditional TARGET_SRC line, not a flag block.
target_line = next(l for l in CMAKE.splitlines() if l.startswith("set(TARGET_SRC"))
assert '"link_health.cpp"' in target_line, "link_health.cpp must be unconditional"

# Both polled surfaces carry the fragment.
status = OTA[OTA.index("static esp_err_t ota_status_handler"):OTA.index("static esp_err_t ota_rollback_handler")]
stats = OTA[OTA.index("static esp_err_t playback_stats_handler"):OTA.index("void pipecat_init_ota_server")]
assert "pipecat_link_health_json(" in status, "/ota/status lost link health"
assert "pipecat_link_health_json(" in stats, "/playback/stats lost link health"
# /ota/status body was 640 bytes; the fragment is ~110, so the buffer grew.
m = re.search(r"char body\[(\d+)\]", status)
assert m and int(m.group(1)) >= 768, "/ota/status body too small for the fragment"
# /playback/stats formats the fragment into its own scratch and sizes the malloc for it.
assert "kLinkHealthCapacity" in stats and "+ kLinkHealthCapacity)" in stats

for key in ("rssi_dbm", "heap_free", "heap_min_free", "heap_largest_dma", "psram_free"):
    assert f'\\"{key}\\":' in LH, f"fragment lost {key}"
# Disassociated => null, never a stale or fake dBm.
assert '"null"' in LH and "esp_wifi_sta_get_ap_info" in LH
assert "MALLOC_CAP_DMA" in LH and "heap_caps_get_largest_free_block" in LH

# 5 s serial line on the esp_timer task, started once Wi-Fi + OTA server are up.
assert re.search(r"#define LINK_HEALTH_PERIOD_MS 5000\b", LHH)
assert ".dispatch_method = ESP_TIMER_TASK" in LH and "xTaskCreate" not in LH
assert 'LINK_HEALTH rssi_dbm=' in LH
assert MAIN.index("pipecat_init_ota_server();") < MAIN.index("pipecat_link_health_start();")
print("link health source contract: OK")
