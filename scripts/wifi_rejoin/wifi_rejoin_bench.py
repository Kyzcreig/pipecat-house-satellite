#!/usr/bin/env python3
"""wifi_rejoin_bench.py -- t_2a5f2312 (GADGET-3): forced-reconnect re-join bench.

Kicks the satellite under test (default KITCHEN .185; the bench is refused) off Wi-Fi N times from the UniFi controller
(``POST cmd/stamgr {"cmd":"kick-sta"}``) and measures, per kick:

* ``ping_gap_ms``   -- wall time from the kick POST to the first ICMP reply after
                       the outage, sampled at 100 ms from this Mac (firmware-agnostic
                       oracle; includes association + DHCP + ARP).
* ``dev_rejoin_ms`` -- the device's own DISCONNECTED -> GOT_IP clock from
                       ``/ota/status`` ``wifi_rejoin_last_ms`` (instrumented builds
                       only; null on the live df784f6/cd45607 image).
* which path the device took (``wifi_fast_rejoins`` / ``wifi_fallback_rejoins``
  deltas, instrumented builds only), whether it rebooted (``uptime_s`` went
  backwards), and whether the WebRTC peer survived (hub ``/health`` active_peers).
* the AP it landed on (UniFi ``stat/sta`` ``ap_mac``/``channel``/``signal``), so the
  "near AP 20/20" leg of the pass criterion is read from the controller, not guessed.

Pass criterion (vault note "Muse Gadget SDK vs Clanker ..." line 152): median re-join
< 2 s AND p95 < 5 s AND the joined BSSID is the near AP 20/20.

READ-MOSTLY: the only controller mutations are kick-sta (the measurement itself) and,
with ``lock-ap``/``unlock-ap``, the satellite client's ``fixed_ap_*`` fields (snapshot
written first). Test target = KITCHEN per Apollo 09:25 PT 10-04 (bench hands-off until S2);
run only under the kitchen acoustic lease with START/STOP lines to the home chat.

Usage (from the Mac Studio, daytime only, under the kitchen lease):
    python3 wifi_rejoin_bench.py run --arm live-cd45607 --trials 20 --out runs/
    python3 wifi_rejoin_bench.py lock-ap --ap 0c:ea:14:8e:e5:9b      # B1 arm
    python3 wifi_rejoin_bench.py unlock-ap
    python3 wifi_rejoin_bench.py report runs/*.jsonl
"""
from __future__ import annotations

import argparse
import http.cookiejar
import json
import os
import re
import ssl
import statistics
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any

# Test target per room. Apollo ruling 09:25 PT 2026-10-04 (Ace 09:22): the KITCHEN is the
# test target for every bench-queue card until the bench moves to the shed (S2); the bench
# (.97, Ace's bedroom) is hands-off (no audio, no flashes, no reboots, no kicks).
ROOMS: dict[str, dict[str, str]] = {
    "kitchen": {"ip": "192.168.1.185", "mac": "dc:b4:d9:38:a6:cc",
                "hub_health": "http://192.168.1.216:7861/health"},
    "bench": {"ip": "192.168.1.97", "mac": "1c:db:d4:74:64:84",
              "hub_health": "http://192.168.1.216:7860/health"},
}
BENCH_OFF_LIMITS = "bench is OFF LIMITS (Apollo 09:25 PT 10-04: Ace's bedroom until S2); test target is the kitchen"
CFG: dict[str, str] = dict(ROOMS["kitchen"], room="kitchen")


def select_room(room: str) -> dict[str, str]:
    if room == "bench" and os.environ.get("ALLOW_BENCH") != "1":
        raise SystemExit(f"REFUSED: {BENCH_OFF_LIMITS}")
    if room not in ROOMS:
        raise SystemExit(f"unknown room {room!r}")
    CFG.clear()
    CFG.update(ROOMS[room], room=room)
    return CFG
CONTROLLER = os.environ.get("UNIFI_CONTROLLER", "https://192.168.1.1")
SITE = os.environ.get("UNIFI_SITE", "default")
OP_FETCH = Path.home() / ".hermes/skills-shared/general/1password/scripts/op-fetch.sh"
# AP mac -> name. Seeded with the two APs the bench has been seen on (unifi skill + 09-17
# research doc); `run` refreshes it from the controller's stat/device so the kitchen's APs
# (Front / Hall / Theater ...) are named too.
AP_NAMES: dict[str, str] = {
    "0c:ea:14:8e:eb:b3": "Ace's Bathroom AP",
    "0c:ea:14:8e:e5:9b": "Front AP",
}
PASS_MEDIAN_MS = 2000
PASS_P95_MS = 5000


# ----------------------------------------------------------------------------- UniFi
class UnifiError(RuntimeError):
    pass


def _ssl_context() -> ssl.SSLContext:
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    return ctx


def _password() -> str:
    direct = os.environ.get("UNIFI_PASSWORD", "").strip()
    if direct:
        return direct
    cache = Path(os.environ.get("UNIFI_PASS_CACHE", "/tmp/.unifi_pass_cache"))
    if cache.is_file():
        cached = cache.read_text().strip()
        if cached:
            return cached
    if not OP_FETCH.is_file():
        raise UnifiError("no UNIFI_PASSWORD and op-fetch.sh missing")
    for attempt in range(4):
        res = subprocess.run(
            [str(OP_FETCH), "item", "UniFi Local Admin - forge", "Engineering", "password"],
            capture_output=True, text=True, timeout=90,
        )
        pw = res.stdout.strip()
        if pw:
            try:
                cache.write_text(pw)
                cache.chmod(0o600)
            except OSError:
                pass
            return pw
        time.sleep(2 + attempt)
    raise UnifiError("1Password fetch returned nothing after 4 tries")


class Unifi:
    """Legacy-API session with CSRF (needed for the kick + the fixed_ap PUT)."""

    def __init__(self) -> None:
        self._opener: urllib.request.OpenerDirector | None = None
        self._csrf = ""

    def login(self) -> None:
        jar = http.cookiejar.CookieJar()
        opener = urllib.request.build_opener(
            urllib.request.HTTPCookieProcessor(jar),
            urllib.request.HTTPSHandler(context=_ssl_context()),
        )
        body = json.dumps({"username": "forge", "password": _password()}).encode()
        req = urllib.request.Request(
            f"{CONTROLLER}/api/auth/login", data=body, method="POST",
            headers={"Content-Type": "application/json"},
        )
        try:
            with opener.open(req, timeout=20) as resp:
                resp.read()
                self._csrf = (resp.headers.get("x-csrf-token")
                              or resp.headers.get("x-updated-csrf-token") or "")
        except urllib.error.HTTPError as exc:
            raise UnifiError(f"login HTTP {exc.code}") from exc
        except urllib.error.URLError as exc:
            raise UnifiError(f"login failed: {exc.reason}") from exc
        self._opener = opener

    def _call(self, method: str, path: str, payload: dict | None = None) -> dict[str, Any]:
        """GET reads retry on a transient controller 5xx / socket error (the 12:17 PT
        A1 leg died on one `stat/sta` HTTP 500); writes (kick, PUT) are sent once."""
        attempts = 4 if method == "GET" else 1
        last_exc: Exception | None = None
        for attempt in range(attempts):
            if self._opener is None:
                self.login()
            assert self._opener is not None
            url = f"{CONTROLLER}/proxy/network/api/s/{SITE}/{path.lstrip('/')}"
            data = json.dumps(payload).encode() if payload is not None else None
            headers = {"Content-Type": "application/json"}
            if method != "GET":
                headers["X-Csrf-Token"] = self._csrf
            req = urllib.request.Request(url, data=data, method=method, headers=headers)
            try:
                with self._opener.open(req, timeout=20) as resp:
                    raw = resp.read()
                    upd = resp.headers.get("x-updated-csrf-token")
                    if upd:
                        self._csrf = upd
                    return json.loads(raw.decode("utf-8", errors="replace"))
            except urllib.error.HTTPError as exc:
                if exc.code in (401, 403) and method == "GET":
                    self._opener = None
                    last_exc = exc
                    continue
                if exc.code >= 500 and method == "GET":
                    last_exc = UnifiError(f"{method} {path} HTTP {exc.code}: {exc.read()[:200]!r}")
                    time.sleep(1.0 + attempt)
                    continue
                raise UnifiError(f"{method} {path} HTTP {exc.code}: {exc.read()[:200]!r}") from exc
            except (urllib.error.URLError, TimeoutError, OSError) as exc:
                if method != "GET":
                    raise UnifiError(f"{method} {path} failed: {exc}") from exc
                last_exc = exc
                time.sleep(1.0 + attempt)
        raise UnifiError(f"{method} {path} failed after {attempts} attempts: {last_exc}")

    def get(self, path: str) -> dict[str, Any]:
        return self._call("GET", path)

    def post(self, path: str, payload: dict) -> dict[str, Any]:
        return self._call("POST", path, payload)

    def put(self, path: str, payload: dict) -> dict[str, Any]:
        return self._call("PUT", path, payload)

    # -- satellite client helpers (the room's MAC from CFG unless given) ----------
    def sta(self, mac: str | None = None) -> dict[str, Any] | None:
        mac = (mac or CFG["mac"]).lower()
        for c in self.get("stat/sta").get("data", []) or []:
            if str(c.get("mac", "")).lower() == mac:
                return c
        return None

    def user_record(self, mac: str | None = None) -> dict[str, Any]:
        mac = (mac or CFG["mac"]).lower()
        rows = self.get(f"stat/user/{mac}").get("data", []) or []
        if not rows:
            raise UnifiError(f"no stat/user record for {mac}")
        return rows[0]

    def kick(self, mac: str | None = None) -> dict[str, Any]:
        return self.post("cmd/stamgr", {"cmd": "kick-sta", "mac": (mac or CFG["mac"]).lower()})

    def refresh_ap_names(self) -> dict[str, str]:
        """Fill AP_NAMES from stat/device (uap rows) so the landing AP is named, not just a MAC."""
        try:
            for d in self.get("stat/device").get("data", []) or []:
                if d.get("type") == "uap" and d.get("mac"):
                    AP_NAMES[str(d["mac"]).lower()] = str(d.get("name") or d.get("model") or d["mac"])
        except UnifiError:
            pass
        return AP_NAMES


def sta_view(c: dict[str, Any] | None) -> dict[str, Any]:
    if not c:
        return {"present": False}
    ap = str(c.get("ap_mac") or "").lower() or None
    return {
        "present": True,
        "ap_mac": ap,
        "ap_name": AP_NAMES.get(ap or "", None),
        "channel": c.get("channel"),
        "radio": c.get("radio"),
        "signal": c.get("signal"),
        "noise": c.get("noise"),
        "assoc_time": c.get("assoc_time"),
        "latest_assoc_time": c.get("latest_assoc_time"),
        "uptime": c.get("uptime"),
        "ip": c.get("ip"),
    }


# ----------------------------------------------------------------------------- device
def http_json(url: str, timeout: float = 3.0) -> dict[str, Any] | None:
    try:
        with urllib.request.urlopen(url, timeout=timeout) as resp:
            return json.loads(resp.read().decode("utf-8", errors="replace"))
    except Exception:
        return None


def ota_status() -> dict[str, Any] | None:
    return http_json(f"http://{CFG['ip']}/ota/status")


def hub_peers() -> int | None:
    h = http_json(CFG["hub_health"])
    return h.get("active_peers") if isinstance(h, dict) else None


# ----------------------------------------------------------------------------- pinger
class Pinger:
    """`ping -i 0.1` subprocess; records wall time of every reply."""

    def __init__(self, ip: str | None = None) -> None:
        self.ip = ip or CFG["ip"]
        self.replies: list[float] = []
        self._proc: subprocess.Popen | None = None
        self._thread: threading.Thread | None = None
        self._lock = threading.Lock()

    def start(self) -> None:
        self._proc = subprocess.Popen(
            ["ping", "-i", "0.1", self.ip], stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True, bufsize=1,
        )
        self._thread = threading.Thread(target=self._pump, daemon=True)
        self._thread.start()

    def _pump(self) -> None:
        assert self._proc and self._proc.stdout
        for line in self._proc.stdout:
            if "bytes from" in line and "icmp_seq" in line:
                with self._lock:
                    self.replies.append(time.monotonic())

    def last_reply_before(self, t: float) -> float | None:
        with self._lock:
            prior = [r for r in self.replies if r <= t]
        return prior[-1] if prior else None

    def first_reply_after(self, t: float) -> float | None:
        with self._lock:
            later = [r for r in self.replies if r > t]
        return later[0] if later else None

    def stop(self) -> None:
        if self._proc:
            self._proc.terminate()
            try:
                self._proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self._proc.kill()


# ----------------------------------------------------------------------------- stats
def percentile(xs: list[float], p: float) -> float | None:
    if not xs:
        return None
    ys = sorted(xs)
    k = (len(ys) - 1) * p
    lo, hi = int(k), min(int(k) + 1, len(ys) - 1)
    return ys[lo] + (ys[hi] - ys[lo]) * (k - lo)


def summarize(trials: list[dict[str, Any]], near_ap: str | None) -> dict[str, Any]:
    """Pure: table rows -> numbers + verdict. Unit-tested in test_wifi_rejoin_bench.py."""
    ping = [t["ping_gap_ms"] for t in trials if isinstance(t.get("ping_gap_ms"), (int, float))]
    dev = [t["dev_rejoin_ms"] for t in trials if isinstance(t.get("dev_rejoin_ms"), (int, float))]
    aps = [t.get("post_sta", {}).get("ap_mac") for t in trials]
    ap_counts: dict[str, int] = {}
    for ap in aps:
        ap_counts[str(ap)] = ap_counts.get(str(ap), 0) + 1
    rssi_by_ap: dict[str, list[int]] = {}
    for t in trials:
        s = t.get("post_sta", {})
        if s.get("ap_mac") and isinstance(s.get("signal"), (int, float)):
            rssi_by_ap.setdefault(s["ap_mac"], []).append(int(s["signal"]))
    # "near AP" = explicit, else the AP with the best mean RSSI across the run.
    if near_ap is None and rssi_by_ap:
        near_ap = max(rssi_by_ap, key=lambda k: statistics.mean(rssi_by_ap[k]))
    near_hits = sum(1 for ap in aps if ap and ap == near_ap)
    n = len(trials)
    reboots = sum(1 for t in trials if t.get("rebooted"))
    peer_lost = sum(1 for t in trials if t.get("peer_survived") is False)
    fast = sum(1 for t in trials if t.get("path") == "fast")
    fallback = sum(1 for t in trials if t.get("path") == "fallback")
    timeouts = sum(1 for t in trials if t.get("ping_gap_ms") is None)
    # Verdict uses the device clock when every trial has it, else the ping gap.
    basis = "device" if dev and len(dev) == n else "ping"
    series = dev if basis == "device" else ping
    med = statistics.median(series) if series else None
    p95 = percentile(series, 0.95) if series else None
    out = {
        "n": n,
        "basis": basis,
        "median_ms": med,
        "p95_ms": p95,
        "max_ms": max(series) if series else None,
        "ping_median_ms": statistics.median(ping) if ping else None,
        "ping_p95_ms": percentile(ping, 0.95) if ping else None,
        "dev_median_ms": statistics.median(dev) if dev else None,
        "dev_p95_ms": percentile(dev, 0.95) if dev else None,
        "timeouts": timeouts,
        "reboots": reboots,
        "peer_lost": peer_lost,
        "path_fast": fast,
        "path_fallback": fallback,
        "ap_counts": ap_counts,
        "rssi_mean_by_ap": {k: round(statistics.mean(v), 1) for k, v in rssi_by_ap.items()},
        "near_ap": near_ap,
        "near_ap_hits": near_hits,
        "pass_median": med is not None and med < PASS_MEDIAN_MS,
        "pass_p95": p95 is not None and p95 < PASS_P95_MS,
        "pass_near_ap": n > 0 and near_hits == n,
    }
    out["pass"] = bool(out["pass_median"] and out["pass_p95"] and out["pass_near_ap"]
                       and timeouts == 0)
    return out


def fmt_table(name: str, s: dict[str, Any]) -> str:
    def ms(v: Any) -> str:
        return "-" if v is None else f"{v:.0f}"
    verdict = "PASS" if s["pass"] else "FAIL"
    return (
        f"| {name} | {s['n']} | {ms(s['median_ms'])} | {ms(s['p95_ms'])} | {ms(s['max_ms'])} | "
        f"{s['basis']} | {s['near_ap_hits']}/{s['n']} | {s['path_fast']}/{s['path_fallback']} | "
        f"{s['reboots']} | {s['peer_lost']} | {s['timeouts']} | {verdict} |"
    )


TABLE_HEADER = (
    "| arm | n | median ms | p95 ms | max ms | clock | near-AP | fast/fallback | "
    "reboots | peer lost | timeouts | verdict (<2000 / <5000 / 20/20) |\n"
    "|---|---|---|---|---|---|---|---|---|---|---|---|"
)


# ----------------------------------------------------------------------------- run
def wait_sta_reassoc(u: Unifi, before: dict[str, Any], deadline_s: float = 90.0) -> dict[str, Any]:
    """Poll stat/sta until the controller shows a NEW association (or give up)."""
    t0 = time.monotonic()
    last = None
    while time.monotonic() - t0 < deadline_s:
        try:
            cur = sta_view(u.sta())
        except UnifiError:
            cur = {"present": False}
        last = cur
        if cur.get("present"):
            fresh = (cur.get("latest_assoc_time") != before.get("latest_assoc_time")
                     or cur.get("assoc_time") != before.get("assoc_time")
                     or (isinstance(cur.get("uptime"), int) and cur["uptime"] < 60))
            if fresh:
                return cur
        time.sleep(3)
    return {**(last or {"present": False}), "stale": True}


def one_trial(u: Unifi, idx: int, pinger: Pinger, settle_s: float, log) -> dict[str, Any]:
    pre_ota = ota_status()
    try:
        pre_sta = sta_view(u.sta())
    except UnifiError as exc:
        log(f"[{idx}] WARNING: stat/sta read failed before the kick ({exc}); landing AP still read after")
        pre_sta = {"present": False}
    pre_peers = hub_peers()
    t_kick = time.monotonic()
    kick = u.kick()
    rc = (kick.get("meta") or {}).get("rc")
    log(f"[{idx}] kick rc={rc} pre_ap={pre_sta.get('ap_name') or pre_sta.get('ap_mac')} "
        f"ch{pre_sta.get('channel')} rssi={pre_sta.get('signal')}")
    # Wait for ICMP to resume: a gap of >= 400 ms with no reply, then a reply.
    first_after = None
    gap_seen = False
    deadline = t_kick + 120
    while time.monotonic() < deadline:
        time.sleep(0.05)
        last = pinger.last_reply_before(time.monotonic())
        if not gap_seen and last is not None and time.monotonic() - last > 0.4:
            gap_seen = True
            gap_start = last
        if gap_seen:
            r = pinger.first_reply_after(gap_start)
            if r is not None:
                first_after = r
                break
    if not gap_seen:
        log(f"[{idx}] WARNING: no ICMP gap observed after the kick (kick rc={rc})")
    ping_gap_ms = round((first_after - t_kick) * 1000) if first_after else None
    # Device clock + path: poll /ota/status until wifi_disconnects advances.
    dev_rejoin_ms = None
    path = None
    rebooted = None
    post_ota = None
    t_dev0 = time.monotonic()
    while time.monotonic() - t_dev0 < 30:
        post_ota = ota_status()
        if post_ota:
            break
        time.sleep(0.5)
    if pre_ota and post_ota:
        rebooted = post_ota.get("uptime_s", 0) < pre_ota.get("uptime_s", 0)
        if "wifi_disconnects" in post_ota:
            # instrumented build: give the counter a moment to land
            for _ in range(10):
                if post_ota.get("wifi_disconnects", 0) > pre_ota.get("wifi_disconnects", 0) or rebooted:
                    break
                time.sleep(0.5)
                post_ota = ota_status() or post_ota
            if not rebooted:
                dev_rejoin_ms = post_ota.get("wifi_rejoin_last_ms")
                if post_ota.get("wifi_fast_rejoins", 0) > pre_ota.get("wifi_fast_rejoins", 0):
                    path = "fast"
                elif post_ota.get("wifi_fallback_rejoins", 0) > pre_ota.get("wifi_fallback_rejoins", 0):
                    path = "fallback"
    post_sta = wait_sta_reassoc(u, pre_sta)
    time.sleep(2)
    post_peers = hub_peers()
    peer_survived = (pre_peers == 1 and post_peers == 1 and rebooted is False) if pre_peers is not None else None
    row = {
        "trial": idx,
        "t_wall": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "kick_rc": rc,
        "ping_gap_ms": ping_gap_ms,
        "dev_rejoin_ms": dev_rejoin_ms,
        "path": path,
        "rebooted": rebooted,
        "peer_survived": peer_survived,
        "pre_sta": pre_sta,
        "post_sta": post_sta,
        "pre_ota": {k: pre_ota.get(k) for k in ("firmware_version", "sha256", "booted_slot", "uptime_s",
                                                 "wifi_fast_rejoin", "wifi_disconnects", "wifi_fast_rejoins",
                                                 "wifi_fallback_rejoins", "wifi_rejoin_last_ms")} if pre_ota else None,
        "post_ota": {k: post_ota.get(k) for k in ("uptime_s", "wifi_disconnects", "wifi_fast_rejoins",
                                                   "wifi_fallback_rejoins", "wifi_rejoin_last_ms",
                                                   "ota_state", "firmware_version")} if post_ota else None,
        "hub_peers_pre": pre_peers,
        "hub_peers_post": post_peers,
    }
    log(f"[{idx}] ping_gap={ping_gap_ms} ms dev={dev_rejoin_ms} ms path={path} rebooted={rebooted} "
        f"peer={peer_survived} post_ap={post_sta.get('ap_name') or post_sta.get('ap_mac')} "
        f"ch{post_sta.get('channel')} rssi={post_sta.get('signal')}")
    time.sleep(settle_s)
    return row


def cmd_run(args: argparse.Namespace) -> int:
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    path = out_dir / f"{args.arm}-{stamp}.jsonl"
    logf = open(out_dir / f"{args.arm}-{stamp}.log", "a")

    def log(msg: str) -> None:
        line = f"{time.strftime('%H:%M:%S')} {msg}"
        print(line, flush=True)
        logf.write(line + "\n")
        logf.flush()

    u = Unifi()
    u.login()
    u.refresh_ap_names()
    ota = ota_status()
    if not ota or ota.get("satellite_id") != CFG["room"]:
        log(f"REFUSE: /ota/status at {CFG['ip']} is not the {CFG['room']} satellite: {ota}")
        return 3
    sta = sta_view(u.sta())
    log(f"room={CFG['room']} ip={CFG['ip']} mac={CFG['mac']} hub={CFG['hub_health']}")
    log(f"arm={args.arm} fw={ota.get('firmware_version')} sha={str(ota.get('sha256'))[:8]} "
        f"fast_rejoin={ota.get('wifi_fast_rejoin', 'n/a')} uptime={ota.get('uptime_s')} "
        f"sta={sta.get('ap_name') or sta.get('ap_mac')} ch{sta.get('channel')} rssi={sta.get('signal')} "
        f"hub_peers={hub_peers()}")
    if args.dry_run:
        log("dry-run: no kick sent")
        return 0
    pinger = Pinger()
    pinger.start()
    time.sleep(1.5)
    trials: list[dict[str, Any]] = []
    try:
        with open(path, "a") as f:
            for i in range(1, args.trials + 1):
                row = one_trial(u, i, pinger, args.settle, log)
                row["arm"] = args.arm
                trials.append(row)
                f.write(json.dumps(row, sort_keys=True) + "\n")
                f.flush()
                if row["rebooted"]:
                    log(f"[{i}] device REBOOTED during the trial; waiting for ota_state=valid + peer")
                    for _ in range(60):
                        o = ota_status()
                        if o and o.get("ota_state") == "valid" and hub_peers() == 1:
                            break
                        time.sleep(3)
    finally:
        pinger.stop()
    s = summarize(trials, args.near_ap)
    log(json.dumps(s, sort_keys=True))
    print(TABLE_HEADER)
    print(fmt_table(args.arm, s))
    (out_dir / f"{args.arm}-{stamp}.summary.json").write_text(json.dumps(s, indent=2, sort_keys=True))
    return 0


def cmd_report(args: argparse.Namespace) -> int:
    print(TABLE_HEADER)
    for p in args.files:
        rows = [json.loads(l) for l in Path(p).read_text().splitlines() if l.strip()]
        if not rows:
            continue
        s = summarize(rows, args.near_ap)
        print(fmt_table(rows[0].get("arm", Path(p).stem), s))
    return 0


def cmd_lock(args: argparse.Namespace, enable: bool) -> int:
    u = Unifi()
    u.login()
    rec = u.user_record()
    snap = Path(args.snapshot_dir).expanduser()
    snap.mkdir(parents=True, exist_ok=True)
    snap_path = snap / f"unifi-user-{CFG['room']}-{CFG['mac'].replace(':', '')}-{time.strftime('%Y%m%d-%H%M%S')}.json"
    snap_path.write_text(json.dumps(rec, indent=2, sort_keys=True))
    print(f"snapshot: {snap_path} (fixed_ap_enabled={rec.get('fixed_ap_enabled')} fixed_ap_mac={rec.get('fixed_ap_mac')})")
    payload: dict[str, Any] = {"fixed_ap_enabled": enable}
    if enable:
        payload["fixed_ap_mac"] = args.ap.lower()
    res = u.put(f"rest/user/{rec['_id']}", payload)
    rc = (res.get("meta") or {}).get("rc")
    back = u.user_record()
    print(f"rc={rc} readback fixed_ap_enabled={back.get('fixed_ap_enabled')} fixed_ap_mac={back.get('fixed_ap_mac')}")
    ok = rc == "ok" and bool(back.get("fixed_ap_enabled")) == enable and (
        not enable or str(back.get("fixed_ap_mac", "")).lower() == args.ap.lower())
    return 0 if ok else 4


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--room", default="kitchen", choices=sorted(ROOMS),
                    help="satellite under test (default kitchen; bench is refused per Apollo 09:25 PT 10-04)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--arm", required=True, help="label, e.g. live-df784f6 | instr-off | fast-on | lock-front")
    r.add_argument("--trials", type=int, default=20)
    r.add_argument("--settle", type=float, default=20.0, help="seconds between kicks")
    r.add_argument("--out", default="runs")
    r.add_argument("--near-ap", default=None, help="BSSID that counts as the near AP (default: best mean RSSI seen)")
    r.add_argument("--dry-run", action="store_true")
    rp = sub.add_parser("report")
    rp.add_argument("files", nargs="+")
    rp.add_argument("--near-ap", default=None)
    lk = sub.add_parser("lock-ap")
    lk.add_argument("--ap", required=True)
    lk.add_argument("--snapshot-dir", default="~/.hermes/backups/unifi-t_2a5f2312")
    ul = sub.add_parser("unlock-ap")
    ul.add_argument("--snapshot-dir", default="~/.hermes/backups/unifi-t_2a5f2312")
    ul.set_defaults(ap=None)
    args = ap.parse_args(argv)
    select_room(args.room)
    if args.cmd == "run":
        return cmd_run(args)
    if args.cmd == "report":
        return cmd_report(args)
    if args.cmd == "lock-ap":
        return cmd_lock(args, True)
    if args.cmd == "unlock-ap":
        return cmd_lock(args, False)
    return 2


if __name__ == "__main__":
    sys.exit(main())
