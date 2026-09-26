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
depending on XVF3800 microphone bring-up. For real XVF3800 microphone capture,
set `PIPECAT_BENCH_SEND_TONE=0`.

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

### 2026-05-21 20:07-20:22 PDT resume validation

- Status: passed Skynet WebRTC loopback and XVF3800 I2C probe on bench board.
  Mic capture is narrowed but not closed.
- Firmware commits: `346d8c5` plus local follow-up converter diagnostics/fix.
- Command:
  ```bash
  PIPECAT_BENCH_SEND_TONE=0 \
    PIPECAT_SMALLWEBRTC_URL=http://192.168.1.78:7860/api/offer \
    scripts/flash-bench.sh flash monitor
  ```
- Flash completed on `/dev/cu.usbmodem4101`; final image size was
  `0x11eb20`, leaving `0x614e0` bytes free in the app partition.
- Skynet health before test: `{"ok":true,"service":"pipecat-house-pipeline","stt_provider":"local","wake_gate_enabled":true,"active_peers":0}`.
- Board reached Skynet: `PeerConnectionState` progressed through
  `checking` -> `connected` -> `completed`; `DataChannel created` logged.
- XVF3800 control probe passed over I2C:
  `XVF3800 alive at 0x2c, DFU firmware v6.0.0`.
- Raw I2S reads intermittently showed non-zero samples, but the 16 kHz mono
  frame sent to Opus was still `mono=0`. Root cause: the downsampler sampled a
  fixed slot/phase that missed the sparse non-zero I2S samples.
- Follow-up patch changed the downsampler to select the highest-magnitude
  sample from each 48 kHz stereo group and kept the `mono=` diagnostic.
- The patched image flashed once (`src.bin` `0x11eb70`, `0x61490` bytes free),
  but post-flash serial auto-reset/monitor became unreliable. Post-patch
  `mono` verification still needs a physical reset/boot-mode assist or another
  successful serial session.
- Post-test Skynet health eventually returned `active_peers:0`; the board is
  not left actively streaming to Skynet.

Board monitor excerpt:

```text
I (660) pipecat: XVF3800 alive at 0x2c, DFU firmware v6.0.0
I (4725) pipecat: Connecting to http://192.168.1.78:7860/api/offer
I (5454) pipecat: PeerConnectionState: connected
I (6100) pipecat: PeerConnectionState: completed
I (6261) pipecat: DataChannel created
I (6416) pipecat: mic capture: 50/50 ok, 0 zero-byte, last_err=ESP_OK, peak |s16|=3004 raw=196909376
I (7416) pipecat: mic capture: 50/50 ok, 0 zero-byte, last_err=ESP_OK, peak |s16|=9193 raw=602501312
I (11416) pipecat: mic capture: 50/50 ok, 0 zero-byte, last_err=ESP_OK, peak |s16|=18298 raw=1199152768
I (23592) pipecat: mic capture: 50/50 ok, 0 zero-byte, last_err=ESP_OK, peak |s16|=1842 mono=0 raw=120754432
```

Remaining gap: physical speaker playback still needs a human-audible bench
test. Post-patch mono mic verification is also pending because serial
auto-reset stopped entering the ESP32 bootloader reliably.

### 2026-05-21 local aiortc validation

- Status: passed local WebRTC loopback on bench board.
- Date: 2026-05-21.
- Board: XIAO ESP32-S3, MAC `1c:db:d4:74:64:84`, USB serial
  `/dev/cu.usbmodem4101`.
- Server endpoint used: `http://192.168.1.18:7860/api/offer`.
- Skynet endpoint was not reachable during the initial validation:
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
