#!/usr/bin/env bash
# run_leg.sh <tag> <ARMLABEL> [<img> <project_ver> <COMMIT>]
# t_2a5f2312 GADGET-3: ONE silent Wi-Fi re-join leg on the KITCHEN (.185), adapted from
# t_092093f1/run_soak.sh. Under the run + kitchen acoustic locks, DAYTIME ONLY (09:00-22:00 PT).
#   1. notify home chat START (silent leg: no audio)
#   2. if <img> given: device must be booted on golden (rule 1: golden stays on the other
#      slot); OTA <img> to the kitchen (MAC + satellite_id asserted, sha verified), register
#      it as a TEST-ONLY successor (nightly guard stays green), wait valid + peer + settle
#   3. 20 UniFi kick-sta trials (wifi_rejoin_bench.py run) -- NO audio
#   4. notify STOP; if <img> given and RESTORE_GOLDEN=1: POST /ota/rollback -> golden, verify
#      valid + peer, unregister the successor
# Output: runs/<tag>/{ota_status_*.json, register.txt, bench log/jsonl/summary}
set -uo pipefail
TAG=$1 ARM=$2 IMG=${3:-} VER=${4:-} COMMIT=${5:-}
W=/Volumes/fleet-scratch/workspaces/house-voice/t_2a5f2312
OUT=$W/runs/$TAG; mkdir -p "$OUT"
ROOM=kitchen CARD=t_2a5f2312 IP=192.168.1.185 MAC=dc:b4:d9:38:a6:cc PORT=7861
TRIALS=${TRIALS:-20} SETTLE=${SETTLE:-20}
R=$HOME/Projects/pipecat-house-voice
RS=/srv/ci/scratch/$CARD
GOLD_VER=${GOLD_VER:-cd45607}

hour=$(TZ=America/Los_Angeles date +%H)
if [ "$hour" -lt 9 ] || [ "$hour" -ge 22 ]; then echo "REFUSED: outside 09:00-22:00 PT"; exit 98; fi

note() {  # home chat line (Apollo rule: start/stop per leg). Delivery is mandatory.
  HERMES_NOTIFY_REAL=1 python3 "$HOME/.hermes/scripts/notify.py" --profile default --sev "${2:-info}" --channel telegram \
    --target 571820863 --send "$CARD: $1" || { echo "NOTIFY FAILED rc=$?: $1"; exit 9; }
}

if [ "${SKIP_LOCKS:-0}" != 1 ]; then  # sequence.sh holds the locks across all arms and sets SKIP_LOCKS=1
  . "$R/scripts/lib/acoustic-room-lock.sh"
  LOCK_DEADLINE=$(( $(date +%s) + ${LOCK_WAIT_MIN:-90} * 60 ))
  until acoustic_run_lock_acquire "rejoin-$CARD"; do
    echo "$(date +%T) run lock held: $ACOUSTIC_RUN_LOCK_HOLDER"; [ "$(date +%s)" -lt "$LOCK_DEADLINE" ] || exit 97; sleep 30
  done
  until acoustic_room_lock_acquire "$ROOM" "rejoin-$CARD"; do
    echo "$(date +%T) $ROOM lock held: $ACOUSTIC_ROOM_LOCK_HOLDER"; [ "$(date +%s)" -lt "$LOCK_DEADLINE" ] || { acoustic_run_lock_release; exit 97; }; sleep 30
  done
  hour=$(TZ=America/Los_Angeles date +%H); if [ "$hour" -lt 9 ] || [ "$hour" -ge 22 ]; then echo "REFUSED: outside 09:00-22:00 PT after lock wait"; acoustic_room_lock_release; acoustic_run_lock_release; exit 98; fi
  trap 'acoustic_room_lock_release; acoustic_run_lock_release' EXIT
fi
J() { curl -s -m5 "http://$IP/$1"; }
peers() { curl -s -m3 "http://192.168.1.216:$PORT/health" | /usr/bin/python3 -c 'import json,sys;print(json.load(sys.stdin).get("active_peers"))' 2>/dev/null; }
wait_valid_ver() {  # $1=version
  for i in $(seq 1 60); do sleep 3; s=$(J ota/status) && [ -n "$s" ] && echo "$s" | grep -q "\"firmware_version\":\"$1\"" && echo "$s" | grep -q '"ota_state":"valid"' && break; done
  for i in $(seq 1 40); do h=$(peers); [ "$h" = "1" ] && break; sleep 3; done
}

ping -c1 -W2 $IP >/dev/null; got=$(arp -n $IP | awk '{print $4}' | head -1)
norm() { /usr/bin/python3 -c 'import sys;print(":".join(f"{int(p,16):02x}" for p in sys.argv[1].split(":")))' "$1" 2>/dev/null || echo "$1"; }
[ "$(norm "$got")" = "$MAC" ] || { echo "MAC MISMATCH arp=$got want=$MAC"; exit 3; }
J ota/status | tee "$OUT/ota_status_before.json" | grep -q "\"satellite_id\":\"$ROOM\"" || { echo "NOT $ROOM"; exit 3; }

note "$ROOM Wi-Fi re-join drills START (GADGET-3 arm $ARM, SILENT: $TRIALS UniFi kicks, no audio, ~$(( TRIALS * (SETTLE + 10) / 60 + 3 )) min$( [ -n "$IMG" ] && echo ", 1 OTA reboot first"))"

# Rule 1: golden stays on the OTHER slot -> device must be BOOTED on golden before an upload;
# and the A0 baseline must be measured ON golden, so the gate applies to every arm.
if ! grep -q "\"firmware_version\":\"$GOLD_VER\"" "$OUT/ota_status_before.json"; then
  echo "device not on golden ($(sed -n 's/.*"firmware_version":"\([^"]*\)".*/\1/p' "$OUT/ota_status_before.json")); POST /ota/rollback first"
  curl -sS -m8 -X POST "http://$IP/ota/rollback"; echo; sleep 25; wait_valid_ver "$GOLD_VER"
  s=$(J ota/status); echo "$s" > "$OUT/ota_status_pregolden.json"
  echo "$s" | grep -q "\"firmware_version\":\"$GOLD_VER\"" && echo "$s" | grep -q '"ota_state":"valid"' && [ "$(peers)" = 1 ] \
    || { note "$ROOM re-join drills STOP (aborted: could not get the kitchen onto golden $GOLD_VER first)" warn; exit 3; }
  echo "settle 60 s on golden"; sleep 60
fi
if [ -n "$IMG" ]; then
  WANT=$(shasum -a 256 "$IMG" | cut -c1-64)
  ssh ace-ai-lan "install -d $RS"
  scp -q "$IMG" "ace-ai-lan:$RS/$TAG.bin"; scp -q "$W/register_arm.py" "$W/unregister_arm.py" "ace-ai-lan:$RS/"
  echo "$(date +%T) pre: $(J ota/status | head -c 300)"
  curl -sS -m 120 -w ' HTTP%{http_code}\n' -X POST --data-binary @"$IMG" -H 'Content-Type: application/octet-stream' "http://$IP/ota/upload" | tee "$OUT/ota_upload.txt"
  grep -q "\"sha256\":\"$WANT\"" "$OUT/ota_upload.txt" || { echo "UPLOAD SHA MISMATCH"; note "$ROOM re-join drills STOP (aborted: upload sha mismatch)" warn; exit 4; }
  SLOT=$(sed -n 's/.*"boot_slot":"\(ota_[01]\)".*/\1/p' "$OUT/ota_upload.txt")
  sleep 25
  for i in $(seq 1 60); do sleep 3; s=$(J ota/status) && [ -n "$s" ] && echo "$s" | grep -q "\"sha256\":\"$WANT\"" && break; done
  BOOTED=$(echo "$s" | sed -n 's/.*"sha256":"\([0-9a-f]*\)".*/\1/p')
  ssh ace-ai-lan "python3 -c 'import json;json.dump(json.load(open(\"/home/ace/firmware-golden/golden-2026-10-03/$ROOM/build-config.json\"))[\"build_environment\"],open(\"$RS/build_env.json\",\"w\"))' && \
    ARM='$ARM' ARMDESC='GADGET-3 wifi re-join arm $ARM' BASE='cd45607' COMMIT='$COMMIT' PROJECT_VER='$VER' python3 $RS/register_arm.py $ROOM $RS/$TAG.bin $BOOTED $SLOT $RS/build_env.json" | tee -a "$OUT/register.txt"
  wait_valid_ver "$VER"
  echo "settle 60 s"; sleep 60
  s=$(J ota/status); echo "$s" > "$OUT/ota_status_arm.json"
  echo "$s" | grep -q "\"sha256\":\"$WANT\"" && echo "$s" | grep -q '"ota_state":"valid"' && echo "$s" | grep -q "\"firmware_version\":\"$VER\"" && [ "$(peers)" = 1 ] \
    || { echo "ARM NOT VALID / NO PEER: $(echo "$s" | head -c 300)"; note "$ROOM re-join drills STOP (aborted: arm image not valid or no peer)" warn; exit 4; }
  echo "$s" | grep -q '"wifi_rejoin_last_ms"' || { echo "no wifi_* counters on /ota/status"; exit 4; }
fi

# --- the leg: silent kicks ------------------------------------------------------
/usr/bin/python3 "$W/wifi_rejoin_bench.py" --room kitchen run --arm "$ARM" --trials "$TRIALS" --settle "$SETTLE" --out "$OUT" ${NEAR_AP:+--near-ap "$NEAR_AP"}
LEG_RC=$?
J ota/status > "$OUT/ota_status_after.json"
SUMMARY=$(ls -t "$OUT"/*.summary.json 2>/dev/null | head -1)
LINE=$( [ -n "$SUMMARY" ] && /usr/bin/python3 -c 'import json,sys;s=json.load(open(sys.argv[1]));print("n=%s median=%s p95=%s basis=%s near_ap=%s/%s fast/fallback=%s/%s reboots=%s peer_lost=%s pass=%s"%(s["n"],s["median_ms"],s["p95_ms"],s["basis"],s["near_ap_hits"],s["n"],s["path_fast"],s["path_fallback"],s["reboots"],s["peer_lost"],s["pass"]))' "$SUMMARY" )
note "$ROOM Wi-Fi re-join drills STOP (arm $ARM rc=$LEG_RC: $LINE)"

# --- restore golden (rule 1/3: rollback is one reboot) ------------------------------
if [ -n "$IMG" ] && [ "${RESTORE_GOLDEN:-1}" = 1 ]; then
  echo "restoring golden $GOLD_VER via POST /ota/rollback"; curl -sS -m8 -X POST "http://$IP/ota/rollback" | tee "$OUT/rollback.txt"; echo
  sleep 25; wait_valid_ver "$GOLD_VER"
  s=$(J ota/status); echo "$s" > "$OUT/ota_status_restored.json"; h=$(peers)
  echo "$s" | /usr/bin/python3 -c 'import json,sys;d=json.load(sys.stdin);print("RESTORED",{k:d.get(k) for k in ("booted_slot","ota_state","firmware_version","sha256","uptime_s")})'
  if echo "$s" | grep -q "\"firmware_version\":\"$GOLD_VER\"" && echo "$s" | grep -q '"ota_state":"valid"' && [ "$h" = 1 ]; then
    note "$ROOM restored to golden $GOLD_VER (ota_state valid, peer up) after GADGET-3 arm $ARM"
  else
    echo "rollback did not land golden; fallback: OTA golden from the archive"
    ssh ace-ai-lan "curl -sS -m 120 -w ' HTTP%{http_code}\n' -X POST --data-binary @/home/ace/firmware-golden/golden-2026-10-03/$ROOM/src.bin -H 'Content-Type: application/octet-stream' http://$IP/ota/upload" | tee "$OUT/ota_upload_golden.txt"
    sleep 25; wait_valid_ver "$GOLD_VER"; s=$(J ota/status); echo "$s" > "$OUT/ota_status_restored.json"; h=$(peers)
    if echo "$s" | grep -q "\"firmware_version\":\"$GOLD_VER\"" && echo "$s" | grep -q '"ota_state":"valid"' && [ "$h" = 1 ]; then
      note "$ROOM restored to golden $GOLD_VER by OTA re-upload after GADGET-3 arm $ARM"
    else
      note "$ROOM NOT confirmed on golden after rollback+reupload (fw=$(echo "$s" | sed -n 's/.*"firmware_version":"\([^"]*\)".*/\1/p') peers=$h) - operator check needed" warn; exit 6
    fi
  fi
  [ "${UNREGISTER:-1}" = 1 ] && ssh ace-ai-lan "python3 $RS/unregister_arm.py $ROOM $WANT" | tee -a "$OUT/register.txt"
fi
exit $LEG_RC
