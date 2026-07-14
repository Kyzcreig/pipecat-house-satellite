#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PEER = ROOT / "xiao-esp32-s3/components/peer"
cmake = (PEER / "CMakeLists.txt").read_text()
rtp = (PEER / "rtp.c").read_text()
peer = (PEER / "peer_connection.c").read_text()

assert '"red_wrap.c"' in cmake
assert '#include "red_wrap.h"' in rtp
assert "red_wrap_packet(" in rtp
assert "PT_RED" in rtp
assert "PIPECAT_UPLINK_RED" in rtp
assert "Uplink mic audio" in peer and "PT 63" in peer
print("uplink RED source contract: PASS")
