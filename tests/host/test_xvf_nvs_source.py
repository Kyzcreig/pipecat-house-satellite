#!/usr/bin/env python3
"""Source contract for NVS-persistent XVF DSP tuning."""
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "xiao-esp32-s3" / "src" / "main.cpp").read_text()
MEDIA = (ROOT / "xiao-esp32-s3" / "src" / "media.cpp").read_text()
OTA = (ROOT / "xiao-esp32-s3" / "src" / "ota.cpp").read_text()


def function_body(source: str, signature: str) -> str:
    """Return one C/C++ function body using balanced braces."""
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

PERSISTENT_PARAMS = {
    "asr_gain",
    "dtsensitive",
    "agc_maxgain",
    "agc_desired",
    "min_nn",
    "min_ns",
    "dac_atten",
}
table_start = MEDIA.index("static const TuneEntry kTuneEntries[]")
table_end = MEDIA.index("\n};", table_start)
table = MEDIA[table_start:table_end]
persistent_in_table = set()
for match in re.finditer(r'\{"([^"]+)",(.*?)\},', table, re.DOTALL):
    flags = re.search(
        r"TuneTarget::[A-Z0-9_]+,\s*(true|false),\s*(true|false),",
        match.group(2),
    )
    assert flags, f"cannot parse tune-table flags for {match.group(1)}"
    if flags.group(1) == "true":
        persistent_in_table.add(match.group(1))
assert persistent_in_table == PERSISTENT_PARAMS

# One table owns the allowlist, value range, typed write/readback, and replay defaults.
assert "bool persistent;" in MEDIA
assert "pipecat_xvf_persistent_param_count" in MEDIA
assert "pipecat_xvf_persistent_param_name" in MEDIA
assert "pipecat_xvf_param_default" in MEDIA
assert "xvf_read_scalar" in MEDIA
assert "clamp_dtsensitive" in MEDIA
assert "1.0e-8f" in MEDIA  # PP_AGCDESIREDLEVEL documented floor
assert "1000.0f" in MEDIA  # ASROUTGAIN / PP_AGCMAXGAIN documented ceiling
tune_body = function_body(MEDIA, "esp_err_t pipecat_xvf_tune(")
assert tune_body.index("xvf_write_float") < tune_body.index("xvf_read_scalar")
assert "result->readback_valid = true" in tune_body
assert "result->applied = fabsf(result->readback - applied_value)" in tune_body
assert "ret = ESP_ERR_INVALID_RESPONSE" in tune_body

# Successful verified writes persist the normalized value unless explicitly disabled.
assert '#define DSP_NVS_NAMESPACE "xvf_dsp"' in OTA
save_body = function_body(OTA, "static esp_err_t save_dsp_param(")
assert save_body.index("nvs_set_blob") < save_body.index("nvs_commit")
tune_handler = function_body(OTA, "static esp_err_t xvf_tune_handler(")
assert 'httpd_query_key_value(query, "persist"' in tune_handler
assert 'strcmp(persist_s, "0") != 0' in tune_handler
tune_handler_tokens = re.sub(r"\s+", " ", tune_handler)
assert (
    "if (ret == ESP_OK && result.applied && persist && "
    "pipecat_xvf_param_persistent(param)) { "
    "ret = save_dsp_param(param, result.applied_value);"
) in tune_handler_tokens
assert r'\"persisted\"' in tune_handler

# Boot replay is non-fatal, uses the same tune/readback path, and precedes WebRTC.
replay_body = function_body(OTA, "void pipecat_replay_xvf_params()")
assert "pipecat_xvf_tune(param, stored_value, &result)" in replay_body
assert "restore_dsp_default(param)" in replay_body
restore_body = function_body(OTA, "static void restore_dsp_default(")
assert "pipecat_xvf_param_default(param, &default_value)" in restore_body
assert '"nvs_dsp: %lu params applied"' in replay_body
assert MAIN.index("pipecat_init_audio_capture();") < MAIN.index(
    "pipecat_replay_xvf_params();"
) < MAIN.index("pipecat_init_webrtc();")

# Introspection and reset expose only stored truth, not baked assumptions.
params_body = function_body(OTA, "static esp_err_t xvf_params_handler(")
assert 'httpd_query_key_value(query, "reset"' in params_body
assert "clear_dsp_params()" in params_body
assert "load_dsp_param(nvs, param, &value)" in params_body
clear_body = function_body(OTA, "static esp_err_t clear_dsp_params(")
assert clear_body.index("nvs_erase_all") < clear_body.index("nvs_commit")
server_body = function_body(OTA, "void pipecat_init_ota_server()")
assert '.uri = "/xvf/params"' in server_body
assert ".method = HTTP_GET" in server_body
assert "httpd_register_uri_handler(g_ota_server, &params_uri)" in server_body
registered_handlers = server_body.count("httpd_register_uri_handler(")
assert registered_handlers == 10
assert f"config.max_uri_handlers = {registered_handlers}" in server_body

print("xvf nvs source contract: PASS")
