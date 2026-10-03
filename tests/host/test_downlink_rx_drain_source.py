"""Source contract: downlink datagrams are drained per loop pass (t_5faf78b4).

One datagram per peer_connection_loop pass + main.cpp's TICK_INTERVAL (15 ms)
sleep serviced ~1 packet / 19 ms against the 20 ms downlink: the device ran
~65 ms behind the hub (p50), and a ~120 ms wifi hold overflowed lwIP's 6-deep
UDP mailbox (bench soak 2026-10-03: 71 hub-sent seqs never serviced in 9 min,
every gap_resume = hold + 6 serviced + 3-5 dropped). Draining per pass +
a 16-deep mailbox: 0 never-serviced, p50 lag 21 ms.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "xiao-esp32-s3"
PC = (BOARD / "components/peer/peer_connection.c").read_text()
SDK = (BOARD / "sdkconfig.defaults").read_text()
PEER_CMAKE = (BOARD / "components/peer/CMakeLists.txt").read_text()


def _completed() -> str:
    loop_fn = PC.index("int peer_connection_loop(")
    start = PC.index("case PEER_CONNECTION_COMPLETED:", loop_fn)
    return PC[start : PC.index("case PEER_CONNECTION_FAILED:", start)]


def test_completed_state_drains_in_a_bounded_loop() -> None:
    body = _completed()
    loop = body.index("for (int rx_n = 0;")
    assert loop < body.index("agent_recv(&pc->agent, pc->agent_buf,")
    assert "rx_n < PIPECAT_RX_DRAIN_MAX &&" in body
    # the single-shot upstream form must not come back
    assert not re.search(r"if \(\(pc->agent_ret = agent_recv\(", body)


def test_drain_bound_default() -> None:
    m = re.search(r"#define PIPECAT_RX_DRAIN_MAX (\d+)", PC)
    assert m and int(m.group(1)) >= 4


def test_udp_mailbox_holds_a_wifi_burst() -> None:
    m = re.search(r"^CONFIG_LWIP_UDP_RECVMBOX_SIZE=(\d+)$", SDK, re.M)
    assert m and int(m.group(1)) >= 16


def test_arrival_trace_is_not_in_the_default_build() -> None:
    default_codes = re.search(r'set\(LOCAL_CODES ([^)]*)\)', PEER_CMAKE).group(1)
    assert "arrival_trace" not in default_codes
    assert '"$ENV{PIPECAT_ARRIVAL_TRACE}" STREQUAL "1"' in PEER_CMAKE
    for hook in re.finditer(r"at_push\(", PC):
        guard = PC.rfind("#if PIPECAT_ARRIVAL_TRACE", 0, hook.start())
        assert guard != -1 and PC.find("#endif", guard) > hook.start()


if __name__ == "__main__":
    for _name, _fn in sorted(globals().items()):
        if _name.startswith("test_") and callable(_fn):
            _fn()
    print("downlink rx drain source contract: OK")
