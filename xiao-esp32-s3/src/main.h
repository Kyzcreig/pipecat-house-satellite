#include <peer.h>

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
extern esp_err_t pipecat_xvf_tune(const char *param, float value);

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
extern void pipecat_rtvi_handle_message(const char* msg);

// Screen
extern void pipecat_init_screen();
extern void pipecat_screen_system_log(const char *text);
extern void pipecat_screen_new_log();
extern void pipecat_screen_log(const char *text);
