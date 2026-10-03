#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"
python3 test_audio_publisher_stack_source.py
