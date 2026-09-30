#!/usr/bin/env python3
"""Source contract for the READ-ONLY XVF diagnostic register table (t_7595755b).

Locks: the XMOS control-map ids for every row, that the diag path never
writes a register or touches NVS, and that /xvf/read falls back to it only
for names that are not tune params.
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


def const(name: str) -> int:
    m = re.search(rf"static constexpr uint8_t {name} = (\d+);", MEDIA)
    assert m, name
    return int(m.group(1))


# XMOS XVF3800 control map (Obsidian "XVF3800 - Complete Device Reference",
# control rows): name -> (resid, cmd, type, count). Every row is RO.
EXPECTED = {
    "AEC_AECPATHCHANGE": (33, 0, "INT32", 1),
    "AEC_AECCONVERGED": (33, 3, "INT32", 1),
    "AEC_RT60": (33, 9, "FLOAT", 1),
    "AEC_NUM_MICS": (33, 71, "INT32", 1),
    "AEC_NUM_FARENDS": (33, 72, "INT32", 1),
    "AEC_AZIMUTH_VALUES": (33, 75, "FLOAT", 4),
    "AEC_CURRENT_IDLE_TIME": (33, 77, "UINT32", 1),
    "AEC_MIN_IDLE_TIME": (33, 78, "UINT32", 1),
    "AEC_SPENERGY_VALUES": (33, 80, "FLOAT", 4),
    "SPECIAL_CMD_AEC_FILTER_LENGTH": (33, 93, "INT32", 1),
    "AUDIO_MGR_CURRENT_IDLE_TIME": (35, 2, "INT32", 1),
    "AUDIO_MGR_MIN_IDLE_TIME": (35, 3, "INT32", 1),
    "MAX_CONTROL_TIME": (35, 5, "INT32", 1),
    "AUDIO_MGR_SELECTED_AZIMUTHS": (35, 11, "FLOAT", 2),
}
RESIDS = {"XVF_RESID_AEC": 33, "XVF_RESID_AUDIO_MGR": 35}
assert const("XVF_RESID_AEC") == 33 and const("XVF_RESID_AUDIO_MGR") == 35

table_start = MEDIA.index("static const DiagEntry kDiagEntries[]")
table = MEDIA[table_start : MEDIA.index("};", table_start)]
rows = re.findall(
    r'\{"(\w+)",\s*(XVF_RESID_\w+),\s*(XVF_CMD_\w+),\s*DiagType::(\w+),\s*(\d+)\}',
    table,
)
got = {
    name: (RESIDS[resid], const(cmd), typ, int(count))
    for name, resid, cmd, typ, count in rows
}
assert got == EXPECTED, f"diag table drift:\n got={got}\n want={EXPECTED}"

# Strictly READ-ONLY: the diag reader issues reads only; no NVS, no writes.
diag_body = function_body(MEDIA, "esp_err_t pipecat_xvf_read_diag(")
for forbidden in ("xvf_write", "nvs_", "i2c_master_transmit(", "TuneEntry"):
    assert forbidden not in diag_body, forbidden
assert "xvf_read_floats(" in diag_body and "xvf_read_bytes(" in diag_body
assert "strcasecmp(param, candidate.name)" in diag_body
# Diag names never shadow a tune param (tune lookup wins, diag is a fallback).
tune_names = set(re.findall(r'\{"(\w+)",', MEDIA[
    MEDIA.index("static const TuneEntry kTuneEntries[]") : table_start]))
assert not {n.lower() for n in EXPECTED} & {n.lower() for n in tune_names}
assert "pipecat_xvf_read_diag" in MAIN_H

handler = function_body(OTA, "static esp_err_t xvf_read_handler(")
tokens = re.sub(r"\s+", " ", handler)
assert tokens.index("pipecat_xvf_read_param(") < tokens.index("pipecat_xvf_read_diag(")
assert r'\"readonly\":true' in handler
assert "char body[256];" in handler

print("xvf diag source contract: PASS")
