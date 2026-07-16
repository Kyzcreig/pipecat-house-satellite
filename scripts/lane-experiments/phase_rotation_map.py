#!/usr/bin/env python3
"""Windowed marker-phase map: locate phase rotations across a packed capture."""
import struct, sys
from pathlib import Path

raw = Path(sys.argv[1]).read_bytes()
raw = raw[: len(raw) - (len(raw) % 8)]
flat = struct.unpack(f"<{len(raw)//4}i", raw)
L = list(flat[0::2])

W = 2304  # 48 ms
phases = []
for start in range(0, len(L) - W, W):
    counts = [0, 0, 0]
    for i in range(W):
        counts[(start + i) % 3] += L[start + i] & 1
    dens = [c / (W / 3) for c in counts]
    low = min(range(3), key=lambda p: dens[p])
    clean = dens[low] < 0.2 and sorted(dens)[1] > 0.8
    phases.append((start, low if clean else -1, [round(d, 2) for d in dens]))

# summarize runs of constant phase
runs = []
cur = None
for start, ph, dens in phases:
    if cur is None or ph != cur[1]:
        if cur is not None:
            runs.append(cur)
        cur = [start, ph, start]
    cur[2] = start
if cur:
    runs.append(cur)
print(f"windows={len(phases)} total_48k_samples={len(L)}")
for s, ph, e in runs:
    dur_ms = (e - s + W) / 48
    print(f"phase={ph:2d} from sample {s:>8} to {e + W:>8} ({dur_ms:8.0f} ms)")
