#ifndef LINUX_BUILD
#include <driver/i2s_std.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <opus.h>
#endif

#include <esp_event.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <string.h>

#include "peer_connection.h"
#include "main.h"

#ifdef PIPECAT_NACK
#include <esp_timer.h>

#include "nack_client.h"
#endif

static PeerConnection *peer_connection = NULL;
#define RTVI_MESSAGE_CAP 2048u

volatile uint32_t g_rtx_malformed = 0;

#ifdef PIPECAT_NACK
static uint16_t s_nack_rtx_sid = UINT16_MAX;
static bool s_nack_rtx_armed = false;
static bool s_nack_rtx_bad_type_logged = false;
#define NACK_RTX_OPEN_TIMEOUT_MS 5000u
static uint32_t s_nack_rtx_open_deadline_ms = 0;
static bool s_nack_rtx_setup_started = false;
static bool s_nack_rtx_timeout_logged = false;
static_assert(DATA_CHANNEL_PARTIAL_RELIABLE_REXMIT_UNORDERED ==
                  DCEP_CHANNEL_TYPE,
              "generated NACK DCEP type must remain 0x81");

static void pipecat_nack_datachannel_send(const char *json, size_t len) {
  // Requests deliberately remain on reliable RTVI SID 0; only server->device
  // rtx frames use the unordered/unreliable binary channel.
  peer_connection_datachannel_send_sid(peer_connection, (char *)json, len, 0);
}

static void pipecat_nack_try_arm_channel(void) {
  if (s_nack_rtx_armed) {
    return;
  }
  if (!s_nack_rtx_setup_started) {
    return;
  }
  uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
  if ((int32_t)(now_ms - s_nack_rtx_open_deadline_ms) > 0) {
    if (!s_nack_rtx_timeout_logged) {
      s_nack_rtx_timeout_logged = true;
      ESP_LOGW(LOG_TAG,
               "NACK-v2 stays dark: pipecat-rtx not valid within 5000ms");
    }
    return;
  }
  uint16_t sid = 0;
  uint8_t channel_type = 0;
  uint32_t reliability_parameter = UINT32_MAX;
  if (peer_connection_lookup_datachannel(
          peer_connection, "pipecat-rtx", &sid, &channel_type,
          &reliability_parameter) != 0) {
    return;
  }
  if (channel_type != DCEP_CHANNEL_TYPE ||
      !nack_client_accepts_reliability(reliability_parameter)) {
    if (!s_nack_rtx_bad_type_logged) {
      s_nack_rtx_bad_type_logged = true;
      ESP_LOGE(LOG_TAG,
               "NACK-v2 stays dark: pipecat-rtx sid=%u type=0x%02x rel=%lu",
               (unsigned)sid, (unsigned)channel_type,
               (unsigned long)reliability_parameter);
    }
    return;
  }
  s_nack_rtx_sid = sid;
  s_nack_rtx_armed = true;
  rtp_nack_register_sender(pipecat_nack_datachannel_send);
  ESP_LOGI(LOG_TAG, "NACK-v2 armed: pipecat-rtx sid=%u type=0x81 rel=%lu",
           (unsigned)sid, (unsigned long)reliability_parameter);
}

static void pipecat_nack_handle_rtx_frame(const char *msg, size_t len) {
  uint16_t seq = 0;
  const uint8_t *payload = NULL;
  size_t payload_len = 0;
  if (!nack_parse_rtx_frame((const uint8_t *)msg, len, &seq, &payload,
                            &payload_len)) {
    g_rtx_malformed++;
    return;
  }
  rtp_nack_feed_rtx(seq, payload, payload_len,
                    (uint32_t)(esp_timer_get_time() / 1000));
}
#endif

// Persistent connection watchdog inputs. Peer state alone is insufficient: a
// server-side eviction can leave the local ICE/SCTP stack half-open without a
// state callback. The server heartbeat timestamp detects that silent wedge.
volatile bool pipecat_webrtc_connected = false;
static volatile uint32_t last_server_ping_ms = 0;
static constexpr uint32_t WEBRTC_SERVER_HEARTBEAT_STALE_MS = 35000;

void pipecat_webrtc_note_server_ping() {
  last_server_ping_ms = (uint32_t)(esp_timer_get_time() / 1000);
}

bool pipecat_webrtc_server_heartbeat_fresh() {
  const uint32_t last_ping_ms = last_server_ping_ms;
  if (last_ping_ms == 0) return false;
  const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
  return (uint32_t)(now_ms - last_ping_ms) <=
         WEBRTC_SERVER_HEARTBEAT_STALE_MS;
}

#ifndef LINUX_BUILD
StaticTask_t task_buffer;
void pipecat_send_audio_task(void *user_data) {
  pipecat_init_audio_encoder();
  TickType_t next_frame_at = xTaskGetTickCount();

  while (1) {
    pipecat_send_audio(peer_connection);
    // Pace to absolute 20 ms frame deadlines. A fixed post-processing sleep
    // under-produced RTP media by 4.85%; a tight yield loop starved the
    // lower-priority peer/data-channel task and lost heartbeat replies.
    vTaskDelayUntil(&next_frame_at, pdMS_TO_TICKS(20));
  }
}
#endif

static void pipecat_ondatachannel_onmessage_task(char *msg, size_t len,
                                                 void *userdata, uint16_t sid) {
#ifdef PIPECAT_NACK
  char *label = peer_connection_lookup_sid_label(peer_connection, sid);
  if (label != NULL && strcmp(label, "pipecat-rtx") == 0) {
    pipecat_nack_try_arm_channel();
    if (s_nack_rtx_armed && sid == s_nack_rtx_sid) {
      pipecat_nack_handle_rtx_frame(msg, len);
    }
    return;  // binary data never reaches the JSON RTVI parser
  }
#endif
#ifdef LOG_DATACHANNEL_MESSAGES
  ESP_LOGI(LOG_TAG, "DataChannel Message: %.*s", (int)len, msg);
#endif
  if (len > RTVI_MESSAGE_CAP) {
    ESP_LOGW(LOG_TAG, "Dropping oversized RTVI message: %uB", (unsigned)len);
    return;
  }
  char rtvi_message[RTVI_MESSAGE_CAP + 1];
  memcpy(rtvi_message, msg, len);
  rtvi_message[len] = '\0';
  if (pipecat_rtvi_handle_heartbeat(rtvi_message, sid)) return;
  pipecat_rtvi_handle_message(rtvi_message);
}

static void pipecat_ondatachannel_onopen_task(void *userdata) {
#ifdef PIPECAT_NACK
  s_nack_rtx_setup_started = true;
  s_nack_rtx_open_deadline_ms =
      (uint32_t)(esp_timer_get_time() / 1000) + NACK_RTX_OPEN_TIMEOUT_MS;
#endif
  if (peer_connection_create_datachannel(peer_connection, DATA_CHANNEL_RELIABLE,
                                         0, 0, (char *)"rtvi-ai",
                                         (char *)"") != -1) {
    ESP_LOGI(LOG_TAG, "DataChannel created");
  } else {
    ESP_LOGE(LOG_TAG, "Failed to create DataChannel");
  }
}

static void pipecat_onconnectionstatechange_task(PeerConnectionState state,
                                                 void *user_data) {
  ESP_LOGI(LOG_TAG, "PeerConnectionState: %s",
           peer_connection_state_to_string(state));

  if (state == PEER_CONNECTION_DISCONNECTED ||
      state == PEER_CONNECTION_CLOSED ||
      state == PEER_CONNECTION_FAILED) {
    pipecat_webrtc_connected = false;
#ifndef LINUX_BUILD
    ESP_LOGW(LOG_TAG, "Peer connection lost (%s); reconnect watchdog armed",
             peer_connection_state_to_string(state));
#endif
  } else if (state == PEER_CONNECTION_CONNECTED) {
#ifndef LINUX_BUILD
    pipecat_webrtc_connected = true;
    StackType_t *stack_memory = (StackType_t *)heap_caps_malloc(
        30000 * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
    xTaskCreateStaticPinnedToCore(pipecat_send_audio_task, "audio_publisher",
                                  30000, NULL, 7, stack_memory, &task_buffer,
                                  0);
    // LED ring task: owns ALL XVF control-I2C for the ring (state decision,
    // beam telemetry read, 48-byte ring write) on core 1 at low priority, so a
    // slow/contended XVF control transaction can never stall the audio
    // publisher (core 0, prio 7) and cause RTP "no audio frame" churn. Created
    // once (survives peer reconnects; it reads pipecat_webrtc_connected).
    static bool led_task_started = false;
    if (!led_task_started) {
      led_task_started = true;
      xTaskCreatePinnedToCore(pipecat_led_task, "led_ring", 4096, NULL, 2, NULL,
                              1);
    }
    pipecat_init_rtvi(peer_connection, &pipecat_rtvi_callbacks);
#endif
  } else if (state == PEER_CONNECTION_COMPLETED) {
    pipecat_webrtc_connected = true;
  } else {
    pipecat_webrtc_connected = false;
  }
}

static void pipecat_on_icecandidate_task(char *description, void *user_data) {
  char *local_buffer = (char *)malloc(MAX_HTTP_OUTPUT_BUFFER + 1);
  memset(local_buffer, 0, MAX_HTTP_OUTPUT_BUFFER + 1);
  pipecat_http_request(description, local_buffer);
  if (local_buffer[0] != '\0') {
    peer_connection_set_remote_description(peer_connection, local_buffer,
                                           SDP_TYPE_ANSWER);
  } else {
    ESP_LOGW(LOG_TAG, "No WebRTC answer available; OTA server remains online");
  }
  free(local_buffer);
}

void pipecat_init_webrtc() {
  PeerConfiguration peer_connection_config = {
      .ice_servers = {},
      .audio_codec = CODEC_OPUS,
      .video_codec = CODEC_NONE,
      .datachannel = DATA_CHANNEL_STRING,
      .onaudiotrack = [](uint8_t *data, size_t size, void *userdata) -> void {
#ifndef LINUX_BUILD
        pipecat_audio_decode(data, size);
#endif
      },
      .onvideotrack = NULL,
      .on_request_keyframe = NULL,
      .user_data = NULL,
  };

  peer_connection = peer_connection_create(&peer_connection_config);
  if (peer_connection == NULL) {
    ESP_LOGE(LOG_TAG, "Failed to create peer connection");
#ifndef LINUX_BUILD
    esp_restart();
#endif
  }

  peer_connection_oniceconnectionstatechange(
      peer_connection, pipecat_onconnectionstatechange_task);
  peer_connection_onicecandidate(peer_connection, pipecat_on_icecandidate_task);
  peer_connection_ondatachannel(peer_connection,
                                pipecat_ondatachannel_onmessage_task,
                                pipecat_ondatachannel_onopen_task, NULL);

  peer_connection_create_offer(peer_connection);
}

void pipecat_webrtc_loop() {
  peer_connection_loop(peer_connection);
#ifdef PIPECAT_NACK
  // The server creates pipecat-rtx during initial setup. Poll the libpeer DCEP
  // stream table without blocking SCTP; arm only after byte 1 is exactly 0x81.
  pipecat_nack_try_arm_channel();
#endif
  pipecat_rtvi_send_pending_heartbeat();
}
