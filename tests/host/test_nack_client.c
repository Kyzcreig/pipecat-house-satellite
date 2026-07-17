/* Host-side unit tests for the NACK retransmit client (Phase 5 planner + table).
 *
 * Pure byte logic — builds and runs on ANY host compiler, no ESP-IDF:
 *   ./tests/host/run_nack_tests.sh          (from the repo root)
 * which does: cc -I xiao-esp32-s3/components/peer test_nack_client.c nack_client.c
 *
 * Mirrors the RED host-test layout (tests/host/test_red_unwrap.c). Covers the
 * spec's Phase-5 requirements: RED owns <=2 (planner returns 0), NACK owns >=3,
 * the 8-seq amplification cap, seq wraparound, the 20ms splice window (in-window
 * recover vs late drop), unknown-seq ignore, table-full eviction, and the
 * never-arrived sweep -> late accounting.
 */
#include <assert.h>
#include <stdio.h>

#include "nack_client.h"

static int tests_run = 0;
#define RUN(fn)                 \
  do {                          \
    fn();                       \
    tests_run++;                \
    printf("ok - %s\n", #fn);   \
  } while (0)

/* ── planner: RED/NACK boundary + cap ─────────────────────────────────────── */

static void test_plan_below_min_gap_is_red(void) {
  /* gap 1 and 2 belong to FEC/RED — planner must return 0 (no NACK). */
  uint16_t out[NACK_MAX_SEQS];
  assert(nack_client_plan_gap(100, 1, out, NACK_MAX_SEQS) == 0);
  assert(nack_client_plan_gap(100, 2, out, NACK_MAX_SEQS) == 0);
}

static void test_plan_triple_gap(void) {
  /* gap 3: NACK the three missing seqs ascending from first_missing. */
  uint16_t out[NACK_MAX_SEQS];
  assert(nack_client_plan_gap(100, 3, out, NACK_MAX_SEQS) == 3);
  assert(out[0] == 100 && out[1] == 101 && out[2] == 102);
}

static void test_plan_caps_at_8(void) {
  /* A 20-packet burst is capped to NACK_MAX_SEQS (amplification guard). */
  uint16_t out[NACK_MAX_SEQS];
  assert(nack_client_plan_gap(0, 20, out, NACK_MAX_SEQS) == NACK_MAX_SEQS);
  for (int i = 0; i < NACK_MAX_SEQS; i++) {
    assert(out[i] == (uint16_t)i);
  }
}

static void test_plan_respects_out_capacity(void) {
  uint16_t out[3];
  assert(nack_client_plan_gap(50, 6, out, 3) == 3);
  assert(out[0] == 50 && out[2] == 52);
}

static void test_plan_seq_wraparound(void) {
  /* first_missing near the 16-bit top wraps through 0. */
  uint16_t out[NACK_MAX_SEQS];
  assert(nack_client_plan_gap(65534, 4, out, NACK_MAX_SEQS) == 4);
  assert(out[0] == 65534 && out[1] == 65535 && out[2] == 0 && out[3] == 1);
}

static void test_plan_bad_args(void) {
  uint16_t out[NACK_MAX_SEQS];
  assert(nack_client_plan_gap(0, 3, NULL, NACK_MAX_SEQS) == 0);
  assert(nack_client_plan_gap(0, 3, out, 0) == 0);
  assert(nack_client_plan_gap(0, 0, out, NACK_MAX_SEQS) == 0);
}

/* ── NACK-v2 binary frame parser: bounds + fuzz surface ──────────────────── */

static void test_parse_binary_rtx_frame(void) {
  const uint8_t frame[] = {0x12, 0x34, 0x00, 0x03, 0xaa, 0xbb, 0xcc};
  uint16_t seq = 0;
  const uint8_t *payload = NULL;
  size_t payload_len = 0;
  assert(nack_parse_rtx_frame(frame, sizeof(frame), &seq, &payload,
                              &payload_len) == 1);
  assert(seq == 0x1234);
  assert(payload_len == 3);
  assert(payload == frame + RTX_FRAME_HEADER_SIZE);
  assert(payload[0] == 0xaa && payload[2] == 0xcc);
}

static void test_parse_binary_rtx_rejects_malformed_bounds(void) {
  uint16_t seq = 0;
  const uint8_t *payload = NULL;
  size_t payload_len = 0;
  const uint8_t short_header[] = {0, 1, 0};
  const uint8_t zero_len[] = {0, 1, 0, 0};
  const uint8_t truncated[] = {0, 1, 0, 2, 0xaa};
  uint8_t oversized[RTX_FRAME_HEADER_SIZE + RTX_FRAME_CAP + 1] = {0};
  oversized[2] = (uint8_t)((RTX_FRAME_CAP + 1) >> 8);
  oversized[3] = (uint8_t)((RTX_FRAME_CAP + 1) & 0xff);

  assert(nack_parse_rtx_frame(NULL, 0, &seq, &payload, &payload_len) == 0);
  assert(nack_parse_rtx_frame(short_header, sizeof(short_header), &seq,
                              &payload, &payload_len) == 0);
  assert(nack_parse_rtx_frame(zero_len, sizeof(zero_len), &seq, &payload,
                              &payload_len) == 0);
  assert(nack_parse_rtx_frame(truncated, sizeof(truncated), &seq, &payload,
                              &payload_len) == 0);
  assert(nack_parse_rtx_frame(oversized, sizeof(oversized), &seq, &payload,
                              &payload_len) == 0);
}

static void test_parse_binary_rtx_fuzzed_lengths_drop_safely(void) {
  uint8_t frame[RTX_FRAME_HEADER_SIZE + 32] = {0};
  uint16_t seq = 0;
  const uint8_t *payload = NULL;
  size_t payload_len = 0;
  for (size_t actual = 0; actual <= sizeof(frame); actual++) {
    for (uint16_t declared = 0; declared <= 40; declared++) {
      frame[2] = (uint8_t)(declared >> 8);
      frame[3] = (uint8_t)(declared & 0xff);
      int ok = nack_parse_rtx_frame(frame, actual, &seq, &payload,
                                    &payload_len);
      int expected = actual >= RTX_FRAME_HEADER_SIZE && declared > 0 &&
                     declared <= RTX_FRAME_CAP &&
                     declared == actual - RTX_FRAME_HEADER_SIZE;
      assert(ok == expected);
      if (ok) {
        assert(payload == frame + RTX_FRAME_HEADER_SIZE);
        assert(payload_len == declared);
      }
    }
  }
}

/* ── pending table: arm / take within window / late / unknown ─────────────── */

static void test_arm_and_recover_in_window(void) {
  NackClient c;
  nack_client_init(&c);
  nack_client_arm(&c, 42, 1000);           /* deadline = 1020 */
  assert(c.nack_sent == 1);
  /* rtx at 1015 (within 20ms) -> spliced, counted recovered. */
  assert(nack_client_take(&c, 42, 1015) == NACK_TAKE_INWINDOW);
  assert(c.nack_recovered == 1 && c.nack_late == 0);
  /* Consumed: a second take is UNKNOWN. */
  assert(nack_client_take(&c, 42, 1016) == NACK_TAKE_UNKNOWN);
}

static void test_recover_at_exact_deadline(void) {
  NackClient c;
  nack_client_init(&c);
  nack_client_arm(&c, 7, 500);             /* deadline = 520 */
  assert(nack_client_take(&c, 7, 520) == NACK_TAKE_INWINDOW); /* boundary ok */
  assert(c.nack_recovered == 1);
}

static void test_late_rtx_dropped(void) {
  NackClient c;
  nack_client_init(&c);
  nack_client_arm(&c, 9, 1000);            /* deadline = 1000 + NACK_WAIT_MS */
  /* rtx 1ms past the deadline -> LATE, not spliced. */
  assert(nack_client_take(&c, 9, 1001 + NACK_WAIT_MS) == NACK_TAKE_LATE);
  assert(c.nack_recovered == 0 && c.nack_late == 1);
}

static void test_unknown_seq_ignored(void) {
  NackClient c;
  nack_client_init(&c);
  /* Never NACKed 5 -> a stray rtx for it is ignored, no counter moves. */
  assert(nack_client_take(&c, 5, 100) == NACK_TAKE_UNKNOWN);
  assert(c.nack_recovered == 0 && c.nack_late == 0);
}

static void test_sweep_never_arrived(void) {
  NackClient c;
  nack_client_init(&c);
  nack_client_arm(&c, 1, 1000);            /* deadline 1020 */
  nack_client_arm(&c, 2, 1000);            /* deadline 1020 */
  /* Before expiry: nothing swept. */
  assert(nack_client_sweep(&c, 1010) == 0);
  /* After expiry: both tallied late; slots kept EXPIRED for RTT capture. */
  assert(nack_client_sweep(&c, 1010 + NACK_WAIT_MS) == 2);
  assert(c.nack_late == 2);
  /* A straggler rtx on an expired slot: LATE result, RTT recorded, no
   * double late-count, slot then freed. */
  assert(nack_client_take(&c, 1, 1200) == NACK_TAKE_LATE);
  assert(c.nack_late == 2);           /* counted once, at sweep */
  assert(c.last_rtt_ms == 200);       /* 1200 - arm(1000) */
  assert(nack_client_take(&c, 1, 1300) == NACK_TAKE_UNKNOWN); /* now freed */
}

static void test_arm_full_table_rejects_without_evicting_pending_audio(void) {
  NackClient c;
  nack_client_init(&c);
  /* Fill every slot with staggered deadlines (seq i armed at t=i). */
  for (int i = 0; i < NACK_PENDING_SLOTS; i++) {
    nack_client_arm(&c, (uint16_t)(200 + i), (uint32_t)(1000 + i));
  }
  assert(c.nack_sent == NACK_PENDING_SLOTS);
  /* One more arm fails closed: evicting seq 200 would suppress its deferred PLC. */
  assert(nack_client_arm(&c, 999, 1010) == 0);
  assert(c.nack_sent == NACK_PENDING_SLOTS);
  assert(nack_client_take(&c, 200, 1010) == NACK_TAKE_INWINDOW);
  assert(nack_client_take(&c, 999, 1010) == NACK_TAKE_UNKNOWN);
}

static void test_ms_clock_wrap_boundary(void) {
  /* deadline computed near the 32-bit ms wrap; signed delta keeps it correct. */
  NackClient c;
  nack_client_init(&c);
  uint32_t near_wrap = 0xFFFFFFF0u;        /* +NACK_WAIT_MS wraps past 2^32 */
  nack_client_arm(&c, 3, near_wrap);       /* deadline wraps (near_wrap + WAITd) */
  uint32_t after = near_wrap + 10;         /* 10ms later, still in window */
  assert(nack_client_take(&c, 3, after) == NACK_TAKE_INWINDOW);
}

/* ── deferred-concealment exclusivity (the rtp.c ordering contract) ────────
 * rtp.c defers concealment for NACKed seqs: exactly ONE of {rtx splice,
 * sweep-driven PLC} may fire per armed seq — otherwise the ring gets two
 * frames for one timestamp (time stretch). That exclusivity rests entirely
 * on take() and sweep() both consuming the slot; pin it from both sides. */

static void test_recovered_seq_not_swept(void) {
  /* rtx splice consumed the slot -> a later sweep must NOT count it late
   * (which would trigger rtp.c's deferred PLC = duplicate audio). */
  NackClient c;
  nack_client_init(&c);
  nack_client_arm(&c, 42, 1000);           /* deadline 1020 */
  assert(nack_client_take(&c, 42, 1005) == NACK_TAKE_INWINDOW);
  assert(nack_client_sweep(&c, 1010 + NACK_WAIT_MS) == 0);  /* nothing left to expire */
  assert(c.nack_recovered == 1 && c.nack_late == 0);
}

static void test_swept_seq_rejects_rtx(void) {
  /* sweep already fired the deferred PLC -> a straggler rtx must be UNKNOWN
   * (not spliced), and the mix of outcomes across seqs stays disjoint. */
  NackClient c;
  nack_client_init(&c);
  nack_client_arm(&c, 10, 1000);           /* will recover in-window */
  nack_client_arm(&c, 11, 1000);           /* will expire via sweep */
  assert(nack_client_take(&c, 10, 1010) == NACK_TAKE_INWINDOW);
  assert(nack_client_sweep(&c, 1030 + NACK_WAIT_MS) == 1);  /* only seq 11 */
  /* Straggler rtx on the swept slot: LATE (rtp.c only splices INWINDOW),
   * RTT captured, late counted once (at sweep). */
  assert(nack_client_take(&c, 11, 1051) == NACK_TAKE_LATE); /* no splice */
  assert(c.nack_recovered == 1 && c.nack_late == 1 && c.nack_sent == 2);
}

/* ── rolling effectiveness circuit breaker ──────────────────────────────── */

static void test_nack_wait_starts_at_spec_v2_value(void) {
  assert(NACK_WAIT_MS == 60);
}

static void test_circuit_breaker_darks_below_twenty_percent(void) {
  NackClient c;
  nack_client_init(&c);
  for (uint16_t seq = 0; seq < 10; seq++) {
    nack_client_arm(&c, seq, seq);
  }
  assert(nack_client_should_dark(&c, 10) == 0); /* wait for outcomes */
  assert(nack_client_sweep(&c, 1000) == 10);
  assert(nack_client_should_dark(&c, 1000) == 1);
  assert(c.auto_dark == 1);
}

static void test_circuit_breaker_keeps_exact_twenty_percent(void) {
  NackClient c;
  nack_client_init(&c);
  for (uint16_t seq = 0; seq < 10; seq++) {
    nack_client_arm(&c, seq, 100);
  }
  assert(nack_client_take(&c, 0, 110) == NACK_TAKE_INWINDOW);
  assert(nack_client_take(&c, 1, 110) == NACK_TAKE_INWINDOW);
  assert(nack_client_sweep(&c, 1000) == 8);
  assert(nack_client_should_dark(&c, 1000) == 0);
  assert(c.auto_dark == 0);
}

static void test_circuit_breaker_resets_on_reconnect_init(void) {
  NackClient c;
  nack_client_init(&c);
  for (uint16_t seq = 0; seq < 10; seq++) {
    nack_client_arm(&c, seq, 0);
  }
  nack_client_sweep(&c, 1000);
  assert(nack_client_should_dark(&c, 1000) == 1);
  nack_client_init(&c);
  assert(c.auto_dark == 0);
  assert(nack_client_should_dark(&c, 1001) == 0);
}

static void test_circuit_breaker_expires_old_healthy_window(void) {
  NackClient c;
  nack_client_init(&c);
  for (uint16_t seq = 0; seq < 10; seq++) {
    nack_client_arm(&c, seq, seq);
    assert(nack_client_take(&c, seq, seq + 1) == NACK_TAKE_INWINDOW);
  }
  uint32_t now = NACK_EFFECT_BUCKET_MS * NACK_EFFECT_BUCKETS;
  for (uint16_t seq = 100; seq < 110; seq++) {
    nack_client_arm(&c, seq, now);
  }
  assert(nack_client_sweep(&c, now + NACK_WAIT_MS + 1) == 10);
  assert(nack_client_should_dark(&c, now + NACK_WAIT_MS + 1) == 1);
}

int main(void) {
  RUN(test_plan_below_min_gap_is_red);
  RUN(test_plan_triple_gap);
  RUN(test_plan_caps_at_8);
  RUN(test_plan_respects_out_capacity);
  RUN(test_plan_seq_wraparound);
  RUN(test_plan_bad_args);
  RUN(test_parse_binary_rtx_frame);
  RUN(test_parse_binary_rtx_rejects_malformed_bounds);
  RUN(test_parse_binary_rtx_fuzzed_lengths_drop_safely);
  RUN(test_arm_and_recover_in_window);
  RUN(test_recover_at_exact_deadline);
  RUN(test_late_rtx_dropped);
  RUN(test_unknown_seq_ignored);
  RUN(test_sweep_never_arrived);
  RUN(test_arm_full_table_rejects_without_evicting_pending_audio);
  RUN(test_ms_clock_wrap_boundary);
  RUN(test_recovered_seq_not_swept);
  RUN(test_swept_seq_rejects_rtx);
  RUN(test_nack_wait_starts_at_spec_v2_value);
  RUN(test_circuit_breaker_darks_below_twenty_percent);
  RUN(test_circuit_breaker_keeps_exact_twenty_percent);
  RUN(test_circuit_breaker_resets_on_reconnect_init);
  RUN(test_circuit_breaker_expires_old_healthy_window);
  printf("PASS: %d nack_client host tests\n", tests_run);
  return 0;
}
