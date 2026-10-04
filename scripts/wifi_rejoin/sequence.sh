#!/usr/bin/env bash
# sequence.sh -- t_2a5f2312 GADGET-3: the three silent kitchen arms under ONE lock hold.
# Waits (LOCK_WAIT_MIN, default 240) for the run + kitchen acoustic locks, then A0 -> A1 -> B.
# Each arm posts its own START/STOP line; OTA arms restore golden + unregister before the next.
set -uo pipefail
W=/Volumes/fleet-scratch/workspaces/house-voice/t_2a5f2312; cd "$W"
CARD=t_2a5f2312 ROOM=kitchen
R=$HOME/Projects/pipecat-house-voice
. "$R/scripts/lib/acoustic-room-lock.sh"
LOCK_DEADLINE=$(( $(date +%s) + ${LOCK_WAIT_MIN:-240} * 60 ))
until acoustic_run_lock_acquire "rejoin-$CARD"; do
  echo "$(date +%T) run lock held: $ACOUSTIC_RUN_LOCK_HOLDER"; [ "$(date +%s)" -lt "$LOCK_DEADLINE" ] || exit 97; sleep 30
done
until acoustic_room_lock_acquire "$ROOM" "rejoin-$CARD"; do
  echo "$(date +%T) $ROOM lock held: $ACOUSTIC_ROOM_LOCK_HOLDER"; [ "$(date +%s)" -lt "$LOCK_DEADLINE" ] || { acoustic_run_lock_release; exit 97; }; sleep 30
done
trap 'acoustic_room_lock_release; acoustic_run_lock_release; echo "$(date +%T) locks released"' EXIT
hour=$(TZ=America/Los_Angeles date +%H); if [ "$hour" -lt 9 ] || [ "$hour" -ge 22 ]; then echo "REFUSED: outside 09:00-22:00 PT after lock wait"; exit 98; fi
echo "$(date +%T) LOCKS HELD, starting arms"
export SKIP_LOCKS=1
COMMIT=d5e9f41961d68af24691af784894d4c7028b4ab2
bash run_leg.sh A0 live-cd45607;                                              echo "A0 rc=$?"
bash run_leg.sh A1 instr-off images/kitchen-instr-off-d5e9f41.bin d5e9f41 $COMMIT; echo "A1 rc=$?"
bash run_leg.sh B  fast-on   images/kitchen-fast-on-d5e9f41.bin   d5e9f41 $COMMIT; echo "B rc=$?"
echo "$(date +%T) SEQUENCE DONE"
/usr/bin/python3 wifi_rejoin_bench.py report runs/*/*.jsonl
