# Track-B playback rasp — investigation state (2026-07-09 01:15, session end)

## THE DECISIVE RESULT
A/B on the SAME kitchen hardware, same room, same sentence:
- **ESPHome firmware (restored from backup): "fantastic, super clean, no raspiness at all"** (Ace, ears)
- Track-B pipecat firmware: raspy — through every fix below
=> Hardware fully exonerated. The defect is IN the Track-B playback chain, and it is findable.

## Current device state
- kitchen = ESPHOME (restored from backups/kitchen-esphome-20260706/kitchen-full-backup.bin,
  sha 1c81924b...). Rejoined HA as media_player.clanker_kitchen. ESPHome hub transport is
  RETIRED so voice doesn't work on kitchen — it's in A/B state deliberately.
- theater = Track-B (commit ae87e9c firmware), still on :7860, voice works (raspy).
- Track-B kitchen firmware to restore after investigation: build with
  PIPECAT_SMALLWEBRTC_URL=http://192.168.1.216:7861/api/offer, SATELLITE_ID=kitchen,
  AGC 0.015 (see scripts/flash-bench.sh + AGENTS/skill for env).

## Fixed-but-not-the-root (all real, all committed, keep them)
1. Cartesia native-16k spectral gutting -> synth 24k + resample (server 0db726f)
2. ZOH upsampler imaging -> linear (d248940) -> windowed-sinc FIR (4300137)
3. FIR intersample clipping (worst-case sum 49022 -> peaks 36052>32767) -> taps x0.668 (ae87e9c)
4. Silence pre-roll injected MID-SPEECH via is_playing flap -> REVERTED (f1fb2d3)
5. DMA ring 40ms -> 128ms (0681725) — harmless, keep
6. XVF far_extgain 12->0, ref_gain 8->1 live (volatile, reverts on reboot) — "a lot better but still raspy"

## Measured facts
- 997Hz sine through Track-B /test-tone path, mic'd (Shure MV7+): THD H2-H7 all -46..-55dBc,
  imaging products -96dBc, noise floor -57dBc => the STATIC tone path measures CLEAN.
- Speech through the same path: audibly raspy. Tone clean + speech raspy => the defect is
  DYNAMIC (bursty writes / discontinuities / underruns during real streaming), not a
  static transfer-function problem.
- ESPHome speech acoustic fingerprint (Shure capture /tmp/esphome_cap.wav on Mac +
  ace-ai /tmp/): bands 0-300:2.1% 300-1k:76.9% 1-4k:8.6% 4-8k:9.6% 8-16k:2.8%.

## PRIME SUSPECTS for tomorrow (in order)
1. **Write-path timing/underruns during streamed speech.** ESPHome plays via a 100ms
   buffer_duration speaker component + mixer + resampler pipeline feeding I2S
   continuously. Track-B writes opus-packet-sized chunks (20ms) directly into
   i2s_channel_write from the webrtc callback thread — bursty, no decoupling task/ring
   between decode and I2S. A late/short write = DMA underrun = gap. The /test-tone path
   also goes through OutputAudioRawFrame -> same server->webrtc->device path, so it
   rasps too. The 997Hz tone measuring clean but speech rasping is consistent with
   content-dependent packet timing (tone = perfectly regular frames).
   -> Fix shape: dedicated playback task + ring buffer on the ESP32 (decouple decode
   from I2S write), matching ESPHome's architecture.
2. **AIC3104 codec register defaults.** ESPHome's aic3104 component only writes DAC
   volume (0x2B/0x2C) — but the ESPHome BOOT may leave codec analog config from the
   XVF DFU default whereas our init_i2c_and_codec() writes something different.
   -> Diff our codec init writes vs whatever ESPHome relies on (check init_i2c_and_codec
   in media.cpp vs the restored-ESPHome running register state if readable).
3. i2s slot config (bit_shift/left_align on TX) — lower probability given tone measures
   clean, but cheap to diff against ESPHome's generated i2s driver config.

## Tools/rigs that exist now
- /test-tone?sound=<name> plays any staged .pcm on a room (server/sounds/*.pcm; abtest_full,
  abtest_quiet, sine997, humanwake staged on ace-ai)
- Shure MV7+ capture on Mac Studio: rec -q -r 48000 -c 1 /tmp/cap.wav trim 0 14
  (trigger playback in a subshell with sleep 3; ffmpeg avfoundation truncates — use rec)
- ESPHome-side playback for A/B: HA tts.speak entity tts.kokoro ->
  media_player.clanker_kitchen (proven tonight)
- XVF live tuning (Track-B only): POST http://<dev>/xvf/tune?param=...&value=...

## Session-end service state (all healthy)
- Both webrtc servers active; theater peer connected; kitchen peer ABSENT (ESPHome on device)
- Tier-3 Wave 1: COMPLETE (all 7 phases, see docs/closeout-tier3-wave1.md in pipecat-house-voice)
- Brain + scheduler + mining cron: live
