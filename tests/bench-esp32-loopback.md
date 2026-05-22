# XIAO ESP32-S3 Bench WebRTC Loopback

## Target

- Board: XIAO ESP32-S3 + ReSpeaker XVF3800 bench unit
- USB serial: `/dev/cu.usbmodem4101`
- Firmware target: `xiao-esp32-s3`
- Board profile: `boards/xiao-esp32-s3.config`
- Branch: `phase2c/pipecat-esp32-fork`

## Reproduce

```bash
cd /Users/alexgierczyk/Projects/pipecat-house-satellite
PIPECAT_SMALLWEBRTC_URL=http://skynet.local:7860/api/offer \
  scripts/flash-bench.sh flash monitor
```

`scripts/flash-bench.sh` pulls the Wi-Fi password from 1Password when
`WIFI_PASSWORD` is not already set. It defaults `PIPECAT_BENCH_SEND_TONE=1`
so the ESP32 sends a generated 16 kHz square-wave test tone upstream without
depending on XVF3800 microphone bring-up.

For local bench validation when Skynet is unavailable:

```bash
python3.13 -m venv /tmp/pipecat-loopback-venv
/tmp/pipecat-loopback-venv/bin/pip install aiohttp aiortc av
/tmp/pipecat-loopback-venv/bin/python scripts/loopback-tone-server.py \
  --host 0.0.0.0 --port 7860
PIPECAT_SMALLWEBRTC_URL=http://192.168.1.18:7860/api/offer \
  scripts/flash-bench.sh flash
```

## Expected Power-On Log

```text
I (...) pipecat: Connecting to WiFi SSID: Wi-Fight this Feeling
I (...) pipecat: got ip:<bench-board-ip>
I (...) pipecat: Connecting to http://skynet.local:7860/api/offer
I (...) pipecat: PeerConnectionState: connected
I (...) pipecat: DataChannel created
```

## Validation Log

- Status: passed local WebRTC loopback on bench board.
- Date: 2026-05-21.
- Board: XIAO ESP32-S3, MAC `1c:db:d4:74:64:84`, USB serial
  `/dev/cu.usbmodem4101`.
- Server endpoint used: `http://192.168.1.18:7860/api/offer`.
- Skynet endpoint was not reachable during validation:
  `skynet.local` did not resolve and `192.168.1.78:7860` timed out.
- Build/flash gate: `scripts/flash-bench.sh build` and
  `scripts/flash-bench.sh flash` completed with ESP-IDF 5.5.3. Final image
  size was `0x11dfa0` bytes, leaving `0x62060` bytes free in the app
  partition.

Board monitor excerpt:

```text
I (704) pipecat: Connecting to WiFi SSID: Wi-Fight this Feeling
I (1315) wifi:connected with Wi-Fight this Feeling
I (2845) pipecat: got ip:192.168.1.97
I (3201) pipecat: Connecting to http://192.168.1.18:7860/api/offer
I (8380) pipecat: PeerConnectionState: checking
I (8398) pipecat: PeerConnectionState: connected
INFO ... dtls_srtp.c 308 Created inbound SRTP session
INFO ... dtls_srtp.c 328 Created outbound SRTP session
I (9173) pipecat: PeerConnectionState: completed
I (9231) pipecat: DataChannel created
```

Loopback server excerpt:

```text
peer-1: offer received
peer-1: inbound track kind=audio
peer-1: answer sent
peer-1: state=connecting
peer-1: state=connected
peer-1: inbound audio frame received
```

Notes:

- aiortc returned sha-256, sha-384, and sha-512 fingerprints. The fork now
  overlays libpeer's SDP parser to pin the remote DTLS fingerprint to the
  sha-256 line, matching libpeer's certificate digest check.
- The firmware defaults the console to USB Serial/JTAG so GPIO43/GPIO44 remain
  available for I2S on the XIAO bench board.
- Audible speaker output was not independently verified by a human during this
  run. The ESP32 decoded the returned RTP stream but logged I2S write timeouts,
  so physical speaker playback remains part of the XVF3800 clock/control port.
  The WebRTC transport and ESP-to-server audio path were verified.
