#!/usr/bin/env python3
"""Correlate the PIPECAT_ARRIVAL_TRACE device trace with ring drains (t_5faf78b4).

Collect: poll GET /playback/stats?trace_from=N (trace build) into <run>/trace.txt,
optionally tcpdump hub->device UDP into <run>/*.pcap (adds rel. one-way delay).

Usage: analyze_trace.py <run_dir>   (reads trace.txt, optional <tag>.pcap)
trace.txt rows: idx t_ms kind seq aux  (kind 1 ARR aux=queued-behind, 2 GAP aux=n,
3 LATE, 4 DRAIN, 5 REFILL aux=1 if gap_resume)
"""
import collections
import glob
import os
import struct
import sys

run = sys.argv[1]
# trace.txt is appended in pull order; the trace index restarts at 0 on a
# device reboot -> split into boot segments and analyse SEG (default 0).
segs, prev = [[]], -1
for ln in open(os.path.join(run, "trace.txt")):
    p = ln.split()
    if len(p) != 5 or not p[0].isdigit():
        continue
    r = tuple(int(x) for x in p)
    if r[0] < prev:
        segs.append([])
    prev = r[0]
    segs[-1].append(r)
SEG = int(os.environ.get("SEG", "0"))
print(f"boot segments: {[len(x) for x in segs]}; analysing {SEG}")
rows = sorted(set(segs[SEG]))
idxs = [r[0] for r in rows]
missing = sum(b - a - 1 for a, b in zip(idxs, idxs[1:]) if b - a > 1)
arr = [(t, seq, aux) for _, t, k, seq, aux in rows if k == 1]
print(f"records {len(rows)} (idx {idxs[0]}..{idxs[-1]}, missing {missing}); arrivals {len(arr)}")

# unwrap seq
def unwrap(seqs):
    out, base, prev = [], 0, None
    for s in seqs:
        if prev is not None and s - prev < -30000:
            base += 65536
        prev = s
        out.append(s + base)
    return out

useq = unwrap([s for _, s, _ in arr])

# --- service intervals + backlog
dts = [b[0] - a[0] for a, b in zip(arr, arr[1:])]
bk = sum(a for _, _, a in arr)
hist = collections.Counter()
edges = [0, 5, 10, 14, 17, 20, 25, 30, 40, 60, 80, 100, 140, 200, 10**9]
for d in dts:
    for lo, hi in zip(edges, edges[1:]):
        if lo <= d < hi:
            hist[(lo, hi)] += 1
print(f"queued-behind arrivals: {bk}/{len(arr)} = {100.0*bk/max(1,len(arr)):.1f}%")
print("service interval histogram (ms):")
for lo, hi in zip(edges, edges[1:]):
    if hist[(lo, hi)]:
        print(f"  [{lo:>3},{hi if hi < 10**9 else 'inf':>4}) {hist[(lo,hi)]}")
# service rate while backlogged
bd = [b[0] - a[0] for a, b in zip(arr, arr[1:]) if a[2]]
if bd:
    bd.sort()
    print(f"interval after a queued-behind arrival: median {bd[len(bd)//2]} ms, p10 {bd[len(bd)//10]} p90 {bd[9*len(bd)//10]}")
# longest backlog runs
runs, cur, start = [], 0, None
for t, s, a in arr:
    if a:
        cur += 1
        start = start if start is not None else t
    else:
        if cur:
            runs.append((cur, start, t))
        cur, start = 0, None
runs.sort(reverse=True)
print("longest queued-behind runs (n, t_start..t_end):", runs[:8])

# --- pcap: hub send time per seq
send = {}
pc = glob.glob(os.path.join(run, "*.pcap"))
if pc:
    with open(pc[0], "rb") as f:
        gh = f.read(24)
        magic = struct.unpack("<I", gh[:4])[0]
        e = "<" if magic in (0xA1B2C3D4, 0xA1B23C4D) else ">"
        nano = magic in (0xA1B23C4D, 0x4D3CB2A1)
        link = struct.unpack(e + "I", gh[20:24])[0]
        while True:
            h = f.read(16)
            if len(h) < 16:
                break
            ts, tf, cap, _ = struct.unpack(e + "IIII", h)
            d = f.read(cap)
            off = {1: 14, 113: 16, 276: 20}[link]
            ip = d[off:]
            ihl = (ip[0] & 15) * 4
            u = ip[ihl + 8:]
            if len(u) >= 12 and (u[0] >> 6) == 2 and ((u[1] & 127) == 63 or 96 <= (u[1] & 127) <= 127):
                seq = struct.unpack(">H", u[2:4])[0]
                send.setdefault(seq, ts + tf / (1e9 if nano else 1e6))
    print(f"pcap: {len(send)} RTP seqs from hub")

# relative one-way delay (device service - hub send), offset removed by min
delay = {}
if send:
    pairs = [(t, s, us) for (t, s, _), us in zip(arr, useq) if s in send]
    raw = [t - send[s] * 1000.0 for t, s, _ in pairs]
    if raw:
        m = min(raw)
        for (t, s, us), r in zip(pairs, raw):
            delay[us] = r - m
        ds = sorted(delay.values())
        q = lambda p: ds[int(p * (len(ds) - 1))]
        print(f"rel one-way delay ms: p50 {q(.5):.1f} p90 {q(.9):.1f} p99 {q(.99):.1f} p99.9 {q(.999):.1f} max {ds[-1]:.1f}")
        # hub seqs never serviced on device = lost (air or socket overflow)
        lo, hi = min(useq), max(useq)
        got = set(s for _, s, _ in arr)
        lost = [s for s in send if s not in got]
        print(f"hub-sent seqs never serviced on device: {len(lost)}")

# --- drain/refill episodes
ev = [(t, k, a) for _, t, k, _, a in rows if k in (4, 5)]
eps = []
for (t0, k0, a0), (t1, k1, a1) in zip(ev, ev[1:]):
    if k0 == 4 and k1 == 5:
        eps.append((t0, t1, a1))
gr = [e for e in eps if e[2] == 1]
print(f"drain->refill episodes {len(eps)}, gap_resumes {len(gr)}")
ai = {t: i for i, (t, _, _) in enumerate(arr)}
import bisect
ts_list = [t for t, _, _ in arr]
gaps = [(t, s, a) for _, t, k, s, a in rows if k == 2]
for t0, t1, _ in gr:
    i = bisect.bisect_left(ts_list, t0 - 400)
    j = bisect.bisect_right(ts_list, t1 + 100)
    win = arr[i:j]
    wdt = [b[0] - a[0] for a, b in zip(win, win[1:])]
    big = max(wdt) if wdt else None
    gi = [g for g in gaps if t0 - 600 <= g[0] <= t1 + 100]
    print(f"\nGAP_RESUME drain@{t0} refill@{t1} (+{t1-t0} ms); max service gap in window {big} ms; seq gaps {[(g[0], g[2]) for g in gi]}")
    for t, s, a in win:
        us = useq[ts_list.index(t)] if t in ai else None
        d = delay.get(us)
        print(f"   t={t} seq={s} queued={a}" + (f" delay={d:.0f}" if d is not None else ""))
