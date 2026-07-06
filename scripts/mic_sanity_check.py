#!/usr/bin/env python3
"""mic_sanity_check.py — runtime E2E guard for the XVF3800 I2S mic path.

WHY THIS EXISTS (bug, 2026-07-05)
---------------------------------
An I2S master/slave role inversion made the ESP32 read bit-smeared, rail-pinned
garbage from the XVF3800 instead of real mic audio. The tell was unmistakable in
the SERVER-side inbound-audio RMS (the reliable oracle — the device's own serial
`peak|s16|` diag is a never-reset high-water mark and is USELESS):

  * BROKEN:  rms ~13000+ and peak pinned at 32768 (0x8000) CONSTANTLY — identical
             in silence and under sound. Never varies. (clock-fight garbage)
  * HEALTHY: rms ~10-30 in a quiet room, peak < ~500; rises proportionally when
             real sound is present (a noise burst read rms ~4000, peak ~25000).

This checker parses the webrtc_server.py "INBOUND AUDIO rms=.. peak=.." log lines
over a window and FAILS if the signature matches the broken pattern (high floor +
pinned peak that doesn't move). It is meant to be run right after a firmware flash
during a quiet moment, as the runtime companion to the static i2s_role_lint.py.

Usage (on ACE-AI, where the server runs):
  journalctl --user -u clanker-webrtc-test --since "30 seconds ago" -o cat \\
    | python3 mic_sanity_check.py
  # or point it at a file of captured log lines:
  python3 mic_sanity_check.py --file /tmp/rms.log

Exit 0 = mic path looks healthy (quiet reads quiet).
Exit 1 = BROKEN signature (rail-pinned / constant high floor) — the I2S bug.
Exit 2 = no INBOUND AUDIO samples found (server not running / no peer / no tap).
"""
from __future__ import annotations

import argparse
import re
import sys

RMS_RE = re.compile(r"INBOUND AUDIO rms=(\d+) peak=(\d+)")

# Thresholds. The broken mode sat at rms~13000/peak=32768 constantly. A quiet
# room reads rms<~100. We flag "broken" when the MINIMUM rms across the window is
# already high (never goes quiet) AND peak is pinned near full-scale throughout.
BROKEN_MIN_RMS = 2000        # even the quietest frame is loud -> not real silence
PINNED_PEAK = 32000          # peak essentially at int16 rail
QUIET_RMS_OK = 400           # a healthy quiet-room floor is well under this


def evaluate(samples: list[tuple[int, int]]) -> tuple[int, str]:
    if not samples:
        return 2, "no INBOUND AUDIO samples found (server down / no peer / tap absent)."

    rmss = [r for r, _ in samples]
    peaks = [p for _, p in samples]
    min_rms = min(rmss)
    max_rms = max(rmss)
    min_peak = min(peaks)
    n = len(samples)

    # BROKEN: never quiets down AND peak pinned at the rail on every sample.
    if min_rms >= BROKEN_MIN_RMS and min_peak >= PINNED_PEAK:
        return 1, (
            f"BROKEN mic signature over {n} samples: min_rms={min_rms} (never goes "
            f"quiet) + peak pinned >= {PINNED_PEAK} on every frame (min_peak={min_peak}). "
            f"This is the 2026-07-05 I2S clock-fight garbage — check init_i2s() role "
            f"is I2S_ROLE_SLAVE (run i2s_role_lint.py) and reflash."
        )

    # HEALTHY quiet: at least one frame reads a real quiet floor.
    if min_rms <= QUIET_RMS_OK:
        return 0, (
            f"HEALTHY over {n} samples: quiet floor min_rms={min_rms} (peak {min_peak}), "
            f"max_rms={max_rms}. Mic reads quiet when quiet — I2S path OK."
        )

    # Ambiguous: elevated but not pinned. Likely genuine room noise; warn, pass.
    return 0, (
        f"AMBIGUOUS-PASS over {n} samples: min_rms={min_rms}, peak range "
        f"[{min_peak}..{max(peaks)}]. No pinned-rail signature; likely real ambient "
        f"sound. Re-run in a quiet moment to confirm the floor drops < {QUIET_RMS_OK}."
    )


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--file", default=None, help="log file (default: stdin)")
    args = ap.parse_args(argv)

    text = open(args.file).read() if args.file else sys.stdin.read()
    samples = [(int(m.group(1)), int(m.group(2))) for m in RMS_RE.finditer(text)]

    code, msg = evaluate(samples)
    prefix = {0: "mic-sanity: OK —", 1: "mic-sanity: FAIL —", 2: "mic-sanity: NODATA —"}[code]
    print(f"{prefix} {msg}", file=sys.stderr if code else sys.stdout)
    return code


if __name__ == "__main__":
    raise SystemExit(main())
