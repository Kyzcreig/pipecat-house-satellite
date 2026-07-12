/* red_unwrap.c — RFC 2198 (RED) depacketizer. See red_unwrap.h for contract.
 *
 * Pure byte logic, host-testable (tests/host/test_red_unwrap.c). Fail-safe:
 * ANY malformation returns -1 and the caller treats the payload as bare opus.
 */
#include "red_unwrap.h"

#include <string.h>

int red_unwrap(const uint8_t* payload, size_t size, uint8_t expected_pt,
               RedParsed* out) {
  if (payload == NULL || out == NULL || size < 1) {
    return -1;
  }
  memset(out, 0, sizeof(*out));

  /* Pass 1: walk the header chain (4-byte headers while F=1, then the 1-byte
   * final header). */
  size_t pos = 0;
  size_t data_total = 0;
  while (pos < size && (payload[pos] & 0x80)) {
    if (pos + 4 > size) {
      return -1; /* truncated redundant header */
    }
    if (out->block_count >= RED_MAX_BLOCKS) {
      return -1; /* more blocks than we ever expect: treat as malformed */
    }
    uint8_t pt = payload[pos] & 0x7F;
    if (pt != expected_pt) {
      return -1; /* foreign codec in the chain — we only wrap opus */
    }
    uint32_t v = ((uint32_t)payload[pos + 1] << 16) |
                 ((uint32_t)payload[pos + 2] << 8) | payload[pos + 3];
    RedBlock* b = &out->blocks[out->block_count];
    b->ts_offset = (uint16_t)(v >> 10);   /* 14 bits */
    b->length = (uint16_t)(v & 0x3FF);    /* 10 bits */
    if (b->ts_offset == 0) {
      return -1; /* a redundant block at offset 0 makes no sense */
    }
    data_total += b->length;
    out->block_count++;
    pos += 4;
  }
  if (pos >= size) {
    return -1; /* ran out before the final header */
  }
  if ((payload[pos] & 0x7F) != expected_pt) {
    return -1; /* primary is not opus */
  }
  pos += 1; /* consume final header */

  /* Pass 2: slice block data (oldest-first order, primary last). */
  if (pos + data_total > size) {
    return -1; /* declared block lengths exceed the payload */
  }
  size_t dpos = pos;
  for (int i = 0; i < out->block_count; i++) {
    out->blocks[i].data = payload + dpos;
    dpos += out->blocks[i].length;
  }
  out->primary = payload + dpos;
  out->primary_size = size - dpos;
  if (out->primary_size == 0) {
    return -1; /* empty primary: nothing sane to decode */
  }
  return 0;
}

int red_recover_plan(const RedParsed* parsed, int gap, uint32_t ts_step,
                     int8_t actions[RED_MAX_GAP]) {
  if (parsed == NULL || actions == NULL || gap <= 0 || ts_step == 0) {
    return 0;
  }
  int n = gap > RED_MAX_GAP ? RED_MAX_GAP : gap;
  /* Missing frame i (i=0 oldest) sits (n - i) packets before the primary
   * (the gap frames are the ones IMMEDIATELY preceding this packet). */
  for (int i = 0; i < n; i++) {
    uint32_t want = (uint32_t)(n - i) * ts_step;
    actions[i] = -1;
    if (want <= 0x3FFF) { /* 14-bit ts_offset ceiling */
      for (int b = 0; b < parsed->block_count; b++) {
        if (parsed->blocks[b].ts_offset == want &&
            parsed->blocks[b].length > 0) {
          actions[i] = (int8_t)b;
          break;
        }
      }
    }
  }
  return n;
}
