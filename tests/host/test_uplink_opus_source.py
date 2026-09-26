#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MEDIA_SOURCES = [
    ROOT / "xiao-esp32-s3/src/media.cpp",
    ROOT / "esp32-s3-box-3/src/media.cpp",
    ROOT / "esp32-m5stack-cores3/src/media.cpp",
    ROOT / "esp32-m5stack-atoms3r/src/media.cpp",
]

for path in MEDIA_SOURCES:
    source = path.read_text()
    assert "OPUS_SET_INBAND_FEC(1)" in source, f"missing in-band FEC: {path}"
    assert "OPUS_SET_PACKET_LOSS_PERC(OPUS_EXPECTED_PACKET_LOSS_PCT)" in source, (
        f"missing packet-loss hint: {path}"
    )
    assert "OPUS_EXPECTED_PACKET_LOSS_PCT" in source, f"missing named loss hint: {path}"

xiao = MEDIA_SOURCES[0].read_text()
branch_end = xiao.index("opus_encoder_ctl(opus_encoder, OPUS_SET_COMPLEXITY")
branch = xiao[xiao.index("#if PIPECAT_DUAL_STREAM", xiao.index("void pipecat_init_audio_encoder")):branch_end]
assert "OPUS_SET_BITRATE(OPUS_ENCODER_BITRATE * 2)" in branch
assert "OPUS_SET_BITRATE(OPUS_ENCODER_BITRATE)" in branch
# Loss controls must sit after the mono/dual bitrate branch so one encoder setup
# applies identically to both build variants.
loss_offset = xiao.index("OPUS_SET_INBAND_FEC(1)")
assert loss_offset > branch_end
print("uplink opus encoder source contract: PASS")
