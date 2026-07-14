#pragma once

#include <stdint.h>

class PipecatReconnectWatchdog {
 public:
  static constexpr uint32_t kReconnectAfterMs = 30000;

  bool update(bool peer_connected, bool server_heartbeat_fresh,
              uint32_t elapsed_ms) {
    if (peer_connected && server_heartbeat_fresh) {
      unhealthy_ms_ = 0;
      return false;
    }

    const uint32_t remaining_ms = kReconnectAfterMs - unhealthy_ms_;
    if (elapsed_ms >= remaining_ms) {
      unhealthy_ms_ = kReconnectAfterMs;
      return true;
    }
    unhealthy_ms_ += elapsed_ms;
    return false;
  }

 private:
  uint32_t unhealthy_ms_ = 0;
};
