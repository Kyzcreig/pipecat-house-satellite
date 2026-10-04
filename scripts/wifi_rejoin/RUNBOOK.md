# t_2a5f2312 GADGET-3 — runbook (KITCHEN target, SILENT, daytime 09:00–22:00 PT)

Pass criterion (vault note `Muse Gadget SDK vs Clanker ... (2026-10-04).md` line 152):
median re-join < 2 s AND p95 < 5 s AND joined BSSID is the near AP 20/20.

## Rulings that shape this leg
- Apollo 09:25 PT 10-04 (Ace 09:22): test target = KITCHEN (.185, hub :7861). The bench (.97,
  Ace's bedroom) is hands-off until S2. `wifi_rejoin_bench.py` refuses `--room bench`.
- Apollo 11:50 PT 10-04 (Ace 11:42): kitchen workloads are SILENT. This leg is UniFi
  `kick-sta` + ICMP + `/ota/status` reads only; it never calls /announce or /test-tone.
- Kitchen rule 1: golden cd45607 stays on the other slot (`run_leg.sh` refuses to upload unless
  the device is booted on golden; restore = `POST /ota/rollback`). Arm images are
  cd45607 + cherry-pick of the fast-rejoin commit (boot_guard-protected lineage).
- One holder at a time: `run_leg.sh` waits for the run + kitchen acoustic locks.
- START/STOP lines to the home chat per leg (notify.py --profile default).

## Arms (20 kicks each, `--settle 20`, ~10 min per arm)
| arm | image | what it measures |
|---|---|---|
| A0 `live-cd45607` | golden as flashed | baseline, ping-gap clock only (no counters) |
| A1 `instr-off` | `kinstr/src.bin` (d5e9f41, flag OFF) | live behaviour + device clock `wifi_rejoin_last_ms` |
| B `fast-on` | `kfast/src.bin` (d5e9f41, PIPECAT_WIFI_FAST_REJOIN=1) | the Muse pattern |
| B1 `lock-<ap>` (free arm, optional) | A1 or B image + UniFi client AP lock | does the UI lock change landing AP / time |

## Steps (from the card workspace; images built with `B=kinstr bash build_arm_kitchen.sh` /
`B=kfast PIPECAT_WIFI_FAST_REJOIN=1 bash build_arm_kitchen.sh`)
1. `bash run_leg.sh A0 live-cd45607`                                   (no OTA)
2. `bash run_leg.sh A1 instr-off sat-cd45607/xiao-esp32-s3/kinstr/src.bin d5e9f41 <full sha>`
3. `bash run_leg.sh B fast-on sat-cd45607/xiao-esp32-s3/kfast/src.bin d5e9f41 <full sha>`
4. optional B1: `wifi_rejoin_bench.py lock-ap --ap <mac>` → leg → `unlock-ap` (snapshot first)
5. `python3 wifi_rejoin_bench.py report runs/*/*.jsonl` → the hand-back table.
Each OTA leg restores golden (rollback, verified valid + peer) and unregisters its TEST-ONLY
successor before exiting.

## Verdict rules
- adopt only if arm B PASSes all three legs (device clock) AND peer_lost == 0 AND reboots == 0.
- A1 vs A0 ping medians must agree within ~300 ms, else the device clock is suspect — say so.
- if B fails: no PR; decision row "no change" with the table; firmware branch stays unmerged.
