#include <esp_log.h>

#include "main.h"

// The server (ACE-AI webrtc_server.py) drives the LED ring phase over the RTVI
// data channel, exactly like the old ESPHome firmware used voice_assistant_phase.
// bot-started/stopped-speaking are standard RTVI events; the wake + thinking
// phases arrive as RTVI server-messages (see pipecat_led_on_server_message).

static void on_bot_started_speaking() {
  pipecat_led_set_phase(PIPECAT_LED_PHASE_SPEAKING);
}

static void on_bot_stopped_speaking() {
  // Bot finished replying -> back to idle (rainbow, waiting for wake).
  pipecat_led_set_phase(PIPECAT_LED_PHASE_IDLE);
}

static void on_bot_tts_text(const char *text) {
  // (unused) reply transcript; screen path is disabled on this board.
  (void)text;
}

rtvi_callbacks_t pipecat_rtvi_callbacks = {
    .on_bot_started_speaking = on_bot_started_speaking,
    .on_bot_stopped_speaking = on_bot_stopped_speaking,
    .on_bot_tts_text = on_bot_tts_text,
};
