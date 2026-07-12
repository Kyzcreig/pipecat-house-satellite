#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

#include "main.h"

#define OTA_HTTP_PORT 80
#define OTA_REBOOT_DELAY_MS 1000
#define OTA_ROLLBACK_TIMEOUT_MS 60000
#define OTA_CHUNK_SIZE 4096
#define OTA_NVS_NAMESPACE "ota"
#define OTA_NVS_SHA_KEY "last_sha"
#define OTA_NVS_LABEL_KEY "last_label"

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

static httpd_handle_t g_ota_server = nullptr;
static bool g_mdns_started = false;
static bool g_validation_confirmed = false;

static const char *ota_state_name(esp_ota_img_states_t state) {
  switch (state) {
    case ESP_OTA_IMG_NEW:
      return "new";
    case ESP_OTA_IMG_PENDING_VERIFY:
      return "pending_verify";
    case ESP_OTA_IMG_VALID:
      return "valid";
    case ESP_OTA_IMG_INVALID:
      return "invalid";
    case ESP_OTA_IMG_ABORTED:
      return "aborted";
    case ESP_OTA_IMG_UNDEFINED:
      return "undefined";
    default:
      return "unknown";
  }
}

static void bytes_to_hex(const uint8_t *bytes, size_t len, char *out,
                         size_t out_len) {
  static const char hex[] = "0123456789abcdef";
  if (out_len < (len * 2 + 1)) {
    if (out_len > 0) out[0] = '\0';
    return;
  }
  for (size_t i = 0; i < len; i++) {
    out[i * 2] = hex[(bytes[i] >> 4) & 0x0f];
    out[i * 2 + 1] = hex[bytes[i] & 0x0f];
  }
  out[len * 2] = '\0';
}

static bool get_running_ota_state(esp_ota_img_states_t *state) {
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_err_t ret = esp_ota_get_state_partition(running, state);
  if (ret == ESP_ERR_NOT_SUPPORTED || ret == ESP_ERR_NOT_FOUND) {
    *state = ESP_OTA_IMG_UNDEFINED;
    return true;
  }
  if (ret != ESP_OK) {
    ESP_LOGW(LOG_TAG, "Unable to read OTA state: %s", esp_err_to_name(ret));
    *state = ESP_OTA_IMG_UNDEFINED;
    return false;
  }
  return true;
}

static bool ota_state_is_valid_for_status(esp_ota_img_states_t state) {
  return state == ESP_OTA_IMG_VALID || state == ESP_OTA_IMG_UNDEFINED ||
         state == ESP_OTA_IMG_NEW;
}

static bool health_check_passes() {
  bool healthy = pipecat_wifi_connected() && pipecat_mdns_started() &&
                 pipecat_ota_server_started() && pipecat_xvf3800_present();
  if (!healthy) {
    static int64_t last_log_us = 0;
    int64_t now_us = esp_timer_get_time();
    if (now_us - last_log_us > 5000000LL) {
      last_log_us = now_us;
      ESP_LOGW(LOG_TAG,
               "OTA validation health failed: wifi=%d mdns=%d ota_http=%d xvf3800=%d",
               pipecat_wifi_connected(), pipecat_mdns_started(),
               pipecat_ota_server_started(), pipecat_xvf3800_present());
    }
  }
  return healthy;
}

static esp_err_t save_uploaded_sha(const char *sha_hex, const char *label) {
  nvs_handle_t nvs;
  esp_err_t ret = nvs_open(OTA_NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (ret != ESP_OK) return ret;
  ret = nvs_set_str(nvs, OTA_NVS_SHA_KEY, sha_hex);
  if (ret == ESP_OK) ret = nvs_set_str(nvs, OTA_NVS_LABEL_KEY, label);
  if (ret == ESP_OK) ret = nvs_commit(nvs);
  nvs_close(nvs);
  return ret;
}

static bool load_uploaded_sha_for_running(char *sha_hex, size_t sha_hex_len) {
  const esp_partition_t *running = esp_ota_get_running_partition();
  char label[17] = {};
  char saved_sha[65] = {};
  size_t label_len = sizeof(label);
  size_t sha_len = sizeof(saved_sha);
  nvs_handle_t nvs;
  esp_err_t ret = nvs_open(OTA_NVS_NAMESPACE, NVS_READONLY, &nvs);
  if (ret != ESP_OK) return false;
  ret = nvs_get_str(nvs, OTA_NVS_LABEL_KEY, label, &label_len);
  if (ret == ESP_OK) ret = nvs_get_str(nvs, OTA_NVS_SHA_KEY, saved_sha, &sha_len);
  nvs_close(nvs);
  if (ret != ESP_OK || strncmp(label, running->label, sizeof(label)) != 0) {
    return false;
  }
  strlcpy(sha_hex, saved_sha, sha_hex_len);
  return true;
}

static void running_partition_sha(char *sha_hex, size_t sha_hex_len) {
  const esp_partition_t *running = esp_ota_get_running_partition();
  uint8_t sha[32] = {};
  esp_err_t ret = esp_partition_get_sha256(running, sha);
  if (ret != ESP_OK) {
    ESP_LOGW(LOG_TAG, "Unable to hash running partition: %s",
             esp_err_to_name(ret));
    strlcpy(sha_hex, "unknown", sha_hex_len);
    return;
  }
  bytes_to_hex(sha, sizeof(sha), sha_hex, sha_hex_len);
}

static void reboot_task(void *arg) {
  vTaskDelay(pdMS_TO_TICKS(OTA_REBOOT_DELAY_MS));
  ESP_LOGI(LOG_TAG, "Rebooting into OTA image");
  esp_restart();
}

static esp_err_t ota_status_handler(httpd_req_t *req) {
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  get_running_ota_state(&state);

  char sha_hex[65] = {};
  if (!load_uploaded_sha_for_running(sha_hex, sizeof(sha_hex))) {
    running_partition_sha(sha_hex, sizeof(sha_hex));
  }

  const esp_app_desc_t *app = esp_app_get_description();
  int64_t uptime_s = esp_timer_get_time() / 1000000LL;
  bool app_valid = ota_state_is_valid_for_status(state);
  char body[512];
  snprintf(body, sizeof(body),
           "{\"booted_slot\":\"%s\",\"app_valid\":%s,"
           "\"ota_state\":\"%s\",\"sha256\":\"%s\",\"uptime_s\":%" PRId64
           ",\"firmware_version\":\"%s\",\"satellite_id\":\"%s\","
           "\"mdns_hostname\":\"%s.local\"}",
           running->label, app_valid ? "true" : "false", ota_state_name(state),
           sha_hex, uptime_s, app->version, PIPECAT_SATELLITE_ID,
           PIPECAT_MDNS_HOSTNAME);

  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, body);
}

static esp_err_t ota_rollback_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "application/json");
  esp_err_t ret = httpd_resp_sendstr(req, "{\"status\":\"rollback_pending\"}");
  ESP_LOGW(LOG_TAG, "Manual OTA rollback requested");
  esp_ota_mark_app_invalid_rollback_and_reboot();
  return ret;
}

static esp_err_t ota_upload_handler(httpd_req_t *req) {
  if (req->content_len <= 0) {
    httpd_resp_send_err(req, HTTPD_411_LENGTH_REQUIRED,
                        "Content-Length is required");
    return ESP_FAIL;
  }

  const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
  if (update == nullptr) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "No OTA update partition available");
    return ESP_FAIL;
  }

  ESP_LOGI(LOG_TAG, "OTA upload starting: %d bytes -> %s", req->content_len,
           update->label);

  esp_ota_handle_t ota_handle = 0;
  esp_err_t ret = esp_ota_begin(update, OTA_SIZE_UNKNOWN, &ota_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(LOG_TAG, "esp_ota_begin failed: %s", esp_err_to_name(ret));
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
    return ESP_FAIL;
  }

  uint8_t *buf = (uint8_t *)malloc(OTA_CHUNK_SIZE);
  if (buf == nullptr) {
    esp_ota_abort(ota_handle);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "Unable to allocate OTA buffer");
    return ESP_FAIL;
  }

  mbedtls_sha256_context sha_ctx;
  mbedtls_sha256_init(&sha_ctx);
  mbedtls_sha256_starts(&sha_ctx, false);

  int remaining = req->content_len;
  int written = 0;
  while (remaining > 0) {
    int recv_len = httpd_req_recv(req, (char *)buf, MIN(remaining, OTA_CHUNK_SIZE));
    if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
      continue;
    }
    if (recv_len <= 0) {
      ESP_LOGE(LOG_TAG, "OTA upload recv failed: %d", recv_len);
      free(buf);
      mbedtls_sha256_free(&sha_ctx);
      esp_ota_abort(ota_handle);
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                          "OTA upload receive failed");
      return ESP_FAIL;
    }

    ret = esp_ota_write(ota_handle, buf, recv_len);
    if (ret != ESP_OK) {
      ESP_LOGE(LOG_TAG, "esp_ota_write failed: %s", esp_err_to_name(ret));
      free(buf);
      mbedtls_sha256_free(&sha_ctx);
      esp_ota_abort(ota_handle);
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                          "OTA write failed");
      return ESP_FAIL;
    }
    mbedtls_sha256_update(&sha_ctx, buf, recv_len);
    remaining -= recv_len;
    written += recv_len;
    if ((written % (256 * 1024)) < recv_len || remaining == 0) {
      ESP_LOGI(LOG_TAG, "OTA upload progress: %d/%d bytes", written,
               req->content_len);
    }
  }
  free(buf);

  uint8_t raw_sha[32] = {};
  char sha_hex[65] = {};
  mbedtls_sha256_finish(&sha_ctx, raw_sha);
  mbedtls_sha256_free(&sha_ctx);
  bytes_to_hex(raw_sha, sizeof(raw_sha), sha_hex, sizeof(sha_hex));

  ret = esp_ota_end(ota_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(LOG_TAG, "esp_ota_end failed: %s", esp_err_to_name(ret));
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA end failed");
    return ESP_FAIL;
  }

  ret = save_uploaded_sha(sha_hex, update->label);
  if (ret != ESP_OK) {
    ESP_LOGW(LOG_TAG, "Unable to persist uploaded SHA: %s", esp_err_to_name(ret));
  }

  ret = esp_ota_set_boot_partition(update);
  if (ret != ESP_OK) {
    ESP_LOGE(LOG_TAG, "esp_ota_set_boot_partition failed: %s",
             esp_err_to_name(ret));
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "OTA boot partition switch failed");
    return ESP_FAIL;
  }

  char body[128];
  snprintf(body, sizeof(body),
           "{\"status\":\"reboot_pending\",\"boot_slot\":\"%s\",\"sha256\":\"%s\"}",
           update->label, sha_hex);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, body);
  xTaskCreate(reboot_task, "ota_reboot", 2048, NULL, 5, NULL);
  return ESP_OK;
}

void pipecat_init_mdns() {
  ESP_ERROR_CHECK(mdns_init());
  ESP_ERROR_CHECK(mdns_hostname_set(PIPECAT_MDNS_HOSTNAME));
  ESP_ERROR_CHECK(mdns_instance_name_set(PIPECAT_MDNS_INSTANCE));
  mdns_txt_item_t service_txt[] = {
      {"satellite_id", PIPECAT_SATELLITE_ID},
      {"fw", "pipecat-house-satellite"},
      {"role", "xvf3800"},
  };
  ESP_ERROR_CHECK(mdns_service_add(PIPECAT_MDNS_INSTANCE, "_http", "_tcp",
                                   OTA_HTTP_PORT, service_txt,
                                   sizeof(service_txt) / sizeof(service_txt[0])));
  g_mdns_started = true;
  ESP_LOGI(LOG_TAG, "mDNS registered: %s.local (%s)",
           PIPECAT_MDNS_HOSTNAME, PIPECAT_MDNS_INSTANCE);
}

// POST /xvf/tune?param=<name>&value=<float> — live AEC/AUDIO_MGR tuning without a
// reflash. Param allowlist is in pipecat_xvf_tune() (media.cpp). Volatile writes.
static esp_err_t xvf_tune_handler(httpd_req_t *req) {
  char query[128] = {0};
  char param[32] = {0};
  char value_s[32] = {0};
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "param", param, sizeof(param)) != ESP_OK ||
      httpd_query_key_value(query, "value", value_s, sizeof(value_s)) != ESP_OK) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "{\"error\":\"need ?param=<name>&value=<float>\"}");
    return ESP_OK;
  }
  float value = strtof(value_s, NULL);
  esp_err_t ret = pipecat_xvf_tune(param, value);
  char body[128];
  if (ret == ESP_OK) {
    snprintf(body, sizeof(body), "{\"ok\":true,\"param\":\"%s\",\"value\":%.4f}",
             param, (double)value);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body);
  } else if (ret == ESP_ERR_NOT_FOUND) {
    httpd_resp_set_status(req, "404 Not Found");
    snprintf(body, sizeof(body), "{\"error\":\"unknown param '%s'\"}", param);
    httpd_resp_sendstr(req, body);
  } else {
    httpd_resp_set_status(req, "500 Internal Server Error");
    snprintf(body, sizeof(body), "{\"error\":\"write failed: %s\"}",
             esp_err_to_name(ret));
    httpd_resp_sendstr(req, body);
  }
  return ESP_OK;
}

// GET /playback/stats[?prebuffer_ms=N] — cumulative ring/playback counters.
// Crackle triage: underruns>0 during crackle = delivery timing (raise
// prebuffer_ms); clean counters during crackle = look below the ring
// (XVF/codec registers). prebuffer_ms is volatile (default 100).
extern volatile uint32_t g_play_stat_frames;
extern volatile uint32_t g_play_stat_write_fail;
extern volatile uint32_t g_play_stat_underruns;
extern volatile uint32_t g_play_stat_plc;
extern volatile uint32_t g_play_stat_fec;
// gap_resumes: full-ring-drain followed by refill within the resume window
// (default 750ms) = a mid-speech gap the old underruns counter missed
// (blind spot proven 2026-07-11: audible gaps with underruns=0).
extern volatile uint32_t g_play_stat_gap_resumes;
extern volatile uint32_t g_play_prebuffer_samples;
// Phase 6 adaptive prebuffer (NetEQ-lite, dark unless
// PIPECAT_ADAPTIVE_PREBUFFER=1): effective prebuffer actually in force +
// cumulative step transitions. Adaptive off => effective==prebuffer_ms, steps=0.
extern volatile uint32_t g_play_prebuffer_effective_ms;
extern volatile uint32_t g_play_prebuffer_steps;
// From vendored components/peer/rtp.c — splits reordering from true loss.
extern "C" {
extern volatile uint32_t g_rtp_late_drops;
extern volatile uint32_t g_rtp_gap_events;
// RED / RFC 2198 counters (audio-resilience ladder Phase 3, 2026-07-11).
// packets_received is the frozen loss_burden denominator (ladder spec REV 3):
//   loss_burden = (plc + fec + red_recovered + nack_recovered) / packets_received
extern volatile uint32_t g_rtp_packets_received;
extern volatile uint32_t g_red_recovered;
extern volatile uint32_t g_red_dup_drops;
// NACK retransmit counters (audio-resilience ladder Phase 5, 2026-07-12).
// Always linked (rtp.c defines them unconditionally); they stay 0 unless the
// firmware was built with PIPECAT_NACK=1 AND the server lane is enabled.
extern volatile uint32_t g_nack_sent;
extern volatile uint32_t g_nack_recovered;
extern volatile uint32_t g_nack_late;
}
void pipecat_play_selftest_clip();

// POST /playback/selftest — play the flash-embedded clip straight into the
// playback ring: NO opus, NO network. Splits opus/transport vs DAC/analog.
static esp_err_t playback_selftest_handler(httpd_req_t *req) {
  pipecat_play_selftest_clip();
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"ok\":true,\"path\":\"flash->ring->FIR->I2S (no opus/network)\"}");
  return ESP_OK;
}

static esp_err_t playback_stats_handler(httpd_req_t *req) {
  char query[64] = {0};
  char val[16] = {0};
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
      httpd_query_key_value(query, "prebuffer_ms", val, sizeof(val)) ==
          ESP_OK) {
    int ms = atoi(val);
    if (ms >= 20 && ms <= 1000) {
      g_play_prebuffer_samples = (uint32_t)(ms * 16);  // 16 samples/ms @16k
    }
  }
  char body[640];
  snprintf(body, sizeof(body),
           "{\"frames\":%lu,\"write_fail\":%lu,\"underruns\":%lu,\"plc\":%lu,"
           "\"fec\":%lu,\"late_drops\":%lu,\"gap_events\":%lu,"
           "\"packets_received\":%lu,\"red_recovered\":%lu,\"red_dup_drops\":%lu,"
           "\"nack_sent\":%lu,\"nack_recovered\":%lu,\"nack_late\":%lu,"
           "\"gap_resumes\":%lu,\"prebuffer_ms\":%lu,"
           "\"prebuffer_effective_ms\":%lu,\"prebuffer_steps\":%lu}",
           (unsigned long)g_play_stat_frames,
           (unsigned long)g_play_stat_write_fail,
           (unsigned long)g_play_stat_underruns,
           (unsigned long)g_play_stat_plc,
           (unsigned long)g_play_stat_fec,
           (unsigned long)g_rtp_late_drops,
           (unsigned long)g_rtp_gap_events,
           (unsigned long)g_rtp_packets_received,
           (unsigned long)g_red_recovered,
           (unsigned long)g_red_dup_drops,
           (unsigned long)g_nack_sent,
           (unsigned long)g_nack_recovered,
           (unsigned long)g_nack_late,
           (unsigned long)g_play_stat_gap_resumes,
           (unsigned long)(g_play_prebuffer_samples / 16),
           (unsigned long)g_play_prebuffer_effective_ms,
           (unsigned long)g_play_prebuffer_steps);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, body);
  return ESP_OK;
}

void pipecat_init_ota_server() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = OTA_HTTP_PORT;
  config.ctrl_port = 32768;
  config.max_uri_handlers = 6;
  config.recv_wait_timeout = 10;
  config.send_wait_timeout = 10;

  ESP_ERROR_CHECK(httpd_start(&g_ota_server, &config));

  httpd_uri_t upload_uri = {
      .uri = "/ota/upload",
      .method = HTTP_POST,
      .handler = ota_upload_handler,
      .user_ctx = NULL,
  };
  httpd_uri_t status_uri = {
      .uri = "/ota/status",
      .method = HTTP_GET,
      .handler = ota_status_handler,
      .user_ctx = NULL,
  };
  httpd_uri_t rollback_uri = {
      .uri = "/ota/rollback",
      .method = HTTP_POST,
      .handler = ota_rollback_handler,
      .user_ctx = NULL,
  };
  // Live XVF AEC/AUDIO_MGR tuning: POST /xvf/tune?param=<name>&value=<float>.
  // Writes are volatile (lost on XMOS power-cycle); bake winners into
  // configure_xvf3800_dsp_profile(). Allowlist lives in pipecat_xvf_tune().
  httpd_uri_t tune_uri = {
      .uri = "/xvf/tune",
      .method = HTTP_POST,
      .handler = xvf_tune_handler,
      .user_ctx = NULL,
  };
  httpd_uri_t stats_uri = {
      .uri = "/playback/stats",
      .method = HTTP_GET,
      .handler = playback_stats_handler,
      .user_ctx = NULL,
  };
  httpd_uri_t selftest_uri = {
      .uri = "/playback/selftest",
      .method = HTTP_POST,
      .handler = playback_selftest_handler,
      .user_ctx = NULL,
  };
  ESP_ERROR_CHECK(httpd_register_uri_handler(g_ota_server, &upload_uri));
  ESP_ERROR_CHECK(httpd_register_uri_handler(g_ota_server, &status_uri));
  ESP_ERROR_CHECK(httpd_register_uri_handler(g_ota_server, &rollback_uri));
  ESP_ERROR_CHECK(httpd_register_uri_handler(g_ota_server, &tune_uri));
  ESP_ERROR_CHECK(httpd_register_uri_handler(g_ota_server, &stats_uri));
  ESP_ERROR_CHECK(httpd_register_uri_handler(g_ota_server, &selftest_uri));
  ESP_LOGI(LOG_TAG, "OTA HTTP server listening on port %d", OTA_HTTP_PORT);
}

bool pipecat_mdns_started() { return g_mdns_started; }

bool pipecat_ota_server_started() { return g_ota_server != nullptr; }

static void ota_validation_watchdog_task(void *arg) {
  vTaskDelay(pdMS_TO_TICKS(OTA_ROLLBACK_TIMEOUT_MS));
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  get_running_ota_state(&state);
  if (!g_validation_confirmed && state == ESP_OTA_IMG_PENDING_VERIFY) {
    ESP_LOGE(LOG_TAG,
             "OTA image still pending after %d ms; marking invalid and rolling back",
             OTA_ROLLBACK_TIMEOUT_MS);
    esp_ota_mark_app_invalid_rollback_and_reboot();
  }
  vTaskDelete(NULL);
}

void pipecat_start_ota_validation_watchdog() {
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  get_running_ota_state(&state);
  ESP_LOGI(LOG_TAG, "Running OTA slot state: %s", ota_state_name(state));
  if (state == ESP_OTA_IMG_PENDING_VERIFY) {
    xTaskCreate(ota_validation_watchdog_task, "ota_watchdog", 4096, NULL, 5,
                NULL);
  } else {
    g_validation_confirmed = true;
  }
}

void pipecat_validate_ota_if_healthy() {
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  get_running_ota_state(&state);
  if (state != ESP_OTA_IMG_PENDING_VERIFY) {
    g_validation_confirmed = ota_state_is_valid_for_status(state);
    return;
  }

  if (!health_check_passes()) {
    return;
  }

  esp_err_t ret = esp_ota_mark_app_valid_cancel_rollback();
  if (ret == ESP_OK) {
    g_validation_confirmed = true;
    ESP_LOGI(LOG_TAG, "OTA image validated; rollback cancelled");
  } else {
    ESP_LOGE(LOG_TAG, "Unable to validate OTA image: %s", esp_err_to_name(ret));
  }
}
