/* Host tests for the passive BLE scanner's HCI half (t_6215e36e).
 *   ./tests/host/run_ble_proxy_tests.sh
 * Byte vectors are written out from the Bluetooth Core spec layouts, not
 * produced by the code under test.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ble_hci.h"

#define RUN(fn)               \
  do {                        \
    fn();                     \
    printf("ok - %s\n", #fn); \
  } while (0)

static void test_scan_params_command_bytes(void) {
  uint8_t buf[BLE_HCI_CMD_MAX];
  uint16_t iv, wn;
  /* ESPHome's default operating point: 320 ms interval, 30 ms window. */
  assert(ble_hci_scan_timing(320, 30, &iv, &wn) == 0);
  assert(iv == 512 && wn == 48); /* 0.625 ms units */
  size_t n = ble_hci_cmd_le_set_scan_params(buf, 1, iv, wn);
  const uint8_t want[] = {0x01, 0x0B, 0x20, 0x07, 0x00, 0x00,
                          0x02, 0x30, 0x00, 0x00, 0x00};
  assert(n == sizeof(want) && memcmp(buf, want, n) == 0);
  /* passive=0 flips only the scan type byte. */
  ble_hci_cmd_le_set_scan_params(buf, 0, iv, wn);
  assert(buf[4] == 0x01);
}

static void test_other_commands(void) {
  uint8_t buf[BLE_HCI_CMD_MAX];
  size_t n = ble_hci_cmd_reset(buf);
  const uint8_t reset[] = {0x01, 0x03, 0x0C, 0x00};
  assert(n == 4 && memcmp(buf, reset, 4) == 0);

  n = ble_hci_cmd_set_event_mask(buf);
  const uint8_t mask[] = {0x01, 0x01, 0x0C, 0x08, 0xFF, 0xFF,
                          0xFF, 0xFF, 0xFF, 0x1F, 0x00, 0x20};
  assert(n == sizeof(mask) && memcmp(buf, mask, n) == 0);

  n = ble_hci_cmd_le_set_scan_enable(buf, 1);
  const uint8_t en[] = {0x01, 0x0C, 0x20, 0x02, 0x01, 0x00};
  assert(n == sizeof(en) && memcmp(buf, en, n) == 0);
  ble_hci_cmd_le_set_scan_enable(buf, 0);
  assert(buf[4] == 0x00 && buf[5] == 0x00);
}

static void test_scan_timing_clamps(void) {
  uint16_t iv, wn;
  /* window > interval is clamped to the interval. */
  assert(ble_hci_scan_timing(100, 200, &iv, &wn) == 1);
  assert(iv == 160 && wn == 160);
  /* below 2.5 ms and above 10.24 s are clamped to the spec range. */
  assert(ble_hci_scan_timing(1, 1, &iv, &wn) == 1);
  assert(iv == 4 && wn == 4);
  assert(ble_hci_scan_timing(4000000000u, 10, &iv, &wn) == 1);
  assert(iv == 0x4000 && wn == 16);
}

static void test_cmd_complete_and_status(void) {
  uint16_t op = 0;
  uint8_t st = 0xEE;
  /* Command Complete for LE Set Scan Enable, status 0. */
  const uint8_t cc[] = {0x04, 0x0E, 0x04, 0x05, 0x0C, 0x20, 0x00};
  assert(ble_hci_parse_cmd_result(cc, sizeof(cc), &op, &st) == 1);
  assert(op == 0x200C && st == 0x00);
  /* Command Status, status 0x0C (command disallowed) for 0x200B. */
  const uint8_t cs[] = {0x04, 0x0F, 0x04, 0x0C, 0x01, 0x0B, 0x20};
  assert(ble_hci_parse_cmd_result(cs, sizeof(cs), &op, &st) == 1);
  assert(op == 0x200B && st == 0x0C);
  /* Truncated. */
  assert(ble_hci_parse_cmd_result(cc, sizeof(cc) - 1, &op, &st) == -1);
  /* Another event is not a command result. */
  const uint8_t other[] = {0x04, 0x3E, 0x01, 0x02};
  assert(ble_hci_parse_cmd_result(other, sizeof(other), &op, &st) == 0);
}

/* One report: ADV_NONCONN_IND, random address C0:11:22:33:44:55 (sent
 * little-endian), 3 bytes of AD data (flags), RSSI -67 (0xBD). */
static const uint8_t kOneReport[] = {
    0x04, 0x3E, 0x0F,                   /* event LE Meta, plen 15 */
    0x02, 0x01,                         /* subevent adv report, 1 report */
    0x03, 0x01,                         /* event type, address type */
    0x55, 0x44, 0x33, 0x22, 0x11, 0xC0, /* address LE */
    0x03, 0x02, 0x01, 0x06,             /* len 3, data */
    0xBD                                /* rssi */
};

static void test_single_report(void) {
  BleAdvReport r[4];
  memset(r, 0xA5, sizeof(r));
  int n = ble_hci_parse_adv_report(kOneReport, sizeof(kOneReport), r, 4);
  assert(n == 1);
  assert(r[0].address == 0xC01122334455ULL);
  assert(r[0].address_type == 1 && r[0].event_type == 3);
  assert(r[0].rssi == -67);
  assert(r[0].data_len == 3);
  assert(memcmp(r[0].data, "\x02\x01\x06", 3) == 0);
}

static void test_two_reports_interleaved_and_cap(void) {
  /* Report A: public 00:00:00:00:00:01, empty data, rssi 127 (no reading).
   * Report B: random 11:22:33:44:55:66, 31-byte data, rssi -100. */
  uint8_t pkt[3 + 2 + 10 + 41];
  size_t i = 0;
  pkt[i++] = 0x04;
  pkt[i++] = 0x3E;
  pkt[i++] = (uint8_t)(sizeof(pkt) - 3);
  pkt[i++] = 0x02;
  pkt[i++] = 2;
  /* A */
  pkt[i++] = 0x00;
  pkt[i++] = 0x00;
  const uint8_t a_addr[6] = {0x01, 0, 0, 0, 0, 0};
  memcpy(pkt + i, a_addr, 6);
  i += 6;
  pkt[i++] = 0;
  pkt[i++] = 0x7F;
  /* B */
  pkt[i++] = 0x00;
  pkt[i++] = 0x01;
  const uint8_t b_addr[6] = {0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
  memcpy(pkt + i, b_addr, 6);
  i += 6;
  pkt[i++] = 31;
  for (int k = 0; k < 31; ++k)
    pkt[i++] = (uint8_t)k;
  pkt[i++] = (uint8_t)(int8_t)-100;
  assert(i == sizeof(pkt));

  BleAdvReport r[2];
  assert(ble_hci_parse_adv_report(pkt, sizeof(pkt), r, 2) == 2);
  assert(r[0].address == 1 && r[0].data_len == 0 && r[0].rssi == 127);
  assert(r[1].address == 0x112233445566ULL && r[1].address_type == 1);
  assert(r[1].data_len == 31 && r[1].data[30] == 30 && r[1].rssi == -100);
  /* A cap of 1 writes one report and still validates the rest. */
  memset(r, 0, sizeof(r));
  assert(ble_hci_parse_adv_report(pkt, sizeof(pkt), r, 1) == 1);
  assert(r[1].address == 0);
}

static void test_malformed_reports_rejected(void) {
  BleAdvReport r[2];
  uint8_t pkt[sizeof(kOneReport)];
  /* Truncated by one byte (rssi missing). */
  assert(ble_hci_parse_adv_report(kOneReport, sizeof(kOneReport) - 1, r, 2) ==
         -1);
  /* Data length runs past the event. */
  memcpy(pkt, kOneReport, sizeof(pkt));
  pkt[13] = 4;
  assert(ble_hci_parse_adv_report(pkt, sizeof(pkt), r, 2) == -1);
  /* Data length above the legacy 31-byte limit. */
  memcpy(pkt, kOneReport, sizeof(pkt));
  pkt[13] = 32;
  assert(ble_hci_parse_adv_report(pkt, sizeof(pkt), r, 2) == -1);
  /* Claims two reports, carries one. */
  memcpy(pkt, kOneReport, sizeof(pkt));
  pkt[4] = 2;
  assert(ble_hci_parse_adv_report(pkt, sizeof(pkt), r, 2) == -1);
  /* A self-consistent report with 32 data bytes (an extended-length PDU on a
   * legacy report) must still be refused: data[] holds 31. */
  uint8_t long_pkt[3 + 2 + 9 + 32 + 1];
  memset(long_pkt, 0, sizeof(long_pkt));
  long_pkt[0] = 0x04;
  long_pkt[1] = 0x3E;
  long_pkt[2] = (uint8_t)(sizeof(long_pkt) - 3);
  long_pkt[3] = 0x02;
  long_pkt[4] = 1;
  long_pkt[13] = 32;
  assert(ble_hci_parse_adv_report(long_pkt, sizeof(long_pkt), r, 2) == -1);
  /* Zero reports. */
  const uint8_t zero[] = {0x04, 0x3E, 0x02, 0x02, 0x00};
  assert(ble_hci_parse_adv_report(zero, sizeof(zero), r, 2) == -1);
  /* Other LE Meta subevent (connection complete) and other events: 0. */
  const uint8_t conn[] = {0x04, 0x3E, 0x02, 0x01, 0x00};
  assert(ble_hci_parse_adv_report(conn, sizeof(conn), r, 2) == 0);
  const uint8_t cc[] = {0x04, 0x0E, 0x04, 0x05, 0x0C, 0x20, 0x00};
  assert(ble_hci_parse_adv_report(cc, sizeof(cc), r, 2) == 0);
}

int main(void) {
  RUN(test_scan_params_command_bytes);
  RUN(test_other_commands);
  RUN(test_scan_timing_clamps);
  RUN(test_cmd_complete_and_status);
  RUN(test_single_report);
  RUN(test_two_reports_interleaved_and_cap);
  RUN(test_malformed_reports_rejected);
  printf("ble_hci: PASS\n");
  return 0;
}
