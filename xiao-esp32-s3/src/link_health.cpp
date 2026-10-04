// Link health telemetry (GADGET-1, t_1910d632): Wi-Fi RSSI + heap on the
// surfaces the hub already polls (/playback/stats, /ota/status) and one
// serial line every LINK_HEALTH_PERIOD_MS. Lift of the Muse 5 s
// `link.heartbeat` line (app.c:2789-2800). Read-only: no audio, network or
// boot behaviour changes. Purpose: join gap_resumes deltas to rssi_dbm /
// heap_largest_dma by timestamp on the U1 soak, so a residual drain can be
// told apart as "signal dip" vs "DMA-heap trough" vs "neither".
#include "link_health.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <stdio.h>

#include "main.h"

static esp_timer_handle_t s_link_health_timer = nullptr;

void pipecat_link_health_snapshot(PipecatLinkHealth *out) {
  // esp_wifi_sta_get_ap_info fails (ESP_ERR_WIFI_NOT_CONNECT) while
  // disassociated; report "unknown" instead of a stale value.
  wifi_ap_record_t ap = {};
  out->rssi_known =
      pipecat_wifi_connected() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
  out->rssi_dbm = out->rssi_known ? ap.rssi : 0;
  out->heap_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  out->heap_min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
  // Largest contiguous DMA-capable block: the I2S/Wi-Fi driver and SRTP
  // working set come from here; a trough is what a "DMA-heap pressure"
  // drain would show.
  out->heap_largest_dma = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
  out->psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
}

int pipecat_link_health_json(char *out, size_t capacity) {
  PipecatLinkHealth h;
  pipecat_link_health_snapshot(&h);
  char rssi[8];
  if (h.rssi_known)
    snprintf(rssi, sizeof(rssi), "%d", h.rssi_dbm);
  else
    snprintf(rssi, sizeof(rssi), "null");
  return snprintf(out, capacity,
                  "\"rssi_dbm\":%s,\"heap_free\":%lu,\"heap_min_free\":%lu,"
                  "\"heap_largest_dma\":%lu,\"psram_free\":%lu",
                  rssi, (unsigned long)h.heap_free,
                  (unsigned long)h.heap_min_free,
                  (unsigned long)h.heap_largest_dma,
                  (unsigned long)h.psram_free);
}

static void link_health_tick(void *arg) {
  PipecatLinkHealth h;
  pipecat_link_health_snapshot(&h);
  // One grep-able line; the hub-side soak join reads the HTTP fields, this
  // is for the serial-attached bench and post-mortems.
  ESP_LOGI(LOG_TAG,
           "LINK_HEALTH rssi_dbm=%d rssi_known=%d heap_free=%lu "
           "heap_min_free=%lu heap_largest_dma=%lu psram_free=%lu "
           "uptime_s=%lld",
           h.rssi_dbm, h.rssi_known, (unsigned long)h.heap_free,
           (unsigned long)h.heap_min_free, (unsigned long)h.heap_largest_dma,
           (unsigned long)h.psram_free,
           (long long)(esp_timer_get_time() / 1000000LL));
}

void pipecat_link_health_start() {
  if (s_link_health_timer != nullptr)
    return;
  const esp_timer_create_args_t args = {
      .callback = &link_health_tick,
      .arg = nullptr,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "link_health",
      .skip_unhandled_events = true,
  };
  ESP_ERROR_CHECK(esp_timer_create(&args, &s_link_health_timer));
  ESP_ERROR_CHECK(esp_timer_start_periodic(s_link_health_timer,
                                           LINK_HEALTH_PERIOD_MS * 1000ULL));
}
