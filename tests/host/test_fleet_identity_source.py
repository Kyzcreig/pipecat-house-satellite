#!/usr/bin/env python3
"""Source contract: the fleet image is room-agnostic (t_54916498 P4, PRD §5.3).

- CMake no longer invents a satellite id; an unset PIPECAT_SATELLITE_ID stays "".
- The offer carries request_data {mac, fw_sha256, fw_version, boot_count}.
- /ota/status reports mac, offer_url and label; satellite_id stays for the window.
- mDNS/netif hostname comes from pipecat_hostname() (baked name or xvf3800-<mac3>).
- No source file, outside the build template, reads the PIPECAT_SATELLITE_ID
  macro for anything but the migration-window fallback in /ota/status.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FW = ROOT / "xiao-esp32-s3"
CMAKE = (FW / "CMakeLists.txt").read_text()
OTA = (FW / "src" / "ota.cpp").read_text()
HTTP = (FW / "src" / "http.cpp").read_text()
WIFI = (FW / "src" / "wifi.cpp").read_text()

assert 'set(PIPECAT_SATELLITE_ID_VALUE "scratch")' not in CMAKE, "CMake must not invent an id"
assert 'if("${PIPECAT_SATELLITE_ID_VALUE}" STREQUAL "")' not in CMAKE
# mDNS defaults are derived from the id ONLY when one was baked (legacy per-room build).
assert re.search(r'STREQUAL "" AND NOT "\$\{PIPECAT_SATELLITE_ID_VALUE\}" STREQUAL ""', CMAKE)

# request_data on the offer
assert 'cJSON_AddItemToObject(j_offer, "request_data", j_rd)' in HTTP
assert "pipecat_request_data_json(" in HTTP
for arg in ("pipecat_sta_mac_str()", "pipecat_ota_running_sha()", "pipecat_boots_since_poweron()"):
    assert arg in HTTP, f"request_data no longer carries {arg}"
# the hub falls back to ARP-only identity; the offer is still sent if the object fails
assert "request_data omitted" in HTTP

# /ota/status
start = OTA.index("static esp_err_t ota_status_handler")
end = OTA.index("static esp_err_t ota_rollback_handler", start)
status = OTA[start:end]
for key in ("mac", "label", "offer_url", "satellite_id", "mdns_hostname"):
    assert f'\\"{key}\\":' in status, f"/ota/status lost {key}"
assert "pipecat_sta_mac_str()" in status and "PIPECAT_SMALLWEBRTC_URL" in status
assert "pipecat_hostname()" in status
assert 'PIPECAT_SATELLITE_ID, PIPECAT_MDNS_HOSTNAME' not in status

# mDNS + netif hostname come from the runtime identity, not the macro
mdns = OTA[OTA.index("void pipecat_init_mdns()"):]
assert "mdns_hostname_set(host)" in mdns and 'const char *host = pipecat_hostname();' in mdns
assert '{"mac", pipecat_sta_mac_str()}' in mdns
assert 'mdns_hostname_set(PIPECAT_MDNS_HOSTNAME)' not in OTA
assert "esp_wifi_get_mac(WIFI_IF_STA, s_sta_mac)" in WIFI
assert "esp_netif_set_hostname(sta_netif, s_hostname)" in WIFI
assert "esp_netif_set_hostname(sta_netif, PIPECAT_MDNS_HOSTNAME)" not in WIFI
assert "pipecat_mac_hostname(s_sta_mac" in WIFI

# The running sha is cached: the offer path must not hash the partition per dial.
assert "static char s_running_sha[65]" in OTA
assert OTA.count("running_partition_sha(") == 2, "partition hash must have ONE caller (the cache)"

# PIPECAT_SATELLITE_ID macro: only the /ota/status migration-window fallback reads it.
uses = [(p.name, m.start()) for p in (FW / "src").glob("*.cpp") for m in re.finditer(r"PIPECAT_SATELLITE_ID\b", p.read_text())]
assert all(name == "ota.cpp" for name, _ in uses), uses
assert len(uses) == 2, uses  # the two reads of the fallback ternary in ota_status_handler
print("fleet identity source contract: OK")
