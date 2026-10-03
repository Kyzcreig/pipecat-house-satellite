#!/usr/bin/env bash
# Mutation check for the arrival-stall input (U1d, t_9afd3ebe): each mutation
# removes ONE property the tests claim to pin, and run_prebuffer_tests.sh must
# go RED for every one of them. A test that cannot fail is decoration.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
CTL=xiao-esp32-s3/src/prebuffer_ctl.c
MEDIA=xiao-esp32-s3/src/media.cpp
PEER=xiao-esp32-s3/components/peer/peer_connection.c
BK="$(mktemp -d)"
cp "$CTL" "$BK/ctl"; cp "$MEDIA" "$BK/media"; cp "$PEER" "$BK/peer"
restore() { cp "$BK/ctl" "$CTL"; cp "$BK/media" "$MEDIA"; cp "$BK/peer" "$PEER"; }
trap 'restore; rm -rf "$BK"' EXIT

fails=0
check() { # name
  if bash tests/host/run_prebuffer_tests.sh >/dev/null 2>&1; then
    echo "MUTATION SURVIVED (tests are blind): $1"; fails=$((fails+1))
  else
    echo "ok - tests go RED on: $1"
  fi
  restore
}
mutate() { # file perl-expr name
  local before; before="$(cksum < "$1")"
  perl -0pi -e "$2" "$1"
  if [ "$before" = "$(cksum < "$1")" ]; then
    echo "MUTATION DID NOT APPLY (script is stale): $3"; fails=$((fails+1)); restore; return
  fi
  check "$3"
}

# 1. stall input neutered: never reports a stall
mutate "$CTL" 's/  if \(!c->have_arrival\) \{/  return 0;\n  if (!c->have_arrival) {/' \
  "pbc_track_arrival neutered (returns 0)"
# 2. stall counted but never grows
mutate "$CTL" 's/    pbc_grow_one\(c\);\n    c->last_recovery_ms = now_ms;/    c->last_recovery_ms = now_ms;/' \
  "stall no longer grows a step"
# 3. stall growth does not restart the decay timer (decays immediately)
mutate "$CTL" 's/    c->last_recovery_ms = now_ms; \/\* same decay timer/    \/\* same decay timer/' \
  "stall growth skips the shared decay timer"
# 4. playing gate dropped
mutate "$CTL" 's/if \(!playing \|\| gap <= PBC_STALL_MS/if (gap <= PBC_STALL_MS/' \
  "stall counted while not playing"
# 5. resume-window bound dropped (end of utterance -> next utterance grows)
mutate "$CTL" 's/ \|\| gap > c->resume_window_ms\)/)/' \
  "next utterance after end-of-utterance counted as a stall"
# 6. dark path leaks growth
mutate "$CTL" 's/  if \(c->adaptive\) \{\n    pbc_grow_one/  if (1) {\n    pbc_grow_one/' \
  "dark build grows on a stall"
# 7. playback task stops feeding the input
mutate "$MEDIA" 's/    pbc_track_arrival\(&pbc, g_rtp_last_arrival_ms,\n\s*!prebuffering \|\| pbc.drain_pending, now_ms\);\n//' \
  "playback task no longer calls pbc_track_arrival"
# 8. RX stamp only in the diagnostic trace build
mutate "$PEER" 's/            g_rtp_last_arrival_ms = \(uint32_t\)\(esp_timer_get_time\(\) \/ 1000\);\n#if PIPECAT_ARRIVAL_TRACE\n/#if PIPECAT_ARRIVAL_TRACE\n            g_rtp_last_arrival_ms = (uint32_t)(esp_timer_get_time() \/ 1000);\n/' \
  "RX stamp moved under PIPECAT_ARRIVAL_TRACE"

echo
if [ "$fails" -eq 0 ]; then
  echo "all 8 mutations caught — the arrival-stall tests actually gate"
else
  echo "$fails mutation(s) survived or did not apply"; exit 1
fi
