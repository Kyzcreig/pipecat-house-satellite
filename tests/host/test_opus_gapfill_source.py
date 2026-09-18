"""Source guard: the downlink gap-fill must route FEC-vs-PLC on LBRR presence.

t_5730dda1: `opus_decode(..., decode_fec=1)` returns a positive count even for a
packet with no LBRR (libopus runs PLC internally), so `fec_size > 0` cannot
attribute `g_play_stat_fec`. The attribution lives in opus_gapfill.c (host-
tested against the vendored esp-libopus by tests/host/run_gapfill_tests.sh);
media.cpp must call it and must not re-grow a private decode_fec branch.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "xiao-esp32-s3/src"
MEDIA = (SRC / "media.cpp").read_text()
GAPFILL = (SRC / "opus_gapfill.c").read_text()
CMAKE = (SRC / "CMakeLists.txt").read_text()


def test_media_routes_gap_fill_through_opus_gapfill() -> None:
    start = MEDIA.index("void pipecat_audio_decode(")
    end = MEDIA.index("static OpusEncoder *opus_encoder", start)
    body = MEDIA[start:end]
    assert "opus_gapfill_recover_one(opus_decoder, data, size," in body
    assert "&g_play_stat_fec, &g_play_stat_plc" in body
    # No private decode_fec=1 call left in media.cpp: that is the conflation.
    assert "1 /* decode_fec */" not in body
    assert "fec_size" not in body


def test_gapfill_attributes_on_lbrr_not_return_value() -> None:
    assert "opus_gapfill_packet_has_lbrr(data, len) == 1" in GAPFILL
    # Never truthiness: <0 (malformed) must not be counted as FEC.
    assert "if (opus_gapfill_packet_has_lbrr(data, len))" not in GAPFILL


def test_gapfill_is_compiled_into_firmware() -> None:
    assert '"opus_gapfill.c"' in CMAKE
    assert '#include "opus_gapfill.h"' in MEDIA


def test_playback_stats_field_names_unchanged() -> None:
    # /playback/stats is user-visible and read by transport_conformance_check.py.
    ota = (SRC / "ota.cpp").read_text()
    assert "g_play_stat_plc" in ota and "g_play_stat_fec" in ota
