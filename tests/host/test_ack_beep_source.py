"""Source contract: wake-ACK beep playback telemetry wiring (t_69ffa409)."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "xiao-esp32-s3/src"
MEDIA = (SRC / "media.cpp").read_text()
RTVI = (SRC / "rtvi.cpp").read_text()
OTA = (SRC / "ota.cpp").read_text()
CMAKE = (SRC / "CMakeLists.txt").read_text()


def test_only_frames_i2s_accepted_feed_the_matcher() -> None:
    start = MEDIA.index("static void pipecat_playback_task")
    task = MEDIA[start : MEDIA.index("void pipecat_init_audio_decoder", start)]
    ok_branch = task[task.index("if (ret == ESP_OK) {") : task.index("} else {", task.index("if (ret == ESP_OK) {"))]
    assert "ack_beep_played_frame(pop_buf, PCM_SAMPLES_PER_FRAME);" in ok_branch
    assert task.count("ack_beep_played_frame(") == 1
    assert "ack_beep_idle_tick();" in task


def test_rtvi_ack_beep_marker_arms_the_matcher() -> None:
    assert 'hash("ack_beep")' in RTVI
    assert "pipecat_ack_beep_arm(&m);" in RTVI


def test_playback_stats_exposes_the_counters() -> None:
    start = OTA.index("static esp_err_t playback_stats_handler")
    handler = OTA[start : OTA.index("void pipecat_init_ota_server", start)]
    for key in ("ack_beep_marks", "ack_beeps_played", "ack_beep_missed", "ack_beep_last_id", "uptime_ms"):
        assert f'\\"{key}\\":' in handler, key
    assert "pipecat_ack_beep_snapshot(&ab)" in handler


def test_ack_beep_is_built() -> None:
    assert '"ack_beep.c"' in CMAKE
