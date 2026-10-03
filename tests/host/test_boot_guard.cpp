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

// ---- t_cf433ede: slot fallback after N crash boots on a VALID image ---------

static bool flip_ok(const PipecatBootGuardState &s) {
  return pipecat_boot_guard_should_flip_slot(&s, /*other_slot_present=*/true,
                                             PipecatOtaSlotState::kValid,
                                             /*other_is_same_image=*/false);
}

static void test_three_fault_boots_request_a_slot_flip() {
  PipecatBootGuardState s = garbage();
  pipecat_boot_guard_on_boot(&s, PipecatResetKind::kPowerOn);
  assert(s.crash_boots == 0 && s.slot_flipped == 0);
  pipecat_boot_guard_on_boot(&s, PipecatResetKind::kFault);
  assert(s.crash_boots == 1 && !flip_ok(s));
  pipecat_boot_guard_on_boot(&s, PipecatResetKind::kFault);
  assert(s.crash_boots == 2 && !flip_ok(s));
  pipecat_boot_guard_on_boot(&s, PipecatResetKind::kFault);
  assert(s.crash_boots == kPipecatSlotFlipCrashBoots);
  assert(flip_ok(s));
  // The other slot must hold a different, bootable app.
  assert(pipecat_boot_guard_should_flip_slot(
      &s, true, PipecatOtaSlotState::kUndefined, false));
  const PipecatOtaSlotState bad[] = {
      PipecatOtaSlotState::kNew, PipecatOtaSlotState::kPendingVerify,
      PipecatOtaSlotState::kInvalid, PipecatOtaSlotState::kUnknown};
  for (PipecatOtaSlotState b : bad)
    assert(!pipecat_boot_guard_should_flip_slot(&s, true, b, false));
  assert(!pipecat_boot_guard_should_flip_slot(
      &s, false, PipecatOtaSlotState::kValid, false));  // no other slot
  assert(!pipecat_boot_guard_should_flip_slot(
      &s, true, PipecatOtaSlotState::kValid, true));  // same image twice
}

static void test_healthy_tick_resets_the_crash_streak() {
  PipecatBootGuardState s = garbage();
  pipecat_boot_guard_on_boot(&s, PipecatResetKind::kPowerOn);
  pipecat_boot_guard_on_boot(&s, PipecatResetKind::kFault);
  pipecat_boot_guard_on_boot(&s, PipecatResetKind::kFault);
  pipecat_boot_guard_on_healthy(&s);  // ran fine for a while
  assert(s.crash_boots == 0);
  pipecat_boot_guard_on_boot(&s, PipecatResetKind::kFault);
  pipecat_boot_guard_on_boot(&s, PipecatResetKind::kFault);
  assert(s.crash_boots == 2 && !flip_ok(s));
  pipecat_boot_guard_on_boot(&s, PipecatResetKind::kFault);
  assert(flip_ok(s));
}

static void test_non_crash_boots_do_not_accumulate_toward_a_flip() {
  // A plain esp_restart (OTA reboot, reconnect re-offer) ends the streak:
  // the previous image ran to a deliberate restart, it did not crash.
  PipecatBootGuardState s = garbage();
  pipecat_boot_guard_on_boot(&s, PipecatResetKind::kPowerOn);
  pipecat_boot_guard_on_boot(&s, PipecatResetKind::kFault);
  pipecat_boot_guard_on_boot(&s, PipecatResetKind::kFault);
  pipecat_boot_guard_on_boot(&s, PipecatResetKind::kSoftware);
  assert(s.crash_boots == 0);
  pipecat_boot_guard_on_boot(&s, PipecatResetKind::kFault);
  assert(s.crash_boots == 1 && !flip_ok(s));
  // Network-watchdog restarts are unhealthy-not-crashing: they neither count
  // nor clear (an unreachable hub is not an image defect to flip away from).
  pipecat_boot_guard_on_boot(&s, PipecatResetKind::kFault);
  pipecat_boot_guard_note_netwdt_restart(&s);
  assert(pipecat_boot_guard_on_boot(&s, PipecatResetKind::kSoftware));
  assert(s.crash_boots == 2 && !flip_ok(s));
  for (int i = 0; i < 5; i++) {
    pipecat_boot_guard_note_netwdt_restart(&s);
    pipecat_boot_guard_on_boot(&s, PipecatResetKind::kSoftware);
  }
  assert(s.crash_boots == 2 && !flip_ok(s));
  // Existing fault ladder is untouched by the new fields.
  assert(pipecat_boot_guard_hold_ms(&s) == 900000);
}

static void test_slot_flip_happens_once_per_power_cycle() {
  PipecatBootGuardState s = garbage();
  pipecat_boot_guard_on_boot(&s, PipecatResetKind::kPowerOn);
  for (int i = 0; i < 3; i++)
    pipecat_boot_guard_on_boot(&s, PipecatResetKind::kFault);
  assert(flip_ok(s));
  pipecat_boot_guard_note_slot_flip(&s);
  assert(s.slot_flipped == 1 && s.crash_boots == 0);
  // The flip restart itself is a software reset: the fallback image boots
  // with a clean ladder (no hold) and is not itself a fault boot.
  assert(!pipecat_boot_guard_on_boot(&s, PipecatResetKind::kSoftware));
  assert(pipecat_boot_guard_hold_ms(&s) == 0);
  // Fallback image also crashes 3x (or more): back off, never flip back.
  for (int i = 0; i < 10; i++) {
    pipecat_boot_guard_on_boot(&s, PipecatResetKind::kFault);
    assert(!flip_ok(s));
  }
  assert(s.crash_boots >= kPipecatSlotFlipCrashBoots);
  assert(pipecat_boot_guard_hold_ms(&s) == 900000);
  // Health does not re-arm the flip within the same power cycle either.
  pipecat_boot_guard_on_healthy(&s);
  for (int i = 0; i < 3; i++)
    pipecat_boot_guard_on_boot(&s, PipecatResetKind::kFault);
  assert(!flip_ok(s));
  // A cold power-on re-arms it (counters restart, RTC memory is garbage).
  pipecat_boot_guard_on_boot(&s, PipecatResetKind::kPowerOn);
  assert(s.slot_flipped == 0 && s.crash_boots == 0 && !flip_ok(s));
  assert(pipecat_boot_guard_hold_ms(&s) == 0);
}

static void test_cold_power_on_and_garbage_rtc_never_flip() {
  PipecatBootGuardState s = garbage();  // 0xA5A5A5A5 crash_boots >= 3
  pipecat_boot_guard_on_boot(&s, PipecatResetKind::kPowerOn);
  assert(!flip_ok(s) && s.crash_boots == 0 && s.slot_flipped == 0);
  // Old-layout magic ("PBGT") from an image flashed before these fields
  // existed must re-initialise, not read garbage into crash_boots.
  PipecatBootGuardState old = garbage();
  old.magic = 0x50424754u;
  assert(pipecat_boot_guard_on_boot(&old, PipecatResetKind::kFault));
  assert(old.crash_boots == 1 && old.slot_flipped == 0 && !flip_ok(old));
  // Garbage RTC on a fault boot (first-ever brownout) counts as one.
  PipecatBootGuardState g = garbage();
  pipecat_boot_guard_on_boot(&g, PipecatResetKind::kFault);
  assert(g.crash_boots == 1 && !flip_ok(g));
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
  test_three_fault_boots_request_a_slot_flip();
  test_healthy_tick_resets_the_crash_streak();
  test_non_crash_boots_do_not_accumulate_toward_a_flip();
  test_slot_flip_happens_once_per_power_cycle();
  test_cold_power_on_and_garbage_rtc_never_flip();
  puts("boot guard host tests: PASS");
  return 0;
}
