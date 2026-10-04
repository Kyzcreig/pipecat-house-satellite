#!/usr/bin/env python3
"""GADGET-2 measurement: re-offer without esp_restart() (t_db77e56b).

Runs ON THE HUB HOST (ACE-AI; the satellite :80 and the hub :786x are LAN-only
there) against a room satellite running a PIPECAT_REDIAL=1 build, through the
legs the vault note's pass criterion names (Muse/Clanker comparison note,
shortlist #2, line 151) plus the sat#27 repro Apollo attached to this card:

  S. ``--story SECONDS``    chained SILENT /test-tone chunks (20 Hz at volume
                            0.0: zero PCM, still Opus RTP) so downlink media
                            flows continuously for SECONDS (the UPLINK-STALL /
                            t_56a17737 "long story" shape: ping gaps under
                            sustained downlink). Reads the sat#27 /ota/status fields
                            server_ping_rx / server_ping_gap_max_ms /
                            media_liveness_holds before and after; pass = no
                            reboot during the story.
  A. ``--hub-restarts N``   `systemctl --user restart <unit>` N times; per
                            restart the clock runs from the restart command to
                            the first `/playback/stats` `frames` increment
                            after a SILENT `/test-tone` lands on the re-dialed
                            peer (audio path actually flowing, not just
                            CONNECTED; the room hears nothing). Also
                            records trigger -> CONNECTED from the device's own
                            `redial` telemetry.
  B. ``--redial-cycles N``  POST /webrtc/redial on the satellite N times (hub
                            untouched): the heap-leak soak. Records internal
                            free / min-free / largest-DMA / PSRAM after every
                            cycle.

Pass (note, line 151): leg A median < 10 s with every restart re-offered, and
fault_boots / netwdt_restarts / boots_since_poweron unchanged across the whole
run; leg B heap flat (first-10 vs last-10 mean of heap_free_int within
--heap-slack bytes, and largest DMA block not trending down).

``--baseline``: the same leg A clock against a NON-redial build (today's
golden), where the re-offer is a reboot. Produces the "before" row of the
table with the same instrument; the redial-only checks are skipped.

EVERYTHING THIS SCRIPT PLAYS IS SILENT (Apollo ruling 11:50 PT 10-04: no
audible soak on the kitchen; re-dial drills need no acoustics). The satellite's
`frames` counter is the oracle, verified 11:48 PT: idle = flat, a 2 s tone at
volume 0.0 = +84 frames within 3 s. aiortc 1.14 does not enable Opus DTX, so
zero PCM is still a full RTP stream.

Plain mode (no --apply) prints the plan and the current device/hub state and
exits 0. Start/stop lines to the home chat, the acoustic room lock, the OTA
and the golden restore are the wrapper's job (run_redial.sh, Mac side); this
script refuses to run outside 09:00-22:00 PT regardless (operator rule).

Writes a JSON ledger (--out) with every sample so the decision row can cite it.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import statistics
import subprocess
import sys
import time
import urllib.error
import urllib.request
import zoneinfo
from pathlib import Path

PT = zoneinfo.ZoneInfo("America/Los_Angeles")

ROOMS = {
    # room: (satellite ip, hub base on the hub host, systemd --user unit)
    "kitchen": ("192.168.1.185", "127.0.0.1:7861", "clanker-webrtc-kitchen"),
    # bench .97 is OFF LIMITS for audio/reboots until S2 (Apollo 09:25 PT 10-04)
}

STORY_CHUNK_MS = 10000  # one silent /test-tone chunk; re-posted every ~9.5 s


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
    def __init__(self, sat: str, hub: str, unit: str) -> None:
        self.sat = f"http://{sat}"
        self.hub = f"http://{hub}"
        self.unit = unit

    # device
    def params(self) -> dict | None:
        return _get(f"{self.sat}/xvf/params")

    def stats(self) -> dict | None:
        return _get(f"{self.sat}/playback/stats")

    def ota_status(self) -> dict | None:
        return _get(f"{self.sat}/ota/status")

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

    def liveness(self) -> dict | None:
        """sat#27 server-liveness fields (+ boot counters) from /ota/status."""
        s = self.ota_status()
        if not s:
            return None
        keys = ("firmware_version", "booted_slot", "ota_state", "uptime_s", "boots_since_poweron",
                "boot_fault_count", "crash_boots", "server_ping_rx", "server_ping_gap_max_ms",
                "server_ping_age_ms", "media_liveness_holds", "uplink_frames")
        return {k: s.get(k) for k in keys}

    # hub
    def health(self) -> dict | None:
        return _get(f"{self.hub}/health")

    def active_peers(self) -> int:
        h = self.health()
        return int(h.get("active_peers", 0)) if h else -1

    def restart_hub(self) -> None:
        subprocess.run(["systemctl", "--user", "restart", self.unit], check=True, timeout=60)

    # SILENT by construction: volume=0.0 -> zero PCM -> still Opus RTP.
    def test_tone(self, ms: int = 300) -> dict | None:
        return _post(f"{self.hub}/test-tone?freq=20&ms={ms}&volume=0.0", timeout=8.0)


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


def leg_story(b: Bench, seconds: float, ledger: dict) -> None:
    """Continuous SILENT downlink media for `seconds` via chained zero-volume tones."""
    before = b.liveness() or {}
    frames0 = (b.stats() or {}).get("frames", -1)
    t0 = time.monotonic()
    chunks: list[dict] = []
    i = 0
    gap_max = 0.0
    last_ok_end = t0
    while time.monotonic() - t0 < seconds:
        r = b.test_tone(ms=STORY_CHUNK_MS)
        now = time.monotonic()
        if r and r.get("ok"):
            gap_max = max(gap_max, now - last_ok_end)
            fr = (b.stats() or {}).get("frames", -1)
            chunks.append({"i": i, "t_s": round(now - t0, 1), "bytes": r.get("bytes"), "frames": fr})
            print(json.dumps(chunks[-1]), flush=True)
            time.sleep(STORY_CHUNK_MS / 1000.0 - 0.5)  # re-post just before the chunk ends
            last_ok_end = time.monotonic()
            i += 1
        else:
            reason = (r or {}).get("reason") or (r or {}).get("error") or "no_response"
            chunks.append({"i": i, "t_s": round(now - t0, 1), "deferred": reason})
            time.sleep(1.5)
    after = b.liveness() or {}
    frames1 = (b.stats() or {}).get("frames", -1)
    ledger["story"] = {
        "seconds": round(time.monotonic() - t0, 1), "chunks_ok": sum(1 for c in chunks if "bytes" in c),
        "chunks_deferred": sum(1 for c in chunks if "deferred" in c), "inter_chunk_gap_max_s": round(gap_max, 2),
        "frames_delta": frames1 - frames0 if frames0 >= 0 and frames1 >= 0 else None,
        "liveness_before": before, "liveness_after": after, "chunks": chunks,
        "rebooted": before.get("boots_since_poweron") != after.get("boots_since_poweron"),
    }
    print("story:", json.dumps({k: v for k, v in ledger["story"].items() if k != "chunks"}), flush=True)


def leg_hub_restarts(b: Bench, n: int, ledger: dict, timeout_s: float, baseline: bool) -> None:
    samples: list[dict] = []
    for i in range(1, n + 1):
        before = b.params() or {}
        gen0 = before.get("redial", {}).get("generation", -1)
        boots0 = before.get("boot_guard", {}).get("boots_since_poweron", -1)
        live0 = b.liveness() or {}
        t_cmd = time.monotonic()
        wall = now_pt().isoformat(timespec="seconds")
        b.restart_hub()
        t_restart_done = time.monotonic() - t_cmd

        # 1. the hub is back (health 200)
        t_hub_up = wait_for(lambda: b.health() is not None, timeout_s)
        # 2. the satellite re-offered: redial generation bumped and CONNECTED
        #    (redial build) or the hub counts a peer again (baseline/reboot)
        if baseline:
            def _connected() -> bool:
                return b.active_peers() >= 1
        else:
            def _connected() -> bool:
                p = b.params()
                r = (p or {}).get("redial", {})
                return bool(r.get("connected")) and r.get("generation", -1) > gen0
        t_connected = wait_for(_connected, timeout_s)
        # 3. audio actually flows: a test-tone lands and frames increments
        #    (read the counter right before each tone: a reboot resets it to 0).
        t_frames: float | None = None
        tone_tries = 0
        if t_connected is not None:
            wait_for(lambda: b.active_peers() >= 1, 15.0)
            t_tone = time.monotonic()
            while time.monotonic() - t_cmd < timeout_s:
                tone_tries += 1
                base = (b.stats() or {}).get("frames", -1)
                if base < 0:
                    time.sleep(0.5)
                    continue
                b.test_tone()
                got = wait_for(lambda: (b.stats() or {}).get("frames", -1) > base, 2.5)
                if got is not None:
                    t_frames = time.monotonic() - t_cmd
                    break
            ledger.setdefault("tone_waits", []).append(round(time.monotonic() - t_tone, 2))
        after = b.params() or {}
        r = after.get("redial", {})
        live1 = b.liveness() or {}
        boots1 = after.get("boot_guard", {}).get("boots_since_poweron", -1)
        s = {
            "i": i, "wall_pt": wall,
            "restart_cmd_s": round(t_restart_done, 3),
            "hub_health_s": None if t_hub_up is None else round(t_hub_up, 3),
            "connected_s": None if t_connected is None else round(t_connected, 3),
            "first_frames_s": None if t_frames is None else round(t_frames, 3),
            "tone_tries": tone_tries,
            "device_trigger": r.get("last_trigger"),
            "device_redial_to_connected_ms": r.get("last_redial_to_connected_ms"),
            "device_attempts_total": r.get("attempts_total"),
            "device_total_failures": r.get("total_failures"),
            "generation": r.get("generation"),
            "boots_since_poweron": boots1,
            "rebooted": boots1 != boots0,
            "uptime_s_after": after.get("boot_guard", {}).get("uptime_s"),
            "heap_free_int": r.get("heap_free_int"),
            "heap_largest_dma": r.get("heap_largest_dma"),
            "ping_gap_max_ms_before": live0.get("server_ping_gap_max_ms"),
            "ping_gap_max_ms_after": live1.get("server_ping_gap_max_ms"),
            "media_holds_after": live1.get("media_liveness_holds"),
        }
        samples.append(s)
        print(json.dumps(s), flush=True)
        # let the session settle (30 s stable-session reset, hub ping cadence)
        time.sleep(35)
    ledger["hub_restarts"] = samples


def leg_redial_cycles(b: Bench, n: int, ledger: dict, timeout_s: float) -> None:
    samples: list[dict] = []
    for i in range(1, n + 1):
        p0 = b.params() or {}
        gen0 = p0.get("redial", {}).get("generation", -1)
        boots0 = p0.get("boot_guard", {}).get("boots_since_poweron", -1)
        wall = now_pt().isoformat(timespec="seconds")
        kicked = b.redial()
        def _connected() -> bool:
            r = (b.params() or {}).get("redial", {})
            return bool(r.get("connected")) and r.get("generation", -1) > gen0
        t_connected = wait_for(_connected, timeout_s)
        time.sleep(2.0)  # let teardown frees settle before the heap read
        p = b.params() or {}
        r = p.get("redial", {})
        s = {
            "i": i, "wall_pt": wall, "kicked": bool(kicked and kicked.get("ok")),
            "connected_s": None if t_connected is None else round(t_connected, 3),
            "device_redial_to_connected_ms": r.get("last_redial_to_connected_ms"),
            "generation": r.get("generation"),
            "consecutive_failures": r.get("consecutive_failures"),
            "total_failures": r.get("total_failures"),
            "heap_free_int": r.get("heap_free_int"),
            "heap_min_free_int": r.get("heap_min_free_int"),
            "heap_largest_int": r.get("heap_largest_int"),
            "heap_largest_dma": r.get("heap_largest_dma"),
            "heap_free_psram": r.get("heap_free_psram"),
            "hub_active_peers": b.active_peers(),
            "rebooted": p.get("boot_guard", {}).get("boots_since_poweron", -1) != boots0,
        }
        samples.append(s)
        print(json.dumps(s), flush=True)
        time.sleep(3.0)
    ledger["redial_cycles"] = samples


def verdict(ledger: dict, heap_slack: int, baseline: bool) -> dict:
    v: dict = {"baseline": baseline}
    st = ledger.get("story")
    if st:
        la, lb = st["liveness_after"] or {}, st["liveness_before"] or {}
        v["story_seconds"] = st["seconds"]
        v["story_rebooted"] = st["rebooted"]
        v["story_ping_gap_max_ms"] = la.get("server_ping_gap_max_ms")
        v["story_media_holds_delta"] = (
            (la.get("media_liveness_holds") or 0) - (lb.get("media_liveness_holds") or 0)
            if la.get("media_liveness_holds") is not None else None)
        v["story_frames_delta"] = st["frames_delta"]
        v["pass_story_no_reboot"] = not st["rebooted"] and st["chunks_ok"] > 0 and (st["frames_delta"] or 0) > 0
    hr = ledger.get("hub_restarts") or []
    if hr:
        ff = [s["first_frames_s"] for s in hr if s["first_frames_s"] is not None]
        cc = [s["connected_s"] for s in hr if s["connected_s"] is not None]
        v["hub_restarts_n"] = len(hr)
        v["first_frames_ok_n"] = len(ff)
        v["first_frames_median_s"] = round(statistics.median(ff), 2) if ff else None
        v["first_frames_min_s"] = round(min(ff), 2) if ff else None
        v["first_frames_max_s"] = round(max(ff), 2) if ff else None
        v["connected_median_s"] = round(statistics.median(cc), 2) if cc else None
        v["reboots_during_restarts"] = sum(1 for s in hr if s["rebooted"])
        v["triggers"] = sorted({str(s.get("device_trigger")) for s in hr})
        v["pass_median_lt_10s"] = bool(ff) and len(ff) == len(hr) and statistics.median(ff) < 10.0
    rc = ledger.get("redial_cycles") or []
    if rc:
        ok = [s for s in rc if s["connected_s"] is not None]
        hf = [s["heap_free_int"] for s in rc if s["heap_free_int"] is not None]
        dma = [s["heap_largest_dma"] for s in rc if s["heap_largest_dma"] is not None]
        mf = [s["heap_min_free_int"] for s in rc if s["heap_min_free_int"] is not None]
        v["redial_cycles_n"] = len(rc)
        v["redial_connected_n"] = len(ok)
        v["redial_connected_median_s"] = round(statistics.median([s["connected_s"] for s in ok]), 2) if ok else None
        v["redial_reboots"] = sum(1 for s in rc if s["rebooted"])
        if len(hf) >= 20:
            head, tail = statistics.mean(hf[:10]), statistics.mean(hf[-10:])
            v["heap_free_int_first10_mean"] = round(head)
            v["heap_free_int_last10_mean"] = round(tail)
            v["heap_free_int_drift"] = round(tail - head)
            v["heap_min_free_int_first"] = mf[0] if mf else None
            v["heap_min_free_int_last"] = mf[-1] if mf else None
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
    gate_keys = ("pass_median_lt_10s", "pass_heap_flat", "pass_no_reboots", "pass_story_no_reboot")
    gates = [v.get(k) for k in gate_keys if k in v]
    v["PASS"] = bool(gates) and all(g is True for g in gates)
    return v


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--room", default="kitchen", choices=sorted(ROOMS))
    ap.add_argument("--story", type=float, default=0.0, help="leg S: continuous SILENT downlink media for N s (sat#27 repro)")
    ap.add_argument("--hub-restarts", type=int, default=10)
    ap.add_argument("--redial-cycles", type=int, default=50)
    ap.add_argument("--timeout", type=float, default=90.0, help="per-event wait (s)")
    ap.add_argument("--heap-slack", type=int, default=4096, help="bytes of drift still 'flat'")
    ap.add_argument("--out", default=None, help="ledger JSON path")
    ap.add_argument("--apply", action="store_true", help="actually drive the room")
    ap.add_argument("--baseline", action="store_true", help="non-redial build: measure the reboot re-offer with the same clock")
    ap.add_argument("--allow-night", action="store_true", help="override the 09:00-22:00 PT window (operator only)")
    a = ap.parse_args()

    sat, hub, unit = ROOMS[a.room]
    b = Bench(sat, hub, unit)
    p = b.params()
    h = b.health()
    print("satellite:", json.dumps({k: p.get(k) for k in ("build", "boot_guard", "redial")} if p else None))
    print("liveness:", json.dumps(b.liveness()))
    print("hub:", json.dumps({k: h.get(k) for k in ("room", "active_peers")} if h else None))
    if not p or "boot_guard" not in p:
        print("REFUSE: satellite unreachable or no boot_guard on /xvf/params", file=sys.stderr)
        return 2
    if not a.baseline and "redial" not in p:
        print("REFUSE: satellite has no `redial` object on /xvf/params -> not a PIPECAT_REDIAL=1 build "
              "(use --baseline for the golden 'before' row)", file=sys.stderr)
        return 2
    if a.baseline and "redial" in p:
        print("REFUSE: --baseline on a redial build measures the wrong thing", file=sys.stderr)
        return 2
    if a.baseline and a.redial_cycles:
        a.redial_cycles = 0  # no kick endpoint on a non-redial build
    if not a.apply:
        print(f"plan[{a.room}{' BASELINE' if a.baseline else ''}]: story {a.story:.0f} s, {a.hub_restarts} restarts of "
              f"{unit}, {a.redial_cycles} POST /webrtc/redial cycles on {sat}; pass = story no reboot, "
              f"median first-frames < 10 s, 0 reboots, heap flat.")
        return 0
    if not daytime_ok() and not a.allow_night:
        print(f"REFUSE: {now_pt():%H:%M} PT is outside 09:00-22:00 (operator rule)", file=sys.stderr)
        return 3

    ledger: dict = {"started_pt": now_pt().isoformat(timespec="seconds"), "args": vars(a),
                    "room": a.room, "satellite": sat, "unit": unit}
    ledger["boot_before"] = b.boot_counters()
    ledger["liveness_before"] = b.liveness()
    ledger["build"] = p.get("build")
    out = Path(a.out or f"gadget2-redial-{a.room}-{now_pt():%Y%m%d-%H%M}.json")
    try:
        if a.story:
            leg_story(b, a.story, ledger)
        if a.hub_restarts:
            leg_hub_restarts(b, a.hub_restarts, ledger, a.timeout, a.baseline)
        if a.redial_cycles:
            leg_redial_cycles(b, a.redial_cycles, ledger, a.timeout)
    finally:
        ledger["boot_after"] = b.boot_counters()
        ledger["liveness_after"] = b.liveness()
        ledger["ended_pt"] = now_pt().isoformat(timespec="seconds")
        ledger["verdict"] = verdict(ledger, a.heap_slack, a.baseline)
        out.write_text(json.dumps(ledger, indent=1))
        print("verdict:", json.dumps(ledger["verdict"], indent=1))
        print("ledger:", out)
    return 0 if ledger["verdict"].get("PASS") else 1


if __name__ == "__main__":
    sys.exit(main())
