"""Mutation checks proving the NACK breaker tests gate its three key effects."""
from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[2]
PEER = ROOT / "xiao-esp32-s3" / "components" / "peer"
HOST_TEST = ROOT / "tests" / "host" / "test_nack_client.c"


@pytest.mark.parametrize(
    ("target", "old", "new", "failing_test"),
    [
        (
            "nack_client.c",
            """    if (!bucket->packets_sampled || bucket->outage ||
        packet_delta <= NACK_OUTAGE_MAX_PACKET_DELTA) {
      continue;
    }
""",
            """    (void)packet_delta;
    if (!bucket->packets_sampled) {
      continue;
    }
""",
            "test_circuit_breaker_excludes_zero_progress_outage_bucket",
        ),
        (
            "nack_client.h",
            "#define NACK_BREAKER_MIN_RECOVERY_PERCENT 20u",
            "#define NACK_BREAKER_MIN_RECOVERY_PERCENT 21u",
            "test_circuit_breaker_keeps_exact_twenty_percent",
        ),
        (
            "nack_client.c",
            """    c->auto_dark = 0;
    c->dark_until_ms = 0;
""",
            """    c->auto_dark = 1;
    c->dark_until_ms = 0;
""",
            "test_circuit_breaker_reprobes_after_sixty_seconds",
        ),
    ],
)
def test_breaker_mutation_is_killed(
    tmp_path: Path, target: str, old: str, new: str, failing_test: str
) -> None:
    peer = tmp_path / "peer"
    peer.mkdir()
    for name in ("nack_client.c", "nack_client.h", "nack_protocol_generated.h"):
        shutil.copy2(PEER / name, peer / name)

    path = peer / target
    source = path.read_text(encoding="utf-8")
    assert source.count(old) == 1, f"mutation seam drifted in {target}"
    path.write_text(source.replace(old, new), encoding="utf-8")

    binary = tmp_path / "test_nack_client"
    compile_result = subprocess.run(
        [
            os.environ.get("CC", "cc"),
            "-std=c99",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(peer),
            str(HOST_TEST),
            str(peer / "nack_client.c"),
            "-o",
            str(binary),
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    assert compile_result.returncode == 0, compile_result.stderr

    mutant = subprocess.run(binary, capture_output=True, text=True, check=False)
    output = mutant.stdout + mutant.stderr
    assert mutant.returncode != 0, "breaker mutant survived the host gate"
    assert failing_test in output, output
