#!/usr/bin/env python3
"""D-43 reliable-channel load/liveness certificate for one room.

The load leg uses a monotonic absolute schedule and deliberately does not call
``sleep``. On the Mac Studio, short sleeps overshoot to roughly 160 ms, which
collapsed the former nominal 20 Hz producer to about 6.7 Hz. Missed time is not
repaid with bursts: every request start remains at least one 20 Hz interval
after the preceding start.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import threading
import time
from typing import Any, Callable
import urllib.error
import urllib.request

LOAD_SECONDS = 35.0
RATE_HZ = 20.0
SETTLE_SECONDS = 35.0
MIN_REQUESTS = 600


def get_json(url: str) -> dict[str, Any]:
    with urllib.request.urlopen(url, timeout=6) as response:
        payload = json.load(response)
    if not isinstance(payload, dict):
        raise RuntimeError(f"non-object response from {url}: {payload!r}")
    return payload


def post(url: str) -> dict[str, Any]:
    request = urllib.request.Request(url, data=b"", method="POST")
    with urllib.request.urlopen(request, timeout=6) as response:
        body = json.load(response)
        return {"http_status": response.status, "body": body}


def split_ok(document: dict[str, Any]) -> bool:
    rows = list((document.get("dual_stream_counters") or {}).values())
    return bool(rows) and all(
        row.get("stereo_frames_split", 0) > 0
        and row.get("stt_left_bytes") == row.get("detector_right_bytes")
        and row.get("mono_passthrough_frames") == 0
        and row.get("mislabeled_mono_frames") == 0
        for row in rows
    )


def atomic_write(path: Path, document: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n")
    temporary.replace(path)


def error_text(exc: BaseException) -> str:
    return f"{type(exc).__name__}: {exc}"


def rtvi_drop_delta(before: dict[str, Any], after: dict[str, Any]) -> int:
    return int(after.get("rtvi_rx_dropped", 0)) - int(
        before.get("rtvi_rx_dropped", 0)
    )


def run_fixed_rate(
    post_fn: Callable[[], dict[str, Any]],
    *,
    load_s: float,
    rate_hz: float,
    monotonic: Callable[[], float] = time.monotonic,
) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    """Issue non-bursting posts on a monotonic fixed-rate schedule.

    Short sleeps are intentionally forbidden here. The caller can be delayed by
    request latency or OS preemption, but the driver never compensates by
    bunching later requests closer than one target interval.
    """
    interval_s = 1.0 / rate_hz
    target_slots = int(load_s * rate_hz)
    started = monotonic()
    deadline = started + load_s
    starts: list[float] = []
    results: list[dict[str, Any]] = []

    for slot in range(target_slots):
        nominal = started + slot * interval_s
        earliest = starts[-1] + interval_s if starts else started
        target = max(nominal, earliest)
        if target >= deadline:
            break
        while monotonic() < target:
            pass
        request_started = monotonic()
        if request_started >= deadline:
            break
        starts.append(request_started)
        results.append(post_fn())

    intervals = [later - earlier for earlier, later in zip(starts, starts[1:])]
    observed_span_s = starts[-1] - starts[0] if len(starts) > 1 else 0.0
    schedule = {
        "target_rate_hz": rate_hz,
        "target_slots": target_slots,
        "sent_slots": len(results),
        "skipped_slots": target_slots - len(results),
        "observed_span_s": round(observed_span_s, 6),
        "achieved_rate_hz": round(
            (
                (len(starts) - 1) / observed_span_s
                if observed_span_s > 0
                else 0.0
            ),
            6,
        ),
        "minimum_start_interval_ms": round(
            min(intervals) * 1000 if intervals else 0.0, 6
        ),
        "maximum_start_interval_ms": round(
            max(intervals) * 1000 if intervals else 0.0, 6
        ),
        "non_bursting": bool(intervals)
        and min(intervals) >= interval_s * 0.90,
    }
    return results, schedule


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--device", required=True)
    parser.add_argument("--server", required=True)
    parser.add_argument("--expected-sha", required=True)
    parser.add_argument("--room", required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument(
        "--rate-proof-only",
        action="store_true",
        help="prove the corrected producer rate on a stable image without claiming full D-43",
    )
    parser.add_argument(
        "--apply",
        action="store_true",
        help="send the live reliable-channel load; omitted means network-dark plan only",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    if not args.apply:
        print(
            "PLAN: network-dark; would send "
            f"{int(LOAD_SECONDS * RATE_HZ)} slots at {RATE_HZ:g} Hz for "
            f"{LOAD_SECONDS:g}s, settle {SETTLE_SECONDS:g}s, and require "
            f">={MIN_REQUESTS} accepted requests. Re-run with --apply."
        )
        return 0

    def health() -> dict[str, Any]:
        return get_json(args.server + "/health")

    def stats() -> dict[str, Any]:
        return get_json(args.device + "/playback/stats")

    def status() -> dict[str, Any]:
        return get_json(args.device + "/ota/status")

    run_started = time.monotonic()
    phase = "preflight"
    before_h: dict[str, Any] = {}
    before_p: dict[str, Any] = {}
    before_s: dict[str, Any] = {}
    after_h: dict[str, Any] = {}
    after_p: dict[str, Any] = {}
    after_s: dict[str, Any] = {}
    peer_ids: list[str] = []
    samples: list[dict[str, Any]] = []
    recovery_samples: list[dict[str, Any]] = []
    request_results: list[dict[str, Any]] = []
    schedule: dict[str, Any] = {}
    sample_errors: list[str] = []

    def sample(label: str) -> dict[str, Any]:
        current_health = health()
        current_status = status()
        return {
            "label": label,
            "at_s": round(time.monotonic() - run_started, 3),
            "peer_ids": sorted(
                (current_health.get("dual_stream_counters") or {}).keys()
            ),
            "active_peers": current_health.get("active_peers"),
            "missed_pongs": current_health.get("missed_pongs"),
            "evicted_peers_total": current_health.get("evicted_peers_total"),
            "firmware_uptime_s": current_status.get("uptime_s"),
            "firmware_sha256": current_status.get("sha256"),
        }

    sampler_stop = threading.Event()

    def sample_load() -> None:
        while not sampler_stop.is_set():
            try:
                samples.append(sample("load"))
            except (OSError, TimeoutError, urllib.error.URLError, ValueError) as exc:
                sample_errors.append(error_text(exc))
                return
            sampler_stop.wait(1.0)

    try:
        before_h = health()
        before_p = stats()
        before_s = status()
        peer_ids = sorted((before_h.get("dual_stream_counters") or {}).keys())
        if (
            before_s.get("sha256") != args.expected_sha
            or before_s.get("app_valid") is not True
            or before_s.get("satellite_id") != args.room
            or before_h.get("room") != args.room
            or before_h.get("active_peers") != 1
            or before_h.get("dual_stream_split") is not True
            or before_h.get("nack_enabled") is not False
            or not split_ok(before_h)
        ):
            raise RuntimeError("pre-load room state is not certifiable")

        phase = "load"
        sampler = threading.Thread(target=sample_load, name="d43-sampler", daemon=True)
        sampler.start()
        try:
            request_results, schedule = run_fixed_rate(
                lambda: post(args.server + "/test-rtvi-load?payload_bytes=512"),
                load_s=LOAD_SECONDS,
                rate_hz=RATE_HZ,
            )
        finally:
            sampler_stop.set()
            sampler.join(timeout=8)
        if sampler.is_alive():
            raise RuntimeError("load sampler did not stop")
        if sample_errors:
            raise RuntimeError(f"load sampler failed: {sample_errors[0]}")

        phase = "settle"
        settle_deadline = time.monotonic() + SETTLE_SECONDS
        while time.monotonic() < settle_deadline:
            samples.append(sample("settle"))
            remaining = settle_deadline - time.monotonic()
            if remaining > 0:
                time.sleep(min(2.0, remaining))

        phase = "postflight"
        after_h = health()
        after_p = stats()
        after_s = status()
    except (OSError, TimeoutError, urllib.error.URLError, ValueError, RuntimeError) as exc:
        sampler_stop.set()
        failure = error_text(exc)
        recovery_deadline = time.monotonic() + (30 if before_h or before_s else 0)
        while time.monotonic() < recovery_deadline:
            row: dict[str, Any] = {
                "label": "failure-recovery",
                "at_s": round(time.monotonic() - run_started, 3),
            }
            try:
                after_h = health()
                row.update(
                    {
                        "peer_ids": sorted(
                            (after_h.get("dual_stream_counters") or {}).keys()
                        ),
                        "active_peers": after_h.get("active_peers"),
                        "missed_pongs": after_h.get("missed_pongs"),
                        "evicted_peers_total": after_h.get("evicted_peers_total"),
                    }
                )
            except (OSError, TimeoutError, urllib.error.URLError, ValueError) as probe_exc:
                row["health_error"] = error_text(probe_exc)
            try:
                after_s = status()
                row.update(
                    {
                        "firmware_uptime_s": after_s.get("uptime_s"),
                        "firmware_sha256": after_s.get("sha256"),
                    }
                )
            except (OSError, TimeoutError, urllib.error.URLError, ValueError) as probe_exc:
                row["status_error"] = error_text(probe_exc)
            try:
                after_p = stats()
            except (OSError, TimeoutError, urllib.error.URLError, ValueError) as probe_exc:
                row["playback_error"] = error_text(probe_exc)
            recovery_samples.append(row)
            if after_h and after_s and after_p:
                break
            time.sleep(2)

        post_peer_ids = sorted((after_h.get("dual_stream_counters") or {}).keys())
        before_uptime = before_s.get("uptime_s")
        after_uptime = after_s.get("uptime_s")
        reboot_detected = (
            before_uptime is not None
            and after_uptime is not None
            and after_s.get("sha256") == args.expected_sha
            and int(after_uptime) < int(before_uptime)
        )
        result = {
            "status": "FAIL",
            "mode": "rate-proof" if args.rate_proof_only else "d43-clean",
            "room": args.room,
            "expected_sha": args.expected_sha,
            "phase": phase,
            "error": failure,
            "elapsed_s": round(time.monotonic() - run_started, 3),
            "requests_sent": len(request_results),
            "requests_accepted_200": sum(
                row.get("http_status") == 200 for row in request_results
            ),
            "schedule": schedule,
            "reboot_detected": reboot_detected,
            "peer_generation_changed": bool(peer_ids)
            and bool(post_peer_ids)
            and post_peer_ids != peer_ids,
            "last_successful_sample": samples[-1] if samples else None,
            "samples": samples,
            "failure_recovery_samples": recovery_samples,
            "before": {"device": before_s, "playback": before_p, "health": before_h},
            "after": {"device": after_s, "playback": after_p, "health": after_h},
        }
        atomic_write(args.out, result)
        print(json.dumps(result, sort_keys=True))
        return 1

    delta_total = int(after_p.get("rtvi_rx_total", 0)) - int(
        before_p.get("rtvi_rx_total", 0)
    )
    delta_server = int(after_p.get("rtvi_rx_server_msg", 0)) - int(
        before_p.get("rtvi_rx_server_msg", 0)
    )
    delta_dropped = rtvi_drop_delta(before_p, after_p)
    samples_stable = bool(samples) and all(
        row.get("peer_ids") == peer_ids
        and row.get("active_peers") == 1
        and row.get("firmware_sha256") == args.expected_sha
        and all(
            int(value or 0) == 0
            for value in (row.get("missed_pongs") or {}).values()
        )
        and row.get("evicted_peers_total") == before_h.get("evicted_peers_total")
        for row in samples
    )
    all_requests_accepted = len(request_results) >= MIN_REQUESTS and all(
        row.get("http_status") == 200
        and (row.get("body") or {}).get("ok") is True
        and int((row.get("body") or {}).get("sent", 0)) >= 1
        for row in request_results
    )
    checks = {
        "fixed_rate_schedule": schedule.get("target_rate_hz") == RATE_HZ
        and schedule.get("target_slots") == int(LOAD_SECONDS * RATE_HZ)
        and schedule.get("sent_slots") == len(request_results)
        and schedule.get("non_bursting") is True,
        "all_load_requests_accepted": all_requests_accepted,
        "load_reached_firmware": delta_total > 0,
        "bounded_drop_counter_present": "rtvi_rx_dropped" in after_p,
        "zero_rtvi_drops": "rtvi_rx_dropped" in after_p and delta_dropped == 0,
        "same_peer_through_30s_failure_boundary": samples_stable
        and sorted((after_h.get("dual_stream_counters") or {}).keys()) == peer_ids,
        "app_valid_same_boot": after_s.get("sha256") == args.expected_sha
        and after_s.get("app_valid") is True
        and int(after_s.get("uptime_s", 0)) > int(before_s.get("uptime_s", 0)),
        "split_still_armed": after_h.get("active_peers") == 1
        and after_h.get("dual_stream_split") is True
        and split_ok(after_h),
        "nack_still_dark": after_h.get("nack_enabled") is False
        and all(
            int(after_p.get(key, 0)) == 0
            for key in (
                "nack_sent",
                "nack_recovered",
                "nack_rtx_arrived",
                "nack_auto_dark",
            )
        ),
        "no_transport_eviction": after_h.get("evicted_peers_total")
        == before_h.get("evicted_peers_total"),
    }
    rate_proof_keys = (
        "fixed_rate_schedule",
        "all_load_requests_accepted",
        "load_reached_firmware",
        "same_peer_through_30s_failure_boundary",
        "app_valid_same_boot",
        "split_still_armed",
        "nack_still_dark",
        "no_transport_eviction",
    )
    passed = (
        all(checks[key] for key in rate_proof_keys)
        if args.rate_proof_only
        else all(checks.values())
    )
    result = {
        "status": (
            "PASS_RATE_PROOF"
            if passed and args.rate_proof_only
            else "PASS"
            if passed
            else "FAIL"
        ),
        "mode": "rate-proof" if args.rate_proof_only else "d43-clean",
        "room": args.room,
        "expected_sha": args.expected_sha,
        "load_seconds": LOAD_SECONDS,
        "settle_seconds": SETTLE_SECONDS,
        "target_rate_hz": RATE_HZ,
        "minimum_required": MIN_REQUESTS,
        "requests_sent": len(request_results),
        "requests_accepted_200": sum(
            row.get("http_status") == 200 for row in request_results
        ),
        "endpoint_messages_accepted": sum(
            int((row.get("body") or {}).get("sent", 0))
            for row in request_results
        ),
        "schedule": schedule,
        "checks": checks,
        "rtvi_delta": {
            "total": delta_total,
            "server_msg": delta_server,
            "dropped": delta_dropped,
        },
        "samples": samples,
        "before": {"device": before_s, "playback": before_p, "health": before_h},
        "after": {"device": after_s, "playback": after_p, "health": after_h},
    }
    atomic_write(args.out, result)
    print(json.dumps(result, sort_keys=True))
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
