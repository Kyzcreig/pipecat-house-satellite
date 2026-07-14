#!/usr/bin/env python3
"""Source contract for the ESP32 persistent WebRTC reconnect watchdog."""
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "xiao-esp32-s3" / "src" / "main.cpp").read_text()
HEADER = (ROOT / "xiao-esp32-s3" / "src" / "main.h").read_text()
WEBRTC = (ROOT / "xiao-esp32-s3" / "src" / "webrtc.cpp").read_text()
RTVI = (ROOT / "xiao-esp32-s3" / "src" / "rtvi.cpp").read_text()

assert '#include "reconnect_watchdog.h"' in MAIN
assert "PipecatReconnectWatchdog reconnect_watchdog" in MAIN
assert "reconnect_watchdog.update(" in MAIN
assert "pipecat_webrtc_server_heartbeat_fresh()" in MAIN
assert "ticks_since_boot" not in MAIN

assert "pipecat_webrtc_connected = false;" in WEBRTC
assert "pipecat_webrtc_connected = true;" in WEBRTC
assert "void pipecat_webrtc_note_server_ping()" in WEBRTC
assert "bool pipecat_webrtc_server_heartbeat_fresh()" in WEBRTC
assert "WEBRTC_SERVER_HEARTBEAT_STALE_MS" in WEBRTC

assert "pipecat_webrtc_note_server_ping();" in RTVI
assert "pipecat_webrtc_note_server_ping" in HEADER
assert "pipecat_webrtc_server_heartbeat_fresh" in HEADER

lost_start = WEBRTC.index("if (state == PEER_CONNECTION_DISCONNECTED")
lost_end = WEBRTC.index("} else if (state == PEER_CONNECTION_CONNECTED)", lost_start)
lost_handler = WEBRTC[lost_start:lost_end]
assert "pipecat_webrtc_connected = false;" in lost_handler
assert "esp_restart();" not in lost_handler

print("persistent reconnect watchdog source contract: PASS")
