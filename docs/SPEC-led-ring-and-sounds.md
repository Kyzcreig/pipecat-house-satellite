# SPEC — LED Ring + Acknowledgement Sounds (port from ESPHome, keep pipecat-esp32)

**Status:** DRAFT → executing Stage 1 + LED-confirm
**Author:** Apollo, 2026-07-06
**Goal:** Replicate the ESPHome `respeaker_xvf3800` **LED ring** behavior and its
**designed FLAC acknowledgement sounds** (wake / mute-on / mute-off / timer) on our
**pipecat-esp32 / WebRTC** firmware — WITHOUT switching back to ESPHome. Make both a
**canonical part of the flash setup** going forward.

## 0. Why this is a port, not a reverse-engineer
The entire ESPHome source is on disk at
`~/Projects/pipecat-house-voice/deploy/esphome-wakeword-firmware/components/respeaker_xvf3800/`.
Every mechanism below was read directly from `respeaker_xvf3800.cpp/.h`. There is
nothing to decompile — it's a code-read + translate into our `media.cpp`.

## 1. Ground truth — the exact mechanisms (read from ESPHome source)

### 1.1 LED ring — XMOS-side, over the I2C bus we already use
- **12 RGB LEDs**, driven entirely by the XVF3800 chip (NOT the ESP32).
- **One write:** 48-byte payload = `12 × [B, G, R, 0x00]` (little-endian: byte0=Blue,
  byte1=Green, byte2=Red, byte3=White/unused).
- **Address:** `GPO_SERVICER_RESID = 20`, cmd `GPO_SERVICER_RESID_LED_RING_VALUE = 18`.
- ESPHome fn (`respeaker_xvf3800.cpp:860`):
  ```cpp
  void set_led_ring(uint32_t *rgb_array) {           // rgb_array[12], each 0x00RRGGBB
    uint8_t payload[48];
    for (int i=0;i<12;i++){ uint32_t c=rgb_array[i];
      payload[i*4+0]= c & 0xFF;         // B
      payload[i*4+1]= (c>>8)&0xFF;       // G
      payload[i*4+2]= (c>>16)&0xFF;      // R
      payload[i*4+3]= 0x00; }
    xmos_write_bytes(20, 18, payload, 48);
  }
  ```
- **Our equivalent primitive** already exists: `xvf_write_bytes(resid,cmd,val,len)`
  (`media.cpp:129`) — structurally identical to ESPHome's `xmos_write_bytes`.
  ⚠️ **GOTCHA:** our `xvf_write_bytes` hard-caps `value_len > 29` (buffer
  `uint8_t payload[32]`). The 48-byte LED payload will be rejected. → add a dedicated
  `xvf_write_led_ring(const uint32_t rgb[12])` with a **51-byte** buffer (3 header + 48),
  or widen the shared helper. Spec chooses a dedicated helper (surgical, no risk to the
  proven DSP-config path).

### 1.2 Mute — XMOS GPIO 30, same servicer
- **Read state** (`read_mute_status`, cpp:669): request `{20, 0|0x80, 6}`, read 6 bytes,
  `muted = (data[2] & 0x01)` (byte[1] of the 5 GPO bytes → data index 2 incl. status).
- **Write state** (`write_mute_status`, cpp:684): `{20, 1, 2, 30, value}` — GPO write,
  pin 30, value 0/1.
- We can mirror both with `xvf_write_bytes(20,1,{30,val},2)` and a small GPO read.

### 1.3 Beam direction — already computed in our firmware
- ESPHome reads `AEC_SERVICER_RESID=33`, `AEC_AZIMUTH_VALUES_CMD=75` → 4 floats
  (beam1, beam2, free-running, auto-select) → radians → LED index.
- **We ALREADY do this:** `xvf_read_float4(XVF_RESID_AEC, XVF_CMD_AEC_AZIMUTH_VALUES=75)`
  (`media.cpp:691`) + `azimuth_to_led(radians)` (`media.cpp:236`, 12 sectors of 30°).
  Today we only LOG `led=N`. Stage 3 = light that LED instead of dropping it.

### 1.4 Sounds — ESP32 speaker path, FLAC files (NOT chip-generated)
- The "nice" ack is `wake_word_triggered.flac` from the Home Assistant Voice PE repo
  (`esphome/home-assistant-voice-pe`, `sounds/`). Same for:
  - `mute_switch_on.flac`, `mute_switch_off.flac`
  - `timer_finished.flac`
  - `wake_word_triggered.flac` (wake ack)
- Played through the speaker via ESPHome `media_player`/`play_sound`. We already own a
  proven speaker path (`WakeBeepProcessor` → WebRTC → device Opus-decode → i2s, AND the
  device-side `pipecat_audio_decode → i2s_play_buffer`).

## 2. Color / state design (from ESPHome effects + HA Voice PE convention)
| State | Color (0xRRGGBB) | Pattern |
|---|---|---|
| idle / off | `0x000000` | ring off (or very dim breathing if desired) |
| listening (wake fired, mic open) | `0x00A9E0` (HA cyan) | solid ring |
| thinking (STT→brain) | `0x1E88E5` (blue) | single dot spinning |
| speaking (TTS playing) | `0x00A9E0` | breathing |
| beam / DoA | speaker color | single LED at `azimuth_to_led` sector |
| muted | `0xF44336` (red) | solid dim ring (or 2 opposing dots) |
| error | `0xF44336` | 3 quick flashes |

Brightness scale factor (all channels × k, k≈0.4 default) — env-tunable
`PIPECAT_LED_BRIGHTNESS`.

## 3. Stages (lowest-risk-first)

### Stage 1 — nice wake sound, server-side (no firmware) ✅ FIRST
- Fetch `wake_word_triggered.flac` (+ mute/timer) into `server/sounds/`.
- In `webrtc_server.py` `WakeBeepProcessor`: decode the FLAC once at startup to 16 kHz
  mono PCM; on wake, emit that instead of `_make_beep_pcm()` sine.
- Env `CLANKER_WAKE_SOUND` (path) so it's swappable without code.
- Proof: `/test-tone?sound=wake` → record via Shure/camera → confirm the chime (not sine).

### LED-confirm — single I2C write, prove the ring is XMOS-wired
- Add `xvf_write_led_ring()` + a one-shot "all LEDs dim red" call at end of
  `configure_xvf3800_dsp_profile()`, gated by `PIPECAT_LED_SELFTEST=1`.
- Build + flash kitchen (on USB). **Watch for the ring to light.** This is the one
  hardware unknown (is the ring on XMOS like the ReSpeaker dev board?). Cheap, reversible.

### Stage 2 — state-driven LED ring (firmware)
- `led_set_state(LedState)` maps state→colors[12]→`xvf_write_led_ring`.
- Hook points in our firmware: WebRTC connected (idle), wake fired (listening),
  first TTS frame arrives (speaking), TTS end (idle). Wake event comes from the server
  over the data channel — OR locally if we add on-device wake later. Interim: drive
  listening/speaking from the audio-decode start/stop (`is_playing`).

### Stage 3 — beam-direction LED
- On the existing 1 Hz beam poll (`media.cpp:687`), when `az_ok`, light
  `azimuth_to_led(azimuth[3])` in the speaker color while listening.

### Stage 4 — on-device FLAC (lower latency, works during server restart)
- Embed the FLACs via `EMBED_FILES` in CMake (`target_add_binary_data` /
  `.rodata` blobs). Decode with the existing Opus/PCM path? FLAC needs a decoder —
  simplest: pre-decode FLAC → raw 16 kHz mono PCM at build time (a Python step in
  `flash-bench.sh`), embed the **PCM** blob, play via `i2s_play_buffer`. No on-device
  FLAC decoder needed. Trigger locally on wake/mute for zero-latency ack.

### Stage 5 — mute button + sounds
- Poll GPO mute state (or wire a GPIO/touch). On mute→ red ring + `mute_switch_on` PCM;
  on unmute→ ring off + `mute_switch_off`.

## 4. Canonical flash setup (the "going forward" ask)
- `flash-bench.sh`: add a `PRECOMPUTE_SOUNDS` step (FLAC→PCM blob) + default
  `PIPECAT_LED_ENABLE=1`.
- New env knobs (all baked at CMake configure — remember `idf.py reconfigure`):
  `PIPECAT_LED_ENABLE`, `PIPECAT_LED_BRIGHTNESS`, `PIPECAT_LED_SELFTEST`,
  `CLANKER_WAKE_SOUND`.
- Document in `docs/HANDOFF-*` + `clanker-e2e` skill so every future flash includes it.

## 5. Risks / unknowns
- **R1 (only real hardware unknown):** is the LED ring wired to XMOS on THIS board
  (XIAO ESP32-S3 + XVF3800) or absent? → the LED-confirm step settles it in one flash.
- **R2:** 48-byte write exceeds our helper cap → dedicated helper (handled).
- **R3:** on-device FLAC decode → sidestepped by pre-decoding to PCM at build time.
- **R4:** LED writes on the I2C bus during active audio — keep them off the audio hot
  path (the 1 Hz beam poll already shares the bus fine; LED writes are ~51 bytes, rare).
