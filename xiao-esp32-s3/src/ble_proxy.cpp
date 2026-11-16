// Passive BLE scanner exposed to Home Assistant as an ESPHome Bluetooth proxy
// (t_6215e36e). Compiled only with PIPECAT_BLE_PROXY=1; at runtime it does
// nothing unless NVS ble_proxy/enabled = 1 (default 0, set over HTTP).
//
//   OFF (default): the BT controller is never initialised and its static
//       RAM is handed back to the heap (esp_bt_controller_mem_release).
//   ON:  BT controller only (no host stack), passive LE scan driven over
//       VHCI (ble_hci.c), reports served on TCP 6053 in the ESPHome native
//       API (esphome_api.c) and announced as _esphomelib._tcp, so Home
//       Assistant adopts the satellite as a passive Bluetooth proxy and
//       Bermuda can use it as a scanner.
//
//   GET  /ble/proxy                         state + counters
//   POST /ble/proxy?enabled=0|1[&interval_ms=N&window_ms=M]
//        persists to NVS; reboots when on/off flips, or the timing changes
//        while on (the controller is only ever started at boot).
#include "main.h"

#if PIPECAT_BLE_PROXY

#include <esp_app_desc.h>
#include <esp_bt.h>
#include <esp_coexist.h>
#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <mdns.h>
#include <nvs.h>
#include <stdio.h>
#include <string.h>

#include "ble_hci.h"
#include "esphome_api.h"

#define BLE_NVS_NAMESPACE "ble_proxy"
#define BLE_NVS_ENABLED "enabled"
#define BLE_NVS_INTERVAL "interval_ms"
#define BLE_NVS_WINDOW "window_ms"

// ESPHome's esp32_ble_tracker default (320 ms interval, 30 ms window): the
// radio listens for BLE 9.4% of the time and Wi-Fi keeps the rest. The
// operating point the six Apollo R PRO-1 scanners already run with Bermuda.
// Tunable over HTTP without a reflash; the acceptance measurement decides.
static constexpr uint16_t kDefaultIntervalMs = 320;
static constexpr uint16_t kDefaultWindowMs = 30;

static constexpr uint32_t kAdvQueueDepth = 64;  // x sizeof(BleAdvReport) 48 B
static constexpr uint32_t kBatchFlushMs = 100;
static constexpr uint32_t kIdleCloseMs = 90 * 1000;  // no rx, not subscribed
static constexpr uint32_t kRebootDelayMs = 1500;

static bool s_enabled = false;  // NVS value read at boot
static uint16_t s_interval_ms = kDefaultIntervalMs;
static uint16_t s_window_ms = kDefaultWindowMs;
static volatile bool s_running = false;  // scan enabled, server listening
static const char *s_error = "";
static size_t s_heap_before = 0, s_heap_after = 0;

static QueueHandle_t s_adv_q = nullptr;
static QueueHandle_t s_cmd_q = nullptr;

// Counters (single writer each; read racily for /ble/proxy).
static volatile uint32_t s_reports_rx = 0, s_reports_sent = 0;
static volatile uint32_t s_drop_queue = 0, s_drop_nosub = 0, s_hci_bad = 0;
static volatile uint32_t s_batches = 0, s_clients_total = 0;
static volatile bool s_client_connected = false, s_client_subscribed = false;

static char s_bt_mac[18] = "";
static char s_friendly[48] = "";
static char s_esphome_version[40] = "";
static EspApiDeviceInfo s_info;

static uint32_t now_ms() {
  return (uint32_t)(esp_timer_get_time() / 1000);
}

// ---- NVS -------------------------------------------------------------------

static void load_nvs() {
  nvs_handle_t h;
  if (nvs_open(BLE_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK)
    return;  // never written: defaults (OFF)
  uint8_t en = 0;
  uint16_t v = 0;
  if (nvs_get_u8(h, BLE_NVS_ENABLED, &en) == ESP_OK)
    s_enabled = en == 1;
  if (nvs_get_u16(h, BLE_NVS_INTERVAL, &v) == ESP_OK)
    s_interval_ms = v;
  if (nvs_get_u16(h, BLE_NVS_WINDOW, &v) == ESP_OK)
    s_window_ms = v;
  nvs_close(h);
}

static esp_err_t save_nvs(bool enabled, uint16_t interval_ms,
                          uint16_t window_ms) {
  nvs_handle_t h;
  esp_err_t ret = nvs_open(BLE_NVS_NAMESPACE, NVS_READWRITE, &h);
  if (ret != ESP_OK)
    return ret;
  ret = nvs_set_u8(h, BLE_NVS_ENABLED, enabled ? 1 : 0);
  if (ret == ESP_OK)
    ret = nvs_set_u16(h, BLE_NVS_INTERVAL, interval_ms);
  if (ret == ESP_OK)
    ret = nvs_set_u16(h, BLE_NVS_WINDOW, window_ms);
  if (ret == ESP_OK)
    ret = nvs_commit(h);
  nvs_close(h);
  return ret;
}

// ---- VHCI ------------------------------------------------------------------

// Runs in the controller's task. Copies out and returns; never blocks.
static BleAdvReport s_rx_reports[4];
static int vhci_host_recv(uint8_t *data, uint16_t len) {
  uint16_t op = 0;
  uint8_t st = 0;
  if (ble_hci_parse_cmd_result(data, len, &op, &st) == 1) {
    const uint32_t v = ((uint32_t)op << 8) | st;
    xQueueSend(s_cmd_q, &v, 0);
    return 0;
  }
  const int n = ble_hci_parse_adv_report(data, len, s_rx_reports, 4);
  if (n < 0) {
    s_hci_bad++;
    return 0;
  }
  for (int i = 0; i < n; ++i) {
    s_reports_rx++;
    if (xQueueSend(s_adv_q, &s_rx_reports[i], 0) != pdTRUE)
      s_drop_queue++;
  }
  return 0;
}

static void vhci_send_available() {}

static const esp_vhci_host_callback_t s_vhci_cb = {
    .notify_host_send_available = vhci_send_available,
    .notify_host_recv = vhci_host_recv,
};

// Send one command and wait for its Command Complete/Status. True on status 0.
static bool hci_cmd(const uint8_t *buf, size_t n, uint16_t opcode) {
  for (int i = 0; i < 100 && !esp_vhci_host_check_send_available(); ++i)
    vTaskDelay(pdMS_TO_TICKS(10));
  if (!esp_vhci_host_check_send_available())
    return false;
  xQueueReset(s_cmd_q);
  esp_vhci_host_send_packet(const_cast<uint8_t *>(buf), (uint16_t)n);
  uint32_t v = 0;
  while (xQueueReceive(s_cmd_q, &v, pdMS_TO_TICKS(1000)) == pdTRUE) {
    if ((uint16_t)(v >> 8) == opcode)
      return (v & 0xFF) == 0;
  }
  return false;
}

static bool start_scan() {
  uint8_t buf[BLE_HCI_CMD_MAX];
  uint16_t iv = 0, wn = 0;
  ble_hci_scan_timing(s_interval_ms, s_window_ms, &iv, &wn);
  if (!hci_cmd(buf, ble_hci_cmd_reset(buf), BLE_HCI_OP_RESET)) {
    s_error = "hci reset";
    return false;
  }
  if (!hci_cmd(buf, ble_hci_cmd_set_event_mask(buf),
               BLE_HCI_OP_SET_EVENT_MASK)) {
    s_error = "hci event mask";
    return false;
  }
  if (!hci_cmd(buf, ble_hci_cmd_le_set_scan_params(buf, 1, iv, wn),
               BLE_HCI_OP_LE_SET_SCAN_PARAMS)) {
    s_error = "hci scan params";
    return false;
  }
  if (!hci_cmd(buf, ble_hci_cmd_le_set_scan_enable(buf, 1),
               BLE_HCI_OP_LE_SET_SCAN_ENABLE)) {
    s_error = "hci scan enable";
    return false;
  }
  ESP_LOGI(LOG_TAG, "ble_proxy: passive scan on, interval %u ms window %u ms",
           (unsigned)s_interval_ms, (unsigned)s_window_ms);
  return true;
}

// ---- API server ------------------------------------------------------------

static int send_all(int fd, const uint8_t *p, size_t n) {
  while (n) {
    const int w = send(fd, p, n, 0);
    if (w <= 0)
      return -1;
    p += w;
    n -= (size_t)w;
  }
  return 0;
}

static void close_client(int *fd) {
  if (*fd >= 0)
    close(*fd);
  *fd = -1;
  s_client_connected = false;
  s_client_subscribed = false;
}

static int open_listener() {
  const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0)
    return -1;
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_port = htons(ESPAPI_PORT);
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(fd, 1) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

// Send the pending batch as one BluetoothLERawAdvertisementsResponse. A failed
// send drops the client. Returns false when the client is gone.
static bool flush_batch(int *cfd, const BleAdvReport *batch, size_t *batch_n) {
  static uint8_t frame[ESPAPI_TX_MAX];
  const size_t n =
      espapi_encode_raw_adverts(batch, *batch_n, frame, sizeof(frame));
  if (n == 0 || send_all(*cfd, frame, n) != 0) {
    close_client(cfd);
    *batch_n = 0;
    return false;
  }
  s_reports_sent += *batch_n;
  s_batches++;
  *batch_n = 0;
  return true;
}

static void ble_proxy_task(void *) {
  if (!start_scan()) {
    ESP_LOGE(LOG_TAG, "ble_proxy: scan start failed (%s); scanner off",
             s_error);
    vTaskDelete(nullptr);
    return;
  }
  const int ls = open_listener();
  if (ls < 0) {
    s_error = "listen 6053";
    ESP_LOGE(LOG_TAG, "ble_proxy: cannot listen on %d", ESPAPI_PORT);
    vTaskDelete(nullptr);
    return;
  }
  s_running = true;

  static EspApiSession sess;
  static uint8_t tx[ESPAPI_TX_MAX];
  static BleAdvReport batch[ESPAPI_ADV_BATCH_MAX];
  size_t batch_n = 0;
  uint32_t batch_first_ms = 0;
  uint32_t last_rx_ms = 0;
  int cfd = -1;

  for (;;) {
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(ls, &rf);
    int maxfd = ls;
    if (cfd >= 0) {
      FD_SET(cfd, &rf);
      maxfd = cfd > ls ? cfd : ls;
    }
    struct timeval tv = {0, 20 * 1000};
    const int sel = select(maxfd + 1, &rf, nullptr, nullptr, &tv);
    const uint32_t now = now_ms();

    if (sel > 0 && FD_ISSET(ls, &rf)) {
      const int nfd = accept(ls, nullptr, nullptr);
      if (nfd >= 0) {
        // Newest connection wins: a reconnecting Home Assistant must not be
        // locked out by its own stale socket.
        close_client(&cfd);
        cfd = nfd;
        struct timeval to = {0, 500 * 1000};
        setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof(to));
        espapi_session_reset(&sess);
        batch_n = 0;
        last_rx_ms = now;
        s_clients_total++;
        s_client_connected = true;
      }
    }
    if (cfd >= 0 && sel > 0 && FD_ISSET(cfd, &rf)) {
      uint8_t in[256];
      const int r = recv(cfd, in, sizeof(in), 0);
      size_t out_len = 0;
      int rc = ESPAPI_ERROR;
      if (r > 0) {
        last_rx_ms = now;
        rc = espapi_session_feed(&sess, &s_info, in, (size_t)r, tx, sizeof(tx),
                                 &out_len);
      }
      if (rc != ESPAPI_ERROR && out_len && send_all(cfd, tx, out_len) != 0)
        rc = ESPAPI_ERROR;
      if (rc != ESPAPI_OK)
        close_client(&cfd);
      else
        s_client_subscribed = sess.subscribed;
    }
    // Home Assistant pings only when it hears nothing, so a subscriber
    // receiving adverts may stay silent; only an idle, unsubscribed socket is
    // reaped. A dead subscriber fails the next send (SO_SNDTIMEO).
    if (cfd >= 0 && !sess.subscribed && now - last_rx_ms > kIdleCloseMs)
      close_client(&cfd);

    // Drain every queued report: discard with no subscriber, else batch and
    // send a frame per ESPAPI_ADV_BATCH_MAX, the remainder after
    // kBatchFlushMs.
    const bool sink = cfd >= 0 && sess.subscribed;
    if (!sink)
      batch_n = 0;
    BleAdvReport rep;
    while (xQueueReceive(s_adv_q, &rep, 0) == pdTRUE) {
      if (!sink) {
        s_drop_nosub++;
        continue;
      }
      if (batch_n == 0)
        batch_first_ms = now;
      batch[batch_n++] = rep;
      if (batch_n == ESPAPI_ADV_BATCH_MAX &&
          !flush_batch(&cfd, batch, &batch_n))
        break;
    }
    if (cfd >= 0 && sess.subscribed && batch_n > 0 &&
        now - batch_first_ms >= kBatchFlushMs)
      flush_batch(&cfd, batch, &batch_n);
  }
}

static void advertise_mdns() {
  if (!pipecat_mdns_started())
    return;
  // HA's ESPHome zeroconf flow keys the device by `mac` (12 hex digits).
  char mac_plain[13] = "";
  const char *m = pipecat_sta_mac_str();
  for (size_t i = 0, j = 0; m[i] && j < 12; ++i)
    if (m[i] != ':')
      mac_plain[j++] = m[i];
  mdns_txt_item_t txt[] = {
      {"mac", mac_plain},
      {"version", s_esphome_version},
      {"platform", "ESP32"},
      {"board", "xiao-esp32-s3"},
      {"network", "wifi"},
      {"project_name", s_info.project_name},
      {"project_version", s_info.project_version},
  };
  const esp_err_t ret =
      mdns_service_add(nullptr, "_esphomelib", "_tcp", ESPAPI_PORT, txt,
                       sizeof(txt) / sizeof(txt[0]));
  if (ret != ESP_OK)
    ESP_LOGW(LOG_TAG, "ble_proxy: mdns _esphomelib add failed: %s",
             esp_err_to_name(ret));
}

void pipecat_ble_proxy_start() {
  load_nvs();
  if (!s_enabled) {
    // OFF costs nothing at runtime: no controller, no task, no socket, and
    // the controller's static RAM goes back to the heap for this boot.
    const size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    ESP_LOGI(LOG_TAG, "ble_proxy: off (NVS); released %d B controller RAM",
             (int)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) - before));
    return;
  }
  uint8_t bt[6] = {};
  esp_read_mac(bt, ESP_MAC_BT);
  snprintf(s_bt_mac, sizeof(s_bt_mac), "%02x:%02x:%02x:%02x:%02x:%02x", bt[0],
           bt[1], bt[2], bt[3], bt[4], bt[5]);
  const esp_app_desc_t *app = esp_app_get_description();
  snprintf(s_esphome_version, sizeof(s_esphome_version), "pipecat-%s",
           app->version);
  snprintf(s_friendly, sizeof(s_friendly), "XVF3800 %s", pipecat_hostname());
  s_info.name = pipecat_hostname();
  s_info.friendly_name = s_friendly;
  s_info.mac_address = pipecat_sta_mac_str();
  s_info.bt_mac_address = s_bt_mac;
  s_info.model = "XVF3800 voice satellite (XIAO ESP32-S3)";
  s_info.manufacturer = "Seeed Studio";
  s_info.project_name = "kyzcreig.pipecat-house-satellite";
  s_info.project_version = app->version;
  s_info.esphome_version = s_esphome_version;
  s_info.compilation_time = "";
  s_info.server_info = "pipecat-house-satellite ble_proxy";

  s_adv_q = xQueueCreate(kAdvQueueDepth, sizeof(BleAdvReport));
  s_cmd_q = xQueueCreate(4, sizeof(uint32_t));
  if (!s_adv_q || !s_cmd_q) {
    s_error = "queue alloc";
    return;
  }

  s_heap_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  esp_err_t ret = esp_bt_controller_init(&cfg);
  if (ret == ESP_OK)
    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
  if (ret == ESP_OK)
    ret = esp_vhci_host_register_callback(&s_vhci_cb);
  if (ret != ESP_OK) {
    s_error = "controller init";
    ESP_LOGE(LOG_TAG, "ble_proxy: controller init failed: %s",
             esp_err_to_name(ret));
    return;
  }
  // Voice first: ask the coexistence scheduler to favour Wi-Fi. Deprecated in
  // favour of per-profile status bits, which describe BLE roles (mesh, A2DP)
  // and have none for a plain scanner; the preference is still honoured.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
#pragma GCC diagnostic pop
  s_heap_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  ESP_LOGI(LOG_TAG, "ble_proxy: controller up, internal heap %u -> %u B",
           (unsigned)s_heap_before, (unsigned)s_heap_after);

  // Core 1, priority 1: below every audio, RTVI and LED task.
  if (xTaskCreatePinnedToCore(ble_proxy_task, "ble_proxy", 6144, nullptr, 1,
                              nullptr, 1) != pdPASS) {
    s_error = "task";
    return;
  }
  advertise_mdns();
}

// ---- HTTP ------------------------------------------------------------------

static esp_err_t ble_proxy_get(httpd_req_t *req) {
  char body[700];
  snprintf(body, sizeof(body),
           "{\"compiled\":true,\"enabled\":%s,\"running\":%s,\"error\":\"%s\","
           "\"scan\":\"passive\",\"interval_ms\":%u,\"window_ms\":%u,"
           "\"api_port\":%d,\"bt_mac\":\"%s\",\"client_connected\":%s,"
           "\"client_subscribed\":%s,\"clients_total\":%u,"
           "\"reports_rx\":%u,\"reports_sent\":%u,\"batches\":%u,"
           "\"drop_queue\":%u,\"drop_nosub\":%u,\"hci_bad\":%u,"
           "\"heap_internal_before_bt\":%u,\"heap_internal_after_bt\":%u,"
           "\"heap_internal_free\":%u}",
           s_enabled ? "true" : "false", s_running ? "true" : "false", s_error,
           (unsigned)s_interval_ms, (unsigned)s_window_ms, ESPAPI_PORT,
           s_bt_mac, s_client_connected ? "true" : "false",
           s_client_subscribed ? "true" : "false", (unsigned)s_clients_total,
           (unsigned)s_reports_rx, (unsigned)s_reports_sent,
           (unsigned)s_batches, (unsigned)s_drop_queue, (unsigned)s_drop_nosub,
           (unsigned)s_hci_bad, (unsigned)s_heap_before, (unsigned)s_heap_after,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, body);
}

static void ble_reboot_task(void *) {
  vTaskDelay(pdMS_TO_TICKS(kRebootDelayMs));
  ESP_LOGI(LOG_TAG, "ble_proxy: rebooting to apply the scanner setting");
  esp_restart();
}

static bool query_uint(const char *q, const char *key, uint32_t *out) {
  char v[12];
  if (httpd_query_key_value(q, key, v, sizeof(v)) != ESP_OK)
    return false;
  char *end = nullptr;
  const unsigned long x = strtoul(v, &end, 10);
  if (end == v || *end != '\0' || x > 0xFFFFu)
    return false;
  *out = (uint32_t)x;
  return true;
}

static esp_err_t ble_proxy_post(httpd_req_t *req) {
  char q[96] = "";
  if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "enabled=0|1 [interval_ms= window_ms=]");
    return ESP_FAIL;
  }
  // The values in NVS now (s_enabled etc. hold the boot-time copy).
  bool enabled = s_enabled;
  uint32_t interval = s_interval_ms, window = s_window_ms, x = 0;
  char tmp[4];
  const bool has_en =
      httpd_query_key_value(q, "enabled", tmp, sizeof(tmp)) == ESP_OK;
  if (has_en) {
    if (strcmp(tmp, "0") != 0 && strcmp(tmp, "1") != 0) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "enabled must be 0 or 1");
      return ESP_FAIL;
    }
    enabled = tmp[0] == '1';
  }
  const bool has_iv = query_uint(q, "interval_ms", &x);
  if (has_iv)
    interval = x;
  const bool has_wn = query_uint(q, "window_ms", &x);
  if (has_wn)
    window = x;
  uint16_t iu = 0, wu = 0;
  if (ble_hci_scan_timing(interval, window, &iu, &wu) != 0 || window < 3) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "need 3 <= window_ms <= interval_ms <= 10240");
    return ESP_FAIL;
  }
  if (!has_en && !has_iv && !has_wn) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "nothing to set");
    return ESP_FAIL;
  }
  const bool changed = enabled != s_enabled || interval != s_interval_ms ||
                       window != s_window_ms;
  // The controller is only started at boot: a reboot applies an on/off flip,
  // or new timing while on. Timing saved while off needs none.
  const bool reboot = enabled != s_enabled || (enabled && changed);
  if (changed) {
    const esp_err_t ret =
        save_nvs(enabled, (uint16_t)interval, (uint16_t)window);
    if (ret != ESP_OK) {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs write");
      return ESP_FAIL;
    }
    if (!reboot) {
      s_interval_ms = (uint16_t)interval;
      s_window_ms = (uint16_t)window;
    }
  }
  char body[160];
  snprintf(body, sizeof(body),
           "{\"status\":\"%s\",\"enabled\":%s,\"interval_ms\":%u,"
           "\"window_ms\":%u}",
           reboot ? "reboot_pending" : (changed ? "saved" : "unchanged"),
           enabled ? "true" : "false", (unsigned)interval, (unsigned)window);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, body);
  if (reboot)
    xTaskCreate(ble_reboot_task, "ble_reboot", 2048, nullptr, 5, nullptr);
  return ESP_OK;
}

esp_err_t pipecat_ble_proxy_register_http(httpd_handle_t server) {
  httpd_uri_t get_uri = {
      .uri = "/ble/proxy",
      .method = HTTP_GET,
      .handler = ble_proxy_get,
      .user_ctx = nullptr,
  };
  httpd_uri_t post_uri = {
      .uri = "/ble/proxy",
      .method = HTTP_POST,
      .handler = ble_proxy_post,
      .user_ctx = nullptr,
  };
  esp_err_t ret = httpd_register_uri_handler(server, &get_uri);
  if (ret == ESP_OK)
    ret = httpd_register_uri_handler(server, &post_uri);
  return ret;
}

#endif  // PIPECAT_BLE_PROXY
