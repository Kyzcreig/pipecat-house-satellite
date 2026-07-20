"""Regression tests for the D-43 fixed-rate load certificate."""
from __future__ import annotations

import importlib.util
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/d43_load_cert.py"


def load_module():
    assert SCRIPT.is_file(), "the reviewed D-43 harness must be committed under scripts/"
    spec = importlib.util.spec_from_file_location("d43_load_cert", SCRIPT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class FakeClock:
    def __init__(self, tick_s: float = 0.001) -> None:
        self.now = 0.0
        self.tick_s = tick_s

    def monotonic(self) -> float:
        self.now += self.tick_s
        return self.now

    def advance(self, seconds: float) -> None:
        self.now += seconds


def test_fixed_rate_driver_emits_every_20_hz_slot_without_sleep() -> None:
    module = load_module()
    clock = FakeClock()
    starts = []

    def post():
        starts.append(clock.now)
        clock.advance(0.002)
        return {"http_status": 200, "body": {"ok": True, "sent": 1}}

    results, schedule = module.run_fixed_rate(
        post, load_s=1.0, rate_hz=20.0, monotonic=clock.monotonic
    )

    assert len(results) == 20
    assert schedule["target_slots"] == 20
    assert schedule["skipped_slots"] == 0
    intervals = [later - earlier for earlier, later in zip(starts, starts[1:])]
    assert intervals
    assert all(0.045 <= interval <= 0.055 for interval in intervals)


def test_fixed_rate_driver_skips_missed_slots_instead_of_bursting() -> None:
    module = load_module()
    clock = FakeClock()
    starts = []

    def slow_post():
        starts.append(clock.now)
        clock.advance(0.12)
        return {"http_status": 200, "body": {"ok": True, "sent": 1}}

    results, schedule = module.run_fixed_rate(
        slow_post, load_s=1.0, rate_hz=20.0, monotonic=clock.monotonic
    )

    assert 1 <= len(results) < 20
    assert schedule["skipped_slots"] > 0
    intervals = [later - earlier for earlier, later in zip(starts, starts[1:])]
    assert intervals
    assert min(intervals) >= 0.10


def test_rtvi_drop_delta_must_remain_zero_for_a_clean_gate() -> None:
    module = load_module()

    assert module.rtvi_drop_delta(
        {"rtvi_rx_dropped": 7}, {"rtvi_rx_dropped": 7}
    ) == 0
    assert module.rtvi_drop_delta(
        {"rtvi_rx_dropped": 7}, {"rtvi_rx_dropped": 9}
    ) == 2


def test_no_apply_is_a_network_dark_plan(tmp_path: Path) -> None:
    out = tmp_path / "receipt.json"
    completed = subprocess.run(
        [
            sys.executable,
            str(SCRIPT),
            "--device",
            "http://127.0.0.1:1",
            "--server",
            "http://127.0.0.1:1",
            "--expected-sha",
            "0" * 64,
            "--room",
            "theater",
            "--out",
            str(out),
        ],
        check=False,
        capture_output=True,
        text=True,
        timeout=5,
    )

    assert completed.returncode == 0, completed.stderr
    assert "PLAN" in completed.stdout
    assert not out.exists()
