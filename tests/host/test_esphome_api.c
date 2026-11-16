/* Host tests for the minimal ESPHome native API server (t_6215e36e).
 *   ./tests/host/run_ble_proxy_tests.sh
 * Golden payloads were serialized by Google protobuf from aioesphomeapi's
 * api_pb2 (the classes Home Assistant decodes with), not by the code under
 * test. The full client handshake is covered against the real library by
 * run_esphome_api_interop_tests.sh.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "esphome_api.h"

#define RUN(fn)               \
  do {                        \
    fn();                     \
    printf("ok - %s\n", #fn); \
  } while (0)

static const EspApiDeviceInfo kInfo = {
    .name = "xvf3800-a6cc4c",
    .friendly_name = "XVF3800 a6cc4c",
    .mac_address = "dc:b4:d9:38:a6:cc",
    .bt_mac_address = "dc:b4:d9:38:a6:ce",
    .model = "xiao-esp32-s3",
    .manufacturer = "Espressif",
    .project_name = "kyzcreig.pipecat-house-satellite",
    .project_version = "abc1234",
    .esphome_version = "pipecat-abc1234",
    .compilation_time = "",
    .server_info = "srv",
};

/* Frame + feed helper: one request with an empty body. */
static int feed_req(EspApiSession *s, uint32_t type, uint8_t *out, size_t cap,
                    size_t *out_len) {
  uint8_t req[8];
  size_t n = espapi_frame(req, sizeof(req), type, NULL, 0);
  assert(n >= 3);
  return espapi_session_feed(s, &kInfo, req, n, out, cap, out_len);
}

static void test_varint_and_frame(void) {
  uint8_t b[16];
  assert(espapi_put_varint(b, 0) == 1 && b[0] == 0);
  assert(espapi_put_varint(b, 300) == 2 && b[0] == 0xAC && b[1] == 0x02);
  /* Frame: 0x00, len, type, payload. Type 93 fits one byte. */
  const uint8_t pl[2] = {0xAA, 0xBB};
  size_t n = espapi_frame(b, sizeof(b), 93, pl, 2);
  const uint8_t want[] = {0x00, 0x02, 0x5D, 0xAA, 0xBB};
  assert(n == sizeof(want) && memcmp(b, want, n) == 0);
  assert(espapi_frame(b, 4, 93, pl, 2) == 0); /* does not fit */
}

static void test_hello_golden(void) {
  EspApiSession s;
  espapi_session_reset(&s);
  uint8_t out[256];
  size_t out_len = 0;
  /* HelloRequest{client_info="HA"}: 0a 02 48 41 */
  const uint8_t req[] = {0x00, 0x04, 0x01, 0x0A, 0x02, 0x48, 0x41};
  assert(espapi_session_feed(&s, &kInfo, req, sizeof(req), out, sizeof(out),
                             &out_len) == ESPAPI_OK);
  const uint8_t want_pl[] = {0x08, 0x01, 0x10, 0x0A, 0x1A, 0x03, 0x73,
                             0x72, 0x76, 0x22, 0x0E, 0x78, 0x76, 0x66,
                             0x33, 0x38, 0x30, 0x30, 0x2D, 0x61, 0x36,
                             0x63, 0x63, 0x34, 0x63};
  assert(out_len == 3 + sizeof(want_pl));
  assert(out[0] == 0x00 && out[1] == sizeof(want_pl) && out[2] == 0x02);
  assert(memcmp(out + 3, want_pl, sizeof(want_pl)) == 0);
  assert(s.hello_done);
}

static void test_ping_disconnect_list(void) {
  EspApiSession s;
  espapi_session_reset(&s);
  uint8_t out[64];
  size_t out_len = 0;
  assert(feed_req(&s, ESPAPI_MSG_PING_REQ, out, sizeof(out), &out_len) ==
         ESPAPI_OK);
  assert(out_len == 3 && out[0] == 0 && out[1] == 0 && out[2] == 8);
  out_len = 0;
  assert(feed_req(&s, ESPAPI_MSG_LIST_ENTITIES_REQ, out, sizeof(out),
                  &out_len) == ESPAPI_OK);
  assert(out_len == 3 && out[2] == 19);
  out_len = 0;
  /* Subscriptions with nothing to report produce no bytes. */
  assert(feed_req(&s, ESPAPI_MSG_SUBSCRIBE_STATES, out, sizeof(out),
                  &out_len) == ESPAPI_OK);
  assert(feed_req(&s, ESPAPI_MSG_SUBSCRIBE_HA_STATES, out, sizeof(out),
                  &out_len) == ESPAPI_OK);
  assert(out_len == 0);
  assert(feed_req(&s, ESPAPI_MSG_DISCONNECT_REQ, out, sizeof(out),
                  &out_len) == ESPAPI_CLOSE);
  assert(out_len == 3 && out[2] == 6);
}

static void test_byte_by_byte_and_pipelined(void) {
  /* DeviceInfoRequest + ListEntitiesRequest in one write (aioesphomeapi
   * sends them that way), delivered one byte at a time. */
  const uint8_t req[] = {0x00, 0x00, 0x09, 0x00, 0x00, 0x0B};
  EspApiSession s;
  espapi_session_reset(&s);
  uint8_t out[600];
  size_t out_len = 0;
  for (size_t i = 0; i < sizeof(req); ++i)
    assert(espapi_session_feed(&s, &kInfo, req + i, 1, out, sizeof(out),
                               &out_len) == ESPAPI_OK);
  assert(s.frames_in == 2 && s.rx_len == 0);
  /* First reply is DeviceInfoResponse (type 10), then ListEntitiesDone. Its
   * body is over 127 bytes, so the length varint takes two bytes. */
  assert(out[0] == 0x00 && (out[1] & 0x80) && out[3] == 10);
  const size_t body = (size_t)(out[1] & 0x7F) | ((size_t)out[2] << 7);
  const size_t first = 4 + body;
  assert(out_len == first + 3 && out[first + 2] == 19);
  /* bluetooth_proxy_feature_flags (field 15, varint) = 0x21 is present. */
  int found = 0;
  for (size_t i = 4; i + 1 < first; ++i)
    if (out[i] == 0x78 && out[i + 1] == 0x21)
      found = 1;
  assert(found);
}

static void test_subscribe_flags(void) {
  EspApiSession s;
  espapi_session_reset(&s);
  uint8_t out[16];
  size_t out_len = 0;
  /* flags=1 (raw) */
  const uint8_t raw[] = {0x00, 0x02, 0x42, 0x08, 0x01};
  assert(espapi_session_feed(&s, &kInfo, raw, sizeof(raw), out, sizeof(out),
                             &out_len) == ESPAPI_OK);
  assert(s.subscribed && out_len == 0);
  /* Unsubscribe */
  assert(feed_req(&s, ESPAPI_MSG_UNSUBSCRIBE_BLE_ADV, out, sizeof(out),
                  &out_len) == ESPAPI_OK);
  assert(!s.subscribed);
  /* flags=0 (legacy parsed format, not implemented): not subscribed. */
  const uint8_t legacy[] = {0x00, 0x00, 0x42};
  assert(espapi_session_feed(&s, &kInfo, legacy, sizeof(legacy), out,
                             sizeof(out), &out_len) == ESPAPI_OK);
  assert(!s.subscribed);
  /* Malformed body (truncated varint) is a protocol error. */
  const uint8_t bad[] = {0x00, 0x01, 0x42, 0x08};
  assert(espapi_session_feed(&s, &kInfo, bad, sizeof(bad), out, sizeof(out),
                             &out_len) == ESPAPI_ERROR);
}

static void test_protocol_errors(void) {
  EspApiSession s;
  uint8_t out[64];
  size_t out_len = 0;
  /* Noise client hello starts with 0x01. */
  espapi_session_reset(&s);
  const uint8_t noise[] = {0x01, 0x00, 0x00};
  assert(espapi_session_feed(&s, &kInfo, noise, sizeof(noise), out,
                             sizeof(out), &out_len) == ESPAPI_ERROR);
  /* Declared length above what we accept. */
  espapi_session_reset(&s);
  const uint8_t big[] = {0x00, 0x80, 0x08, 0x07}; /* len 1024 */
  assert(espapi_session_feed(&s, &kInfo, big, sizeof(big), out, sizeof(out),
                             &out_len) == ESPAPI_ERROR);
  /* Over-long varint. */
  espapi_session_reset(&s);
  const uint8_t vlong[] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x01};
  assert(espapi_session_feed(&s, &kInfo, vlong, sizeof(vlong), out,
                             sizeof(out), &out_len) == ESPAPI_ERROR);
  /* A flood that never completes a frame overflows the buffer. */
  espapi_session_reset(&s);
  uint8_t junk[ESPAPI_RX_MAX + 1];
  memset(junk, 0, sizeof(junk));
  assert(espapi_session_feed(&s, &kInfo, junk, sizeof(junk), out, sizeof(out),
                             &out_len) == ESPAPI_ERROR);
  /* Unknown message types are counted and ignored. */
  espapi_session_reset(&s);
  out_len = 0;
  assert(feed_req(&s, 200, out, sizeof(out), &out_len) == ESPAPI_OK);
  assert(out_len == 0 && s.unknown_in == 1);
}

static void test_raw_adverts_golden(void) {
  BleAdvReport r[2];
  memset(r, 0, sizeof(r));
  r[0].address = 0xC01122334455ULL;
  r[0].rssi = -67;
  r[0].address_type = 1;
  r[0].data_len = 3;
  memcpy(r[0].data, "\x02\x01\x06", 3);
  r[1].address = 1;
  r[1].rssi = 127;
  uint8_t out[ESPAPI_TX_MAX];
  size_t n = espapi_encode_raw_adverts(r, 2, out, sizeof(out));
  const uint8_t want_pl[] = {0x0A, 0x12, 0x08, 0xD5, 0x88, 0xCD, 0x91,
                             0x92, 0x82, 0x30, 0x10, 0x85, 0x01, 0x18,
                             0x01, 0x22, 0x03, 0x02, 0x01, 0x06, 0x0A,
                             0x05, 0x08, 0x01, 0x10, 0xFE, 0x01};
  assert(n == 3 + sizeof(want_pl));
  assert(out[0] == 0x00 && out[1] == sizeof(want_pl) && out[2] == 93);
  assert(memcmp(out + 3, want_pl, sizeof(want_pl)) == 0);
}

static void test_full_batch_fits(void) {
  BleAdvReport r[ESPAPI_ADV_BATCH_MAX];
  memset(r, 0xFF, sizeof(r));
  for (int i = 0; i < ESPAPI_ADV_BATCH_MAX; ++i) {
    r[i].data_len = BLE_ADV_DATA_MAX;
    r[i].rssi = -128;
    r[i].address = 0xFFFFFFFFFFFFULL;
  }
  uint8_t out[ESPAPI_TX_MAX];
  size_t n = espapi_encode_raw_adverts(r, ESPAPI_ADV_BATCH_MAX, out,
                                       sizeof(out));
  assert(n > 0 && n <= ESPAPI_TX_MAX);
  assert(espapi_encode_raw_adverts(r, ESPAPI_ADV_BATCH_MAX + 1, out,
                                   sizeof(out)) == 0);
  assert(espapi_encode_raw_adverts(r, 0, out, sizeof(out)) == 0);
  printf("   worst-case batch frame: %zu bytes\n", n);
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  RUN(test_varint_and_frame);
  RUN(test_hello_golden);
  RUN(test_ping_disconnect_list);
  RUN(test_byte_by_byte_and_pipelined);
  RUN(test_subscribe_flags);
  RUN(test_protocol_errors);
  RUN(test_raw_adverts_golden);
  RUN(test_full_batch_fits);
  printf("esphome_api: PASS\n");
  return 0;
}
