#!/usr/bin/env python3
"""Source contract: downlink media is a sign of server life (t_56a17737).

The kitchen satellite rebooted mid-TTS twice on 2026-10-04 because the
reconnect watchdog judged the hub dead off ONE server ping that arrived >5 s
late (35 s window on a 30 s ping interval) while RTP flowed both ways. The
media note must sit in the libpeer onaudiotrack callback (every downlink RTP
packet), the oracle must be the shared header, and /ota/status must expose
the gap telemetry the bench soak reads.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "xiao-esp32-s3" / "src"
WEBRTC = (SRC / "webrtc.cpp").read_text()
HEADER = (SRC / "main.h").read_text()
OTA = (SRC / "ota.cpp").read_text()
LIVENESS = (SRC / "server_liveness.h").read_text()

assert '#include "server_liveness.h"' in WEBRTC
assert "static PipecatServerLiveness s_server_liveness(" in WEBRTC
assert "WEBRTC_SERVER_HEARTBEAT_STALE_MS = 35000" in WEBRTC
assert "s_server_liveness.fresh(" in WEBRTC
assert "s_server_liveness.note_ping(" in WEBRTC
assert "void pipecat_webrtc_note_server_media()" in WEBRTC
assert "pipecat_webrtc_note_server_media" in HEADER

# The media note rides the audio-track callback, before decode.
track_start = WEBRTC.index(".onaudiotrack = [](uint8_t *data, size_t size")
track_end = WEBRTC.index("},", track_start)
track = WEBRTC[track_start:track_end]
assert "pipecat_webrtc_note_server_media();" in track
assert track.index("pipecat_webrtc_note_server_media();") < track.index(
    "pipecat_audio_decode(data, size);"
)

# Never-pinged stays "not fresh" (first-connection watchdog semantics).
assert "if (ping == 0)\n      return false;" in LIVENESS

# Telemetry on /ota/status.
for key in (
    "server_ping_rx",
    "server_ping_gap_max_ms",
    "server_ping_age_ms",
    "media_liveness_holds",
):
    assert f'\\"{key}\\":%lu' in OTA, key
assert "pipecat_webrtc_server_liveness_stats(" in OTA

print("server liveness source contract: PASS")
