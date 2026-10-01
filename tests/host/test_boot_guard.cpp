#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../xiao-esp32-s3/src/boot_guard.h"

static PipecatBootGuardState garbage() {
  PipecatBootGuardState s;
  memset(&s, 0xA5, sizeof(s));  // RTC_NOINIT contents after a cold boot
  return s;
}

static void test_cold_power_on_never_holds_and_uses_base_deadline() {
  PipecatBootGuardState s = garbage();
  assert(!pipecat_boot_guard_on_boot(&s, PipecatResetKind::kPowerOn));
  assert(s.magic == kPipecatBootGuardMagic);
  assert(pipecat_boot_guard_hold_ms(&s) == 0);
  assert(pipecat_net_watchdog_deadline_ms(&s) == 10u * 60u * 1000u);
  assert(s.total_boots == 1);
}

static void test_garbage_rtc_after_non_poweron_reset_is_reinitialised() {
  PipecatBootGuardState s = garbage();
  // e.g. brownout on the very first boot: counters restart, then count it.
  assert(pipecat_boot_guard_on_boot(&s, PipecatResetKind::kFault));
  assert(s.fault_boots == 1);
  assert(s.netwdt_restarts == 0);
}

static void test_fault_reboots_are_never_faster_than_one_per_minute() {
  PipecatBootGuardState s = garbage();
  pipecat_boot_guard_on_boot(&s, PipecatResetKind::kPowerOn);
  const uint32_t expect[] = {60000,  120000, 240000, 480000,
                             900000, 900000, 900000};
  for (uint32_t e : expect) {
    assert(pipecat_boot_guard_on_boot(&s, PipecatResetKind::kFault));
    assert(pipecat_boot_guard_hold_ms(&s) == e);
    assert(pipecat_boot_guard_hold_ms(&s) >= 60000);
  }
  for (int i = 0; i < 40; i++)
    pipecat_boot_guard_on_boot(&s, PipecatResetKind::kFault);
  assert(s.fault_boots == kPipecatBootGuardCountCap);  // saturates, no wrap
  assert(pipecat_boot_guard_hold_ms(&s) == 900000);
}

static void test_healthy_resets_the_ladder() {
  PipecatBootGuardState s = garbage();
  pipecat_boot_guard_on_boot(&s, PipecatResetKind::kPowerOn);
  pipecat_boot_guard_on_boot(&s, PipecatResetKind::kFault);
  pipecat_boot_guard_on_boot(&s, PipecatResetKind::kFault);
  pipecat_boot_guard_on_healthy(&s);
  pipecat_boot_guard_on_boot(&s, PipecatResetKind::kFault);
  assert(pipecat_boot_guard_hold_ms(&s) == 60000);
}

static void test_plain_software_restart_is_not_a_fault() {
  // The 30 s WebRTC re-offer and OTA reboots keep their cadence.
  PipecatBootGuardState s = garbage();
  pipecat_boot_guard_on_boot(&s, PipecatResetKind::kPowerOn);
  assert(!pipecat_boot_guard_on_boot(&s, PipecatResetKind::kSoftware));
  assert(pipecat_boot_guard_hold_ms(&s) == 0);
}

static void test_network_watchdog_restart_backs_off_across_reboots() {
  PipecatBootGuardState s = garbage();
  pipecat_boot_guard_on_boot(&s, PipecatResetKind::kPowerOn);
  const uint32_t expect[] = {20u * 60000u, 40u * 60000u, 60u * 60000u,
                             60u * 60000u};
  for (uint32_t e : expect) {
    pipecat_boot_guard_note_netwdt_restart(&s);
    assert(pipecat_boot_guard_on_boot(&s, PipecatResetKind::kSoftware));
    assert(pipecat_net_watchdog_deadline_ms(&s) == e);
    assert(pipecat_boot_guard_hold_ms(&s) >= 60000);
  }
  pipecat_boot_guard_on_healthy(&s);
  assert(pipecat_net_watchdog_deadline_ms(&s) == 10u * 60000u);
}

static void test_netwdt_flag_is_consumed_once() {
  PipecatBootGuardState s = garbage();
  pipecat_boot_guard_on_boot(&s, PipecatResetKind::kPowerOn);
  pipecat_boot_guard_note_netwdt_restart(&s);
  assert(pipecat_boot_guard_on_boot(&s, PipecatResetKind::kSoftware));
  pipecat_boot_guard_on_healthy(&s);
  assert(!pipecat_boot_guard_on_boot(&s, PipecatResetKind::kSoftware));
}

static void test_net_watchdog_fires_at_deadline_and_resets_on_health() {
  PipecatNetWatchdog w;
  const uint32_t d = 600000;
  for (uint32_t t = 0; t < d - 1000; t += 1000)
    assert(!w.update(false, 1000, d));
  assert(w.update(false, 1000, d));  // exactly 10 min unhealthy

  PipecatNetWatchdog w2;
  for (int i = 0; i < 599; i++)
    assert(!w2.update(false, 1000, d));
  assert(!w2.update(true, 1000, d));  // one healthy tick clears it
  for (int i = 0; i < 599; i++)
    assert(!w2.update(false, 1000, d));
  assert(w2.update(false, 1000, d));
}

static void test_wifi_backoff_never_gives_up_and_caps_at_30s() {
  const uint32_t expect[] = {1000, 2000, 4000, 8000, 16000, 30000, 30000};
  for (uint32_t i = 0; i < 7; i++)
    assert(pipecat_wifi_backoff_ms(i) == expect[i]);
  assert(pipecat_wifi_backoff_ms(1000000) == 30000);
  assert(pipecat_wifi_backoff_ms(UINT32_MAX) == 30000);
}

static void
test_total_boots_counts_past_fault_cap_while_fault_boots_saturates() {
  // t_f7fd9a8f: total_boots shared the 16 cap, so boots_since_poweron froze.
  PipecatBootGuardState s = garbage();
  pipecat_boot_guard_on_boot(&s, PipecatResetKind::kPowerOn);
  for (int i = 0; i < 40; i++)
    pipecat_boot_guard_on_boot(&s, PipecatResetKind::kFault);
  assert(s.total_boots == 41);
  assert(s.total_boots > kPipecatBootGuardCountCap);
  assert(s.fault_boots == kPipecatBootGuardCountCap);
  assert(pipecat_boot_guard_hold_ms(&s) == 900000);
  for (int i = 0; i < 20; i++)
    pipecat_boot_guard_on_boot(&s, PipecatResetKind::kSoftware);
  assert(s.total_boots == 61);
  // Saturates at UINT32_MAX instead of wrapping to 0.
  s.total_boots = UINT32_MAX - 1;
  pipecat_boot_guard_on_boot(&s, PipecatResetKind::kSoftware);
  assert(s.total_boots == UINT32_MAX);
  pipecat_boot_guard_on_boot(&s, PipecatResetKind::kSoftware);
  assert(s.total_boots == UINT32_MAX);
  // A cold power-on still restarts the count.
  pipecat_boot_guard_on_boot(&s, PipecatResetKind::kPowerOn);
  assert(s.total_boots == 1);
}

int main() {
  test_cold_power_on_never_holds_and_uses_base_deadline();
  test_garbage_rtc_after_non_poweron_reset_is_reinitialised();
  test_fault_reboots_are_never_faster_than_one_per_minute();
  test_healthy_resets_the_ladder();
  test_plain_software_restart_is_not_a_fault();
  test_network_watchdog_restart_backs_off_across_reboots();
  test_netwdt_flag_is_consumed_once();
  test_net_watchdog_fires_at_deadline_and_resets_on_health();
  test_wifi_backoff_never_gives_up_and_caps_at_30s();
  test_total_boots_counts_past_fault_cap_while_fault_boots_saturates();
  puts("boot guard host tests: PASS");
  return 0;
}
