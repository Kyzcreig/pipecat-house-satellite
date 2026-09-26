"""Source guard for the playback-stats HTTP task stack budget."""
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
OTA = (ROOT / "xiao-esp32-s3/src/ota.cpp").read_text()


def test_playback_stats_large_response_buffers_are_not_on_httpd_stack() -> None:
    start = OTA.index("static esp_err_t playback_stats_handler")
    end = OTA.index("void pipecat_init_ota_server", start)
    handler = OTA[start:end]

    assert "char rtt_samples[800]" not in handler
    assert "char body[1600]" not in handler
    assert "malloc(kRttSamplesCapacity + kBodyCapacity)" in handler
    assert "Unable to allocate playback stats response" in handler
    assert "free(scratch)" in handler
