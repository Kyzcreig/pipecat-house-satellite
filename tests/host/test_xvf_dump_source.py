#!/usr/bin/env python3
"""Source contract for GET /xvf/dump (t_a527ebfa endpoint, t_d355a513 additions).

Locks:
- the dump is READ-ONLY: the row renderer only issues xvf_read_bytes();
- every name in the volatile list exists in the dump table (a typo would
  silently pull a telemetry row into dsp_fingerprint);
- the canonical serialisation the hub recomputes: '{' + non-volatile rendered
  rows joined by ',' + '}', sha256, hex;
- the rate limit: one dump in flight, 10 s cooldown, 429 + Retry-After;
- a bus timeout is retried once before a row reports err.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "xiao-esp32-s3" / "src"
MEDIA = (SRC / "media.cpp").read_text()
OTA = (SRC / "ota.cpp").read_text()
MAIN_H = (SRC / "main.h").read_text()


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


def block(source: str, opener: str) -> str:
    start = source.index(opener)
    return source[start : source.index("};", start)]


# Dump table names: literal rows plus the PP_/AEC_/AM helper macros.
table = block(MEDIA, "static const DumpEntry kDumpEntries[]")
names = set(re.findall(r'^\s*\{"(\w+)",', table, re.M))
names |= {"PP_" + n for n in re.findall(r"XVF_DUMP_PP\((\w+),", table)}
names |= {"AEC_" + n for n in re.findall(r"XVF_DUMP_AEC\((\w+),", table)}
names |= set(re.findall(r'XVF_DUMP_AM\("(\w+)",', table))
assert len(names) >= 100, len(names)

volatile = set(re.findall(r'"(\w+)"', block(MEDIA, "static const char *const kDumpVolatile[]")))
missing = volatile - names
assert not missing, f"volatile names not in kDumpEntries: {sorted(missing)}"
# Configuration registers that MUST stay in the fingerprint (the theater-gain
# P0 compares exactly these across rooms).
for must in ("AUDIO_MGR_MIC_GAIN", "AUDIO_MGR_REF_GAIN", "PP_AGCMAXGAIN",
             "PP_AGCDESIREDLEVEL", "PP_AGCONOFF", "AUDIO_MGR_OP_ALL",
             "AUDIO_MGR_SYS_DELAY", "AEC_HPFONOFF", "VERSION"):
    assert must in names and must not in volatile, must

# READ-ONLY renderer.
row = function_body(MEDIA, "bool pipecat_xvf_dump_row(")
for forbidden in ("xvf_write", "nvs_", "i2c_master_transmit(", "TuneEntry"):
    assert forbidden not in row, forbidden
assert row.count("xvf_read_bytes(") == 2, "one read + one timeout retry"
assert "if (ret == ESP_ERR_TIMEOUT)" in row

for sym in ("pipecat_xvf_dump_row_volatile", "pipecat_xvf_dump_row_name",
            "pipecat_xvf_dump_count", "pipecat_xvf_dump_row"):
    assert sym in MAIN_H, sym

# Fingerprint: '{' + non-volatile rows joined by ',' + '}' over the SAME bytes
# that go out in "regs" (hashed before the trailing ',' is appended).
stream = function_body(OTA, "static esp_err_t xvf_dump_stream(")
tok = re.sub(r"\s+", " ", stream)
assert 'reinterpret_cast<const unsigned char *>("{"), 1' in tok
assert 'reinterpret_cast<const unsigned char *>("}"), 1' in tok
assert "if (!pipecat_xvf_dump_row_volatile(i))" in tok
assert tok.index("mbedtls_sha256_update(&fp, reinterpret_cast<const unsigned char *>(line)") < tok.index(
    'strlcat(line, ",", sizeof(line))'
), "row must be hashed before its separator is appended"
assert r'\"dsp_fingerprint\\\":\\\"%s\\\"' in stream or '\\"dsp_fingerprint\\":\\"%s\\"' in stream
assert '\\"volatile\\":[' in stream
for forbidden in ("xvf_write", "nvs_", "pipecat_xvf_tune"):
    assert forbidden not in stream, forbidden

# Rate limit: in-flight flag + 10 s cooldown -> 429 with Retry-After.
handler = function_body(OTA, "static esp_err_t xvf_dump_handler(")
assert "static constexpr int64_t kXvfDumpCooldownUs = 10LL * 1000 * 1000;" in OTA
assert "compare_exchange_strong(expected, true)" in handler
assert '"429 Too Many Requests"' in handler
assert '"Retry-After"' in handler
htok = re.sub(r"\s+", " ", handler)
assert htok.index("xvf_dump_stream(req)") < htok.index("g_xvf_dump_done_us.store(")
assert htok.index("g_xvf_dump_done_us.store(") < htok.index("g_xvf_dump_busy.store(false)")

# Registered as GET with its own handler slot.
assert '.uri = "/xvf/dump"' in OTA and "&dump_uri" in OTA
print("xvf dump source contract: PASS")
