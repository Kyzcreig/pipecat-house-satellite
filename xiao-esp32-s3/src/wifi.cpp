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
#include "fleet_identity.h"
#include "main.h"

static volatile bool g_wifi_connected = false;
static uint8_t s_sta_mac[6] = {};
static char s_sta_mac_str[PIPECAT_MAC_STR_LEN] = "00:00:00:00:00:00";
static char s_hostname[64] = "xvf3800";

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

// Re-join instrumentation (t_2a5f2312, GADGET-3): device-side DISCONNECTED ->
// GOT_IP wall time, read back on /ota/status so the UniFi-kick bench does not
// need the serial port (a serial RTS reset drops the live WebRTC peer).
static int64_t s_disconnect_at_us = 0;
static uint32_t s_wifi_disconnects = 0;
static uint32_t s_wifi_fast_rejoins = 0;
static uint32_t s_wifi_fallback_rejoins = 0;
static uint32_t s_wifi_rejoin_last_ms = 0;

#if PIPECAT_WIFI_FAST_REJOIN
// Muse-pattern fast re-join (vault note "Muse Gadget SDK vs Clanker" item 3,
// wifi_mgr.c:67-79, 281-318): on GOT_IP remember the serving BSSID + channel;
// on the next DISCONNECTED reconnect at once, pinned to that BSSID on that
// channel (WIFI_FAST_SCAN, no all-channel sweep, no 1 s backoff), refusing it
// below PIPECAT_WIFI_FAST_REJOIN_RSSI_FLOOR dBm. If that one attempt fails,
// fall back to today's path: all-channel scan sorted by signal, 1 s doubling
// backoff. Compile-time gated (default off) per the never-flashed rule.
static bool s_fast_cache_valid = false;
static uint8_t s_fast_bssid[6] = {};
static uint8_t s_fast_channel = 0;
static bool s_fast_attempt_in_flight = false;

static void pipecat_wifi_apply_sta_config(bool pinned) {
  wifi_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  strncpy((char *)cfg.sta.ssid, (char *)WIFI_SSID, sizeof(cfg.sta.ssid));
  strncpy((char *)cfg.sta.password, (char *)WIFI_PASSWORD,
          sizeof(cfg.sta.password));
  if (pinned) {
    cfg.sta.scan_method = WIFI_FAST_SCAN;
    cfg.sta.bssid_set = 1;
    memcpy(cfg.sta.bssid, s_fast_bssid, sizeof(cfg.sta.bssid));
    cfg.sta.channel = s_fast_channel;
    cfg.sta.threshold.rssi = PIPECAT_WIFI_FAST_REJOIN_RSSI_FLOOR;
  } else {
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
  }
  esp_err_t err =
      esp_wifi_set_config(static_cast<wifi_interface_t>(ESP_IF_WIFI_STA), &cfg);
  if (err != ESP_OK)
    ESP_LOGW(LOG_TAG, "esp_wifi_set_config(pinned=%d) failed: %s", (int)pinned,
             esp_err_to_name(err));
}
#endif

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
    const bool was_up = g_wifi_connected;
    g_wifi_connected = false;
    esp_timer_stop(s_ip_timeout_timer);
    const wifi_event_sta_disconnected_t *ev =
        (const wifi_event_sta_disconnected_t *)event_data;
    if (was_up) {
      // Start the re-join clock only on a real link loss, not on a failed
      // attempt inside the loop, so last_ms spans the whole outage.
      s_disconnect_at_us = esp_timer_get_time();
      s_wifi_disconnects++;
    }
#if PIPECAT_WIFI_FAST_REJOIN
    if (was_up && s_fast_cache_valid) {
      // First attempt after a link loss: pinned, immediate. Not counted as a
      // backoff attempt, so a failed pin falls back at the 1 s base delay.
      s_fast_attempt_in_flight = true;
      ESP_LOGW(LOG_TAG,
               "WiFi disconnected (reason=%d); fast re-join to "
               "%02x:%02x:%02x:%02x:%02x:%02x ch%u now",
               ev ? (int)ev->reason : -1, s_fast_bssid[0], s_fast_bssid[1],
               s_fast_bssid[2], s_fast_bssid[3], s_fast_bssid[4],
               s_fast_bssid[5], (unsigned)s_fast_channel);
      pipecat_wifi_apply_sta_config(true);
      esp_timer_stop(s_reconnect_timer);
      esp_timer_start_once(s_reconnect_timer, 0);
      return;
    }
    if (s_fast_attempt_in_flight) {
      // The pinned attempt did not get us an IP: unpin and take the
      // all-channel by-signal path with the normal backoff below.
      s_fast_attempt_in_flight = false;
      ESP_LOGW(LOG_TAG, "fast re-join failed (reason=%d); all-channel fallback",
               ev ? (int)ev->reason : -1);
      pipecat_wifi_apply_sta_config(false);
    }
#endif
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
    // IP landed: cancel the no-IP deadline first.
    esp_timer_stop(s_ip_timeout_timer);
    if (s_disconnect_at_us > 0) {
      s_wifi_rejoin_last_ms =
          (uint32_t)((esp_timer_get_time() - s_disconnect_at_us) / 1000LL);
      s_disconnect_at_us = 0;
#if PIPECAT_WIFI_FAST_REJOIN
      if (s_fast_attempt_in_flight)
        s_wifi_fast_rejoins++;
      else
        s_wifi_fallback_rejoins++;
#else
      s_wifi_fallback_rejoins++;
#endif
    }
    wifi_ap_record_t ap;
    const bool have_ap = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    ESP_LOGI(LOG_TAG,
             "got ip:" IPSTR
             " after %u reconnect attempt(s); rejoin_ms=%u "
             "bssid=%02x:%02x:%02x:%02x:%02x:%02x ch%u rssi=%d",
             IP2STR(&event->ip_info.ip), (unsigned)s_reconnect_attempt,
             (unsigned)s_wifi_rejoin_last_ms, have_ap ? ap.bssid[0] : 0,
             have_ap ? ap.bssid[1] : 0, have_ap ? ap.bssid[2] : 0,
             have_ap ? ap.bssid[3] : 0, have_ap ? ap.bssid[4] : 0,
             have_ap ? ap.bssid[5] : 0, have_ap ? (unsigned)ap.primary : 0u,
             have_ap ? (int)ap.rssi : 0);
#if PIPECAT_WIFI_FAST_REJOIN
    s_fast_attempt_in_flight = false;
    if (have_ap) {
      memcpy(s_fast_bssid, ap.bssid, sizeof(s_fast_bssid));
      s_fast_channel = ap.primary;
      s_fast_cache_valid = true;
    }
#endif
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

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
  // Fleet identity (t_54916498 P4): the STA MAC is what ARP sees at the hub, and
  // the default hostname derives from it so the image carries no room name.
  ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, s_sta_mac));
  pipecat_mac_format(s_sta_mac, s_sta_mac_str, sizeof(s_sta_mac_str));
  if (PIPECAT_MDNS_HOSTNAME[0] == '\0') {
    pipecat_mac_hostname(s_sta_mac, s_hostname, sizeof(s_hostname));
  } else {
    strlcpy(s_hostname, PIPECAT_MDNS_HOSTNAME, sizeof(s_hostname));
  }
  ESP_ERROR_CHECK(esp_netif_set_hostname(sta_netif, s_hostname));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(LOG_TAG, "Connecting to WiFi SSID: %s as %s (mac %s)", WIFI_SSID,
           s_hostname, s_sta_mac_str);
  wifi_config_t wifi_config;
  memset(&wifi_config, 0, sizeof(wifi_config));
  strncpy((char *)wifi_config.sta.ssid, (char *)WIFI_SSID,
          sizeof(wifi_config.sta.ssid));
  strncpy((char *)wifi_config.sta.password, (char *)WIFI_PASSWORD,
          sizeof(wifi_config.sta.password));
#if PIPECAT_WIFI_FAST_REJOIN
  // Boot join: all-channel scan, strongest AP, so the BSSID we cache for the
  // fast path is the near one (a zeroed config is WIFI_FAST_SCAN = first match,
  // which on a multi-AP SSID can be the far node).
  wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
  wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
#endif

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

const char *pipecat_sta_mac_str() {
  return s_sta_mac_str;
}

const char *pipecat_hostname() {
  return s_hostname;
}

uint32_t pipecat_wifi_disconnects() {
  return s_wifi_disconnects;
}
uint32_t pipecat_wifi_fast_rejoins() {
  return s_wifi_fast_rejoins;
}
uint32_t pipecat_wifi_fallback_rejoins() {
  return s_wifi_fallback_rejoins;
}
uint32_t pipecat_wifi_rejoin_last_ms() {
  return s_wifi_rejoin_last_ms;
}
bool pipecat_wifi_fast_rejoin_enabled() {
  return PIPECAT_WIFI_FAST_REJOIN != 0;
}
