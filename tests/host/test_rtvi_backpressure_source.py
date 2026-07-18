"""Regression for the kitchen NACK-v2 arm reset (t_5c931cb9 / bench t_d7da8b62).

Root cause (source- and journal-grounded, not inferred):

  * Inbound data-channel messages are parsed on the prio-8 transport loop
    (main.cpp: WEBRTC_LOOP_TASK_PRIORITY = 8) inside peer_connection_loop()'s
    onmessage callback, which calls pipecat_rtvi_handle_message().
  * That function pushed into a 10-deep queue drained by the prio-2 RTVI task
    with xQueueSend(rtvi_queue, &rtvi_msg, portMAX_DELAY).
  * Under a sustained inbound RTVI flood the queue fills; the BLOCKING send then
    stalls the transport loop, so peer_connection_loop() stops servicing ICE
    keepalive. After CONFIG_KEEPALIVE_TIMEOUT (30_000ms, components/peer) libpeer
    logs "binding request timeout" -> PEER_CONNECTION_CLOSED -> webrtc.cpp
    esp_restart(). The device reboots ~30s after load start.

  Live corroboration (kitchen-soak-failure-journal.log): 512B /test-rtvi-load
  flood begins 02:06:00; first "Timeout: No audio frame" 02:06:34.558 (~34s
  later); reboot re-offer (peer #1) 02:06:39 — the predicted 30s window.

Fix (doctrine 3d — liveness beats chat): the transport-loop send must be
non-blocking. A full queue sheds the inbound message and counts it in
g_rtvi_rx_dropped; it must NEVER block peer_connection_loop().

This test asserts the shipping firmware carries the non-blocking policy. It
FAILS on the pre-fix source (portMAX_DELAY send, no drop counter) and PASSES
after the fix — the mutation proof for this card.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RTVI = (ROOT / "xiao-esp32-s3/src/rtvi.cpp").read_text()
OTA = (ROOT / "xiao-esp32-s3/src/ota.cpp").read_text()


def _handle_message_body() -> str:
    """Body of the transport-callback producer pipecat_rtvi_handle_message()."""
    start = RTVI.index("void pipecat_rtvi_handle_message(const char *msg)")
    # function runs to end of file; that's fine — it is the last symbol.
    return RTVI[start:]


def test_transport_callback_send_is_non_blocking() -> None:
    """The producer must not block the prio-8 transport loop on a full queue."""
    body = _handle_message_body()
    assert "xQueueSend(rtvi_queue" in body
    # A portMAX_DELAY / blocking send on THIS path is exactly the reset vector.
    # Assert the specific blocking send statement is gone (a prose mention of the
    # token in an explanatory comment is fine — match the call, not the word).
    assert "xQueueSend(rtvi_queue, &rtvi_msg, portMAX_DELAY)" not in body, (
        "pipecat_rtvi_handle_message must not block the transport loop; "
        "a full RTVI queue would stall ICE keepalive -> esp_restart"
    )
    # Bounded/zero-timeout send.
    assert "xQueueSend(rtvi_queue, &rtvi_msg, 0)" in body


def test_full_queue_sheds_and_counts_the_drop() -> None:
    """On a full queue the message is counted AND its cJSON freed (no leak)."""
    body = _handle_message_body()
    assert "g_rtvi_rx_dropped" in body
    # The drop branch must free the parsed cJSON to avoid a heap leak under flood.
    assert "cJSON_Delete(j_msg)" in body


def test_rtvi_task_consumer_still_blocks_for_work() -> None:
    """The CONSUMER (rtvi_task) legitimately waits on the queue — unchanged."""
    assert "xQueueReceive(rtvi_queue, &msg, portMAX_DELAY)" in RTVI


def test_dropped_counter_is_declared_like_its_siblings() -> None:
    assert "volatile uint32_t g_rtvi_rx_dropped" in RTVI


def test_dropped_counter_is_exposed_via_playback_stats() -> None:
    """Bench telemetry needs the drop count to confirm shedding under load."""
    assert "extern volatile uint32_t g_rtvi_rx_dropped;" in OTA
    assert "rtvi_rx_dropped" in OTA
    assert "(unsigned long)g_rtvi_rx_dropped" in OTA
