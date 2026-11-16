// Minimal ESPHome native API server, protocol half (t_6215e36e). Pure C, no
// ESP-IDF, no sockets: bytes in, bytes out. Host-tested in
// tests/host/test_esphome_api.c and against the real Home Assistant client
// library (aioesphomeapi, pinned to HA 2026.10.2's version) by
// tests/host/run_esphome_api_interop_tests.sh.
//
// Scope: plaintext frames only (no Noise encryption, no password); the
// messages Home Assistant sends while connecting to a device with no
// entities; device info advertising a PASSIVE, raw-advertisement Bluetooth
// proxy; raw advertisement batches out. Everything else is ignored.
//
// Reimplemented from the public wire format (message ids and field numbers as
// shipped in aioesphomeapi's api_pb2, MIT). No ESPHome C++ (GPLv3) copied.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ble_hci.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESPAPI_PORT 6053
// The API version we answer Hello with. 1.10 is the first with raw
// advertisements and feature flags read from bluetooth_proxy_feature_flags,
// and stays below 1.15, where clients start asking DeviceCapabilities.
#define ESPAPI_VERSION_MAJOR 1
#define ESPAPI_VERSION_MINOR 10

// Message type ids (api.proto).
enum {
  ESPAPI_MSG_HELLO_REQ = 1,
  ESPAPI_MSG_HELLO_RESP = 2,
  ESPAPI_MSG_AUTH_REQ = 3,
  ESPAPI_MSG_AUTH_RESP = 4,
  ESPAPI_MSG_DISCONNECT_REQ = 5,
  ESPAPI_MSG_DISCONNECT_RESP = 6,
  ESPAPI_MSG_PING_REQ = 7,
  ESPAPI_MSG_PING_RESP = 8,
  ESPAPI_MSG_DEVICE_INFO_REQ = 9,
  ESPAPI_MSG_DEVICE_INFO_RESP = 10,
  ESPAPI_MSG_LIST_ENTITIES_REQ = 11,
  ESPAPI_MSG_LIST_ENTITIES_DONE = 19,
  ESPAPI_MSG_SUBSCRIBE_STATES = 20,
  ESPAPI_MSG_SUBSCRIBE_LOGS = 28,
  ESPAPI_MSG_SUBSCRIBE_HA_SERVICES = 34,
  ESPAPI_MSG_SUBSCRIBE_HA_STATES = 38,
  ESPAPI_MSG_SUBSCRIBE_BLE_ADV = 66,
  ESPAPI_MSG_BLE_CONN_FREE_REQ = 80,
  ESPAPI_MSG_BLE_CONN_FREE_RESP = 81,
  ESPAPI_MSG_UNSUBSCRIBE_BLE_ADV = 87,
  ESPAPI_MSG_BLE_RAW_ADVERTISEMENTS = 93,
};

// BluetoothProxyFeature bits we claim: PASSIVE_SCAN | RAW_ADVERTISEMENTS.
// No ACTIVE_CONNECTIONS (no GATT), no FEATURE_STATE_AND_MODE (HA cannot
// switch us to active scanning).
#define ESPAPI_BLE_FEATURES 0x21u
// SubscribeBluetoothLEAdvertisementsRequest.flags bit for raw adverts.
#define ESPAPI_SUB_FLAG_RAW 0x01u

// Incoming frames larger than this close the connection. Home Assistant's
// requests to a device with no entities are a few dozen bytes.
#define ESPAPI_RX_MAX 512
// Advertisements per BluetoothLERawAdvertisementsResponse.
#define ESPAPI_ADV_BATCH_MAX 16
// Worst case one batch frame: 16 x (tag+10 addr, tag+5 rssi, tag+1 type,
// tag+1 len+31 data, tag+2 len) + header.
#define ESPAPI_TX_MAX 1024

typedef struct {
  const char *name;  // must equal the mDNS hostname (HA checks it)
  const char *friendly_name;
  const char *mac_address;     // "aa:bb:cc:dd:ee:ff", Wi-Fi STA (HA unique id)
  const char *bt_mac_address;  // "aa:bb:..", the scanner source address
  const char *model;
  const char *manufacturer;
  const char *project_name;  // set, so HA skips its ESPHome-version nag
  const char *project_version;
  const char *esphome_version;  // what we put in the version field
  const char *compilation_time;
  const char *server_info;
} EspApiDeviceInfo;

typedef struct {
  uint8_t rx[ESPAPI_RX_MAX];
  size_t rx_len;
  bool hello_done;
  bool subscribed;  // raw advertisement subscriber
  uint32_t frames_in;
  uint32_t pings_in;
  uint32_t unknown_in;
} EspApiSession;

enum {
  ESPAPI_OK = 0,     // keep going
  ESPAPI_CLOSE = 1,  // flush `out`, then close (client asked to disconnect)
  ESPAPI_ERROR = -1  // protocol error: close without flushing
};

void espapi_session_reset(EspApiSession *s);

// Feed received bytes. Complete frames are handled in order; replies are
// appended to out[*out_len..out_cap). Partial frames stay buffered.
int espapi_session_feed(EspApiSession *s, const EspApiDeviceInfo *info,
                        const uint8_t *in, size_t n, uint8_t *out,
                        size_t out_cap, size_t *out_len);

// One BluetoothLERawAdvertisementsResponse frame carrying `n` (<=
// ESPAPI_ADV_BATCH_MAX) reports. Returns the frame length, 0 if `cap` is too
// small.
size_t espapi_encode_raw_adverts(const BleAdvReport *adv, size_t n,
                                 uint8_t *out, size_t cap);

// Frame/varint primitives, exposed for the tests.
size_t espapi_put_varint(uint8_t *out, uint64_t v);
size_t espapi_frame(uint8_t *out, size_t cap, uint32_t type,
                    const uint8_t *payload, size_t len);

#ifdef __cplusplus
}
#endif
