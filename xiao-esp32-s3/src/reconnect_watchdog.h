#pragma once

#include <stdint.h>

class PipecatReconnectWatchdog {
 public:
  static constexpr uint32_t kReconnectAfterMs = 30000;

  bool update(bool peer_connected, bool server_heartbeat_fresh,
              uint32_t elapsed_ms) {
    if (peer_connected && server_heartbeat_fresh) {
      healthy_connection_seen_ = true;
      unhealthy_ms_ = 0;
      return false;
    }

    // Heartbeat freshness already includes the server's full ping interval
    // plus jitter grace. Once a connection has proved healthy, adding another
    // 30s here would make eviction-to-reoffer exceed the 60s acceptance bound.
    if (healthy_connection_seen_ && peer_connected &&
        !server_heartbeat_fresh) {
      return true;
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
  bool healthy_connection_seen_ = false;
  uint32_t unhealthy_ms_ = 0;
};
