// Host tests for the server-liveness oracle (t_56a17737). Reproduces the
// kitchen 2026-10-04 02:40:05 / 03:04:15 reboot timeline in milliseconds and
// proves that downlink media now holds the connection alive through a late
// ping, while every pre-existing ping-only behavior is unchanged.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "../../xiao-esp32-s3/src/reconnect_watchdog.h"
#include "../../xiao-esp32-s3/src/server_liveness.h"

static constexpr uint32_t kStale = 35000;   // WEBRTC_SERVER_HEARTBEAT_STALE_MS
static constexpr uint32_t kPingInterval = 30000;  // hub _PEER_PING_INTERVAL_S

static void test_never_pinged_is_not_fresh_even_with_media() {
  PipecatServerLiveness l(kStale);
  assert(!l.fresh(1000));
  l.note_media(1000);
  assert(!l.fresh(1000));
  assert(!l.fresh(1001));
  assert(l.media_holds == 0);
}

static void test_ping_only_keeps_the_old_35s_window() {
  PipecatServerLiveness l(kStale);
  l.note_ping(10000);
  assert(l.fresh(10000));
  assert(l.fresh(10000 + kStale));
  assert(!l.fresh(10000 + kStale + 1));
  assert(l.media_holds == 0);
}

// THE kitchen reset: peer connected at T0, pings at T0+30k, downlink TTS
// flowing, the ping due at T0+30*k is late by >5 s. Old firmware rebooted at
// T0+30*(k-1)+35 s. With media as a sign of life the reconnect watchdog never
// fires, and the late ping re-arms everything when it finally lands.
static void test_media_holds_through_a_late_ping_mid_tts() {
  PipecatServerLiveness l(kStale);
  PipecatReconnectWatchdog wd;
  uint32_t t = 0;
  l.note_ping(t);  // ping slot 0 on connect
  // Three healthy ping intervals.
  for (int k = 1; k <= 3; k++) {
    for (; t < (uint32_t)k * kPingInterval; t += 15) {
      l.note_media(t);  // 20 ms RTP cadence, modelled on the 15 ms tick
      assert(!wd.update(true, l.fresh(t), 15));
    }
    l.note_ping(t);
  }
  // Ping slot 4 (t = 120 000) never arrives on time; TTS keeps playing.
  const uint32_t old_reboot_at = 3 * kPingInterval + kStale;  // 125 000
  for (; t <= old_reboot_at + 10000; t += 15) {
    l.note_media(t);
    assert(!wd.update(true, l.fresh(t), 15));
  }
  assert(l.media_holds == 1);  // one hold episode, counted once
  assert(l.ping_gap_max_ms == kPingInterval);
  // The late ping lands 15 s after its slot; gap telemetry records it.
  l.note_ping(t);
  assert(l.fresh(t));
  assert(l.ping_gap_max_ms == t - 3 * kPingInterval);
  assert(l.ping_gap_max_ms > kStale);  // the metric the bench soak reads
  assert(l.ping_rx == 5);
  assert(!wd.update(true, l.fresh(t), 15));
}

// Media does not extend the detection latency past the LAST sign of life:
// server dies at T with media as the last signal -> stale at T + 35 s.
static void test_dead_server_still_detected_35s_after_last_media() {
  PipecatServerLiveness l(kStale);
  l.note_ping(0);
  uint32_t t = 0;
  for (; t <= 34000; t += 20)
    l.note_media(t);
  const uint32_t last_media = t - 20;  // 34 000
  // Ping stale from 35 001 on; media carries until last_media + 35 s.
  assert(l.fresh(kStale + 1));
  assert(l.fresh(last_media + kStale));
  assert(!l.fresh(last_media + kStale + 1));
  PipecatReconnectWatchdog wd;
  assert(!wd.update(true, true, 15));  // proved healthy once
  assert(wd.update(true, l.fresh(last_media + kStale + 1), 15));
}

// Idle (no media) is the silent-wedge case the ping path exists for: a stale
// ping with no media still reads dead exactly as before.
static void test_idle_stale_ping_without_media_is_dead() {
  PipecatServerLiveness l(kStale);
  l.note_ping(5000);
  l.note_media(5000);  // last playback ended long ago
  assert(l.fresh(5000 + kStale));
  assert(!l.fresh(5000 + kStale + 1));
  assert(!l.fresh(5000 + 2 * kStale));
  assert(l.media_holds == 0);  // media was stale too: no hold episode
}

// Time only moves forward: media is noted at t, then fresh(t) is asked.
static void test_hold_episode_counts_once_and_rearms_on_ping() {
  PipecatServerLiveness l(kStale);
  l.note_ping(0);
  for (uint32_t t = 0; t <= 60000; t += 20) {
    l.note_media(t);
    assert(l.fresh(t));
  }
  assert(l.media_holds == 1);  // stale from 35 001 on: one episode
  l.note_ping(60000);
  assert(l.fresh(60000));
  for (uint32_t t = 60000; t <= 120000; t += 20) {
    l.note_media(t);
    assert(l.fresh(t));
  }
  assert(l.media_holds == 2);  // the second episode started at 95 001
}

static void test_uint32_wrap_and_zero_timestamps() {
  PipecatServerLiveness l(kStale);
  l.note_ping(0);  // real timestamp 0 must not read as "never"
  assert(l.fresh(0));
  assert(l.fresh(kStale));
  const uint32_t near_wrap = 0xFFFFFFFFu - 1000u;
  PipecatServerLiveness w(kStale);
  w.note_ping(near_wrap);
  assert(w.fresh(near_wrap + 20000));  // wrapped past zero
  assert(!w.fresh(near_wrap + kStale + 1));
  assert(w.ping_age_ms(near_wrap + 20000) == 20000);
}

int main() {
  test_never_pinged_is_not_fresh_even_with_media();
  test_ping_only_keeps_the_old_35s_window();
  test_media_holds_through_a_late_ping_mid_tts();
  test_dead_server_still_detected_35s_after_last_media();
  test_idle_stale_ping_without_media_is_dead();
  test_hold_episode_counts_once_and_rearms_on_ping();
  test_uint32_wrap_and_zero_timestamps();
  puts("server liveness host tests: PASS");
  return 0;
}
