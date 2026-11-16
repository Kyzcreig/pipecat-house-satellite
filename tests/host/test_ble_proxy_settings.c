// Host test for ble_proxy_settings.c (t_6215e36e): the POST /ble/proxy
// decision. Covers the three Prism P2 on PR #44: present-but-invalid values
// are refused (09effbd59d49), a second POST compares against what NVS holds
// rather than the boot copy (ef8a8c874df9), and a pending save is reported
// as pending (28f87717c228).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ble_proxy_settings.h"

static int g_fail = 0;
#define CHECK(c)                                                   \
  do {                                                             \
    if (!(c)) {                                                    \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
      g_fail++;                                                    \
    }                                                              \
  } while (0)

static const BleProxySettings kOff = {false, 320, 30};
static const BleProxySettings kOn = {true, 320, 30};

static int apply(const BleProxySettings *run, const BleProxySettings *st,
                 const char *en, const char *iv, const char *wn,
                 BleProxySettings *out) {
  const char *err = NULL;
  const int r = ble_proxy_settings_apply(run, st, en, iv, wn, out, &err);
  CHECK(err != NULL);
  if (r == BLE_PROXY_SET_BAD) {
    CHECK(err[0] != '\0');
    CHECK(memcmp(out, st, sizeof(*out)) == 0);  // nothing applied
  }
  return r;
}

static void test_invalid_values_refused(void) {
  BleProxySettings o;
  const char *bad_en[] = {"",     "2",  "01", "1x",
                          "true", "-1", " 1", BLE_PROXY_PARAM_INVALID};
  for (size_t i = 0; i < sizeof(bad_en) / sizeof(bad_en[0]); ++i)
    CHECK(apply(&kOff, &kOff, bad_en[i], NULL, NULL, &o) == BLE_PROXY_SET_BAD);
  // A bad timing next to a good switch must not be dropped silently.
  const char *bad_ms[] = {
      "",      "abc",   "12a",    "-5",  "+5",
      "10241", "99999", "123456", " 30", BLE_PROXY_PARAM_INVALID};
  for (size_t i = 0; i < sizeof(bad_ms) / sizeof(bad_ms[0]); ++i) {
    CHECK(apply(&kOff, &kOff, "1", bad_ms[i], NULL, &o) == BLE_PROXY_SET_BAD);
    CHECK(apply(&kOff, &kOff, "1", NULL, bad_ms[i], &o) == BLE_PROXY_SET_BAD);
  }
  // Range: window >= 3, window <= interval, interval <= 10240.
  CHECK(apply(&kOff, &kOff, NULL, "100", "2", &o) == BLE_PROXY_SET_BAD);
  CHECK(apply(&kOff, &kOff, NULL, "100", "101", &o) == BLE_PROXY_SET_BAD);
  CHECK(apply(&kOff, &kOff, NULL, "20", NULL, &o) == BLE_PROXY_SET_BAD);
  CHECK(apply(&kOff, &kOff, NULL, NULL, NULL, &o) == BLE_PROXY_SET_BAD);
  // Edges that are fine.
  CHECK(apply(&kOff, &kOff, NULL, "10240", "3", &o) == BLE_PROXY_SET_SAVED);
  CHECK(o.interval_ms == 10240 && o.window_ms == 3 && !o.enabled);
  CHECK(apply(&kOff, &kOff, NULL, "30", "30", &o) == BLE_PROXY_SET_SAVED);
}

static void test_off_timing_saves_without_reboot(void) {
  BleProxySettings o;
  CHECK(apply(&kOff, &kOff, NULL, "640", "60", &o) == BLE_PROXY_SET_SAVED);
  CHECK(!ble_proxy_settings_reboot_pending(&kOff, &o));
  CHECK(apply(&kOff, &kOff, "0", NULL, NULL, &o) == BLE_PROXY_SET_UNCHANGED);
}

static void test_on_flip_and_on_timing_reboot(void) {
  BleProxySettings o;
  CHECK(apply(&kOff, &kOff, "1", NULL, NULL, &o) == BLE_PROXY_SET_REBOOT);
  CHECK(o.enabled && o.interval_ms == 320 && o.window_ms == 30);
  CHECK(apply(&kOn, &kOn, NULL, NULL, "60", &o) == BLE_PROXY_SET_REBOOT);
  CHECK(apply(&kOn, &kOn, "0", NULL, NULL, &o) == BLE_PROXY_SET_REBOOT);
  CHECK(apply(&kOn, &kOn, "1", "320", "30", &o) == BLE_PROXY_SET_UNCHANGED);
}

// ef8a8c874df9: the second POST compares against stored, not the boot copy.
static void test_second_post_reads_stored(void) {
  BleProxySettings stored, o;
  // Boot OFF; POST enabled=1 saved (reboot pending).
  CHECK(apply(&kOff, &kOff, "1", NULL, NULL, &stored) == BLE_PROXY_SET_REBOOT);
  // Then POST window_ms=60: must keep enabled=1 from NVS, not reset to 0.
  CHECK(apply(&kOff, &stored, NULL, NULL, "60", &o) == BLE_PROXY_SET_REBOOT);
  CHECK(o.enabled && o.window_ms == 60 && o.interval_ms == 320);
  // Repeating the same enabled=1 is a no-op against NVS...
  CHECK(apply(&kOff, &stored, "1", NULL, NULL, &o) == BLE_PROXY_SET_UNCHANGED);
  // ...yet the reboot is still owed (28f87717c228).
  CHECK(ble_proxy_settings_reboot_pending(&kOff, &o));
  // Undoing it before the reboot saves and leaves nothing pending.
  CHECK(apply(&kOff, &stored, "0", NULL, NULL, &o) == BLE_PROXY_SET_SAVED);
  CHECK(!ble_proxy_settings_reboot_pending(&kOff, &o));
}

static void test_reboot_pending(void) {
  const BleProxySettings on_new = {true, 640, 30};
  const BleProxySettings off_new = {false, 640, 30};
  CHECK(!ble_proxy_settings_reboot_pending(&kOff, &kOff));
  CHECK(!ble_proxy_settings_reboot_pending(&kOn, &kOn));
  CHECK(ble_proxy_settings_reboot_pending(&kOff, &kOn));
  CHECK(ble_proxy_settings_reboot_pending(&kOn, &kOff));
  CHECK(ble_proxy_settings_reboot_pending(&kOn, &on_new));
  CHECK(!ble_proxy_settings_reboot_pending(&kOff, &off_new));
}

static void test_valid(void) {
  const BleProxySettings zero = {false, 0, 0};
  const BleProxySettings big = {false, 20000, 30};
  CHECK(ble_proxy_settings_valid(&kOff));
  CHECK(!ble_proxy_settings_valid(&zero));
  CHECK(!ble_proxy_settings_valid(&big));
}

int main(void) {
  test_invalid_values_refused();
  test_off_timing_saves_without_reboot();
  test_on_flip_and_on_timing_reboot();
  test_second_post_reads_stored();
  test_reboot_pending();
  test_valid();
  if (g_fail) {
    fprintf(stderr, "%d failure(s)\n", g_fail);
    return 1;
  }
  printf("ble_proxy_settings: PASS\n");
  return 0;
}
