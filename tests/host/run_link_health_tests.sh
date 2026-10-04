#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"
python3 test_link_health_source.py
