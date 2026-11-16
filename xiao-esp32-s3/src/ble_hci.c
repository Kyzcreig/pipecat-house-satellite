// Passive BLE scanner, HCI half. See ble_hci.h.
#include "ble_hci.h"

#include <string.h>

#define BLE_HCI_EVT_CMD_COMPLETE 0x0E
#define BLE_HCI_EVT_CMD_STATUS 0x0F
#define BLE_HCI_EVT_LE_META 0x3E
#define BLE_HCI_LE_SUBEVT_ADV_REPORT 0x02

#define BLE_SCAN_UNITS_MIN 0x0004u
#define BLE_SCAN_UNITS_MAX 0x4000u

static uint32_t ms_to_units(uint32_t ms) {
  // 1 unit = 0.625 ms; round to nearest.
  return (ms * 8u + 2u) / 5u;
}

static uint32_t clamp_units(uint32_t u, int *clamped) {
  if (u < BLE_SCAN_UNITS_MIN) {
    *clamped = 1;
    return BLE_SCAN_UNITS_MIN;
  }
  if (u > BLE_SCAN_UNITS_MAX) {
    *clamped = 1;
    return BLE_SCAN_UNITS_MAX;
  }
  return u;
}

int ble_hci_scan_timing(uint32_t interval_ms, uint32_t window_ms,
                        uint16_t *interval_units, uint16_t *window_units) {
  int clamped = 0;
  // Bound the ms inputs first so the multiply cannot wrap.
  if (interval_ms > 20000u) {
    interval_ms = 20000u;
    clamped = 1;
  }
  if (window_ms > 20000u) {
    window_ms = 20000u;
    clamped = 1;
  }
  uint32_t iv = clamp_units(ms_to_units(interval_ms), &clamped);
  uint32_t wn = clamp_units(ms_to_units(window_ms), &clamped);
  if (wn > iv) {
    wn = iv;
    clamped = 1;
  }
  *interval_units = (uint16_t)iv;
  *window_units = (uint16_t)wn;
  return clamped;
}

static size_t put_cmd_header(uint8_t *buf, uint16_t opcode, uint8_t plen) {
  buf[0] = BLE_HCI_H4_CMD;
  buf[1] = (uint8_t)(opcode & 0xFF);
  buf[2] = (uint8_t)(opcode >> 8);
  buf[3] = plen;
  return 4;
}

size_t ble_hci_cmd_reset(uint8_t *buf) {
  return put_cmd_header(buf, BLE_HCI_OP_RESET, 0);
}

size_t ble_hci_cmd_set_event_mask(uint8_t *buf) {
  // Spec default 0x00001FFFFFFFFFFF | bit 61 (LE Meta), little-endian.
  static const uint8_t mask[8] = {0xFF, 0xFF, 0xFF, 0xFF,
                                  0xFF, 0x1F, 0x00, 0x20};
  size_t n = put_cmd_header(buf, BLE_HCI_OP_SET_EVENT_MASK, sizeof(mask));
  memcpy(buf + n, mask, sizeof(mask));
  return n + sizeof(mask);
}

size_t ble_hci_cmd_le_set_scan_params(uint8_t *buf, int passive,
                                      uint16_t interval_units,
                                      uint16_t window_units) {
  size_t n = put_cmd_header(buf, BLE_HCI_OP_LE_SET_SCAN_PARAMS, 7);
  buf[n++] = passive ? 0x00 : 0x01;  // LE_Scan_Type
  buf[n++] = (uint8_t)(interval_units & 0xFF);
  buf[n++] = (uint8_t)(interval_units >> 8);
  buf[n++] = (uint8_t)(window_units & 0xFF);
  buf[n++] = (uint8_t)(window_units >> 8);
  buf[n++] = 0x00;  // Own_Address_Type: public
  buf[n++] = 0x00;  // Scanning_Filter_Policy: accept all
  return n;
}

size_t ble_hci_cmd_le_set_scan_enable(uint8_t *buf, int enable) {
  size_t n = put_cmd_header(buf, BLE_HCI_OP_LE_SET_SCAN_ENABLE, 2);
  buf[n++] = enable ? 0x01 : 0x00;
  buf[n++] = 0x00;  // Filter_Duplicates off
  return n;
}

int ble_hci_parse_cmd_result(const uint8_t *pkt, size_t len, uint16_t *opcode,
                             uint8_t *status) {
  if (len < 3 || pkt[0] != BLE_HCI_H4_EVT)
    return 0;
  const uint8_t code = pkt[1];
  const size_t plen = pkt[2];
  if (code != BLE_HCI_EVT_CMD_COMPLETE && code != BLE_HCI_EVT_CMD_STATUS)
    return 0;
  if (len < 3 + plen)
    return -1;
  const uint8_t *p = pkt + 3;
  if (code == BLE_HCI_EVT_CMD_COMPLETE) {
    // Num_HCI_Command_Packets, Opcode(2), Return_Parameters (status first).
    if (plen < 4)
      return -1;
    *opcode = (uint16_t)(p[1] | (p[2] << 8));
    *status = p[3];
  } else {
    // Status, Num_HCI_Command_Packets, Opcode(2).
    if (plen < 4)
      return -1;
    *status = p[0];
    *opcode = (uint16_t)(p[2] | (p[3] << 8));
  }
  return 1;
}

int ble_hci_parse_adv_report(const uint8_t *pkt, size_t len, BleAdvReport *out,
                             size_t max_out) {
  if (len < 3 || pkt[0] != BLE_HCI_H4_EVT || pkt[1] != BLE_HCI_EVT_LE_META)
    return 0;
  const size_t plen = pkt[2];
  if (plen < 1 || len < 3 + plen)
    return -1;
  const uint8_t *p = pkt + 3;
  const uint8_t *end = p + plen;
  if (p[0] != BLE_HCI_LE_SUBEVT_ADV_REPORT)
    return 0;
  if (plen < 2)
    return -1;
  const unsigned num = p[1];
  p += 2;
  if (num == 0)
    return -1;
  size_t written = 0;
  for (unsigned i = 0; i < num; ++i) {
    // event_type(1) addr_type(1) addr(6) data_len(1) data(n) rssi(1)
    if ((size_t)(end - p) < 9)
      return -1;
    const uint8_t evt = p[0];
    const uint8_t atype = p[1];
    const uint8_t *addr = p + 2;
    const uint8_t dlen = p[8];
    p += 9;
    if (dlen > BLE_ADV_DATA_MAX || (size_t)(end - p) < (size_t)dlen + 1)
      return -1;
    if (written < max_out) {
      BleAdvReport *r = &out[written];
      uint64_t a = 0;
      // HCI carries the address little-endian: addr[5] is the first octet.
      for (int b = 5; b >= 0; --b)
        a = (a << 8) | addr[b];
      r->address = a;
      r->address_type = atype;
      r->event_type = evt;
      r->data_len = dlen;
      memcpy(r->data, p, dlen);
      r->rssi = (int8_t)p[dlen];
      ++written;
    }
    p += dlen + 1;
  }
  return (int)written;
}
