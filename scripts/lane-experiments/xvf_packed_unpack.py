#!/usr/bin/env python3
"""Unpack XVF3800 packed-mode captures: stereo 48 kHz/32-bit -> six 16 kHz lanes.

XMOS XVF3800 v3.2.1 packed mode (AUDIO_MGR_OP_PACKED) time-division-multiplexes
three 16 kHz signals into each 48 kHz I2S slot. Sample n of PK0, then PK1, then
PK2, repeating. Packing markers live in the LSB of each 32-bit sample and encode
the packing sequence (programming guide 4.1.2: "Signal packing uses
least-significant bit markers to encode the channel packing sequence").

MARKER CAVEAT (honest limitation): the exact vendor marker encoding is
implemented in XMOS's xvf_tools packing.py, which we do not have (the release
bundle is behind an xmos.com account; not vendored in docs/vendor/xvf3800/).
The vendor error message "Over 50 markers incorrectly spaced so giving up"
implies markers recur at a fixed spacing. This tool assumes the natural scheme:
LSB set on the first sample of each 3-sample group (PK0 position), clear
elsewhere. detect_phase() measures LSB density per (mod 3) position and fails
closed when no dominant phase exists, so a real capture with a different
marker scheme is REPORTED, not silently mis-unpacked. Verify against a real
kitchen-bench capture before trusting channel identity ordering.

Input formats:
  .pcm/.raw : interleaved stereo int32 LE frames (L0,R0,L1,R1,...), 48 kHz
  .wav      : 2-channel 32-bit integer PCM WAV, 48 kHz

Output: <out_prefix>_ch0..5.pcm (s16le 16 kHz mono) and a JSON summary on
stdout. Channels 0..2 = L slot PK0..PK2, channels 3..5 = R slot PK0..PK2.

Self-test: python3 xvf_packed_unpack.py --self-test
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
import wave
from pathlib import Path


def read_capture(path: Path) -> tuple[list[int], list[int]]:
    """Return (left_samples, right_samples) as int32 lists."""
    if path.suffix.lower() == ".wav":
        with wave.open(str(path), "rb") as stream:
            if stream.getnchannels() != 2:
                raise SystemExit(f"need 2-channel WAV, got {stream.getnchannels()}")
            if stream.getsampwidth() != 4:
                raise SystemExit(f"need 32-bit WAV, got {stream.getsampwidth() * 8}-bit")
            raw = stream.readframes(stream.getnframes())
    else:
        raw = path.read_bytes()
    if len(raw) % 8:
        raw = raw[: len(raw) - (len(raw) % 8)]
    flat = struct.unpack(f"<{len(raw) // 4}i", raw)
    return list(flat[0::2]), list(flat[1::2])


def lsb_density(samples: list[int]) -> list[float]:
    """Fraction of samples with LSB set at each (mod 3) position."""
    counts = [0, 0, 0]
    totals = [0, 0, 0]
    for i, s in enumerate(samples):
        pos = i % 3
        totals[pos] += 1
        counts[pos] += s & 1
    return [c / t if t else 0.0 for c, t in zip(counts, totals)]


def detect_phase(samples: list[int], *, dominance: float = 3.0) -> tuple[int, list[float]]:
    """Find the (mod 3) offset of PK0 by LSB marker density.

    Fails closed (SystemExit) when no position dominates by `dominance`x —
    that means the capture is not packed, or the vendor marker scheme differs
    from our assumption and the capture must be inspected by hand first.
    """
    density = lsb_density(samples)
    ranked = sorted(range(3), key=lambda p: density[p], reverse=True)
    best, second = ranked[0], ranked[1]
    if density[best] < 0.5 or density[best] < dominance * max(density[second], 1e-9):
        raise SystemExit(
            "no dominant LSB marker phase found "
            f"(densities={['%.4f' % d for d in density]}); capture is either "
            "not packed or uses a different marker scheme — inspect manually"
        )
    return best, density


def unpack_slot(samples: list[int], phase: int) -> list[list[int]]:
    """Deinterleave one 48 kHz slot into three 16 kHz channels (PK0..PK2).

    Leading samples before the first PK0 position are dropped (vendor tools
    likewise discard partial frames to preserve time alignment). Marker LSB is
    stripped; output stays int32 (audio in the top bits).
    """
    start = phase % 3
    channels: list[list[int]] = [[], [], []]
    usable = samples[start:]
    usable = usable[: len(usable) - (len(usable) % 3)]
    for i, s in enumerate(usable):
        channels[i % 3].append(s & ~1)
    return channels


def to_s16(ch: list[int]) -> bytes:
    out = bytearray()
    for s in ch:
        v = s >> 16
        if v > 32767:
            v = 32767
        if v < -32768:
            v = -32768
        out += struct.pack("<h", v)
    return bytes(out)


def rms(ch: list[int]) -> float:
    if not ch:
        return 0.0
    acc = 0
    for s in ch:
        v = s >> 16
        acc += v * v
    return (acc / len(ch)) ** 0.5


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", nargs="?", help="stereo 48k/32-bit packed capture (.pcm/.raw/.wav)")
    parser.add_argument("out_prefix", nargs="?", help="output path prefix for _ch0..5.pcm")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        self_test()
        return
    if not args.capture or not args.out_prefix:
        parser.error("capture and out_prefix are required (or use --self-test)")

    left, right = read_capture(Path(args.capture))
    summary: dict = {"input": args.capture, "frames_48k": len(left), "slots": {}}
    for slot_name, samples, base in (("L", left, 0), ("R", right, 3)):
        phase, density = detect_phase(samples)
        channels = unpack_slot(samples, phase)
        slot_info = {"marker_phase": phase, "lsb_density": [round(d, 4) for d in density], "channels": {}}
        for k, ch in enumerate(channels):
            out_path = Path(f"{args.out_prefix}_ch{base + k}.pcm")
            out_path.write_bytes(to_s16(ch))
            slot_info["channels"][f"ch{base + k}"] = {
                "path": str(out_path),
                "samples_16k": len(ch),
                "duration_s": round(len(ch) / 16000, 3),
                "rms_s16": round(rms(ch), 2),
            }
        summary["slots"][slot_name] = slot_info
    print(json.dumps(summary, indent=2))


def self_test() -> None:
    """Synthesize a packed stereo stream per our marker assumption and verify
    exact round-trip recovery of all six channels, at all three phases."""
    import math

    def make_channel(freq: float, n: int, amp: int) -> list[int]:
        return [int(amp * math.sin(2 * math.pi * freq * i / 16000)) << 16 for i in range(n)]

    n16 = 1600  # 100 ms
    chans = [make_channel(200 + 130 * k, n16, 6000 + 900 * k) for k in range(6)]

    failures = 0
    for lead in (0, 1, 2):  # simulate capture starting mid-group
        left_packed: list[int] = []
        right_packed: list[int] = []
        for i in range(n16):
            for k in range(3):
                marker = 1 if k == 0 else 0
                left_packed.append((chans[k][i] & ~1) | marker)
                right_packed.append((chans[3 + k][i] & ~1) | marker)
        left_packed = left_packed[lead:]
        right_packed = right_packed[lead:]

        for slot_name, packed, base in (("L", left_packed, 0), ("R", right_packed, 3)):
            phase, _ = detect_phase(packed)
            out = unpack_slot(packed, phase)
            for k in range(3):
                expect = [s & ~1 for s in chans[base + k]]
                # leading partial group dropped => compare the overlap
                got = out[k]
                skip = 1 if lead else 0
                exp_cmp = expect[skip : skip + len(got)]
                if got[: len(exp_cmp)] != exp_cmp:
                    failures += 1
                    print(f"FAIL lead={lead} slot={slot_name} ch={base + k}")

    # negative control: unpacked (non-packed) stream must be rejected
    plain = [((i * 2654435761) & 0xFFFF) << 12 for i in range(4800)]
    try:
        detect_phase(plain)
        failures += 1
        print("FAIL: negative control accepted a non-packed stream")
    except SystemExit:
        pass

    if failures:
        raise SystemExit(f"self-test FAILED ({failures} failures)")
    print("xvf_packed_unpack self-test: PASS (6ch round-trip x 3 phases + negative control)")


if __name__ == "__main__":
    main()
