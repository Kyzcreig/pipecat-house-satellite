#!/usr/bin/env python3
"""One-shot leg-B-alt capture: [4,0]/[5,0] far-end pair + [3,0]/[11,0] mic pair."""
from __future__ import annotations
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_lane_acoustic_legs import (assert_identity, captured_leg, set_packed,
                                    http_json, DEVICE)
import json, time

# L_PK0=[4,0] far-end no delay, L_PK1=[5,0] far-end + delay, L_PK2 silence
# R_PK0=[3,0] amp mic + delay,  R_PK1=[11,0] amp mic no delay, R_PK2 silence
OP_ALL = [4, 0, 5, 0, 0, 0, 3, 0, 11, 0, 0, 0]

if "--rig-released" not in sys.argv:
    raise SystemExit("pass --rig-released")
out_dir = Path(__file__).resolve().parent / "runs" / time.strftime("%Y%m%d-%H%M%S-legBalt")
out_dir.mkdir(parents=True, exist_ok=True)
assert_identity()
try:
    result = captured_leg(out_dir, "legBalt", OP_ALL, "abtest_full", 9000)
    print(json.dumps({k: {c: v["rms_s16"] for c, v in s["channels"].items()}
                      for k, s in result["slots"].items()}, indent=1))
finally:
    set_packed(False)
    mux = http_json(f"{DEVICE}/xvf/audio-mux?op_l_category=7&op_l_source=3"
                    f"&op_r_category=7&op_r_source=3")
    print(f"restored mux: {mux}")
