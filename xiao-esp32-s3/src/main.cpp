#include "main.h"

#include <esp_event.h>
#include <esp_log.h>
#include <peer.h>

#include "reconnect_watchdog.h"

#ifndef LINUX_BUILD
#include "nvs_flash.h"

extern "C" void app_main(void) {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  ESP_ERROR_CHECK(esp_event_loop_create_default());
  peer_init();
  pipecat_init_audio_capture();
  pipecat_init_audio_decoder();
  pipecat_init_wifi();
  pipecat_init_mdns();
  pipecat_init_ota_server();
  pipecat_start_ota_validation_watchdog();
  pipecat_init_webrtc();
  pipecat_validate_ota_if_healthy();

  // Persistent reconnect watchdog. A peer-state callback is not guaranteed for
  // a half-open SCTP/ICE path, so require both a connected peer and recent
  // server heartbeat traffic. Restarting is the firmware's safe re-offer path;
  // the 30s unhealthy window prevents a reboot loop during brief jitter.
  PipecatReconnectWatchdog reconnect_watchdog;

  while (1) {
    pipecat_webrtc_loop();
    pipecat_validate_ota_if_healthy();
    const bool heartbeat_fresh = pipecat_webrtc_server_heartbeat_fresh();
    if (reconnect_watchdog.update(pipecat_webrtc_connected, heartbeat_fresh,
                                  TICK_INTERVAL)) {
      ESP_LOGW(LOG_TAG,
               "WebRTC unhealthy for %us (peer_connected=%d "
               "server_heartbeat_fresh=%d); restarting to re-offer",
               (unsigned)(PipecatReconnectWatchdog::kReconnectAfterMs / 1000),
               pipecat_webrtc_connected, heartbeat_fresh);
      esp_restart();
    }
    vTaskDelay(pdMS_TO_TICKS(TICK_INTERVAL));
  }
}
#else
int main(void) {
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  peer_init();
  pipecat_webrtc();

  while (1) {
    pipecat_webrtc_loop();
    vTaskDelay(pdMS_TO_TICKS(TICK_INTERVAL));
  }
}
#endif
