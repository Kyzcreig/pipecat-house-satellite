"""Static transport contracts for firmware paths that require ESP-IDF to run."""
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
WEBRTC = (ROOT / "xiao-esp32-s3/src/webrtc.cpp").read_text()
RTVI = (ROOT / "xiao-esp32-s3/src/rtvi.cpp").read_text()
SCTP = (ROOT / "xiao-esp32-s3/components/peer/sctp.c").read_text()
PEER_HEADER = (ROOT / "xiao-esp32-s3/components/peer/peer_connection.h").read_text()


def test_dcep_unordered_zero_retransmit_type_is_exactly_0x81() -> None:
    assert "DATA_CHANNEL_PARTIAL_RELIABLE_REXMIT_UNORDERED = 0x81" in PEER_HEADER
    assert "channel_type" in SCTP
    assert "reliability_parameter" in SCTP
    peer = (ROOT / "xiao-esp32-s3/components/peer/peer_connection.c").read_text()
    assert "msg[1] = channel_type" in peer


def test_nack_arms_only_after_validated_rtx_channel_mapping() -> None:
    on_open_start = WEBRTC.index("pipecat_ondatachannel_onopen_task")
    on_open_end = WEBRTC.index("pipecat_onconnectionstatechange_task", on_open_start)
    assert "rtp_nack_register_sender" not in WEBRTC[on_open_start:on_open_end]
    assert "peer_connection_lookup_datachannel" in WEBRTC
    assert "DATA_CHANNEL_PARTIAL_RELIABLE_REXMIT_UNORDERED" in WEBRTC
    assert "NACK_RTX_OPEN_TIMEOUT_MS 5000u" in WEBRTC
    assert "rtp_nack_register_sender" in WEBRTC


def test_rtx_channel_is_binary_and_bypasses_json_rtvi_parser() -> None:
    assert "nack_parse_rtx_frame" in WEBRTC
    assert "rtp_nack_feed_rtx" in WEBRTC
    assert "payload_b64" not in RTVI
    assert 'hash(j_t->valuestring) == hash("rtx")' not in RTVI


def test_malformed_rtx_has_dedicated_telemetry() -> None:
    assert "g_rtx_malformed" in WEBRTC
    assert "rtx_malformed" in (ROOT / "xiao-esp32-s3/src/ota.cpp").read_text()


def test_request_to_splice_rtt_samples_are_machine_parseable() -> None:
    rtp = (ROOT / "xiao-esp32-s3/components/peer/rtp.c").read_text()
    ota = (ROOT / "xiao-esp32-s3/src/ota.cpp").read_text()
    assert "NACK_V2_REQUEST" in rtp
    assert "NACK_V2_RTX" in rtp
    assert "NACK_V2_SPLICE" in rtp
    assert "rtt_ms=" in rtp
    assert "nack_rtt_samples_ms" in ota


def test_rtx_staging_fifo_has_capacity_for_full_eight_seq_burst() -> None:
    rtp = (ROOT / "xiao-esp32-s3/components/peer/rtp.c").read_text()
    assert "NACK_RTX_QUEUE_SLOTS (NACK_PENDING_SLOTS + 1)" in rtp
    assert "s_rtx_q[NACK_RTX_QUEUE_SLOTS]" in rtp


def test_custom_sctp_negotiates_and_honors_partial_reliability() -> None:
    sctp = (ROOT / "xiao-esp32-s3/components/peer/sctp.c").read_text()
    header = (ROOT / "xiao-esp32-s3/components/peer/sctp.h").read_text()
    assert "SCTP_PARAM_PRSCTP_SUPPORTED = 0xC000" in header
    assert "supported->value[0] = SCTP_FORWARD_TSN" in sctp
    assert "case SCTP_FORWARD_TSN" in sctp
    assert "inbound_gap_bitmap" in sctp
    assert "cumulative_tsn_ack = data_chunk->tsn" not in sctp


def test_custom_sctp_is_compiled_only_when_nack_flag_is_enabled() -> None:
    cmake = (ROOT / "xiao-esp32-s3/components/peer/CMakeLists.txt").read_text()
    gate_start = cmake.index('if("$ENV{PIPECAT_NACK}" STREQUAL "1")')
    gate_end = cmake.index("endif()", gate_start)
    gate = cmake[gate_start:gate_end]
    assert 'list(FILTER CODES EXCLUDE REGEX ".*/sctp\\\\.c$")' in gate
    assert 'list(APPEND LOCAL_CODES "sctp.c")' in gate
    assert "SRCS ${CODES} ${LOCAL_CODES}" in cmake


def test_dark_build_keeps_stock_sctp_abi() -> None:
    header = (ROOT / "xiao-esp32-s3/components/peer/sctp.h").read_text()
    peer_header = (ROOT / "xiao-esp32-s3/components/peer/peer_connection.h").read_text()
    peer = (ROOT / "xiao-esp32-s3/components/peer/peer_connection.c").read_text()
    assert "#ifdef PIPECAT_NACK\n  uint8_t channel_type" in header
    assert "#ifdef PIPECAT_NACK\n  uint32_t inbound_cumulative_tsn" in header
    assert "#ifdef PIPECAT_NACK\nint peer_connection_lookup_datachannel" in peer_header
    assert "#ifdef PIPECAT_NACK\nint peer_connection_lookup_datachannel" in peer