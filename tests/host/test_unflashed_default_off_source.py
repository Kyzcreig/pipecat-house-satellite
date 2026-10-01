#!/usr/bin/env python3
"""Source contract: features merged to main but never flashed stay DEFAULT OFF.

t_4f8fe707: main carried four features no satellite has run (#2 uplink 64k/lane,
#4 decimator droop comp, #5 FEC-only-on-LBRR gap fill, #13 GET /xvf/aec_filter).
A flash from main would have shipped all four untested and unannounced. Each is
now a configure-time flag, default 0, so a default build from main reproduces
the live image (proven by a normalized A/B build against 5e6717e). Turning one
on needs its own card. This guard fails if a default flips, a gated source
re-enters the unconditional SRCS list, or a default-off arm stops being the
live behaviour.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
XIAO = ROOT / "xiao-esp32-s3"
SRC = XIAO / "src"
CONFIG = (SRC / "pipecat_build_config.h.in").read_text()
TOP_CMAKE = (XIAO / "CMakeLists.txt").read_text()
SRC_CMAKE = (SRC / "CMakeLists.txt").read_text()
MEDIA = (SRC / "media.cpp").read_text()
OTA = (SRC / "ota.cpp").read_text()

FLAGS = {
    "PIPECAT_UPLINK_64K": None,
    "PIPECAT_DECIM_COMP": "decim_comp.c",
    "PIPECAT_FEC_LBRR": "opus_gapfill.c",
    "PIPECAT_XVF_AEC_FILTER": "xvf_aec_filter.cpp",
}
NOTE = "built, never flashed, needs its own card"


def test_every_flag_defaults_off_with_the_note() -> None:
    for flag in FLAGS:
        m = re.search(rf"#ifndef {flag}\n#define {flag} 0  // (.*)\n#endif", CONFIG)
        assert m, f"{flag}: no default-0 block in pipecat_build_config.h.in"
        assert NOTE in m.group(1), f"{flag}: default lacks the '{NOTE}' note"


def test_no_source_file_redefines_a_default_on() -> None:
    for path in SRC.iterdir():
        if path.suffix not in (".c", ".cpp", ".h"):
            continue
        text = path.read_text()
        for flag in FLAGS:
            assert not re.search(rf"#define {flag} 1\b", text), f"{path.name}: {flag} 1"


def test_cmake_only_defines_a_flag_when_asked() -> None:
    m = re.search(r"foreach\(_pipecat_flag ([^)]*)\)(.*?)endforeach\(\)", TOP_CMAKE, re.S)
    assert m, "top CMakeLists.txt: flag foreach missing"
    assert set(m.group(1).split()) == set(FLAGS)
    assert '"$ENV{${_pipecat_flag}}" STREQUAL "1"' in m.group(2)
    for flag in FLAGS:
        assert f"{flag}=0" not in TOP_CMAKE and f"{flag}=1)" not in TOP_CMAKE


def test_gated_sources_are_compiled_only_when_on() -> None:
    base = re.search(r"set\(TARGET_SRC ([^)]*)\)", SRC_CMAKE)
    assert base, "src/CMakeLists.txt: TARGET_SRC base list missing"
    register = re.search(r"else\(\)\s*idf_component_register\(\s*SRCS ([^\n]*)", SRC_CMAKE)
    assert register and register.group(1).split() == ["${COMMON_SRC}", "${TARGET_SRC}"]
    for flag, source in FLAGS.items():
        if source is None:
            continue
        assert f'"{source}"' not in base.group(1), f"{source} is unconditional"
        assert re.search(
            rf'if\("\$ENV\{{{flag}\}}" STREQUAL "1"\)\s*list\(APPEND TARGET_SRC "{re.escape(source)}"\)',
            SRC_CMAKE,
        ), f"{source} not gated on {flag}"


def test_default_arms_are_the_live_behaviour() -> None:
    assert re.search(
        r"#if PIPECAT_UPLINK_64K\n#define OPUS_ENCODER_BITRATE 64000\n#else\n"
        r"#define OPUS_ENCODER_BITRATE 30000\n#endif",
        MEDIA,
    ), "uplink bitrate: default arm must be the live 30k/lane"
    assert re.search(
        r"#if PIPECAT_XVF_AEC_FILTER\nstatic constexpr size_t XVF_READ_MAX_PAYLOAD = 60;\n"
        r"#else\nstatic constexpr size_t XVF_READ_MAX_PAYLOAD = 31;\n#endif",
        MEDIA,
    ), "XVF read payload: default arm must be the live 31"
    assert re.search(
        r"#if PIPECAT_XVF_AEC_FILTER\n  config.max_uri_handlers = 10;\n#else\n"
        r"  config.max_uri_handlers = 9;\n#endif",
        OTA,
    ), "httpd slots: default arm must be the live 9"
    reg = OTA.index("httpd_register_uri_handler(g_ota_server, &aec_filter_uri)")
    assert OTA.rfind("#if PIPECAT_XVF_AEC_FILTER", 0, reg) > OTA.rfind("#endif", 0, reg)
    assert re.search(
        r"#else\n  constexpr size_t kBodyCapacity = 2200;\n#endif", OTA
    ), "/playback/stats body: default arm must be the live 2200"
    decode = MEDIA[MEDIA.index("void pipecat_audio_decode(") :]
    live_arm = decode[decode.index("#else") : decode.index("#endif")]
    flat = re.sub(r"\s+", "", live_arm)
    assert "1/*decode_fec*/);s_pending_gap=0;if(fec_size>0){g_play_stat_fec++;" in flat


if __name__ == "__main__":
    # CI's host-tests job runs run_*.sh with a bare python3 (no pytest).
    for name, fn in list(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
    print("unflashed default-off source contract: PASS")
