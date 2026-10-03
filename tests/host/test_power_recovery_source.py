#!/usr/bin/env python3
"""Source contract: unattended power-outage recovery (t_2e80e072).

Root cause 2026-09-25: wifi.cpp stopped calling esp_wifi_connect() after 5
retries and app_main blocked forever waiting for an IP, before any watchdog
existed. These asserts keep that class from coming back.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "xiao-esp32-s3" / "src"
WIFI = (SRC / "wifi.cpp").read_text()
MAIN = (SRC / "main.cpp").read_text()
MEDIA = (SRC / "media.cpp").read_text()
DEFAULTS = (ROOT / "xiao-esp32-s3" / "sdkconfig.defaults").read_text()


def config_on(name):
    return re.search(rf"^{name}=y$", DEFAULTS, re.M) is not None


# (a) Wi-Fi: no retry cap; every disconnect schedules another attempt.
assert "s_retry_num" not in WIFI, "bounded Wi-Fi retry counter is back"
assert not re.search(r"<\s*5\s*\)", WIFI), "a retry cap re-appeared in wifi.cpp"
disc = WIFI[WIFI.index("WIFI_EVENT_STA_DISCONNECTED"):WIFI.index("IP_EVENT_STA_GOT_IP")]
assert "esp_timer_start_once(s_reconnect_timer" in disc
assert "pipecat_wifi_backoff_ms(" in disc
assert "g_wifi_connected = false;" in disc
wait = WIFI[WIFI.index("while (!g_wifi_connected)"):]
assert "esp_task_wdt_reset();" in wait.split("}")[0]

# Associated-but-no-IP stall: a GOT_IP deadline armed on STA_CONNECTED
# that drops the association (reproduced 2026-09-25 21:00 on kitchen).
conn = WIFI[WIFI.index("WIFI_EVENT_STA_CONNECTED"):WIFI.index("WIFI_EVENT_STA_DISCONNECTED")]
assert "esp_timer_start_once(s_ip_timeout_timer" in conn
cb = WIFI[WIFI.index("static void pipecat_wifi_ip_timeout_cb"):]
assert "esp_wifi_disconnect()" in cb.split("\n}\n")[0]
got = WIFI[WIFI.index("IP_EVENT_STA_GOT_IP) {"):]
assert "esp_timer_stop(s_ip_timeout_timer);" in got.split("}")[0]

# Network watchdog + boot guard armed BEFORE any blocking init.
body = MAIN[MAIN.index('extern "C" void app_main(void) {'):]
first = body.index("pipecat_boot_guard_start();")
for later in ("nvs_flash_init()", "pipecat_init_audio_capture();", "pipecat_init_wifi();"):
    assert first < body.index(later), f"boot guard must run before {later}"
assert "RTC_NOINIT_ATTR PipecatBootGuardState" in MAIN
assert "esp_task_wdt_add(nullptr)" in MAIN
assert "esp_timer_start_periodic(" in MAIN
loop = body[body.index("while (1) {"):]
assert loop.index("esp_task_wdt_reset();") < loop.index("pipecat_webrtc_loop();")

# Slot fallback (t_cf433ede): decided from RTC state before any init, flips
# only through esp_ota_set_boot_partition (which verifies the target image),
# latches the RTC flag BEFORE the restart, and the policy stays in boot_guard.h
# (main.cpp only reads otadata and acts).
guard_fn = MAIN[MAIN.index("static void pipecat_boot_guard_start()"):MAIN.index('extern "C" void app_main(void) {')]
assert guard_fn.index("pipecat_boot_guard_try_slot_flip();") < guard_fn.index("esp_task_wdt_add(nullptr)")
assert guard_fn.index("pipecat_boot_guard_try_slot_flip();") < guard_fn.index("pipecat_boot_guard_hold_ms(")
flip_fn = MAIN[MAIN.index("static void pipecat_boot_guard_try_slot_flip()"):MAIN.index("static void pipecat_boot_guard_start()")]
assert "pipecat_boot_guard_should_flip_slot(" in flip_fn
assert "esp_ota_set_boot_partition(other)" in flip_fn
assert flip_fn.index("pipecat_boot_guard_note_slot_flip(&s_boot_guard);") < flip_fn.rindex("esp_restart();")
assert flip_fn.index("esp_ota_set_boot_partition(other)") < flip_fn.index("pipecat_boot_guard_note_slot_flip(")
assert "app_elf_sha256" in flip_fn, "same-image guard must compare the app ELF sha"
assert "esp_ota_mark_app_invalid" not in flip_fn, "fallback must not INVALIDATE the crashing (valid) slot"
BOOT_GUARD = (SRC / "boot_guard.h").read_text()
assert "esp_ota" not in BOOT_GUARD and "#include <esp" not in BOOT_GUARD, "boot_guard.h must stay host-pure"
assert "kPipecatSlotFlipCrashBoots = 3u" in BOOT_GUARD

# (b)(d) brownout + task WDT on; idle checks off (audio saturates core 0).
for name in ("CONFIG_ESP_TASK_WDT_EN", "CONFIG_ESP_TASK_WDT_INIT",
             "CONFIG_ESP_TASK_WDT_PANIC", "CONFIG_ESP_BROWNOUT_DET"):
    assert config_on(name), f"{name}=y missing from sdkconfig.defaults"
assert "# CONFIG_ESP_TASK_WDT_EN is not set" not in DEFAULTS
for cpu in ("CPU0", "CPU1"):
    assert f"# CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_{cpu} is not set" in DEFAULTS

# (c) XVF3800 slower-than-ESP32 boot: probe is retried, not one-shot.
assert "XVF_BOOT_PROBE_ATTEMPTS" in MEDIA
probe = MEDIA[MEDIA.index("for (int attempt = 0; attempt < XVF_BOOT_PROBE_ATTEMPTS"):]
assert "i2c_master_probe(" in probe[:400]

print("power-outage recovery source contract: PASS")
