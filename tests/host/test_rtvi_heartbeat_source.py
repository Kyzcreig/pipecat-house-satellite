#!/usr/bin/env python3
"""Source contract for the ESP32 half of WebRTC peer liveness."""
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "xiao-esp32-s3" / "src" / "rtvi.cpp"
WEBRTC_SOURCE = ROOT / "xiao-esp32-s3" / "src" / "webrtc.cpp"
text = SOURCE.read_text()
webrtc_text = WEBRTC_SOURCE.read_text()

assert 'hash(j_t->valuestring) == hash("ping")' in text
assert 'create_rtvi_message("client-message")' in text
assert 'cJSON_AddObjectToObject(pong->msg, "data")' in text
assert 'cJSON_AddStringToObject(pong_data, "t", "pong")' in text
assert 'cJSON_AddNumberToObject(pong_data, "nonce", j_nonce->valuedouble)' in text
assert "peer_connection_datachannel_send_sid(peer_connection, pong_str, strlen(pong_str), msg->sid)" in text
assert "uint16_t sid;" in text
assert "pipecat_rtvi_handle_message(msg, sid)" in webrtc_text
print("rtvi heartbeat source contract: PASS")
