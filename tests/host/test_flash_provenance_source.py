#!/usr/bin/env python3
"""Source and ancestry contract for the firmware build provenance gate."""

from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[2]
FLASH_SCRIPT = ROOT / "scripts" / "flash-bench.sh"
REQUIRED_BASE = "fc3edc0"
KNOWN_DIVERGENT_BUILD = "8e57d8f"


def ancestry(candidate: str) -> int:
    return subprocess.run(
        [
            "git",
            "-C",
            str(ROOT),
            "merge-base",
            "--is-ancestor",
            REQUIRED_BASE,
            candidate,
        ],
        check=False,
    ).returncode


def main() -> None:
    text = FLASH_SCRIPT.read_text()
    assert (
        'REQUIRED_FIRMWARE_BASE_COMMIT="${REQUIRED_FIRMWARE_BASE_COMMIT:-fc3edc0}"'
        in text
    )
    assert 'git -C "$REPO_ROOT" merge-base --is-ancestor' in text
    assert '"$REQUIRED_FIRMWARE_BASE_COMMIT" HEAD' in text
    assert "exit 4" in text

    subprocess.run(["bash", "-n", str(FLASH_SCRIPT)], check=True)
    assert ancestry("HEAD") == 0, "current build branch must descend from production fc3edc0"
    assert ancestry(KNOWN_DIVERGENT_BUILD) != 0, (
        "regressed 8e57d8f lineage must be rejected by the provenance premise"
    )
    print("firmware build provenance source contract: PASS")


if __name__ == "__main__":
    main()
