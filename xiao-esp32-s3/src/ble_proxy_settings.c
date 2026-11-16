// BLE scanner toggle, settings half (t_6215e36e). See ble_proxy_settings.h.
#include "ble_proxy_settings.h"

#include <stddef.h>

const char BLE_PROXY_PARAM_INVALID[] = "\x01invalid";

#define BLE_PROXY_MS_MIN 3u
#define BLE_PROXY_MS_MAX 10240u

bool ble_proxy_settings_valid(const BleProxySettings *s) {
  return s->window_ms >= BLE_PROXY_MS_MIN && s->window_ms <= s->interval_ms &&
         s->interval_ms <= BLE_PROXY_MS_MAX;
}

bool ble_proxy_settings_reboot_pending(const BleProxySettings *running,
                                       const BleProxySettings *stored) {
  if (running->enabled != stored->enabled)
    return true;
  return stored->enabled && (running->interval_ms != stored->interval_ms ||
                             running->window_ms != stored->window_ms);
}

// Plain decimal digits, 1..5 of them, value <= BLE_PROXY_MS_MAX.
static bool parse_ms(const char *v, uint16_t *out) {
  if (v == BLE_PROXY_PARAM_INVALID || v[0] == '\0')
    return false;
  uint32_t x = 0;
  size_t n = 0;
  for (; v[n]; ++n) {
    if (n >= 5 || v[n] < '0' || v[n] > '9')
      return false;
    x = x * 10u + (uint32_t)(v[n] - '0');
  }
  if (x > BLE_PROXY_MS_MAX)
    return false;
  *out = (uint16_t)x;
  return true;
}

static bool same(const BleProxySettings *a, const BleProxySettings *b) {
  return a->enabled == b->enabled && a->interval_ms == b->interval_ms &&
         a->window_ms == b->window_ms;
}

BleProxySetResult ble_proxy_settings_apply(
    const BleProxySettings *running, const BleProxySettings *stored,
    const char *enabled, const char *interval_ms, const char *window_ms,
    BleProxySettings *out, const char **err) {
  *out = *stored;
  *err = "";
  if (!enabled && !interval_ms && !window_ms) {
    *err = "nothing to set: enabled=0|1 [interval_ms= window_ms=]";
    return BLE_PROXY_SET_BAD;
  }
  BleProxySettings next = *stored;
  if (enabled) {
    if (enabled == BLE_PROXY_PARAM_INVALID ||
        !((enabled[0] == '0' || enabled[0] == '1') && enabled[1] == '\0')) {
      *err = "enabled must be 0 or 1";
      return BLE_PROXY_SET_BAD;
    }
    next.enabled = enabled[0] == '1';
  }
  if (interval_ms && !parse_ms(interval_ms, &next.interval_ms)) {
    *err = "interval_ms must be whole ms, 3..10240";
    return BLE_PROXY_SET_BAD;
  }
  if (window_ms && !parse_ms(window_ms, &next.window_ms)) {
    *err = "window_ms must be whole ms, 3..10240";
    return BLE_PROXY_SET_BAD;
  }
  if (!ble_proxy_settings_valid(&next)) {
    *err = "need 3 <= window_ms <= interval_ms <= 10240";
    return BLE_PROXY_SET_BAD;
  }
  *out = next;
  if (same(&next, stored))
    return BLE_PROXY_SET_UNCHANGED;
  return ble_proxy_settings_reboot_pending(running, &next)
             ? BLE_PROXY_SET_REBOOT
             : BLE_PROXY_SET_SAVED;
}
