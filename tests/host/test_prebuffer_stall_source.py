"""Source contract: the arrival-stall input is wired (U1d, t_9afd3ebe).

The host C tests prove prebuffer_ctl's stall logic. These pin the two seams
the C tests cannot see: the RTP receive path stamps every audio packet
unconditionally (not only in the PIPECAT_ARRIVAL_TRACE diagnostic build), and
the playback task feeds that stamp to pbc_track_arrival every loop pass.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MEDIA = (ROOT / "xiao-esp32-s3/src/media.cpp").read_text()
PEER = (ROOT / "xiao-esp32-s3/components/peer/peer_connection.c").read_text()


def _strip_trace_blocks(src: str) -> str:
    """Drop every #if PIPECAT_ARRIVAL_TRACE ... #endif/#else block body."""
    return re.sub(
        r"#if PIPECAT_ARRIVAL_TRACE\n.*?#(?:endif|else)", "", src, flags=re.S
    )


def test_rx_stamp_is_unconditional() -> None:
    assert "volatile uint32_t g_rtp_last_arrival_ms = 0;" in PEER
    live = _strip_trace_blocks(PEER)
    branch = live.index("if (ssrc == pc->remote_assrc) {")
    stamp = live.index(
        "g_rtp_last_arrival_ms = (uint32_t)(esp_timer_get_time() / 1000);", branch
    )
    assert stamp < live.index("rtp_decoder_decode(&pc->artp_decoder", branch)
    assert '#include "esp_timer.h"' in live


def _task() -> str:
    start = MEDIA.index("static void pipecat_playback_task")
    return MEDIA[start : MEDIA.index("void pipecat_init_audio_decoder", start)]


def test_playback_task_feeds_arrival_stall_input() -> None:
    task = _task()
    loop = task.index("for (;;) {")
    call = task.index("pbc_track_arrival(&pbc, g_rtp_last_arrival_ms,", loop)
    # evaluated before the effective prebuffer is read for this pass
    assert call < task.index("uint32_t effective_ms =", loop)
    assert "!prebuffering || pbc.drain_pending, now_ms);" in task
    assert task.count("pbc_track_arrival(") == 1
    assert "extern volatile uint32_t g_rtp_last_arrival_ms;" in MEDIA


def test_adaptive_stays_dark_by_default() -> None:
    assert "#define PIPECAT_ADAPTIVE_PREBUFFER 0" in MEDIA
    assert "pbc_init(&pbc, PIPECAT_ADAPTIVE_PREBUFFER, PIPECAT_GAP_RESUME_MS);" in MEDIA


if __name__ == "__main__":
    for _name, _fn in sorted(globals().items()):
        if _name.startswith("test_") and callable(_fn):
            _fn()
    print("prebuffer arrival-stall source contract: OK")
