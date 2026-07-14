#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "xiao-esp32-s3/src/webrtc.cpp").read_text()
start = source.index("void pipecat_send_audio_task")
loop = source[start : source.index("}", source.index("while (1)", start)) + 1]

# Production capture is paced by the blocking 20 ms I2S read. Sleeping another
# 15 ms after processing accumulates work time and under-produces RTP media.
assert "TICK_INTERVAL" not in loop
assert "taskYIELD()" in loop
# The synthetic bench path has no blocking I2S producer, so it still needs an
# explicit one-frame cadence.
assert "PIPECAT_BENCH_SEND_TONE" in loop
assert "pdMS_TO_TICKS(20)" in loop
print("uplink capture pacing source contract: PASS")
