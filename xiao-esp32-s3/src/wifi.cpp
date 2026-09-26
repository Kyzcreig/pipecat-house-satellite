#include <assert.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boot_guard.h"
#include "main.h"

static volatile bool g_wifi_connected = false;

// Reconnect FOREVER with backoff (t_2e80e072). The stock handler gave up after
// 5 retries (~4 s): a satellite that booted while the AP was still
// re-provisioning after a power blip never associated again and sat in the
// blocking wait below until someone power-cycled it (2026-08-20, 2026-09-25).
static esp_timer_handle_t s_reconnect_timer = nullptr;
static uint32_t s_reconnect_attempt = 0;

// Associated-but-no-IP stall (t_2e80e072, reproduced on kitchen 2026-09-25
// 21:00: L2 association succeeded right after the AP released the client,
// DHCP never completed, satellite stayed dark with no further events). If no
// IP arrives within this window after association, drop the association so
// the DISCONNECTED handler's backoff loop re-joins from scratch.
static constexpr uint32_t WIFI_GOT_IP_TIMEOUT_MS = 60000;
static esp_timer_handle_t s_ip_timeout_timer = nullptr;

static void pipecat_wifi_ip_timeout_cb(void *arg) {
  if (g_wifi_connected)
    return;
  ESP_LOGW(LOG_TAG, "WiFi associated but no IP after %us; re-associating",
           (unsigned)(WIFI_GOT_IP_TIMEOUT_MS / 1000));
  esp_err_t err = esp_wifi_disconnect();  // -> STA_DISCONNECTED -> backoff
  if (err != ESP_OK) {
    ESP_LOGW(LOG_TAG, "esp_wifi_disconnect failed: %s", esp_err_to_name(err));
    esp_timer_start_once(s_reconnect_timer, 1000ULL * 1000ULL);
  }
}

static void pipecat_wifi_reconnect_cb(void *arg) {
  esp_err_t err = esp_wifi_connect();
  if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
    // No attempt started, so no DISCONNECTED event will re-arm us: re-arm here
    // or the loop would silently end. (ESP_ERR_WIFI_CONN = an attempt is
    // already in flight; its DISCONNECTED/GOT_IP event drives the loop.)
    const uint32_t delay_ms = pipecat_wifi_backoff_ms(s_reconnect_attempt);
    s_reconnect_attempt++;
    ESP_LOGW(LOG_TAG, "esp_wifi_connect failed: %s; retry in %u ms",
             esp_err_to_name(err), (unsigned)delay_ms);
    esp_timer_start_once(s_reconnect_timer, (uint64_t)delay_ms * 1000ULL);
  }
}

static void pipecat_event_handler(void *arg, esp_event_base_t event_base,
                                  int32_t event_id, void *event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
    esp_timer_stop(s_ip_timeout_timer);  // ESP_ERR_INVALID_STATE if idle: fine
    esp_timer_start_once(s_ip_timeout_timer,
                         (uint64_t)WIFI_GOT_IP_TIMEOUT_MS * 1000ULL);
  } else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {
    g_wifi_connected = false;
    esp_timer_stop(s_ip_timeout_timer);
    const wifi_event_sta_disconnected_t *ev =
        (const wifi_event_sta_disconnected_t *)event_data;
    const uint32_t delay_ms = pipecat_wifi_backoff_ms(s_reconnect_attempt);
    s_reconnect_attempt++;
    ESP_LOGW(LOG_TAG,
             "WiFi disconnected (reason=%d); reconnect attempt %u in %u ms",
             ev ? (int)ev->reason : -1, (unsigned)s_reconnect_attempt,
             (unsigned)delay_ms);
    esp_timer_stop(s_reconnect_timer);  // ESP_ERR_INVALID_STATE if idle: fine
    esp_timer_start_once(s_reconnect_timer, (uint64_t)delay_ms * 1000ULL);
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(LOG_TAG, "got ip:" IPSTR " after %u reconnect attempt(s)",
             IP2STR(&event->ip_info.ip), (unsigned)s_reconnect_attempt);
    esp_timer_stop(s_ip_timeout_timer);
    s_reconnect_attempt = 0;
    g_wifi_connected = true;
  }
}

void pipecat_init_wifi() {
  const esp_timer_create_args_t reconnect_args = {
      .callback = &pipecat_wifi_reconnect_cb,
      .arg = nullptr,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "wifi_reconnect",
      .skip_unhandled_events = true,
  };
  ESP_ERROR_CHECK(esp_timer_create(&reconnect_args, &s_reconnect_timer));
  const esp_timer_create_args_t ip_timeout_args = {
      .callback = &pipecat_wifi_ip_timeout_cb,
      .arg = nullptr,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "wifi_ip_timeout",
      .skip_unhandled_events = true,
  };
  ESP_ERROR_CHECK(esp_timer_create(&ip_timeout_args, &s_ip_timeout_timer));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                             &pipecat_event_handler, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                             &pipecat_event_handler, NULL));

  ESP_ERROR_CHECK(esp_netif_init());
  esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
  assert(sta_netif);
  ESP_ERROR_CHECK(esp_netif_set_hostname(sta_netif, PIPECAT_MDNS_HOSTNAME));

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(LOG_TAG, "Connecting to WiFi SSID: %s as %s", WIFI_SSID,
           PIPECAT_MDNS_HOSTNAME);
  wifi_config_t wifi_config;
  memset(&wifi_config, 0, sizeof(wifi_config));
  strncpy((char *)wifi_config.sta.ssid, (char *)WIFI_SSID,
          sizeof(wifi_config.sta.ssid));
  strncpy((char *)wifi_config.sta.password, (char *)WIFI_PASSWORD,
          sizeof(wifi_config.sta.password));

  ESP_ERROR_CHECK(esp_wifi_set_config(
      static_cast<wifi_interface_t>(ESP_IF_WIFI_STA), &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_connect());

  // Block until we get an IP address. Reconnects never give up; if the
  // network stays unusable the network watchdog (main.cpp, armed before this
  // call) restarts us. Feed the task WDT: waiting here is liveness, not a hang.
  while (!g_wifi_connected) {
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

bool pipecat_wifi_connected() {
  return g_wifi_connected;
}
