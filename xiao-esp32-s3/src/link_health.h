// Link health telemetry (GADGET-1, t_1910d632): RSSI + heap snapshot.
// Read-only; consumed by /playback/stats, /ota/status and a periodic serial
// line. See link_health.cpp.
#pragma once

#include <stddef.h>
#include <stdint.h>

// Serial LINK_HEALTH line cadence (Muse logs its link.heartbeat every 5 s).
#define LINK_HEALTH_PERIOD_MS 5000

struct PipecatLinkHealth {
  int rssi_dbm;             // valid only when rssi_known
  bool rssi_known;          // false while disassociated
  uint32_t heap_free;       // MALLOC_CAP_INTERNAL free bytes
  uint32_t heap_min_free;   // MALLOC_CAP_INTERNAL low-water mark since boot
  uint32_t heap_largest_dma;  // largest free MALLOC_CAP_DMA block
  uint32_t psram_free;      // MALLOC_CAP_SPIRAM free bytes
};

void pipecat_link_health_snapshot(PipecatLinkHealth *out);
// Writes the JSON fragment `"rssi_dbm":-54,"heap_free":N,...` (no braces,
// no trailing comma) into out; returns snprintf's count.
int pipecat_link_health_json(char *out, size_t capacity);
// Starts the LINK_HEALTH_PERIOD_MS esp_timer that logs the serial line.
void pipecat_link_health_start();
