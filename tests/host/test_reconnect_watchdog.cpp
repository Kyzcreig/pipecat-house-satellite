#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "../../xiao-esp32-s3/src/reconnect_watchdog.h"

static void test_reconnects_after_thirty_seconds_unhealthy() {
  PipecatReconnectWatchdog watchdog;

  assert(!watchdog.update(false, false, 29999));
  assert(watchdog.update(false, false, 1));
}

static void test_connected_peer_without_server_heartbeat_is_unhealthy() {
  PipecatReconnectWatchdog watchdog;

  assert(!watchdog.update(true, false, 29999));
  assert(watchdog.update(true, false, 1));
}

static void test_fresh_heartbeat_resets_accumulated_unhealthy_time() {
  PipecatReconnectWatchdog watchdog;

  assert(!watchdog.update(false, false, 20000));
  assert(!watchdog.update(true, true, 15));
  assert(!watchdog.update(false, false, 20000));
}

static void test_peer_state_and_heartbeat_must_both_be_healthy() {
  PipecatReconnectWatchdog watchdog;

  assert(!watchdog.update(false, true, 29999));
  assert(watchdog.update(false, true, 1));
}

int main() {
  test_reconnects_after_thirty_seconds_unhealthy();
  test_connected_peer_without_server_heartbeat_is_unhealthy();
  test_fresh_heartbeat_resets_accumulated_unhealthy_time();
  test_peer_state_and_heartbeat_must_both_be_healthy();
  puts("reconnect watchdog host tests: PASS");
  return 0;
}
