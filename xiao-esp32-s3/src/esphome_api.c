// Minimal ESPHome native API server, protocol half. See esphome_api.h.
#include "esphome_api.h"

#include <string.h>

// ---- protobuf writer -------------------------------------------------------

size_t espapi_put_varint(uint8_t *out, uint64_t v) {
  size_t n = 0;
  while (v >= 0x80) {
    out[n++] = (uint8_t)(v | 0x80);
    v >>= 7;
  }
  out[n++] = (uint8_t)v;
  return n;
}

static size_t varint_len(uint64_t v) {
  size_t n = 1;
  while (v >= 0x80) {
    v >>= 7;
    ++n;
  }
  return n;
}

typedef struct {
  uint8_t *p;
  size_t cap;
  size_t len;
  bool overflow;
} PbBuf;

static void pb_raw(PbBuf *b, const void *src, size_t n) {
  if (b->overflow || b->len + n > b->cap) {
    b->overflow = true;
    return;
  }
  memcpy(b->p + b->len, src, n);
  b->len += n;
}

static void pb_varint(PbBuf *b, uint64_t v) {
  uint8_t tmp[10];
  pb_raw(b, tmp, espapi_put_varint(tmp, v));
}

// proto3: default values (0, "", empty) are not written.
static void pb_field_varint(PbBuf *b, uint32_t field, uint64_t v) {
  if (v == 0)
    return;
  pb_varint(b, ((uint64_t)field << 3) | 0);
  pb_varint(b, v);
}

static void pb_field_bytes(PbBuf *b, uint32_t field, const void *data,
                           size_t n) {
  if (n == 0)
    return;
  pb_varint(b, ((uint64_t)field << 3) | 2);
  pb_varint(b, n);
  pb_raw(b, data, n);
}

static void pb_field_str(PbBuf *b, uint32_t field, const char *s) {
  if (s)
    pb_field_bytes(b, field, s, strlen(s));
}

static uint32_t zigzag32(int32_t v) {
  return ((uint32_t)v << 1) ^ (uint32_t)(v >> 31);
}

// ---- frames ----------------------------------------------------------------

// Plaintext frame: 0x00, varint(payload length), varint(type), payload.
size_t espapi_frame(uint8_t *out, size_t cap, uint32_t type,
                    const uint8_t *payload, size_t len) {
  const size_t need = 1 + varint_len(len) + varint_len(type) + len;
  if (need > cap)
    return 0;
  size_t n = 0;
  out[n++] = 0x00;
  n += espapi_put_varint(out + n, len);
  n += espapi_put_varint(out + n, type);
  if (len)
    memcpy(out + n, payload, len);
  return n + len;
}

// Append one frame to out[*out_len..cap). False if it does not fit.
static bool emit(uint8_t *out, size_t cap, size_t *out_len, uint32_t type,
                 const PbBuf *body) {
  if (body && body->overflow)
    return false;
  size_t n = espapi_frame(out + *out_len, cap - *out_len, type,
                          body ? body->p : NULL, body ? body->len : 0);
  if (n == 0)
    return false;
  *out_len += n;
  return true;
}

// ---- protobuf reader (only what the requests need) -------------------------

// Read a varint of at most `max_bytes`. Returns bytes consumed, 0 if the
// buffer ends first, -1 if it is longer than `max_bytes`.
static int read_varint(const uint8_t *p, size_t n, size_t max_bytes,
                       uint64_t *v) {
  uint64_t r = 0;
  for (size_t i = 0; i < n; ++i) {
    if (i >= max_bytes)
      return -1;
    r |= (uint64_t)(p[i] & 0x7F) << (7 * i);
    if ((p[i] & 0x80) == 0) {
      *v = r;
      return (int)(i + 1);
    }
  }
  return n >= max_bytes ? -1 : 0;
}

// Value of varint field `field` in a message, or `dflt` when absent. Unknown
// fields of any wire type are skipped. Returns false on a malformed message.
static bool pb_get_varint(const uint8_t *p, size_t n, uint32_t field,
                          uint64_t dflt, uint64_t *out) {
  *out = dflt;
  size_t i = 0;
  while (i < n) {
    uint64_t key, v;
    int k = read_varint(p + i, n - i, 10, &key);
    if (k <= 0)
      return false;
    i += (size_t)k;
    const uint32_t wt = (uint32_t)(key & 7);
    const uint64_t fn = key >> 3;
    switch (wt) {
      case 0:
        k = read_varint(p + i, n - i, 10, &v);
        if (k <= 0)
          return false;
        i += (size_t)k;
        if (fn == field)
          *out = v;
        break;
      case 1:
        if (n - i < 8)
          return false;
        i += 8;
        break;
      case 2:
        k = read_varint(p + i, n - i, 10, &v);
        if (k <= 0 || v > n - i - (size_t)k)
          return false;
        i += (size_t)k + (size_t)v;
        break;
      case 5:
        if (n - i < 4)
          return false;
        i += 4;
        break;
      default:
        return false;
    }
  }
  return true;
}

// ---- session ---------------------------------------------------------------

void espapi_session_reset(EspApiSession *s) {
  memset(s, 0, sizeof(*s));
}

static bool reply_hello(const EspApiDeviceInfo *info, uint8_t *out, size_t cap,
                        size_t *out_len) {
  uint8_t tmp[160];
  PbBuf b = {tmp, sizeof(tmp), 0, false};
  pb_field_varint(&b, 1, ESPAPI_VERSION_MAJOR);
  pb_field_varint(&b, 2, ESPAPI_VERSION_MINOR);
  pb_field_str(&b, 3, info->server_info);
  pb_field_str(&b, 4, info->name);
  return emit(out, cap, out_len, ESPAPI_MSG_HELLO_RESP, &b);
}

static bool reply_device_info(const EspApiDeviceInfo *info, uint8_t *out,
                              size_t cap, size_t *out_len) {
  uint8_t tmp[480];
  PbBuf b = {tmp, sizeof(tmp), 0, false};
  pb_field_str(&b, 2, info->name);
  pb_field_str(&b, 3, info->mac_address);
  pb_field_str(&b, 4, info->esphome_version);
  pb_field_str(&b, 5, info->compilation_time);
  pb_field_str(&b, 6, info->model);
  pb_field_str(&b, 8, info->project_name);
  pb_field_str(&b, 9, info->project_version);
  pb_field_str(&b, 12, info->manufacturer);
  pb_field_str(&b, 13, info->friendly_name);
  pb_field_varint(&b, 15, ESPAPI_BLE_FEATURES);
  pb_field_str(&b, 18, info->bt_mac_address);
  return emit(out, cap, out_len, ESPAPI_MSG_DEVICE_INFO_RESP, &b);
}

// Handle one complete request. Returns ESPAPI_OK / ESPAPI_CLOSE / ESPAPI_ERROR.
static int handle(EspApiSession *s, const EspApiDeviceInfo *info, uint32_t type,
                  const uint8_t *payload, size_t len, uint8_t *out, size_t cap,
                  size_t *out_len) {
  bool ok = true;
  uint64_t v = 0;
  s->frames_in++;
  switch (type) {
    case ESPAPI_MSG_HELLO_REQ:
      s->hello_done = true;
      ok = reply_hello(info, out, cap, out_len);
      break;
    case ESPAPI_MSG_AUTH_REQ:
      // No password: an empty AuthenticationResponse (invalid_password=false).
      ok = emit(out, cap, out_len, ESPAPI_MSG_AUTH_RESP, NULL);
      break;
    case ESPAPI_MSG_DISCONNECT_REQ:
      if (!emit(out, cap, out_len, ESPAPI_MSG_DISCONNECT_RESP, NULL))
        return ESPAPI_ERROR;
      return ESPAPI_CLOSE;
    case ESPAPI_MSG_DISCONNECT_RESP:
      return ESPAPI_CLOSE;
    case ESPAPI_MSG_PING_REQ:
      s->pings_in++;
      ok = emit(out, cap, out_len, ESPAPI_MSG_PING_RESP, NULL);
      break;
    case ESPAPI_MSG_PING_RESP:
      break;
    case ESPAPI_MSG_DEVICE_INFO_REQ:
      ok = reply_device_info(info, out, cap, out_len);
      break;
    case ESPAPI_MSG_LIST_ENTITIES_REQ:
      // No entities: the scanner is the whole device.
      ok = emit(out, cap, out_len, ESPAPI_MSG_LIST_ENTITIES_DONE, NULL);
      break;
    case ESPAPI_MSG_SUBSCRIBE_STATES:
    case ESPAPI_MSG_SUBSCRIBE_LOGS:
    case ESPAPI_MSG_SUBSCRIBE_HA_SERVICES:
    case ESPAPI_MSG_SUBSCRIBE_HA_STATES:
      // Nothing to report and nothing to ask for.
      break;
    case ESPAPI_MSG_SUBSCRIBE_BLE_ADV:
      if (!pb_get_varint(payload, len, 1, 0, &v))
        return ESPAPI_ERROR;
      // Only the raw format is implemented (it is what our feature flags
      // advertise). The newest subscriber on this connection wins.
      s->subscribed = (v & ESPAPI_SUB_FLAG_RAW) != 0;
      break;
    case ESPAPI_MSG_UNSUBSCRIBE_BLE_ADV:
      s->subscribed = false;
      break;
    case ESPAPI_MSG_BLE_CONN_FREE_REQ: {
      // Not sent for a passive proxy; answered anyway: 0 free of 0.
      ok = emit(out, cap, out_len, ESPAPI_MSG_BLE_CONN_FREE_RESP, NULL);
      break;
    }
    default:
      s->unknown_in++;
      break;
  }
  return ok ? ESPAPI_OK : ESPAPI_ERROR;
}

int espapi_session_feed(EspApiSession *s, const EspApiDeviceInfo *info,
                        const uint8_t *in, size_t n, uint8_t *out,
                        size_t out_cap, size_t *out_len) {
  if (n > ESPAPI_RX_MAX - s->rx_len)
    return ESPAPI_ERROR;  // a frame larger than we accept, or a flood
  memcpy(s->rx + s->rx_len, in, n);
  s->rx_len += n;

  size_t pos = 0;
  int rc = ESPAPI_OK;
  while (rc == ESPAPI_OK && s->rx_len - pos >= 3) {
    const uint8_t *p = s->rx + pos;
    const size_t avail = s->rx_len - pos;
    if (p[0] != 0x00)
      return ESPAPI_ERROR;  // not plaintext (a Noise client sends 0x01)
    uint64_t len, type;
    int a = read_varint(p + 1, avail - 1, 4, &len);
    if (a < 0)
      return ESPAPI_ERROR;
    if (a == 0)
      break;
    int b = read_varint(p + 1 + a, avail - 1 - (size_t)a, 4, &type);
    if (b < 0)
      return ESPAPI_ERROR;
    if (b == 0)
      break;
    const size_t hdr = 1 + (size_t)a + (size_t)b;
    if (len > ESPAPI_RX_MAX - hdr)
      return ESPAPI_ERROR;
    if (avail < hdr + len)
      break;
    rc = handle(s, info, (uint32_t)type, p + hdr, (size_t)len, out, out_cap,
                out_len);
    pos += hdr + (size_t)len;
  }
  if (rc == ESPAPI_ERROR)
    return rc;
  memmove(s->rx, s->rx + pos, s->rx_len - pos);
  s->rx_len -= pos;
  return rc;
}

size_t espapi_encode_raw_adverts(const BleAdvReport *adv, size_t n,
                                 uint8_t *out, size_t cap) {
  if (n == 0 || n > ESPAPI_ADV_BATCH_MAX)
    return 0;
  uint8_t body[ESPAPI_TX_MAX];
  PbBuf b = {body, sizeof(body), 0, false};
  for (size_t i = 0; i < n; ++i) {
    uint8_t one[64];
    PbBuf m = {one, sizeof(one), 0, false};
    pb_field_varint(&m, 1, adv[i].address);
    pb_field_varint(&m, 2, zigzag32(adv[i].rssi));
    pb_field_varint(&m, 3, adv[i].address_type);
    pb_field_bytes(&m, 4, adv[i].data, adv[i].data_len);
    if (m.overflow)
      return 0;
    pb_field_bytes(&b, 1, one, m.len);
  }
  if (b.overflow)
    return 0;
  return espapi_frame(out, cap, ESPAPI_MSG_BLE_RAW_ADVERTISEMENTS, body, b.len);
}
