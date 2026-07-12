#include <cJSON.h>
#include <esp_log.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef PIPECAT_NACK
#include <esp_timer.h>
#include <mbedtls/base64.h>

#include "nack_client.h"
#endif

#include "main.h"

#define MAX_TYPE_LEN 32
#define MAX_ID_LEN 64

static int rtvi_id = 0;

// rtx-delivery trace counters (2026-07-12): exposed via /playback/stats to
// find WHERE server->device messages die. Volatile: written on the RTVI task,
// read from the httpd task.
volatile uint32_t g_rtvi_rx_total = 0;       // every parsed data-channel msg
volatile uint32_t g_rtvi_rx_server_msg = 0;  // type == server-message
volatile uint32_t g_rtvi_rx_rtx = 0;         // ... with data.t == rtx
volatile uint32_t g_rtvi_rx_parse_fail = 0;  // cJSON_Parse failures
static QueueHandle_t rtvi_queue;
static PeerConnection *peer_connection = NULL;
static rtvi_callbacks_t *rtvi_callbacks = NULL;

typedef struct {
  cJSON *msg;
} rtvi_msg_t;

// Simple hashing function so we can fake pattern matching and switch on strings
// as a constexpr so it gets evaluated in compile time for static strings
static constexpr unsigned int hash(const char *s, int off = 0) {
  return !s[off] ? 5381 : (hash(s, off + 1) * 33) ^ s[off];
}

static rtvi_msg_t *create_rtvi_message(const char *type) {
  cJSON *j_msg = cJSON_CreateObject();

  if (j_msg == NULL) {
    ESP_LOGE(LOG_TAG, "Unable to create RTVI message");
    return NULL;
  }
  if (cJSON_AddStringToObject(j_msg, "label", "rtvi-ai") == NULL) {
    cJSON_Delete(j_msg);
    ESP_LOGE(LOG_TAG, "Unable to create RTVI message");
    return NULL;
  }
  if (cJSON_AddStringToObject(j_msg, "type", type) == NULL) {
    cJSON_Delete(j_msg);
    ESP_LOGE(LOG_TAG, "Unable to create RTVI message");
    return NULL;
  }

  char id[MAX_ID_LEN];
  sprintf(id, "%d", rtvi_id++);
  if (cJSON_AddStringToObject(j_msg, "id", id) == NULL) {
    cJSON_Delete(j_msg);
    ESP_LOGE(LOG_TAG, "Unable to create RTVI message");
    return NULL;
  }

  rtvi_msg_t *msg = (rtvi_msg_t *)malloc(sizeof(rtvi_msg_t));
  msg->msg = j_msg;

  return msg;
}

static void destroy_rtvi_message(rtvi_msg_t *msg) {
  cJSON_Delete(msg->msg);
  free(msg);
}

static char *rtvi_message_to_string(rtvi_msg_t *msg) {
  if (msg == NULL || msg->msg == NULL) {
    return NULL;
  }

  char *msg_str = cJSON_Print(msg->msg);

  return msg_str;
}

static void rtvi_handle_message(const rtvi_msg_t *msg) {
  g_rtvi_rx_total++;
  cJSON *j_type = cJSON_GetObjectItem(msg->msg, "type");
  if (j_type == NULL) {
    ESP_LOGE(LOG_TAG, "Unable to find `type` field in RTVI message");
    return;
  }

  switch (hash(j_type->valuestring)) {
    case hash("bot-started-speaking"):
      rtvi_callbacks->on_bot_started_speaking();
      break;
    case hash("bot-stopped-speaking"):
      rtvi_callbacks->on_bot_stopped_speaking();
      break;
    case hash("bot-tts-text"): {
      cJSON *j_data = cJSON_GetObjectItem(msg->msg, "data");
      cJSON *j_text = cJSON_GetObjectItem(j_data, "text");
      rtvi_callbacks->on_bot_tts_text(j_text->valuestring);
      break;
    }
    case hash("server-message"): {
      g_rtvi_rx_server_msg++;
      // App-specific message from webrtc_server.py. We use it to drive the LED
      // ring phase: {"data":{"t":"led","phase":"waiting|thinking|speaking|idle"}}
      ESP_LOGI(LOG_TAG, "RTVI server-message received");
      cJSON *j_data = cJSON_GetObjectItem(msg->msg, "data");
      if (j_data == NULL) break;
      cJSON *j_t = cJSON_GetObjectItem(j_data, "t");
      if (j_t == NULL || j_t->valuestring == NULL) break;
      if (hash(j_t->valuestring) == hash("led")) {
        cJSON *j_phase = cJSON_GetObjectItem(j_data, "phase");
        if (j_phase == NULL || j_phase->valuestring == NULL) break;
        switch (hash(j_phase->valuestring)) {
          case hash("idle"):
            pipecat_led_set_phase(PIPECAT_LED_PHASE_IDLE);
            break;
          case hash("waiting"):
            pipecat_led_set_phase(PIPECAT_LED_PHASE_WAITING);
            break;
          case hash("thinking"):
            pipecat_led_set_phase(PIPECAT_LED_PHASE_THINKING);
            break;
          case hash("speaking"):
            pipecat_led_set_phase(PIPECAT_LED_PHASE_SPEAKING);
            break;
          default:
            break;
        }
      }
#ifdef PIPECAT_NACK
      // NACK retransmit reply (audio-resilience ladder Phase 5, server
      // nack_retransmit.py): {"data":{"t":"rtx","seq":<u16>,
      // "payload_b64":"<base64 opus>"}} — one message per re-sent seq.
      // Decode the opus bytes (stack buffer; frames are <400B) and offer them
      // to the vendored rtp.c, which validates the 20ms window and stages
      // in-window frames for the decode task (never touches opus from this
      // task — see the threading note in rtp.c). Late/unknown seqs are
      // counted and dropped there.
      else if (hash(j_t->valuestring) == hash("rtx")) {
        g_rtvi_rx_rtx++;
        cJSON *j_seq = cJSON_GetObjectItem(j_data, "seq");
        cJSON *j_b64 = cJSON_GetObjectItem(j_data, "payload_b64");
        if (!cJSON_IsNumber(j_seq) || j_b64 == NULL ||
            j_b64->valuestring == NULL) {
          break;
        }
        // RED-wrapped rtx = primary + up to 2 redundant blocks + headers
        // (~3x a bare opus frame). 512 truncated 7/11 real rtx (measured
        // 2026-07-12: rtvi_rx_rtx=11 vs nack_rtx_arrived=4).
        unsigned char opus[1024];
        size_t opus_len = 0;
        if (mbedtls_base64_decode(opus, sizeof(opus), &opus_len,
                                  (const unsigned char *)j_b64->valuestring,
                                  strlen(j_b64->valuestring)) != 0 ||
            opus_len == 0) {
          break;  // oversized/corrupt payload — drop it
        }
        rtp_nack_feed_rtx((uint16_t)j_seq->valueint, opus, opus_len,
                          (uint32_t)(esp_timer_get_time() / 1000));
      }
#endif
      break;
    }
    default:
      break;
  }
}

static void rtvi_task(void *pvParameter) {
  rtvi_msg_t msg;

  while (1) {
    if (xQueueReceive(rtvi_queue, &msg, portMAX_DELAY)) {
      rtvi_handle_message(&msg);
      cJSON_Delete(msg.msg);
    }
  }
}

void pipecat_init_rtvi(PeerConnection *connection,
                       rtvi_callbacks_t *callbacks) {
  peer_connection = connection;
  rtvi_callbacks = callbacks;

  rtvi_queue = xQueueCreate(10, sizeof(rtvi_msg_t));
  xTaskCreatePinnedToCore(rtvi_task, "RTVI Task", 4096, NULL, 2, NULL, 1);
}

void pipecat_rtvi_send_client_ready() {
  rtvi_msg_t *msg = create_rtvi_message("client-ready");

  char *msg_str = rtvi_message_to_string(msg);

  peer_connection_datachannel_send(peer_connection, msg_str, strlen(msg_str));

  cJSON_free(msg_str);

  destroy_rtvi_message(msg);
}

void pipecat_rtvi_handle_message(const char *msg) {
  cJSON *j_msg = cJSON_Parse(msg);
  if (j_msg == NULL) {
    g_rtvi_rx_parse_fail++;
    ESP_LOGE(LOG_TAG, "Error parsing RTVI message");
    return;
  }

  rtvi_msg_t rtvi_msg = {.msg = j_msg};

  xQueueSend(rtvi_queue, &rtvi_msg, portMAX_DELAY);
}
