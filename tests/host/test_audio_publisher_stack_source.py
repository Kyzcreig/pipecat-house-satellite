#!/usr/bin/env python3
"""Source contract: codec arm C boot loop (t_6ec97d6b).

2026-10-02: arm C (PIPECAT_UPLINK_64K=1 + PIPECAT_UPLINK_INBAND_FEC=0, CELT)
joined Wi-Fi, offered, and reset before ICE completed, on every boot. Cause:
audio_publisher had a 30000-BYTE stack (ESP-IDF StackType_t is uint8_t) and
runs opus_encode() with USE_ALLOCA. Host-measured on the vendored esp-libopus,
16 kHz stereo, complexity 0, 500 frames:
    SILK (arms A/B)            22,800 B
    CELT (arm C, 128k, FEC 0)  33,360 B   > 30,000 -> overflow on frame 1
Then the OTA check marked the image valid on Wi-Fi+mDNS+HTTP+XVF, BEFORE
WebRTC, so the rollback path never fired.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "xiao-esp32-s3" / "src"
WEBRTC = (SRC / "webrtc.cpp").read_text()
OTA = (SRC / "ota.cpp").read_text()

CELT_STACK_MEASURED = 33360
MIN_HEADROOM = 1.5  # libopus version / complexity / RED-path growth margin

m = re.search(r"#define AUDIO_PUBLISHER_STACK_BYTES \((\d+) \* (\d+)\)", WEBRTC)
assert m, "AUDIO_PUBLISHER_STACK_BYTES must be a named (a * b) byte constant"
stack = int(m.group(1)) * int(m.group(2))
assert stack >= CELT_STACK_MEASURED * MIN_HEADROOM, (
    f"audio_publisher stack {stack} B < {MIN_HEADROOM}x measured CELT encode "
    f"({CELT_STACK_MEASURED} B): codec arm C overflows on its first frame"
)
# Every task-create literal must use the constant, never a bare number again.
create = WEBRTC[WEBRTC.index('"audio_publisher"') - 200:WEBRTC.index('"audio_publisher"') + 200]
assert "AUDIO_PUBLISHER_STACK_BYTES" in create
assert "30000" not in create, "bare 30000 stack literal is back"
assert "AUDIO_PUBLISHER_STACK_BYTES * sizeof(StackType_t)" in WEBRTC

# OTA validation must require the image to have done its job, not just Wi-Fi.
hc = OTA[OTA.index("static bool health_check_passes()"):]
hc = hc[:hc.index("\n}\n")]
for need in ("pipecat_webrtc_connected", "pipecat_webrtc_server_heartbeat_fresh()",
             "OTA_VALIDATION_MIN_UPLINK_FRAMES", "pipecat_uplink_frames_sent()"):
    assert need in hc, f"OTA health check lost {need}"
frames = int(re.search(r"#define OTA_VALIDATION_MIN_UPLINK_FRAMES (\d+)", OTA).group(1))
assert frames >= 50, "need at least 1 s of uplink encodes before marking valid"
timeout = int(re.search(r"#define OTA_ROLLBACK_TIMEOUT_MS (\d+)", OTA).group(1))
assert timeout >= 90000, "pending window must cover boot + join + offer + ICE"

# The counter is advanced by the publisher loop itself (after an encode).
loop = WEBRTC[WEBRTC.index("void pipecat_send_audio_task"):]
loop = loop[:loop.index("\n}\n")]
assert loop.index("pipecat_send_audio(peer_connection);") < loop.index("s_uplink_frames_sent")

# Live headroom readout on /ota/status.
assert '\\"audio_stack_free\\"' in OTA and "pipecat_audio_publisher_stack_free()" in OTA
print("audio_publisher stack + OTA validation source contract: PASS")
