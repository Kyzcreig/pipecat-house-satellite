#!/usr/bin/env python3
"""Source contract for the XMOS LED post-stage tune params (t_61530bca).

Background: the ESP32 scales every ring colour by s_led_brightness/255 in
led_render() and pushes a 48-byte ring payload; the XMOS then applies its OWN
LED_BRIGHTNESS (GPO 20/13, factory 127) and, with LED_GAMMIFY=1 (20/14), a
gamma curve before driving the WS2812s. Measured 2026-10-11 on the bench via
the bedroom camera: with that post-stage at factory, every ESP32 level <= ~30%
rendered at the same floor as 0. Until this change the two XMOS registers were
readable in /xvf/dump but had no write path, so nothing above the firmware
could adjust or even test them.

Invariants:
  1. xmos_led_brightness (20/13, 0..255, default 127) and xmos_led_gammify
     (20/14, 0..1, default 1) are in the tune table as TuneTarget::XVF_UINT8,
     VOLATILE (persistent=false) so the xvf_dsp key-set the golden guard asserts
     is untouched and /xvf/params still lists exactly the DSP keys.
  2. XVF_UINT8 writes ONE byte and reads ONE byte back (the XMOS uint8 wire
     format); it must not fall through to the 4-byte int32 path.
  3. pipecat_xvf_tune() and pipecat_xvf_read_param() both dispatch XVF_UINT8.
  4. No second ESP32-side scaler was added at xvf_write_led_ring (led_render's
     bmax is the one ESP32 scaler; double-scaling would square the curve).
"""
from pathlib import Path
import re

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


# 1. Register ids and table rows.
assert "XVF_CMD_GPO_LED_BRIGHTNESS = 13" in MEDIA
assert "XVF_CMD_GPO_LED_GAMMIFY = 14" in MEDIA
table_start = MEDIA.index("static const TuneEntry kTuneEntries[]")
table = MEDIA[table_start : MEDIA.index("\n};", table_start)]
assert re.search(
    r'\{"xmos_led_brightness",\s*XVF_RESID_GPO,\s*XVF_CMD_GPO_LED_BRIGHTNESS,\s*'
    r"TuneTarget::XVF_UINT8,\s*false,\s*true,\s*0\.0f,\s*255\.0f,\s*127\.0f,\s*false\}",
    table,
), "xmos_led_brightness row missing or not volatile uint8 0..255 default 127"
assert re.search(
    r'\{"xmos_led_gammify",\s*XVF_RESID_GPO,\s*XVF_CMD_GPO_LED_GAMMIFY,\s*'
    r"TuneTarget::XVF_UINT8,\s*false,\s*true,\s*0\.0f,\s*1\.0f,\s*1\.0f,\s*false\}",
    table,
), "xmos_led_gammify row missing or not volatile uint8 0..1 default 1"
assert "XVF_UINT8," in MEDIA[MEDIA.index("enum class TuneTarget") : table_start]

# 2. One-byte wire format.
w = function_body(MEDIA, "static esp_err_t xvf_write_uint8(")
assert "xvf_write_bytes(resid, cmd, &value, 1)" in w, "uint8 write must be 1 byte"
r = function_body(MEDIA, "static esp_err_t xvf_read_uint8(")
assert "xvf_read_bytes(resid, cmd, &byte, 1)" in r, "uint8 read must be 1 byte"

# 3. Dispatch in tune and read.
tune = function_body(MEDIA, "esp_err_t pipecat_xvf_tune(")
assert "is_uint8 = entry->target == TuneTarget::XVF_UINT8" in tune
assert "xvf_write_uint8(entry->resid, entry->cmd" in tune
assert "xvf_read_uint8(entry->resid, entry->cmd, &result->readback)" in tune
# The uint8 branch must be chosen BEFORE the int32 fallthrough.
assert tune.index("xvf_write_uint8(") < tune.index("xvf_write_int32(")
read = function_body(MEDIA, "esp_err_t pipecat_xvf_read_param(")
assert "TuneTarget::XVF_UINT8" in read and "xvf_read_uint8(entry->resid, entry->cmd, readback)" in read

# 4. Exactly one ESP32 scaler: led_render's bmax; the ring writer stays raw.
ring = function_body(MEDIA, "static esp_err_t xvf_write_led_ring(")
assert "s_led_brightness" not in ring, "second scaler at the ring writer = double scaling"
render = function_body(MEDIA, "static void led_render(")
assert "s_led_brightness.load() / 255.0f" in render

print("xmos led post-stage source contract: PASS")
