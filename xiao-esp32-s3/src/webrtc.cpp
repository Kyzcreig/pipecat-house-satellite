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

#include "main.h"
#include "peer_connection.h"
#include "server_liveness.h"

#ifdef PIPECAT_NACK
#include <esp_timer.h>

#include "nack_client.h"
#endif

#if PIPECAT_REDIAL
#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <stdio.h>

#include "webrtc_redial.h"
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
  if (peer_connection_lookup_datachannel(peer_connection, "pipecat-rtx", &sid,
                                         &channel_type,
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
// 35 s = the hub's 30 s ping interval + 5 s jitter grace. On its own that was
// too tight: one ping delayed >5 s rebooted a device mid-TTS (kitchen
// 2026-10-04 02:40 / 03:04, t_56a17737). Downlink media now also counts as a
// sign of life -- see server_liveness.h.
static constexpr uint32_t WEBRTC_SERVER_HEARTBEAT_STALE_MS = 35000;
static PipecatServerLiveness s_server_liveness(
    WEBRTC_SERVER_HEARTBEAT_STALE_MS);

static inline uint32_t pipecat_now_ms() {
  return (uint32_t)(esp_timer_get_time() / 1000);
}

void pipecat_webrtc_note_server_ping() {
  s_server_liveness.note_ping(pipecat_now_ms());
}

void pipecat_webrtc_note_server_media() {
  s_server_liveness.note_media(pipecat_now_ms());
}

bool pipecat_webrtc_server_heartbeat_fresh() {
  return s_server_liveness.fresh(pipecat_now_ms());
}

void pipecat_webrtc_server_liveness_stats(uint32_t *ping_rx,
                                          uint32_t *ping_gap_max_ms,
                                          uint32_t *ping_age_ms,
                                          uint32_t *media_holds) {
  *ping_rx = s_server_liveness.ping_rx;
  *ping_gap_max_ms = s_server_liveness.ping_gap_max_ms;
  *ping_age_ms = s_server_liveness.ping_age_ms(pipecat_now_ms());
  *media_holds = s_server_liveness.media_holds;
}

#ifndef LINUX_BUILD
// audio_publisher stack, in BYTES (ESP-IDF StackType_t is uint8_t). It runs
// opus_encode() with USE_ALLOCA, so the stack holds the encoder's scratch.
// t_6ec97d6b: the old 30000 B covered SILK (host-measured 22,800 B per encode,
// 16 kHz stereo) but not CELT (33,360 B at 64k/lane with FEC off), so codec
// arm C overflowed on its first frame after CONNECTED and panicked every
// boot. 64 KiB lives in PSRAM; /playback/stats reports the live high-water.
#define AUDIO_PUBLISHER_STACK_BYTES (64 * 1024)
StaticTask_t task_buffer;
static TaskHandle_t s_audio_publisher_task = nullptr;

static volatile uint32_t s_uplink_frames_sent = 0;

// Publisher <-> transport-loop handoff (PIPECAT_REDIAL, t_db77e56b). The
// publisher runs on core 0 at prio 7 and calls into the PeerConnection every
// 20 ms; a re-dial destroys that object from the prio-8 transport loop. The
// publisher reads its target through s_publisher_pc once per frame and
// brackets the call with s_publisher_in_send, so the teardown can park the
// pointer at NULL and wait for the in-flight frame to finish (SEQ_CST on both
// sides: if the publisher loaded the old pointer, its in_send=true is ordered
// before that load and is still visible to the teardown's poll). With the
// flag off the pointer is written exactly once, on first CONNECTED.
static PeerConnection *s_publisher_pc = NULL;
static bool s_publisher_in_send = false;

uint32_t pipecat_uplink_frames_sent() {
  return s_uplink_frames_sent;
}

uint32_t pipecat_audio_publisher_stack_free() {
  TaskHandle_t task = s_audio_publisher_task;
  return task ? (uint32_t)uxTaskGetStackHighWaterMark(task) : 0;
}

void pipecat_send_audio_task(void *user_data) {
  pipecat_init_audio_encoder();
  TickType_t next_frame_at = xTaskGetTickCount();

  while (1) {
    __atomic_store_n(&s_publisher_in_send, true, __ATOMIC_SEQ_CST);
    PeerConnection *peer_connection =
        __atomic_load_n(&s_publisher_pc, __ATOMIC_SEQ_CST);
    // NULL between re-dials: media.cpp keeps draining I2S and encoding so
    // the mic ring and the encoder stay warm; only the RTP send is skipped.
    pipecat_send_audio(peer_connection);
    __atomic_store_n(&s_publisher_in_send, false, __ATOMIC_SEQ_CST);
    s_uplink_frames_sent = s_uplink_frames_sent + 1;
    // Pace to absolute 20 ms frame deadlines. A fixed post-processing sleep
    // under-produced RTP media by 4.85%; a tight yield loop starved the
    // lower-priority peer/data-channel task and lost heartbeat replies.
    vTaskDelayUntil(&next_frame_at, pdMS_TO_TICKS(20));
  }
}

static void pipecat_audio_publisher_attach(PeerConnection *pc) {
  __atomic_store_n(&s_publisher_pc, pc, __ATOMIC_SEQ_CST);
  if (s_audio_publisher_task != nullptr)
    return;  // re-dial: the task survives; it just picks up the new target
  StackType_t *stack_memory = (StackType_t *)heap_caps_malloc(
      AUDIO_PUBLISHER_STACK_BYTES * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
  s_audio_publisher_task = xTaskCreateStaticPinnedToCore(
      pipecat_send_audio_task, "audio_publisher", AUDIO_PUBLISHER_STACK_BYTES,
      NULL, 7, stack_memory, &task_buffer, 0);
}

#if PIPECAT_REDIAL
// Park the publisher off the PeerConnection and wait for any in-flight frame.
// Bounded: one frame is an i2s read (<=200 ms timeout) + encode + send.
static void pipecat_audio_publisher_detach() {
  __atomic_store_n(&s_publisher_pc, (PeerConnection *)NULL, __ATOMIC_SEQ_CST);
  for (int waited_ms = 0; waited_ms < 500; waited_ms += 5) {
    if (!__atomic_load_n(&s_publisher_in_send, __ATOMIC_SEQ_CST))
      return;
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  ESP_LOGW(LOG_TAG, "redial: audio publisher still in send after 500 ms");
}
#endif
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
  if (pipecat_rtvi_handle_heartbeat(rtvi_message, sid))
    return;
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

#if PIPECAT_REDIAL
// Re-dial state. Written from libpeer callbacks (transport-loop context) and
// the httpd task (POST /webrtc/redial); consumed by pipecat_webrtc_loop() on
// the transport loop AFTER peer_connection_loop() returns, never inside a
// callback (the callback runs on the object we are about to destroy).
static PipecatRedialTrigger s_redial_requested = PipecatRedialTrigger::kNone;
static PipecatRedialPolicy s_redial_policy;
static bool s_offer_answered = false;
static uint32_t s_offer_sent_ms = 0;        // 0 = no attempt in flight
static uint32_t s_connected_at_ms = 0;      // 0 = not connected
static uint32_t s_next_attempt_at_ms = 0;   // valid while peer_connection==NULL
static uint32_t s_redials_total = 0;        // trigger-driven teardown+re-offer
static uint32_t s_attempts_total = 0;       // dials incl. ladder retries
static uint32_t s_redial_connects = 0;      // CONNECTED edges after a re-dial
static uint32_t s_peer_generation = 0;      // 1 = boot connection
static uint32_t s_last_redial_ms = 0;
static uint32_t s_last_redial_to_connected_ms = 0;
static PipecatRedialTrigger s_last_trigger = PipecatRedialTrigger::kNone;

static inline uint32_t redial_now_ms() {
  return (uint32_t)(esp_timer_get_time() / 1000);
}

static void pipecat_redial_request(PipecatRedialTrigger why) {
  PipecatRedialTrigger none = PipecatRedialTrigger::kNone;
  // First request wins; later ones until the loop consumes it are the same
  // event seen through another layer (SCTP ABORT then DTLS close_notify).
  __atomic_compare_exchange_n(&s_redial_requested, &none, why, false,
                              __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

void pipecat_webrtc_request_redial() {
  pipecat_redial_request(PipecatRedialTrigger::kHttp);
}

static void pipecat_ondatachannel_onclose_task(void *userdata) {
  // SCTP ABORT/SHUTDOWN from the hub: pc.close() on a graceful shutdown or
  // a PEER_LIVENESS_DROP eviction. Today this is silent and the device waits
  // for the heartbeat to go stale (~35 s) before rebooting.
  ESP_LOGW(LOG_TAG, "redial: SCTP association closed by the hub");
  pipecat_redial_request(PipecatRedialTrigger::kSctpClosed);
}
#endif

static void pipecat_onconnectionstatechange_task(PeerConnectionState state,
                                                 void *user_data) {
  ESP_LOGI(LOG_TAG, "PeerConnectionState: %s",
           peer_connection_state_to_string(state));

  if (state == PEER_CONNECTION_DISCONNECTED ||
      state == PEER_CONNECTION_CLOSED || state == PEER_CONNECTION_FAILED) {
    pipecat_webrtc_connected = false;
#ifndef LINUX_BUILD
    ESP_LOGW(LOG_TAG, "Peer connection lost (%s); reconnect watchdog armed",
             peer_connection_state_to_string(state));
#endif
#if PIPECAT_REDIAL
    // libpeer never leaves these states on its own; waiting the watchdog's
    // 30 s grace only delays the re-offer.
    s_connected_at_ms = 0;
    pipecat_redial_request(PipecatRedialTrigger::kPeerClosed);
#endif
  } else if (state == PEER_CONNECTION_CONNECTED) {
#ifndef LINUX_BUILD
    pipecat_webrtc_connected = true;
#if PIPECAT_REDIAL
    s_connected_at_ms = redial_now_ms();
    s_offer_sent_ms = 0;  // attempt landed; connect-timeout disarmed
    if (s_peer_generation > 1) {
      s_redial_connects++;
      s_last_redial_to_connected_ms = s_connected_at_ms - s_last_redial_ms;
      ESP_LOGI(LOG_TAG, "redial: CONNECTED gen=%u after %u ms (trigger=%s)",
               (unsigned)s_peer_generation,
               (unsigned)s_last_redial_to_connected_ms,
               pipecat_redial_trigger_name(s_last_trigger));
    }
#endif
    pipecat_audio_publisher_attach(peer_connection);
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
#if PIPECAT_REDIAL
    s_offer_answered = true;
#endif
    peer_connection_set_remote_description(peer_connection, local_buffer,
                                           SDP_TYPE_ANSWER);
  } else {
    ESP_LOGW(LOG_TAG, "No WebRTC answer available; OTA server remains online");
  }
  free(local_buffer);
}

// Create the PeerConnection, wire the callbacks and POST the offer (blocking,
// HTTP_TIMEOUT_MS). Shared by the boot path and the re-dial path.
static bool pipecat_webrtc_dial() {
  PeerConfiguration peer_connection_config = {
      .ice_servers = {},
      .audio_codec = CODEC_OPUS,
      .video_codec = CODEC_NONE,
      .datachannel = DATA_CHANNEL_STRING,
      .onaudiotrack = [](uint8_t *data, size_t size, void *userdata) -> void {
        // Downlink media is a sign of server life (server_liveness.h).
        pipecat_webrtc_note_server_media();
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
    return false;
  }

  peer_connection_oniceconnectionstatechange(
      peer_connection, pipecat_onconnectionstatechange_task);
  peer_connection_onicecandidate(peer_connection, pipecat_on_icecandidate_task);
  peer_connection_ondatachannel(peer_connection,
                                pipecat_ondatachannel_onmessage_task,
                                pipecat_ondatachannel_onopen_task,
#if PIPECAT_REDIAL
                                pipecat_ondatachannel_onclose_task);
  s_peer_generation++;
  s_offer_answered = false;
  s_offer_sent_ms = redial_now_ms();
#else
                                NULL);
#endif

  peer_connection_create_offer(peer_connection);
  return true;
}

void pipecat_init_webrtc() {
  if (!pipecat_webrtc_dial()) {
#ifndef LINUX_BUILD
    esp_restart();
#endif
  }
#if PIPECAT_REDIAL
  if (!s_offer_answered)
    pipecat_webrtc_note_attempt_failed(PipecatRedialTrigger::kOfferFailed);
#endif
}

#if PIPECAT_REDIAL
// Tear the current PeerConnection down without touching XVF, I2S, LED, mdns
// or the OTA server. Transport-loop context only, never from a callback.
static void pipecat_webrtc_teardown() {
  pipecat_webrtc_connected = false;
  s_connected_at_ms = 0;
  s_offer_sent_ms = 0;
  pipecat_audio_publisher_detach();
  pipecat_rtvi_detach();
#ifdef PIPECAT_NACK
  s_nack_rtx_sid = UINT16_MAX;
  s_nack_rtx_armed = false;
  s_nack_rtx_bad_type_logged = false;
  s_nack_rtx_setup_started = false;
  s_nack_rtx_timeout_logged = false;
#endif
  PeerConnection *old = peer_connection;
  peer_connection = NULL;
  // sctp: in-struct state machine (CONFIG_USE_USRSCTP=0), nothing to free;
  // dtls: mbedtls ssl/conf/cert/pkey/drbg + srtp sessions freed;
  // agent: both UDP sockets closed; then the ~100 KiB struct itself.
  peer_connection_destroy(old);
}

// A dial attempt failed (no answer, or answered but never CONNECTED).
// Schedules the next attempt per the ladder; reboots after the cap.
void pipecat_webrtc_note_attempt_failed(PipecatRedialTrigger why) {
  const bool give_up = s_redial_policy.note_failure();
  ESP_LOGW(LOG_TAG, "redial: attempt failed (%s) consecutive=%u next in %u ms%s",
           pipecat_redial_trigger_name(why),
           (unsigned)s_redial_policy.consecutive_failures(),
           (unsigned)s_redial_policy.next_backoff_ms(),
           give_up ? " -> restart fallback" : "");
  if (give_up) {
    // Same software reset the reconnect watchdog issues today; boot_guard
    // classifies it kSoftware exactly as before.
    esp_restart();
  }
  if (peer_connection != NULL)
    pipecat_webrtc_teardown();
  s_next_attempt_at_ms = redial_now_ms() + s_redial_policy.next_backoff_ms();
}

// One dial attempt: tear down whatever is there, create + offer, account.
static void pipecat_webrtc_attempt() {
  if (peer_connection != NULL)
    pipecat_webrtc_teardown();
  s_attempts_total++;
  if (!pipecat_webrtc_dial()) {
    // Allocation failure: the heap is the thing a re-dial must not leak, so
    // this is exactly the case for the reboot fallback.
    ESP_LOGE(LOG_TAG, "redial: peer_connection_create failed; restarting");
    esp_restart();
  }
  if (!s_offer_answered)
    pipecat_webrtc_note_attempt_failed(PipecatRedialTrigger::kOfferFailed);
}

// A fresh trigger: stamp it (the measurement clock runs trigger -> CONNECTED,
// across however many ladder attempts that takes) and dial now.
static void pipecat_webrtc_redial(PipecatRedialTrigger why) {
  ESP_LOGW(LOG_TAG, "redial: trigger=%s gen=%u -> tearing down and re-offering",
           pipecat_redial_trigger_name(why), (unsigned)s_peer_generation);
  s_last_trigger = why;
  s_last_redial_ms = redial_now_ms();
  s_redials_total++;
  pipecat_webrtc_attempt();
}

// Called by the transport loop each tick, after peer_connection_loop().
// Returns true when a re-dial or a scheduled attempt ran this tick (the
// caller re-arms its reconnect watchdog so the new attempt gets fresh grace).
bool pipecat_webrtc_redial_tick(bool watchdog_expired) {
  const uint32_t now = redial_now_ms();
  PipecatRedialTrigger why =
      __atomic_exchange_n(&s_redial_requested, PipecatRedialTrigger::kNone,
                          __ATOMIC_SEQ_CST);
  if (why == PipecatRedialTrigger::kNone && watchdog_expired)
    why = PipecatRedialTrigger::kWatchdog;

  if (peer_connection == NULL) {
    // Between attempts: triggers are moot, the ladder owns the clock.
    if ((int32_t)(now - s_next_attempt_at_ms) >= 0) {
      pipecat_webrtc_attempt();  // ladder retry of the stamped trigger
      return true;
    }
    return false;
  }
  if (why != PipecatRedialTrigger::kNone) {
    pipecat_webrtc_redial(why);
    return true;
  }
  if (s_connected_at_ms != 0) {
    s_redial_policy.note_connected_for(now - s_connected_at_ms);
  } else if (s_offer_sent_ms != 0 &&
             now - s_offer_sent_ms > PipecatRedialPolicy::kConnectTimeoutMs) {
    pipecat_webrtc_note_attempt_failed(PipecatRedialTrigger::kConnectTimeout);
    return true;
  }
  return false;
}

// POST /webrtc/redial -> queue a re-dial for the transport loop. Registered
// from here (not ota.cpp) so the OTA server's handler table stays the
// surface that file owns; the slot is reserved in pipecat_init_ota_server.
static esp_err_t webrtc_redial_handler(httpd_req_t *req) {
  pipecat_webrtc_request_redial();
  char body[96];
  snprintf(body, sizeof(body),
           "{\"ok\":true,\"queued\":\"http\",\"generation\":%u}",
           (unsigned)s_peer_generation);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, body);
}

esp_err_t pipecat_webrtc_register_http(httpd_handle_t server) {
  httpd_uri_t redial_uri = {
      .uri = "/webrtc/redial",
      .method = HTTP_POST,
      .handler = webrtc_redial_handler,
      .user_ctx = NULL,
  };
  return httpd_register_uri_handler(server, &redial_uri);
}

// `"redial":{...}` fragment for /ota/status. Heap fields are namespaced here
// (GADGET-1 owns the top-level rssi/heap fields on /playback/stats).
size_t pipecat_webrtc_redial_json(char *out, size_t capacity) {
  const uint32_t now = redial_now_ms();
  const char *state = peer_connection
                          ? peer_connection_state_to_string(
                                peer_connection_get_state(peer_connection))
                          : "none";
  int n = snprintf(
      out, capacity,
      "\"redial\":{\"enabled\":true,\"generation\":%u,\"peer_state\":\"%s\","
      "\"connected\":%s,\"connected_for_ms\":%u,\"redials_total\":%u,"
      "\"attempts_total\":%u,\"redial_connects\":%u,\"last_trigger\":\"%s\",\"last_redial_ms\":%u,"
      "\"last_redial_to_connected_ms\":%u,\"consecutive_failures\":%u,"
      "\"total_failures\":%u,\"next_attempt_in_ms\":%u,"
      "\"heap_free_int\":%u,\"heap_min_free_int\":%u,\"heap_largest_int\":%u,"
      "\"heap_largest_dma\":%u,\"heap_free_psram\":%u}",
      (unsigned)s_peer_generation, state,
      pipecat_webrtc_connected ? "true" : "false",
      (unsigned)(s_connected_at_ms ? now - s_connected_at_ms : 0),
      (unsigned)s_redials_total, (unsigned)s_attempts_total,
      (unsigned)s_redial_connects, pipecat_redial_trigger_name(s_last_trigger),
      (unsigned)s_last_redial_ms,
      (unsigned)s_last_redial_to_connected_ms,
      (unsigned)s_redial_policy.consecutive_failures(),
      (unsigned)s_redial_policy.total_failures(),
      (unsigned)(peer_connection == NULL &&
                         (int32_t)(s_next_attempt_at_ms - now) > 0
                     ? s_next_attempt_at_ms - now
                     : 0),
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
      (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  if (n < 0)
    return 0;
  return (size_t)n < capacity ? (size_t)n : capacity - 1;
}
#endif  // PIPECAT_REDIAL

void pipecat_webrtc_loop() {
#if PIPECAT_REDIAL
  if (peer_connection == NULL)
    return;  // between re-dial attempts; pipecat_webrtc_redial_tick owns it
#endif
  peer_connection_loop(peer_connection);
#ifdef PIPECAT_NACK
  // The server creates pipecat-rtx during initial setup. Poll the libpeer DCEP
  // stream table without blocking SCTP; arm only after byte 1 is exactly 0x81.
  pipecat_nack_try_arm_channel();
#endif
  pipecat_rtvi_send_pending_heartbeat();
}
