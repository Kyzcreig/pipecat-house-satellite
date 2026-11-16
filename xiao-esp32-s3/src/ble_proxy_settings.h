// BLE scanner toggle, settings half (t_6215e36e). Pure C, no ESP-IDF: decides
// what a POST /ble/proxy does to the three persisted settings. Host-tested in
// tests/host/test_ble_proxy_settings.c.
//
// Two copies exist on the device. `running` is what this boot applied (the
// controller is only started at boot); `stored` is what NVS holds now, which
// differs from `running` after a save that waits for a reboot.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  bool enabled;
  uint16_t interval_ms;
  uint16_t window_ms;
} BleProxySettings;

// Marker for a query value that was present but could not be read (too long
// for the buffer, bad encoding). Never valid.
extern const char BLE_PROXY_PARAM_INVALID[];

typedef enum {
  BLE_PROXY_SET_BAD = -1,       // 400; *err says why, nothing saved
  BLE_PROXY_SET_UNCHANGED = 0,  // stored already holds it
  BLE_PROXY_SET_SAVED = 1,      // save; takes effect without a reboot
  BLE_PROXY_SET_REBOOT = 2,     // save; differs from running, reboot applies
} BleProxySetResult;

// 3 <= window_ms <= interval_ms <= 10240 (the HCI range, whole ms).
bool ble_proxy_settings_valid(const BleProxySettings *s);

// True when `stored` would run differently from `running` after a reboot: an
// on/off flip, or new timing while on.
bool ble_proxy_settings_reboot_pending(const BleProxySettings *running,
                                       const BleProxySettings *stored);

// Apply the request's raw query values (NULL = key absent) on top of
// `stored`. "enabled" must be exactly "0" or "1"; the timings plain decimal
// digits. Any present-but-bad value, an all-absent request, or an invalid
// combination returns BLE_PROXY_SET_BAD and leaves *out = *stored.
BleProxySetResult ble_proxy_settings_apply(
    const BleProxySettings *running, const BleProxySettings *stored,
    const char *enabled, const char *interval_ms, const char *window_ms,
    BleProxySettings *out, const char **err);

#ifdef __cplusplus
}
#endif
