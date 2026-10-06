#!/usr/bin/env bash
# Source contracts for the XVF tune table (kTuneEntries), NVS persistence and
# the read-only diag reader. These existed without a run_*.sh, so host-tests.yml
# (which discovers suites by run_*.sh) never executed them (t_ef9294b3).
set -euo pipefail

cd "$(dirname "$0")"
python3 test_xvf_nvs_source.py
python3 test_xvf_diag_source.py
