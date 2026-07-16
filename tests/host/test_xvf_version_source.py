#!/usr/bin/env python3
"""Source contract for the XVF application-version read + reporting.

WHY THIS EXISTS (bug, 2026-07-16)
---------------------------------
The boot-time XVF version read used a WRONG I2C transaction framing: a bare
1-byte write of 0xB3 followed by a 3-byte read, with no command byte, no
read-length byte, and no leading status byte in the response. The device
answered with garbage that decoded as "v6.34.4" — an artifact, not a firmware
version. The real version (vendor protocol: DFU controller servicer resid 240,
cmd 88 GETVERSION, 3x uint8 MAJOR MINOR PATCH, read via the
{resid, cmd|0x80, len+1} framing that xvf_read_bytes() implements — the same
transaction behind the live 'Version request successful: 1.0.7' log from the
respeaker_xvf3800 ESPHome component) is 1.0.7 — the formatBCE/Seeed
I2S-master build.

This contract pins:
1. the version read goes through xvf_read_bytes() with resid 240 / cmd 88;
2. the mis-framed bare-0xB3 transaction cannot come back;
3. the read version is exposed on /ota/status and /xvf/params as
   "xvf_version" so device fingerprinting is honest.
"""
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
MAIN_H = (ROOT / "xiao-esp32-s3" / "src" / "main.h").read_text()
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


# 1. Correct framing: DFU controller servicer resid 240, GETVERSION cmd 88,
#    read via the shared xvf_read_bytes() helper (which frames
#    {resid, cmd|0x80, len+1} and strips the leading status byte).
assert "static constexpr uint8_t XVF_RESID_DFU_CONTROLLER = 240;" in MEDIA
assert "static constexpr uint8_t XVF_CMD_DFU_GETVERSION = 88;" in MEDIA
codec_body = function_body(MEDIA, "static void init_i2c_and_codec()")
assert re.search(
    r"xvf_read_bytes\(\s*XVF_RESID_DFU_CONTROLLER,\s*XVF_CMD_DFU_GETVERSION,\s*ver,\s*sizeof\(ver\)\)",
    codec_body,
), "version read must use xvf_read_bytes(XVF_RESID_DFU_CONTROLLER, XVF_CMD_DFU_GETVERSION, ...)"

# 2. The mis-framed transaction is gone: no bare-resid 0xB3 register constant,
#    and no raw i2c_master_transmit_receive in the version-read path (only
#    xvf_read_bytes/xvf_write_bytes may touch the control protocol there).
assert not re.search(
    r"=\s*0xB3\b", MEDIA
), "bogus bare-0xB3 version resid must not return"
assert "XVF3800_RESID_VERSION" not in MEDIA
assert "i2c_master_transmit_receive" not in codec_body

# 3. The read version lands in stable storage with an honest fallback and is
#    exported for the HTTP surfaces.
assert 'static char s_xvf_version[16] = "unknown";' in MEDIA
assert re.search(
    r'snprintf\(s_xvf_version,\s*sizeof\(s_xvf_version\),\s*"%u\.%u\.%u"',
    codec_body,
)
assert "const char *pipecat_xvf3800_version() { return s_xvf_version; }" in MEDIA
assert "extern const char *pipecat_xvf3800_version();" in MAIN_H

# 4. /ota/status and /xvf/params (both branches) report "xvf_version".
status_body = function_body(OTA, "static esp_err_t ota_status_handler(")
assert r"\"xvf_version\"" in status_body
assert "pipecat_xvf3800_version()" in status_body
params_body = function_body(OTA, "static esp_err_t xvf_params_handler(")
assert params_body.count("pipecat_xvf3800_version()") == 2, (
    "both the reset and report branches of /xvf/params must carry xvf_version"
)
assert r"\"xvf_version\"" in params_body

print("xvf version source contract: PASS")
