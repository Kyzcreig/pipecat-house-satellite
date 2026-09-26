#!/usr/bin/env python3
"""device_identity.py — resolve which XVF3800 room a USB board is, BEFORE flashing.

WHY THIS EXISTS (mix-up, 2026-07-05)
------------------------------------
A device connected to the Mac over USB was flashed while being called a "bench
spare" — it was actually the LIVE **theater** satellite, so the flash silently
dropped theater off the house hub. Root cause: reasoning about the device by
NICKNAME / assumed location instead of HARDWARE IDENTITY (MAC).

This tool reads the board's MAC over the USB serial link and maps it to a room
via the registry below. Use it as a flash PREFLIGHT: it prints the room and, in
--assert mode, exits non-zero if the connected board is NOT the room you intended
to flash — so you can't nuke a live satellite by mistake.

Registry is the single source of truth for "which MAC is which room." Keep it in
sync with the clanker-e2e skill's identity map. VERIFY on physical moves.

Usage:
    python3 scripts/device_identity.py                 # identify connected board
    python3 scripts/device_identity.py --assert theater  # exit 1 unless it's theater
    python3 scripts/device_identity.py --port /dev/cu.usbmodem4101
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from glob import glob

# room -> lowercase MAC (colon-separated). Ground-truthed 2026-07-05.
# VERIFY after any physical relocation; mirror in the clanker-e2e skill.
REGISTRY: dict[str, str] = {
    "theater": "1c:db:d4:74:64:84",
    "kitchen": "dc:b4:d9:38:a6:cc",
}


def default_port() -> str | None:
    ports = sorted(glob("/dev/cu.usbmodem*") + glob("/dev/ttyUSB*") + glob("/dev/ttyACM*"))
    return ports[0] if ports else None


def read_mac(port: str, esptool: str = "esptool") -> str | None:
    """Read the board MAC via esptool. Returns lowercase colon MAC or None."""
    # Try, in order: the given esptool (may be a full path or shell string),
    # `esptool` on PATH, then `<python> -m esptool` for several candidate pythons
    # (the IDF venv python is the one that reliably has esptool).
    import shlex

    base = ["--chip", "esp32s3", "-p", port, "--before", "default_reset",
            "--after", "no_reset", "read_mac"]
    candidates: list[list[str]] = []
    if esptool and esptool != "esptool":
        candidates.append(shlex.split(esptool) + base)
    candidates.append(["esptool"] + base)
    candidates.append([sys.executable, "-m", "esptool"] + base)
    # IDF venv pythons commonly present on the fleet Mac.
    for py in glob(os.path.expanduser("~/.espressif/python_env/*/bin/python")):
        candidates.append([py, "-m", "esptool"] + base)

    for cmd in candidates:
        try:
            out = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        except (FileNotFoundError, subprocess.TimeoutExpired):
            continue
        blob = (out.stdout or "") + (out.stderr or "")
        m = re.search(r"MAC:\s*([0-9a-fA-F:]{17})", blob)
        if m:
            return m.group(1).lower()
    return None


def room_for(mac: str) -> str | None:
    mac = mac.lower()
    for room, known in REGISTRY.items():
        if known.lower() == mac:
            return room
    return None


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", default=None, help="serial port (default: first cu.usbmodem*)")
    ap.add_argument("--assert", dest="assert_room", default=None,
                    help="exit non-zero unless the connected board is this room")
    ap.add_argument("--esptool", default="esptool")
    args = ap.parse_args(argv)

    port = args.port or default_port()
    if not port:
        print("device-identity: no USB serial port found (no /dev/cu.usbmodem*).", file=sys.stderr)
        return 2

    mac = read_mac(port, esptool=args.esptool)
    if not mac:
        print(f"device-identity: could NOT read MAC on {port} (esptool missing/failed). "
              f"Do NOT flash blind — resolve identity first.", file=sys.stderr)
        return 2

    room = room_for(mac)
    label = room or "UNKNOWN (not in registry)"
    print(f"device-identity: {port} MAC={mac} -> room={label}")

    if args.assert_room:
        if room is None:
            print(f"device-identity: ASSERT FAIL — board {mac} is not in the registry; "
                  f"refusing to treat it as '{args.assert_room}'. Add it or check the cable.",
                  file=sys.stderr)
            return 1
        if room != args.assert_room:
            print(f"device-identity: ASSERT FAIL — you intended '{args.assert_room}' but the "
                  f"connected board is '{room}' ({mac}). Flashing would hit the WRONG satellite. "
                  f"STOP.", file=sys.stderr)
            return 1
        print(f"device-identity: ASSERT OK — connected board IS '{args.assert_room}'. Safe to flash.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
