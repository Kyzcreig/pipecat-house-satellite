#!/usr/bin/env python3
"""Empirical packed-lane mapping tracer (t_2ccb0829).

Writes OP_ALL with cat-0 digital silence in exactly ONE byte-pair position per
capture; whichever measured (slot, mod-3 phase) lane goes to exact zeros IS
that logical position. Three captures pin: PK0 phase, slot-major vs
interleaved byte order, and lane ordering direction. All other positions carry
[3,0] amplified mic (ambient room noise, clearly nonzero).

Restores packed=0 + production mux in finally.
"""
from __future__ import annotations

import json
import struct
import subprocess
import sys
import time
from pathlib import Path

DEVICE = "http://192.168.1.185"
HERE = Path(__file__).resolve().parent
MIC = [3, 0]
SIL = [0, 0]


def http_json(url: str) -> dict:
    out = subprocess.run(["curl", "-fsS", "--max-time", "15", url],
                         check=True, capture_output=True, text=True).stdout
    return json.loads(out)


def set_packed(enable: bool, op_all: list[int] | None = None) -> dict:
    url = f"{DEVICE}/xvf/packed?enable={1 if enable else 0}"
    if op_all:
        url += "&op_all=" + ",".join(str(v) for v in op_all)
    result = http_json(url)
    if not result.get("ok"):
        raise SystemExit(f"packed write rejected: {result}")
    return result


def capture(ms: int, out: Path) -> None:
    subprocess.run(["curl", "-fsS", "--max-time", str(ms // 1000 + 20),
                    "-o", str(out), f"{DEVICE}/xvf/raw-capture?ms={ms}"],
                   check=True)
    size = out.stat().st_size
    expect = int(ms / 1000 * 48000 * 2 * 4)
    if size < expect * 0.9:
        raise SystemExit(f"short capture: {size} < 90% of {expect}")


def lane_zero_fracs(path: Path) -> dict:
    raw = path.read_bytes()
    raw = raw[: len(raw) - (len(raw) % 8)]
    flat = struct.unpack(f"<{len(raw)//4}i", raw)
    slots = {"L": list(flat[0::2]), "R": list(flat[1::2])}
    report = {}
    for name, samples in slots.items():
        for pos in range(3):
            vals = [(v >> 16) for v in samples[pos::3]]
            zf = sum(1 for v in vals if v == 0) / len(vals)
            report[f"{name}pos{pos}"] = round(zf, 4)
    return report


def main() -> None:
    if "--rig-released" not in sys.argv:
        raise SystemExit("pass --rig-released (t_2ccb0829 owns the kitchen rig window)")
    out_dir = HERE / "runs" / time.strftime("%Y%m%d-%H%M%S-tracer")
    out_dir.mkdir(parents=True, exist_ok=True)

    # silence at byte-pair index: 0 (both orders: L_PK0), 1 (slot-major:
    # L_PK1 / interleaved: R_PK0), 2 (slot-major: L_PK2 / interleaved: L_PK1),
    # 5 (slot-major: R_PK2 / interleaved: R_PK2)
    trials = {
        "sil_at_pair0": SIL + MIC * 5,
        "sil_at_pair1": MIC + SIL + MIC * 4,
        "sil_at_pair2": MIC * 2 + SIL + MIC * 3,
        "sil_at_pair5": MIC * 5 + SIL,
    }
    results = {}
    try:
        for label, op_all in trials.items():
            state = set_packed(True, op_all)
            time.sleep(0.3)
            pcm = out_dir / f"{label}.pcm"
            capture(4000, pcm)
            zf = lane_zero_fracs(pcm)
            quiet = [k for k, v in zf.items() if v > 0.99]
            results[label] = {"op_all": op_all, "readback": state.get("op_all"),
                              "zero_fracs": zf, "silent_lanes": quiet}
            print(f"{label}: silent={quiet} zf={zf}")
    finally:
        set_packed(False)
        mux = http_json(f"{DEVICE}/xvf/audio-mux?op_l_category=7&op_l_source=3"
                        f"&op_r_category=7&op_r_source=3")
        print(f"restored mux: {mux}")
    (out_dir / "tracer_results.json").write_text(json.dumps(results, indent=2))
    print(f"results -> {out_dir}/tracer_results.json")


if __name__ == "__main__":
    main()
