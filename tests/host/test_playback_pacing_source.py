"""Source contract: playback task paces I2S writes by DMA lead (t_a57274a4).

Without the pace, one write burst emptied the 80 ms prebuffer into the ~117 ms
TX DMA, the ring read empty, the prebuffer re-armed, and playback ran as an
80 ms sawtooth (gap_resumes +9 per 400 ms chime; ack chime missed).
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MEDIA = (ROOT / "xiao-esp32-s3/src/media.cpp").read_text()


def _task() -> str:
    start = MEDIA.index("static void pipecat_playback_task")
    return MEDIA[start : MEDIA.index("void pipecat_init_audio_decoder", start)]


def test_empty_ring_is_not_a_drain_while_dma_still_plays() -> None:
    task = _task()
    drain = task.index("if (avail < PCM_SAMPLES_PER_FRAME) {")
    wait = task.index("if (lead_ms > 0) {", drain)
    assert wait < task.index("pbc_on_full_drain(&pbc, now_ms);", drain)


def test_writes_are_paced_to_the_dma_lead_cap() -> None:
    task = _task()
    pace = task.index("if (lead_ms > PIPECAT_PLAY_DMA_LEAD_MS) {")
    assert pace < task.index("i2s_channel_write(")
    assert "#define PIPECAT_PLAY_DMA_LEAD_MS 40" in MEDIA


def test_only_accepted_frames_extend_the_lead() -> None:
    task = _task()
    ok = task.index("if (ret == ESP_OK) {")
    assert task.index("pbc_on_frame_written(", ok) < task.index("} else {", ok)
    assert task.count("pbc_on_frame_written(") == 1


def test_default_prebuffer_is_120_ms() -> None:
    # Apollo 2026-10-02 (t_a57274a4): default 120 ms (16 samples/ms @16k).
    assert "#define PIPECAT_PLAY_PREBUFFER_MS 120" in MEDIA
    assert (
        "volatile uint32_t g_play_prebuffer_samples = PIPECAT_PLAY_PREBUFFER_MS * 16;"
        in MEDIA
    )


if __name__ == "__main__":
    for _name, _fn in sorted(globals().items()):
        if _name.startswith("test_") and callable(_fn):
            _fn()
    print("playback pacing source contract: OK")
