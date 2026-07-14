#!/usr/bin/env python3
"""Source contract for the ESP32 half of WebRTC peer liveness."""
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "xiao-esp32-s3" / "src" / "rtvi.cpp"
WEBRTC_SOURCE = ROOT / "xiao-esp32-s3" / "src" / "webrtc.cpp"
text = SOURCE.read_text()
webrtc_text = WEBRTC_SOURCE.read_text()

assert 'strcmp(j_t->valuestring, "ping") == 0' in text
assert 'bool pipecat_rtvi_handle_heartbeat(const char *msg, uint16_t sid)' in text
assert 'create_rtvi_message("client-message")' in text
assert 'cJSON_AddObjectToObject(pong->msg, "data")' in text
assert 'cJSON_AddStringToObject(pong_data, "t", "pong")' in text
assert 'cJSON_AddNumberToObject(pong_data, "nonce", j_nonce->valuedouble)' in text
assert "peer_connection_datachannel_send_sid(" in text
assert "peer_connection, pending_pong, pending_pong_len, pending_pong_sid" in text
assert "void pipecat_rtvi_send_pending_heartbeat()" in text
assert "pending_pong_ready = true" in text
heartbeat_call = "if (pipecat_rtvi_handle_heartbeat(msg, sid)) return;"
regular_call = "pipecat_rtvi_handle_message(msg);"
assert heartbeat_call in webrtc_text
assert regular_call in webrtc_text
assert webrtc_text.index(heartbeat_call) < webrtc_text.index(regular_call)
loop_call = "peer_connection_loop(peer_connection);"
send_call = "pipecat_rtvi_send_pending_heartbeat();"
assert send_call in webrtc_text
assert webrtc_text.index(loop_call) < webrtc_text.index(send_call)
print("rtvi heartbeat source contract: PASS")
