#include "main.h"

#include <esp_event.h>
#include <esp_log.h>
#include <peer.h>

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

  // Initial-connection watchdog: if the peer never reaches CONNECTED within
  // ~30s of boot (e.g. the SmallWebRTC server was down/restarting so our offer
  // got no answer), restart to re-offer instead of sitting idle forever.
  // Once connected, the DISCONNECTED handler owns re-connection via esp_restart.
  const uint32_t connect_deadline_ticks = 30000 / TICK_INTERVAL;
  uint32_t ticks_since_boot = 0;

  while (1) {
    pipecat_webrtc_loop();
    pipecat_validate_ota_if_healthy();
    if (!pipecat_webrtc_connected) {
      ticks_since_boot++;
      if (ticks_since_boot >= connect_deadline_ticks) {
        ESP_LOGW(LOG_TAG,
                 "WebRTC not connected %us after boot; restarting to re-offer",
                 (unsigned)(30000 / 1000));
        esp_restart();
      }
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
