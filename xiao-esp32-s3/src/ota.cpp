#include <inttypes.h>
#include <math.h>
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
#define DSP_NVS_NAMESPACE "xvf_dsp"

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

static esp_err_t save_dsp_param(const char *param, float value) {
  if (!pipecat_xvf_param_persistent(param) || !isfinite(value)) {
    return ESP_ERR_INVALID_ARG;
  }
  nvs_handle_t nvs;
  esp_err_t ret = nvs_open(DSP_NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (ret != ESP_OK) return ret;
  ret = nvs_set_blob(nvs, param, &value, sizeof(value));
  if (ret == ESP_OK) ret = nvs_commit(nvs);
  nvs_close(nvs);
  return ret;
}

static esp_err_t load_dsp_param(nvs_handle_t nvs, const char *param,
                                float *value) {
  size_t value_size = sizeof(*value);
  esp_err_t ret = nvs_get_blob(nvs, param, value, &value_size);
  if (ret == ESP_OK && value_size != sizeof(*value)) {
    return ESP_ERR_INVALID_SIZE;
  }
  return ret;
}

static esp_err_t clear_dsp_params() {
  nvs_handle_t nvs;
  esp_err_t ret = nvs_open(DSP_NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (ret != ESP_OK) return ret;
  ret = nvs_erase_all(nvs);
  if (ret == ESP_OK) ret = nvs_commit(nvs);
  nvs_close(nvs);
  return ret;
}

static void restore_dsp_default(const char *param) {
  float default_value = 0.0f;
  if (!pipecat_xvf_param_default(param, &default_value)) {
    ESP_LOGE(LOG_TAG, "nvs_dsp: %s has no baked fallback", param);
    return;
  }
  PipecatXvfTuneResult fallback = {};
  esp_err_t ret = pipecat_xvf_tune(param, default_value, &fallback);
  if (ret != ESP_OK || !fallback.applied) {
    ESP_LOGE(LOG_TAG,
             "nvs_dsp: %s baked fallback %.6g FAILED: %s applied=%d",
             param, (double)default_value, esp_err_to_name(ret),
             fallback.applied);
  } else {
    ESP_LOGW(LOG_TAG, "nvs_dsp: %s restored baked fallback %.6g", param,
             (double)fallback.applied_value);
  }
}

void pipecat_replay_xvf_params() {
  uint32_t applied = 0;
  nvs_handle_t nvs;
  esp_err_t open_ret = nvs_open(DSP_NVS_NAMESPACE, NVS_READONLY, &nvs);
  if (open_ret == ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGI(LOG_TAG, "nvs_dsp: %lu params applied", (unsigned long)applied);
    return;
  }
  if (open_ret != ESP_OK) {
    ESP_LOGE(LOG_TAG, "nvs_dsp: namespace open failed: %s",
             esp_err_to_name(open_ret));
    ESP_LOGI(LOG_TAG, "nvs_dsp: %lu params applied", (unsigned long)applied);
    return;
  }

  for (size_t i = 0; i < pipecat_xvf_persistent_param_count(); i++) {
    const char *param = pipecat_xvf_persistent_param_name(i);
    float stored_value = 0.0f;
    esp_err_t load_ret = load_dsp_param(nvs, param, &stored_value);
    if (load_ret == ESP_ERR_NVS_NOT_FOUND) {
      continue;
    }
    if (load_ret != ESP_OK || !isfinite(stored_value)) {
      ESP_LOGE(LOG_TAG,
               "nvs_dsp: %s stored value invalid (%s); restoring baked default",
               param, esp_err_to_name(load_ret));
      restore_dsp_default(param);
      continue;
    }

    PipecatXvfTuneResult result = {};
    esp_err_t tune_ret = pipecat_xvf_tune(param, stored_value, &result);
    if (tune_ret != ESP_OK || !result.applied) {
      ESP_LOGE(LOG_TAG,
               "nvs_dsp: %s replay %.6g FAILED: %s applied=%d; restoring "
               "baked default",
               param, (double)stored_value, esp_err_to_name(tune_ret),
               result.applied);
      restore_dsp_default(param);
      continue;
    }

    applied++;
    ESP_LOGI(LOG_TAG,
             "nvs_dsp: %s stored=%.6g applied=%.6g readback=%s%.6g "
             "clamped=%d",
             param, (double)stored_value, (double)result.applied_value,
             result.readback_valid ? "" : "ack-only:",
             (double)result.readback, result.clamped);
    if (result.clamped) {
      esp_err_t save_ret = save_dsp_param(param, result.applied_value);
      if (save_ret != ESP_OK) {
        ESP_LOGE(LOG_TAG, "nvs_dsp: %s normalized-value persist failed: %s",
                 param, esp_err_to_name(save_ret));
      }
    }
  }
  nvs_close(nvs);
  ESP_LOGI(LOG_TAG, "nvs_dsp: %lu params applied", (unsigned long)applied);
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

// POST /xvf/tune?param=<name>&value=<float>[&persist=0]. Persistent allowlisted
// params are committed to NVS only after typed write/readback succeeds. dac_atten
// is ack-only. persist=0 keeps one-off experiments volatile.
static esp_err_t xvf_tune_handler(httpd_req_t *req) {
  char query[128] = {0};
  char param[32] = {0};
  char value_s[32] = {0};
  char persist_s[8] = {0};
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "param", param, sizeof(param)) != ESP_OK ||
      httpd_query_key_value(query, "value", value_s, sizeof(value_s)) != ESP_OK) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "{\"error\":\"need ?param=<name>&value=<float>\"}");
    return ESP_OK;
  }
  bool persist =
      httpd_query_key_value(query, "persist", persist_s, sizeof(persist_s)) !=
          ESP_OK ||
      strcmp(persist_s, "0") != 0;
  char *value_end = nullptr;
  float value = strtof(value_s, &value_end);
  if (value_end == value_s || *value_end != '\0' || !isfinite(value)) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "{\"error\":\"value must be a finite number\"}");
    return ESP_OK;
  }

  PipecatXvfTuneResult result = {};
  esp_err_t ret = pipecat_xvf_tune(param, value, &result);
  bool persisted = false;
  if (ret == ESP_OK && result.applied && persist &&
      pipecat_xvf_param_persistent(param)) {
    ret = save_dsp_param(param, result.applied_value);
    if (ret == ESP_OK) {
      persisted = true;
    }
  }

  char body[320];
  if (ret == ESP_OK) {
    if (result.readback_valid) {
      snprintf(body, sizeof(body),
               "{\"ok\":true,\"param\":\"%s\",\"requested\":%.6g,"
               "\"value\":%.6g,\"readback\":%.6g,\"applied\":true,"
               "\"clamped\":%s,\"persisted\":%s}",
               param, (double)value, (double)result.applied_value,
               (double)result.readback, result.clamped ? "true" : "false",
               persisted ? "true" : "false");
    } else {
      snprintf(body, sizeof(body),
               "{\"ok\":true,\"param\":\"%s\",\"requested\":%.6g,"
               "\"value\":%.6g,\"readback\":null,\"applied\":null,"
               "\"clamped\":%s,\"persisted\":%s}",
               param, (double)value, (double)result.applied_value,
               result.clamped ? "true" : "false",
               persisted ? "true" : "false");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body);
  } else if (ret == ESP_ERR_NOT_FOUND) {
    httpd_resp_set_status(req, "404 Not Found");
    snprintf(body, sizeof(body), "{\"error\":\"unknown param '%s'\"}", param);
    httpd_resp_sendstr(req, body);
  } else if (ret == ESP_ERR_INVALID_ARG) {
    httpd_resp_set_status(req, "400 Bad Request");
    snprintf(body, sizeof(body), "{\"error\":\"invalid value for '%s'\"}",
             param);
    httpd_resp_sendstr(req, body);
  } else {
    httpd_resp_set_status(req, "500 Internal Server Error");
    snprintf(body, sizeof(body), "{\"error\":\"tune/persist failed: %s\"}",
             esp_err_to_name(ret));
    httpd_resp_sendstr(req, body);
  }
  return ESP_OK;
}

// GET /xvf/params[?reset=1] — report the durable device-owned source of truth.
static esp_err_t xvf_params_handler(httpd_req_t *req) {
  char query[64] = {0};
  char reset[8] = {0};
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
      httpd_query_key_value(query, "reset", reset, sizeof(reset)) == ESP_OK &&
      strcmp(reset, "1") == 0) {
    esp_err_t ret = clear_dsp_params();
    if (ret != ESP_OK) {
      httpd_resp_set_status(req, "500 Internal Server Error");
      char error_body[96];
      snprintf(error_body, sizeof(error_body),
               "{\"error\":\"NVS reset failed: %s\"}", esp_err_to_name(ret));
      return httpd_resp_sendstr(req, error_body);
    }
    ESP_LOGW(LOG_TAG, "nvs_dsp: namespace cleared by /xvf/params?reset=1");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(
        req, "{\"ok\":true,\"reset\":true,\"params\":{},\"count\":0}");
  }

  char body[512] = {0};
  size_t used = static_cast<size_t>(
      snprintf(body, sizeof(body), "{\"params\":{"));
  uint32_t count = 0;
  nvs_handle_t nvs;
  esp_err_t open_ret = nvs_open(DSP_NVS_NAMESPACE, NVS_READONLY, &nvs);
  if (open_ret == ESP_OK) {
    for (size_t i = 0; i < pipecat_xvf_persistent_param_count(); i++) {
      const char *param = pipecat_xvf_persistent_param_name(i);
      float value = 0.0f;
      esp_err_t load_ret = load_dsp_param(nvs, param, &value);
      if (load_ret == ESP_ERR_NVS_NOT_FOUND) continue;
      if (load_ret != ESP_OK) {
        ESP_LOGE(LOG_TAG, "nvs_dsp: introspection read %s failed: %s", param,
                 esp_err_to_name(load_ret));
        continue;
      }
      int written = isfinite(value)
                        ? snprintf(body + used, sizeof(body) - used,
                                   "%s\"%s\":%.9g", count ? "," : "", param,
                                   (double)value)
                        : snprintf(body + used, sizeof(body) - used,
                                   "%s\"%s\":null", count ? "," : "", param);
      if (written < 0 || static_cast<size_t>(written) >= sizeof(body) - used) {
        nvs_close(nvs);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req,
                                  "{\"error\":\"params response overflow\"}");
      }
      used += static_cast<size_t>(written);
      count++;
    }
    nvs_close(nvs);
  } else if (open_ret != ESP_ERR_NVS_NOT_FOUND) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    char error_body[96];
    snprintf(error_body, sizeof(error_body),
             "{\"error\":\"NVS open failed: %s\"}",
             esp_err_to_name(open_ret));
    return httpd_resp_sendstr(req, error_body);
  }

  snprintf(body + used, sizeof(body) - used, "},\"count\":%lu}",
           (unsigned long)count);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, body);
}

// GET /xvf/audio-mux[?op_r_category=N&op_r_source=N] — typed live readback and
// experiment-only right-slot selection. Build flags are intent; register values
// prove what the live XVF actually accepted. The media-layer API bounds the
// complete vendor surface (category 0..12, source 0..5) and verifies the write
// with a register readback; the experiment records undefined pairs separately.
static esp_err_t xvf_audio_mux_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "application/json");
  PipecatXvfAudioMuxStatus status = {};
  char query[96] = {0};
  char category_text[8] = {0};
  char source_text[8] = {0};
  bool runtime_write = false;
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    esp_err_t category_ret = httpd_query_key_value(
        query, "op_r_category", category_text, sizeof(category_text));
    esp_err_t source_ret = httpd_query_key_value(
        query, "op_r_source", source_text, sizeof(source_text));
    if ((category_ret == ESP_OK) != (source_ret == ESP_OK)) {
      httpd_resp_set_status(req, "400 Bad Request");
      return httpd_resp_sendstr(
          req, "{\"ok\":false,\"error\":\"op_r_category and op_r_source are both required\"}");
    }
    if (category_ret == ESP_OK) {
      char *category_end = nullptr;
      char *source_end = nullptr;
      long category_value = strtol(category_text, &category_end, 10);
      long source_value = strtol(source_text, &source_end, 10);
      if (*category_text == '\0' || *category_end != '\0' ||
          *source_text == '\0' || *source_end != '\0' || category_value < 0 ||
          category_value > UINT8_MAX || source_value < 0 ||
          source_value > UINT8_MAX) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(
            req, "{\"ok\":false,\"error\":\"mux values must be unsigned bytes\"}");
      }
      uint8_t category = static_cast<uint8_t>(category_value);
      uint8_t source = static_cast<uint8_t>(source_value);
      runtime_write = true;
      if (!pipecat_xvf_set_audio_mux_right(category, source, &status)) {
        httpd_resp_set_status(req, "422 Unprocessable Entity");
        char error_body[160];
        snprintf(error_body, sizeof(error_body),
                 "{\"ok\":false,\"error\":\"mux write/readback rejected\","
                 "\"requested_op_r\":[%u,%u]}",
                 category, source);
        return httpd_resp_sendstr(req, error_body);
      }
    }
  }
  esp_err_t ret = runtime_write ? ESP_OK : pipecat_xvf_audio_mux_status(&status);
  char body[256];
  if (ret != ESP_OK) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    snprintf(body, sizeof(body),
             "{\"ok\":false,\"error\":\"mux readback failed: %s\"}",
             esp_err_to_name(ret));
  } else {
    snprintf(body, sizeof(body),
             "{\"ok\":true,\"op_l\":[%u,%u],\"op_r\":[%u,%u],"
             "\"upsample\":[%u,%u],\"dual_stream\":%s,\"raw_mic\":%d,"
             "\"runtime_write\":%s}",
             status.op_l_category, status.op_l_source,
             status.op_r_category, status.op_r_source,
             status.upsample_l, status.upsample_r,
             PIPECAT_DUAL_STREAM ? "true" : "false",
             PIPECAT_DUAL_STREAM_RAW_MIC,
             runtime_write ? "true" : "false");
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, body);
  return ESP_OK;
}

// GET/POST /xvf/packed[?enable=0|1&op_all=c,s,c,s,c,s,c,s,c,s,c,s] —
// packed six-channel TDM mode (t_2ccb0829 experiment C). GET with no query =
// readback. With enable, writes OP_ALL first (when given), then OP_PACKED,
// and verifies both by register readback. Packed mode breaks the normal
// uplink decimators — bench-capture (/xvf/raw-capture) only; always restore
// enable=0 + production mux afterwards.
static esp_err_t xvf_packed_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "application/json");
  PipecatXvfPackedStatus status = {};
  char query[128] = {0};
  char enable_text[4] = {0};
  char op_all_text[64] = {0};
  bool runtime_write = false;
  bool have_op_all = false;
  uint8_t op_all[12] = {0};
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    if (httpd_query_key_value(query, "op_all", op_all_text,
                              sizeof(op_all_text)) == ESP_OK) {
      int count = 0;
      char *cursor = op_all_text;
      while (count < 12) {
        char *end = nullptr;
        long value = strtol(cursor, &end, 10);
        if (end == cursor || value < 0 || value > UINT8_MAX) {
          break;
        }
        op_all[count++] = static_cast<uint8_t>(value);
        if (*end == '\0') break;
        if (*end != ',') break;
        cursor = end + 1;
      }
      if (count != 12) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(
            req,
            "{\"ok\":false,\"error\":\"op_all needs 12 comma-separated bytes\"}");
      }
      have_op_all = true;
    }
    if (httpd_query_key_value(query, "enable", enable_text,
                              sizeof(enable_text)) == ESP_OK) {
      if (strcmp(enable_text, "0") != 0 && strcmp(enable_text, "1") != 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(
            req, "{\"ok\":false,\"error\":\"enable must be 0 or 1\"}");
      }
      runtime_write = true;
      if (!pipecat_xvf_set_packed_mode(enable_text[0] == '1',
                                       have_op_all ? op_all : nullptr,
                                       &status)) {
        httpd_resp_set_status(req, "422 Unprocessable Entity");
        return httpd_resp_sendstr(
            req,
            "{\"ok\":false,\"error\":\"packed write/readback rejected\"}");
      }
    } else if (have_op_all) {
      httpd_resp_set_status(req, "400 Bad Request");
      return httpd_resp_sendstr(
          req, "{\"ok\":false,\"error\":\"op_all requires enable\"}");
    }
  }
  if (!runtime_write && pipecat_xvf_packed_status(&status) != ESP_OK) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    return httpd_resp_sendstr(
        req, "{\"ok\":false,\"error\":\"packed readback failed\"}");
  }
  char body[224];
  snprintf(body, sizeof(body),
           "{\"ok\":true,\"packed\":[%u,%u],"
           "\"op_all\":[%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u],"
           "\"runtime_write\":%s}",
           status.packed_l, status.packed_r, status.op_all[0],
           status.op_all[1], status.op_all[2], status.op_all[3],
           status.op_all[4], status.op_all[5], status.op_all[6],
           status.op_all[7], status.op_all[8], status.op_all[9],
           status.op_all[10], status.op_all[11],
           runtime_write ? "true" : "false");
  return httpd_resp_sendstr(req, body);
}

// GET /xvf/raw-capture?ms=N — stream N ms (<=15000) of RAW 48 kHz/32-bit
// stereo I2S frames as application/octet-stream (interleaved int32 LE,
// L0,R0,L1,R1,...). LSB packing markers are preserved, so this is the
// packed-mode capture path AND the full-precision cat-3/11 alignment path.
// The uplink publisher is paused (silence frames keep RTP alive) while
// this runs. Chunked; a dropped client aborts the capture cleanly.
static bool raw_capture_sink(const uint8_t *chunk, size_t len, void *ctx) {
  httpd_req_t *req = static_cast<httpd_req_t *>(ctx);
  return httpd_resp_send_chunk(req, reinterpret_cast<const char *>(chunk),
                               static_cast<ssize_t>(len)) == ESP_OK;
}

static esp_err_t xvf_raw_capture_handler(httpd_req_t *req) {
  char query[32] = {0};
  char ms_text[8] = {0};
  long ms = 0;
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
      httpd_query_key_value(query, "ms", ms_text, sizeof(ms_text)) == ESP_OK) {
    char *end = nullptr;
    ms = strtol(ms_text, &end, 10);
    if (end == ms_text || *end != '\0') ms = 0;
  }
  if (ms <= 0 || ms > 15000) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_sendstr(
        req, "{\"ok\":false,\"error\":\"ms must be 1..15000\"}");
  }
  httpd_resp_set_type(req, "application/octet-stream");
  esp_err_t ret = pipecat_raw_i2s_capture(static_cast<uint32_t>(ms),
                                          raw_capture_sink, req);
  if (ret != ESP_OK) {
    ESP_LOGW(LOG_TAG, "raw capture aborted: %s", esp_err_to_name(ret));
  }
  // End chunked response (harmless if the client already vanished).
  httpd_resp_send_chunk(req, nullptr, 0);
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
extern volatile uint32_t g_nack_last_rtt_ms;
extern volatile uint32_t g_nack_max_rtt_ms;
extern volatile uint32_t g_nack_rtx_arrived;
extern volatile uint32_t g_rtvi_rx_total;
extern volatile uint32_t g_rtvi_rx_server_msg;
extern volatile uint32_t g_rtvi_rx_rtx;
extern volatile uint32_t g_rtvi_rx_parse_fail;
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
           "\"nack_last_rtt_ms\":%lu,\"nack_max_rtt_ms\":%lu,\"nack_rtx_arrived\":%lu,"
           "\"rtvi_rx_total\":%lu,\"rtvi_rx_server_msg\":%lu,\"rtvi_rx_rtx\":%lu,\"rtvi_rx_parse_fail\":%lu,"
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
           (unsigned long)g_nack_last_rtt_ms,
           (unsigned long)g_nack_max_rtt_ms,
           (unsigned long)g_nack_rtx_arrived,
           (unsigned long)g_rtvi_rx_total,
           (unsigned long)g_rtvi_rx_server_msg,
           (unsigned long)g_rtvi_rx_rtx,
           (unsigned long)g_rtvi_rx_parse_fail,
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
  config.max_uri_handlers = 10;
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
  // Persistent XVF/AIC tuning and its device-owned NVS introspection surface.
  httpd_uri_t tune_uri = {
      .uri = "/xvf/tune",
      .method = HTTP_POST,
      .handler = xvf_tune_handler,
      .user_ctx = NULL,
  };
  httpd_uri_t params_uri = {
      .uri = "/xvf/params",
      .method = HTTP_GET,
      .handler = xvf_params_handler,
      .user_ctx = NULL,
  };
  httpd_uri_t audio_mux_uri = {
      .uri = "/xvf/audio-mux",
      .method = HTTP_GET,
      .handler = xvf_audio_mux_handler,
      .user_ctx = NULL,
  };
  // Packed six-channel TDM mode + raw I2S bench capture (t_2ccb0829).
  httpd_uri_t packed_uri = {
      .uri = "/xvf/packed",
      .method = HTTP_GET,
      .handler = xvf_packed_handler,
      .user_ctx = NULL,
  };
  httpd_uri_t raw_capture_uri = {
      .uri = "/xvf/raw-capture",
      .method = HTTP_GET,
      .handler = xvf_raw_capture_handler,
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
  ESP_ERROR_CHECK(httpd_register_uri_handler(g_ota_server, &params_uri));
  ESP_ERROR_CHECK(httpd_register_uri_handler(g_ota_server, &audio_mux_uri));
  ESP_ERROR_CHECK(httpd_register_uri_handler(g_ota_server, &packed_uri));
  ESP_ERROR_CHECK(httpd_register_uri_handler(g_ota_server, &raw_capture_uri));
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
