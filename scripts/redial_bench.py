#!/usr/bin/env python3
"""GADGET-2 bench measurement: re-offer without esp_restart() (t_db77e56b).

Drives the BENCH satellite (.97) running a PIPECAT_REDIAL=1 build through the
two legs the vault note's pass criterion names (Muse/Clanker comparison note,
shortlist #2, line 151):

  A. ``--hub-restarts N``   `systemctl --user restart clanker-webrtc` on the hub
                            N times; per restart, the clock runs from the
                            restart command to the first `/playback/stats`
                            `frames` increment after a `/test-tone` lands on
                            the re-dialed peer (audio actually flowing, not
                            just CONNECTED). Also records trigger -> CONNECTED
                            from the device's own `redial` telemetry.
  B. ``--redial-cycles N``  POST /webrtc/redial on the satellite N times (hub
                            untouched): the heap-leak soak. Records internal
                            free / min-free / largest-DMA / PSRAM after every
                            cycle.

Pass (note, line 151): leg A median < 10 s, and fault_boots / netwdt_restarts
/ boots_since_poweron unchanged across the run; leg B heap flat (first-10 vs
last-10 mean of heap_free_int within --heap-slack bytes, and largest DMA block
not trending down).

Plain mode (no --apply) prints the plan and the current device/hub state and
exits 0. Every leg posts a start and a stop line to the home chat via
notify.py (operator rule: bench audio/reboots are 09:00-22:00 PT only, and the
house is told). --no-notify skips that for a dry run.

Writes a JSON ledger (--out) with every sample so the decision row can cite it.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import statistics
import subprocess
import sys
import time
import urllib.error
import urllib.request
import zoneinfo
from pathlib import Path

PT = zoneinfo.ZoneInfo("America/Los_Angeles")
NOTIFY = Path.home() / ".hermes" / "scripts" / "notify.py"


def _get(url: str, timeout: float = 3.0) -> dict | None:
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            return json.loads(r.read().decode())
    except (urllib.error.URLError, TimeoutError, ValueError, OSError):
        return None


def _post(url: str, timeout: float = 5.0) -> dict | None:
    req = urllib.request.Request(url, method="POST", data=b"")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read().decode())
    except (urllib.error.URLError, TimeoutError, ValueError, OSError):
        return None


class Bench:
    def __init__(self, sat: str, hub: str, hub_ssh: str, unit: str) -> None:
        self.sat = f"http://{sat}"
        self.hub = f"http://{hub}"
        self.hub_ssh = hub_ssh
        self.unit = unit

    # device
    def params(self) -> dict | None:
        return _get(f"{self.sat}/xvf/params")

    def stats(self) -> dict | None:
        return _get(f"{self.sat}/playback/stats")

    def redial(self) -> dict | None:
        return _post(f"{self.sat}/webrtc/redial")

    def boot_counters(self) -> dict | None:
        p = self.params()
        if not p or "boot_guard" not in p:
            return None
        bg = p["boot_guard"]
        return {
            "fault_boots": bg["fault_boots"],
            "netwdt_restarts": bg["netwdt_restarts"],
            "boots_since_poweron": bg["boots_since_poweron"],
            "crash_boots": bg["crash_boots"],
            "uptime_s": bg["uptime_s"],
            "git_sha": p.get("build", {}).get("git_sha"),
        }

    # hub
    def health(self) -> dict | None:
        return _get(f"{self.hub}/health")

    def active_peers(self) -> int:
        h = self.health()
        return int(h.get("active_peers", 0)) if h else -1

    def restart_hub(self) -> None:
        subprocess.run(
            ["ssh", "-o", "ConnectTimeout=8", self.hub_ssh,
             f"systemctl --user restart {self.unit}"],
            check=True, timeout=60,
        )

    def test_tone(self) -> dict | None:
        return _post(f"{self.hub}/test-tone?freq=880&ms=300&volume=0.3")


def notify(text: str, enabled: bool) -> None:
    print(f"[notify] {text}", flush=True)
    if not enabled:
        return
    env = dict(os.environ, HERMES_NOTIFY_REAL="1")
    r = subprocess.run(
        [sys.executable, str(NOTIFY), "--send", text, "--channel", "telegram"],
        env=env, capture_output=True, text=True, timeout=30,
    )
    if r.returncode != 0:
        print(f"NOTIFY FAILED rc={r.returncode} {r.stderr.strip()}", file=sys.stderr)
        sys.exit(9)


def now_pt() -> dt.datetime:
    return dt.datetime.now(PT)


def daytime_ok() -> bool:
    h = now_pt().hour
    return 9 <= h < 22


def wait_for(pred, timeout_s: float, period_s: float = 0.25) -> float | None:
    t0 = time.monotonic()
    while time.monotonic() - t0 < timeout_s:
        if pred():
            return time.monotonic() - t0
        time.sleep(period_s)
    return None


def leg_hub_restarts(b: Bench, n: int, ledger: dict, timeout_s: float) -> None:
    samples: list[dict] = []
    for i in range(1, n + 1):
        before = b.params() or {}
        gen0 = before.get("redial", {}).get("generation", -1)
        boots0 = before.get("boot_guard", {}).get("boots_since_poweron", -1)
        frames0 = (b.stats() or {}).get("frames", -1)
        t_cmd = time.monotonic()
        wall = now_pt().isoformat(timespec="seconds")
        b.restart_hub()
        t_restart_done = time.monotonic() - t_cmd

        # 1. the hub is back (health 200)
        t_hub_up = wait_for(lambda: b.health() is not None, timeout_s)
        # 2. the satellite re-dialed: generation bumped and CONNECTED
        def _connected() -> bool:
            p = b.params()
            r = (p or {}).get("redial", {})
            return bool(r.get("connected")) and r.get("generation", -1) > gen0
        t_connected = wait_for(_connected, timeout_s)
        # 3. audio actually flows: a test-tone lands and frames increments
        t_frames: float | None = None
        if t_connected is not None:
            # the hub counts the peer only once its pipeline task is up
            wait_for(lambda: b.active_peers() >= 1, 15.0)
            t_tone = time.monotonic()
            while time.monotonic() - t_cmd < timeout_s:
                b.test_tone()
                got = wait_for(lambda: (b.stats() or {}).get("frames", -1) > frames0, 2.0)
                if got is not None:
                    t_frames = time.monotonic() - t_cmd
                    break
            ledger.setdefault("tone_waits", []).append(time.monotonic() - t_tone)
        after = b.params() or {}
        r = after.get("redial", {})
        s = {
            "i": i, "wall_pt": wall,
            "restart_cmd_s": round(t_restart_done, 3),
            "hub_health_s": None if t_hub_up is None else round(t_hub_up, 3),
            "connected_s": None if t_connected is None else round(t_connected, 3),
            "first_frames_s": None if t_frames is None else round(t_frames, 3),
            "device_trigger": r.get("last_trigger"),
            "device_redial_to_connected_ms": r.get("last_redial_to_connected_ms"),
            "device_attempts_total": r.get("attempts_total"),
            "generation": r.get("generation"),
            "boots_since_poweron": after.get("boot_guard", {}).get("boots_since_poweron"),
            "rebooted": after.get("boot_guard", {}).get("boots_since_poweron", -1) != boots0,
            "heap_free_int": r.get("heap_free_int"),
            "heap_largest_dma": r.get("heap_largest_dma"),
        }
        samples.append(s)
        print(json.dumps(s), flush=True)
        # let the session settle (30 s stable-session reset, hub ping cadence)
        time.sleep(35)
    ledger["hub_restarts"] = samples


def leg_redial_cycles(b: Bench, n: int, ledger: dict, timeout_s: float) -> None:
    samples: list[dict] = []
    for i in range(1, n + 1):
        before = (b.params() or {}).get("redial", {})
        gen0 = before.get("generation", -1)
        t0 = time.monotonic()
        wall = now_pt().isoformat(timespec="seconds")
        kicked = b.redial()
        def _connected() -> bool:
            r = (b.params() or {}).get("redial", {})
            return bool(r.get("connected")) and r.get("generation", -1) > gen0
        t_connected = wait_for(_connected, timeout_s)
        time.sleep(2.0)  # let teardown frees settle before the heap read
        r = (b.params() or {}).get("redial", {})
        s = {
            "i": i, "wall_pt": wall, "kicked": bool(kicked and kicked.get("ok")),
            "connected_s": None if t_connected is None else round(t_connected, 3),
            "device_redial_to_connected_ms": r.get("last_redial_to_connected_ms"),
            "generation": r.get("generation"),
            "consecutive_failures": r.get("consecutive_failures"),
            "heap_free_int": r.get("heap_free_int"),
            "heap_min_free_int": r.get("heap_min_free_int"),
            "heap_largest_int": r.get("heap_largest_int"),
            "heap_largest_dma": r.get("heap_largest_dma"),
            "heap_free_psram": r.get("heap_free_psram"),
            "hub_active_peers": b.active_peers(),
        }
        samples.append(s)
        print(json.dumps(s), flush=True)
        time.sleep(3.0)
    ledger["redial_cycles"] = samples


def verdict(ledger: dict, heap_slack: int) -> dict:
    v: dict = {}
    hr = ledger.get("hub_restarts") or []
    if hr:
        ff = [s["first_frames_s"] for s in hr if s["first_frames_s"] is not None]
        cc = [s["connected_s"] for s in hr if s["connected_s"] is not None]
        v["hub_restarts_n"] = len(hr)
        v["first_frames_ok_n"] = len(ff)
        v["first_frames_median_s"] = round(statistics.median(ff), 2) if ff else None
        v["first_frames_max_s"] = round(max(ff), 2) if ff else None
        v["connected_median_s"] = round(statistics.median(cc), 2) if cc else None
        v["reboots_during_restarts"] = sum(1 for s in hr if s["rebooted"])
        v["pass_median_lt_10s"] = bool(ff) and len(ff) == len(hr) and statistics.median(ff) < 10.0
    rc = ledger.get("redial_cycles") or []
    if rc:
        ok = [s for s in rc if s["connected_s"] is not None]
        hf = [s["heap_free_int"] for s in rc if s["heap_free_int"] is not None]
        dma = [s["heap_largest_dma"] for s in rc if s["heap_largest_dma"] is not None]
        v["redial_cycles_n"] = len(rc)
        v["redial_connected_n"] = len(ok)
        if len(hf) >= 20:
            head, tail = statistics.mean(hf[:10]), statistics.mean(hf[-10:])
            v["heap_free_int_first10_mean"] = round(head)
            v["heap_free_int_last10_mean"] = round(tail)
            v["heap_free_int_drift"] = round(tail - head)
            v["heap_largest_dma_first"] = dma[0] if dma else None
            v["heap_largest_dma_last"] = dma[-1] if dma else None
            v["pass_heap_flat"] = abs(tail - head) <= heap_slack and (not dma or dma[-1] >= dma[0] - heap_slack)
        else:
            v["pass_heap_flat"] = None
    b0, b1 = ledger.get("boot_before"), ledger.get("boot_after")
    if b0 and b1:
        v["fault_boots_delta"] = b1["fault_boots"] - b0["fault_boots"]
        v["netwdt_restarts_delta"] = b1["netwdt_restarts"] - b0["netwdt_restarts"]
        v["boots_delta"] = b1["boots_since_poweron"] - b0["boots_since_poweron"]
        v["pass_no_reboots"] = v["fault_boots_delta"] == 0 and v["netwdt_restarts_delta"] == 0 and v["boots_delta"] == 0
    gates = [v.get(k) for k in ("pass_median_lt_10s", "pass_heap_flat", "pass_no_reboots") if k in v]
    v["PASS"] = bool(gates) and all(g is True for g in gates)
    return v


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sat", default="192.168.1.97", help="bench satellite (OTA/HTTP :80)")
    ap.add_argument("--hub", default="192.168.1.216:7860", help="bench hub /health, /test-tone")
    ap.add_argument("--hub-ssh", default="ace-ai")
    ap.add_argument("--unit", default="clanker-webrtc", help="bench room systemd --user unit")
    ap.add_argument("--hub-restarts", type=int, default=10)
    ap.add_argument("--redial-cycles", type=int, default=50)
    ap.add_argument("--timeout", type=float, default=90.0, help="per-event wait (s)")
    ap.add_argument("--heap-slack", type=int, default=4096, help="bytes of drift still 'flat'")
    ap.add_argument("--out", default=None, help="ledger JSON path")
    ap.add_argument("--apply", action="store_true", help="actually drive the bench")
    ap.add_argument("--no-notify", action="store_true")
    ap.add_argument("--allow-night", action="store_true", help="override the 09:00-22:00 PT window (operator only)")
    a = ap.parse_args()

    b = Bench(a.sat, a.hub, a.hub_ssh, a.unit)
    p = b.params()
    h = b.health()
    print("satellite:", json.dumps({k: p.get(k) for k in ("build", "boot_guard", "redial")} if p else None))
    print("hub:", json.dumps({k: h.get(k) for k in ("room", "active_peers", "peer_age_s")} if h else None))
    if not p or "redial" not in p:
        print("REFUSE: satellite has no `redial` object on /xvf/params -> not a PIPECAT_REDIAL=1 build", file=sys.stderr)
        return 2
    if not a.apply:
        print(f"plan: {a.hub_restarts} hub restarts of {a.unit} on {a.hub_ssh}, then {a.redial_cycles} "
              f"POST /webrtc/redial cycles on {a.sat}; pass = median first-frames < 10 s, 0 reboots, heap flat.")
        return 0
    if not daytime_ok() and not a.allow_night:
        print(f"REFUSE: {now_pt():%H:%M} PT is outside 09:00-22:00 (bench is in the bedroom until S2)", file=sys.stderr)
        return 3

    ledger: dict = {"started_pt": now_pt().isoformat(timespec="seconds"), "args": vars(a)}
    ledger["boot_before"] = b.boot_counters()
    out = Path(a.out or f"/tmp/gadget2-redial-{now_pt():%Y%m%d-%H%M}.json")
    notify(f"GADGET-2 bench START (t_db77e56b): {a.hub_restarts} hub restarts + {a.redial_cycles} "
           f"re-dial cycles on bench .97; a short 880 Hz blip per restart. ~{a.hub_restarts * 1 + a.redial_cycles * 0.2:.0f} min.",
           not a.no_notify)
    try:
        if a.hub_restarts:
            leg_hub_restarts(b, a.hub_restarts, ledger, a.timeout)
        if a.redial_cycles:
            leg_redial_cycles(b, a.redial_cycles, ledger, a.timeout)
    finally:
        ledger["boot_after"] = b.boot_counters()
        ledger["ended_pt"] = now_pt().isoformat(timespec="seconds")
        ledger["verdict"] = verdict(ledger, a.heap_slack)
        out.write_text(json.dumps(ledger, indent=1))
        print("verdict:", json.dumps(ledger["verdict"], indent=1))
        print("ledger:", out)
        notify(f"GADGET-2 bench STOP: {'PASS' if ledger['verdict'].get('PASS') else 'FAIL'} "
               f"median first-frames {ledger['verdict'].get('first_frames_median_s')} s, "
               f"reboots {ledger['verdict'].get('boots_delta')}, heap drift {ledger['verdict'].get('heap_free_int_drift')} B. "
               f"ledger {out}", not a.no_notify)
    return 0 if ledger["verdict"].get("PASS") else 1


if __name__ == "__main__":
    sys.exit(main())
