#!/usr/bin/env python3
"""Leg A scorer: chip-ref vs synth-ref AEC3 A/B on the same raw-mic capture.

Runs on ACE-AI next to the phase-4 evaluator. Metric: external linear ERLE
(rms(raw)/rms(linear), dB) over far-end-active windows, defined as 100 ms
blocks where the RENDER signal has meaningful energy. Never reads APM stats.
"""
import json
import math
import struct
import subprocess
import sys
from pathlib import Path

EVAL = "/home/ace/raw-tap-t_f2f4f499/analysis/aec3_offline_eval"
ROOT = Path("/home/ace/lane-exp-t_2ccb0829")
RATE = 16000
BLOCK = RATE // 10  # 100 ms


def read_pcm(path: Path) -> list[int]:
    data = path.read_bytes()
    return list(struct.unpack(f"<{len(data)//2}h", data[: len(data) - len(data) % 2]))


def rms(vals) -> float:
    return math.sqrt(sum(v * v for v in vals) / len(vals)) if vals else 0.0


def active_blocks(render: list[int], threshold_frac: float = 0.05) -> list[int]:
    peak = max(rms(render[i:i + BLOCK]) for i in range(0, len(render) - BLOCK, BLOCK))
    thr = peak * threshold_frac
    return [i for i in range(0, len(render) - BLOCK, BLOCK)
            if rms(render[i:i + BLOCK]) > thr]


def erle_db(raw: list[int], linear: list[int], blocks: list[int]) -> float:
    r = rms([v for b in blocks for v in raw[b:b + BLOCK]])
    l = rms([v for b in blocks for v in linear[b:b + BLOCK]])
    if r <= 0 or l <= 0:
        return float("nan")
    return 20.0 * math.log10(r / l)


def run_eval(render: Path, capture: Path, delay_ms: int, tag: str) -> dict:
    cleaned = ROOT / f"{tag}-cleaned.pcm"
    linear = ROOT / f"{tag}-linear.pcm"
    proc = subprocess.run([EVAL, str(render), str(capture), str(cleaned),
                           str(delay_ms), str(linear)],
                          capture_output=True, text=True)
    if proc.returncode != 0:
        raise SystemExit(f"evaluator failed ({tag}): {proc.stderr}")
    raw = read_pcm(capture)
    lin = read_pcm(linear)
    ren = read_pcm(render)
    n = min(len(raw), len(lin))
    blocks = [b for b in active_blocks(ren) if b + BLOCK <= n]
    return {
        "tag": tag,
        "delay_ms": delay_ms,
        "aec_stdout": proc.stdout.strip(),
        "active_blocks": len(blocks),
        "raw_rms_active": round(rms([v for b in blocks for v in raw[b:b + BLOCK]]), 3),
        "linear_rms_active": round(rms([v for b in blocks for v in lin[b:b + BLOCK]]), 3),
        "linear_erle_db": round(erle_db(raw[:n], lin[:n], blocks), 3),
        "final_erle_db": round(erle_db(raw[:n], read_pcm(cleaned)[:n], blocks), 3),
    }


def main() -> None:
    capture = ROOT / "legAC-a2-lane_ch1.pcm"   # [1,3] raw mic
    chip_ref = ROOT / "legAC-a2-lane_ch2.pcm"  # [5,0] chip far-end ref
    asr_lane = ROOT / "legAC-a2-lane_ch0.pcm"  # [7,3] production ASR (context)
    synth_ref = ROOT / "abtest_full.pcm"       # exact fixture the server played

    results: dict = {"capture": str(capture)}
    # Arm 2: chip reference, shared clock => small sweep around 0 for rigor
    arm2 = [run_eval(chip_ref, capture, d, f"chipref-d{d}") for d in (0, 10, 20)]
    results["arm2_chip_ref"] = sorted(arm2, key=lambda r: -r["linear_erle_db"])
    # Arm 1: synthesized reference at swept delays
    arm1 = [run_eval(synth_ref, capture, d, f"synthref-d{d}")
            for d in (0, 50, 100, 150, 200, 250, 300, 400, 500)]
    results["arm1_synth_ref"] = sorted(arm1, key=lambda r: -r["linear_erle_db"])
    # Context: how echo-y is each lane during playback
    ren = read_pcm(chip_ref)
    blocks = active_blocks(ren)
    for name, path in (("raw_mic_1_3", capture), ("chip_ref_5_0", chip_ref),
                       ("asr_7_3", asr_lane)):
        vals = read_pcm(path)
        act = [v for b in blocks if b + BLOCK <= len(vals) for v in vals[b:b + BLOCK]]
        r = rms(act)
        results[f"lane_{name}_active_rms"] = round(r, 3)
        results[f"lane_{name}_active_dbfs"] = round(
            20 * math.log10(r / 32768), 2) if r > 0 else -330.0
    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
