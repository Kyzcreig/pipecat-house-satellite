#!/usr/bin/env python3
"""Source contract for the ESP32 half of WebRTC peer liveness."""
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "xiao-esp32-s3" / "src" / "rtvi.cpp"
text = SOURCE.read_text()

assert 'hash(j_t->valuestring) == hash("ping")' in text
assert 'cJSON_AddStringToObject(pong, "t", "pong")' in text
assert 'cJSON_AddNumberToObject(pong, "nonce", j_nonce->valuedouble)' in text
assert "peer_connection_datachannel_send(peer_connection, pong_str, strlen(pong_str))" in text
print("rtvi heartbeat source contract: PASS")
