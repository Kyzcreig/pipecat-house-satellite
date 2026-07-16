# t_2ccb0829 — XVF lane experiments: prep runbook + feasibility verdicts

Status 2026-07-16: OFFLINE/FIRMWARE PREP COMPLETE. No acoustic capture has
run — the kitchen rig is owned by phase-4 (t_f2f4f499) and the theater rig is
SILENT-ONLY (Ace asleep). Every claim below is labeled prep or measured.

## What exists (all verified by execution)

| Artifact | Where | Verified how |
|---|---|---|
| Firmware branch `task/t_2ccb0829-packed-capture` (commit 35190ef, on b548c9c lineage; ancestor check vs live kitchen fw b548c9c PASS) | main repo `~/Projects/pipecat-house-satellite` (worktree at `<ws>/fw`) | full `idf.py fullclean/reconfigure/build` PASS; `strings src.bin` shows `/xvf/packed` + `/xvf/raw-capture`; 8/8 `tests/host/*_source.py` PASS |
| `GET /xvf/packed[?enable=0|1&op_all=c,s x6]` — OP_PACKED (35/13) + OP_ALL (35/23) with readback verification | firmware ota.cpp | compile + source-contract test |
| `GET /xvf/raw-capture?ms=N` — raw 48k/32-bit stereo I2S, LSB markers intact, uplink paused to silence frames | firmware ota.cpp/media.cpp | compile + source-contract test |
| `tools/xvf_packed_unpack.py` — packed capture -> 6x 16k lanes, windowed phase-continuity gate | workspace | self-test PASS (6ch round-trip x3 phases + negative control + continuity clean/broken controls) |
| `tools/xcorr_alignment.py` — cat-3 vs cat-11 delay measurement | workspace | self-test PASS (exact recovery of 0/1/12/200/799-sample delays + negative control) |
| `tools/run_cat5_ref_experiment.py` — experiment-A orchestrator (identity gate, packed capture, restore-in-finally, refuses to run without `--rig-released`) | workspace | syntax + refusal path exercised; NOT run against the rig |

## Feasibility verdicts

### (C) PACKED six-lane mode — FEASIBLE, with one caveat
- Vendor commands confirmed in frozen docs (`docs/vendor/xvf3800/xmos`,
  user guide v3.2.1 tables 10.4/3.2): OP_PACKED (resid 35 cmd 13), OP_ALL
  (35/23, 12 uint8), I2S requires 32-bit — our I2S is already 32-bit/48k
  stereo (`I2S_DATA_BIT_WIDTH_32BIT`, media.cpp), so the transport is
  compatible AS A CAPTURE PATH.
- The NORMAL opus uplink is NOT packed-compatible: both decimators
  (`stereo_48k_32bit_to_stereo_16k`, `stereo_48k_32bit_to_mono_16k`) average
  across what would be TDM boundaries. Packed mode is therefore bench-capture
  only via `/xvf/raw-capture` for now. A production packed uplink would need a
  firmware unpacker + 3ch encoder (multistream opus is available in
  esp-libopus `opus_multistream.h`) — real work, sized only after the bench
  capture proves lane quality. (Marconi consult risk #1 concurs: a lossy
  speech codec destroys the inter-sample TDM structure; production packed
  uplink implies lossless/multistream transport — FOOTGUN-asymmetric-transport
  applies to any new uplink work.)
- DEMUX PHASE FRAGILITY (Marconi risk #2, now mitigated in-tool): one
  dropped/duplicated 48 kHz sample anywhere rotates the packing phase and
  silently corrupts ALL SIX lanes downstream. `xvf_packed_unpack.py` now runs
  a windowed marker-phase continuity scan (48 ms windows) over the whole
  capture and FAILS CLOSED on the first discontinuity; self-test includes a
  dropped-sample positive control. A rejected capture means recapture, not
  salvage.
- HOT-PATH COST (Marconi risk #3): `/xvf/raw-capture` pauses the uplink and
  ships raw frames — acceptable for bench capture. Any production demux would
  land near the priority-7 `audio_publisher` 20 ms-deadline task on a device
  already carrying 94.5 KiB full-duplex I2S DMA; re-check heartbeat priority
  (FOOTGUN-heartbeat) before sizing that build.
- CAVEAT (marker scheme): the exact LSB packing-marker encoding is implemented
  in XMOS's `xvf_tools packing.py`, which is not vendored (xmos.com account
  wall; fwk_xvf GitHub 404s publicly, PyPI has no xvf-tools). The unpacker
  assumes LSB set on each PK0 sample and FAILS CLOSED (refuses to unpack) when
  no dominant marker phase exists. First bench capture must validate this
  assumption; if it fails, grab packing.py from the XMOS release bundle.
- VOLUME CORRUPTION (vendor, PG 4.1.2 note): "it is critical to ensure that
  any volume controls are disabled (volume = 100%) to prevent the packed audio
  frame being corrupted" — ANY gain stage between the XVF I2S output and the
  capture buffer flips LSBs and destroys markers + lane data. Our
  /xvf/raw-capture reads the I2S RX DMA directly with no gain, so we're clean
  by construction; keep it that way (never route packed samples through the
  opus/volume path).
- CHANNEL-ORDER vs vendor tools (measured from the v3.2.1 PG example, see
  unpacker docstring): vendor unpack numbers channels INTERLEAVED
  (ch1=L_PK0, ch2=R_PK0, ch3=L_PK1, ...); our unpacker is slot-major
  (ch0..2=L_PK0..2, ch3..5=R_PK0..2). The same vendor example
  (`OP_ALL 12 0 3 0 3 2 6 3 3 1 3 3` -> [ref, beam, MIC0..3]) uniquely pins
  the OP_ALL byte order as L_PK0,L_PK1,L_PK2,R_PK0,R_PK1,R_PK2 — exactly what
  our firmware endpoint and run_cat5_ref_experiment.PACKED_OP_ALL assume.
  That byte-order assumption is now vendor-doc-confirmed, no longer a guess.

### (B) Cat-3 vs cat-11 alignment — READY, two capture options
- Sequential option (no reflash): runtime mux probe `/xvf/audio-mux?...`
  (exists on live kitchen fw b548c9c) — capture [11,x] then [3,x] on two
  playbacks of one fixture; xcorr gives a sanity figure only (playback-start
  jitter pollutes it; the tool prints that caveat).
- Simultaneous option (proper): packed capture carrying [3,x] and [11,x]
  in one slot -> shared clock -> exact internal delay. Boot profile writes
  SYS_DELAY=12 samples; expected xcorr peak +12 samples (0.75 ms) unless the
  chip adds more. Result feeds AEC3 delay config + Lanes doc.
- NOTE: live kitchen firmware b548c9c caps runtime mux at category<=8,
  source<=3 — cat-11 probing NEEDS the widened bounds (this branch or the
  phase-4 corrected branch; both carry the identical 0..12/0..5 fix).

### (A) Cat-5 chip-reference AEC3 — DESIGNED, blocked on rig
- Two-arm A/B in `run_cat5_ref_experiment.py`: packed capture carries raw mic
  [1,3] AND chip ref [5,0] on one clock -> AEC3 offline eval (phase-4
  `aec3_offline_eval.cpp` with `export_linear_aec_output`) at lag 0; baseline
  arm = same capture scored against the server-synthesized reference at its
  best swept delay. Metric: external linear ERLE on far-end-only windows
  (per the t_f2f4f499 AEC3 deep-analysis: never the capped APM erle stat).
- If chip-ref >> synth-ref: alignment hypothesis confirmed; next step is a
  production reference lane ([5,0] or [12,0]) instead of byte-aligner
  gymnastics. If ~equal: alignment exonerated, linearity/transport remains.

## Sequencing to run the acoustic legs (when phase-4 releases the kitchen rig)

DECISION RULE (Marconi consult #1 on t_f2f4f499, 2026-07-16 — read before
spending rig time): phase-4's cat-1 [1,3] run comes first and its telemetry
gates everything here. Read the cat-1 run's delay-slope + render-continuity
counters:
- MISALIGNMENT pattern (delay unstable/drifting/pinned at bracket edge, weak
  peak, filters diverged) -> experiment (A) cat-5 chip-ref IS the indicated
  next experiment (a chip-side reference erases transport/Opus/prebuffer/
  clock-drift from the error budget — a static delay number cannot).
- FALSIFICATION pattern (delay acquired + stable, reference continuous, no
  saturation, converged — yet linear ERLE <3 dB) -> (A) will NOT save it (the
  nonlinearity is past any reference tap); stop, do not spend the rig on A/C.
- cat-1 PASSES -> (A) unnecessary; archive it.
(B) cat-3x11 xcorr is cheap and gates nothing — run opportunistically; its
customer is (A)'s delay budget. (C) packed bench-capture runs last and only
if (A) demonstrates the chip-side reference matters. Note [5,0]+[1,3] without
packed mode consumes both I2S slots and evicts the [7,3] ASR lane for the
duration — bounded-experiment acceptable, not free.

1. Identity + provenance gates (skill clanker-satellite-flash): MAC
   dc:b4:d9:38:a6:cc, ancestor check, build with real Wi-Fi creds from 1P item
   eend5ky2brz2uz6y3apfzn6rki, ROOM=kitchen env, `reconfigure` before build.
2. OTA `task/t_2ccb0829-packed-capture` image; verify /ota/status sha +
   /health active_peers>=1.
3. `python3 tools/run_cat5_ref_experiment.py --rig-released` (does C-capture,
   A-arm-2, unpack; restores production mux in finally).
4. Cat-3/11: packed capture with op_all carrying [3,0] and [11,0], unpack,
   `xcorr_alignment.py lane_chX.pcm lane_chY.pcm --simultaneous`.
5. AEC3 scoring on ACE-AI with the phase-4 evaluator (webrtc-audio-processing
   1.3, linear output). A/B receipts (raw-tap scar): every lane identity claim
   needs an echo-content/RMS/spectral comparison, not a category label.
6. Restore production firmware per phase-4's ensure_production_firmware flow;
   update Lanes doc rows Pending->measured with receipts.

## Explicit non-claims
- No ERLE/separation numbers exist yet for any of A/B/C.
- The packed-mode LSB marker assumption is UNVERIFIED against real silicon.
- The firmware image built here used dummy Wi-Fi creds (compile proof only);
  a flashable image must be rebuilt with the real env per the flash skill.
