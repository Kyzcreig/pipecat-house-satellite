#!/usr/bin/env python3
"""Source contract for phase-independent XVF beam telemetry."""
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
MAIN_H = (ROOT / "xiao-esp32-s3" / "src" / "main.h").read_text()
MEDIA = (ROOT / "xiao-esp32-s3" / "src" / "media.cpp").read_text()
OTA = (ROOT / "xiao-esp32-s3" / "src" / "ota.cpp").read_text()
BEAM_JSON = (ROOT / "xiao-esp32-s3" / "src" / "xvf_beam_json.cpp").read_text()
BEAM_H = (ROOT / "xiao-esp32-s3" / "src" / "xvf_beam_json.h").read_text()


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


# The public sample contract preserves the documented 4/2/4 float shapes.
assert "struct PipecatXvfBeamTelemetry" in BEAM_H
assert "float azimuth[4];" in BEAM_H
assert "float selected_azimuth[2];" in BEAM_H
assert "float spenergy[4];" in BEAM_H
assert "pipecat_xvf_read_beam" in MAIN_H

# Every HTTP poll performs all three device reads, independent of LED/assistant phase.
assert "XVF_CMD_AUDIO_MGR_SELECTED_AZIMUTHS = 11" in MEDIA
read_body = function_body(MEDIA, "esp_err_t pipecat_xvf_read_beam(")
read_tokens = re.sub(r"\s+", " ", read_body)
presence_guard = "if (!xvf3800_present || !xvf_beam_telemetry_supported)"
assert presence_guard in read_tokens
assert read_tokens.index(presence_guard) < read_tokens.index("xvf_read_floats(")
required_reads = [
    "xvf_read_floats( XVF_RESID_AEC, XVF_CMD_AEC_AZIMUTH_VALUES, telemetry->azimuth, 4)",
    "xvf_read_floats( XVF_RESID_AUDIO_MGR, XVF_CMD_AUDIO_MGR_SELECTED_AZIMUTHS, telemetry->selected_azimuth, 2)",
    "xvf_read_floats( XVF_RESID_AEC, XVF_CMD_AEC_SPENERGY_VALUES, telemetry->spenergy, 4)",
]
for read in required_reads:
    assert read in read_tokens
first_error_check = read_tokens.index("if (azimuth_ret != ESP_OK)")
for read in required_reads:
    assert read_tokens.index(read) < first_error_check

# The existing bounded control retry covers WAIT and silence-time status 0x40.
control_body = function_body(MEDIA, "static esp_err_t xvf_read_bytes(")
assert "attempt < XVF_CONTROL_RETRIES" in control_body
assert "status != XVF_CTRL_WAIT && status != XVF_SERVICER_COMMAND_RETRY" in control_body
assert "XVF_SERVICER_COMMAND_RETRY = 0x40" in MEDIA

# The endpoint emits valid JSON (NaN selected azimuth becomes null) or a 503 error.
append_body = function_body(BEAM_JSON, "static bool append_json_float(")
assert "isfinite(value)" in append_body
assert '"null%s"' in append_body
serializer_body = function_body(BEAM_JSON, "bool pipecat_xvf_beam_json(")
assert serializer_body.count("append_json_float(") == 10
handler_body = function_body(OTA, "static esp_err_t xvf_beam_handler(")
assert "pipecat_xvf_read_beam(&telemetry)" in handler_body
assert "pipecat_xvf_beam_json(&telemetry, body, sizeof(body))" in handler_body
assert '"503 Service Unavailable"' in handler_body
assert r'\"azimuth\"' in serializer_body
assert r'\"selected_azimuth\"' in serializer_body
assert r'\"spenergy\"' in serializer_body

server_body = function_body(OTA, "void pipecat_init_ota_server()")
assert '.uri = "/xvf/beam"' in server_body
assert ".method = HTTP_GET" in server_body
assert "httpd_register_uri_handler(g_ota_server, &beam_uri)" in server_body
registered_handlers = server_body.count("httpd_register_uri_handler(")
assert registered_handlers == 8
assert f"config.max_uri_handlers = {registered_handlers}" in server_body

print("xvf beam source contract: PASS")
