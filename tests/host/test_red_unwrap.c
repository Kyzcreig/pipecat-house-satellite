/* Host-side unit tests for the RED (RFC 2198) depacketizer + recovery planner.
 *
 * Pure byte logic — builds and runs on ANY host compiler, no ESP-IDF:
 *   ./tests/host/run_red_tests.sh          (from the repo root)
 * which does: cc -I xiao-esp32-s3/components/peer test_red_unwrap.c red_unwrap.c
 *
 * Golden vectors are SHARED with the server-side tests
 * (pipecat-house-voice server/tests/test_red_encapsulation.py) — the server's
 * RedEncapsulator produced these exact bytes; keep both files in sync.
 *
 * Covers (spec test plan): byte-exact parse, dup/reorder handling is exercised
 * in rtp.c (planner here), gap coverage N-1/N-2, malformed inputs -> safe
 * fallback (-1), truncation, foreign PT, oversize declarations, empty primary.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "red_unwrap.h"

#define OPUS_PT 111
#define FRAME_TS 960 /* one 20ms opus frame @48k */

static int tests_run = 0;
#define RUN(fn)                 \
  do {                          \
    fn();                       \
    tests_run++;                \
    printf("ok - %s\n", #fn);   \
  } while (0)

/* Build one 4-byte redundant block header. */
static size_t hdr(uint8_t* out, uint8_t pt, uint16_t ts_offset, uint16_t len) {
  uint32_t v = ((uint32_t)ts_offset << 10) | len;
  out[0] = 0x80 | pt;
  out[1] = (v >> 16) & 0xFF;
  out[2] = (v >> 8) & 0xFF;
  out[3] = v & 0xFF;
  return 4;
}

/* ── golden vectors (mirror server tests) ─────────────────────────────────── */

static void test_primary_only(void) {
  /* server: test_first_packet_primary_only */
  uint8_t payload[] = {OPUS_PT, 0xAA, 0xBB, 0xCC};
  RedParsed p;
  assert(red_unwrap(payload, sizeof(payload), OPUS_PT, &p) == 0);
  assert(p.block_count == 0);
  assert(p.primary_size == 3);
  assert(memcmp(p.primary, "\xAA\xBB\xCC", 3) == 0);
}

static void test_one_redundant_block(void) {
  /* server: test_second_packet_one_redundant_block */
  uint8_t payload[32];
  size_t n = hdr(payload, OPUS_PT, FRAME_TS, 2);
  payload[n++] = OPUS_PT;
  memcpy(payload + n, "\x01\x02", 2); n += 2;         /* N-1 block */
  memcpy(payload + n, "\x03\x04\x05", 3); n += 3;     /* primary */
  RedParsed p;
  assert(red_unwrap(payload, n, OPUS_PT, &p) == 0);
  assert(p.block_count == 1);
  assert(p.blocks[0].ts_offset == FRAME_TS && p.blocks[0].length == 2);
  assert(memcmp(p.blocks[0].data, "\x01\x02", 2) == 0);
  assert(p.primary_size == 3 && memcmp(p.primary, "\x03\x04\x05", 3) == 0);
}

static void test_two_blocks_oldest_first(void) {
  /* server: test_third_packet_two_redundant_blocks_oldest_first */
  uint8_t payload[32];
  size_t n = hdr(payload, OPUS_PT, 2 * FRAME_TS, 1);      /* N-2 oldest */
  n += hdr(payload + n, OPUS_PT, FRAME_TS, 2);            /* N-1 */
  payload[n++] = OPUS_PT;
  payload[n++] = 0x01;                                    /* N-2 data */
  memcpy(payload + n, "\x02\x02", 2); n += 2;             /* N-1 data */
  memcpy(payload + n, "\x03\x03\x03", 3); n += 3;         /* primary */
  RedParsed p;
  assert(red_unwrap(payload, n, OPUS_PT, &p) == 0);
  assert(p.block_count == 2);
  assert(p.blocks[0].ts_offset == 2 * FRAME_TS && p.blocks[0].length == 1);
  assert(p.blocks[1].ts_offset == FRAME_TS && p.blocks[1].length == 2);
  assert(p.blocks[0].data[0] == 0x01);
  assert(memcmp(p.blocks[1].data, "\x02\x02", 2) == 0);
  assert(p.primary_size == 3 && memcmp(p.primary, "\x03\x03\x03", 3) == 0);
}

static void test_realistic_sizes_vector(void) {
  /* server: test_golden_vector_realistic_sizes (80/100/90-byte frames) */
  uint8_t payload[512];
  size_t n = hdr(payload, OPUS_PT, 1920, 80);
  n += hdr(payload + n, OPUS_PT, 960, 100);
  payload[n++] = OPUS_PT;
  memset(payload + n, 0x11, 80); n += 80;
  memset(payload + n, 0x22, 100); n += 100;
  memset(payload + n, 0x33, 90); n += 90;
  RedParsed p;
  assert(red_unwrap(payload, n, OPUS_PT, &p) == 0);
  assert(p.block_count == 2);
  assert(p.blocks[0].length == 80 && p.blocks[0].data[0] == 0x11);
  assert(p.blocks[1].length == 100 && p.blocks[1].data[99] == 0x22);
  assert(p.primary_size == 90 && p.primary[0] == 0x33 && p.primary[89] == 0x33);
}

/* ── malformed inputs -> safe fallback (-1) ───────────────────────────────── */

static void test_truncated_header(void) {
  uint8_t payload[] = {0x80 | OPUS_PT, 0x0F}; /* F=1 but only 2 bytes */
  RedParsed p;
  assert(red_unwrap(payload, sizeof(payload), OPUS_PT, &p) == -1);
}

static void test_missing_final_header(void) {
  uint8_t payload[8];
  size_t n = hdr(payload, OPUS_PT, FRAME_TS, 2); /* header chain never ends */
  RedParsed p;
  assert(red_unwrap(payload, n, OPUS_PT, &p) == -1);
}

static void test_foreign_pt_rejected(void) {
  uint8_t payload[32];
  size_t n = hdr(payload, 96 /* H264, not opus */, FRAME_TS, 1);
  payload[n++] = OPUS_PT;
  payload[n++] = 0x01;
  payload[n++] = 0x02;
  RedParsed p;
  assert(red_unwrap(payload, n, OPUS_PT, &p) == -1);
}

static void test_primary_pt_mismatch(void) {
  uint8_t payload[] = {96, 0xAA}; /* final header says H264 */
  RedParsed p;
  assert(red_unwrap(payload, sizeof(payload), OPUS_PT, &p) == -1);
}

static void test_lengths_exceed_payload(void) {
  uint8_t payload[16];
  size_t n = hdr(payload, OPUS_PT, FRAME_TS, 200); /* declares 200 bytes */
  payload[n++] = OPUS_PT;
  payload[n++] = 0x01; /* ...but only 1 byte follows */
  RedParsed p;
  assert(red_unwrap(payload, n, OPUS_PT, &p) == -1);
}

static void test_empty_primary_rejected(void) {
  uint8_t payload[8];
  size_t n = hdr(payload, OPUS_PT, FRAME_TS, 1);
  payload[n++] = OPUS_PT;
  payload[n++] = 0x01; /* block data eats everything; primary empty */
  RedParsed p;
  assert(red_unwrap(payload, n, OPUS_PT, &p) == -1);
}

static void test_zero_ts_offset_rejected(void) {
  uint8_t payload[8];
  size_t n = hdr(payload, OPUS_PT, 0, 1); /* offset 0 is nonsense */
  payload[n++] = OPUS_PT;
  payload[n++] = 0x01;
  payload[n++] = 0x02;
  RedParsed p;
  assert(red_unwrap(payload, n, OPUS_PT, &p) == -1);
}

static void test_too_many_blocks_rejected(void) {
  uint8_t payload[64];
  size_t n = 0;
  for (int i = 0; i < RED_MAX_BLOCKS + 1; i++) {
    n += hdr(payload + n, OPUS_PT, (uint16_t)((i + 1) * FRAME_TS), 1);
  }
  payload[n++] = OPUS_PT;
  memset(payload + n, 0xEE, RED_MAX_BLOCKS + 2); n += RED_MAX_BLOCKS + 2;
  RedParsed p;
  assert(red_unwrap(payload, n, OPUS_PT, &p) == -1);
}

static void test_empty_and_null(void) {
  RedParsed p;
  uint8_t b = OPUS_PT;
  assert(red_unwrap(NULL, 10, OPUS_PT, &p) == -1);
  assert(red_unwrap(&b, 0, OPUS_PT, &p) == -1);
  assert(red_unwrap(&b, 1, OPUS_PT, NULL) == -1);
}

/* ── recovery planner: gap / dup / reorder semantics ─────────────────────── */

static RedParsed make_n2_parsed(void) {
  /* Steady-state packet: N-2 (offset 1920) + N-1 (offset 960). */
  static uint8_t payload[64];
  size_t n = hdr(payload, OPUS_PT, 2 * FRAME_TS, 3);
  n += hdr(payload + n, OPUS_PT, FRAME_TS, 4);
  payload[n++] = OPUS_PT;
  memset(payload + n, 0xA2, 3); n += 3; /* N-2 data */
  memset(payload + n, 0xA1, 4); n += 4; /* N-1 data */
  memset(payload + n, 0xA0, 5); n += 5; /* primary */
  RedParsed p;
  assert(red_unwrap(payload, n, OPUS_PT, &p) == 0);
  return p;
}

static void test_plan_single_gap(void) {
  /* 1 packet lost: the missing frame is 1 packet before primary -> N-1 block. */
  RedParsed p = make_n2_parsed();
  int8_t actions[RED_MAX_GAP];
  assert(red_recover_plan(&p, 1, FRAME_TS, actions) == 1);
  assert(actions[0] == 1); /* blocks[1] = offset 960 = N-1 */
  assert(p.blocks[actions[0]].data[0] == 0xA1);
}

static void test_plan_double_gap(void) {
  /* 2 packets lost: oldest missing = N-2 block, newest = N-1 block. */
  RedParsed p = make_n2_parsed();
  int8_t actions[RED_MAX_GAP];
  assert(red_recover_plan(&p, 2, FRAME_TS, actions) == 2);
  assert(actions[0] == 0); /* oldest <- blocks[0] (offset 1920) */
  assert(actions[1] == 1); /* newest <- blocks[1] (offset 960) */
}

static void test_plan_triple_gap_partial(void) {
  /* 3 lost: only the two newest are covered; oldest falls to PLC (-1). */
  RedParsed p = make_n2_parsed();
  int8_t actions[RED_MAX_GAP];
  assert(red_recover_plan(&p, 3, FRAME_TS, actions) == 3);
  assert(actions[0] == -1); /* 3 back: no block */
  assert(actions[1] == 0);  /* 2 back: N-2 */
  assert(actions[2] == 1);  /* 1 back: N-1 */
}

static void test_plan_no_blocks_all_plc(void) {
  /* Primary-only packet (stream start): every gap frame -> PLC. */
  uint8_t payload[] = {OPUS_PT, 0xAA};
  RedParsed p;
  assert(red_unwrap(payload, sizeof(payload), OPUS_PT, &p) == 0);
  int8_t actions[RED_MAX_GAP];
  assert(red_recover_plan(&p, 2, FRAME_TS, actions) == 2);
  assert(actions[0] == -1 && actions[1] == -1);
}

static void test_plan_gap_cap(void) {
  /* Gap beyond RED_MAX_GAP is clamped (rtp.c already resyncs at >16). */
  RedParsed p = make_n2_parsed();
  int8_t actions[RED_MAX_GAP];
  assert(red_recover_plan(&p, 99, FRAME_TS, actions) == RED_MAX_GAP);
}

static void test_plan_bad_args(void) {
  RedParsed p = make_n2_parsed();
  int8_t actions[RED_MAX_GAP];
  assert(red_recover_plan(NULL, 1, FRAME_TS, actions) == 0);
  assert(red_recover_plan(&p, 0, FRAME_TS, actions) == 0);
  assert(red_recover_plan(&p, -3, FRAME_TS, actions) == 0);
  assert(red_recover_plan(&p, 1, 0, actions) == 0);
  assert(red_recover_plan(&p, 1, FRAME_TS, NULL) == 0);
}

static void test_plan_mismatched_ts_step(void) {
  /* ptime change (e.g. 60ms frames): offsets no longer match -> PLC, never
   * a WRONG block. */
  RedParsed p = make_n2_parsed();
  int8_t actions[RED_MAX_GAP];
  assert(red_recover_plan(&p, 1, 2880, actions) == 1);
  assert(actions[0] == -1);
}

int main(void) {
  RUN(test_primary_only);
  RUN(test_one_redundant_block);
  RUN(test_two_blocks_oldest_first);
  RUN(test_realistic_sizes_vector);
  RUN(test_truncated_header);
  RUN(test_missing_final_header);
  RUN(test_foreign_pt_rejected);
  RUN(test_primary_pt_mismatch);
  RUN(test_lengths_exceed_payload);
  RUN(test_empty_primary_rejected);
  RUN(test_zero_ts_offset_rejected);
  RUN(test_too_many_blocks_rejected);
  RUN(test_empty_and_null);
  RUN(test_plan_single_gap);
  RUN(test_plan_double_gap);
  RUN(test_plan_triple_gap_partial);
  RUN(test_plan_no_blocks_all_plc);
  RUN(test_plan_gap_cap);
  RUN(test_plan_bad_args);
  RUN(test_plan_mismatched_ts_step);
  printf("PASS: %d red_unwrap host tests\n", tests_run);
  return 0;
}
