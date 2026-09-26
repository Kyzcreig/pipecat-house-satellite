# Dual-stream firmware notes

## Build flag

`PIPECAT_DUAL_STREAM` is disabled by default. The normal build retains the
existing mono path: both XVF3800 output slots are `[7,3]`, the ESP32 averages the
slots during 48 kHz to 16 kHz decimation, and Opus remains mono at a 30 kb/s
target.

Build the experimental firmware by exporting the flag for CMake:

```sh
PIPECAT_DUAL_STREAM=1 idf.py reconfigure build
```

Do not deploy a flag-on firmware build until the receiving server is configured
to negotiate, decode, and split stereo Opus. A mono receiver can discard or mix
the lanes and violate the routing contract below.

## Firmware channel contract

With the flag enabled, the ESP32 configures and preserves two independent
XVF3800 lanes:

| Opus channel | I2S slot | XVF mux | Consumer |
| --- | --- | --- | --- |
| 0 (left) | left | `[7,3]` ASR auto-select beam | STT feed |
| 1 (right) | right | `[6,3]` post-processed auto-select beam | wake/barge detector feed |

`AEC_ASROUTONOFF=1` remains required for channel 0 to carry the clean ASR beam.
Both XVF slots are explicitly upsampled onto the existing 48 kHz stereo I2S bus.
The ESP32 decimates each slot independently to 16 kHz and passes interleaved
left/right PCM to a two-channel Opus encoder; it never averages the lanes in the
flag-on path.

The server must split the decoded stereo frame immediately. Forward only the
left channel to VAD/STT and use only the right channel for wake/barge scoring,
including while playback is active. Downstream STT, brain, and TTS processing
remain mono. Channel order is part of the transport contract and must not be
inferred from signal level.

## Bitrate and resource cost

The flag-on encoder uses a nominal 60 kb/s Opus target instead of the current
30 kb/s mono target: approximately +30 kb/s, or 2x the encoded audio payload,
before RTP/SRTP/UDP/IP overhead and normal Opus VBR variation. Packet cadence
stays at one 20 ms frame (320 samples per channel) every 20 ms.

For comparison, uncompressed 16 kHz signed 16-bit PCM rises from 256 kb/s mono
to 512 kb/s stereo. The board-level I2S wire rate does not increase because that
link already carries 48 kHz stereo 32-bit samples. Incremental device costs are
the second decimation lane, a stereo Opus encode, and a 640-sample capture
buffer instead of 320 samples.

## Rollback and verification

Unset `PIPECAT_DUAL_STREAM` and rebuild to restore `[7,3]/[7,3]`, mono averaging,
a one-channel Opus encoder, and the 30 kb/s target. The host-side static gate is:

```sh
python3 scripts/dualstream_lint.py
python3 scripts/asr_routing_lint.py
```

Before any device test, verify the matching server split independently. A live
flag-on device should read back `op_l=[7,3]`, `op_r=[6,3]`, `upsample=[1,1]`, and
`asr_on=1`; a rollback build should read back `op_l=[7,3]`, `op_r=[7,3]`, and
`asr_on=1`.
