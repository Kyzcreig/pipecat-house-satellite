#include <peer.h>

#include <stddef.h>

#include "esp_err.h"
#include "pipecat_build_config.h"

#define LOG_TAG "pipecat"
#define MAX_HTTP_OUTPUT_BUFFER 4096
#define HTTP_TIMEOUT_MS 10000
#define TICK_INTERVAL 15

// Wifi
extern void pipecat_init_wifi();
extern bool pipecat_wifi_connected();

// WebRTC / Media
extern void pipecat_init_audio_capture();
extern void pipecat_init_audio_decoder();
extern void pipecat_init_audio_encoder();
extern void pipecat_send_audio(PeerConnection *peer_connection);
extern void pipecat_led_task(void *arg);
extern void pipecat_audio_decode(uint8_t *data, size_t size);
extern void pipecat_reset_audio_decoder();
extern bool pipecat_xvf3800_present();
// XVF application firmware version (DFU controller resid 240 cmd 88, e.g.
// "1.0.7"), or "unknown" if the boot-time read failed. Stable; safe to cache.
extern const char *pipecat_xvf3800_version();
struct PipecatXvfTuneResult {
  float requested;
  float applied_value;
  float readback;
  bool readback_valid;
  bool applied;
  bool clamped;
  bool ack_only;
};
extern esp_err_t pipecat_xvf_tune(const char *param, float value,
                                  PipecatXvfTuneResult *result);
extern bool pipecat_xvf_param_persistent(const char *param);
extern size_t pipecat_xvf_persistent_param_count();
extern const char *pipecat_xvf_persistent_param_name(size_t index);
extern bool pipecat_xvf_param_default(const char *param, float *value);
extern void pipecat_replay_xvf_params();
struct PipecatXvfAudioMuxStatus {
  uint8_t op_l_category;
  uint8_t op_l_source;
  uint8_t op_r_category;
  uint8_t op_r_source;
  uint8_t upsample_l;
  uint8_t upsample_r;
};
extern esp_err_t pipecat_xvf_audio_mux_status(
    PipecatXvfAudioMuxStatus *status);
extern bool pipecat_xvf_set_audio_mux_right(
    uint8_t category, uint8_t source, PipecatXvfAudioMuxStatus *status);
// Packed six-channel mode (t_2ccb0829): op_all is L_PK0..2,R_PK0..2 as six
// [category,source] pairs; OP_ALL is written first, then OP_PACKED <f>,<f>.
struct PipecatXvfPackedStatus {
  uint8_t packed_l;
  uint8_t packed_r;
  uint8_t op_all[12];
};
extern esp_err_t pipecat_xvf_packed_status(PipecatXvfPackedStatus *status);
extern bool pipecat_xvf_set_packed_mode(bool enable, const uint8_t op_all[12],
                                        PipecatXvfPackedStatus *status);
// Raw 48 kHz/32-bit stereo I2S bench capture (LSB packing markers intact).
// Pauses the uplink publisher (silence frames) while active; <=15 s.
extern volatile bool g_raw_capture_pause;
extern esp_err_t pipecat_raw_i2s_capture(
    uint32_t duration_ms,
    bool (*sink)(const uint8_t *chunk, size_t len, void *ctx), void *ctx);

// OTA / mDNS
extern void pipecat_init_mdns();
extern void pipecat_init_ota_server();
extern void pipecat_start_ota_validation_watchdog();
extern void pipecat_validate_ota_if_healthy();
extern bool pipecat_mdns_started();
extern bool pipecat_ota_server_started();

// WebRTC / Signalling
extern void pipecat_init_webrtc();
extern void pipecat_webrtc_loop();
extern volatile bool pipecat_webrtc_connected;
extern void pipecat_webrtc_note_server_ping();
extern bool pipecat_webrtc_server_heartbeat_fresh();
extern void pipecat_http_request(char *offer, char *answer);

// RTVI
typedef struct {
  void (*on_bot_started_speaking)();
  void (*on_bot_stopped_speaking)();
  void (*on_bot_tts_text)(const char *text);
} rtvi_callbacks_t;

extern rtvi_callbacks_t pipecat_rtvi_callbacks;

// LED phase (Stage 2b): server -> device authoritative voice-assistant state.
// Values MUST match the ServerPhase enum in media.cpp.
enum PipecatLedPhase {
  PIPECAT_LED_PHASE_NONE = 0,
  PIPECAT_LED_PHASE_IDLE = 1,
  PIPECAT_LED_PHASE_WAITING = 2,   // wake fired
  PIPECAT_LED_PHASE_THINKING = 3,  // processing
  PIPECAT_LED_PHASE_SPEAKING = 4,  // replying
};
extern "C" void pipecat_led_set_phase(int phase);

extern void pipecat_init_rtvi(PeerConnection *peer_connection, rtvi_callbacks_t *callbacks);
extern void pipecat_rtvi_send_client_ready();
extern bool pipecat_rtvi_handle_heartbeat(const char* msg, uint16_t sid);
extern void pipecat_rtvi_send_pending_heartbeat();
extern void pipecat_rtvi_handle_message(const char* msg);

// Screen
extern void pipecat_init_screen();
extern void pipecat_screen_system_log(const char *text);
extern void pipecat_screen_new_log();
extern void pipecat_screen_log(const char *text);
