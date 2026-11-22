/* Host tests for the server-driven LED phase decision (led_phase.h, t_54a2a223).
 * Build/run: run_led_phase_tests.sh. No ESP-IDF required. */
#include <assert.h>
#include <stdio.h>

#include "led_phase.h"

static int fails = 0;
#define CHECK(cond)                                         \
  do {                                                      \
    if (!(cond)) {                                          \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      fails++;                                              \
    }                                                       \
  } while (0)

int main(void) {
  /* Existing four phases: unchanged, honoured inside the TTL, NONE after. */
  const int classic[] = {LED_PH_IDLE, LED_PH_WAITING, LED_PH_THINKING,
                         LED_PH_SPEAKING};
  for (unsigned i = 0; i < sizeof classic / sizeof classic[0]; i++) {
    CHECK(led_phase_effective(classic[i], 0, false) == classic[i]);
    CHECK(led_phase_effective(classic[i], LED_PHASE_TTL_MS - 1, false) ==
          classic[i]);
    CHECK(led_phase_effective(classic[i], LED_PHASE_TTL_MS, false) ==
          LED_PH_NONE);
  }
  /* Playback does not override a non-conversation phase (idle stays idle;
   * the device-local is_playing fallback only applies to NONE). */
  CHECK(led_phase_effective(LED_PH_IDLE, 0, true) == LED_PH_IDLE);
  CHECK(led_phase_effective(LED_PH_NONE, 0, true) == LED_PH_NONE);

  /* Conversation: white while quiet, speaking while audio plays, white again
   * when playback stops, and no TTL (minutes of conversation mode). */
  CHECK(led_phase_effective(LED_PH_CONVERSATION, 0, false) ==
        LED_PH_CONVERSATION);
  CHECK(led_phase_effective(LED_PH_CONVERSATION, 100, true) ==
        LED_PH_SPEAKING);
  CHECK(led_phase_effective(LED_PH_CONVERSATION, 200, false) ==
        LED_PH_CONVERSATION);
  CHECK(led_phase_effective(LED_PH_CONVERSATION, LED_PHASE_TTL_MS, false) ==
        LED_PH_CONVERSATION);
  CHECK(led_phase_effective(LED_PH_CONVERSATION, 10u * 60u * 1000u, false) ==
        LED_PH_CONVERSATION);
  CHECK(led_phase_effective(LED_PH_CONVERSATION, 10u * 60u * 1000u, true) ==
        LED_PH_SPEAKING);

  /* Breath: floor at tick 0, full at half period, periodic, always within
   * [MIN, 1] so the brightness scalar (night level) stays the ceiling. */
  CHECK(led_conversation_level(0) > LED_CONVERSATION_BREATH_MIN - 1e-4f &&
        led_conversation_level(0) < LED_CONVERSATION_BREATH_MIN + 1e-4f);
  CHECK(led_conversation_level(LED_CONVERSATION_BREATH_TICKS / 2) > 0.999f);
  CHECK(led_conversation_level(LED_CONVERSATION_BREATH_TICKS) ==
        led_conversation_level(0));
  for (uint32_t t = 0; t < 3 * LED_CONVERSATION_BREATH_TICKS; t++) {
    float v = led_conversation_level(t);
    CHECK(v >= LED_CONVERSATION_BREATH_MIN - 1e-4f && v <= 1.0f + 1e-4f);
  }
  /* Slow: one tick moves the level by under 5 % of the range. */
  for (uint32_t t = 0; t < LED_CONVERSATION_BREATH_TICKS; t++) {
    float d = led_conversation_level(t + 1) - led_conversation_level(t);
    if (d < 0)
      d = -d;
    CHECK(d < 0.05f);
  }

  if (fails) {
    printf("%d failure(s)\n", fails);
    return 1;
  }
  printf("ok - led_phase: all checks pass\n");
  return 0;
}
