#pragma once

#include <stdint.h>

// Server-liveness oracle behind pipecat_webrtc_server_heartbeat_fresh()
// (t_56a17737, kitchen 2026-10-04).
//
// The hub pings this device over the reliable RTVI data channel every 30 s
// (webrtc_server.py _PEER_PING_INTERVAL_S). Until this change the firmware
// treated a server ping older than WEBRTC_SERVER_HEARTBEAT_STALE_MS (35 s) as
// "server gone" and esp_restart()ed -- ONE ping delayed >5 s on a lossy,
// retransmitting SCTP lane rebooted a device that was mid-conversation.
// Kitchen 2026-10-04: resets at 02:40:05 and 03:04:15, each landing at exactly
// last-ping-slot + 35 s (peer connect 02:21:59.7 -> ping slots :29.7/:59.7;
// peer connect 02:59:10.3 -> slots :10.3/:40.3), while downlink RTP (a TTS
// story) and uplink RTP were both flowing right up to the reset. The hub saw
// the result as 'No audio frame received' x15 -> peer drop.
//
// Rule: a server that is streaming audio to us is alive. Downlink media
// arrival refreshes liveness with the same staleness window as a ping, so
// the detection latency after the LAST sign of life is unchanged (35 s);
// what changes is that mid-playback a late ping can no longer reboot the
// device. Pings alone still govern the idle case (no media), which is where
// the silent-wedge detection (hub evicted us without a state callback) lives.
// The first-connection rule is unchanged too: with no ping ever seen the
// server is not fresh, media or not.
//
// Timestamps are esp_timer ms truncated to uint32 (wraps at ~49.7 days;
// unsigned subtraction keeps the ages correct across the wrap). A real
// timestamp of 0 is bumped to 1 so 0 can keep meaning "never".
struct PipecatServerLiveness {
  explicit PipecatServerLiveness(uint32_t stale_ms) : stale_ms_(stale_ms) {}

  void note_ping(uint32_t now_ms) {
    now_ms = now_ms ? now_ms : 1u;
    if (last_ping_ms != 0) {
      const uint32_t gap = now_ms - last_ping_ms;
      if (gap > ping_gap_max_ms)
        ping_gap_max_ms = gap;
    }
    last_ping_ms = now_ms;
    ping_rx = ping_rx + 1u;  // no ++ on volatile (C++20 -Wvolatile)
  }

  void note_media(uint32_t now_ms) { last_media_ms = now_ms ? now_ms : 1u; }

  // True when the server has shown a sign of life inside the window. Counts a
  // "media hold" once per episode in which the ping is stale but media keeps
  // the connection alive -- that is the kitchen reset class, made observable.
  bool fresh(uint32_t now_ms) {
    now_ms = now_ms ? now_ms : 1u;  // same bump as the notes, so t=0 is sane
    const uint32_t ping = last_ping_ms;
    if (ping == 0)
      return false;
    if ((uint32_t)(now_ms - ping) <= stale_ms_) {
      in_media_hold_ = false;
      return true;
    }
    const uint32_t media = last_media_ms;
    if (media != 0 && (uint32_t)(now_ms - media) <= stale_ms_) {
      if (!in_media_hold_) {
        in_media_hold_ = true;
        media_holds = media_holds + 1u;
      }
      return true;
    }
    in_media_hold_ = false;
    return false;
  }

  uint32_t ping_age_ms(uint32_t now_ms) const {
    now_ms = now_ms ? now_ms : 1u;
    const uint32_t ping = last_ping_ms;
    return ping == 0 ? 0u : (uint32_t)(now_ms - ping);
  }

  // Written on the transport loop task, read from the esp_timer task and the
  // httpd task: plain word-sized stores, no torn reads to worry about.
  volatile uint32_t last_ping_ms = 0;
  volatile uint32_t last_media_ms = 0;
  volatile uint32_t ping_rx = 0;
  volatile uint32_t ping_gap_max_ms = 0;
  volatile uint32_t media_holds = 0;

 private:
  const uint32_t stale_ms_;
  volatile bool in_media_hold_ = false;
};
