#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../xiao-esp32-s3/components/peer/red_wrap.h"

static uint16_t block_offset(const uint8_t *header) {
  uint32_t value = ((uint32_t)header[1] << 16) |
                   ((uint32_t)header[2] << 8) | header[3];
  return (uint16_t)(value >> 10);
}

static uint16_t block_length(const uint8_t *header) {
  uint32_t value = ((uint32_t)header[1] << 16) |
                   ((uint32_t)header[2] << 8) | header[3];
  return (uint16_t)(value & 0x3ff);
}

int main(void) {
  RedWrapState state;
  uint8_t out[1200];
  const uint8_t p0[] = {0x10, 0x11};
  const uint8_t p1[] = {0x20, 0x21, 0x22};
  const uint8_t p2[] = {0x30};
  const uint8_t p3[] = {0x40, 0x41};
  const uint8_t p4[] = {0x50};

  red_wrap_init(&state, 111, 4);
  int size = red_wrap_packet(&state, p0, sizeof(p0), 0, out, sizeof(out));
  assert(size == 1 + (int)sizeof(p0));
  assert(out[0] == 111);
  assert(memcmp(out + 1, p0, sizeof(p0)) == 0);

  size = red_wrap_packet(&state, p1, sizeof(p1), 960, out, sizeof(out));
  assert(size == 4 + 1 + (int)sizeof(p0) + (int)sizeof(p1));
  assert(out[0] == (uint8_t)(0x80 | 111));
  assert(block_offset(out) == 960);
  assert(block_length(out) == sizeof(p0));
  assert(out[4] == 111);
  assert(memcmp(out + 5, p0, sizeof(p0)) == 0);
  assert(memcmp(out + 5 + sizeof(p0), p1, sizeof(p1)) == 0);

  assert(red_wrap_packet(&state, p2, sizeof(p2), 1920, out, sizeof(out)) > 0);
  assert(red_wrap_packet(&state, p3, sizeof(p3), 2880, out, sizeof(out)) > 0);
  size = red_wrap_packet(&state, p4, sizeof(p4), 3840, out, sizeof(out));
  assert(size > 0);
  for (int i = 0; i < 4; ++i) {
    assert(out[i * 4] == (uint8_t)(0x80 | 111));
    assert(block_offset(out + i * 4) == (uint16_t)((4 - i) * 960));
  }
  assert(out[16] == 111);

  // Redundancy is best-effort under the MTU budget; the primary must always
  // survive when history cannot fit.
  uint8_t large[1000];
  memset(large, 0x7a, sizeof(large));
  red_wrap_init(&state, 111, 4);
  assert(red_wrap_packet(&state, large, sizeof(large), 0, out, sizeof(out)) == 1001);
  size = red_wrap_packet(&state, p4, sizeof(p4), 960, out, 16);
  assert(size == 2);
  assert(out[0] == 111 && out[1] == p4[0]);

  puts("uplink RED wrap host tests: PASS");
  return 0;
}
