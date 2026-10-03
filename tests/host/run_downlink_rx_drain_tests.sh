#!/usr/bin/env bash
# Downlink per-pass datagram drain + UDP mailbox depth (t_5faf78b4).
set -euo pipefail
cd "$(dirname "$0")"
python3 test_downlink_rx_drain_source.py
