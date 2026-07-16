#!/usr/bin/env python3
"""Source contract for the default-off cat-1 raw-mic right-slot experiment."""
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
CMAKE = (ROOT / "xiao-esp32-s3" / "CMakeLists.txt").read_text()
CONFIG = (ROOT / "xiao-esp32-s3" / "src" / "pipecat_build_config.h.in").read_text()
MEDIA = (ROOT / "xiao-esp32-s3" / "src" / "media.cpp").read_text()
MAIN_H = (ROOT / "xiao-esp32-s3" / "src" / "main.h").read_text()
OTA = (ROOT / "xiao-esp32-s3" / "src" / "ota.cpp").read_text()


assert re.search(
    r"#ifndef\s+PIPECAT_DUAL_STREAM_RAW_MIC\s+"
    r"#define\s+PIPECAT_DUAL_STREAM_RAW_MIC\s+-1\s+"
    r"#endif",
    CONFIG,
), "raw-mic routing must remain disabled by default"
assert re.search(
    r'if\(NOT "\$ENV\{PIPECAT_DUAL_STREAM_RAW_MIC\}" STREQUAL ""\).*?'
    r'if\(NOT "\$ENV\{PIPECAT_DUAL_STREAM_RAW_MIC\}" MATCHES "\^\[0-3\]\$"\).*?'
    r"PIPECAT_DUAL_STREAM_RAW_MIC=\$ENV\{PIPECAT_DUAL_STREAM_RAW_MIC\}",
    CMAKE,
    re.DOTALL,
), "CMake must reject non-mic sources and compile the selected raw mic"
assert "static constexpr uint8_t XVF_AUDIO_CATEGORY_RAW = 1;" in MEDIA
assert "static constexpr uint8_t XVF_AUDIO_CATEGORY_MAX = 8;" in MEDIA
assert re.search(
    r"XVF_CMD_AUDIO_MGR_OP_L,\s*XVF_AUDIO_CATEGORY_ASR,\s*"
    r"XVF_AUDIO_SOURCE_AUTO_SELECT",
    MEDIA,
), "left/STT slot must remain cat-7 ASR auto-select"
assert re.search(
    r"#if\s+PIPECAT_DUAL_STREAM_RAW_MIC\s*>=\s*0\s+"
    r"record\(xvf_write_u8_pair\(XVF_RESID_AUDIO_MGR,\s*"
    r"XVF_CMD_AUDIO_MGR_OP_R,\s*XVF_AUDIO_CATEGORY_RAW,\s*"
    r"PIPECAT_DUAL_STREAM_RAW_MIC\)\);\s*"
    r"#else\s+"
    r"record\(xvf_write_u8_pair\(XVF_RESID_AUDIO_MGR,\s*"
    r"XVF_CMD_AUDIO_MGR_OP_R,\s*XVF_AUDIO_CATEGORY_PROCESSED",
    MEDIA,
    re.DOTALL,
), "right slot must select cat-1 raw only under the explicit build flag"
assert "pipecat_xvf_audio_mux_status(&status)" in OTA
assert "pipecat_xvf_set_audio_mux_right(category, source, &status)" in OTA
assert "op_r_category" in OTA and "op_r_source" in OTA
assert "httpd_query_key_value" in OTA
assert re.search(
    r"bool\s+pipecat_xvf_set_audio_mux_right\(uint8_t\s+category,\s*"
    r"uint8_t\s+source,\s*PipecatXvfAudioMuxStatus\s*\*status\)",
    MAIN_H,
), "runtime right-slot mux API must be typed and explicit"
assert re.search(
    r"bool\s+pipecat_xvf_set_audio_mux_right\(.*?"
    r"category\s*>\s*XVF_AUDIO_CATEGORY_MAX.*?source\s*>\s*3.*?"
    r"XVF_CMD_AUDIO_MGR_OP_R.*?category.*?source",
    MEDIA,
    re.DOTALL,
), "runtime mux must fail closed outside categories 0..8 and sources 0..3"
assert '.uri = "/xvf/audio-mux"' in OTA
assert "httpd_register_uri_handler(g_ota_server, &audio_mux_uri)" in OTA
server_start = OTA.index("void pipecat_init_ota_server()")
server = OTA[server_start:]
registered_handlers = server.count("httpd_register_uri_handler(")
assert registered_handlers == 8
assert f"config.max_uri_handlers = {registered_handlers}" in server

print("raw mic3 source contract: PASS")
