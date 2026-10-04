#pragma once

#include <stdint.h>

// Re-offer WITHOUT esp_restart() (GADGET-2, t_db77e56b). Pure policy, no
// FreeRTOS/IDF, host-tested by tests/host/test_webrtc_redial.cpp.
//
// Today a hub restart costs the satellite a reboot: the reconnect watchdog
// esp_restart()s ~30-40 s after the server heartbeat goes stale, then the
// board re-boots XVF, Wi-Fi, mdns and the OTA server (~60 s of deafness per
// hub deploy; Z1-fix 10-03). With PIPECAT_REDIAL=1 the firmware instead tears
// down and recreates the libpeer PeerConnection and POSTs a fresh offer,
// keeping XVF, I2S, LED, mdns and OTA alive (Muse `drop_connection` + backoff
// shape; the vault comparison note, shortlist #2).
//
// esp_restart() stays as the fallback: after kMaxConsecutiveFailures failed
// re-dials in a row the firmware reboots exactly as it does today (software
// reset, boot_guard kSoftware; boot_guard semantics untouched). The 10-min
// network watchdog in main.cpp is unchanged and remains the outer backstop.

enum class PipecatRedialTrigger : uint8_t {
  kNone = 0,
  kPeerClosed,      // libpeer state FAILED/CLOSED/DISCONNECTED (incl. DTLS
                    // close_notify from a graceful hub shutdown)
  kSctpClosed,      // SCTP ABORT / SHUTDOWN from the hub (pc.close())
  kWatchdog,        // PipecatReconnectWatchdog deadline (stale heartbeat)
  kHttp,            // POST /webrtc/redial (operator / soak harness kick)
  kOfferFailed,     // POST /api/offer got no answer (hub down / booting)
  kConnectTimeout,  // answer received but never reached CONNECTED
};

inline const char *pipecat_redial_trigger_name(PipecatRedialTrigger t) {
  switch (t) {
    case PipecatRedialTrigger::kNone:
      return "none";
    case PipecatRedialTrigger::kPeerClosed:
      return "peer_closed";
    case PipecatRedialTrigger::kSctpClosed:
      return "sctp_closed";
    case PipecatRedialTrigger::kWatchdog:
      return "watchdog";
    case PipecatRedialTrigger::kHttp:
      return "http";
    case PipecatRedialTrigger::kOfferFailed:
      return "offer_failed";
    case PipecatRedialTrigger::kConnectTimeout:
      return "connect_timeout";
  }
  return "unknown";
}

class PipecatRedialPolicy {
 public:
  // Retry ladder after a FAILED attempt (consecutive failures f, 1-based):
  //   f 1..15  -> 2 s   (first 30 s: a hub restart's python boot window)
  //   f 16..21 -> 5 s   (to 60 s)
  //   f 22..27 -> 10 s  (to 2 min)
  //   f 28..   -> 30 s
  // A fresh trigger (peer closed, watchdog, http) dials immediately.
  static constexpr uint32_t kFastRetryMs = 2000;
  static constexpr uint32_t kFastRetryCount = 15;
  static constexpr uint32_t kMidRetryMs = 5000;
  static constexpr uint32_t kMidRetryCount = 21;
  static constexpr uint32_t kSlowRetryMs = 10000;
  static constexpr uint32_t kSlowRetryCount = 27;
  static constexpr uint32_t kMaxRetryMs = 30000;
  // Answer received but no CONNECTED within this: count a failure, re-dial.
  static constexpr uint32_t kConnectTimeoutMs = 20000;
  // Consecutive failures that clear only after this long CONNECTED
  // (Adolanium's 30 s stable-session reset) so a connect/drop flap still
  // walks toward the reboot fallback instead of spinning forever.
  static constexpr uint32_t kStableSessionMs = 30000;
  // Reboot fallback: 15x2 + 6x5 + 6x10 = 120 s, then 13x30 s -> ~8.5 min of
  // failed re-dials, just inside the 10-min network watchdog base deadline.
  static constexpr uint32_t kMaxConsecutiveFailures = 40;

  static uint32_t backoff_ms(uint32_t consecutive_failures) {
    if (consecutive_failures == 0)
      return 0;
    if (consecutive_failures <= kFastRetryCount)
      return kFastRetryMs;
    if (consecutive_failures <= kMidRetryCount)
      return kMidRetryMs;
    if (consecutive_failures <= kSlowRetryCount)
      return kSlowRetryMs;
    return kMaxRetryMs;
  }

  // Record a failed attempt. Returns true when the caller must fall back to
  // esp_restart() (same reset the reconnect watchdog issues today).
  bool note_failure() {
    if (consecutive_failures_ < UINT32_MAX)
      consecutive_failures_++;
    total_failures_++;
    return consecutive_failures_ >= kMaxConsecutiveFailures;
  }

  // Called every tick while CONNECTED with the time since CONNECTED.
  void note_connected_for(uint32_t connected_ms) {
    if (connected_ms >= kStableSessionMs)
      consecutive_failures_ = 0;
  }

  uint32_t consecutive_failures() const {
    return consecutive_failures_;
  }
  uint32_t total_failures() const {
    return total_failures_;
  }
  uint32_t next_backoff_ms() const {
    return backoff_ms(consecutive_failures_);
  }

 private:
  uint32_t consecutive_failures_ = 0;
  uint32_t total_failures_ = 0;
};
