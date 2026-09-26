"""Regression guards for the firmware build with the NACK lane disabled."""
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PEER_CMAKE = (ROOT / "xiao-esp32-s3/components/peer/CMakeLists.txt").read_text()
RTP = (ROOT / "xiao-esp32-s3/components/peer/rtp.c").read_text()
NACK_GATE = 'if("$ENV{PIPECAT_NACK}" STREQUAL "1")'


def test_dark_build_manifest_excludes_nack_client_source() -> None:
    """A default/theater build must not compile the retired NACK client."""
    gate_start = PEER_CMAKE.index(NACK_GATE)
    gate_end = PEER_CMAKE.index("endif()", gate_start)
    assert '"nack_client.c"' not in PEER_CMAKE[:gate_start]
    assert '"nack_client.c"' in PEER_CMAKE[gate_start:gate_end]


def test_dark_build_does_not_initialize_nack_client() -> None:
    """The registration shim may exist in a dark build, but initialization may not."""
    function_start = RTP.index("void rtp_nack_register_sender")
    function_end = RTP.index("int rtp_nack_feed_rtx", function_start)
    function = RTP[function_start:function_end]
    gate_start = function.index("#ifdef PIPECAT_NACK")
    gate_end = function.index("#endif", gate_start)
    init = function.index("nack_client_init(&g_nack)")
    assert gate_start < init < gate_end
