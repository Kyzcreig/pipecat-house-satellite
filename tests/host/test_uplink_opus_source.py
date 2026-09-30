#!/usr/bin/env python3
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MEDIA_SOURCES = [
    ROOT / "xiao-esp32-s3/src/media.cpp",
    ROOT / "esp32-s3-box-3/src/media.cpp",
    ROOT / "esp32-m5stack-cores3/src/media.cpp",
    ROOT / "esp32-m5stack-atoms3r/src/media.cpp",
]

# Non-XVF boards keep the literal FEC-on contract.
for path in MEDIA_SOURCES[1:]:
    source = path.read_text()
    assert "OPUS_SET_INBAND_FEC(1)" in source, f"missing in-band FEC: {path}"
    assert "OPUS_SET_PACKET_LOSS_PERC(OPUS_EXPECTED_PACKET_LOSS_PCT)" in source, (
        f"missing packet-loss hint: {path}"
    )
    assert "OPUS_EXPECTED_PACKET_LOSS_PCT" in source, f"missing named loss hint: {path}"

# XVF3800 (xiao) board: in-band FEC is a build-time ARM (t_109a4778) because
# INBAND_FEC(1) pins the encoder to SILK. The contract is:
#   * default (PIPECAT_UPLINK_INBAND_FEC unset) == live behaviour: FEC on, plp 10;
#   * PIPECAT_UPLINK_INBAND_FEC=0 is the only way to reach FEC off / plp 0;
#   * the ctl calls consume the resolved macros, never a literal.
xiao = MEDIA_SOURCES[0].read_text()
assert "OPUS_EXPECTED_PACKET_LOSS_PCT" in xiao, "missing named loss hint (xiao)"
assert re.search(
    r"#ifndef PIPECAT_UPLINK_INBAND_FEC\s*\n#define PIPECAT_UPLINK_INBAND_FEC 1\s*\n#endif",
    xiao,
), "xiao: PIPECAT_UPLINK_INBAND_FEC must default to 1 (live behaviour)"
on_branch = re.search(
    r"#if PIPECAT_UPLINK_INBAND_FEC\s*\n"
    r"#define OPUS_UPLINK_FEC_ENABLE 1\s*\n"
    r"#define OPUS_UPLINK_PLP OPUS_EXPECTED_PACKET_LOSS_PCT\s*\n"
    r"#else\s*\n"
    r"#define OPUS_UPLINK_FEC_ENABLE 0\s*\n"
    r"#define OPUS_UPLINK_PLP 0\s*\n"
    r"#endif",
    xiao,
)
assert on_branch, "xiao: FEC arm macros must resolve to (1, PCT) / (0, 0)"
assert "OPUS_SET_INBAND_FEC(OPUS_UPLINK_FEC_ENABLE)" in xiao, "xiao: FEC ctl must use the arm macro"
assert "OPUS_SET_PACKET_LOSS_PERC(OPUS_UPLINK_PLP)" in xiao, "xiao: plp ctl must use the arm macro"
assert "opus_encoder_ctl(opus_encoder, OPUS_SET_INBAND_FEC(1))" not in xiao and (
    "opus_encoder_ctl(opus_encoder, OPUS_SET_INBAND_FEC(0))" not in xiao
), "xiao: no literal FEC ctl — the arm must be the single source of truth"
# The CMake gate must exist and only ever ADD the =0 define (never =1), so an
# unset env cannot flip the default.
cmake = (ROOT / "xiao-esp32-s3/CMakeLists.txt").read_text()
assert re.search(
    r'if\("\$ENV\{PIPECAT_UPLINK_INBAND_FEC\}" STREQUAL "0"\)\s*\n'
    r"\s*add_compile_definitions\(PIPECAT_UPLINK_INBAND_FEC=0\)",
    cmake,
), "xiao CMake: PIPECAT_UPLINK_INBAND_FEC=0 gate missing"
assert "PIPECAT_UPLINK_INBAND_FEC=1" not in cmake, "xiao CMake: never force-define =1"

branch_end = xiao.index("opus_encoder_ctl(opus_encoder, OPUS_SET_COMPLEXITY")
branch = xiao[xiao.index("#if PIPECAT_DUAL_STREAM", xiao.index("void pipecat_init_audio_encoder")):branch_end]
assert "OPUS_SET_BITRATE(OPUS_ENCODER_BITRATE * 2)" in branch
assert "OPUS_SET_BITRATE(OPUS_ENCODER_BITRATE)" in branch
# Loss controls must sit after the mono/dual bitrate branch so one encoder setup
# applies identically to both build variants.
loss_offset = xiao.index("OPUS_SET_INBAND_FEC(OPUS_UPLINK_FEC_ENABLE)")
assert loss_offset > branch_end

# ---------------------------------------------------------------------------
# CELT-ARM PREMISE GUARD (t_109a4778). Measured 2026-09-18 on 12 real far-field
# captures with the vendored esp-libopus and libopus 1.6.1: the CELT unlock is
# a JOINT condition, not the FEC flag alone —
#     mode = (equiv_rate >= threshold) ? CELT : SILK   [src/opus_encoder.c]
#     if (useInBandFEC && packetLossPercentage > (128-voice_est)>>4) mode = SILK
# With OPUS_SIGNAL_VOICE (voice_est=127) the second predicate is `plp > 0`, and
# the first flips between 30k/lane (SILK 100%) and 40k/lane (CELT 100%).
#
# So a future bitrate reduction below ~40k/lane would put the uplink back on
# SILK with FEC OFF: the worse codec mode AND no in-band FEC — strictly worse
# than either shipped arm, with NO compile error and NO log change to notice it
# by. This guard makes that regression impossible to land silently.
CELT_MIN_BITRATE_PER_LANE = 40000
m = re.search(r"#define OPUS_ENCODER_BITRATE (\d+)", xiao)
assert m, "xiao: OPUS_ENCODER_BITRATE not found"
bitrate = int(m.group(1))
if "PIPECAT_UPLINK_INBAND_FEC=0" in cmake:  # the CELT arm exists in this tree
    assert bitrate >= CELT_MIN_BITRATE_PER_LANE, (
        f"xiao: OPUS_ENCODER_BITRATE={bitrate} is below the measured CELT-mode "
        f"threshold ({CELT_MIN_BITRATE_PER_LANE}/lane). With FEC off, libopus "
        f"falls back to SILK — worse mode AND no in-band FEC. Either raise the "
        f"bitrate or delete the PIPECAT_UPLINK_INBAND_FEC arm."
    )
assert "OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE)" in xiao, (
    "xiao: the plp>0 SILK-lock predicate is (128-voice_est)>>4; SIGNAL_VOICE "
    "pins voice_est=127 so the predicate is exactly plp>0. Changing the signal "
    "hint moves the threshold and invalidates the measured arm boundary."
)
print("uplink opus encoder source contract: PASS")
