#!/usr/bin/env python3
"""i2s_role_lint.py — guard the XVF3800 I2S master/slave role in the firmware.

WHY THIS EXISTS (bug, 2026-07-05)
---------------------------------
The pipecat-esp32 firmware configured the ESP32-S3 I2S peripheral as
``I2S_ROLE_MASTER``. But the XVF3800 (formatBCE/Seeed I2S-master firmware
v1.0.7; an earlier mis-framed I2C read misreported it as "DFU v6.34.4") on the
theater/kitchen boards
is itself the I2S **master** — it drives BCLK/WS off its own audio pipeline, which
is exactly what the working ``respeaker_xvf3800`` ESPHome component assumes
(``i2s_mode: secondary``, i.e. the ESP32 is the I2S secondary/slave).

With BOTH ends driving the clock, the captured 32-bit words came back
bit-smeared / rail-pinned: consecutive samples shared 28 bits, ``peak`` sat pinned
at ``0x8000`` (32768), and the server-side RMS read ~13600 IDENTICALLY in silence
and under a test tone — pure clock-fight garbage, not mic audio. VAD/wake could
never fire. The one-line fix was ``I2S_ROLE_MASTER`` -> ``I2S_ROLE_SLAVE``.

This lint fails loudly if anyone flips the RX (capture) channel back to
ROLE_MASTER, so the regression can't ship silently. It is a cheap static check:
it reads media.cpp, finds the ``init_i2s()`` channel config, and asserts the role.

Exit 0 = OK. Exit 1 = wrong role (the tonight bug). Exit 2 = couldn't parse
(structure changed — human must re-verify, not a silent pass).

Run:  python3 scripts/i2s_role_lint.py
      python3 scripts/i2s_role_lint.py --file xiao-esp32-s3/src/media.cpp
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

DEFAULT_FILE = "xiao-esp32-s3/src/media.cpp"


def find_init_i2s_role(src: str) -> str | None:
    """Return the .role value used inside init_i2s()'s i2s_chan_config_t, or None."""
    # Isolate the init_i2s() function body (up to the next top-level 'static' or EOF).
    m = re.search(r"\bstatic\s+void\s+init_i2s\s*\([^)]*\)\s*\{", src)
    if not m:
        return None
    body = src[m.end():]
    # Cut at the chan_cfg initializer's role field.
    role_m = re.search(r"i2s_chan_config_t\s+chan_cfg\s*=\s*\{.*?\.role\s*=\s*(I2S_ROLE_\w+)",
                       body, re.DOTALL)
    if not role_m:
        return None
    return role_m.group(1)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--file", default=DEFAULT_FILE, help="path to media.cpp")
    ap.add_argument("--repo", default=".", help="repo root")
    args = ap.parse_args(argv)

    path = Path(args.repo) / args.file
    if not path.is_file():
        print(f"i2s-role-lint: FILE NOT FOUND: {path}", file=sys.stderr)
        return 2

    src = path.read_text(encoding="utf-8", errors="replace")
    role = find_init_i2s_role(src)

    if role is None:
        print(
            "i2s-role-lint: COULD NOT PARSE init_i2s() i2s_chan_config_t .role — "
            "structure changed; a human must re-verify the ESP32 is I2S SLAVE "
            "(XVF3800 is the clock master). NOT a silent pass.",
            file=sys.stderr,
        )
        return 2

    if role == "I2S_ROLE_SLAVE":
        print(f"i2s-role-lint: OK — ESP32 RX channel is {role} (XVF3800 drives the clock).")
        return 0

    print(
        f"i2s-role-lint: FAIL — init_i2s() sets .role = {role}, but the XVF3800 is the "
        f"I2S MASTER. ESP32 must be I2S_ROLE_SLAVE or the mic capture is bit-smeared "
        f"rail-pinned garbage (2026-07-05 bug: peak pinned 0x8000, rms identical in "
        f"silence and under tone; wake never fires). Fix: I2S_ROLE_SLAVE.",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
