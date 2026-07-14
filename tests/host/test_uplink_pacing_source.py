#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "xiao-esp32-s3/src/webrtc.cpp").read_text()
start = source.index("void pipecat_send_audio_task")
loop = source[start : source.index("}", source.index("while (1)", start)) + 1]

# Pace to absolute 20 ms deadlines. A fixed post-processing delay accumulates
# work time and under-produces RTP media; a tight yield loop can starve the
# lower-priority peer/data-channel task when I2S has buffered samples ready.
assert "TICK_INTERVAL" not in loop
assert "vTaskDelayUntil" in loop
assert "taskYIELD()" not in loop
assert "pdMS_TO_TICKS(20)" in loop
print("uplink capture pacing source contract: PASS")
