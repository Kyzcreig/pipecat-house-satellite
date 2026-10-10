#!/usr/bin/env bash
# Source contract for GET /xvf/dump: read-only, volatile/fingerprint split,
# rate limit (t_a527ebfa / t_d355a513).
set -euo pipefail

cd "$(dirname "$0")"
python3 test_xvf_dump_source.py
