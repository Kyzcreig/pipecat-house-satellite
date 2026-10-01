#pragma once

// Unattended-recovery policy (t_2e80e072, power-outage recovery).
//
// Pure logic only (no ESP-IDF includes) so tests/host can exercise it. The
// device glue lives in main.cpp: the state struct is RTC_NOINIT (survives a
// software/watchdog/brownout reset, garbage after a cold power-on), the
// network watchdog is an esp_timer armed BEFORE any blocking init.
//
// Policy:
//   * Network watchdog: not healthy (Wi-Fi up AND WebRTC peer connected AND
//     server heartbeat fresh AND XVF3800 present) for the deadline =>
//     esp_restart(). Deadline 10 min, doubling per consecutive watchdog
//     restart, capped at 60 min. Reset on the first healthy tick.
//   * Boot-loop safety: a boot that follows a FAULT reset (task/int WDT,
//     panic, brownout, or our own network-watchdog restart) holds for
//     60 s * 2^(n-1) (n = consecutive fault boots), capped at 15 min, before
//     init — so fault reboots are never faster than 1/min. A cold power-on
//     never holds (that is the outage-recovery path and must be fast).
//     Plain esp_restart() re-offers (30 s reconnect watchdog, OTA) are not
//     faults and keep their existing cadence.

#include <stdint.h>

static constexpr uint32_t kPipecatBootGuardMagic = 0x50424754u;  // "PBGT"
static constexpr uint32_t kPipecatNetWatchdogBaseMs = 10u * 60u * 1000u;
static constexpr uint32_t kPipecatNetWatchdogCapMs = 60u * 60u * 1000u;
static constexpr uint32_t kPipecatFaultHoldBaseMs = 60u * 1000u;
static constexpr uint32_t kPipecatFaultHoldCapMs = 15u * 60u * 1000u;
static constexpr uint32_t kPipecatBootGuardCountCap = 16u;
// total_boots is a diagnostic counter (/xvf/params boots_since_poweron), not
// a backoff input: it must keep counting past the fault-ladder cap so reboot
// deltas stay visible (t_f7fd9a8f). Saturate only at the type's max.
static constexpr uint32_t kPipecatTotalBootsCap = UINT32_MAX;

enum class PipecatResetKind : uint8_t {
  kPowerOn,   // cold boot: RTC memory is garbage, counters restart
  kFault,     // WDT / panic / brownout
  kSoftware,  // esp_restart(); becomes kNetWatchdog if we flagged it first
  kOther,     // external pin, deep sleep, unknown
};

struct PipecatBootGuardState {
  uint32_t magic;
  uint32_t fault_boots;      // consecutive boots that followed a fault reset
  uint32_t netwdt_restarts;  // consecutive network-watchdog restarts
  uint32_t netwdt_pending;   // 1 = the next ESP_RST_SW was our watchdog
  uint32_t total_boots;      // since the last cold power-on (diagnostic)
};

static inline uint32_t pipecat_boot_guard_sat_inc(uint32_t v) {
  return v < kPipecatBootGuardCountCap ? v + 1 : v;
}

static inline uint32_t pipecat_boot_guard_total_inc(uint32_t v) {
  return v < kPipecatTotalBootsCap ? v + 1 : v;
}

// Call exactly once per boot, before any init. Returns true if this boot
// followed a fault (for logging).
static inline bool pipecat_boot_guard_on_boot(PipecatBootGuardState *s,
                                              PipecatResetKind kind) {
  if (s->magic != kPipecatBootGuardMagic ||
      kind == PipecatResetKind::kPowerOn) {
    s->magic = kPipecatBootGuardMagic;
    s->fault_boots = 0;
    s->netwdt_restarts = 0;
    s->netwdt_pending = 0;
    s->total_boots = 0;
  }
  s->total_boots = pipecat_boot_guard_total_inc(s->total_boots);
  const bool netwdt = kind == PipecatResetKind::kSoftware && s->netwdt_pending;
  s->netwdt_pending = 0;
  if (kind == PipecatResetKind::kFault || netwdt) {
    s->fault_boots = pipecat_boot_guard_sat_inc(s->fault_boots);
    return true;
  }
  return false;
}

// How long to hold before init on this boot.
static inline uint32_t pipecat_boot_guard_hold_ms(
    const PipecatBootGuardState *s) {
  if (s->fault_boots == 0)
    return 0;
  uint32_t shift = s->fault_boots - 1;
  if (shift > 4)
    shift = 4;  // 60 s << 4 = 16 min > cap
  const uint32_t ms = kPipecatFaultHoldBaseMs << shift;
  return ms > kPipecatFaultHoldCapMs ? kPipecatFaultHoldCapMs : ms;
}

static inline uint32_t pipecat_net_watchdog_deadline_ms(
    const PipecatBootGuardState *s) {
  uint32_t shift = s->netwdt_restarts;
  if (shift > 3)
    shift = 3;  // 10 min << 3 = 80 min > cap
  const uint32_t ms = kPipecatNetWatchdogBaseMs << shift;
  return ms > kPipecatNetWatchdogCapMs ? kPipecatNetWatchdogCapMs : ms;
}

// The satellite reached full health: the fault/backoff ladder resets.
static inline void pipecat_boot_guard_on_healthy(PipecatBootGuardState *s) {
  s->fault_boots = 0;
  s->netwdt_restarts = 0;
}

// Record that the next software reset is the network watchdog's.
static inline void pipecat_boot_guard_note_netwdt_restart(
    PipecatBootGuardState *s) {
  s->netwdt_restarts = pipecat_boot_guard_sat_inc(s->netwdt_restarts);
  s->netwdt_pending = 1;
}

// Continuous-unhealthy accumulator. Returns true when the deadline is hit.
class PipecatNetWatchdog {
 public:
  bool update(bool healthy, uint32_t elapsed_ms, uint32_t deadline_ms) {
    if (healthy) {
      unhealthy_ms_ = 0;
      return false;
    }
    if (elapsed_ms >= deadline_ms - unhealthy_ms_) {
      unhealthy_ms_ = deadline_ms;
      return true;
    }
    unhealthy_ms_ += elapsed_ms;
    return false;
  }
  uint32_t unhealthy_ms() const {
    return unhealthy_ms_;
  }

 private:
  uint32_t unhealthy_ms_ = 0;
};

// Wi-Fi (re)association backoff: 1 s doubling to a 30 s cap, forever.
static constexpr uint32_t kPipecatWifiBackoffBaseMs = 1000u;
static constexpr uint32_t kPipecatWifiBackoffCapMs = 30000u;

static inline uint32_t pipecat_wifi_backoff_ms(uint32_t attempt) {
  uint32_t shift = attempt;
  if (shift > 5)
    shift = 5;  // 1 s << 5 = 32 s > cap
  const uint32_t ms = kPipecatWifiBackoffBaseMs << shift;
  return ms > kPipecatWifiBackoffCapMs ? kPipecatWifiBackoffCapMs : ms;
}
