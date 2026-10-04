// Host test for the re-dial policy (GADGET-2, t_db77e56b).
// Build: c++ -std=c++17 -Wall -Wextra -Werror test_webrtc_redial.cpp
#include <cassert>
#include <cstdio>
#include <cstring>

#include "../../xiao-esp32-s3/src/webrtc_redial.h"

static void test_ladder_shape() {
  // Fresh trigger dials now; the ladder only governs retries.
  assert(PipecatRedialPolicy::backoff_ms(0) == 0);
  for (uint32_t f = 1; f <= 15; f++)
    assert(PipecatRedialPolicy::backoff_ms(f) == 2000);
  for (uint32_t f = 16; f <= 21; f++)
    assert(PipecatRedialPolicy::backoff_ms(f) == 5000);
  for (uint32_t f = 22; f <= 27; f++)
    assert(PipecatRedialPolicy::backoff_ms(f) == 10000);
  assert(PipecatRedialPolicy::backoff_ms(28) == 30000);
  assert(PipecatRedialPolicy::backoff_ms(1000) == 30000);
}

static void test_hub_boot_window_is_covered_by_fast_retries() {
  // A hub restart takes seconds to tens of seconds to accept offers again.
  // The first 30 s of retries are 2 s apart so a hub that is back at t+N s
  // is re-offered within ~2 s of that, not after an exponential gap.
  uint32_t t = 0;
  PipecatRedialPolicy p;
  for (uint32_t f = 1; f <= PipecatRedialPolicy::kFastRetryCount; f++) {
    p.note_failure();
    t += p.next_backoff_ms();
  }
  assert(t == 30000);
  assert(p.consecutive_failures() == 15);
}

static void test_fallback_after_cap_and_elapsed_budget() {
  PipecatRedialPolicy p;
  uint32_t elapsed_ms = 0;
  bool restart = false;
  uint32_t n = 0;
  while (!restart) {
    restart = p.note_failure();
    n++;
    if (!restart)
      elapsed_ms += p.next_backoff_ms();
  }
  assert(n == PipecatRedialPolicy::kMaxConsecutiveFailures);
  // 15x2 + 6x5 + 6x10 + 12x30 = 480 s of waiting before the 40th failure:
  // under the 600 s network-watchdog base deadline, so the policy's own
  // reboot lands first and boot_guard sees the same software reset as today.
  assert(elapsed_ms == 480000);
  assert(elapsed_ms < 600000);
}

static void test_stable_session_resets_failures_flap_does_not() {
  PipecatRedialPolicy p;
  p.note_failure();
  p.note_failure();
  assert(p.consecutive_failures() == 2);
  // Connected, but only for 10 s before it drops again: still counting.
  p.note_connected_for(10000);
  assert(p.consecutive_failures() == 2);
  p.note_failure();
  assert(p.consecutive_failures() == 3);
  // 30 s of CONNECTED clears the streak; total is kept for telemetry.
  p.note_connected_for(PipecatRedialPolicy::kStableSessionMs);
  assert(p.consecutive_failures() == 0);
  assert(p.total_failures() == 3);
  assert(p.next_backoff_ms() == 0);
}

static void test_connect_timeout_is_shorter_than_the_watchdog() {
  // An answered offer that never reaches CONNECTED must be retried before
  // the 30 s reconnect watchdog would have, or the watchdog path races it.
  assert(PipecatRedialPolicy::kConnectTimeoutMs < 30000);
  assert(PipecatRedialPolicy::kConnectTimeoutMs >= 10000);  // > HTTP timeout
}

static void test_trigger_names_are_stable_telemetry() {
  assert(strcmp(pipecat_redial_trigger_name(PipecatRedialTrigger::kNone),
                "none") == 0);
  assert(strcmp(pipecat_redial_trigger_name(PipecatRedialTrigger::kPeerClosed),
                "peer_closed") == 0);
  assert(strcmp(pipecat_redial_trigger_name(PipecatRedialTrigger::kSctpClosed),
                "sctp_closed") == 0);
  assert(strcmp(pipecat_redial_trigger_name(PipecatRedialTrigger::kWatchdog),
                "watchdog") == 0);
  assert(strcmp(pipecat_redial_trigger_name(PipecatRedialTrigger::kHttp),
                "http") == 0);
  assert(strcmp(pipecat_redial_trigger_name(PipecatRedialTrigger::kOfferFailed),
                "offer_failed") == 0);
  assert(strcmp(
             pipecat_redial_trigger_name(PipecatRedialTrigger::kConnectTimeout),
             "connect_timeout") == 0);
}

int main() {
  test_ladder_shape();
  test_hub_boot_window_is_covered_by_fast_retries();
  test_fallback_after_cap_and_elapsed_budget();
  test_stable_session_resets_failures_flap_does_not();
  test_connect_timeout_is_shorter_than_the_watchdog();
  test_trigger_names_are_stable_telemetry();
  printf("webrtc redial policy: PASS\n");
  return 0;
}
