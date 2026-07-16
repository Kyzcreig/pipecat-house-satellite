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
| `tools/xvf_packed_unpack.py` — packed capture -> 6x 16k lanes | workspace | self-test PASS (6ch round-trip x3 phases + negative control) |
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
  capture proves lane quality.
- CAVEAT (marker scheme): the exact LSB packing-marker encoding is implemented
  in XMOS's `xvf_tools packing.py`, which is not vendored (xmos.com account
  wall; fwk_xvf GitHub 404s publicly, PyPI has no xvf-tools). The unpacker
  assumes LSB set on each PK0 sample and FAILS CLOSED (refuses to unpack) when
  no dominant marker phase exists. First bench capture must validate this
  assumption; if it fails, grab packing.py from the XMOS release bundle.

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
