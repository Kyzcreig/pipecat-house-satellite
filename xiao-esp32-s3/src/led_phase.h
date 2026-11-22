// Server-driven LED phase decision (pure, host-testable).
//
// The server sends a phase over RTVI; media.cpp's LED task asks this header
// which phase to render on each 50 ms tick. Values MUST match PipecatLedPhase
// in main.h (media.cpp static_asserts it).
//
// Rules:
//   - A phase is honoured for LED_PHASE_TTL_MS after it arrived, then the ring
//     falls back to device-local inference (NONE), so a dropped "idle" can't
//     stick forever.
//   - CONVERSATION is the exception: it holds until the server sends another
//     phase (conversation mode can stream the mic for minutes). media.cpp
//     clears it when the WebRTC peer drops.
//   - While in CONVERSATION, reply audio playing shows SPEAKING; when playback
//     stops the ring goes back to CONVERSATION, not to idle.
#ifndef PIPECAT_LED_PHASE_H
#define PIPECAT_LED_PHASE_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#define LED_PH_NONE 0
#define LED_PH_IDLE 1
#define LED_PH_WAITING 2
#define LED_PH_THINKING 3
#define LED_PH_SPEAKING 4
#define LED_PH_CONVERSATION 5

#define LED_PHASE_TTL_MS 30000u

// Breath period of the conversation ring, in 50 ms LED ticks (4 s).
#define LED_CONVERSATION_BREATH_TICKS 80u
// Floor of the breath, as a fraction of the brightness scalar.
#define LED_CONVERSATION_BREATH_MIN 0.35f

static inline int led_phase_effective(int phase, uint32_t age_ms,
                                      bool playing) {
  if (phase == LED_PH_NONE)
    return LED_PH_NONE;
  if (phase == LED_PH_CONVERSATION)
    return playing ? LED_PH_SPEAKING : LED_PH_CONVERSATION;
  if (age_ms >= LED_PHASE_TTL_MS)
    return LED_PH_NONE;
  return phase;
}

// Phase to store for a standard RTVI bot-started/stopped-speaking event. Those
// events write SPEAKING / IDLE; in conversation mode they must not replace the
// sticky CONVERSATION phase (speech already shows via is_playing), so the ring
// returns to white, not rainbow, when the reply ends.
static inline int led_phase_on_bot_event(int current, int requested) {
  return current == LED_PH_CONVERSATION ? LED_PH_CONVERSATION : requested;
}

// 0..1 multiplier on the brightness scalar for the conversation breath:
// LED_CONVERSATION_BREATH_MIN at tick 0, 1.0 at half period, smooth cosine.
static inline float led_conversation_level(uint32_t tick) {
  const float two_pi = 6.28318530718f;
  float t = (float)(tick % LED_CONVERSATION_BREATH_TICKS) /
            (float)LED_CONVERSATION_BREATH_TICKS;
  float wave = 0.5f - 0.5f * cosf(two_pi * t);  // 0..1..0
  return LED_CONVERSATION_BREATH_MIN +
         (1.0f - LED_CONVERSATION_BREATH_MIN) * wave;
}

#endif  // PIPECAT_LED_PHASE_H
