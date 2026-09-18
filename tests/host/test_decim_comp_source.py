#!/usr/bin/env python3
"""Source guard for the 48->16 kHz decimator droop compensator (t_1ce88efe).

The failure this exists to catch is an INERT FIX: a filter that compiles, links,
tests green in isolation, and is never actually called on the capture path (or is
called on only one of the two build variants). It also pins the properties the
host C test cannot see from inside the module: wiring, ordering, build gate,
and the cost instrumentation that makes the hot-path claim checkable on the box.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MEDIA = (ROOT / "xiao-esp32-s3/src/media.cpp").read_text()
OTA = (ROOT / "xiao-esp32-s3/src/ota.cpp").read_text()
CMAKE = (ROOT / "xiao-esp32-s3/src/CMakeLists.txt").read_text()
COMP_C = (ROOT / "xiao-esp32-s3/src/decim_comp.c").read_text()


def test_compensator_is_compiled_into_the_firmware() -> None:
    assert '"decim_comp.c"' in CMAKE, "decim_comp.c not in the component SRCS"


def test_compensator_runs_on_the_capture_path_for_both_variants() -> None:
    """Both decimators get it, or the two paths silently diverge."""
    start = MEDIA.index("stereo_48k_32bit_to_stereo_16k(i2s_capture_buffer")
    end = MEDIA.index("// PCM frames are 20ms", start)
    # Strip comment lines: a commented-out call still contains the call text,
    # and a guard that matches commented code cannot see the fix being disabled
    # (that mutation survived the first version of this contract).
    block = "\n".join(
        ln for ln in MEDIA[start:end].splitlines() if not ln.lstrip().startswith("//")
    )
    assert "decim_comp_run_stereo(&s_decim_comp_l, &s_decim_comp_r" in block, (
        "dual-stream capture path does not run the compensator"
    )
    assert "decim_comp_run(&s_decim_comp_l" in block, (
        "mono capture path does not run the compensator"
    )
    # ...and AFTER decimation, which is the whole point (16 kHz, not 48 kHz).
    assert block.index("stereo_48k_32bit_to_mono_16k") < block.index("decim_comp_run")


def test_state_is_initialised_before_the_first_frame() -> None:
    start = MEDIA.index("void pipecat_init_audio_encoder")
    end = MEDIA.index("opus_encoder_ctl", start)
    setup = MEDIA[start : MEDIA.index("static void pipecat_audio", start) if
                  "static void pipecat_audio" in MEDIA[start:] else len(MEDIA)]
    assert "decim_comp_init(&s_decim_comp_l)" in setup
    assert "decim_comp_init(&s_decim_comp_r)" in setup
    assert end > start


def test_build_gate_exists_and_defaults_on() -> None:
    """One rebuild must be able to restore bit-identical pre-change capture."""
    assert "#ifndef PIPECAT_DECIM_COMP" in MEDIA
    assert "#define PIPECAT_DECIM_COMP 1" in MEDIA
    assert "#if PIPECAT_DECIM_COMP" in MEDIA


def test_cost_is_instrumented_and_reported() -> None:
    """The publisher task's budget claim has to be checkable on the device."""
    assert "esp_timer_get_time()" in MEDIA[MEDIA.index("#if PIPECAT_DECIM_COMP\n    // Undo"):][:1200]
    for field in ("decim_comp_last_us", "decim_comp_max_us", "decim_comp_frames",
                  "decim_comp_total_us"):
        assert f'\\"{field}\\"' in OTA, f"/playback/stats does not report {field}"
    assert '\\"decim_comp\\":%u' in OTA, "no on/off flag in /playback/stats"


def test_stats_body_buffer_grew_with_the_new_fields() -> None:
    """The response is snprintf'd into a fixed buffer; new fields must fit."""
    assert "kBodyCapacity = 2400" in OTA


def test_taps_are_q14_unity_and_fit_int16() -> None:
    """Q15 would put the centre tap at 37782 — outside int16. Pin the format."""
    line = [ln for ln in COMP_C.splitlines() if ln.strip().startswith("-197,")][0]
    taps = [int(t) for t in line.strip().rstrip("};").split(",")]
    assert len(taps) == 7
    assert sum(taps) == 16384, "taps are not exactly unity gain in Q14"
    assert all(-32768 <= t <= 32767 for t in taps)
    assert taps == taps[::-1], "taps are not symmetric (non-linear phase)"
    assert ">> 14" in COMP_C, "accumulator shift does not match the Q14 taps"


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print(f"ok - {name}")
    print("decim_comp source contract: PASS")
