// Passive BLE scanner, HCI half (t_6215e36e). Pure C, no ESP-IDF: builds the
// four HCI commands the scanner sends over VHCI and parses the controller
// events it gets back. Host-tested in tests/host/test_ble_hci.c.
//
// Written from the Bluetooth Core spec (Vol 4 Part E §7.3.1, §7.3.2, §7.8.10,
// §7.8.11, §7.7.14, §7.7.15, §7.7.65.2). No ESPHome or Espressif code copied.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// H4 packet indicator VHCI prepends to every packet.
#define BLE_HCI_H4_CMD 0x01
#define BLE_HCI_H4_EVT 0x04

#define BLE_HCI_OP_RESET 0x0C03
#define BLE_HCI_OP_SET_EVENT_MASK 0x0C01
#define BLE_HCI_OP_LE_SET_SCAN_PARAMS 0x200B
#define BLE_HCI_OP_LE_SET_SCAN_ENABLE 0x200C

// Legacy advertising PDUs carry at most 31 bytes of AD data.
#define BLE_ADV_DATA_MAX 31
// Largest command we build (LE Set Scan Parameters: 1+3+7).
#define BLE_HCI_CMD_MAX 16

// One received advertisement, in the shape the ESPHome raw-advertisement
// message wants. `address` is the 48-bit device address as an integer, most
// significant byte = first octet of the printed form (AA in AA:BB:..).
typedef struct {
  uint64_t address;
  int8_t rssi;           // dBm; 127 = controller had no reading
  uint8_t address_type;  // HCI: 0 public, 1 random, 2/3 resolved identity
  uint8_t event_type;    // 0 ADV_IND .. 4 SCAN_RSP
  uint8_t data_len;      // 0..BLE_ADV_DATA_MAX
  uint8_t data[BLE_ADV_DATA_MAX];
} BleAdvReport;

// Scan timing in 0.625 ms units, clamped to the spec range (0x0004..0x4000)
// with window <= interval. Returns 0 when the inputs were usable as given, 1
// when anything was clamped.
int ble_hci_scan_timing(uint32_t interval_ms, uint32_t window_ms,
                        uint16_t *interval_units, uint16_t *window_units);

// Command builders. Each writes a complete H4 command packet into `buf`
// (capacity >= BLE_HCI_CMD_MAX) and returns its length.
size_t ble_hci_cmd_reset(uint8_t *buf);
// Event mask = spec default plus LE Meta (bit 61), so LE advertising reports
// reach the host.
size_t ble_hci_cmd_set_event_mask(uint8_t *buf);
// passive=1 sends no SCAN_REQ (no transmissions, no scan responses).
// Own address public, filter policy 0 (accept all undirected).
size_t ble_hci_cmd_le_set_scan_params(uint8_t *buf, int passive,
                                      uint16_t interval_units,
                                      uint16_t window_units);
// filter_duplicates is always 0: RSSI tracking (Bermuda) needs every report.
size_t ble_hci_cmd_le_set_scan_enable(uint8_t *buf, int enable);

// Command Complete (0x0E) or Command Status (0x0F) event. Returns 1 and fills
// opcode/status when `pkt` is one, 0 when it is another event, -1 when it is
// a malformed one. `pkt` starts at the H4 indicator.
int ble_hci_parse_cmd_result(const uint8_t *pkt, size_t len, uint16_t *opcode,
                             uint8_t *status);

// LE Advertising Report (LE Meta 0x3E, subevent 0x02). Reports are laid out
// one after another (event type, address type, address, length, data, RSSI),
// the layout NimBLE and BlueZ parse. Writes up to `max_out` reports and
// returns how many; 0 for any other event; -1 when the packet claims to be an
// advertising report but is truncated or inconsistent (nothing is written
// past the first bad report).
int ble_hci_parse_adv_report(const uint8_t *pkt, size_t len, BleAdvReport *out,
                             size_t max_out);

#ifdef __cplusplus
}
#endif
