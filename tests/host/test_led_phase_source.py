#!/usr/bin/env python3
"""Source contract for the 'conversation' LED phase (t_54a2a223).

Pins the wiring the C test (test_led_phase.c) cannot see:
  1. rtvi.cpp maps "conversation" to PIPECAT_LED_PHASE_CONVERSATION, and an
     unknown phase still falls to `default: break`.
  2. main.h gives it value 5; media.cpp static_asserts the enums agree.
  3. pipecat_led_step() decides through led_phase_effective() with is_playing,
     renders LED_CONVERSATION for it, and clears it when the peer drops.
  4. The LED_CONVERSATION render is white (saturation 0) scaled by bmax, i.e.
     the runtime brightness scalar (night level).
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "xiao-esp32-s3" / "src"
MEDIA = (SRC / "media.cpp").read_text()
RTVI = (SRC / "rtvi.cpp").read_text()
MAIN_H = (SRC / "main.h").read_text()


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1 : pos]
    raise AssertionError(f"unterminated function: {signature}")


# 1. RTVI parse.
assert re.search(
    r'case hash\("conversation"\):\s*pipecat_led_set_phase\(PIPECAT_LED_PHASE_CONVERSATION\);\s*break;',
    RTVI,
), "rtvi.cpp does not map 'conversation'"
led_switch = RTVI[RTVI.index('case hash("idle"):') :]
led_switch = led_switch[: led_switch.index("}")]
assert re.search(r"default:\s*break;", led_switch), "unknown phase no longer ignored"

# 2. Enum values.
assert re.search(r"PIPECAT_LED_PHASE_CONVERSATION = 5,", MAIN_H)
assert "PHASE_CONVERSATION == PIPECAT_LED_PHASE_CONVERSATION" in MEDIA
assert "PHASE_CONVERSATION == LED_PH_CONVERSATION" in MEDIA

# 3. Decision + disconnect clear.
step = function_body(MEDIA, "static void pipecat_led_step()")
assert "led_phase_effective(g_server_phase.load()" in step
assert "is_playing.load()" in step.split("led_phase_effective(", 1)[1].split(";", 1)[0]
assert re.search(r"case PHASE_CONVERSATION:\s*st = LED_CONVERSATION;", step)
off = step[step.index("if (!pipecat_webrtc_connected)") : step.index("} else if (phase != PHASE_NONE)")]
assert "compare_exchange_strong(conv, PHASE_NONE)" in off, "conversation not cleared on peer loss"

# 4. Render.
render = function_body(MEDIA, "static void led_render(")
conv = render[render.index("case LED_CONVERSATION:") : render.index("case LED_SPEAKING:")]
assert re.search(r"led_hsv\(0\.0f, 0\.0f, bmax \* led_conversation_level\(", conv), \
    "conversation ring is not white scaled by bmax"
assert "s_led_brightness.load() / 255.0f" in render

print("ok - led conversation phase source contract")
