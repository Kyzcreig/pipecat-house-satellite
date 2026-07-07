#!/usr/bin/env python3
"""asr_routing_lint.py — guard the XVF3800 ASR audio routing in the firmware.

WHY THIS EXISTS (bug, 2026-07-07)
---------------------------------
The XVF3800 is a *conferencing* DSP whose default output is tuned for human
full-duplex listening: the "voice-communication" path runs an aggressive
noise-suppression / de-reverb post-processor that **spectrally guts speech for
ASR**. The ESP32 mono capture path reads I2S channel 0 (the LEFT output slot).

Our firmware shipped OP_L routed to category 6 (PROCESSED / voice-comm) with
AEC ASROUTONOFF=0 (ASR-mode OFF) — so STT got the suppressed audio and garbled
clean-but-loud speech: "what time is it" -> 'a 20', "what's the temperature" ->
'Now these readings truly'. Fragmented transcripts then caused double-replies and
"no command" beeps.

The fix (ported from ESPHome respeaker_xvf3800 "Phase 2c PERMANENT FIX", proven
6/6-vs-0/4 word-recall A/B 2026-06-09): route OP_L to **category 7 (ASR)** — the
clean post-beamformer autoselect output — and set **AEC ASROUTONOFF=1**. XVF
params are volatile so this re-applies every boot in configure_xvf3800_dsp_profile().

This lint fails loudly if anyone regresses either half (OP_L off category-7, or
ASROUTONOFF back to 0), so the garbling can't ship silently.

Exit 0 = OK. Exit 1 = wrong routing (the garbling bug). Exit 2 = couldn't parse
(structure changed — a human must re-verify, not a silent pass).

Run:  python3 scripts/asr_routing_lint.py
      python3 scripts/asr_routing_lint.py --file xiao-esp32-s3/src/media.cpp
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

DEFAULT_FILE = "xiao-esp32-s3/src/media.cpp"


def check(src: str) -> tuple[int, str]:
    """Return (exit_code, message)."""
    # 1) Confirm the ASR category constant exists and equals 7.
    cat_m = re.search(r"XVF_AUDIO_CATEGORY_ASR\s*=\s*(\d+)", src)
    if cat_m is None:
        return 2, ("COULD NOT PARSE XVF_AUDIO_CATEGORY_ASR — structure changed; a human "
                   "must re-verify OP_L routes to the clean ASR beam (category 7). NOT a silent pass.")
    if cat_m.group(1) != "7":
        return 1, (f"XVF_AUDIO_CATEGORY_ASR = {cat_m.group(1)}, expected 7 (the clean ASR beam). "
                   "OP_L must carry category 7, not the suppressed voice-comm path.")

    # 2) Confirm OP_L is written with the ASR category (not PROCESSED).
    opl_m = re.search(r"XVF_CMD_AUDIO_MGR_OP_L\s*,\s*(XVF_AUDIO_CATEGORY_\w+)", src)
    if opl_m is None:
        return 2, ("COULD NOT PARSE the OP_L write — structure changed; a human must re-verify "
                   "OP_L = [category 7 (ASR), auto-select]. NOT a silent pass.")
    if opl_m.group(1) != "XVF_AUDIO_CATEGORY_ASR":
        return 1, (f"OP_L is routed to {opl_m.group(1)}, but STT reads I2S ch0 = LEFT slot and needs "
                   "the clean ASR beam. Set OP_L to XVF_AUDIO_CATEGORY_ASR (category 7). "
                   "Category 6 (PROCESSED/voice-comm) spectrally guts speech -> garbled STT "
                   "('what time is it' -> 'a 20'). 2026-07-07 bug.")

    # 3) Confirm ASROUTONOFF is set to 1 (ASR-mode ON), not 0.
    asr_m = re.search(r"XVF_CMD_AEC_ASROUTONOFF\s*,\s*(\d+)\s*\)", src)
    if asr_m is None:
        return 2, ("COULD NOT PARSE the ASROUTONOFF write — structure changed; a human must "
                   "re-verify AEC ASROUTONOFF=1 (ASR-mode ON). NOT a silent pass.")
    if asr_m.group(1) != "1":
        return 1, (f"AEC ASROUTONOFF is set to {asr_m.group(1)}, expected 1. ASR-mode OFF feeds STT "
                   "the conferencing post-processor output -> garbled transcripts. Set it to 1 "
                   "(the mate to the category-7 OP_L routing). 2026-07-07 bug.")

    return 0, ("OK — OP_L routes the clean ASR beam (category 7) to the read (left) slot and "
               "AEC ASROUTONOFF=1. STT gets unsuppressed audio.")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--file", default=DEFAULT_FILE, help="path to media.cpp")
    ap.add_argument("--repo", default=".", help="repo root")
    args = ap.parse_args(argv)

    path = Path(args.repo) / args.file
    if not path.is_file():
        print(f"asr-routing-lint: FILE NOT FOUND: {path}", file=sys.stderr)
        return 2

    src = path.read_text(encoding="utf-8", errors="replace")
    code, msg = check(src)
    stream = sys.stdout if code == 0 else sys.stderr
    label = "OK" if code == 0 else ("FAIL" if code == 1 else "PARSE")
    print(f"asr-routing-lint: {label} — {msg}", file=stream)
    return code


if __name__ == "__main__":
    raise SystemExit(main())
