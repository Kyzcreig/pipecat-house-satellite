#!/usr/bin/env python3
"""Source contract for the AEC filter-coefficient read path (t_c1bfa4f6).

Locks: the XMOS RESID 33 command ids, that the ONLY writes are the XMOS
read-sequence selectors + ABORT (never a coefficient write, SHF_BYPASS, a
tune param or NVS), that the diag reader stays write-free, and that the
endpoint is registered with enough handler slots.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "xiao-esp32-s3" / "src"
HDR = (SRC / "xvf_aec_filter.h").read_text()
SEQ = (SRC / "xvf_aec_filter.cpp").read_text()
MEDIA = (SRC / "media.cpp").read_text()
OTA = (SRC / "ota.cpp").read_text()
CMAKE = (SRC / "CMakeLists.txt").read_text()


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


def const(name: str) -> int:
    m = re.search(rf"static constexpr (?:uint8_t|size_t) {name} = (\d+);", HDR)
    assert m, name
    return int(m.group(1))


# XMOS host-control map (docs/vendor/xvf3800/respeaker/xvf_host.py), RESID 33.
assert const("XVF_AEC_CMD_FAR_MIC_INDEX") == 90
assert const("XVF_AEC_CMD_FILTER_COEFF_START_OFFSET") == 91
assert const("XVF_AEC_CMD_FILTER_COEFFS") == 92
assert const("XVF_AEC_CMD_FILTER_LENGTH") == 93
assert const("XVF_AEC_CMD_FILTER_ABORT") == 94
assert const("XVF_AEC_CMD_NUM_MICS") == 71
assert const("XVF_AEC_CMD_NUM_FARENDS") == 72
assert const("XVF_AEC_COEFFS_PER_PAGE") == 15

# The sequence module writes exactly three commands, all read-sequence control.
written = set(re.findall(r"write_i32\(\s*ops->ctx,\s*(XVF_AEC_CMD_\w+)", SEQ))
assert written == {
    "XVF_AEC_CMD_FAR_MIC_INDEX",
    "XVF_AEC_CMD_FILTER_COEFF_START_OFFSET",
    "XVF_AEC_CMD_FILTER_ABORT",
}, written
assert "XVF_AEC_CMD_FILTER_COEFFS, page" in SEQ  # coefficients are only read
assert "BYPASS" not in SEQ.upper()

# media.cpp glue: RESID 33 only, no NVS, no tune table, no bypass.
glue = "".join(
    function_body(MEDIA, sig)
    for sig in (
        "static int aec_filter_write_i32(",
        "static int aec_filter_read_bytes(",
        "esp_err_t pipecat_xvf_read_aec_filter(",
    )
)
for forbidden in ("nvs_", "TuneEntry", "kTuneEntries", "BYPASS", "xvf_write_float"):
    assert forbidden not in glue, forbidden
assert glue.count("XVF_RESID_AEC") == 2
assert "PIPECAT_XVF_AEC_FILTER_BUDGET_US" in glue

# The shared read path grew to one COEFFS page (15 floats), no further.
assert "static constexpr size_t XVF_READ_MAX_PAYLOAD = 60;" in MEDIA
read_body = function_body(MEDIA, "static esp_err_t xvf_read_bytes(")
assert "out_len > XVF_READ_MAX_PAYLOAD" in read_body
assert "resp[XVF_READ_MAX_PAYLOAD + 1]" in read_body

# t_7595755b's diag reader stays strictly read-only.
diag = function_body(MEDIA, "esp_err_t pipecat_xvf_read_diag(")
assert "xvf_write" not in diag and "aec_filter" not in diag

# Endpoint: GET, registered, slot count covers every registration.
handler = function_body(OTA, "static esp_err_t xvf_aec_filter_handler(")
assert "pipecat_xvf_read_aec_filter(" in handler
assert '"application/octet-stream"' in handler
registered = len(re.findall(r"httpd_register_uri_handler\(", OTA))
slots = int(re.search(r"config\.max_uri_handlers = (\d+);", OTA).group(1))
assert registered <= slots, (registered, slots)
assert re.search(r'\.uri = "/xvf/aec_filter",\s*\.method = HTTP_GET', OTA)
assert '"xvf_aec_filter.cpp"' in CMAKE

print("xvf aec filter source contract: PASS")
