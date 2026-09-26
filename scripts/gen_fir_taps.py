#!/usr/bin/env python3
"""Generate the polyphase windowed-sinc FIR taps used by
mono_16k_to_stereo_48k_32bit() in xiao-esp32-s3/src/media.cpp.

24-tap Hamming-windowed sinc, L=3 interpolation, fc=7kHz (at 48k),
gain L baked in, quantized Q15. Each polyphase branch (8 taps) sums
to ~32768 (unity DC gain) — verify after any change.
"""
import math

N = 24
L = 3
fc = 7000.0 / 48000.0

taps = []
for n in range(N):
    m = n - (N - 1) / 2.0
    s = 2 * fc * (1.0 if m == 0 else math.sin(2 * math.pi * fc * m) / (2 * math.pi * fc * m))
    w = 0.54 - 0.46 * math.cos(2 * math.pi * n / (N - 1))  # Hamming
    taps.append(s * w * L)

q = [round(t * 32768) for t in taps]
for p in range(L):
    ph = q[p::L]
    print(f"phase {p}: sum={sum(ph)} (want ~32768)")
print("static const int16_t kInterpFir[24] = {")
print("    " + ", ".join(str(v) for v in q) + "};")
