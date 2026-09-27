#!/usr/bin/env python3
"""Source contract for the runtime LED-ring brightness knob (t_1819da10).

Invariants this pins:
  1. No LED render path reads the compile-time PIPECAT_LED_BRIGHTNESS directly;
     every bmax/b derives from the runtime atomic s_led_brightness.
  2. led_brightness is in the /xvf/tune table, range 0..255, NOT in the
     xvf_dsp persistent set (own NVS namespace "led"), so the golden guard's
     exact-key-set check on /xvf/params is untouched by day/night schedules.
  3. The persisted level is loaded BEFORE the boot splash runs.
  4. /xvf/tune honors persist=0 for it (self-persist hook sits under `persist`).
  5. /playback/stats exposes the live value.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
MEDIA = (ROOT / "xiao-esp32-s3" / "src" / "media.cpp").read_text()
OTA = (ROOT / "xiao-esp32-s3" / "src" / "ota.cpp").read_text()
MAIN_H = (ROOT / "xiao-esp32-s3" / "src" / "main.h").read_text()


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


# 1. The macro is only the baked default of the atomic + the table default.
macro_uses = [
    line for line in MEDIA.splitlines()
    if "PIPECAT_LED_BRIGHTNESS" in line and not line.strip().startswith("//")
]
for line in macro_uses:
    assert (
        "#ifndef" in line or "#define" in line
        or "s_led_brightness{PIPECAT_LED_BRIGHTNESS}" in line
        or "static_cast<float>(PIPECAT_LED_BRIGHTNESS)" in line
    ), f"LED render path still reads the compile-time constant: {line.strip()}"
assert "PIPECAT_LED_BRIGHTNESS / 255.0f" not in MEDIA
assert "static std::atomic<uint8_t> s_led_brightness{PIPECAT_LED_BRIGHTNESS};" in MEDIA
render = function_body(MEDIA, "static void led_render(")
assert "s_led_brightness.load() / 255.0f" in render
splash_start = MEDIA.index("#if PIPECAT_LED_ENABLE && PIPECAT_LED_BOOT_SPLASH")
splash = MEDIA[splash_start : MEDIA.index("#endif", splash_start)]
assert "s_led_brightness.load() / 255.0f" in splash

# 2. Tune-table entry: own target, non-persistent in xvf_dsp, range 0..255.
table_start = MEDIA.index("static const TuneEntry kTuneEntries[]")
table = MEDIA[table_start : MEDIA.index("\n};", table_start)]
assert re.search(
    r'\{"led_brightness",\s*0,\s*0,\s*TuneTarget::LED_BRIGHTNESS,\s*false,\s*true,'
    r"\s*0\.0f,\s*255\.0f,\s*static_cast<float>\(PIPECAT_LED_BRIGHTNESS\),\s*false\}",
    table,
), "led_brightness tune entry missing or mis-flagged"
assert '#define LED_NVS_NAMESPACE "led"' in MEDIA
assert 'DSP_NVS_NAMESPACE "xvf_dsp"' in OTA
save = function_body(MEDIA, "static esp_err_t led_brightness_save(")
assert "nvs_open(LED_NVS_NAMESPACE" in save
assert save.index("nvs_set_u8") < save.index("nvs_commit")

# Tune applies to the live atomic and reads back from it.
tune = function_body(MEDIA, "esp_err_t pipecat_xvf_tune(")
led_branch = tune[tune.index("TuneTarget::LED_BRIGHTNESS") :]
assert "s_led_brightness.store(level)" in led_branch
assert "result->applied = true" in led_branch
read = function_body(MEDIA, "esp_err_t pipecat_xvf_read_param(")
assert "TuneTarget::LED_BRIGHTNESS" in read and "s_led_brightness.load()" in read

# 3. Load precedes the splash (init_i2c_and_codec runs the DSP profile + splash).
init = function_body(MEDIA, "void pipecat_init_audio_capture()")
assert init.index("led_brightness_load();") < init.index("init_i2c_and_codec();")
load = function_body(MEDIA, "static void led_brightness_load(")
assert "nvs_get_u8" in load and "s_led_brightness.store(stored)" in load

# 4. HTTP handler: self-persist only under `persist` (persist=0 stays volatile).
handler = re.sub(r"\s+", " ", function_body(OTA, "static esp_err_t xvf_tune_handler("))
assert (
    "else if (ret == ESP_OK && result.applied && persist) { "
    "// Self-persisting params (led_brightness) store outside xvf_dsp. "
    "esp_err_t self_ret = ESP_OK; "
    "if (pipecat_xvf_param_self_persist(param, result.applied_value, &self_ret)) {"
) in handler
assert "pipecat_xvf_param_self_persist" in MAIN_H
assert "pipecat_led_brightness" in MAIN_H

# 5. /playback/stats exposes it.
stats = function_body(OTA, "static esp_err_t playback_stats_handler(")
assert r'\"led_brightness\":%u' in stats
assert "(unsigned)pipecat_led_brightness()" in stats

print("led brightness source contract: PASS")
