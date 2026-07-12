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
  nack_client_arm(&c, 9, 1000);            /* deadline = 1020 */
  /* rtx at 1021 (1ms late) -> LATE, not spliced. */
  assert(nack_client_take(&c, 9, 1021) == NACK_TAKE_LATE);
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
  /* After expiry: both tallied late and freed. */
  assert(nack_client_sweep(&c, 1030) == 2);
  assert(c.nack_late == 2);
  /* Freed: a take now returns UNKNOWN. */
  assert(nack_client_take(&c, 1, 1031) == NACK_TAKE_UNKNOWN);
}

static void test_arm_full_table_evicts_oldest(void) {
  NackClient c;
  nack_client_init(&c);
  /* Fill every slot with staggered deadlines (seq i armed at t=i). */
  for (int i = 0; i < NACK_PENDING_SLOTS; i++) {
    nack_client_arm(&c, (uint16_t)(200 + i), (uint32_t)(1000 + i));
  }
  assert(c.nack_sent == NACK_PENDING_SLOTS);
  /* One more arm evicts the oldest-deadline slot (seq 200, deadline 1020). */
  nack_client_arm(&c, 999, 2000);
  /* seq 200 was evicted -> UNKNOWN; the new seq 999 is live. */
  assert(nack_client_take(&c, 200, 2001) == NACK_TAKE_UNKNOWN);
  assert(nack_client_take(&c, 999, 2005) == NACK_TAKE_INWINDOW);
}

static void test_ms_clock_wrap_boundary(void) {
  /* deadline computed near the 32-bit ms wrap; signed delta keeps it correct. */
  NackClient c;
  nack_client_init(&c);
  uint32_t near_wrap = 0xFFFFFFF0u;        /* +20 wraps past 2^32 */
  nack_client_arm(&c, 3, near_wrap);       /* deadline = near_wrap + 20 (wrapped) */
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
  assert(nack_client_sweep(&c, 1030) == 0);  /* nothing left to expire */
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
  assert(nack_client_sweep(&c, 1050) == 1);            /* only seq 11 */
  assert(nack_client_take(&c, 11, 1051) == NACK_TAKE_UNKNOWN); /* no splice */
  assert(c.nack_recovered == 1 && c.nack_late == 1 && c.nack_sent == 2);
}

int main(void) {
  RUN(test_plan_below_min_gap_is_red);
  RUN(test_plan_triple_gap);
  RUN(test_plan_caps_at_8);
  RUN(test_plan_respects_out_capacity);
  RUN(test_plan_seq_wraparound);
  RUN(test_plan_bad_args);
  RUN(test_arm_and_recover_in_window);
  RUN(test_recover_at_exact_deadline);
  RUN(test_late_rtx_dropped);
  RUN(test_unknown_seq_ignored);
  RUN(test_sweep_never_arrived);
  RUN(test_arm_full_table_evicts_oldest);
  RUN(test_ms_clock_wrap_boundary);
  RUN(test_recovered_seq_not_swept);
  RUN(test_swept_seq_rejects_rtx);
  printf("PASS: %d nack_client host tests\n", tests_run);
  return 0;
}
