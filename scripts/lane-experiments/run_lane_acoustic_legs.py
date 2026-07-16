#!/usr/bin/env python3
"""Acoustic legs A/B/C orchestrator (t_2ccb0829), kitchen rig.

Leg C (bench packed capture) + Leg A capture arm: packed OP_ALL carries
production ASR [7,3], raw mic [1,3], chip far-end ref [5,0] simultaneously
while a KNOWN fixture PCM plays via /test-tone. The fixture file IS the exact
server-side reference (no render probe needed), so:
  arm-1 (baseline synth ref): AEC3(render=fixture.pcm swept, capture=[1,3])
  arm-2 (chip ref)          : AEC3(render=[5,0] lane, capture=[1,3], lag 0)
Leg B: packed OP_ALL carries [3,0] amplified+delay and [11,0] amplified
no-delay on one clock while a fixture plays; xcorr gives the system delay.

Mitigations from the first real capture (runs/20260716-140650-cat5ref):
  * first ~200 ms after capture start is phase-unstable -> trim lead
  * a dropped 48k sample mid-capture rotates TDM phase (seen at ~6.1 s in a
    15 s capture) -> continuity gate fails closed; we RETRY up to 3x
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
KITCHEN_MAC = "dc:b4:d9:38:a6:cc"
HERE = Path(__file__).resolve().parent
TRIM_LEAD_BYTES = 9600 * 8  # 200 ms of stereo 48k/32-bit frames
SOUNDS = "/home/ace/projects/voice/pipecat-house-voice-git/server/sounds"

# Leg A/C: L = [7,3] ASR, [1,3] raw mic4, [5,0] chip ref; R = redundant copy
OP_ALL_LEG_A = [7, 3, 1, 3, 5, 0, 7, 3, 1, 3, 5, 0]
# Leg B: L = [3,0] amplified+delay, [11,0] amplified no-delay, [0,0]; R prod
OP_ALL_LEG_B = [3, 0, 11, 0, 0, 0, 7, 3, 0, 0, 0, 0]


def run(cmd: list[str], *, check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(cmd, check=check, capture_output=True, text=True)


def http_json(url: str) -> dict:
    return json.loads(run(["curl", "-fsS", "--max-time", "15", url]).stdout)


def assert_identity() -> None:
    status = http_json(f"{DEVICE}/ota/status")
    if status.get("satellite_id") != "kitchen":
        raise SystemExit(f"identity gate: not kitchen: {status}")
    neigh = run(["ssh", "ace-ai-lan",
                 "ping -c1 192.168.1.185 >/dev/null; ip neigh show 192.168.1.185"]).stdout
    if KITCHEN_MAC not in neigh:
        raise SystemExit(f"identity gate: MAC mismatch: {neigh.strip()}")
    print(f"identity OK: kitchen {KITCHEN_MAC} fw {status.get('firmware_version')}")


def set_packed(enable: bool, op_all: list[int] | None = None) -> dict:
    url = f"{DEVICE}/xvf/packed?enable={1 if enable else 0}"
    if op_all:
        url += "&op_all=" + ",".join(str(v) for v in op_all)
    result = http_json(url)
    if not result.get("ok"):
        raise SystemExit(f"packed write rejected: {result}")
    return result


def fire_fixture(sound: str) -> dict:
    out = run(["ssh", "ace-ai-lan",
               "curl -fsS --max-time 30 -X POST --get "
               f"--data-urlencode 'sound={sound}' http://127.0.0.1:7861/test-tone"]).stdout
    resp = json.loads(out)
    if resp.get("ok") is not True or not resp.get("peers_sent"):
        raise SystemExit(f"test-tone failed: {resp}")
    return resp


def capture_trimmed(ms: int, out: Path) -> None:
    tmp = out.with_suffix(".raw-untrimmed")
    run(["curl", "-fsS", "--max-time", str(ms // 1000 + 20), "-o", str(tmp),
         f"{DEVICE}/xvf/raw-capture?ms={ms}"])
    data = tmp.read_bytes()
    expect = int(ms / 1000 * 48000 * 2 * 4)
    if len(data) < expect * 0.9:
        raise SystemExit(f"short capture: {len(data)} < 90% of {expect}")
    trimmed = data[TRIM_LEAD_BYTES:]
    trimmed = trimmed[: len(trimmed) - (len(trimmed) % 8)]
    out.write_bytes(trimmed)
    tmp.unlink()
    print(f"captured {len(data)} bytes -> trimmed {len(trimmed)} -> {out}")


def unpack(pcm: Path, prefix: Path) -> dict:
    proc = run([sys.executable, str(HERE / "xvf_packed_unpack.py"),
                str(pcm), str(prefix)], check=False)
    if proc.returncode != 0:
        raise RuntimeError(f"unpack rejected: {proc.stdout.strip()}{proc.stderr.strip()}")
    return json.loads(proc.stdout)


def captured_leg(out_dir: Path, label: str, op_all: list[int], sound: str,
                 capture_ms: int, attempts: int = 3) -> dict:
    for attempt in range(1, attempts + 1):
        state = set_packed(True, op_all)
        time.sleep(0.3)
        tone = fire_fixture(sound)
        time.sleep(0.2)
        pcm = out_dir / f"{label}-a{attempt}.pcm"
        capture_trimmed(capture_ms, pcm)
        try:
            summary = unpack(pcm, out_dir / f"{label}-a{attempt}-lane")
        except RuntimeError as exc:
            print(f"{label} attempt {attempt}: {exc}; retrying")
            continue
        summary["op_all_readback"] = state.get("op_all")
        summary["tone"] = tone
        summary["attempt"] = attempt
        (out_dir / f"{label}-unpack.json").write_text(json.dumps(summary, indent=2))
        return summary
    raise SystemExit(f"{label}: no phase-clean capture in {attempts} attempts")


def main() -> None:
    if "--rig-released" not in sys.argv:
        raise SystemExit("pass --rig-released only after t_f2f4f499 released the rig")
    out_dir = HERE / "runs" / time.strftime("%Y%m%d-%H%M%S-acoustic")
    out_dir.mkdir(parents=True, exist_ok=True)
    assert_identity()
    results: dict = {"out_dir": str(out_dir)}
    try:
        # Leg B: system-delay measurement (cheap; run first)
        results["leg_b"] = captured_leg(
            out_dir, "legB", OP_ALL_LEG_B, "timer_finished", 6000)
        # Leg A/C: raw mic + chip ref during a long fixture
        results["leg_ac"] = captured_leg(
            out_dir, "legAC", OP_ALL_LEG_A, "abtest_full", 12000)
    finally:
        set_packed(False)
        mux = http_json(f"{DEVICE}/xvf/audio-mux?op_l_category=7&op_l_source=3"
                        f"&op_r_category=7&op_r_source=3")
        print(f"restored mux: {mux}")
    (out_dir / "legs_results.json").write_text(json.dumps(results, indent=2))
    print(f"results -> {out_dir}/legs_results.json")


if __name__ == "__main__":
    main()
