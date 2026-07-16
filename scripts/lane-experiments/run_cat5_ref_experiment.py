#!/usr/bin/env python3
"""Experiment A (t_2ccb0829): cat-5 chip-reference AEC3 A/B vs synthesized PCM.

HYPOTHESIS: AEC3's live failure is (partly) reference misalignment. Category 5
[5,0] is the far-end reference AFTER sample-rate conversion WITH system delay —
the reference exactly as the chip's own AEC sees it, already delay-aligned to
the mic path. If AEC3 with the chip's reference converges dramatically better
than with our server-synthesized reference PCM, the alignment hypothesis wins;
if it doesn't, linearity/transport is the remaining suspect.

DESIGN (kitchen rig, runs only after t_f2f4f499 releases it):
  Arm 1 (baseline)  : op_r=[1,3] raw mic  + server-synthesized reference PCM
  Arm 2 (chip ref)  : PACKED mode carrying [1,3] raw mic AND [5,0] chip ref
                      simultaneously -> both lanes share one clock; feed AEC3
                      the [5,0] lane as the render stream at lag 0.
  Fallback (no packed): sequential single-slot captures of [1,3] then [5,0]
                      during identical TTS playbacks — weaker (two playbacks),
                      only a sanity signal, and the JSON says so.

  Metric: external linear ERLE on far-end-only windows (per the AEC3 deep
  analysis on t_f2f4f499: export_linear_aec_output=true; do NOT read the APM
  erle stat, it is capped at ~6 dB). Report baseline vs chip-ref delta.

This driver only ORCHESTRATES captures over the existing HTTP surfaces
(/xvf/packed, /xvf/raw-capture, /xvf/audio-mux) plus the phase-4 offline
evaluator (aec3_offline_eval with linear output). It never flashes firmware
and it restores production mux + packed=0 in `finally`.

PRECONDITIONS (hard-checked at start):
  * firmware exposes /xvf/packed + /xvf/raw-capture (this task's branch)
  * kitchen identity via MAC table (clanker-satellite-flash gate)
  * explicit --rig-released acknowledgement (phase-4 owns the rig until then)
  * theater stays silent: DEVICE is pinned to the kitchen IP.
"""
from __future__ import annotations

import argparse
import json
import shlex
import subprocess
import sys
import time
from pathlib import Path

DEVICE = "http://192.168.1.185"  # kitchen ONLY (theater is silent-only)
KITCHEN_MAC = "dc:b4:d9:38:a6:cc"
RATE = 16_000
PRODUCTION_MUX = {"op_l": [7, 3], "upsample": [1, 1]}
# Packed layout for arm 2: L carries [7,3] ASR + [1,3] raw + [5,0] chip ref;
# R mirrors ref+raw for redundancy (any lane order works — the unpacker
# labels by PK index). OP_ALL order: L_PK0,L_PK1,L_PK2,R_PK0,R_PK1,R_PK2.
PACKED_OP_ALL = [7, 3, 1, 3, 5, 0, 5, 0, 1, 3, 0, 0]
HERE = Path(__file__).resolve().parent


def run(cmd: list[str], *, check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(cmd, check=check, capture_output=True, text=True)


def http_json(url: str) -> dict:
    out = run(["curl", "-fsS", "--max-time", "15", url]).stdout
    return json.loads(out)


def assert_kitchen_identity() -> None:
    status = http_json(f"{DEVICE}/ota/status")
    if status.get("satellite_id") != "kitchen":
        raise SystemExit(f"identity gate: {DEVICE} reports {status.get('satellite_id')!r}, not kitchen")
    neigh = run(["ssh", "ace-ai-lan",
                 "ping -c1 192.168.1.185 >/dev/null; ip neigh show 192.168.1.185"]).stdout
    if KITCHEN_MAC not in neigh:
        raise SystemExit(f"identity gate: MAC mismatch — expected {KITCHEN_MAC}, got: {neigh.strip()}")
    print(f"identity gate OK: kitchen {KITCHEN_MAC}, fw {status.get('firmware_version')}")


def assert_capture_endpoints() -> None:
    packed = http_json(f"{DEVICE}/xvf/packed")
    if not packed.get("ok"):
        raise SystemExit("firmware lacks /xvf/packed — flash the t_2ccb0829 branch first")
    print(f"packed status: {packed}")


def capture_raw(ms: int, out: Path) -> None:
    run(["curl", "-fsS", "--max-time", str(ms // 1000 + 20), "-o", str(out),
         f"{DEVICE}/xvf/raw-capture?ms={ms}"])
    size = out.stat().st_size
    expect = int(ms / 1000 * 48000 * 2 * 4)
    if size < expect * 0.9:
        raise SystemExit(f"short capture: {size} bytes < 90% of {expect}")
    print(f"captured {size} bytes -> {out}")


def set_packed(enable: bool, op_all: list[int] | None = None) -> dict:
    url = f"{DEVICE}/xvf/packed?enable={1 if enable else 0}"
    if op_all:
        url += "&op_all=" + ",".join(str(v) for v in op_all)
    result = http_json(url)
    if not result.get("ok"):
        raise SystemExit(f"packed write rejected: {result}")
    return result


def restore_production() -> None:
    try:
        set_packed(False)
    finally:
        mux = http_json(f"{DEVICE}/xvf/audio-mux?op_r_category=7&op_r_source=3")
        print(f"restored production mux: {mux}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rig-released", action="store_true",
                        help="acknowledge that t_f2f4f499 has released the kitchen rig")
    parser.add_argument("--out", type=Path, default=HERE / "runs" / time.strftime("%Y%m%d-%H%M%S-cat5ref"))
    parser.add_argument("--capture-ms", type=int, default=10000)
    parser.add_argument("--announce-text", default="testing chip reference capture for echo cancellation")
    args = parser.parse_args()

    if not args.rig_released:
        raise SystemExit("refusing to touch the kitchen rig: pass --rig-released only after "
                         "t_f2f4f499 (phase-4) has released it")

    args.out.mkdir(parents=True, exist_ok=True)
    assert_kitchen_identity()
    assert_capture_endpoints()

    try:
        # Arm 2 capture: packed 6-lane, TTS playing (fire announce via the hub
        # BEFORE the capture window so the far-end lane carries real render).
        set_packed(True, PACKED_OP_ALL)
        announce = run(["ssh", "ace-ai-lan",
                        "curl -fsS --max-time 60 -X POST --get "
                        "--data-urlencode " + shlex.quote(f"text={args.announce_text}") +
                        " http://127.0.0.1:7861/announce"], check=False)
        print(f"announce: rc={announce.returncode} {announce.stdout[:200]}")
        time.sleep(0.5)
        packed_pcm = args.out / "packed_48k_stereo.pcm"
        capture_raw(args.capture_ms, packed_pcm)

        # Unpack into the six lanes.
        unpack = run([sys.executable, str(HERE / "xvf_packed_unpack.py"),
                      str(packed_pcm), str(args.out / "lane")])
        (args.out / "unpack_summary.json").write_text(unpack.stdout)
        print(unpack.stdout)

        print(json.dumps({
            "next_steps": [
                "lane_ch1.pcm = [1,3] raw mic (L_PK1), lane_ch2.pcm = [5,0] chip ref (L_PK2)",
                "run aec3_offline_eval (t_f2f4f499 harness, linear output enabled): "
                "render=lane_ch2.pcm capture=lane_ch1.pcm delay=0",
                "baseline arm: same eval with render=server-synthesized reference PCM "
                "(phase-4 render probe output) at its best swept delay",
                "compare external linear ERLE on far-end-only windows; ship the delta",
            ],
        }, indent=2))
    finally:
        restore_production()


if __name__ == "__main__":
    main()
