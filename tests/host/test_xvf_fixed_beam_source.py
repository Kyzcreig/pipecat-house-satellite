#!/usr/bin/env python3
"""Source contract: fixed-beam tune params (t_a6061357, paired-capture #8).

The hub applies FIXED_BEAM_<room> at connect through POST /xvf/tune, so the
firmware surface must stay exactly: three VOLATILE allowlist entries on the
XMOS fixed-beam registers, the azimuth pair written read-modify-write per
slot with a per-slot readback, and the baked boot profile still forcing
auto beams (fail-safe on reboot, the kill switch of last resort).
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MEDIA = (ROOT / "xiao-esp32-s3" / "src" / "media.cpp").read_text()


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1 : pos]
    raise AssertionError(f"unterminated function: {signature}")


def tune_table() -> str:
    start = MEDIA.index("static const TuneEntry kTuneEntries[]")
    return MEDIA[start : MEDIA.index("\n};", start)]


def test_registers_match_the_xmos_control_map() -> None:
    assert "XVF_CMD_AEC_FIXEDBEAMSONOFF = 37" in MEDIA
    assert "XVF_CMD_AEC_FIXEDBEAMSAZIMUTH_VALUES = 81" in MEDIA
    assert "XVF_RESID_AEC = 33" in MEDIA


def test_three_volatile_entries_with_ranges() -> None:
    table = tune_table()
    onoff = re.search(
        r'\{"fixed_beams_onoff",\s*XVF_RESID_AEC,\s*XVF_CMD_AEC_FIXEDBEAMSONOFF,\s*'
        r"TuneTarget::XVF_INT32,\s*false,\s*true,\s*0\.0f,\s*1\.0f,\s*0\.0f,\s*false\}",
        table,
    )
    assert onoff, "fixed_beams_onoff must be a volatile int32 0..1 defaulting to 0 (auto)"
    for name, slot in (("fixed_beam_az1", 0), ("fixed_beam_az2", 1)):
        entry = re.search(
            rf'\{{"{name}",\s*XVF_RESID_AEC,\s*XVF_CMD_AEC_FIXEDBEAMSAZIMUTH_VALUES,\s*'
            rf"TuneTarget::XVF_FLOAT_PAIR_SLOT,\s*false,\s*true,\s*0\.0f,\s*6\.2831855f,"
            rf"\s*0\.0f,\s*false,\s*{slot}\}}",
            table,
        )
        assert entry, f"{name}: volatile float pair slot {slot}, range 0..2pi rad"


def test_pair_slot_is_read_modify_write_with_slot_readback() -> None:
    write = function_body(MEDIA, "static esp_err_t xvf_write_float_pair_slot(")
    assert "xvf_read_floats(resid, cmd, pair, 2)" in write
    assert "pair[slot] = value;" in write
    assert "xvf_write_bytes(resid, cmd, payload, sizeof(payload))" in write
    read = function_body(MEDIA, "static esp_err_t xvf_read_float_pair_slot(")
    assert "xvf_read_floats(resid, cmd, pair, 2)" in read
    assert "*value = pair[slot];" in read

    tune = re.sub(r"\s+", " ", function_body(MEDIA, "esp_err_t pipecat_xvf_tune("))
    assert "entry->target == TuneTarget::XVF_FLOAT_PAIR_SLOT" in tune
    assert "xvf_write_float_pair_slot(entry->resid, entry->cmd, entry->pair_slot, applied_value)" in tune
    assert "xvf_read_float_pair_slot(entry->resid, entry->cmd, entry->pair_slot, &result->readback)" in tune
    # applied==true still requires readback agreement; no ack-only shortcut.
    assert "result->applied = fabsf(result->readback - applied_value) <= 0.0001f;" in tune

    read_param = re.sub(r"\s+", " ", function_body(MEDIA, "esp_err_t pipecat_xvf_read_param("))
    assert "xvf_read_float_pair_slot(entry->resid, entry->cmd, entry->pair_slot, readback)" in read_param

    normalize = re.sub(r"\s+", " ", function_body(MEDIA, "static bool normalize_tune_value("))
    assert (
        "entry.target != TuneTarget::XVF_FLOAT && entry.target != TuneTarget::XVF_FLOAT_PAIR_SLOT"
        in normalize
    ), "a radian azimuth must not be int-truncated"


def test_boot_profile_still_forces_auto_beams() -> None:
    boot = function_body(MEDIA, "static void configure_xvf3800_dsp_profile()")
    assert "xvf_write_int32(XVF_RESID_AEC, XVF_CMD_AEC_FIXEDBEAMSONOFF, 0)" in boot
    assert "FIXEDBEAMSAZIMUTH" not in boot, "boot must not bake an aim; the hub owns it"


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print(f"ok - {name}")
    print("xvf fixed-beam source contract: PASS")
