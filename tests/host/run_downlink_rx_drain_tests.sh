#!/usr/bin/env bash
# Downlink per-pass datagram drain; UDP mailbox stays at 6 (t_5faf78b4).
set -euo pipefail
cd "$(dirname "$0")"
python3 test_downlink_rx_drain_source.py
