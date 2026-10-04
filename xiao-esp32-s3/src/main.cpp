#include "main.h"

#include <esp_event.h>
#include <esp_log.h>
#include <peer.h>
#include <string.h>

#include "reconnect_watchdog.h"

#ifndef LINUX_BUILD
#include <esp_app_desc.h>
#include <esp_attr.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <freertos/task.h>

#include "boot_guard.h"
#include "nvs_flash.h"

static constexpr unsigned WEBRTC_LOOP_TASK_PRIORITY = 8;

// ---- Unattended recovery (t_2e80e072) -------------------------------------
// RTC_NOINIT survives WDT/panic/brownout/software resets; boot_guard.h
// re-initialises it on a cold power-on (contents are garbage then).
static RTC_NOINIT_ATTR PipecatBootGuardState s_boot_guard;
static PipecatNetWatchdog s_net_watchdog;
static uint32_t s_net_deadline_ms = kPipecatNetWatchdogBaseMs;
static esp_reset_reason_t s_reset_reason = ESP_RST_UNKNOWN;
static uint32_t s_boot_fault_count = 0;
static constexpr uint32_t NET_WATCHDOG_PERIOD_MS = 1000;

static PipecatResetKind pipecat_classify_reset(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:
      return PipecatResetKind::kPowerOn;
    case ESP_RST_BROWNOUT:
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
      return PipecatResetKind::kFault;
    case ESP_RST_SW:
      return PipecatResetKind::kSoftware;
    default:
      return PipecatResetKind::kOther;
  }
}

const char *pipecat_reset_reason_name() {
  switch (s_reset_reason) {
    case ESP_RST_POWERON:
      return "poweron";
    case ESP_RST_EXT:
      return "ext";
    case ESP_RST_SW:
      return "sw";
    case ESP_RST_PANIC:
      return "panic";
    case ESP_RST_INT_WDT:
      return "int_wdt";
    case ESP_RST_TASK_WDT:
      return "task_wdt";
    case ESP_RST_WDT:
      return "wdt";
    case ESP_RST_DEEPSLEEP:
      return "deepsleep";
    case ESP_RST_BROWNOUT:
      return "brownout";
    case ESP_RST_SDIO:
      return "sdio";
    case ESP_RST_USB:
      return "usb";
    case ESP_RST_JTAG:
      return "jtag";
    default:
      return "unknown";
  }
}
uint32_t pipecat_boot_fault_count() {
  return s_boot_fault_count;
}
uint32_t pipecat_boots_since_poweron() {
  return s_boot_guard.total_boots;
}
uint32_t pipecat_netwdt_restarts() {
  return s_boot_guard.netwdt_restarts;
}
uint32_t pipecat_crash_boots() {
  return s_boot_guard.crash_boots;
}
uint32_t pipecat_slot_flipped() {
  return s_boot_guard.slot_flipped;
}
uint32_t pipecat_net_watchdog_deadline_s() {
  return s_net_deadline_ms / 1000;
}

// Network watchdog. Armed BEFORE any blocking init so it also covers a stuck
// Wi-Fi join or a wedged peripheral init -- the stock reconnect watchdog only
// exists once app_main reaches its loop, which a satellite that never
// associates after a power blip never does.
static void pipecat_net_watchdog_cb(void *arg) {
  const bool healthy = pipecat_wifi_connected() && pipecat_webrtc_connected &&
                       pipecat_webrtc_server_heartbeat_fresh() &&
                       pipecat_xvf3800_present();
  if (healthy)
    pipecat_boot_guard_on_healthy(&s_boot_guard);
  if (s_net_watchdog.update(healthy, NET_WATCHDOG_PERIOD_MS,
                            s_net_deadline_ms)) {
    ESP_LOGE(LOG_TAG,
             "Network watchdog: unhealthy for %us (wifi=%d peer=%d hb=%d "
             "xvf=%d); restarting",
             (unsigned)(s_net_deadline_ms / 1000), pipecat_wifi_connected(),
             pipecat_webrtc_connected, pipecat_webrtc_server_heartbeat_fresh(),
             pipecat_xvf3800_present());
    pipecat_boot_guard_note_netwdt_restart(&s_boot_guard);
    esp_restart();
  }
}

// Reads otadata for the other OTA slot and, if boot_guard.h agrees, boots it.
// Returns only when no flip happened. esp_ota_set_boot_partition verifies the
// target image (header, segments, checksum, SHA) before rewriting otadata, so
// a corrupt/erased slot is refused here and we fall through to the hold.
static void pipecat_boot_guard_try_slot_flip() {
  const esp_partition_t *running = esp_ota_get_running_partition();
  const esp_partition_t *other = esp_ota_get_next_update_partition(nullptr);
  const bool present = other != nullptr && running != nullptr &&
                       other->address != running->address;

  PipecatOtaSlotState other_state = PipecatOtaSlotState::kUnknown;
  bool same_image = false;
  if (present) {
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    const esp_err_t ret = esp_ota_get_state_partition(other, &st);
    if (ret == ESP_OK) {
      switch (st) {
        case ESP_OTA_IMG_VALID:
          other_state = PipecatOtaSlotState::kValid;
          break;
        case ESP_OTA_IMG_UNDEFINED:
          other_state = PipecatOtaSlotState::kUndefined;
          break;
        case ESP_OTA_IMG_NEW:
          other_state = PipecatOtaSlotState::kNew;
          break;
        case ESP_OTA_IMG_PENDING_VERIFY:
          other_state = PipecatOtaSlotState::kPendingVerify;
          break;
        default:
          other_state = PipecatOtaSlotState::kInvalid;
          break;
      }
    } else if (ret == ESP_ERR_NOT_FOUND) {
      // No otadata entry names that slot; the app there (if any) was never
      // selected. Treat as undefined and let image verification decide.
      other_state = PipecatOtaSlotState::kUndefined;
    }
    // Same image on both slots (e.g. the same src.bin uploaded twice to land
    // in a specific slot): flipping would re-run the crashing code.
    esp_app_desc_t other_desc = {};
    const esp_app_desc_t *self = esp_app_get_description();
    if (esp_ota_get_partition_description(other, &other_desc) == ESP_OK) {
      same_image = memcmp(other_desc.app_elf_sha256, self->app_elf_sha256,
                          sizeof(self->app_elf_sha256)) == 0;
    } else {
      other_state = PipecatOtaSlotState::kInvalid;  // no app header there
    }
  }

  const bool flip = pipecat_boot_guard_should_flip_slot(
      &s_boot_guard, present, other_state, same_image);
  ESP_LOGE(LOG_TAG,
           "Boot guard: %u consecutive crash boots on %s (other=%s state=%d "
           "same_image=%d) -> %s",
           (unsigned)s_boot_guard.crash_boots, running ? running->label : "?",
           present ? other->label : "none", (int)other_state, same_image,
           flip ? "booting other slot" : "no eligible fallback, backing off");
  if (!flip)
    return;

  const esp_err_t ret = esp_ota_set_boot_partition(other);
  if (ret != ESP_OK) {
    ESP_LOGE(LOG_TAG, "Boot guard: esp_ota_set_boot_partition(%s) failed: %s",
             other->label, esp_err_to_name(ret));
    return;
  }
  pipecat_boot_guard_note_slot_flip(&s_boot_guard);
  esp_restart();
}

static void pipecat_boot_guard_start() {
  s_reset_reason = esp_reset_reason();
  const bool after_fault = pipecat_boot_guard_on_boot(
      &s_boot_guard, pipecat_classify_reset(s_reset_reason));
  s_boot_fault_count = s_boot_guard.fault_boots;
  s_net_deadline_ms = pipecat_net_watchdog_deadline_ms(&s_boot_guard);
  ESP_LOGI(
      LOG_TAG,
      "Boot guard: reset=%s fault=%d fault_boots=%u crash_boots=%u "
      "slot_flipped=%u boots=%u net_watchdog=%us",
      pipecat_reset_reason_name(), after_fault,
      (unsigned)s_boot_guard.fault_boots, (unsigned)s_boot_guard.crash_boots,
      (unsigned)s_boot_guard.slot_flipped, (unsigned)s_boot_guard.total_boots,
      (unsigned)(s_net_deadline_ms / 1000));

  // Slot fallback (t_cf433ede): a VALID image that crashes before health on
  // kPipecatSlotFlipCrashBoots consecutive boots hands over to the other OTA
  // slot, once per power cycle. Decided before any init so a crash during
  // init cannot pre-empt it. Only the pure policy lives in boot_guard.h.
  if (s_boot_guard.crash_boots >= kPipecatSlotFlipCrashBoots &&
      !s_boot_guard.slot_flipped) {
    pipecat_boot_guard_try_slot_flip();
  }

  // Task WDT on app_main: every blocking init below and the main loop must
  // make progress within CONFIG_ESP_TASK_WDT_TIMEOUT_S or the chip resets.
  ESP_ERROR_CHECK(esp_task_wdt_add(nullptr));

  // Boot-loop safety: fault reboots are never faster than 1/min.
  const uint32_t hold_ms = pipecat_boot_guard_hold_ms(&s_boot_guard);
  if (hold_ms > 0) {
    ESP_LOGW(LOG_TAG, "Boot guard: holding %us after fault reset",
             (unsigned)(hold_ms / 1000));
    for (uint32_t waited = 0; waited < hold_ms; waited += 1000) {
      esp_task_wdt_reset();
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  }

  const esp_timer_create_args_t args = {
      .callback = &pipecat_net_watchdog_cb,
      .arg = nullptr,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "net_watchdog",
      .skip_unhandled_events = true,
  };
  esp_timer_handle_t timer = nullptr;
  ESP_ERROR_CHECK(esp_timer_create(&args, &timer));
  ESP_ERROR_CHECK(
      esp_timer_start_periodic(timer, NET_WATCHDOG_PERIOD_MS * 1000ULL));
}

extern "C" void app_main(void) {
  pipecat_boot_guard_start();

  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  ESP_ERROR_CHECK(esp_event_loop_create_default());
  peer_init();
  esp_task_wdt_reset();
  pipecat_init_audio_capture();
  esp_task_wdt_reset();
  // Audio init probes XVF I2C and writes the baked DSP profile. Overlay durable
  // tuning before signalling creates the first WebRTC offer.
  pipecat_replay_xvf_params();
  pipecat_init_audio_decoder();
  esp_task_wdt_reset();
  pipecat_init_wifi();  // feeds the task WDT while it waits for an IP
  pipecat_init_mdns();
  pipecat_init_ota_server();
  pipecat_start_ota_validation_watchdog();
  esp_task_wdt_reset();
  pipecat_init_webrtc();
  pipecat_validate_ota_if_healthy();

  // Persistent reconnect watchdog. A peer-state callback is not guaranteed for
  // a half-open SCTP/ICE path, so require both a connected peer and recent
  // server heartbeat traffic. Restarting is the firmware's safe re-offer path;
  // disconnects get a 30s grace, while an expired heartbeat freshness window
  // already includes one full server ping interval plus jitter grace.
  PipecatReconnectWatchdog reconnect_watchdog;

  // audio_publisher runs at priority 7 on this core. Keep peer/SCTP handling
  // above it so an overrun in full-duplex audio cannot strand a server ping or
  // staged pong on the default low-priority app_main task.
  vTaskPrioritySet(nullptr, WEBRTC_LOOP_TASK_PRIORITY);

  while (1) {
    esp_task_wdt_reset();
    pipecat_webrtc_loop();
    pipecat_validate_ota_if_healthy();
    const bool heartbeat_fresh = pipecat_webrtc_server_heartbeat_fresh();
    const bool reconnect_deadline = reconnect_watchdog.update(
        pipecat_webrtc_connected, heartbeat_fresh, TICK_INTERVAL);
#if PIPECAT_REDIAL
    // t_db77e56b: the watchdog deadline (and a hub-closed peer, SCTP ABORT,
    // or POST /webrtc/redial) re-dials the hub in place; esp_restart() is
    // the fallback inside the policy after kMaxConsecutiveFailures.
    if (reconnect_deadline) {
      ESP_LOGW(LOG_TAG,
               "WebRTC reconnect deadline reached (peer_connected=%d "
               "server_heartbeat_fresh=%d); re-dialing without restart",
               pipecat_webrtc_connected, heartbeat_fresh);
    }
    if (pipecat_webrtc_redial_tick(reconnect_deadline) || reconnect_deadline) {
      // Fresh grace for the new attempt (an expired watchdog is pinned at
      // its deadline and would fire every tick).
      reconnect_watchdog = PipecatReconnectWatchdog();
    }
#else
    if (reconnect_deadline) {
      ESP_LOGW(LOG_TAG,
               "WebRTC reconnect deadline reached (peer_connected=%d "
               "server_heartbeat_fresh=%d); restarting to re-offer",
               pipecat_webrtc_connected, heartbeat_fresh);
      esp_restart();
    }
#endif
    vTaskDelay(pdMS_TO_TICKS(TICK_INTERVAL));
  }
}
#else
int main(void) {
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  peer_init();
  pipecat_webrtc();

  while (1) {
    pipecat_webrtc_loop();
    vTaskDelay(pdMS_TO_TICKS(TICK_INTERVAL));
  }
}
#endif
