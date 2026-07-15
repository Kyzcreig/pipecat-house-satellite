#!/usr/bin/env python3
"""Guard the default-off XVF3800 dual-stream firmware capture contract."""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


def require(pattern: str, text: str, message: str, errors: list[str]) -> None:
    if re.search(pattern, text, re.DOTALL) is None:
        errors.append(message)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=".", help="repository root")
    args = parser.parse_args(argv)

    repo = Path(args.repo)
    media_path = repo / "xiao-esp32-s3/src/media.cpp"
    ota_path = repo / "xiao-esp32-s3/src/ota.cpp"
    config_path = repo / "xiao-esp32-s3/src/pipecat_build_config.h.in"
    cmake_path = repo / "xiao-esp32-s3/CMakeLists.txt"
    if (
        not media_path.is_file()
        or not ota_path.is_file()
        or not config_path.is_file()
        or not cmake_path.is_file()
    ):
        print("dualstream-lint: required firmware source file is missing", file=sys.stderr)
        return 2

    media = media_path.read_text(encoding="utf-8")
    ota = ota_path.read_text(encoding="utf-8")
    config = config_path.read_text(encoding="utf-8")
    cmake = cmake_path.read_text(encoding="utf-8")
    errors: list[str] = []

    require(
        r"#ifndef\s+PIPECAT_DUAL_STREAM\s+"
        r"#define\s+PIPECAT_DUAL_STREAM\s+0\s+"
        r"#endif",
        config,
        "PIPECAT_DUAL_STREAM must default to 0 while remaining compiler-overridable",
        errors,
    )
    require(
        r'if\("\$ENV\{PIPECAT_DUAL_STREAM\}" STREQUAL "1"\)\s+'
        r"add_compile_definitions\(PIPECAT_DUAL_STREAM=1\)\s+endif\(\)",
        cmake,
        "CMake must enable the compile flag only for PIPECAT_DUAL_STREAM=1",
        errors,
    )
    require(
        r"#ifndef\s+PIPECAT_DUAL_STREAM_RAW_MIC\s+"
        r"#define\s+PIPECAT_DUAL_STREAM_RAW_MIC\s+-1\s+"
        r"#endif",
        config,
        "PIPECAT_DUAL_STREAM_RAW_MIC must default to -1",
        errors,
    )
    require(
        r'if\(NOT "\$ENV\{PIPECAT_DUAL_STREAM_RAW_MIC\}" STREQUAL ""\).*?'
        r'if\(NOT "\$ENV\{PIPECAT_DUAL_STREAM_RAW_MIC\}" MATCHES "\^\[0-3\]\$"\).*?'
        r"add_compile_definitions\(\s*"
        r"PIPECAT_DUAL_STREAM_RAW_MIC=\$ENV\{PIPECAT_DUAL_STREAM_RAW_MIC\}\)",
        cmake,
        "CMake must reject raw mic indexes outside 0..3 and compile the selected index",
        errors,
    )
    require(
        r"#if\s+PIPECAT_DUAL_STREAM\s+"
        r"#if\s+PIPECAT_DUAL_STREAM_RAW_MIC\s*>=\s*0\s+"
        r"record\(xvf_write_u8_pair\(XVF_RESID_AUDIO_MGR,\s*"
        r"XVF_CMD_AUDIO_MGR_OP_R,\s*XVF_AUDIO_CATEGORY_RAW,\s*"
        r"PIPECAT_DUAL_STREAM_RAW_MIC\)\);\s*"
        r"#else\s+"
        r"record\(xvf_write_u8_pair\(XVF_RESID_AUDIO_MGR,\s*"
        r"XVF_CMD_AUDIO_MGR_OP_R,\s*XVF_AUDIO_CATEGORY_PROCESSED,\s*"
        r"XVF_AUDIO_SOURCE_AUTO_SELECT\)\);\s*"
        r"#endif\s+"
        r"#else\s+"
        r"record\(xvf_write_u8_pair\(XVF_RESID_AUDIO_MGR,\s*"
        r"XVF_CMD_AUDIO_MGR_OP_R,\s*XVF_AUDIO_CATEGORY_ASR,\s*"
        r"XVF_AUDIO_SOURCE_AUTO_SELECT\)\);\s*"
        r"#endif",
        media,
        "OP_R must be [1,mic] in raw mode, [6,3] in normal dual mode, and [7,3] in mono",
        errors,
    )
    require(
        r"pipecat_xvf_audio_mux_status\(&status\).*?"
        r'"/xvf/audio-mux".*?'
        r"\.handler\s*=\s*xvf_audio_mux_handler.*?"
        r"httpd_register_uri_handler\(g_ota_server,\s*&audio_mux_uri\)",
        ota,
        "live /xvf/audio-mux must return typed register readback and be registered",
        errors,
    )
    require(
        r"#if\s+PIPECAT_DUAL_STREAM\s+"
        r"static void stereo_48k_32bit_to_stereo_16k\(.*?"
        r"src\[i \+ 0\].*?src\[i \+ 2\].*?src\[i \+ 4\].*?"
        r"src\[i \+ 1\].*?src\[i \+ 3\].*?src\[i \+ 5\].*?"
        r"#endif",
        media,
        "dual-stream decimation must preserve independent left/right slots",
        errors,
    )
    require(
        r"#if\s+PIPECAT_DUAL_STREAM\s+"
        r"opus_encoder\s*=\s*opus_encoder_create\(WEBRTC_SAMPLE_RATE,\s*2,.*?"
        r"#else\s+"
        r"opus_encoder\s*=\s*opus_encoder_create\(WEBRTC_SAMPLE_RATE,\s*1,.*?"
        r"#endif",
        media,
        "dual-stream must use a 2-channel Opus encoder while flag-off stays mono",
        errors,
    )
    require(
        r"#if\s+PIPECAT_DUAL_STREAM\s+"
        r"opus_encoder_ctl\(opus_encoder,\s*"
        r"OPUS_SET_BITRATE\(OPUS_ENCODER_BITRATE \* 2\)\);\s*"
        r"#else\s+"
        r"opus_encoder_ctl\(opus_encoder,\s*"
        r"OPUS_SET_BITRATE\(OPUS_ENCODER_BITRATE\)\);\s*"
        r"#endif",
        media,
        "dual-stream Opus must budget 60 kb/s while flag-off stays at 30 kb/s",
        errors,
    )
    require(
        r"#if\s+PIPECAT_DUAL_STREAM\s+"
        r"read_buffer\s*=.*?heap_caps_malloc\(PCM_BUFFER_SIZE \* 2,\s*"
        r"MALLOC_CAP_8BIT\);\s*"
        r"#else\s+"
        r"read_buffer\s*=.*?heap_caps_malloc\(PCM_BUFFER_SIZE,\s*"
        r"MALLOC_CAP_8BIT\);\s*"
        r"#endif",
        media,
        "dual-stream capture must allocate two PCM channels while flag-off stays mono",
        errors,
    )
    require(
        r"#if\s+PIPECAT_DUAL_STREAM\s+"
        r"stereo_48k_32bit_to_stereo_16k\(i2s_capture_buffer,.*?read_buffer\);\s*"
        r"#else\s+"
        r"stereo_48k_32bit_to_mono_16k\(i2s_capture_buffer,.*?read_buffer\);\s*"
        r"#endif",
        media,
        "capture must call the lane-preserving decimator only under the flag",
        errors,
    )

    if errors:
        for error in errors:
            print(f"dualstream-lint: FAIL — {error}", file=sys.stderr)
        return 1

    print(
        "dualstream-lint: OK — default is mono [7,3]/[7,3]; flag-on preserves "
        "[7,3]/[6,3], or explicitly routes [7,3]/[1,mic], as stereo Opus at 60 kb/s"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
