#!/usr/bin/env python3
"""Measure the XVF3800 internal system-delay by cross-correlating cat-3 vs cat-11.

Experiment B (t_2ccb0829): category 3 = amplified mic WITH system delay,
category 11 = the same amplified mic WITHOUT system delay. Capture the same
utterance on the two lanes (either sequentially via the runtime mux single-slot
probe, or simultaneously in packed mode), cross-correlate, and the lag of the
correlation peak IS the applied system delay in samples @16 kHz.

Interpretation:
  delay_samples = argmax_k sum_n a[n] * b[n+k]   with a=cat-11 (no delay),
  b=cat-3 (delayed). Positive lag means cat-3 is LATER (delayed) than cat-11,
  which is the expected sign. The firmware boot profile writes
  AUDIO_MGR_SYS_DELAY=12, so the expected result is +12 samples (0.75 ms)
  unless the chip adds extra alignment internally — measuring exactly that gap
  is the point.

Inputs: two s16le 16 kHz mono PCM files (as produced by the capture harness or
tools/xvf_packed_unpack.py). If captured SEQUENTIALLY (two playbacks of the
same fixture), pass --coarse-window to allow the utterance-start alignment to
be found first; the reported figure is still only trustworthy for SIMULTANEOUS
(packed/dual-slot) captures, and the tool says so in its output.

Self-test: python3 xcorr_alignment.py --self-test
"""
from __future__ import annotations

import argparse
import json
import math
import struct
import sys
from pathlib import Path


def read_s16(path: Path) -> list[float]:
    raw = path.read_bytes()
    if len(raw) % 2:
        raw = raw[:-1]
    samples = struct.unpack(f"<{len(raw) // 2}h", raw)
    return [float(s) for s in samples]


def rms(x: list[float]) -> float:
    return math.sqrt(sum(v * v for v in x) / len(x)) if x else 0.0


def xcorr_peak(a: list[float], b: list[float], max_lag: int) -> dict:
    """Return peak lag of correlation between a (reference) and b (delayed).

    Positive lag => b lags a by that many samples. Normalized so a peak value
    near 1.0 means the signals really are the same waveform.
    """
    n = min(len(a), len(b))
    a = a[:n]
    b = b[:n]
    mean_a = sum(a) / n
    mean_b = sum(b) / n
    a = [v - mean_a for v in a]
    b = [v - mean_b for v in b]
    denom = math.sqrt(sum(v * v for v in a) * sum(v * v for v in b))
    if denom == 0:
        raise SystemExit("zero-energy input; nothing to correlate")

    try:
        import numpy as np

        fa = np.array(a)
        fb = np.array(b)
        size = 1
        while size < 2 * n:
            size *= 2
        spec = np.fft.rfft(fa, size) * np.conj(np.fft.rfft(fb, size))
        corr = np.fft.irfft(spec, size)
        # corr[k] = sum a[n]*b[n-k] -> lag k means b shifted right by -k.
        lags = list(range(-max_lag, max_lag + 1))
        values = [float(corr[(-lag) % size]) / denom for lag in lags]
    except ImportError:
        lags = list(range(-max_lag, max_lag + 1))
        values = []
        for lag in lags:
            acc = 0.0
            if lag >= 0:
                m = n - lag
                for i in range(m):
                    acc += a[i] * b[i + lag]
            else:
                m = n + lag
                for i in range(m):
                    acc += a[i - lag] * b[i]
            values.append(acc / denom)

    best = max(range(len(values)), key=lambda i: values[i])
    # Parabolic interpolation for sub-sample estimate when neighbors exist.
    frac = 0.0
    if 0 < best < len(values) - 1:
        y0, y1, y2 = values[best - 1], values[best], values[best + 1]
        denom2 = y0 - 2 * y1 + y2
        if denom2 != 0:
            frac = 0.5 * (y0 - y2) / denom2
    return {
        "lag_samples": lags[best],
        "lag_subsample": round(lags[best] + frac, 3),
        "lag_ms": round((lags[best] + frac) / 16.0, 4),
        "peak_corr": round(values[best], 4),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("no_delay", nargs="?", help="cat-11 capture (s16le 16k mono)")
    parser.add_argument("with_delay", nargs="?", help="cat-3 capture (s16le 16k mono)")
    parser.add_argument("--max-lag", type=int, default=1600,
                        help="search window in samples (default 1600 = 100 ms)")
    parser.add_argument("--simultaneous", action="store_true",
                        help="captures share one clock/timeline (packed mode)")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        self_test()
        return
    if not args.no_delay or not args.with_delay:
        parser.error("no_delay and with_delay captures are required")

    a = read_s16(Path(args.no_delay))
    b = read_s16(Path(args.with_delay))
    result = xcorr_peak(a, b, args.max_lag)
    verdict = {
        "cat11_no_delay": {"path": args.no_delay, "samples": len(a), "rms": round(rms(a), 2)},
        "cat3_with_delay": {"path": args.with_delay, "samples": len(b), "rms": round(rms(b), 2)},
        **result,
        "capture_mode": "simultaneous" if args.simultaneous else "sequential",
    }
    if result["peak_corr"] < 0.5:
        verdict["warning"] = (
            "peak correlation < 0.5 — lanes are probably not the same signal; "
            "do not trust this delay figure"
        )
    if not args.simultaneous:
        verdict["caveat"] = (
            "sequential captures include playback-start jitter; only a "
            "simultaneous (packed / dual-slot) capture yields the true "
            "internal system delay"
        )
    print(json.dumps(verdict, indent=2))


def self_test() -> None:
    import random

    random.seed(20260716)
    n = 16000  # 1 s
    base = [random.gauss(0, 3000) for _ in range(n)]
    # band-limit a little so parabolic interp behaves like speech-ish signal
    sig = [0.0] * n
    for i in range(2, n):
        sig[i] = 0.55 * base[i] + 0.3 * base[i - 1] + 0.15 * base[i - 2]

    failures = 0
    for true_delay in (0, 1, 12, 200, 799):
        a = sig[:]                                   # cat-11: no delay
        b = [0.0] * true_delay + sig[: n - true_delay]  # cat-3: delayed
        noise_b = [v + random.gauss(0, 30) for v in b]
        got = xcorr_peak(a, noise_b, 1600)
        if got["lag_samples"] != true_delay:
            failures += 1
            print(f"FAIL true={true_delay} got={got}")
        if got["peak_corr"] < 0.9:
            failures += 1
            print(f"FAIL low corr for true={true_delay}: {got}")

    # negative control: uncorrelated signals must warn (low peak)
    other = [random.gauss(0, 3000) for _ in range(n)]
    got = xcorr_peak(sig, other, 1600)
    if got["peak_corr"] > 0.2:
        failures += 1
        print(f"FAIL negative control corr too high: {got}")

    if failures:
        raise SystemExit(f"self-test FAILED ({failures} failures)")
    print("xcorr_alignment self-test: PASS (delays 0/1/12/200/799 exact + negative control)")


if __name__ == "__main__":
    main()
